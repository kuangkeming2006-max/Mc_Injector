#pragma once

#include <jni.h>
#include <jvmti.h>

namespace mcoverlay::jvm {

// Uses the JavaVM passed to Agent_OnLoad/Agent_OnAttach whenever available.
// The lookup fallback exists only for an explicitly invoked ordinary-DLL
// entry point; DllMain never performs JVM work.
[[nodiscard]] JavaVM* resolveJavaVm(JavaVM* supplied) noexcept;
[[nodiscard]] jvmtiEnv* resolveJvmti(JavaVM* vm) noexcept;

class ScopedThreadEnv final {
public:
    explicit ScopedThreadEnv(JavaVM* vm, bool attachIfDetached = true) noexcept;
    ~ScopedThreadEnv();

    ScopedThreadEnv(const ScopedThreadEnv&) = delete;
    ScopedThreadEnv& operator=(const ScopedThreadEnv&) = delete;

    [[nodiscard]] JNIEnv* get() const noexcept { return m_env; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_env != nullptr; }

private:
    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    bool m_attachedByUs = false;
};

} // namespace mcoverlay::jvm

