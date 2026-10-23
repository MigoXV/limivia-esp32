# 固件验证记录

## v2 / 0.2.0 代码候选与 USB 实机验收

日期：2026-10-22 至 2026-10-23（Asia/Shanghai）。目标为序列号 `E4:B0:63:CA:B4:78` 的既有合宙 CORE ESP32-C3 简约版，通过稳定 by-id 端口明确绑定。范围及判定标准见 [实机计划](hardware-test-plan.md)。

- ESP-IDF 6.0.2 构建和 IDF 生成参数烧录通过，Flash 写入哈希校验成功；分区不变。最终 `limivia.bin` 为 **527712 bytes**。
- 最终设备返回的 `build_id` 与本地 ELF SHA256 相同：`4e418a2f78cd454b84a251754b5b49f058c6a4e1bceb4368234e5da4dfacd676`。
- 生产 C 原生测试 **14 组通过**，Unity + ASan/UBSan；包括共享协议向量、1024/1025 边界、16/17 层、非法 UTF-8/NUL/重复字段、控制与旧会话、停止保留槽、资源故障、ADC 首读及运行失败、定时跳过、合法最大混合负载、逐分配点失败和 BLE 整帧 deadline/代次。
- Python **14 项通过**，Ruff 通过；覆盖显式目标、IDF 环境隔离、分片、超时、中断、错误身份、关闭后重新打开清理及保留原始错误。`poetry check` 通过，仅有既有 Poetry scripts 弃用提示。
- USB 最终用例通过：v2 身份/能力、异常帧拒绝后恢复、无控制权与保护脚拒绝、D5 GPIO 内部读回及上下拉、PWM 5Hz/40kHz 配置及 0%/100% 端点、呼吸配置、ADC0 原始/校准标记/流实例、脉冲自然完成、断开 6 秒后相同 boot 和连续 uptime、旧租约撤销与重新接管。
- USB 慢消费者与停止拥塞测试通过：停止读取让 TX 有界失败，普通槽发生真实拒绝；250ms 本地脉冲仍完成，32 个修改请求之后的独立停止请求使所有实验引脚 idle，旧写入未重启输出。发送失败计数是此用例的预期刺激，不是“所有发送均成功”。
- 重启清空通过：在 D5 无限闪烁期间，用 esptool 5.3.1 的明确目标 `--before default-reset --after hard-reset --no-stub read-mac` 进入 ROM 并复位；boot 从 `a7e4759a645ca7bd` 变为 `41e3de6b6a120e06`，所有实验引脚恢复 idle，任务为空。此过程不写 Flash，不等于冷上电测试。
- 最终重新核对设备并执行停止/查询，**清理已确认**。全部实机刺激只驱动 D5/GPIO13，采样 ADC0。

故障测试曾发现：定时任务清理失败后，软件任务槽号可能被当作 PWM 槽号保留。新增回归先复现失败，再将非 PWM 槽的清除统一到清理边界，最终通过；原失败日志保留在产物目录。此错误由假驱动复现，未对实板注入 GPIO 驱动失败。首轮/后续 USB 验收均保留，后来的通过没有覆盖先前记录。

产物与证据（均为本地 Git 忽略目录）：

- `artifacts/firmware-v2-0.2.0/`：bin/ELF/map、bootloader、分区表、配置、烧录参数、锁定文件、源文件哈希清单、各次烧录与故障回归日志。
- `artifacts/verification/v2-usb-release-candidate.json`：最终固件的全部 USB 用例及原始请求/响应、拥塞计数和最终清理状态。
- `artifacts/verification/v2-reset-before.json` / `v2-reset-result.json`：复位前后身份、状态和 ROM 操作日志。
- `artifacts/verification/ble-fixture.json`：蓝牙夹具失败证据。
- `artifacts/v1-before-v2/`：v1 构建产物及配置/哈希；不是原厂 Flash 完整备份。

当前 **真实 BLE 和双链路验收未完成**：Linux 虽有 hci0，但地址全零、MTU 为零、DOWN；BlueZ 没有 adapter，Bleak 3.0.2 扫描返回 `No Bluetooth adapters found`。`verify-ble` 与 `verify-dual` 已提供，不能用主机核心测试替代 radio 验收。真实用户任务、冷上电、示波器波形及外部已知电压源测试仍未运行；浏览器经后端 SDK 的 USB 实机验证见下一节。此版本是通过 USB 验收的代码候选，不能声称所有发布门槛均已满足。

## 配套 SDK 与浏览器验证

2026-10-23：SDK 0.2.0a1 已发布 PyPI，server 通过官方源 pip 安装；协议真源已同步到 limivia-server。SDK 151 项测试通过；server 后端 23 项、前端 10 项、浏览器模拟流程 30 项通过。

除本仓库直接 USB 验证外，还完成 Chromium → 工作台后端 → PyPI SDK → 同一 USB 板卡的实机验证：身份、控制权、GPIO13 写入、全部停止撤权、重新获取及诊断正常。最终 stop + state 确认实验引脚全部 idle；原始帧和截图在 limivia-server 的 artifacts/server-usb-v2，详细结果见该仓库 docs/verification.md。浏览器实机仅 USB；真实用户测试与 BLE v2 实机仍未完成。

以下为历史记录，不代表当前 v2 行为。

## 迁移前实机记录

日期：2026-10-21，来源：LIMIVIA 提交 `a6a88af`。这是迁移前执行的记录，本次仓库拆分不代表重新烧录或新增实机验收。硬件：合宙 ESP32-C3 USB 简约版，ESP32-C3 revision v0.4，4MB 外置 Flash，MAC e4:b0:63:ca:b4:78。

- 使用本机 ESP-IDF 6.0.2 编译并通过项目 CLI 烧录到当前板，写入哈希校验通过。
- 原生 USB 实机握手、协议版本与设备身份读取成功。
- D5 GPIO 高低电平内部读回、输入模式、上下拉及引脚释放通过。
- USB、Flash、绑带脚及 D4 状态灯保护通过；PWM 占用冲突和非法频率请求被拒绝，原配置保持。
- PWM 和呼吸配置、5Hz/40kHz 边界、0%/100% 端点配置、停止通过；ADC1 单次读数与校准 mV 返回、周期采样事件通过。
- 本地脉冲完成后回到低电平；持续闪烁任务在 USB 断开 6 秒、重新打开后仍存在，设备运行时间连续；重连可停止。
- 重启前启动任务，使用 ROM 下载流程复位后重新查询，任务清空、所有实验引脚为 idle。
- 超长请求、深度嵌套 JSON、错误协议版本均被拒绝，随后设备正常响应。

原生串口自动验证工具现位于 `src/limivia_esp32/validation.py`，执行 `poetry run limivia-esp32 verify --port ...`。最后一次实机验证结束后释放了全部实验引脚。

## 未验证与实际失败

- 当前 Linux BlueZ 没有暴露可用控制器：`bluetoothctl list` 为空，`show` 返回 No default controller available。BLE 固件构建及初始化通过、浏览器 BLE 适配器模拟测试通过；真实广播、手机/浏览器 GATT 连接和实际 BLE 收发尚未验证。
- 当前无头 Chromium 未在 Web Serial 设备选择中枚举到目标板，返回 NotFoundError。真实 USB 控制验证使用本机原生串口；不能声称完成了浏览器 USB 实机验收。
- 未连接已知电压源、示波器或逻辑分析仪，ADC 浮空读数仅验证接口；PWM/脉冲实际精度、可见 LED 发光未作物理测量确认。
- 烧录前尝试读取原有 4MB Flash 备份，在约 1MB 处出现 `Packet content transfer stopped`，未生成完整可恢复备份。随后按授权覆盖安装专用固件，烧录校验成功。

## 独立仓库验证

日期：2026-10-21。

- Python 3.10.20、独立 Poetry 环境安装成功；不依赖 LIMIVIA 的包或相邻目录。
- `poetry run pytest -q` 5 项通过：烧录/验证必须选择目标、缺失 IDF 提示、含空格路径与串口参数传递、Poetry/IDF 环境隔离及构建失败退出码。测试不连接设备。
- `poetry run ruff check src tests`、`poetry check` 与固件 skill 校验通过。Poetry 对 `[tool.poetry.scripts]` 给出弃用提示，当前遵循工程规范保留此入口。
- `poetry run limivia-esp32 build` 从新仓库根目录完成 ESP-IDF 6.0.2 全量编译，生成 `build/limivia.bin`（512992 bytes）；构建后 Poetry 仍为 Python 3.10.20。
- 从迁移前提交逐字节比对 10 个固件源文件与实机验证脚本，内容一致；协议正文与主仓库一致。
- 本次仅构建与工具测试，没有重新烧录或运行会改变板卡状态的实机验证。

## 本地产物

`artifacts/usb-verification.json` 保存迁移前的原生串口验证结果；`artifacts/pre-migration-build/` 保留原路径下的生成产物，仅用于追溯，不用于新仓库构建。新固件输出到 `build/`。这些目录均被 Git 忽略。
