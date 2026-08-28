#include "HotkeyCaptureService.h"

#include <QCoreApplication>
#include <QKeyEvent>

#include <cassert>

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
    assert(capturedKey == 0x4B);
    assert(!capture.capturing());

    capture.beginCapture();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&application, &escape);
    assert(canceled);
    assert(!capture.capturing());
    return 0;
}
