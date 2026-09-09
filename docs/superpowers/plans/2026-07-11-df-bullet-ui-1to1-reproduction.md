# DF Bullet UI 1:1 Reproduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the isolated merged edition server and client Qt Widgets windows so they visually reproduce the approved dual-window mockup while retaining the existing controllers, persistence, and two-EXE architecture.

**Architecture:** Add one shared frameless title-bar widget and one shared native resize helper, then rebuild only the two `MainWindow` presentation layers around the existing controllers. Static UI contract tests define the required structure and style tokens before production changes; Release x64 builds and native screenshots provide final verification.

**Tech Stack:** C++17, Qt 6.6.3 Widgets, QSS, Win32 `WM_NCHITTEST`, Python contract tests, Visual Studio/MSBuild x64.

---

## File Structure

- Create `Shared/FramelessTitleBar.h`: reusable title-bar widget API and window-control behavior.
- Create `Shared/FramelessTitleBar.cpp`: title-bar layout, drag, double-click maximize/restore, and three control dots.
- Create `Shared/FramelessWindowSupport.h`: header-only Win32 edge hit testing for frameless resize.
- Modify `DF_PlayClick_Server_Qt/MainWindow.h`: frameless native-event override and new UI members.
- Modify `DF_PlayClick_Server_Qt/MainWindow.cpp`: exact approved service layout, state styling, and data binding.
- Modify `DF_PlayClick_Client_Qt/MainWindow.h`: frameless native-event override and separate connect/disconnect UI members.
- Modify `DF_PlayClick_Client_Qt/MainWindow.cpp`: exact approved client layout, OCR metrics, state styling, and local parameter auto-save.
- Modify both Qt `.vcxproj` and `.vcxproj.filters`: compile and display the shared title-bar source.
- Modify `Tests/MergedUiSkeletonTests.py`: test the approved visual and interaction contract.

### Task 1: Define the 1:1 UI contract

**Files:**
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: Add a shared frameless-window contract test**

Assert that the shared title-bar files expose:

```python
assert "class FramelessTitleBar" in title_h
assert "startSystemMove" in title_cpp
assert "showMinimized" in title_cpp
assert "showMaximized" in title_cpp
assert "showNormal" in title_cpp
assert "WM_NCHITTEST" in support_h
```

- [ ] **Step 2: Add the service visual contract test**

Assert exact structure and key tokens:

```python
assert "resize(850, 650)" in source
assert "setMinimumSize" in source
assert 'QStringLiteral("服务端调度中心")' in source
assert 'QStringLiteral("启动监听")' in source
assert 'QStringLiteral("停止监听")' in source
assert "setMinimumWidth(74)" in source
assert "#102a5c" in source and "#245bc4" in source
assert "#0f172a" in source and "#a7f3d0" in source
assert "当前为调度 UI 骨架" not in source
```

- [ ] **Step 3: Add the client visual contract test**

```python
assert "resize(620, 650)" in source
assert "disconnectButton_" in header
assert 'QStringLiteral("连接")' in source
assert 'QStringLiteral("断开")' in source
assert 'QStringLiteral("客户端执行端")' in source
assert "ocrValueLabel_" in header
assert "unitValueLabel_" in header
assert "configuredUnitPriceLabel_" in header
assert "#113f3e" in source and "#16817b" in source
```

- [ ] **Step 4: Run the contract test and verify RED**

Run:

```powershell
python Tests/MergedUiSkeletonTests.py
```

Expected: FAIL because the shared frameless files, exact dimensions, separate disconnect button, and approved style structure do not exist yet.

- [ ] **Step 5: Commit the failing contract**

```powershell
git add Tests/MergedUiSkeletonTests.py
git commit -m "test: define exact dual-window UI contract"
```

### Task 2: Add reusable frameless window chrome

**Files:**
- Create: `Shared/FramelessTitleBar.h`
- Create: `Shared/FramelessTitleBar.cpp`
- Create: `Shared/FramelessWindowSupport.h`
- Modify: `DF_PlayClick_Server_Qt/DF_PlayClick_Server_Qt.vcxproj`
- Modify: `DF_PlayClick_Server_Qt/DF_PlayClick_Server_Qt.vcxproj.filters`
- Modify: `DF_PlayClick_Client_Qt/DF_PlayClick_Client_Qt.vcxproj`
- Modify: `DF_PlayClick_Client_Qt/DF_PlayClick_Client_Qt.vcxproj.filters`

- [ ] **Step 1: Declare the title-bar widget**

Use a focused QWidget without `Q_OBJECT`:

```cpp
class FramelessTitleBar final : public QWidget
{
public:
    FramelessTitleBar(const QString& title, QWidget* targetWindow, QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void toggleMaximized();
    QWidget* targetWindow_ = nullptr;
};
```

- [ ] **Step 2: Implement title-bar controls**

Build a 38 logical-pixel bar with a title label and three 9-pixel circular buttons. Connect buttons directly to `showMinimized`, maximize/restore, and `close`. Use `QWindow::startSystemMove()` for native dragging and toggle maximize on a left-button double-click.

Add short Chinese comments around native drag and maximize/restore behavior.

- [ ] **Step 3: Add Win32 edge resize hit testing**

Implement a header-only helper:

```cpp
inline bool handleFramelessResize(QWidget* window, void* message, qintptr* result, int borderWidth = 7);
```

On `WM_NCHITTEST`, return the correct `HTTOPLEFT`, `HTTOP`, `HTTOPRIGHT`, `HTLEFT`, `HTRIGHT`, `HTBOTTOMLEFT`, `HTBOTTOM`, or `HTBOTTOMRIGHT` code when the pointer is within the resize border. Do not return `HTCAPTION`; the title-bar widget owns dragging so its buttons remain clickable.

- [ ] **Step 4: Add the shared source to both projects**

Add `..\Shared\FramelessTitleBar.cpp` as `ClCompile` and both shared headers as `ClInclude` in each project and filter file.

- [ ] **Step 5: Run the contract test**

Run:

```powershell
python Tests/MergedUiSkeletonTests.py
```

Expected: still FAIL on server/client layout assertions, while shared frameless assertions pass.

- [ ] **Step 6: Compile both projects**

Run:

```powershell
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick_Server_Qt\DF_PlayClick_Server_Qt.vcxproj" /p:Configuration=Release /p:Platform=x64 /m
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick_Client_Qt\DF_PlayClick_Client_Qt.vcxproj" /p:Configuration=Release /p:Platform=x64 /m
```

Expected: both succeed with 0 errors.

- [ ] **Step 7: Commit**

```powershell
git add Shared/FramelessTitleBar.* Shared/FramelessWindowSupport.h DF_PlayClick_Server_Qt/*.vcxproj* DF_PlayClick_Client_Qt/*.vcxproj*
git commit -m "feat: add shared frameless window chrome"
```

### Task 3: Rebuild the service window

**Files:**
- Modify: `DF_PlayClick_Server_Qt/MainWindow.h`
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`
- Test: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: Add frameless members and native event override**

Declare:

```cpp
bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
QWidget* createAppHeader();
QWidget* createCard(const QString& title, QLayout* contentLayout);
QLabel* createBadge(...);
void applyStyle();
```

Keep existing controller-facing members and role-combo helpers.

- [ ] **Step 2: Apply the approved window shell**

Set:

```cpp
setWindowFlag(Qt::FramelessWindowHint, true);
setAttribute(Qt::WA_StyledBackground, true);
resize(850, 650);
setMinimumSize(790, 590);
```

Root order:

1. `FramelessTitleBar("DF 倒子弹服务端", ...)`;
2. blue gradient application header;
3. content body.

- [ ] **Step 3: Rebuild the top cards**

Use a horizontal layout matching the approved 1:1 proportions. The listen row must contain port, `启动监听`, and `停止监听`; both buttons use `setMinimumWidth(74)` and `setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed)`.

The dispatch card contains countdown, group delay, `执行`, `上架`, and a two-item order selector whose visible text is `币号 → 老板` and `老板 → 币号`.

- [ ] **Step 4: Rebuild the five-column client table**

Use approved labels and row heights. Keep the role `QComboBox`, display `币号固定点 · 自适配` or `老板固定点 · 自适配`, and retain the disconnect operation.

- [ ] **Step 5: Rebuild flow preview, status, and log**

Place the flow preview directly below the table and use the approved pale-blue style. Use a compact status line only when needed; the dark log is the final expanding area.

Generate flow text from real order and group delay, without the old “UI 骨架” text.

- [ ] **Step 6: Apply exact QSS tokens**

Include the approved fonts, colors, heights, borders, radii, gradients, hover states, disabled states, table selection colors, and log palette from the spec.

- [ ] **Step 7: Wire native resize and preserve controller signals**

Forward `nativeEvent` to `handleFramelessResize`. Keep all existing listen, stop, role update, kick, trigger, listing, client refresh, status, and log signals intact.

- [ ] **Step 8: Run contract test and service build**

```powershell
python Tests/MergedUiSkeletonTests.py
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick_Server_Qt\DF_PlayClick_Server_Qt.vcxproj" /p:Configuration=Release /p:Platform=x64 /m
```

Expected: client-specific assertions may still fail; server assertions and service build pass.

- [ ] **Step 9: Commit**

```powershell
git add DF_PlayClick_Server_Qt/MainWindow.* Tests/MergedUiSkeletonTests.py
git commit -m "feat: reproduce approved service control UI"
```

### Task 4: Rebuild the client window

**Files:**
- Modify: `DF_PlayClick_Client_Qt/MainWindow.h`
- Modify: `DF_PlayClick_Client_Qt/MainWindow.cpp`
- Test: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: Add the approved UI members**

Add a separate `disconnectButton_`, `configuredUnitPriceLabel_`, and compact status labels while keeping `ocrValueLabel_`, `unitValueLabel_`, `ocrDecisionLabel_`, `clientRoleLabel_`, `executionStatusLabel_`, and the existing controller.

- [ ] **Step 2: Apply the approved window shell**

Set:

```cpp
setWindowFlag(Qt::FramelessWindowHint, true);
setAttribute(Qt::WA_StyledBackground, true);
resize(620, 650);
setMinimumSize(580, 610);
```

Root order:

1. `FramelessTitleBar("DF 倒子弹客户端", ...)`;
2. green gradient application header;
3. stacked content cards.

- [ ] **Step 3: Rebuild connection card**

Use labeled IP and port fields followed by independent `连接` and `断开` buttons. Keep local IP persistence and heartbeat/reconnect state text. Enable/disable buttons from real connection and reconnecting states instead of rewriting one button label.

- [ ] **Step 4: Rebuild OCR card**

Place labeled unit price and bullet-total inputs on one row. Add three equal metric panels:

1. OCR result;
2. result divided by bullet total;
3. configured unit price.

Keep automatic save on `editingFinished`. Invalid values show the red decision state and do not overwrite valid local settings. Waiting state must not display fake OCR values.

- [ ] **Step 5: Rebuild local execution card and log**

Show the server-provided role, fixed-coordinate adaptation status, latest instruction/status, and the no-manual-picker hint. Keep the dark expanding log at the bottom.

- [ ] **Step 6: Apply exact client QSS tokens**

Use the approved green gradient, shared cards, metric panels, decision colors, execution panels, input/button sizes, and dark log styling.

- [ ] **Step 7: Preserve controller initialization and signals**

Keep deferred `controller_.initialize(reinterpret_cast<HWND>(winId()))` in `showEvent`, connection state, reconnect state, countdown, OCR parameter persistence, messages, and logs.

- [ ] **Step 8: Run contract test and client build**

```powershell
python Tests/MergedUiSkeletonTests.py
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick_Client_Qt\DF_PlayClick_Client_Qt.vcxproj" /p:Configuration=Release /p:Platform=x64 /m
```

Expected: all UI contract tests pass and client build succeeds with 0 errors.

- [ ] **Step 9: Commit**

```powershell
git add DF_PlayClick_Client_Qt/MainWindow.* Tests/MergedUiSkeletonTests.py
git commit -m "feat: reproduce approved client execution UI"
```

### Task 5: Regression, native visual verification, and packaging

**Files:**
- Modify only if a verified defect is found: files from Tasks 2-4
- Output: `Relase/DF_PlayClick_Server_Qt.exe`
- Output: `Relase/DF_PlayClick_Client_Qt.exe`

- [ ] **Step 1: Run relevant Python regressions**

```powershell
python Tests/MergedUiSkeletonTests.py
python Tests/ClientClickQueueDelayTests.py
python Tests/ClientStartupGuardTests.py
python Tests/ConnectionHeartbeatTests.py
python Tests/ServerDisconnectedClientTests.py
```

Expected: all pass.

- [ ] **Step 2: Run C++ protocol and role tests**

Build and run the existing test binaries using the repository’s established commands. Expected: all pass.

- [ ] **Step 3: Rebuild the full solution**

```powershell
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick.sln" /p:Configuration=Release /p:Platform=x64 /t:Rebuild /m
```

Expected: 0 errors. Existing Qt generated-resource encoding warnings may remain, but no new source encoding warnings are allowed.

- [ ] **Step 4: Launch both EXEs and capture native screenshots**

Launch the service and client independently. Capture each native window and compare it with `02-exact-reproduction-v1.html` for window proportion, title bar, gradients, card order, text, spacing, colors, table, metric panels, and log area.

- [ ] **Step 5: Check resize behavior**

Verify initial, minimum, and enlarged sizes. Confirm `启动监听`, `停止监听`, `连接`, and `断开` remain single-line. Verify title-bar drag, double-click maximize/restore, all three control dots, and edge resizing.

- [ ] **Step 6: Copy verified executables to Relase**

Copy the final server and client executables, along with any required runtime files already present in the package, to:

```text
D:\visual Studio\visual studio Project\倒子弹\DF_PlayClick_倒子弹合并版\Relase
```

Do not rename the directory to `Release`.

- [ ] **Step 7: Final repository checks**

```powershell
git diff --check
git status --short
git log --oneline -6
```

Expected: no whitespace errors and no unintended modifications outside the isolated worktree.

- [ ] **Step 8: Final commit if verification required fixes**

```powershell
git add <verified-fix-files>
git commit -m "fix: align native UI with approved mockup"
```