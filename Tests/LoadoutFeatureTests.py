from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"
CLIENT_QT = ROOT / "DF_PlayClick_Client_Qt"
SHARED = ROOT / "Shared"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_server_ui_exposes_loadout_action_and_remark_type_choices():
    main_window_cpp = read_text(SERVER_QT / "MainWindow.cpp")
    controller_h = read_text(SERVER_QT / "ServerController.h")

    assert "requestLoadout" in controller_h
    assert "配装" in main_window_cpp
    assert "triggerClientAction(id, actionName, false)" in main_window_cpp
    assert "controller_.requestLoadout(clientId)" in main_window_cpp
    assert "QStringLiteral(\"普通\")" in main_window_cpp
    assert "QStringLiteral(\"机密\")" in main_window_cpp
    assert "QInputDialog::getItem" in main_window_cpp
    assert "客户端备注" not in main_window_cpp


def test_server_connection_manager_sends_loadout_and_defaults_remark_to_normal():
    manager_h = read_text(SERVER_QT / "ServerConnectionManager.h")
    manager_cpp = read_text(SERVER_QT / "ServerConnectionManager.cpp")

    assert "requestLoadout" in manager_h
    assert "loadoutResponseReceived" in manager_h
    assert "QStringLiteral(\"普通\")" in manager_h
    assert "BuildLoadoutRequestMessage" in manager_cpp
    assert "runningAction = QStringLiteral(\"配装\")" in manager_cpp
    assert "MessageType::LoadoutResponse" in manager_cpp


def test_auto_start_runs_loadout_before_triggering_clients():
    controller_h = read_text(SERVER_QT / "ServerController.h")
    controller_cpp = read_text(SERVER_QT / "ServerController.cpp")

    assert "Loadout" in controller_h
    assert "pendingLoadoutRequestId" in controller_h
    assert "pendingLoadoutRequests_" in controller_h
    assert "handleLoadoutResponse" in controller_cpp
    assert "requestLoadout(client.id, stateIterator.value())" in controller_cpp
    assert "allLoadoutFinished" in controller_cpp
    assert "triggerClients(autoStartCountdownText_)" in controller_cpp
    assert "自动开始：队伍已满员，已发送配装指令" in controller_cpp
    assert "自动开始：配装完成，已执行全局出发" in controller_cpp
    assert "自动开始：队伍已满员，已执行全局出发" not in controller_cpp


def test_client_executes_loadout_sequence_with_reference_coordinates_and_type_branch():
    connection_h = read_text(CLIENT_QT / "ClientConnection.h")
    connection_cpp = read_text(CLIENT_QT / "ClientConnection.cpp")
    controller_h = read_text(CLIENT_QT / "ClientController.h")
    controller_cpp = read_text(CLIENT_QT / "ClientController.cpp")

    assert "loadoutRequestReceived" in connection_h
    assert "sendLoadoutResponse" in connection_h
    assert "MessageType::LoadoutRequest" in connection_cpp
    assert "handleLoadoutRequest" in controller_h
    assert "executeLoadoutSequence" in controller_h
    assert "const POINT openLoadout{ 2063, 1290 }" in controller_cpp
    assert "input_.pressKey('L', errorText)" in controller_cpp
    assert "const POINT normalOption{ 220, 569 }" in controller_cpp
    assert "const POINT secretOption{ 216, 666 }" in controller_cpp
    assert "const POINT firstConfirm{ 2263, 1165 }" in controller_cpp
    assert "const POINT secondConfirm{ 2260, 1278 }" in controller_cpp
    assert "const POINT finalConfirm{ 1282, 936 }" in controller_cpp
    assert "ReferenceWidth = 2560" in controller_cpp
    assert "ReferenceHeight = 1440" in controller_cpp
    assert "QThread::msleep(1000)" in controller_cpp


if __name__ == "__main__":
    test_server_ui_exposes_loadout_action_and_remark_type_choices()
    test_server_connection_manager_sends_loadout_and_defaults_remark_to_normal()
    test_auto_start_runs_loadout_before_triggering_clients()
    test_client_executes_loadout_sequence_with_reference_coordinates_and_type_branch()
