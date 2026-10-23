"""协议 v3 的固件验证连接；不属于上位机 SDK。"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import time

from limivia_esp32.device import BleLink, Client, ProtocolError, SerialLink
from limivia_esp32.validation import VerificationFailure


class V3Client(Client):
    protocol_version = 3

    async def job(self, op: str, params: dict | None = None, timeout: float = 20) -> dict:
        accepted = await self.request(op, params)
        if "job_id" not in accepted:
            return accepted
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            current = await self.request("job.get", {"job_id": accepted["job_id"]})
            if current["state"] == "completed":
                if current.get("result_expired"):
                    raise RuntimeError(f"作业结果已过期: {current}")
                return current.get("result", {})
            if current["state"] != "accepted":
                raise RuntimeError(f"作业失败: {current}")
            await asyncio.sleep(0.02)
        raise TimeoutError(f"作业超时，不重试写事务: {accepted}")

    async def resources(self) -> list[dict]:
        for _attempt in range(10):
            pages = []
            revision = None
            try:
                for page in range(6):
                    params = {"page": page}
                    if revision is not None:
                        params["revision"] = revision
                    state = await self.request("state", params)
                    revision = state["revision"]
                    pages.extend(state["resources"])
                return pages
            except ProtocolError as exc:
                if exc.code != "snapshot_expired":
                    raise
        raise RuntimeError("资源状态持续变化，无法取得一致的分页快照")

    async def open_resource(
        self, kind: str, pins: dict, config: dict | None = None, **options
    ) -> int:
        accepted = await self.request(
            "resource.open", {"kind": kind, "pins": pins, "config": config or {}, **options}
        )
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            job = await self.request("job.get", {"job_id": accepted["job_id"]})
            if job["state"] == "completed":
                return accepted["resource"]
            if job["state"] != "accepted":
                raise RuntimeError(f"资源创建失败: {job}")
            await asyncio.sleep(0.02)
        raise TimeoutError("资源创建超时")

    async def call(self, resource: int, action: str, args: dict | None = None) -> dict:
        return await self.job(
            "resource.call", {"resource": resource, "action": action, "args": args or {}}
        )

    async def upload(self, name: str, data: bytes) -> None:
        started = await self.job(
            "storage.begin",
            {"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()},
        )
        for offset in range(0, len(data), 384):
            await self.job(
                "storage.chunk",
                {
                    "upload_id": started["upload_id"],
                    "offset": offset,
                    "data": base64.b64encode(data[offset : offset + 384]).decode(),
                },
            )
        await self.job("storage.commit", {"upload_id": started["upload_id"]})


async def verify_usb_load(client: V3Client) -> dict:
    before = await client.request("diagnostics")
    pin = await client.open_resource("gpio", {"pin": 13}, {"mode": "output", "level": 1})
    batch = []
    # Deliberately fill the OS + device TX buffers without consuming replies.
    for index in range(64):
        op = "diagnostics" if index < 40 else "resource.call"
        params = {} if index < 40 else {"resource": pin, "action": "write", "args": {"level": 1}}
        frame = client.make_request(op, params)
        batch.append(frame)
        await client.send(frame)
        await asyncio.sleep(0.015)
    client.transcript.append({"sent_batch": batch})
    await client.job("outputs.stop")
    assert not await client.resources(), "拥塞停止后旧命令重新建立了资源"
    after = await client.request("diagnostics")
    assert after["system"]["admission_rejected"] > before["system"]["admission_rejected"], after
    return {"before": before, "after": after, "boundary": "USB慢读、真实准入拒绝和保留停止入口"}


async def verify_link(link, expected_device: str | None = None) -> dict:
    client = V3Client(link)
    report = {"checks": [], "cleanup": {"status": "not_attempted"}}
    identified = False
    try:
        await client.open()
        identity = await client.hello(expected_device)
        identified = True
        expected_device = identity["device_id"]
        report["identity"] = identity
        await client.job("outputs.stop")
        await client.request("control.acquire")
        for page in range(identity["capability_pages"]):
            await client.request("capabilities", {"page": page})
        report["checks"].append("capability_pages")
        pin = await client.open_resource("gpio", {"pin": 13}, {"mode": "output", "level": 1})
        assert (await client.call(pin, "read"))["level"] == 1
        await client.call(pin, "write", {"level": 0})
        assert (await client.call(pin, "read"))["level"] == 0
        report["checks"].append("gpio13_internal_readback")
        await client.job("resource.close", {"resource": pin})
        adc = await client.open_resource("adc", {"pin": 0})
        sample = await client.call(adc, "read")
        assert 0 <= sample["raw"] <= 4095
        await client.expect_error(
            "resource.open", {"kind": "gpio", "pins": {"pin": 0}, "config": {}}, "pin_conflict"
        )
        await client.job("resource.close", {"resource": adc})
        digital = await client.open_resource("gpio", {"pin": 0}, {"mode": "input"})
        await client.call(digital, "read")
        await client.job("resource.close", {"resource": digital})
        report["checks"].append("adc0_to_digital_gpio_exclusion_and_reuse")
        if identity["profile"] in ("bus", "iot"):
            uart = await client.open_resource(
                "uart", {"tx": 20, "rx": 21}, {"loopback": True, "baud": 115200}
            )
            payload = b"limivia-v3-uart\x00\xff"
            await client.call(uart, "write", {"data": base64.b64encode(payload).decode()})
            await client.call(uart, "drain")
            received = await client.call(uart, "read", {"length": len(payload), "timeout_ms": 100})
            assert base64.b64decode(received["data"]) == payload
            await client.job("resource.close", {"resource": uart})
            report["checks"].append("uart_internal_loopback")
        temp = await client.open_resource("temperature", {})
        report["temperature"] = await client.call(temp, "read")
        await client.job("resource.close", {"resource": temp})
        report["diagnostics"] = await client.request("diagnostics")
        report["checks"].append("temperature_and_diagnostics")
        if isinstance(link, SerialLink):
            report["usb_load"] = await verify_usb_load(client)
            report["checks"].append("usb_congestion_priority_stop")
    except BaseException as exc:  # noqa: BLE001 -- preserve cancellation and original failure
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if identified:
            try:
                await client.close()
                await client.open()
                await client.hello(expected_device)
                await client.job("outputs.stop")
                remaining = await client.resources()
                assert not remaining, f"清理后仍有资源: {remaining}"
                report["cleanup"] = {"status": "confirmed"}
            except BaseException as exc:  # noqa: BLE001 -- interrupted cleanup is unknown
                report["cleanup"] = {"status": "unknown", "error": str(exc)}
        await client.close()
        report["transcript"] = client.transcript
    if "error" in report or report["cleanup"]["status"] == "unknown":
        raise VerificationFailure(report)
    return report


def verify_device(port: str, expected_device: str | None = None) -> dict:
    return asyncio.run(verify_link(SerialLink(port), expected_device))


def verify_ble(address: str, expected_device: str | None = None) -> dict:
    return asyncio.run(verify_link(BleLink(address), expected_device))


def encode_flow(value: dict) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()
