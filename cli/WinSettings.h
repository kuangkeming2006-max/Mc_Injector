#pragma once

#include <windows.h>

#include <string>

namespace cli {

// The seven feature toggles plus radius/colour preferences mirrored by the
// controller and the in-game Click GUI.
struct FeatureSettings
{
    bool espEnabled = true;
    bool entityEspEnabled = true;
    bool entityEspPlayersOnly = false;
    bool bedEspEnabled = true;
    bool bedAutoRefreshEnabled = false;
    bool espLabelsEnabled = true;
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
    int bedDefenseHotkey = 0xA4; // VK_LMENU
    int bedDefensePanelOpacity = 78;
    std::wstring playerEspColor = L"#FF3B30";
    std::wstring bedEspColor = L"#FF5C68";
    std::wstring bedDefensePanelColor = L"#191621";
};

// Registry-backed preferences in the same key tree the Qt dashboard's
// QSettings uses, so both front-ends share one configuration.
// QSettings stores bools/ints as REG_SZ ("true"/"false", decimal strings);
// reads tolerate REG_DWORD as well.
class WinSettings
{
public:
    static constexpr wchar_t kRootKey[] =
        L"Software\\Overlay Studio\\MinecraftOverlayManager";

    [[nodiscard]] int menuHotkey() const;
    void setMenuHotkey(int virtualKey);
    [[nodiscard]] int guiScaleIndex() const;
    void setGuiScaleIndex(int index);

    [[nodiscard]] FeatureSettings loadFeatures() const;
    void storeFeatures(const FeatureSettings &features) const;
};

} // namespace cli
