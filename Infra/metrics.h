#pragma once

#include <windows.h>

#include <cstdint>

struct TrimMetricSnapshot {
    DWORD pid = 0;
    const char* type = nullptr; /* "soft" or "hard" */
    SIZE_T wsBefore = 0;
    SIZE_T wsAfter = 0;
    SIZE_T privateBefore = 0;
    SIZE_T privateAfter = 0;
    int commitBefore = -1;
    int commitAfter = -1;
    ULONGLONG hardFaultsBefore = 0;
    ULONGLONG hardFaultsAfter = 0;
    bool hasHardFaultsBefore = false;
    bool hasHardFaultsAfter = false;
};

/* Records one completed trim and keeps at most the configured 1000 newest
   records. The function is thread-safe and does not retain the type pointer. */
void MetricsRecordTrim(const TrimMetricSnapshot& snapshot);

/* Emits the configured aggregate report when its interval is due. */
void MetricsMaybeReport(ULONGLONG nowMs);

/* Per-client process/idle snapshot. The sampler is deliberately observer-only. */
void MetricsSampleClients(ULONGLONG nowMs);

/* Commit-limit guard. Called by the main loop every ~30 seconds. */
void MetricsCheckCommitLimits(ULONGLONG nowMs);

/* Logs the memory budget recommendation without changing JobMemoryCapMB. */
void MetricsMaybeReportBudget(ULONGLONG nowMs);

/* Drops all in-memory observations on shutdown/tests. */
void MetricsReset(void);
