from __future__ import annotations

import logging
import os
import shutil
import subprocess
from enum import Enum
from pathlib import Path
from typing import Annotated

import typer
from serial.tools import list_ports

from limivia_esp32.paths import ROOT

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s [%(name)s] %(message)s")
logger = logging.getLogger(__name__)
app = typer.Typer(help="LIMIVIA ESP32 固件开发与实机验证", no_args_is_help=True)


class Profile(str, Enum):
    bus = "bus"
    signal = "signal"
    iot = "iot"


def profile_arguments(profile: Profile) -> list[str]:
    return ["-B", str(ROOT / "build" / profile.value), f"-DLIMIVIA_PROFILE={profile.value}"]


def run_idf(idf_path: Path | None, arguments: list[str]) -> None:
    if idf_path is None or not (idf_path / "export.sh").is_file():
        raise typer.BadParameter("请通过 --idf-path 或 IDF_PATH 指定 ESP-IDF 6.0.2 目录")
    activation = Path(os.environ.get("LIMIVIA_IDF_ACTIVATE", str(idf_path / "export.sh")))
    if not activation.is_file():
        raise typer.BadParameter("LIMIVIA_IDF_ACTIVATE 指定的环境激活脚本不存在")
    environment = os.environ.copy()
    # ESP-IDF owns a separate interpreter; never hand it the active Poetry venv.
    for key in ("VIRTUAL_ENV", "POETRY_ACTIVE", "PYTHONHOME"):
        environment.pop(key, None)
    # Pass paths as positional arguments, never interpolate user input into shell code.
    result = subprocess.run(
        [
            "bash",
            "-c",
            'source "$1" >/dev/null && shift && idf.py "$@"',
            "bash",
            str(activation.resolve()),
            "-C",
            str(ROOT),
            *arguments,
        ],
        cwd=ROOT,
        env=environment,
        check=False,
    )
    if result.returncode:
        raise typer.Exit(result.returncode)


@app.command()
def doctor() -> None:
    """显示开发环境及 USB 设备，不占用串口。"""
    for name in ("poetry", "cmake", "ninja"):
        logger.info("%s: %s", name, shutil.which(name) or "未找到")
    logger.info("IDF_PATH: %s", os.environ.get("IDF_PATH", "未设置"))
    for port in list_ports.comports():
        logger.info("串口 %s | %s | %s", port.device, port.description, port.serial_number)


@app.command()
def build(
    profile: Annotated[Profile, typer.Option(help="固件用途镜像")] = Profile.bus,
    idf_path: Annotated[
        Path | None, typer.Option(envvar="IDF_PATH", help="ESP-IDF 6.0.2 目录")
    ] = None,
) -> None:
    """编译专用固件。"""
    run_idf(idf_path, [*profile_arguments(profile), "build"])


@app.command("test-native")
def test_native(
    idf_path: Annotated[
        Path | None, typer.Option(envvar="IDF_PATH", help="ESP-IDF 6.0.2 目录")
    ] = None,
) -> None:
    """在主机运行生产 C 核心、Unity 和 ASan/UBSan；不连接设备。"""
    if idf_path is None:
        raise typer.BadParameter("请通过 --idf-path 或 IDF_PATH 指定 ESP-IDF 6.0.2 目录")
    run_idf(idf_path, [*profile_arguments(Profile.bus), "reconfigure"])
    directory = ROOT / "build" / "native"
    commands = [
        [
            "cmake",
            "-S",
            str(ROOT / "tests" / "native"),
            "-B",
            str(directory),
            f"-DIDF_PATH={idf_path.resolve()}",
        ],
        ["cmake", "--build", str(directory)],
        ["ctest", "--test-dir", str(directory), "--output-on-failure"],
    ]
    for command in commands:
        result = subprocess.run(command, cwd=ROOT, check=False)
        if result.returncode:
            raise typer.Exit(result.returncode)


@app.command()
def flash(
    profile: Annotated[Profile, typer.Option(help="固件用途镜像")] = Profile.bus,
    port: str = typer.Option(
        ..., "--port", "-p", envvar="LIMIVIA_SERIAL_PORT", help="目标 USB 串口"
    ),
    idf_path: Annotated[
        Path | None, typer.Option(envvar="IDF_PATH", help="ESP-IDF 6.0.2 目录")
    ] = None,
) -> None:
    """构建并烧录，覆盖目标板现有固件。请先在网页断开 USB。"""
    run_idf(idf_path, [*profile_arguments(profile), "-p", port, "flash"])


@app.command()
def verify(
    port: str = typer.Option(
        ..., "--port", "-p", envvar="LIMIVIA_SERIAL_PORT", help="目标 USB 串口"
    ),
    expected_device: str | None = typer.Option(None, help="预期 device_id（MAC），不匹配即停止"),
    output: Annotated[
        Path | None, typer.Option(help="证据 JSON 路径；默认 artifacts/verification/ 唯一文件")
    ] = None,
) -> None:
    """实机验证：清空任务、操作 D5 和采样 ADC0；结束后释放实验引脚。"""
    from limivia_esp32.v3_validation import verify_device

    verification_result(
        lambda: verify_device(port, expected_device), output, {"transport": "usb", "port": port}
    )


@app.command("verify-rpc")
def verify_rpc_command(
    port: str = typer.Option(
        ..., "--port", "-p", envvar="LIMIVIA_SERIAL_PORT", help="明确选定的原生 USB 串口"
    ),
    expected_device: str = typer.Option(..., help="预期设备 MAC"),
    output: Annotated[Path | None, typer.Option(help="证据 JSON 路径")] = None,
) -> None:
    """JSON-RPC 实机验收：去重、批量、容量、优先停止与USB基线。"""
    from limivia_esp32.rpc_validation import verify_rpc

    verification_result(
        lambda: verify_rpc(port, expected_device), output, {"transport": "usb", "port": port}
    )


@app.command("export-rpc-vectors")
def export_rpc_vectors_command(
    evidence: Annotated[Path, typer.Option(help="三profile原始验收证据目录")],
    manifest: Annotated[Path, typer.Option(help="冻结镜像manifest.json")],
    output: Annotated[Path, typer.Option(help="新建消息向量JSON文件，不覆盖已有文件")],
) -> None:
    """导出精确ELF匹配的真实消息，供SDK协议解析测试使用。"""
    from limivia_esp32.rpc_vectors import export_vectors

    try:
        logger.info("协议向量：%s", export_vectors(evidence, manifest, output))
    except ValueError as exc:
        raise typer.BadParameter(str(exc)) from exc


@app.command("verify-ble")
def verify_ble_command(
    address: str = typer.Option(..., "--address", help="明确选定的 BLE 地址或平台设备 ID"),
    expected_device: str | None = typer.Option(None, help="预期 device_id（MAC）"),
    output: Annotated[Path | None, typer.Option(help="证据 JSON 路径")] = None,
) -> None:
    """BLE 实机验收：驱动 D5、采样 ADC0、重连并清理。"""
    from limivia_esp32.v3_validation import verify_ble

    verification_result(
        lambda: verify_ble(address, expected_device),
        output,
        {"transport": "ble", "address": address},
    )


@app.command("verify-profile")
def verify_profile_command(
    port: str = typer.Option(..., "--port", "-p", help="明确选定的原生 USB 串口"),
    expected_device: str = typer.Option(..., help="预期设备 MAC"),
    output: Annotated[Path | None, typer.Option(help="证据 JSON 路径")] = None,
) -> None:
    """按当前镜像验证单板外设、已初始化的文件系统和离线流程。"""
    from limivia_esp32.profile_validation import verify_profile

    verification_result(
        lambda: verify_profile(port, expected_device), output, {"transport": "usb", "port": port}
    )


@app.command("ota")
def ota_command(
    port: str = typer.Option(..., "--port", "-p", help="明确选定的原生 USB 串口"),
    expected_device: str = typer.Option(..., help="预期设备 MAC"),
    image: Annotated[
        Path, typer.Option(exists=True, dir_okay=False, help="已构建的 v3 镜像")
    ] = ...,
    expect_rollback: Annotated[bool, typer.Option(help="用于专门的首次启动失败测试镜像")] = False,
    output: Annotated[Path | None, typer.Option(help="证据 JSON 路径")] = None,
) -> None:
    """经 v3 上传并激活非活动 OTA 槽，重启后核对 ELF 身份。"""
    from limivia_esp32.ota_validation import ota

    verification_result(
        lambda: ota(port, expected_device, image, expect_rollback),
        output,
        {"transport": "usb", "port": port, "image": str(image)},
    )


@app.command("verify-variants")
def verify_variants_command(
    port: str = typer.Option(..., "--port", "-p", help="明确选定的原生 USB 串口"),
    expected_device: str = typer.Option(..., help="预期设备 MAC"),
    output: Annotated[Path | None, typer.Option(help="证据 JSON 路径")] = None,
) -> None:
    """补验双UART、SPI多线、I²S模式、ADC标签和RMT RX等板内驱动路径。"""
    from limivia_esp32.profile_validation import verify_variants

    verification_result(
        lambda: verify_variants(port, expected_device), output, {"transport": "usb", "port": port}
    )


@app.command("verify-lifecycle")
def verify_lifecycle_command(
    port: str = typer.Option(..., "--port", "-p", help="明确选定的原生 USB 串口"),
    expected_device: str = typer.Option(..., help="预期设备 MAC"),
    output: Annotated[Path | None, typer.Option(help="证据 JSON 路径")] = None,
) -> None:
    """实机重启与流程验收；IoT 增加睡眠和无线驱动生命周期。"""
    from limivia_esp32.lifecycle_validation import verify_lifecycle

    verification_result(
        lambda: verify_lifecycle(port, expected_device), output, {"transport": "usb", "port": port}
    )


@app.command("verify-dual")
def verify_dual_command(
    port: str = typer.Option(..., "--port", "-p", help="明确选定的 USB 端口"),
    address: str = typer.Option(..., "--address", help="同一块板的 BLE 地址"),
    expected_device: str | None = typer.Option(None, help="预期 device_id（MAC）"),
    output: Annotated[Path | None, typer.Option(help="证据 JSON 路径")] = None,
) -> None:
    """双链路验证：单一控制、观察者停止、BLE 断开后 USB 接管。"""
    from limivia_esp32.dual_validation import verify_dual

    verification_result(
        lambda: verify_dual(port, address, expected_device),
        output,
        {"transport": "dual", "port": port, "address": address},
    )


def verification_result(operation, output: Path | None, fixture: dict) -> None:
    import json
    import uuid
    from datetime import datetime, timezone

    from limivia_esp32.validation import VerificationFailure

    timestamp = datetime.now(timezone.utc).isoformat()
    destination = output or ROOT / "artifacts" / "verification" / f"{uuid.uuid4().hex}.json"
    if destination.exists():
        raise typer.BadParameter("证据路径已存在，请选择新文件；已有失败记录不能覆盖")
    failed = False
    try:
        report = operation()
    except VerificationFailure as exc:
        report = exc.report
        failed = True
    report.update(timestamp=timestamp, fixture=fixture)
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Preserve every attempt, including first failures.
    with destination.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
    typer.echo(
        json.dumps(
            {key: value for key, value in report.items() if key != "transcript"},
            ensure_ascii=False,
            indent=2,
        )
    )
    logger.info("原始证据：%s", destination)
    if failed:
        raise typer.Exit(1)


if __name__ == "__main__":
    app()
