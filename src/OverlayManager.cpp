#include "OverlayManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace {

constexpr int kAttachTimeoutMilliseconds = 15000;
constexpr int kDetachTimeoutMilliseconds = 2500;
constexpr qsizetype kMaximumAgentMessageBytes = 64 * 1024;
constexpr qint64 kAgentReadChunkBytes = 4096;
constexpr qint64 kGameStateStaleAfterMilliseconds = 3500;

// QML color pickers and QSettings store feature colours exclusively as
// six-digit "#RRGGBB" strings. Parsing them directly keeps this shared
// controller backend free of QtGui so the lightweight console injector can
// link QtCore/QtNetwork only.
QString normalizedRgbColor(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.size() != 7 || trimmed.at(0) != QLatin1Char('#'))
        return {};

    bool hexadecimal = true;
    for (qsizetype index = 1; index < trimmed.size(); ++index) {
        const QChar character = trimmed.at(index);
        const bool digit = character >= QLatin1Char('0') && character <= QLatin1Char('9');
        const bool upper = character >= QLatin1Char('A') && character <= QLatin1Char('F');
        const bool lower = character >= QLatin1Char('a') && character <= QLatin1Char('f');
        if (!digit && !upper && !lower) {
            hexadecimal = false;
            break;
        }
    }
    return hexadecimal ? trimmed.toUpper() : QString{};
}

quint32 rgbFromHexColor(const QString &value)
{
    const QString normalized = normalizedRgbColor(value);
    if (normalized.isEmpty())
        return 0U;

    bool valid = false;
    const quint32 rgb = normalized.mid(1).toUInt(&valid, 16);
    return valid ? rgb & 0xFFFFFFU : 0U;
}

QString decodeProtocolToken(const QByteArray &token)
{
    if (token == QByteArrayLiteral("-"))
        return {};
    return QString::fromUtf8(QByteArray::fromPercentEncoding(token));
}

QByteArray encodeProtocolToken(const QString &value)
{
    return value.isEmpty() ? QByteArrayLiteral("-")
                           : value.toUtf8().toPercentEncoding();
}

QString firstExistingFile(const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile())
            return QDir::cleanPath(info.absoluteFilePath());
    }
    return {};
}

bool modularRuntimeContainsAttach(const QDir &runtimeRoot)
{
    if (QFileInfo::exists(runtimeRoot.filePath(QStringLiteral("jmods/jdk.attach.jmod"))))
        return true;

    // A bundled jlink image has no jmods directory. Its release metadata lists
    // the modules retained in lib/modules, so an arbitrary trimmed runtime is
    // not mistaken for an Attach-capable JDK image.
    QFile releaseFile(runtimeRoot.filePath(QStringLiteral("release")));
    if (!releaseFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QByteArray release = releaseFile.readAll();
    return release.contains("jdk.attach")
        && QFileInfo::exists(runtimeRoot.filePath(QStringLiteral("lib/modules")));
}

#ifdef Q_OS_WIN

class ScopedHandle final
{
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : m_handle(handle) {}
    ~ScopedHandle()
    {
        if (m_handle && m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }

    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle && m_handle != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_handle = nullptr;
};

struct WindowSearch {
    DWORD pid = 0;
    HWND best = nullptr;
    int score = -1;
};

BOOL CALLBACK findTargetWindow(HWND window, LPARAM context)
{
    auto *search = reinterpret_cast<WindowSearch *>(context);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window))
        return TRUE;

    int score = GetWindowTextLengthW(window) > 0 ? 50 : 0;
    if (GetWindow(window, GW_OWNER) == nullptr)
        score += 25;
    if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0)
        score += 15;
    if (score > search->score) {
        search->score = score;
        search->best = window;
    }
    return TRUE;
}

QString nativeWindowTitle(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
        return {};

    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    return copied > 0 ? QString::fromWCharArray(text.data(), copied) : QString{};
}

#endif

} // namespace

OverlayManager::OverlayManager(QObject *parent)
    : QObject(parent)
{
    loadFeatureSettings();
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection,
            this, &OverlayManager::acceptAgentConnection);

    m_attachProcess.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_attachProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_helperStandardOutput += m_attachProcess.readAllStandardOutput();
    });
    connect(&m_attachProcess, &QProcess::readyReadStandardError, this, [this] {
        m_helperStandardError += m_attachProcess.readAllStandardError();
    });
    connect(&m_attachProcess, &QProcess::finished,
            this, &OverlayManager::handleAttachFinished);
    connect(&m_attachProcess, &QProcess::errorOccurred,
            this, &OverlayManager::handleAttachError);

    m_attachTimeout.setSingleShot(true);
    m_attachTimeout.setInterval(kAttachTimeoutMilliseconds);
    connect(&m_attachTimeout, &QTimer::timeout, this, [this] {
        if (!m_authenticated) {
            fail(QStringLiteral("AGENT_HANDSHAKE_TIMEOUT"),
                 QStringLiteral("No authenticated native-agent connection arrived within 15 seconds."));
        }
    });

    m_nativeFallbackGrace.setSingleShot(true);
    m_nativeFallbackGrace.setInterval(2200);
    connect(&m_nativeFallbackGrace, &QTimer::timeout, this, [this] {
        if (!m_authenticated && m_state == State::WaitingForAgent
            && m_loaderKind == LoaderKind::JvmAttach
            && !m_nativeFallbackAttempted) {
            (void) startNativeLoaderFallback();
        }
    });

    // A graceful detach is a protocol transaction. The agent first drains
    // render callbacks and restores its hooks, then replies DETACH_COMPLETE.
    // Only an unresponsive peer reaches this short force-close deadline.
    m_detachTimeout.setSingleShot(true);
    m_detachTimeout.setInterval(kDetachTimeoutMilliseconds);
    connect(&m_detachTimeout, &QTimer::timeout, this, [this] {
        if (m_state == State::Detaching && !m_detachTransportComplete)
            completeDetach(true);
    });

    m_targetMonitor.setInterval(1000);
    connect(&m_targetMonitor, &QTimer::timeout,
            this, &OverlayManager::monitorTarget);

    // Telemetry is rate-limited by the agent (currently at most 10 Hz). A
    // coarse controller timer marks the last snapshot stale without trusting
    // the JVM's clock or waking the Qt event loop for every freshness check.
    m_gameStateFreshnessTimer.setInterval(1000);
    connect(&m_gameStateFreshnessTimer, &QTimer::timeout,
            this, &OverlayManager::refreshGameStateFreshness);
    m_gameStateReceiptClock.start();
    m_gameStateFreshnessTimer.start();

    // Color pickers can update once per rendered frame. Coalesce those
    // changes into one registry write after interaction settles so the
    // controller never turns an in-game drag into synchronous I/O churn.
    m_featureSettingsStoreTimer.setSingleShot(true);
    m_featureSettingsStoreTimer.setInterval(300);
    connect(&m_featureSettingsStoreTimer, &QTimer::timeout,
            this, &OverlayManager::flushFeatureSettings);
}

OverlayManager::~OverlayManager()
{
    // QObject destruction cannot depend on another event-loop turn. Make one
    // best-effort protocol write, then release local resources without any
    // waitForFinished/waitForStarted call on the GUI thread.
    m_destroying = true;
    m_pendingAttachPid = 0;
    m_attachTimeout.stop();
    m_nativeFallbackGrace.stop();
    m_detachTimeout.stop();
    m_targetMonitor.stop();
    m_featureSettingsStoreTimer.stop();
    flushFeatureSettings();
    if (m_authenticated) {
        writeAgentCommand(QByteArrayLiteral("DETACH\n"));
        if (m_agentSocket)
            m_agentSocket->flush();
    }
    disconnect(&m_attachProcess, nullptr, this, nullptr);
    (void) closeSessionTransport();
}

bool OverlayManager::attached() const noexcept
{
    return m_state == State::WaitingForOpenGL || m_state == State::Active;
}

bool OverlayManager::busy() const noexcept
{
    return m_state == State::Validating
        || m_state == State::StartingIpc
        || m_state == State::LaunchingAttachHelper
        || m_state == State::WaitingForAgent
        || m_state == State::Detaching;
}

bool OverlayManager::attachToProcess(quint32 pid)
{
    if (pid == 0) {
        fail(QStringLiteral("INVALID_PID"),
             QStringLiteral("Select a Java process before loading the native agent."));
        return false;
    }

    // Loading another process while a session is live is a queued transition:
    // finish the authenticated DETACH transaction first, then start the new
    // helper from a later event-loop turn. This also avoids reusing QProcess
    // while an earlier helper is still emitting its terminal signals.
    if (m_state == State::Detaching) {
        m_pendingAttachPid = pid;
        setStatusMessage(QStringLiteral("Finishing the previous detach before attaching to PID %1...")
                             .arg(pid));
        return true;
    }
    if (m_state != State::Detached || m_targetPid != 0 || m_agentSocket
        || m_server.isListening()
        || m_attachProcess.state() != QProcess::NotRunning) {
        m_pendingAttachPid = pid;
        beginDetach();
        return true;
    }
    (void) closeSessionTransport();
    m_pendingAttachPid = 0;

    clearError();
    setRenderer({});
    resetGameState();
    m_pipeToken.clear();
    m_agentDllPath.clear();
    m_agentOptions.clear();
    m_loaderKind = LoaderKind::None;
    m_nativeFallbackAttempted = false;
    m_nativeFallbackGrace.stop();
    if (m_targetPid != 0 || !m_targetTitle.isEmpty()) {
        m_targetPid = 0;
        m_targetTitle.clear();
        emit targetChanged();
    }
    setState(State::Validating);

    const QString executablePath = targetExecutablePath(pid);
    if (executablePath.isEmpty()) {
        fail(QStringLiteral("PROCESS_ACCESS_DENIED"),
             QStringLiteral("Windows could not query the selected Java process. Run both programs at the same integrity level."));
        return false;
    }
    if (!targetArchitectureSupported(pid)) {
        fail(QStringLiteral("UNSUPPORTED_ARCHITECTURE"),
             QStringLiteral("McOverlayAgent is x64 and can only be loaded into an x64 JVM."));
        return false;
    }
    // A detached native-agent DLL normally remains resident in HotSpot.
    // Agent_OnAttach and McOverlay_Start are intentionally restartable, so an
    // existing module is not itself an error during a fresh IPC session.

    const QString agentDll = locateAgentDll();
    const QString attachHelper = locateAttachHelper();
    const JavaRuntime java = locateJavaRuntime(executablePath);
    if (agentDll.isEmpty()) {
        fail(QStringLiteral("AGENT_NOT_FOUND"),
             QStringLiteral("McOverlayAgent.dll was not found beside the application."));
        return false;
    }
    if (attachHelper.isEmpty()) {
        fail(QStringLiteral("ATTACH_HELPER_NOT_FOUND"),
             QStringLiteral("McOverlayAttachHelper.jar was not found beside the application."));
        return false;
    }
    if (java.executable.isEmpty()) {
        fail(QStringLiteral("ATTACH_JDK_NOT_FOUND"),
             QStringLiteral("A JDK containing the jdk.attach API is required. Configure JAVA_HOME or install a current x64 JDK."));
        return false;
    }

    m_targetPid = pid;
    m_targetTitle = targetWindowTitle(pid).trimmed();
    if (m_targetTitle.isEmpty())
        m_targetTitle = QStringLiteral("Minecraft 1.8.9 (PID %1)").arg(pid);
    emit targetChanged();

    setState(State::StartingIpc);
    m_pipeToken = QUuid::createUuid().toString(QUuid::WithoutBraces)
                      .remove(QLatin1Char('-'));
    const QString serverName = QStringLiteral("McOverlay-%1-%2")
                                   .arg(pid)
                                   .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!m_server.listen(serverName)) {
        fail(QStringLiteral("IPC_LISTEN_FAILED"), m_server.errorString());
        return false;
    }

    const QString options = QStringLiteral("pipe=%1;token=%2;protocol=1")
                                .arg(m_server.fullServerName(), m_pipeToken);
    m_agentDllPath = agentDll;
    m_agentOptions = options;
    QStringList arguments;
    if (java.modular) {
        arguments << QStringLiteral("--add-modules") << QStringLiteral("jdk.attach")
                  << QStringLiteral("-jar") << attachHelper;
    } else {
        const QString classPath = attachHelper + QLatin1Char(';') + java.toolsJar;
        arguments << QStringLiteral("-cp") << classPath
                  << QStringLiteral("com.mcoverlay.attach.AttachHelper");
    }
    arguments << QString::number(pid) << agentDll << options;

    m_helperStandardOutput.clear();
    m_helperStandardError.clear();
    m_jvmAttachFallbackReason.clear();
    m_authenticated = false;
    m_loaderKind = LoaderKind::JvmAttach;
    setState(State::LaunchingAttachHelper);
    setStatusMessage(QStringLiteral("Loading the JNI/JVMTI agent into %1...")
                         .arg(m_targetTitle));
    m_attachProcess.setProgram(java.executable);
    m_attachProcess.setArguments(arguments);
    m_attachProcess.start();
    setState(State::WaitingForAgent);
    m_attachTimeout.start();
    m_targetMonitor.start();
    return true;
}

void OverlayManager::detach()
{
    // An explicit user detach cancels a previously queued process switch.
    m_pendingAttachPid = 0;
    beginDetach();
}

void OverlayManager::beginDetach()
{
    if (m_state == State::Detaching)
        return;

    const bool hasTransport = m_agentSocket || m_server.isListening();
    const bool helperRunning = m_attachProcess.state() != QProcess::NotRunning;
    if (m_state == State::Detached && m_targetPid == 0
        && !hasTransport && !helperRunning) {
        return;
    }

    m_attachTimeout.stop();
    m_targetMonitor.stop();
    m_detachTransportComplete = false;
    m_detachTimedOut = false;
    setState(State::Detaching);
    setStatusMessage(QStringLiteral("Waiting for the native agent to restore its OpenGL hooks..."));

    // Only an authenticated peer may receive controller commands. During an
    // incomplete attach there is no runtime whose shutdown can be confirmed,
    // so close that partial session immediately and wait asynchronously for
    // the helper's QProcess::finished signal if necessary.
    if (m_authenticated && m_agentSocket
        && m_agentSocket->state() == QLocalSocket::ConnectedState) {
        writeAgentCommand(QByteArrayLiteral("DETACH\n"));
        m_agentSocket->flush();
        m_detachTimeout.start();
        return;
    }

    completeDetach(false);
}

void OverlayManager::completeDetach(const bool timedOut)
{
    if (m_detachTransportComplete)
        return;

    m_detachTimeout.stop();
    m_detachTimedOut = timedOut;
    m_detachTransportComplete = true;
    const bool helperStopped = closeSessionTransport();

    const bool hadTarget = m_targetPid != 0 || !m_targetTitle.isEmpty();
    m_targetPid = 0;
    m_targetTitle.clear();
    m_pipeToken.clear();
    setRenderer({});
    resetGameState();
    if (hadTarget)
        emit targetChanged();

    if (helperStopped)
        finalizeDetachedState();
    else
        setStatusMessage(QStringLiteral("Native session closed; waiting for the attach helper to exit..."));
}

void OverlayManager::finalizeDetachedState()
{
    if (m_destroying)
        return;

    m_detachTransportComplete = false;
    clearError();
    setState(State::Detached);
    setStatusMessage(m_detachTimedOut
        ? QStringLiteral("Native overlay detached after the agent response timed out")
        : QStringLiteral("Native overlay is detached"));
    m_detachTimedOut = false;
    startPendingAttach();
}

void OverlayManager::startPendingAttach()
{
    if (m_destroying || m_pendingAttachPid == 0
        || m_attachProcess.state() != QProcess::NotRunning) {
        return;
    }

    const quint32 pid = std::exchange(m_pendingAttachPid, 0U);
    QTimer::singleShot(0, this, [this, pid] {
        if (!m_destroying && m_state == State::Detached)
            (void) attachToProcess(pid);
    });
}

void OverlayManager::setOverlayEnabled(bool enabled)
{
    if (m_overlayEnabled == enabled && (enabled || !m_interactive))
        return;
    m_overlayEnabled = enabled;
    emit overlayEnabledChanged();
    if (!enabled && m_interactive) {
        m_interactive = false;
        emit interactiveChanged();
    }
    sendStateSnapshot();
}

void OverlayManager::setInteractive(bool interactive)
{
    if (m_interactive == interactive && (!interactive || m_overlayEnabled))
        return;
    if (interactive && !m_overlayEnabled) {
        m_overlayEnabled = true;
        emit overlayEnabledChanged();
    }
    m_interactive = interactive;
    emit interactiveChanged();
    sendStateSnapshot();
}

void OverlayManager::setEspEnabled(const bool enabled)
{
    if (m_espEnabled == enabled) return;
    m_espEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setEntityEspEnabled(const bool enabled)
{
    if (m_entityEspEnabled == enabled) return;
    m_entityEspEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setEntityEspPlayersOnly(const bool enabled)
{
    if (m_entityEspPlayersOnly == enabled) return;
    m_entityEspPlayersOnly = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedEspEnabled(const bool enabled)
{
    if (m_bedEspEnabled == enabled) return;
    m_bedEspEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedAutoRefreshEnabled(const bool enabled)
{
    if (m_bedAutoRefreshEnabled == enabled) return;
    m_bedAutoRefreshEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setEspLabelsEnabled(const bool enabled)
{
    if (m_espLabelsEnabled == enabled) return;
    m_espLabelsEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setHypixelPanelEnabled(const bool enabled)
{
    if (m_hypixelPanelEnabled == enabled) return;
    m_hypixelPanelEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedThreatAlertsEnabled(const bool enabled)
{
    if (m_bedThreatAlertsEnabled == enabled) return;
    m_bedThreatAlertsEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedDefensePanelEnabled(const bool enabled)
{
    if (m_bedDefensePanelEnabled == enabled) return;
    m_bedDefensePanelEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedEspFilled(const bool enabled)
{
    if (m_bedEspFilled == enabled) return;
    m_bedEspFilled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setDebugChatEnabled(const bool enabled)
{
    if (m_debugChatEnabled == enabled) return;
    m_debugChatEnabled = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setShowOwnBedDefenseInfo(const bool enabled)
{
    if (m_showOwnBedDefenseInfo == enabled) return;
    m_showOwnBedDefenseInfo = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setShowTeammateBoxes(const bool enabled)
{
    if (m_showTeammateBoxes == enabled) return;
    m_showTeammateBoxes = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedDefenseHoldToShow(const bool enabled)
{
    if (m_bedDefenseHoldToShow == enabled) return;
    m_bedDefenseHoldToShow = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedDefensePerspectiveScale(const bool enabled)
{
    if (m_bedDefensePerspectiveScale == enabled) return;
    m_bedDefensePerspectiveScale = enabled;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedDefenseRadius(const int radius)
{
    const int bounded = std::clamp(radius, 3, 10);
    if (m_bedDefenseRadius == bounded) return;
    m_bedDefenseRadius = bounded;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedThreatRadius(const int radius)
{
    const int bounded = std::clamp(radius, 3, 32);
    if (m_bedThreatRadius == bounded) return;
    m_bedThreatRadius = bounded;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedDefenseHotkey(const int virtualKey)
{
    if (virtualKey < 8 || virtualKey > 254 || m_bedDefenseHotkey == virtualKey) return;
    m_bedDefenseHotkey = virtualKey;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedDefensePanelOpacity(const int opacity)
{
    const int bounded = std::clamp(opacity, 0, 100);
    if (m_bedDefensePanelOpacity == bounded) return;
    m_bedDefensePanelOpacity = bounded;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setPlayerEspColor(const QString &color)
{
    const QString normalized = normalizedRgbColor(color);
    if (normalized.isEmpty() || normalized == m_playerEspColor) return;
    m_playerEspColor = normalized;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedEspColor(const QString &color)
{
    const QString normalized = normalizedRgbColor(color);
    if (normalized.isEmpty() || normalized == m_bedEspColor) return;
    m_bedEspColor = normalized;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setBedDefensePanelColor(const QString &color)
{
    const QString normalized = normalizedRgbColor(color);
    if (normalized.isEmpty() || normalized == m_bedDefensePanelColor) return;
    m_bedDefensePanelColor = normalized;
    storeFeatureSettings();
    emit featureSettingsChanged();
    sendFeatureSnapshot();
}

void OverlayManager::setMenuHotkey(const int virtualKey)
{
    if (virtualKey < 8 || virtualKey > 254 || m_menuHotkey == virtualKey) return;
    m_menuHotkey = virtualKey;
    emit menuHotkeyChanged();
    sendBindSnapshot();
}

void OverlayManager::setGuiScaleIndex(const int index)
{
    const int bounded = std::clamp(index, 0, 3);
    if (m_guiScaleIndex == bounded) return;
    m_guiScaleIndex = bounded;
    emit guiScaleIndexChanged();
    sendGuiScaleSnapshot();
}

void OverlayManager::refreshBedCache()
{
    if (!m_authenticated) return;
    writeAgentCommand(QByteArrayLiteral("BED_RESCAN\n"));
    setStatusMessage(QStringLiteral("Immediate bed-cache refresh requested"));
}

void OverlayManager::publishHypixelResult(
    const int state, const QString &uuid, const QString &displayName,
    const qint64 wins, const qint64 losses, const qint64 finalKills,
    const qint64 finalDeaths, const qint64 bedsBroken, const qint64 bedsLost,
    const double winRate, const double fkdr, const QString &status)
{
    if (!m_authenticated) return;
    QByteArray command = QByteArrayLiteral("HYPIXEL_RESULT ")
        + QByteArray::number(std::clamp(state, 0, 3)) + ' '
        + encodeProtocolToken(uuid) + ' ' + encodeProtocolToken(displayName) + ' '
        + QByteArray::number(wins) + ' ' + QByteArray::number(losses) + ' '
        + QByteArray::number(finalKills) + ' ' + QByteArray::number(finalDeaths) + ' '
        + QByteArray::number(bedsBroken) + ' ' + QByteArray::number(bedsLost) + ' '
        + QByteArray::number(std::isfinite(winRate) ? winRate : 0.0, 'g', 9) + ' '
        + QByteArray::number(std::isfinite(fkdr) ? fkdr : 0.0, 'g', 9) + ' '
        + encodeProtocolToken(status) + '\n';
    writeAgentCommand(command);
}

void OverlayManager::publishPlayerStats(const QString &playerName,
                                        const QString &teamPrefix,
                                        const int stars,
                                        const double fkdr,
                                        const int level)
{
    if (!m_authenticated) return;
    static const QRegularExpression nameExpression(
        QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    const QString team = teamPrefix.toLower();
    const bool validTeam = team.size() == 2 && team.at(0) == QChar(0x00A7) &&
        ((team.at(1) >= QLatin1Char('0') && team.at(1) <= QLatin1Char('9')) ||
         (team.at(1) >= QLatin1Char('a') && team.at(1) <= QLatin1Char('f')));
    if (!nameExpression.match(playerName).hasMatch() || !validTeam ||
        stars < 0 || level < 0 || !std::isfinite(fkdr) || fkdr < 0.0) {
        return;
    }
    writeAgentCommand(QByteArrayLiteral("STATS ") + encodeProtocolToken(playerName) + ' ' +
                      encodeProtocolToken(team) + ' ' + QByteArray::number(stars) + ' ' +
                      QByteArray::number(fkdr, 'g', 9) + ' ' +
                      QByteArray::number(level) + '\n');
}

void OverlayManager::publishPlayerStatsError(const QString &playerName,
                                             const QString &reason)
{
    if (!m_authenticated) return;
    static const QRegularExpression nameExpression(
        QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
    const QString safeReason = reason.simplified().left(96);
    if (!nameExpression.match(playerName).hasMatch() || safeReason.isEmpty()) return;
    writeAgentCommand(QByteArrayLiteral("STATS_ERROR ") +
                      encodeProtocolToken(playerName) + ' ' +
                      encodeProtocolToken(safeReason) + '\n');
}

void OverlayManager::acceptAgentConnection()
{
    while (m_server.hasPendingConnections()) {
        QLocalSocket *candidate = m_server.nextPendingConnection();
        if (!candidate)
            continue;

        if (m_agentSocket) {
            candidate->disconnectFromServer();
            candidate->deleteLater();
            continue;
        }

        m_agentSocket = candidate;
        candidate->setReadBufferSize(kMaximumAgentMessageBytes + 1);
        m_agentReadBuffer.clear();
        connect(candidate, &QLocalSocket::readyRead,
                this, &OverlayManager::readAgentMessages);
        connect(candidate, &QLocalSocket::disconnected,
                this, &OverlayManager::handleAgentDisconnected);
        readAgentMessages();
    }
}

void OverlayManager::readAgentMessages()
{
    if (!m_agentSocket)
        return;

    QLocalSocket *const source = m_agentSocket;
    while (m_agentSocket == source && source->bytesAvailable() > 0) {
        const QByteArray chunk = source->read(kAgentReadChunkBytes);
        if (chunk.isEmpty())
            break;
        m_agentReadBuffer += chunk;

        qsizetype newline = -1;
        while ((newline = m_agentReadBuffer.indexOf('\n')) >= 0) {
            // Apply the limit to each wire line, not to a readAll() batch that
            // may legitimately contain many small telemetry messages.
            if (newline > kMaximumAgentMessageBytes) {
                fail(QStringLiteral("IPC_MESSAGE_TOO_LARGE"),
                     QStringLiteral("The native agent exceeded the per-line IPC message limit."));
                return;
            }
            const QByteArray line = m_agentReadBuffer.left(newline).trimmed();
            m_agentReadBuffer.remove(0, newline + 1);
            if (!line.isEmpty())
                processAgentLine(line);
            if (m_agentSocket != source)
                return;
        }

        // No newline is buffered, so these bytes all belong to one partial
        // line. Bound it independently of how the named-pipe read was split.
        if (m_agentReadBuffer.size() > kMaximumAgentMessageBytes) {
            fail(QStringLiteral("IPC_MESSAGE_TOO_LARGE"),
                 QStringLiteral("The native agent exceeded the per-line IPC message limit."));
            return;
        }
    }
}

void OverlayManager::handleAgentDisconnected()
{
    if (m_agentSocket) {
        m_agentSocket->deleteLater();
        m_agentSocket = nullptr;
    }
    if (m_state == State::Detaching) {
        // A peer close is also a valid detach boundary. Older agents may not
        // emit DETACH_COMPLETE, but their disconnected pipe proves they can no
        // longer receive controller state or retain this IPC session.
        completeDetach(false);
    } else if (m_state != State::Detached && m_state != State::Error &&
               m_loaderKind == LoaderKind::JvmAttach &&
               !m_nativeFallbackAttempted && targetProcessIsRunning(m_targetPid)) {
        // A few Forge 1.8.9 VMs complete asynchronous Agent_OnAttach and then
        // tear down that first control-pipe worker while the launcher is
        // reporting its compatibility result. Keep the authenticated server
        // and token alive: a duplicate bootstrap can reconnect, otherwise the
        // existing bounded grace timer advances to McOverlay_Start exactly
        // once. AgentRuntime::start serializes and cleanly replaces the stopped
        // same-session runtime, so this is recovery rather than double load.
        m_authenticated = false;
        m_jvmAttachFallbackReason = QStringLiteral(
            "The JVM Attach agent disconnected before the session stabilized.");
        setState(State::WaitingForAgent);
        setStatusMessage(QStringLiteral(
            "Forge agent pipe closed during startup; waiting briefly before native recovery..."));
        m_nativeFallbackGrace.start();
    } else if (m_state != State::Detached && m_state != State::Error) {
        fail(QStringLiteral("AGENT_DISCONNECTED"),
             QStringLiteral("The native agent disconnected from its control pipe."));
    }
}

void OverlayManager::handleAttachFinished(int exitCode,
                                          QProcess::ExitStatus exitStatus)
{
    m_helperStandardOutput += m_attachProcess.readAllStandardOutput();
    m_helperStandardError += m_attachProcess.readAllStandardError();

    if (m_state == State::Detaching) {
        if (m_detachTransportComplete)
            finalizeDetachedState();
        return;
    }
    if (m_closingTransport || m_state == State::Detached
        || m_state == State::Error)
        return;
    // The helper process is only a launcher. Once HELLO has authenticated the
    // resident agent, its later exit status must not move an already-live
    // session back to WaitingForAgent or start a second bootstrap path.
    if (m_authenticated)
        return;
    // Some Forge 1.8.9 HotSpot builds surface a native Agent_OnAttach load as
    // AgentLoadException("Failed to load agent library: 0") (helper exit 12),
    // even though the same exact-path DLL is safe to start through its explicit
    // McOverlay_Start export. Initialization return failures (13) may also
    // leave the normally loaded image resident. The visible LoadLibrary path
    // handles both a new and an already-resident module and reports its own
    // structured failure if re-entry is not possible. Do not fall back for I/O,
    // security, or an unexpected helper failure.
    const bool recoverableJvmAttachFailure = exitCode == 10 || exitCode == 12 ||
                                             exitCode == 13;
    if (exitStatus == QProcess::NormalExit && recoverableJvmAttachFailure
        && m_loaderKind == LoaderKind::JvmAttach
        && !m_nativeFallbackAttempted) {
        m_jvmAttachFallbackReason =
            QString::fromLocal8Bit(m_helperStandardError).trimmed();
        // Do not immediately launch the second entry point. Agent_OnAttach now
        // queues its work and returns, and old Forge launchers can still report
        // an Attach error before that queued worker reaches the pipe. Starting
        // the native fallback at once used to create two runtimes with the same
        // token; the second stopped the first and appeared as AGENT_DISCONNECTED.
        setState(State::WaitingForAgent);
        setStatusMessage(QStringLiteral(
            "JVM Attach returned a compatibility error; waiting briefly for its asynchronous Agent handshake..."));
        m_nativeFallbackGrace.start();
        return;
    }

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        const QString detail = QString::fromLocal8Bit(m_helperStandardError).trimmed();
        const bool nativeLoader = m_loaderKind == LoaderKind::NativeLoadLibrary;
        QString resolvedDetail = detail.isEmpty()
            ? QStringLiteral("%1 exited with code %2.")
                  .arg(nativeLoader ? QStringLiteral("Native DLL loader")
                                    : QStringLiteral("JVM Attach helper"))
                  .arg(exitCode)
            : detail;
        if (nativeLoader && !m_jvmAttachFallbackReason.isEmpty()) {
            resolvedDetail = QStringLiteral("JVM Attach failed first: %1\nNative fallback failed: %2")
                                 .arg(m_jvmAttachFallbackReason, resolvedDetail);
        }
        fail(nativeLoader ? QStringLiteral("NATIVE_DLL_LOAD_FAILED")
                          : QStringLiteral("JVM_ATTACH_FAILED"),
             resolvedDetail);
        return;
    }

    if (!m_authenticated) {
        setState(State::WaitingForAgent);
        setStatusMessage(m_loaderKind == LoaderKind::NativeLoadLibrary
            ? QStringLiteral("Windows loaded the native DLL; waiting for the authenticated agent handshake...")
            : QStringLiteral("JVM accepted the DLL; waiting for the authenticated agent handshake..."));
    }
}

void OverlayManager::handleAttachError(QProcess::ProcessError error)
{
    if (m_closingTransport || error == QProcess::UnknownError
        || m_state == State::Detached
        || m_state == State::Detaching || m_state == State::Error) {
        return;
    }
    fail(m_loaderKind == LoaderKind::NativeLoadLibrary
             ? QStringLiteral("NATIVE_LOADER_PROCESS_ERROR")
             : QStringLiteral("ATTACH_PROCESS_ERROR"),
         m_attachProcess.errorString());
}

void OverlayManager::monitorTarget()
{
    if (m_targetPid != 0 && !targetProcessIsRunning(m_targetPid)) {
        const quint32 exitedPid = m_targetPid;
        m_pendingAttachPid = 0;
        m_detachTimedOut = false;
        m_detachTransportComplete = false;
        setState(State::Detaching);
        completeDetach(false);
        if (m_state == State::Detached)
            setStatusMessage(QStringLiteral("Target JVM exited; native overlay detached"));
        else
            setStatusMessage(QStringLiteral("Target JVM exited; closing the attach helper..."));
        emit targetExited(exitedPid);
    }
}

QString OverlayManager::locateAgentDll() const
{
    const QDir appDirectory(QCoreApplication::applicationDirPath());
    return firstExistingFile({
        appDirectory.filePath(QStringLiteral("agent/McOverlayAgent.dll")),
        appDirectory.filePath(QStringLiteral("McOverlayAgent.dll")),
        appDirectory.filePath(QStringLiteral("../agent/McOverlayAgent.dll"))
    });
}

QString OverlayManager::locateAttachHelper() const
{
    const QDir appDirectory(QCoreApplication::applicationDirPath());
    return firstExistingFile({
        appDirectory.filePath(QStringLiteral("tools/McOverlayAttachHelper.jar")),
        appDirectory.filePath(QStringLiteral("McOverlayAttachHelper.jar")),
        appDirectory.filePath(QStringLiteral("../attach-helper/McOverlayAttachHelper.jar"))
    });
}

QString OverlayManager::locateNativeLoader() const
{
    const QDir appDirectory(QCoreApplication::applicationDirPath());
    return firstExistingFile({
        appDirectory.filePath(QStringLiteral("tools/McOverlayNativeLoader.exe")),
        appDirectory.filePath(QStringLiteral("McOverlayNativeLoader.exe")),
        appDirectory.filePath(QStringLiteral("../native-loader/McOverlayNativeLoader.exe"))
    });
}

OverlayManager::JavaRuntime OverlayManager::locateJavaRuntime(
    const QString &targetExecutable) const
{
    const QDir appDirectory(QCoreApplication::applicationDirPath());
    QStringList candidates;
    candidates << appDirectory.filePath(QStringLiteral("runtime/bin/java.exe"));

    const QString javaHome = QProcessEnvironment::systemEnvironment()
                                 .value(QStringLiteral("JAVA_HOME"));
    if (!javaHome.isEmpty())
        candidates << QDir(javaHome).filePath(QStringLiteral("bin/java.exe"));

    const QString pathJava = QStandardPaths::findExecutable(QStringLiteral("java.exe"));
    if (!pathJava.isEmpty())
        candidates << pathJava;

    const QFileInfo targetInfo(targetExecutable);
    candidates << targetInfo.dir().filePath(QStringLiteral("java.exe"));

    for (const QString &candidate : candidates) {
        const QFileInfo javaInfo(candidate);
        if (!javaInfo.isFile())
            continue;

        // java.exe lives in <jdk>/bin. Resolve its parent explicitly instead
        // of relying on the controller's current directory or JAVA_HOME text.
        const QDir runtimeRoot(javaInfo.dir().absoluteFilePath(QStringLiteral("..")));
        const QString toolsJar = runtimeRoot.filePath(QStringLiteral("lib/tools.jar"));

        if (modularRuntimeContainsAttach(runtimeRoot))
            return {javaInfo.absoluteFilePath(), {}, true};
        if (QFileInfo::exists(toolsJar))
            return {javaInfo.absoluteFilePath(), toolsJar, false};
    }
    return {};
}

QString OverlayManager::targetExecutablePath(quint32 pid) const
{
#ifdef Q_OS_WIN
    const ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                           FALSE, pid));
    if (!process.valid())
        return {};

    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
        return {};
    return QString::fromWCharArray(path.data(), static_cast<int>(length));
#else
    Q_UNUSED(pid)
    return {};
#endif
}

QString OverlayManager::targetWindowTitle(quint32 pid) const
{
#ifdef Q_OS_WIN
    WindowSearch search{pid};
    EnumWindows(findTargetWindow, reinterpret_cast<LPARAM>(&search));
    return nativeWindowTitle(search.best);
#else
    Q_UNUSED(pid)
    return {};
#endif
}

bool OverlayManager::targetArchitectureSupported(quint32 pid) const
{
#ifdef Q_OS_WIN
    const ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                           FALSE, pid));
    if (!process.valid())
        return false;

    using IsWow64Process2Function = BOOL (WINAPI *)(HANDLE, USHORT *, USHORT *);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Function>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process.get(), &processMachine, &nativeMachine))
            return false;
        return processMachine == IMAGE_FILE_MACHINE_UNKNOWN
            && nativeMachine == IMAGE_FILE_MACHINE_AMD64;
    }

    BOOL wow64 = FALSE;
    return IsWow64Process(process.get(), &wow64) && !wow64
        && sizeof(void *) == 8;
#else
    Q_UNUSED(pid)
    return false;
#endif
}

bool OverlayManager::targetProcessIsRunning(quint32 pid) const
{
#ifdef Q_OS_WIN
    const ScopedHandle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (process.valid())
        return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;

    // A transient access-denied result must not be interpreted as process
    // death. Toolhelp still lets us distinguish a live PID from an exited one.
    const ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return true;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry))
        return true;
    do {
        if (entry.th32ProcessID == pid)
            return true;
    } while (Process32NextW(snapshot.get(), &entry));
    return false;
#else
    Q_UNUSED(pid)
    return false;
#endif
}

bool OverlayManager::targetHasLoadedJvm(quint32 pid) const
{
#ifdef Q_OS_WIN
    // Restrict the OS-loader fallback to a process that is demonstrably a live
    // x64 JVM. This avoids treating every AttachNotSupportedException as proof
    // that an arbitrary java-named process is a valid native-agent target.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const HANDLE rawSnapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (rawSnapshot == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_BAD_LENGTH)
                continue;
            return false;
        }

        const ScopedHandle snapshot(rawSnapshot);
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (!Module32FirstW(snapshot.get(), &module))
            return false;
        do {
            if (_wcsicmp(module.szModule, L"jvm.dll") == 0)
                return true;
        } while (Module32NextW(snapshot.get(), &module));
        return false;
    }
    return false;
#else
    Q_UNUSED(pid)
    return false;
#endif
}

bool OverlayManager::targetHasLoadedOverlayAgent(quint32 pid) const
{
#ifdef Q_OS_WIN
    for (int attempt = 0; attempt < 8; ++attempt) {
        const HANDLE rawSnapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (rawSnapshot == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_BAD_LENGTH)
                continue;
            return false;
        }

        const ScopedHandle snapshot(rawSnapshot);
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (!Module32FirstW(snapshot.get(), &module))
            return false;
        do {
            if (_wcsicmp(module.szModule, L"McOverlayAgent.dll") == 0)
                return true;
        } while (Module32NextW(snapshot.get(), &module));
        return false;
    }
    return false;
#else
    Q_UNUSED(pid)
    return false;
#endif
}

bool OverlayManager::startNativeLoaderFallback()
{
    m_nativeFallbackGrace.stop();
    m_nativeFallbackAttempted = true;
    if (!targetHasLoadedJvm(m_targetPid)
        || targetWindowTitle(m_targetPid).trimmed().isEmpty()) {
        fail(QStringLiteral("NATIVE_LOADER_TARGET_REJECTED"),
             QStringLiteral("JVM Attach is unavailable, but the selected process does not expose both "
                            "a loaded jvm.dll and a visible game window. The DLL was not loaded."));
        return false;
    }
    const QString nativeLoader = locateNativeLoader();
    if (nativeLoader.isEmpty()) {
        fail(QStringLiteral("NATIVE_LOADER_NOT_FOUND"),
             QStringLiteral("Runtime JVM Attach is unavailable, and "
                            "McOverlayNativeLoader.exe was not found beside the application."));
        return false;
    }
    if (m_agentDllPath.isEmpty() || m_agentOptions.isEmpty() || m_targetPid == 0) {
        fail(QStringLiteral("NATIVE_LOADER_INVALID_SESSION"),
             QStringLiteral("The native loader session was incomplete; select the process and try again."));
        return false;
    }

    m_helperStandardOutput.clear();
    m_helperStandardError.clear();
    m_loaderKind = LoaderKind::NativeLoadLibrary;
    setState(State::LaunchingAttachHelper);
    setStatusMessage(QStringLiteral("JVM Attach did not complete; retrying through the visible native DLL export..."));
    m_attachProcess.setProgram(nativeLoader);
    m_attachProcess.setArguments({QString::number(m_targetPid),
                                  m_agentDllPath,
                                  m_agentOptions});
    m_attachProcess.start();

    // Give the fallback a full handshake window instead of consuming the
    // remainder of the failed Attach attempt's timer.
    m_attachTimeout.start();
    setState(State::WaitingForAgent);
    return true;
}

void OverlayManager::processAgentLine(const QByteArray &line)
{
    const QList<QByteArray> fields = line.simplified().split(' ');
    if (fields.isEmpty())
        return;

    const QByteArray type = fields.first();
    if (type == QByteArrayLiteral("HELLO")) {
        bool pidValid = false;
        const quint32 reportedPid = fields.value(2).toUInt(&pidValid);
        const QByteArray expectedToken = m_pipeToken.toUtf8();
        if (fields.size() < 4 || fields.value(1) != QByteArrayLiteral("1")
            || !pidValid || reportedPid != m_targetPid
            || fields.value(3) != expectedToken) {
            fail(QStringLiteral("AGENT_AUTHENTICATION_FAILED"),
                 QStringLiteral("The native agent supplied an invalid protocol, PID, or token."));
            return;
        }

        m_authenticated = true;
        m_attachTimeout.stop();
        m_nativeFallbackGrace.stop();
        setState(State::WaitingForOpenGL);
        setStatusMessage(QStringLiteral("Native DLL loaded; waiting for Minecraft's first OpenGL frame..."));
        sendStateSnapshot();
        emit agentSessionReady();
        return;
    }

    if (!m_authenticated)
        return;

    if (type == QByteArrayLiteral("HOOK_READY")) {
        setRenderer(QString::fromUtf8(fields.value(1)));
        setStatusMessage(QStringLiteral("OpenGL presentation hook installed; waiting for a render context..."));
    } else if (type == QByteArrayLiteral("RENDERER_READY")) {
        setRenderer(QString::fromUtf8(fields.value(1)));
        setState(State::Active);
        setStatusMessage(QStringLiteral("Native %1 overlay is active inside Minecraft")
                             .arg(m_renderer.isEmpty() ? QStringLiteral("OpenGL") : m_renderer));
    } else if (type == QByteArrayLiteral("STATE_CHANGED")) {
        if (fields.size() != 3
            || (fields.at(1) != QByteArrayLiteral("0")
                && fields.at(1) != QByteArrayLiteral("1"))
            || (fields.at(2) != QByteArrayLiteral("0")
                && fields.at(2) != QByteArrayLiteral("1"))) {
            fail(QStringLiteral("AGENT_PROTOCOL_ERROR"),
                 QStringLiteral("The native agent sent an invalid STATE_CHANGED message."));
            return;
        }
        const bool enabled = fields.at(1) == QByteArrayLiteral("1");
        const bool interactive = fields.at(2) == QByteArrayLiteral("1");
        if (m_overlayEnabled != enabled) {
            m_overlayEnabled = enabled;
            emit overlayEnabledChanged();
        }
        if (m_interactive != interactive) {
            m_interactive = interactive;
            emit interactiveChanged();
        }
    } else if (type == QByteArrayLiteral("FEATURE_STATE_CHANGED")) {
        if (fields.size() != 23) return;
        std::array<bool, 15U> values{};
        for (int index = 0; index < 15; ++index) {
            const QByteArray token = fields.at(index + 1);
            if (token != QByteArrayLiteral("0") && token != QByteArrayLiteral("1")) return;
            values[static_cast<std::size_t>(index)] = token == QByteArrayLiteral("1");
        }
        bool defenseRadiusOk = false;
        bool threatRadiusOk = false;
        bool bedHotkeyOk = false;
        bool panelOpacityOk = false;
        bool playerColorOk = false;
        bool bedColorOk = false;
        bool panelColorOk = false;
        const int defenseRadius = fields.at(16).toInt(&defenseRadiusOk);
        const int threatRadius = fields.at(17).toInt(&threatRadiusOk);
        const int bedHotkey = fields.at(18).toInt(&bedHotkeyOk);
        const int panelOpacity = fields.at(19).toInt(&panelOpacityOk);
        const quint32 playerColor = fields.at(20).toUInt(&playerColorOk);
        const quint32 bedColor = fields.at(21).toUInt(&bedColorOk);
        const quint32 panelColor = fields.at(22).toUInt(&panelColorOk);
        if (!defenseRadiusOk || defenseRadius < 3 || defenseRadius > 10 ||
            !threatRadiusOk || threatRadius < 3 || threatRadius > 32 ||
            !bedHotkeyOk || bedHotkey < 8 || bedHotkey > 254 ||
            !panelOpacityOk || panelOpacity < 0 || panelOpacity > 100 ||
            !playerColorOk || playerColor > 0xFFFFFFU ||
            !bedColorOk || bedColor > 0xFFFFFFU ||
            !panelColorOk || panelColor > 0xFFFFFFU) return;
        const QString playerColorName = QStringLiteral("#%1")
            .arg(playerColor, 6, 16, QLatin1Char('0')).toUpper();
        const QString bedColorName = QStringLiteral("#%1")
            .arg(bedColor, 6, 16, QLatin1Char('0')).toUpper();
        const QString panelColorName = QStringLiteral("#%1")
            .arg(panelColor, 6, 16, QLatin1Char('0')).toUpper();
        const bool changed = m_espEnabled != values[0] ||
            m_entityEspEnabled != values[1] || m_bedEspEnabled != values[2] ||
            m_espLabelsEnabled != values[3] || m_hypixelPanelEnabled != values[4] ||
            m_bedThreatAlertsEnabled != values[5] ||
            m_bedDefensePanelEnabled != values[6] ||
            m_entityEspPlayersOnly != values[7] ||
            m_bedAutoRefreshEnabled != values[8] || m_bedEspFilled != values[9] ||
            m_debugChatEnabled != values[10] || m_showOwnBedDefenseInfo != values[11] ||
            m_showTeammateBoxes != values[12] ||
            m_bedDefenseHoldToShow != values[13] ||
            m_bedDefensePerspectiveScale != values[14] ||
            m_bedDefenseRadius != defenseRadius || m_bedThreatRadius != threatRadius ||
            m_bedDefenseHotkey != bedHotkey ||
            m_bedDefensePanelOpacity != panelOpacity ||
            m_playerEspColor != playerColorName || m_bedEspColor != bedColorName ||
            m_bedDefensePanelColor != panelColorName;
        m_espEnabled = values[0];
        m_entityEspEnabled = values[1];
        m_bedEspEnabled = values[2];
        m_espLabelsEnabled = values[3];
        m_hypixelPanelEnabled = values[4];
        m_bedThreatAlertsEnabled = values[5];
        m_bedDefensePanelEnabled = values[6];
        m_entityEspPlayersOnly = values[7];
        m_bedAutoRefreshEnabled = values[8];
        m_bedEspFilled = values[9];
        m_debugChatEnabled = values[10];
        m_showOwnBedDefenseInfo = values[11];
        m_showTeammateBoxes = values[12];
        m_bedDefenseHoldToShow = values[13];
        m_bedDefensePerspectiveScale = values[14];
        m_bedDefenseRadius = defenseRadius;
        m_bedThreatRadius = threatRadius;
        m_bedDefenseHotkey = bedHotkey;
        m_bedDefensePanelOpacity = panelOpacity;
        m_playerEspColor = playerColorName;
        m_bedEspColor = bedColorName;
        m_bedDefensePanelColor = panelColorName;
        if (changed) {
            storeFeatureSettings();
            emit featureSettingsChanged();
        }
    } else if (type == QByteArrayLiteral("BIND_CHANGED")) {
        bool valid = false;
        const int virtualKey = fields.value(1).toInt(&valid);
        if (fields.size() == 2 && valid && virtualKey >= 8 && virtualKey <= 254 &&
            m_menuHotkey != virtualKey) {
            m_menuHotkey = virtualKey;
            emit menuHotkeyChanged();
        }
    } else if (type == QByteArrayLiteral("GUI_SCALE_CHANGED")) {
        bool valid = false;
        const int index = fields.value(1).toInt(&valid);
        if (fields.size() == 2 && valid && index >= 0 && index <= 3
            && m_guiScaleIndex != index) {
            m_guiScaleIndex = index;
            emit guiScaleIndexChanged();
        }
    } else if (type == QByteArrayLiteral("PLAYER_FOUND")) {
        if (fields.size() != 3) return;
        const QString playerName = decodeProtocolToken(fields.at(1));
        const QString teamPrefix = decodeProtocolToken(fields.at(2)).toLower();
        static const QRegularExpression nameExpression(
            QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
        const bool validTeam = teamPrefix.size() == 2 &&
            teamPrefix.at(0) == QChar(0x00A7) &&
            ((teamPrefix.at(1) >= QLatin1Char('0') && teamPrefix.at(1) <= QLatin1Char('9')) ||
             (teamPrefix.at(1) >= QLatin1Char('a') && teamPrefix.at(1) <= QLatin1Char('f')));
        if (nameExpression.match(playerName).hasMatch() && validTeam)
            emit playerFound(playerName, teamPrefix);
    } else if (type == QByteArrayLiteral("MATCH_STATE")) {
        if (fields.size() != 2 ||
            (fields.at(1) != QByteArrayLiteral("0") && fields.at(1) != QByteArrayLiteral("1"))) {
            return;
        }
        const bool active = fields.at(1) == QByteArrayLiteral("1");
        if (m_matchActive != active) {
            m_matchActive = active;
            emit matchStateChanged(active);
        }
    } else if (type == QByteArrayLiteral("PLAYER_STATUS")) {
        if (fields.size() != 2) return;
        const QString name = decodeProtocolToken(fields.at(1));
        static const QRegularExpression nameExpression(
            QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
        if (nameExpression.match(name).hasMatch() && name != m_playerName) {
            m_playerName = name;
            emit playerStatusChanged();
        }
    } else if (type == QByteArrayLiteral("HYPIXEL_QUERY")) {
        if (fields.size() != 2) return;
        const QString playerId = decodeProtocolToken(fields.at(1));
        static const QRegularExpression nameExpression(
            QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
        if (nameExpression.match(playerId).hasMatch())
            emit hypixelQueryRequested(playerId);
    } else if (type == QByteArrayLiteral("DETACH_COMPLETE")) {
        if (fields.size() == 1 && m_state == State::Detaching)
            completeDetach(false);
    } else if (type == QByteArrayLiteral("GAME_STATE")) {
        // Protocol v1 (tokens separated by one or more ASCII spaces):
        // GAME_STATE 1 seq unixMs valid hp maxHp entityId x y z
        //            loadedEntities bedCount mappingPct statePct
        // String tokens are UTF-8 percent encoded; `-` means empty. Invalid
        // mapping snapshots retain zero numeric placeholders and valid=0.
        if (fields.size() != 15 || fields.at(1) != QByteArrayLiteral("1"))
            return;

        bool sequenceOk = false;
        bool timestampOk = false;
        bool healthOk = false;
        bool maxHealthOk = false;
        bool entityIdOk = false;
        bool xOk = false;
        bool yOk = false;
        bool zOk = false;
        bool entitiesOk = false;
        bool bedsOk = false;
        const quint64 sequence = fields.at(2).toULongLong(&sequenceOk);
        const qint64 timestamp = fields.at(3).toLongLong(&timestampOk);
        const bool validToken = fields.at(4) == QByteArrayLiteral("0")
                             || fields.at(4) == QByteArrayLiteral("1");
        const bool available = fields.at(4) == QByteArrayLiteral("1");
        const double health = fields.at(5).toDouble(&healthOk);
        const double maxHealth = fields.at(6).toDouble(&maxHealthOk);
        const int entityId = fields.at(7).toInt(&entityIdOk);
        const double x = fields.at(8).toDouble(&xOk);
        const double y = fields.at(9).toDouble(&yOk);
        const double z = fields.at(10).toDouble(&zOk);
        const int entities = fields.at(11).toInt(&entitiesOk);
        const int beds = fields.at(12).toInt(&bedsOk);

        const bool finiteNumbers = std::isfinite(health)
                                && std::isfinite(maxHealth)
                                && std::isfinite(x)
                                && std::isfinite(y)
                                && std::isfinite(z);
        const bool sensibleRanges = health >= -2048.0 && health <= 1000000.0
                                 && maxHealth >= 0.0 && maxHealth <= 1000000.0
                                 && std::abs(x) <= 100000000.0
                                 && std::abs(y) <= 100000000.0
                                 && std::abs(z) <= 100000000.0
                                 && entities >= 0 && entities <= 10000000
                                 && beds >= 0 && beds <= 10000000;
        if (!sequenceOk || sequence == 0 || !timestampOk || timestamp < 0 || !validToken
            || !healthOk || !maxHealthOk || !entityIdOk || !xOk || !yOk
            || !zOk || !entitiesOk || !bedsOk || !finiteNumbers
            || !sensibleRanges) {
            return;
        }

        // Protocol v1 deliberately keeps a fixed grammar for unavailable
        // mappings. Never surface stale or attacker-supplied gameplay values
        // from a valid=0 frame: every numeric placeholder must be zero.
        if (!available && (health != 0.0 || maxHealth != 0.0
                           || entityId != 0 || x != 0.0 || y != 0.0
                           || z != 0.0 || entities != 0 || beds != 0)) {
            return;
        }

        // Ignore delayed/reordered samples within one authenticated session.
        if (m_gameStateReceived && sequence <= m_gameStateSequence)
            return;

        m_gameStateReceived = true;
        m_gameStateAvailable = available;
        m_gameStateStale = false;
        m_playerHealth = health;
        m_playerMaxHealth = maxHealth;
        m_playerEntityId = entityId;
        m_playerX = x;
        m_playerY = y;
        m_playerZ = z;
        m_loadedEntities = entities;
        m_bedCount = beds;
        m_mappingProfile = decodeProtocolToken(fields.at(13));
        m_mappingState = decodeProtocolToken(fields.at(14));
        if (m_mappingState.isEmpty())
            m_mappingState = available ? QStringLiteral("ready")
                                           : QStringLiteral("unavailable");
        m_gameStateTimestamp = timestamp;
        m_lastGameStateReceiptElapsedMs = m_gameStateReceiptClock.elapsed();
        m_gameStateSequence = sequence;
        emit gameStateChanged();
    } else if (type == QByteArrayLiteral("STATUS")) {
        const int separator = line.indexOf(' ');
        if (separator >= 0)
            setStatusMessage(QString::fromUtf8(line.mid(separator + 1)));
    } else if (type == QByteArrayLiteral("ERROR")) {
        const QString code = QString::fromUtf8(fields.value(1));
        const int detailStart = line.indexOf(' ', line.indexOf(' ') + 1);
        fail(code.isEmpty() ? QStringLiteral("AGENT_ERROR") : code,
             detailStart >= 0 ? QString::fromUtf8(line.mid(detailStart + 1))
                              : QStringLiteral("The native agent reported an error."));
    }
}

void OverlayManager::sendStateSnapshot()
{
    if (!m_authenticated)
        return;
    writeAgentCommand(QByteArrayLiteral("STATE ")
                      + (m_overlayEnabled ? QByteArrayLiteral("1 ")
                                          : QByteArrayLiteral("0 "))
                      + (m_interactive ? QByteArrayLiteral("1\n")
                                       : QByteArrayLiteral("0\n")));
    sendFeatureSnapshot();
    sendBindSnapshot();
    sendGuiScaleSnapshot();
}

void OverlayManager::loadFeatureSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("features"));
    m_espEnabled = settings.value(QStringLiteral("espEnabled"), true).toBool();
    m_entityEspEnabled = settings.value(QStringLiteral("entityEspEnabled"), true).toBool();
    m_entityEspPlayersOnly = settings.value(QStringLiteral("entityEspPlayersOnly"), false).toBool();
    m_bedEspEnabled = settings.value(QStringLiteral("bedEspEnabled"), true).toBool();
    m_bedAutoRefreshEnabled = settings.value(QStringLiteral("bedAutoRefreshEnabled"), false).toBool();
    m_espLabelsEnabled = settings.value(QStringLiteral("labelsEnabled"), true).toBool();
    m_hypixelPanelEnabled = settings.value(QStringLiteral("hypixelPanelEnabled"), true).toBool();
    m_bedThreatAlertsEnabled = settings.value(QStringLiteral("bedThreatAlertsEnabled"), true).toBool();
    m_bedDefensePanelEnabled = settings.value(QStringLiteral("bedDefensePanelEnabled"), true).toBool();
    m_bedEspFilled = settings.value(QStringLiteral("bedEspFilled"), false).toBool();
    m_debugChatEnabled = settings.value(QStringLiteral("debugChatEnabled"), true).toBool();
    m_showOwnBedDefenseInfo = settings.value(QStringLiteral("showOwnBedDefenseInfo"), true).toBool();
    m_showTeammateBoxes = settings.value(QStringLiteral("showTeammateBoxes"), true).toBool();
    m_bedDefenseHoldToShow = settings.value(QStringLiteral("bedDefenseHoldToShow"), true).toBool();
    m_bedDefensePerspectiveScale = settings.value(QStringLiteral("bedDefensePerspectiveScale"), false).toBool();
    m_bedDefenseRadius = std::clamp(
        settings.value(QStringLiteral("bedDefenseRadius"), 6).toInt(), 3, 10);
    m_bedThreatRadius = std::clamp(
        settings.value(QStringLiteral("bedThreatRadius"), 8).toInt(), 3, 32);
    m_bedDefenseHotkey = std::clamp(
        settings.value(QStringLiteral("bedDefenseHotkey"), 0xA4).toInt(), 8, 254);
    m_bedDefensePanelOpacity = std::clamp(
        settings.value(QStringLiteral("bedDefensePanelOpacity"), 78).toInt(), 0, 100);
    const QString savedPlayerColor = normalizedRgbColor(
        settings.value(QStringLiteral("playerEspColor"), QStringLiteral("#FF3B30")).toString());
    const QString savedBedColor = normalizedRgbColor(
        settings.value(QStringLiteral("bedEspColor"), QStringLiteral("#FF5C68")).toString());
    const QString savedPanelColor = normalizedRgbColor(
        settings.value(QStringLiteral("bedDefensePanelColor"), QStringLiteral("#191621")).toString());
    m_playerEspColor = savedPlayerColor.isEmpty() ? QStringLiteral("#FF3B30") : savedPlayerColor;
    m_bedEspColor = savedBedColor.isEmpty() ? QStringLiteral("#FF5C68") : savedBedColor;
    m_bedDefensePanelColor = savedPanelColor.isEmpty()
        ? QStringLiteral("#191621") : savedPanelColor;
    settings.endGroup();
}

void OverlayManager::storeFeatureSettings()
{
    m_featureSettingsStoreTimer.start();
}

void OverlayManager::flushFeatureSettings() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("features"));
    settings.setValue(QStringLiteral("espEnabled"), m_espEnabled);
    settings.setValue(QStringLiteral("entityEspEnabled"), m_entityEspEnabled);
    settings.setValue(QStringLiteral("entityEspPlayersOnly"), m_entityEspPlayersOnly);
    settings.setValue(QStringLiteral("bedEspEnabled"), m_bedEspEnabled);
    settings.setValue(QStringLiteral("bedAutoRefreshEnabled"), m_bedAutoRefreshEnabled);
    settings.setValue(QStringLiteral("labelsEnabled"), m_espLabelsEnabled);
    settings.setValue(QStringLiteral("hypixelPanelEnabled"), m_hypixelPanelEnabled);
    settings.setValue(QStringLiteral("bedThreatAlertsEnabled"), m_bedThreatAlertsEnabled);
    settings.setValue(QStringLiteral("bedDefensePanelEnabled"), m_bedDefensePanelEnabled);
    settings.setValue(QStringLiteral("bedEspFilled"), m_bedEspFilled);
    settings.setValue(QStringLiteral("debugChatEnabled"), m_debugChatEnabled);
    settings.setValue(QStringLiteral("showOwnBedDefenseInfo"), m_showOwnBedDefenseInfo);
    settings.setValue(QStringLiteral("showTeammateBoxes"), m_showTeammateBoxes);
    settings.setValue(QStringLiteral("bedDefenseHoldToShow"), m_bedDefenseHoldToShow);
    settings.setValue(QStringLiteral("bedDefensePerspectiveScale"), m_bedDefensePerspectiveScale);
    settings.setValue(QStringLiteral("bedDefenseRadius"), m_bedDefenseRadius);
    settings.setValue(QStringLiteral("bedThreatRadius"), m_bedThreatRadius);
    settings.setValue(QStringLiteral("bedDefenseHotkey"), m_bedDefenseHotkey);
    settings.setValue(QStringLiteral("bedDefensePanelOpacity"), m_bedDefensePanelOpacity);
    settings.setValue(QStringLiteral("playerEspColor"), m_playerEspColor);
    settings.setValue(QStringLiteral("bedEspColor"), m_bedEspColor);
    settings.setValue(QStringLiteral("bedDefensePanelColor"), m_bedDefensePanelColor);
    settings.endGroup();
    settings.sync();
}

void OverlayManager::sendFeatureSnapshot()
{
    if (!m_authenticated) return;
    writeAgentCommand(QByteArrayLiteral("FEATURE_STATE ")
                      + (m_espEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_entityEspEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_bedEspEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_espLabelsEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_hypixelPanelEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_bedThreatAlertsEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_bedDefensePanelEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_entityEspPlayersOnly ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_bedAutoRefreshEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_bedEspFilled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_debugChatEnabled ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_showOwnBedDefenseInfo ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_showTeammateBoxes ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_bedDefenseHoldToShow ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + (m_bedDefensePerspectiveScale ? QByteArrayLiteral("1 ") : QByteArrayLiteral("0 "))
                      + QByteArray::number(std::clamp(m_bedDefenseRadius, 3, 10)) + ' '
                      + QByteArray::number(std::clamp(m_bedThreatRadius, 3, 32)) + ' '
                      + QByteArray::number(std::clamp(m_bedDefenseHotkey, 8, 254)) + ' '
                      + QByteArray::number(std::clamp(m_bedDefensePanelOpacity, 0, 100)) + ' '
                      + QByteArray::number(rgbFromHexColor(m_playerEspColor)) + ' '
                      + QByteArray::number(rgbFromHexColor(m_bedEspColor)) + ' '
                      + QByteArray::number(rgbFromHexColor(m_bedDefensePanelColor)) + '\n');
}

void OverlayManager::sendBindSnapshot()
{
    if (!m_authenticated) return;
    writeAgentCommand(QByteArrayLiteral("BIND ") +
                      QByteArray::number(std::clamp(m_menuHotkey, 8, 254)) + '\n');
}

void OverlayManager::sendGuiScaleSnapshot()
{
    if (!m_authenticated) return;
    writeAgentCommand(QByteArrayLiteral("GUI_SCALE ")
                      + QByteArray::number(std::clamp(m_guiScaleIndex, 0, 3)) + '\n');
}

void OverlayManager::writeAgentCommand(const QByteArray &command)
{
    if (!m_agentSocket || m_agentSocket->state() != QLocalSocket::ConnectedState)
        return;
    m_agentSocket->write(command);
}

bool OverlayManager::closeSessionTransport()
{
    if (m_closingTransport)
        return m_attachProcess.state() == QProcess::NotRunning;
    m_closingTransport = true;
    m_attachTimeout.stop();
    m_nativeFallbackGrace.stop();
    m_detachTimeout.stop();
    m_targetMonitor.stop();
    m_authenticated = false;
    m_loaderKind = LoaderKind::None;
    m_agentReadBuffer.clear();

    if (m_agentSocket) {
        disconnect(m_agentSocket, nullptr, this, nullptr);
        m_agentSocket->abort();
        m_agentSocket->deleteLater();
        m_agentSocket = nullptr;
    }
    m_server.close();

    bool helperStopped = true;
    if (m_attachProcess.state() != QProcess::NotRunning) {
        m_attachProcess.kill();
        // kill() is asynchronous. Never spin a nested event loop or block the
        // GUI thread here; handleAttachFinished advances queued re-attachment.
        helperStopped = m_attachProcess.state() == QProcess::NotRunning;
    }
    m_agentDllPath.clear();
    m_agentOptions.clear();
    m_closingTransport = false;
    return helperStopped;
}

void OverlayManager::setState(State state)
{
    if (m_state == state)
        return;
    const bool wasAttached = attached();
    const bool wasBusy = busy();
    m_state = state;
    emit stateChanged();
    if (wasAttached != attached())
        emit attachedChanged();
    if (wasBusy != busy())
        emit busyChanged();
}

void OverlayManager::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message)
        return;
    m_statusMessage = message;
    emit statusMessageChanged();
}

void OverlayManager::setRenderer(const QString &renderer)
{
    if (m_renderer == renderer)
        return;
    m_renderer = renderer;
    emit rendererChanged();
}

void OverlayManager::resetGameState()
{
    const bool changed = m_gameStateReceived || m_gameStateAvailable
                      || m_gameStateStale || m_playerHealth != 0.0
                      || m_playerMaxHealth != 0.0 || m_playerEntityId != 0
                      || m_playerX != 0.0 || m_playerY != 0.0
                      || m_playerZ != 0.0 || m_loadedEntities != 0
                      || m_bedCount != 0 || !m_mappingProfile.isEmpty()
                      || m_mappingState != QStringLiteral("waiting")
                      || m_gameStateTimestamp != 0 || m_gameStateSequence != 0;
    m_gameStateReceived = false;
    m_gameStateAvailable = false;
    m_gameStateStale = false;
    m_playerHealth = 0.0;
    m_playerMaxHealth = 0.0;
    m_playerEntityId = 0;
    m_playerX = 0.0;
    m_playerY = 0.0;
    m_playerZ = 0.0;
    m_loadedEntities = 0;
    m_bedCount = 0;
    m_mappingProfile.clear();
    m_mappingState = QStringLiteral("waiting");
    m_gameStateTimestamp = 0;
    m_lastGameStateReceiptElapsedMs = 0;
    m_gameStateSequence = 0;
    if (!m_playerName.isEmpty()) {
        m_playerName.clear();
        emit playerStatusChanged();
    }
    if (m_matchActive) {
        m_matchActive = false;
        emit matchStateChanged(false);
    }
    if (changed)
        emit gameStateChanged();
}

void OverlayManager::refreshGameStateFreshness()
{
    if (!m_gameStateReceived || m_gameStateStale)
        return;
    if (m_gameStateReceiptClock.elapsed() - m_lastGameStateReceiptElapsedMs
        <= kGameStateStaleAfterMilliseconds) {
        return;
    }
    m_gameStateStale = true;
    emit gameStateChanged();
}

void OverlayManager::clearError()
{
    if (m_errorCode.isEmpty() && m_errorDetail.isEmpty())
        return;
    m_errorCode.clear();
    m_errorDetail.clear();
    emit errorChanged();
}

void OverlayManager::setErrorState(const QString &code, const QString &detail)
{
    m_errorCode = code;
    m_errorDetail = detail;
    emit errorChanged();
    setState(State::Error);
    setStatusMessage(detail);
}

void OverlayManager::fail(const QString &code, const QString &detail)
{
    (void) closeSessionTransport();
    setRenderer({});
    resetGameState();
    setErrorState(code, detail);
}
