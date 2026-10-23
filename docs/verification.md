# JSON-RPC v3 最终验证记录

日期：2026-10-23。目标：合宙CORE ESP32-C3简约版，原生USB、外置4MB/DIO/40MHz，C3 rev0.4，MAC `e4:b0:63:ca:b4:78`。固件0.3.0-dev，业务协议仍为3，JSON-RPC为2.0，contract为 `limivia-v3-jsonrpc2`。

本轮仅修改固件及仓库开发工具。历史自定义v3已失效；现有SDK0.3.0a1和旧server均未适配，不能据相同版本号推断兼容。旧协议的事实保存在 [历史验证](verification-custom-v3.md)，当前契约见 [协议](protocol.md)。

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
