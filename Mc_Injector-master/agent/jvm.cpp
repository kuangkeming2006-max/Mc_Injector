#include "jvm.h"

#include "src/AgentLog.h"
#include "src/AgentRuntime.h"

#include <windows.h>

#include <process.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mcoverlay::jvm {

JavaVM* resolveJavaVm(JavaVM* const supplied) noexcept
{
    if (supplied != nullptr) {
        return supplied;
    }

    // An ordinary LoadLibrary caller must explicitly invoke McOverlay_Start.
    // Looking up the already-loaded JVM avoids a compile-time/link-time
    // dependency on a particular Java installation.
    const HMODULE jvmModule = ::GetModuleHandleW(L"jvm.dll");
    if (jvmModule == nullptr) {
        return nullptr;
    }

    using GetCreatedJavaVMs = jint(JNICALL*)(JavaVM**, jsize, jsize*);
    const FARPROC address = ::GetProcAddress(jvmModule, "JNI_GetCreatedJavaVMs");
    GetCreatedJavaVMs getCreated = nullptr;
    static_assert(sizeof(getCreated) == sizeof(address));
    std::memcpy(&getCreated, &address, sizeof(getCreated));
    if (getCreated == nullptr) {
        return nullptr;
    }

    JavaVM* vm = nullptr;
    jsize count = 0;
    if (getCreated(&vm, 1, &count) != JNI_OK || count != 1 || vm == nullptr) {
        return nullptr;
    }
    return vm;
}

jvmtiEnv* resolveJvmti(JavaVM* const vm) noexcept
{
    if (vm == nullptr) {
        return nullptr;
    }

    jvmtiEnv* environment = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&environment), JVMTI_VERSION_1_2) != JNI_OK) {
        environment = nullptr;
    }
    return environment;
}

ScopedThreadEnv::ScopedThreadEnv(JavaVM* const vm, const bool attachIfDetached) noexcept
    : m_vm(vm)
{
    if (m_vm == nullptr) {
        return;
    }

    const jint result = m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
    if (result == JNI_OK) {
        return;
    }

    m_env = nullptr;
    if (result != JNI_EDETACHED || !attachIfDetached) {
        return;
    }

    JavaVMAttachArgs arguments{};
    arguments.version = JNI_VERSION_1_6;
    arguments.name = const_cast<char*>("McOverlayAgent");
    arguments.group = nullptr;
    if (m_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&m_env), &arguments) == JNI_OK) {
        m_attachedByUs = true;
    } else {
        m_env = nullptr;
    }
}

ScopedThreadEnv::~ScopedThreadEnv()
{
    if (m_attachedByUs && m_vm != nullptr) {
        m_vm->DetachCurrentThread();
    }
}

} // namespace mcoverlay::jvm

namespace {

constexpr std::size_t kMaximumBootstrapOptionsBytes = 64U * 1024U;

enum class BootstrapOrigin {
    AgentOnLoad,
    AgentOnAttach,
    NativeExport,
};

struct BootstrapContext {
    JavaVM* vm = nullptr;
    std::string options;
    BootstrapOrigin origin = BootstrapOrigin::AgentOnAttach;
};

// Every queued bootstrap owns a real loader reference to this DLL until its
// thread has completely returned. Agent_OnAttach normally causes HotSpot to
// retain the library itself, but the explicit reference also covers direct
// callers of McOverlay_Start and adversarial FreeLibrary timing.
struct BootstrapRecord {
    HANDLE thread = nullptr;
    HMODULE moduleReference = nullptr;
    unsigned threadId = 0U;
    bool completed = false;
};

std::mutex g_bootstrapRegistryMutex;
std::mutex g_bootstrapExecutionMutex;
std::vector<BootstrapRecord> g_bootstraps;
bool g_vmUnloading = false;
HMODULE g_agentModule = nullptr;

bool copyOptions(const char* const rawOptions, std::string& destination)
{
    destination.clear();
    if (rawOptions == nullptr) {
        return true;
    }

    // The controller already enforces this limit. Recheck it before copying so
    // a malformed agent request cannot make an unbounded allocation on the
    // JVM's Attach listener thread.
    std::size_t length = 0U;
    while (length < kMaximumBootstrapOptionsBytes && rawOptions[length] != '\0') {
        ++length;
    }
    if (length == kMaximumBootstrapOptionsBytes) {
        return false;
    }
    destination.assign(rawOptions, length);
    return true;
}

HMODULE retainAgentModule() noexcept
{
    HMODULE retained = nullptr;
    // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS interprets the second parameter
    // as an address. A data address avoids non-portable function-pointer casts.
    const auto* const address = reinterpret_cast<LPCWSTR>(
        static_cast<const void*>(&g_agentModule));
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                              address, &retained)) {
        return nullptr;
    }
    return retained;
}

void reapCompletedBootstrapsLocked() noexcept
{
    auto iterator = g_bootstraps.begin();
    while (iterator != g_bootstraps.end()) {
        if (::WaitForSingleObject(iterator->thread, 0U) != WAIT_OBJECT_0) {
            ++iterator;
            continue;
        }
        ::CloseHandle(iterator->thread);
        if (iterator->moduleReference != nullptr) {
            ::FreeLibrary(iterator->moduleReference);
        }
        iterator = g_bootstraps.erase(iterator);
    }
}

bool bootstrapAllowed() noexcept
{
    std::lock_guard lock(g_bootstrapRegistryMutex);
    return !g_vmUnloading;
}

unsigned __stdcall bootstrapThread(void* const opaque) noexcept
{
    std::unique_ptr<BootstrapContext> context(
        static_cast<BootstrapContext*>(opaque));
    if (!context || context->vm == nullptr) {
        return 1U;
    }

    // AgentRuntime owns one process-wide runtime. Serialize concurrent Attach,
    // OnLoad, and native-export requests here rather than ever blocking a JVM
    // listener thread. Reattachment remains supported: AgentRuntime::start
    // cleanly replaces an earlier detached/runtime instance.
    std::lock_guard executionLock(g_bootstrapExecutionMutex);
    if (!bootstrapAllowed()) {
        return 1U;
    }

    // _beginthreadex creates a raw native thread. Attach it as a daemon before
    // asking the VM for JVMTI state. In the OnLoad case this call may wait until
    // JVM startup advances, which is safe because Agent_OnLoad has already
    // returned and no longer blocks the VM's initialization thread.
    mcoverlay::jvm::ScopedThreadEnv environment(context->vm, true);
    if (!environment) {
        mcoverlay::log::error("Could not attach the asynchronous bootstrap thread to the JVM.");
        return 1U;
    }
    if (!bootstrapAllowed()) {
        return 1U;
    }

    const jint result = mcoverlay::AgentRuntime::start(
        context->vm, context->options.c_str());
    if (result != JNI_OK) {
        mcoverlay::log::error("Asynchronous bootstrap could not start the agent runtime.");
        return 1U;
    }
    return 0U;
}

// Copies all caller-owned state, takes a DLL reference, and queues the only
// potentially blocking work. Returning JNI_OK means the request was safely
// scheduled; runtime success is reported over the authenticated IPC channel.
jint scheduleBootstrap(JavaVM* const vm,
                       const char* const rawOptions,
                       const BootstrapOrigin origin) noexcept
{
    if (vm == nullptr) {
        mcoverlay::log::error("Could not schedule agent bootstrap without a JavaVM.");
        return JNI_ERR;
    }

    HMODULE moduleReference = nullptr;
    try {
        auto context = std::make_unique<BootstrapContext>();
        context->vm = vm;
        context->origin = origin;
        if (!copyOptions(rawOptions, context->options)) {
            mcoverlay::log::error("Agent options exceed the 64 KiB bootstrap limit.");
            return JNI_ERR;
        }

        moduleReference = retainAgentModule();
        if (moduleReference == nullptr) {
            mcoverlay::log::error("Could not retain the agent DLL for asynchronous startup.");
            return JNI_ERR;
        }

        std::lock_guard registryLock(g_bootstrapRegistryMutex);
        reapCompletedBootstrapsLocked();
        if (g_vmUnloading) {
            ::FreeLibrary(moduleReference);
            return JNI_ERR;
        }

        // Reserve before the thread exists. Once _beginthreadex succeeds the
        // record insertion is non-throwing, so Agent_OnUnload can never miss a
        // live bootstrap or release its code out from under it.
        g_bootstraps.reserve(g_bootstraps.size() + 1U);
        unsigned threadId = 0U;
        const uintptr_t rawThread = ::_beginthreadex(
            nullptr, 0U, &bootstrapThread, context.get(), 0U, &threadId);
        if (rawThread == 0U) {
            mcoverlay::log::error("Could not create the asynchronous agent bootstrap thread.");
            ::FreeLibrary(moduleReference);
            return JNI_ERR;
        }

        context.release();
        g_bootstraps.push_back(BootstrapRecord{
            reinterpret_cast<HANDLE>(rawThread), moduleReference, threadId});
        return JNI_OK;
    } catch (...) {
        if (moduleReference != nullptr) {
            ::FreeLibrary(moduleReference);
        }
        mcoverlay::log::error("Could not allocate the asynchronous bootstrap context.");
        return JNI_ERR;
    }
}

void stopAndDrainBootstraps() noexcept
{
    std::vector<BootstrapRecord> bootstraps;
    {
        std::lock_guard registryLock(g_bootstrapRegistryMutex);
        g_vmUnloading = true;
        bootstraps = std::move(g_bootstraps);
    }

    // Waiting for the exact thread handles, rather than a counter decremented
    // near the end of the entry function, guarantees that no instruction in
    // this DLL is still executing when its extra loader reference is released.
    const DWORD currentThreadId = ::GetCurrentThreadId();
    for (BootstrapRecord& record : bootstraps) {
        if (record.thread != nullptr && record.threadId != currentThreadId) {
            record.completed =
                ::WaitForSingleObject(record.thread, INFINITE) == WAIT_OBJECT_0;
            if (!record.completed) {
                mcoverlay::log::error(
                    "Could not confirm bootstrap-thread completion during Agent_OnUnload; DLL remains pinned.");
            }
        }
    }

    // g_vmUnloading prevents queued threads from entering AgentRuntime::start;
    // a request already inside start is covered by the waits above. Runtime
    // teardown can therefore safely use the JavaVM for its final JNI cleanup.
    mcoverlay::AgentRuntime::stop(true);

    for (const BootstrapRecord& record : bootstraps) {
        if (record.threadId == currentThreadId || !record.completed) {
            // This is not a JVM-supported call pattern. Fail closed by keeping
            // the module pinned instead of unloading a possibly live callback.
            continue;
        }
        if (record.thread != nullptr) {
            ::CloseHandle(record.thread);
        }
        if (record.moduleReference != nullptr) {
            ::FreeLibrary(record.moduleReference);
        }
    }
}

} // namespace

extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void*)
{
    // Never perform hook installation, pipe connection, or a five-second
    // runtime-ready wait on the JVM's OnLoad thread.
    return scheduleBootstrap(mcoverlay::jvm::resolveJavaVm(vm), options,
                             BootstrapOrigin::AgentOnLoad);
}

extern "C" JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void*)
{
    // HotSpot's Attach listener must be released immediately; Forge JVMs can
    // otherwise report "Failed to load agent library: 0" after a pipe timeout.
    return scheduleBootstrap(mcoverlay::jvm::resolveJavaVm(vm), options,
                             BootstrapOrigin::AgentOnAttach);
}

extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM*)
{
    stopAndDrainBootstraps();
}

extern "C" __declspec(dllexport) DWORD WINAPI McOverlay_Start(LPVOID rawOptions)
{
    const auto* const options = static_cast<const char*>(rawOptions);
    JavaVM* const vm = mcoverlay::jvm::resolveJavaVm(nullptr);
    if (vm == nullptr) {
        mcoverlay::log::error("JNI_GetCreatedJavaVMs did not return a running JVM.");
        return 1U;
    }

    // The remote options allocation belongs to the controller. The common
    // scheduler copies it before this raw CreateRemoteThread entry returns.
    return scheduleBootstrap(vm, options, BootstrapOrigin::NativeExport) == JNI_OK
               ? 0U
               : 1U;
}

BOOL APIENTRY DllMain(HMODULE module, const DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        // Loader-lock safety: initialization, threads, hooks and ImGui all start
        // from an explicit agent/export entry point, never from DllMain.
        ::DisableThreadLibraryCalls(module);
        g_agentModule = module;
    }
    return TRUE;
}
