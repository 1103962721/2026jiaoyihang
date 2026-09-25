#pragma once

#include <windows.h>

#include <QString>

class Win32InputSequenceLock
{
public:
    Win32InputSequenceLock() = default;
    ~Win32InputSequenceLock();

    Win32InputSequenceLock(const Win32InputSequenceLock&) = delete;
    Win32InputSequenceLock& operator=(const Win32InputSequenceLock&) = delete;

    bool acquire(QString& errorText, DWORD waitMs = 10000);

private:
    HANDLE mutex_ = nullptr;
    bool owns_ = false;
};

class Win32InputDispatcher
{
public:
    bool clickClientPoint(HWND hwnd, POINT point, int releaseDelayMs, QString& errorText) const;
    bool rightClickReferencePoint(HWND hwnd, POINT point, int referenceWidth, int referenceHeight, int releaseDelayMs, QString& errorText) const;
    bool scrollReferencePoint(HWND hwnd, POINT point, int referenceWidth, int referenceHeight, int wheelDelta, QString& errorText) const;
    bool clickReferencePoint(HWND hwnd, POINT point, int referenceWidth, int referenceHeight, int releaseDelayMs, QString& errorText) const;
    bool moveReferencePoint(HWND hwnd, POINT point, int referenceWidth, int referenceHeight, QString& errorText) const;
    bool pressKeyChord(WORD modifierKey, WORD virtualKey, QString& errorText) const;
    bool pressKeyRepeated(WORD virtualKey, int count, int intervalMs, QString& errorText) const;
    bool pasteText(HWND hwnd, const QString& text, QString& errorText) const;
    bool moveReferencePointBezier(HWND hwnd, POINT from, POINT to, int referenceWidth, int referenceHeight, int durationMs, QString& errorText) const;
    bool pressKey(WORD virtualKey, QString& errorText) const;
    QString readClipboardText(QString& errorText) const;
};
