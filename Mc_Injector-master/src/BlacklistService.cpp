#include "BlacklistService.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace {

constexpr qsizetype kProfileLimit = 256 * 1024;
constexpr qsizetype kSkinLimit = 2 * 1024 * 1024;
constexpr qsizetype kMaximumEntries = 128;
constexpr qsizetype kMaximumRecent = 128;

QByteArray boundedReply(QNetworkReply *reply, const qsizetype limit, bool &valid)
{
    valid = false;
    if (reply == nullptr || reply->error() != QNetworkReply::NoError) return {};
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) return {};
    QByteArray bytes = reply->read(limit + 1);
    if (bytes.size() > limit || reply->bytesAvailable() > 0) return {};
    valid = true;
    return bytes;
}

QString normalizedColor(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    const QString trimmed = value.trimmed();
    return expression.match(trimmed).hasMatch() ? trimmed.toUpper() : QString{};
}

} // namespace

BlacklistService::BlacklistService(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this))
{
    load();
    for (const Entry &entry : std::as_const(m_entries)) {
        if (!entry.uuid.isEmpty()) refreshIdentityAndSkin(entry.key);
    }
}

BlacklistService::~BlacklistService()
{
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_replies)) {
        if (reply) reply->abort();
    }
}

QString BlacklistService::normalizedUuid(const QString &uuid)
{
    QString value = uuid.trimmed().toLower();
    value.remove(QLatin1Char('-'));
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{32}$"));
    return expression.match(value).hasMatch() ? value : QString{};
}

bool BlacklistService::validName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    return expression.match(name.trimmed()).hasMatch();
}

QByteArray BlacklistService::encodeToken(const QString &value)
{
    if (value.isEmpty()) return QByteArrayLiteral("-");
    return QUrl::toPercentEncoding(value, QByteArrayLiteral("-_.~"));
}

int BlacklistService::findEntry(const QString &key) const
{
    for (int index = 0; index < m_entries.size(); ++index) {
        if (m_entries.at(index).key.compare(key, Qt::CaseInsensitive) == 0) return index;
    }
    return -1;
}

QVariantList BlacklistService::entries() const
{
    QVariantList result;
    result.reserve(m_entries.size());
    for (const Entry &entry : m_entries) {
        QVariantMap item;
        item.insert(QStringLiteral("key"), entry.key);
        item.insert(QStringLiteral("uuid"), entry.uuid);
        item.insert(QStringLiteral("name"), entry.currentName);
        item.insert(QStringLiteral("observedName"), entry.observedName);
        item.insert(QStringLiteral("reason"), entry.reason);
        item.insert(QStringLiteral("addedAt"), entry.addedAt);
        item.insert(QStringLiteral("addedText"), QDateTime::fromMSecsSinceEpoch(
            entry.addedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item.insert(QStringLiteral("nick"), entry.nick);
        item.insert(QStringLiteral("idOnly"), entry.idOnly);
        item.insert(QStringLiteral("warnOnEncounter"), entry.warnOnEncounter);
        item.insert(QStringLiteral("faceUrl"), entry.facePath.isEmpty()
            ? QString{} : QUrl::fromLocalFile(entry.facePath).toString());
        result.push_back(item);
    }
    return result;
}

QVariantList BlacklistService::recentPlayers() const
{
    QVariantList result;
    result.reserve(m_recent.size());
    for (const Recent &recent : m_recent) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), recent.name);
        item.insert(QStringLiteral("uuid"), recent.uuid);
        item.insert(QStringLiteral("teamPrefix"), recent.teamPrefix);
        item.insert(QStringLiteral("nick"), recent.nick);
        item.insert(QStringLiteral("lastSeen"), recent.lastSeen);
        result.push_back(item);
    }
    return result;
}

void BlacklistService::observePlayer(const QString &name,
                                     const QString &teamPrefix,
                                     const QString &uuid)
{
    const QString cleanName = name.trimmed();
    if (!validName(cleanName)) return;
    const QString cleanUuid = normalizedUuid(uuid);
    const QString recentKey = cleanUuid.isEmpty()
        ? QStringLiteral("n:") + cleanName.toLower()
        : QStringLiteral("u:") + cleanUuid;
    auto found = std::find_if(m_recent.begin(), m_recent.end(),
        [&](const Recent &candidate) {
            const QString key = candidate.uuid.isEmpty()
                ? QStringLiteral("n:") + candidate.name.toLower()
                : QStringLiteral("u:") + candidate.uuid;
            return key == recentKey;
        });
    Recent value{cleanName, cleanUuid, teamPrefix, cleanUuid.isEmpty(),
                 QDateTime::currentMSecsSinceEpoch()};
    if (found == m_recent.end()) {
        m_recent.prepend(value);
        while (m_recent.size() > kMaximumRecent) m_recent.removeLast();
    } else {
        *found = value;
        if (found != m_recent.begin()) m_recent.move(found - m_recent.begin(), 0);
    }
    emit recentPlayersChanged();

    const int index = findEntry(recentKey);
    if (index >= 0 && m_entries[index].currentName != cleanName) {
        m_entries[index].currentName = cleanName;
        save();
        emit entriesChanged();
        emitEntryCommand(m_entries[index]);
    }
}

void BlacklistService::addPlayer(const QString &name, const QString &uuid,
                                 const QString &reason, const bool idOnlyNick,
                                 const bool warnOnEncounter)
{
    const QString cleanName = name.trimmed();
    if (!validName(cleanName)) {
        setError(QStringLiteral("Invalid Minecraft player name."));
        return;
    }
    const QString cleanUuid = normalizedUuid(uuid);
    if (!cleanUuid.isEmpty()) {
        addResolvedEntry(cleanName, cleanUuid, reason, false, false,
                         warnOnEncounter);
        return;
    }
    if (idOnlyNick) {
        if (!m_allowIdOnlyNicks) {
            setError(QStringLiteral("ID-only nick entries are disabled."));
            return;
        }
        addResolvedEntry(cleanName, {}, reason, true, true, warnOnEncounter);
        return;
    }
    resolveNameForAdd(cleanName, reason, false, warnOnEncounter);
}

void BlacklistService::addResolvedEntry(const QString &name, const QString &uuid,
                                        const QString &reason, const bool nick,
                                        const bool idOnly,
                                        const bool warnOnEncounter)
{
    const QString key = idOnly ? QStringLiteral("n:") + name.toLower()
                               : QStringLiteral("u:") + uuid;
    const int existing = findEntry(key);
    Entry entry;
    if (existing >= 0) entry = m_entries.at(existing);
    entry.key = key;
    entry.uuid = uuid;
    entry.currentName = name;
    entry.observedName = name;
    entry.reason = reason.trimmed().left(160);
    if (entry.reason.isEmpty()) entry.reason = QStringLiteral("No reason supplied");
    if (entry.addedAt <= 0) entry.addedAt = QDateTime::currentMSecsSinceEpoch();
    entry.nick = nick;
    entry.idOnly = idOnly;
    entry.warnOnEncounter = warnOnEncounter;
    if (existing >= 0) m_entries[existing] = entry;
    else {
        if (m_entries.size() >= kMaximumEntries) {
            setError(QStringLiteral("Blacklist capacity reached (128 entries)."));
            return;
        }
        m_entries.prepend(entry);
    }
    setError({});
    save();
    emit entriesChanged();
    emitEntryCommand(entry);
    if (!uuid.isEmpty()) refreshIdentityAndSkin(key);
}

void BlacklistService::resolveNameForAdd(const QString &name,
                                         const QString &reason,
                                         const bool idOnlyNick,
                                         const bool warnOnEncounter)
{
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.minecraftservices.com/minecraft/profile/lookup/name/") +
        QString::fromLatin1(QUrl::toPercentEncoding(name))));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("JavaOverlayStudio/1.0"));
    request.setTransferTimeout(10'000);
    QNetworkReply *reply = m_network->get(request);
    m_replies.push_back(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, name, reason, idOnlyNick, warnOnEncounter] {
            m_replies.removeAll(reply);
            bool ok = false;
            const QByteArray bytes = boundedReply(reply, kProfileLimit, ok);
            reply->deleteLater();
            const QString uuid = ok ? normalizedUuid(QJsonDocument::fromJson(bytes)
                .object().value(QStringLiteral("id")).toString()) : QString{};
            if (uuid.isEmpty()) {
                if (idOnlyNick || m_allowIdOnlyNicks)
                    addResolvedEntry(name, {}, reason, true, true, warnOnEncounter);
                else
                    setError(QStringLiteral("This player could not be resolved to a UUID."));
                return;
            }
            addResolvedEntry(name, uuid, reason, false, false, warnOnEncounter);
        });
}

void BlacklistService::removeEntry(const QString &key)
{
    const int index = findEntry(key);
    if (index < 0) return;
    m_entries.removeAt(index);
    save();
    emit entriesChanged();
    emit commandReady(QByteArrayLiteral("BLACKLIST_REMOVE ") +
                      encodeToken(key) + '\n');
}

void BlacklistService::setEntryWarning(const QString &key, const bool enabled)
{
    const int index = findEntry(key);
    if (index < 0 || m_entries[index].warnOnEncounter == enabled) return;
    m_entries[index].warnOnEncounter = enabled;
    save();
    emit entriesChanged();
    emit commandReady(QByteArrayLiteral("BLACKLIST_WARNING ") +
                      encodeToken(key) + ' ' + (enabled ? "1\n" : "0\n"));
}

void BlacklistService::refreshEntry(const QString &key)
{
    if (findEntry(key) >= 0) refreshIdentityAndSkin(key);
}

void BlacklistService::refreshIdentityAndSkin(const QString &key)
{
    const int index = findEntry(key);
    if (index < 0 || m_entries[index].uuid.isEmpty()) return;
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.minecraftservices.com/minecraft/profile/lookup/") +
        m_entries[index].uuid));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("JavaOverlayStudio/1.0"));
    request.setTransferTimeout(10'000);
    QNetworkReply *reply = m_network->get(request);
    m_replies.push_back(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
        m_replies.removeAll(reply);
        bool ok = false;
        const QByteArray bytes = boundedReply(reply, kProfileLimit, ok);
        reply->deleteLater();
        const QString currentName = ok ? QJsonDocument::fromJson(bytes).object()
            .value(QStringLiteral("name")).toString() : QString{};
        const int current = findEntry(key);
        if (current < 0) return;
        if (validName(currentName)) m_entries[current].currentName = currentName;
        save();
        emit entriesChanged();
        emitEntryCommand(m_entries[current]);
        requestSkinProfile(key);
    });
}

void BlacklistService::requestSkinProfile(const QString &key)
{
    const int index = findEntry(key);
    if (index < 0 || m_entries[index].uuid.isEmpty()) return;
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://sessionserver.mojang.com/session/minecraft/profile/") +
        m_entries[index].uuid));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("JavaOverlayStudio/1.0"));
    request.setTransferTimeout(10'000);
    QNetworkReply *reply = m_network->get(request);
    m_replies.push_back(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
        m_replies.removeAll(reply);
        bool ok = false;
        const QByteArray bytes = boundedReply(reply, kProfileLimit, ok);
        reply->deleteLater();
        if (!ok || findEntry(key) < 0) return;
        const QJsonArray properties = QJsonDocument::fromJson(bytes).object()
            .value(QStringLiteral("properties")).toArray();
        QUrl skinUrl;
        for (const QJsonValue &value : properties) {
            const QJsonObject property = value.toObject();
            if (property.value(QStringLiteral("name")).toString() !=
                QStringLiteral("textures")) continue;
            const QByteArray decoded = QByteArray::fromBase64(
                property.value(QStringLiteral("value")).toString().toLatin1());
            skinUrl = QUrl(QJsonDocument::fromJson(decoded).object()
                .value(QStringLiteral("textures")).toObject()
                .value(QStringLiteral("SKIN")).toObject()
                .value(QStringLiteral("url")).toString());
            break;
        }
        if (!skinUrl.isValid() || skinUrl.scheme() != QStringLiteral("https") ||
            skinUrl.host().compare(QStringLiteral("textures.minecraft.net"),
                                   Qt::CaseInsensitive) != 0) return;
        requestSkinImage(key, skinUrl);
    });
}

void BlacklistService::requestSkinImage(const QString &key, const QUrl &skinUrl)
{
    QNetworkRequest request(skinUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("JavaOverlayStudio/1.0"));
    request.setTransferTimeout(10'000);
    QNetworkReply *reply = m_network->get(request);
    m_replies.push_back(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
        m_replies.removeAll(reply);
        bool ok = false;
        const QByteArray bytes = boundedReply(reply, kSkinLimit, ok);
        reply->deleteLater();
        QImage skin;
        if (!ok || !skin.loadFromData(bytes) || skin.width() < 64 || skin.height() < 32)
            return;
        QImage face(64, 64, QImage::Format_ARGB32_Premultiplied);
        face.fill(Qt::transparent);
        QPainter painter(&face);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(QRect(0, 0, 64, 64), skin, QRect(8, 8, 8, 8));
        if (skin.width() >= 48 && skin.height() >= 16)
            painter.drawImage(QRect(0, 0, 64, 64), skin, QRect(40, 8, 8, 8));
        painter.end();
        const int index = findEntry(key);
        if (index < 0) return;
        const QString directory = QStandardPaths::writableLocation(
            QStandardPaths::CacheLocation) + QStringLiteral("/blacklist-heads");
        if (!QDir().mkpath(directory)) return;
        const QString path = directory + QLatin1Char('/') +
            m_entries[index].uuid + QStringLiteral(".png");
        if (!face.save(path, "PNG")) return;
        m_entries[index].facePath = QDir::toNativeSeparators(path);
        save();
        emit entriesChanged();
        emitEntryCommand(m_entries[index]);
    });
}

void BlacklistService::setReasonPresets(const QStringList &presets)
{
    QStringList cleaned;
    for (const QString &preset : presets) {
        const QString value = preset.trimmed().left(80);
        if (!value.isEmpty() && !cleaned.contains(value, Qt::CaseInsensitive))
            cleaned.push_back(value);
        if (cleaned.size() >= 8) break;
    }
    if (cleaned.isEmpty() || cleaned == m_reasonPresets) return;
    m_reasonPresets = cleaned;
    save();
    emit settingsChanged();
    synchronizeAgent();
}

void BlacklistService::setPanelEnabled(const bool enabled)
{
    if (m_panelEnabled == enabled) return;
    m_panelEnabled = enabled; save(); emit settingsChanged(); emitSettingsCommand();
}

void BlacklistService::setMatchAlertsEnabled(const bool enabled)
{
    if (m_matchAlertsEnabled == enabled) return;
    m_matchAlertsEnabled = enabled; save(); emit settingsChanged(); emitSettingsCommand();
}

void BlacklistService::setAllowIdOnlyNicks(const bool enabled)
{
    if (m_allowIdOnlyNicks == enabled) return;
    m_allowIdOnlyNicks = enabled; save(); emit settingsChanged(); emitSettingsCommand();
}

void BlacklistService::setShowWithClickGui(const bool enabled)
{
    if (m_showWithClickGui == enabled) return;
    m_showWithClickGui = enabled; save(); emit settingsChanged(); emitSettingsCommand();
}

void BlacklistService::setCollapsed(const bool collapsed)
{
    if (m_collapsed == collapsed) return;
    m_collapsed = collapsed; save(); emit settingsChanged(); emitSettingsCommand();
}

void BlacklistService::setPanelOpacity(const int opacity)
{
    const int bounded = std::clamp(opacity, 0, 100);
    if (m_panelOpacity == bounded) return;
    m_panelOpacity = bounded; save(); emit settingsChanged(); emitSettingsCommand();
}

void BlacklistService::setPanelColor(const QString &color)
{
    const QString normalized = normalizedColor(color);
    if (normalized.isEmpty() || normalized == m_panelColor) return;
    m_panelColor = normalized; save(); emit settingsChanged(); emitSettingsCommand();
}

void BlacklistService::handleAgentAdd(const QString &name, const QString &uuid,
                                      const QString &reason,
                                      const bool idOnlyNick,
                                      const bool warnOnEncounter)
{
    addPlayer(name, uuid, reason, idOnlyNick, warnOnEncounter);
}

void BlacklistService::handleAgentRemove(const QString &key) { removeEntry(key); }
void BlacklistService::handleAgentWarning(const QString &key, const bool enabled)
{ setEntryWarning(key, enabled); }

void BlacklistService::handleAgentLayout(const int x, const int y,
                                         const int width, const int height)
{
    m_panelX = std::clamp(x, -1, 1000);
    m_panelY = std::clamp(y, -1, 1000);
    m_panelWidth = std::clamp(width, 60, 180);
    m_panelHeight = std::clamp(height, 60, 300);
    save();
}

void BlacklistService::handleAgentSettings(const bool panelEnabled,
                                           const bool matchAlertsEnabled,
                                           const bool allowIdOnlyNicks,
                                           const bool showWithClickGui,
                                           const bool collapsed,
                                           const int opacity,
                                           const QString &color)
{
    m_panelEnabled = panelEnabled;
    m_matchAlertsEnabled = matchAlertsEnabled;
    m_allowIdOnlyNicks = allowIdOnlyNicks;
    m_showWithClickGui = showWithClickGui;
    m_collapsed = collapsed;
    m_panelOpacity = std::clamp(opacity, 0, 100);
    const QString normalized = normalizedColor(color);
    if (!normalized.isEmpty()) m_panelColor = normalized;
    save();
    emit settingsChanged();
}

void BlacklistService::emitSettingsCommand()
{
    const quint32 rgb = m_panelColor.mid(1).toUInt(nullptr, 16);
    QByteArray line = QByteArrayLiteral("BLACKLIST_SETTINGS ") +
        QByteArray::number(m_panelEnabled ? 1 : 0) + ' ' +
        QByteArray::number(m_matchAlertsEnabled ? 1 : 0) + ' ' +
        QByteArray::number(m_allowIdOnlyNicks ? 1 : 0) + ' ' +
        QByteArray::number(m_showWithClickGui ? 1 : 0) + ' ' +
        QByteArray::number(m_collapsed ? 1 : 0) + ' ' +
        QByteArray::number(m_panelOpacity) + ' ' + QByteArray::number(rgb) + ' ' +
        QByteArray::number(m_panelX) + ' ' + QByteArray::number(m_panelY) + ' ' +
        QByteArray::number(m_panelWidth) + ' ' + QByteArray::number(m_panelHeight) + '\n';
    emit commandReady(line);
}

void BlacklistService::emitEntryCommand(const Entry &entry)
{
    QByteArray line = QByteArrayLiteral("BLACKLIST_ENTRY ") + encodeToken(entry.key) + ' ' +
        encodeToken(entry.uuid) + ' ' + encodeToken(entry.currentName) + ' ' +
        encodeToken(entry.reason) + ' ' + QByteArray::number(entry.addedAt) + ' ' +
        QByteArray::number(entry.nick ? 1 : 0) + ' ' +
        QByteArray::number(entry.idOnly ? 1 : 0) + ' ' +
        QByteArray::number(entry.warnOnEncounter ? 1 : 0) + ' ' +
        encodeToken(entry.facePath) + '\n';
    emit commandReady(line);
}

void BlacklistService::synchronizeAgent()
{
    emit commandReady(QByteArrayLiteral("BLACKLIST_RESET\n"));
    emitSettingsCommand();
    for (const QString &preset : std::as_const(m_reasonPresets))
        emit commandReady(QByteArrayLiteral("BLACKLIST_PRESET ") +
                          encodeToken(preset) + '\n');
    for (const Entry &entry : std::as_const(m_entries)) emitEntryCommand(entry);
    emit commandReady(QByteArrayLiteral("BLACKLIST_SYNC_END\n"));
}

void BlacklistService::setError(const QString &message)
{
    if (m_lastError == message) return;
    m_lastError = message;
    emit lastErrorChanged();
}

void BlacklistService::load()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("blacklist"));
    m_panelEnabled = settings.value(QStringLiteral("panelEnabled"), true).toBool();
    m_matchAlertsEnabled = settings.value(QStringLiteral("matchAlertsEnabled"), true).toBool();
    m_allowIdOnlyNicks = settings.value(QStringLiteral("allowIdOnlyNicks"), true).toBool();
    m_showWithClickGui = settings.value(QStringLiteral("showWithClickGui"), true).toBool();
    m_collapsed = settings.value(QStringLiteral("collapsed"), false).toBool();
    m_panelOpacity = std::clamp(settings.value(QStringLiteral("panelOpacity"), 82).toInt(), 0, 100);
    const QString color = normalizedColor(settings.value(
        QStringLiteral("panelColor"), QStringLiteral("#111218")).toString());
    m_panelColor = color.isEmpty() ? QStringLiteral("#111218") : color;
    m_panelX = std::clamp(settings.value(QStringLiteral("panelX"), -1).toInt(), -1, 1000);
    m_panelY = std::clamp(settings.value(QStringLiteral("panelY"), -1).toInt(), -1, 1000);
    m_panelWidth = std::clamp(settings.value(QStringLiteral("panelWidth"), 100).toInt(), 60, 180);
    m_panelHeight = std::clamp(settings.value(QStringLiteral("panelHeight"), 100).toInt(), 60, 300);
    const QStringList presets = settings.value(QStringLiteral("reasonPresets"), m_reasonPresets).toStringList();
    if (!presets.isEmpty()) m_reasonPresets = presets.mid(0, 8);
    const QByteArray json = settings.value(QStringLiteral("entries")).toByteArray();
    settings.endGroup();
    const QJsonArray array = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &value : array) {
        if (m_entries.size() >= kMaximumEntries) break;
        const QJsonObject object = value.toObject();
        Entry entry;
        entry.uuid = normalizedUuid(object.value(QStringLiteral("uuid")).toString());
        entry.currentName = object.value(QStringLiteral("currentName")).toString();
        entry.observedName = object.value(QStringLiteral("observedName")).toString();
        entry.reason = object.value(QStringLiteral("reason")).toString().left(160);
        entry.facePath = object.value(QStringLiteral("facePath")).toString();
        entry.addedAt = static_cast<qint64>(object.value(QStringLiteral("addedAt")).toDouble());
        entry.nick = object.value(QStringLiteral("nick")).toBool();
        entry.idOnly = object.value(QStringLiteral("idOnly")).toBool();
        entry.warnOnEncounter = object.value(QStringLiteral("warnOnEncounter")).toBool(true);
        if (!validName(entry.currentName)) continue;
        entry.key = entry.idOnly ? QStringLiteral("n:") + entry.currentName.toLower()
                                 : QStringLiteral("u:") + entry.uuid;
        if ((!entry.idOnly && entry.uuid.isEmpty()) || findEntry(entry.key) >= 0) continue;
        m_entries.push_back(entry);
    }
}

void BlacklistService::save() const
{
    QJsonArray array;
    for (const Entry &entry : m_entries) {
        QJsonObject object;
        object.insert(QStringLiteral("uuid"), entry.uuid);
        object.insert(QStringLiteral("currentName"), entry.currentName);
        object.insert(QStringLiteral("observedName"), entry.observedName);
        object.insert(QStringLiteral("reason"), entry.reason);
        object.insert(QStringLiteral("facePath"), entry.facePath);
        object.insert(QStringLiteral("addedAt"), static_cast<double>(entry.addedAt));
        object.insert(QStringLiteral("nick"), entry.nick);
        object.insert(QStringLiteral("idOnly"), entry.idOnly);
        object.insert(QStringLiteral("warnOnEncounter"), entry.warnOnEncounter);
        array.push_back(object);
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("blacklist"));
    settings.setValue(QStringLiteral("panelEnabled"), m_panelEnabled);
    settings.setValue(QStringLiteral("matchAlertsEnabled"), m_matchAlertsEnabled);
    settings.setValue(QStringLiteral("allowIdOnlyNicks"), m_allowIdOnlyNicks);
    settings.setValue(QStringLiteral("showWithClickGui"), m_showWithClickGui);
    settings.setValue(QStringLiteral("collapsed"), m_collapsed);
    settings.setValue(QStringLiteral("panelOpacity"), m_panelOpacity);
    settings.setValue(QStringLiteral("panelColor"), m_panelColor);
    settings.setValue(QStringLiteral("panelX"), m_panelX);
    settings.setValue(QStringLiteral("panelY"), m_panelY);
    settings.setValue(QStringLiteral("panelWidth"), m_panelWidth);
    settings.setValue(QStringLiteral("panelHeight"), m_panelHeight);
    settings.setValue(QStringLiteral("reasonPresets"), m_reasonPresets);
    settings.setValue(QStringLiteral("entries"), QJsonDocument(array).toJson(
        QJsonDocument::Compact));
    settings.endGroup();
    settings.sync();
}
