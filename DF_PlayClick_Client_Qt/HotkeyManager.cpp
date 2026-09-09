#include "HotkeyManager.h"

#include <QCoreApplication>

namespace
{
constexpr int PickHotkeyId = 3001;
constexpr int F3HotkeyId = 3002;
constexpr UINT PickHotkeyModifiers = MOD_CONTROL | MOD_SHIFT;
constexpr UINT PickHotkeyKey = 'P';
}

HotkeyManager::HotkeyManager(QObject* parent)
    : QObject(parent)
{
}

HotkeyManager::~HotkeyManager()
{
    stop();
}

bool HotkeyManager::start(HWND windowHandle)
{
    stop();
    if (!windowHandle)
    {
        return false;
    }

    if (!RegisterHotKey(windowHandle, PickHotkeyId, PickHotkeyModifiers, PickHotkeyKey))
    {
        return false;
    }
    if (!RegisterHotKey(windowHandle, F3HotkeyId, 0, VK_F3))
    {
        UnregisterHotKey(windowHandle, PickHotkeyId);
        return false;
    }

    windowHandle_ = windowHandle;
    active_ = true;
    QCoreApplication::instance()->installNativeEventFilter(this);
    return true;
}

void HotkeyManager::stop()
{
    if (active_ && windowHandle_)
    {
        UnregisterHotKey(windowHandle_, PickHotkeyId);
        UnregisterHotKey(windowHandle_, F3HotkeyId);
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    active_ = false;
    windowHandle_ = nullptr;
}

bool HotkeyManager::isActive() const
{
    return active_;
}

bool HotkeyManager::nativeEventFilter(const QByteArray&, void* message, qintptr*)
{
    MSG* msg = static_cast<MSG*>(message);
    if (!msg || msg->message != WM_HOTKEY)
    {
        return false;
    }

    const int hotkeyId = static_cast<int>(msg->wParam);
    if (hotkeyId == PickHotkeyId)
    {
        emit hotkeyPressed();
        return true;
    }
    if (hotkeyId == F3HotkeyId)
    {
        emit f3Pressed();
        return true;
    }
    return false;
}
