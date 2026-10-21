# LIMIVIA ESP32 固件

LIMIVIA 的独立 ESP32 固件仓库，首版适配 **合宙 CORE ESP32-C3 简约版（无串口芯片、外置 4MB Flash）**。浏览器通过原生 USB Serial/JTAG 或 BLE GATT 控制板卡；前后端工作台由独立的 LIMIVIA 仓库维护。

本仓库包含 ESP-IDF 固件、Poetry/Typer 开发工具、板卡专用 skill 与协议快照，独立构建无需检出工作台。支持 GPIO、D5 LED、4 路 PWM/呼吸、ADC1 采样和 4 个本地定时任务；断线后任务继续，复位后清空。

## 灵感来源与致谢

LIMIVIA 受到 [wave-del 同学的 esp32-audio-web 项目](https://github.com/wave-del/esp32-audio-web) 的启发。在本项目发起者的了解与交流范围内，wave-del 同学最早提出了类似思路，其项目为 LIMIVIA 的构思提供了最初的启发。感谢他对通过网页连接和操纵物理设备这一方向的探索。

## 安装与构建

依赖：Python 3.10、Poetry、ESP-IDF **6.0.2** 及其 ESP32-C3 工具链。CLI 的环境启动逻辑使用 Bash（本次在 Linux 验证）。ESP-IDF 项目位于仓库根目录，默认目标 esp32c3，Flash DIO / 4MB / 40MHz。

```bash
poetry env use python3.10
poetry install
export IDF_PATH=/实际安装目录/esp-idf
poetry run limivia-esp32 doctor
poetry run limivia-esp32 build
```

Espressif Installation Manager 安装可额外设置：

```bash
export LIMIVIA_IDF_ACTIVATE=/实际工具目录/activate_idf_v6.0.2.sh
poetry run limivia-esp32 build
```

CLI 在子 shell 中加载 IDF 环境，不修改全局 shell 配置；Poetry 与 IDF 各自使用独立的 Python。用 `poetry run python --version` 确认 CLI 是 Python 3.10。若 Poetry 实际创建了错误版本，可在移除本项目错误环境后用 `python3.10 -m venv .venv`、`poetry env use .venv/bin/python`、`poetry install` 重建。

也可以在已激活的 IDF shell 中，从仓库根目录直接执行 `idf.py build`。输出位于 `build/`，主固件为 `build/limivia.bin`；完整首次烧录还包含 bootloader 和分区表，由 `idf.py flash` 处理。锁定文件 `dependencies.lock` 随仓库提交，受管组件与构建产物不提交。

## 烧录与实机验证

```bash
poetry run limivia-esp32 flash --port /dev/serial/by-id/实际设备
poetry run limivia-esp32 verify --port /dev/serial/by-id/实际设备
```

`flash` 会构建并覆盖目标板已有固件。执行前断开浏览器 USB 与串口监视器。`verify` 会停止现有实验任务、操纵 D5/GPIO13、采样 ADC0、断开重连并校验状态，最后释放实验引脚；仅对明确选定的实验板运行。可用 `LIMIVIA_SERIAL_PORT` 设置目标端口。首版没有 OTA 或网页烧录器。

无法进入下载模式时，按住 BOOT，再按下并松开 RST，最后松开 BOOT，重新枚举后烧录。原生 USB 使用 GPIO18/19，不能挪作实验。VID:PID 仅识别 Espressif 接口，不能证明板型。

板载 D4/GPIO12 为系统状态 LED，D5/GPIO13 为实验 LED，高电平有效。实验引脚为 GPIO0/1/3/4/5/6/7/10/13/20/21；ADC 仅开放 0/1/3/4。GPIO 使用 3.3V 逻辑，ADC 建议输入 0–2.5V，禁止将 5V 接入 GPIO/ADC。详细资料见本仓库 [板卡与芯片 skill](.agents/skills/limivia-esp32-firmware/SKILL.md)。

## 协议与仓库边界

[docs/protocol.md](docs/protocol.md) 是协议 v1 的本地快照，注明上游来源提交。协议真源在 LIMIVIA 仓库 `docs/protocol.md`。更新协议需协同更新两端实现、快照与测试，不能只改一端。握手用 `hello.protocol` 检查协议兼容性，固件版本来自根目录 CMake 项目版本。

迁移来源为 LIMIVIA 提交 `a6a88afac008d2ed5d0f9dd57107a43961d5c8f3` 中的 `src/esp32` 和实机验证工具；迁移不改变固件功能。工作台的旧 `limivia firmware build/flash/verify` 命令已移除，改用本仓库 `limivia-esp32 build/flash/verify`。

## 开发与验证

```bash
poetry run pytest -q
poetry run ruff check src tests
poetry run limivia-esp32 build
```

VS Code 的“LIMIVIA ESP32 CLI”使用模块入口 `limivia_esp32.commands.app`，读取 `.env`；可从 `.env.example` 复制。终端 CLI 读取当前 shell 环境变量，不自动加载 `.env`。

实机观察与限制见 [验证记录](docs/verification.md)。原生串口验证不等同于浏览器实机验收；没有仪器时不宣称 ADC 电压或 PWM 波形精度已测量。

```text
main/                 ESP-IDF 固件、协议处理与板级策略
sdkconfig.defaults    默认目标与外围配置
dependencies.lock     ESP-IDF 组件锁定
src/limivia_esp32/     Poetry/Typer CLI 与显式实机验证
tests/                工具测试（不操作设备）
docs/                 协议快照与验证记录
.agents/skills/       仅本固件仓库使用的开发知识
```

## 开源协议

本仓库采用 **GNU Affero General Public License v3.0（AGPL-3.0-only）**，完整条款见 [LICENSE](LICENSE)。第三方依赖与另有许可声明的内容保留各自的许可证。
