# 开发、烧录与恢复

## 独立环境与三镜像

Poetry/Python3.10只运行开发CLI；IDF6.0.2使用自身解释器。设置IDF_PATH，EIM安装另设LIMIVIA_IDF_ACTIVATE指向activate_idf_v6.0.2.sh。CLI以Bash子shell加载它，不传入VIRTUAL_ENV/POETRY_ACTIVE/PYTHONHOME。源码根目录就是IDF工程，不依赖相邻SDK/server。

```bash
poetry run limivia-esp32 doctor
poetry run limivia-esp32 build --profile bus
poetry run limivia-esp32 build --profile signal
poetry run limivia-esp32 build --profile iot
```

配置与产物隔离build/<profile>，不能使用旧根build/limivia.bin；直接idf.py也要显式-B build/<profile> -DLIMIVIA_PROFILE=<profile>。同一构建目录不要一边重构建一边烧录。dependencies.lock锁定cJSON、LittleFS1.23.0、MQTT1.1.0。

## 迁移与安装

目标仅esp32c3、外置4MB/DIO/40MHz、原生USB简约版。新分区含双OTA槽和LittleFS，详见仓库partitions.csv。首次从旧分区迁移前，完整读取0..0x400000并核验长度/SHA；默认stub读取失败不代表备份成功，改用ROM/no-stub并保留首次失败。当前完整备份位于artifacts/v2-before-v3/flash-4mb-rom.bin，SHA见docs/v3-status.md。

```bash
poetry run limivia-esp32 flash --profile bus --port /dev/serial/by-id/明确目标
poetry run limivia-esp32 ota --port /dev/serial/by-id/明确目标 --expected-device 目标MAC --image build/signal/limivia-signal-v3rpc.bin
```

flash写入bootloader/分区/初始otadata/应用，必须先完成迁移备份。日常换profile使用OTA保留当前分区，直接写非活动槽并校验。OTA不更新bootloader、分区或eFuse。首次新FS初始化需明确storage.format{confirm:"erase_littlefs"}；不把格式化当作挂载失败的自动恢复。

ROM备份与回写可使用IDF自己的esptool，完整恢复时仅对已识别设备执行--no-stub write-flash 0x0 备份.bin；先检查备份SHA与板型，完整回写会恢复旧分区及全部数据。实际命令以当前esptool --help为准，不把未完成读取当成可恢复备份。

## 原生USB与恢复

USB固定GPIO18/19，不能挪作实验；无USB-UART桥。应用UART控制台和bootloader日志关闭，USB仅协议。Linux枚举/dev/ttyACM*，稳定身份以by-id+握手MAC确认，不固定tty编号。PySerial保持本板已验证的默认DTR/RTS；此前显式撤销控制线会复位，不能用它证明不断电断线。

正常复位不能下载时按住BOOT，按下松开RST，再松开BOOT，重新枚举端口并烧录。检查数据线/供电，不写eFuse“解锁”。一次失败后先明确修复假设，再重烧录，保留原失败和清理状态。

## 验证工具

```bash
poetry run pytest -q
poetry run ruff check src tests
poetry run limivia-esp32 test-native
poetry run limivia-esp32 verify-rpc --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify-profile --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify-lifecycle --port /dev/serial/by-id/明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify-ble --address 明确目标 --expected-device 目标MAC
poetry run limivia-esp32 verify-dual --port /dev/serial/by-id/明确目标 --address 同一板BLE地址 --expected-device 目标MAC
```

verify-rpc验证标准包络、ID保真、通知、滑动序号防重复、batch容量和停止屏障；verify包含GPIO/ADC/数字复用、UART内部回环、能力页、USB拥塞停止；profile包含总线/信号驱动、原子文件和离线记录；lifecycle会重启，IoT还深睡/浅睡、无线扫描、显式AP、MQTT/ESP-NOW驱动生命周期。只在无外部危险负载的目标板运行。结束重新核对同设备，等待stop完成并分页确认无资源；无法确认清理时必须记录unknown。

BLE/双链路需要实际可用控制器；本轮hci0初始化失败不能当成有效夹具。WebSocket需主机能访问板上Wi-Fi；SDK/浏览器不是本仓库组件。每次输出唯一JSON，不覆盖失败。内部回环/驱动创建/广播MAC回调不证明外部互通或物理精度。

LV_TEST_BOOT_FAILURE仅单独build/rollback用于OTA首次健康确认前复位测试，默认OFF，不能作为正常交付镜像。证据与当前缺口以docs/verification.md、docs/v3-status.md为准。
