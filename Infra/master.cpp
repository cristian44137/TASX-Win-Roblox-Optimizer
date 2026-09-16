#define NOMINMAX

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <string>

#include "WMI.h"
#include "CPU.h"
#include "trimmer.h"
#include "metrics.h"
#include "winhook.h"
#include "hotkey.h"
#include "config.h"
#include "ntsys.h"
#include "tweaks.h"
#include "fflags.h"
#include "jobs.h"
#include "stats.h"
#include "audio.h"
#include "warm.h"
#include "desktop.h"
#include "netcache.h"
#include "respawn.h"

#include "log.h"
#include "lograte.h"

#include <unordered_set>

/* ------------------------------------------------------------------ */
/* Event core - WMI, job completion port, foreground hook and the       */
/* low-memory reactor all feed one single-consumer loop, so process     */
/* state is touched by the main thread only.                            */
/* ------------------------------------------------------------------ */

namespace {

enum class Ev { RobloxCreated, RobloxExited, JobChild, Focus, LowMem, ConfigChanged, HotkeyBoost };

struct Event {
    Ev    kind;
    DWORD pid;
};

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<Event> g_queue;
constexpr size_t kMaxEventQueue = 16384;
std::atomic<bool> g_reconcileNeeded{false};
std::atomic<unsigned long> g_droppedQueueEvents{0};

void PushEvent(Event ev) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        /* Focus, low-memory and config notifications are level-triggered:
           one pending notification is enough, and callbacks can otherwise
           outrun the single-consumer loop during a busy desktop or a reload
           storm. */
        const bool coalescible = ev.kind == Ev::Focus ||
                                 ev.kind == Ev::LowMem ||
                                 ev.kind == Ev::ConfigChanged;
        if (coalescible) {
            for (auto it = g_queue.rbegin(); it != g_queue.rend(); ++it) {
                if (it->kind == ev.kind)
                    return;
            }
        } else if (ev.kind == Ev::RobloxCreated ||
                   ev.kind == Ev::RobloxExited ||
                   ev.kind == Ev::JobChild) {
            /* WMI and the job completion port may both report the same
               transition. Repeating the same PID/kind is idempotent; retain
               one pending event instead of allowing a process storm to grow
               the queue without bound. */
            for (auto it = g_queue.rbegin(); it != g_queue.rend(); ++it) {
                if (it->kind == ev.kind && it->pid == ev.pid)
                    return;
            }
        }

        /* A bounded queue is mandatory for a long-lived agent: a broken WMI
           provider or a callback storm must not turn event backlog into an
           unbounded heap allocation. Dropped lifecycle events are repaired
           by a process-table reconciliation on the main thread. */
        if (g_queue.size() >= kMaxEventQueue) {
            g_reconcileNeeded.store(true, std::memory_order_release);
            g_droppedQueueEvents.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        g_queue.push_back(ev);
    } catch (...) {
        /* PushEvent is called from COM, WinEvent, hotkey and IOCP callbacks.
           Allocation failure must never escape through one of those foreign
           callback boundaries and terminate TASX. */
        g_reconcileNeeded.store(true, std::memory_order_release);
        g_droppedQueueEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_queueCv.notify_one();
}

std::optional<Event> PopEvent(unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(g_queueMutex);
    if (!g_queueCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [] { return !g_queue.empty(); }))
        return std::nullopt;

    Event ev = g_queue.front();
    g_queue.pop_front();
    return ev;
}

} /* namespace */

/* Cross-module notifications (WMI sink / job IOCP / hook threads). */
void TasxNotifyProcess(unsigned long pid, int created)
{
    PushEvent({created ? Ev::RobloxCreated : Ev::RobloxExited, (DWORD)pid});
}

void TasxNotifyJobEvent(int kind, DWORD pid)
{
    PushEvent({kind == 1 ? Ev::RobloxExited : Ev::JobChild, pid});
}

void WinHook::NotifyFocusChanged()
{
    PushEvent({Ev::Focus, 0});
}

void TasxNotifyHotkey()
{
    PushEvent({Ev::HotkeyBoost, 0});
}

/* ------------------------------------------------------------------ */
/* Roblox instance bookkeeping                                          */
/* ------------------------------------------------------------------ */

namespace {

std::unordered_map<DWORD, HANDLE> g_rbxHandles;
std::unordered_map<DWORD, HANDLE> g_waitHandles; // RegisterWait handles
/* Commit-blocked spawns: pid -> next retry time (ms). The WMI creation
   event is already consumed when the block hits, so without this queue
   the client would be dropped forever. Retried on the main loop (no new
   threads); dropped silently if the process exits first. */
std::unordered_map<DWORD, ULONGLONG> g_retryBlocked;
DWORD g_appliedFocus = 0; /* PID currently running the focused profile */
std::atomic<bool> g_pageInBusy{false};
HANDLE g_pageInThread = nullptr;
ULONGLONG g_lastPageInMs = 0;
/* FarmBoost (Ctrl+Alt+B): mirrors the flag jobs.cc/trimmer.cc each keep
   their own copy of (so those modules don't need to depend on master.cpp);
   this one is only for master's own decisions (logging, startup default). */
std::atomic<bool> g_farmBoostOn{false};
WinHook* g_hook = nullptr;
FarmHotkey* g_hotkey = nullptr;
HANDLE g_singleInstanceMutex = nullptr;
bool g_wmiOk = false;

char g_iniPath[MAX_PATH] = {};
ULONGLONG g_iniMtime = 0;
volatile LONG g_stop = 0;
/* Set by the Ctrl handler; wakes the low-memory reactor so shutdown is
   prompt even during a long low-mem cooldown (no more Sleep(cd * 1000)). */
HANDLE g_stopEvent = nullptr;
HANDLE g_lowMemThread = nullptr;

ULONGLONG g_lastWsUpdateMs = 0;
ULONGLONG g_lastCrashSweepMs = 0;
ULONGLONG g_lastRetryMs = 0;
ULONGLONG g_lastIniCheckMs = 0;

/* Forward: defined below, used by RetryBlockedHooks. */
void ApplyFocusIfChanged();
void TimerOn();
void TimerOffIfFarmNoFocus();
void HandleRobloxExited(DWORD pid);
void SweepTrackedClients();

void ApplyPerProcessAffinityPolicy()
{
    unsigned long long allMask = tasx_get_all_mask();
    unsigned long long eMask = tasx_get_ecore_mask();
    if (!allMask) return;

    bool split = g_appliedFocus != 0 &&
                 config_get_bool("TASX", "DynamicAffinity", 0) &&
                 !g_farmBoostOn.load() && eMask;
    for (auto& kv : g_rbxHandles) {
        DWORD_PTR target = (split && kv.first != g_appliedFocus)
            ? (DWORD_PTR)eMask : (DWORD_PTR)allMask;
        SetProcessAffinityMask(kv.second, target);
    }
}

BOOL WINAPI CtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&g_stop, 1);
        if (g_stopEvent) SetEvent(g_stopEvent);
        return TRUE;
    }
    return FALSE;
}

bool NameContains(const std::wstring& full, const wchar_t* needle)
{
    std::wstring hay = full, ndl = needle;
    for (auto& c : hay) c = (wchar_t)towlower(c);
    for (auto& c : ndl) c = (wchar_t)towlower(c);
    return hay.find(ndl) != std::wstring::npos;
}

bool IsRobloxName(const std::wstring& name)
{
    return NameContains(name, L"robloxplayerbeta.exe") ||
           name == L"Roblox.exe" || NameContains(name, L"\\roblox.exe");
}

std::unordered_set<DWORD> ClientPidSet()
{
    std::unordered_set<DWORD> pids;
    pids.reserve(g_rbxHandles.size());
    for (auto& kv : g_rbxHandles) pids.insert(kv.first);
    return pids;
}

ULONGLONG IniMtimeKey()
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad))
        return 0;
    return ((ULONGLONG)fad.ftLastWriteTime.dwLowDateTime) |
           ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32);
}

VOID CALLBACK OnProcessExit(PVOID lpParam, BOOLEAN)
{
    DWORD pid = (DWORD)(ULONG_PTR)lpParam;
    PushEvent({Ev::RobloxExited, pid});
}

void RegisterProcessWait(DWORD pid, HANDLE hProc)
{
    HANDLE hWait = nullptr;
    if (!RegisterWaitForSingleObject(&hWait, hProc, OnProcessExit,
                                     (PVOID)(ULONG_PTR)pid, INFINITE,
                                     WT_EXECUTEONLYONCE)) {
        if (LogRateLimit("register-process-wait", 60))
            LOGW("[TASX] Exit notification registration failed for PID %lu (err %lu); periodic liveness sweep remains active",
                 pid, (unsigned long)GetLastError());
        return;
    }
    // store for UnregisterWaitEx on clean exit
    auto it = g_waitHandles.find(pid);
    if (it != g_waitHandles.end()) {
        UnregisterWaitEx(it->second, nullptr);
    }
    g_waitHandles[pid] = hWait;
}

void UnregisterProcessWait(DWORD pid)
{
    auto it = g_waitHandles.find(pid);
    if (it != g_waitHandles.end()) {
        UnregisterWaitEx(it->second, INVALID_HANDLE_VALUE);
        g_waitHandles.erase(it);
    }
}

/* Event-driven hook of a single client (WMI event or sweep discovery). */
void HookClient(DWORD pid)
{
    if (g_rbxHandles.count(pid)) return;

    // Commit charge guard: defer (not drop) new spawns past the threshold.
    int blockPct = config_get_int("TASX", "CommitBlockThreshold", 85);
    if (blockPct <= 0) blockPct = 85;
    int curPct = tasx_get_commit_percent();
    if (curPct >= 0 && curPct >= blockPct) {
        if (!g_retryBlocked.count(pid) && LogRateLimit("commit-block", 60))
            LOGW("[TASX] Commit charge %d%%, spawn of PID %lu deferred (threshold %d%%) - retry in ~5s",
                 curPct, pid, blockPct);
        g_retryBlocked[pid] = GetTickCount64() + 5000;
        return;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA |
        PROCESS_SET_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE |
        PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) {
        LOGW("[TASX] Failed to open PID %lu", pid);
        return;
    }

    g_rbxHandles[pid] = hProc;

    // Respawn support: grab the launch command line while we hold a query
    // handle (one NtQueryInformationProcess syscall per client hook).
    {
        wchar_t cmd[2048];
        if (tasx_read_cmdline(hProc, cmd, 2048))
            RespawnRemember(pid, std::wstring(cmd));
    }

    // Register wait for exit notification (zero polling)
    RegisterProcessWait(pid, hProc);

    bool inJob = JobHookProcess(pid, hProc);
    if (inJob) {
        /* Job Object carries affinity/caps only. Apply priority per process
           so this client can later become HIGH when focused. */
        CpuApplyFocusProfile(hProc, 0);
        tasx_process_power_throttling(hProc, 1); // background EcoQoS
        tasx_set_io_priority(hProc, 0);
        tasx_set_memory_priority(hProc, 1);
        LOGI("[TASX] New Roblox instance PID %lu hooked (job-tracked)", pid);
    }
    else {
        CpuApplyFocusProfile(hProc, 0);
        LOGI("[TASX] New Roblox instance PID %lu hooked (per-process fallback)", pid);
    }

    /* A newly hooked client must receive the current per-process affinity
       policy even though it was admitted to the all-core shared job. */
    if (g_appliedFocus && config_get_bool("TASX", "DynamicAffinity", 0) &&
        !g_farmBoostOn.load()) {
        unsigned long long eMask = tasx_get_ecore_mask();
        if (eMask) SetProcessAffinityMask(hProc, (DWORD_PTR)eMask);
    }

    StartTrimmer(pid, hProc);
    WarmClientFilesAsync(pid);
}

/* Retry commit-deferred spawns (~5s cadence, on the existing main loop).
   A retried client gets the FULL hook once commit drops below the
   threshold; PIDs that exited meanwhile are dropped silently. */
void RetryBlockedHooks()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastRetryMs < 5000 || g_retryBlocked.empty())
        return;
    g_lastRetryMs = now;

    int blockPct = config_get_int("TASX", "CommitBlockThreshold", 85);
    if (blockPct <= 0) blockPct = 85;
    int curPct = tasx_get_commit_percent();

    std::vector<DWORD> due;
    due.reserve(g_retryBlocked.size());
    for (auto& kv : g_retryBlocked) {
        if (now >= kv.second)
            due.push_back(kv.first);
    }
    for (DWORD pid : due) {
        g_retryBlocked.erase(pid);
        if (g_rbxHandles.count(pid))
            continue;
        HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!probe)
            continue; /* exited while deferred - silent drop */
        CloseHandle(probe);
        if (curPct >= 0 && curPct >= blockPct) {
            g_retryBlocked[pid] = now + 5000; /* still choking, stay queued */
            continue;
        }
        bool wasEmpty = g_rbxHandles.empty();
        HookClient(pid);
        if (g_rbxHandles.count(pid)) {
            ApplyFocusIfChanged();
            if (wasEmpty) {
                TweaksPowerEnter();
                TimerOn();
            }
        }
    }
}

void ReleaseClient(DWORD pid, bool logExit)
{
    g_retryBlocked.erase(pid); /* deferred spawn that never hooked: silent */

    auto it = g_rbxHandles.find(pid);
    if (it == g_rbxHandles.end()) return;

    if (logExit)
        LOGI("[TASX] Roblox PID %lu exited.", pid);

    UnregisterProcessWait(pid);
    StopTrimmer(pid);
    TrimmerRemoveWorkingSet(pid);
    JobRelease(pid);
    DesktopForgetClient(pid);
    AudioForgetPid(pid);
    CloseHandle(it->second);
    g_rbxHandles.erase(it);

    if (g_appliedFocus == pid) {
        g_appliedFocus = 0;
        TrimmerSetFocused(0);
        // The PID is dead - its WASAPI sessions are already gone.
        // No audio API call; just drop focus/mute bookkeeping (above).
    }
}

void KillCrashHandlerPid(DWORD pid)
{
    std::wstring name = QueryProcessNameByPid(pid);
    if (!NameContains(name, L"robloxcrashhandler.exe")) return;

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
        LOGI("[TASX] Killed RobloxCrashHandler PID %lu", pid);
    }
}

void SweepCrashHandlers()
{
    if (!config_get_bool("TASX", "KillCrashHandler", 1)) return;

    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;

    for (const auto& s : stats)
        if (s.name.size() && NameContains(s.name, L"robloxcrashhandler.exe"))
            KillCrashHandlerPid(s.pid);
}

/* Hung-client watchdog (opt-in, default OFF - only logs). IsHungAppWindow
   reports TRUE when a top-level window's message pump has not answered for
   ~5s; farm clients can legitimately block during asset loads, so a bogus
   kill would be worse than a hung client. HungKill=1 terminates the hung
   process - the normal exit event then feeds auto-respawn, so a relaunch
   only happens when RespawnOnCrash is on (loop protection is the respawn
   hourly caps). */
typedef std::unordered_map<DWORD, int> HungMap;

BOOL CALLBACK HungEnumProc(HWND hwnd, LPARAM lp)
{
    auto* hung = (HungMap*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    auto it = hung->find(pid);
    if (it == hung->end())
        it = hung->emplace(pid, 0).first;
    if (IsHungAppWindow(hwnd))
        it->second += 1;
    return TRUE;
}

/* pid -> tick when the client first looked unresponsive. Persisted across
   sweeps (HungSweep runs on the ~10s tick): this is what makes HungKill a
   "kill after HungKillAfterSecMin of CONTINUOUS unresponsiveness" policy
   instead of "kill on the first ~5s message-pump stall". */
static std::unordered_map<DWORD, ULONGLONG> g_hungSince;

void HungSweep()
{
    if (!config_get_bool("TASX", "HungClientWatch", 0)) {
        g_hungSince.clear();
        return;
    }
    if (g_rbxHandles.empty()) {
        g_hungSince.clear();
        return;
    }

    HungMap hung;
    EnumWindows(HungEnumProc, (LPARAM)&hung);

    bool kill = config_get_bool("TASX", "HungKill", 0) != 0;
    /* Minutes of continuous unresponsiveness before HungKill actually fires;
       values below 1 min would defeat the dwell, so they clamp to 1. */
    int killAfterMin = config_get_int("TASX", "HungKillAfterSecMin", 60);
    if (killAfterMin < 1) killAfterMin = 1;
    ULONGLONG now = GetTickCount64();

    for (const auto& kv : hung) {
        if (!g_rbxHandles.count(kv.first)) continue; /* window not ours */

        if (kv.second == 0) {
            g_hungSince.erase(kv.first); /* pumped again - reset the dwell */
            continue;
        }

        auto ins = g_hungSince.emplace(kv.first, now);
        unsigned long heldSec = (unsigned long)((now - ins.first->second) / 1000);
        bool killNow = kill && heldSec >= (unsigned long)killAfterMin * 60;

        if (LogRateLimit("hung-client", 300))
            LOGW("[TASX] Client PID %lu appears hung (%d unresponsive window(s)) for %lus%s",
                 kv.first, kv.second, heldSec,
                 kill ? (killNow ? " - killing (auto-respawn follows when enabled)"
                                 : " - waiting out HungKillAfterSecMin")
                      : "");
        if (killNow) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, kv.first);
            if (h) {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
            g_hungSince.erase(kv.first);
        }
    }

    /* Drop bookkeeping for clients we no longer track (released or respawned
       under a new PID) so the map cannot grow without bound. */
    for (auto it = g_hungSince.begin(); it != g_hungSince.end();) {
        if (!g_rbxHandles.count(it->first)) it = g_hungSince.erase(it);
        else ++it;
    }
}

/* Fallback via Toolhelp snapshot (only if WMI down) */
void SweepDiscoverViaSnapshot()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            if (!IsRobloxName(name)) continue;
            DWORD pid = pe.th32ProcessID;
            if (g_rbxHandles.count(pid)) continue;
            if (g_retryBlocked.count(pid)) continue; // owned by the retry queue
            // commit guard inside HookClient
            HookClient(pid);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

/* Discovery fallback (WMI down / missed events): one syscall, no snapshots ideally. */
void SweepDiscover()
{
    if (!g_wmiOk) {
        // Primary fallback is snapshot per spec when WMI dead
        SweepDiscoverViaSnapshot();
        return;
    }
    // WMI healthy: use single-syscall table (lighter than snapshot)
    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;

    for (const auto& s : stats) {
        if (!s.name.size() || !IsRobloxName(s.name)) continue;
        if (g_rbxHandles.count(s.pid)) continue;
        if (g_retryBlocked.count(s.pid)) continue;
        HookClient(s.pid);
    }
}

ULONGLONG g_farmWsBytes = 0; /* sum of tracked clients WS, refreshed ~10s */

void UpdateWorkingSetCache()
{
    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;
    ULONGLONG sum = 0;
    for (const auto& s : stats) {
        if (g_rbxHandles.count(s.pid)) {
            TrimmerUpdateWorkingSet(s.pid, s.workingSet);
            sum += (ULONGLONG)s.workingSet;
        }
    }
    g_farmWsBytes = sum;
    g_lastWsUpdateMs = GetTickCount64();
}

/* Farm RAM budget honesty check: keep-hot is physically impossible when the
   farm working set alone exceeds ~75% of installed RAM - paging is then
   inevitable and the log should say so instead of silently trimming. */
static void FarmBudgetCheck()
{
    if (g_rbxHandles.empty()) return;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms) || !ms.ullTotalPhys) return;
    double frac = (double)g_farmWsBytes / (double)ms.ullTotalPhys;
    if (frac > 0.75) {
        if (LogRateLimit("farm-budget", 600)) {
            double farmGB = (double)g_farmWsBytes / (1ull << 30);
            double totalGB = (double)ms.ullTotalPhys / (1ull << 30);
            LOGW("[TASX] WARNING: farm WS %.1f/%.1f GB RAM (>75%%) - keep-hot impossible, paging inevitable: reduce client count or add RAM",
                 farmGB, totalGB);
        }
    }
}

static int GetFocusDwellMs()
{
    // primary: FocusDwellMs, fallback: FocusHysteresisMs for compat, default 1800
    const char* dwellStr = config_get_str("TASX", "FocusDwellMs", nullptr);
    if (dwellStr)
        return config_get_int("TASX", "FocusDwellMs", 1800);
    const char* hyst = config_get_str("TASX", "FocusHysteresisMs", nullptr);
    if (hyst)
        return config_get_int("TASX", "FocusHysteresisMs", 1800);
    return 1800;
}

void ApplyFocusIfChanged()
{
    HWND hwnd = GetForegroundWindow();
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);

    bool isRoblox = pid != 0 && g_rbxHandles.count(pid) != 0;
    ULONGLONG now = GetTickCount64();
    int dwellMs = GetFocusDwellMs();
    if (dwellMs < 100) dwellMs = 100;
    if (dwellMs > 10000) dwellMs = 10000;

    DWORD candidate = isRoblox ? pid : 0;

    // Dwell/anti-flap: pending coalescing state
    static DWORD s_pendingPid = 0;
    static ULONGLONG s_pendingSince = 0;
    static bool s_hasPending = false;

    if (candidate == g_appliedFocus) {
        // stable state - clear pending (no switch needed)
        s_hasPending = false;
    } else if (g_appliedFocus == 0 && candidate != 0) {
        // Legacy bypass restored: very first focus after startup is instant
        // (old FocusHysteresisMs behavior). Dwell applies only to later switches.
        s_hasPending = false;
        // fallthrough to apply below
    } else {
        // need to switch to candidate, but must dwell
        if (s_hasPending && s_pendingPid == candidate) {
            if (now - s_pendingSince < (ULONGLONG)dwellMs) {
                return; // still dwelling, ignore flap
            }
            // dwell elapsed -> commit
            s_hasPending = false;
        } else if (s_hasPending && s_pendingPid != candidate) {
            // flap: candidate changed again before dwell elapsed - coalesce, restart dwell
            s_pendingPid = candidate;
            s_pendingSince = now;
            return;
        } else {
            s_hasPending = true;
            s_pendingPid = candidate;
            s_pendingSince = now;
            return;
        }
    }
    // dwell satisfied - proceed to apply candidate (which equals isRoblox?pid:0)

    DWORD focusBefore = g_appliedFocus;

    // Focus changes use all-cores profile (always P+E)
    if (isRoblox && g_appliedFocus != pid)
    {
        DWORD oldFocus = g_appliedFocus;
        if (oldFocus) {
            auto old = g_rbxHandles.find(oldFocus);
            if (old != g_rbxHandles.end()) {
                LOGI("[TASX] Roblox PID %lu lost focus", oldFocus);
                if (!JobApplyProfile(oldFocus, 0))
                    CpuApplyFocusProfile(old->second, 0);
                AudioMuteByPid(oldFocus);
            }
        }

        LOGI("[TASX] Roblox PID %lu in focus", pid);
        auto cur = g_rbxHandles.find(pid);
        if (cur != g_rbxHandles.end()) {
            if (!JobApplyProfile(pid, 1))
                CpuApplyFocusProfile(cur->second, 1);
        }
        AudioUnmuteByPid(pid);
        TrimmerSetFocused(pid);
        g_appliedFocus = pid;
        TimerOn();
    }
    else if (!isRoblox && g_appliedFocus)
    {
        DWORD oldFocus = g_appliedFocus;
        auto old = g_rbxHandles.find(oldFocus);
        if (old != g_rbxHandles.end()) {
            LOGI("[TASX] Roblox PID %lu lost focus (other app)", oldFocus);
            if (!JobApplyProfile(oldFocus, 0))
                CpuApplyFocusProfile(old->second, 0);
            AudioMuteByPid(oldFocus);
        }
        TrimmerSetFocused(0);
        g_appliedFocus = 0;
        TimerOffIfFarmNoFocus();
    }

    // Rewrite the dynamic job profile only when the focus STATE
    // (focused / not focused) actually changed, not on every 250 ms tick.
    // FarmBoost counts as focused (farm runs all-cores, rate-cap policy flips).
    static int lastAppliedFocusState = -1;
    int currentState = (g_appliedFocus != 0) ? 1 : 0;
    if (currentState != lastAppliedFocusState || focusBefore != g_appliedFocus) {
        JobsRefreshDynamic(currentState);
        ApplyPerProcessAffinityPolicy();
        lastAppliedFocusState = currentState;
    }
}

/* Page-in hotkey: pull all tracked clients out of pagefile.
   Always keeps P+E affinity; never changes CPU profile or trims. */
/* Parallel page-in with a fixed worker ceiling. The old implementation
   created one Windows thread per client, so a large farm could turn a
   manual optimization into a thread/I/O storm. */
struct PageInClientCtx {
    HANDLE h;
    CpuPageInStats st{};
};

struct PageInBatchCtx {
    PageInClientCtx* clients;
    size_t count;
    size_t first;
    size_t stride;
};

static DWORD WINAPI PageInWorker(LPVOID arg)
{
    auto* batch = static_cast<PageInBatchCtx*>(arg);
    try {
        for (size_t i = batch->first; i < batch->count; i += batch->stride)
            CpuPageInProcess(batch->clients[i].h, &batch->clients[i].st);
    } catch (const std::exception& ex) {
        LOGW("[TASX] Page-in worker exception: %s; remaining clients in this batch skipped",
             ex.what());
    } catch (...) {
        LOGW("[TASX] Page-in worker exception; remaining clients in this batch skipped");
    }
    return 0;
}

/* Handoff payload from the main thread to the page-in worker thread. Every
   HANDLE in here is TASX's own duplicate, owned solely by the worker - the
   worker closes every one of them, so it never touches g_rbxHandles or
   anything else the main thread owns. */
struct PageInJob {
    std::vector<std::pair<DWORD, HANDLE>> clients;
    ~PageInJob()
    {
        for (auto& client : clients) {
            if (client.second) {
                CloseHandle(client.second);
                client.second = nullptr;
            }
        }
    }
};

struct PageInStateGuard {
    ~PageInStateGuard()
    {
        TrimmerSetPageInPause(0);
        g_pageInBusy.store(false);
    }
};

/* Runs entirely off the main thread: this is the part that used to block
   the event loop. Cleans up after itself (closes duplicated handles,
   un-pauses the trimmer, clears g_pageInBusy).  The thread handle is retained
   and joined during teardown. */
static DWORD WINAPI PageInFarmWorkerMain(LPVOID arg)
{
    std::unique_ptr<PageInJob> job(static_cast<PageInJob*>(arg));
    PageInStateGuard stateGuard;
    try {
    ULONGLONG t0 = GetTickCount64();

    std::vector<PageInClientCtx> ctxs;
    ctxs.reserve(job->clients.size());
    for (auto& p : job->clients) ctxs.push_back({p.second, {}});

    constexpr size_t kMaxPageInWorkers = 4;
    const size_t workerCount = std::min(kMaxPageInWorkers, ctxs.size());
    std::vector<PageInBatchCtx> batches;
    batches.reserve(workerCount);
    std::vector<HANDLE> threads;
    threads.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        batches.push_back({ctxs.data(), ctxs.size(), i, workerCount});
        HANDLE th = CreateThread(nullptr, 0, PageInWorker, &batches.back(), 0, nullptr);
        if (th) threads.push_back(th);
    }
    for (HANDLE th : threads) {
        /* CpuPageInProcess owns the duplicated handle through this job.  Do
           not time out and then destroy the worker context: that would leave
           a still-running thread with a dangling ctx pointer. */
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    }

    unsigned clientsDone = 0;
    unsigned long long totalAttempted = 0, totalTouched = 0, totalFailed = 0, totalBytes = 0;
    for (size_t i = 0; i < job->clients.size(); ++i) {
        clientsDone++;
        totalAttempted += ctxs[i].st.attemptedPages;
        totalTouched += ctxs[i].st.touchedPages;
        totalFailed += ctxs[i].st.failedPages;
        totalBytes += ctxs[i].st.bytesTouched;
        LOGI("[TASX] Page-in PID %lu: %llu pages touched, %llu failed | ~%llu KB",
             job->clients[i].first,
             (unsigned long long)ctxs[i].st.touchedPages,
             (unsigned long long)ctxs[i].st.failedPages,
             (unsigned long long)(ctxs[i].st.bytesTouched >> 10));
        CloseHandle(ctxs[i].h); // always our own duplicate - always ours to close
        ctxs[i].h = nullptr;
        job->clients[i].second = nullptr;
    }

    ULONGLONG t1 = GetTickCount64();
    LOGI("[TASX] Page-in complete (%lu ms): %u clients | %llu pages attempted, %llu touched, %llu failed | ~%llu MB fetched",
         (unsigned long)(t1 - t0), clientsDone,
         (unsigned long long)totalAttempted,
         (unsigned long long)totalTouched,
         (unsigned long long)totalFailed,
         (unsigned long long)(totalBytes >> 20));

    } catch (const std::exception& ex) {
        LOGW("[TASX] Page-in farm worker exception: %s; operation aborted safely", ex.what());
    } catch (...) {
        LOGW("[TASX] Page-in farm worker exception; operation aborted safely");
    }
    return 0;
}

void PageInFarm()
{
    /* Reap a completed page-in worker before starting another one.
       The main loop is the sole owner of this thread handle. */
    if (g_pageInThread && !g_pageInBusy.load()) {
        WaitForSingleObject(g_pageInThread, INFINITE);
        CloseHandle(g_pageInThread);
        g_pageInThread = nullptr;
    }

    bool expected = false;
    if (!g_pageInBusy.compare_exchange_strong(expected, true)) {
        LOGI("[TASX] Page-in already running (hotkey ignored)");
        return;
    }

    /* The only part that touches g_rbxHandles - runs on the main thread,
       which is the sole owner, exactly like everywhere else in this file.
       We duplicate every handle so the worker thread never depends on the
       main thread's copy staying alive/valid. */
    auto job = std::make_unique<PageInJob>();
    job->clients.reserve(g_rbxHandles.size());
    for (auto& kv : g_rbxHandles) {
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), kv.second, GetCurrentProcess(),
                            &dup, 0, FALSE, DUPLICATE_SAME_ACCESS) && dup)
            job->clients.push_back({kv.first, dup});
    }

    if (job->clients.empty()) {
        g_pageInBusy.store(false);
        LOGI("[TASX] Page-in: no clients to touch");
        return;
    }

    TrimmerSetPageInPause(1);

    PageInJob* raw = job.release();
    HANDLE th = CreateThread(nullptr, 0, PageInFarmWorkerMain, raw, 0, nullptr);
    if (!th) {
        LOGE("[TASX] Page-in: failed to start worker thread");
        delete raw;
        TrimmerSetPageInPause(0);
        g_pageInBusy.store(false);
        return;
    }
    g_pageInThread = th; // main thread reaps it on next run or shutdown
}

void PageInFarmStop()
{
    if (!g_pageInThread) return;
    /* The worker owns only duplicated process handles, but it still logs and
       clears the page-in/trimmer state. Join it before those dependencies are
       torn down and before the logger is closed. */
    WaitForSingleObject(g_pageInThread, INFINITE);
    CloseHandle(g_pageInThread);
    g_pageInThread = nullptr;
}

/* FarmBoost (Ctrl+Alt+B): toggles the whole background cgroup (all tracked,
   non-focused clients) between the normal IDLE/E-cores/EcoQoS-on policy and
   a full-power NORMAL/all-cores/EcoQoS-off/trim-paused one. The focused
   instance (if any) is untouched either way - it already runs full power
   via CpuApplyFocusProfile. */
void ToggleFarmBoost()
{
    bool on = !g_farmBoostOn.load();
    g_farmBoostOn.store(on);

    JobsSetFarmBoost(on ? 1 : 0);
    TrimmerSetFarmBoost(on ? 1 : 0);

    // EcoQoS is per-process, so flip it for every job-tracked background
    // client here (jobs.cc only tracks pid->state, not handles).
    for (auto& kv : g_rbxHandles) {
        if (kv.first == g_appliedFocus) continue; // focused instance untouched
        SetPriorityClass(kv.second, on ? NORMAL_PRIORITY_CLASS : IDLE_PRIORITY_CLASS);
        tasx_process_power_throttling(kv.second, on ? 0 : 1);
    }

    JobsRefreshDynamic(g_appliedFocus != 0);
    ApplyPerProcessAffinityPolicy();

    LOGI("[TASX] FarmBoost %s (Ctrl+Alt+B): background clients now %s",
         on ? "ON" : "OFF",
         on ? "NORMAL priority / all cores / EcoQoS off / trim paused"
            : "IDLE priority / E-cores (or low half) / EcoQoS on / trim active");
}

/* Timer resolution: 0.5 ms tick while Roblox is running -> smoother frame
   pacing and snappier input scheduling. State-guarded: prints/acts only on
   the on/off transition. */
bool g_timerActive = false;

/* Farm preset detection (single place): farm15 is a deprecated alias of
   farm20; weak/balanced are the other TASX-owned-graphics presets. */
static bool IsFarmPreset()
{
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    if (!pPre) return false;
    char pl[16] = {};
    size_t pn = 0;
    for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn)
        pl[pn] = (char)tolower((unsigned char)pPre[pn]);
    pl[pn] = '\0';
    return (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
            strcmp(pl, "farm30") == 0 || strcmp(pl, "weak") == 0 ||
            strcmp(pl, "balanced") == 0);
}

void TimerOffIfFarmNoFocus()
{
    // disable 0.5ms timer when farm preset, nothing focused
    if (IsFarmPreset() && g_appliedFocus == 0 && g_timerActive) {
        unsigned long actual = 0;
        tasx_set_timer_resolution(5000, 0, &actual);
        g_timerActive = false;
        // Do NOT log on every toggle to avoid spam (timer off is silent/good)
    }
}

void TimerOn()
{
    if (g_timerActive) return;
    // If farm preset and nothing focused, keep timer off (user directive)
    if (IsFarmPreset() && g_appliedFocus == 0) return; // no timer for pure farm
    unsigned long actual = 0;
    if (config_get_bool("TASX", "TimerResolution", 1) &&
        tasx_set_timer_resolution(5000, 1, &actual)) {
        g_timerActive = true;
        LOGI("[TASX] Timer resolution set to %lu us", actual / 10);
    }
}

void TimerOff()
{
    if (!g_timerActive) return;
    g_timerActive = false;

    unsigned long actual = 0;
    tasx_set_timer_resolution(5000, 0, &actual);
    LOGI("[TASX] Timer resolution released");
}

/* Full-system memory pass, split in two halves (farm keep-hot):
   - standby purge: cheap page-cache reclaim, safe for farms (default ON);
   - empty every working set: evicts ALL processes incl. farm clients,
     the direct cause of "slow return from swap" (default OFF on farm*).
   Needs elevation; without admin this is a silent no-op (the limited-mode
   banner at startup already told the user). */
static bool IsFarmStrict()
{
    // Strict farm presets only (farm15/20/30): keep-hot farms where a global
    // empty-WS is pure harm. weak/balanced keep legacy default (ON).
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    if (!pPre) return false;
    char pl[16] = {};
    size_t pn = 0;
    for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn)
        pl[pn] = (char)tolower((unsigned char)pPre[pn]);
    pl[pn] = '\0';
    return (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
            strcmp(pl, "farm30") == 0);
}

static bool SystemCleanEmptyWSAllowed()
{
    const char* v = config_get_str("TASX", "SystemCleanEmptyWS", nullptr);
    if (!v) return IsFarmStrict() ? false : true;
    return config_get_bool("TASX", "SystemCleanEmptyWS", 0) != 0;
}

void SystemCleanPass(bool announce)
{
    if (!config_get_bool("TASX", "SystemCleaner", 0)) return;
    if (!tasx_is_elevated()) return;

    bool standby = false, emptied = false;
    if (config_get_bool("TASX", "SystemCleanStandby", 0))
        standby = tasx_purge_standby_list() != 0;
    // Global empty-WS never touches a focused client and never runs on a
    // keep-hot farm unless explicitly opted in (SystemCleanEmptyWS=1).
    if (SystemCleanEmptyWSAllowed() && g_appliedFocus == 0)
        emptied = tasx_empty_working_sets_system() != 0;

    if (announce && (standby || emptied))
        LOGI("[TASX] System clean: standby %s%s",
             standby ? "purged" : "skipped",
             emptied ? " + all working sets emptied" : "");
}

/* Kernel wakes us only when commit memory runs low -> no cleaning timer. */
void LowMemReactorStart()
{
    if (!config_get_bool("TASX", "LowMemReactor", 0)) return;
    if (!g_stopEvent) {
        LOGW("[TASX] Low-memory reactor disabled: shutdown event unavailable");
        return;
    }
    if (g_lowMemThread) return;

    g_lowMemThread = CreateThread(nullptr, 0, [](LPVOID)->DWORD {
        HANDLE hLow = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        if (!hLow) {
            LOGW("[TASX] Low-memory notification unavailable");
            return 0;
        }
        // The notification is manual-reset and stays signaled while memory
        // is low - debounce so the event queue is not flooded. The cooldown
        // wait is on g_stopEvent too, so Ctrl+C exits promptly instead of
        // blocking up to 300 s in Sleep().
        HANDLE hs[2] = { hLow, g_stopEvent };
        while (true) {
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0)
                break; /* stop requested */
            PushEvent({Ev::LowMem, 0});
            int cd = config_get_int("TASX", "LowMemCooldownSec", 30);
            if (cd < 5) cd = 5;
            if (cd > 300) cd = 300;
            if (WaitForSingleObject(g_stopEvent, (DWORD)cd * 1000) == WAIT_OBJECT_0)
                break;
        }
        CloseHandle(hLow);
        return 0;
    }, nullptr, 0, nullptr);
    if (!g_lowMemThread)
        LOGW("[TASX] Failed to start low-memory reactor thread");
}

void LowMemReactorStop()
{
    if (!g_lowMemThread) return;
    if (g_stopEvent) SetEvent(g_stopEvent);
    /* The reactor owns hLow and snapshots g_stopEvent into its wait set.
       Join before closing either handle or shutting down the logger. */
    WaitForSingleObject(g_lowMemThread, INFINITE);
    CloseHandle(g_lowMemThread);
    g_lowMemThread = nullptr;
}

void CheckIniReload()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastIniCheckMs < 10000)
        return;
    g_lastIniCheckMs = now;
    ULONGLONG mtime = IniMtimeKey();
    if (mtime && mtime != g_iniMtime)
        PushEvent({Ev::ConfigChanged, 0});
}

void HandleConfigChanged()
{
    g_iniMtime = IniMtimeKey();
    config_reload(g_iniPath);
    tasx_log_configure(config_get_str("Log", "LogFile", ""),
                       tasx_log_level_from_str(config_get_str("Log", "LogLevel", "info")));
    tasx_log_set_timestamps(config_get_bool("Log", "LogTimestamps", 1));
    if (config_truncated())
        LOGW("[TASX] WARNING: %s holds more keys than the %d-entry table - keys past the limit were ignored",
             g_iniPath, config_max_entries());
    LOGI("[TASX] Config reloaded: %s", g_iniPath);
    /* Idempotent re-applies; trimmer thresholds are picked up lazily by
       IntervalMs()/BelowSkipThresholdCached on the next deadline. */
    TimerOffIfFarmNoFocus();
    FFlagsMarkDirty();
    FFlagsApply();
    TweaksApplyOneShot();
    JobsRefreshDynamic(g_appliedFocus != 0);
    ApplyPerProcessAffinityPolicy();
}

static std::wstring GetPagefileVolumeRoot()
{
    wchar_t sysDrive[16] = L"C:";
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
            0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
        wchar_t buf[512] = {};
        DWORD type = 0, sz = sizeof(buf);
        if (RegQueryValueExW(h, L"PagingFiles", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
            // REG_MULTI_SZ: first string is e.g. "C:\\pagefile.sys 0 0"
            if (buf[1] == L':' && (buf[2] == L'\\' || buf[2] == L'/')) {
                sysDrive[0] = buf[0];
                sysDrive[1] = L':';
                sysDrive[2] = L'\0';
            }
        }
        RegCloseKey(h);
    } else {
        wchar_t env[16] = {};
        if (GetEnvironmentVariableW(L"SystemDrive", env, 16) && env[1] == L':') {
            sysDrive[0] = env[0];
            sysDrive[1] = L':';
            sysDrive[2] = L'\0';
        }
    }
    std::wstring root(sysDrive);
    if (root.size() == 2) root += L"\\";
    else if (root.back() != L'\\') root += L"\\";
    return root;
}

static void CheckPagefileWarning(const char* tag)
{
    int threshGB = config_get_int("TASX", "PagefileWarnFreeGB", 8);
    if (threshGB <= 0) return;
    std::wstring root = GetPagefileVolumeRoot();
    ULARGE_INTEGER freeBytes{}, totalBytes{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, nullptr)) {
        // fallback to NULL (current volume)
        if (!GetDiskFreeSpaceExW(nullptr, &freeBytes, &totalBytes, nullptr))
            return;
        root = L"?\\";
    }
    ULONGLONG thresh = (ULONGLONG)threshGB << 30;
    if (freeBytes.QuadPart < thresh) {
        if (LogRateLimit(tag, 300)) {
            char driveA[8] = "?";
            if (!root.empty() && root[0] != L'?') {
                driveA[0] = (char)root[0];
                driveA[1] = ':';
                driveA[2] = '\0';
            }
            double f = (double)freeBytes.QuadPart / (1ull<<30);
            double t = (double)totalBytes.QuadPart / (1ull<<30);
            LOGW("[TASX] WARNING: Pagefile volume %s low free %.1f/%.1f GB (< %d GB) - enlarge/move the pagefile: System -> Advanced settings -> Performance -> Advanced -> Virtual memory", driveA, f, t, threshGB);
        }
    }
}

void HandleRobloxCreated(DWORD pid)
{
    CheckPagefileWarning("pagefile-low-boot");
    bool isNew = !g_rbxHandles.count(pid);
    HookClient(pid);
    if (g_rbxHandles.count(pid))
        g_retryBlocked.erase(pid); /* hooked (possibly via retry) - dequeue */
    ApplyFocusIfChanged(); /* the new instance may already be the foreground */

    if (isNew && g_rbxHandles.count(pid)) {
        if (tasx_is_elevated() && config_get_bool("TASX", "PurgeStandbyOnLaunch", 0)) {
            // Debounced: a 30-client pack spawns 30 events, one purge is enough.
            static ULONGLONG lastLaunchPurgeMs = 0;
            ULONGLONG nowPurge = GetTickCount64();
            if (nowPurge - lastLaunchPurgeMs >= 60000) {
                lastLaunchPurgeMs = nowPurge;
                if (tasx_purge_standby_list())
                    LOGI("[TASX] Standby list purged (RAM reclaimed for the game)");
            }
        }
        FFlagsMarkDirty();
        SweepCrashHandlers();
        FFlagsApply();
        UpdateWorkingSetCache();
    }

    TweaksPowerEnter();
    TimerOn();
}

void HandleRobloxExited(DWORD pid)
{
    /* WMI, Job Object and RegisterWait can all report the same exit. Only
       the first event that still owns the PID may trigger cleanup/respawn;
       this makes the operation idempotent and prevents duplicate launches. */
    if (!g_rbxHandles.count(pid))
        return;

    ReleaseClient(pid, true);
    // commit block auto-unblocks on next hook due to fresh check

    if (g_rbxHandles.empty()) {
        TimerOff();
        TweaksPowerExit();
    }

    // Farm auto-respawn (opt-in). Runs on the existing event - the new
    // process is picked up by WMI and gets the full hook automatically.
    RespawnTrySpawn(pid);
}

/* RegisterWait is the normal exit path, but it can fail under handle/thread
   pressure. A cheap readiness check on the owned process handles is the
   final liveness backstop and also repairs a lost WMI/job notification. */
void SweepTrackedClients()
{
    std::vector<DWORD> exited;
    exited.reserve(g_rbxHandles.size());

    for (const auto& kv : g_rbxHandles) {
        DWORD result = WaitForSingleObject(kv.second, 0);
        if (result == WAIT_OBJECT_0)
            exited.push_back(kv.first);
        else if (result == WAIT_FAILED &&
                 LogRateLimit("tracked-wait-fail", 60))
            LOGW("[TASX] Liveness check failed for tracked PID %lu (err %lu)",
                 kv.first, (unsigned long)GetLastError());
    }

    for (DWORD pid : exited) {
        if (g_rbxHandles.count(pid))
            HandleRobloxExited(pid);
    }
}

/* Ini hygiene: warn once per unknown section|key (typos like BackgroudCpuCap
   otherwise fail silently into defaults). [FastFlags] accepts any flag name
   (filtered separately by the FFlags allowlist), so only its section name
   and the reserved keys (Preset/InjectorOwnsGraphics/ForceGraphicsFlags)
   are checked here. */
static void ValidateIniKeys()
{
    static const char* known[] = {
        "tasx|trimunfocused", "tasx|trimintervalsec", "tasx|adaptivetrim",
        "tasx|trimskipbelowmb", "tasx|farmkeephot", "tasx|hardtrimaftersec",
        "tasx|trimmetrics", "tasx|trimlogintervalsec", "tasx|idletrimenabled",
        "tasx|idletrimsec", "tasx|idletrimcpupct", "tasx|idletrimnetworkbytes",
        "tasx|idletrimsampleintervalsec", "tasx|perclientcommitlimitmb",
        "tasx|commitlimitaction", "tasx|commitlimitcooldownsec",
        "tasx|commitcheckintervalsec", "tasx|memoryflags",
        "tasx|budgetplannerenabled", "tasx|budgetplannerlogintervalsec",
        "tasx|dynamicmemorycapenforce",
        "tasx|backgroundmempriority", "tasx|jobassignmode",
        "tasx|backgroundcpucappercent",
        "tasx|jobcpucappercent",
        "tasx|jobmemorycapmb", "tasx|killonagentexit",
        "tasx|commitblockthreshold", "tasx|mutebackground",
        "tasx|pinbackgroundtoecores", "tasx|dynamicaffinity",
        "tasx|focusdwellms", "tasx|focushysteresisms",
        "tasx|pagefilewarnfreegb", "tasx|lowmemreactor",
        "tasx|lowmemcooldownsec", "tasx|systemcleaner",
        "tasx|systemcleanstandby", "tasx|systemcleanemptyws",
        "tasx|systemcleanminintervalsec", "tasx|purgestandbyonlaunch",
        "tasx|selfcpuwatchdogpercent", "tasx|killcrashhandler",
        "tasx|timerresolution", "tasx|powerplan", "tasx|applytweaks",
        "tasx|disablecpuboost", "tasx|warmclientfiles", "tasx|warmmaxmb",
        "tasx|desktopheapexpand", "tasx|injectorownsgraphics",
        "tasx|forcegraphicsflags", "tasx|preset", "tasx|disabletelemetry",
        "tasx|boosthotkey", "tasx|pageinintervalsec", "tasx|farmboostdefault",
        "tasx|respawnoncrash", "tasx|respawnperhourmax",
        "tasx|respawnglobalhourcap", "tasx|hungclientwatch", "tasx|hungkill",
        "tasx|hungkillaftersecmin", "tasx|cachemaxgb",
        "roblox|uncapfps", "roblox|targetfps", "roblox|renderer",
        "roblox|lighting", "roblox|texturequality",
        "roblox|disabletelemetry", "roblox|extraversionsdirs",
        "etw|disabletelemetry", "log|loglevel", "log|logfile",
        "log|logtimestamps",
        "net|sharedcacheroot", "net|cachelinks",
        // [FastFlags] section name itself (+ any flag key, see above)
        "fastflags|preset", "fastflags|injectorownsgraphics",
        "fastflags|forcegraphicsflags", "fastflags|jobassignmode",
    };
    int cnt = config_get_entry_count();
    for (int i = 0; i < cnt; ++i) {
        char sec[32] = {}, key[64] = {}, val[192] = {};
        if (!config_get_entry(i, sec, sizeof(sec), key, sizeof(key), val, sizeof(val)))
            continue;
        char lowSec[32] = {}, lowKey[64] = {}, low[96] = {};
        size_t a = 0;
        for (; sec[a] && a + 1 < sizeof(lowSec); ++a) {
            char c = sec[a];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            lowSec[a] = c;
        }
        size_t b = 0;
        for (; key[b] && b + 1 < sizeof(lowKey); ++b) {
            char c = key[b];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            lowKey[b] = c;
        }
        if (strcmp(lowSec, "fastflags") == 0) continue; // any flag name allowed
        snprintf(low, sizeof(low), "%s|%s", lowSec, lowKey);
        bool ok = false;
        for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); ++k) {
            if (strcmp(low, known[k]) == 0) { ok = true; break; }
        }
        if (!ok) {
            char tag[128];
            snprintf(tag, sizeof(tag), "ini-unknown-%s", low);
            if (LogRateLimit(tag, 3600))
                LOGW("[TASX] WARNING: unknown ini key [%s] %s - typo? ignored, default applies",
                     sec, key);
        }
    }
}

/* One-time startup in dependency order. Returns 1 when the main loop
   should run, 0 when the process must exit silently (already running). */
int InitSubsystems()
{
    tasx_log_init();

    config_default_path(g_iniPath, MAX_PATH);
    // If ini is missing, create documented defaults atomically
    {
        DWORD attr = GetFileAttributesA(g_iniPath);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (config_create_default(g_iniPath)) {
                // will be loaded below
            }
        }
    }
    config_load(g_iniPath);
    tasx_log_configure(config_get_str("Log", "LogFile", ""),
                       tasx_log_level_from_str(config_get_str("Log", "LogLevel", "info")));
    tasx_log_set_timestamps(config_get_bool("Log", "LogTimestamps", 1));
    g_iniMtime = IniMtimeKey();
    if (!config_loaded()) {
        LOGW("[TASX] WARNING: TASX.ini not found - built-in defaults apply. Expected path: %s", g_iniPath);
        if (config_create_default(g_iniPath))
            LOGI("[TASX] Created default TASX.ini at %s", g_iniPath);
    } else {
        LOGI("[TASX] Config: %s (loaded)", g_iniPath);
        if (config_truncated())
            LOGW("[TASX] WARNING: %s holds more keys than the %d-entry table - keys past the limit were ignored",
                 g_iniPath, config_max_entries());
        ValidateIniKeys();
    }

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\TASX_Optimizer_Mutex");
    if (!g_singleInstanceMutex) {
        LOGE("[TASX] Cannot create single-instance mutex (err %lu); refusing to start safely",
             (unsigned long)GetLastError());
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
#ifdef TASX_CONSOLE
        LOGI("[TASX] TASX is already running in the background (agent autostart or another window).");
        LOGI("[TASX] Nothing is broken - this window closes in 5 seconds.");
        Sleep(5000);
#else
        MessageBoxW(nullptr,
            L"TASX уже запущен и работает в фоне (агент автозапуска).\n"
            L"Второй экземпляр не нужен - всё уже оптимизируется.",
            L"TASX", MB_OK | MB_ICONINFORMATION);
#endif
        return 0;
    }

    /* Only the instance that owns the mutex may recover a stale global power
       journal. A second launch must never restore the scheme underneath an
       already-running agent. */
    TweaksPowerRecoverStale();

    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    /* Signaled by the Ctrl handler; lets the low-memory reactor and any
       long waits observe shutdown immediately (see LowMemReactorStart). */
    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    bool elevated = tasx_is_elevated() != 0;

    CpuInit();
    if (!config_get_bool("TASX", "TrimUnfocused", 0))
        LOGW("[Trimmer] WARN: триммер отключён пользователем (TrimUnfocused=0)");
    if (config_get_bool("TASX", "DisableCpuBoost", 0)) {
        if (elevated)
            CpuSetBoostMode(0);
        else if (LogRateLimit("boost-noadmin", 3600))
            LOGW("[TASX] DisableCpuBoost skipped (needs admin)");
    }
    TweaksApplyOneShot();
    FFlagsApply();
    NetCacheApply();
    SystemCleanPass(true);

    bool jobsOk = JobsInit();
    if (config_get_bool("TASX", "FarmBoostDefault", 0))
        ToggleFarmBoost(); // starts boosted, per FarmBoostDefault=1
    if (!config_get_bool("TASX", "TrimUnfocused", 0)) StopAllTrimmers();
    if (config_get_bool("TASX", "MemoryFlags", 0))
        LOGI("[FFlags] MemoryFlags=1 requested; only verified allowlist entries will be considered");
    if (config_get_bool("TASX", "DynamicMemoryCapEnforce", 0))
        LOGW("[Budget] DynamicMemoryCapEnforce=1 is opt-in; cap enforcement remains disabled until measured rollback support is available");

    // Startup banner: admin status, job mode, enabled/disabled features.
    // Printed ONCE - replaces the old per-feature "needs admin" spam.
    LOGI("[TASX] Admin: %s | Job mode: %s",
         elevated ? "YES (elevated)" : "NO (limited mode)",
         jobsOk ? "native cgroup + per-process focus" : "per-process fallback");
    if (!elevated) {
        LOGI("[TASX] Limited mode WITHOUT admin - DISABLED: standby purge, system cleaner, HKLM tweaks, ETW, CPU boost. ENABLED: priority, affinity, FFlags, crash-handler kill, trimmer(soft). Run Install\\ScheduledTaskInstaller.bat for full effect.");
    } else {
        LOGI("[TASX] Features: priority/affinity/jobs/FFlags=ON | trimmer=%s | standby-clean=%s | system-cleaner=%s | HKLM-tweaks=%s | power-plan=%s",
             config_get_bool("TASX", "TrimUnfocused", 0) ? "ON" : "OFF",
             (config_get_bool("TASX", "SystemCleanStandby", 0) ||
              config_get_bool("TASX", "PurgeStandbyOnLaunch", 0)) ? "ON" : "OFF",
             config_get_bool("TASX", "SystemCleaner", 0) ? "ON" : "OFF",
             config_get_bool("TASX", "ApplyTweaks", 0) ? "ON" : "OFF",
             config_get_bool("TASX", "PowerPlan", 0) ? "ON" : "OFF");
    }

    // FPS config sanity (single place): UncapFps=1 makes TargetFps
    // meaningless - the FFlags writer ignores it and uncaps to 999.
    if (config_get_bool("Roblox", "UncapFps", 0)) {
        int tf = config_get_int("Roblox", "TargetFps", 999);
        if (tf < 240 && LogRateLimit("fps-conflict", 3600))
            LOGW("[FFlags] WARNING: UncapFps=1 conflicts with TargetFps=%d -> TargetFps ignored (uncapped)", tf);
    }

    g_wmiOk = _wmimon();
    if (!g_wmiOk)
        LOGW("[TASX] WMI unavailable -> snapshot discovery active");

    g_hook = new WinHook();
    g_hook->Start();

    g_hotkey = new FarmHotkey();
    g_hotkey->Start();

    LowMemReactorStart();

    SweepDiscover();
    ApplyFocusIfChanged();
    UpdateWorkingSetCache();
    ULONGLONG now = GetTickCount64();
    g_lastCrashSweepMs = now;
    g_lastRetryMs = now;
    g_lastIniCheckMs = now;
    if (!g_rbxHandles.empty()) { TweaksPowerEnter(); TimerOn(); }

    // Page-in hotkey (always P+E); no startup page-in by default.
    LOGI("[TASX] Launched");
    return 1;
}

/* Teardown in reverse order. Runs on Ctrl/close/logoff and on natural exit
   (the main loop only ends via g_stop, so this is always reached). */
void ShutdownSubsystems()
{
    LOGI("[TASX] Shutting down...");
    delete g_hotkey;
    g_hotkey = nullptr;
    delete g_hook;
    g_hook = nullptr;
    AudioShutdown();
    _wmishutdown();
    WarmShutdown();
    /* Release every owned process handle and wait registration before
       destroying the job. This makes restart/upgrade paths clean and, with
       KillOnAgentExit=0, guarantees that shutting TASX down does not leave
       stale state or terminate the clients. */
    while (!g_rbxHandles.empty())
        ReleaseClient(g_rbxHandles.begin()->first, false);
    PageInFarmStop();
    StopAllTrimmers();
    LowMemReactorStop();
    JobShutdown();
    TimerOff();
    TweaksPowerExit();
    RespawnClearAll();
    MetricsReset();
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }
    LOGI("[TASX] Stopped");
    tasx_log_shutdown();
}

int RunTasx()
{
    if (!InitSubsystems())
        return 0;

    while (!InterlockedCompareExchange(&g_stop, 0, 0))
    {
        auto ev = PopEvent(250);

        if (ev) {
            switch (ev->kind) {
            case Ev::RobloxCreated: HandleRobloxCreated(ev->pid); break;
            case Ev::RobloxExited:  HandleRobloxExited(ev->pid);  break;
            case Ev::JobChild:      KillCrashHandlerPid(ev->pid); break;
            case Ev::Focus:         ApplyFocusIfChanged();        break;
            case Ev::ConfigChanged: HandleConfigChanged();        break;
            case Ev::HotkeyBoost:   ToggleFarmBoost();            break;
            case Ev::LowMem: {
                static ULONGLONG lastLowMemCleanMs = 0;
                ULONGLONG now = GetTickCount64();
                int minIntervalSec = config_get_int("TASX", "SystemCleanMinIntervalSec", 60);
                if (now - lastLowMemCleanMs < (ULONGLONG)minIntervalSec * 1000) {
                    break;  // rate-limited
                }
                lastLowMemCleanMs = now;
                LOGI("[TASX] Low memory event -> reactive clean");
                /* LowMemReactor and global standby purge are separate
                   controls.  A user who opted into event detection must not
                   also receive a system-wide purge unless explicitly opting
                   into SystemCleanStandby. */
                if (config_get_bool("TASX", "SystemCleanStandby", 0))
                    tasx_purge_standby_list();
                TrimmerTrimAllAggressive();
                // SystemCleanPass is internally split: standby always, global
                // empty-WS only when allowed AND nothing focused.
                if (g_appliedFocus == 0) {
                    SystemCleanPass(true);
                } else {
                    LOGI("[TASX] Skipped system clean (focused PID %lu active)", g_appliedFocus);
                }
                break;
            }
            }
        }

        if (g_reconcileNeeded.exchange(false, std::memory_order_acq_rel)) {
            unsigned long dropped =
                g_droppedQueueEvents.exchange(0, std::memory_order_acq_rel);
            if (dropped && LogRateLimit("event-queue-pressure", 60))
                LOGW("[TASX] Event queue saturated; dropped %lu notification(s), reconciling tracked processes",
                     dropped);
            SweepTrackedClients();
            SweepDiscover();
        }

        // Focus safety net (hook may miss exotic switches): PopEvent timeout 250ms already does cadence via loop
        // But also ensure ApplyFocus on each iteration (lightweight, does GetForegroundWindow)
        ApplyFocusIfChanged();
        RetryBlockedHooks();
        RespawnRetryPending();

        ULONGLONG now = GetTickCount64();

        // Auto page-in every N minutes when explicitly enabled; default is
        // manual-only because a full private-memory read is intentionally I/O-heavy.
        int pageInSec = config_get_int("TASX", "PageInIntervalSec", 0);
        if (pageInSec > 0 && !g_rbxHandles.empty() && (now - g_lastPageInMs) >= (ULONGLONG)pageInSec * 1000 && !g_pageInBusy.load()) {
            g_lastPageInMs = now;
            PageInFarm();
        }

        // Periodic WS cache refresh ~10s, plus GDI guard, audio refresh, dynamic jobs
        if (now - g_lastWsUpdateMs >= 10000) {
            SweepTrackedClients();
            /* WMI is an acceleration path, not the sole source of truth.
               Providers can lose a subscription after startup without making
               the original _wmimon() call fail; periodic reconciliation keeps
               newly launched clients discoverable in that case too. */
            SweepDiscover();
            UpdateWorkingSetCache();
            for (auto& kv : g_rbxHandles)
                DesktopCheckClient(kv.first, kv.second);
            // audio sessions appear/disappear without our knowledge -> periodic bulk sync
            AudioApplyBackgroundMute(ClientPidSet(), g_appliedFocus);
            JobsRefreshDynamic(g_appliedFocus != 0);
            CheckIniReload();
            HungSweep(); // hung-client watchdog (opt-in, logs/kills)
            MetricsSampleClients(now);
            MetricsMaybeReport(now);
            MetricsCheckCommitLimits(now);
            MetricsMaybeReportBudget(now);
        }

        // Crash handlers: only every 60s plus on RBX_ON/JobChild
        if (now - g_lastCrashSweepMs >= 60000) {
            SweepCrashHandlers();
            NetCacheLogConnections(ClientPidSet());
            NetCacheTrimIfOversized(); // shared-cache LRU trim (rate-limited inside)
            g_lastCrashSweepMs = now;

            // periodic pagefile free-space check + farm budget + self CPU watchdog.
            CheckPagefileWarning("pagefile-low");
            FarmBudgetCheck();
            {
                static ULONGLONG lastCpuCheckMs = 0;
                static ULONGLONG lastSelfCpuMs = 0;
                FILETIME ftime, fexit, fkernel, fuser;
                if (GetProcessTimes(GetCurrentProcess(), &ftime, &fexit, &fkernel, &fuser)) {
                    ULONGLONG ku =
                        (((ULONGLONG)fkernel.dwHighDateTime << 32) | fkernel.dwLowDateTime) +
                        (((ULONGLONG)fuser.dwHighDateTime << 32) | fuser.dwLowDateTime);
                    if (lastCpuCheckMs && now > lastCpuCheckMs && ku >= lastSelfCpuMs) {
                        int cpuPct = (int)(((ku - lastSelfCpuMs) * 100) /
                                           ((now - lastCpuCheckMs) * 10000));
                        int threshold = config_get_int("TASX", "SelfCpuWatchdogPercent", 5);
                        if (cpuPct > threshold && LogRateLimit("self-cpu", 300)) {
                            LOGW("[TASX] WARNING: Self CPU %d%% exceeds watchdog threshold %d%%",
                                 cpuPct, threshold);
                        }
                    }
                    lastSelfCpuMs = ku;
                    lastCpuCheckMs = now;
                }
            }
        }
    }

    ShutdownSubsystems();
    return 0;
}

/* Last-resort boundary for C++ failures in the long-running agent. The
   subsystems are intentionally written so ShutdownSubsystems is idempotent;
   keeping this boundary here prevents an allocation/container failure from
   skipping handle release, power rollback, WMI drain, and worker joins. */
int RunTasxSafely()
{
    try {
        return RunTasx();
    } catch (const std::exception& ex) {
        LOGE("[TASX] Unhandled runtime exception: %s; entering safe shutdown", ex.what());
    } catch (...) {
        LOGE("[TASX] Unhandled runtime exception; entering safe shutdown");
    }
    ShutdownSubsystems();
    return 2;
}

} /* namespace */

/* --- CLI helpers (console build) -------------------------------------- */

#ifdef TASX_CONSOLE
static void CliPrintUsage(void)
{
    printf("TASX usage:\n"
           "  TASX.exe                 run the optimizer (foreground agent)\n"
           "  TASX.exe --status        one-shot farm summary, then exit\n"
           "  TASX.exe --preset NAME   set [FastFlags] Preset=NAME in TASX.ini\n"
           "                           (farm15|farm20|farm30|weak|balanced|off)\n"
           "  TASX.exe --help          this text\n");
}

static void CliLower(char* s, size_t n)
{
    for (size_t i = 0; i + 1 < n && s[i]; ++i)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + ('a' - 'A'));
}

static int CliStatus(void)
{
    config_default_path(g_iniPath, MAX_PATH);
    config_load(g_iniPath);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        printf("TASX --status: GlobalMemoryStatusEx failed (err %lu)\n",
               (unsigned long)GetLastError());
        return 1;
    }

    int commit = tasx_get_commit_percent();
    int clients = 0;
    std::vector<ProcStat> stats;
    if (QueryProcStats(stats)) {
        for (const auto& s : stats)
            if (s.name.size() && IsRobloxName(s.name))
                ++clients;
    }

    double ramGB  = (double)ms.ullTotalPhys / (double)(1ull << 30);
    double freeGB = (double)ms.ullAvailPhys / (double)(1ull << 30);
    printf("TASX status | RAM free %.1f/%.1f GB | commit %d%% | Roblox clients %d\n",
           freeGB, ramGB, commit, clients);
    return 0;
}

static bool CliIsValidPreset(const char* p)
{
    char low[24] = {};
    size_t n = 0;
    for (; p[n] && n + 1 < sizeof(low); ++n) low[n] = p[n];
    CliLower(low, sizeof(low));
    return strcmp(low, "farm15") == 0 || strcmp(low, "farm20") == 0 ||
           strcmp(low, "farm30") == 0 || strcmp(low, "weak") == 0 ||
           strcmp(low, "balanced") == 0 || strcmp(low, "off") == 0;
}

/* Rewrites (or appends) [FastFlags] Preset=NAME in TASX.ini atomically
   (tmp file + MoveFileEx). Returns 0 on success. */
static int CliSetPreset(const char* name)
{
    if (!CliIsValidPreset(name)) {
        printf("TASX --preset: unknown preset '%s' (farm15|farm20|farm30|weak|balanced|off)\n", name);
        return 1;
    }

    config_default_path(g_iniPath, MAX_PATH);

    std::vector<std::string> lines;
    bool inFastFlags = false;
    bool found = false;
    {
        FILE* f = fopen(g_iniPath, "r");
        if (f) {
            char buf[512];
            while (fgets(buf, sizeof(buf), f)) {
                std::string line(buf);
                if (!line.empty() && line.back() == '\n') line.pop_back();
                std::string t = line;
                size_t b = t.find_first_not_of(" \t");
                if (b != std::string::npos) t = t.substr(b);
                if (!t.empty() && t[0] == '[') {
                    std::string sec = t;
                    size_t c = t.find(']');
                    if (c != std::string::npos) sec = t.substr(1, c - 1);
                    inFastFlags = (sec.find("FastFlags") != std::string::npos ||
                                   sec.find("fastflags") != std::string::npos);
                } else if (inFastFlags && !found) {
                    std::string low = t;
                    CliLower(&low[0], low.size() + 1);
                    if (low.find("preset=") == 0) {
                        lines.push_back("Preset=" + std::string(name));
                        found = true;
                        continue;
                    }
                }
                lines.push_back(line);
            }
            fclose(f);
        }
    }
    if (!found) {
        lines.push_back("");
        lines.push_back("[FastFlags]");
        lines.push_back(std::string("Preset=") + name);
    }

    std::string tmp(g_iniPath);
    tmp += ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        printf("TASX --preset: cannot write %s\n", tmp.c_str());
        return 1;
    }
    for (const auto& line : lines)
        fprintf(f, "%s\n", line.c_str());
    fclose(f);

    if (!MoveFileExA(tmp.c_str(), g_iniPath, MOVEFILE_REPLACE_EXISTING)) {
        printf("TASX --preset: failed to replace %s (err %lu)\n",
               g_iniPath, (unsigned long)GetLastError());
        return 1;
    }
    printf("TASX --preset: [FastFlags] Preset=%s written to %s\n", name, g_iniPath);
    return 0;
}
#endif /* TASX_CONSOLE */

#ifdef TASX_CONSOLE
int main(int argc, char** argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--status") == 0)
                return CliStatus();
            if (strcmp(argv[i], "--preset") == 0) {
                if (i + 1 >= argc) { CliPrintUsage(); return 1; }
                return CliSetPreset(argv[++i]);
            }
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                CliPrintUsage();
                return 0;
            }
            CliPrintUsage();
            return 1;
        }
    }
    return RunTasxSafely();
}
#else
int WINAPI WinMain(
    _In_ HINSTANCE,
    _In_opt_ HINSTANCE,
    _In_ LPSTR,
    _In_ int)
{
    return RunTasxSafely();
}
#endif
