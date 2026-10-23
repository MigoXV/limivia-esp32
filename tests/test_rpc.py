import asyncio
import json
from pathlib import Path

import pytest

from limivia_esp32 import rpc
from limivia_esp32.device import Client, ProtocolError
from limivia_esp32.lifecycle_validation import resume_light_link
from limivia_esp32.rpc_vectors import export_vectors


@pytest.mark.parametrize("data", [b'{"x":1,"x":2}', b'{"x":NaN}', b'{"x":1e999}',
                                  b'{"x":"\\u0000"}', b'{"x":"\\ud800"}'])
def test_strict_wire_json(data):
    with pytest.raises((ValueError, UnicodeError)):
        rpc.decode(data)


def test_request_limits_apply_to_the_entire_batch():
    with pytest.raises(ValueError, match="8"):
        rpc.encode([0] * 9)
    with pytest.raises(ValueError, match="1024"):
        rpc.encode(["x" * 600] * 2)
    assert rpc.decode(rpc.encode({"id": 9007199254740993}))["id"] == 9007199254740993


def test_id_and_sequence_are_separate_and_nonce_is_fresh():
    client = Client(None)
    first = client.make_request("hello")
    assert "seq" not in first["params"]["context"]
    client.request_id = 123
    client.sequence = 4
    client.session = "session"
    next_frame = client.make_request("state")
    assert next_frame["id"] == 124
    assert next_frame["params"]["context"]["seq"] == 5
    notify = client.make_request("session.ping", notify=True)
    assert "id" not in notify and notify["params"]["context"]["seq"] == 6
    second = client.make_request("hello")
    assert first["params"]["context"] != second["params"]["context"]


@pytest.mark.parametrize("value", [
    {"jsonrpc": "2.0", "id": 1, "result": {}, "error": {}},
    {"v": 3, "id": 1, "ok": True, "result": {}},
    {"jsonrpc": "2.0", "id": True, "result": {}},
    {"jsonrpc": "2.0", "id": 1, "error": {"code": "wrong", "message": "error"}},
])
def test_bad_response_shapes_are_rejected(value):
    with pytest.raises((ValueError, TypeError)):
        rpc.response(value)


def test_business_reason_and_rpc_code_remain_distinct():
    exc = ProtocolError({"error": {"code": 1000, "message": "Operation rejected",
                                   "data": {"reason": "pin_conflict"}}})
    assert exc.code == "pin_conflict"
    assert exc.rpc_code == 1000


def test_batch_credential_payloads_are_redacted_in_transcripts():
    class Capture:
        async def write(self, data):
            assert json.loads(data)[0]["params"]["args"]["password"] == "private"

    async def run():
        c = Client(Capture())
        await c.send([c.make_request("wifi.configure", {"ssid": "lab", "password": "private"})])
        assert "private" not in json.dumps(c.transcript)

    asyncio.run(run())


CAPTURED = json.loads((Path(__file__).parent / "vectors/jsonrpc-captured.json").read_text())


@pytest.mark.parametrize("case", CAPTURED["cases"],
                         ids=lambda c: f"{c['profile']}-{c['method']}")
def test_production_firmware_messages_decode_in_the_python_tool(case):
    value = rpc.decode(json.dumps(case["message"], ensure_ascii=False).encode())
    assert value["jsonrpc"] == "2.0"
    assert not {"v", "ok", "op", "event"} & value.keys()
    if "id" in value:
        rpc.response(value)
    else:
        assert value["method"] in {"job.finished", "state.changed", "data.available"}
        assert isinstance(value["params"]["context"]["boot_id"], str)
        assert isinstance(value["params"]["args"], dict)


def test_vector_export_checks_exact_build_and_never_overwrites(tmp_path):
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"source_commit": "test", "contract": rpc.CONTRACT, "images": {
        p: {"build_id": p} for p in ("bus", "signal", "iot")}}))
    for profile in ("bus", "signal", "iot"):
        (tmp_path / f"{profile}-release.json").write_text(json.dumps({
            "identity": {"profile": profile, "build_id": profile},
            "cleanup": {"status": "confirmed"},
            "transcript": [{"received": {"jsonrpc": "2.0", "id": 1,
                "result": {"context": {"boot_id": "boot", "session": "session"},
                           "value": {}}}}]}))
    output = tmp_path / "vectors.json"
    assert export_vectors(tmp_path, manifest, output)["cases"] == 3
    with pytest.raises(ValueError, match="覆盖"):
        export_vectors(tmp_path, manifest, output)
    path = tmp_path / "bus-release.json"
    value = json.loads(path.read_text())
    value["identity"]["build_id"] = "wrong"
    path.write_text(json.dumps(value))
    with pytest.raises(ValueError, match="身份"):
        export_vectors(tmp_path, manifest, tmp_path / "wrong.json")


def test_light_sleep_readiness_only_repeats_handshake_and_preserves_attempts(monkeypatch):
    async def no_sleep(_):
        pass

    monkeypatch.setattr(asyncio, "sleep", no_sleep)

    class WakeClient:
        hellos = 0

        async def open(self):
            pass

        async def close(self):
            pass

        async def hello(self, expected):
            assert expected == "explicit-device"
            self.hellos += 1
            if self.hellos == 1:
                raise TimeoutError("truncated USB reply")
            return {"boot_id": "same-boot"}

        async def request(self, *_args):
            raise AssertionError("readiness must not replay a sleep or business command")

    report = {}
    c = WakeClient()
    result = asyncio.run(resume_light_link(c, "explicit-device", "same-boot", report))
    assert result["boot_id"] == "same-boot" and c.hellos == 2
    assert [a["status"] for a in report["light_sleep_reconnect_attempts"]] == ["not_ready", "ready"]
