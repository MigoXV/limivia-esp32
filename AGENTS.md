# LIMIVIA ESP32 固件工程约定

- 使用中文沟通与维护README。根目录为ESP-IDF工程，main/为固件，目标ESP32-C3，ESP-IDF6.0.2。
- Python工具使用Poetry/Python3.10，源码src/limivia_esp32，Typer入口limivia_esp32.commands.app:app，通过poetry run limivia-esp32运行。
- 修改板级、引脚、传输、烧录或恢复时读取仓库专用[limivia-esp32-firmware](.agents/skills/limivia-esp32-firmware/SKILL.md)，不安装到全局。
- 当前用户授权固件先行：0.3.0-dev/业务协议v3/JSON-RPC2.0（contract=limivia-v3-jsonrpc2），docs/protocol.md为本轮集成契约，本轮不改SDK/server，现有SDK0.3.0a1的自定义v3包络不兼容；不保留旧包络解析。历史v2上游快照及来源保留docs/protocol-v2.md，后续跨仓库适配必须核对版本、实现与测试，不能假定同步检出。
- bus/signal/iot构建分别位于build/<profile>，配置不能混用。main/resources.c为引脚/资源策略真源，main/operations.c为操作描述真源，actor独占核心状态。
- 本仓库不含前后端，不依赖相邻检出；浏览器/SDK可承载同一USB/BLE协议，Wi-Fi提供板上WebSocket，IoT另有MQTT逻辑客户端。
- 默认验证：poetry run pytest -q、poetry run ruff check src tests、poetry run limivia-esp32 test-native、三profile build。Poetry和IDF Python环境隔离。
- 烧录/实机必须明确目标串口与身份；沿用现有授权，不操作任意设备。首次改分区先完整备份；挂载失败不能自动格式化；不改eFuse。记录真实证据，内部回环/驱动确认不能当外部互通或物理测量。
