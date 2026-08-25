#include "opengl_hook.h"

#include "src/AgentLog.h"

#include <MinHook.h>
#include <gl/GL.h>

#include <cassert>

namespace mcoverlay {

OpenGlHook::~OpenGlHook()
{
    if (!remove()) {
        // Destruction after this diagnostic is unsafe; normal ownership must
        // retain the containing runtime when remove() reports failure.
        log::error("OpenGL hook destroyed after MinHook disable failed.");
    }
}

bool OpenGlHook::install(const FrameCallback callback, void* const context) noexcept
{
    if (callback == nullptr) {
        return false;
    }
    ::AcquireSRWLockShared(&s_dispatchLock);
    const bool dispatchOccupied = s_active.load(std::memory_order_acquire) != nullptr;
    ::ReleaseSRWLockShared(&s_dispatchLock);
    if (dispatchOccupied) {
        return false;
    }

    m_detoursIdleEvent = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (m_detoursIdleEvent == nullptr) {
        return false;
    }

    const HMODULE gdi = ::GetModuleHandleW(L"gdi32.dll");
    if (gdi == nullptr) {
        log::error("gdi32.dll is not loaded.");
        return false;
    }
    m_swapBuffersTarget = reinterpret_cast<void*>(::GetProcAddress(gdi, "SwapBuffers"));
    const HMODULE openGl = ::GetModuleHandleW(L"opengl32.dll");
    m_swapLayerBuffersTarget = openGl == nullptr ? nullptr : reinterpret_cast<void*>(
        ::GetProcAddress(openGl, "wglSwapLayerBuffers"));
    if (m_swapBuffersTarget == nullptr) {
        log::error("gdi32!SwapBuffers was not found.");
        return false;
    }

    const MH_STATUS initialize = ::MH_Initialize();
    if (initialize != MH_OK && initialize != MH_ERROR_ALREADY_INITIALIZED) {
        log::error("MH_Initialize failed.");
        return false;
    }
    m_minHookInitialized = true;

    if (::MH_CreateHook(m_swapBuffersTarget,
                        reinterpret_cast<void*>(&OpenGlHook::swapBuffersDetour),
                        reinterpret_cast<void**>(&m_originalSwapBuffers)) != MH_OK) {
        log::error("Could not create the gdi32!SwapBuffers hook.");
        (void)remove();
        return false;
    }
    m_swapHookCreated = true;

    // LWJGL2 normally uses SwapBuffers. The layer hook is a compatibility
    // path for drivers/wrappers that present through wglSwapLayerBuffers.
    if (m_swapLayerBuffersTarget != nullptr &&
        ::MH_CreateHook(m_swapLayerBuffersTarget,
                        reinterpret_cast<void*>(&OpenGlHook::swapLayerBuffersDetour),
                        reinterpret_cast<void**>(&m_originalSwapLayerBuffers)) == MH_OK) {
        m_layerHookCreated = true;
    }
    m_callback = callback;
    m_context = context;
    ::AcquireSRWLockExclusive(&s_dispatchLock);
    OpenGlHook* expected = nullptr;
    const bool published = s_active.compare_exchange_strong(
        expected, this, std::memory_order_release, std::memory_order_relaxed);
    ::ReleaseSRWLockExclusive(&s_dispatchLock);
    if (!published) {
        log::error("Another OpenGL hook became active during installation.");
        (void)remove();
        return false;
    }
    if (::MH_EnableHook(m_swapBuffersTarget) != MH_OK) {
        log::error("Could not enable the gdi32!SwapBuffers hook.");
        (void)remove();
        return false;
    }
    m_swapHookEnabled = true;
    if (m_layerHookCreated && ::MH_EnableHook(m_swapLayerBuffersTarget) != MH_OK) {
        ::MH_RemoveHook(m_swapLayerBuffersTarget);
        m_layerHookCreated = false;
        m_originalSwapLayerBuffers = nullptr;
    } else if (m_layerHookCreated) {
        m_layerHookEnabled = true;
    }
    return true;
}

void OpenGlHook::beforePresent(HDC const deviceContext) noexcept
{
    if (m_callback != nullptr) {
        m_callback(m_context, deviceContext);
    }
}

void OpenGlHook::enterDetour() noexcept
{
    if (m_activeDetours.fetch_add(1U, std::memory_order_acq_rel) == 0U) {
        ::ResetEvent(m_detoursIdleEvent);
    }
}

void OpenGlHook::leaveDetour() noexcept
{
    if (m_activeDetours.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        ::SetEvent(m_detoursIdleEvent);
    }
}

BOOL WINAPI OpenGlHook::swapBuffersDetour(HDC const deviceContext) noexcept
{
    OpenGlHook* hook = nullptr;
    SwapBuffersFunction original = nullptr;
    ::AcquireSRWLockShared(&s_dispatchLock);
    hook = s_active.load(std::memory_order_acquire);
    if (hook != nullptr && hook->m_originalSwapBuffers != nullptr) {
        // Pointer acquisition and rundown acquisition are one indivisible
        // operation with respect to disable().
        hook->enterDetour();
        original = hook->m_originalSwapBuffers;
    }
    ::ReleaseSRWLockShared(&s_dispatchLock);

    if (hook == nullptr || original == nullptr) {
        // s_active is unpublished only after MinHook has restored the target
        // prologue.  A thread that had already branched into this detour but
        // had not acquired the gate can therefore continue through gdi32.
        return ::SwapBuffers(deviceContext);
    }

    const bool outermost = !s_insidePresent;
    if (outermost) {
        s_insidePresent = true;
        hook->beforePresent(deviceContext);
    }
    const BOOL result = original(deviceContext);
    if (outermost) {
        s_insidePresent = false;
    }
    hook->leaveDetour();
    return result;
}

BOOL WINAPI OpenGlHook::swapLayerBuffersDetour(HDC const deviceContext,
                                               const UINT planes) noexcept
{
    OpenGlHook* hook = nullptr;
    SwapLayerBuffersFunction original = nullptr;
    ::AcquireSRWLockShared(&s_dispatchLock);
    hook = s_active.load(std::memory_order_acquire);
    if (hook != nullptr && hook->m_originalSwapLayerBuffers != nullptr) {
        hook->enterDetour();
        original = hook->m_originalSwapLayerBuffers;
    }
    ::ReleaseSRWLockShared(&s_dispatchLock);

    if (hook == nullptr || original == nullptr) {
        return ::wglSwapLayerBuffers(deviceContext, planes);
    }

    const bool outermost = !s_insidePresent;
    if (outermost) {
        s_insidePresent = true;
        hook->beforePresent(deviceContext);
    }
    const BOOL result = original(deviceContext, planes);
    if (outermost) {
        s_insidePresent = false;
    }
    hook->leaveDetour();
    return result;
}

void OpenGlHook::waitForIdle() noexcept
{
    if (m_detoursIdleEvent != nullptr) {
        ::WaitForSingleObject(m_detoursIdleEvent, INFINITE);
    }
}

bool OpenGlHook::disable() noexcept
{
    // Restore both prologues before cancelling publication.  While MinHook is
    // patching, any detour that does run still sees a valid object/trampoline
    // and participates in rundown.
    bool disabled = true;
    if (m_swapHookEnabled) {
        const MH_STATUS status = ::MH_DisableHook(m_swapBuffersTarget);
        if (status == MH_OK || status == MH_ERROR_DISABLED) {
            m_swapHookEnabled = false;
        } else {
            log::error("Could not disable the gdi32!SwapBuffers hook.");
            disabled = false;
        }
    }
    if (m_layerHookEnabled) {
        const MH_STATUS status = ::MH_DisableHook(m_swapLayerBuffersTarget);
        if (status == MH_OK || status == MH_ERROR_DISABLED) {
            m_layerHookEnabled = false;
        } else {
            log::error("Could not disable the wglSwapLayerBuffers hook.");
            disabled = false;
        }
    }
    if (!disabled) {
        // At least one target may still branch to this detour. Keep dispatch,
        // trampoline pointers, events and callback context fully published.
        return false;
    }

    ::AcquireSRWLockExclusive(&s_dispatchLock);
    OpenGlHook* expected = this;
    (void)s_active.compare_exchange_strong(expected, nullptr,
                                           std::memory_order_acq_rel);
    ::ReleaseSRWLockExclusive(&s_dispatchLock);
    return true;
}

bool OpenGlHook::remove() noexcept
{
    if (!disable()) {
        return false;
    }
    waitForIdle();
#ifndef NDEBUG
    assert(s_active.load(std::memory_order_acquire) != this);
    assert(m_activeDetours.load(std::memory_order_acquire) == 0U);
#endif
    // No published detour can acquire this object now, and every acquisition
    // made before unpublication has left.  Only at this point is it safe to
    // remove trampolines, clear original pointers, and close the idle event.
    if (m_swapHookCreated) {
        ::MH_RemoveHook(m_swapBuffersTarget);
        m_swapHookCreated = false;
    }
    if (m_layerHookCreated) {
        ::MH_RemoveHook(m_swapLayerBuffersTarget);
        m_layerHookCreated = false;
    }
    if (m_minHookInitialized) {
        ::MH_Uninitialize();
        m_minHookInitialized = false;
    }
    m_callback = nullptr;
    m_context = nullptr;
    m_swapBuffersTarget = nullptr;
    m_swapLayerBuffersTarget = nullptr;
    m_originalSwapBuffers = nullptr;
    m_originalSwapLayerBuffers = nullptr;
    if (m_detoursIdleEvent != nullptr) {
        ::CloseHandle(m_detoursIdleEvent);
        m_detoursIdleEvent = nullptr;
    }
    return true;
}

} // namespace mcoverlay
