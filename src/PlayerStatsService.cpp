#include "PlayerStatsService.h"
#include "ApiKeyStore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kDedupeMilliseconds = 10LL * 60LL * 1000LL;
constexpr qint64 kStatsCacheMilliseconds = 6LL * 60LL * 60LL * 1000LL;
constexpr qint64 kHypixelSpacingMilliseconds = 1100LL;
constexpr qsizetype kIdentityLimit = 64 * 1024;
constexpr qsizetype kHypixelLimit = 4 * 1024 * 1024;
constexpr qsizetype kMaximumRememberedPlayers = 512;
constexpr qsizetype kMaximumCurrentMatchPlayers = 64;

bool validName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    return expression.match(name).hasMatch();
}

bool validTeamPrefix(const QString &prefix)
{
    return prefix.size() == 2 && prefix.at(0) == QChar(0x00A7) &&
        ((prefix.at(1) >= QLatin1Char('0') && prefix.at(1) <= QLatin1Char('9')) ||
         (prefix.at(1) >= QLatin1Char('a') && prefix.at(1) <= QLatin1Char('f')));
}

qint64 integerField(const QJsonObject &object, const char *name)
{
    const QJsonValue value = object.value(QLatin1StringView(name));
    if (value.isDouble() && std::isfinite(value.toDouble()))
        return static_cast<qint64>(value.toDouble());
    if (value.isString()) {
        bool valid = false;
        const qint64 parsed = value.toString().toLongLong(&valid);
        if (valid) return parsed;
    }
    return 0;
}

int networkLevel(const double experience)
{
    if (!std::isfinite(experience) || experience <= 0.0) return 1;
    return std::max(1, static_cast<int>(std::floor(
        (std::sqrt(2.0 * experience + 30625.0) / 50.0) - 2.5)));
}

} // namespace

PlayerStatsService::PlayerStatsService(ApiKeyStore *apiKeys, QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_apiKeys(apiKeys)
{
}

PlayerStatsService::~PlayerStatsService()
{
    cancel();
}

void PlayerStatsService::enqueuePlayer(const QString &playerName,
                                       const QString &teamPrefix)
{
    if (!m_matchActive) return;
    const QString name = playerName.trimmed();
    const QString team = teamPrefix.toLower();
    if (!validName(name) || !validTeamPrefix(team)) return;

    const QString key = name.toLower();
    if (m_discoveredPlayers.size() >= kMaximumCurrentMatchPlayers &&
        !m_discoveredPlayers.contains(key)) {
        return;
    }
    m_discoveredPlayers.insert(key, Job{name, team});

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_statsCache.begin(); it != m_statsCache.end();) {
        it = it.value().expiresAtMs <= now ? m_statsCache.erase(it) : ++it;
    }
    const auto cached = m_statsCache.constFind(key);
    if (cached != m_statsCache.cend()) {
        emit statsReady(name, team, cached->stars, cached->fkdr, cached->level);
        m_deduplicateUntil.insert(key, now + kDedupeMilliseconds);
        return;
    }
    // Discovery is still remembered while no key exists. As soon as the user
    // saves one, reloadConfiguration() restarts this current-match roster.
    if (m_apiKeys == nullptr || !m_apiKeys->configured()) {
        emit statsFailed(name, QStringLiteral("Hypixel API key is not configured"));
        return;
    }

    for (auto it = m_deduplicateUntil.begin(); it != m_deduplicateUntil.end();) {
        it = it.value() <= now ? m_deduplicateUntil.erase(it) : ++it;
    }
    if (m_deduplicateUntil.value(key, 0) > now) return;
    if (m_deduplicateUntil.size() >= kMaximumRememberedPlayers)
        m_deduplicateUntil.erase(m_deduplicateUntil.begin());
    m_deduplicateUntil.insert(key, now + kDedupeMilliseconds);
    m_queue.enqueue(Job{name, team});
    startNext();
}

void PlayerStatsService::setMatchActive(const bool active)
{
    if (m_matchActive == active) return;
    m_matchActive = active;
    cancel();
    // A new match is a new query session even when some names appeared in the
    // previous match. In-match duplicates are still suppressed for ten minutes.
    m_deduplicateUntil.clear();
    m_discoveredPlayers.clear();
}

void PlayerStatsService::reloadConfiguration()
{
    cancel();
    m_deduplicateUntil.clear();
    if (!m_matchActive) return;

    // Route through enqueuePlayer again so cached values are returned even if
    // the key was removed, while uncached values wait for a usable key.
    const QHash<QString, Job> discovered = m_discoveredPlayers;
    m_discoveredPlayers.clear();
    for (auto it = discovered.cbegin(); it != discovered.cend(); ++it)
        enqueuePlayer(it->playerName, it->teamPrefix);
}

void PlayerStatsService::startNext()
{
    if (!m_matchActive || m_activeValid || m_reply || m_queue.isEmpty()) return;
    m_active = m_queue.dequeue();
    m_activeValid = true;
    ++m_generation;
    startIdentityLookup();
}

void PlayerStatsService::startIdentityLookup()
{
    const QByteArray encodedName = QUrl::toPercentEncoding(m_active.playerName);
    const QUrl url(QStringLiteral(
        "https://api.minecraftservices.com/minecraft/profile/lookup/name/") +
        QString::fromLatin1(encodedName));
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("JavaOverlayStudio/1.0"));
    request.setTransferTimeout(10'000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    const quint64 generation = m_generation;
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation] { finishIdentityLookup(reply, generation); });
}

QByteArray PlayerStatsService::takeBoundedReply(QNetworkReply *reply,
                                                 const qsizetype limit,
                                                 bool &tooLarge) const
{
    tooLarge = false;
    if (reply == nullptr) return {};
    QByteArray result = reply->read(limit + 1);
    tooLarge = result.size() > limit || reply->bytesAvailable() > 0;
    if (tooLarge) result.clear();
    return result;
}

void PlayerStatsService::finishIdentityLookup(QNetworkReply *reply,
                                               const quint64 generation)
{
    if (reply == nullptr) return;
    if (generation != m_generation || m_reply.data() != reply) {
        reply->deleteLater();
        return;
    }
    bool tooLarge = false;
    const QByteArray document = takeBoundedReply(reply, kIdentityLimit, tooLarge);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool networkOk = reply->error() == QNetworkReply::NoError;
    m_reply.clear();
    reply->deleteLater();
    if (tooLarge || !networkOk || status < 200 || status >= 300) {
        failActive(QStringLiteral("Minecraft player ID lookup failed"));
        return;
    }
    const QJsonDocument json = QJsonDocument::fromJson(document);
    const QString uuid = json.object().value(QStringLiteral("id")).toString().toLower();
    static const QRegularExpression uuidExpression(QStringLiteral("^[0-9a-f]{32}$"));
    if (!uuidExpression.match(uuid).hasMatch()) {
        failActive(QStringLiteral("Minecraft Services returned an invalid player ID"));
        return;
    }
    startHypixelLookup(uuid, generation);
}

void PlayerStatsService::startHypixelLookup(const QString &uuid,
                                             const quint64 generation)
{
    if (generation != m_generation || !m_activeValid) return;
    const QByteArray apiKey = m_apiKeys == nullptr ? QByteArray{} : m_apiKeys->apiKey();
    if (apiKey.isEmpty()) {
        failActive(QStringLiteral("HYPIXEL_API_KEY is not configured"));
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 delay = kHypixelSpacingMilliseconds - (now - m_lastHypixelRequestMs);
    if (delay > 0) {
        QTimer::singleShot(delay, this, [this, uuid, generation] {
            startHypixelLookup(uuid, generation);
        });
        return;
    }

    QUrl url(QStringLiteral("https://api.hypixel.net/v2/player"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("uuid"), uuid);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("API-Key", apiKey);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("JavaOverlayStudio/1.0 (registered Hypixel application)"));
    request.setTransferTimeout(10'000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    m_lastHypixelRequestMs = now;
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, uuid, generation] {
                finishHypixelLookup(reply, uuid, generation);
            });
}

void PlayerStatsService::finishHypixelLookup(QNetworkReply *reply,
                                              const QString &uuid,
                                              const quint64 generation)
{
    Q_UNUSED(uuid)
    if (reply == nullptr) return;
    if (generation != m_generation || m_reply.data() != reply) {
        reply->deleteLater();
        return;
    }
    bool tooLarge = false;
    const QByteArray document = takeBoundedReply(reply, kHypixelLimit, tooLarge);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool networkOk = reply->error() == QNetworkReply::NoError;
    const QString networkError = reply->errorString();
    m_reply.clear();
    reply->deleteLater();
    if (tooLarge || !networkOk || status < 200 || status >= 300) {
        failActive(status == 429 ? QStringLiteral("Hypixel rate limit reached")
                                 : QStringLiteral("Hypixel query failed: %1").arg(networkError));
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(document).object();
    const QJsonObject player = root.value(QStringLiteral("player")).toObject();
    if (!root.value(QStringLiteral("success")).toBool(false) || player.isEmpty()) {
        failActive(QStringLiteral("Hypixel has no data for this player"));
        return;
    }
    const QJsonObject bedWars = player.value(QStringLiteral("stats")).toObject()
        .value(QStringLiteral("Bedwars")).toObject();
    const QJsonObject achievements = player.value(QStringLiteral("achievements")).toObject();
    const qint64 finalKills = std::max<qint64>(0, integerField(bedWars, "final_kills_bedwars"));
    const qint64 finalDeaths = std::max<qint64>(0, integerField(bedWars, "final_deaths_bedwars"));
    const int stars = static_cast<int>(std::clamp<qint64>(
        integerField(achievements, "bedwars_level"), 0, 100000));
    const double fkdr = static_cast<double>(finalKills) /
                        static_cast<double>(std::max<qint64>(1, finalDeaths));
    const int level = networkLevel(player.value(QStringLiteral("networkExp")).toDouble(0.0));
    if (m_statsCache.size() >= 1024)
        m_statsCache.erase(m_statsCache.begin());
    m_statsCache.insert(m_active.playerName.toLower(),
                        CachedStats{stars, fkdr, level,
                                    QDateTime::currentMSecsSinceEpoch() +
                                        kStatsCacheMilliseconds});
    emit statsReady(m_active.playerName, m_active.teamPrefix, stars, fkdr, level);
    completeActive();
}

void PlayerStatsService::failActive(const QString &reason)
{
    if (m_activeValid) emit statsFailed(m_active.playerName, reason);
    completeActive();
}

void PlayerStatsService::completeActive()
{
    m_active = {};
    m_activeValid = false;
    QTimer::singleShot(0, this, &PlayerStatsService::startNext);
}

void PlayerStatsService::cancel()
{
    ++m_generation;
    if (m_reply) {
        QNetworkReply *reply = m_reply.data();
        m_reply.clear();
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_queue.clear();
    m_active = {};
    m_activeValid = false;
}
