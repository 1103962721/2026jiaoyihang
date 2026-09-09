from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT_QT = ROOT / "DF_PlayClick_Client_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_client_startup_guards_run_before_normal_window_but_missing_game_does_not_exit():
    main_cpp = read_text(CLIENT_QT / "main.cpp")
    controller_h = read_text(CLIENT_QT / "ClientController.h")
    controller_cpp = read_text(CLIENT_QT / "ClientController.cpp")
    window_cpp = read_text(CLIENT_QT / "MainWindow.cpp")

    assert "QApplication app(argc, argv);" in main_cpp
    assert main_cpp.index("QApplication app(argc, argv);") < main_cpp.index("if (!isRunningAsAdministrator())")
    assert main_cpp.index("return 0;") < main_cpp.index("MainWindow window;")
    assert 'showStartupMessage(QStringLiteral("请以管理员身份运行客户端"), true);' in main_cpp
    assert 'showStartupMessage(QStringLiteral("请将 Windows 显示缩放设置为 100% 后再运行"), true);' in main_cpp

    assert "bool initialize(HWND mainWindowHandle);" in controller_h
    assert "bool ClientController::initialize(HWND mainWindowHandle)" in controller_cpp
    assert "const bool bound = autoBindGameWindow(false);" in controller_cpp
    assert "return bound;" in controller_cpp

    assert "controller_.initialize(reinterpret_cast<HWND>(winId()));" in window_cpp
    assert "QApplication::quit" not in window_cpp
    assert "请先运行游戏再启动软件" not in window_cpp


def test_startup_dialog_has_room_and_controller_messages_use_standard_qt_dialogs():
    main_cpp = read_text(CLIENT_QT / "main.cpp")
    window_cpp = read_text(CLIENT_QT / "MainWindow.cpp")

    assert "messageBox.setMinimumWidth(360);" in main_cpp
    assert "button->setMinimumWidth(96);" in main_cpp
    assert "QMessageBox::critical" in window_cpp
    assert "QMessageBox::information" in window_cpp
