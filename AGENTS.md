# LIMIVIA ESP32 固件工程约定

- 使用中文沟通与维护 README。ESP-IDF 工程根目录就是仓库根目录，固件位于 `main/`，目标 ESP32-C3，ESP-IDF 6.0.2。
- Python 工具使用 Poetry / Python 3.10，源码位于 `src/limivia_esp32`，Typer 入口为 `limivia_esp32.commands.app:app`。通过 `poetry run limivia-esp32` 运行。
- 修改板级代码、引脚策略、传输、烧录或恢复流程时，读取仓库专用 [limivia-esp32-firmware](.agents/skills/limivia-esp32-firmware/SKILL.md)。不安装至全局技能目录。
- `docs/protocol.md` 是注明来源提交的协议快照；协议真源在 LIMIVIA 主仓库。协议变更需同步快照、两端实现与测试，不能假设两个仓库始终检出同一版本。
- 浏览器直接使用 Web Serial / Web Bluetooth；本仓库不包含前后端，也不依赖相邻 LIMIVIA 检出。
- 默认验证：`poetry run pytest -q`、`poetry run ruff check src tests`、`poetry run limivia-esp32 build`。Poetry 与 IDF 的 Python 环境必须隔离。
- 烧录和实机验证必须指定目标串口；已有授权可沿用，资料本身不授权操作任意设备。记录真实结果，不将模拟测试或固件确认视为物理测量。
