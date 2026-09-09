# Server Table Balanced Columns Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebalance the server client table so role controls, coordinates, and the disconnect button remain visible.

**Architecture:** Keep the existing QTableWidget and cell widgets. Change only QHeaderView resize policies: the first three information columns share available width with Stretch, while the operation column uses a fixed width sized for its button.

**Tech Stack:** C++17, Qt 6 Widgets, Python source-contract tests, Visual Studio/MSBuild.

---

### Task 1: Lock balanced column behavior

**Files:**
- Modify: `Tests/DefaultReadyRoleSwitchTests.py`

- [ ] Add assertions that columns 0, 1, and 2 use QHeaderView::Stretch.
- [ ] Add assertions that column 3 uses QHeaderView::Fixed and receives an explicit width.
- [ ] Run `python Tests/DefaultReadyRoleSwitchTests.py` and confirm the new assertion fails.

### Task 2: Rebalance the service table

**Files:**
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`

- [ ] Change the role column from ResizeToContents to Stretch.
- [ ] Change the operation column from ResizeToContents to Fixed.
- [ ] Set the operation section to a width that fully displays the disconnect button.
- [ ] Run the targeted UI tests and confirm they pass.

### Task 3: Build and package

**Files:**
- Update local build artifact: `Relase/DF_PlayClick_Server_Qt.exe`

- [ ] Build DF_PlayClick_Server_Qt in Release x64.
- [ ] Copy the executable to Relase.
- [ ] Run a startup smoke test and calculate SHA-256.
- [ ] Verify Git diff, UTF-8 encoding, CRLF, and absence of replacement characters.
- [ ] Commit the implementation.

### Task 4: Keep the role capsule compact

**Files:**
- Modify: `Tests/DefaultReadyRoleSwitchTests.py`
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`

- [ ] Add a failing contract test for a transparent role cell and centered 132px capsule.
- [ ] Wrap the capsule in a transparent cell widget.
- [ ] Center the fixed-width capsule without changing table column sizing.
- [ ] Run UI tests, rebuild, package, and smoke-test the server.
