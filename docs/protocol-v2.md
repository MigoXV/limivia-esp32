# LIMIVIA 固件协议 v2

本文件是协议快照，来源 limivia-server 提交 `e6a8e12168efccf2bfc730662bcca794c7ef60fd` 的 `docs/protocol.md`（跨仓库真源）。当前组合为固件 0.2.0、Python SDK 0.2.0a1 与 limivia-server v2 工作台；v2 不执行 v1 控制请求。固件仓库维护注明本文件来源提交的快照，不能假定不同仓库始终位于同一版本。

网页通过同源 WebSocket 使用后端 AsyncLimivia。浏览器提交 v/id/op/params；SDK 管理设备 session/control 和后台心跳，并在写入前分配递增的线协议 ID。浏览器 ID、SDK 请求 ID、设备 ID 分开映射。浏览器显式 control.acquire 后才可操作；页面首次连接会尝试获取，control_busy 时保持观察者。此桥接是托管会话，不是原始串口透传。

## 传输与版本

USB Serial/JTAG 与 BLE GATT 承载相同 UTF-8 JSON + LF。LF 以外请求最多 1024 bytes，响应最多 8192 bytes；数组/对象嵌套最多 16 层。分片跨过 3 秒间隔、NUL、无效 UTF-8、超长帧丢弃到下一个 LF，然后恢复接收。空行忽略。JSON 里的 `\u0000`、重复字段、非有限数、非标准数字、额外顶层字段和未知参数均拒绝。

- Service：`7b9d0001-7a21-4c9b-8b30-69e5ce1d0001`
- RX：`7b9d0002-7a21-4c9b-8b30-69e5ce1d0001`，Write with response。
- TX：`7b9d0003-7a21-4c9b-8b30-69e5ce1d0001`，Notify，先订阅再发送请求。
- 广播名：`LIMIVIA-xxxx`。BLE 开放连接，**控制 token 用于协调，不是身份认证凭据**。同时最多 1 个 BLE 连接。
- BLE 按实际 ATT MTU−3 分片，单片最多 180 bytes；整帧截止时间 3000ms，不能每片重新计时。失败时终止该代连接，避免截断 JSON 污染下一帧；必须重连、清空接收缓冲、重新 hello。连接句柄复用不延续原会话。
- USB 写入最长等待 100ms；失败会计数。USB 和 BLE 各有发送任务，不会互相占用长时间发送锁。

协议版本 `v=2` 与固件版本 `0.2.0` 独立。`hello` 返回 `protocol=2`、固件版本、构建 ELF SHA256、设备 ID、随机启动 ID、会话、能力摘要和完整状态。没有自动降级或多版本协商；不支持的 `v` 返回 `unsupported_version` 和 `supported:[2]`，不执行命令。

## 请求、会话与控制权

```json
{"v":2,"id":1,"op":"hello","params":{}}
{"v":2,"id":2,"session":"<hello.session>","op":"control.acquire","params":{}}
{"v":2,"id":3,"session":"<hello.session>","control":"<acquire.control>","op":"pwm.set","params":{"pin":13,"frequency":1000,"duty":50,"breath_ms":0}}
```

`id` 是 1–2147483647 的整数；一个会话内必须严格递增，已经执行或拒绝的合法会话请求不能用相同 ID 重放。超时后查询状态；不要把新 ID 的重试理解为恰好执行一次。`hello` 无需会话，每次都替换本传输的旧会话；旧会话的待执行请求随即失效。

响应包络同时返回 `op`、`session`、`boot_id`，客户端应核对 ID 和会话，防止重握手后重复使用 ID 时误接收旧响应。hello 的响应 session 是新 token；未知操作/不合法帧的 op 为空。过载的最小 busy 响应可能只有 ID，不携带会话字段。已经发出的旧完整帧可能仍在链路缓冲内。

会话绑定传输、连接代次和 boot。合法会话请求续期 5000ms；建议无其他请求时每 1500ms 发送 `session.ping`。语法/参数验证失败不续期。BLE 断开或退订立即撤销该会话；USB 以 hello/心跳确认应用会话，USB 接线或主机 SOF 不是应用在线证明。

只有一个控制会话。USB/BLE 都可握手和查询；空闲时 `control.acquire` 返回 token，其他会话返回 `control_busy`。持有者重复 acquire 返回现有 token。`control.release`、会话超时、重握手或断开撤销 token，但已启动的 PWM、定时和 ADC 流继续。新持有者接管完整现有状态。重启清空实验资源，不保存实验到 NVS。

`outputs.stop` 只要求有效会话，任何观察者都能停止全部实验，并撤销当前控制 token。停止成功后必须重新 acquire。它有独立的接收/响应保留槽，优先于管理和普通请求；正在进行的驱动调用结束后才可执行，不等于硬件瞬时急停。旧队列中的写入会因旧会话、旧 ID 或旧控制 token 被拒绝，不能重新启动输出。

每个传输分别允许 1 个普通请求、1 个管理请求（hello/capabilities/ping/acquire/release）、1 个 stop 请求在途，直到本地发送结束。超出返回 `busy`；过载错误本身使用一个可覆盖的有界槽，因此洪泛时不保证每个被拒绝的 ID 都收到回复。**没有响应不代表执行成功。** 正常客户端串行请求；停止可独立提交。响应优先于事件，已经开始发送的完整帧不会被另一帧插入。

## 操作与参数

完整机器可读描述由 `capabilities.operations` 返回，与参数校验使用同一张编译期操作表。所有操作要求 `params` 对象；没有参数则为 `{}`。未注明的操作均不要求控制 token。

| 操作 | 参数 | 控制 token | 返回 |
|---|---|---|---|
| hello | 无 | 否；也无需 session | 身份、session、能力摘要、state |
| capabilities | 无 | 否 | 引脚、保留原因、操作 schema、容量/帧/超时限制 |
| session.ping | 无 | 否 | 完整 state，续期 |
| control.acquire | 无 | 否 | control、state |
| control.release | 无 | 是 | state，任务继续 |
| state | 无 | 否 | 完整 state |
| diagnostics | 无 | 否 | 身份/资源/会话、迟到、失败、队列、TX、堆、栈、复位原因、MTU |
| outputs.stop | 无 | 否 | state；不完全清理返回 partial_cleanup |
| gpio.read | pin | 否 | pin、level；仅 input/output/timer |
| gpio.set | pin, mode, pull；output 时 level | 是 | state |
| pin.release | pin | 是 | state；故障脚必须用 recover |
| pin.recover | pin | 是 | 清理故障资源，确认成功才回到 idle |
| pwm.set | pin, frequency, duty, breath_ms | 是 | state |
| pwm.stop | pin | 是 | state |
| adc.read | pin | 是；会配置 ADC 引脚 | 一次 sample，保持 adc 占用 |
| adc.start | pin, interval_ms | 是 | 首个 sample，首读成功后才建立流 |
| adc.stop | pin | 是 | state，释放引脚 |
| timer.start | pin, kind, high_ms, low_ms, count | 是 | state |
| timer.stop | pin | 是 | state，释放引脚 |

- 实验引脚：0/1/3/4/5/6/7/10/13/20/21；ADC 仅 0/1/3/4。2/8/9 为绑带脚，11 为 Flash 电压相关，12 为状态 LED，14–17 为 Flash，18/19 为原生 USB。D5 为 GPIO13。
- GPIO：mode=`input|output`，pull=`none|up|down`。输出必须 pull=none 且 level=0|1；输入不能提供 level。
- PWM：4 个独立定时器，frequency 整数 5–40000 Hz，duty 有限数 0–100%。breath_ms 为 0 或整数 200–60000ms。固定 40MHz XTAL，自动选分辨率；0/100% 使用恒低/高端点。状态 frequency 为请求配置频率，不是仪器测量频率。
- ADC：12-bit/12dB，原始值 0–4095；内部校准成功才有 `millivolts`，否则 null。interval_ms 整数 100–10000ms，最多 4 流。建议输入范围 0–2.5V 不构成测量精度保证。每次 start 获得新的 instance 和初始 sequence=1；每个实际采样递增 sequence。
- 定时：4 个本地任务。kind=`blink|pulse`；high_ms/low_ms 整数 10–3600000ms，count 整数 0–100000。blink 的 count=0 为无限；pulse 只执行一次 high_ms，其余字段仍需合法。自然结束变成 output/low，显式停止则 idle。
- 资源受 11 个可用引脚共同约束，不能同时承诺 4 PWM + 4 timer + 4 ADC。input/output 可互换；其他功能切换需先释放。

## 成功、错误、状态与事件

```json
{"v":2,"id":3,"ok":true,"result":{"...":"按操作返回"}}
{"v":2,"id":3,"ok":false,"error":{"code":"invalid_argument","applied":false,"supported":[2]}}
{"v":2,"event":"changed","boot_id":"...","revision":12}
{"v":2,"event":"adc","boot_id":"...","revision":12,"data":{"pin":0,"instance":4,"sequence":7,"raw":1234,"millivolts":753,"calibrated":true,"uptime_ms":900}}
```

`applied=true` 表示该请求已造成固件配置/生命周期变化（包括建立会话、撤销控制权或隔离故障），不是保证所请求的输出完整成功。`encoding_failed` 仍用无需分配内存的最小错误帧报告 applied，客户端应查询 state。错误中统一包含 supported，不能因此把其他错误误判为版本错误。

| 错误 | 含义/恢复 |
|---|---|
| invalid_request | 不合法 JSON/包络/帧；解析内存不足也可能落入此类 |
| unsupported_version | 只接受 v2 |
| unknown_operation / invalid_argument | 未支持操作或参数不合法；已有配置不变 |
| invalid_session / stale_request | 重新握手或检查 ID；不可重放旧写入 |
| control_busy / control_required | 等待持有者释放/超时，或取得有效 token |
| busy | 有界请求槽占用；该次未入执行队列 |
| protected_pin / pin_busy / capacity | 板级限制/占用冲突/资源耗尽 |
| resource_faulted / not_faulted | 故障脚只能 recover；正常脚不能 recover |
| hardware_error | 驱动失败，已隔离相关引脚；查询 fault 和 cleanup_error |
| partial_cleanup | stop 已撤销控制权，但有引脚清理失败；不能认为全部 idle |
| encoding_failed | 结果已保留，但 JSON 构建失败；按 applied 和 state 恢复 |

参数、权限和资源冲突在驱动前校验。驱动失败可能已经改变硬件，因此立即尝试清理并将该引脚保留为 fault，记录原驱动错误和清理错误；其他资源继续。清理失败的 PWM 槽仍占用，避免复用未知硬件。`pin.recover` 成功才解除 fault；重启也重新初始化。ADC 运行采样失败终止该流，并产生 `resource.fault`，不能默默保留假活动流。

完整 state 是恢复依据。revision 仅随配置、会话/控制生命周期、任务完成、故障变化，不随普通 ADC 样本或输入电平变化。定时、ADC 和呼吸使用单调截止时间：迟到跳过已错过的更新，有限 blink 按经过的时间结束，不补发脉冲。诊断报告 late_updates/missed_updates/max_lateness_ms，不声称物理边沿数量或精度。

changed 合并到最新 revision；ADC 事件按引脚保存最新样本，覆盖计入 coalesced，丢弃计入 events_dropped。用 boot_id + instance + sequence 识别旧流及中间样本丢失。事件可能丢失；故障同时持久呈现在本次启动的 state 中。诊断堆/栈为采样时软件观测值。

## 上位机恢复约定

SDK 负责 hello、会话心跳和控制 token；SDK 不自动取得控制权、不重试写入。工作台连接时读取完整 capabilities，尝试 acquire；busy 时继续观察。停止后重新获取控制权，故障脚用 recover；响应失败且 applied=true 时刷新完整状态。后台心跳不依赖页面轮询。

ADC 通知按 boot/instance/sequence 识别，忽略旧流与重复样本，保留跳号信息。页面断线保留最后状态并标记过期，重连查询当前设备，不回放旧参数。
