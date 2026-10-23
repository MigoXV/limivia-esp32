"""真实 USB 的 JSON-RPC 契约、去重、批量容量与停止验收。"""

from __future__ import annotations

import asyncio
import base64
import copy
import json
import math
import statistics
import time

from limivia_esp32 import rpc
from limivia_esp32.device import ProtocolError, SerialLink
from limivia_esp32.paths import ROOT
from limivia_esp32.v3_validation import V3Client
from limivia_esp32.validation import VerificationFailure, cleanup


async def verify_rpc_link(link, expected_device: str) -> dict:
    c = V3Client(link)
    report = {"checks": [], "cleanup": {"status": "not_attempted"}, "vectors": []}
    identified = False
    buffers: set[int] = set()

    async def receive():
        return await c.message(time.monotonic() + c.timeout)

    async def exchange(frame):
        await c.send(frame)
        return await receive()

    async def silent_probe():
        # One serial receive timeout allows the submitted notification to run;
        # then verify that its queue contains only the new, identifiable probe.
        await asyncio.sleep(0.05)
        probe = c.make_request("session.ping")
        value = await exchange(probe)
        assert value.get("id") == probe["id"], value
        assert "result" in value, value

    try:
        await c.open()
        report["identity"] = await c.hello(expected_device)
        identified = True
        await c.job("outputs.stop")
        assert not await c.resources()
        await c.request("control.acquire")
        token, control = c.session, c.control
        hello_frame = next(t["sent"] for t in c.transcript if t.get("sent", {}).get("method") == "hello")
        hello_frame = copy.deepcopy(hello_frame)
        hello_frame["id"] = "repeat-hello"
        result = await exchange(hello_frame)
        assert result["result"]["value"]["session"] == token
        assert c.control == control
        report["checks"].append("same_nonce_keeps_session")

        vectors = json.loads((ROOT / "tests/vectors/jsonrpc.json").read_text())
        for vector in vectors:
            c.sequence += 1
            text = vector["wire"].replace("$SESSION", c.session).replace('"$SEQ"', str(c.sequence))
            c.transcript.append({"sent_vector": vector["name"], "sent_hex": text.encode().hex()})
            await c.link.write(text.encode() + b"\n")
            if vector.get("silent"):
                await silent_probe()
                report["vectors"].append({"name": vector["name"], "silent": True})
                continue
            value = await receive()
            responses = value if isinstance(value, list) else [value]
            codes = []
            for response in responses:
                rpc.response(response)
                codes.append(response.get("error", {}).get("code", 0))
            assert codes == vector["codes"], (vector, value)
            if "ids" in vector:
                assert [r["id"] for r in responses] == vector["ids"], value
            report["vectors"].append({"name": vector["name"], "response": value})
        report["checks"].append("independent_spec_vectors")

        first = c.make_request("buffer.create", {"size": 1})
        result = await exchange(first)
        first_id = result["result"]["value"]["buffer_id"]
        buffers.add(first_id)
        duplicate = copy.deepcopy(first)
        duplicate["id"] = "different-rpc-id"
        result = await exchange(duplicate)
        assert result["error"]["code"] == 1002, result
        second_id = (await c.request("buffer.create", {"size": 1}))["buffer_id"]
        buffers.add(second_id)
        assert second_id == first_id + 1, (first_id, second_id)
        for buffer_id in (first_id, second_id):
            await c.request("buffer.delete", {"buffer_id": buffer_id})
            buffers.discard(buffer_id)
        report["checks"].append("duplicate_sequence_never_creates_second_buffer")

        first = c.make_request("session.ping")
        second = c.make_request("session.ping")
        assert "result" in await exchange(second)
        assert "result" in await exchange(first)
        report["checks"].append("out_of_order_sequences")

        batch = []
        for _ in range(8):
            frame = c.make_request("session.ping")
            frame["params"]["context"].pop("control", None)
            frame["params"].pop("args")
            batch.append(frame)
        result = await exchange(batch)
        assert len(result) == 8 and all("result" in r for r in result), result
        report["checks"].append("eight_item_batch")

        prior = (await c.request("buffer.create", {"size": 1}))["buffer_id"]
        buffers.add(prior)
        await c.request("buffer.delete", {"buffer_id": prior})
        buffers.discard(prior)
        await c.link.write(
            json.dumps([c.make_request("buffer.create", {"size": 1}), *([0] * 8)],
                       separators=(",", ":")).encode() + b"\n"
        )
        await silent_probe()
        following = (await c.request("buffer.create", {"size": 1}))["buffer_id"]
        buffers.add(following)
        assert following == prior + 1
        await c.request("buffer.delete", {"buffer_id": following})
        buffers.discard(following)
        report["checks"].append("over_limit_batch_has_zero_execution")

        buffer_id = (await c.request("buffer.create", {"size": 4096}))["buffer_id"]
        buffers.add(buffer_id)
        for offset in range(0, 4096, 384):
            await c.request("buffer.write", {"buffer_id": buffer_id, "offset": offset,
                                           "data": base64.b64encode(bytes(min(384, 4096-offset))).decode()})
        result = await exchange([
            c.make_request("buffer.read", {"buffer_id": buffer_id, "offset": 0, "length": 4096})
            for _ in range(2)
        ])
        assert "result" in result[0] and result[1]["error"]["code"] == 1001, result
        assert result[1]["error"]["data"]["external_effect_possible"] is True
        assert len(json.dumps(result, separators=(",", ":")).encode()) <= 8192
        await c.request("buffer.delete", {"buffer_id": buffer_id})
        buffers.discard(buffer_id)
        report["checks"].append("batch_response_budget_preserves_complete_json")

        for position in range(3):
            batch = []
            for index in range(3):
                batch.append(c.make_request("outputs.stop") if index == position else
                             c.make_request("resource.open", {"kind": "gpio", "pins": {"pin": 13},
                                                               "config": {"mode": "output", "level": 1}}))
            started = time.perf_counter()
            result = await exchange(batch)
            report.setdefault("stop_ack_ms", []).append((time.perf_counter()-started)*1000)
            assert result[0]["id"] == batch[position]["id"] and "result" in result[0], result
            assert all(r["error"]["data"]["reason"] == "stop_in_progress" for r in result[1:])
            c.control = ""
            # Confirm cleanup; an accepted stop alone does not prove completion.
            await c.job("outputs.stop")
            assert not await c.resources()
            await c.request("control.acquire")
        report["checks"].append("stop_barrier_all_three_positions")

        # One write queues a normal batch followed by a separate urgent frame.
        # Control acquisition needs no control token and would expose a failure
        # to invalidate the remaining batch after an externally arriving stop.
        batch = []
        for _ in range(8):
            frame = c.make_request("control.acquire")
            frame["params"]["context"].pop("control", None)
            frame["params"].pop("args")
            batch.append(frame)
        urgent = c.make_request("outputs.stop")
        await c.link.write(rpc.encode(batch) + rpc.encode(urgent))
        outputs = [await receive(), await receive()]
        responses = next(item for item in outputs if isinstance(item, list))
        stopped = next(item for item in outputs if isinstance(item, dict))
        assert stopped["id"] == urgent["id"] and "result" in stopped, outputs
        errors = [r for r in responses if "error" in r]
        assert errors, "夹具没有在batch执行中插入stop；不能声称验证了抢占"
        assert all(r["error"]["data"]["reason"] == "stop_in_progress" for r in errors)
        c.control = ""
        await c.job("outputs.stop")
        state = await c.request("state")
        assert state["owner_endpoint"] == -1 and not await c.resources(), state
        report["external_stop_batch"] = {"responses": responses, "stop": stopped}
        report["checks"].append("incoming_stop_preempts_batch_and_prevents_control_reacquisition")
        await c.request("control.acquire")

        frame = c.make_request("session.ping")
        frame["id"] = "split-中文"
        data = rpc.encode(frame)
        boundary = data.index("中".encode()) + 1
        await c.link.write(data[:boundary])
        await c.link.write(data[boundary:])
        assert (await receive())["id"] == frame["id"]
        await c.link.write(b"x" * 1025 + b"\n")
        assert (await receive())["error"]["code"] == -32700
        await c.request("session.ping")
        report["checks"].append("split_utf8_and_overlong_frame_recovery")

        await c.request("control.release")
        report["benchmark_scope"] = "host Client.request roundtrip, session.ping, no control token"
        times = []
        for _ in range(30):
            started = time.perf_counter()
            await c.request("session.ping")
            times.append((time.perf_counter() - started)*1000)
        report["usb_ping_ms"] = {"count": len(times), "median": statistics.median(times),
                                 "p95": sorted(times)[math.ceil(.95*len(times))-1], "max": max(times)}
        await c.request("control.acquire")
        await c.request("event.subscribe", {"topics": ["resource"]})
        pin = await c.open_resource("gpio", {"pin": 13}, {"mode": "input"})
        await c.job("resource.close", {"resource": pin})
        await c.request("session.ping")
        assert any(e.get("method") == "state.changed" for e in c.events)
        await c.request("event.unsubscribe")
        await c.job("outputs.stop")
        report["checks"].append("state_changed_notification_and_query_recovery")
        report["diagnostics"] = await c.request("diagnostics")
    except BaseException as exc:  # noqa: BLE001 -- evidence includes original interruption
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if identified:
            report["cleanup"] = await cleanup(c, expected_device)
            if buffers and report["cleanup"]["status"] == "confirmed":
                try:
                    if c.boot_id == report["identity"]["boot_id"]:
                        await c.request("control.acquire")
                        for buffer_id in sorted(buffers):
                            try:
                                await c.request("buffer.delete", {"buffer_id": buffer_id})
                            except ProtocolError as exc:
                                if exc.code != "buffer_not_found":
                                    raise
                        await c.job("outputs.stop")
                    buffers.clear()
                except BaseException as exc:  # noqa: BLE001 -- preserve cleanup uncertainty
                    report["cleanup"] = {"status": "unknown", "error": str(exc)}
        report["remaining_test_buffers"] = sorted(buffers)
        await c.close()
        report["transcript"] = c.transcript
    if "error" in report or report["cleanup"]["status"] == "unknown":
        raise VerificationFailure(report)
    return report


def verify_rpc(port: str, expected_device: str) -> dict:
    return asyncio.run(verify_rpc_link(SerialLink(port), expected_device))
