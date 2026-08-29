#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

class QEvent;

// Application-level key capture avoids QML focus races after a MouseArea
// click. It is intentionally one-shot: the next physical keyboard press is
// consumed before shortcuts/text fields can see it, then emitted as a Win32
// virtual-key value.
class HotkeyCaptureService final : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_PROPERTY(bool capturing READ capturing NOTIFY capturingChanged)

public:
    explicit HotkeyCaptureService(QObject *parent = nullptr);
    ~HotkeyCaptureService() override;

    [[nodiscard]] bool capturing() const noexcept { return m_capturing; }

    Q_INVOKABLE void beginCapture();
    Q_INVOKABLE void cancelCapture();

signals:
    void capturingChanged();
    void captureStarted();
    void keyCaptured(int virtualKey);
    void captureCanceled();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;

private:
    static int fallbackVirtualKey(int qtKey) noexcept;
    void setCapturing(bool capturing);

    bool m_capturing = false;
};
