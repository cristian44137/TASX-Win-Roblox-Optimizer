#include "jobs.h"

#include "config.h"
#include "ntsys.h"
#include "log.h"
#include "lograte.h"
#include "stats.h"

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>

#ifndef JOB_OBJECT_MSG_EXIT_PROCESS
#define JOB_OBJECT_MSG_EXIT_PROCESS 7
#endif
#ifndef JOB_OBJECT_MSG_NEW_PROCESS
#define JOB_OBJECT_MSG_NEW_PROCESS 6
#endif

namespace {

std::mutex g_mtx;
HANDLE g_jobBackground = nullptr;
HANDLE g_iocp = nullptr;
HANDLE g_iocpThread = nullptr;
DWORD  g_iocpThreadId = 0;
/* pid -> 0 in cgroup with background settings, 2 in cgroup with the
   focus profile applied, -1 sticky per-process fallback (foreign job /
   denied, never retried). */
std::unordered_map<DWORD, int> g_pidJob;

/* Background policy cache for JobsRefreshDynamic */
DWORD_PTR g_dynamicBgMask = 0;
bool      g_bgRateCap = false;
int       g_dynamicCpuCap = -1;

/* FarmBoost (Ctrl+Alt+B): while on, the shared background job runs at full
   power (all cores, no CPU cap, higher priority) regardless of focus state
   or DynamicAffinity. Toggled by master.cpp; read here without an extra
   lock since it's a single atomic flag. */
std::atomic<bool> g_farmBoostOn{false};

/* JobAssignMode values: auto (default, try assign, sticky fallback on foreign Job),
   diagnose (same + log InJob info for broken foreign-job diagnostics),
   off (never assign, pure per-process profile). Deprecated alias: force. */
int GetJobAssignMode()
{
    const char* v = config_get_str("TASX", "JobAssignMode", "auto");
    if (!v || !*v) v = config_get_str("FastFlags", "JobAssignMode", "auto");
    char low[16] = {};
    size_t n = 0;
    for (; v[n] && n + 1 < sizeof(low); ++n) {
        char c = v[n];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        low[n] = c;
    }
    low[n] = '\0';
    if (strcmp(low, "off") == 0) return 2; // off
    if ((low[0] == 'd' && low[1] == 'i') || (low[0] == 'f' && low[1] == 'o')) {
        // diagnose or deprecated force
        return 1;
    }
    return 0; // auto
}

bool ShouldLogPerPid(const char* prefix, DWORD pid, int intervalSec)
{
    char tag[64];
    snprintf(tag, sizeof(tag), "%s-%lu", prefix, (unsigned long)pid);
    return LogRateLimit(tag, intervalSec);
}

DWORD_PTR GetBgMaskForCurrentPolicy(int anyFocused)
{
    (void)anyFocused;
    unsigned long long allMask = tasx_get_all_mask();

    /* A shared job affinity applies to every member, including the focused
       client. Keep the job's ceiling at all cores and let master.cpp apply
       DynamicAffinity per process; this prevents a focused client from being
       accidentally pinned to E-cores when another client is backgrounded. */
    return (DWORD_PTR)allMask;
}

/* Single readable CPU-cap path: BackgroundCpuCapPercent, legacy fallback
   JobCpuCapPercent, default 20 in farm mode, 0 when something is focused. */
int BgCpuCapPercent(int anyFocused)
{
    if (g_farmBoostOn.load(std::memory_order_relaxed))
        return 0; // boosted: never rate-cap the farm

    int cap = config_get_int("TASX", "BackgroundCpuCapPercent", -1);
    if (cap < 0)
        cap = config_get_int("TASX", "JobCpuCapPercent", 0);
    if (cap < 0) {
        const char* v = config_get_str("TASX", "BackgroundCpuCapPercent", nullptr);
        if (!v)
            v = config_get_str("TASX", "JobCpuCapPercent", nullptr);
        cap = (!v && !anyFocused) ? 20 : 0;
    }
    if (cap < 0)
        cap = 0;
    if (cap > 100)
        cap = 100;
    return cap;
}

bool ApplyLimitsToJob(HANDLE job, DWORD_PTR bgMask, int cpuCap)
{
    if (!job)
        return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
    /* Priority is deliberately per-process, not a job-wide limit. A job-level
       IDLE limit prevents the focused client from being promoted to HIGH;
       keeping only affinity/memory limits here lets master.cpp apply the
       correct focused/background profile without container interference. */
    ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_AFFINITY;
    ext.BasicLimitInformation.Affinity = bgMask ? bgMask : (DWORD_PTR)-1;

    int memCap = config_get_int("TASX", "JobMemoryCapMB", 0);
    if (memCap > 0) {
        ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        ext.ProcessMemoryLimit = (SIZE_T)(unsigned)memCap << 20;
    }

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &ext, sizeof(ext))) {
        return false;
    }

    if (cpuCap > 0 && cpuCap < 100) {
        if (!tasx_job_set_cpu_rate(job, (unsigned long)cpuCap)) {
            JOBOBJECT_CPU_RATE_CONTROL_INFORMATION crc{};
            crc.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE
                             | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
            crc.CpuRate = (DWORD)cpuCap * 100;
            SetInformationJobObject(job, JobObjectCpuRateControlInformation,
                                    &crc, sizeof(crc));
        }
    } else {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION crc{};
        crc.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE;
        crc.CpuRate = 10000; /* 100% - no cap */
        SetInformationJobObject(job, JobObjectCpuRateControlInformation,
                                &crc, sizeof(crc));
    }
    /* Job-level I/O priority is not portable: per-process VeryLow is set
       once in JobHookProcess for each newly assigned PID. */

    if (config_get_bool("ETW", "DisableTelemetry", 1) ||
        config_get_bool("TASX", "DisableTelemetry", 0)) {
        tasx_etw_disable_provider(L"Microsoft-Windows-Diagnostics-Performance");
        tasx_etw_disable_provider(L"Microsoft-Windows-Kernel-Processor-Power");
    }
    return true;
}

bool NameHas(const std::wstring& full, const wchar_t* needle)
{
    if (full.empty() || !needle)
        return false;
    std::wstring hay = full;
    for (auto& c : hay)
        c = (wchar_t)towlower(c);
    std::wstring ndl = needle;
    for (auto& c : ndl)
        c = (wchar_t)towlower(c);
    return hay.find(ndl) != std::wstring::npos;
}

DWORD WINAPI IocpLoopImpl(LPVOID)
{
    for (;;) {
        DWORD code = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        (void)key;
        BOOL ok = GetQueuedCompletionStatus(g_iocp, &code, &key, &ov, INFINITE);
        if (!ok) {
            if (!g_iocp)
                break;
            DWORD err = GetLastError();
            if (err == ERROR_ABANDONED_WAIT_0 || err == ERROR_INVALID_HANDLE)
                break;
            continue;
        }
        if (!ov && code == 0)
            break; /* orderly shutdown sentinel */
        DWORD pid = (DWORD)(ULONG_PTR)ov;
        if (code == JOB_OBJECT_MSG_EXIT_PROCESS) {
            TasxNotifyJobEvent(1, pid);
        } else if (code == JOB_OBJECT_MSG_NEW_PROCESS) {
            /* A job child spawned. Only the crash handler is of interest -
               never forward arbitrary children (would cause kill storms). */
            std::wstring name = QueryProcessNameByPid(pid);
            if (NameHas(name, L"robloxcrashhandler.exe"))
                TasxNotifyJobEvent(2, pid);
        }
    }
    return 0;
}

DWORD WINAPI IocpLoop(LPVOID arg) noexcept
{
    try {
        return IocpLoopImpl(arg);
    } catch (const std::exception& ex) {
        LOGE("[Jobs] IOCP worker exception: %s; worker stopped safely", ex.what());
    } catch (...) {
        LOGE("[Jobs] IOCP worker exception; worker stopped safely");
    }
    return 0;
}

bool CreateBackgroundJob()
{
    /* Never let a missing/broken config turn agent shutdown into a farm-wide
       kill. KILL_ON_JOB_CLOSE is an explicit opt-in for disposable sessions. */
    int killOnClose = config_get_bool("TASX", "KillOnAgentExit", 0);
    g_jobBackground = tasx_job_create_ex(killOnClose ? 1 : 0);
    if (!g_jobBackground)
        return false;

    if (g_iocp) {
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp{};
        acp.CompletionPort = g_iocp;
        acp.CompletionKey = nullptr;
        SetInformationJobObject(g_jobBackground,
                                JobObjectAssociateCompletionPortInformation,
                                &acp, sizeof(acp));
    }

    DWORD_PTR bgMask = GetBgMaskForCurrentPolicy(0);
    int cap = BgCpuCapPercent(0);
    bool ok = ApplyLimitsToJob(g_jobBackground, bgMask, cap);
    if (!ok) {
        /* Do not retain a half-configured Job Object: callers will use the
           safer per-process path instead, and Roblox clients are never put
           into an unknown policy container. */
        if (LogRateLimit("jobs-applylimits", 60))
            LOGW("[Jobs] Background cgroup policy failed; disabling Job assignment and using per-process fallback");
        CloseHandle(g_jobBackground);
        g_jobBackground = nullptr;
        return false;
    }
    g_dynamicBgMask = bgMask;
    g_bgRateCap = cap > 0;
    g_dynamicCpuCap = cap;
    return true;
}

} // namespace

bool JobsInit()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_jobBackground)
        return true;

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!g_iocp) {
        LOGE("[Jobs] Failed to create completion port | Code: %lu",
             (unsigned long)GetLastError());
        return false;
    }
    g_iocpThread = CreateThread(nullptr, 0, IocpLoop, nullptr, 0, &g_iocpThreadId);
    if (!g_iocpThread) {
        LOGE("[Jobs] Failed to create IOCP thread");
        CloseHandle(g_iocp);
        g_iocp = nullptr;
        return false;
    }

    if (!CreateBackgroundJob()) {
        LOGE("[Jobs] Failed to create background job");
        if (g_iocpThread) {
            /* Wake the IOCP worker and let it leave through its normal
               control path. Closing the port underneath a blocked worker or
               using TerminateThread can strand locks and corrupt teardown. */
            PostQueuedCompletionStatus(g_iocp, 0, 0, nullptr);
            WaitForSingleObject(g_iocpThread, INFINITE);
            CloseHandle(g_iocpThread);
            g_iocpThread = nullptr;
        }
        CloseHandle(g_iocp);
        g_iocp = nullptr;
        return false;
    }

    LOGI("[Jobs] Background cgroup ready (single job + per-process focus profiles)");
    return true;
}

bool JobHookProcess(DWORD pid, HANDLE hProc)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_jobBackground)
        return false;
    auto known = g_pidJob.find(pid);
    if (known != g_pidJob.end())
        return known->second != -1; // already tracked: job or sticky fallback

    int mode = GetJobAssignMode();
    if (mode == 2) { // off
        g_pidJob[pid] = -1;
        if (ShouldLogPerPid("job-off", pid, 300))
            LOGW("[Jobs] PID %lu JobAssignMode=off -> per-process fallback (no Job assign)", pid);
        return false;
    }

    /* Assign-once: a process can live in exactly one Job. Retry is
       pointless for both failure modes, so the fallback is sticky. */
    if (!tasx_job_assign(g_jobBackground, hProc)) {
        DWORD err = GetLastError();
        g_pidJob[pid] = -1;
        if (mode == 1) {
            BOOL inJob = FALSE;
            IsProcessInJob(hProc, nullptr, &inJob);
            if (ShouldLogPerPid("job-force", pid, 300))
                LOGW("[Jobs] PID %lu assign denied (err %lu, inJob=%d, mode=force) -> per-process fallback (foreign Job without BREAKAWAY_OK)", pid, (unsigned long)err, (int)inJob);
        } else if (err == ERROR_ALREADY_ASSIGNED) {
            // foreign job without BREAKAWAY_OK - external launcher/manager owns it
            if (ShouldLogPerPid("job-foreign", pid, 300))
                LOGW("[Jobs] PID %lu already in foreign job (no BREAKAWAY_OK) -> per-process fallback, no retry", pid);
        } else if (err == ERROR_ACCESS_DENIED) {
            // err 5: process already in another Job, sticky fallback
            if (ShouldLogPerPid("job-denied", pid, 300))
                LOGW("[Jobs] PID %lu assign denied (err 5, foreign Job) -> per-process fallback, sticky no retry", pid);
        } else {
            if (ShouldLogPerPid("job-assign-fail", pid, 60))
                LOGW("[Jobs] PID %lu assign to background failed | Code: %lu -> per-process fallback", pid,
                     (unsigned long)err);
        }
        return false;
    }

    tasx_process_power_throttling(hProc, 1);
    tasx_set_io_priority(hProc, 0);
    tasx_set_memory_priority(hProc, 1);

    g_pidJob[pid] = 0;
    LOGI("[TASX] PID %lu -> background cgroup", pid);
    return true;
}

bool JobApplyProfile(DWORD pid, int focused)
{
    /* State cache for cgroup-tracked processes. The shared cgroup carries
       only farm-wide CPU/MEM caps; priority/affinity/EcoQoS per focus state
       are applied per-process by the caller (CpuApplyFocusProfile).
       Returns true when the process already runs the requested state
       (caller skips all syscalls), false when the caller must apply the
       per-process profile - and after a cross move there is deliberately
       NO AssignProcessToJobObject: moving between sibling jobs always
       fails with ERROR_ACCESS_DENIED. */
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_pidJob.find(pid);
    if (it == g_pidJob.end() || it->second == -1)
        return false;
    int want = focused ? 2 : 0;
    if (it->second == want)
        return true;
    it->second = want;
    return false;
}

void JobsRefreshDynamic(int anyFocused)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_jobBackground)
        return;

    DWORD_PTR target = GetBgMaskForCurrentPolicy(anyFocused);
    int cap = BgCpuCapPercent(anyFocused);
    bool rateCap = cap > 0;

    if (target == g_dynamicBgMask && rateCap == g_bgRateCap &&
        cap == g_dynamicCpuCap)
        return;

    if (!ApplyLimitsToJob(g_jobBackground, target, cap)) {
        if (LogRateLimit("jobs-refresh-fail", 60))
            LOGW("[Jobs] Failed to refresh background cgroup policy; retaining previous policy and retrying");
        return;
    }
    g_dynamicBgMask = target;
    g_bgRateCap = rateCap;
    g_dynamicCpuCap = cap;

    LOGI("[Jobs] Background job ceiling -> %s rewritten (CPU cap %d%%)",
         rateCap ? "all cores (CPU-capped)" : "all cores", cap);
}

void JobsSetFarmBoost(int on)
{
    g_farmBoostOn.store(on != 0, std::memory_order_relaxed);
    /* Caller (master.cpp) is responsible for calling JobsRefreshDynamic()
       right after this so the new mask/cap/priority are actually written
       to the job object - this function only flips the flag that the
       policy functions above read. */
}

void JobRelease(DWORD pid)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_pidJob.erase(pid);
}

void JobShutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_pidJob.clear();
    }
    if (g_iocpThread) {
        /* Stop the worker before closing the port it is waiting on. A null
           OVERLAPPED is an internal sentinel and is never interpreted as a
           process PID by IocpLoop. */
        if (g_iocp)
            PostQueuedCompletionStatus(g_iocp, 0, 0, nullptr);
        WaitForSingleObject(g_iocpThread, INFINITE);
        CloseHandle(g_iocpThread);
        g_iocpThread = nullptr;
    }
    if (g_iocp) {
        CloseHandle(g_iocp);
        g_iocp = nullptr;
    }
    if (g_jobBackground) {
        CloseHandle(g_jobBackground);
        g_jobBackground = nullptr;
    }
    g_dynamicBgMask = 0;
    g_bgRateCap = false;
    g_dynamicCpuCap = -1;
}
