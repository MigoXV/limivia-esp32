# LIMIVIA ESP32 固件

面向 **合宙 CORE ESP32-C3 简约版、原生 USB、外置 4MB Flash** 的独立实验固件。当前固件 **0.3.0-dev / 业务协议 v3 / JSON-RPC 2.0**，通过资源、作业、缓存和离线流程，覆盖总线实验、信号采集与无人值守节点。本轮仅交付固件及开发工具；现有 SDK 0.3.0a1 使用历史自定义 v3 包络，**不能连接当前 JSON-RPC v3**；SDK/server 本轮均未适配。旧包络直接拒绝，不提供兼容层。

USB Serial/JTAG、BLE GATT、Wi-Fi WebSocket 和 IoT MQTT 控制共享同一应用协议，分别持有会话。一个控制者，任何有效会话可执行全局停止；控制 token 用于协调，开放实验室模式不提供访问认证。

## 镜像与引脚

|能力|bus|signal|iot|
|---|---|---|---|
|GPIO/IRQ、6路PWM/4共享定时器、ADC1单次/周期、本地定时、温度|✓|✓|✓|
|资源矩阵、异步作业、分块缓存、离线流程、LittleFS/NVS、OTA|✓|✓|✓|
|USB/BLE、Wi-Fi STA/显式维护AP、WebSocket|✓|✓|✓|
|I²C主机、RMT 2TX/2RX|✓|✓|✓|
|UART 2控制器、SPI主机、TWAI经典CAN|✓||✓|
|I²C/SPI从机|✓|||
|连续ADC、I²S Std/TDM/PDM、4路Sigma-Delta||✓||
|MQTT3.1.1/5+TLS、ESP-NOW、低功耗|||✓|

表中表示已接入驱动和协议；实机覆盖及外部互通缺口见 [验证记录](docs/verification.md)。资源上限不能全部同时用满，引脚、控制器、DMA和剩余RAM共同约束准入。

逐引脚与控制器共享规则见 [外设矩阵](docs/peripheral-matrix.md)。

开放 GPIO **0/1/3/4/5/6/7/10/13/20/21**。GPIO **0/1/3/4 既可作 ADC1，也可作数字引脚**，先关闭原资源再改用途；模拟通道固定，数字信号经 GPIO Matrix 路由。保护绑带、Flash、USB18/19、Flash电压11和系统LED12。GPIO13连接D5，有板载LED负载；GPIO5仅开放数字功能。使用3.3V逻辑，电气边界见 [板卡资料](.agents/skills/limivia-esp32-firmware/references/board-and-chip.md)。

## 构建

依赖 Python3.10、Poetry、ESP-IDF **6.0.2** 和 ESP32-C3 工具链：

```bash
poetry env use python3.10
poetry install
export IDF_PATH=/实际安装目录/esp-idf
# EIM 安装时额外指定其激活脚本：
export LIMIVIA_IDF_ACTIVATE=/实际工具目录/activate_idf_v6.0.2.sh
poetry run limivia-esp32 doctor
poetry run limivia-esp32 build --profile bus
poetry run limivia-esp32 build --profile signal
poetry run limivia-esp32 build --profile iot
```

各镜像的配置和产物分别位于 `build/bus`、`build/signal`、`build/iot`，应用名为 `limivia-<profile>-v3rpc.bin`。CLI 以独立子 shell 激活 IDF，不把 Poetry 环境传入 IDF；无需相邻仓库。依赖由 `dependencies.lock` 锁定。

## 安装、验证与升级

首次从旧固件迁移会更换分区表。先读取并校验完整4MB备份，再按新分区烧录；详见 [开发与恢复](.agents/skills/limivia-esp32-firmware/references/development.md)。不因文件系统挂载失败自动格式化。

```bash
poetry run limivia-esp32 flash --profile bus --port /dev/serial/by-id/明确目标
poetry run limivia-esp32 verify-rpc --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify-profile --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify-variants --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify-lifecycle --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 ota --port /dev/serial/by-id/明确目标 --expected-device 目标MAC --image build/signal/limivia-signal-v3rpc.bin
```

验证命令会停止实验、操作允许引脚、写入测试文件；lifecycle 还会重启，IoT 增加睡眠、无线扫描和显式维护AP。每次写入唯一证据文件，失败记录不覆盖。BLE 使用明确的 `verify-ble --address`；双链路用 `verify-dual --port ... --address ...`，需要可用蓝牙控制器。

OTA 写非活动槽，校验长度、SHA256、芯片和项目/profile，启动自检后确认；故障镜像可回滚。OTA 不更新bootloader和分区表。ROM恢复：按住BOOT，按下并松开RST，再松开BOOT；重新枚举后选择明确端口。禁止随意占用USB18/19或烧写eFuse。

## 协议与开发

[协议v3](docs/protocol.md) 是本轮固件先行的集成契约；历史上游协议来源保存在 [v2快照](docs/protocol-v2.md) 和 [v1快照](docs/protocol-v1.md)。以后同步SDK/工作台时逐项对照契约与测试，不能假定不同仓库版本相同。

```bash
poetry run pytest -q
poetry run ruff check src tests
poetry run limivia-esp32 test-native
poetry run limivia-esp32 build --profile bus
```

真实消息形状向量在 `tests/vectors/jsonrpc-captured.json`，包含冻结ELF、证据SHA与来源。可用 `export-rpc-vectors --evidence 证据目录 --manifest 冻结清单 --output 新文件.json` 重新导出；输出不覆盖已有文件。向量用于协议解析，历史句柄不可直接重放，扫描到的SSID已脱敏。

原生测试运行生产C核心及帧/发送逻辑，启用ASan/UBSan；不连接设备。架构所有权见 [架构](docs/architecture.md)，产品取舍见 [产品规划](docs/product-plan.md)，交付阶段见 [v3计划](docs/v3-plan.md)，证据账本见 [v3状态](docs/v3-status.md)。

`main/` 为固件；`profiles/` 为镜像差异；`src/limivia_esp32/` 为Poetry/Typer开发工具；`tests/` 为软件测试；`docs/` 和仓库专用 `.agents/skills/` 维护契约与知识。开发工具不是产品SDK，不发布为旧SDK的替代包。

## 灵感来源与致谢

LIMIVIA 受到 [wave-del 同学的 esp32-audio-web 项目](https://github.com/wave-del/esp32-audio-web) 的启发。在本项目发起者的了解与交流范围内，wave-del 同学最早提出了类似思路，其项目为 LIMIVIA 的构思提供了最初的启发。感谢他对通过网页连接和操纵物理设备这一方向的探索。

历史独立仓库迁移来源为 LIMIVIA 提交 `a6a88afac008d2ed5d0f9dd57107a43961d5c8f3` 的 `src/esp32` 与实机工具。

## 开源协议

**AGPL-3.0-only**，详见 [LICENSE](LICENSE)。第三方依赖保留各自许可证。

### 通知突发缓冲修复

固件每个发送通道的通知缓冲为有界16格，容纳普通状态任务的突发通知；总发送字节限制与RPC响应优先级保持原有规则。通知仍是可丢失的查询提示，客户端不能用通知缺口撤销已确认的设备状态。SDK 0.3.0a3另行修正采样丢弃计数的解释。
