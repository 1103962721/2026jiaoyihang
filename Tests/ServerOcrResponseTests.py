from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_server_receives_ocr_response_and_releases_action_lock():
    manager_cpp = read_text(SERVER_QT / "ServerConnectionManager.cpp")

    assert "message.type == Protocol::MessageType::OcrResponse" in manager_cpp
    assert "emit ocrResponseReceived" in manager_cpp

    ocr_branch = manager_cpp.split("MessageType::OcrResponse", 1)[1].split("MessageType::Ping", 1)[0]
    assert "client->actionRunning = false" in ocr_branch
    assert "client->runningAction.clear()" in ocr_branch
    assert "QString::fromStdString(message.payload)" in ocr_branch


if __name__ == "__main__":
    test_server_receives_ocr_response_and_releases_action_lock()
