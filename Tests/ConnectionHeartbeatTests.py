from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read_text(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8-sig")


def test_protocol_defines_ping_pong_heartbeat_messages():
    protocol = read_text("Shared/Protocol.h")
    tests = read_text("Tests/ProtocolTests.cpp")
    assert "Ping" in protocol
    assert "Pong" in protocol
    assert "BuildPingMessage" in protocol
    assert "BuildPongMessage" in protocol
    assert 'line == "PING"' in protocol
    assert 'line == "PONG"' in protocol
    assert "BuildPingMessage() == \"PING\\n\"" in tests
    assert "BuildPongMessage() == \"PONG\\n\"" in tests


def test_qt_client_sends_heartbeat_and_handles_failed_sends():
    header = read_text("DF_PlayClick_Client_Qt/ClientConnection.h")
    source = read_text("DF_PlayClick_Client_Qt/ClientConnection.cpp")
    policy = read_text("Shared/ConnectionLifecyclePolicy.h")
    assert "QTimer heartbeatTimer_" in header
    assert "void sendHeartbeat()" in header
    assert "void handleConnectionLost" in header
    assert "heartbeatMisses_" in header
    assert "ShouldAutoDisconnectForHeartbeatMiss" in source
    assert "ShouldAutoDisconnectForHeartbeatMiss" in policy
    assert "heartbeatTimer_.setInterval" in source
    assert "Protocol::BuildPingMessage()" in source
    assert "heartbeatTimer_.start()" in source
    assert "heartbeatTimer_.stop()" in source
    assert "if (!SocketUtil::SendLine" in source
    assert "handleConnectionLost" in source
    assert "Protocol::MessageType::Pong" in source
    assert "heartbeatMisses_ = 0" in source
    assert "++heartbeatMisses_" in source
    assert "心跳超时" in source


def test_qt_client_only_marks_ready_after_ready_send_succeeds():
    header = read_text("DF_PlayClick_Client_Qt/ClientConnection.h")
    connection = read_text("DF_PlayClick_Client_Qt/ClientConnection.cpp")
    controller = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    assert "bool sendReady(bool ready)" in header
    assert "bool ClientConnection::sendReady" in connection
    assert "if (!connection_.sendReady(ready))" in controller
    assert "ready_ = ready;" in controller
    assert controller.index("if (!connection_.sendReady(ready))") < controller.index("ready_ = ready;")


def test_qt_client_clears_local_ready_state_when_connection_drops():
    controller = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    assert "void ClientController::clearLocalReadyState" in controller
    assert "clearLocalReadyState();" in controller
    section = controller[controller.index("void ClientController::clearLocalReadyState"):controller.index("void ClientController::setStatus")]
    assert "connection_.sendReady" not in section


def test_qt_client_exposes_persistent_connection_log_area():
    connection_header = read_text("DF_PlayClick_Client_Qt/ClientConnection.h")
    connection_source = read_text("DF_PlayClick_Client_Qt/ClientConnection.cpp")
    controller_header = read_text("DF_PlayClick_Client_Qt/ClientController.h")
    controller_source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    window_header = read_text("DF_PlayClick_Client_Qt/MainWindow.h")
    window_source = read_text("DF_PlayClick_Client_Qt/MainWindow.cpp")
    assert "void connectionLogAppended" in connection_header
    assert "emit connectionLogAppended" in connection_source
    assert "WSAGetLastError()" in connection_source
    assert "recv 返回 0" in connection_source
    assert "Socket 接收失败" in connection_source
    assert "心跳超时" in connection_source
    assert "void connectionLogAppended" in controller_header
    assert "emit connectionLogAppended(text)" in controller_source
    assert "QTextEdit" in window_header
    assert "QTextEdit* connectionLogEdit_" in window_header
    assert "void appendConnectionLog" in window_header
    assert "连接日志" in window_source
    assert "connectionLogEdit_->setReadOnly(true)" in window_source
    assert "connectionLogEdit_->append" in window_source
    assert "&ClientController::connectionLogAppended" in window_source


def test_qt_server_disconnect_log_uses_peer_closed_wording():
    source = read_text("DF_PlayClick_Server_Qt/ServerConnectionManager.cpp")
    assert "recv 返回 0，对端已关闭连接" in source
    assert "客户端正常关闭连接" not in source


def test_qt_server_exposes_persistent_connection_log_area():
    header = read_text("DF_PlayClick_Server_Qt/MainWindow.h")
    source = read_text("DF_PlayClick_Server_Qt/MainWindow.cpp")
    controller_header = read_text("DF_PlayClick_Server_Qt/ServerController.h")
    controller_source = read_text("DF_PlayClick_Server_Qt/ServerController.cpp")
    assert "QTextEdit" in header
    assert "QTextEdit* connectionLogEdit_" in header
    assert "void appendConnectionLog" in header
    assert "运行日志" in source
    assert "connectionLogEdit_->setReadOnly(true)" in source
    assert "connectionLogEdit_->append" in source
    assert "&ServerController::connectionLogAppended" in source
    assert "void connectionLogAppended" in controller_header
    assert "emit connectionLogAppended(text)" in controller_source


def test_qt_server_records_disconnect_diagnostics():
    header = read_text("DF_PlayClick_Server_Qt/ServerConnectionManager.h")
    source = read_text("DF_PlayClick_Server_Qt/ServerConnectionManager.cpp")
    controller = read_text("DF_PlayClick_Server_Qt/ServerController.cpp")
    assert "recordDisconnectReason" in header
    assert "WSAGetLastError()" in source
    assert "recv 返回 0" in source
    assert "Socket 接收失败" in source
    assert "Protocol::MessageType::Ping" in source
    assert "Protocol::BuildPongMessage()" in source
    assert "networkError" in source
    assert "&ServerConnectionManager::networkError" in controller
