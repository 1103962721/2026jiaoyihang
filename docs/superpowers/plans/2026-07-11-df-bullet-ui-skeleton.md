# DF 倒子弹双窗口 UI 骨架 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 DF_PlayClick Qt 服务端和客户端中复现已确认的两个独立窗口 UI，并建立币号/老板类型、客户端 OCR 参数本地保存、上架占位入口和后续调度所需的第一阶段骨架。

**Architecture:** 第一阶段只改 Qt 主线，不迁移未知分辨率的真实 OCR 区域和固定点击坐标。服务端继续通过 `ServerController`/`ServerConnectionManager` 管理客户端，新增强类型客户端角色和只筛选币号的上架占位入口；客户端继续通过 `ClientController` 管理连接，并通过 `ClientConfig` 持久化单价和子弹总数。旧自动化控制器代码暂时保留但不在新 UI 中暴露，以减少第一阶段编译和行为风险。

**Tech Stack:** C++17、Qt Widgets、Win32、Visual Studio/MSBuild、Python 静态回归测试、现有 C++ assert 测试。

---

## Scope decomposition

本计划只实现可编译、可运行、可验收的 UI 与配置骨架：

- 双独立窗口的新布局；
- 服务端币号/老板类型；
- 默认币号；
- 上架只统计币号且为明确占位功能；
- 倒计时、分组延迟和顺序控件；
- 客户端连接 UI；
- 客户端单价、子弹总数本地保存；
- OCR 与本地执行的状态占位区。

以下拆到后续计划，等待基础分辨率和坐标：

- OCR 组件真实迁移与 Umi-OCR 调用；
- OCR 截图矩形全分辨率映射；
- 币号/老板固定点击点映射；
- 高精度分组调度器和真实新协议；
- 上架最终业务动作。

## File map

### 新增

- `Shared/ClientRole.h`：客户端角色枚举、显示名称和安全解析。
- `Tests/ClientRoleTests.cpp`：默认角色、转换和解析测试。
- `Tests/MergedUiSkeletonTests.py`：服务端/客户端 UI 与上架过滤的静态契约测试。

### 修改

- `DF_PlayClick_Server_Qt/ServerModels.h`：客户端列表项使用 `ClientRole`。
- `DF_PlayClick_Server_Qt/ServerConnectionManager.h/.cpp`：新连接默认币号；角色更新；币号计数/快照入口。
- `DF_PlayClick_Server_Qt/ServerController.h/.cpp`：角色更新、上架占位、延迟和顺序输入校验入口。
- `DF_PlayClick_Server_Qt/MainWindow.h/.cpp`：复现服务端控制台 UI。
- `DF_PlayClick_Client_Qt/ClientConfig.h/.cpp`：保存单价和子弹总数。
- `DF_PlayClick_Client_Qt/ClientController.h/.cpp`：暴露 OCR 参数 getter/setter 给 UI。
- `DF_PlayClick_Client_Qt/MainWindow.h/.cpp`：复现客户端连接/OCR/执行状态 UI，移除手动取点控件入口。
- `DF_PlayClick_Server_Qt/DF_PlayClick_Server_Qt.vcxproj(.filters)`、`DF_PlayClick_Client_Qt/DF_PlayClick_Client_Qt.vcxproj(.filters)`：必要时登记新增共享头文件。

---

### Task 1: 建立第一阶段 UI 契约测试

**Files:**
- Create: `Tests/MergedUiSkeletonTests.py`
- Test: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: 写服务端和客户端的新 UI 失败测试**

测试读取 UTF-8 BOM 源文件并断言：

```python
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "DF_PlayClick_Server_Qt"
CLIENT = ROOT / "DF_PlayClick_Client_Qt"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_server_exposes_grouped_trigger_and_listing_controls():
    header = read(SERVER / "MainWindow.h")
    source = read(SERVER / "MainWindow.cpp")
    assert "groupDelayEdit_" in header
    assert "triggerOrderCombo_" in header
    assert "listingButton_" in header
    assert "全部出发" in source
    assert "上架" in source
    assert "币号优先" in source
    assert "老板优先" in source


def test_client_exposes_connection_and_local_ocr_parameters_without_picker_ui():
    header = read(CLIENT / "MainWindow.h")
    source = read(CLIENT / "MainWindow.cpp")
    assert "unitPriceEdit_" in header
    assert "bulletTotalEdit_" in header
    assert "OCR 上架判定" in source
    assert "连接服务器" in source
    assert "windowsTable_" not in header
    assert "tasksTable_" not in header
    assert "autoPointButton_" not in header
```

- [ ] **Step 2: 运行测试并确认失败**

Run:

```powershell
python Tests/MergedUiSkeletonTests.py
```

Expected: pytest/直接调用时至少一项断言失败，因为新控件尚未存在。

- [ ] **Step 3: 提交测试**

```powershell
git add Tests/MergedUiSkeletonTests.py
git commit -m "test: define merged dual-window UI contract"
```

---

### Task 2: 引入币号/老板强类型模型

**Files:**
- Create: `Shared/ClientRole.h`
- Create: `Tests/ClientRoleTests.cpp`
- Modify: `DF_PlayClick_Server_Qt/ServerModels.h`
- Modify: `DF_PlayClick_Server_Qt/DF_PlayClick_Server_Qt.vcxproj`
- Modify: `DF_PlayClick_Server_Qt/DF_PlayClick_Server_Qt.vcxproj.filters`

- [ ] **Step 1: 写角色转换失败测试**

```cpp
#include <cassert>
#include "../Shared/ClientRole.h"

int main()
{
    assert(ClientRoleUtil::DefaultRole == ClientRole::Coin);
    assert(ClientRoleUtil::ToDisplayText(ClientRole::Coin) == L"币号");
    assert(ClientRoleUtil::ToDisplayText(ClientRole::Boss) == L"老板");
    assert(ClientRoleUtil::FromDisplayText(L"老板") == ClientRole::Boss);
    assert(ClientRoleUtil::FromDisplayText(L"未知") == ClientRole::Coin);
    return 0;
}
```

- [ ] **Step 2: 编译并确认失败**

Run:

```powershell
g++ -std=c++17 -I. Tests/ClientRoleTests.cpp -o Tests/ClientRoleTests.exe
```

Expected: FAIL，`Shared/ClientRole.h` 不存在。

- [ ] **Step 3: 实现最小角色模型**

```cpp
#pragma once
#include <string_view>

enum class ClientRole { Coin, Boss };

namespace ClientRoleUtil
{
    constexpr ClientRole DefaultRole = ClientRole::Coin;

    inline std::wstring_view ToDisplayText(ClientRole role)
    {
        return role == ClientRole::Boss ? L"老板" : L"币号";
    }

    inline ClientRole FromDisplayText(std::wstring_view text)
    {
        return text == L"老板" ? ClientRole::Boss : ClientRole::Coin;
    }
}
```

将 `ClientListItem::remark` 替换或补充为 `ClientRole role = ClientRoleUtil::DefaultRole`。

- [ ] **Step 4: 运行角色测试**

Run:

```powershell
g++ -std=c++17 -I. Tests/ClientRoleTests.cpp -o Tests/ClientRoleTests.exe
& Tests/ClientRoleTests.exe
```

Expected: PASS，退出码 0。

- [ ] **Step 5: 提交**

```powershell
git add Shared/ClientRole.h Tests/ClientRoleTests.cpp DF_PlayClick_Server_Qt/ServerModels.h DF_PlayClick_Server_Qt/DF_PlayClick_Server_Qt.vcxproj DF_PlayClick_Server_Qt/DF_PlayClick_Server_Qt.vcxproj.filters
git commit -m "feat: add coin and boss client roles"
```

---

### Task 3: 服务端角色管理和上架占位业务

**Files:**
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.h`
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.cpp`
- Modify: `DF_PlayClick_Server_Qt/ServerController.h`
- Modify: `DF_PlayClick_Server_Qt/ServerController.cpp`
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: 扩展失败测试，约束默认币号和老板过滤**

追加断言：

```python
def test_listing_stub_only_targets_coin_clients():
    manager_h = read(SERVER / "ServerConnectionManager.h")
    manager_cpp = read(SERVER / "ServerConnectionManager.cpp")
    controller_cpp = read(SERVER / "ServerController.cpp")
    assert "setClientRole" in manager_h
    assert "coinClientCount" in manager_h
    assert "ClientRole::Coin" in manager_cpp
    assert "listingPlaceholder" in controller_cpp
    assert "功能待完善" in controller_cpp
```

- [ ] **Step 2: 运行并确认失败**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: FAIL，角色管理和上架入口不存在。

- [ ] **Step 3: 实现角色更新和币号计数**

在连接项中使用：

```cpp
ClientRole role = ClientRoleUtil::DefaultRole;
```

提供：

```cpp
bool setClientRole(quint64 clientId, ClientRole role);
int coinClientCount(bool readyOnly) const;
```

计数逻辑必须显式检查：

```cpp
client->connected && client->role == ClientRole::Coin
```

- [ ] **Step 4: 实现上架占位入口**

`ServerController::listingPlaceholder()`：

- 只调用 `coinClientCount(false)`；
- 不发送任何网络消息；
- 状态文案明确为“上架功能待完善，当前币号客户端 N 个”；
- 老板不计入数量。

- [ ] **Step 5: 运行测试**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: 新增角色/上架断言 PASS；UI 断言仍可失败，等待下一任务。

- [ ] **Step 6: 提交**

```powershell
git add DF_PlayClick_Server_Qt/ServerConnectionManager.* DF_PlayClick_Server_Qt/ServerController.* Tests/MergedUiSkeletonTests.py
git commit -m "feat: add coin-only listing placeholder"
```

---

### Task 4: 复现服务端调度窗口 UI

**Files:**
- Modify: `DF_PlayClick_Server_Qt/MainWindow.h`
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: 补全服务端 UI 失败断言**

断言以下控件与文案：

```python
assert "QComboBox" in header
assert "clientRoleComboForRow" in header
assert "倒计时" in source
assert "分组延迟" in source
assert "流程预览" in source
assert "币号 → 延迟 → 老板" in source
```

- [ ] **Step 2: 运行并确认失败**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: FAIL，服务端 UI 尚未重建。

- [ ] **Step 3: 重建窗口布局**

在 `MainWindow.h` 中保留连接控件并新增：

```cpp
QLineEdit* groupDelayEdit_ = nullptr;
QComboBox* triggerOrderCombo_ = nullptr;
QPushButton* triggerAllButton_ = nullptr;
QPushButton* listingButton_ = nullptr;
QLabel* flowPreviewLabel_ = nullptr;
```

界面必须与确认的 mockup 一致：

- 监听设置卡片；
- 全部出发卡片；
- 倒计时和分组延迟并排；
- 顺序下拉包含“币号优先”“老板优先”；
- 客户端表格包含 IP、状态、备注类型、点击策略、操作；
- 上架按钮明确可点击但当前为占位；
- 保留日志和状态栏。

- [ ] **Step 4: 将备注列改为下拉框**

每行使用 `QComboBox`：

```cpp
combo->addItem(QStringLiteral("币号"), static_cast<int>(ClientRole::Coin));
combo->addItem(QStringLiteral("老板"), static_cast<int>(ClientRole::Boss));
```

默认选择币号；改变时调用 `controller_.setClientRole(...)`。

- [ ] **Step 5: 连接占位操作和流程预览**

- `listingButton_` 调用 `listingPlaceholder()`；
- 顺序或延迟变化时只更新流程预览；
- 第一阶段“全部出发”可以调用现有触发入口或显示“调度骨架待接入”，不得伪装高精度分组执行已经完成；
- 输入使用 `QIntValidator`，倒计时和延迟均不允许负数。

- [ ] **Step 6: 运行 UI 契约测试**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: 服务端部分 PASS。

- [ ] **Step 7: Release x64 构建服务端**

Run:

```powershell
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick_Server_Qt\DF_PlayClick_Server_Qt.vcxproj" /p:Configuration=Release /p:Platform=x64 /m
```

Expected: Build succeeded，0 errors。

- [ ] **Step 8: 提交**

```powershell
git add DF_PlayClick_Server_Qt/MainWindow.* Tests/MergedUiSkeletonTests.py
git commit -m "feat: reproduce grouped server control UI"
```

---

### Task 5: 客户端本地 OCR 参数持久化

**Files:**
- Modify: `DF_PlayClick_Client_Qt/ClientConfig.h`
- Modify: `DF_PlayClick_Client_Qt/ClientConfig.cpp`
- Modify: `DF_PlayClick_Client_Qt/ClientController.h`
- Modify: `DF_PlayClick_Client_Qt/ClientController.cpp`
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: 写配置失败断言**

```python
def test_client_ocr_parameters_are_persisted_locally():
    config_h = read(CLIENT / "ClientConfig.h")
    config_cpp = read(CLIENT / "ClientConfig.cpp")
    controller_h = read(CLIENT / "ClientController.h")
    assert "loadUnitPrice" in config_h
    assert "saveUnitPrice" in config_h
    assert "loadBulletTotal" in config_h
    assert "saveBulletTotal" in config_h
    assert 'L"Ocr"' in config_cpp
    assert 'L"UnitPrice"' in config_cpp
    assert 'L"BulletTotal"' in config_cpp
    assert "setOcrParameters" in controller_h
```

- [ ] **Step 2: 运行并确认失败**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: FAIL，OCR 参数配置方法不存在。

- [ ] **Step 3: 实现配置读写**

推荐默认值集中在 `ClientConfig.h`：

```cpp
static constexpr double DefaultUnitPrice = 0.0;
static constexpr int DefaultBulletTotal = 1;
```

INI 节点：

```text
[Ocr]
UnitPrice=0
BulletTotal=1
```

- 单价使用可逆字符串格式保存；
- 子弹总数必须至少为 1；
- 非法配置回退默认值。

- [ ] **Step 4: Controller 暴露 getter/setter**

```cpp
double unitPrice() const;
int bulletTotal() const;
void setOcrParameters(double unitPrice, int bulletTotal);
```

setter 完成校验、内存更新、INI 保存并发射 `ocrParametersChanged`。

- [ ] **Step 5: 运行测试**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: 配置断言 PASS。

- [ ] **Step 6: 提交**

```powershell
git add DF_PlayClick_Client_Qt/ClientConfig.* DF_PlayClick_Client_Qt/ClientController.* Tests/MergedUiSkeletonTests.py
git commit -m "feat: persist local OCR pricing parameters"
```

---

### Task 6: 复现客户端连接与 OCR 状态窗口

**Files:**
- Modify: `DF_PlayClick_Client_Qt/MainWindow.h`
- Modify: `DF_PlayClick_Client_Qt/MainWindow.cpp`
- Modify: `Tests/MergedUiSkeletonTests.py`

- [ ] **Step 1: 补全客户端 UI 失败断言**

```python
assert "ocrValueLabel_" in header
assert "unitValueLabel_" in header
assert "ocrDecisionLabel_" in header
assert "clientRoleLabel_" in header
assert "executionStatusLabel_" in header
assert "窗口绑定" not in source
assert "任务列表" not in source
assert "自动取点" not in source
```

- [ ] **Step 2: 运行并确认失败**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: FAIL，新客户端 UI 尚未建立。

- [ ] **Step 3: 精简 `MainWindow.h`**

只保留新界面实际使用的成员：

```cpp
QLabel* connectionBadge_ = nullptr;
QLineEdit* ipEdit_ = nullptr;
QLineEdit* portEdit_ = nullptr;
QPushButton* connectButton_ = nullptr;
QLineEdit* unitPriceEdit_ = nullptr;
QLineEdit* bulletTotalEdit_ = nullptr;
QLabel* ocrValueLabel_ = nullptr;
QLabel* unitValueLabel_ = nullptr;
QLabel* ocrDecisionLabel_ = nullptr;
QLabel* clientRoleLabel_ = nullptr;
QLabel* executionStatusLabel_ = nullptr;
QTextEdit* connectionLogEdit_ = nullptr;
```

- [ ] **Step 4: 重建客户端窗口**

界面区块：

- 连接服务器；
- OCR 上架判定；
- 本机执行状态；
- 日志。

第一阶段 OCR 值显示“等待 OCR 接入”，不可展示伪造识别结果。客户端角色显示“币号（服务端默认）”或当前已知状态。点击位置显示“等待参考坐标”。

- [ ] **Step 5: 连接参数保存信号**

- 初始化时从 controller 载入单价和子弹总数；
- 输入完成或点击保存时调用 `setOcrParameters`；
- 使用 `QDoubleValidator` 和 `QIntValidator`；
- 非法输入显示红色状态，不覆盖有效配置。

- [ ] **Step 6: 保持连接状态完整**

继续连接现有信号：

- `connectedChanged`；
- `reconnectingChanged`；
- `statusChanged`；
- `connectionLogAppended`；
- `messageRequested`。

新 UI 不再连接窗口枚举、任务编辑、准星、自动取点和任务顺序按钮。

- [ ] **Step 7: 运行 UI 契约测试**

Run: `python Tests/MergedUiSkeletonTests.py`

Expected: 全部 PASS。

- [ ] **Step 8: Release x64 构建客户端**

Run:

```powershell
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick_Client_Qt\DF_PlayClick_Client_Qt.vcxproj" /p:Configuration=Release /p:Platform=x64 /m
```

Expected: Build succeeded，0 errors。

- [ ] **Step 9: 提交**

```powershell
git add DF_PlayClick_Client_Qt/MainWindow.* Tests/MergedUiSkeletonTests.py
git commit -m "feat: reproduce client connection and OCR status UI"
```

---

### Task 7: 全量回归和人工 UI 验收

**Files:**
- Modify if needed: files changed in Tasks 1-6
- Test: `Tests/ProtocolTests.cpp`
- Test: `Tests/MergedUiSkeletonTests.py`
- Test: existing Python regressions

- [ ] **Step 1: 运行新增测试**

```powershell
python Tests/MergedUiSkeletonTests.py
g++ -std=c++17 -I. Tests/ClientRoleTests.cpp -o Tests/ClientRoleTests.exe
& Tests/ClientRoleTests.exe
```

Expected: PASS。

- [ ] **Step 2: 运行现有关键回归**

```powershell
python Tests/ClientClickQueueDelayTests.py
python Tests/ClientStartupGuardTests.py
python Tests/ConnectionHeartbeatTests.py
python Tests/ServerDisconnectedClientTests.py
g++ -std=c++17 -I. Tests/ProtocolTests.cpp -o Tests/ProtocolTests.exe
& Tests/ProtocolTests.exe
```

Expected: PASS；若旧 UI 静态测试因有意移除控件失败，更新测试以匹配新产品行为，而不是恢复旧 UI。

- [ ] **Step 3: 构建整个解决方案**

```powershell
& "D:\visual Studio\visual studio IDE\MSBuild\Current\Bin\MSBuild.exe" "DF_PlayClick.sln" /p:Configuration=Release /p:Platform=x64 /m
```

Expected: Build succeeded，0 errors。

- [ ] **Step 4: 启动两个 Qt 程序做人工验收**

检查：

- 两个程序仍为独立窗口；
- 服务端可监听；
- 客户端可连接并显示心跳/重连状态；
- 新客户端默认币号；
- 服务端可切换币号/老板；
- 上架只显示币号数量且不发送老板；
- 客户端参数重启后恢复；
- 新 UI 无组队、自雷、配装、旧出发和手动取点入口；
- 不出现文字截断、控件重叠和横向溢出。

- [ ] **Step 5: 检查工作树差异**

```powershell
git diff --check
git status --short
```

Expected: 无空白错误；只包含预期文件和原有未跟踪 `README_CLI.md`。

- [ ] **Step 6: 最终提交**

```powershell
git add Shared DF_PlayClick_Server_Qt DF_PlayClick_Client_Qt Tests
git commit -m "feat: add merged DF bullet UI skeleton"
```
