from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "DF_PlayClick_Client_Qt"


def read_text(name: str) -> str:
    return (CLIENT / name).read_text(encoding="utf-8-sig")


def test_f3_hotkey_and_bullet_ocr_loop_contract():
    hotkey_h = read_text("HotkeyManager.h")
    hotkey_cpp = read_text("HotkeyManager.cpp")
    controller_h = read_text("ClientController.h")
    controller_cpp = read_text("ClientController.cpp")

    assert "void f3Pressed();" in hotkey_h
    assert "F3HotkeyId" in hotkey_cpp
    assert "VK_F3" in hotkey_cpp
    assert "UnregisterHotKey(windowHandle_, F3HotkeyId)" in hotkey_cpp
    assert "emit f3Pressed();" in hotkey_cpp

    assert "toggleBulletOcrLoop" in controller_h
    assert "runBulletOcrRound" in controller_h
    assert "bulletOcrRunningChanged" in controller_h
    assert "bulletOcrResultChanged" in controller_h
    assert "ParseOcrPrice" in controller_cpp
    assert "EvaluateBulletOcr" in controller_cpp
    assert "clickReferencePoint" in controller_cpp
    assert "VK_ESCAPE" in controller_cpp


def test_modified_client_sources_have_no_question_mark_corruption():
    for name in (
        "ClientConfig.cpp",
        "ClientController.cpp",
        "MainWindow.cpp",
    ):
        assert "??" not in read_text(name), f"question-mark corruption found in {name}"


def test_empty_ocr_retries_silently_until_a_valid_result():
    controller_h = read_text("ClientController.h")
    controller_cpp = read_text("ClientController.cpp")

    empty_branch_start = controller_cpp.index("if (rawText.isEmpty())")
    empty_branch_end = controller_cpp.index("const OcrPriceParseResult", empty_branch_start)
    empty_branch = controller_cpp[empty_branch_start:empty_branch_end]
    assert "scheduleNextBulletOcrRound();" in empty_branch
    assert "bulletOcrWaitingForResult_ = true;" in empty_branch
    assert "appendBulletOcrLog" not in empty_branch
    assert "++bulletOcrRoundIndex_" not in empty_branch
    assert controller_cpp.index("appendBulletOcrLog(parsed.debugText);") > controller_cpp.index("if (!parsed.ok)")

    assert "bool bulletOcrWaitingForResult_ = false;" in controller_h
    assert "if (!bulletOcrWaitingForResult_)" in controller_cpp
    assert "bulletOcrWaitingForResult_ = false;" in controller_cpp
    assert "void ClientController::scheduleNextBulletOcrRound(bool completedDecision)" in controller_cpp
    assert "if (completedDecision)" in controller_cpp
    assert "++bulletOcrRoundIndex_;" in controller_cpp
    assert "scheduleNextBulletOcrRound(true);" in controller_cpp


if __name__ == "__main__":
    test_f3_hotkey_and_bullet_ocr_loop_contract()
    test_modified_client_sources_have_no_question_mark_corruption()
