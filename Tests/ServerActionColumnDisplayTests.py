from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_action_buttons_are_stable_and_do_not_rebuild_for_running_state():
    main_window_cpp = read_text(SERVER_QT / "MainWindow.cpp")

    assert "createActionCellWidget" in main_window_cpp
    assert "clientTable_->cellWidget(row, ColumnActions)" in main_window_cpp
    assert "if (!clientTable_->cellWidget(row, ColumnActions))" in main_window_cpp
    assert "button->setObjectName(QStringLiteral(\"compactActionButton\"))" in main_window_cpp
    assert "button->setText(runningThisAction ? QStringLiteral(\"执行中\") : text)" not in main_window_cpp
    assert "button->setEnabled(!client.actionRunning || runningThisAction)" not in main_window_cpp
    assert "client.actionRunning ? QStringLiteral(\"执行中...\") : QStringLiteral(\"出发\")" not in main_window_cpp
    assert "client.actionRunning ? QStringLiteral(\"执行中...\") : QStringLiteral(\"获取组队码\")" not in main_window_cpp
    assert "client.actionRunning ? QStringLiteral(\"执行中...\") : QStringLiteral(\"识别队伍状态\")" not in main_window_cpp


if __name__ == "__main__":
    test_action_buttons_are_stable_and_do_not_rebuild_for_running_state()
