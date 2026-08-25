#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVariantMap>
#include <QVector>

class ProcessScanner final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(quint32 selectedPid READ selectedPid NOTIFY selectedPidChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshingChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString lastRefresh READ lastRefresh NOTIFY lastRefreshChanged)

public:
    enum Role {
        PidRole = Qt::UserRole + 1,
        ExecutableNameRole,
        ExecutablePathRole,
        WindowTitleRole,
        MemoryBytesRole,
        MemoryTextRole,
        WindowHandleRole,
        HasWindowRole,
        SelectedRole
    };
    Q_ENUM(Role)

    explicit ProcessScanner(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] quint32 selectedPid() const noexcept { return m_selectedPid; }
    [[nodiscard]] bool refreshing() const noexcept { return m_refreshing; }
    [[nodiscard]] QString statusMessage() const { return m_statusMessage; }
    [[nodiscard]] QString lastRefresh() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectProcess(quint32 pid);
    Q_INVOKABLE QVariantMap processForPid(quint32 pid) const;

signals:
    void countChanged();
    void selectedPidChanged();
    void refreshingChanged();
    void statusMessageChanged();
    void lastRefreshChanged();

private:
    struct ProcessInfo {
        quint32 pid = 0;
        QString executableName;
        QString executablePath;
        QString windowTitle;
        quint64 memoryBytes = 0;
        quintptr windowHandle = 0;
    };

    [[nodiscard]] static QVector<ProcessInfo> enumerateJavaProcesses();
    [[nodiscard]] static QString formatBytes(quint64 bytes);
    [[nodiscard]] QVariantMap toVariantMap(const ProcessInfo &process) const;
    void setRefreshing(bool refreshing);
    void setStatusMessage(const QString &message);

    QVector<ProcessInfo> m_processes;
    quint32 m_selectedPid = 0;
    bool m_refreshing = false;
    QString m_statusMessage = QStringLiteral("Ready to scan");
    QDateTime m_lastRefresh;
};
