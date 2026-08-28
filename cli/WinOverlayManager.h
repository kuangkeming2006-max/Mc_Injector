#pragma once

#include "ThreadQueue.h"
#include "WinSettings.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cli {

// The read-only game telemetry snapshot populated by authenticated GAME_STATE
// IPC messages. `available` means the numeric payload is valid; `stale` is
// driven by controller receipt time so an incorrect target clock cannot make
// old values appear live.
struct GameSnapshot
{
    bool received = false;
    bool available = false;
    bool stale = false;
    double health = 0.0;
    double maxHealth = 0.0;
    int entityId = 0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    int loadedEntities = 0;
    int bedCount = 0;
    std::wstring mappingProfile;
    std::wstring mappingState = L"waiting";
    uint64_t sequence = 0;
    unsigned long long timestampMs = 0;
};

// Pure-Win32 controller of the native JVM agent. It owns the named-pipe
// server, the authenticated line protocol, the Attach-helper/native-loader
// processes, and the feature/bind/scale snapshots. All public methods run on
// the console main loop's thread; the pipe reader and helper reaper run on
// private threads and marshal their events through WaitQueue.
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

    // Timer ids the console main loop maps onto SetTimer/KillTimer.
    enum TimerId : UINT_PTR
    {
        TimerAttach = 100,
        TimerFallbackGrace = 101,
        TimerDetach = 102,
        TimerTargetMonitor = 103,
        TimerFreshness = 104,
        TimerFeatureStore = 105
    };

    WinOverlayManager();
    ~WinOverlayManager();

    WinOverlayManager(const WinOverlayManager &) = delete;
    WinOverlayManager &operator=(const WinOverlayManager &) = delete;

    // Timer bridge: implement with a message window in the main loop.
    std::function<void(TimerId, UINT ms)> requestTimer;
    std::function<void(TimerId)> cancelTimer;

    // Main-thread notifications.
    std::function<void()> onStateChanged;
    std::function<void(const std::wstring &)> onStatus;
    std::function<void(const std::wstring &, const std::wstring &)> onError;
    std::function<void(const std::wstring &)> onRenderer;
    std::function<void()> onTelemetry;
    std::function<void(const std::wstring &)> onPlayerName;
    std::function<void(bool)> onMatchState;
    std::function<void(const std::wstring &, const std::wstring &)> onPlayerFound;
    std::function<void(const std::wstring &)> onHypixelQuery;
    std::function<void(uint32_t)> onTargetExited;
    std::function<void(int)> onMenuHotkeyChanged;
    std::function<void(int)> onGuiScaleIndexChanged;
    std::function<void()> onSessionReady;

    // Returns true when the asynchronous attach was started or queued behind
    // a graceful detach. attached() becomes true only after an authenticated
    // handshake arrives from the DLL inside the target JVM.
    bool attachToProcess(uint32_t pid);
    void detach();
    void refreshBedCache();

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
    [[nodiscard]] const GameSnapshot &game() const noexcept { return m_game; }
    [[nodiscard]] const std::wstring &playerName() const noexcept { return m_playerName; }
    [[nodiscard]] bool matchActive() const noexcept { return m_matchActive; }
    [[nodiscard]] int menuHotkey() const noexcept { return m_menuHotkey; }
    [[nodiscard]] int guiScaleIndex() const noexcept { return m_guiScaleIndex; }

    void setMenuHotkey(int virtualKey);
    void setGuiScaleIndex(int index);

    // Agent publication mirrors of the Hypixel/statistics pipeline.
    void publishHypixelResult(int state, const std::wstring &uuid,
                              const std::wstring &displayName,
                              long long wins, long long losses,
                              long long finalKills, long long finalDeaths,
                              long long bedsBroken, long long bedsLost,
                              double winRate, double fkdr,
                              const std::wstring &status);
    void publishPlayerStats(const std::wstring &playerName,
                            const std::wstring &teamPrefix,
                            int stars, double fkdr, int level);
    void publishPlayerStatsError(const std::wstring &playerName,
                                 const std::wstring &reason);

    // Console main-loop integration.
    void processMessages(); // drains the pipe and helper queues
    void postDrain();       // processes a queued process switch
    void onTimer(TimerId id);
    void shutdown();        // synchronous teardown at exit

    HANDLE pipeEvent() const noexcept { return m_pipeQueue.event(); }
    HANDLE helperEvent() const noexcept { return m_helperQueue.event(); }

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
    void refreshFreshness();
    void resetGameState();
    void storeFeatureSettings();

    void sendStateSnapshot();
    void sendFeatureSnapshot();
    void sendBindSnapshot();
    void sendGuiScaleSnapshot();
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

    WinSettings m_settings;
    FeatureSettings m_features;

    State m_state = State::Detached;
    LoaderKind m_loaderKind = LoaderKind::None;
    uint32_t m_targetPid = 0;
    std::wstring m_targetTitle;
    std::wstring m_renderer;
    std::wstring m_status = L"Native overlay is detached";
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

    bool m_overlayEnabled = true;
    bool m_interactive = false;
    int m_menuHotkey = 0xDE;   // VK_OEM_7 / apostrophe
    int m_guiScaleIndex = 1;   // S/M/L/XL -> 0..3

    GameSnapshot m_game;
    unsigned long long m_lastGameStateReceiptTick = 0;
    std::wstring m_playerName;
    bool m_matchActive = false;
};

} // namespace cli
