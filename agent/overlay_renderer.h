#pragma once

#include "bindings/GameBindings.h"
#include "wndproc_hook.h"

#include <windows.h>

#include <atomic>
#include <array>
#include <cstdint>

struct ImGuiContext;
struct ImFont;

namespace mcoverlay {

struct OverlayInputState;

struct FeatureSettings final {
    bool espEnabled = true;
    bool entityEspEnabled = true;
    bool entityEspPlayersOnly = false;
    bool bedEspEnabled = true;
    bool bedAutoRefreshEnabled = false;
    bool labelsEnabled = true;
    bool hypixelPanelEnabled = true;
    bool bedThreatAlertsEnabled = true;
    bool bedDefensePanelEnabled = true;
    bool bedEspFilled = false;
    bool debugChatEnabled = true;
    bool showOwnBedDefenseInfo = true;
    bool showTeammateBoxes = true;
    bool bedDefenseHoldToShow = true;
    bool bedDefensePerspectiveScale = false;
    int bedDefenseRadius = 6;
    int bedThreatRadius = 8;
    int bedDefenseHotkey = VK_LMENU;
    int bedDefensePanelOpacity = 78;
    // Stored as 0xRRGGBB so the value is renderer-independent and can travel
    // through the text IPC protocol without floating-point round trips.
    std::uint32_t playerEspColor = 0xFF3B30U;
    std::uint32_t bedEspColor = 0xFF5C68U;
    std::uint32_t bedDefensePanelColor = 0x191621U;

    [[nodiscard]] bool operator==(const FeatureSettings&) const noexcept = default;
};

struct HypixelOverlaySnapshot final {
    enum class State : std::uint8_t { Idle, Loading, Ready, Error };
    State state = State::Idle;
    std::array<char, 40U> uuid{};
    std::array<char, 48U> displayName{};
    std::array<char, 160U> status{};
    std::int64_t wins = 0;
    std::int64_t losses = 0;
    std::int64_t finalKills = 0;
    std::int64_t finalDeaths = 0;
    std::int64_t bedsBroken = 0;
    std::int64_t bedsLost = 0;
    double winRate = 0.0;
    double fkdr = 0.0;
};

struct PlayerStatsEntry final {
    std::array<char, 17U> name{};
    std::array<char, 5U> teamPrefix{};
    std::int32_t stars = 0;
    double fkdr = 0.0;
    std::int32_t level = 0;
};

struct PlayerStatsOverlaySnapshot final {
    static constexpr std::size_t Capacity = 64U;
    std::array<PlayerStatsEntry, Capacity> entries{};
    std::uint32_t count = 0U;
};

class OverlayRenderer final {
public:
    OverlayRenderer();
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    // Returns true exactly once after each successful HWND/HGLRC generation.
    [[nodiscard]] bool render(HDC deviceContext,
                              const GameSnapshot& snapshot,
                              bool interactive) noexcept;
    void shutdownWithCurrentContext() noexcept;
    void abandonAfterHookDisabled() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return m_initialized; }
    [[nodiscard]] bool ownsCurrentContext() const noexcept;
    [[nodiscard]] bool consumeClickGuiToggle() noexcept;
    void setFeatureSettings(const FeatureSettings& settings) noexcept;
    [[nodiscard]] bool consumeFeatureSettings(FeatureSettings& settings) noexcept;
    void setHypixelSnapshot(const HypixelOverlaySnapshot& snapshot) noexcept;
    void setPlayerStatsSnapshot(const PlayerStatsOverlaySnapshot& snapshot) noexcept;
    [[nodiscard]] bool consumeHypixelQuery(std::array<char, 17U>& playerId) noexcept;
    void setMenuHotkey(unsigned virtualKey) noexcept;
    [[nodiscard]] bool consumeMenuHotkeyChange(unsigned& virtualKey) noexcept;
    void setGuiScaleIndex(int index) noexcept;
    [[nodiscard]] bool consumeGuiScaleChange(int& index) noexcept;
    [[nodiscard]] bool consumeBedRescanRequest() noexcept;

private:
    [[nodiscard]] bool initialize(HWND window, HGLRC context) noexcept;
    void pollFallbackInput() noexcept;
    void renderInventoryBlur(float strength) noexcept;
    void applyGuiScaleStyle(float scale, int fontIndex) noexcept;
    void enqueueFeatureToasts(const FeatureSettings& before,
                              const FeatureSettings& after) noexcept;
    void enqueueToast(const char* label, bool enabled) noexcept;
    void enqueueMessage(const char* message, bool positive) noexcept;
    void updateBedThreatAlerts(const GameSnapshot& snapshot) noexcept;
    void renderToasts(float deltaSeconds, float uiScale) noexcept;
    void abandonForContextChange() noexcept;
    void abandonAfterWndProcDrainTimeout() noexcept;
    static LRESULT handleWindowMessage(void* context,
                                       HWND window,
                                       UINT message,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       bool& handled) noexcept;
    [[nodiscard]] static LRESULT onWindowMessage(OverlayInputState& input,
                                                 HWND window,
                                                 UINT message,
                                                 WPARAM wParam,
                                                 LPARAM lParam,
                                                 bool& handled) noexcept;

    HWND m_window = nullptr;
    HGLRC m_glContext = nullptr;
    WndProcHook m_windowProcedure;
    ImGuiContext* m_imguiContext = nullptr;
    // Kept separate from OverlayRenderer so a bounded WndProc drain timeout
    // can intentionally retain this tiny bridge without retaining/dereferencing
    // a destroyed OverlayRenderer object.
    OverlayInputState* m_inputState = nullptr;
    bool m_initialized = false;
    bool m_permanentlyDisabled = false;
    bool m_wndProcFallbackLogged = false;
    FeatureSettings m_features{};
    HypixelOverlaySnapshot m_hypixel{};
    PlayerStatsOverlaySnapshot m_playerStats{};
    bool m_featureSettingsDirty = false;
    bool m_hypixelQueryPending = false;
    std::array<char, 17U> m_hypixelQuery{};
    std::array<char, 17U> m_hypixelInput{};
    float m_clickGuiProgress = 0.0F;
    float m_toggleAnimation[15]{};
    float m_clickGuiX = -9999.0F;
    float m_clickGuiY = 18.0F;
    double m_lastBedRefreshTime = 0.0;
    int m_guiScaleIndex = 1;
    int m_appliedGuiScaleIndex = -1;
    float m_animatedGuiScale = 1.25F;
    std::array<ImFont*, 4U> m_fonts{};
    unsigned m_menuHotkey = VK_OEM_7;
    bool m_menuHotkeyDirty = false;
    bool m_guiScaleDirty = false;
    bool m_bedRescanPending = false;
    bool m_waitingForHotkey = false;
    int m_hotkeyCaptureCooldownFrames = 0;
    bool m_hotkeyCaptureArmed = false;
    bool m_featureSnapshotInitialized = false;
    struct Toast final {
        std::array<char, 52U> label{};
        float age = 0.0F;
        std::uint64_t sequence = 0U;
        bool enabled = false;
        bool active = false;
    };
    std::array<Toast, 6U> m_toasts{};
    std::uint64_t m_toastSequence = 0U;
    struct ThreatContact final {
        jint entityId = -1;
        std::array<char, 17U> playerName{};
        char teamColor = 'u';
        int bedX = 0;
        int bedY = 0;
        int bedZ = 0;
        double distance = 0.0;
        std::uint64_t lastSeenTick = 0U;
        bool inside = false;
    };
    std::array<ThreatContact, 64U> m_threatContacts{};
    std::uint64_t m_lastEntitySampleGeneration = 0U;
    float m_lastEntityPartialTicks = 0.0F;
    unsigned m_missedEntityTicks = 0U;
    unsigned m_blurTexture = 0U;
    unsigned m_bedTexture = 0U;
    unsigned m_blockTextures[6]{};
    int m_blurWidth = 0;
    int m_blurHeight = 0;

};

} // namespace mcoverlay
