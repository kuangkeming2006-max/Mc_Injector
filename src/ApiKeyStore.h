#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

// Stores the Hypixel application key with Windows DPAPI. The clear-text value
// is never exposed as a QML property; callers can only replace/clear it and ask
// whether a usable key exists. HYPIXEL_API_KEY remains a non-persistent override.
class ApiKeyStore final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool configured READ configured NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)

public:
    explicit ApiKeyStore(QObject *parent = nullptr);
    ~ApiKeyStore() override;

    [[nodiscard]] bool configured() const noexcept { return !m_key.isEmpty(); }
    [[nodiscard]] QString statusMessage() const { return m_statusMessage; }
    [[nodiscard]] QByteArray apiKey() const { return m_key; }

    Q_INVOKABLE bool saveKey(const QString &key);
    Q_INVOKABLE void clearKey();

signals:
    void changed();

private:
    void load();
    void replaceInMemory(QByteArray key);
    [[nodiscard]] static QByteArray protect(const QByteArray &plainText);
    [[nodiscard]] static QByteArray unprotect(const QByteArray &cipherText);

    QByteArray m_key;
    QString m_statusMessage;
};
