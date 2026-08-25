#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QJsonObject;
class ApiKeyStore;

// Asynchronous, manually-triggered client for Hypixel's official /v2/player
// endpoint. The UI accepts a Minecraft player name; the controller resolves
// that public ID to the UUID required by Hypixel's player endpoint. The API key
// is read from HYPIXEL_API_KEY for a developer-owned registered application;
// it is never exposed to QML, logged, or persisted.
class HypixelApiClient final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Loading,
        Ready,
        Error
    };
    Q_ENUM(State)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool apiConfigured READ apiConfigured NOTIFY configurationChanged)
    Q_PROPERTY(QString queriedUuid READ queriedUuid NOTIFY statsChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY statsChanged)
    Q_PROPERTY(qint64 wins READ wins NOTIFY statsChanged)
    Q_PROPERTY(qint64 losses READ losses NOTIFY statsChanged)
    Q_PROPERTY(qint64 finalKills READ finalKills NOTIFY statsChanged)
    Q_PROPERTY(qint64 finalDeaths READ finalDeaths NOTIFY statsChanged)
    Q_PROPERTY(qint64 bedsBroken READ bedsBroken NOTIFY statsChanged)
    Q_PROPERTY(qint64 bedsLost READ bedsLost NOTIFY statsChanged)
    Q_PROPERTY(double winRate READ winRate NOTIFY statsChanged)
    Q_PROPERTY(double fkdr READ fkdr NOTIFY statsChanged)
    Q_PROPERTY(QString updatedAt READ updatedAt NOTIFY statsChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(int rateLimit READ rateLimit NOTIFY rateLimitChanged)
    Q_PROPERTY(int rateRemaining READ rateRemaining NOTIFY rateLimitChanged)
    Q_PROPERTY(int rateResetSeconds READ rateResetSeconds NOTIFY rateLimitChanged)

public:
    explicit HypixelApiClient(ApiKeyStore *apiKeys, QObject *parent = nullptr);
    ~HypixelApiClient() override;

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool busy() const noexcept { return m_state == State::Loading; }
    [[nodiscard]] bool apiConfigured() const;
    [[nodiscard]] QString queriedUuid() const { return m_stats.uuid; }
    [[nodiscard]] QString displayName() const { return m_stats.displayName; }
    [[nodiscard]] qint64 wins() const noexcept { return m_stats.wins; }
    [[nodiscard]] qint64 losses() const noexcept { return m_stats.losses; }
    [[nodiscard]] qint64 finalKills() const noexcept { return m_stats.finalKills; }
    [[nodiscard]] qint64 finalDeaths() const noexcept { return m_stats.finalDeaths; }
    [[nodiscard]] qint64 bedsBroken() const noexcept { return m_stats.bedsBroken; }
    [[nodiscard]] qint64 bedsLost() const noexcept { return m_stats.bedsLost; }
    [[nodiscard]] double winRate() const noexcept { return m_stats.winRate; }
    [[nodiscard]] double fkdr() const noexcept { return m_stats.fkdr; }
    [[nodiscard]] QString updatedAt() const { return m_stats.updatedAt; }
    [[nodiscard]] QString statusMessage() const { return m_statusMessage; }
    [[nodiscard]] QString errorMessage() const { return m_errorMessage; }
    [[nodiscard]] int rateLimit() const noexcept { return m_rateLimit; }
    [[nodiscard]] int rateRemaining() const noexcept { return m_rateRemaining; }
    [[nodiscard]] int rateResetSeconds() const noexcept { return m_rateResetSeconds; }

    // The public UI accepts the in-game player ID/name. A 32-digit UUID is
    // accepted internally as well so Agent requests resolved by older builds
    // remain compatible.
    Q_INVOKABLE void lookupPlayer(const QString &playerId);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reloadConfiguration();

signals:
    void stateChanged();
    void configurationChanged();
    void statsChanged();
    void statusMessageChanged();
    void errorMessageChanged();
    void rateLimitChanged();

private:
    struct BedWarsStats {
        QString uuid;
        QString displayName;
        qint64 wins = 0;
        qint64 losses = 0;
        qint64 finalKills = 0;
        qint64 finalDeaths = 0;
        qint64 bedsBroken = 0;
        qint64 bedsLost = 0;
        double winRate = 0.0;
        double fkdr = 0.0;
        QString updatedAt;
    };

    struct CacheEntry {
        BedWarsStats stats;
        qint64 expiresAtMs = 0;
        qint64 lastAccessedAtMs = 0;
    };

    [[nodiscard]] static QString normalizeUuid(const QString &uuid);
    [[nodiscard]] static bool isValidPlayerName(const QString &playerId);
    [[nodiscard]] static bool parsePlayerDocument(const QByteArray &document,
                                                  const QString &uuid,
                                                  BedWarsStats &stats,
                                                  QString &error);
    static qint64 integerField(const QJsonObject &object, const char *name);
    void consumeReplyData(QNetworkReply *reply, quint64 generation);
    void validateResponseSize(QNetworkReply *reply, quint64 generation);
    void startUuidLookup(const QString &uuid);
    void finishIdentityReply(QNetworkReply *reply, const QString &playerId,
                             quint64 generation);
    void finishReply(QNetworkReply *reply, const QString &uuid, quint64 generation);
    void pruneExpiredCache(qint64 now);
    void insertCacheEntry(const QString &uuid, const BedWarsStats &stats, qint64 now);
    void applyStats(const BedWarsStats &stats, const QString &status);
    void setState(State state);
    void setStatusMessage(const QString &message);
    void setError(const QString &message);
    void updateRateLimit(QNetworkReply *reply);

    QNetworkAccessManager *m_network = nullptr;
    ApiKeyStore *m_apiKeys = nullptr;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_responseBuffer;
    QHash<QString, CacheEntry> m_cache;
    BedWarsStats m_stats;
    QString m_statusMessage = QStringLiteral("Enter a Minecraft player name to query Bed Wars statistics");
    QString m_errorMessage;
    State m_state = State::Idle;
    qint64 m_lastRequestAtMs = 0;
    quint64 m_requestGeneration = 0;
    bool m_responseTooLarge = false;
    int m_rateLimit = -1;
    int m_rateRemaining = -1;
    int m_rateResetSeconds = -1;
};
