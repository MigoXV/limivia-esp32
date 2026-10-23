"""通过 v3 对指定单板执行 OTA，并验证实际启动的 ELF 身份。"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import logging
import struct
from pathlib import Path

from limivia_esp32.device import SerialLink
from limivia_esp32.v3_validation import V3Client
from limivia_esp32.validation import VerificationFailure

logger = logging.getLogger(__name__)


def image_manifest(path: Path) -> tuple[bytes, dict]:
    data = path.read_bytes()
    if len(data) < 288 or len(data) > 0x1B0000:
        raise ValueError("镜像超出固定 OTA 槽范围")
    # ESP-IDF 6.0.2 esp_image_header_t + first segment header + esp_app_desc_t.
    if data[0] != 0xE9 or struct.unpack_from("<H", data, 12)[0] != 5:
        raise ValueError("镜像目标不是 ESP32-C3")
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("首段缺少 ESP-IDF 应用描述")
    version = data[48:80].split(b"\0", 1)[0].decode("ascii")
    project = data[80:112].split(b"\0", 1)[0].decode("ascii")
    profiles = {f"limivia-{p}-v3rpc": p for p in ("bus", "signal", "iot")}
    if project not in profiles or not version.startswith("0.3."):
        raise ValueError("镜像不是 LIMIVIA 固件 v3")
    return data, {
        "board": "luatos-esp32c3-usb",
        "protocol": 3,
        "profile": profiles[project],
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "build_id": data[176:208].hex(),
        "firmware": version,
    }


async def ota_async(port: str, expected_device: str, image: Path, expect_rollback: bool) -> dict:
    data, manifest = image_manifest(image)
    client = V3Client(SerialLink(port))
    report = {"manifest": manifest, "checks": [], "expect_rollback": expect_rollback}
    try:
        await client.open()
        before = await client.hello(expected_device)
        report["before"] = before
        await client.job("outputs.stop")
        await client.request("control.acquire")
        started = await client.job(
            "ota.begin", {k: v for k, v in manifest.items() if k not in ("build_id", "firmware")}
        )
        for offset in range(0, len(data), 384):
            await client.job(
                "ota.chunk",
                {
                    "upload_id": started["upload_id"],
                    "offset": offset,
                    "data": base64.b64encode(data[offset : offset + 384]).decode(),
                },
            )
            if offset % (384 * 128) == 0:
                logger.info("OTA %s/%s bytes", offset, len(data))
        await client.job("ota.commit", {"upload_id": started["upload_id"], "reboot": True})
        report["checks"].append("image_hash_and_activation")
        await client.close()
        await asyncio.sleep(2)
        await client.open()
        after = await client.hello(expected_device)
        report["after"] = after
        assert after["boot_id"] != before["boot_id"]
        if expect_rollback:
            assert after["build_id"] == before["build_id"]
            assert after["build_id"] != manifest["build_id"]
            report["checks"].append("bootloader_rollback_to_previous_elf")
        else:
            assert after["build_id"] == manifest["build_id"]
            assert after["profile"] == manifest["profile"]
            report["checks"].append("booted_exact_uploaded_elf")
        await client.request("control.acquire")
        report["ota_status"] = await client.job("ota.status")
        assert not await client.resources()
    except BaseException as exc:  # noqa: BLE001 -- preserve the complete OTA attempt
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        try:
            await client.close()
        except BaseException as exc:  # noqa: BLE001 -- close must not discard the attempt
            report["close_error"] = str(exc)
        report["transcript"] = client.transcript
    if "error" in report or "close_error" in report:
        raise VerificationFailure(report)
    return report


def ota(port: str, expected_device: str, image: Path, expect_rollback: bool = False) -> dict:
    return asyncio.run(ota_async(port, expected_device, image, expect_rollback))
