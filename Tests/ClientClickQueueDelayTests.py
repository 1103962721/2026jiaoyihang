from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT_QT = ROOT / "DF_PlayClick_Client_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_click_queue_delay_uses_shared_config_default_50_without_legacy_ui():
    config_h = read_text(CLIENT_QT / "ClientConfig.h")
    config_cpp = read_text(CLIENT_QT / "ClientConfig.cpp")
    controller_h = read_text(CLIENT_QT / "ClientController.h")
    controller_cpp = read_text(CLIENT_QT / "ClientController.cpp")
    dispatcher_h = read_text(CLIENT_QT / "Win32InputDispatcher.h")
    dispatcher_cpp = read_text(CLIENT_QT / "Win32InputDispatcher.cpp")
    window_h = read_text(CLIENT_QT / "MainWindow.h")
    window_cpp = read_text(CLIENT_QT / "MainWindow.cpp")

    assert "DefaultClickQueueDelayMs = 50" in config_h
    assert "ClickQueue" in config_cpp
    assert "DelayMs" in config_cpp
    assert "loadClickQueueDelayMs" in config_h
    assert "saveClickQueueDelayMs" in config_h

    assert "setClickQueueDelayMs" in controller_h
    assert "clickQueueDelayChanged" in controller_h
    assert "WM_DF_PLAYCLICK_DELAY_CHANGED" in controller_cpp
    assert "RegisterWindowMessageW" in controller_cpp
    assert "PostMessageW(HWND_BROADCAST" in controller_cpp
    assert "nativeEventFilter" in controller_h
    assert "qApp->installNativeEventFilter(this)" in controller_cpp

    assert "clickClientPoint(HWND hwnd, POINT point, int releaseDelayMs" in dispatcher_h
    assert "Sleep(static_cast<DWORD>(releaseDelayMs))" in dispatcher_cpp
    assert "ClickQueueReleaseDelayMs" not in dispatcher_cpp

    assert "delayEdit_" not in window_h
    assert "点击间隔(ms)" not in window_cpp
    assert "controller_.setClickQueueDelayMs" not in window_cpp
    assert "unitPriceEdit_" in window_h
    assert "bulletTotalEdit_" in window_h
