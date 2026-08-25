#pragma once

#include <windows.h>

#include <atomic>
namespace mcoverlay {

class OpenGlHook final {
public:
    using FrameCallback = void (*)(void* context, HDC deviceContext) noexcept;

    OpenGlHook() = default;
    ~OpenGlHook();

    OpenGlHook(const OpenGlHook&) = delete;
    OpenGlHook& operator=(const OpenGlHook&) = delete;

    [[nodiscard]] bool install(FrameCallback callback, void* context) noexcept;
    [[nodiscard]] bool disable() noexcept;
    void waitForIdle() noexcept;
    [[nodiscard]] bool remove() noexcept;

private:
    using SwapBuffersFunction = BOOL(WINAPI*)(HDC);
    using SwapLayerBuffersFunction = BOOL(WINAPI*)(HDC, UINT);

    static BOOL WINAPI swapBuffersDetour(HDC deviceContext) noexcept;
    static BOOL WINAPI swapLayerBuffersDetour(HDC deviceContext, UINT planes) noexcept;
    void beforePresent(HDC deviceContext) noexcept;
    void enterDetour() noexcept;
    void leaveDetour() noexcept;

    static inline std::atomic<OpenGlHook*> s_active{nullptr};
    // s_dispatchLock is the acquisition gate for s_active.  A detour may only
    // retain the published object after incrementing m_activeDetours while it
    // holds this lock shared.  Teardown takes it exclusively to unpublish the
    // object, so it cannot observe an idle count while a detour still owns a
    // raw pointer but has not entered the count yet.
    static inline SRWLOCK s_dispatchLock = SRWLOCK_INIT;
    static inline thread_local bool s_insidePresent = false;

    FrameCallback m_callback = nullptr;
    void* m_context = nullptr;
    void* m_swapBuffersTarget = nullptr;
    void* m_swapLayerBuffersTarget = nullptr;
    SwapBuffersFunction m_originalSwapBuffers = nullptr;
    SwapLayerBuffersFunction m_originalSwapLayerBuffers = nullptr;
    bool m_minHookInitialized = false;
    bool m_swapHookCreated = false;
    bool m_layerHookCreated = false;
    bool m_swapHookEnabled = false;
    bool m_layerHookEnabled = false;
    std::atomic<unsigned> m_activeDetours{0U};
    HANDLE m_detoursIdleEvent = nullptr;
};

} // namespace mcoverlay
