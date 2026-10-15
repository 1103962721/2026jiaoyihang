#include "CloudWindowPicker.h"
#include "CloudWindowTarget.h"
#include "AdbWindowOcr.h"
#include <QMouseEvent>
#include <QKeyEvent>

CloudWindowPicker::CloudWindowPicker(QWidget* parent) : QPushButton(parent)
{
    setText(QStringLiteral("⊕ 拖动绑定窗口"));
    setToolTip(QStringLiteral("拖到目标窗口，蓝框显示选中范围；松开确认后输入 ADB 端口；Esc 取消"));
    setFocusPolicy(Qt::StrongFocus);
    outlineTimer_.setInterval(80);
    connect(&outlineTimer_, &QTimer::timeout, this, &CloudWindowPicker::updateOutline);
}
void CloudWindowPicker::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) { QPushButton::mousePressEvent(event); return; }
    candidates_ = Win32WindowBinder().enumerateWindows(reinterpret_cast<HWND>(window()->winId()), true);
    picking_ = true;
    setFocus(); setDown(true);
    setText(QStringLiteral("松开绑定 · Esc 取消"));
    grabMouse(Qt::CrossCursor);
    outlineTimer_.start();
    updateOutline();
    event->accept();
}
void CloudWindowPicker::mouseReleaseEvent(QMouseEvent* event)
{
    if (!picking_ || event->button() != Qt::LeftButton) { QPushButton::mouseReleaseEvent(event); return; }
    const auto target = targetAtCursor();
    cancelPick();
    if (target) emit windowPicked(target);
    else emit pickFailed(QStringLiteral("没有选中应用窗口，请拖到目标窗口上松开"));
    event->accept();
}
void CloudWindowPicker::cancelPick()
{
    outlineTimer_.stop();
    candidates_.clear();
    if (outline_) { DestroyWindow(reinterpret_cast<HWND>(outline_)); outline_ = 0; }
    if (!picking_) return;
    picking_ = false;
    releaseMouse(); setDown(false);
    setText(QStringLiteral("⊕ 拖动绑定窗口"));
}
void CloudWindowPicker::keyPressEvent(QKeyEvent* event)
{
    if (picking_ && event->key() == Qt::Key_Escape) { cancelPick(); event->accept(); return; }
    QPushButton::keyPressEvent(event);
}
bool CloudWindowPicker::event(QEvent* event)
{
    if (picking_ && (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse
        || event->type() == QEvent::WindowDeactivate)) cancelPick();
    return QPushButton::event(event);
}

CloudWindowPicker::~CloudWindowPicker() { cancelPick(); }

quintptr CloudWindowPicker::targetAtCursor() const
{
    AdbWindowOcr::DpiScope dpi;
    POINT point{};
    if (!GetCursorPos(&point)) return 0;
    const HWND excluded = reinterpret_cast<HWND>(window()->winId());
    return reinterpret_cast<quintptr>(CloudWindowTarget::resolve(WindowFromPoint(point), excluded, candidates_));
}

void CloudWindowPicker::updateOutline()
{
    AdbWindowOcr::DpiScope dpi;
    const HWND target = reinterpret_cast<HWND>(targetAtCursor());
    RECT rect{};
    if (!target || !GetWindowRect(target, &rect) || IsIconic(target))
    {
        if (outline_) ShowWindow(reinterpret_cast<HWND>(outline_), SW_HIDE);
        return;
    }
    if (!outline_)
    {
        static const ATOM registered = [] {
            WNDCLASSW wc{};
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"DFPlayClick.PickerOutline";
            wc.lpfnWndProc = DefWindowProcW;
            wc.hbrBackground = CreateSolidBrush(RGB(0, 145, 255));
            return RegisterClassW(&wc);
        }();
        if (!registered) return;
        outline_ = reinterpret_cast<quintptr>(CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            L"DFPlayClick.PickerOutline", L"", WS_POPUP, 0, 0, 0, 0,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
        if (!outline_) return;
        SetLayeredWindowAttributes(reinterpret_cast<HWND>(outline_), 0, 230, LWA_ALPHA);
    }
    const HWND overlay = reinterpret_cast<HWND>(outline_);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HRGN border = CreateRectRgn(0, 0, width, height);
    HRGN inside = CreateRectRgn(4, 4, width - 4, height - 4);
    CombineRgn(border, border, inside, RGN_DIFF);
    DeleteObject(inside);
    if (!SetWindowRgn(overlay, border, TRUE)) DeleteObject(border);
    SetWindowPos(overlay, HWND_TOPMOST, rect.left, rect.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
