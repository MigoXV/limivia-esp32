import asyncio
import base64
import json
import struct

import pytest

from limivia_esp32.device import IdentityError, SerialLink
from limivia_esp32.ota_validation import image_manifest
from limivia_esp32.v3_validation import V3Client


def test_serial_receive_reads_available_bytes_without_filling_a_block():
    class Port:
        in_waiting = 3

        def read(self, count):
            assert count == 3
            return b"{}\n"

    link = SerialLink("/dev/explicit")
    link.serial = Port()
    assert asyncio.run(link.receive()) == b"{}\n"


class Link:
    def __init__(self, reset=False):
        self.incoming = bytearray()
        self.reset = reset
        self.writes = []

    async def open(self):
        pass

    async def close(self):
        pass

    async def write(self, data):
        request = json.loads(data)
        self.writes.append(request)
        if request["method"] == "hello":
            result = {
                "board": "luatos-esp32c3-usb",
                "protocol": 3,
                "device_id": "device",
                "boot_id": "boot",
                "session": "session",
                "profile": "bus",
                "jsonrpc": "2.0",
                "contract": "limivia-v3-jsonrpc2-r2",
                "sequence_high": 0,
            }
        else:
            result = {}
            self.incoming.extend(
                (
                    json.dumps(
                        {"jsonrpc": "2.0", "method": "data.available",
                         "params": {"context": {"session": "old", "boot_id": "boot"},
                                    "args": {}}}
                    )
                    + "\n"
                ).encode()
            )
        self.incoming.extend(
            (
                json.dumps(
                    {
                        "jsonrpc": "2.0",
                        "id": request["id"],
                        "result": {
                            "context": {
                                "session": "session",
                                "boot_id": "new" if self.reset and request["method"] != "hello"
                                else "boot",
                            },
                            "value": result,
                        },
                    }
                )
                + "\n"
            ).encode()
        )

    async def receive(self):
        data = bytes(self.incoming)
        self.incoming.clear()
        return data


def test_old_session_notifications_are_discarded():
    async def run():
        client = V3Client(Link())
        await client.open()
        await client.hello("device")
        await client.request("diagnostics")
        assert not client.events

    asyncio.run(run())


def test_new_boot_is_an_identity_error_before_another_write():
    async def run():
        link = Link(reset=True)
        client = V3Client(link)
        await client.open()
        await client.hello("device")
        with pytest.raises(IdentityError, match="重新握手"):
            await client.request("diagnostics")
        assert len(link.writes) == 2

    asyncio.run(run())


def test_manifest_checks_target_before_hardware_access(tmp_path):
    data = bytearray(288)
    data[0] = 0xE9
    struct.pack_into("<H", data, 12, 5)
    struct.pack_into("<I", data, 32, 0xABCD5432)
    data[48:57] = b"0.3.0-dev"
    data[80:97] = b"limivia-bus-v3rpc"
    data[176:208] = bytes(range(32))
    path = tmp_path / "firmware.bin"
    path.write_bytes(data)
    _, manifest = image_manifest(path)
    assert manifest["profile"] == "bus"
    assert manifest["build_id"] == bytes(range(32)).hex()
    old = bytearray(data)
    old[80:112] = b"limivia-bus-v3".ljust(32, b"\0")
    path.write_bytes(old)
    with pytest.raises(ValueError, match="LIMIVIA"):
        image_manifest(path)
    data[12] = 0
    path.write_bytes(data)
    with pytest.raises(ValueError, match="ESP32-C3"):
        image_manifest(path)


def test_firmware_upload_chunks_are_canonical_and_bounded():
    async def run():
        class Capture(V3Client):
            def __init__(self):
                self.calls = []

            async def job(self, op, params, **kwargs):
                self.calls.append((op, params))
                return {"upload_id": 42}

        client = Capture()
        original = bytes(range(256)) * 5
        await client.upload("file.bin", original)
        chunks = [p for op, p in client.calls if op == "storage.chunk"]
        assert b"".join(base64.b64decode(p["data"], validate=True) for p in chunks) == original
        assert all(len(json.dumps(p).encode()) + 150 < 1024 for p in chunks)

    asyncio.run(run())
