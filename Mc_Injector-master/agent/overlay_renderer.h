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
    static constexpr std::size_t FeatureHotkeyCount = 15U;
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
    bool hypixelPanelHoldToShow = true;
    bool clickGuiLightTheme = false;
    bool nametagEnabled = true;
    bool nametagSidePlacement = false;
    bool enemyItemIndicatorsEnabled = true;
    bool showTeammateNametags = true;
    bool nametagNearbyEnemiesOnly = false;
    bool nametagTeamPulse = true;
    bool showTeammateArrows = true;
    bool safewalkEnabled = false;
    bool scaffoldEnabled = false;
    bool flyEnabled = false;
    bool bhopEnabled = false;
    bool bhopAutoJump = true;
    bool aimAssistEnabled = false;
    bool aimSlowdownMode = true;
    bool textGuiEnabled = false;
    bool fireballEspEnabled = false;
    bool fireballEspFilled = true;
    bool longJumpEnabled = false;
    // High-risk movement helpers fail closed on Hypixel. This explicit,
    // persisted opt-in is intentionally separate from each feature switch so
    // an accidental hotkey press can never silently override the server guard.
    bool allowHypixelMovement = false;
    int bedDefenseRadius = 6;
    int bedThreatRadius = 8;
    int bedDefenseHotkey = VK_LMENU;
    int bedDefensePanelOpacity = 78;
    int hypixelPanelHotkey = VK_TAB;
    int hypixelPanelOpacity = 76;
    // The statistics card has an independent scale and normalized top-left
    // position so it remains usable when the Minecraft resolution changes.
    int hypixelPanelScale = 100;
    // Width and height are intentionally independent.  The legacy "scale"
    // value now controls width only; glyphs/row pitch remain fixed and crisp.
    int hypixelPanelHeight = 100;
    int hypixelPanelX = -1; // -1 = default right aligned; otherwise 0..1000
    int hypixelPanelY = -1; // -1 = default top aligned; otherwise 0..1000
    // Independent font preset (15/19/23/28 px). Panel resizing intentionally
    // does not scale glyphs, so rows remain readable and never overlap.
    int hypixelPanelFontIndex = 1;
    int nametagRange = 32;
    int nametagSizeIndex = 1;
    int safewalkReleaseDelayMs = 120;
    int safewalkEdgeSensitivity = 55;
    int safewalkMinimumPitch = -5;
    int safewalkHotkey = VK_F8;
    int flySpeedPercent = 100;
    int bhopAirSpeedPercent = 100;
    int longJumpSpeedPercent = 100;
    int aimSlowdownPercent = 45;
    int aimSpeedPercent = 35;
    int aimMinimumDistance = 0;
    int aimMaximumDistance = 16;
    int aimFovDegrees = 90;
    int clickGuiWidthPercent = 100;
    int clickGuiHeightPercent = 100;
    int clickGuiOpacity = 96;
    int textGuiX = -1;
    int textGuiY = -1;
    // Stored as 0xRRGGBB so the value is renderer-independent and can travel
    // through the text IPC protocol without floating-point round trips.
    std::uint32_t playerEspColor = 0xFF3B30U;
    std::uint32_t bedEspColor = 0xFF5C68U;
    std::uint32_t bedDefensePanelColor = 0x191621U;
    std::uint32_t hypixelPanelColor = 0x000000U;
    std::uint32_t hypixelRailColor = 0x825DE8U;
    std::uint32_t nametagPanelColor = 0x101218U;
    std::uint32_t clickGuiAccentColor = 0x825DE8U;
    std::uint32_t textGuiColor = 0x7EE7FFU;
    std::uint32_t fireballEspColor = 0xFF9D3DU;
    int nametagPanelOpacity = 82;
    int hypixelRailOpacity = 100;
    // Page master hotkeys in navigation order, excluding Interface. Zero is
    // deliberately "Unbound"; configured keys are persisted by Controller.
    std::array<int, FeatureHotkeyCount> featureHotkeys{};

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
    std::array<char, 97U> status{};
    std::int32_t stars = 0;
    double fkdr = 0.0;
    double wlr = 0.0;
    double bblr = 0.0;
    std::int64_t wins = 0;
    std::int64_t finalKills = 0;
    std::int64_t bedsBroken = 0;
    std::int32_t winStreak = 0;
    std::int32_t level = 0;
    bool failed = false;
};

struct PlayerStatsOverlaySnapshot final {
    static constexpr std::size_t Capacity = 64U;
    std::array<PlayerStatsEntry, Capacity> entries{};
    std::uint32_t count = 0U;
};

struct BlacklistEntry final {
    std::array<char, 50U> key{};
    std::array<char, 37U> uuid{};
    std::array<char, 17U> name{};
    std::array<char, 161U> reason{};
    std::array<char, 260U> facePath{};
    std::int64_t addedAt = 0;
    bool nick = false;
    bool idOnly = false;
    bool warnOnEncounter = true;
};

struct BlacklistOverlaySnapshot final {
    static constexpr std::size_t Capacity = 128U;
    static constexpr std::size_t PresetCapacity = 8U;
    std::array<BlacklistEntry, Capacity> entries{};
    std::uint32_t count = 0U;
    std::array<std::array<char, 81U>, PresetCapacity> presets{};
    std::uint32_t presetCount = 0U;
    bool panelEnabled = true;
    bool matchAlertsEnabled = true;
    bool allowIdOnlyNicks = true;
    bool showWithClickGui = true;
    bool collapsed = false;
    int panelOpacity = 82;
    std::uint32_t panelColor = 0x111218U;
    int panelX = -1;
    int panelY = -1;
    int panelWidth = 100;
    int panelHeight = 100;
};

struct BlacklistAction final {
    enum class Type : std::uint8_t { None, Add, Remove, Warning, Layout, Settings };
    Type type = Type::None;
    std::array<char, 50U> key{};
    std::array<char, 37U> uuid{};
    std::array<char, 17U> name{};
    std::array<char, 161U> reason{};
    bool idOnlyNick = false;
    bool warnOnEncounter = true;
    int x = -1;
    int y = -1;
    int width = 100;
    int height = 100;
    bool panelEnabled = true;
    bool matchAlertsEnabled = true;
    bool allowIdOnlyNicks = true;
    bool showWithClickGui = true;
    bool collapsed = false;
    int panelOpacity = 82;
    std::uint32_t panelColor = 0x111218U;
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
    void setBlacklistSnapshot(const BlacklistOverlaySnapshot& snapshot) noexcept;
    [[nodiscard]] bool consumeBlacklistAction(BlacklistAction& action) noexcept;
    [[nodiscard]] bool consumeHypixelQuery(std::array<char, 17U>& playerId) noexcept;
    void setMenuHotkey(unsigned virtualKey) noexcept;
    [[nodiscard]] bool consumeMenuHotkeyChange(unsigned& virtualKey) noexcept;
    void setGuiScaleIndex(int index) noexcept;
    [[nodiscard]] bool consumeGuiScaleChange(int& index) noexcept;
    [[nodiscard]] bool consumeBedRescanRequest() noexcept;

private:
    [[nodiscard]] bool initialize(HWND window, HGLRC context) noexcept;
    void pollFallbackInput() noexcept;
    void captureBackdropTexture() noexcept;
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
    bool m_cursorSessionActive = false;
    FeatureSettings m_features{};
    HypixelOverlaySnapshot m_hypixel{};
    PlayerStatsOverlaySnapshot m_playerStats{};
    BlacklistOverlaySnapshot m_blacklist{};
    BlacklistAction m_blacklistAction{};
    bool m_blacklistActionDirty = false;
    bool m_featureSettingsDirty = false;
    bool m_hypixelQueryPending = false;
    std::array<char, 17U> m_hypixelQuery{};
    std::array<char, 17U> m_hypixelInput{};
    float m_clickGuiProgress = 0.0F;
    float m_clickGuiVelocity = 0.0F;
    float m_statsPanelProgress = 0.0F;
    float m_statsPanelVelocity = 0.0F;
    float m_blacklistPanelProgress = 0.0F;
    float m_blacklistPanelVelocity = 0.0F;
    float m_toggleAnimation[48]{};
    float m_clickGuiX = -9999.0F;
    float m_clickGuiY = 18.0F;
    double m_lastBedRefreshTime = 0.0;
    int m_guiScaleIndex = 1;
    int m_appliedGuiScaleIndex = -1;
    float m_animatedGuiScale = 1.25F;
    std::array<ImFont*, 4U> m_fonts{};
    std::array<ImFont*, 4U> m_boldFonts{};
    unsigned m_menuHotkey = VK_OEM_7;
    bool m_menuHotkeyDirty = false;
    bool m_guiScaleDirty = false;
    bool m_bedRescanPending = false;
    bool m_waitingForHotkey = false;
    int m_hotkeyCaptureCooldownFrames = 0;
    bool m_hotkeyCaptureArmed = false;
    int m_hotkeyCaptureTarget = 0; // 1=menu, 2=bed, 3=stats, 4=safewalk
    int m_clickGuiPage = 0;
    int m_previousClickGuiPage = 0;
    float m_clickGuiPageProgress = 1.0F;
    float m_clickGuiNavPosition = 0.0F;
    std::array<float, 16U> m_clickGuiNavHover{};
    float m_clickGuiThemeProgress = 0.0F;
    bool m_statsPanelTransformDirty = false;
    bool m_statsPanelDragging = false;
    bool m_statsPanelResizing = false;
    float m_statsPanelResizeStartX = 0.0F;
    float m_statsPanelResizeStartY = 0.0F;
    int m_statsPanelResizeStartScale = 100;
    int m_statsPanelResizeStartHeight = 100;
    float m_statsPanelDragStartMouseX = 0.0F;
    float m_statsPanelDragStartMouseY = 0.0F;
    float m_statsPanelDragStartPanelX = 0.0F;
    float m_statsPanelDragStartPanelY = 0.0F;
    bool m_blacklistAddOpen = false;
    float m_blacklistAddProgress = 0.0F;
    float m_blacklistAddVelocity = 0.0F;
    int m_blacklistSelectedPlayer = -1;
    std::array<char, 161U> m_blacklistReasonInput{};
    bool m_blacklistIdOnlyNick = false;
    bool m_blacklistWarnOnEncounter = true;
    bool m_blacklistPanelDragging = false;
    bool m_blacklistPanelResizing = false;
    bool m_blacklistPanelTransformDirty = false;
    float m_blacklistDragStartMouseX = 0.0F;
    float m_blacklistDragStartMouseY = 0.0F;
    float m_blacklistDragStartPanelX = 0.0F;
    float m_blacklistDragStartPanelY = 0.0F;
    float m_blacklistResizeStartMouseX = 0.0F;
    float m_blacklistResizeStartMouseY = 0.0F;
    int m_blacklistResizeStartWidth = 100;
    int m_blacklistResizeStartHeight = 100;
    bool m_safewalkHotkeyWasDown = false;
    std::array<bool, FeatureSettings::FeatureHotkeyCount> m_featureHotkeyWasDown{};
    std::array<float, 14U> m_textGuiModuleProgress{};
    std::array<float, 14U> m_textGuiModuleVelocity{};
    bool m_scaffoldBlockedNoticeShown = false;
    bool m_textGuiDragging = false;
    float m_textGuiDragOffsetX = 0.0F;
    float m_textGuiDragOffsetY = 0.0F;
    struct BlacklistTexture final {
        std::array<char, 260U> path{};
        unsigned texture = 0U;
    };
    std::array<BlacklistTexture, 128U> m_blacklistTextures{};
    bool m_blacklistMatchWasActive = false;
    std::array<std::array<char, 50U>, 128U> m_blacklistWarnedKeys{};
    std::uint32_t m_blacklistWarnedCount = 0U;
    bool m_featureSnapshotInitialized = false;
    struct Toast final {
        std::array<char, 96U> label{};
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
        std::array<char, 37U> uuid{};
        char teamColor = 'u';
        int bedX = 0;
        int bedY = 0;
        int bedZ = 0;
        double distance = 0.0;
        std::uint64_t lastSeenTick = 0U;
        bool inside = false;
        bool invisible = false;
        std::uint32_t skinTextureId = 0U;
        float presentation = 0.0F;
    };
    std::array<ThreatContact, 64U> m_threatContacts{};
    struct NametagAnimation final {
        jint entityId = -1;
        float displayedHealth = 0.0F;
        std::uint64_t lastSeenTick = 0U;
    };
    std::array<NametagAnimation, GameSnapshot::MaxEntityMarkers> m_nametagAnimations{};
    std::uint64_t m_lastEntitySampleGeneration = 0U;
    float m_lastEntityPartialTicks = 0.0F;
    unsigned m_missedEntityTicks = 0U;
    unsigned m_blurTexture = 0U;
    unsigned m_bedTexture = 0U;
    unsigned m_blockTextures[6]{};
    int m_blurWidth = 0;
    int m_blurHeight = 0;
    bool m_backdropCapturedThisFrame = false;

};

} // namespace mcoverlay
