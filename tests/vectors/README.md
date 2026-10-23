# JSON-RPC v3 验证向量

- `jsonrpc.json`：独立书写的请求、错误和通知案例，由真实设备的 `verify-rpc` 执行；`$SESSION/$SEQ` 由夹具替换。标准依据见 [JSON-RPC 2.0](https://www.jsonrpc.org/specification)。
- `jsonrpc-captured.json`：三种profile、精确ELF身份匹配的真实消息形状向量；头部记录固件源码提交、各build_id及原始证据SHA。包含请求、响应、错误、三类设备通知和batch子响应；按形状去重，不代表全部外设参数组合都已验证。

请求中的二进制或秘密字段可能已脱敏，扫描到的SSID替换为 `LAB-FIXTURE`。历史会话、boot、资源及job句柄不能直接重放。这些文件供SDK消息解析测试使用，规范真源仍为 [协议](../../docs/protocol.md)、生产C实现及契约测试。

重新导出使用 `poetry run limivia-esp32 export-rpc-vectors --evidence 证据目录 --manifest 冻结清单 --output 新文件.json`。导出器拒绝混入不同ELF的记录，不覆盖已有输出；失败或清理未确认的尝试保留在 `excluded_evidence`，不会混入通过向量。

原始证据位于本地忽略目录 `artifacts/jsonrpc/final/`；历史失败及恢复经过见 [验证记录](../../docs/verification.md)。
