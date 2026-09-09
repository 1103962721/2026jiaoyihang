#include "Win32InputDispatcher.h"

#include <cstring>
#include <vector>

namespace
{
const wchar_t LocalClickQueueMutex[] = L"Local\\DF_PlayClick_Client_ClickQueue";

bool pointInsideClient(HWND hwnd, POINT point)
{
    RECT rect{};
    if (!GetClientRect(hwnd, &rect))
    {
        return false;
    }

    return point.x >= 0 && point.y >= 0 && point.x < (rect.right - rect.left) && point.y < (rect.bottom - rect.top);
}
}

Win32InputSequenceLock::~Win32InputSequenceLock()
{
    if (owns_)
    {
        ReleaseMutex(mutex_);
    }
    if (mutex_)
    {
        CloseHandle(mutex_);
    }
}

bool Win32InputSequenceLock::acquire(QString& errorText, DWORD waitMs)
{
    if (owns_)
    {
        return true;
    }

    mutex_ = CreateMutexW(nullptr, FALSE, LocalClickQueueMutex);
    if (!mutex_)
    {
        errorText = QStringLiteral("本机点击队列创建失败");
        return false;
    }

    // 多个客户端进程共享同一个本机互斥锁，整段自动化可持锁避免真实输入交叉。
    const DWORD waitResult = WaitForSingleObject(mutex_, waitMs);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED)
    {
        owns_ = true;
        return true;
    }

    errorText = QStringLiteral("等待本机点击队列超时");
    return false;
}

bool Win32InputDispatcher::clickClientPoint(HWND hwnd, POINT point, int releaseDelayMs, QString& errorText) const
{
    if (!hwnd || !IsWindow(hwnd))
    {
        errorText = QStringLiteral("绑定窗口已失效，请重新绑定");
        return false;
    }

    if (!pointInsideClient(hwnd, point))
    {
        errorText = QStringLiteral("点击坐标不在绑定窗口客户区内");
        return false;
    }

    Win32InputSequenceLock gate;
    if (!gate.acquire(errorText))
    {
        return false;
    }

    // 点击坐标始终按绑定窗口客户区坐标计算，再转换成屏幕坐标发送真实鼠标输入。
    ClientToScreen(hwnd, &point);
    SetForegroundWindow(hwnd);

    INPUT inputs[3]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    inputs[0].mi.dx = static_cast<LONG>(point.x * 65535 / GetSystemMetrics(SM_CXSCREEN));
    inputs[0].mi.dy = static_cast<LONG>(point.y * 65535 / GetSystemMetrics(SM_CYSCREEN));
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    const UINT sent = SendInput(3, inputs, sizeof(INPUT));
    Sleep(static_cast<DWORD>(releaseDelayMs));
    if (sent != 3)
    {
        errorText = QStringLiteral("发送鼠标输入失败");
        return false;
    }

    return true;
}

bool Win32InputDispatcher::clickReferencePoint(HWND hwnd, POINT point, int referenceWidth, int referenceHeight, int releaseDelayMs, QString& errorText) const
{
    RECT rect{};
    if (!hwnd || !IsWindow(hwnd) || !GetClientRect(hwnd, &rect))
    {
        errorText = QStringLiteral("绑定窗口已失效，请重新绑定");
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || referenceWidth <= 0 || referenceHeight <= 0)
    {
        errorText = QStringLiteral("窗口客户区尺寸无效");
        return false;
    }

    // 游戏坐标以参考客户区为基准，窗口分辨率变化时按比例缩放。
    POINT scaled{};
    scaled.x = point.x * width / referenceWidth;
    scaled.y = point.y * height / referenceHeight;
    return clickClientPoint(hwnd, scaled, releaseDelayMs, errorText);
}

bool Win32InputDispatcher::moveReferencePoint(HWND hwnd, POINT point, int referenceWidth, int referenceHeight, QString& errorText) const
{
    RECT rect{};
    if (!hwnd || !IsWindow(hwnd) || !GetClientRect(hwnd, &rect))
    {
        errorText = QStringLiteral("绑定窗口已失效，请重新绑定");
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || referenceWidth <= 0 || referenceHeight <= 0)
    {
        errorText = QStringLiteral("窗口客户区尺寸无效");
        return false;
    }

    POINT scaled{ point.x * width / referenceWidth, point.y * height / referenceHeight };
    if (!pointInsideClient(hwnd, scaled))
    {
        errorText = QStringLiteral("鼠标移动坐标不在绑定窗口客户区内");
        return false;
    }

    Win32InputSequenceLock gate;
    if (!gate.acquire(errorText))
    {
        return false;
    }

    ClientToScreen(hwnd, &scaled);
    SetForegroundWindow(hwnd);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.dx = static_cast<LONG>(scaled.x * 65535 / GetSystemMetrics(SM_CXSCREEN));
    input.mi.dy = static_cast<LONG>(scaled.y * 65535 / GetSystemMetrics(SM_CYSCREEN));
    if (SendInput(1, &input, sizeof(INPUT)) != 1)
    {
        errorText = QStringLiteral("发送鼠标移动输入失败");
        return false;
    }
    return true;
}
bool Win32InputDispatcher::moveReferencePointBezier(HWND hwnd, POINT from, POINT to, int referenceWidth, int referenceHeight, int durationMs, QString& errorText) const
{
    RECT rect{};
    if (!hwnd || !IsWindow(hwnd) || !GetClientRect(hwnd, &rect))
    {
        errorText = QStringLiteral("绑定窗口已失效，请重新绑定");
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || referenceWidth <= 0 || referenceHeight <= 0)
    {
        errorText = QStringLiteral("窗口客户区尺寸无效");
        return false;
    }

    POINT start{ from.x * width / referenceWidth, from.y * height / referenceHeight };
    POINT end{ to.x * width / referenceWidth, to.y * height / referenceHeight };
    if (!pointInsideClient(hwnd, start) || !pointInsideClient(hwnd, end))
    {
        errorText = QStringLiteral("鼠标移动坐标不在绑定窗口客户区内");
        return false;
    }

    Win32InputSequenceLock gate;
    if (!gate.acquire(errorText))
    {
        return false;
    }

    POINT screenStart = start;
    POINT screenEnd = end;
    ClientToScreen(hwnd, &screenStart);
    ClientToScreen(hwnd, &screenEnd);
    SetForegroundWindow(hwnd);

    // 自雷前用二次贝塞尔曲线把鼠标移到客户区底部，再补充相对位移让游戏视角收到足够的向下拖拽量。
    const POINT control{ (screenStart.x + screenEnd.x) / 2 + width / 12, (screenStart.y + screenEnd.y) / 2 };
    constexpr int StepCount = 40;
    const DWORD stepDelay = static_cast<DWORD>(durationMs > 0 ? durationMs / StepCount : 1);
    LONG previousX = screenStart.x;
    LONG previousY = screenStart.y;
    for (int step = 0; step <= StepCount; ++step)
    {
        const double t = static_cast<double>(step) / StepCount;
        const double oneMinusT = 1.0 - t;
        const LONG x = static_cast<LONG>(oneMinusT * oneMinusT * screenStart.x + 2.0 * oneMinusT * t * control.x + t * t * screenEnd.x);
        const LONG y = static_cast<LONG>(oneMinusT * oneMinusT * screenStart.y + 2.0 * oneMinusT * t * control.y + t * t * screenEnd.y);

        INPUT inputs[2]{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        inputs[0].mi.dx = static_cast<LONG>(x * 65535 / GetSystemMetrics(SM_CXSCREEN));
        inputs[0].mi.dy = static_cast<LONG>(y * 65535 / GetSystemMetrics(SM_CYSCREEN));
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_MOVE;
        inputs[1].mi.dx = x - previousX;
        inputs[1].mi.dy = y - previousY;
        if (SendInput(2, inputs, sizeof(INPUT)) != 2)
        {
            errorText = QStringLiteral("发送鼠标移动输入失败");
            return false;
        }
        previousX = x;
        previousY = y;
        Sleep(stepDelay);
    }

    constexpr int ExtraDragSteps = 16;
    const LONG extraDragY = height / 5;
    for (int step = 0; step < ExtraDragSteps; ++step)
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dy = extraDragY;
        if (SendInput(1, &input, sizeof(INPUT)) != 1)
        {
            errorText = QStringLiteral("发送鼠标下拉输入失败");
            return false;
        }
        Sleep(20);
    }
    return true;
}

bool Win32InputDispatcher::pressKey(WORD virtualKey, QString& errorText) const
{
    Win32InputSequenceLock gate;
    if (!gate.acquire(errorText))
    {
        return false;
    }

    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtualKey;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtualKey;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    if (SendInput(2, inputs, sizeof(INPUT)) != 2)
    {
        errorText = QStringLiteral("发送键盘输入失败");
        return false;
    }
    return true;
}

bool Win32InputDispatcher::pressKeyChord(WORD modifierKey, WORD virtualKey, QString& errorText) const
{
    Win32InputSequenceLock gate;
    if (!gate.acquire(errorText))
    {
        return false;
    }

    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = modifierKey;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtualKey;
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = virtualKey;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = modifierKey;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(4, inputs, sizeof(INPUT)) != 4)
    {
        errorText = QStringLiteral("发送组合键失败");
        return false;
    }
    return true;
}

bool Win32InputDispatcher::pressKeyRepeated(WORD virtualKey, int count, int intervalMs, QString& errorText) const
{
    Win32InputSequenceLock gate;
    if (!gate.acquire(errorText))
    {
        return false;
    }

    if (count < 0 || intervalMs < 0)
    {
        errorText = QStringLiteral("重复按键参数无效");
        return false;
    }
    for (int index = 0; index < count; ++index)
    {
        if (!pressKey(virtualKey, errorText))
        {
            return false;
        }
        if (intervalMs > 0 && index + 1 < count)
        {
            Sleep(static_cast<DWORD>(intervalMs));
        }
    }
    return true;
}

bool Win32InputDispatcher::pasteText(HWND hwnd, const QString& text, QString& errorText) const
{
    Win32InputSequenceLock gate;
    if (!gate.acquire(errorText))
    {
        return false;
    }

    if (text.isEmpty())
    {
        errorText = QStringLiteral("待粘贴文本为空");
        return false;
    }
    if (!OpenClipboard(hwnd))
    {
        errorText = QStringLiteral("打开剪切板失败");
        return false;
    }

    EmptyClipboard();
    const SIZE_T byteCount = static_cast<SIZE_T>(text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!memory)
    {
        CloseClipboard();
        errorText = QStringLiteral("分配剪切板内存失败");
        return false;
    }

    void* target = GlobalLock(memory);
    if (!target)
    {
        GlobalFree(memory);
        CloseClipboard();
        errorText = QStringLiteral("锁定剪切板内存失败");
        return false;
    }
    std::memcpy(target, text.utf16(), static_cast<size_t>(text.size()) * sizeof(wchar_t));
    static_cast<wchar_t*>(target)[text.size()] = L'\0';
    GlobalUnlock(memory);

    // SetClipboardData 成功后由系统接管内存；上架文本和价格随后统一通过 Ctrl+V 输入。
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        CloseClipboard();
        errorText = QStringLiteral("写入剪切板失败");
        return false;
    }
    CloseClipboard();
    return pressKeyChord(VK_CONTROL, 'V', errorText);
}
QString Win32InputDispatcher::readClipboardText(QString& errorText) const
{
    if (!OpenClipboard(nullptr))
    {
        errorText = QStringLiteral("打开剪切板失败");
        return QString();
    }

    QString result;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data)
    {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text)
        {
            result = QString::fromWCharArray(text).trimmed();
            GlobalUnlock(data);
        }
    }
    CloseClipboard();

    if (result.isEmpty())
    {
        errorText = QStringLiteral("剪切板为空或不是文本");
    }
    return result;
}
