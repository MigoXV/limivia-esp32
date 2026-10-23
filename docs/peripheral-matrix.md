# 外设与引脚矩阵

能力分四层：芯片有、板上可用、镜像编入、运行时有空闲资源。机器查询用hello→capabilities全部页→mux.preflight；本表解释限制，不能绕过固件准入。数字角色通过GPIO Matrix路由，ADC为固定模拟通道。

## 板级引脚

|GPIO|本板开放数字用途|ADC|特殊限制|
|---|---|---|---|
|0/1/3/4|GPIO、PWM、UART、I²C、SPI、TWAI、RMT、I²S、SDM、本地定时|ADC1 ch0/1/3/4|数字/模拟资源互斥；深睡可唤醒|
|5|同上|不开放ADC2|芯片ADC2限制；深睡可唤醒|
|6/7/10/20/21|同上|无|高频数字路由仍受驱动与电气限制|
|13|同上|无|板载D5 LED负载，不适合假定高阻的信号；无LED脱离开关|
|2/8/9|禁止|2芯片有ADC1 ch2但板级保留|启动绑带；9为BOOT|
|11|禁止|无|Flash电压，不写VDD_SPI_AS_GPIO eFuse|
|12|系统使用|无|板载D4，至少一个有效会话时亮|
|14/15/16/17|禁止|无|外置Flash|
|18/19|禁止|无|原生USB D−/D+|

“可用数字用途”取决于当前镜像、角色方向和控制器容量，不表示同一引脚能同时承担多个用途。关闭资源确认后才可复用；fault也继续占用。GPIO0/1/3/4改作数字用途不需要更改eFuse。

## 镜像/控制器

|外设|芯片/固件上限|bus|signal|iot|共享与限制|
|---|---|---|---|---|---|
|GPIO/IRQ|开放11脚|✓|✓|✓|去抖与时间戳为软件语义|
|LEDC|6通道、4定时器、14bit|✓|✓|✓|频率/位宽相同共用定时器，XTAL40MHz；频率×2^位宽≤40MHz|
|ADC1 oneshot/周期|4开放通道|✓|✓|✓|单元模式与continuous互斥；raw12bit，mV可空|
|ADC continuous|1单元、4通道| |✓| |GDMA、有界池，type2标签数据；记录丢失|
|UART|2控制器|✓| |✓|UART日志禁用；RS485需RTS，无外置电平转换|
|I²C|1控制器|主/从|主|主|主从互斥；主总线最多8子设备；设备地址唯一；实际同时资源受24总槽限制|
|SPI2|1控制器|主/从| |主|SPI0/1留Flash；主总线最多6子设备，分别占CS；共享DMA和暂存；主从互斥|
|TWAI|1经典CAN控制器|✓| |✓|需要外部收发器；无CAN FD|
|RMT|2TX+2RX|✓|✓|✓|硬件同步2TX，载波/采集/有界原始符号；无波形精度测量|
|I²S|1控制器| |✓| |Std/TDM/PDM；PDM不能duplex，RX无硬件PDM→PCM|
|Sigma-Delta|4通道| |✓| |数字调制，需外部滤波才有模拟效果；不是DAC|
|本地timer|4软件任务|✓|✓|✓|阶段≥10ms，断线继续，无精密脉冲承诺|
|片内温度|1|✓|✓|✓|片内传感器，非环境温度标定|
|Wi-Fi/BLE/USB|共享内存与radio|✓|✓|✓|Wi-Fi驱动懒加载；BLE最多1；WS最多2逻辑连接|
|MQTT/ESP-NOW/睡眠|共享STA/radio/Flash| | |✓|4逻辑MQTT客户端、ESP-NOW250字节；同信道；维护AP暂停业务|

不存在/不开放：DAC、PCNT、MCPWM、USB OTG、经典蓝牙、CAN FD、ADC2、任意寄存器写入。GPIO Matrix不会创造这些外设。

## 运行时组合

24资源槽、16作业历史、4×4096字节缓存、2流程。实际组合还受11实验脚、驱动堆、GDMA和radio约束；驱动准入可返回capacity，不承诺把表中所有上限同时用满。设备diagnostics公开free/minimum/largest heap与worker栈。

连续ADC/I²S/RMT活动与文件写、持久设置、OTA等Flash写互斥，先停止采集/发射再落盘。短事务输出不能被stop撤回；pending SPI保留DMA描述符直到确认完成或清理。UART流控BREAK拒绝，SPI半双工DMA同事务TX+RX拒绝，属于明确硬件/驱动边界。外部I²C需合适上拉，UART/I²S/CAN电气接口需外部器件。

单板验证的级别和缺失夹具见 [硬件验收](hardware-test-plan.md) 与 [验证记录](verification.md)。
