#pragma once

#include <windows.h>

#include <QAbstractNativeEventFilter>
#include <QObject>

class HotkeyManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager() override;

    bool start(HWND windowHandle);
    void stop();
    bool isActive() const;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void hotkeyPressed();
    void f3Pressed();

private:
    HWND windowHandle_ = nullptr;
    bool active_ = false;
};
