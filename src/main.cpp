#include "OverlayManager.h"
#include "ProcessScanner.h"
#include "HypixelApiClient.h"
#include "PlayerStatsService.h"
#include "ApiKeyStore.h"
#include "AppSettings.h"
#include "SkinProfileService.h"
#include "SkinCuboidGeometry.h"
#include "HotkeyCaptureService.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQmlError>
#include <QDebug>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqml.h>

#include <cstdlib>

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("MinecraftOverlayManager"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Java Overlay Studio"));
    QGuiApplication::setOrganizationName(QStringLiteral("Overlay Studio"));

    const bool smokeTest = qEnvironmentVariableIsSet("MC_OVERLAY_SMOKE_TEST")
                        || application.arguments().contains(
                               QStringLiteral("--smoke-test"));

    // The controller UI remains hardware accelerated. The Minecraft overlay is
    // no longer a second Qt window: McOverlayAgent.dll renders Dear ImGui inside
    // the target process' active OpenGL context.
#ifdef Q_OS_WIN
    QQuickWindow::setGraphicsApi(smokeTest
        ? QSGRendererInterface::Software
        : QSGRendererInterface::Direct3D11);
#endif
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    ProcessScanner processScanner;
    OverlayManager overlayManager;
    ApiKeyStore apiKeys;
    AppSettings appSettings;
    HypixelApiClient hypixelApi(&apiKeys);
    PlayerStatsService playerStatsService(&apiKeys);
    SkinProfileService skinProfile;
    HotkeyCaptureService hotkeyCapture;

    // Restore controller preferences before QML or the native Agent observes
    // them. Changes coming back from the in-game GUI are persisted through the
    // same object, so both interfaces always converge on one value.
    overlayManager.setMenuHotkey(appSettings.menuHotkey());
    overlayManager.setGuiScaleIndex(appSettings.guiScaleIndex());
    QObject::connect(&overlayManager, &OverlayManager::menuHotkeyChanged,
                     &appSettings, [&overlayManager, &appSettings] {
        appSettings.setMenuHotkey(overlayManager.menuHotkey());
    });
    QObject::connect(&overlayManager, &OverlayManager::guiScaleIndexChanged,
                     &appSettings, [&overlayManager, &appSettings] {
        appSettings.setGuiScaleIndex(overlayManager.guiScaleIndex());
    });

    QObject::connect(&apiKeys, &ApiKeyStore::changed,
                     &hypixelApi, &HypixelApiClient::reloadConfiguration);
    QObject::connect(&apiKeys, &ApiKeyStore::changed,
                     &playerStatsService, &PlayerStatsService::reloadConfiguration);

    QObject::connect(&overlayManager, &OverlayManager::hypixelQueryRequested,
                     &hypixelApi, &HypixelApiClient::lookupPlayer);
    QObject::connect(&overlayManager, &OverlayManager::playerFound,
                     &playerStatsService, &PlayerStatsService::enqueuePlayer);
    QObject::connect(&overlayManager, &OverlayManager::matchStateChanged,
                     &playerStatsService, &PlayerStatsService::setMatchActive);
    QObject::connect(&overlayManager, &OverlayManager::playerStatusChanged,
                     &overlayManager, [&overlayManager, &skinProfile] {
        skinProfile.lookup(overlayManager.playerName());
    });
    QObject::connect(&playerStatsService, &PlayerStatsService::statsReady,
                     &overlayManager, &OverlayManager::publishPlayerStats);
    QObject::connect(&playerStatsService, &PlayerStatsService::statsFailed,
                     &overlayManager, &OverlayManager::publishPlayerStatsError);
    const auto publishHypixelToAgent = [&overlayManager, &hypixelApi] {
        const QString status = hypixelApi.errorMessage().isEmpty()
            ? hypixelApi.statusMessage() : hypixelApi.errorMessage();
        overlayManager.publishHypixelResult(
            static_cast<int>(hypixelApi.state()), hypixelApi.queriedUuid(),
            hypixelApi.displayName(), hypixelApi.wins(), hypixelApi.losses(),
            hypixelApi.finalKills(), hypixelApi.finalDeaths(),
            hypixelApi.bedsBroken(), hypixelApi.bedsLost(),
            hypixelApi.winRate(), hypixelApi.fkdr(), status);
    };
    QObject::connect(&hypixelApi, &HypixelApiClient::stateChanged,
                     &overlayManager, publishHypixelToAgent);
    QObject::connect(&hypixelApi, &HypixelApiClient::statsChanged,
                     &overlayManager, publishHypixelToAgent);
    QObject::connect(&hypixelApi, &HypixelApiClient::statusMessageChanged,
                     &overlayManager, publishHypixelToAgent);
    QObject::connect(&hypixelApi, &HypixelApiClient::errorMessageChanged,
                     &overlayManager, publishHypixelToAgent);
    QObject::connect(&overlayManager, &OverlayManager::agentSessionReady,
                     &overlayManager, publishHypixelToAgent);

    QObject::connect(&overlayManager, &OverlayManager::targetExited,
                     &processScanner, [&processScanner](quint32) {
        processScanner.selectProcess(0);
        processScanner.refresh();
    });

    qmlRegisterSingletonInstance("McOverlay", 1, 0,
                                 "ProcessScanner", &processScanner);
    qmlRegisterSingletonInstance("McOverlay", 1, 0,
                                 "OverlayManager", &overlayManager);
    qmlRegisterSingletonInstance("McOverlay", 1, 0,
                                 "HotkeyCapture", &hotkeyCapture);
    qmlRegisterSingletonInstance("McOverlay", 1, 0,
                                 "HypixelApi", &hypixelApi);
    qmlRegisterSingletonInstance("McOverlay", 1, 0,
                                 "ApiKeys", &apiKeys);
    qmlRegisterSingletonInstance("McOverlay", 1, 0,
                                 "AppSettings", &appSettings);
    qmlRegisterSingletonInstance("McOverlay", 1, 0,
                                 "SkinProfile", &skinProfile);
    qmlRegisterType<SkinCuboidGeometry>("McOverlay", 1, 0, "SkinCuboidGeometry");

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     &application, [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qCritical().noquote() << warning.toString();
    });

    const QUrl mainUrl(QStringLiteral("qrc:/qml/main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &application, [mainUrl](QObject *object, const QUrl &objectUrl) {
        if (!object && objectUrl == mainUrl) {
            qCritical().noquote() << "Failed to create root QML object:" << objectUrl.toString();
            QCoreApplication::exit(EXIT_FAILURE);
        }
    }, Qt::QueuedConnection);
    engine.load(mainUrl);
    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;

    // Opt-in smoke-test mode is useful for CI and packaging checks: the real
    // engine loads every component, then exits without requiring user input.
    // The command-line form also works for a GUI-subsystem executable, whose
    // environment can be inconvenient to control from some Windows launchers.
    if (smokeTest)
        QTimer::singleShot(250, &application, &QCoreApplication::quit);

    return application.exec();
}
