#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct ProcStat {
    DWORD   pid          = 0;
    SIZE_T  workingSet   = 0;   /* bytes */
    SIZE_T  privateBytes = 0;   /* bytes */
    ULONGLONG pageFaultCount = 0; /* cumulative process page faults */
    ULONGLONG cycleTime = 0;       /* cumulative CPU cycles, when available */
    ULONGLONG ioReadBytes = 0;     /* cumulative process I/O bytes */
    ULONGLONG ioWriteBytes = 0;
    bool      hasCpuTime = false;
    bool      hasIoCounters = false;
    std::wstring name;          /* exe name, e.g. L"RobloxPlayerBeta.exe" */
};

/* Snapshot of every process in ONE syscall (NtQuerySystemInformation).
   Used for discovery and cheap WS/private/page-fault baselines. CPU and I/O
   deltas are filled from documented per-process APIs only for tracked PIDs. */
bool QueryProcStats(std::vector<ProcStat>& out);

/* Enrich tracked process rows with documented Win32 counters. The process
   handle must have PROCESS_QUERY_LIMITED_INFORMATION or
   PROCESS_QUERY_INFORMATION. No undocumented structures are used here. */
void EnrichProcStat(HANDLE processHandle, ProcStat& stat);

/* System-wide hard-fault count when a supported provider is available.
   Returns false when Windows exposes no documented counter. */
bool QuerySystemHardFaults(ULONGLONG& count);

/* Current system RAM/commit snapshot for the observer and planner. */
struct SystemMemoryStat {
    ULONGLONG totalPhys = 0;
    ULONGLONG availPhys = 0;
    ULONGLONG commitUsed = 0;
    ULONGLONG commitLimit = 0;
    ULONGLONG hardFaults = 0;
    bool hasHardFaults = false;
};
bool QuerySystemMemoryStat(SystemMemoryStat& stat);

/* Full image path of one PID ("" when not accessible). Used by the job
   completion-port filter and the crash-handler sweep. */
std::wstring QueryProcessNameByPid(DWORD pid);
