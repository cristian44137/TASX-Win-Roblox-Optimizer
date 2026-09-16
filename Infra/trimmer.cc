#include "trimmer.h"

#include "CPU.h"
#include "config.h"
#include "ntsys.h"
#include "log.h"
#include "lograte.h"
#include "metrics.h"
#include "stats.h"

#include <psapi.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace {

struct Due {
    std::int64_t ms;
    DWORD pid;
};
struct DueGreater {
    bool operator()(const Due& a, const Due& b) const { return a.ms > b.ms; }
};

std::mutex g_mtx;
std::priority_queue<Due, std::vector<Due>, DueGreater> g_heap;
std::unordered_map<DWORD, HANDLE> g_handles;   /* pid -> proc handle */
std::unordered_map<DWORD, SIZE_T> g_wsCache;   /* pid -> last WS bytes (from master) */
std::unordered_map<DWORD, std::int64_t> g_wsTime; /* pid -> cache timestamp ms */
std::unordered_map<DWORD, std::int64_t> g_unfocusedSince; /* pid -> when lost focus */
struct IdleState {
    bool idle = false;
    std::int64_t since = 0;
    std::int64_t lastUpdate = 0;
};
std::unordered_map<DWORD, IdleState> g_idle;
std::atomic<DWORD> g_focusedPid{0};
std::atomic<int> g_pageInPause{0};
/* FarmBoost (Ctrl+Alt+B): "trim paused" per the boosted-farm contract.
   Kept separate from g_pageInPause (different lifetime/owner) but checked
   together via TrimmingPaused() below so every call site only needs to
   ask one question. */
std::atomic<int> g_farmBoostPause{0};

bool TrimmingPaused()
{
    return g_pageInPause.load() != 0 || g_farmBoostPause.load() != 0;
}

HANDLE g_hThread = nullptr;
HANDLE g_hStop = nullptr;
HANDLE g_hWake = nullptr;
/* NOTE: no LowMem handle here by design. The single low-memory owner is
   master.cpp LowMemReactorStart (standby + aggressive trim + system-clean
   gating). A second watcher in this module caused double-trim per signal. */

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t ClampInterval(std::int64_t ms)
{
    if (ms < 3000) return 3000;
    if (ms > 600000) return 600000;
    return ms;
}

namespace {

/* Shared snapshot of memory pressure with TTL = avoids 20 NT calls per tick.
   Both trimmer IntervalMs() and master low-mem reactor use this if needed. */
static std::int64_t g_snapshotMs = 0;
static int g_snapshotLoad = 50;
static int g_snapshotCommit = -1;
static std::mutex g_snapshotMtx;

int GetPressureLoad()
{
    std::lock_guard<std::mutex> lk(g_snapshotMtx);
    std::int64_t now = NowMs();
    if (now - g_snapshotMs < 5000) return g_snapshotLoad; // TTL 5s
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) g_snapshotLoad = (int)ms.dwMemoryLoad;
    else g_snapshotLoad = 50;
    g_snapshotCommit = tasx_get_commit_percent();
    g_snapshotMs = now;
    return g_snapshotLoad;
}

int GetPressureCommit()
{
    std::lock_guard<std::mutex> lk(g_snapshotMtx);
    std::int64_t now = NowMs();
    if (now - g_snapshotMs < 5000) return g_snapshotCommit;
    GetPressureLoad(); // refresh
    return g_snapshotCommit;
}

} // namespace

/* Base interval modulated by system memory pressure (shared TTL snapshot). */
std::int64_t IntervalMs()
{
    std::int64_t base = (std::int64_t)config_get_int("TASX", "TrimIntervalSec", 10) * 1000;

    if (!config_get_bool("TASX", "AdaptiveTrim", 1))
        return ClampInterval(base);

    int load = GetPressureLoad();
    if (load >= 90) return ClampInterval(base / 4);   /* farm is choking RAM */
    if (load >= 75) return ClampInterval(base / 2);
    if (load <= 60) return ClampInterval(base * 2);   /* plenty of headroom */
    int commitPct = GetPressureCommit();
    if (commitPct >= 80) return ClampInterval(base / 2);
    if (commitPct >= 0 && commitPct < 50) return ClampInterval(base * 2);
    return ClampInterval(base);
}

/* Farm keep-hot policy: unfocused farm clients are WORKING, not idle.
   With FarmKeepHot=1 (default on farm* presets) the periodic pass is
   soft-only; HardTrim (EmptyWorkingSet) fires only after HardTrimAfterSec
   of continuous unfocus (default 1800s) or on commit-critical (>=90%).
   The old "hard after 2*interval" (~6-20s) is what evicted whole farms. */
static bool IsFarmKeepHot()
{
    const char* v = config_get_str("TASX", "FarmKeepHot", nullptr);
    if (v) return config_get_bool("TASX", "FarmKeepHot", 1) != 0;
    const char* pPre = config_get_str("FastFlags", "Preset", nullptr);
    if (!pPre) pPre = config_get_str("TASX", "Preset", nullptr);
    if (pPre) {
        char pl[16] = {};
        size_t pn = 0;
        for (; pPre[pn] && pn + 1 < sizeof(pl); ++pn)
            pl[pn] = (char)tolower((unsigned char)pPre[pn]);
        if (strcmp(pl, "farm15") == 0 || strcmp(pl, "farm20") == 0 ||
            strcmp(pl, "farm30") == 0)
            return true;
    }
    return false;
}

static std::int64_t HardTrimAfterMs()
{
    int s = config_get_int("TASX", "HardTrimAfterSec", 1800);
    if (s < 60) s = 60;
    if (s > 86400) s = 86400;
    return (std::int64_t)s * 1000;
}

static unsigned long BgMemPrio()
{
    return CpuBackgroundMemPrio(); // single source of truth lives in CPU.cc
}

bool QueryTrimSnapshot(HANDLE h, TrimMetricSnapshot& out, DWORD pid,
                       const char* type)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&counters,
                              sizeof(counters)))
        return false;

    out = {};
    out.pid = pid;
    out.type = type;
    out.wsBefore = counters.WorkingSetSize;
    out.privateBefore = counters.PrivateUsage;
    out.commitBefore = tasx_get_commit_percent();
    out.hasHardFaultsBefore = QuerySystemHardFaults(out.hardFaultsBefore);
    return true;
}

void FinishTrimSnapshot(HANDLE h, TrimMetricSnapshot& out)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&counters,
                              sizeof(counters))) {
        out.wsAfter = counters.WorkingSetSize;
        out.privateAfter = counters.PrivateUsage;
    }
    out.commitAfter = tasx_get_commit_percent();
    out.hasHardFaultsAfter = QuerySystemHardFaults(out.hardFaultsAfter);
    MetricsRecordTrim(out);
}

void SoftTrim(HANDLE h, DWORD pid, const char* reason)
{
    if (!config_get_bool("TASX", "TrimUnfocused", 0)) return; // user disabled
    TrimMetricSnapshot metric;
    bool measured = QueryTrimSnapshot(h, metric, pid, "soft");
    tasx_set_memory_priority(h, BgMemPrio());
    BOOL ok = SetProcessWorkingSetSizeEx(h, (SIZE_T)-1, (SIZE_T)-1, 0);
    if (measured) {
        FinishTrimSnapshot(h, metric);
        if (reason && LogRateLimit("trim-soft-reason", 60))
            LOGI("[Trimmer] SoftTrim PID %lu reason=%s api=%d WS %llu->%llu MB",
                 pid, reason, ok ? 1 : 0,
                 (unsigned long long)(metric.wsBefore >> 20),
                 (unsigned long long)(metric.wsAfter >> 20));
    }
}

void HardTrim(HANDLE h, DWORD pid, const char* reason)
{
    if (!config_get_bool("TASX", "TrimUnfocused", 0)) return; // user disabled
    TrimMetricSnapshot metric;
    bool measured = QueryTrimSnapshot(h, metric, pid, "hard");
    BOOL ok = EmptyWorkingSet(h);
    if (measured) {
        FinishTrimSnapshot(h, metric);
        if (reason && LogRateLimit("trim-hard-reason", 60))
            LOGW("[Trimmer] HardTrim PID %lu reason=%s api=%d WS %llu->%llu MB",
                 pid, reason, ok ? 1 : 0,
                 (unsigned long long)(metric.wsBefore >> 20),
                 (unsigned long long)(metric.wsAfter >> 20));
    }
}

bool BelowSkipThresholdCached(DWORD pid)
{
    int skipMB = config_get_int("TASX", "TrimSkipBelowMB", 250);
    if (skipMB <= 0) return false;

    auto it = g_wsCache.find(pid);
    auto itT = g_wsTime.find(pid);
    if (it == g_wsCache.end() || itT == g_wsTime.end()) return false;

    std::int64_t age = NowMs() - itT->second;
    if (age > 30000) return false; // stale -> don't skip, allow trim; master will refresh

    const SIZE_T limit = (SIZE_T)(unsigned)skipMB << 20;
    return it->second < limit;
}

/* One trim pass for a single client. The caller MUST hold g_mtx: the HANDLE
   has to be used while the lock is held because ReleaseClient() erases the
   map entry (StopTrimmer) and then CloseHandle()s the process handle - if we
   released the lock in between, this thread could trim through a closed (and
   possibly recycled) handle. Holding it also makes the g_wsCache reads inside
   BelowSkipThresholdCached() safe against the writer thread
   (TrimmerUpdateWorkingSet). No function called here re-enters g_mtx. */
static bool IsIdleForAggressiveLocked(DWORD pid, std::int64_t now)
{
    auto it = g_idle.find(pid);
    if (it == g_idle.end() || !it->second.idle) return false;
    int idleSec = config_get_int("TASX", "IdleTrimSec", 60);
    if (idleSec < 1) idleSec = 60;
    return now - it->second.since >= (std::int64_t)idleSec * 1000;
}

static void TrimClientLocked(DWORD pid, HANDLE h, int& trimmed, int& skipped,
                             int& softOnly)
{
    /* IdleTrimEnabled is intentionally opt-in. When disabled, a scheduled
       soft trim remains the legacy behavior; aggressive hard trim is never
       inferred from WS growth alone. */
    bool idleEnabled = config_get_bool("TASX", "IdleTrimEnabled", 0) != 0;
    std::int64_t now = NowMs();
    bool idle = IsIdleForAggressiveLocked(pid, now);

    if (BelowSkipThresholdCached(pid)) {
        ++skipped;
        return;
    }

    /* No automatic hard trim on commit pressure. CommitLimitAction=softtrim
       calls TrimmerRequestSoftTrim explicitly and is separately rate-limited. */
    if (!idleEnabled || !idle) {
        SoftTrim(h, pid, "scheduled");
        ++softOnly;
        return;
    }

    SoftTrim(h, pid, "idle");
    ++softOnly;
    std::int64_t hardAfter = IsFarmKeepHot() ? HardTrimAfterMs()
                                             : (std::int64_t)config_get_int("TASX", "HardTrimAfterSec", 600) * 1000;
    if (hardAfter < 60000) hardAfter = 60000;
    auto itU = g_unfocusedSince.find(pid);
    std::int64_t unfocusedMs = itU == g_unfocusedSince.end() ? 0 : now - itU->second;
    if (unfocusedMs >= hardAfter) {
        HardTrim(h, pid, "idle-timeout");
        ++trimmed;
    }
}

DWORD WINAPI SchedulerThreadImpl(LPVOID)
{
    std::int64_t lastLog = NowMs();
    int trimmed = 0, skipped = 0, softOnly = 0;

    HANDLE waitHandles[2];
    waitHandles[0] = g_hStop;
    waitHandles[1] = g_hWake;

    while (true)
    {
        std::int64_t timeoutMs;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_heap.empty()) {
                std::int64_t due = g_heap.top().ms;
                std::int64_t now = NowMs();
                if (due <= now) timeoutMs = 0;
                else timeoutMs = due - now;
                if (timeoutMs > 600000) timeoutMs = 600000;
            } else {
                timeoutMs = INT64_MAX; /* infinite below */
            }
        }

        DWORD to = (timeoutMs == INT64_MAX) ? INFINITE : (DWORD)timeoutMs;
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, to);

        if (waitResult == WAIT_OBJECT_0) {
            break; /* stop */
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            continue; /* woken by Start/Stop/Focus — re-evaluate heap top */
        }
        if (waitResult == WAIT_FAILED) {
            Sleep(100);
            continue;
        }
        /* WAIT_TIMEOUT: a heap deadline fired — fall through. */

        if (!config_get_bool("TASX", "TrimUnfocused", 0)) {
            Sleep(30000);
            continue;
        }

        Due d{};
        bool haveDue = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_heap.empty()) continue;
            if (g_heap.top().ms > NowMs()) continue; // woken early
            d = g_heap.top();
            g_heap.pop();
            haveDue = true;
        }
        if (!haveDue) continue;

        DWORD pid = d.pid;
        {
            /* Everything below runs under g_mtx, up to and including the
               trim syscalls (see TrimClientLocked). The scheduler thread and
               ReleaseClient() are the only owners of g_handles/g_heap, so
               this is the single point where a concurrent release is
               serialised against in-flight trimming. */
            std::lock_guard<std::mutex> lk(g_mtx);

            if (TrimmingPaused() || g_focusedPid.load() == pid) {
                g_heap.push({NowMs() + 3000, pid}); // never fight the player
                g_unfocusedSince.erase(pid);
                g_idle.erase(pid);
                continue;
            }

            auto itH = g_handles.find(pid);
            if (itH == g_handles.end()) {
                // stale entry (client released): drop, no re-push
                g_unfocusedSince.erase(pid);
                continue;
            }

            TrimClientLocked(pid, itH->second, trimmed, skipped, softOnly);
            g_heap.push({NowMs() + IntervalMs() + (std::int64_t)(pid % 5) * 1000, pid});
        }

        std::int64_t now = NowMs();
        if (now - lastLog >= 30000) {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_handles.empty()) {
                // No clients: drop stale window counters instead of printing
                // someone else's stats under "0 client(s)".
                trimmed = 0;
                skipped = 0;
                softOnly = 0;
                lastLog = now;
            } else {
            // Farm observability: avg WS + commit in the same line, zero new
            // syscalls (wsCache already here, pressure via 5s TTL snapshot).
            unsigned long long wsSum = 0;
            for (auto& kv : g_wsCache) wsSum += (unsigned long long)kv.second;
            unsigned avgMB = g_wsCache.empty() ? 0
                : (unsigned)(wsSum / g_wsCache.size() >> 20);
            int commit = g_snapshotCommit;
            LOGI("[Trimmer] %u client(s), %d hard trimmed, %d soft, %d skipped (30s) | avg WS %u MB, commit %d%%",
                 (unsigned)g_handles.size(), trimmed, softOnly, skipped, avgMB, commit);
            lastLog = now;
            trimmed = 0;
            skipped = 0;
            softOnly = 0;
            } // end else (non-empty): log summary
        }
    }
    return 0;
}

DWORD WINAPI SchedulerThread(LPVOID arg) noexcept
{
    try {
        return SchedulerThreadImpl(arg);
    } catch (const std::exception& ex) {
        LOGE("[Trimmer] Scheduler exception: %s; scheduler stopped safely", ex.what());
    } catch (...) {
        LOGE("[Trimmer] Scheduler exception; scheduler stopped safely");
    }
    return 0;
}

} // namespace

void TrimmerSetPageInPause(int on)
{
    g_pageInPause.store(on ? 1 : 0);
    if (g_hWake) SetEvent(g_hWake);
}

void TrimmerSetFarmBoost(int on)
{
    g_farmBoostPause.store(on ? 1 : 0);
    if (g_hWake) SetEvent(g_hWake);
}

void TrimmerSetFocused(DWORD pid)
{
    g_focusedPid.store(pid ? pid : 0);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (pid) {
        g_unfocusedSince.erase(pid);
        g_idle.erase(pid);
    } else {
        // lost focus: mark all current as unfocused start if not already
        std::int64_t now = NowMs();
        for (auto &kv : g_handles) {
            if (g_unfocusedSince.find(kv.first) == g_unfocusedSince.end())
                g_unfocusedSince[kv.first] = now;
        }
    }
    if (g_hWake) SetEvent(g_hWake);
}

bool StartTrimmer(DWORD pid, HANDLE processHandle)
{
    if (!config_get_bool("TASX", "TrimUnfocused", 0))
        return false;

    bool needStart = false;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_handles.count(pid)) return true;

        g_handles[pid] = processHandle;
        // Jitter deadlines by PID so N clients don't trim in one burst
        // (thundering herd -> fault storm). Deterministic, no rand() needed.
        g_heap.push({NowMs() + IntervalMs() + (std::int64_t)(pid % 7) * 1000, pid});
        // initialize unfocused tracking: if not focused, start counting
        if (g_focusedPid.load() != pid) {
            if (g_unfocusedSince.find(pid) == g_unfocusedSince.end())
                g_unfocusedSince[pid] = NowMs();
        }
        needStart = (g_hThread == nullptr);
        if (needStart) {
            g_hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_hStop || !g_hWake) {
                LOGE("[Trimmer] Failed to create scheduler events");
                if (g_hStop) { CloseHandle(g_hStop); g_hStop = nullptr; }
                if (g_hWake) { CloseHandle(g_hWake); g_hWake = nullptr; }
                g_handles.erase(pid);
                return false;
            }
        }
    }

    if (needStart) {
        g_hThread = CreateThread(nullptr, 0, SchedulerThread, nullptr, 0, nullptr);
        if (!g_hThread) {
            LOGE("[Trimmer] Failed to create scheduler thread");
            std::lock_guard<std::mutex> lock(g_mtx);
            g_handles.erase(pid);
            if (g_hStop) { CloseHandle(g_hStop); g_hStop = nullptr; }
            if (g_hWake) { CloseHandle(g_hWake); g_hWake = nullptr; }
            return false;
        }
    }

    if (g_hWake) SetEvent(g_hWake);
    return true;
}

void StopTrimmer(DWORD pid)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_handles.erase(pid);
    g_wsCache.erase(pid);
    g_wsTime.erase(pid);
    g_unfocusedSince.erase(pid);
    g_idle.erase(pid);
    // heap entry is skipped lazily on pop
    if (g_hWake) SetEvent(g_hWake);
}

void TrimmerUpdateWorkingSet(DWORD pid, SIZE_T ws)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_wsCache[pid] = ws;
    g_wsTime[pid] = NowMs();
}

void TrimmerRemoveWorkingSet(DWORD pid)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_wsCache.erase(pid);
    g_wsTime.erase(pid);
}

void TrimmerUpdateIdle(DWORD pid, int idle, ULONGLONG nowMs)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    IdleState& state = g_idle[pid];
    state.lastUpdate = (std::int64_t)nowMs;
    if (idle && g_focusedPid.load() != pid) {
        if (!state.idle) state.since = (std::int64_t)nowMs;
        state.idle = true;
    } else {
        state.idle = false;
        state.since = 0;
    }
    if (g_hWake) SetEvent(g_hWake);
}

bool TrimmerIsIdle(DWORD pid, ULONGLONG nowMs)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_idle.find(pid);
    if (it == g_idle.end() || !it->second.idle) return false;
    int idleSec = config_get_int("TASX", "IdleTrimSec", 60);
    if (idleSec < 1) idleSec = 60;
    return nowMs >= (ULONGLONG)it->second.since &&
           nowMs - (ULONGLONG)it->second.since >= (ULONGLONG)idleSec * 1000;
}

void TrimmerRequestSoftTrim(DWORD pid, const char* reason)
{
    if (!config_get_bool("TASX", "TrimUnfocused", 0)) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_focusedPid.load() == pid || TrimmingPaused()) return;
    auto it = g_handles.find(pid);
    if (it == g_handles.end()) return;
    auto idleIt = g_idle.find(pid);
    int idleSec = config_get_int("TASX", "IdleTrimSec", 60);
    if (idleSec < 1) idleSec = 60;
    bool idle = idleIt != g_idle.end() && idleIt->second.idle &&
                NowMs() - idleIt->second.since >= (std::int64_t)idleSec * 1000;
    if (!idle) {
        if (LogRateLimit("commit-soft-active", 60))
            LOGW("[Trimmer] SoftTrim request PID %lu skipped: client is active or idle threshold not met", pid);
        return;
    }
    SoftTrim(it->second, pid, reason ? reason : "requested");
}

void TrimmerTrimAllAggressive()
{
    // Hard trim ONLY for long-inactive background clients.
    // Farm keep-hot: hard only after HardTrimAfterSec; otherwise 2x interval.
    // Below-TrimSkipBelowMB processes are skipped entirely (0 syscalls
    // beyond the cached check). Focused instance is never touched.
    // FarmBoost ON: the whole farm runs hot — nothing is trimmed at all.
    if (TrimmingPaused()) {
        if (LogRateLimit("trim-pagein", 60))
            LOGI("[Trimmer] Aggressive skipped (page-in/FarmBoost in progress)");
        return;
    }
    DWORD focused = g_focusedPid.load();
    int skipMB = config_get_int("TASX", "TrimSkipBelowMB", 250);
    std::int64_t now = NowMs();
    std::int64_t hardAfterMs = IsFarmKeepHot() ? HardTrimAfterMs()
                                               : 2 * IntervalMs();
    unsigned softCount = 0, hardCount = 0;

    /* Everything - including the actual trim syscalls - runs under g_mtx.
       FIX: this used to copy HANDLEs into local vectors, release the lock,
       and THEN call Soft/HardTrim on them. That's exactly the hazard the
       comment above TrimClientLocked warns about: if ReleaseClient() (main
       thread) closes one of those PIDs in the gap between releasing the
       lock and using the handle, this thread trims through a closed (and
       possibly OS-recycled) handle - a real, if rare, use-after-close.
       Holding the lock for the whole pass costs a bit of contention with
       the scheduler thread but makes the module's own stated invariant
       actually true. */
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& kv : g_handles) {
        if (kv.first == focused) continue;
        auto itW = g_wsCache.find(kv.first);
        auto itT = g_wsTime.find(kv.first);
        if (skipMB > 0 && itW != g_wsCache.end() && itT != g_wsTime.end() &&
            (now - itT->second) <= 30000 &&
            itW->second < (SIZE_T)(unsigned)skipMB << 20)
            continue; // below threshold, fresh cache: skip
        auto itU = g_unfocusedSince.find(kv.first);
        std::int64_t age = (itU != g_unfocusedSince.end()) ? (now - itU->second) : 0;
        if (age > hardAfterMs) {
            HardTrim(kv.second, kv.first, "aggressive-idle-timeout");
            ++hardCount;
        } else {
            SoftTrim(kv.second, kv.first, "aggressive-idle");
            ++softCount;
        }
    }
    if (softCount || hardCount)
        LOGI("[Trimmer] Aggressive: %u soft, %u hard (reason: low-mem/commit, focused untouched)",
             softCount, hardCount);
}

void StopAllTrimmers()
{
    if (g_hStop) SetEvent(g_hStop);
    if (g_hThread) {
        /* The scheduler only waits on g_hStop/g_hWake and must be fully
           stopped before either event is closed.  A timeout here would leave
           a live thread dereferencing handles that the teardown immediately
           invalidates. */
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = nullptr;
    }
    if (g_hStop) { CloseHandle(g_hStop); g_hStop = nullptr; }
    if (g_hWake) { CloseHandle(g_hWake); g_hWake = nullptr; }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_handles.clear();
    g_wsCache.clear();
    g_wsTime.clear();
    g_unfocusedSince.clear();
    g_idle.clear();
    while (!g_heap.empty()) g_heap.pop();
}
