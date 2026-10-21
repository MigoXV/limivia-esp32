"""Opt-in on-device checks. Only D5 is driven; ADC0 is sampled without a voltage claim."""

from __future__ import annotations

import json
import time

import serial


def verify_device(port: str) -> dict:
    sequence = 0
    checks: list[str] = []
    link = serial.Serial(port, 115200, timeout=0.1, exclusive=True)

    def request(op: str, params: dict | None = None, error: str | None = None) -> dict:
        nonlocal sequence
        sequence += 1
        payload = {"v": 1, "id": sequence, "op": op, "params": params or {}}
        link.write((json.dumps(payload) + "\n").encode())
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            line = link.readline()
            try:
                response = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue
            if response.get("id") != sequence:
                continue
            if error:
                assert not response["ok"] and response["error"]["code"] == error, response
                return response
            assert response["ok"], response
            return response["result"]
        raise TimeoutError(f"{op}: no response")

    def mode(state: dict, pin: int) -> dict:
        return next(item for item in state["pins"] if item["pin"] == pin)

    try:
        hello = request("hello")
        assert hello["protocol"] == 1 and hello["board"] == "luatos-esp32c3-usb"
        checks.append("USB handshake, board and protocol")
        request("outputs.stop")
        for pin in (2, 8, 9, 11, 12, 14, 15, 16, 17, 18, 19):
            request(
                "gpio.set",
                {"pin": pin, "mode": "output", "pull": "none", "level": 1},
                "protected_pin",
            )
        checks.append("USB, flash, boot and status LED pin protection")
        for level in (1, 0):
            state = request(
                "gpio.set", {"pin": 13, "mode": "output", "pull": "none", "level": level}
            )
            assert mode(state, 13)["level"] == level
        checks.append("D5 GPIO output/readback high and low")
        request("gpio.set", {"pin": 13, "mode": "input", "pull": "up"})
        assert mode(request("state"), 13)["pull"] == "up"
        request("gpio.set", {"pin": 13, "mode": "input", "pull": "down"})
        assert mode(request("state"), 13)["pull"] == "down"
        request("pin.release", {"pin": 13})
        checks.append("GPIO input, pull-up, pull-down and release")
        state = request("pwm.set", {"pin": 13, "frequency": 1000, "duty": 50, "breath_ms": 0})
        assert state["pwm"][0]["frequency"] == 1000
        request("gpio.set", {"pin": 13, "mode": "output", "pull": "none", "level": 1}, "pin_busy")
        request(
            "pwm.set", {"pin": 13, "frequency": 4, "duty": 50, "breath_ms": 0}, "invalid_argument"
        )
        assert request("state")["pwm"][0]["frequency"] == 1000
        request("pwm.set", {"pin": 13, "frequency": 1000, "duty": 80, "breath_ms": 1000})
        assert request("state")["pwm"][0]["breath_ms"] == 1000
        for frequency, duty in ((5, 50), (40000, 50), (1000, 0), (1000, 100)):
            applied = request(
                "pwm.set", {"pin": 13, "frequency": frequency, "duty": duty, "breath_ms": 0}
            )
            assert abs(applied["pwm"][0]["frequency"] - frequency) <= 1
            assert applied["pwm"][0]["duty"] == duty
        request("pwm.stop", {"pin": 13})
        checks.append("PWM/breathing setup, resource conflict and invalid-range rejection")
        sample = request("adc.read", {"pin": 0})
        assert 0 <= sample["raw"] <= 4095
        request("adc.start", {"pin": 0, "interval_ms": 100})
        deadline = time.monotonic() + 3
        received = False
        while time.monotonic() < deadline:
            try:
                event = json.loads(link.readline())
            except ValueError:
                continue
            if event.get("event") == "adc":
                received = True
                break
        assert received, "No periodic ADC event"
        request("adc.stop", {"pin": 0})
        checks.append(
            "ADC1 raw/calibration response and periodic telemetry (input not calibrated externally)"
        )
        request(
            "timer.start", {"pin": 13, "kind": "pulse", "high_ms": 250, "low_ms": 10, "count": 1}
        )
        assert mode(request("state"), 13)["level"] == 1
        time.sleep(0.4)
        state = request("state")
        assert not state["timers"] and mode(state, 13)["level"] == 0
        request("pin.release", {"pin": 13})
        request(
            "timer.start", {"pin": 13, "kind": "blink", "high_ms": 150, "low_ms": 150, "count": 0}
        )
        before = request("state")["uptime_ms"]
        link.close()
        time.sleep(6)
        link.open()
        state = request("state")
        assert state["uptime_ms"] > before and len(state["timers"]) == 1
        request("timer.stop", {"pin": 13})
        checks.append("Local pulse completion and blink survival across USB disconnect/reconnect")
        state = request("outputs.stop")
        assert not state["timers"] and not state["pwm"] and not state["streams"]
        assert all(pin["mode"] == "idle" for pin in state["pins"])
        checks.append("Stop-all releases every experiment resource")
        return {
            "device_id": hello["device_id"],
            "firmware": hello["firmware"],
            "checks": checks,
            "adc_sample": sample,
            "limitations": [
                "No waveform instrument or known voltage source",
                "BLE requires a working host Bluetooth controller",
            ],
        }
    finally:
        if link.is_open:
            try:
                request("outputs.stop")
            finally:
                link.close()
