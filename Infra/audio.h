#pragma once

#include <windows.h>
#include <unordered_set>

/* Background audio suppression via the WASAPI Session API — the same
   cross-process mechanism the Windows Volume Mixer uses. No injection:
   TASX enumerates audio sessions from outside and mutes sessions owned by
   background Roblox clients; the focused client keeps its audio. */
void AudioApplyBackgroundMute(const std::unordered_set<DWORD>& clientPids,
                              DWORD focusedPid);

/* Per-PID mute helpers required by ApplyFocusIfChanged.
   Use the same WASAPI session enumeration but target a single PID.
   GUIDs are defined locally via DEFINE_GUID-style literals, no uuid.lib needed. */
void AudioMuteByPid(DWORD pid);
void AudioUnmuteByPid(DWORD pid);

/* Releases the session manager and any COM reference owned by this module. */
void AudioShutdown(void);

/* Drops TASX-owned mute state for an exited PID. */
void AudioForgetPid(DWORD pid);
