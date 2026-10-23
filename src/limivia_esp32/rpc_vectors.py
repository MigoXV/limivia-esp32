"""从已验收的精确固件证据导出供后续 SDK 使用的消息形状向量。"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from limivia_esp32.rpc import CONTRACT, response


def _shape(value, key=""):
    if isinstance(value, dict):
        return tuple((k, _shape(v, k)) for k, v in sorted(value.items()))
    if isinstance(value, list):
        return tuple(sorted({repr(_shape(v)) for v in value}))
    if key in {"state", "reason", "action", "error_code", "method"}:
        return value
    return type(value).__name__


def _public(value):
    if isinstance(value, list):
        return [_public(item) for item in value]
    if isinstance(value, dict):
        return {key: "TEFCLUZJWFRVUkU=" if key == "ssid_base64" else _public(item)
                for key, item in value.items()}
    return value


def export_vectors(evidence: Path, manifest: Path, output: Path) -> dict:
    if output.exists():
        raise ValueError("向量路径已存在，不能覆盖已有证据")
    frozen = json.loads(manifest.read_text())
    if frozen.get("contract") != CONTRACT or set(frozen["images"]) != {"bus", "signal", "iot"}:
        raise ValueError("需要当前JSON-RPC契约的三profile冻结清单")
    cases, sources, seen, excluded = [], {}, set(), {}
    profiles = set()
    for path in sorted(evidence.glob("*-release.json")):
        report = json.loads(path.read_text())
        identity = report.get("identity", {})
        profile = identity.get("profile")
        if profile not in frozen["images"]:
            continue
        if identity.get("build_id") != frozen["images"][profile]["build_id"]:
            raise ValueError(f"镜像身份与冻结清单不一致：{path.name}")
        if report.get("error") or report.get("cleanup", {}).get("status") != "confirmed":
            excluded[path.name] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                   "error": report.get("error"), "cleanup": report.get("cleanup")}
            continue
        profiles.add(profile)
        sources[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        pending = {}
        for entry in report.get("transcript", []):
            sent = entry.get("sent")
            if "sent_hex" in entry:
                try:
                    sent = json.loads(bytes.fromhex(entry["sent_hex"]))
                except (ValueError, UnicodeError):
                    sent = None
            for request in sent if isinstance(sent, list) else [sent]:
                if isinstance(request, dict) and "id" in request:
                    pending[json.dumps(request["id"], sort_keys=True)] = request
            received = entry.get("received")
            for value in received if isinstance(received, list) else [received]:
                if not isinstance(value, dict):
                    continue
                request = None
                if "id" in value:
                    response(value)
                    request = pending.pop(json.dumps(value["id"], sort_keys=True), None)
                method = value.get("method") or (request or {}).get("method")
                fingerprint = (profile, method, repr(_shape(value)))
                if fingerprint in seen:
                    continue
                seen.add(fingerprint)
                cases.append({"profile": profile, "method": method, "source": path.name,
                              "request": _public(request), "message": _public(value)})
    if profiles != set(frozen["images"]):
        raise ValueError("缺少已确认的三profile实机证据")
    result = {"contract": CONTRACT, "source_commit": frozen["source_commit"],
              "build_ids": {p: m["build_id"] for p, m in frozen["images"].items()},
              "purpose": "消息解析向量；请求可能脱敏，SSID替换为LAB-FIXTURE，句柄/会话/boot属于历史实机，不可直接重放",
              "evidence_sha256": sources, "excluded_evidence": excluded, "cases": cases}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    return {"cases": len(cases), "profiles": sorted(profiles), "output": str(output)}
