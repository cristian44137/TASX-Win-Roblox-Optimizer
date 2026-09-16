#include "winhook.h"

#include "log.h"

WinHook::~WinHook()
{
    if (m_pumpThread) {
        /* The worker owns the message queue.  Wait until it has created the
           queue and completed hook setup before posting WM_QUIT; otherwise a
           fast shutdown can lose the post and free `this` while the worker is
           still starting. */
        if (m_readyEvent)
            WaitForSingleObject(m_readyEvent, INFINITE);
        if (m_hook) UnhookWinEvent(m_hook);
        PostThreadMessageW(m_pumpThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(m_pumpThread, INFINITE);
        CloseHandle(m_pumpThread);
        m_pumpThread = nullptr;
    }
    if (m_readyEvent) CloseHandle(m_readyEvent);
}

void WinHook::Start()
{
    m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_readyEvent) {
        LOGW("[TASX] Failed to create foreground hook readiness event");
        return;
    }

    m_pumpThread = CreateThread(
        nullptr, 0,
        [](LPVOID self) -> DWORD {
            auto* hook = static_cast<WinHook*>(self);

            /* Force creation of the thread message queue before signalling
               readiness, so a subsequent PostThreadMessage cannot race it. */
            MSG msg{};
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

            hook->m_hook = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                nullptr, HookCallback,
                0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

            if (!hook->m_hook) {
                LOGW("[TASX] Foreground hook unavailable, polling only");
                SetEvent(hook->m_readyEvent);
                return 0;
            }

            SetEvent(hook->m_readyEvent);

            /* GetMessageW pumps events to the hook callback until WM_QUIT. */
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) { /* drain */ }

            return 0;
        },
        this, 0, &m_pumpThreadId);

    if (!m_pumpThread) {
        CloseHandle(m_readyEvent);
        m_readyEvent = nullptr;
        LOGW("[TASX] Failed to start foreground hook pump");
    }
}

void CALLBACK WinHook::HookCallback(HWINEVENTHOOK, DWORD, HWND,
                                    LONG, LONG, DWORD, DWORD)
{
    NotifyFocusChanged();
}
