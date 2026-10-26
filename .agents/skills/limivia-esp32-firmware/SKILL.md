---
name: limivia-esp32-firmware
description: 在 limivia-esp32 仓库开发、构建、烧录与验证合宙 CORE ESP32-C3 简约版v3固件，处理资源矩阵、总线与信号外设、离线流程、USB/BLE/Wi-Fi、存储和OTA。仅适用于确认过的无串口芯片、外置Flash板型。
---

# LIMIVIA ESP32 固件

本技能仅服务含根目录 `CMakeLists.txt`、`main/` 与 `src/limivia_esp32` 的固件仓库，不安装到全局。浏览器与 Python 后端由独立 LIMIVIA 仓库维护。

## 按任务读取

- 板型、引脚、电气边界或驱动参数：读 [板卡与芯片](references/board-and-chip.md)，区分芯片能力、板上接线和本固件开放范围。
- 构建、烧录、原生 USB 故障或实机验证：读 [开发与恢复](references/development.md)。串口编号与 MAC 仅为历史观察。
- 命令、分片或能力变更：读 [v3 契约](../../../docs/protocol.md) 和 [外设矩阵](../../../docs/peripheral-matrix.md)。本轮用户授权固件、SDK 和 server 协同修改；历史上游来源在 [v2快照](../../../docs/protocol-v2.md) 和 [v1快照](../../../docs/protocol-v1.md)。跨仓库适配必须核对版本与测试，历史SDK0.3.0a1不能连接JSON-RPC v3，当前配套SDK为0.3.0a7；必须核对contract=limivia-v3-jsonrpc2-r2。

## 不可丢失的工程约束

- ESP-IDF 6.0.2，根目录为 IDF 项目；Poetry Python 3.10 只负责 CLI，不能污染 IDF 自身的 Python 环境。构建无需相邻主仓库。
- 本板没有 USB-UART 桥，GPIO18/19 保留给原生 USB Serial/JTAG，禁止为 USB 故障随意配置这两个脚。
- 目标为外置 4MB Flash / DIO 板；D4/GPIO12 与 D5/GPIO13 高电平 LED 映射不能用于内置 Flash 变体。
- 固件是引脚和资源校验的最终边界。保护 USB、Flash、绑带脚、GPIO11 与状态 LED；不执行 eFuse 解锁。
- PWM 与本地任务断线继续，重启清空；时间更新不能等待 USB/BLE 发送。慢消费者可丢通知，查询状态作为恢复依据。
- v3核心由 `main/engine.c` 的actor独占；`resources.c`管理矩阵与保护，`operations.c`共用校验/发现。驱动关闭失败保留fault与占用，`outputs.stop`撤权并持久抑制自启，旧完成不能恢复输出。职责见 [架构](../../../docs/architecture.md)。
- 硬件修改需编译及相关测试。烧录和实机验证需明确目标串口与影响；沿用已有授权，不因读取技能而授权操作其他板卡。
- 协议确认与物理测量分别报告；没有仪器时不声称确认 PWM 精度、ADC 电压精度或可见 LED 发光。
