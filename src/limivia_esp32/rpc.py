"""固件验证工具的 JSON-RPC 2.0 编解码；不提供重试或旧协议适配。"""

from __future__ import annotations

import json
import math

CONTRACT = "limivia-v3-jsonrpc2"


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("重复 JSON 字段")
        value[key] = item
    return value


def _validate(value, depth=0):
    if isinstance(value, (dict, list)):
        if depth >= 16:
            raise ValueError("JSON 深度超过 16")
        items = value.items() if isinstance(value, dict) else enumerate(value)
        for key, item in items:
            if isinstance(key, str):
                _validate(key, depth + 1)
            _validate(item, depth + 1)
    elif isinstance(value, str):
        value.encode("utf-8", errors="strict")
        if "\0" in value:
            raise ValueError("JSON 不允许 NUL")
    elif isinstance(value, float) and not math.isfinite(value):
        raise ValueError("JSON 不允许非有限数")


def decode(data: bytes):
    value = json.loads(data, object_pairs_hook=_pairs)
    _validate(value)
    return value


def encode(value) -> bytes:
    _validate(value)
    data = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode()
    if len(data) > 1024:
        raise ValueError("整条请求超过 1024 字节")
    if isinstance(value, list) and len(value) > 8:
        raise ValueError("batch 超过 8 项")
    return data + b"\n"


def response(value: dict) -> tuple[dict, dict]:
    if value.get("jsonrpc") != "2.0" or "id" not in value:
        raise ValueError("需要 JSON-RPC 2.0 响应")
    if type(value["id"]) not in (int, float, str, type(None)):
        raise ValueError("响应ID类型无效")
    if ("result" in value) == ("error" in value):
        raise ValueError("result/error 必须且只能出现一个")
    body = value.get("result", value.get("error"))
    if not isinstance(body, dict):
        raise TypeError("响应主体必须为对象")
    if "error" in value:
        if type(body.get("code")) is not int or not isinstance(body.get("message"), str):
            raise ValueError("错误码或错误说明无效")
        data = body.get("data")
        if not isinstance(data, dict):
            raise ValueError("缺少业务错误数据")
        context = data.get("context")
    else:
        context = body.get("context")
        if not isinstance(body.get("value"), dict):
            raise ValueError("缺少业务结果对象")
    if not isinstance(context, dict) or not all(
        isinstance(context.get(key), str) for key in ("boot_id", "session")
    ):
        raise ValueError("缺少启动或会话上下文")
    return context, body
