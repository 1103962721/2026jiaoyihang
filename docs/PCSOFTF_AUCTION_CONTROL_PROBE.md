# PCsoftf 老板端：AuctionQtControl 控件探测

探测日期：2026-10-11。初始只读探测及后续已确认实现记录。

## 已验证

- 程序：`C:\Users\Administrator\Desktop\10-2版本\10-2版本\AuctionQtControl.exe`
- 可枚举到顶层窗口，标题为 `交易行 · 库存与交易 · Qt`。
- 本次 PID 为 37572、HWND 为 `0x8079e`。这是运行时快照，后续必须重新枚举，不得硬编码。
- 能读取 UI Automation 控件树，定位目标按钮，并连续两次读取到禁用状态。
- 当前界面显示 `连接异常 · 等待恢复`、`未连接到本机桥接服务，或连接已断开`。

## 目标按钮

截图中顶部“当前任务”面板内的两个按钮：

| 按钮名称 | AutomationId | 本次状态 |
| --- | --- | --- |
| 开始购买 → | `QApplication.dashboard.dashboard-scroll.qt_scrollarea_viewport.scroll-content.shell.QWidget.QWidget.task-panel.start` | 禁用 |
| 开始上架 → | `QApplication.dashboard.dashboard-scroll.qt_scrollarea_viewport.scroll-content.shell.QWidget.QWidget.task-panel.sell-start-quick` | 禁用 |

下方“上架任务”面板还有一个同名“开始上架 →”按钮：

`QApplication.dashboard.dashboard-scroll.qt_scrollarea_viewport.scroll-content.shell.QWidget.sell-task-panel.sell-start`

接入时使用进程/窗口与控件标识共同定位，区分这两个上架按钮。控件树中的临时序号不作为持久标识。

## 后续接入建议与验证范围

- 启用状态指控件是否可操作，与目标窗口是否位于前台、是否已开始交易分别记录。
- 客户端可以使用现有的 Windows UI Automation 技术读取按钮启用状态，优先按控件定位，避免依赖截图 OCR 和固定屏幕点击坐标。
- 调用前重新检查目标进程、窗口与按钮启用状态；检查 InvokePattern 支持情况，再接入按钮调用。
- 本轮没有执行购买、上架或修改该软件配置。尚未验证 InvokePattern 可用性、实际按钮触发结果或启用到禁用的状态变化。
- 用户后续提供老板执行逻辑后再集成；本轮未修改客户端执行代码或编译客户端。

## PCsoftf 已确认流程与实现

- PCsoftf 成品独立命名为 `Relase/DF_PlayClick_Client_Qt_PCsoftf.exe`，不得覆盖 `Andirons` 或原稳定版成品。编译脚本按当前分支选择成品后缀，并写入项目根目录的 `Relase`。

- 只替换 PC 老板的准备检查及触发执行链路；币号与 ADB 老板保持原业务流程。
- PC 老板角色下按 `AuctionQtControl.exe` 枚举绑定目标窗口，不使用固定 HWND 或开发机路径；多个候选时拒绝自动猜测。
- 准备查询：读取顶部当前任务面板的两个按钮。两个都可用回报 `[1/1]`，否则 `[0/1]`；执行前准备请求持续等待两个都可用再回报 OK。
- 原 PC 老板资金 OCR、金额上报、仓库、输入价格、上下架、领取等链路不再在 PC 老板准备/触发时执行。服务端下发价格不再由客户端输入，交易软件自身处理业务。
- 倒计时/定时触发保留：到点后持续等待开始购买可用，通过 UI Automation Invoke 调用一次；观察购买按钮不可用后，持续等待开始上架可用并调用一次。
- 上架调用后先观察上架按钮不可用，再等待两个按钮重新可用，回报本轮完成及 `[1/1]`。未观察到对应忙碌状态时持续等待，防止旧的可用状态误报成功；不擅自按超时判定完成。
- 后台线程约每 100ms 轮询，缓存本轮控件引用；控件失效时重新定位。Qt AutomationId 前缀差异可用严格 `task-panel.start` / `task-panel.sell-start-quick` 后缀兼容，多个匹配则拒绝调用。
- 只调用顶部按钮，不能误调用下方同名上架按钮。准备时检查 InvokePattern 支持，失败时明确报错，不回退到固定坐标点击或 OCR。
- 重复触发不重复执行。停止、断开、踢出、退出及角色切换取消监控和后续调用；已经交给交易软件执行的任务不通过这两个开始按钮停止。
- 执行中不回报 TEAM_READY，避免提前解除服务端动作锁。PREPARE_RESPONSE 自身更新准备状态，后面不追加会与 TRIGGER 竞争的 TEAM_READY。
- 取消资金上报后，PC 老板收益统计及达到期望金额自动停止不再根据客户端资金更新。币号和 ADB 的金额上报保持原样。

## 验证

- `Tests/AuctionRoundStateTests.cpp`：不可用等待、调用顺序、UI 延迟、旧状态防误判、每轮重置。
- `Tests/AuctionControlAutomationTests.pro`：独立真实 Qt 测试窗口与原生 UIA，验证按钮状态、Invoke、两个同名上架按钮区分、取消、关闭及 GUI 响应。
- `python Tests/RunAuctionControllerRuntimeTests.py`：使用生产控制器和独立回环 socket，验证准备/完成协议、无金额上报、重复触发、动作锁、停止回执、PC 币号和 ADB 分流。
- `python Tests/RunAdbWorkerRuntimeTests.py`：ADB 回归，使用模拟 adb/OCR，包含完整下架及减价重上架。
- Qt 模拟窗口上的原生 Invoke 已验证；没有在用户的真实交易软件执行购买/上架，实际交易及按钮状态变化仍需目标软件联调。
