#pragma once

#include <QObject>
#include <QVariant>

// Small, non-secret UI/runtime preferences. The Hypixel API key deliberately
// remains in ApiKeyStore, where it is encrypted with Windows DPAPI; QSettings
// is only used for values that are safe to store as plain registry data.
class AppSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double navigationPaneWidth READ navigationPaneWidth
                   WRITE setNavigationPaneWidth NOTIFY navigationPaneWidthChanged)
    Q_PROPERTY(int menuHotkey READ menuHotkey
                   WRITE setMenuHotkey NOTIFY menuHotkeyChanged)
    Q_PROPERTY(int guiScaleIndex READ guiScaleIndex
                   WRITE setGuiScaleIndex NOTIFY guiScaleIndexChanged)
    Q_PROPERTY(bool processAutoRefresh READ processAutoRefresh
                   WRITE setProcessAutoRefresh NOTIFY processAutoRefreshChanged)

public:
    explicit AppSettings(QObject *parent = nullptr);

    [[nodiscard]] double navigationPaneWidth() const noexcept
    {
        return m_navigationPaneWidth;
    }
    [[nodiscard]] int menuHotkey() const noexcept { return m_menuHotkey; }
    [[nodiscard]] int guiScaleIndex() const noexcept { return m_guiScaleIndex; }
    [[nodiscard]] bool processAutoRefresh() const noexcept { return m_processAutoRefresh; }

public slots:
    void setNavigationPaneWidth(double width);
    void setMenuHotkey(int virtualKey);
    void setGuiScaleIndex(int index);
    void setProcessAutoRefresh(bool enabled);

signals:
    void navigationPaneWidthChanged();
    void menuHotkeyChanged();
    void guiScaleIndexChanged();
    void processAutoRefreshChanged();

private:
    void store(const char *key, const QVariant &value);

    double m_navigationPaneWidth = 232.0;
    int m_menuHotkey = 0xDE; // VK_OEM_7 / apostrophe
    int m_guiScaleIndex = 1; // M
    bool m_processAutoRefresh = false;
};
