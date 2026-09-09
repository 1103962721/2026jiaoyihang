# F3 Bullet OCR Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the original F3-controlled bullet-price OCR loop in the isolated merged client while scaling OCR and retry-click coordinates for the bound game window.

**Architecture:** Keep network dispatch and the local F3 OCR loop independent inside `ClientController`. Extract OCR number parsing and reference-coordinate math into small testable units, reuse `UmiOcrRunner` and the existing bound-window screenshot/input facilities, and expose loop state/results to the existing OCR card through Qt signals.

**Tech Stack:** C++17, Qt 6.6.3 Widgets, Win32 global hotkeys/input, UmiOCR local HTTP API, Python/C++ regression tests, MSBuild x64.

---

### Task 1: Define OCR parsing, decision, and scaling behavior

**Files:**
- Create: `DF_PlayClick_Client_Qt/OcrNumberParser.h`
- Create: `DF_PlayClick_Client_Qt/OcrNumberParser.cpp`
- Create: `DF_PlayClick_Client_Qt/BulletOcrRules.h`
- Create: `Tests/OcrNumberParserTests.cpp`
- Create: `Tests/BulletOcrRulesTests.cpp`

- [ ] Port the legacy OCR number cases into failing tests.
- [ ] Add tests for `OCR price / bullet total <= unit price` and invalid inputs.
- [ ] Add tests for scaling the legacy OCR rectangle and retry point from a reference client size.
- [ ] Run the tests and verify they fail because the new units do not exist.
- [ ] Implement the minimal parser and pure rules, then verify all tests pass.

### Task 2: Restore the F3 global hotkey

**Files:**
- Modify: `DF_PlayClick_Client_Qt/HotkeyManager.h`
- Modify: `DF_PlayClick_Client_Qt/HotkeyManager.cpp`
- Create: `Tests/ClientF3OcrContractTests.py`

- [ ] Add a source contract test requiring a distinct F3 hotkey id, registration, unregistration, and `f3Pressed` signal.
- [ ] Verify the contract fails first.
- [ ] Register F3 without removing the existing Ctrl+Shift+P picker hotkey and emit separate signals.
- [ ] Verify the contract passes.

### Task 3: Add local bullet OCR configuration

**Files:**
- Modify: `DF_PlayClick_Client_Qt/ClientConfig.h`
- Modify: `DF_PlayClick_Client_Qt/ClientConfig.cpp`

- [ ] Add configuration for the legacy OCR rectangle, retry click, reference width/height, ESC delay, and loop interval.
- [ ] Keep the reference size in INI-backed configuration so the exact legacy base resolution can be corrected later without redesigning the loop.
- [ ] Validate dimensions and delays before use.

### Task 4: Implement the OCR loop in the client controller

**Files:**
- Modify: `DF_PlayClick_Client_Qt/ClientController.h`
- Modify: `DF_PlayClick_Client_Qt/ClientController.cpp`
- Modify: `DF_PlayClick_Client_Qt/DF_PlayClick_Client_Qt.pro`
- Modify: `DF_PlayClick_Client_Qt/DF_PlayClick_Client_Qt.vcxproj`
- Modify: `DF_PlayClick_Client_Qt/DF_PlayClick_Client_Qt.vcxproj.filters`

- [ ] Connect F3 to a toggle method.
- [ ] Validate a bound window, positive unit price, and positive bullet total before starting.
- [ ] Capture the scaled OCR region, run UmiOCR, parse the legacy numeric formats, calculate unit price, and stop when the threshold is met.
- [ ] When the threshold is not met, click the scaled legacy retry point, wait the configured ESC delay, press ESC, then start the next round.
- [ ] Make a second F3 press stop timers/current state safely.
- [ ] Prevent the local OCR loop from overlapping another local automation action.
- [ ] Emit loop running/result/log signals only through the controller.

### Task 5: Wire the existing OCR card and publish

**Files:**
- Modify: `DF_PlayClick_Client_Qt/MainWindow.cpp`
- Modify: `DF_PlayClick_Client_Qt/MainWindow.h`
- Output: `DF_PlayClick_Client_Qt/release/DF_PlayClick_Client_Qt.exe`
- Output: `Relase/DF_PlayClick_Client_Qt.exe`

- [ ] Display F3 running/stopped state, raw OCR price, calculated unit price, decision, and loop logs in the existing client window without changing the approved overall layout.
- [ ] Run parser/rule/source-contract tests and existing UI/network regressions.
- [ ] Build `Release|x64`, perform a startup smoke test, check encoding/CRLF/U+FFFD, and copy the verified EXE to `Relase`.