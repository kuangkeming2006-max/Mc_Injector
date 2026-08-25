#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>

class QNetworkAccessManager;
class QNetworkReply;
class ApiKeyStore;

// Sequential automatic lobby lookup pipeline. It is intentionally separate
// from HypixelApiClient's user-facing request so automatic roster traffic can
// never overwrite a manual result card or its cancellation state.
class PlayerStatsService final : public QObject
{
    Q_OBJECT

public:
    explicit PlayerStatsService(ApiKeyStore *apiKeys, QObject *parent = nullptr);
    ~PlayerStatsService() override;

public slots:
    void enqueuePlayer(const QString &playerName, const QString &teamPrefix);
    void setMatchActive(bool active);
    void reloadConfiguration();
    void cancel();

signals:
    void statsReady(const QString &playerName, const QString &teamPrefix,
                    int stars, double fkdr, int level);
    void statsFailed(const QString &playerName, const QString &reason);

private:
    struct Job {
        QString playerName;
        QString teamPrefix;
    };
    struct CachedStats {
        int stars = 0;
        double fkdr = 0.0;
        int level = 0;
        qint64 expiresAtMs = 0;
    };

    void startNext();
    void startIdentityLookup();
    void finishIdentityLookup(QNetworkReply *reply, quint64 generation);
    void startHypixelLookup(const QString &uuid, quint64 generation);
    void finishHypixelLookup(QNetworkReply *reply, const QString &uuid,
                             quint64 generation);
    void completeActive();
    void failActive(const QString &reason);
    [[nodiscard]] QByteArray takeBoundedReply(QNetworkReply *reply,
                                               qsizetype limit,
                                               bool &tooLarge) const;

    QNetworkAccessManager *m_network = nullptr;
    ApiKeyStore *m_apiKeys = nullptr;
    QPointer<QNetworkReply> m_reply;
    QQueue<Job> m_queue;
    QHash<QString, qint64> m_deduplicateUntil;
    // Retain only the current match roster so saving/replacing an API key
    // mid-match can resume automatic lookups without waiting for another
    // scoreboard change from the Agent.
    QHash<QString, Job> m_discoveredPlayers;
    QHash<QString, CachedStats> m_statsCache;
    Job m_active;
    quint64 m_generation = 0U;
    qint64 m_lastHypixelRequestMs = 0;
    bool m_activeValid = false;
    bool m_matchActive = false;
};
