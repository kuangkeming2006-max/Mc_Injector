#pragma once

#include <windows.h>

namespace mcoverlay {

struct WndProcDispatchState;

class WndProcHook final {
public:
    using Handler = LRESULT (*)(void* context,
                                HWND window,
                                UINT message,
                                WPARAM wParam,
                                LPARAM lParam,
                                bool& handled) noexcept;

    WndProcHook() = default;
    ~WndProcHook();

    WndProcHook(const WndProcHook&) = delete;
    WndProcHook& operator=(const WndProcHook&) = delete;

    [[nodiscard]] bool install(HWND window, Handler handler, void* context) noexcept;
    // Stops handler dispatch, restores the WndProc on its owning thread, and
    // drains callbacks which may already have selected the handler.  False
    // means the bounded drain timed out; the independent dispatch state stays
    // alive and inert so callers can leak backend state instead of racing it.
    [[nodiscard]] bool restore() noexcept;
    [[nodiscard]] bool installed() const noexcept { return m_state != nullptr; }

    // Public only so the Win32 restoration helper in the implementation can
    // compare the current chain link. It is not an application entry point.
    static LRESULT CALLBACK procedure(HWND window,
                                      UINT message,
                                      WPARAM wParam,
                                      LPARAM lParam) noexcept;

private:
    WndProcDispatchState* m_state = nullptr;
};

} // namespace mcoverlay
