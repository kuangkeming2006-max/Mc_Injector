#include "AppSettings.h"

#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace {
constexpr auto kNavigationPaneWidth = "ui/navigationPaneWidth";
constexpr auto kMenuHotkey = "overlay/menuHotkey";
constexpr auto kGuiScaleIndex = "overlay/guiScaleIndex";
constexpr auto kProcessAutoRefresh = "scanner/autoRefresh";
}

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    m_navigationPaneWidth = std::clamp(
        settings.value(QLatin1StringView(kNavigationPaneWidth), 232.0).toDouble(),
        208.0, 320.0);
    m_menuHotkey = std::clamp(
        settings.value(QLatin1StringView(kMenuHotkey), 0xDE).toInt(), 8, 254);
    m_guiScaleIndex = std::clamp(
        settings.value(QLatin1StringView(kGuiScaleIndex), 1).toInt(), 0, 3);
    m_processAutoRefresh = settings.value(
        QLatin1StringView(kProcessAutoRefresh), false).toBool();
}

void AppSettings::store(const char *key, const QVariant &value)
{
    QSettings settings;
    settings.setValue(QLatin1StringView(key), value);
    settings.sync();
}

void AppSettings::setNavigationPaneWidth(const double width)
{
    if (!std::isfinite(width))
        return;
    const double bounded = std::clamp(width, 208.0, 320.0);
    if (qFuzzyCompare(m_navigationPaneWidth, bounded))
        return;
    m_navigationPaneWidth = bounded;
    store(kNavigationPaneWidth, bounded);
    emit navigationPaneWidthChanged();
}

void AppSettings::setMenuHotkey(const int virtualKey)
{
    if (virtualKey < 8 || virtualKey > 254 || virtualKey == m_menuHotkey)
        return;
    m_menuHotkey = virtualKey;
    store(kMenuHotkey, virtualKey);
    emit menuHotkeyChanged();
}

void AppSettings::setGuiScaleIndex(const int index)
{
    const int bounded = std::clamp(index, 0, 3);
    if (bounded == m_guiScaleIndex)
        return;
    m_guiScaleIndex = bounded;
    store(kGuiScaleIndex, bounded);
    emit guiScaleIndexChanged();
}

void AppSettings::setProcessAutoRefresh(const bool enabled)
{
    if (enabled == m_processAutoRefresh)
        return;
    m_processAutoRefresh = enabled;
    store(kProcessAutoRefresh, enabled);
    emit processAutoRefreshChanged();
}
