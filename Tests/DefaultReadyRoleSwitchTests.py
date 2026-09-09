from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "DF_PlayClick_Client_Qt"
SERVER = ROOT / "DF_PlayClick_Server_Qt"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_client_automatically_sends_ready_after_every_hello():
    connection_h = read(CLIENT / "ClientConnection.h")
    connection_cpp = read(CLIENT / "ClientConnection.cpp")
    controller_cpp = read(CLIENT / "ClientController.cpp")

    assert "bool desiredReady_ = true;" in connection_h
    assert "Protocol::BuildReadyMessage(true)" in connection_cpp
    assert "reconnecting && desiredReady_" not in connection_cpp
    assert connection_cpp.index("Protocol::BuildHelloMessage()") < connection_cpp.index("Protocol::BuildReadyMessage(true)")
    assert connection_cpp.index("Protocol::BuildReadyMessage(true)") < connection_cpp.index("emit connectedChanged(true)")
    assert "ready_ = true;" in controller_cpp


def test_server_uses_four_columns_without_ready_status():
    source = read(SERVER / "MainWindow.cpp")

    assert "new QTableWidget(0, 4" in source
    assert 'QStringLiteral("客户端 IP"), QStringLiteral("备注类型"),' in source
    assert 'QStringLiteral("点击坐标"), QStringLiteral("操作")' in source
    assert "statusItem" not in source
    assert 'QStringLiteral("已准备")' not in source
    assert 'QStringLiteral("未准备")' not in source


def test_server_balances_table_columns_and_preserves_operation_width():
    source = read(SERVER / "MainWindow.cpp")

    assert "setSectionResizeMode(0, QHeaderView::Stretch)" in source
    assert "setSectionResizeMode(1, QHeaderView::Stretch)" in source
    assert "setSectionResizeMode(2, QHeaderView::Stretch)" in source
    assert "setSectionResizeMode(3, QHeaderView::Fixed)" in source
    assert "resizeSection(3, 92)" in source

def test_server_centers_a_compact_role_capsule_without_stretching_it():
    source = read(SERVER / "MainWindow.cpp")

    assert "auto* roleCell = new QWidget(clientTable_)" in source
    assert "auto* roleSelector = new QWidget(roleCell)" in source
    assert "roleSelector->setFixedWidth(132)" in source
    assert "roleCellLayout->addWidget(roleSelector, 0, Qt::AlignCenter)" in source
    assert "setCellWidget(row, 1, roleCell)" in source

def test_server_uses_capsule_role_selector_and_inline_coordinates():
    header = read(SERVER / "MainWindow.h")
    source = read(SERVER / "MainWindow.cpp")

    assert "QComboBox" not in header
    assert "QComboBox" not in source
    assert "QButtonGroup" in source
    assert 'setObjectName(QStringLiteral("roleSelector"))' in source
    assert 'setObjectName(QStringLiteral("coinRoleButton"))' in source
    assert 'setObjectName(QStringLiteral("bossRoleButton"))' in source
    assert "setCheckable(true)" in source
    assert 'QStringLiteral("X: 1756  Y: 1000")' in source
    assert 'QStringLiteral("X: 2262  Y: 1159")' in source
    assert "#2563eb" in source
    assert "#7653c9" in source


if __name__ == "__main__":
    tests = [
        test_client_automatically_sends_ready_after_every_hello,
        test_server_uses_four_columns_without_ready_status,
        test_server_balances_table_columns_and_preserves_operation_width,
        test_server_centers_a_compact_role_capsule_without_stretching_it,
        test_server_uses_capsule_role_selector_and_inline_coordinates,
    ]
    for test in tests:
        test()
    print(f"PASS: {len(tests)} default-ready/role-switch contract tests")