#include "HypixelApiClient.h"
#include "ApiKeyStore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kCacheLifetimeMs = 6LL * 60LL * 60LL * 1000LL;
constexpr qint64 kMinimumRequestIntervalMs = 1000LL;
constexpr qint64 kMaximumResponseBytes = 4LL * 1024LL * 1024LL;
constexpr qsizetype kMaximumCacheEntries = 256;

int headerInteger(QNetworkReply *reply, const QByteArray &name)
{
    bool valid = false;
    const int result = reply->rawHeader(name).toInt(&valid);
    return valid ? result : -1;
}

} // namespace

HypixelApiClient::HypixelApiClient(ApiKeyStore *apiKeys, QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_apiKeys(apiKeys)
{
}

HypixelApiClient::~HypixelApiClient()
{
    cancel();
}

bool HypixelApiClient::apiConfigured() const
{
    return m_apiKeys != nullptr && m_apiKeys->configured();
}

QString HypixelApiClient::normalizeUuid(const QString &uuid)
{
    QString normalized = uuid.trimmed();
    normalized.remove(QLatin1Char('-'));
    static const QRegularExpression expression(QStringLiteral("^[0-9A-Fa-f]{32}$"));
    return expression.match(normalized).hasMatch() ? normalized.toLower() : QString{};
}

bool HypixelApiClient::isValidPlayerName(const QString &playerId)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    return expression.match(playerId).hasMatch();
}

qint64 HypixelApiClient::integerField(const QJsonObject &object, const char *name)
{
    const QJsonValue value = object.value(QLatin1StringView(name));
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::isfinite(number))
            return static_cast<qint64>(number);
    }
    if (value.isString()) {
        bool valid = false;
        const qint64 number = value.toString().toLongLong(&valid);
        if (valid)
            return number;
    }
    return 0;
}

bool HypixelApiClient::parsePlayerDocument(const QByteArray &document,
                                            const QString &uuid,
                                            BedWarsStats &stats,
                                            QString &error)
{
    QJsonParseError parseError{};
    const QJsonDocument json = QJsonDocument::fromJson(document, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        error = QStringLiteral("Hypixel returned invalid JSON: %1")
                    .arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = json.object();
    if (!root.value(QStringLiteral("success")).toBool(false)) {
        error = root.value(QStringLiteral("cause")).toString(
            QStringLiteral("Hypixel reported an unsuccessful response"));
        return false;
    }

    const QJsonValue playerValue = root.value(QStringLiteral("player"));
    if (playerValue.isNull() || !playerValue.isObject()) {
        error = QStringLiteral("No Hypixel player exists for this UUID");
        return false;
    }

    const QJsonObject player = playerValue.toObject();
    const QJsonObject bedWars = player.value(QStringLiteral("stats"))
                                     .toObject()
                                     .value(QStringLiteral("Bedwars"))
                                     .toObject();

    stats = {};
    stats.uuid = uuid;
    stats.displayName = player.value(QStringLiteral("displayname")).toString(uuid);
    stats.wins = std::max<qint64>(0, integerField(bedWars, "wins_bedwars"));
    stats.losses = std::max<qint64>(0, integerField(bedWars, "losses_bedwars"));
    stats.finalKills = std::max<qint64>(0, integerField(bedWars, "final_kills_bedwars"));
    stats.finalDeaths = std::max<qint64>(0, integerField(bedWars, "final_deaths_bedwars"));
    stats.bedsBroken = std::max<qint64>(0, integerField(bedWars, "beds_broken_bedwars"));
    stats.bedsLost = std::max<qint64>(0, integerField(bedWars, "beds_lost_bedwars"));
    stats.winRate = static_cast<double>(stats.wins) /
                    static_cast<double>(std::max<qint64>(1, stats.losses));
    stats.fkdr = static_cast<double>(stats.finalKills) /
                 static_cast<double>(std::max<qint64>(1, stats.finalDeaths));
    stats.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return true;
}

void HypixelApiClient::lookupPlayer(const QString &playerId)
{
    if (busy()) {
        // A second click must not invalidate the in-flight request. In
        // particular, keep State::Loading so the eventual reply remains the
        // sole authority that transitions this request to Ready or Error.
        setStatusMessage(QStringLiteral("A Hypixel request is already in progress"));
        return;
    }

    const QString requestedId = playerId.trimmed();
    const QString normalized = normalizeUuid(requestedId);
    if (normalized.isEmpty() && !isValidPlayerName(requestedId)) {
        setError(QStringLiteral("Enter a valid Minecraft player name"));
        return;
    }

    const QByteArray apiKey = m_apiKeys == nullptr ? QByteArray{} : m_apiKeys->apiKey();
    if (apiKey.isEmpty()) {
        setError(QStringLiteral("HYPIXEL_API_KEY is not configured for this developer-owned build"));
        emit configurationChanged();
        return;
    }

    // Hypixel's /v2/player contract requires a UUID. Keep UUIDs out of the UI:
    // resolve the in-game player name first through Minecraft Services, then
    // continue through the same bounded/cached Hypixel request path.
    if (normalized.isEmpty()) {
        const QByteArray encodedName = QUrl::toPercentEncoding(requestedId);
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

        m_errorMessage.clear();
        emit errorMessageChanged();
        setStatusMessage(QStringLiteral("Resolving Minecraft player name…"));
        setState(State::Loading);
        const quint64 generation = ++m_requestGeneration;
        m_responseBuffer.clear();
        m_responseTooLarge = false;
        QNetworkReply *reply = m_network->get(request);
        m_reply = reply;
        connect(reply, &QNetworkReply::readyRead, this, [this, reply, generation] {
            consumeReplyData(reply, generation);
        });
        connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, generation] {
            validateResponseSize(reply, generation);
        });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, requestedId, generation] {
                    finishIdentityReply(reply, requestedId, generation);
                });
        return;
    }

    startUuidLookup(normalized);
}

void HypixelApiClient::startUuidLookup(const QString &normalized)
{
    if (busy()) {
        setStatusMessage(QStringLiteral("A Hypixel request is already in progress"));
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    pruneExpiredCache(now);

    auto cached = m_cache.find(normalized);
    if (cached != m_cache.end()) {
        cached->lastAccessedAtMs = now;
        applyStats(cached->stats, QStringLiteral("Using a cached result (six-hour policy cache)"));
        return;
    }

    const QByteArray apiKey = m_apiKeys == nullptr ? QByteArray{} : m_apiKeys->apiKey();
    if (now - m_lastRequestAtMs < kMinimumRequestIntervalMs) {
        setError(QStringLiteral("Please wait one second before another API request"));
        return;
    }

    QUrl url(QStringLiteral("https://api.hypixel.net/v2/player"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("uuid"), normalized);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("API-Key", apiKey);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("JavaOverlayStudio/1.0 (registered Hypixel application)"));
    request.setTransferTimeout(10'000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);

    m_lastRequestAtMs = now;
    m_errorMessage.clear();
    emit errorMessageChanged();
    setStatusMessage(QStringLiteral("Requesting Bed Wars statistics from Hypixel…"));
    setState(State::Loading);

    const quint64 generation = ++m_requestGeneration;
    m_responseBuffer.clear();
    m_responseTooLarge = false;

    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, generation] {
        consumeReplyData(reply, generation);
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, generation] {
        validateResponseSize(reply, generation);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, normalized, generation] {
        finishReply(reply, normalized, generation);
    });
}

void HypixelApiClient::finishIdentityReply(QNetworkReply *reply,
                                           const QString &playerId,
                                           const quint64 generation)
{
    if (reply == nullptr) return;
    if (m_reply.data() != reply || generation != m_requestGeneration) {
        reply->deleteLater();
        return;
    }
    consumeReplyData(reply, generation);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray document = m_responseBuffer;
    const bool responseTooLarge = m_responseTooLarge;
    const QString networkMessage = reply->errorString();
    m_reply.clear();
    m_responseBuffer.clear();
    m_responseTooLarge = false;
    reply->deleteLater();

    if (responseTooLarge) {
        setError(QStringLiteral("Minecraft Services response exceeded the safety limit"));
        return;
    }
    if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
        setError(status == 404
            ? QStringLiteral("No Minecraft player exists with the name %1").arg(playerId)
            : QStringLiteral("Minecraft player lookup failed (%1): %2")
                  .arg(status).arg(networkMessage));
        return;
    }
    const QString uuid = QJsonDocument::fromJson(document).object()
                             .value(QStringLiteral("id")).toString().toLower();
    if (normalizeUuid(uuid).isEmpty()) {
        setError(QStringLiteral("Minecraft Services returned an invalid player ID"));
        return;
    }

    // The identity request has completed. Move back to Idle before entering
    // the regular UUID path so its busy guard cannot reject this continuation.
    setState(State::Idle);
    startUuidLookup(uuid);
}

void HypixelApiClient::consumeReplyData(QNetworkReply *reply, const quint64 generation)
{
    if (reply == nullptr || m_reply.data() != reply ||
        generation != m_requestGeneration || m_responseTooLarge) {
        return;
    }

    // Read at most one byte beyond the limit. This avoids readAll() allocating
    // an arbitrarily large response before the safety limit can be enforced.
    while (reply->bytesAvailable() > 0) {
        const qint64 remaining = kMaximumResponseBytes - m_responseBuffer.size();
        const qint64 bytesToRead = std::min<qint64>(reply->bytesAvailable(),
                                                   std::max<qint64>(1, remaining + 1));
        const QByteArray chunk = reply->read(bytesToRead);
        if (chunk.isEmpty())
            break;

        m_responseBuffer.append(chunk);
        if (m_responseBuffer.size() > kMaximumResponseBytes) {
            m_responseTooLarge = true;
            // Keep no attacker-controlled body after the limit is crossed.
            m_responseBuffer.clear();
            reply->abort();
            return;
        }
    }
}

void HypixelApiClient::validateResponseSize(QNetworkReply *reply, const quint64 generation)
{
    if (reply == nullptr || m_reply.data() != reply ||
        generation != m_requestGeneration || m_responseTooLarge) {
        return;
    }

    bool valid = false;
    const qint64 contentLength =
        reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&valid);
    if (valid && contentLength > kMaximumResponseBytes) {
        m_responseTooLarge = true;
        m_responseBuffer.clear();
        reply->abort();
    }
}

void HypixelApiClient::finishReply(QNetworkReply *reply,
                                   const QString &uuid,
                                   const quint64 generation)
{
    if (reply == nullptr)
        return;

    // A cancelled/replaced reply may still have a queued finished signal.
    // Ignore it completely so it cannot overwrite newer UI state or stats.
    if (m_reply.data() != reply || generation != m_requestGeneration) {
        reply->deleteLater();
        return;
    }

    consumeReplyData(reply, generation);

    updateRateLimit(reply);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray document = m_responseBuffer;
    const bool responseTooLarge = m_responseTooLarge;
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkMessage = reply->errorString();
    m_reply.clear();
    m_responseBuffer.clear();
    m_responseTooLarge = false;
    reply->deleteLater();

    if (responseTooLarge) {
        setError(QStringLiteral("Hypixel response exceeded the 4 MiB safety limit"));
        return;
    }
    if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (status == 429) {
            setError(QStringLiteral("Hypixel rate limit reached; retry after %1 seconds")
                         .arg(std::max(0, m_rateResetSeconds)));
        } else if (status == 403) {
            setError(QStringLiteral("Hypixel rejected the registered API key"));
        } else {
            setError(QStringLiteral("Hypixel request failed (%1): %2")
                         .arg(status)
                         .arg(networkMessage));
        }
        return;
    }

    BedWarsStats stats;
    QString parseError;
    if (!parsePlayerDocument(document, uuid, stats, parseError)) {
        setError(parseError);
        return;
    }

    insertCacheEntry(uuid, stats, QDateTime::currentMSecsSinceEpoch());
    applyStats(stats, QStringLiteral("Bed Wars statistics updated from Hypixel"));
}

void HypixelApiClient::pruneExpiredCache(const qint64 now)
{
    for (auto iterator = m_cache.begin(); iterator != m_cache.end();) {
        if (iterator->expiresAtMs <= now)
            iterator = m_cache.erase(iterator);
        else
            ++iterator;
    }
}

void HypixelApiClient::insertCacheEntry(const QString &uuid,
                                        const BedWarsStats &stats,
                                        const qint64 now)
{
    pruneExpiredCache(now);

    if (!m_cache.contains(uuid) && m_cache.size() >= kMaximumCacheEntries) {
        auto leastRecentlyUsed = m_cache.begin();
        for (auto iterator = std::next(m_cache.begin()); iterator != m_cache.end(); ++iterator) {
            if (iterator->lastAccessedAtMs < leastRecentlyUsed->lastAccessedAtMs)
                leastRecentlyUsed = iterator;
        }
        if (leastRecentlyUsed != m_cache.end())
            m_cache.erase(leastRecentlyUsed);
    }

    m_cache.insert(uuid, CacheEntry{stats, now + kCacheLifetimeMs, now});
}

void HypixelApiClient::cancel()
{
    ++m_requestGeneration;
    if (m_reply) {
        QNetworkReply *reply = m_reply.data();
        m_reply.clear();
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_responseBuffer.clear();
    m_responseTooLarge = false;
    if (m_state == State::Loading) {
        setStatusMessage(QStringLiteral("Hypixel request cancelled"));
        setState(State::Idle);
    }
}

void HypixelApiClient::reloadConfiguration()
{
    emit configurationChanged();
    setStatusMessage(apiConfigured()
        ? QStringLiteral("Hypixel API configuration detected")
        : QStringLiteral("Set HYPIXEL_API_KEY for a registered developer-owned application"));
}

void HypixelApiClient::applyStats(const BedWarsStats &stats, const QString &status)
{
    m_stats = stats;
    emit statsChanged();
    m_errorMessage.clear();
    emit errorMessageChanged();
    setStatusMessage(status);
    setState(State::Ready);
}

void HypixelApiClient::setState(const State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void HypixelApiClient::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message)
        return;
    m_statusMessage = message;
    emit statusMessageChanged();
}

void HypixelApiClient::setError(const QString &message)
{
    m_errorMessage = message;
    emit errorMessageChanged();
    setStatusMessage(QStringLiteral("Hypixel statistics unavailable"));
    setState(State::Error);
}

void HypixelApiClient::updateRateLimit(QNetworkReply *reply)
{
    const int limit = headerInteger(reply, QByteArrayLiteral("RateLimit-Limit"));
    const int remaining = headerInteger(reply, QByteArrayLiteral("RateLimit-Remaining"));
    const int reset = headerInteger(reply, QByteArrayLiteral("RateLimit-Reset"));
    if (m_rateLimit == limit && m_rateRemaining == remaining &&
        m_rateResetSeconds == reset) {
        return;
    }
    m_rateLimit = limit;
    m_rateRemaining = remaining;
    m_rateResetSeconds = reset;
    emit rateLimitChanged();
}
