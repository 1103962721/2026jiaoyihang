# 2K Role Dispatch and Client Height Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete role-aware fixed-coordinate dispatch, update bullet OCR to the supplied 2K rectangle, and apply the approved client height fix without changing the approved palette.

**Architecture:** Extend the existing line protocol with a constrained `ROLE COIN|BOSS` message. The server synchronizes roles and schedules grouped trigger sends with a precise Qt timer; the client stores the role and uses existing reference-coordinate scaling/input code to click one hard-coded 2560 x 1440 point. Preserve the OCR decision pipeline and give its legacy retry point a separate reference size.

**Tech Stack:** C++17, Qt 6.6.3 Widgets/Network timers, Win32 input, Python/C++ regression tests, MSBuild x64.

---

### Task 1: Lock protocol and coordinate contracts

**Files:**
- Modify: `Tests/ProtocolTests.cpp`
- Modify: `Tests/BulletOcrRulesTests.cpp`
- Modify: `Tests/ClientF3OcrContractTests.py`
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] Add failing protocol assertions for `ROLE COIN` and `ROLE BOSS` build/parse behavior and invalid roles.
- [ ] Add failing coordinate assertions for the 2560 x 1440 OCR rectangle and coin/boss points at native and scaled resolutions.
- [ ] Add failing source-contract assertions for separate retry reference dimensions, role reception, fixed-point dispatch, grouped ordering/delay wiring, and the approved height/badge constraints.
- [ ] Run each targeted test and confirm it fails for the intended missing behavior.

### Task 2: Implement role protocol and synchronization

**Files:**
- Modify: `Shared/Protocol.h`
- Modify: `DF_PlayClick_Client_Qt/ClientConnection.h`
- Modify: `DF_PlayClick_Client_Qt/ClientConnection.cpp`
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.h`
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.cpp`

- [ ] Add the constrained role message type, builder, and parser.
- [ ] Emit role changes from the client connection receive loop.
- [ ] Send the default role after accept and send later role changes immediately.
- [ ] Run protocol and source-contract tests until green.

### Task 3: Implement fixed-coordinate client dispatch and OCR defaults

**Files:**
- Modify: `DF_PlayClick_Client_Qt/BulletOcrRules.h`
- Modify: `DF_PlayClick_Client_Qt/BulletOcrRules.cpp`
- Modify: `DF_PlayClick_Client_Qt/ClientConfig.h`
- Modify: `DF_PlayClick_Client_Qt/ClientConfig.cpp`
- Modify: `DF_PlayClick_Client_Qt/ClientController.h`
- Modify: `DF_PlayClick_Client_Qt/ClientController.cpp`
- Modify: `DF_PlayClick_Client_Qt/MainWindow.cpp`

- [ ] Add role-to-reference-point helpers using the existing scaling math.
- [ ] Update OCR defaults to `(2047,1126,400,80)` at `2560x1440` and retain retry scaling at `1650x1080`.
- [ ] Store the synchronized client role and replace trigger execution with one fixed-point click against the bound game client area.
- [ ] Update the existing role/strategy labels from controller signals without adding picker controls.
- [ ] Run coordinate, OCR, and client contract tests until green.

### Task 4: Wire grouped precise dispatch

**Files:**
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.h`
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.cpp`
- Modify: `DF_PlayClick_Server_Qt/ServerController.h`
- Modify: `DF_PlayClick_Server_Qt/ServerController.cpp`
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`

- [ ] Add a role-filtered ready-client trigger method that sends each group in a tight loop.
- [ ] Pass countdown, group delay, and selected order from the server UI.
- [ ] Send the first group immediately and the second through `Qt::PreciseTimer` after the configured delay.
- [ ] Preserve the existing auto-start call through explicit default ordering/delay values.
- [ ] Run grouped-dispatch and UI contract tests until green.

### Task 5: Apply approved client height option A

**Files:**
- Modify: `DF_PlayClick_Client_Qt/MainWindow.cpp`
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] Increase initial/minimum client height to match the approved preview while preserving width and all color values.
- [ ] Give the connection badge a minimum width and a non-compressing horizontal size policy.
- [ ] Run the UI contract and inspect the localhost preview contract.

### Task 6: Verify, build, and publish

**Files:**
- Output: `DF_PlayClick_Client_Qt/release/DF_PlayClick_Client_Qt.exe`
- Output: `DF_PlayClick_Server_Qt/release/DF_PlayClick_Server_Qt.exe`
- Output: `Relase/DF_PlayClick_Client_Qt.exe`
- Output: `Relase/DF_PlayClick_Server_Qt.exe`

- [ ] Run all targeted C++ and Python regressions plus `git diff --check`.
- [ ] Verify touched source files preserve BOM and CRLF and contain no U+FFFD.
- [ ] Build both Qt projects in `Release|x64`.
- [ ] Perform hidden startup smoke tests, copy both executables to `Relase`, and verify SHA-256 equality.
- [ ] Commit the isolated branch changes.
