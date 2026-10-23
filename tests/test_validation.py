import asyncio
import json

import pytest

from limivia_esp32.device import IdentityError
from limivia_esp32.v3_validation import V3Client as Client
from limivia_esp32.v3_validation import verify_link
from limivia_esp32.validation import VerificationFailure, cleanup


class FakeLink:
    def __init__(self, fail_on=None, cleanup_failure=False, wrong_device=False):
        self.incoming = bytearray()
        self.commands = []
        self.opens = 0
        self.fail_on = fail_on
        self.cleanup_failure = cleanup_failure
        self.wrong_device = wrong_device

    async def open(self):
        self.opens += 1
        if self.opens > 1 and self.cleanup_failure:
            raise OSError("reopen failed")

    async def close(self):
        pass

    async def write(self, data):
        value = json.loads(data)
        op = value["method"]
        self.commands.append(op)
        if self.fail_on == op:
            raise asyncio.CancelledError("interrupted")
        result = {"resources": [], "revision": 1}
        if op == "hello":
            result = {
                "board": "luatos-esp32c3-usb",
                "protocol": 3,
                "session": "session",
                "device_id": "wrong" if self.wrong_device else "device",
                "firmware": "0.3.0-dev",
                "boot_id": "boot",
                "build_id": "build",
                "capability_pages": 1,
                "jsonrpc": "2.0",
                "contract": "limivia-v3-jsonrpc2",
                "sequence_high": 0,
            }
        elif op == "capabilities":
            result = {"page": 0, "pages": 1, "items": []}
        elif op == "control.acquire":
            result = {"control": "control"}
        self.incoming.extend(
            (json.dumps({"jsonrpc": "2.0", "id": value["id"],
                         "result": {"context": {"boot_id": "boot", "session": "session"},
                                    "value": result}}) + "\n").encode()
        )

    async def receive(self):
        # Exercise partial frames, not a line-at-a-time fake transport.
        part = bytes(self.incoming[:7])
        del self.incoming[:7]
        await asyncio.sleep(0)
        return part


def test_fragmented_hello_and_identity_mismatch():
    async def run():
        client = Client(FakeLink(wrong_device=True))
        await client.open()
        with pytest.raises(IdentityError):
            await client.hello("device")
        assert client.link.commands == ["hello"]

    asyncio.run(run())


def test_cleanup_reopens_and_identifies_before_stop():
    async def run():
        link = FakeLink()
        client = Client(link)
        await client.open()
        await client.close()
        result = await cleanup(client, "device")
        assert result["status"] == "confirmed"
        assert link.commands == ["hello", "outputs.stop", *(["state"] * 6)]
        assert link.opens == 2

    asyncio.run(run())


@pytest.mark.parametrize("cleanup_failure", [False, True])
def test_interruption_preserved_independently_of_cleanup(cleanup_failure):
    link = FakeLink(fail_on="capabilities", cleanup_failure=cleanup_failure)
    with pytest.raises(VerificationFailure) as caught:
        asyncio.run(verify_link(link, "device"))
    report = caught.value.report
    assert "CancelledError: interrupted" in report["error"]
    assert report["cleanup"]["status"] == ("unknown" if cleanup_failure else "confirmed")
    assert report["transcript"]


def test_wrong_identity_never_mutates_even_in_cleanup():
    link = FakeLink(wrong_device=True)
    with pytest.raises(VerificationFailure) as caught:
        asyncio.run(verify_link(link, "device"))
    assert caught.value.report["cleanup"]["status"] == "not_attempted"
    assert link.commands == ["hello"]


def test_response_timeout():
    class Silent(FakeLink):
        async def write(self, data):
            pass

    async def run():
        client = Client(Silent(), timeout=0.01)
        with pytest.raises(TimeoutError):
            await client.request("hello")

    asyncio.run(run())


def test_old_response_with_reused_id_is_filtered_by_session():
    class OldReply(FakeLink):
        async def write(self, data):
            value = json.loads(data)
            self.incoming.extend(
                (
                    json.dumps(
                        {
                            "jsonrpc": "2.0",
                            "id": value["id"],
                            "result": {"context": {"session": "old", "boot_id": "boot"},
                                       "value": {"wrong": True}},
                        }
                    )
                    + "\n"
                ).encode()
            )
            await super().write(data)

    async def run():
        client = Client(OldReply())
        client.session = "session"
        result = await client.request("state")
        assert "wrong" not in result
        assert "resources" in result

    asyncio.run(run())
