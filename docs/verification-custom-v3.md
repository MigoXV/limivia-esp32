> 历史自定义v3验证记录，提交2198795之前。当前JSON-RPC v3见 [最新验证](verification.md)，本文件保留原始事实，不代表当前镜像。

# v3最终验证记录

日期：2026-10-23。对象：合宙CORE ESP32-C3简约版、外置4MB DIO/40MHz、C3 rev0.4，MAC `e4:b0:63:ca:b4:78`。本轮仅固件与开发工具；SDK/server未改。历史v2及迁移记录另见 [verification-v2.md](verification-v2.md)。

## 冻结镜像

固件源码提交 `7c53341699007551de317dfa4e3601532bb9b73e`，工具提交 `6750cd3`。所有应用均为0.3.0-dev/协议3，IDF6.0.2；三配置独立构建，LV_TEST_BOOT_FAILURE=OFF。最终构建没有编译警告或错误。

|profile|应用字节|OTA槽剩余字节|完整ELF build_id|
|---|---:|---:|---|
|bus|1291248|478224|8279ce3e84a2b6bb944185fead6483f3552bdcdd697ee6693e19d40c8ac81e45|
|signal|1263296|506176|94dcb3d12702ac4cbb071587a84ef61c08899088ce375ae011b7ccb38202d3c8|
|iot|1436304|333168|3ea02b0755a2971b7b36de15ba4e209d5b2d002b9e0f2e3a22681536ae114e0c|

本地忽略目录 `artifacts/firmware-v3-0.3.0-dev/` 保存bin/ELF/map、bootloader、分区表、SDK配置、CMakeCache、flash参数、锁定依赖、源文件SHA及manifest.json。SHA256是bin文件校验，build_id是ELF SHA，两者不能混用。各profile最终OTA均核对真实启动的完整ELF身份。

## 软件验证

- `poetry run pytest -q`：19项通过；Poetry解释器Python3.10.20。
- `poetry run ruff check src tests`、`poetry check`通过；后者仍有现有tool.poetry.scripts弃用提示。
- `poetry run limivia-esp32 test-native`：两个测试程序通过，包含26项生产C核心用例及2项生产帧/发送用例，启用ASan/UBSan。覆盖严格解析、版本/会话/代次、保护/冲突、ADC单元互斥、LEDC容量、父子级联、停止保留槽、取消打开/旧完成、清理故障、结果预算、Base64原子写、流程循环/离线/停止/检查点、订阅隔离及MQTT保留/重复拒绝。
- 对resource.open和全部能力页逐JSON分配点注入失败，释放和采用失败均由同一生产实现验证。
- bus/signal/iot最终独立IDF构建通过，均小于0x1b0000槽。日志复制至构建产物目录。
- MQTT锁顺序按锁定esp-mqtt1.1.0的回调执行上下文审查并修正：路由锁不跨vendor API，生命周期锁保护客户端替换；这不是并发无线互通实测。

## 最终单板验收

每个profile都执行OTA、verify、verify-profile、verify-variants和verify-lifecycle。对应15份原始JSON为 `artifacts/verification/v3-<profile>-final-<命令>.json`。三次OTA及12组验收全部通过，每组均确认结束清理。详细请求、响应、期望失败作业及诊断在各原始记录中。

|范围|真实结果|证据边界|
|---|---|---|
|全部profile|28能力页、GPIO13输出内部读回、ADC0/数字复用、片内温度|不证明外部电压或LED可见发光|
|全部profile|六路LEDC共用一个同频定时器、GPIO组操作和IRQ、RMT两TX同步/循环取消、两RX启动/中止|未测量PWM频率、相位、脉宽或输入波形|
|bus/iot|两个UART控制器同时打开并分别内部回环；I²C主机NACK/recover及10位设备创建；SPI普通事务、quad及16位命令/64位地址阶段；TWAI生命周期|无外部器件/收发器，不判断SPI返回内容，不证明I²C/CAN互通|
|bus|I²C从机预装/清TX，SPI从机无时钟pending关闭|真实驱动创建/释放，不证明外部主机交换|
|signal|连续ADC捕获/溢出、C3 type2通道/单元标签、I²S Std/TDM/PDM TX与原始PDM RX、四路SDM同时创建|无外部音频、已知电压或滤波测量|
|全部profile|错误SHA替换保留旧文件；离线6秒后3条ADC记录完整，boot不变|6秒超过5秒会话租约，非采样时序精度|
|全部profile|通知订阅、12次开关资源后worker栈释放、手动不自启、重启从入口、全局停止跨重启抑制|system.restart，不是冷上电|
|iot|计划深睡前x=7，正常唤醒后下一步x=8，前后记录各一；light sleep完成后同boot、原job可查、定时唤醒位图16|无电流测量，不宣称功耗数值|
|iot|STA扫描、显式维护AP暂停ESP-NOW并拒绝重启、MQTT3.1.1/5客户端创建/停止、ESP-NOW广播MAC回调|无真实broker/TLS连接和第二节点，application_ack=false|
|全部profile|停止读取USB制造真实准入拒绝，再走优先stop；最终无旧输出复活|独立队列与实际拥塞，非所有消息都发送成功|

最终收尾证据 `artifacts/verification/v3-final-device-state.json`：设备保留bus镜像；resources和flows均为空，owner_endpoint=-1，autostart_inhibited=true，持久化待办为false，NVS/FS/检查点错误均0。残留测试定义已清除。此时free_heap=95328、minimum_heap=78656、largest_block=77824字节；这是该观察时刻的诊断，不是所有组合的内存保证。

IoT在完成无线/两种MQTT客户端生命周期后的最小空闲堆约15KiB，容量受radio与TLS峰值限制；TLS证书链、DMA和并发资源需在目标组合下再验证，不能将单项上限相加作为同时运行保证。

## 备份、OTA与失败记录

换分区前完整ROM/no-stub读取4194304字节，`artifacts/v2-before-v3/flash-4mb-rom.bin` 的SHA256为 `0eaec6ddeda006148a11e1c235bfb0106192f7e4bc810c96e7eeaa7e554625eb`，应用区与v2构建逐字节一致。默认stub读取失败已保留；旧表尾部含非FF字节但未定义文件系统，完整备份覆盖它们。新LittleFS首次挂载失败未自动格式化，确认旧分区后显式初始化一次。

`v3-ota-rollback-first.json` 使用单独build/rollback故障镜像，在首次健康确认前复位，实测返回此前同一signal ELF且boot变化。此回滚测试基于中间候选；最终三镜像重新执行非活动槽上传、SHA验证、精确ELF启动及健康槽查询，没有对最终每个镜像重复故障注入。

首轮UART内存不足、首次FS未初始化、流程脚本缺少timeout_ms和light sleep进入时丢USB回复均保留独立失败JSON。后续修复分别为懒加载/worker回收、一次明确初始化、脚本契约修正和睡眠前留出回复窗口/醒后查询已知job；后来的通过没有覆盖这些失败。逐JSON分配失败还发现并修复过对象采用失败泄漏，生产C原生测试已回归。

## 仍未验证

主机接口发现保存于 `v3-radio-fixture.json`：无Wi-Fi接口；BlueZ无adapter，hci0地址与MTU全零、初始化错误。真实BLE/双链路、板上WebSocket、MQTT/TLS broker、ESP-NOW第二节点互通未运行。

外部I²C/SPI/RS485/CAN器件、外部I²S/TDM/PDM音频、RMT输入、示波器/逻辑分析仪、已知电压、功耗、冷上电和真实目标用户任务也未运行。本轮不采购夹具。上述缺口不应由模拟、驱动成功或MAC回调替代；当前交付为开发候选。夹具与后续用例见 [hardware-test-plan.md](hardware-test-plan.md)。
