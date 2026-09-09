# 上架流程 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为所有币号客户端实现由服务端批量下发的全分辨率上架流程，并回显每个客户端的成功/失败状态。

**Architecture:** 在现有文本行协议中新增 LISTING_REQUEST/LISTING_RESPONSE，服务端保存每个客户端的上架状态并只向币号发送；客户端串行执行 2560×1440 参考坐标动作、粘贴上架文本和价格，最后通过 OCR 区域是否包含“上架”判断结果。真实输入继续使用 Win32 SendInput，坐标统一按绑定窗口客户区比例换算。

**Tech Stack:** C++17、Qt 5、Win32、Winsock、UmiOCR、pytest/协议单元测试。

---

### Task 1: 协议与回归测试

**Files:**
- Modify: `Shared/Protocol.h`
- Modify: `Tests/ProtocolTests.cpp`
- Create: `Tests/ListingFeatureTests.py`

- [ ] 先增加 LISTING_REQUEST/LISTING_RESPONSE 的失败测试。
- [ ] 实现请求参数（requestId、有兑换、上架价格、上架文本）的构建与解析。
- [ ] 运行协议与 Python 契约测试。

### Task 2: 服务端界面与状态分发

**Files:**
- Modify: `DF_PlayClick_Server_Qt/MainWindow.h`
- Modify: `DF_PlayClick_Server_Qt/MainWindow.cpp`
- Modify: `DF_PlayClick_Server_Qt/ServerModels.h`
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.h`
- Modify: `DF_PlayClick_Server_Qt/ServerConnectionManager.cpp`
- Modify: `DF_PlayClick_Server_Qt/ServerController.h`
- Modify: `DF_PlayClick_Server_Qt/ServerController.cpp`

- [ ] 增加上架价格输入框和“有兑换”勾选项。
- [ ] 客户端表格增加上架状态列，币号显示待执行/执行中/成功/失败，老板显示不适用。
- [ ] 点击上架时校验文本与价格，只向币号客户端下发并更新状态。

### Task 3: 客户端自动化与 OCR 判断

**Files:**
- Modify: `DF_PlayClick_Client_Qt/ClientConnection.h`
- Modify: `DF_PlayClick_Client_Qt/ClientConnection.cpp`
- Modify: `DF_PlayClick_Client_Qt/ClientController.h`
- Modify: `DF_PlayClick_Client_Qt/ClientController.cpp`
- Modify: `DF_PlayClick_Client_Qt/Win32InputDispatcher.h`
- Modify: `DF_PlayClick_Client_Qt/Win32InputDispatcher.cpp`

- [ ] 接收上架请求并拒绝老板角色执行。
- [ ] 按固定坐标与延迟执行流程，第 7 步 5 次退格后粘贴上架价格。
- [ ] OCR `(1448,368,630,700)`，包含“上架”回传成功，否则回传失败。

### Task 4: 验证、发布与提交

**Files:**
- Output: `Relase/DF_PlayClick_Client_Qt.exe`
- Output: `Relase/DF_PlayClick_Server_Qt.exe`

- [ ] 运行全部测试并构建 `Release|x64`。
- [ ] 检查 UTF-8/CRLF、乱码和替换字符。
- [ ] 复制成品到 `Relase` 并校验哈希。
- [ ] 提交隔离分支。