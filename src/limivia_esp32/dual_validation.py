"""Explicit two-link acceptance; must identify the same board before mutation."""

from __future__ import annotations

import asyncio

from limivia_esp32.device import BleLink, IdentityError, ProtocolError, SerialLink
from limivia_esp32.v3_validation import V3Client as Client
from limivia_esp32.validation import VerificationFailure, cleanup, require


async def verify_dual_links(usb: Client, ble: Client, expected_device: str | None = None) -> dict:
    report: dict = {"status": "not_run", "checks": []}
    identity = None
    matched = False
    failure = None
    try:
        await usb.open()
        hello = await usb.hello(expected_device)
        identity = hello["device_id"]
        await ble.open()
        peer = await ble.hello(identity)
        if peer["boot_id"] != hello["boot_id"]:
            raise IdentityError("两条连接的 boot_id 不同")
        matched = True
        report.update(device_id=identity, boot_id=hello["boot_id"], build_id=hello["build_id"])
        await usb.job("outputs.stop")
        await usb.request("control.acquire")
        await ble.expect_error("control.acquire", None, "control_busy")
        gpio = {"kind": "gpio", "pins": {"pin": 13}, "config": {"mode": "output", "level": 1}}
        await usb.open_resource("gpio", gpio["pins"], gpio["config"])
        require((await ble.request("state"))["owner_endpoint"] == 0, "BLE 观察到错误持有者")
        await ble.job("outputs.stop")
        await usb.expect_error("resource.open", gpio, "control_required")
        await ble.request("control.acquire")
        timer = await ble.open_resource("timer", {"pin": 13}, {"high_ms": 150, "low_ms": 150})
        before = (await usb.request("state"))["uptime_ms"]
        await ble.close()
        # GAP disconnect has an asynchronous delivery path; bounded settling is
        # an observation delay, never a timing-precision claim.
        await asyncio.sleep(0.3)
        state = await usb.request("state")
        require(
            state["owner_endpoint"] == -1 and len(await usb.resources()) == 1,
            "BLE 断开未释放控制或任务丢失",
        )
        require(usb.boot_id == hello["boot_id"] and state["uptime_ms"] > before, "意外复位")
        await usb.request("control.acquire")
        await usb.job("resource.close", {"resource": timer})
        report.update(
            status="pass",
            checks=[
                "same_device",
                "exclusive_control",
                "observer_state",
                "observer_stop_revokes_token",
                "ble_disconnect_usb_takeover",
            ],
        )
    except BaseException as exc:  # noqa: BLE001 -- retain interruption and cleanup evidence
        failure = exc
        report.update(
            status="product_failure"
            if isinstance(exc, (AssertionError, ProtocolError))
            else "fixture_error",
            error=f"{type(exc).__name__}: {exc}",
        )
    finally:
        # No output mutation before both routes match; a wrong peer must never
        # receive cleanup commands intended for the USB board.
        report["cleanup"] = await cleanup(usb, identity) if matched else {"status": "not_attempted"}
        for client in (ble, usb):
            try:
                await client.close()
            except BaseException as exc:  # noqa: BLE001 -- preserve initial failure
                report.setdefault("close_errors", []).append(str(exc))
        report["transcript"] = {"usb": usb.transcript, "ble": ble.transcript}
    if failure or report["cleanup"].get("status") != "confirmed":
        report.setdefault("error", "最终状态未确认")
        if report["status"] == "pass":
            report["status"] = "fixture_error"
        raise VerificationFailure(report) from failure
    return report


def verify_dual(port: str, address: str, expected_device: str | None = None) -> dict:
    return asyncio.run(
        verify_dual_links(Client(SerialLink(port)), Client(BleLink(address)), expected_device)
    )
