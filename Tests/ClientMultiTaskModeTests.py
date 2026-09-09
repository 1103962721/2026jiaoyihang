from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read_text(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8-sig")


def test_qt_client_exposes_multi_task_state_and_actions():
    header = read_text("DF_PlayClick_Client_Qt/ClientController.h")
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")

    assert "struct ClickTask" in header
    assert "QVector<ClickTask> tasks_" in header
    assert "QVector<ClickTask> tasks() const" in header
    assert "void tasksChanged()" in header
    assert "void selectedTaskChanged(int taskIndex)" in header
    assert "void addTaskFromWindow(int index, bool bringToFront)" in header
    assert "void clickEnabledTasks()" in header

    assert "QVector<ClickTask> ClientController::tasks() const" in source
    assert "int ClientController::selectedTaskIndex() const" in source
    assert "void ClientController::addTaskFromWindow" in source
    assert "void ClientController::selectTask" in source
    assert "void ClientController::removeSelectedTask" in source
    assert "void ClientController::moveSelectedTaskUp" in source
    assert "void ClientController::moveSelectedTaskDown" in source
    assert "void ClientController::toggleSelectedTaskEnabled" in source
    assert "bool ClientController::hasEnabledTasks() const" in source
    assert "void ClientController::clickEnabledTasks()" in source
    assert "syncBoundWindowFromSelectedTask" in source
    assert "clickEnabledTasks();" in source
    assert "clickBoundPoint" not in source
    assert "setReady(false);\n    setStatus(QStringLiteral(\"已排队点击绑定窗口坐标\"))" not in source


def test_qt_client_ui_contains_multi_task_layout_and_bindings():
    header = read_text("DF_PlayClick_Client_Qt/MainWindow.h")
    source = read_text("DF_PlayClick_Client_Qt/MainWindow.cpp")

    assert "QTableWidget* tasksTable_" in header
    assert "void updateTasksTable()" in header
    assert "void updateTaskEditor(int taskIndex)" in header
    assert "QLabel* currentTaskLabel_" in header
    assert "QPushButton* taskToggleButton_" in header

    assert "点击任务列表" in source
    assert "可选目标窗口" in source
    assert "添加为任务" in source
    assert "上移" in source
    assert "下移" in source
    assert "删除" in source
    assert "启用" in source
    assert "tasksTable_" in source
    assert "controller_.addTaskFromWindow" in source
    assert "controller_.selectTask" in source
    assert "controller_.moveSelectedTaskUp" in source
    assert "controller_.moveSelectedTaskDown" in source
    assert "controller_.removeSelectedTask" in source
    assert "controller_.toggleSelectedTaskEnabled" in source
    assert "&ClientController::tasksChanged" in source
    assert "&ClientController::selectedTaskChanged" in source
    assert "rightColumn->addWidget(createCard(QStringLiteral(\"当前任务编辑\"), currentTaskCardLayout))" in source
    assert "centerLayout->addWidget(createCard(QStringLiteral(\"当前任务编辑\"), currentTaskCardLayout))" not in source
    assert "rightPanel->setMinimumWidth(330)" in source
    assert "boundLabel_" not in source


def test_auto_pick_supports_1024_768_rule():
    rules = read_text("DF_PlayClick_Client/ClientBindingRules.h")
    tests = read_text("Tests/ClientBindingRulesTests.cpp")

    assert "{1024, 768, 1159, 692}" in rules
    assert "{1024, 768, 1159, 692}" in tests
