#include "WinSettings.h"

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <string>

namespace cli {
namespace {

class RegKey
{
public:
    explicit RegKey(const wchar_t *subKey)
    {
        (void) ::RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0,
                                 KEY_READ | KEY_WRITE, nullptr, &m_key, nullptr);
    }

    ~RegKey()
    {
        if (m_key != nullptr)
            ::RegCloseKey(m_key);
    }

    [[nodiscard]] bool valid() const noexcept { return m_key != nullptr; }

    std::wstring readString(const wchar_t *name, const std::wstring &fallback) const
    {
        if (!valid())
            return fallback;

        wchar_t buffer[512]{};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        if (::RegQueryValueExW(m_key, name, nullptr, &type,
                               reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
            if (type == REG_SZ || type == REG_EXPAND_SZ)
                return buffer;
            if (type == REG_DWORD && size == sizeof(DWORD)) {
                const DWORD value = *reinterpret_cast<const DWORD *>(buffer);
                wchar_t text[32]{};
                swprintf(text, std::size(text), L"%lu", value);
                return text;
            }
        }
        return fallback;
    }

    bool readBool(const wchar_t *name, const bool fallback) const
    {
        const std::wstring text = readString(name, {});
        if (text == L"true")
            return true;
        if (text == L"false")
            return false;
        return fallback;
    }

    int readInt(const wchar_t *name, const int fallback) const
    {
        const std::wstring text = readString(name, {});
        if (text.empty())
            return fallback;
        wchar_t *end = nullptr;
        const long parsed = wcstol(text.c_str(), &end, 10);
        return (end != text.c_str() && *end == L'\0') ? static_cast<int>(parsed) : fallback;
    }

    void writeString(const wchar_t *name, const std::wstring &value) const
    {
        if (!valid())
            return;
        (void) ::RegSetValueExW(m_key, name, 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    void writeBool(const wchar_t *name, const bool value) const
    {
        writeString(name, value ? L"true" : L"false");
    }

    void writeInt(const wchar_t *name, const int value) const
    {
        wchar_t text[32]{};
        swprintf(text, std::size(text), L"%d", value);
        writeString(name, text);
    }

private:
    HKEY m_key = nullptr;
};

constexpr wchar_t kOverlaySubKey[] = L"overlay";
constexpr wchar_t kFeaturesSubKey[] = L"features";

} // namespace

int WinSettings::menuHotkey() const
{
    const RegKey key((std::wstring(kRootKey) + L"\\" + kOverlaySubKey).c_str());
    return std::clamp(key.readInt(L"menuHotkey", 0xDE), 8, 254);
}

void WinSettings::setMenuHotkey(const int virtualKey)
{
    const RegKey key((std::wstring(kRootKey) + L"\\" + kOverlaySubKey).c_str());
    key.writeInt(L"menuHotkey", std::clamp(virtualKey, 8, 254));
}

int WinSettings::guiScaleIndex() const
{
    const RegKey key((std::wstring(kRootKey) + L"\\" + kOverlaySubKey).c_str());
    return std::clamp(key.readInt(L"guiScaleIndex", 1), 0, 3);
}

void WinSettings::setGuiScaleIndex(const int index)
{
    const RegKey key((std::wstring(kRootKey) + L"\\" + kOverlaySubKey).c_str());
    key.writeInt(L"guiScaleIndex", std::clamp(index, 0, 3));
}

FeatureSettings WinSettings::loadFeatures() const
{
    FeatureSettings features;
    const RegKey key((std::wstring(kRootKey) + L"\\" + kFeaturesSubKey).c_str());
    if (!key.valid())
        return features;

    features.espEnabled = key.readBool(L"espEnabled", features.espEnabled);
    features.entityEspEnabled = key.readBool(L"entityEspEnabled", features.entityEspEnabled);
    features.entityEspPlayersOnly = key.readBool(L"entityEspPlayersOnly", features.entityEspPlayersOnly);
    features.bedEspEnabled = key.readBool(L"bedEspEnabled", features.bedEspEnabled);
    features.bedAutoRefreshEnabled = key.readBool(L"bedAutoRefreshEnabled", features.bedAutoRefreshEnabled);
    features.espLabelsEnabled = key.readBool(L"labelsEnabled", features.espLabelsEnabled);
    features.hypixelPanelEnabled = key.readBool(L"hypixelPanelEnabled", features.hypixelPanelEnabled);
    features.bedThreatAlertsEnabled = key.readBool(L"bedThreatAlertsEnabled", features.bedThreatAlertsEnabled);
    features.bedDefensePanelEnabled = key.readBool(L"bedDefensePanelEnabled", features.bedDefensePanelEnabled);
    features.bedEspFilled = key.readBool(L"bedEspFilled", features.bedEspFilled);
    features.debugChatEnabled = key.readBool(L"debugChatEnabled", features.debugChatEnabled);
    features.showOwnBedDefenseInfo = key.readBool(L"showOwnBedDefenseInfo", features.showOwnBedDefenseInfo);
    features.showTeammateBoxes = key.readBool(L"showTeammateBoxes", features.showTeammateBoxes);
    features.bedDefenseHoldToShow = key.readBool(L"bedDefenseHoldToShow", features.bedDefenseHoldToShow);
    features.bedDefensePerspectiveScale = key.readBool(L"bedDefensePerspectiveScale", features.bedDefensePerspectiveScale);
    features.bedDefenseRadius = std::clamp(key.readInt(L"bedDefenseRadius", 6), 3, 10);
    features.bedThreatRadius = std::clamp(key.readInt(L"bedThreatRadius", 8), 3, 32);
    features.bedDefenseHotkey = std::clamp(key.readInt(L"bedDefenseHotkey", 0xA4), 8, 254);
    features.bedDefensePanelOpacity = std::clamp(key.readInt(L"bedDefensePanelOpacity", 78), 0, 100);

    auto color = key.readString(L"playerEspColor", features.playerEspColor);
    if (color.size() == 7 && color.front() == L'#')
        features.playerEspColor = color;
    color = key.readString(L"bedEspColor", features.bedEspColor);
    if (color.size() == 7 && color.front() == L'#')
        features.bedEspColor = color;
    color = key.readString(L"bedDefensePanelColor", features.bedDefensePanelColor);
    if (color.size() == 7 && color.front() == L'#')
        features.bedDefensePanelColor = color;

    return features;
}

void WinSettings::storeFeatures(const FeatureSettings &features) const
{
    const RegKey key((std::wstring(kRootKey) + L"\\" + kFeaturesSubKey).c_str());
    if (!key.valid())
        return;

    key.writeBool(L"espEnabled", features.espEnabled);
    key.writeBool(L"entityEspEnabled", features.entityEspEnabled);
    key.writeBool(L"entityEspPlayersOnly", features.entityEspPlayersOnly);
    key.writeBool(L"bedEspEnabled", features.bedEspEnabled);
    key.writeBool(L"bedAutoRefreshEnabled", features.bedAutoRefreshEnabled);
    key.writeBool(L"labelsEnabled", features.espLabelsEnabled);
    key.writeBool(L"hypixelPanelEnabled", features.hypixelPanelEnabled);
    key.writeBool(L"bedThreatAlertsEnabled", features.bedThreatAlertsEnabled);
    key.writeBool(L"bedDefensePanelEnabled", features.bedDefensePanelEnabled);
    key.writeBool(L"bedEspFilled", features.bedEspFilled);
    key.writeBool(L"debugChatEnabled", features.debugChatEnabled);
    key.writeBool(L"showOwnBedDefenseInfo", features.showOwnBedDefenseInfo);
    key.writeBool(L"showTeammateBoxes", features.showTeammateBoxes);
    key.writeBool(L"bedDefenseHoldToShow", features.bedDefenseHoldToShow);
    key.writeBool(L"bedDefensePerspectiveScale", features.bedDefensePerspectiveScale);
    key.writeInt(L"bedDefenseRadius", std::clamp(features.bedDefenseRadius, 3, 10));
    key.writeInt(L"bedThreatRadius", std::clamp(features.bedThreatRadius, 3, 32));
    key.writeInt(L"bedDefenseHotkey", std::clamp(features.bedDefenseHotkey, 8, 254));
    key.writeInt(L"bedDefensePanelOpacity", std::clamp(features.bedDefensePanelOpacity, 0, 100));
    key.writeString(L"playerEspColor", features.playerEspColor);
    key.writeString(L"bedEspColor", features.bedEspColor);
    key.writeString(L"bedDefensePanelColor", features.bedDefensePanelColor);
}

} // namespace cli
