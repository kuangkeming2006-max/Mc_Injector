#include "wndproc_hook.h"

#include "src/AgentLog.h"

#include <atomic>
#include <cassert>
#include <new>

namespace mcoverlay {

struct WndProcDispatchState final {
    std::atomic<unsigned long> references{1UL}; // WndProcHook owner
    std::atomic<unsigned> activeCallbacks{0U};
    HANDLE callbacksIdleEvent = nullptr;
    HWND window = nullptr;
    DWORD windowThreadId = 0U;
    WNDPROC original = nullptr;
    std::atomic<WndProcHook::Handler> handler{nullptr};
    std::atomic<void*> context{nullptr};
    std::atomic<bool> stopping{false};
    std::atomic<bool> propertyReference{false};
};

namespace {

constexpr wchar_t kHookProperty[] = L"McOverlayAgent.WndProcHook.State.v2";
// This second property deliberately outlives a normal restore. A thread can
// have entered WndProcHook::procedure immediately before the state property is
// removed, but not yet acquired the registry lock. It can still forward the
// message through the original chain without touching an object or trampoline.
constexpr wchar_t kOriginalProperty[] = L"McOverlayAgent.WndProcHook.Original.v2";
constexpr DWORD kRestoreSendTimeoutMs = 500U;
constexpr DWORD kCallbackDrainTimeoutMs = 1500U;

SRWLOCK g_registryLock = SRWLOCK_INIT;

UINT restoreMessage() noexcept
{
    static const UINT message = ::RegisterWindowMessageW(
        L"McOverlayAgent.WndProcHook.Restore.v2");
    return message;
}

void addReference(WndProcDispatchState* const state) noexcept
{
    state->references.fetch_add(1UL, std::memory_order_relaxed);
}

void releaseReference(WndProcDispatchState* const state) noexcept
{
    if (state->references.fetch_sub(1UL, std::memory_order_acq_rel) == 1UL) {
#ifndef NDEBUG
        assert(state->activeCallbacks.load(std::memory_order_acquire) == 0U);
        assert(!state->propertyReference.load(std::memory_order_acquire));
#endif
        if (state->callbacksIdleEvent != nullptr) {
            ::CloseHandle(state->callbacksIdleEvent);
        }
        delete state;
    }
}

void enterCallback(WndProcDispatchState* const state) noexcept
{
    addReference(state);
    if (state->activeCallbacks.fetch_add(1U, std::memory_order_acq_rel) == 0U) {
        ::ResetEvent(state->callbacksIdleEvent);
    }
}

void leaveCallback(WndProcDispatchState* const state) noexcept
{
    if (state->activeCallbacks.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        ::SetEvent(state->callbacksIdleEvent);
    }
    releaseReference(state);
}

WndProcDispatchState* acquireState(HWND const window) noexcept
{
    ::AcquireSRWLockShared(&g_registryLock);
    auto* const state = static_cast<WndProcDispatchState*>(
        ::GetPropW(window, kHookProperty));
    if (state != nullptr) {
        // GetProp + reference/callback acquisition is atomic with respect to
        // removeStateProperty(), closing the classic raw-this rundown race.
        enterCallback(state);
    }
    ::ReleaseSRWLockShared(&g_registryLock);
    return state;
}

WNDPROC originalFromProperty(HWND const window) noexcept
{
    return reinterpret_cast<WNDPROC>(::GetPropW(window, kOriginalProperty));
}

void removeStateProperty(WndProcDispatchState* const state) noexcept
{
    bool releasePropertyReference = false;
    ::AcquireSRWLockExclusive(&g_registryLock);
    if (state->propertyReference.load(std::memory_order_acquire) &&
        ::GetPropW(state->window, kHookProperty) == state) {
        (void)::RemovePropW(state->window, kHookProperty);
        releasePropertyReference = state->propertyReference.exchange(
            false, std::memory_order_acq_rel);
    }
    ::ReleaseSRWLockExclusive(&g_registryLock);
    if (releasePropertyReference) {
        releaseReference(state);
    }
}

// This is the only function which changes GWLP_WNDPROC after installation.
// It is intentionally a no-op off the window's owning thread.
bool restoreOnWindowThread(WndProcDispatchState* const state) noexcept
{
    if (::GetCurrentThreadId() != state->windowThreadId) {
        return false;
    }

    if (::IsWindow(state->window) == FALSE) {
        // The window manager has already discarded its property table.
        ::AcquireSRWLockExclusive(&g_registryLock);
        const bool releasePropertyReference = state->propertyReference.exchange(
            false, std::memory_order_acq_rel);
        ::ReleaseSRWLockExclusive(&g_registryLock);
        if (releasePropertyReference) {
            releaseReference(state);
        }
        return true;
    }

    const auto current = reinterpret_cast<WNDPROC>(
        ::GetWindowLongPtrW(state->window, GWLP_WNDPROC));
    if (current == &WndProcHook::procedure) {
        ::SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = ::SetWindowLongPtrW(
            state->window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(state->original));
        if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
            return false;
        }
        removeStateProperty(state);
        return true;
    }
    if (current == state->original) {
        // Another owner already restored our link.
        removeStateProperty(state);
        return true;
    }

    // A later subclass sits above us. Never overwrite it: its saved original
    // points at procedure(), so the independent inert state must stay attached
    // and keep forwarding to our original until that subclass is removed or
    // the window is destroyed.
    return false;
}

LRESULT callOriginal(WndProcDispatchState* const state,
                     HWND const window,
                     UINT const message,
                     WPARAM const wParam,
                     LPARAM const lParam) noexcept
{
    return state->original != nullptr
        ? ::CallWindowProcW(state->original, window, message, wParam, lParam)
        : ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

WndProcHook::~WndProcHook()
{
    (void)restore();
}

bool WndProcHook::install(HWND const window,
                          const Handler handler,
                          void* const context) noexcept
{
    if (window == nullptr || handler == nullptr || m_state != nullptr) {
        return false;
    }

    DWORD processId = 0U;
    const DWORD windowThreadId = ::GetWindowThreadProcessId(window, &processId);
    if (windowThreadId == 0U || processId != ::GetCurrentProcessId()) {
        // Never subclass another process. Same-process split presentation
        // threads are valid (notably Lunar): their WndProc only performs
        // atomic dispatch, while ImGui input is polled on the GL owner thread.
        return false;
    }
    ::SetLastError(ERROR_SUCCESS);
    const auto currentProcedure = reinterpret_cast<WNDPROC>(
        ::GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (currentProcedure == nullptr && ::GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    if (currentProcedure == &WndProcHook::procedure) {
        // An inert bridge from an earlier/later subclass generation is still
        // part of the chain. Installing our procedure on top of itself would
        // make its saved original recursive.
        return false;
    }

    auto* const state = new (std::nothrow) WndProcDispatchState;
    if (state == nullptr) {
        return false;
    }
    state->callbacksIdleEvent = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (state->callbacksIdleEvent == nullptr) {
        delete state;
        return false;
    }
    state->window = window;
    state->windowThreadId = windowThreadId;
    // Publish the chain target before procedure() itself can become reachable.
    // Cross-thread same-process installation is permitted, so the window may
    // dispatch immediately after SetWindowLongPtr returns.
    state->original = currentProcedure;
    state->handler.store(handler, std::memory_order_relaxed);
    state->context.store(context, std::memory_order_relaxed);

    ::AcquireSRWLockExclusive(&g_registryLock);
    const bool propertyVacant = ::GetPropW(window, kHookProperty) == nullptr;
    const bool propertyInstalled = propertyVacant &&
        ::SetPropW(window, kHookProperty, state) != FALSE;
    if (propertyInstalled) {
        addReference(state); // property/reference held by procedure()
        state->propertyReference.store(true, std::memory_order_release);
    }
    ::ReleaseSRWLockExclusive(&g_registryLock);
    if (!propertyInstalled) {
        releaseReference(state);
        return false;
    }

    if (::SetPropW(window, kOriginalProperty,
                   reinterpret_cast<HANDLE>(currentProcedure)) == FALSE) {
        removeStateProperty(state);
        releaseReference(state);
        return false;
    }

    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = ::SetWindowLongPtrW(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcHook::procedure));
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        (void)::RemovePropW(window, kOriginalProperty);
        removeStateProperty(state);
        releaseReference(state);
        return false;
    }
    state->original = reinterpret_cast<WNDPROC>(previous);
    if (state->original != currentProcedure &&
        ::SetPropW(window, kOriginalProperty,
                   reinterpret_cast<HANDLE>(state->original)) == FALSE) {
        // Roll back the just-installed same-process link before unpublishing
        // its state. No handler dereferences OverlayRenderer itself.
        (void)::SetWindowLongPtrW(window, GWLP_WNDPROC, previous);
        (void)::RemovePropW(window, kOriginalProperty);
        removeStateProperty(state);
        releaseReference(state);
        return false;
    }

    m_state = state;
    return true;
}

bool WndProcHook::restore() noexcept
{
    WndProcDispatchState* const state = m_state;
    m_state = nullptr;
    if (state == nullptr) {
        return true;
    }

    // Unpublish the handler while holding the same gate used by callback
    // acquisition. All callbacks which could have selected the old context
    // are now represented in activeCallbacks; later callbacks only forward.
    ::AcquireSRWLockExclusive(&g_registryLock);
    state->stopping.store(true, std::memory_order_release);
    state->handler.store(nullptr, std::memory_order_release);
    ::ReleaseSRWLockExclusive(&g_registryLock);

    bool restored = false;
    if (::GetCurrentThreadId() == state->windowThreadId) {
        restored = restoreOnWindowThread(state);
    } else if (::IsWindow(state->window) != FALSE) {
        DWORD_PTR messageResult = 0U;
        const UINT message = restoreMessage();
        restored = message != 0U && ::SendMessageTimeoutW(
            state->window, message, reinterpret_cast<WPARAM>(state), 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, kRestoreSendTimeoutMs,
            &messageResult) != 0 && messageResult != 0U;
    } else {
        // The window manager has removed the property table; no future
        // procedure lookup can acquire the property reference.
        bool releasePropertyReference = false;
        ::AcquireSRWLockExclusive(&g_registryLock);
        releasePropertyReference = state->propertyReference.exchange(
            false, std::memory_order_acq_rel);
        ::ReleaseSRWLockExclusive(&g_registryLock);
        if (releasePropertyReference) {
            releaseReference(state);
        }
        restored = true;
    }

    // A timeout never falls back to cross-thread SetWindowLongPtr. The inert
    // procedure retries restoration on the window thread when it next runs.
    const bool drained = ::WaitForSingleObject(
        state->callbacksIdleEvent, kCallbackDrainTimeoutMs) == WAIT_OBJECT_0;
    if (drained) {
        state->context.store(nullptr, std::memory_order_release);
    } else {
        log::error("Timed out draining WndProc callbacks; keeping dispatch state inert.");
    }

    // restored=false is safe when a later subclass owns the chain: its
    // property reference retains this inert forwarding bridge.
    (void)restored;
    releaseReference(state); // WndProcHook owner
    return drained;
}

LRESULT CALLBACK WndProcHook::procedure(HWND const window,
                                        const UINT message,
                                        const WPARAM wParam,
                                        const LPARAM lParam) noexcept
{
    WndProcDispatchState* const state = acquireState(window);
    if (state == nullptr) {
        const WNDPROC original = originalFromProperty(window);
        return original != nullptr
            ? ::CallWindowProcW(original, window, message, wParam, lParam)
            : ::DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT result = 0;
    const UINT registeredRestoreMessage = restoreMessage();
    if (registeredRestoreMessage != 0U && message == registeredRestoreMessage &&
        wParam == reinterpret_cast<WPARAM>(state) &&
        state->stopping.load(std::memory_order_acquire)) {
        result = restoreOnWindowThread(state) ? 1 : 0;
    } else {
        if (state->stopping.load(std::memory_order_acquire)) {
            // Covers SendMessageTimeout: once the hung window pumps again it
            // removes our top-level subclass on the correct thread.
            (void)restoreOnWindowThread(state);
        }

        bool handled = false;
        const Handler handler = state->handler.load(std::memory_order_acquire);
        void* const context = state->context.load(std::memory_order_acquire);
        if (!state->stopping.load(std::memory_order_acquire) &&
            handler != nullptr && context != nullptr) {
            result = handler(context, window, message, wParam, lParam, handled);
        }
        if (!handled) {
            result = callOriginal(state, window, message, wParam, lParam);
        }
    }

    if (message == WM_NCDESTROY) {
        removeStateProperty(state);
        (void)::RemovePropW(window, kOriginalProperty);
    }
    leaveCallback(state);
    return result;
}

} // namespace mcoverlay
