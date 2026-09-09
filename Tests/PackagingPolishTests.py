from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT_QT = ROOT / "DF_PlayClick_Client_Qt"
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_team_ready_request_keeps_previous_display_text():
    manager_cpp = read_text(SERVER_QT / "ServerConnectionManager.cpp")
    request_body = manager_cpp[manager_cpp.index("bool ServerConnectionManager::requestTeamReady") : manager_cpp.index("bool ServerConnectionManager::requestOcr")]

    assert "runningAction = QStringLiteral(\"识别队伍状态\")" in request_body
    assert "teamReadyText = QStringLiteral(\"识别中...\")" not in request_body
    assert "teamReadyUpdatedAtMs = 0" not in request_body
    assert "teamReadyState = -1" not in request_body


def test_umiocr_path_is_package_relative_not_developer_absolute_default():
    config_cpp = read_text(CLIENT_QT / "ClientConfig.cpp")

    assert "D:\\IDMDownLoad\\修复" not in config_cpp
    assert "QFileInfo" in config_cpp
    assert "applicationDirPath" in config_cpp
    assert "Umi-OCR_Paddle_v2.1.5" in config_cpp
    assert "Umi-OCR" in config_cpp
    assert "Umi-OCR.exe" in config_cpp


def test_free_sponsor_warning_text_removed_from_qt_apps():
    for folder in (CLIENT_QT, SERVER_QT):
        sponsor_cpp = read_text(folder / "SponsorDialog.cpp")
        main_cpp = read_text(folder / "main.cpp")
        qrc = read_text(folder / "AppIcon.qrc")
        assert "本软件完全免费" not in sponsor_cpp
        assert "一块不嫌少" not in sponsor_cpp
        assert "SponsorWechat.png" not in sponsor_cpp
        assert "qrPixmap" not in sponsor_cpp
        assert "SponsorDialog" not in main_cpp
        assert "SponsorWechat.png" not in qrc


if __name__ == "__main__":
    test_team_ready_request_keeps_previous_display_text()
    test_umiocr_path_is_package_relative_not_developer_absolute_default()
    test_free_sponsor_warning_text_removed_from_qt_apps()
