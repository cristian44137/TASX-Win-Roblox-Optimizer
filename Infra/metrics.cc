#include "metrics.h"

#include "config.h"
#include "log.h"
#include "lograte.h"
#include "stats.h"
#include "trimmer.h"

#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct TrimRecord {
    TrimMetricSnapshot s{};
    std::string type;
    ULONGLONG recordedAt = 0;
    ULONGLONG recoveryMs = 0;
    bool recovered = false;
};

struct ClientSample {
    ULONGLONG at = 0;
    ULONGLONG cycleTime = 0;
    ULONGLONG ioBytes = 0;
    SIZE_T workingSet = 0;
    SIZE_T privateBytes = 0;
    bool hasCpu = false;
    bool hasIo = false;
};

std::mutex g_mutex;
std::deque<TrimRecord> g_records;
std::unordered_map<DWORD, ClientSample> g_lastSamples;
std::unordered_map<DWORD, ULONGLONG> g_lastCommitAction;
std::unordered_map<DWORD, ULONGLONG> g_lastRecoverySample;
ULONGLONG g_lastMetricsReport = 0;
ULONGLONG g_lastClientSample = 0;
ULONGLONG g_lastCommitCheck = 0;
ULONGLONG g_lastBudgetReport = 0;

constexpr size_t kMaxRecords = 1000;
constexpr ULONGLONG kRecoveryWindowMs = 30ULL * 60ULL * 1000ULL;

ULONGLONG NowMs()
{
    return GetTickCount64();
}

bool EqualInsensitive(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == *b;
}

bool IsTrimMetricsEnabled()
{
    return config_get_bool("TASX", "TrimMetrics", 1) != 0;
}

int NetworkBytesForPid(DWORD pid)
{
    /* Documented process I/O counters are used as a conservative network
       activity proxy. TCP owner-PID tables provide connection counts, but do
       not provide byte counters without enabling TCP extended statistics;
       enabling those is privileged and not necessary for the safe default.
       This helper intentionally has no side effects. */
    (void)pid;
    return -1;
}

bool IsFocusedPid(DWORD pid)
{
    HWND hwnd = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &foregroundPid);
    return foregroundPid == pid;
}

bool HasMinimalNetworkActivity(DWORD pid, ULONGLONG ioDelta)
{
    int configured = config_get_int("TASX", "IdleTrimNetworkBytes", 1024);
    if (configured < 0) configured = 1024;
    /* Until a documented per-process network-byte API is enabled, classify
       only observed process I/O as activity. Unknown network state is not
       treated as idle, which keeps aggressive trim conservative. */
    int tcpBytes = NetworkBytesForPid(pid);
    if (tcpBytes >= 0) return tcpBytes <= configured;
    return ioDelta <= (ULONGLONG)configured;
}

bool IsCpuLow(const ClientSample& previous, const ProcStat& current,
              ULONGLONG elapsedMs)
{
    int limit = config_get_int("TASX", "IdleTrimCpuPct", 2);
    if (limit < 0) limit = 2;
    if (!previous.hasCpu || !current.hasCpuTime || elapsedMs == 0) return false;

    ULONGLONG delta = current.cycleTime >= previous.cycleTime
        ? current.cycleTime - previous.cycleTime : 0;
    /* QueryProcessCycleTime reports cycles, not 100-ns CPU time. The exact
       CPU percentage requires the process's processor affinity/topology;
       conservatively compare against elapsed wall time * nominal 1 GHz and
       require a very small result. A missing/ambiguous sample is active. */
    long double pctAt1GHz = ((long double)delta /
                             ((long double)elapsedMs * 10000000.0L)) * 100.0L;
    return pctAt1GHz <= (long double)limit;
}

void LogIdleState(DWORD pid, bool idle, bool focused, bool cpuLow,
                  bool networkLow, ULONGLONG idleMs)
{
    const char* state = idle ? "idle" : "active";
    if (LogRateLimit("idle-state", 60)) {
        LOGI("[Idle] PID %lu=%s focus=%d cpu_low=%d network_low=%d idle_ms=%llu",
             pid, state, focused ? 1 : 0, cpuLow ? 1 : 0,
             networkLow ? 1 : 0, (unsigned long long)idleMs);
    }
}

void UpdateRecoveryLocked(ULONGLONG nowMs, DWORD pid, SIZE_T ws)
{
    auto it = g_lastRecoverySample.find(pid);
    if (it == g_lastRecoverySample.end()) return;

    /* A recovery sample is the first post-trim sample at least 50 MiB above
       the post-trim WS. The metric is deliberately "unknown" until observed
       and is never converted into a guessed percentage. */
    for (auto& record : g_records) {
        if (record.s.pid != pid || record.recovered) continue;
        if (record.recordedAt > it->second) continue;
        const SIZE_T threshold = record.s.wsAfter + (SIZE_T)50 * 1024 * 1024;
        if (ws >= threshold) {
            record.recoveryMs = nowMs - record.recordedAt;
            record.recovered = true;
        }
    }
}

void ReportTrimAggregatesLocked()
{
    if (g_records.empty()) return;

    long double wsBefore = 0.0L;
    long double wsAfter = 0.0L;
    long double recoveryMs = 0.0L;
    size_t recoveryCount = 0;
    size_t useful = 0;
    size_t useless = 0;

    for (const auto& record : g_records) {
        wsBefore += (long double)record.s.wsBefore;
        wsAfter += (long double)record.s.wsAfter;
        /* "Useful" means the measured post-WS is lower than pre-WS. There is
           no claim that this persists or improves gameplay. */
        if (record.s.wsAfter < record.s.wsBefore) ++useful;
        else ++useless;
        if (record.recovered) {
            recoveryMs += (long double)record.recoveryMs;
            ++recoveryCount;
        }
    }

    const long double n = (long double)g_records.size();
    LOGI("[TrimMetrics] records=%u avg_ws_before_mb=%.1f avg_ws_after_mb=%.1f "
         "avg_recovery_ms=%s useful=%u useless=%u recovery_samples=%u",
         (unsigned)g_records.size(),
         (double)(wsBefore / n / (1024.0L * 1024.0L)),
         (double)(wsAfter / n / (1024.0L * 1024.0L)),
         recoveryCount ? std::to_string((unsigned long long)(recoveryMs / recoveryCount)).c_str() : "unknown",
         (unsigned)useful, (unsigned)useless, (unsigned)recoveryCount);
}

int CommitAction()
{
    const char* action = config_get_str("TASX", "CommitLimitAction", "warn");
    if (EqualInsensitive(action, "off")) return 0;
    if (EqualInsensitive(action, "softtrim")) return 2;
    return 1; /* warn is the safe fallback for invalid values */
}

void LogInvalidCommitAction()
{
    const char* action = config_get_str("TASX", "CommitLimitAction", "warn");
    if (!EqualInsensitive(action, "off") && !EqualInsensitive(action, "warn") &&
        !EqualInsensitive(action, "softtrim")) {
        if (LogRateLimit("commit-action-invalid", 3600))
            LOGW("[Commit] Invalid CommitLimitAction='%s'; using warn (allowed: off|warn|softtrim)", action);
    }
}

} // namespace

void MetricsRecordTrim(const TrimMetricSnapshot& snapshot)
{
    if (!IsTrimMetricsEnabled()) return;

    TrimRecord record;
    record.s = snapshot;
    record.type = snapshot.type ? snapshot.type : "unknown";
    record.recordedAt = NowMs();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_records.push_back(std::move(record));
    /* Set the pointer only after the move into the deque. With short-string
       optimization, setting it before push_back can leave TrimMetricSnapshot
       pointing at the moved-from stack object's buffer. */
    g_records.back().s.type = g_records.back().type.c_str();
    const ULONGLONG recordedAt = g_records.back().recordedAt;
    while (g_records.size() > kMaxRecords)
        g_records.pop_front();
    g_lastRecoverySample[snapshot.pid] = recordedAt;
}

void MetricsMaybeReport(ULONGLONG nowMs)
{
    if (!IsTrimMetricsEnabled()) return;
    int interval = config_get_int("TASX", "TrimLogIntervalSec", 60);
    if (interval < 1) interval = 60;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lastMetricsReport != 0 &&
        nowMs - g_lastMetricsReport < (ULONGLONG)interval * 1000)
        return;
    g_lastMetricsReport = nowMs;
    ReportTrimAggregatesLocked();
}

void MetricsSampleClients(ULONGLONG nowMs)
{
    int interval = config_get_int("TASX", "IdleTrimSampleIntervalSec", 5);
    if (interval < 1) interval = 5;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_lastClientSample != 0 &&
            nowMs - g_lastClientSample < (ULONGLONG)interval * 1000)
            return;
        g_lastClientSample = nowMs;
    }

    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;

    std::unordered_set<DWORD> seen;
    for (auto& stat : stats) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, stat.pid);
        if (!process) continue;
        EnrichProcStat(process, stat);
        CloseHandle(process);

        /* The trimmer owns the tracked-PID set; this observer only enriches
           Roblox-looking rows and logs state. The no-injection architecture
           avoids deriving network state from undocumented internals. */
        if (stat.name.empty()) continue;
        std::wstring lower = stat.name;
        for (wchar_t& c : lower) c = (wchar_t)towlower(c);
        if (lower.find(L"robloxplayerbeta.exe") == std::wstring::npos &&
            lower.find(L"\\roblox.exe") == std::wstring::npos)
            continue;

        /* Mark the PID as live before the first-sample fast path so every
           per-PID observer map is pruned consistently below. */
        seen.insert(stat.pid);

        ClientSample previous;
        bool havePrevious = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_lastSamples.find(stat.pid);
            if (it != g_lastSamples.end()) {
                previous = it->second;
                havePrevious = true;
            }
            g_lastSamples[stat.pid] = {
                nowMs,
                stat.cycleTime,
                stat.ioReadBytes + stat.ioWriteBytes,
                stat.workingSet,
                stat.privateBytes,
                stat.hasCpuTime,
                stat.hasIoCounters
            };
            UpdateRecoveryLocked(nowMs, stat.pid, stat.workingSet);
        }

        if (!havePrevious) continue;
        ULONGLONG elapsed = nowMs >= previous.at ? nowMs - previous.at : 0;
        ULONGLONG ioNow = stat.ioReadBytes + stat.ioWriteBytes;
        ULONGLONG ioDelta = ioNow >= previous.ioBytes ? ioNow - previous.ioBytes : 0;
        bool focused = IsFocusedPid(stat.pid);
        bool cpuLow = IsCpuLow(previous, stat, elapsed);
        bool networkLow = stat.hasIoCounters && HasMinimalNetworkActivity(stat.pid, ioDelta);
        int idleSec = config_get_int("TASX", "IdleTrimSec", 60);
        if (idleSec < 1) idleSec = 60;
        bool idle = !focused && cpuLow && networkLow &&
                    elapsed >= (ULONGLONG)idleSec * 1000;
        TrimmerUpdateIdle(stat.pid, idle ? 1 : 0, nowMs);
        LogIdleState(stat.pid, idle, focused, cpuLow, networkLow,
                     idle ? elapsed : 0);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_lastSamples.begin(); it != g_lastSamples.end();) {
        if (!seen.count(it->first)) it = g_lastSamples.erase(it);
        else ++it;
    }
    for (auto it = g_lastCommitAction.begin(); it != g_lastCommitAction.end();) {
        if (!seen.count(it->first)) it = g_lastCommitAction.erase(it);
        else ++it;
    }
    for (auto it = g_lastRecoverySample.begin(); it != g_lastRecoverySample.end();) {
        if (!seen.count(it->first)) it = g_lastRecoverySample.erase(it);
        else ++it;
    }
}

void MetricsCheckCommitLimits(ULONGLONG nowMs)
{
    int interval = config_get_int("TASX", "CommitCheckIntervalSec", 30);
    if (interval < 1) interval = 30;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_lastCommitCheck != 0 &&
            nowMs - g_lastCommitCheck < (ULONGLONG)interval * 1000)
            return;
        g_lastCommitCheck = nowMs;
    }

    LogInvalidCommitAction();
    int limitMB = config_get_int("TASX", "PerClientCommitLimitMB", 4096);
    int cooldown = config_get_int("TASX", "CommitLimitCooldownSec", 120);
    if (limitMB <= 0) return;
    if (cooldown < 0) cooldown = 120;

    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;
    const SIZE_T limitBytes = (SIZE_T)(unsigned)limitMB << 20;
    const int action = CommitAction();

    for (auto& stat : stats) {
        if (stat.privateBytes <= limitBytes || stat.name.empty()) continue;
        std::wstring lower = stat.name;
        for (wchar_t& c : lower) c = (wchar_t)towlower(c);
        if (lower.find(L"robloxplayerbeta.exe") == std::wstring::npos &&
            lower.find(L"\\roblox.exe") == std::wstring::npos)
            continue;

        bool focused = IsFocusedPid(stat.pid);
        LOGW("[Commit] PID %lu PrivateBytes=%llu MB exceeds PerClientCommitLimitMB=%d (action=%s, focused=%d)",
             stat.pid, (unsigned long long)(stat.privateBytes >> 20), limitMB,
             action == 0 ? "off" : action == 2 ? "softtrim" : "warn",
             focused ? 1 : 0);
        if (action != 2 || focused) continue;

        bool allowed = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_lastCommitAction.find(stat.pid);
            allowed = it == g_lastCommitAction.end() ||
                      nowMs - it->second >= (ULONGLONG)cooldown * 1000;
            if (allowed) g_lastCommitAction[stat.pid] = nowMs;
        }
        if (!allowed) continue;

        /* CommitLimitAction=softtrim is explicitly opt-in. The trimmer still
           enforces its own focus/idle safety gate; no hard trim is requested. */
        TrimmerRequestSoftTrim(stat.pid, "commit-limit");
    }
}

void MetricsMaybeReportBudget(ULONGLONG nowMs)
{
    if (!config_get_bool("TASX", "BudgetPlannerEnabled", 1)) return;
    int interval = config_get_int("TASX", "BudgetPlannerLogIntervalSec", 300);
    if (interval < 1) interval = 300;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_lastBudgetReport != 0 &&
            nowMs - g_lastBudgetReport < (ULONGLONG)interval * 1000)
            return;
        g_lastBudgetReport = nowMs;
    }

    SystemMemoryStat mem;
    if (!QuerySystemMemoryStat(mem) || mem.totalPhys == 0) return;

    std::vector<ProcStat> stats;
    if (!QueryProcStats(stats)) return;
    ULONGLONG sumWs = 0;
    unsigned clients = 0;
    for (const auto& stat : stats) {
        if (stat.name.empty()) continue;
        std::wstring lower = stat.name;
        for (wchar_t& c : lower) c = (wchar_t)towlower(c);
        if (lower.find(L"robloxplayerbeta.exe") == std::wstring::npos &&
            lower.find(L"\\roblox.exe") == std::wstring::npos)
            continue;
        ++clients;
        sumWs += stat.workingSet;
    }

    const ULONGLONG safeReserve = std::max<ULONGLONG>(512ULL << 20,
                                                       mem.totalPhys / 5);
    ULONGLONG availableForClients = mem.totalPhys > safeReserve
        ? mem.totalPhys - safeReserve : 0;
    ULONGLONG target = clients ? availableForClients / clients : 0;
    int recommendedCap = (int)((target + ((1ULL << 20) - 1)) >> 20);
    LOGI("[Budget] RAM_total_mb=%llu clients=%u avg_ws_mb=%s target_ws_mb=%s "
         "recommended_limit_mb=%d safe_system_reserve_mb=%llu enforce=%d",
         (unsigned long long)(mem.totalPhys >> 20), clients,
         clients ? std::to_string((unsigned long long)(sumWs / clients >> 20)).c_str() : "unknown",
         clients ? std::to_string((unsigned long long)(target >> 20)).c_str() : "unknown",
         recommendedCap,
         (unsigned long long)(safeReserve >> 20),
         config_get_bool("TASX", "DynamicMemoryCapEnforce", 0));

    if (config_get_bool("TASX", "DynamicMemoryCapEnforce", 0)) {
        LOGW("[Budget] DynamicMemoryCapEnforce=1 is requested, but automatic JobMemoryCapMB changes are not implemented without a measured rollback policy; current cap was not changed");
    }
}

void MetricsReset(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_records.clear();
    g_lastSamples.clear();
    g_lastCommitAction.clear();
    g_lastRecoverySample.clear();
    g_lastMetricsReport = 0;
    g_lastClientSample = 0;
    g_lastCommitCheck = 0;
    g_lastBudgetReport = 0;
}
