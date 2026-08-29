#pragma once

#include "AgentOptions.h"
#include "overlay_renderer.h"

#include <jni.h>
#include <jvmti.h>
#include <windows.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mcoverlay {

class GameBindings;
class IpcClient;
class OpenGlHook;

class AgentRuntime final {
public:
    static jint start(JavaVM* vm, const char* options) noexcept;
    static void stop(bool vmUnloading) noexcept;

    AgentRuntime(const AgentRuntime&) = delete;
    AgentRuntime& operator=(const AgentRuntime&) = delete;

private:
    AgentRuntime(JavaVM* vm, jvmtiEnv* jvmti, AgentOptions options);
    ~AgentRuntime();

    [[nodiscard]] bool launch() noexcept;
    void requestStop(bool vmUnloading) noexcept;
    void join() noexcept;
    static unsigned __stdcall workerEntry(void* context) noexcept;
    void workerMain() noexcept;
    [[nodiscard]] bool launchResolver() noexcept;
    static unsigned __stdcall resolverEntry(void* context) noexcept;
    void resolverMain() noexcept;
    void joinResolver() noexcept;
    [[nodiscard]] bool launchBedScanner() noexcept;
    static unsigned __stdcall bedScannerEntry(void* context) noexcept;
    void bedScannerMain() noexcept;
    void joinBedScanner() noexcept;
    [[nodiscard]] bool launchTelemetry() noexcept;
    static unsigned __stdcall telemetryEntry(void* context) noexcept;
    void telemetryMain() noexcept;
    void joinTelemetry() noexcept;
    void queueTelemetry(const class GameSnapshot& snapshot,
                        std::uint64_t tickMilliseconds) noexcept;
    void queueStateChanged(bool visible, bool interactive) noexcept;
    void queueFeatureChanged(const FeatureSettings& settings) noexcept;
    void queueHypixelQuery(const std::array<char, 17U>& playerId) noexcept;
    void queueMenuHotkeyChanged(unsigned virtualKey) noexcept;
    void queueGuiScaleChanged(int index) noexcept;
    void queueBlacklistAction(const BlacklistAction& action) noexcept;
    void queueRendererReady() noexcept;
    static void frameEntry(void* context, HDC deviceContext) noexcept;
    void beforeSwapBuffers(HDC deviceContext);
    [[nodiscard]] bool handleControlLine(std::string_view line) noexcept;
    [[nodiscard]] bool sendHello() noexcept;
    [[nodiscard]] bool sendHandshake() noexcept;
    [[nodiscard]] bool shutdownGraphics() noexcept;

    JavaVM* m_vm = nullptr;
    jvmtiEnv* m_jvmti = nullptr;
    AgentOptions m_options;

    std::unique_ptr<IpcClient> m_ipc;
    std::unique_ptr<OpenGlHook> m_hook;
    std::unique_ptr<OverlayRenderer> m_renderer;
    std::unique_ptr<GameBindings> m_bindings;

    HANDLE m_stopEvent = nullptr;
    HANDLE m_readyEvent = nullptr;
    HANDLE m_callbacksIdleEvent = nullptr;
    HANDLE m_rendererStoppedEvent = nullptr;
    HANDLE m_worker = nullptr;
    HANDLE m_resolver = nullptr;
    HANDLE m_bedScanner = nullptr;
    HANDLE m_telemetryEvent = nullptr;
    HANDLE m_telemetry = nullptr;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_startSucceeded{false};
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_interactive{false};
    std::atomic<std::uint32_t> m_featureBits{0x286FBC7FU};
    std::atomic<int> m_bedDefenseRadius{6};
    std::atomic<int> m_bedThreatRadius{8};
    std::atomic<int> m_bedDefenseHotkey{VK_LMENU};
    std::atomic<int> m_bedDefensePanelOpacity{78};
    std::atomic<int> m_hypixelPanelHotkey{VK_TAB};
    std::atomic<int> m_hypixelPanelOpacity{76};
    std::atomic<int> m_hypixelPanelScale{100};
    std::atomic<int> m_hypixelPanelHeight{100};
    std::atomic<int> m_hypixelPanelX{-1};
    std::atomic<int> m_hypixelPanelY{-1};
    std::atomic<bool> m_clickGuiLightTheme{false};
    std::atomic<std::uint32_t> m_playerEspColor{0xFF3B30U};
    std::atomic<std::uint32_t> m_bedEspColor{0xFF5C68U};
    std::atomic<std::uint32_t> m_bedDefensePanelColor{0x191621U};
    std::atomic<std::uint32_t> m_hypixelPanelColor{0x000000U};
    std::atomic<std::uint32_t> m_hypixelRailColor{0x825DE8U};
    std::atomic<int> m_hypixelRailOpacity{100};
    std::atomic<int> m_safewalkReleaseDelayMs{120};
    std::atomic<int> m_safewalkEdgeSensitivity{55};
    std::atomic<int> m_safewalkMinimumPitch{-5};
    std::atomic<int> m_safewalkHotkey{VK_F8};
    std::atomic<int> m_flySpeedPercent{100};
    std::atomic<int> m_bhopAirSpeedPercent{100};
    std::atomic<std::uint64_t> m_featureHotkeysPackedA{0U};
    std::atomic<std::uint64_t> m_featureHotkeysPackedB{0U};
    std::atomic<bool> m_fireballEspEnabled{false};
    std::atomic<bool> m_fireballEspFilled{true};
    std::atomic<bool> m_longJumpEnabled{false};
    std::atomic<int> m_longJumpSpeedPercent{100};
    std::atomic<std::uint32_t> m_fireballEspColor{0xFF9D3DU};
    std::atomic<int> m_aimSlowdownPercent{45};
    std::atomic<int> m_aimSpeedPercent{35};
    std::atomic<std::uint32_t> m_textGuiColor{0x7EE7FFU};
    std::atomic<int> m_textGuiX{-1};
    std::atomic<int> m_textGuiY{-1};
    std::atomic<int> m_nametagPanelOpacity{82};
    std::atomic<std::uint32_t> m_nametagPanelColor{0x101218U};
    std::atomic<std::uint32_t> m_clickGuiAccentColor{0x825DE8U};
    std::atomic<int> m_hypixelPanelFontIndex{1};
    std::atomic<int> m_nametagRange{32};
    std::atomic<int> m_nametagSizeIndex{1};
    std::atomic<bool> m_detachRequested{false};
    std::atomic<bool> m_shutdownRequested{false};
    std::atomic<bool> m_vmUnloading{false};
    std::atomic<bool> m_rendererReadySent{false};
    std::atomic<bool> m_handshakeSent{false};
    // Set only after the owning SwapBuffers thread has shut down ImGui while
    // its original HGLRC is current. This is distinct from full runtime
    // cleanup, which additionally requires hook removal and JNI release.
    std::atomic<bool> m_renderCleanupStarted{false};
    std::atomic<bool> m_renderCleanupCompleted{false};
    std::atomic<bool> m_cleanupCompleted{false};
    std::atomic<unsigned> m_activeCallbacks{0U};
    SRWLOCK m_renderLock = SRWLOCK_INIT;

    // SwapBuffers is a latency-critical foreign callback. It may only publish
    // fixed-size values into this mailbox; formatting and all named-pipe I/O
    // happen on m_telemetry. TryAcquireSRWLockExclusive makes publication
    // non-blocking. Dropping one sample is harmless because the next sample
    // replaces it with the newest state.
    struct GameStateMessage final {
        std::uint64_t sequence = 0U;
        std::uint64_t unixMilliseconds = 0U;
        double health = 0.0;
        double maxHealth = 0.0;
        std::int32_t entityId = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        std::int32_t loadedEntities = 0;
        std::uint32_t bedCount = 0U;
        std::uint8_t state = 0U;
        bool valid = false;
        std::array<char, 96U> mapping{};
    };
    struct TelemetryMailbox final {
        GameStateMessage gameState{};
        std::uint64_t gameStateRevision = 0U;
        std::array<char, 17U> hypixelQuery{};
        std::uint64_t hypixelQueryRevision = 0U;
        std::array<PlayerIdentity, GameSnapshot::MaxDiscoveredPlayers> players{};
        std::uint32_t playerCount = 0U;
        std::uint64_t playerRosterGeneration = 0U;
        std::array<char, 17U> localPlayerName{};
        bool matchActive = false;
    };
    SRWLOCK m_telemetryLock = SRWLOCK_INIT;
    TelemetryMailbox m_telemetryMailbox{};
    std::atomic<std::uint32_t> m_stateChangedRevision{0U};
    std::atomic<std::uint8_t> m_stateChangedBits{0U};
    std::atomic<std::uint32_t> m_featureChangedRevision{0U};
    std::atomic<std::uint32_t> m_featureChangedBits{0x286FBC7FU};
    std::atomic<int> m_featureChangedBedRadius{6};
    std::atomic<int> m_featureChangedThreatRadius{8};
    std::atomic<int> m_featureChangedBedHotkey{VK_LMENU};
    std::atomic<int> m_featureChangedPanelOpacity{78};
    std::atomic<int> m_featureChangedHypixelHotkey{VK_TAB};
    std::atomic<int> m_featureChangedHypixelOpacity{76};
    std::atomic<int> m_featureChangedHypixelScale{100};
    std::atomic<int> m_featureChangedHypixelHeight{100};
    std::atomic<int> m_featureChangedHypixelX{-1};
    std::atomic<int> m_featureChangedHypixelY{-1};
    std::atomic<bool> m_featureChangedClickGuiLightTheme{false};
    std::atomic<std::uint32_t> m_featureChangedPlayerColor{0xFF3B30U};
    std::atomic<std::uint32_t> m_featureChangedBedColor{0xFF5C68U};
    std::atomic<std::uint32_t> m_featureChangedPanelColor{0x191621U};
    std::atomic<std::uint32_t> m_featureChangedHypixelColor{0x000000U};
    std::atomic<std::uint32_t> m_featureChangedHypixelRailColor{0x825DE8U};
    std::atomic<int> m_featureChangedHypixelRailOpacity{100};
    std::atomic<int> m_featureChangedNametagOpacity{82};
    std::atomic<std::uint32_t> m_featureChangedNametagColor{0x101218U};
    std::atomic<std::uint32_t> m_featureChangedAccentColor{0x825DE8U};
    std::atomic<int> m_featureChangedHypixelFontIndex{1};
    std::atomic<int> m_featureChangedNametagRange{32};
    std::atomic<int> m_featureChangedNametagSizeIndex{1};
    std::atomic<int> m_featureChangedSafewalkReleaseDelayMs{120};
    std::atomic<int> m_featureChangedSafewalkEdgeSensitivity{55};
    std::atomic<int> m_featureChangedSafewalkMinimumPitch{-5};
    std::atomic<int> m_featureChangedSafewalkHotkey{VK_F8};
    std::atomic<int> m_featureChangedFlySpeedPercent{100};
    std::atomic<int> m_featureChangedBhopAirSpeedPercent{100};
    std::atomic<std::uint64_t> m_featureChangedHotkeysPackedA{0U};
    std::atomic<std::uint64_t> m_featureChangedHotkeysPackedB{0U};
    std::atomic<bool> m_featureChangedFireballEspEnabled{false};
    std::atomic<bool> m_featureChangedFireballEspFilled{true};
    std::atomic<bool> m_featureChangedLongJumpEnabled{false};
    std::atomic<int> m_featureChangedLongJumpSpeedPercent{100};
    std::atomic<std::uint32_t> m_featureChangedFireballEspColor{0xFF9D3DU};
    std::atomic<int> m_featureChangedAimSlowdownPercent{45};
    std::atomic<int> m_featureChangedAimSpeedPercent{35};
    std::atomic<std::uint32_t> m_featureChangedTextGuiColor{0x7EE7FFU};
    std::atomic<int> m_featureChangedTextGuiX{-1};
    std::atomic<int> m_featureChangedTextGuiY{-1};
    std::atomic<bool> m_rendererReadyQueued{false};
    std::atomic<unsigned> m_menuHotkey{VK_OEM_7};
    std::atomic<unsigned> m_bindChangedKey{VK_OEM_7};
    std::atomic<std::uint32_t> m_bindChangedRevision{0U};
    std::atomic<int> m_guiScaleIndex{1};
    std::atomic<int> m_guiScaleChangedIndex{1};
    std::atomic<std::uint32_t> m_guiScaleChangedRevision{0U};
    std::atomic<bool> m_frameFaulted{false};
    std::uint64_t m_lastTelemetryTick = 0U; // render-thread owned
    std::uint64_t m_telemetrySequence = 0U; // render-thread owned
    SRWLOCK m_hypixelLock = SRWLOCK_INIT;
    HypixelOverlaySnapshot m_hypixelSnapshot{};
    SRWLOCK m_playerStatsLock = SRWLOCK_INIT;
    std::unordered_map<std::string, PlayerStatsEntry> m_playerStats;
    SRWLOCK m_blacklistLock = SRWLOCK_INIT;
    BlacklistOverlaySnapshot m_blacklistSnapshot{};
    BlacklistOverlaySnapshot m_runtimeBlacklistSnapshot{};
    std::atomic<std::uint32_t> m_blacklistRevision{0U};
    bool m_blacklistSyncInProgress = false; // control-pipe thread owned
    std::uint32_t m_renderBlacklistRevision = 0U;
    std::uint32_t m_runtimeBlacklistRevision = 0U;
    std::array<std::array<char, 50U>, 128U> m_blacklistChatWarnedKeys{};
    std::uint32_t m_blacklistChatWarnedCount = 0U;
    SRWLOCK m_blacklistActionLock = SRWLOCK_INIT;
    BlacklistAction m_blacklistAction{};
    std::atomic<std::uint32_t> m_blacklistActionRevision{0U};
    bool m_gameInputReleased = false; // render-thread owned
    bool m_renderThreadAttachedByAgent = false; // render-thread owned
    DWORD m_renderJvmThreadId = 0U; // render-thread owned
    std::uint64_t m_lastInputFocusReleaseTick = 0U; // render-thread owned
    bool m_safewalkHotkeyWasDown = false; // render-thread owned
};

} // namespace mcoverlay
