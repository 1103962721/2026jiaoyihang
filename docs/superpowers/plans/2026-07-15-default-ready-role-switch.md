# Default Ready and Role Switch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every connected client automatically ready, remove readiness from the server table, and replace the role dropdown with the approved capsule selector plus inline X/Y coordinates.

**Architecture:** Keep the existing READY wire protocol for compatibility, but have the Qt client send `READY 1` immediately after every successful HELLO. Simplify only the server presentation layer: four table columns, a two-button exclusive role selector, and coordinate text derived from the same role-coordinate rules used by dispatch.

**Tech Stack:** C++17, Qt 6 Widgets, WinSock protocol, Python source-contract tests, Visual Studio/MSBuild.

---

### Task 1: Lock the new behavior with failing tests

**Files:**
- Create: `Tests/DefaultReadyRoleSwitchTests.py`
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] Add assertions that the client sends `BuildReadyMessage(true)` after every HELLO, not only during reconnection.
- [ ] Add assertions that the server table has four columns and no readiness status item.
- [ ] Add assertions for capsule role buttons, blue/purple selected states, and inline coordinate strings.
- [ ] Update the old UI skeleton contract so it rejects `QComboBox` and expects the new selector.
- [ ] Run `python Tests/DefaultReadyRoleSwitchTests.py` and the targeted old UI test; confirm they fail for the missing implementation.

### Task 2: Make connection readiness automatic

**Files:**
- Modify: `DF_PlayClick_Client_Qt/ClientConnection.cpp`
- Modify: `DF_PlayClick_Client_Qt/ClientConnection.h`
- Modify: `DF_PlayClick_Client_Qt/ClientController.cpp`

- [ ] After HELLO succeeds, always send `READY 1` before reporting the connection as established.
- [ ] Keep the desired-ready state true so reconnection behavior remains consistent.
- [ ] Synchronize the controller’s local ready state with connection state without exposing a manual UI action.
- [ ] Run the new readiness contract test and confirm it passes.
- [ ] Commit the automatic readiness change.

### Task 3: Replace the server table role dropdown

**Files:**
- Modify: `DF_PlayClick_Server_Qt/MainWindow.h`
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`

- [ ] Change the table from five columns to four: client IP, remark type, click coordinate, operation.
- [ ] Remove the readiness status item and its colors.
- [ ] Replace `QComboBox` with an exclusive pair of checkable `QPushButton`s inside a rounded capsule container.
- [ ] Use blue selected styling for coin and purple selected styling for boss.
- [ ] Connect each button to the existing `ServerController::setClientRole` path.
- [ ] Display `X: 1756  Y: 1000` for coin and `X: 2262  Y: 1159` for boss on one line.
- [ ] Remove the obsolete role-combo helper and include.
- [ ] Run the UI contract tests and confirm they pass.
- [ ] Commit the server UI change.

### Task 4: Regression verification and packaging

**Files:**
- Update build output under: `Relase/`

- [ ] Run the targeted Python tests for default readiness, merged UI, role coordinates, dispatch ordering, and connection lifecycle.
- [ ] Run the broader Python contract suite and record any pre-existing unrelated failures separately.
- [ ] Build `DF_PlayClick_Client_Qt` and `DF_PlayClick_Server_Qt` in Release with MSBuild.
- [ ] Copy the newly built executables and required runtime files to `Relase/` without modifying the original DF_PlayClick project.
- [ ] Verify executable timestamps and SHA-256 hashes.
- [ ] Check `git diff`, encoding, CRLF, and absence of Unicode replacement characters.
- [ ] Commit the verified implementation.