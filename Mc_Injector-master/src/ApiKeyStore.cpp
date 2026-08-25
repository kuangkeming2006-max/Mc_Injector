#include "ApiKeyStore.h"

#include <QSettings>

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>

namespace {

constexpr auto kSettingsKey = "hypixel/protectedApiKey";
constexpr unsigned char kEntropyBytes[]{
    0x4d, 0x63, 0x4f, 0x76, 0x65, 0x72, 0x6c, 0x61,
    0x79, 0x2d, 0x48, 0x79, 0x70, 0x69, 0x78, 0x65, 0x6c};

DATA_BLOB blobFor(const QByteArray &bytes)
{
    return DATA_BLOB{static_cast<DWORD>(bytes.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()))};
}

DATA_BLOB entropyBlob()
{
    return DATA_BLOB{static_cast<DWORD>(std::size(kEntropyBytes)),
                     const_cast<BYTE*>(kEntropyBytes)};
}

} // namespace

ApiKeyStore::ApiKeyStore(QObject *parent) : QObject(parent)
{
    load();
}

ApiKeyStore::~ApiKeyStore()
{
    replaceInMemory({});
}

void ApiKeyStore::replaceInMemory(QByteArray key)
{
    std::fill(m_key.begin(), m_key.end(), '\0');
    m_key = std::move(key);
}

QByteArray ApiKeyStore::protect(const QByteArray &plainText)
{
    if (plainText.isEmpty()) return {};
    DATA_BLOB input = blobFor(plainText);
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB output{};
    if (::CryptProtectData(&input, L"Java Overlay Studio Hypixel key", &entropy,
                           nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == FALSE) {
        return {};
    }
    QByteArray encrypted(reinterpret_cast<const char*>(output.pbData),
                         static_cast<qsizetype>(output.cbData));
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return encrypted;
}

QByteArray ApiKeyStore::unprotect(const QByteArray &cipherText)
{
    if (cipherText.isEmpty()) return {};
    DATA_BLOB input = blobFor(cipherText);
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB output{};
    if (::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &output) == FALSE) {
        return {};
    }
    QByteArray plainText(reinterpret_cast<const char*>(output.pbData),
                         static_cast<qsizetype>(output.cbData));
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return plainText;
}

void ApiKeyStore::load()
{
    const QByteArray environment = qEnvironmentVariable("HYPIXEL_API_KEY").trimmed().toUtf8();
    if (!environment.isEmpty()) {
        replaceInMemory(environment);
        m_statusMessage = QStringLiteral("Using HYPIXEL_API_KEY from the process environment");
        return;
    }
    const QByteArray encoded = QSettings{}.value(QLatin1StringView(kSettingsKey)).toByteArray();
    replaceInMemory(unprotect(QByteArray::fromBase64(encoded)));
    m_statusMessage = configured()
        ? QStringLiteral("API key is encrypted for this Windows user")
        : QStringLiteral("No Hypixel API key configured");
}

bool ApiKeyStore::saveKey(const QString &key)
{
    const QByteArray trimmed = key.trimmed().toUtf8();
    if (trimmed.size() < 16 || trimmed.size() > 256 ||
        std::any_of(trimmed.cbegin(), trimmed.cend(), [](const char value) {
            return static_cast<unsigned char>(value) <= 0x20U;
        })) {
        m_statusMessage = QStringLiteral("Enter a valid Hypixel application API key");
        emit changed();
        return false;
    }
    const QByteArray encrypted = protect(trimmed);
    if (encrypted.isEmpty()) {
        m_statusMessage = QStringLiteral("Windows could not protect the API key");
        emit changed();
        return false;
    }
    QSettings settings;
    settings.setValue(QLatin1StringView(kSettingsKey), encrypted.toBase64());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        m_statusMessage = QStringLiteral("The encrypted API key could not be saved");
        emit changed();
        return false;
    }
    replaceInMemory(trimmed);
    m_statusMessage = QStringLiteral("API key saved with Windows user encryption");
    emit changed();
    return true;
}

void ApiKeyStore::clearKey()
{
    QSettings settings;
    settings.remove(QLatin1StringView(kSettingsKey));
    settings.sync();
    replaceInMemory({});
    m_statusMessage = QStringLiteral("Stored API key removed");
    emit changed();
}
