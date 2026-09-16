#include "warm.h"

#include "config.h"
#include "log.h"
#include "lograte.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

namespace {

std::mutex g_primedPathsMtx;
std::unordered_set<std::wstring> g_primedPaths; // once per version dir
std::mutex g_workersMtx;
std::condition_variable g_workersCv;
size_t g_activeWorkers = 0;
bool g_shuttingDown = false;
constexpr size_t kMaxWarmWorkers = 2;

struct WarmWorkerGuard {
    ~WarmWorkerGuard()
    {
        std::lock_guard<std::mutex> lock(g_workersMtx);
        if (g_activeWorkers > 0) --g_activeWorkers;
        g_workersCv.notify_all();
    }
};

struct PrimedPathReservation {
    std::wstring path;
    bool committed = false;

    ~PrimedPathReservation()
    {
        if (committed) return;
        std::lock_guard<std::mutex> lock(g_primedPathsMtx);
        g_primedPaths.erase(path);
    }
};

typedef BOOL (WINAPI *PrefetchVirtualMemoryFn)(HANDLE hProcess,
                                               ULONG_PTR numberOfEntries,
                                               void* virtualAddresses,
                                               ULONG flags);

struct RANGE {
    void*  start;
    SIZE_T bytes;
};

PrefetchVirtualMemoryFn GetPrefetchFn()
{
    static PrefetchVirtualMemoryFn fn = nullptr;
    static std::once_flag once;
    std::call_once(once, [&]() {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32)
            fn = TasxProcFn<PrefetchVirtualMemoryFn>(k32, "PrefetchVirtualMemory");
    });
    return fn; /* null on Win7 -> caller falls back to sequential reads */
}

/* Maps the file (shared, named file mapping of the on-disk image) and pulls
   its pages into the shared standby cache once. */
bool WarmFile(const std::wstring& path, ULONGLONG& budget)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0) {
        CloseHandle(hFile);
        return false;
    }

    ULONGLONG bytes = (ULONGLONG)size.QuadPart;
    if (bytes > budget) { CloseHandle(hFile); return false; }

    bool warmed = false;
    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (hMap) {
        void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (view) {
            PrefetchVirtualMemoryFn fn = GetPrefetchFn();
            if (fn) {
                RANGE r{ view, (SIZE_T)bytes };
                warmed = fn(GetCurrentProcess(), 1, &r, 0) != FALSE;
            }
            UnmapViewOfFile(view);
        }
        CloseHandle(hMap);
    }

    if (!warmed) {
        /* PrefetchVirtualMemory is optional and mapping can fail transiently.
           Sequentially touch the file as a last-resort fallback, and count
           it as success only when the complete read finishes without an API
           error. This keeps the reservation/budget accounting honest. */
        LARGE_INTEGER zero{};
        BOOL readOk = SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN);
        char sink[1 << 20];
        DWORD got = 0;
        while (readOk) {
            if (!ReadFile(hFile, sink, sizeof(sink), &got, nullptr)) {
                readOk = FALSE;
                break;
            }
            if (!got) break;
        }
        warmed = readOk != FALSE;
    }

    if (warmed)
        budget -= bytes;
    CloseHandle(hFile);
    return warmed;
}

int FilePriority(const wchar_t* name)
{
    const wchar_t* ext = wcsrchr(name, L'.');
    if (!ext) return 2;
    if (_wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".dll") == 0) return 0;
    if (_wcsicmp(ext, L".pak") == 0) return 1;
    return 2;
}

} /* namespace */

void WarmClientFilesAsync(DWORD pid)
{
    if (!config_get_bool("TASX", "WarmClientFiles", 1)) return;
    /* Once-per-version is inherent (g_primedPaths); no config key. */

    std::lock_guard<std::mutex> lifecycleLock(g_workersMtx);
    if (g_shuttingDown) return;
    if (g_activeWorkers >= kMaxWarmWorkers) {
        /* Warming is an optional cache optimization.  Never let a spawn
           burst turn it into an unbounded disk-I/O fan-out. */
        return;
    }

    // Cache primed paths globally — only prime once per version directory.
    ++g_activeWorkers;
    try {
        std::thread([pid]() {
        WarmWorkerGuard workerGuard;
        try {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return;

        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(h, 0, path, &size);
        CloseHandle(h);
        if (!ok) return;

        std::wstring dir(path);
        size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        dir.resize(slash);

        // BUG 6: version-dir cache - reserve the dir BEFORE the warm pass so
        // concurrent clients of the same version don't repeat it. (This also
        // fixes the old self-deadlock: a scope-wide lock_guard was taken here
        // and again after priming on the same non-recursive mutex.) Scoped
        // block: the lock is released immediately after the check/insert.
        {
            std::lock_guard<std::mutex> lock(g_primedPathsMtx);
            if (g_primedPaths.count(dir)) {
                return;  // already primed this version
            }
            /* Version directories accumulate over months. First remove
               entries for uninstalled versions; only refuse a new
               reservation when the live set is still at the hard ceiling.
               This keeps the optional optimization bounded without causing a
               bulk re-warm storm. */
            if (g_primedPaths.size() >= 1024) {
                for (auto it = g_primedPaths.begin(); it != g_primedPaths.end();) {
                    if (GetFileAttributesW(it->c_str()) == INVALID_FILE_ATTRIBUTES)
                        it = g_primedPaths.erase(it);
                    else
                        ++it;
                }
            }
            if (g_primedPaths.size() >= 1024) {
                if (LogRateLimit("warm-path-cap", 3600))
                    LOGW("[Warm] Version-directory cache reached its safety cap; new versions will be skipped");
                return;
            }
            g_primedPaths.insert(dir);
        }
        PrimedPathReservation reservation{dir};

        ULONGLONG budget =
            (ULONGLONG)std::max(0, config_get_int("TASX", "WarmMaxMB", 256)) << 20;
        if (budget < (1ull << 20)) return;

        /* exe/dll first (image pages shared by every instance), then pak. */
        struct File { std::wstring name; ULONGLONG bytes; int prio; };
        std::vector<File> files;

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ULONGLONG lo = fd.nFileSizeLow, hi = fd.nFileSizeHigh;
            files.push_back({ fd.cFileName, lo | (hi << 32),
                              FilePriority(fd.cFileName) });
        } while (FindNextFileW(find, &fd));
        FindClose(find);

        std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
            if (a.prio != b.prio) return a.prio < b.prio;
            return a.bytes > b.bytes;
        });

        ULONGLONG warmed = 0;
        for (const auto& f : files) {
            if (budget < (1ull << 20)) break;
            if (WarmFile(dir + L"\\" + f.name, budget))
                warmed += f.bytes;
        }

        if (warmed) {
            reservation.committed = true;
            if (LogRateLimit("warm-primed", 10)) {
                char narrow[MAX_PATH];
                size_t conv = 0;
                wcstombs_s(&conv, narrow, sizeof(narrow), dir.c_str(), _TRUNCATE);
                LOGI("[Warm] Shared pages primed (once): %llu MB from %s",
                     warmed >> 20, narrow);
            }
        }
        /* On failure (including an empty budget or missing directory), the
           reservation guard removes the path so a later client can retry. */
        } catch (...) {
            /* A malformed install, allocation failure, or an API edge case
               must never escape a detached worker and terminate TASX. */
            if (LogRateLimit("warm-worker-fail", 60))
                LOGW("[Warm] Worker failed; continuing without file warming");
        }
        }).detach();
    } catch (...) {
        --g_activeWorkers;
        /* Worker creation is optional. A failed warm task must never affect
           client ownership or bring down the long-running agent. */
        if (LogRateLimit("warm-thread-fail", 60))
            LOGW("[Warm] Worker creation failed; continuing without file warming");
    }
}

void WarmShutdown()
{
    std::unique_lock<std::mutex> lock(g_workersMtx);
    g_shuttingDown = true;
    g_workersCv.wait(lock, [] { return g_activeWorkers == 0; });
}
