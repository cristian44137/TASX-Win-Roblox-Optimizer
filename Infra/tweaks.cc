#include "tweaks.h"

#include "config.h"
#include "ntsys.h"
#include "log.h"
#include "lograte.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace {

void SetRegValueDWORD(HKEY root, const wchar_t* path, const wchar_t* name,
                      DWORD value, const char* label)
{
    HKEY key = nullptr;
    DWORD cur = 0, type = 0, size = sizeof(cur);

    LONG qr = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (qr == ERROR_FILE_NOT_FOUND)
        qr = RegCreateKeyExW(root, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (qr != ERROR_SUCCESS) {
        LOGW("[TASX] Tweak '%s' skipped (registry access denied)", label);
        return;
    }

    qr = RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)&cur, &size);
    if (qr == ERROR_SUCCESS && type == REG_DWORD && cur == value) {
        RegCloseKey(key);
        return; /* already correct - keep the log quiet */
    }

    if (RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value,
                       sizeof(value)) == ERROR_SUCCESS)
        LOGI("[TASX] Tweak applied: %s", label);
    else
        LOGW("[TASX] Tweak '%s' write failed", label);

    RegCloseKey(key);
}

void SetRegValueString(HKEY root, const wchar_t* path, const wchar_t* name,
                       const wchar_t* value, const char* label)
{
    HKEY key = nullptr;
    wchar_t cur[256] = {};
    DWORD type = 0, size = sizeof(cur) - sizeof(wchar_t);

    LONG qr = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (qr == ERROR_FILE_NOT_FOUND)
        qr = RegCreateKeyExW(root, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (qr != ERROR_SUCCESS) {
        LOGW("[TASX] Tweak '%s' skipped (registry access denied)", label);
        return;
    }

    qr = RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)cur, &size);
    if (qr == ERROR_SUCCESS && type == REG_SZ && wcscmp(cur, value) == 0) {
        RegCloseKey(key);
        return;
    }

    DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    if (RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, bytes) == ERROR_SUCCESS)
        LOGI("[TASX] Tweak applied: %s", label);
    else
        LOGW("[TASX] Tweak '%s' write failed", label);

    RegCloseKey(key);
}

/* Dynamic PowrProf bindings (header availability differs across SDKs). */
typedef unsigned long (WINAPI *PowerGetActiveSchemeFn)(unsigned char*, GUID**);
typedef unsigned long (WINAPI *PowerSetActiveSchemeFn)(unsigned char*, const GUID*);

/* Desktop heap expansion for 100+ windowed clients. Touches the csrss
   boot parameters (SharedSection=a,b,c -> b is the interactive desktop
   heap in KB). OPT-IN ONLY: a malformed write here can break booting.
   The edit is purely numeric, everything else stays byte-identical. */
void ExpandDesktopHeap()
{
    if (!config_get_bool("TASX", "DesktopHeapExpand", 0)) return;

    const wchar_t* subkey =
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\SubSystems";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;

    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, L"Windows", nullptr, &type, nullptr, &size) !=
            ERROR_SUCCESS ||
        type != REG_EXPAND_SZ || size < 8 || size > 4096)
    {
        RegCloseKey(key);
        return;
    }

    std::wstring val(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, L"Windows", nullptr, &type, (LPBYTE)&val[0],
                         &size) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return;
    }
    if (!val.empty() && val.back() == L'\0') val.pop_back();

    size_t ss = val.find(L"SharedSection=");
    if (ss == std::wstring::npos) { RegCloseKey(key); return; }
    size_t p = ss + 14;

    /* first number (a) */
    while (p < val.size() && iswdigit(val[p])) ++p;
    if (p >= val.size() || val[p] != L',') { RegCloseKey(key); return; }
    size_t bStart = ++p;
    while (p < val.size() && iswdigit(val[p])) ++p;
    if (p == bStart) { RegCloseKey(key); return; }
    int heapKB = _wtoi(val.substr(bStart, p - bStart).c_str());

    const int targetKB = 30720; /* 30 MB interactive desktop heap */
    if (heapKB >= targetKB) { RegCloseKey(key); return; }

    val = val.substr(0, bStart) + std::to_wstring(targetKB) + val.substr(p);
    DWORD bytes = (DWORD)((val.size() + 1) * sizeof(wchar_t));

    if (RegSetValueExW(key, L"Windows", 0, REG_EXPAND_SZ,
                       (const BYTE*)val.c_str(), bytes) == ERROR_SUCCESS)
        LOGI("[TASX] Desktop heap expanded to %d KB (reboot required)", targetKB);

    RegCloseKey(key);
}

PowerGetActiveSchemeFn GetPowerGetActiveScheme(HMODULE powr)
{
    return TasxProcFn<PowerGetActiveSchemeFn>(powr, "PowerGetActiveScheme");
}

PowerSetActiveSchemeFn GetPowerSetActiveScheme(HMODULE powr)
{
    return TasxProcFn<PowerSetActiveSchemeFn>(powr, "PowerSetActiveScheme");
}

const GUID kUltimatePerfGuid =
    { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
const GUID kHighPerfGuid =
    { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };

GUID g_prevScheme = {};
bool  g_prevSaved = false;
GUID g_appliedScheme = {};

bool GuidEqual(const GUID& a, const GUID& b)
{
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

bool IsTasxPowerScheme(const GUID& scheme)
{
    return GuidEqual(scheme, kUltimatePerfGuid) ||
           GuidEqual(scheme, kHighPerfGuid);
}

std::wstring PowerJournalPath()
{
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"TASX.powerstate";
    std::wstring result(path, n);
    size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"TASX.powerstate";
    return result.substr(0, slash + 1) + L"TASX.powerstate";
}

bool SavePowerJournal(const GUID& scheme)
{
    const std::wstring path = PowerJournalPath();
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, &scheme, sizeof(scheme), &written, nullptr) &&
              written == sizeof(scheme) && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool LoadPowerJournal(GUID& scheme)
{
    HANDLE h = CreateFileW(PowerJournalPath().c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    BOOL ok = ReadFile(h, &scheme, sizeof(scheme), &read, nullptr) &&
              read == sizeof(scheme);
    CloseHandle(h);
    return ok == TRUE;
}

void DeletePowerJournal()
{
    DeleteFileW(PowerJournalPath().c_str());
}

/* Full path of an installed RobloxPlayerBeta.exe for UserGpuPreferences
   (the key works best with a full path). First match under
   %LOCALAPPDATA%\Roblox\Versions; falls back to the bare exe name. */
std::wstring FindRobloxPlayerExe()
{
    wchar_t la[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH))
        return L"RobloxPlayerBeta.exe";

    std::wstring versions = std::wstring(la) + L"\\Roblox\\Versions";
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((versions + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return L"RobloxPlayerBeta.exe";

    std::wstring hit;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring exe = versions + L"\\" + fd.cFileName +
                           L"\\RobloxPlayerBeta.exe";
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            hit = exe;
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    return hit.empty() ? L"RobloxPlayerBeta.exe" : hit;
}

} /* namespace */

void TweaksPowerRecoverStale()
{
    GUID previous{};
    if (!LoadPowerJournal(previous)) return;

    HMODULE powr = LoadLibraryW(L"PowrProf.dll");
    auto get = powr ? GetPowerGetActiveScheme(powr) : nullptr;
    auto set = powr ? GetPowerSetActiveScheme(powr) : nullptr;
    GUID* current = nullptr;
    bool owned = get && get(0, &current) == 0 && current &&
                 IsTasxPowerScheme(*current);
    if (current) LocalFree(current);
    if (get && !owned) {
        /* A user/Windows change after a hard kill supersedes our journal.
           Never overwrite that external choice on the next launch. */
        DeletePowerJournal();
        LOGI("[TASX] Stale power journal ignored: active scheme was changed externally");
    } else if (set && owned && set(0, &previous) == 0) {
        DeletePowerJournal();
        LOGW("[TASX] Recovered power scheme left by an interrupted previous run");
    } else {
        LOGE("[TASX] Could not recover the previous power scheme; journal kept for the next startup");
    }
    if (powr) FreeLibrary(powr);
}

void TweaksApplyOneShot()
{
    if (!config_get_bool("TASX", "ApplyTweaks", 0)) return;

    bool elevated = tasx_is_elevated() != 0;
    if (!elevated && LogRateLimit("tweaks-hklm-skip", 3600))
        LOGW("[TASX] HKLM tweaks skipped (needs admin) - HKCU tweaks still applied");

    /* Game DVR off - its background capture pipeline costs FPS. */
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"System\\GameConfigStore", L"GameDVR_Enabled", 0, "GameDVR_Enabled=0");
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        L"AppCaptureEnabled", 0, "AppCaptureEnabled=0");
    if (elevated)
        SetRegValueDWORD(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR",
            L"AllowGameDVR", 0, "AllowGameDVR policy=0");

    /* Route Roblox to the high-performance GPU (full exe path when an
       install is found, bare name otherwise). */
    SetRegValueString(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
        FindRobloxPlayerExe().c_str(), L"GpuPreference=2;",
        "Roblox GPU preference = High performance");

    /* Game Mode on - Windows itself deprioritizes background work in game. */
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\GameBar",
        L"AutoGameModeEnabled", 1, "Game Mode enabled");
    SetRegValueDWORD(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\GameBar",
        L"AllowAutoGameMode", 1, "AllowAutoGameMode=1");

    /* MMCSS: no network throttling, system tuned for foreground work. */
    if (elevated) {
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        L"NetworkThrottlingIndex", 0xFFFFFFFFu, "NetworkThrottlingIndex=off");
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        L"SystemResponsiveness", 0, "SystemResponsiveness=0");

    /* MMCSS 'Games' task profile: max GPU/CPU scheduling priority. */
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"GPU Priority", 8, "Games: GPU Priority=8");
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"Priority", 6, "Games: Priority=6");
    SetRegValueString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"Scheduling Category", L"High", "Games: Scheduling Category=High");
    SetRegValueString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        L"SFIO Priority", L"High", "Games: SFIO Priority=High");

    /* Windowed-farm limits: per-process GDI/USER quotas (default 10 000
       gets exhausted around ~50 windowed clients). Reboot required. */
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
        L"GDIProcessHandleQuota", 65536, "GDIProcessHandleQuota=65536");
    SetRegValueDWORD(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
        L"USERProcessHandleQuota", 18000, "USERProcessHandleQuota=18000");

    ExpandDesktopHeap();
    }

    LOGI("[TASX] One-shot tweaks done%s",
         elevated ? " (HKCU+HKLM)" : " (HKCU only, HKLM needs admin)");
}

void TweaksPowerEnter()
{
    if (!config_get_bool("TASX", "PowerPlan", 0)) return;
    if (g_prevSaved) return;

    HMODULE powr = LoadLibraryW(L"PowrProf.dll");
    if (!powr) return;

    auto get = GetPowerGetActiveScheme(powr);
    auto set = GetPowerSetActiveScheme(powr);
    if (get && set) {
        GUID* cur = nullptr;
        if (get(0, &cur) == 0 && cur) {
            g_prevScheme = *cur;
            LocalFree(cur);
            /* Persist before changing the global scheme. If TASX is killed
               between these operations, the next startup can recover it. */
            if (!SavePowerJournal(g_prevScheme)) {
                LOGE("[TASX] Power scheme journal could not be written; refusing to change the global power plan");
                FreeLibrary(powr);
                return;
            }
            g_prevSaved = true;
        }

        g_appliedScheme = kUltimatePerfGuid;
        bool changed = set(0, &kUltimatePerfGuid) == 0;
        if (!changed) {
            g_appliedScheme = kHighPerfGuid;
            changed = set(0, &kHighPerfGuid) == 0;
        }
        if (!changed) {
            LOGW("[TASX] Power scheme change failed; previous scheme retained");
            DeletePowerJournal();
            g_prevSaved = false;
            FreeLibrary(powr);
            return;
        }

        LOGI("[TASX] Power scheme -> Ultimate/High performance");
    }

    FreeLibrary(powr);
}

void TweaksPowerExit()
{
    if (!g_prevSaved) return;

    HMODULE powr = LoadLibraryW(L"PowrProf.dll");
    if (!powr) return;

    auto get = GetPowerGetActiveScheme(powr);
    auto set = GetPowerSetActiveScheme(powr);
    GUID* current = nullptr;
    bool stillOwned = get && get(0, &current) == 0 && current &&
                      GuidEqual(*current, g_appliedScheme);
    if (current) LocalFree(current);

    if (get && !stillOwned) {
        DeletePowerJournal();
        g_prevSaved = false;
        LOGI("[TASX] Power scheme left unchanged: active scheme was changed externally");
    } else if (set && stillOwned && set(0, &g_prevScheme) == 0) {
        DeletePowerJournal();
        g_prevSaved = false;
        LOGI("[TASX] Power scheme restored");
    } else {
        LOGE("[TASX] Power scheme restore failed; journal retained for next startup");
    }

    FreeLibrary(powr);
}
