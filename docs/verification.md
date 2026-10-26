# 2026-10-25：固件 0.3.0a4 BLE实机对照

414项Python测试、三项生产C原生CTest（ASan/UBSan）、本次参数候选bus/signal/iot构建通过。程序基线1526d7a，正常bus ELF为618aa4944403692d81ff27e8ade4bf09452fc207b8ae0557c1b19390ddc91e1c，镜像SHA256为d02bccfd4602e48fbb5e9184a1aa9b607457b2f89329a490527cd20e0602ac6e。正常构建二进制与ACL包装均关闭。

管理容量对齐SDK为1/4/1，保留拥塞退避和完整帧截止；等GATT订阅完成后才单次提议15～30ms/4秒连接参数，拒绝不影响RPC。中心实际接受30ms，ATT MTU185。a4 SDK配套下100次ping、100次状态页、30次快照和10次能力发现均成功，完整快照均值约0.40秒。

压缩候选已实测相同只读矩阵，P95收益不足且部分指标回退，不启用，不宣称其持续或跨平台验收通过。后台观察下GPIO13百次写入回读成功；持续段按用户要求在28.54分钟停止，622轮ping/完整快照均成功；没有完成一小时或最终平台矩阵。早期原镜像与最小NimBLE程序均复现GATT前故障，物理断电后才重新取得业务样本；没有单方面归因固件或适配器。

补做真实Chromium工作台BLE功能短测：点亮/熄灭GPIO13并分别回读1/0，释放后资源为空，页面刷新仍ready且保持BLE，页面异常0。首轮脚本的资源字段层级错误已保留证据并修正，没有重复点亮写入；不扩大为全外设通过。

完整分布、真实HCI证据与尚未完成项见 [工作台专项记录](../../limivia-server/docs/ble-a4-verification.md)。Windows本轮未测；原始artifacts不会随合并迁移。所有刷写仅目标应用槽，没有改分区或eFuse，也没有推送。

# JSON-RPC v3 最终验证记录

日期：2026-10-23。目标：合宙CORE ESP32-C3简约版，原生USB、外置4MB/DIO/40MHz，C3 rev0.4，MAC `e4:b0:63:ca:b4:78`。固件0.3.0-dev，业务协议仍为3，JSON-RPC为2.0，contract为 `limivia-v3-jsonrpc2`。

本轮仅修改固件及仓库开发工具。历史自定义v3已失效；现有SDK0.3.0a1和旧server均未适配，不能据相同版本号推断兼容。旧协议的事实保存在 [历史验证](verification-custom-v3.md)，当前契约见 [协议](protocol.md)。

## 2026-10-25 第二块新板烧录

目标MAC `90:64:9b:45:e9:0c`，原生USB、ESP32-C3 rev0.4、4MB Flash（manufacturer 20 / device 4016）。写入上一块新板验证通过的bus镜像，build_id `e70bd914b180b0a6954d4ba34580eda9295082d5e34ecaba707160e76b6ef3e1`，bin SHA256 `d8444775fd01a685ea4fa827c5841db58a3ec9258a52f008676b2511256faf0f`。stub完整读取中断，失败日志保留；ROM/no-stub完整备份4MB并核验长度/SHA后烧录，写入哈希校验通过。清除旧NVS并显式初始化LittleFS。

证据在 `artifacts/new-board-45e90c/`。有线verify通过能力分页、D5/GPIO13高低数字读回、ADC0与GPIO互斥复用、UART内部回环、温度诊断及USB拥塞停止；补测GPIO10高低数字读回，清理确认无剩余资源。未进行外接LED物理测量或BLE测试，本次没有修改固件业务代码。

## 2026-10-25 新板有线烧录

目标为 `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_E4:B0:63:CA:BD:D4-if00`，MAC `e4:b0:63:ca:bd:d4`，ESP32-C3 rev0.4、外置4MB Flash。完整备份及全部失败/成功记录保存在 `artifacts/new-board-cabdd4/`；首次旧 NVS/文件区清除后重新初始化 LittleFS。

原静默配置 `ESP_CONSOLE_NONE` 在此板多次握手超时；USB控制台配置可正常运行。交付配置保留 `ESP_CONSOLE_USB_SERIAL_JTAG` 初始化，并关闭默认应用日志、bootloader日志和副控制台，未保留临时HAL发送尝试。低层根因尚未单独证明，不能归因于SDK或旧数据。已有build目录需显式更新sdkconfig或重新配置，defaults不会覆盖旧配置。

最终bus ELF build_id为 `e70bd914b180b0a6954d4ba34580eda9295082d5e34ecaba707160e76b6ef3e1`，bin SHA256为 `d8444775fd01a685ea4fa827c5841db58a3ec9258a52f008676b2511256faf0f`。写入哈希校验通过；`verification-quiet.json`确认能力分页、D5/GPIO13高低电平读回、ADC0与GPIO互斥/复用、UART内部回环、温度、诊断、USB拥塞下停止及最终空资源。`gpio10-storage.json`确认GPIO10高/低读回、LittleFS初始化、最终无资源。数字读回与内部回环不是外部LED亮度或外接线路互通测量。

SDK 0.3.0a3经server重新连接后 ready、stale=false、snapshot_sync=current，工作台持有新板连接。Python414项测试与Ruff通过。本次仅重建并烧录bus，未重建signal/iot或测试BLE。

## 冻结镜像与软件验证

固件源码提交 `e0340006bd2c7bd6fb8345295299ce3085c45fe0`，验证工具提交 `64e312be720bcf7ae71e760ac9dde576dc349e1d`。IDF6.0.2、三profile独立构建、LV_TEST_BOOT_FAILURE=OFF；最终构建没有编译警告或错误。

|profile|应用字节|OTA槽剩余字节|完整ELF build_id|
|---|---:|---:|---|
|bus|1296240|473232|e4949bbcbcca673b3b1729e6cbb61dc3a1f67a54c373e55a8c21d1d43e7b1de4|
|signal|1268272|501200|4087bd92c491c61650b5dab7f21caba582a88bf9d00156ba9a516913058195ed|
|iot|1441152|328320|a6a933bb3d749ee03814c4085fb5e1e2882327c4b462a3da37a29e4191056d07|

本地忽略目录 `artifacts/firmware-v3-jsonrpc2-release/` 保存bin/ELF/map、bootloader、分区表、sdkconfig、CMakeCache、flash参数、依赖锁、固件及工具源码归档与manifest.json。manifest记录bin SHA256、ELF build_id和生产源文件SHA，两种身份不能混用。

- `poetry run pytest -q`：414项通过，其中379项为真实固件报文解析向量。
- `poetry run ruff check src tests`、`poetry check --lock`通过；Poetry保留现有脚本声明弃用提示。
- `poetry run limivia-esp32 test-native`：35项生产C核心测试、2项传输测试，ASan/UBSan通过。覆盖原有资源/作业/流程语义，以及ID保真、命名/位置参数、nonce、滑动序号、通知、batch、停止屏障、外部stop插入、响应容量、逐JSON分配失败和扩容不重复执行。
- `tests/vectors/jsonrpc.json`保存12组独立请求案例，实际通过USB送入设备；`jsonrpc-captured.json`保存379条去重消息形状，来自15份通过且清理确认的实机记录，含三类设备通知。头部记录精确ELF、来源SHA及被排除的失败记录。请求可能脱敏、SSID替换为LAB-FIXTURE，不能直接重放历史句柄。

## 三profile实机结果

每套镜像均完成OTA上传/hash校验、实际重启后的精确ELF匹配与健康槽查询；随后执行五组验收。下表是各组命名检查数，不代表外部器件或所有参数组合均已验证。

|profile|verify-rpc|verify|verify-profile|verify-variants|verify-lifecycle|
|---|---:|---:|---:|---:|---:|
|bus|11|6|10|3|3|
|signal|11|5|8|4|3|
|iot|11|6|8|3|6|

原始记录位于 `artifacts/jsonrpc/final/`，命名为 `<profile>-<命令>-release.json`；IoT lifecycle最终通过记录为 `iot-verify-lifecycle-retry-release.json`。

- JSON-RPC：旧包络拒绝、标准错误、字符串/大整数/null ID、纯通知与混合batch、8项上限、超限零执行、重复写不重建buffer、乱序序号、聚合响应容量兜底、拆分UTF8与超长恢复。
- 停止：batch内stop位于任意位置时先执行并阻止其他修改；另以同一USB写入普通batch和独立stop，真实观察到中途抢占，余下control.acquire被拒绝且owner保持撤销。普通USB拥塞验收也保留真实准入拒绝与优先停止证据。
- 基础与总线：GPIO13内部读回、ADC0/数字复用、UART内部回环、双UART、I²C/SPI主从生命周期、TWAI/RS485、RMT及父子关闭等适用profile路径。
- signal：连续ADC标签、PWM共享定时器、RMT、I²S standard/TDM/PDM与Sigma-Delta等适用路径。
- 系统：存储/流程、订阅、worker回收、手动流程不自启、异常复位从入口、自启抑制持久化；IoT另验证计划deep sleep检查点、light sleep定时唤醒、Wi-Fi扫描/显式维护AP、MQTT3.1.1/5客户端及ESP-NOW驱动生命周期。

内部回环、驱动成功和ESP-NOW广播发送回调不等于外部互通。sleep elapsed_us为软件观察，不是功耗或物理时钟精度测量。

## 延迟、内存与最终状态

以下为同一SerialLink主机路径、无control token、30次session.ping往返的实测，单位ms；包含主机调度与USB传输。

|profile|中位数|P95|最大|
|---|---:|---:|---:|
|bus|6.00|6.02|6.03|
|signal|5.00|5.71|5.92|
|iot|6.00|6.02|6.06|

旧自定义v3的bus基线为中位4.00/P95 4.14 ms；本轮bus约6 ms，没有证明延迟改善，也没有分离包长与调度各自的贡献。

单个GPIO13资源的独立停止测试：accepted回复 9.58 ms，原始stop job查询到completed为 17.50 ms，cleanup_faults/persist_error均为0。记录见 `stop-completion-timing.json`。这是主机观察到的清理/NVS完成，不是物理关断边沿，也不是所有总线事务的停止上限。

每帧输出从512字节起步，按需扩容至8194（含LF/NUL）；扩容只重编码已持有结果，不重新执行业务。先前固定8KB候选在IoT无线生命周期中的minimum_heap为7016字节，按需扩容候选同组复验为14896字节、free_heap 27528、largest_block 14336。诊断包含采样当时在途开销，仅代表本次组合，不能当作TLS或全部外设并发容量保证。

最终留板bus，build_id为 `e4949bbcbcca673b3b1729e6cbb61dc3a1f67a54c373e55a8c21d1d43e7b1de4`。`final-device-state.json`确认resources/flows/files均为空，四个buffer槽均可分配且随后删除，owner_endpoint=-1，自启抑制已持久化、inhibit_pending=false，NVS/FS/检查点错误均0。六组查询后的空闲堆样本为 `[96708, 96708, 96708, 96708, 96708, 96708]`；该观察窗内未持续下降。

## 备份、失败与恢复

首次切换前ROM/no-stub完整读取4194304字节，`artifacts/jsonrpc/pre-jsonrpc-flash-4mb.bin` SHA256为 `d88eaa04e65629868c87743f594ebcc9a472d22d8ff053158cde3b71873f430e`，文件权限600。与当前分区表逐字节核对一致。本轮没有重新分区、格式化或写eFuse；原v2迁移备份也保留。首次使用明确串口烧录，之后全部经新协议写非活动OTA槽；新镜像项目标识v3rpc，拒绝误装历史自定义v3镜像。

- 首次构建的snprintf重叠检查告警保存在build-bus-first.log，已改为明确的分段复制，最终构建通过。
- IoT lifecycle首次复验在light sleep之后出现首条hello回复前缀丢失，原始 `iot-verify-lifecycle-release.json` 保留。power.sleep的accepted已收到；随后独立查询原job20，确认completed、boot不变、wakeup_causes=16，见light-sleep-recovery.json。没有重发该睡眠命令。
- 修正验证夹具为显式等待USB握手就绪，有限次重新打开和hello，不重放睡眠/重启/外设事务；通用Client.request仍不自动重试。复验六个lifecycle检查全部通过并清理确认。被排除的失败记录仍列在向量excluded_evidence中。
- 收尾临时脚本曾误用storage.list的pages字段，已按实际more字段修正；摘要保留在snapshot-helper-first-error.json，最终快照通过。

## 验证边界

主机BlueZ没有可用adapter，本次未建立到板上Wi-Fi的测试链路。真实BLE/双链路、板上WebSocket、MQTT/TLS broker及ESP-NOW第二节点互通未运行；MQTT驱动创建成功不代表broker/TLS连接成功。

外部I²C/SPI/RS485/CAN器件、外部音频/RMT输入、逻辑分析仪、已知电压、功耗和冷上电也未验证。最终新镜像验证了OTA激活/健康确认，本轮未重做首次启动失败故障注入；历史回滚记录不冒充本轮证据。当前交付仍为开发候选。

## 2026-10-25：通知突发缓冲修复与 USB 验收

分支 `fix/board-state-session` 从 dev 创建，实现 c92b2a0。每传输端通知队列容量由 1 增至 16，保持响应优先及总 TX 字节上限。414 项 Python 测试、Ruff、2 项原生 C 测试通过，bus/signal/iot 三种配置均构建成功。

明确 USB 设备 e4:b0:63:ca:b4:78，原生 USB by-id 以 E4:B0:63:CA:B4:78-if00 结尾。升级前停止实验、确认资源清空，完整读取 4MiB Flash；备份 SHA256 为 d3f373c0a5b8948a98f6dc8161757aa5774add960ba24794638dc8ec65a230c8。沿用 OTA 激活流程写入 bus，核对上传哈希及实际启动 ELF build `9e80cd5bf1602899e3d0967c1146436554dcba6ec166666b7b09b23f27e88a0f`，boot `f5eb6a9cc04609d8`。未改分区、未格式化文件系统、未写 eFuse。

SDK 0.3.0a3/server 实例经无头浏览器进行 D5/GPIO13 操作：60 秒 3,600 帧观察稳定、20 次切换、跨页面控制及关闭资源成功；通知 gap=0，lost_notifications 0→0、tx_failed 0→0。诊断仍记录 rejected 0→1、admission_rejected 0→2，不能据此声称所有准入计数为零。最终资源为空。

原始备份、OTA transcript、两主题三尺寸截图与实测 JSON 在 server 本机 artifacts/board-state-session/，为 Git 忽略产物。仅 bus 镜像上板；signal/iot 此轮完成编译，未分别上板验证。BLE、网络与外部负载未测，协议读回不替代仪器或肉眼检查。

## 0.3.0a7：正式 WebSocket 修复与 PWM 确认（2026-10-26）

源代码 `e6db370`、`e03dc45`、`562623a`，SDK 配套 0.3.0a7 / r2。ESP-IDF 6 握手后回调负责两槽 WS 初始化和清理，每连接 TCP_NODELAY；拒绝空 TEXT 与消息级分片。PWM 创建/duty 等真实量化占空比读回，工作线程有界取消等待，创建兼顾切换前较慢周期。实验 TCP 端口、强制 Wi-Fi 不省电、主机 ARP 设置未进入正式代码。

B47A（e4:b0:63:ca:b4:78）仅刷应用 0x20000：bus BIN SHA256 `0a073ae1bed07bfe66d7e4419561b3602d84d46e420a23113c3dd42fe01c4fdf`，hello build_id `385ac1152c37848011da14bfd3758cf844c65408758eb559d087ffc5387e676d`。最终短测 USB/WS 各 10 次 GPIO13 切换、各 5 次 1kHz/10bit PWM 更新即时确认正确；另测 USB 10Hz/14bit 后 WS 恢复 1kHz/10bit。WS 双连接、第三连接容量拒绝、槽位复用及两种无效帧后的重连均通过。共 60 条计时请求无失败，不是长期稳定性或物理波形证明。

排查保留 1Hz/14bit 驱动拒绝，以及尚未兼顾旧低频周期时的两次 open timeout；绑定后重设 duty 的尝试无效，未保留。最终按前后较慢周期预算解决，未通过回显请求值掩盖旧值。

Python 414 项、Ruff、四组 ASan/UBSan 原生测试与三个 profile 编译通过。完整计时分布、失败记录和当前限制见本次独立 limivia-server 仓库 `docs/a7-verification.md`；原始日志在该仓库忽略目录 `artifacts/a7/`，不是固件运行/构建依赖。BLE/MQTT 与 Windows 本轮未重测。
