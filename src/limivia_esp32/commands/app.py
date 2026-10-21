from __future__ import annotations

import logging
import os
import shutil
import subprocess
from pathlib import Path
from typing import Annotated

import typer
from serial.tools import list_ports

from limivia_esp32.paths import ROOT

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s [%(name)s] %(message)s")
logger = logging.getLogger(__name__)
app = typer.Typer(help="LIMIVIA ESP32 固件开发与实机验证", no_args_is_help=True)


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
    idf_path: Annotated[
        Path | None, typer.Option(envvar="IDF_PATH", help="ESP-IDF 6.0.2 目录")
    ] = None,
) -> None:
    """编译专用固件。"""
    run_idf(idf_path, ["build"])


@app.command()
def flash(
    port: str = typer.Option(
        ..., "--port", "-p", envvar="LIMIVIA_SERIAL_PORT", help="目标 USB 串口"
    ),
    idf_path: Annotated[
        Path | None, typer.Option(envvar="IDF_PATH", help="ESP-IDF 6.0.2 目录")
    ] = None,
) -> None:
    """构建并烧录，覆盖目标板现有固件。请先在网页断开 USB。"""
    run_idf(idf_path, ["-p", port, "flash"])


@app.command()
def verify(
    port: str = typer.Option(
        ..., "--port", "-p", envvar="LIMIVIA_SERIAL_PORT", help="目标 USB 串口"
    ),
) -> None:
    """实机验证：清空任务、操作 D5 和采样 ADC0；结束后释放实验引脚。"""
    import json

    from limivia_esp32.validation import verify_device

    typer.echo(json.dumps(verify_device(port), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    app()
