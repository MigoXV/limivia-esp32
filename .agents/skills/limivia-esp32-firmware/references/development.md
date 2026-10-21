# 开发、烧录与恢复

## 工具边界

Python 应用使用 Poetry Python 3.10 环境；ESP-IDF 使用其自身 Python 与 RISC-V 工具链，两者不能混用。固件锁定 ESP-IDF 6.0.2，cJSON 是外部受管组件，依赖和哈希由 `dependencies.lock` 锁定。

标准安装设置 `IDF_PATH`；通过 Espressif Installation Manager 安装时，还可设置 `LIMIVIA_IDF_ACTIVATE` 指向安装器生成的环境激活脚本。本工程 CLI 会在子 shell 中加载环境，不修改用户的 shell 配置。

```bash
poetry run limivia-esp32 doctor
poetry run limivia-esp32 build
poetry run limivia-esp32 flash --port /dev/serial/by-id/实际设备
```

`flash` 覆盖目标固件，先断开浏览器 USB 和其他串口监视器。运行时重新枚举设备，不把 `/dev/ttyACM0` 当稳定身份。

本次机器的 EIM 把工具、constraints 和 Python 放在自己的目录中，直接 `source export.sh` 会找不到默认 Python 环境；使用 EIM 的 `activate_idf_v6.0.2.sh` 可正确激活。激活脚本通过 `$0` 判断被 source，CLI 子 shell 使用 `bash` 作为 `$0`。

## 固件与 USB

- 目标 esp32c3，Flash DIO / 4MB / 40MHz，单应用分区；首版没有 OTA。
- 应用通过 `usb_serial_jtag_driver_install` 直接读取协议，不依赖 UART0 日志。
- Linux 应识别 `/dev/ttyACM*`，用户需要相应 dialout 权限；macOS 和 Windows 使用本系统枚举出的端口。
- USB 未出现时先检查数据线、供电和 USB GPIO 是否被固件占用。不能假设充电线支持数据。
- 正常复位仍无法下载时，按住 BOOT，再按下并松开 RST，最后松开 BOOT，重新枚举串口并烧录。不要把 eFuse 写入当作恢复措施。

## 浏览器能力

- Web Serial 用于 USB；Web Bluetooth 用于 BLE GATT。需要安全上下文和用户操作触发设备选择；本机使用 `http://localhost:8000` 或 `http://127.0.0.1:8000`。
- HTTP 局域网 IP 不等同于 localhost。手机要访问本工作台需另行提供可访问且受信任的 HTTPS；首版不配置远程部署。
- Chromium 的 Linux Web Bluetooth 支持可能需要实验特性，而且依赖实际可工作的系统 Bluetooth/BlueZ 控制器。
- 浏览器不存在对应 API 时显示不可用，不伪造连接、不自动改用后端通道。
- BLE 服务名 `LIMIVIA-xxxx`，具体服务与特征 UUID 以工程协议文档为准。

## 验证

```bash
poetry run limivia-esp32 verify --port /dev/serial/by-id/实际设备
```

该命令会停止现有实验任务、驱动 D5、采样 ADC0、断开并重新连接 USB，最后释放实验引脚。只能对明确选择的实验板使用。自动检查 GPIO 读回、参数拒绝、资源冲突、周期 ADC 消息、定时脉冲完成与断线保持；不证明外部电压或波形精度。

执行结果与剩余限制记录在工程 `docs/verification.md`。重启恢复需单独验证：启动任务、复位、重新握手，确认任务清空且引脚 idle。浏览器测试、模拟 transport 测试和原生串口实机测试分别报告。

来源：[Chrome Web Serial](https://developer.chrome.com/docs/capabilities/serial)、[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)、[乐鑫串口连接](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c3/get-started/establish-serial-connection.html)。核验日期：2026-10-21。
