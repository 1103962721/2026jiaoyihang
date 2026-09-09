from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_auto_start_has_post_trigger_ocr_state_machine():
    controller_h = read_text(SERVER_QT / "ServerController.h")
    controller_cpp = read_text(SERVER_QT / "ServerController.cpp")

    assert "AutoStartStage" in controller_h
    assert "AfterTriggerDelay" in controller_h
    assert "Matching" in controller_h
    assert "Loading" in controller_h
    assert "InGame" in controller_h
    assert "autoStartTimer_.setInterval(100)" in controller_cpp
    assert "AutoStartAfterTriggerDelayMs = 1000" in controller_cpp
    assert "OcrRequestRegion" in controller_cpp
    assert "1945" in controller_cpp and "1261" in controller_cpp
    assert "500" in controller_cpp and "60" in controller_cpp
    assert "MapTypeRegion" in controller_cpp
    assert "ocrResponseReceived" in controller_cpp
    assert "ContainsCancelActionMarker" in controller_cpp
    assert "ContainsActionRegionWaitingMarker" in controller_cpp
    assert "ContainsMapTypeMarker" in controller_cpp
    assert "No text found" in controller_cpp
    assert "正在加载" in controller_cpp
    assert "executeSelfThunderPlaceholder" in controller_cpp


if __name__ == "__main__":
    test_auto_start_has_post_trigger_ocr_state_machine()
