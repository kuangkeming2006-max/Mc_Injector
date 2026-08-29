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
    Q_PROPERTY(bool darkTheme READ darkTheme
                   WRITE setDarkTheme NOTIFY darkThemeChanged)
    Q_PROPERTY(int windowWidth READ windowWidth
                   WRITE setWindowWidth NOTIFY windowWidthChanged)
    Q_PROPERTY(int windowHeight READ windowHeight
                   WRITE setWindowHeight NOTIFY windowHeightChanged)

public:
    explicit AppSettings(QObject *parent = nullptr);

    [[nodiscard]] double navigationPaneWidth() const noexcept
    {
        return m_navigationPaneWidth;
    }
    [[nodiscard]] int menuHotkey() const noexcept { return m_menuHotkey; }
    [[nodiscard]] int guiScaleIndex() const noexcept { return m_guiScaleIndex; }
    [[nodiscard]] bool processAutoRefresh() const noexcept { return m_processAutoRefresh; }
    [[nodiscard]] bool darkTheme() const noexcept { return m_darkTheme; }
    [[nodiscard]] int windowWidth() const noexcept { return m_windowWidth; }
    [[nodiscard]] int windowHeight() const noexcept { return m_windowHeight; }

public slots:
    void setNavigationPaneWidth(double width);
    void setMenuHotkey(int virtualKey);
    void setGuiScaleIndex(int index);
    void setProcessAutoRefresh(bool enabled);
    void setDarkTheme(bool enabled);
    void setWindowWidth(int width);
    void setWindowHeight(int height);

signals:
    void navigationPaneWidthChanged();
    void menuHotkeyChanged();
    void guiScaleIndexChanged();
    void processAutoRefreshChanged();
    void darkThemeChanged();
    void windowWidthChanged();
    void windowHeightChanged();

private:
    void store(const char *key, const QVariant &value);

    double m_navigationPaneWidth = 232.0;
    int m_menuHotkey = 0xDE; // VK_OEM_7 / apostrophe
    int m_guiScaleIndex = 1; // M
    bool m_processAutoRefresh = false;
    bool m_darkTheme = false;
    int m_windowWidth = 1420;
    int m_windowHeight = 880;
};
