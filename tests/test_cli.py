import json
import sys

import pytest
from typer.testing import CliRunner

from limivia_esp32.commands.app import app, run_idf
from limivia_esp32.paths import ROOT


@pytest.mark.parametrize("command", ["flash", "verify", "verify-rpc"])
def test_hardware_operations_require_explicit_target(command):
    result = CliRunner().invoke(app, [command], env={"LIMIVIA_SERIAL_PORT": ""})
    assert result.exit_code == 2
    assert "--port" in result.output


def test_missing_idf_is_actionable(tmp_path):
    result = CliRunner().invoke(app, ["build", "--idf-path", str(tmp_path)])
    assert result.exit_code == 2
    assert "ESP-IDF 6.0.2" in result.output


def test_ble_requires_explicit_target():
    result = CliRunner().invoke(app, ["verify-ble"])
    assert result.exit_code == 2
    assert "--address" in result.output


def test_dual_requires_both_targets():
    result = CliRunner().invoke(app, ["verify-dual", "--port", "/dev/explicit"])
    assert result.exit_code == 2
    assert "--address" in result.output


def test_idf_uses_standalone_root_and_isolated_environment(tmp_path, monkeypatch):
    idf = tmp_path / "idf with spaces"
    idf.mkdir()
    activation = idf / "export.sh"
    activation.write_text('idf.py() { "$CHECK_PYTHON" "$CHECK_SCRIPT" "$@"; }\n')
    capture = tmp_path / "capture.py"
    result = tmp_path / "result.json"
    capture.write_text(
        "import json, os, sys\n"
        "from pathlib import Path\n"
        "Path(os.environ['CHECK_RESULT']).write_text(json.dumps({\n"
        " 'args': sys.argv[1:], 'cwd': os.getcwd(),\n"
        " 'env': {k: os.environ.get(k) for k in "
        "('VIRTUAL_ENV', 'POETRY_ACTIVE', 'PYTHONHOME')}\n"
        "}))\n"
    )
    monkeypatch.setenv("CHECK_PYTHON", sys.executable)
    monkeypatch.setenv("CHECK_SCRIPT", str(capture))
    monkeypatch.setenv("CHECK_RESULT", str(result))
    monkeypatch.setenv("LIMIVIA_IDF_ACTIVATE", str(activation))
    for key in ("VIRTUAL_ENV", "POETRY_ACTIVE", "PYTHONHOME"):
        monkeypatch.setenv(key, "poetry-only")
    marker = tmp_path / "unexpected"
    port = f"/dev/serial with spaces; touch {marker}"
    run_idf(idf, ["-p", port, "flash"])
    observed = json.loads(result.read_text())
    assert observed["args"] == ["-C", str(ROOT), "-p", port, "flash"]
    assert observed["cwd"] == str(ROOT)
    assert all(value is None for value in observed["env"].values())
    assert not marker.exists()


def test_idf_failure_reaches_cli(tmp_path, monkeypatch):
    (tmp_path / "export.sh").write_text("exit 17\n")
    monkeypatch.delenv("LIMIVIA_IDF_ACTIVATE", raising=False)
    result = CliRunner().invoke(app, ["build", "--idf-path", str(tmp_path)])
    assert result.exit_code == 17
