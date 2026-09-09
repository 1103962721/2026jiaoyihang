from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "DF_PlayClick_Client_Qt"
SERVER = ROOT / "DF_PlayClick_Server_Qt"
SHARED = ROOT / "Shared"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_2k_ocr_defaults_and_separate_retry_reference():
    config_h = read(CLIENT / "ClientConfig.h")
    config_cpp = read(CLIENT / "ClientConfig.cpp")
    assert "retryReferenceWidth" in config_h
    assert "retryReferenceHeight" in config_h
    for expected in ['L"X", 2047', 'L"Y", 1126', 'L"Width", 400', 'L"Height", 80']:
        assert expected in config_cpp
    assert 'L"ReferenceWidth", 2560' in config_cpp
    assert 'L"ReferenceHeight", 1440' in config_cpp
    assert 'L"RetryReferenceWidth", 1650' in config_cpp
    assert 'L"RetryReferenceHeight", 1080' in config_cpp


def test_role_is_synchronized_and_drives_fixed_point_dispatch():
    protocol = read(SHARED / "Protocol.h")
    connection_h = read(CLIENT / "ClientConnection.h")
    connection_cpp = read(CLIENT / "ClientConnection.cpp")
    controller_h = read(CLIENT / "ClientController.h")
    controller_cpp = read(CLIENT / "ClientController.cpp")
    main_window = read(CLIENT / "MainWindow.cpp")
    manager_cpp = read(SERVER / "ServerConnectionManager.cpp")
    assert "BuildRoleMessage" in protocol
    assert "MessageType::Role" in protocol
    assert "roleReceived" in connection_h and "roleReceived" in connection_cpp
    assert "currentRole_" in controller_h
    assert "DispatchReferencePoint(currentRole_)" in controller_cpp
    assert "clickReferencePoint(boundHwnd_" in controller_cpp
    assert "clientRoleChanged" in controller_h and "clientRoleChanged" in main_window
    assert "BuildRoleMessage(client->role)" in manager_cpp
    assert "BuildRoleMessage(role)" in manager_cpp


def test_server_dispatches_tight_groups_with_precise_inter_group_delay():
    manager_h = read(SERVER / "ServerConnectionManager.h")
    manager_cpp = read(SERVER / "ServerConnectionManager.cpp")
    controller_h = read(SERVER / "ServerController.h")
    controller_cpp = read(SERVER / "ServerController.cpp")
    main_window = read(SERVER / "MainWindow.cpp")
    assert "triggerReadyClientsByRole" in manager_h and "triggerReadyClientsByRole" in manager_cpp
    assert "ClientRole role" in manager_h
    assert "Qt::PreciseTimer" in controller_cpp
    assert "groupDelayMs" in controller_cpp
    assert "coinFirst" in controller_h
    assert "coinFirstRadio_->isChecked()" in main_window
    assert "groupDelayEdit_->text()" in main_window


if __name__ == "__main__":
    tests = [
        test_2k_ocr_defaults_and_separate_retry_reference,
        test_role_is_synchronized_and_drives_fixed_point_dispatch,
        test_server_dispatches_tight_groups_with_precise_inter_group_delay,
    ]
    for test in tests:
        test()
    print(f"PASS: {len(tests)} role/coordinate contract tests")
