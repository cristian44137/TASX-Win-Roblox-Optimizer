#include "respawn.h"

#include "config.h"
#include "log.h"
#include "lograte.h"
#include "ntsys.h"

#include <unordered_set>

/* Windows command-line grammar helpers.
   SplitCmdLine is exactly the grammar CommandLineToArgvW implements (the
   one every CRT uses to rebuild argv) - using the real API keeps us honest
   with rounding quirks like trailing backslashes before a closing quote. */
std::vector<std::wstring> SplitCmdLine(const std::wstring& cmdline)
{
    std::vector<std::wstring> out;
    if (cmdline.empty()) return out;

    std::wstring copy = cmdline;
    copy.push_back(L'\0');

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(copy.data(), &argc);
    if (!argv) return out;

    out.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i)
        out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

/* Reverse of CommandLineToArgvW: rebuilds a command line that the parser
   splits back into exactly argv. An argument is quoted when it contains
   whitespace or a quote; embedded quotes become \" (backslash count before
   the closing quote is doubled so a trailing backslash round-trips). */
std::wstring BuildCmdLine(const std::vector<std::wstring>& argv)
{
    std::wstring cmd;
    bool first = true;
    for (const auto& raw : argv) {
        if (!first) cmd += L' ';
        first = false;

        std::wstring a = raw;
        bool needQuote = a.find_first_of(L" \t\"") != std::wstring::npos;
        if (!needQuote) {
            cmd += a;
            continue;
        }

        std::wstring q = L"\"";
        size_t run = 0; /* consecutive backslashes before the next char */
        size_t i = 0;
        while (i < a.size()) {
            wchar_t c = a[i];
            if (c == L'\\') { ++run; ++i; continue; }
            if (c == L'\"') {
                /* backslashes BEFORE a quote are literal (keep them), the
                   quote itself gets escaped */
                q.append(run, L'\\');
                run = 0;
                q += L"\\\"";
                ++i;
                continue;
            }
            q.append(run, L'\\');
            run = 0;
            q += c;
            ++i;
        }
        /* trailing backslashes before the closing quote must be doubled */
        q.append(run * 2, L'\\');
        q += L'\"';
        cmd += q;
    }
    return cmd;
}
/* Sliding-window rate limiters for spawn bursts. */
namespace {

std::unordered_map<DWORD, std::wstring> g_cmdlines;
std::unordered_map<DWORD, ULONGLONG> g_pendingRespawns;
/* The PID changes after every respawn, so a PID-keyed limit is trivially
   bypassed by a crash-loop. Keep the sliding window by exact command-line
   lineage; the replacement PID inherits the same bucket when it is
   discovered and remembered. */
std::unordered_map<std::wstring, std::vector<ULONGLONG>> g_lineageTimes; /* ms */
std::vector<ULONGLONG> g_globalTimes;                            /* ms */
constexpr size_t kMaxLineageHistory = 1024;
constexpr size_t kMaxGlobalHistory = 8192;
#ifdef TASX_TEST
int g_testCommitPercent = -2;
#endif

static void PruneWindow(std::vector<ULONGLONG>& v, ULONGLONG now)
{
    size_t c = 0;
    for (ULONGLONG t : v)
        if (now - t < 3600000ull) v[c++] = t;   /* last 1h */
    v.resize(c);
}

static bool HourOverLimit(std::vector<ULONGLONG>& v, ULONGLONG now, int cap)
{
    PruneWindow(v, now);
    if (cap <= 0) return false; /* 0 = unlimited, history still bounded */
    return (int)v.size() >= cap;
}

static void BoundHistory(std::vector<ULONGLONG>& v, size_t maxEntries)
{
    if (v.size() <= maxEntries) return;
    size_t drop = v.size() - maxEntries;
    v.erase(v.begin(), v.begin() + (std::ptrdiff_t)drop);
}

static int CurrentCommitPercent()
{
#ifdef TASX_TEST
    if (g_testCommitPercent != -2)
        return g_testCommitPercent;
#endif
    return tasx_get_commit_percent();
}

static void PruneLineageState(ULONGLONG now)
{
    std::unordered_set<std::wstring> active;
    active.reserve(g_cmdlines.size());
    for (const auto& kv : g_cmdlines)
        active.insert(kv.second);

    for (auto it = g_lineageTimes.begin(); it != g_lineageTimes.end();) {
        PruneWindow(it->second, now);
        if (it->second.empty() && !active.count(it->first))
            it = g_lineageTimes.erase(it);
        else
            ++it;
    }
    PruneWindow(g_globalTimes, now);
    BoundHistory(g_globalTimes, kMaxGlobalHistory);
}

} /* namespace */

void RespawnRemember(DWORD pid, const std::wstring& cmdline)
{
    if (pid == 0 || cmdline.empty()) return;
    PruneLineageState(GetTickCount64());
    g_cmdlines[pid] = cmdline;
    g_lineageTimes[cmdline]; /* ensure the crash-loop bucket exists */
}

void RespawnForget(DWORD pid)
{
    g_cmdlines.erase(pid);
    g_pendingRespawns.erase(pid);
}

void RespawnClearAll(void)
{
    g_cmdlines.clear();
    g_pendingRespawns.clear();
    g_lineageTimes.clear();
    g_globalTimes.clear();
}

int RespawnConfigured(void)
{
    return config_get_bool("TASX", "RespawnOnCrash", 0) &&
           !g_cmdlines.empty();
}

size_t RespawnPendingCount(void)
{
    return g_cmdlines.size();
}

int RespawnTrySpawn(DWORD exitedPid)
{
    if (exitedPid == 0) return 0;
    if (!config_get_bool("TASX", "RespawnOnCrash", 0))
        return 0;

    auto it = g_cmdlines.find(exitedPid);
    if (it == g_cmdlines.end())
        return 0; /* never hooked (e.g. legacy pre-respawn run) - nothing to respawn */

    std::wstring cmdline = it->second;
    ULONGLONG now = GetTickCount64();
    PruneLineageState(now);
    int perLineageCap = config_get_int("TASX", "RespawnPerHourMax", 6);
    if (perLineageCap < 0) perLineageCap = 0;
    auto bucket = g_lineageTimes.find(cmdline);
    if (bucket != g_lineageTimes.end() &&
        HourOverLimit(bucket->second, now, perLineageCap)) {
        if (LogRateLimit("respawn-perpid-limit", 300))
            LOGW("[TASX] Respawn cap hit for command-line lineage of PID %lu (%d/h) - client stays dead to avoid a crash loop",
                 exitedPid, perLineageCap);
        g_cmdlines.erase(it);
        g_pendingRespawns.erase(exitedPid);
        return 0;
    }

    int globalCap = config_get_int("TASX", "RespawnGlobalHourCap", 60);
    if (globalCap < 0) globalCap = 0;
    if (HourOverLimit(g_globalTimes, now, globalCap)) {
        if (LogRateLimit("respawn-global-limit", 300))
            LOGW("[TASX] Global respawn cap reached (%d/h) - refusing further auto-respawns", globalCap);
        g_cmdlines.erase(it);
        g_pendingRespawns.erase(exitedPid);
        return 0;
    }

    /* Honour the commit-charge guard: spawning into an OOM-ing system just
       makes the next crash worse. */
    int blockPct = config_get_int("TASX", "CommitBlockThreshold", 85);
    if (blockPct <= 0) blockPct = 85;
    int curPct = CurrentCommitPercent();
    if (curPct >= 0 && curPct >= blockPct) {
        if (LogRateLimit("respawn-commit-block", 60))
            LOGW("[TASX] Respawn of PID %lu deferred - commit charge %d%% >= %d%%",
                 exitedPid, curPct, blockPct);
        g_pendingRespawns[exitedPid] = now + 5000;
        return 0;
    }

    /* Normalize the captured command line (split -> rebuild).
       CommandLineToArgvW is lossless here: split(build(split(x))) == split(x),
       so this can only clean up quoting cosmetics, never change arguments. */
    std::vector<std::wstring> argv = SplitCmdLine(cmdline);
    if (argv.empty()) {
        LOGW("[TASX] Respawn of PID %lu skipped - unreadable command line", exitedPid);
        g_cmdlines.erase(it);
        g_pendingRespawns.erase(exitedPid);
        return 0;
    }
    std::wstring flat = BuildCmdLine(argv);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, flat.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        if (LogRateLimit("respawn-fail", 60))
            LOGW("[TASX] Respawn of PID %lu failed (err %lu)",
                 exitedPid, (unsigned long)GetLastError());
        g_cmdlines.erase(it);
        g_pendingRespawns.erase(exitedPid);
        return 0;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    bucket->second.push_back(now);
    BoundHistory(bucket->second, kMaxLineageHistory);
    g_globalTimes.push_back(now);
    BoundHistory(g_globalTimes, kMaxGlobalHistory);
    g_cmdlines.erase(it); /* replacement PID will reattach to this lineage */
    g_pendingRespawns.erase(exitedPid);

    LOGI("[TASX] Respawned client PID %lu (was %lu) | cmd: %ls",
         (unsigned long)pi.dwProcessId, exitedPid, flat.c_str());
    return 1;
}

void RespawnRetryPending(void)
{
    if (g_pendingRespawns.empty()) return;

    ULONGLONG now = GetTickCount64();
    std::vector<DWORD> due;
    due.reserve(g_pendingRespawns.size());
    for (const auto& kv : g_pendingRespawns)
        if (now >= kv.second)
            due.push_back(kv.first);

    for (DWORD pid : due) {
        auto it = g_pendingRespawns.find(pid);
        if (it == g_pendingRespawns.end()) continue;
        g_pendingRespawns.erase(it);
        if (g_cmdlines.find(pid) != g_cmdlines.end())
            RespawnTrySpawn(pid);
    }
}

#ifdef TASX_TEST
void RespawnTestSetCommitPercent(int pct)
{
    g_testCommitPercent = pct;
}

void RespawnTestForcePendingDue(DWORD pid)
{
    auto it = g_pendingRespawns.find(pid);
    if (it != g_pendingRespawns.end())
        it->second = 0;
}

size_t RespawnTestDeferredCount(void)
{
    return g_pendingRespawns.size();
}
#endif
