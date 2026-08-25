#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>

class QLocalSocket;

// QML keeps the historical OverlayManager name, but this class now controls
// an in-process JVM native agent. It never creates a transparent top-level
// window: the agent draws Dear ImGui into Minecraft's OpenGL frame.
class OverlayManager final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Detached,
        Validating,
        StartingIpc,
        LaunchingAttachHelper,
        WaitingForAgent,
        WaitingForOpenGL,
        Active,
        Detaching,
        Error
    };
    Q_ENUM(State)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool attached READ attached NOTIFY attachedChanged)
    Q_PROPERTY(bool rendererActive READ rendererActive NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool overlayEnabled READ overlayEnabled WRITE setOverlayEnabled
                   NOTIFY overlayEnabledChanged)
    Q_PROPERTY(bool interactive READ interactive WRITE setInteractive
                   NOTIFY interactiveChanged)
    Q_PROPERTY(bool espEnabled READ espEnabled WRITE setEspEnabled NOTIFY featureSettingsChanged)
    Q_PROPERTY(bool entityEspEnabled READ entityEspEnabled WRITE setEntityEspEnabled NOTIFY featureSettingsChanged)
    Q_PROPERTY(bool bedEspEnabled READ bedEspEnabled WRITE setBedEspEnabled NOTIFY featureSettingsChanged)
    Q_PROPERTY(bool espLabelsEnabled READ espLabelsEnabled WRITE setEspLabelsEnabled NOTIFY featureSettingsChanged)
    Q_PROPERTY(bool hypixelPanelEnabled READ hypixelPanelEnabled WRITE setHypixelPanelEnabled NOTIFY featureSettingsChanged)
    Q_PROPERTY(bool bedThreatAlertsEnabled READ bedThreatAlertsEnabled WRITE setBedThreatAlertsEnabled NOTIFY featureSettingsChanged)
    Q_PROPERTY(bool bedDefensePanelEnabled READ bedDefensePanelEnabled WRITE setBedDefensePanelEnabled NOTIFY featureSettingsChanged)
    Q_PROPERTY(int bedDefenseRadius READ bedDefenseRadius WRITE setBedDefenseRadius NOTIFY featureSettingsChanged)
    Q_PROPERTY(int menuHotkey READ menuHotkey WRITE setMenuHotkey NOTIFY menuHotkeyChanged)
    Q_PROPERTY(int guiScaleIndex READ guiScaleIndex WRITE setGuiScaleIndex
                   NOTIFY guiScaleIndexChanged)
    Q_PROPERTY(quint32 targetPid READ targetPid NOTIFY targetChanged)
    Q_PROPERTY(QString targetTitle READ targetTitle NOTIFY targetChanged)
    Q_PROPERTY(QString renderer READ renderer NOTIFY rendererChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorChanged)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY errorChanged)
    // Read-only game telemetry is populated by authenticated GAME_STATE IPC
    // messages. `gameStateAvailable` means the numeric payload is valid;
    // `gameStateStale` is driven by controller receipt time so an incorrect
    // target clock cannot make old values appear live.
    Q_PROPERTY(bool gameStateReceived READ gameStateReceived NOTIFY gameStateChanged)
    Q_PROPERTY(bool gameStateAvailable READ gameStateAvailable NOTIFY gameStateChanged)
    Q_PROPERTY(bool gameStateStale READ gameStateStale NOTIFY gameStateChanged)
    Q_PROPERTY(double playerHealth READ playerHealth NOTIFY gameStateChanged)
    Q_PROPERTY(double playerMaxHealth READ playerMaxHealth NOTIFY gameStateChanged)
    Q_PROPERTY(int playerEntityId READ playerEntityId NOTIFY gameStateChanged)
    Q_PROPERTY(double playerX READ playerX NOTIFY gameStateChanged)
    Q_PROPERTY(double playerY READ playerY NOTIFY gameStateChanged)
    Q_PROPERTY(double playerZ READ playerZ NOTIFY gameStateChanged)
    Q_PROPERTY(int loadedEntities READ loadedEntities NOTIFY gameStateChanged)
    Q_PROPERTY(int bedCount READ bedCount NOTIFY gameStateChanged)
    Q_PROPERTY(QString mappingProfile READ mappingProfile NOTIFY gameStateChanged)
    Q_PROPERTY(QString mappingState READ mappingState NOTIFY gameStateChanged)
    Q_PROPERTY(qint64 gameStateTimestamp READ gameStateTimestamp NOTIFY gameStateChanged)
    Q_PROPERTY(quint64 gameStateSequence READ gameStateSequence NOTIFY gameStateChanged)
    Q_PROPERTY(QString playerName READ playerName NOTIFY playerStatusChanged)
    Q_PROPERTY(bool matchActive READ matchActive NOTIFY matchStateChanged)

public:
    explicit OverlayManager(QObject *parent = nullptr);
    ~OverlayManager() override;

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] bool rendererActive() const noexcept { return m_state == State::Active; }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool overlayEnabled() const noexcept { return m_overlayEnabled; }
    [[nodiscard]] bool interactive() const noexcept { return m_interactive; }
    [[nodiscard]] bool espEnabled() const noexcept { return m_espEnabled; }
    [[nodiscard]] bool entityEspEnabled() const noexcept { return m_entityEspEnabled; }
    [[nodiscard]] bool bedEspEnabled() const noexcept { return m_bedEspEnabled; }
    [[nodiscard]] bool espLabelsEnabled() const noexcept { return m_espLabelsEnabled; }
    [[nodiscard]] bool hypixelPanelEnabled() const noexcept { return m_hypixelPanelEnabled; }
    [[nodiscard]] bool bedThreatAlertsEnabled() const noexcept { return m_bedThreatAlertsEnabled; }
    [[nodiscard]] bool bedDefensePanelEnabled() const noexcept { return m_bedDefensePanelEnabled; }
    [[nodiscard]] int bedDefenseRadius() const noexcept { return m_bedDefenseRadius; }
    [[nodiscard]] int menuHotkey() const noexcept { return m_menuHotkey; }
    [[nodiscard]] int guiScaleIndex() const noexcept { return m_guiScaleIndex; }
    [[nodiscard]] quint32 targetPid() const noexcept { return m_targetPid; }
    [[nodiscard]] QString targetTitle() const { return m_targetTitle; }
    [[nodiscard]] QString renderer() const { return m_renderer; }
    [[nodiscard]] QString statusMessage() const { return m_statusMessage; }
    [[nodiscard]] QString errorCode() const { return m_errorCode; }
    [[nodiscard]] QString errorDetail() const { return m_errorDetail; }
    [[nodiscard]] bool gameStateReceived() const noexcept { return m_gameStateReceived; }
    [[nodiscard]] bool gameStateAvailable() const noexcept { return m_gameStateAvailable; }
    [[nodiscard]] bool gameStateStale() const noexcept { return m_gameStateStale; }
    [[nodiscard]] double playerHealth() const noexcept { return m_playerHealth; }
    [[nodiscard]] double playerMaxHealth() const noexcept { return m_playerMaxHealth; }
    [[nodiscard]] int playerEntityId() const noexcept { return m_playerEntityId; }
    [[nodiscard]] double playerX() const noexcept { return m_playerX; }
    [[nodiscard]] double playerY() const noexcept { return m_playerY; }
    [[nodiscard]] double playerZ() const noexcept { return m_playerZ; }
    [[nodiscard]] int loadedEntities() const noexcept { return m_loadedEntities; }
    [[nodiscard]] int bedCount() const noexcept { return m_bedCount; }
    [[nodiscard]] QString mappingProfile() const { return m_mappingProfile; }
    [[nodiscard]] QString mappingState() const { return m_mappingState; }
    [[nodiscard]] qint64 gameStateTimestamp() const noexcept { return m_gameStateTimestamp; }
    [[nodiscard]] quint64 gameStateSequence() const noexcept { return m_gameStateSequence; }
    [[nodiscard]] QString playerName() const { return m_playerName; }
    [[nodiscard]] bool matchActive() const noexcept { return m_matchActive; }

    // Returns true when the asynchronous attach was started or queued behind
    // a graceful detach. The attached property becomes true only after an
    // authenticated handshake arrives from the DLL inside the target JVM.
    Q_INVOKABLE bool attachToProcess(quint32 pid);
    Q_INVOKABLE void detach();
    Q_INVOKABLE void refreshBedCache();

public slots:
    void setOverlayEnabled(bool enabled);
    void setInteractive(bool interactive);
    void setEspEnabled(bool enabled);
    void setEntityEspEnabled(bool enabled);
    void setBedEspEnabled(bool enabled);
    void setEspLabelsEnabled(bool enabled);
    void setHypixelPanelEnabled(bool enabled);
    void setBedThreatAlertsEnabled(bool enabled);
    void setBedDefensePanelEnabled(bool enabled);
    void setBedDefenseRadius(int radius);
    void setMenuHotkey(int virtualKey);
    void setGuiScaleIndex(int index);
    void publishHypixelResult(int state, const QString &uuid, const QString &displayName,
                              qint64 wins, qint64 losses, qint64 finalKills,
                              qint64 finalDeaths, qint64 bedsBroken, qint64 bedsLost,
                              double winRate, double fkdr, const QString &status);
    void publishPlayerStats(const QString &playerName, const QString &teamPrefix,
                            int stars, double fkdr, int level);

signals:
    void stateChanged();
    void attachedChanged();
    void busyChanged();
    void overlayEnabledChanged();
    void interactiveChanged();
    void featureSettingsChanged();
    void hypixelQueryRequested(const QString &playerId);
    void playerFound(const QString &playerName, const QString &teamPrefix);
    void matchStateChanged(bool active);
    void playerStatusChanged();
    void menuHotkeyChanged();
    void guiScaleIndexChanged();
    void agentSessionReady();
    void targetChanged();
    void rendererChanged();
    void statusMessageChanged();
    void errorChanged();
    void gameStateChanged();
    void targetExited(quint32 pid);

private:
    enum class LoaderKind {
        None,
        JvmAttach,
        NativeLoadLibrary
    };

    struct JavaRuntime {
        QString executable;
        QString toolsJar;
        bool modular = true;
    };

    void acceptAgentConnection();
    void readAgentMessages();
    void handleAgentDisconnected();
    void handleAttachFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleAttachError(QProcess::ProcessError error);
    void monitorTarget();
    void beginDetach();
    void completeDetach(bool timedOut);
    void finalizeDetachedState();
    void startPendingAttach();

    [[nodiscard]] QString locateAgentDll() const;
    [[nodiscard]] QString locateAttachHelper() const;
    [[nodiscard]] QString locateNativeLoader() const;
    [[nodiscard]] JavaRuntime locateJavaRuntime(const QString &targetExecutable) const;
    [[nodiscard]] QString targetExecutablePath(quint32 pid) const;
    [[nodiscard]] QString targetWindowTitle(quint32 pid) const;
    [[nodiscard]] bool targetArchitectureSupported(quint32 pid) const;
    [[nodiscard]] bool targetProcessIsRunning(quint32 pid) const;
    [[nodiscard]] bool targetHasLoadedJvm(quint32 pid) const;
    [[nodiscard]] bool targetHasLoadedOverlayAgent(quint32 pid) const;
    [[nodiscard]] bool startNativeLoaderFallback();

    // Authenticated Agent -> Controller telemetry protocol (v1):
    //
    // GAME_STATE 1 <sequence> <unix-ms> <valid-0-or-1>
    //            <hp> <max-hp> <entity-id> <x> <y> <z>
    //            <loaded-entities> <bed-count> <mapping-pct> <state-pct>
    //
    // The two text fields are percent-encoded UTF-8 tokens (`-` is empty).
    // Numeric placeholders remain mandatory when valid=0, allowing states
    // such as resolving/unsupported/no_player without a second grammar.
    // Sequence numbers must increase within an authenticated pipe session.
    void processAgentLine(const QByteArray &line);
    void sendStateSnapshot();
    void sendFeatureSnapshot();
    void sendBindSnapshot();
    void sendGuiScaleSnapshot();
    void writeAgentCommand(const QByteArray &command);
    // Closes IPC and asks a still-running helper to terminate. This function
    // is deliberately non-blocking; QProcess::finished completes any queued
    // re-attach after the old helper has actually exited.
    [[nodiscard]] bool closeSessionTransport();
    void setState(State state);
    void setStatusMessage(const QString &message);
    void setRenderer(const QString &renderer);
    void resetGameState();
    void refreshGameStateFreshness();
    void clearError();
    void setErrorState(const QString &code, const QString &detail);
    void fail(const QString &code, const QString &detail);

    QLocalServer m_server;
    QPointer<QLocalSocket> m_agentSocket;
    QProcess m_attachProcess;
    QTimer m_attachTimeout;
    // Forge 1.8.9 can report a recoverable Attach error while the asynchronous
    // Agent_OnAttach bootstrap is already running. This grace period lets that
    // first session authenticate before starting the visible DLL fallback.
    QTimer m_nativeFallbackGrace;
    QTimer m_detachTimeout;
    QTimer m_targetMonitor;
    QTimer m_gameStateFreshnessTimer;
    QElapsedTimer m_gameStateReceiptClock;
    QByteArray m_agentReadBuffer;
    QByteArray m_helperStandardOutput;
    QByteArray m_helperStandardError;
    QString m_jvmAttachFallbackReason;
    QString m_pipeToken;
    quint32 m_targetPid = 0;
    QString m_targetTitle;
    QString m_renderer;
    QString m_statusMessage = QStringLiteral("Native overlay is detached");
    QString m_errorCode;
    QString m_errorDetail;
    QString m_agentDllPath;
    QString m_agentOptions;
    State m_state = State::Detached;
    LoaderKind m_loaderKind = LoaderKind::None;
    bool m_overlayEnabled = true;
    bool m_interactive = false;
    bool m_espEnabled = true;
    bool m_entityEspEnabled = true;
    bool m_bedEspEnabled = true;
    bool m_espLabelsEnabled = true;
    bool m_hypixelPanelEnabled = true;
    bool m_bedThreatAlertsEnabled = true;
    bool m_bedDefensePanelEnabled = true;
    int m_bedDefenseRadius = 6;
    int m_menuHotkey = 0xDE; // VK_OEM_7 / apostrophe
    int m_guiScaleIndex = 1; // S/M/L/XL -> 0..3
    bool m_authenticated = false;
    bool m_closingTransport = false;
    bool m_nativeFallbackAttempted = false;
    bool m_detachTransportComplete = false;
    bool m_detachTimedOut = false;
    bool m_destroying = false;
    quint32 m_pendingAttachPid = 0;

    bool m_gameStateReceived = false;
    bool m_gameStateAvailable = false;
    bool m_gameStateStale = false;
    double m_playerHealth = 0.0;
    double m_playerMaxHealth = 0.0;
    int m_playerEntityId = 0;
    double m_playerX = 0.0;
    double m_playerY = 0.0;
    double m_playerZ = 0.0;
    int m_loadedEntities = 0;
    int m_bedCount = 0;
    QString m_mappingProfile;
    QString m_mappingState = QStringLiteral("waiting");
    qint64 m_gameStateTimestamp = 0;
    qint64 m_lastGameStateReceiptElapsedMs = 0;
    quint64 m_gameStateSequence = 0;
    QString m_playerName;
    bool m_matchActive = false;
};
