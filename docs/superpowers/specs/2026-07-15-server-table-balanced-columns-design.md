# Server Table Balanced Columns Design

## Goal

修复服务端客户端列表中 IP 列占用过多空间、备注类型和操作列被压缩的问题，优先保证胶囊角色切换、完整坐标和断开按钮正常显示。

## Root Cause

当前 IP 与坐标列使用 Stretch，而备注类型和操作列使用 ResizeToContents。QTableWidget 对单元格内 QWidget 的尺寸提示处理不足，导致备注胶囊和断开按钮对应列只按表头文字宽度收缩。

## Approved Design

- IP、备注类型、点击坐标三列统一使用 Stretch，平均分享表格的可用宽度。
- 操作列使用 Fixed，并设置为足够完整显示“断开”按钮的宽度。
- 不改变窗口整体布局、配色、角色逻辑、坐标逻辑或网络逻辑。
- 保留现有最小窗口宽度；正常窗口宽度下三列均衡扩展，操作列始终可见。

## Verification

- 源码契约测试确认前三列为 Stretch、操作列为 Fixed，且操作列明确设置宽度。
- 重新构建服务端 Release。
- 启动服务端进行冒烟验证。

## Compact Role Capsule Refinement

- 备注类型列继续均衡伸缩，但单元格使用透明外层承接表格尺寸。
- 胶囊控件固定为 132px，并在备注类型列内水平居中。
- 两个角色按钮保持紧凑，不随列宽铺满。
