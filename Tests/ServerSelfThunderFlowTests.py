from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"
CLIENT_QT = ROOT / "DF_PlayClick_Client_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_self_thunder_protocol_is_wired_end_to_end():
    server_manager_h = read_text(SERVER_QT / "ServerConnectionManager.h")
    server_manager_cpp = read_text(SERVER_QT / "ServerConnectionManager.cpp")
    server_controller_h = read_text(SERVER_QT / "ServerController.h")
    server_controller_cpp = read_text(SERVER_QT / "ServerController.cpp")
    client_connection_h = read_text(CLIENT_QT / "ClientConnection.h")
    client_connection_cpp = read_text(CLIENT_QT / "ClientConnection.cpp")
    client_controller_h = read_text(CLIENT_QT / "ClientController.h")
    client_controller_cpp = read_text(CLIENT_QT / "ClientController.cpp")
    input_h = read_text(CLIENT_QT / "Win32InputDispatcher.h")
    input_cpp = read_text(CLIENT_QT / "Win32InputDispatcher.cpp")
    main_window_cpp = read_text(SERVER_QT / "MainWindow.cpp")

    assert "requestSelfThunder" in server_manager_h
    assert "selfThunderResponseReceived" in server_manager_h
    assert "BuildSelfThunderRequestMessage" in server_manager_cpp
    assert "MessageType::SelfThunderResponse" in server_manager_cpp
    assert "自雷中" in server_manager_cpp

    assert "pendingSelfThunderRequests_" in server_controller_h
    assert "SelfThunder" in server_controller_h
    assert "handleSelfThunderResponse" in server_controller_cpp
    assert "requestSelfThunder" in server_controller_cpp
    assert "自雷完成" in server_controller_cpp
    assert "自雷（暂未实现）" not in main_window_cpp
    assert "showSelfThunderPlaceholder(clientId)" not in main_window_cpp
    assert "controller_.requestSelfThunder(clientId)" in main_window_cpp

    assert "sendSelfThunderResponse" in client_connection_h
    assert "selfThunderRequestReceived" in client_connection_h
    assert "MessageType::SelfThunderRequest" in client_connection_cpp

    assert "handleSelfThunderRequest" in client_controller_h
    assert "executeSelfThunderSequence" in client_controller_h
    assert "moveReferencePointBezier" in input_h
    assert "Bezier" in input_cpp or "贝塞尔" in input_cpp
    assert "VK_SPACE" in client_controller_cpp
    assert "VK_TAB" in client_controller_cpp
    assert "1273" in client_controller_cpp and "847" in client_controller_cpp
    assert "1494" in client_controller_cpp and "895" in client_controller_cpp


if __name__ == "__main__":
    test_self_thunder_protocol_is_wired_end_to_end()
