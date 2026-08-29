#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

// Persistent UUID-first blacklist. Network identity/skin work intentionally
// lives in the Controller, never in the injected JVM, and the Agent receives a
// compact snapshot over the already-authenticated local pipe.
class BlacklistService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(QVariantList recentPlayers READ recentPlayers NOTIFY recentPlayersChanged)
    Q_PROPERTY(QStringList reasonPresets READ reasonPresets WRITE setReasonPresets
               NOTIFY settingsChanged)
    Q_PROPERTY(bool panelEnabled READ panelEnabled WRITE setPanelEnabled
               NOTIFY settingsChanged)
    Q_PROPERTY(bool matchAlertsEnabled READ matchAlertsEnabled WRITE setMatchAlertsEnabled
               NOTIFY settingsChanged)
    Q_PROPERTY(bool allowIdOnlyNicks READ allowIdOnlyNicks WRITE setAllowIdOnlyNicks
               NOTIFY settingsChanged)
    Q_PROPERTY(bool showWithClickGui READ showWithClickGui WRITE setShowWithClickGui
               NOTIFY settingsChanged)
    Q_PROPERTY(bool collapsed READ collapsed WRITE setCollapsed NOTIFY settingsChanged)
    Q_PROPERTY(int panelOpacity READ panelOpacity WRITE setPanelOpacity
               NOTIFY settingsChanged)
    Q_PROPERTY(QString panelColor READ panelColor WRITE setPanelColor
               NOTIFY settingsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit BlacklistService(QObject *parent = nullptr);
    ~BlacklistService() override;

    [[nodiscard]] QVariantList entries() const;
    [[nodiscard]] QVariantList recentPlayers() const;
    [[nodiscard]] QStringList reasonPresets() const { return m_reasonPresets; }
    [[nodiscard]] bool panelEnabled() const noexcept { return m_panelEnabled; }
    [[nodiscard]] bool matchAlertsEnabled() const noexcept { return m_matchAlertsEnabled; }
    [[nodiscard]] bool allowIdOnlyNicks() const noexcept { return m_allowIdOnlyNicks; }
    [[nodiscard]] bool showWithClickGui() const noexcept { return m_showWithClickGui; }
    [[nodiscard]] bool collapsed() const noexcept { return m_collapsed; }
    [[nodiscard]] int panelOpacity() const noexcept { return m_panelOpacity; }
    [[nodiscard]] QString panelColor() const { return m_panelColor; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    Q_INVOKABLE void addPlayer(const QString &name, const QString &uuid,
                               const QString &reason, bool idOnlyNick = false,
                               bool warnOnEncounter = true);
    Q_INVOKABLE void removeEntry(const QString &key);
    Q_INVOKABLE void setEntryWarning(const QString &key, bool enabled);
    Q_INVOKABLE void refreshEntry(const QString &key);

public slots:
    void observePlayer(const QString &name, const QString &teamPrefix,
                       const QString &uuid);
    void synchronizeAgent();
    void handleAgentAdd(const QString &name, const QString &uuid,
                        const QString &reason, bool idOnlyNick,
                        bool warnOnEncounter);
    void handleAgentRemove(const QString &key);
    void handleAgentWarning(const QString &key, bool enabled);
    void handleAgentLayout(int x, int y, int width, int height);
    void handleAgentSettings(bool panelEnabled, bool matchAlertsEnabled,
                             bool allowIdOnlyNicks, bool showWithClickGui,
                             bool collapsed, int opacity,
                             const QString &color);
    void setReasonPresets(const QStringList &presets);
    void setPanelEnabled(bool enabled);
    void setMatchAlertsEnabled(bool enabled);
    void setAllowIdOnlyNicks(bool enabled);
    void setShowWithClickGui(bool enabled);
    void setCollapsed(bool collapsed);
    void setPanelOpacity(int opacity);
    void setPanelColor(const QString &color);

signals:
    void entriesChanged();
    void recentPlayersChanged();
    void settingsChanged();
    void lastErrorChanged();
    void commandReady(const QByteArray &command);

private:
    struct Entry final {
        QString key;
        QString uuid;
        QString currentName;
        QString observedName;
        QString reason;
        QString facePath;
        qint64 addedAt = 0;
        bool nick = false;
        bool idOnly = false;
        bool warnOnEncounter = true;
    };
    struct Recent final {
        QString name;
        QString uuid;
        QString teamPrefix;
        bool nick = false;
        qint64 lastSeen = 0;
    };

    [[nodiscard]] static QString normalizedUuid(const QString &uuid);
    [[nodiscard]] static bool validName(const QString &name);
    [[nodiscard]] static QByteArray encodeToken(const QString &value);
    [[nodiscard]] int findEntry(const QString &key) const;
    void addResolvedEntry(const QString &name, const QString &uuid,
                          const QString &reason, bool nick, bool idOnly,
                          bool warnOnEncounter);
    void resolveNameForAdd(const QString &name, const QString &reason,
                           bool idOnlyNick, bool warnOnEncounter);
    void refreshIdentityAndSkin(const QString &key);
    void requestSkinProfile(const QString &key);
    void requestSkinImage(const QString &key, const QUrl &skinUrl);
    void load();
    void save() const;
    void setError(const QString &message);
    void emitSettingsCommand();
    void emitEntryCommand(const Entry &entry);

    QNetworkAccessManager *m_network = nullptr;
    QList<QPointer<QNetworkReply>> m_replies;
    QList<Entry> m_entries;
    QList<Recent> m_recent;
    QStringList m_reasonPresets{
        QStringLiteral("Cheating"), QStringLiteral("Toxic behaviour"),
        QStringLiteral("Team griefing")};
    bool m_panelEnabled = true;
    bool m_matchAlertsEnabled = true;
    bool m_allowIdOnlyNicks = true;
    bool m_showWithClickGui = true;
    bool m_collapsed = false;
    int m_panelOpacity = 82;
    QString m_panelColor = QStringLiteral("#111218");
    int m_panelX = -1;
    int m_panelY = -1;
    int m_panelWidth = 100;
    int m_panelHeight = 100;
    QString m_lastError;
};
