#pragma once

#include <windows.h>

/* Single background job + per-process focus profiles.
   Each Roblox process is assigned to the shared background cgroup exactly
   ONCE at hook time (farm-wide CPU/MEM caps). It is NEVER moved between
   jobs — Windows forbids that (always ERROR_ACCESS_DENIED). Focus
   differentiation (HIGH/IDLE priority, affinity, EcoQoS) is applied
   per-process by the caller via CpuApplyFocusProfile. PID state: 0 in the
   cgroup with background settings, 2 in the cgroup with the focus profile
   applied, -1 sticky per-process fallback (foreign job / denied). */

bool JobsInit();
void JobShutdown();

/* Assign process to the background job (initial state). Returns false if
   assignment failed (caller falls back to per-process profile). */
bool JobHookProcess(DWORD pid, HANDLE hProc);

/* State cache: true when the process already runs the requested state
   (caller skips all syscalls), false when the caller must apply the
   per-process profile. Never attempts cross-job moves. */
bool JobApplyProfile(DWORD pid, int focused);

/* Rewrite background cgroup limits when the farm/focus policy flips.
   No-op when nothing changed. */
void JobsRefreshDynamic(int anyFocused);

/* FarmBoost (Ctrl+Alt+B) toggle: while on, the shared background job runs
   at full power (all cores, no CPU cap). Per-process NORMAL/IDLE priority
   and EcoQoS are applied by master.cpp. Just flips the flag - call
   JobsRefreshDynamic() right after to apply the job limits. */
void JobsSetFarmBoost(int on);

void JobRelease(DWORD pid);

/* Implemented in master.cpp */
void TasxNotifyJobEvent(int kind, DWORD pid);
