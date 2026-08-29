#include "HotkeyCaptureService.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <Qt>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

HotkeyCaptureService::HotkeyCaptureService(QObject *parent)
    : QObject(parent)
{
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->installEventFilter(this);
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
}

HotkeyCaptureService::~HotkeyCaptureService()
{
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeEventFilter(this);
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
}

void HotkeyCaptureService::setCapturing(const bool capturing)
{
    if (m_capturing == capturing) return;
    m_capturing = capturing;
    emit capturingChanged();
}

void HotkeyCaptureService::beginCapture()
{
    // This also tells every inactive KeyCaptureButton to clear its local
    // visual state before the clicked button marks itself active.
    setCapturing(true);
    emit captureStarted();
}

void HotkeyCaptureService::cancelCapture()
{
    if (!m_capturing) return;
    setCapturing(false);
    emit captureCanceled();
}

int HotkeyCaptureService::fallbackVirtualKey(const int qtKey) noexcept
{
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) return qtKey;
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) return qtKey;
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12)
        return 0x70 + qtKey - Qt::Key_F1;
    switch (qtKey) {
    case Qt::Key_Backspace: return 0x08;
    case Qt::Key_Tab: return 0x09;
    case Qt::Key_Return:
    case Qt::Key_Enter: return 0x0D;
    case Qt::Key_Shift: return 0x10;
    case Qt::Key_Control: return 0x11;
    case Qt::Key_Alt: return 0x12;
    case Qt::Key_Space: return 0x20;
    case Qt::Key_PageUp: return 0x21;
    case Qt::Key_PageDown: return 0x22;
    case Qt::Key_End: return 0x23;
    case Qt::Key_Home: return 0x24;
    case Qt::Key_Left: return 0x25;
    case Qt::Key_Up: return 0x26;
    case Qt::Key_Right: return 0x27;
    case Qt::Key_Down: return 0x28;
    case Qt::Key_Insert: return 0x2D;
    case Qt::Key_Delete: return 0x2E;
    case Qt::Key_Apostrophe: return 0xDE;
    default: return 0;
    }
}

bool HotkeyCaptureService::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (!m_capturing || event == nullptr) return false;
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    if (event->type() != QEvent::KeyPress) return false;

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    event->accept();
    if (keyEvent->isAutoRepeat()) return true;
    if (keyEvent->key() == Qt::Key_Escape) {
        cancelCapture();
        return true;
    }
    int virtualKey = static_cast<int>(keyEvent->nativeVirtualKey());
    if (virtualKey < 8 || virtualKey > 254)
        virtualKey = fallbackVirtualKey(keyEvent->key());
    if (virtualKey >= 8 && virtualKey <= 254 &&
        virtualKey != 0x01 && virtualKey != 0x02 && virtualKey != 0x04 &&
        virtualKey != 0x05 && virtualKey != 0x06) {
        setCapturing(false);
        emit keyCaptured(virtualKey);
    }
    return true;
}

bool HotkeyCaptureService::nativeEventFilter(const QByteArray &eventType,
                                             void *message,
                                             qintptr *result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)
#ifdef Q_OS_WIN
    if (!m_capturing || message == nullptr) return false;
    const auto *nativeMessage = static_cast<const MSG *>(message);
    if (nativeMessage->message != WM_KEYDOWN &&
        nativeMessage->message != WM_SYSKEYDOWN) {
        return false;
    }
    // Bit 30 is set when the key was already down. Consume repeats without
    // repeatedly changing the binding.
    if ((static_cast<quintptr>(nativeMessage->lParam) & (quintptr{1} << 30U)) != 0U)
        return true;

    const int virtualKey = static_cast<int>(nativeMessage->wParam & 0xFFU);
    if (virtualKey == VK_ESCAPE) {
        cancelCapture();
        return true;
    }
    if (virtualKey >= 8 && virtualKey <= 254 &&
        virtualKey != VK_LBUTTON && virtualKey != VK_RBUTTON &&
        virtualKey != VK_MBUTTON && virtualKey != VK_XBUTTON1 &&
        virtualKey != VK_XBUTTON2) {
        setCapturing(false);
        emit keyCaptured(virtualKey);
    }
    return true;
#else
    return false;
#endif
}
