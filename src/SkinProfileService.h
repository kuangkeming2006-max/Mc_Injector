#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

class SkinProfileService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString displayName READ displayName NOTIFY changed)
    Q_PROPERTY(QString skinUrl READ skinUrl NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)

public:
    explicit SkinProfileService(QObject *parent = nullptr);
    ~SkinProfileService() override;

    [[nodiscard]] QString displayName() const { return m_displayName; }
    [[nodiscard]] QString skinUrl() const { return m_skinUrl; }
    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] QString errorMessage() const { return m_errorMessage; }

public slots:
    void lookup(const QString &playerName);

signals:
    void changed();

private:
    void finishIdentity(QNetworkReply *reply, quint64 generation);
    void finishProfile(QNetworkReply *reply, quint64 generation);
    void finishSkin(QNetworkReply *reply, quint64 generation);
    void cancel();
    void fail(const QString &message);

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QString m_requestedName;
    QString m_displayName;
    QString m_skinUrl;
    QString m_errorMessage;
    quint64 m_generation = 0;
    bool m_loading = false;
};
