#pragma once

/* One-shot system-wide registry tweaks (GameDVR off, MMCSS gaming profile,
   network-throttling off, high-performance GPU preference for Roblox).
   Idempotent: writes only values that differ. HKLM entries need TASX to run
   elevated (the scheduled-task install provides that). */
void TweaksApplyOneShot();

/* Switches the active power scheme to Ultimate Performance (fallback: High
   Performance) while Roblox is running; call TweaksPowerExit() when the
   last instance closes to restore the previous scheme. */
void TweaksPowerEnter();
void TweaksPowerExit();

/* Restores a power scheme left behind by a previous crashed agent run.
   Safe to call on every startup; a journal is deleted only after a confirmed
   successful restore. */
void TweaksPowerRecoverStale();
