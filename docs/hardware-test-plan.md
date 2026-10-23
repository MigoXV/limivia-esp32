# v3实机验收与夹具

授权来自本轮Implement the plan及此前明确设备授权，唯一目标为合宙CORE ESP32-C3简约版，MAC e4:b0:63:ca:b4:78。端口 /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_E4:B0:63:CA:B4:78-if00。不操作任意新发现设备。

## 操作范围

允许三profile安装、完整Flash备份后换分区、实验GPIO/外设、显式存储初始化、流程文件、重启/睡眠、无线扫描和维护AP、OTA与故障镜像回滚。仅当前C3和电脑，无外接负载。保护Flash/USB/绑带/系统LED，禁止eFuse。当前板4MB DIO40，C3 rev0.4。

换分区前ROM/no-stub读取4MB成功，路径及SHA见 [v3状态](v3-status.md)。旧表尾部没有FS，新分区首次明确格式化只有一次；之后任何挂载错误不自动格式化。每次测试先hello核对身份；错误身份不清理到别的板；结束重新核对、stop作业完成、分页确认无资源、自启抑制。失败日志独立保留。

## 软件与单板用例

|层次|用例|通过边界|
|---|---|---|
|native|严格JSON/Base64、版本、会话/代次、资源原子预检/父子、ADC/LEDC容量|生产C、虚拟时钟/故障driver，ASan/UBSan|
|native|取消打开、过期完成、父清理故障、stop保留槽、结果过期、每JSON分配点失败|状态/内存验证，不代替RTOS实机|
|native|流程循环、断线持有、等待停止、睡眠检查点、MQTT保留/重复拒绝|执行同一生产核心，无真实broker|
|USB basic|全部能力页、GPIO13读回、ADC/数字复用、UART内部回环、片内温度|板内接口，不是电压/波形精度|
|USB load|停读填满TX、普通准入拒绝、保留stop、旧修改不复活|记录真实拥塞计数与最终资源|
|bus/iot|I²C主机NACK/recover、SPI主机事务、TWAI生命周期、两RMT同步/循环终止|无外部器件，不判断SPI返回内容/CAN互通|
|bus|I²C从机预装、SPI从机无时钟pending关闭|真实驱动释放，外部主机互通未运行|
|signal|连续ADC捕获/溢出、I²S Std有界TX、SDM、RMT|采集队列和驱动生命周期；无外部音频或仪器|
|存储/流程|错误hash不覆盖旧文件、3次ADC离线写记录、断开6秒|大于5秒会话租约；不是精密采样时序|
|lifecycle|手动不自启、自启重启从入口、全局停止跨重启抑制|真实system.restart，非冷上电|
|iot|计划deep sleep续下一步/变量、light sleep查询同job|boot/记录/唤醒原因证据，非功耗测量|
|无线生命周期|STA扫描、显式AP暂停业务、MQTT3.1.1/5创建停止、ESP-NOW广播回调|无实际broker或第二节点，不能声称应用互通|
|OTA|非活动槽上传/校验/换profile、专用故障镜像首次启动回滚|精确ELF身份；不测试掉电Flash物理损坏|

## 外部夹具缺口

BLE/双链路需要可用BlueZ控制器；Wi-Fi WS/MQTT/TLS链路需要主机无线接口或可访问局域网；ESP-NOW应用确认需要第二节点。当前主机无可用无线接口，蓝牙控制器初始化失败，发现证据保存artifacts/verification。

I²C/SPI主从电气互通、RS485/CAN收发器、I²S/TDM/PDM外部音频、RMT外部输入、已知电压、示波器/逻辑分析仪、功耗仪、冷上电和目标用户任务尚需单独验证。仅软件支持的模式不能计作已实测。当前不采购新夹具。

恢复通过原生USB ROM，必要时BOOT/RST，按对应镜像flash_args恢复。回滚故障选项只构建到单独build/rollback，交付三镜像必须LV_TEST_BOOT_FAILURE=OFF。
