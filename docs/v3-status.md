> 历史账本：本文件记录自定义v3截至2198795的交付情况。当前JSON-RPC v3、镜像及验收结果以 [最新验证](verification.md) 和 [当前契约](protocol.md) 为准；历史验证见 [自定义v3记录](verification-custom-v3.md)。

# v3实施与证据账本

本轮固件、开发工具和可执行的三镜像软件/单板验收已完成。范围见 [v3计划](v3-plan.md)，当前契约见 [protocol.md](protocol.md)，具体测试与限制见 [verification.md](verification.md)。外部互通和仪器测量仍明确未验证。

## 分批提交

|提交|内容|
|---|---|
|0aae909|三profile、固定双OTA分区、锁定依赖|
|a463a99|v3资源/矩阵/作业/缓存/离线流程核心及原生测试|
|181f3a0|异步外设、信号、存储和网络服务|
|5497fff|通知、协作取消、JSON失败处理、worker回收、睡眠检查点；移除旧v2运行时|
|0c0964c|睡眠前accepted回复窗口和唤醒结果、MQTT内存/空payload边界|
|7c53341|MQTT生命周期与回调锁顺序、维护代次、SPI quad能力要求；最终固件源码|
|6750cd3|v3开发工具、OTA及完整单板验收；不属于产品SDK|

分支 `feature/protocol-v3`，v2基线 `dd856b1`。本轮未修改相邻SDK/server，也未推送或发布软件包。

## 阶段门槛

|阶段|本轮状态|依据|
|---|---|---|
|M0|完成|三镜像独立构建、分区核对、完整Flash备份、镜像余量|
|M1|完成|26项生产核心+2项传输原生用例，ASan/UBSan；真实USB拥塞stop|
|M2|完成实现及单板范围|双UART回环、I²C主/从和10位设备、SPI主/从/quad、TWAI、RMT；外部互通未验证|
|M3|完成实现及单板范围|连续ADC标签/溢出、Std/TDM/PDM、4SDM和RMT；外部音频/测量未验证|
|M4|完成实现及单板范围|原子文件、STA扫描/维护AP、三镜像OTA精确启动、中间候选故障镜像回滚；真实WS无线未验证|
|M5|完成实现及单板范围|离线记录/重启/自启抑制/深睡续步/浅睡、MQTT创建停止、ESP-NOW广播；broker/TLS/第二节点未验证|
|M6|完成本轮可执行验收|19项Python、Ruff、原生测试、三构建、15份最终OTA/验收记录；最终bus清理确认|

本轮验收不等于量产、安全认证、外部总线电气互通或精度认证。产品价值/可用性仍需真实用户任务；判断标准见 [product-plan.md](product-plan.md)。

## 不覆盖的历史证据

全部真实证据位于Git忽略目录artifacts/verification，保持各尝试独立：

- v3-bus-first：GPIO/ADC通过，UART内存不足，结束清理确认。
- v3-bus-second：懒分配后UART内部回环通过，完整64位build_id修正。
- v3-bus-extended-first：总线/RMT生命周期通过，新FS未初始化导致存储用例失败。
- v3-initialize-storage：确认旧表无FS后唯一一次明确格式化；不作为挂载失败自动恢复。
- v3-bus-extended-second、v3-signal-extended-first、v3-iot-extended-first：中间候选的单板外设、原子文件、离线记录通过。
- v3-ota-bus-to-signal-first、v3-ota-signal-to-iot-first：中间候选真实跨profile升级。
- v3-ota-rollback-first：故障镜像自动回退，不能与最终应用ELF混淆。
- v3-iot-lifecycle-first：脚本缺少步骤timeout_ms，契约正确拒绝；修正后继续。
- v3-iot-lifecycle-second：深睡续步/变量、重启/抑制、无线生命周期通过，light sleep原始请求回复丢失；醒后确认原作业完成，再修正回复窗口与验收方式。
- v3-<profile>-release-*：后续候选结果，完整保留；最终依据另为final命名的记录。
- v3-<profile>-final-ota/verify/verify-profile/verify-variants/verify-lifecycle：最终三镜像15份记录全部通过。
- v3-final-device-state：最终bus，无实验资源/流程、无控制持有者、自启抑制持久完成。
- v3-radio-fixture：主机缺可用无线夹具的只读发现。

首次换分区前完整备份路径artifacts/v2-before-v3/flash-4mb-rom.bin，4194304字节，SHA256 `0eaec6ddeda006148a11e1c235bfb0106192f7e4bc810c96e7eeaa7e554625eb`。默认stub失败、ROM成功和旧尾部分区核对日志均保留。v2协议与验收来源另存protocol-v2.md、verification-v2.md，不能误当当前v3行为。

## 最终产物

artifacts/firmware-v3-0.3.0-dev/manifest.json记录固件源码提交、源文件SHA、bin SHA、ELF身份、容量、三个完整构建目录的必要产物及配置。每个OTA槽1769472字节：bus应用1291248、signal1263296、iot1436304字节；三者均通过精确身份实机核对。产物目录另存最终日志与证据哈希索引，未把二进制或原始大JSON加入Git。
