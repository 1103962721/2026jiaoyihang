#pragma once

#include <windows.h>

#include <QString>
#include <QVector>

struct TargetWindowInfo
{
    HWND hwnd = nullptr;
    QString title;
    QString processName;
    QString className;
    int clientWidth = 0;
    int clientHeight = 0;
    QString hwndText;
    bool isGameBody = false;
    bool isCloudPhone = false;
    bool isCloudPhoneSurface = false;
};

class Win32WindowBinder
{
public:
    QVector<TargetWindowInfo> enumerateWindows(HWND excludeHwnd) const;
    int findGameBodyIndex(const QVector<TargetWindowInfo>& windows) const;
    bool isValid(HWND hwnd) const;
    bool prepareForBinding(HWND hwnd, int& width, int& height) const;
    bool clientSize(HWND hwnd, int& width, int& height) const;
    bool pointInsideClient(HWND hwnd, POINT point) const;
    bool screenToClientPoint(HWND hwnd, POINT& point) const;
};
