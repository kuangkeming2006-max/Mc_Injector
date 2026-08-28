#pragma once

#include "ThreadQueue.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cli {

// Minimal pure-Win32 controller for the native JVM agent. It does exactly one
// job: own the authenticated named-pipe session, launch the JVM Attach helper
// (with the visible native-DLL fallback), report the handshake result, and
// detach gracefully. Telemetry, Hypixel, and feature plumbing were
// deliberately removed — the agent's own Click GUI keeps its defaults.
class WinOverlayManager
{
public:
    enum class State
    {
        Detached,
        Validating,
        StartingIpc,
        LaunchingAttachHelper,
        WaitingForAgent,
        WaitingForOpenGL,
        Active,
        Detaching,
        Error
    };

    enum class LoaderKind { None, JvmAttach, NativeLoadLibrary };

    WinOverlayManager();
    ~WinOverlayManager();

    WinOverlayManager(const WinOverlayManager &) = delete;
    WinOverlayManager &operator=(const WinOverlayManager &) = delete;

    // Main-thread notifications.
    std::function<void()> onStateChanged;
    std::function<void(const std::wstring &)> onStatus;
    std::function<void(const std::wstring &, const std::wstring &)> onError;
    std::function<void(const std::wstring &)> onRenderer;
    std::function<void(uint32_t)> onTargetExited;

    // Returns true when the asynchronous attach was started or queued behind
    // a graceful detach. attached() becomes true only after an authenticated
    // handshake arrives from the DLL inside the target JVM.
    bool attachToProcess(uint32_t pid);
    void detach();

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool attached() const noexcept
    {
        return m_state == State::WaitingForOpenGL || m_state == State::Active;
    }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] uint32_t targetPid() const noexcept { return m_targetPid; }
    [[nodiscard]] const std::wstring &targetTitle() const noexcept { return m_targetTitle; }
    [[nodiscard]] const std::wstring &renderer() const noexcept { return m_renderer; }
    [[nodiscard]] const std::wstring &statusMessage() const noexcept { return m_status; }
    [[nodiscard]] const std::wstring &errorCode() const noexcept { return m_errorCode; }
    [[nodiscard]] const std::wstring &errorDetail() const noexcept { return m_errorDetail; }

    // Console main-loop integration. Call onTick() every loop iteration (the
    // loop wakes at least every ~400 ms) to drive attach/detach deadlines and
    // the target monitor; processMessages() drains the pipe/helper queues;
    // postDrain() processes a queued process switch.
    void onTick();
    void processMessages();
    void postDrain();
    void shutdown();

    [[nodiscard]] HANDLE pipeEvent() const noexcept { return m_pipeQueue.event(); }
    [[nodiscard]] HANDLE helperEvent() const noexcept { return m_helperQueue.event(); }

private:
    struct PipeMessage
    {
        bool disconnected = false;
        bool lineTooLong = false;
        std::string line;
    };

    struct HelperMessage
    {
        bool exited = false;
        DWORD exitCode = 0;
        bool killed = false;
        std::string stderrText;
    };

    struct JavaRuntime
    {
        std::wstring executable;
        std::wstring toolsJar;
        bool modular = true;
    };

    void setState(State state);
    void setStatus(const std::wstring &status);
    void setRenderer(const std::wstring &renderer);
    void setError(const std::wstring &code, const std::wstring &detail);
    void clearError();
    void fail(const std::wstring &code, const std::wstring &detail);

    void startPipeServer(const std::wstring &pipeName);
    void pipeThreadLoop();
    void handleAgentLine(const std::string &line);
    void handleAgentDisconnected();
    void handleHelperMessage(const HelperMessage &message);

    bool startNativeLoaderFallback();
    void handleAttachFinished(DWORD exitCode, bool normalExit,
                              const std::string &stderrText);

    void beginDetach();
    void completeDetach(bool timedOut);
    void finalizeDetachedState();
    bool closeSessionTransport();
    void monitorTarget();

    void sendInitialSnapshot();
    bool writeAgentLine(const std::string &line);

    [[nodiscard]] static std::wstring executableDir();
    [[nodiscard]] static std::wstring locateAgentDll();
    [[nodiscard]] static std::wstring locateAttachHelper();
    [[nodiscard]] static std::wstring locateNativeLoader();
    [[nodiscard]] static JavaRuntime locateJavaRuntime(const std::wstring &targetExecutable);
    [[nodiscard]] static std::wstring targetExecutablePath(uint32_t pid);
    [[nodiscard]] static std::wstring targetWindowTitle(uint32_t pid);
    [[nodiscard]] static bool targetArchitectureSupported(uint32_t pid);
    [[nodiscard]] static bool targetProcessIsRunning(uint32_t pid);
    [[nodiscard]] static bool targetHasLoadedModule(uint32_t pid, const wchar_t *moduleName);
    [[nodiscard]] static bool spawnHelper(const std::wstring &executable,
                                          const std::wstring &arguments,
                                          HANDLE &process,
                                          std::thread &reaper,
                                          WaitQueue<HelperMessage> &queue,
                                          std::atomic<bool> &killedFlag);
    static void helperReaperProc(HANDLE process, HANDLE stderrPipe,
                                 WaitQueue<HelperMessage> *queue,
                                 std::atomic<bool> *killedFlag);

    // Named pipe.
    std::wstring m_pipeName;
    HANDLE m_pipeStopEvent = nullptr;
    std::thread m_pipeThread;
    std::atomic<HANDLE> m_pipeHandle{INVALID_HANDLE_VALUE};
    std::mutex m_pipeWriteMutex;
    WaitQueue<PipeMessage> m_pipeQueue;

    // Attach helper / native loader process.
    WaitQueue<HelperMessage> m_helperQueue;
    HANDLE m_helperProcess = nullptr;
    std::thread m_helperReaper;
    std::atomic<bool> m_helperKilled{false};
    bool m_helperRunning = false;

    State m_state = State::Detached;
    LoaderKind m_loaderKind = LoaderKind::None;
    uint32_t m_targetPid = 0;
    std::wstring m_targetTitle;
    std::wstring m_renderer;
    std::wstring m_status = L"Detached";
    std::wstring m_errorCode;
    std::wstring m_errorDetail;
    std::string m_pipeToken;
    std::wstring m_agentDllPath;
    std::string m_agentOptions;
    bool m_authenticated = false;
    bool m_nativeFallbackAttempted = false;
    bool m_detachTransportComplete = false;
    bool m_detachTimedOut = false;
    bool m_closingTransport = false;
    uint32_t m_pendingAttachPid = 0;
    std::wstring m_jvmAttachFallbackReason;

    // Deadline-based timers driven by onTick().
    bool m_attachTimerActive = false;
    unsigned long long m_attachDeadline = 0;
    bool m_graceTimerActive = false;
    unsigned long long m_graceDeadline = 0;
    bool m_detachTimerActive = false;
    unsigned long long m_detachDeadline = 0;
    unsigned long long m_nextMonitorTick = 0;
};

} // namespace cli
