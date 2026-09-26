#include "Win32WindowBinder.h"

#include <QFileInfo>

#include <psapi.h>

#include "../Shared/ClientBindingRules.h"

namespace
{
struct EnumContext
{
    HWND excludeHwnd = nullptr;
    QVector<TargetWindowInfo>* windows = nullptr;
};

QString fromWide(const wchar_t* text)
{
    return QString::fromWCharArray(text ? text : L"");
}

QString processNameForWindow(HWND hwnd)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
    {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!process)
    {
        return {};
    }

    wchar_t path[MAX_PATH]{};
    QString name;
    if (GetModuleFileNameExW(process, nullptr, path, MAX_PATH) > 0)
    {
        name = QFileInfo(QString::fromWCharArray(path)).fileName();
    }
    CloseHandle(process);
    return name;
}

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* context = reinterpret_cast<EnumContext*>(lParam);
    if (!context || !context->windows || hwnd == context->excludeHwnd || !IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    if (title[0] == L'\0')
    {
        return TRUE;
    }

    RECT client{};
    if (!GetClientRect(hwnd, &client))
    {
        return TRUE;
    }

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, 128);
    const QString processName = processNameForWindow(hwnd);

    TargetWindowInfo item;
    item.hwnd = hwnd;
    item.title = fromWide(title);
    item.processName = processName;
    item.className = fromWide(className);
    item.clientWidth = client.right - client.left;
    item.clientHeight = client.bottom - client.top;
    item.hwndText = fromWide(FormatHwndLow8Hex(hwnd).c_str());
    item.isGameBody = IsDeltaForceGameBodyTitle(title)
        && IsDeltaForceGameBodyProcessName(reinterpret_cast<const wchar_t*>(processName.utf16()));

    context->windows->append(item);
    return TRUE;
}
}

QVector<TargetWindowInfo> Win32WindowBinder::enumerateWindows(HWND excludeHwnd) const
{
    QVector<TargetWindowInfo> windows;
    EnumContext context{ excludeHwnd, &windows };
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&context));
    return windows;
}

int Win32WindowBinder::findGameBodyIndex(const QVector<TargetWindowInfo>& windows) const
{
    for (int index = 0; index < windows.size(); ++index)
    {
        if (windows[index].isGameBody)
        {
            return index;
        }
    }
    return -1;
}

bool Win32WindowBinder::isValid(HWND hwnd) const
{
    return hwnd && IsWindow(hwnd);
}

bool Win32WindowBinder::prepareForBinding(HWND hwnd, int& width, int& height) const
{
    if (!isValid(hwnd))
    {
        return false;
    }

    bool needsActivation = IsIconic(hwnd) != FALSE;
    if (!clientSize(hwnd, width, height) || width <= 0 || height <= 0)
    {
        needsActivation = true;
    }

    if (needsActivation)
    {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        UpdateWindow(hwnd);
        Sleep(100);
    }

    if (!clientSize(hwnd, width, height) || width <= 0 || height <= 0)
    {
        return false;
    }
    return true;
}

bool Win32WindowBinder::clientSize(HWND hwnd, int& width, int& height) const
{
    RECT rect{};
    if (!hwnd || !GetClientRect(hwnd, &rect))
    {
        return false;
    }

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    return true;
}

bool Win32WindowBinder::pointInsideClient(HWND hwnd, POINT point) const
{
    int width = 0;
    int height = 0;
    return clientSize(hwnd, width, height) && point.x >= 0 && point.y >= 0 && point.x < width && point.y < height;
}

bool Win32WindowBinder::screenToClientPoint(HWND hwnd, POINT& point) const
{
    return hwnd && ScreenToClient(hwnd, &point);
}
