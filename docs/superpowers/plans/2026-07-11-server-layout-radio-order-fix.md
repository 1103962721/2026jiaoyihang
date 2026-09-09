# Server Layout and Radio Order Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore comfortable vertical spacing in the server window and reproduce the approved flat radio-button order selector.

**Architecture:** Keep the existing Qt Widgets structure and controller wiring. Change only server presentation constraints and replace the order `QComboBox` with two mutually exclusive `QRadioButton` members whose state feeds the existing flow-preview logic.

**Tech Stack:** C++17, Qt 6.6.3 Widgets, Python contract tests, MSBuild x64.

---

### Task 1: Define the regression contract

**Files:**
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] Assert `resize(850, 760)`, `setMinimumSize(790, 700)`, table minimum height 160, and log minimum height 120.
- [ ] Assert `coinFirstRadio_` and `bossFirstRadio_` exist and `triggerOrderCombo_` no longer exists.
- [ ] Run `python Tests/MergedUiSkeletonTests.py` and verify failure against the current implementation.

### Task 2: Apply the minimal Qt UI fix

**Files:**
- Modify: `DF_PlayClick_Server_Qt/MainWindow.h`
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`

- [ ] Preserve source encoding as UTF-8 without BOM and CRLF.
- [ ] Adjust initial/minimum height and table/log minimum heights.
- [ ] Replace the order combo with two flat radio buttons and connect their toggled state to `updateFlowPreview()`.
- [ ] Style the radio indicator to match the approved blue selection treatment.
- [ ] Run the UI contract test and required regressions.

### Task 3: Build and publish

**Files:**
- Output: `DF_PlayClick_Server_Qt/release/DF_PlayClick_Server_Qt.exe`
- Output: `Relase/DF_PlayClick_Server_Qt.exe`

- [ ] Build the server project with `Release|x64`.
- [ ] Check `git diff --check`, UTF-8/CRLF, and U+FFFD.
- [ ] Copy the verified server EXE to `Relase` and compare SHA-256 hashes.