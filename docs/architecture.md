# v3 架构与所有权

本轮将v2按引脚管理的同步核心替换为资源图和异步作业。仅面向单核ESP32-C3/IDF6.0.2，不引入通用插件、任意代码执行或多板抽象。产品决定见 [v3计划](v3-plan.md)。

## 权威边界

|概念|唯一负责处|边界|
|---|---|---|
|控制token/资源图/epoch/revision/作业历史|main/engine.c，v3_core|只由actor访问，不将核心指针交给worker|
|板级保护、镜像可用性、引脚与控制器/ADC/LEDC预检|main/resources.c|capabilities和准入读取同一描述表|
|系统操作名、允许参数、外设action参数描述|main/operations.c|校验与发现共用描述；驱动做相关硬件条件检查|
|会话/nonce/序号窗口/JSON-RPC包络/无损ID/批量/通知/兜底编码|main/rpc.c，v3_core|统一context/args；actor内执行，子请求之间响应停止|
|严格JSON/UTF8/Base64/内存采用|main/json.c|cJSON从属对象在父节点失败时仍释放，运行时跟踪OOM|
|流程定义、变量、控制流、资源所有权和恢复游标|main/flows.c|固定2流程/12资源/64步骤/8变量；无任意脚本|
|RTOS队列、worker、TX内存、通知、启动流程|main/runtime.c|actor不等待驱动或传输；输入复制，输出帧独立持有|
|硬件上下文、PM锁、通用GPIO/PWM/ADC/定时|main/peripherals.c|每lane只访问自己的槽位；ISR仅计数/写有界队列|
|UART/I²C/SPI/TWAI|main/buses.c|控制器串行，子设备依赖父控制器；DMA事务内存持续有效|
|RMT/连续ADC/I²S/SDM|main/signals.c|活动采集与Flash写操作共享明确互斥守卫|
|LittleFS/NVS/OTA/睡眠检查点|main/system.c|系统worker串行；错误不自动擦除|
|Wi-Fi/HTTPD/WS/MQTT/ESP-NOW|main/network.c|逻辑连接代次与最后发送检查；回调不修改核心状态|
|USB/NimBLE代次、分片|main/transport.c，framing.c，tx.c|BLE最后检查和notify在host队列；失败中断同代链路|

## 状态与工作生命周期

请求经独立传输分帧和有界准入，actor通过rpc边界验证格式、会话、滑动序号窗口和控制权。先完整预检/预留，再创建作业和自有work副本；每个硬件lane串行执行，完成携带实例、epoch和job。过期完成不能把closing资源恢复ready。

父子总线占用协同，父资源有子资源默认拒绝关闭；cascade标记整棵树closing，由tick按子到父关闭。驱动关闭失败保留fault及原有引脚/控制器，不伪装硬件回滚。恢复重复执行清理，只有确认关闭后归还槽位。工作线程在其lane没有资源和排队工作时，与actor握手后自行退出；不能强杀任务冒充清理成功。

SPI DMA缓冲区、排队事务描述符、TWAI帧、RMT符号等均由资源持有。SPI超时后仍可能在外部时钟下完成，保持pending并通过result/close回收；共享控制器暂存不得被另一子设备覆盖。

## 停止、通知与内存

outputs.stop使用独立准入优先级和最后一个保留job槽，撤权、增加epoch、取消流程/工作、持久写入自启抑制并清理资源。普通资源作业最长请求超时1000ms；某些已提交驱动事务有额外边界，不能承诺零延迟撤回外部副作用。父清理故障也使停止作业明确失败，而非永久等待。旧结果和失败占用仍可查询。

每endpoint普通/查询/停止各一在途准入位；batch最多8项，先登记序号，子项之间让出一个tick使接收任务可准入stop。stop提高epoch后，当前batch余下修改（包括control.acquire）一并拒绝，避免旧batch重新取得控制。编码直接写入待发送帧，从512字节开始按需扩容，上限8194字节（含LF/NUL），预留每项的无分配兜底错误空间。扩容只重编码已持有结果，不重新分发业务；TX独立于actor，RPC优先于单槽通知。共享待发帧字节有上限，普通24576字节，停止可用至32768；通知丢失计数并以状态/作业查询恢复。大JSON编码临时分配，排队帧只保存实际长度；作业结果树预算32768字节，超额显式result_expired。没有无限队列或全采样保证。

ISR包队列、ADC池和传输通知分别有丢弃计数。data通知按订阅节流并轮换资源；event_sequence不等于采样sequence。改变会话或boot后客户端丢弃旧通知。

MQTT回复先取得客户端生命周期锁，在路由锁内核对代次并复制topic，释放路由锁后才调用esp-mqtt。MQTT回调只取得路由锁，不能反向等待生命周期锁；配置/停止/替换客户端使用同一生命周期锁，防止回调持有vendor API锁时形成循环等待。进入维护AP同时递增MQTT端点代次。

## 离线与持久化

流程定义文件显式load/start；启动只自动加载autostart定义，优先处理合法计划睡眠检查点。启动先影子预检全部资源，再提交预留；输出在完成后可保留，stop负责清理。暂停仅阻止后续步骤，正在执行的硬件输出/采样仍存在。

计划deep sleep先释放流程资源并确认清理，保存定义指纹、下一步、变量和循环计数，定时/GPIO唤醒后消费一次检查点并重新初始化驱动。异常复位丢弃游标，明确自启流程从入口开始；全局停止的RTC锁存和NVS抑制优先于恢复。此机制不重放或恢复半笔总线事务。

NVS/LittleFS初始化错误保留数据；上传写临时文件、校验、fsync、rename。连续ADC/I²S/RMT活动期间拒绝Flash写。OTA仅非活动槽，启动基础服务就绪后确认；测试专用LV_TEST_BOOT_FAILURE默认关闭，不能混入交付镜像。

## 演进与验证

新增外设依次更新resources描述、operations动作、所属driver、原生冲突/取消/故障测试和单板验收；不得另建第二份引脚真源。新增传输必须提供endpoint/generation、帧边界、最后发送检查和拥塞策略，复用同一核心。

生产C原生测试证明状态与内存约束；真实IDF构建、单板回环/驱动生命周期、外部互通、仪器测量是不同层证据。结果与缺口见 [验证记录](verification.md)。
