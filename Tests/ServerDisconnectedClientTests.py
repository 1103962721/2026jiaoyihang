from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_disconnected_clients_are_hidden_from_server_list():
    manager_cpp = read_text(SERVER_QT / "ServerConnectionManager.cpp")
    main_window_cpp = read_text(SERVER_QT / "MainWindow.cpp")

    assert "if (!client->connected)" in manager_cpp
    assert "continue;" in manager_cpp
    assert "result.reserve(static_cast<int>(clients_.size()))" not in manager_cpp
    assert "已断开" not in main_window_cpp
