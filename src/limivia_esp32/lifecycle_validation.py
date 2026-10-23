"""指定单板上的流程持久化、通知、网络生命周期和睡眠；不声称外部互通。"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import time
import uuid

from limivia_esp32.device import SerialLink
from limivia_esp32.profile_validation import wait_failed
from limivia_esp32.v3_validation import V3Client, encode_flow
from limivia_esp32.validation import VerificationFailure, cleanup

logger = logging.getLogger(__name__)


async def resume_light_link(client, expected_device: str, boot: str, report: dict) -> dict:
    """显式等待USB唤醒后的握手就绪；不重发睡眠或其他业务命令。"""
    await client.close()
    deadline = time.monotonic() + 15
    attempts = report.setdefault("light_sleep_reconnect_attempts", [])
    while time.monotonic() < deadline:
        await asyncio.sleep(0.3)
        try:
            await client.open()
            identity = await client.hello(expected_device)
            if identity["boot_id"] != boot:
                raise AssertionError("light sleep之后boot_id变化，不能当作正常唤醒")
            attempts.append({"boot_id": identity["boot_id"], "status": "ready"})
            return identity
        except (OSError, TimeoutError) as exc:
            attempts.append({"status": "not_ready", "error": str(exc)})
        await client.close()
    raise TimeoutError("USB唤醒后未能恢复同一boot的握手")


async def verify_lifecycle_async(port: str, expected_device: str) -> dict:
    client = V3Client(SerialLink(port))
    report = {"checks": [], "observations": {}, "cleanup": {"status": "not_attempted"}}
    identified = False
    prefix = "life-" + uuid.uuid4().hex[:8]
    files: list[str] = []
    step = "identity"

    def passed(name: str, value=None):
        logger.info("通过 %s", name)
        report["checks"].append(name)
        if value is not None:
            report["observations"][name] = value

    async def reconnect(old_boot: str) -> dict:
        await client.close()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            await asyncio.sleep(0.3)
            try:
                await client.open()
                identity = await client.hello(expected_device)
                if identity["boot_id"] != old_boot:
                    await client.request("control.acquire")
                    return identity
            except (OSError, TimeoutError):
                pass
            await client.close()
        raise TimeoutError("未观察到目标设备的新 boot_id")

    async def restart() -> dict:
        old_boot = client.boot_id
        await client.job("system.restart")
        return await reconnect(old_boot)

    async def records(name: str) -> list:
        value = await client.job("storage.read", {"name": name, "offset": 0, "length": 4096})
        return [json.loads(line) for line in base64.b64decode(value["data"]).splitlines()]

    async def save_flow(name: str, definition: dict):
        for item in definition["steps"]:
            if item["op"] in ("service", "call", "await"):
                item.setdefault("timeout_ms", 1000)
        await client.upload(name, encode_flow(definition))
        files.append(name)
        return await client.job("flow.load", {"name": name})

    try:
        await client.open()
        identity = await client.hello(expected_device)
        identified = True
        report["identity"] = identity
        await client.job("outputs.stop")
        await client.request("control.acquire")
        profile = identity["profile"]
        step = "subscriptions_and_worker_retirement"
        await client.request("event.subscribe", {"topics": ["job", "data"], "interval_ms": 10})
        adc = await client.open_resource("adc", {"pin": 0}, {"interval_ms": 10})
        await asyncio.sleep(0.15)
        await client.request("session.ping")
        assert any(e.get("method") == "data.available" for e in client.events), list(client.events)
        await client.job("resource.close", {"resource": adc})
        await client.request("event.unsubscribe")
        await asyncio.sleep(0.1)
        before = await client.request("diagnostics")
        for _ in range(12):
            pin = await client.open_resource("gpio", {"pin": 13}, {"mode": "output"})
            await client.job("resource.close", {"resource": pin})
        await asyncio.sleep(0.1)
        after = await client.request("diagnostics")
        assert all(v == 0 for v in after["system"]["worker_stack_free_bytes"][:8]), after
        passed(step, {"before": before, "after": after})

        step = "manual_flow_never_autostarts"
        manual_name = prefix + ".manual.flow.json"
        await save_flow(
            manual_name,
            {"version": 1, "profile": profile, "resources": [], "steps": [{"op": "end"}]},
        )
        await restart()
        assert not (await client.request("flow.list"))["flows"]
        passed(step)

        if profile == "iot":
            step = "planned_deep_sleep_checkpoint"
            log_name, flow_name = prefix + ".sleep.jsonl", prefix + ".sleep.flow.json"
            files.append(log_name)
            loaded = await save_flow(
                flow_name,
                {
                    "version": 1,
                    "profile": profile,
                    "resources": [],
                    "variables": {"x": 0},
                    "steps": [
                        {"op": "set", "variable": "x", "value": 7},
                        {
                            "op": "service",
                            "action": "storage.append",
                            "args": {
                                "name": log_name,
                                "value": {"phase": "before", "x": {"$var": "x"}},
                                "limit_bytes": 4096,
                            },
                        },
                        {"op": "sleep", "duration_ms": 1000},
                        {"op": "add", "variable": "x", "value": 1},
                        {
                            "op": "service",
                            "action": "storage.append",
                            "args": {
                                "name": log_name,
                                "value": {"phase": "after", "x": {"$var": "x"}},
                                "limit_bytes": 4096,
                            },
                        },
                        {"op": "end"},
                    ],
                },
            )
            old_boot = client.boot_id
            await client.request("flow.start", loaded)
            woke = await reconnect(old_boot)
            await asyncio.sleep(0.2)
            rows = await records(log_name)
            assert rows == [{"phase": "before", "x": 7}, {"phase": "after", "x": 8}], rows
            resumed = (await client.request("flow.list"))["flows"]
            assert len(resumed) == 1 and resumed[0]["state"] == "completed", resumed
            await client.request("flow.remove", {"flow_id": resumed[0]["flow_id"]})
            await client.job("storage.delete", {"name": flow_name})
            files.remove(flow_name)
            passed(step, {"woke": woke, "records": rows, "flow": resumed[0]})

        step = "autostart_restarts_from_entry_then_stop_inhibits"
        log_name, flow_name = prefix + ".boot.jsonl", prefix + ".auto.flow.json"
        files.append(log_name)
        loaded = await save_flow(
            flow_name,
            {
                "version": 1,
                "profile": profile,
                "autostart": True,
                "resources": [],
                "steps": [
                    {
                        "op": "service",
                        "action": "storage.append",
                        "args": {
                            "name": log_name,
                            "value": {"phase": "entry"},
                            "limit_bytes": 4096,
                        },
                    },
                    {"op": "delay", "ms": 10000},
                    {
                        "op": "service",
                        "action": "storage.append",
                        "args": {
                            "name": log_name,
                            "value": {"phase": "after_delay"},
                            "limit_bytes": 4096,
                        },
                    },
                    {"op": "end"},
                ],
            },
        )
        await client.request("flow.start", loaded)
        await asyncio.sleep(0.15)
        assert await records(log_name) == [{"phase": "entry"}]
        await restart()
        await asyncio.sleep(0.15)
        rows = await records(log_name)
        assert rows == [{"phase": "entry"}, {"phase": "entry"}], rows
        await client.job("outputs.stop")
        await client.request("control.acquire")
        await restart()
        assert (await client.request("state"))["autostart_inhibited"]
        rows = await records(log_name)
        assert rows == [{"phase": "entry"}, {"phase": "entry"}], rows
        assert all(f["state"] == "loaded" for f in (await client.request("flow.list"))["flows"])
        passed(step, rows)

        if profile == "iot":
            step = "wifi_maintenance_mqtt_and_espnow_lifecycle"
            await client.job("power.configure", {"auto_light_sleep": False, "modem_sleep": "none"})
            scan = await client.job("wifi.scan", timeout=30)
            assert isinstance(scan["networks"], list)
            await client.job("espnow.start")
            broadcast = "ff:ff:ff:ff:ff:ff"
            await client.job("espnow.peer", {"mac": broadcast})
            sent = await client.job(
                "espnow.send", {"mac": broadcast, "data": "AQID", "timeout_ms": 1000}
            )
            assert sent["application_ack"] is False
            await client.job("wifi.maintenance", {"enable": True})
            assert (await client.job("wifi.status"))["maintenance_ap"]
            assert not (await client.job("espnow.status"))["started"]
            blocked = await wait_failed(client, "espnow.start", {})
            await client.job("wifi.maintenance", {"enable": False})
            assert not (await client.job("wifi.status"))["maintenance_ap"]
            for protocol in ("3.1.1", "5.0"):
                await client.job(
                    "mqtt.configure", {"uri": "mqtt://127.0.0.1:1883", "protocol": protocol}
                )
                status = await client.job("mqtt.status")
                assert status["configured"] and not status["connected"]
                await client.job("mqtt.stop")
            passed(
                step,
                {
                    "network_count": len(scan["networks"]),
                    "espnow_broadcast": sent,
                    "maintenance_block": blocked,
                    "boundary": "无主机无线链路、无 broker 或第二节点；只验证驱动生命周期",
                },
            )
            step = "light_sleep_timer_wakeup"
            before = client.boot_id
            accepted = await client.request("power.sleep", {"mode": "light", "duration_ms": 200})
            # Sleep can drop USB traffic already in flight. Reopen and query the known job;
            # never issue the sleep command a second time after a timeout.
            await asyncio.sleep(0.6)
            await resume_light_link(client, expected_device, before, report)
            slept = await client.request("job.get", {"job_id": accepted["job_id"]})
            assert slept["state"] == "completed", slept
            assert slept["result"]["wakeup_causes"] != 0, slept
            await client.request("control.acquire")
            passed(step, {"job": slept, "boundary": "定时唤醒且boot_id不变，未测量电流"})
        report["diagnostics"] = await client.request("diagnostics")
    except BaseException as exc:  # noqa: BLE001 -- preserve failed attempts and interruptions
        report["error"] = f"{step}: {type(exc).__name__}: {exc}"
    finally:
        if identified:
            report["cleanup"] = await cleanup(client, expected_device)
            if report["cleanup"]["status"] == "confirmed":
                try:
                    await client.request("control.acquire")
                    existing = set()
                    for page in range(32):
                        listing = await client.job("storage.list", {"page": page})
                        existing.update(f["name"] for f in listing["files"])
                        if not listing["more"]:
                            break
                    for name in files:
                        if name in existing:
                            await client.job("storage.delete", {"name": name})
                except BaseException as exc:  # noqa: BLE001
                    report["fixture_file_cleanup_error"] = str(exc)
        await client.close()
        report["transcript"] = client.transcript
    if "error" in report or report["cleanup"]["status"] != "confirmed":
        raise VerificationFailure(report)
    return report


def verify_lifecycle(port: str, expected_device: str) -> dict:
    return asyncio.run(verify_lifecycle_async(port, expected_device))
