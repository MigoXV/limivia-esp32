# 固件验证记录

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
