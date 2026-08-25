#include "SkinProfileService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {

constexpr qint64 kMaximumProfileBytes = 256 * 1024;

bool validName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    return expression.match(name).hasMatch();
}

QByteArray boundedBody(QNetworkReply *reply)
{
    if (reply == nullptr) return {};
    QByteArray body = reply->read(kMaximumProfileBytes + 1);
    if (body.size() > kMaximumProfileBytes || reply->bytesAvailable() > 0) return {};
    return body;
}

QNetworkRequest profileRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JavaOverlayStudio/1.0"));
    request.setTransferTimeout(10'000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    return request;
}

} // namespace

SkinProfileService::SkinProfileService(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

SkinProfileService::~SkinProfileService()
{
    cancel();
}

void SkinProfileService::cancel()
{
    ++m_generation;
    if (m_reply) {
        QNetworkReply *reply = m_reply.data();
        m_reply.clear();
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_loading = false;
}

void SkinProfileService::lookup(const QString &playerName)
{
    const QString name = playerName.trimmed();
    if (name == m_requestedName && (m_loading || !m_skinUrl.isEmpty())) return;
    cancel();
    m_requestedName = name;
    m_displayName = name;
    m_skinUrl.clear();
    m_errorMessage.clear();
    if (!validName(name)) {
        emit changed();
        return;
    }
    m_loading = true;
    emit changed();
    const QUrl url(QStringLiteral(
        "https://api.minecraftservices.com/minecraft/profile/lookup/name/") +
        QString::fromLatin1(QUrl::toPercentEncoding(name)));
    const quint64 generation = m_generation;
    QNetworkReply *reply = m_network->get(profileRequest(url));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation] { finishIdentity(reply, generation); });
}

void SkinProfileService::finishIdentity(QNetworkReply *reply, const quint64 generation)
{
    if (reply == nullptr) return;
    if (generation != m_generation || m_reply.data() != reply) {
        reply->deleteLater();
        return;
    }
    const QByteArray body = boundedBody(reply);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
    m_reply.clear();
    reply->deleteLater();
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QString uuid = root.value(QStringLiteral("id")).toString().toLower();
    static const QRegularExpression uuidExpression(QStringLiteral("^[0-9a-f]{32}$"));
    if (!ok || !uuidExpression.match(uuid).hasMatch()) {
        fail(QStringLiteral("Minecraft profile could not be resolved"));
        return;
    }
    m_displayName = root.value(QStringLiteral("name")).toString(m_requestedName);
    const QUrl url(QStringLiteral("https://sessionserver.mojang.com/session/minecraft/profile/") +
                   uuid);
    QNetworkReply *profileReply = m_network->get(profileRequest(url));
    m_reply = profileReply;
    connect(profileReply, &QNetworkReply::finished, this,
            [this, profileReply, generation] { finishProfile(profileReply, generation); });
}

void SkinProfileService::finishProfile(QNetworkReply *reply, const quint64 generation)
{
    if (reply == nullptr) return;
    if (generation != m_generation || m_reply.data() != reply) {
        reply->deleteLater();
        return;
    }
    const QByteArray body = boundedBody(reply);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
    m_reply.clear();
    reply->deleteLater();
    if (!ok) {
        fail(QStringLiteral("Minecraft skin profile request failed"));
        return;
    }
    const QJsonArray properties = QJsonDocument::fromJson(body).object()
                                      .value(QStringLiteral("properties")).toArray();
    QByteArray texturesValue;
    for (const QJsonValue &value : properties) {
        const QJsonObject property = value.toObject();
        if (property.value(QStringLiteral("name")).toString() == QStringLiteral("textures")) {
            texturesValue = property.value(QStringLiteral("value")).toString().toLatin1();
            break;
        }
    }
    QUrl skin(QJsonDocument::fromJson(QByteArray::fromBase64(texturesValue)).object()
                  .value(QStringLiteral("textures")).toObject()
                  .value(QStringLiteral("SKIN")).toObject()
                  .value(QStringLiteral("url")).toString());
    if (skin.scheme() == QStringLiteral("http")) skin.setScheme(QStringLiteral("https"));
    if (!skin.isValid() || skin.scheme() != QStringLiteral("https") ||
        skin.host().compare(QStringLiteral("textures.minecraft.net"), Qt::CaseInsensitive) != 0) {
        fail(QStringLiteral("This Minecraft profile has no trusted skin texture"));
        return;
    }
    // Quick3D texture loading from a remote URL is backend-dependent and used
    // to leave the model with its purple fallback material. Download and
    // validate the tiny PNG ourselves, then expose an atomic local cache file
    // to the renderer.
    QNetworkReply *skinReply = m_network->get(profileRequest(skin));
    m_reply = skinReply;
    connect(skinReply, &QNetworkReply::finished, this,
            [this, skinReply, generation] { finishSkin(skinReply, generation); });
}

void SkinProfileService::finishSkin(QNetworkReply *reply, const quint64 generation)
{
    if (reply == nullptr) return;
    if (generation != m_generation || m_reply.data() != reply) {
        reply->deleteLater();
        return;
    }
    const QByteArray body = boundedBody(reply);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
    m_reply.clear();
    reply->deleteLater();

    QImage skin;
    if (!ok || !skin.loadFromData(body, "PNG") || skin.width() != 64 ||
        (skin.height() != 64 && skin.height() != 32)) {
        fail(QStringLiteral("Minecraft returned an invalid skin texture"));
        return;
    }

    const QString cacheDirectory = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation) + QStringLiteral("/skins");
    if (!QDir().mkpath(cacheDirectory)) {
        fail(QStringLiteral("The local skin cache could not be created"));
        return;
    }
    const QString cachePath = cacheDirectory + QLatin1Char('/') +
                              m_requestedName.toLower() + QStringLiteral(".png");
    QSaveFile output(cachePath);
    if (!output.open(QIODevice::WriteOnly) || !skin.save(&output, "PNG") ||
        !output.commit()) {
        fail(QStringLiteral("The Minecraft skin could not be cached"));
        return;
    }

    m_skinUrl = QUrl::fromLocalFile(cachePath).toString();
    m_loading = false;
    m_errorMessage.clear();
    emit changed();
}

void SkinProfileService::fail(const QString &message)
{
    m_loading = false;
    m_skinUrl.clear();
    m_errorMessage = message;
    emit changed();
}
