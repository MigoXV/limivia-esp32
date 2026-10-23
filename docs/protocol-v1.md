# LIMIVIA 设备协议 v1

本文件是 LIMIVIA 设备协议 v1 的固件仓库快照，来源为 `limivia` 仓库提交 `a6a88afac008d2ed5d0f9dd57107a43961d5c8f3` 的 `docs/protocol.md`（仅此说明段调整为迁移后的路径）。协议真源由 LIMIVIA 主仓库维护；本仓库实现位于 `main/control.c`、`main/transport.c`。更改协议时协同更新主仓库协议、前端类型与测试，并更新本快照及来源提交。构建无需检出主仓库。

## 连接与帧

- USB：ESP32-C3 原生 USB Serial/JTAG，Web Serial 以 115200 打开；此波特率是浏览器串口参数，原生 USB 不以该速率模拟 UART 位时序。
- BLE：NimBLE peripheral，单客户端，名称 `LIMIVIA-xxxx`；服务 `7b9d0001-7a21-4c9b-8b30-69e5ce1d0001`，RX write-with-response 特征 `7b9d0002-7a21-4c9b-8b30-69e5ce1d0001`，TX notify 特征 `7b9d0003-7a21-4c9b-8b30-69e5ce1d0001`。
- UTF-8 JSON 对象加 LF。请求内容最多 1024 bytes（不含 LF），最大嵌套 16 层；前端接收帧最多 8192 characters。每条请求显式包含 `v`、`id`、`op`、`params`。
- BLE 前端每段最多 20 bytes，顺序等待 write-with-response；固件通知按实际 ATT MTU 分片。两端按 LF 重组，不把一次 characteristic write/notification 当完整消息。跨分片 UTF-8 使用流式解码。
- 固件对超长、NUL 或间隔超过 3 秒的未完成请求丢弃至 LF，返回无请求 ID 的 `invalid_request`，随后恢复解析。发送方不得暂停半帧后继续使用旧内容。
- 连接先订阅 BLE 通知，再 `hello`。USB 与 BLE 复用相同协议；每个浏览器仅选择一种连接。固件把两路命令串行执行，以最后完成的合法命令为准。
- 请求队列 8 条，浏览器每次只保持 1 条在途请求；超过固件队列容量的请求可能超时。实时通知队列 16 条，慢消费者允许丢通知，通过 `state` 恢复。

```json
{"v":1,"id":1,"op":"gpio.set","params":{"pin":13,"mode":"output","pull":"none","level":1}}
```

成功：`{"v":1,"id":1,"ok":true,"result":{...}}`。
失败：`{"v":1,"id":1,"ok":false,"error":{"code":"pin_busy","message":"..."}}`。
错误码：`invalid_request`、`unknown_operation`、`protected_pin`、`invalid_argument`、`pin_busy`、`capacity`、`hardware_error`。
`id` 为 1–2147483647 整数；前端使用递增 ID，6 秒超时，不自动重试写操作。超时不等于未执行，须查询状态。协议不承诺跨重连 exactly-once。

## 操作

| op | params | result / 行为 |
|---|---|---|
| `hello` | `{}` | 名称、板型、固件、设备 MAC、协议版本、能力与完整 state |
| `state` | `{}` | 运行时间、状态版本、USB 活跃/BLE 连接、引脚、PWM、定时器、ADC 流 |
| `gpio.set` | `pin, mode: input/output, pull: none/up/down, level: 0/1` | 返回 state；输出必须 pull=none；输入忽略 level |
| `gpio.read` | `pin` | `{pin,level}`；只读已配置的 input/output/timer 引脚 |
| `pwm.set` | `pin, frequency: 5..40000, duty: 0..100, breath_ms: 0 或 200..60000` | 返回 state；占空比为百分数；非零 breath_ms 为三角呼吸周期 |
| `pwm.stop` | `pin` | 停止 PWM 并释放引脚 |
| `adc.read` | `pin: 0/1/3/4` | 原始值、校准状态、电压 mV 或 null、设备时间；占用引脚为 adc |
| `adc.start` | `pin, interval_ms: 100..10000` | 返回首次 sample，后续发送 adc 事件；每个 ADC 引脚一条采样流 |
| `adc.stop` | `pin` | 停止采样并释放引脚 |
| `timer.start` | `pin, kind: blink/pulse, high_ms: 10..3600000, low_ms: 10..3600000, count: 0..100000` | 返回 state；blink 的 count=0 表示持续；pulse 只执行一次 high_ms，忽略 low_ms/count 的作用但仍校验范围 |
| `timer.stop` | `pin` | 停止该脚任务并释放引脚 |
| `pin.release` | `pin` | 停止该脚 PWM/定时/采集，清除上下拉并恢复 idle |
| `outputs.stop` | `{}` | 停止全部实验输出和采集，释放实验引脚，保留系统状态 LED |

引脚支持列表见 `hello.result.state.pins`，能力边界见 `hello.result.capabilities`。固件拒绝 GPIO2/8/9/11/12/14–19 及不存在的引脚。GPIO12 为系统 D4，GPIO13 为用户 D5。单脚资源互斥，GPIO input/output 之间可切换，PWM 可原地更新；其余切换先 `pin.release`。

PWM 首版保守开放 4 路，每路独立使用一个 LEDC timer/channel。分辨率由设备选择并返回。固定 PWM 的 0% / 100% 端点分别保持低/高电平；frequency 返回驱动报告的频率。呼吸峰值受分辨率量化，物理精度仍需仪器测量。

本地定时开始时为高电平。有限闪烁和脉冲正常完成后保持低电平，模式变为 output；显式停止则释放为高阻 idle。断开 USB/BLE 后任务、PWM 与 ADC 流继续（持续供电时）；重启后均清空，不保存至 NVS。

## 状态与事件

state 包含：`revision`、`uptime_ms`、`ble_connected`、`usb_active`、`pins[]`、`pwm[]`、`timers[]`、`streams[]`。

- `pins[]`：`pin,mode,pull,level`。idle/pwm/adc 的 level=null，不伪造数字读数。
- `pwm[]`：`pin,frequency,duty,resolution,breath_ms`。呼吸模式的 duty 是最大占空比。
- `timers[]`：`id,pin,kind,high_ms,low_ms,count,elapsed_ms`。
- `streams[]`：`pin,interval_ms`。
- `usb_active` 表示最近 5 秒收到请求，不代表硬件线缆检测。前端每 1.5 秒查询 state 作为心跳。

事件：`{"v":1,"event":"changed","revision":2}` 或 `{"v":1,"event":"adc","data":{"pin":0,"raw":1234,"calibrated":true,"millivolts":900,"uptime_ms":10000}}`。

应答只发回请求来源，事件广播至活跃 USB 与已订阅 BLE。BLE 连接代次用于丢弃旧连接的待发应答；浏览器断开会拒绝在途与排队请求，旧数据标记失效。周期 state 是恢复资源状态的真源，通知不保证完整历史。

AI 扩展应复用这些结构化操作、能力与错误，而非注入任意固件代码。首版没有模型调用与自动决策。
