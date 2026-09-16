#include "stats.h"

#include "ntsys.h"

#include <psapi.h>

#include <cstdlib>

bool QueryProcStats(std::vector<ProcStat>& out)
{
    TASX_SYS_PROC* head = tasx_query_system_processes();
    if (!head) return false;

    /* Reserve generously once; typical systems have a few hundred entries. */
    if (out.capacity() < 256) out.reserve(256);
    out.clear();

    TASX_SYS_PROC* p = head;
    size_t records = 0;
    for (;;)
    {
        /* The buffer originates in the kernel, but keep a finite traversal
           guard so a malformed offset can never spin the 10-second sampler. */
        if (++records > 1000000u) break;
        if (p->UniqueProcessId)
        {
            ProcStat st;
            st.pid = (DWORD)(ULONG_PTR)p->UniqueProcessId;
            st.workingSet = p->WorkingSetSize;
            st.privateBytes = p->PrivatePageCount;
            st.pageFaultCount = p->PageFaultCount;
            st.cycleTime = p->CycleTime;
            st.hasCpuTime = (p->CycleTime != 0);
            if (p->ImageName.Buffer && p->ImageName.Length &&
                p->ImageName.Length < 512) /* sanity: layout guard */
                st.name.assign(p->ImageName.Buffer,
                               p->ImageName.Length / sizeof(wchar_t));
            out.push_back(std::move(st));
        }

        if (!p->NextEntryOffset) break;
        if (p->NextEntryOffset < sizeof(TASX_SYS_PROC) ||
            (p->NextEntryOffset & (sizeof(void*) - 1)) != 0)
            break;
        p = (TASX_SYS_PROC*)((unsigned char*)p + p->NextEntryOffset);
    }

    free(head);
    return true;
}

void EnrichProcStat(HANDLE processHandle, ProcStat& stat)
{
    if (!processHandle || processHandle == INVALID_HANDLE_VALUE) return;

    ULONGLONG cycles = 0;
    if (QueryProcessCycleTime(processHandle, &cycles)) {
        stat.cycleTime = cycles;
        stat.hasCpuTime = true;
    }

    IO_COUNTERS io{};
    if (GetProcessIoCounters(processHandle, &io)) {
        stat.ioReadBytes = io.ReadTransferCount;
        stat.ioWriteBytes = io.WriteTransferCount;
        stat.hasIoCounters = true;
    }
}

bool QuerySystemHardFaults(ULONGLONG& count)
{
    /* GetPerformanceInfo does not expose a hard-fault counter (the similarly
       named PageFaultCount exists in the internal system-performance record,
       not in PERFORMANCE_INFORMATION). Do not publish page faults as hard
       faults: that would make the trim metrics materially misleading. */
    count = 0;
    return false;
}

bool QuerySystemMemoryStat(SystemMemoryStat& stat)
{
    stat = {};

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return false;

    stat.totalPhys = ms.ullTotalPhys;
    stat.availPhys = ms.ullAvailPhys;
    stat.commitUsed = ms.ullTotalPageFile >= ms.ullAvailPageFile
        ? ms.ullTotalPageFile - ms.ullAvailPageFile : 0;
    stat.commitLimit = ms.ullTotalPageFile;
    stat.hasHardFaults = QuerySystemHardFaults(stat.hardFaults);
    return true;
}

std::wstring QueryProcessNameByPid(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::wstring();

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &size))
        name = path;
    CloseHandle(h);
    return name;
}
