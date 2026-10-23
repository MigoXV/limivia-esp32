"""仅使用明确选定的单板与电脑验证 v3，保留每一步的真实边界。"""

from __future__ import annotations

import asyncio
import base64
import json
import struct
import time
import uuid

from limivia_esp32.device import SerialLink
from limivia_esp32.v3_validation import V3Client, encode_flow
from limivia_esp32.validation import VerificationFailure, cleanup


async def wait_failed(client: V3Client, op: str, params: dict) -> dict:
    accepted = await client.request(op, params)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        job = await client.request("job.get", {"job_id": accepted["job_id"]})
        if job["state"] != "accepted":
            assert job["state"] == "failed", job
            return job
        await asyncio.sleep(0.02)
    raise TimeoutError("预期失败的操作未完成")


async def verify_profile_async(port: str, expected_device: str) -> dict:
    client = V3Client(SerialLink(port))
    report = {"checks": [], "observations": {}, "cleanup": {"status": "not_attempted"}}
    identified = False
    step = "identity"
    prefix = f"verify-{uuid.uuid4().hex[:8]}"

    def passed(name: str, observation=None):
        report["checks"].append(name)
        if observation is not None:
            report["observations"][name] = observation

    async def close(resource: int):
        await client.job("resource.close", {"resource": resource})

    try:
        await client.open()
        identity = await client.hello(expected_device)
        identified = True
        report["identity"] = identity
        assert len(identity["build_id"]) == 64
        await client.job("outputs.stop")
        await client.request("control.acquire")
        profile = identity["profile"]

        step = "six_pwm_channels"
        channels = []
        for pin in (0, 1, 3, 4, 5, 6):
            channels.append(
                await client.open_resource(
                    "pwm", {"pin": pin}, {"frequency": 1000, "resolution": 10, "duty": 25}
                )
            )
        state = await client.resources()
        assert len({r["channel"] for r in state}) == 6
        assert len({r["timer"] for r in state}) == 1
        for resource in channels:
            await client.call(resource, "status")
            await close(resource)
        passed(step, "六个真实 LEDC 通道共用定时器，未测量波形")

        step = "gpio_group_and_edges"
        a = await client.open_resource(
            "gpio", {"pin": 13}, {"mode": "output", "edge": "any", "level": 0}
        )
        b = await client.open_resource("gpio", {"pin": 0}, {"mode": "output", "level": 0})
        result = await client.job(
            "gpio.group.write",
            {"resources": [{"resource": a, "level": 1}, {"resource": b, "level": 1}]},
        )
        assert all(value["level"] == 1 for value in result["values"])
        await client.call(a, "write", {"level": 0})
        await asyncio.sleep(0.05)
        edges = await client.call(a, "events")
        assert edges["sequence"] >= 2, edges
        await close(a)
        await close(b)
        passed(step, {"edge_count": edges["sequence"], "boundary": "板上数字输出内部读回与IRQ"})

        if profile in ("bus", "iot"):
            step = "i2c_master_lifecycle_and_nack"
            bus = await client.open_resource("i2c.master", {"sda": 0, "scl": 1}, {"pullup": True})
            dev = await client.open_resource(
                "i2c.device", {}, {"address": 80, "frequency": 100000}, parent=bus
            )
            nack = await wait_failed(
                client,
                "resource.call",
                {"resource": dev, "action": "read", "args": {"length": 1, "timeout_ms": 20}},
            )
            assert (await client.request("resource.get", {"resource": dev}))["state"] == "ready"
            await client.call(bus, "recover")
            await close(dev)
            await close(bus)
            passed(step, {"nack": nack, "boundary": "无外部从机，只验驱动与错误隔离"})

            step = "spi_master_lifecycle"
            bus = await client.open_resource("spi.master", {"sclk": 6, "mosi": 7, "miso": 10})
            dev = await client.open_resource("spi.device", {"cs": 13}, {}, parent=bus)
            transaction = await client.call(dev, "transfer", {"data": "AQIDBA==", "rx_length": 4})
            assert transaction["length"] == 4
            await close(dev)
            await close(bus)
            passed(step, "完成真实 SPI 驱动事务；未接外部器件，不判断返回内容")

            step = "twai_driver_lifecycle"
            can = await client.open_resource("twai", {"tx": 4, "rx": 5}, {"mode": "self_test"})
            info = await client.call(can, "info")
            await close(can)
            passed(step, {"info": info, "boundary": "仅控制器生命周期，无 CAN 收发器"})

        if profile == "bus":
            step = "i2c_slave_preload_and_release"
            slave = await client.open_resource(
                "i2c.slave", {"sda": 0, "scl": 1}, {"address": 42, "pullup": True}
            )
            result = await client.call(slave, "preload", {"data": "AQIDBA=="})
            assert result["written"] == 4
            await client.call(slave, "clear_tx")
            await close(slave)
            passed(step, "真实从机驱动创建与预装；未验证外部主机读写")

            step = "spi_slave_pending_cancel"
            slave = await client.open_resource(
                "spi.slave", {"sclk": 6, "mosi": 7, "miso": 10, "cs": 13, "ready": 5}
            )
            await client.call(slave, "queue", {"data": "AQIDBA==", "rx_length": 4})
            await close(slave)
            passed(step, "无外部时钟的排队事务关闭与资源释放")

        step = "rmt_two_channel_sync_and_abort"
        first = await client.open_resource("rmt.tx", {"pin": 13})
        second = await client.open_resource("rmt.tx", {"pin": 6})
        symbol = base64.b64encode(struct.pack("<I", 1000 | (1 << 15) | (1000 << 16))).decode()
        await client.job(
            "rmt.sync",
            {
                "resources": [
                    {"resource": first, "data": symbol},
                    {"resource": second, "data": symbol},
                ]
            },
        )
        await client.call(first, "transmit", {"data": symbol, "loops": -1})
        await close(first)
        await close(second)
        passed(step, "驱动同步发射与无限循环取消；未测量相位、脉宽或频率")

        if profile == "signal":
            step = "continuous_adc_capture_and_overflow"
            adc = await client.open_resource(
                "adc.continuous", {"ch0": 0, "ch1": 1}, {"sample_rate": 10000}
            )
            await client.call(adc, "start")
            await asyncio.sleep(0.3)
            await client.call(adc, "stop")
            events = await client.call(adc, "events")
            assert events["received"] > 0
            assert events["dropped"] > 0
            await close(adc)
            passed(step, {"received": events["received"], "dropped": events["dropped"]})

            step = "i2s_standard_bounded_tx"
            i2s = await client.open_resource(
                "i2s", {"bclk": 4, "ws": 5, "dout": 6}, {"direction": "tx", "sample_rate": 16000}
            )
            await client.call(i2s, "play", {"data": base64.b64encode(bytes(256)).decode()})
            await client.call(i2s, "stop")
            await close(i2s)
            passed(step, "真实 I²S 驱动创建和有界播放，无外部音频器件")

            step = "sigma_delta_lifecycle"
            sigma = await client.open_resource("sigma_delta", {"pin": 13})
            await client.call(sigma, "density", {"density": 32})
            await close(sigma)
            passed(step, "数字 Sigma-Delta 通道生命周期，不作为模拟 DAC 验证")

        step = "storage_atomic_upload"
        name = f"{prefix}.bin"
        data = bytes(range(256)) * 4
        await client.upload(name, data)
        stored = await client.job("storage.read", {"name": name, "offset": 0, "length": 4096})
        assert base64.b64decode(stored["data"]) == data
        replacement = await client.job(
            "storage.begin", {"name": name, "size": 4, "sha256": "0" * 64}
        )
        await client.job(
            "storage.chunk",
            {"upload_id": replacement["upload_id"], "offset": 0, "data": "AQIDBA=="},
        )
        await wait_failed(client, "storage.commit", {"upload_id": replacement["upload_id"]})
        stored = await client.job("storage.read", {"name": name, "offset": 0, "length": 4096})
        assert base64.b64decode(stored["data"]) == data
        await client.job("storage.delete", {"name": name})
        passed(step, "错误校验和的替换保留旧文件")

        step = "offline_flow_records"
        flow_name, log_name = f"{prefix}.flow.json", f"{prefix}.jsonl"
        definition = {
            "version": 1,
            "profile": profile,
            "autostart": False,
            "resources": [
                {"name": "led", "kind": "gpio", "pins": {"pin": 13}, "config": {"mode": "output"}},
                {"name": "adc", "kind": "adc", "pins": {"pin": 0}, "config": {}},
            ],
            "variables": {"raw": 0},
            "steps": [
                {"op": "delay", "ms": 100},
                {
                    "op": "call",
                    "resource": "adc",
                    "action": "read",
                    "args": {},
                    "timeout_ms": 1000,
                    "store": "raw",
                    "field": "raw",
                },
                {
                    "op": "service",
                    "action": "storage.append",
                    "args": {
                        "name": log_name,
                        "value": {"raw": {"$var": "raw"}},
                        "limit_bytes": 4096,
                    },
                    "timeout_ms": 1000,
                },
                {
                    "op": "call",
                    "resource": "led",
                    "action": "write",
                    "args": {"level": 1},
                    "timeout_ms": 1000,
                },
                {"op": "loop", "count": 3, "target": 0},
                {"op": "end"},
            ],
        }
        await client.upload(flow_name, encode_flow(definition))
        loaded = await client.job("flow.load", {"name": flow_name})
        await client.request("flow.start", loaded)
        await client.close()
        await asyncio.sleep(6)  # Exceeds the firmware's documented five-second host lease.
        await client.open()
        new_identity = await client.hello(expected_device)
        assert new_identity["boot_id"] == identity["boot_id"]
        flow = await client.request("flow.get", loaded)
        assert flow["state"] == "completed", flow
        await client.request("control.acquire")
        stored = await client.job("storage.read", {"name": log_name, "offset": 0, "length": 4096})
        records = [json.loads(line) for line in base64.b64decode(stored["data"]).splitlines()]
        assert len(records) == 3 and all(0 <= r["raw"] <= 4095 for r in records), records
        await client.request("flow.stop", loaded)
        for _ in range(20):
            if not await client.resources():
                break
            await asyncio.sleep(0.02)
        await client.request("flow.remove", loaded)
        await client.job("storage.delete", {"name": flow_name})
        await client.job("storage.delete", {"name": log_name})
        passed(step, {"records": records, "offline_seconds": 6})
        report["diagnostics"] = await client.request("diagnostics")
    except BaseException as exc:  # noqa: BLE001 -- keep interrupted attempts
        report["error"] = f"{step}: {type(exc).__name__}: {exc}"
    finally:
        if identified:
            try:
                await client.close()
                await client.open()
                await client.hello(expected_device)
                await client.job("outputs.stop")
                remaining = await client.resources()
                assert not remaining, remaining
                report["cleanup"] = {"status": "confirmed"}
            except BaseException as exc:  # noqa: BLE001 -- do not mask uncertain cleanup
                report["cleanup"] = {"status": "unknown", "error": str(exc)}
        await client.close()
        report["transcript"] = client.transcript
    if "error" in report or report["cleanup"]["status"] != "confirmed":
        raise VerificationFailure(report)
    return report


def verify_profile(port: str, expected_device: str) -> dict:
    return asyncio.run(verify_profile_async(port, expected_device))


async def verify_variants_async(port: str, expected_device: str) -> dict:
    client = V3Client(SerialLink(port))
    report = {"checks": [], "observations": {}, "cleanup": {"status": "not_attempted"}}
    identified = False
    step = "identity"

    def passed(name: str, value):
        report["checks"].append(name)
        report["observations"][name] = value

    try:
        await client.open()
        identity = await client.hello(expected_device)
        identified = True
        report["identity"] = identity
        await client.job("outputs.stop")
        await client.request("control.acquire")
        profile = identity["profile"]
        if profile in ("bus", "iot"):
            step = "both_uart_controllers_loopback"
            ports = []
            for controller, (tx, rx) in enumerate(((20, 21), (3, 4))):
                ports.append(
                    await client.open_resource(
                        "uart",
                        {"tx": tx, "rx": rx},
                        {"loopback": True, "baud": 115200},
                        controller=controller,
                    )
                )
            payload = bytes(range(32))
            for uart in ports:
                await client.call(uart, "write", {"data": base64.b64encode(payload).decode()})
                await client.call(uart, "drain")
                data = await client.call(uart, "read", {"length": len(payload)})
                assert base64.b64decode(data["data"]) == payload
                await client.job("resource.close", {"resource": uart})
            passed(step, "两个控制器同时打开，各自板内回环，无外部波形证据")
            step = "i2c_ten_bit_device_and_spi_quad_phases"
            bus = await client.open_resource("i2c.master", {"sda": 0, "scl": 1})
            dev = await client.open_resource(
                "i2c.device", {}, {"address": 683, "address_bits": 10}, parent=bus
            )
            await client.job("resource.close", {"resource": dev})
            await client.job("resource.close", {"resource": bus})
            bus = await client.open_resource(
                "spi.master", {"sclk": 6, "mosi": 7, "miso": 10, "wp": 4, "hd": 5}, {"quad": True}
            )
            dev = await client.open_resource(
                "spi.device",
                {"cs": 13},
                {"half_duplex": True, "command_bits": 16, "address_bits": 64, "dummy_bits": 8},
                parent=bus,
            )
            result = await client.call(
                dev,
                "transfer",
                {"command": 4660, "address_hex": "0123456789abcdef", "lines": 4, "rx_length": 16},
            )
            assert result["length"] == 16
            await client.job("resource.close", {"resource": dev})
            await client.job("resource.close", {"resource": bus})
            passed(step, "10bit设备生命周期及真实quad驱动事务；无从机，不判断返回内容")
        if profile == "signal":
            step = "i2s_tdm_pdm_tx_raw_rx"
            variants = [
                (
                    {"bclk": 4, "ws": 5, "dout": 6},
                    {"mode": "tdm", "direction": "tx", "slots": 8, "slot_mask": 255},
                    "play",
                ),
                ({"bclk": 4, "dout": 6}, {"mode": "pdm", "direction": "tx"}, "play"),
                (
                    {"bclk": 4, "din": 6},
                    {"mode": "pdm", "direction": "rx", "sample_rate": 1024000},
                    "record",
                ),
            ]
            modes = []
            for pins, config, action in variants:
                i2s = await client.open_resource("i2s", pins, config)
                args = (
                    {"length": 128}
                    if action == "record"
                    else {"data": base64.b64encode(bytes(128)).decode()}
                )
                result = await client.call(i2s, action, args)
                await client.call(i2s, "stop")
                await client.job("resource.close", {"resource": i2s})
                modes.append({"config": config, "result": result})
            passed(step, modes)
            step = "four_sigma_delta_channels"
            channels = []
            for pin in (0, 1, 3, 4):
                channels.append(await client.open_resource("sigma_delta", {"pin": pin}))
            for channel in channels:
                await client.call(channel, "density", {"density": -32})
                await client.job("resource.close", {"resource": channel})
            passed(step, "四通道同时分配及释放，未测量调制波形")
            step = "continuous_adc_channel_tags"
            adc = await client.open_resource(
                "adc.continuous", {"ch0": 0, "ch1": 1}, {"sample_rate": 10000}
            )
            await client.call(adc, "start")
            await asyncio.sleep(0.05)
            await client.call(adc, "stop")
            events = await client.call(adc, "events")
            words = [
                word[0]
                for event in events["events"]
                for word in struct.iter_unpack("<I", base64.b64decode(event["data"]))
            ]
            assert words and all(
                ((word >> 13) & 7) in (0, 1) and not (word & (1 << 16)) for word in words
            )
            assert {((word >> 13) & 7) for word in words} == {0, 1}
            await client.job("resource.close", {"resource": adc})
            passed(
                step,
                {"words": len(words), "format": "C3 type2: data[11:0], channel[15:13], unit[16]"},
            )
        step = "two_rmt_rx_capture_abort"
        captures = []
        for pin in (0, 1):
            captures.append(await client.open_resource("rmt.rx", {"pin": pin}))
        for rx in captures:
            await client.call(rx, "capture", {"max_ns": 1000000})
            await client.call(rx, "abort")
            await client.job("resource.close", {"resource": rx})
        passed(step, "两RX通道捕获启动/中止；无外部信号，不判断捕获内容")
        report["diagnostics"] = await client.request("diagnostics")
    except BaseException as exc:  # noqa: BLE001 -- retain failures and cancellation
        report["error"] = f"{step}: {type(exc).__name__}: {exc}"
    finally:
        if identified:
            report["cleanup"] = await cleanup(client, expected_device)
        await client.close()
        report["transcript"] = client.transcript
    if "error" in report or report["cleanup"]["status"] != "confirmed":
        raise VerificationFailure(report)
    return report


def verify_variants(port: str, expected_device: str) -> dict:
    return asyncio.run(verify_variants_async(port, expected_device))
