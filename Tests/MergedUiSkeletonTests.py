from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "DF_PlayClick_Server_Qt"
CLIENT = ROOT / "DF_PlayClick_Client_Qt"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_server_exposes_grouped_trigger_and_listing_controls():
    header = read(SERVER / "MainWindow.h")
    source = read(SERVER / "MainWindow.cpp")
    assert "groupDelayEdit_" in header
    assert "coinFirstRadio_" in header
    assert "bossFirstRadio_" in header
    assert "listingButton_" in header
    assert "listingTextEdit_" in header
    assert 'QStringLiteral("上架文本")' in source
    assert 'setPlaceholderText(QStringLiteral("请输入后续上架使用的文本"))' in source
    assert "全部出发" in source
    assert "上架" in source
    assert "币号优先" in source
    assert "老板优先" in source
    assert "QComboBox" not in header
    assert "QButtonGroup" in source
    assert "roleSelector" in source
    assert "倒计时" in source
    assert "分组延迟" in source
    assert "流程预览" in source
    assert "币号 → 延迟 → 老板" in source


def test_client_exposes_connection_and_local_ocr_parameters_without_picker_ui():
    header = read(CLIENT / "MainWindow.h")
    source = read(CLIENT / "MainWindow.cpp")
    assert "unitPriceEdit_" in header
    assert "bulletTotalEdit_" in header
    assert "OCR 上架判定" in source
    assert "连接服务器" in source
    assert "windowsTable_" not in header
    assert "tasksTable_" not in header
    assert "autoPointButton_" not in header
    assert "ocrValueLabel_" in header
    assert "unitValueLabel_" in header
    assert "ocrDecisionLabel_" in header
    assert "clientRoleLabel_" in header
    assert "executionStatusLabel_" in header
    assert "窗口绑定" not in source
    assert "任务列表" not in source
    assert "自动取点" not in source


def test_listing_stub_only_targets_coin_clients():
    manager_h = read(SERVER / "ServerConnectionManager.h")
    manager_cpp = read(SERVER / "ServerConnectionManager.cpp")
    controller_cpp = read(SERVER / "ServerController.cpp")
    assert "setClientRole" in manager_h
    assert "coinClientCount" in manager_h
    assert "ClientRole::Coin" in manager_cpp
    assert "listingPlaceholder" in controller_cpp
    assert "上架功能暂未实现，当前仅对币号生效" in controller_cpp


def test_client_ocr_parameters_are_persisted_locally():
    config_h = read(CLIENT / "ClientConfig.h")
    config_cpp = read(CLIENT / "ClientConfig.cpp")
    controller_h = read(CLIENT / "ClientController.h")
    assert "loadUnitPrice" in config_h
    assert "saveUnitPrice" in config_h
    assert "loadBulletTotal" in config_h
    assert "saveBulletTotal" in config_h
    assert 'L"Ocr"' in config_cpp
    assert 'L"UnitPrice"' in config_cpp
    assert 'L"BulletTotal"' in config_cpp
    assert "setOcrParameters" in controller_h

def test_shared_frameless_window_chrome_contract():
    title_h_path = ROOT / "Shared" / "FramelessTitleBar.h"
    title_cpp_path = ROOT / "Shared" / "FramelessTitleBar.cpp"
    support_h_path = ROOT / "Shared" / "FramelessWindowSupport.h"
    assert title_h_path.exists(), "缺少共享无边框标题栏声明"
    assert title_cpp_path.exists(), "缺少共享无边框标题栏实现"
    assert support_h_path.exists(), "缺少无边框窗口缩放支持"
    title_h = read(title_h_path)
    title_cpp = read(title_cpp_path)
    support_h = read(support_h_path)
    assert "class FramelessTitleBar" in title_h
    assert "startSystemMove" in title_cpp
    assert "showMinimized" in title_cpp
    assert "showMaximized" in title_cpp
    assert "showNormal" in title_cpp
    assert "WM_NCHITTEST" in support_h


def test_server_matches_approved_visual_contract():
    header = read(SERVER / "MainWindow.h")
    source = read(SERVER / "MainWindow.cpp")
    assert "nativeEvent" in header
    assert "resize(850, 760)" in source
    assert "setMinimumSize(790, 700)" in source
    assert "clientTable_->setMinimumHeight(160)" in source
    assert "connectionLogEdit_->setMinimumHeight(120)" in source
    assert "coinFirstRadio_" in header
    assert "bossFirstRadio_" in header
    assert "triggerOrderCombo_" not in header
    assert "triggerOrderCombo_" not in source
    assert "QRadioButton" in header
    assert 'QStringLiteral("服务端调度中心")' in source
    assert 'QStringLiteral("启动监听")' in source
    assert 'QStringLiteral("停止监听")' in source
    assert source.count("setMinimumWidth(74)") >= 2
    assert "#102a5c" in source and "#245bc4" in source
    assert "#0f172a" in source and "#a7f3d0" in source
    assert "当前为调度 UI 骨架" not in source


def test_client_matches_approved_visual_contract():
    header = read(CLIENT / "MainWindow.h")
    source = read(CLIENT / "MainWindow.cpp")
    assert "nativeEvent" in header
    assert "resize(620, 760)" in source
    assert "setMinimumSize(580, 720)" in source
    assert "connectionBadge_->setMinimumWidth(68)" in source
    assert "QSizePolicy::Fixed" in source
    assert "setMinimumSize" in source
    assert "disconnectButton_" in header
    assert 'QStringLiteral("连接")' in source
    assert 'QStringLiteral("断开")' in source
    assert 'QStringLiteral("客户端执行端")' in source
    assert "ocrValueLabel_" in header
    assert "unitValueLabel_" in header
    assert "configuredUnitPriceLabel_" in header
    assert "#113f3e" in source and "#16817b" in source

if __name__ == "__main__":
    tests = [
        test_server_exposes_grouped_trigger_and_listing_controls,
        test_client_exposes_connection_and_local_ocr_parameters_without_picker_ui,
        test_listing_stub_only_targets_coin_clients,
        test_client_ocr_parameters_are_persisted_locally,
        test_shared_frameless_window_chrome_contract,
        test_server_matches_approved_visual_contract,
        test_client_matches_approved_visual_contract,
    ]
    for test in tests:
        test()
    print(f"PASS: {len(tests)} UI contract tests")
