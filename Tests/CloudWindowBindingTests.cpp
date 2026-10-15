#include <QtWidgets>
#include <QTest>
#include "../DF_PlayClick_Client_Qt/AdbWindowOcr.h"
#include "../DF_PlayClick_Client_Qt/CloudWindowTarget.h"
#include "../DF_PlayClick_Client_Qt/CloudWindowPicker.h"
#include <cstdio>
static void require(bool ok, const char* message)
{ if (!ok) { fprintf(stderr, "FAIL %s\n", message); std::exit(1); } }
static void settle() { QTest::qWait(250); }
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QWidget owner, first, second;
    first.setWindowTitle("VM010001"); second.setWindowTitle("VM010002");
    first.resize(800,500); second.resize(850,540);
    first.show(); second.show(); owner.show(); settle();
    HWND a = reinterpret_cast<HWND>(first.winId()), b = reinterpret_cast<HWND>(second.winId());
    HWND own = reinterpret_cast<HWND>(owner.winId());
    auto windows = Win32WindowBinder().enumerateWindows(own);
    require(CloudWindowTarget::resolve(a, own, windows) == a, "resolve first VM");
    require(CloudWindowTarget::resolve(b, own, windows) == b, "resolve second VM");
    require(!CloudWindowTarget::resolve(own, own, windows), "reject client itself");
    require(!CloudWindowTarget::resolve(GetDesktopWindow(), own, windows), "reject desktop");
    QWidget generic, untitled;
    generic.setWindowTitle("ordinary application"); generic.show(); untitled.show(); settle();
    const HWND genericHwnd = reinterpret_cast<HWND>(generic.winId());
    const HWND untitledHwnd = reinterpret_cast<HWND>(untitled.winId());
    SetWindowTextW(untitledHwnd, L"");
    windows = Win32WindowBinder().enumerateWindows(own, true);
    require(CloudWindowTarget::resolve(genericHwnd, own, windows) == genericHwnd, "allow ordinary application");
    require(CloudWindowTarget::resolve(untitledHwnd, own, windows) == untitledHwnd, "allow untitled application in ADB");
    const auto pcWindows = Win32WindowBinder().enumerateWindows(own);
    require(!CloudWindowTarget::resolve(untitledHwnd, own, pcWindows), "PC enumeration keeps default untitled filtering");
    generic.hide(); untitled.hide();
    QWidget nested(&first); nested.setWindowTitle("VM010001 surface"); nested.resize(100,100); nested.show();
    HWND nestedHwnd = reinterpret_cast<HWND>(nested.winId());
    SetWindowTextW(nestedHwnd, L"VM010001 surface");
    QLabel navigation("navigation", &first); navigation.setWindowTitle("navigation"); navigation.show(); navigation.winId();
    windows = Win32WindowBinder().enumerateWindows(own);
    require(CloudWindowTarget::resolve(a, own, windows) == nestedHwnd, "title bar selects unique actual child surface");
    auto surfaces = CloudWindowTarget::surfaces(windows);
    bool foundB = false;
    for (const auto& w : surfaces) { if(w.hwnd == b) foundB = true; require(w.hwnd != reinterpret_cast<HWND>(navigation.winId()), "navigation not a phone"); }
    require(foundB, "second top-level phone not hidden by first child surface");
    nested.hide();
    const LONG_PTR styleA = GetWindowLongPtrW(a,GWL_STYLE), styleB = GetWindowLongPtrW(b,GWL_STYLE);
    AdbWindowOcr::SizeLock lockA, lockB, duplicate; QString error;
    require(lockA.bind(a,error), "first size lock"); settle();
    require(lockB.bind(b,error), "second size lock"); settle();
    require(!duplicate.bind(a,error), "reject duplicate ownership");
    for(int i=0;i<25;++i)
    {
        // Simulate a slow foreign app repeatedly replacing styles and dimensions.
        SetWindowLongPtrW(a,GWL_STYLE, GetWindowLongPtrW(a,GWL_STYLE) | WS_THICKFRAME | WS_MAXIMIZEBOX);
        first.resize(850+i,530); second.resize(900+i,570);
        require(lockA.enforce() && lockB.enforce(), "locks survive more than 15 consecutive resize corrections");
        QCoreApplication::processEvents();
    }
    for(int i=0;i<5;++i) { require(lockA.enforce() && lockB.enforce(), "settle both windows"); settle(); }
    AdbWindowOcr::Geometry ga,gb;
    require(AdbWindowOcr::geometry(a,ga,error) && AdbWindowOcr::geometry(b,gb,error)
        && ga.size==QSize(737,440) && gb.size==QSize(737,440), "both clients fixed 737x440");
    require(!(GetWindowLongPtrW(a,GWL_STYLE)&(WS_THICKFRAME|WS_MAXIMIZEBOX))
        && !(GetWindowLongPtrW(b,GWL_STYLE)&(WS_THICKFRAME|WS_MAXIMIZEBOX)), "both resize controls disabled");
    lockB.release();
    require(!(GetWindowLongPtrW(a,GWL_STYLE)&(WS_THICKFRAME|WS_MAXIMIZEBOX)), "removing second keeps first locked");
    require((GetWindowLongPtrW(b,GWL_STYLE)&(WS_THICKFRAME|WS_MAXIMIZEBOX)) == (styleB&(WS_THICKFRAME|WS_MAXIMIZEBOX)), "second original style restored");
    lockA.release();
    require((GetWindowLongPtrW(a,GWL_STYLE)&(WS_THICKFRAME|WS_MAXIMIZEBOX)) == (styleA&(WS_THICKFRAME|WS_MAXIMIZEBOX)), "first original style restored");
    CloudWindowPicker picker(&owner); picker.show(); owner.raise(); owner.activateWindow(); settle();
    int picked=0; QObject::connect(&picker,&CloudWindowPicker::windowPicked,[&](quintptr){++picked;});
    QTest::mousePress(&picker,Qt::LeftButton); require(QWidget::mouseGrabber()==&picker, "drag captures mouse");
    QTest::keyClick(&picker,Qt::Key_Escape); require(QWidget::mouseGrabber()!=&picker && picked==0, "Esc releases capture without binding");
    QTest::mousePress(&picker,Qt::LeftButton); picker.hide();
    require(QWidget::mouseGrabber()!=&picker && picked==0, "mode hide cancels drag");
    picker.show(); owner.move(10,10); first.move(350,100); first.raise(); settle();
    SetWindowPos(a, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    POINT previousCursor{}; GetCursorPos(&previousCursor);
    quintptr selected=0; QObject::connect(&picker,&CloudWindowPicker::windowPicked,[&](quintptr h){selected=h;});
    QTest::mousePress(&picker,Qt::LeftButton);
    {
        AdbWindowOcr::DpiScope dpi; POINT point{200,200}; ClientToScreen(a,&point); SetCursorPos(point.x,point.y);
        QTest::qWait(150);
        const HWND outline = FindWindowW(L"DFPlayClick.PickerOutline", nullptr);
        require(outline && IsWindowVisible(outline), "hover displays outline");
        RECT outlined{}, targetRect{}; GetWindowRect(outline,&outlined); GetWindowRect(a,&targetRect);
        require(EqualRect(&outlined,&targetRect), "outline matches selected target");
        require(WindowFromPoint(point)==a, "outline does not intercept hit test");
        QMouseEvent release(QEvent::MouseButtonRelease,QPointF(1,1),QPointF(point.x,point.y),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&picker,&release);
    }
    SetCursorPos(previousCursor.x,previousCursor.y);
    SetWindowPos(a, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    require(picked==1 && selected==reinterpret_cast<quintptr>(a) && QWidget::mouseGrabber()!=&picker, "drag release resolves actual phone and releases capture");
    require(!FindWindowW(L"DFPlayClick.PickerOutline", nullptr), "release removes outline");
    puts("PASS target resolution, independent dual-window locks, >15 resize retries, style restoration, drag capture and cancellation");
}
