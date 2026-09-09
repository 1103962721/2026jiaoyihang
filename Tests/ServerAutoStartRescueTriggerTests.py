from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_QT = ROOT / "DF_PlayClick_Server_Qt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_matching_client_gets_immediate_rescue_trigger_after_peer_loading():
    controller_h = read_text(SERVER_QT / "ServerController.h")
    controller_cpp = read_text(SERVER_QT / "ServerController.cpp")

    assert "AutoStartMatchingRescueDelayMs = 1500" in controller_cpp
    assert "processMatchingRescue" in controller_h
    assert "processMatchingRescue(clients, nowMs)" in controller_cpp
    assert "connectedClientCount >= 2" in controller_cpp
    assert "AutoStartStage::Loading" in controller_cpp
    assert "AutoStartStage::Matching" in controller_cpp
    assert "sawCancelAction" in controller_cpp
    assert "triggerSingleClient(clientId, 0" in controller_cpp
    assert "resetAutoStartClient(clientId)" in controller_cpp
    assert "resetClientTeamReadyDisplay(clientId)" in controller_cpp


def test_waiting_targets_are_not_blocked_by_other_active_auto_start_states():
    controller_cpp = read_text(SERVER_QT / "ServerController.cpp")

    assert "const bool hasActivePostTriggerStates = !autoStartClientStates_.isEmpty()" in controller_cpp
    assert "hasActivePostTriggerStates && waitingTargets.isEmpty()" in controller_cpp
    assert "!hasActivePostTriggerStates && autoStartCanTrigger_ && allFull" in controller_cpp


if __name__ == "__main__":
    test_matching_client_gets_immediate_rescue_trigger_after_peer_loading()
    test_waiting_targets_are_not_blocked_by_other_active_auto_start_states()
