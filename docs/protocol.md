# LIMIVIA 固件协议v3

本文件为 **固件0.3.0-dev的先行集成契约**，实现基线为本仓库 `feature/protocol-v3`，生产契约测试在 `tests/native/test_v3.c`。本轮按用户要求仅改固件；SDK/server暂不适配。v2来自limivia-server提交 `e6a8e12168efccf2bfc730662bcca794c7ef60fd` 的快照已移至 [protocol-v2.md](protocol-v2.md)。现有SDK0.3.0a1使用历史自定义包络，不能连接当前JSON-RPC v3。

## 传输、包络与发现

USB Serial/JTAG、BLE GATT承载UTF8 JSON+LF；请求不含LF最多1024字节，响应不含LF最多8192字节，嵌套深度16。严格拒绝重复字段、NUL/转义NUL、非法UTF8、非有限数、非标准数字和未知参数。USB/BLE丢弃超长或跨3秒的残帧到下一LF后恢复。

BLE Service `7b9d0001-7a21-4c9b-8b30-69e5ce1d0001`；RX `7b9d0002-7a21-4c9b-8b30-69e5ce1d0001` Write with response；TX `7b9d0003-7a21-4c9b-8b30-69e5ce1d0001` Notify。名称LIMIVIA-xxxx。一个BLE连接，TX随MTU分片；整帧中途发送失败中止同代连接。

Wi-Fi HTTP端口80，WebSocket路径 `/v3`，最多2逻辑连接。每个未分片TEXT消息是一条JSON，可带末尾LF；不接受WS消息级分片。响应为完整TEXT帧。MQTT每条PUBLISH是一条JSON、不要求LF，逻辑客户端见后文。

本次在开发阶段直接重定义业务协议v3，线格式为[JSON-RPC 2.0](https://www.jsonrpc.org/specification)。历史自定义v3（提交2198795的v/id/op/ok/event包络）已经失效；SDK0.3.0a1也属于历史自定义v3客户端，本轮不修改SDK/server。没有兼容、降级或旧格式自动识别。

```json
{"jsonrpc":"2.0","id":"hello-1","method":"hello","params":{"context":{"client_nonce":"0123456789abcdef0123456789abcdef"},"args":{}}}
{"jsonrpc":"2.0","id":"query-1","method":"state","params":{"context":{"session":"握手token","seq":1},"args":{"page":0}}}
{"jsonrpc":"2.0","id":"write-1","method":"resource.open","params":{"context":{"session":"握手token","seq":2,"control":"控制token"},"args":{"kind":"gpio","pins":{"pin":13},"config":{"mode":"output","level":0}}}}
```

所有方法接受命名参数 `{context,args}` 或位置参数 `[context,args]`；缺省context/args视为空对象，多余位置参数、未知业务参数拒绝。没有session的业务调用不会执行。业务章节中的 `{...}` 参数均表示args，不表示整个params。额外顶层扩展字段忽略，重复字段仍拒绝。

`jsonrpc`必须为字符串2.0。id支持字符串、数值和显式null，保留原始JSON标识值回传；缺少id才是通知。ID只关联响应，不作为序号、防重放或幂等键。所有通知经过正常校验和执行，但无论成功失败均不回复；写操作、停止和需要job_id的调用应始终携带id。

成功响应：`{jsonrpc:"2.0",id,result:{context:{boot_id,session},value:业务结果}}`。
失败响应：`{jsonrpc:"2.0",id,error:{code:整数,message:说明,data:{reason:业务错误名,context:{boot_id,session}}}}`；result/error互斥。无法识别请求ID时id:null。标准错误码为-32700/-32600/-32601/-32602/-32603；业务拒绝1000，容量/忙1001，重复/过旧序号1002。程序读取data.reason，不依赖message。响应编码容量错误还含external_effect_possible:true和job_id（0表示未获得句柄），不能重发原写操作。

hello仅允许单独调用，context只含32位小写十六进制client_nonce。相同端点/连接代次/未过期会话重复当前nonce，返回同一会话且不撤销控制；不同nonce创建新会话并撤销该端旧控制。超时/连接换代后相同nonce也建立新会话。客户端不自动重试hello或写事务。

hello的value返回session、device_id(MAC)、boot_id、build_id(完整64位ELF SHA)、board、firmware、profile、protocol:3、jsonrpc:"2.0"、contract:"limivia-v3-jsonrpc2"、容量、batch_max:8、sequence_window:64、sequence_high和capability_pages。先确认板型luatos-esp32c3-usb、契约标识及身份；boot改变即废弃旧句柄、作业、缓存和会话。固件版本仍为0.3.0-dev，不能仅凭protocol:3判断历史自定义v3兼容。

除hello外context携带session和seq，需控制的操作还携带control。seq为1..2147483647整数，新会话从1分配，不能复用；同会话递增分配，允许64位滑动窗口内乱序到达。重复或低于窗口的请求明确拒绝，序号在业务执行前消费，执行失败也不复用；不同RPC ID不能绕过该保护。达到上限后等待在途请求结束，再使用新nonce握手。会话和控制token只协调控制权，不是身份认证或密码学防重放。

### 批量与资源限制

batch最多8项，整条请求1024字节、聚合响应8192字节；不是每项分别拥有这些容量。超过整帧、数量或实时准入容量时可整批丢弃并计数，零执行，不伪造部分执行响应。标准语义在声明的嵌入式资源限制内提供，不承诺处理任意大小的合法JSON-RPC。

混合batch逐项返回错误，通知不产生响应，纯通知batch不回复，空数组返回单个-32600错误。batch不提供事务或前项结果引用，不保证响应顺序；hello返回handshake_must_be_single，握手及控制申请应在批量操作前完成。先按输入顺序校验并登记序号，再调度；无效stop不会建立停止屏障。

同批有效outputs.stop先执行，其余修改（包括重新申请控制、修改订阅）返回stop_in_progress，查询可继续。普通batch在子请求之间服务新到停止请求；epoch改变后该批余下修改同样拒绝，旧完成不能恢复输出。聚合响应中的成功描述各项执行当时的结果，后续stop可能已撤销此前返回的控制token；客户端不能把延迟到达的旧结果当成当前控制权，应在停止后清除旧pending的控制绑定。响应先预留各项兜底错误空间，结果无法容纳时只将本项替换为容量错误，不重新执行业务。传输拥塞仍可能丢弃完整响应；超时必须按已知job/资源查询，不自动重试写事务。

`capabilities{page}` 当前28页，以返回pages为准：0板级引脚；1/2资源配置；3..20各资源动作；21起系统操作及允许参数。availability是镜像能力，运行时还需资源预检；配置/action描述由生产校验表生成。详情范围以设备返回和 `main/resources.c`、`main/operations.c` 为准，客户端不要复制固定能力表。

## 会话、控制、停止和查询

endpoint：USB0、BLE1、WS2/3、MQTT4..7。每个连接代次独立会话；任意合法请求续5秒租约，空闲用session.ping。超时/断开撤销控制，已创建资源与流程继续。使用新nonce重新hello也撤销该端旧控制，同nonce重投递保持会话。

control.acquire/release协调单一控制者，不做密码学认证。开放实验室模式允许观察、竞争控制和停止；MQTT/网络凭据与此控制token不同。

outputs.stop允许任何有效会话调用：撤销控制、取消流程与在途工作、提高epoch、持久禁止autostart、关闭全部实验资源。返回停止job，需查询到completed；failed可能含cleanup_faults/persist_error，fault占用保留。清理与NVS写入确认前不称停止完成。再次修改需重新申请控制；停止不删除文件或凭据，也不撤销已发送的外部动作。新flow.start明确重新允许自启。

state{page,revision?}固定6页，每页4资源；首请求取revision，其余携带相同revision，snapshot_expired时重新读取。返回epoch/stopping/autostart_inhibited/owner_endpoint/uptime_ms。mux.query使用相同分页，资源数组名routes。diagnostics返回拒绝/旧完成/通知丢失计数与堆、栈、NVS/FS/检查点、启动流程错误及TX/准入统计。

观察者可用state、capabilities、mux预检/查询、resource.get、flow.get/list、job.get、diagnostics和stop；resource.call中status、GPIO read、温度read、TWAI info可观察。读取会消耗队列的events/read等仍需控制。

## 资源与GPIO Matrix

resource.preflight或mux.preflight与resource.open使用同一参数：
`{kind,parent?,controller?,pins:{角色:GPIO},config:{...}}`。预检不预留；open先整体预留，再排队驱动。返回job_id/resource。resource.get{resource}返回state、实例、父、控制器/通道/定时器、pins位图、flow归属、driver_error/cleanup_error和definition。

状态free→opening→ready→closing→free；创建/清理失败为fault并保留引脚与控制器。resource.close{resource,cascade?}：有子资源默认children_exist；cascade:true返回closing，查询资源树直到删除或fault。resource.recover重试确认关闭，不偷偷重新打开。flow持有的资源禁止主机修改，先停流程。

GPIO0/1/3/4可作ADC1或数字，不能同时占用；GPIO5不开放ADC2。数字角色可映射到开放GPIO，ADC的ch0/ch1/ch3/ch4角色物理固定。保护GPIO2/8/9、11、12、14..19，GPIO13有D5负载。同一资源的角色不能重用同引脚，UART内部loopback例外。模拟连续/oneshot占用整个ADC1模式；PWM最多6通道，4硬件定时器仅相同frequency/resolution可共用。

|kind|pins角色|主要config|action（除通用status）|
|---|---|---|---|
|gpio|pin|mode=input/output/open_drain；pull=none/up/down；level、drive、invert、edge、debounce_us|read、write{level}、events|
|pwm|pin|frequency、resolution1..14、duty0..100、invert、end_level、breath_ms|duty{duty}、fade{duty,duration_ms}|
|adc|pin|attenuation0..3、interval_ms|read、events；raw12位，校准成功mV，否则null|
|adc.continuous|ch0/ch1/ch3/ch4|sample_rate611..83333、attenuation、pool_bytes|start、stop、read、events；C3 type2小端32位，data[11:0]、channel[15:13]、unit[16]，不能当作纯PCM|
|uart|tx/rx/rts/cts|baud50..5000000、data_bits5..8、parity、stop_bits1/2、flow_control、rx_threshold、invert_mask、mode=uart/rs485、缓冲、loopback|write{data或buffer_id}、read{length}、break{data,bits}、drain、clear、errors、events|
|i2c.master|sda/scl|pullup、glitch_ignore|probe{address}、scan、recover|
|i2c.device|无，parent必填|address、address_bits7/10、frequency1000..400000、scl_wait_us|write、read、write_read{data或buffer_id,length}|
|i2c.slave|sda/scl|address、address_bits、pullup、buffer_bytes|preload、clear_tx、events|
|spi.master|sclk/mosi/miso/wp/hd|max_transfer≤4096、quad|status|
|spi.device|cs，parent必填|frequency≤80000000、mode0..3、command_bits0..16、address_bits0..64、dummy_bits、lsb_first、half_duplex、queue_depth|transfer{data或buffer_id,rx_length,command,address_hex,lines}、result|
|spi.slave|sclk/mosi/miso/cs/ready|mode、lsb_first、max_transfer、queue_depth|queue、result、cancel|
|twai|tx/rx|bitrate≤1000000、mode=normal/listen_only/self_test、queue_depth、filter_id/mask、extended|send{id,data,extended?,remote?}、info、recover、events|
|rmt.tx|pin|resolution_hz、invert、carrier_hz/duty、idle_level|transmit{data或buffer_id,loops:-1..1023}、wait、abort|
|rmt.rx|pin|同RMT|capture{min_ns?,max_ns}、result、abort|
|i2s|bclk/ws/din/dout/mclk|mode=standard/tdm/pdm、role=master/slave、direction=tx/rx/duplex、sample_rate、bits、slots/slot_mask、dma_frames、left_align、bit_shift|start、stop、play、record{length}|
|sigma_delta|pin|sample_rate、density:-128..127、invert|density{density}|
|timer|pin|high_ms/low_ms≥10、count0无限、end_level|status，本地单调时钟执行|
|temperature|无|空|read|

各action通过resource.call{resource,action,args}执行，普通timeout_ms0..1000，默认100；参数范围不保证任意组合均硬件可行，驱动错误作为作业失败返回。UART RS485需tx/rx/rts，不与CTS流控混用；BREAK在流控模式拒绝以避免无限等待。SPI每个资源最多一笔pending，共享控制器暂存未完成前不能复用；半双工DMA不支持同一事务同时有TX+RX，分开事务或修改设计，不能假称完整双向事务。多线与命令阶段也受芯片限制。I²S PDM RX为原始PDM，不提供C3没有的PDM→PCM硬件转换。

gpio.group.read{resources:[id,...]}；gpio.group.write{resources:[{resource,level},...]}整体预检后连续驱动，有共同软件时间戳，无纳秒同时翻转承诺。rmt.sync{resources:[{resource,data},{resource,data}],timeout_ms?}使用两TX硬件同步，数据为小端32位RMT符号：duration0[0..14]、level0[15]、duration1[16..30]、level1[31]，持续时间非零。普通transmit同格式，循环-1需显式abort/close。

bus支持主从I²C/SPI、UART/TWAI；signal支持连续ADC/I²S/SDM并保留I²C主机；iot支持总线主机、MQTT/ESP-NOW/低功耗。所有镜像有基础资源和RMT。无DAC、PCNT、MCPWM、USBOTG、Bluetooth Classic、CAN FD。

## 作业、二进制和通知

异步响应 `{job_id,resource,state:"accepted",...}` 只代表准入。job.get{job_id}返回accepted/completed/failed/cancelled、action、driver_error、error_code、external_effect_possible、result_expired、cancel_requested和result。job.cancel仅请求协作取消，不撤销外部副作用；关闭作业不可取消。16个历史槽，末槽为stop保留；历史淘汰返回job_expired，结果树预算超额可先标result_expired。

buffer.create{size:1..4096}最多4个；write{buffer_id,offset,data}严格规范Base64，拒绝非法字符/非零pad位；校验失败不修改缓冲。read{buffer_id,offset,length}；delete{buffer_id}。从offset0开始连续写入；实际used长度用于发送。resource.call的data与buffer_id互斥；MQTT publish和ESP-NOW send也支持buffer_id。准入时复制payload，之后覆盖/删除buffer不改变在途作业。建议每块384原始字节，始终按完整JSON字节数控制1024上限。binary结果含length/data（Base64）；UART/I²C/SPI/I²S单次最长4096，ESP-NOW当前250，CAN8。

event.subscribe{topics:["job","resource","flow","data"],interval_ms:10..60000}，默认100；unsubscribe取消。事件为 `{jsonrpc:"2.0",method:事件名,params:{context:{boot_id,session},args:{event_sequence,revision,resource,instance,job_id,sequence,dropped}}}`，无id，类型job.finished、state.changed、data.available。通知为有界最佳努力，仅提示“可查询”，不承载全部采样数据；events/read消费驱动队列。job/resource/flow查询恢复丢通知，重新握手清除订阅，丢弃旧boot/session/instance。

普通/查询/stop分别限流，不保证每个拥塞请求都有错误响应。遇超时先查询已知job与资源，**禁止自动重试写事务**。睡眠可中断链路；light sleep先记住accepted job_id，待唤醒重新握手后查询它；deep sleep换boot，旧job不恢复。

## 存储与OTA

storage.status/list{page}；read{name,offset,length≤4096}；delete{name,secret?}。文件名最多48字符，仅ASCII字母数字、点、横线、下划线，不允许前导点/路径。上传：
begin{name,size,sha256,secret?}→upload_id；
chunk{upload_id,offset,data}严格连续；
commit{upload_id}校验长度与SHA256后fsync/原子rename；
abort{upload_id}清理临时文件，保留旧文件。
同一时间一个上传。secret:true存储为隐藏秘密文件，不能由公开read/list读出；用于TLS私钥。storage.record{name,value}原子JSON替换；append{name,value,limit_bytes≤262144}有界JSONL，不自动丢旧记录。
storage.format必须confirm:"erase_littlefs"，system.nvs_format必须confirm:"erase_nvs"；正常初始化错误不自动清空。

分区固定：nvs0x9000/0x10000，otadata0x19000/0x2000，phy0x1b000/0x1000，ota_0 0x20000/0x1b0000，ota_1 0x1d0000/0x1b0000，littlefs0x380000/0x80000。修改分区先完整Flash备份。旧版无LittleFS，新分区首次明确初始化与故障恢复不同。

ota.status返回活动槽、上传状态/offset和槽状态；begin{board,protocol:3,profile,size,sha256}仅空闲实验，写非活动槽；chunk/abort与文件上传类似；commit{upload_id,reboot?}检查SHA、IDF芯片/应用描述和项目limivia-<profile>-v3rpc后设启动槽，reboot默认false，true延迟重启。新镜像基础服务初始化/健康确认失败时启动回滚。不支持更改bootloader/分区表/eFuse。编译配置LV_TEST_BOOT_FAILURE只用于单独回滚夹具，默认关闭。

连续ADC/I²S/RMT活动时拒绝大Flash写。OTA、格式化、睡眠排队期间拒绝新开实验资源；NVS/FS错误和持久自启抑制错误由diagnostics独立展示。

## Wi-Fi、MQTT和ESP-NOW

wifi.configure{ssid,password,persist?}配置STA；status/scan/disconnect；wifi.maintenance{enable}显式AP `LIMIVIA-Maintenance`，关闭返回STA，不因联网失败自动开AP。scan以ssid_base64表示原始SSID。维护AP暂停MQTT/ESP-NOW，恢复后需明确重启相关业务。SSID和凭据不通过状态读回；本轮AP开放，仅用于受控实验室。

IoT mqtt.configure{uri,protocol:"3.1.1"或"5.0",username?,password?,ca_pem或ca_file?,certificate_file?,private_key_file?,will_topic?,will?,will_qos?,will_retain?,persist?}。仅mqtt://与mqtts://，禁止URI内嵌账号；TLS必须CA校验，私钥读取secret文件。设置仅可写，status只报是否配置/连接、错误、计数和password_set；persist保存NVS。TLS真实broker互通尚待无线夹具验证，不声明任意证书链都能在剩余RAM内运行。

MQTT前缀 `limivia/<设备MAC去冒号>`（准确格式见network.c/实机能力）；command/<client> 接收命令，reply/<client>返回响应，最多4逻辑client，名字≤32不含斜线。连接重投递通过核心会话序号窗口防止重复写，hello由nonce保持幂等；MQTT DUP仅计为redeliveries，不跳过核心校验；**所有保留控制命令均拒绝retained_command，包括hello**。publish{topic,data或buffer_id或json,qos:0..2,retain?}返回message_id/delivery:"queued"，不等于应用确认。subscribe/unsubscribe{topic,qos?}；receive查询非控制消息有界队列；stop停止客户端。单次发布4096字节，outbox16384字节。

espnow.start{pmk?}、stop、status；peer{mac,encrypt?,key?}增改对端，同STA信道，不手工切信道；remove{mac}；send{mac,data或buffer_id,timeout_ms?}最长250字节；receive含MAC前缀和丢弃标志。密钥为16字节Base64。mac_ack不等于application_ack，广播也不证明接收节点存在。维护AP阻止启动/发送。

## 离线流程与低功耗

文件名以.flow.json结尾，定义schema version:1与线协议3不同：
`{version:1,profile,autostart?:false,resources:[{name,kind,pins,config,parent?:符号名}],variables?:{名字:数值},steps:[...],repeat_ms?}`。
最多2已加载流程、每流程12资源/64步骤/8数值变量，定义文件≤16KiB。资源符号名≤24字符；父资源须先定义。profile必须与当前镜像一致。

flow.load{name}为job，完成返回flow_id；flow.list/get/start/pause/resume/stop/remove均使用flow_id。start原子预检全部声明资源，按依赖打开；pause停止后续步骤，不能当作清空输出；completed仍持有资源，stop负责取消/关闭，再remove。stop不删除定义文件。

|步骤|字段|
|---|---|
|call|resource、action、args、timeout_ms必填；可store/field/on_error|
|service|action、args、timeout_ms；白名单storage.record/append、mqtt.publish、espnow.send、system.time、wifi.status、mqtt.status|
|await|同call，另field、poll_ms；循环查询字段直到为true/非零数/非空数组或超时|
|delay|ms≥1|
|set/add|variable、value数值|
|if|variable、value、compare、then、else，目标为零基步骤索引|
|loop|count有限、target零基索引|
|sleep|IoT，duration_ms、可wake_pin/wake_level；计划deep sleep|
|end|结束当前轮，repeat_ms配置时等待下一轮|

args中的 {"$var":"名字"}替换变量，{"$result":"字段"}引用上一结果字段。拒绝未知字段、缺失变量、无等待CPU环路和越界跳转。等待/调用可有on_error分支；未处理错误为failed，不自动重发外部事务。

默认手动；启动仅加载autostart:true定义。手动start解除全局自启抑制。断线继续。异常复位从入口重启明确自启流程；未自启定义不自动加载。全局stop持久抑制，重启也不自动恢复。

计划sleep先确认所有流程资源关闭，拒绝其他活动实验，然后保存定义指纹/下一步/变量/循环；只有deep sleep正常定时/GPIO唤醒消费一次检查点，从下一步重新建立资源。定义改变或损坏不恢复；没有外部总线半事务续发保证。全局停止的RTC锁存/NVS抑制优先于检查点。

IoT power.configure{auto_light_sleep?:false,modem_sleep?:"none"/"min"/"max"}；自动睡眠需tickless配置，已开放外设持有PM锁。power.sleep{mode:"light"/"deep",duration_ms:1..86400000,wake_pin?,wake_level?}仅空闲实验。light sleep保持boot，可能断USB，需要重新握手/按job查询；deep sleep重新boot，仅流程sleep保存游标。允许deep GPIO0/1/3/4/5唤醒，保护脚拒绝。USB/radio时钟和系统任务影响功耗，没有仪器时不承诺电流指标。

## 集成与证据

错误分为协议拒绝（parse_error/invalid_request/invalid_argument/invalid_session/duplicate_request/control_required等）、资源拒绝（pin_conflict/controller_conflict/adc_unit_conflict/feature_unavailable等）和作业driver_error。driver_error保留IDF原码，result.error_code给出capacity/timeout/nack/invalid_state等含义；外部副作用标记不是“事务回滚成功”。

客户端需先发现当前profile/能力，使用job完成与revision分页查询，不从通知推断成功。原生测试、USB实机、无线互通与仪器测量分别报告，见 [verification.md](verification.md)。
