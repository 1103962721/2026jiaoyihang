from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_single_client_force_clear_status_is_visible_and_resets_auto_state():
    manager_cpp = read_text(SERVER_QT / "ServerConnectionManager.cpp")
    controller_cpp = read_text(SERVER_QT / "ServerController.cpp")
    main_window_cpp = read_text(SERVER_QT / "MainWindow.cpp")

    assert "forceResetClientAction" in manager_cpp
    assert "client->actionRunning = false" in manager_cpp
    assert "client->runningAction.clear()" in manager_cpp
    assert "teamReadyUpdatedAtMs = 0" in manager_cpp
    assert "匹配中" in manager_cpp or "正在加载" in manager_cpp or "进入对局" in manager_cpp
    assert "resetAutoStartClient" in controller_cpp
    assert "强制清零状态" in main_window_cpp
    assert "controller_.forceResetClientAction(clientId)" in main_window_cpp


if __name__ == "__main__":
    test_single_client_force_clear_status_is_visible_and_resets_auto_state()
