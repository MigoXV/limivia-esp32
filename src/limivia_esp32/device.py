"""Explicit test connections; neither transport selects arbitrary devices."""

from __future__ import annotations

import asyncio
import json
import secrets
import time
from collections import deque
from typing import Protocol

import serial

from limivia_esp32 import rpc

SERVICE = "7b9d0001-7a21-4c9b-8b30-69e5ce1d0001"
RX = "7b9d0002-7a21-4c9b-8b30-69e5ce1d0001"
TX = "7b9d0003-7a21-4c9b-8b30-69e5ce1d0001"


class Link(Protocol):
    async def open(self) -> None: ...
    async def close(self) -> None: ...
    async def write(self, data: bytes) -> None: ...
    async def receive(self) -> bytes: ...


class SerialLink:
    def __init__(self, port: str):
        self.port = port
        self.serial: serial.Serial | None = None

    async def open(self) -> None:
        # Default DTR/RTS preserved uptime on the commissioned native USB board.
        # Do not run esptool reset sequences in a protocol connection.
        self.serial = await asyncio.to_thread(
            serial.Serial, self.port, 115200, timeout=0.05, write_timeout=1, exclusive=True
        )

    async def close(self) -> None:
        if self.serial is not None:
            await asyncio.to_thread(self.serial.close)
            self.serial = None

    async def write(self, data: bytes) -> None:
        if self.serial is None:
            raise ConnectionError("串口未连接")
        count = await asyncio.to_thread(self.serial.write, data)
        if count != len(data):
            raise ConnectionError("串口写入不完整")

    async def receive(self) -> bytes:
        if self.serial is None:
            raise ConnectionError("串口未连接")

        def read_available():
            # read(512) waits out the full timeout for every short JSON reply.
            # Wait for one byte only when empty, then drain bytes already available.
            return self.serial.read(min(512, self.serial.in_waiting) or 1)

        return await asyncio.to_thread(read_available)


class BleLink:
    def __init__(self, address: str):
        self.address = address
        self.client = None
        self.incoming: asyncio.Queue[bytes] = asyncio.Queue(maxsize=256)
        self.overflow = False

    async def open(self) -> None:
        try:
            from bleak import BleakClient
        except ImportError as exc:
            raise RuntimeError("BLE 验证需要 poetry install --with dev") from exc
        self.incoming = asyncio.Queue(maxsize=256)
        self.overflow = False
        self.client = BleakClient(self.address, timeout=10)
        await self.client.connect()
        try:
            if self.client.services.get_service(SERVICE) is None:
                raise ConnectionError("目标未提供 LIMIVIA 服务")
            await self.client.start_notify(TX, self._notification)
        except BaseException:
            await self.client.disconnect()
            self.client = None
            raise

    def _notification(self, _characteristic, data: bytearray) -> None:
        try:
            self.incoming.put_nowait(bytes(data))
        except asyncio.QueueFull:
            self.overflow = True

    async def close(self) -> None:
        if self.client is not None:
            await self.client.disconnect()
            self.client = None

    async def write(self, data: bytes) -> None:
        if self.client is None or not self.client.is_connected:
            raise ConnectionError("BLE 未连接")
        # Safe even at ATT MTU 23; firmware TX uses its negotiated MTU.
        for offset in range(0, len(data), 20):
            await self.client.write_gatt_char(RX, data[offset : offset + 20], response=True)

    async def receive(self) -> bytes:
        if self.overflow:
            raise ConnectionError("主机 BLE 接收缓冲溢出，帧完整性未知")
        if self.client is None or not self.client.is_connected:
            raise ConnectionError("BLE 已断开")
        try:
            return await asyncio.wait_for(self.incoming.get(), 0.1)
        except asyncio.TimeoutError:
            return b""


class ProtocolError(RuntimeError):
    def __init__(self, response: dict):
        self.response = response
        error = response.get("error", {})
        self.rpc_code = error.get("code")
        self.code = error.get("data", {}).get("reason", "unknown_error")
        super().__init__(json.dumps(response, ensure_ascii=False))


class IdentityError(RuntimeError):
    pass


class Client:
    protocol_version = 3

    def __init__(self, link: Link, timeout: float = 6):
        self.link = link
        self.timeout = timeout
        self.sequence = 0
        self.request_id = 0
        self.session = ""
        self.control = ""
        self.buffer = bytearray()
        self.events: deque[dict] = deque(maxlen=256)
        self.transcript: list[dict] = []
        self.boot_id = ""

    async def open(self) -> None:
        self.buffer.clear()
        self.session = self.control = ""
        self.boot_id = ""
        await self.link.open()

    async def close(self) -> None:
        await self.link.close()
        self.buffer.clear()

    async def message(self, deadline: float) -> dict | list:
        while time.monotonic() < deadline:
            if b"\n" in self.buffer:
                line, _, rest = self.buffer.partition(b"\n")
                self.buffer = bytearray(rest)
                if len(line) > 8192:
                    raise ConnectionError("单条响应超过 8192 字节")
                try:
                    value = rpc.decode(line)
                except (ValueError, UnicodeError):
                    self.transcript.append({"invalid_line": line.decode(errors="replace")})
                    continue
                if isinstance(value, (dict, list)):
                    self.transcript.append({"received": value})
                    return value
                continue
            self.buffer.extend(await self.link.receive())
            if len(self.buffer) > 8193 and b"\n" not in self.buffer:
                raise ConnectionError("响应超过协议的 8192 字节上限")
        raise TimeoutError("设备响应超时")

    def make_request(self, op: str, params: dict | None = None, *, notify=False) -> dict:
        self.request_id += 1
        if op == "hello":
            context = {"client_nonce": secrets.token_hex(16)}
        else:
            self.sequence += 1
            context = {"session": self.session, "seq": self.sequence}
            if self.control:
                context["control"] = self.control
        frame = {
            "jsonrpc": "2.0",
            "method": op,
            "params": {"context": context, "args": params or {}},
        }
        if not notify:
            frame["id"] = self.request_id
        return frame

    async def send(self, payload: dict | list) -> None:
        # Never record credential/private-key payloads, including nested batch args.
        def redact(value):
            if isinstance(value, list):
                return [redact(item) for item in value]
            if not isinstance(value, dict):
                return value
            hidden = {"password", "username", "pmk", "key", "ca_pem", "will", "data",
                      "private_key", "private_key_pem", "certificate_pem"}
            return {key: "<redacted>" if key in hidden else redact(item)
                    for key, item in value.items()}

        recorded = [redact(p) for p in payload] if isinstance(payload, list) else redact(payload)
        self.transcript.append({"sent": recorded})
        await self.link.write(rpc.encode(payload))

    def notification(self, value: dict) -> bool:
        if value.get("jsonrpc") != "2.0" or "id" in value or "method" not in value:
            return False
        params = value.get("params", {})
        context = params.get("context", {}) if isinstance(params, dict) else {}
        if context.get("session") == self.session and context.get("boot_id") == self.boot_id:
            self.events.append(value)
        return True

    async def request(self, op: str, params: dict | None = None) -> dict:
        payload = self.make_request(op, params)
        await self.send(payload)
        deadline = time.monotonic() + self.timeout
        while True:
            message = await self.message(deadline)
            for value in message if isinstance(message, list) else [message]:
                if not isinstance(value, dict) or self.notification(value):
                    continue
                if value.get("id") != payload["id"]:
                    continue
                try:
                    context, body = rpc.response(value)
                except (ValueError, TypeError) as exc:
                    raise IdentityError(f"目标没有使用 JSON-RPC v3 契约：{exc}") from exc
                if op != "hello" and self.boot_id and context["boot_id"] != self.boot_id:
                    raise IdentityError("设备已重启，需要重新握手")
                if "error" in value:
                    raise ProtocolError(value)
                if op != "hello" and context["session"] != self.session:
                    continue
                result = body["value"]
                if op == "hello":
                    if (result.get("contract") != rpc.CONTRACT
                            or result.get("jsonrpc") != "2.0"):
                        raise IdentityError("目标未使用 JSON-RPC v3 契约")
                    self.session = result["session"]
                    self.sequence = result["sequence_high"]
                    self.boot_id = result["boot_id"]
                    self.control = ""
                elif op == "control.acquire":
                    self.control = result["control"]
                elif op in ("control.release", "outputs.stop"):
                    self.control = ""
                return result

    async def hello(self, expected_device: str | None = None) -> dict:
        hello = await self.request("hello")
        if (
            hello.get("board") != "luatos-esp32c3-usb"
            or hello.get("protocol") != self.protocol_version
        ):
            raise IdentityError("板型或协议不匹配，未授权实验操作")
        if expected_device and hello.get("device_id", "").lower() != expected_device.lower():
            raise IdentityError("设备身份变化，停止实验操作")
        return hello

    async def expect_error(self, op: str, params: dict | None, code: str) -> None:
        try:
            await self.request(op, params)
        except ProtocolError as exc:
            if exc.code == code:
                return
            raise
        raise AssertionError(f"{op} 没有返回预期错误 {code}")

    async def raw_error(self, frame: bytes, code: str) -> None:
        self.transcript.append({"sent_hex": frame.hex(), "expected_error": code})
        await self.link.write(frame)
        deadline = time.monotonic() + self.timeout
        while True:
            message = await self.message(deadline)
            if isinstance(message, list):
                raise TypeError("错误响应意外为 batch")
            if self.notification(message):
                continue
            error = message.get("error", {})
            if error.get("data", {}).get("reason") != code:
                raise AssertionError(f"异常帧未返回 {code}: {message}")
            rpc.response(message)
            return
