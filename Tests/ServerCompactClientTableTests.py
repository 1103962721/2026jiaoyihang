from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_server_client_table_uses_compact_five_column_layout_and_keeps_context_menu():
    main_window_cpp = read_text(SERVER_QT / "MainWindow.cpp")

    assert "QTableWidget(0, 5" in main_window_cpp
    assert "ColumnClient = 0" in main_window_cpp
    assert "ColumnStatus = 1" in main_window_cpp
    assert "ColumnTeamReady = 2" in main_window_cpp
    assert "ColumnTeamCode = 3" in main_window_cpp
    assert "ColumnActions = 4" in main_window_cpp
    assert "客户端" in main_window_cpp
    assert "队伍准备状态" in main_window_cpp
    assert "组队码" in main_window_cpp
    assert "操作" in main_window_cpp
    assert "备注：%2" in main_window_cpp
    assert "setDefaultSectionSize(64)" in main_window_cpp
    assert "setRowHeight(row, 64)" in main_window_cpp
    assert "createActionCellWidget" in main_window_cpp
    assert "出发" in main_window_cpp
    assert "组队码" in main_window_cpp
    assert "识别" in main_window_cpp
    assert "自雷" in main_window_cpp
    assert "setContextMenuPolicy(Qt::CustomContextMenu)" in main_window_cpp
    assert "customContextMenuRequested" in main_window_cpp
    assert "showClientMenu" in main_window_cpp


def test_old_action_columns_are_removed_from_table_layout():
    main_window_cpp = read_text(SERVER_QT / "MainWindow.cpp")

    assert "ColumnIp" not in main_window_cpp
    assert "ColumnRemark" not in main_window_cpp
    assert "ColumnTrigger" not in main_window_cpp
    assert "ColumnFetchTeamCode" not in main_window_cpp
    assert "ColumnFetchTeamReady" not in main_window_cpp
    assert "ColumnSelfThunder" not in main_window_cpp


if __name__ == "__main__":
    test_server_client_table_uses_compact_five_column_layout_and_keeps_context_menu()
    test_old_action_columns_are_removed_from_table_layout()
