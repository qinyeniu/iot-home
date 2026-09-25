from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest


def _load_analyzer():
    repo_root = Path(__file__).resolve().parents[2]
    script_path = repo_root / "tools" / "analyze_rf_cq_short_test.py"
    spec = importlib.util.spec_from_file_location(
        "analyze_rf_cq_short_test", script_path
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _write(tmp_path: Path, name: str, content: str) -> Path:
    path = tmp_path / name
    path.write_text(content, encoding="utf-8")
    return path


PASS_LOG = """I (1000) iot-gw: CMD rf: node=0x1234 query
I (1001) rf-cq: submit id=101 source=0 cmd=1 queued=1
I (1002) rf-cq: dequeue id=101 source=0 cmd=1
I (1010) iot-gw: RF power -> node=0x1234 cmd=1 power=0 mode=auto seq=1: ESP_OK
I (1020) iot-gw: RF power: report accepted from unknown to auto/-10 dBm seq=1
I (1021) rf-cq: stack_hwm id=101 bytes=2048
I (2000) iot-gw: CMD rf: node=0x1234 set 0 dBm
I (2001) rf-cq: submit id=102 source=0 cmd=2 queued=1
I (2002) rf-cq: dequeue id=102 source=0 cmd=2
I (2010) iot-gw: RF power -> node=0x1234 cmd=2 power=0 mode=manual seq=2: ESP_OK
I (2020) iot-gw: RF power: report accepted from previous to manual/0 dBm seq=2
I (2021) rf-cq: stack_hwm id=102 bytes=1900
I (3000) iot-gw: CMD rf: node=0x1234 set -10 dBm
I (3001) rf-cq: submit id=103 source=0 cmd=2 queued=1
I (3002) rf-cq: dequeue id=103 source=0 cmd=2
I (3010) iot-gw: RF power -> node=0x1234 cmd=2 power=-10 mode=manual seq=3: ESP_OK
I (3020) iot-gw: RF power: report accepted from previous to manual/-10 dBm seq=3
I (3021) rf-cq: stack_hwm id=103 bytes=1800
I (4000) iot-gw: CMD rf: node=0x1234 automatic
I (4001) rf-cq: submit id=104 source=0 cmd=2 queued=1
I (4002) rf-cq: dequeue id=104 source=0 cmd=2
I (4010) iot-gw: RF power -> node=0x1234 cmd=2 power=-10 mode=auto seq=4: ESP_OK
I (4020) iot-gw: RF power: report accepted from previous to auto/-10 dBm seq=4
I (4021) rf-cq: stack_hwm id=104 bytes=1750
"""

LEGACY_LOG = """I (1000) iot-gw: RF power -> node=0x1234 cmd=1 power=0 mode=auto seq=1: ESP_OK
I (1020) iot-gw: RF power: report accepted from unknown to auto/-10 dBm seq=1
"""

STACK_FAIL_LOG = """I (1000) iot-gw: CMD rf: node=0x1234 query
I (1001) rf-cq: submit id=101 source=0 cmd=1 queued=1
I (1002) rf-cq: dequeue id=101 source=0 cmd=1
I (1010) iot-gw: RF power -> node=0x1234 cmd=1 power=0 mode=auto seq=1: ESP_OK
I (1020) iot-gw: RF power: report accepted from unknown to auto/-10 dBm seq=1
I (1021) rf-cq: stack_hwm id=101 bytes=128
"""

MISSING_REPORT_LOG = """I (1000) iot-gw: CMD rf: node=0x1234 query
I (1001) rf-cq: submit id=101 source=0 cmd=1 queued=1
I (1002) rf-cq: dequeue id=101 source=0 cmd=1
I (1010) iot-gw: RF power -> node=0x1234 cmd=1 power=0 mode=auto seq=1: ESP_OK
I (1021) rf-cq: stack_hwm id=101 bytes=2048
"""


def test_synthetic_full_sequence_pass(tmp_path, capsys, monkeypatch):
    log = _write(tmp_path, "gateway.log", PASS_LOG)
    monkeypatch.setattr(sys, "argv", ["analyze_rf_cq_short_test.py", str(log)])

    rc = _load_analyzer().main()
    out = capsys.readouterr().out

    assert rc == 0
    assert "操作顺序正确：QUERY -> SET 0 -> SET -10 -> AUTO" in out
    assert "结论：**PASS**" in out


def test_legacy_log_is_incomplete(tmp_path, capsys, monkeypatch):
    log = _write(tmp_path, "gateway.log", LEGACY_LOG)
    monkeypatch.setattr(sys, "argv", ["analyze_rf_cq_short_test.py", str(log)])

    rc = _load_analyzer().main()
    out = capsys.readouterr().out

    assert rc == 4
    assert "LEGACY/INCOMPLETE" in out


def test_low_stack_is_fail(tmp_path, capsys, monkeypatch):
    log = _write(tmp_path, "gateway.log", STACK_FAIL_LOG)
    monkeypatch.setattr(sys, "argv", ["analyze_rf_cq_short_test.py", str(log)])

    rc = _load_analyzer().main()
    out = capsys.readouterr().out

    assert rc == 3
    assert "栈剩余仅 128 字节" in out
    assert "结论：**FAIL**" in out


def test_missing_report_is_degraded(tmp_path, capsys, monkeypatch):
    log = _write(tmp_path, "gateway.log", MISSING_REPORT_LOG)
    monkeypatch.setattr(sys, "argv", ["analyze_rf_cq_short_test.py", str(log)])

    rc = _load_analyzer().main()
    out = capsys.readouterr().out

    assert rc == 2
    assert "seq=1 已 ESP_OK，但未观察到匹配 REPORT" in out
    assert "结论：**DEGRADED**" in out
