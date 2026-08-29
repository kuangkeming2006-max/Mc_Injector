#include "HotkeyCaptureService.h"

#include <QCoreApplication>
#include <QKeyEvent>

#include <cstdio>

namespace {

bool require(const bool condition, const char *const message)
{
    if (condition) return true;
    std::fprintf(stderr, "Hotkey capture test failed: %s\n", message);
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    HotkeyCaptureService capture;
    int capturedKey = 0;
    bool canceled = false;
    QObject::connect(&capture, &HotkeyCaptureService::keyCaptured,
                     [&](const int key) { capturedKey = key; });
    QObject::connect(&capture, &HotkeyCaptureService::captureCanceled,
                     [&] { canceled = true; });

    capture.beginCapture();
    QKeyEvent keyPress(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier,
                       0U, 0x4BU, 0U, QStringLiteral("k"));
    QCoreApplication::sendEvent(&application, &keyPress);
    if (!require(capturedKey == 0x4B, "K did not produce VK_K") ||
        !require(!capture.capturing(), "capture stayed active after K")) return 1;

    capture.beginCapture();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&application, &escape);
    if (!require(canceled, "Escape did not emit captureCanceled") ||
        !require(!capture.capturing(), "capture stayed active after Escape")) return 1;
    std::puts("Hotkey capture tests passed.");
    return 0;
}
