"""实机证据与身份确认；清理失败不能覆盖原始错误。"""

from __future__ import annotations


class VerificationFailure(RuntimeError):
    def __init__(self, report: dict):
        self.report = report
        super().__init__(report.get("error", "验证未通过"))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


async def cleanup(client, device_id: str) -> dict:
    try:
        await client.close()
        await client.open()
        await client.hello(device_id)
        await client.job("outputs.stop")
        remaining = await client.resources()
        require(not remaining, "清理后仍有占用或故障资源")
        return {"status": "confirmed"}
    except BaseException as exc:  # noqa: BLE001 -- cleanup may be interrupted
        return {"status": "unknown", "error": f"{type(exc).__name__}: {exc}"}
