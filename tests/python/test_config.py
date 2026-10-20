from pathlib import Path
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]
COMPONENTS = REPO / "components"
ESPHOME = shutil.which("esphome")

pytestmark = pytest.mark.skipif(ESPHOME is None, reason="esphome is not installed")

COMMITTED = [
    REPO / "examples/minimal.yaml",
    REPO / "tests/components/rainbird/test.esp32-idf.yaml",
    REPO / "tests/components/rainbird/test.esp32-ard.yaml",
    REPO / "tests/components/rainbird/test.esp32-c3-idf.yaml",
    REPO / "tests/components/rainbird/validate.esp32-idf.yaml",
]


def _run(path: Path, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [ESPHOME, "config", str(path)],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )


def _yaml(
    *,
    variant: str = "esp32",
    model: str = "ESP-TM2",
    transmitter: bool = True,
    listen_only: bool = False,
    duty: str = "100%",
    non_blocking: bool | None = True,
    receive_symbols: int | None = 384,
    rx_rmt: int | None = 384,
    tx_rmt: int | None = None,
    idle: str = "4ms",
    filter_us: str = "50us",
    zones: str = "    - zone: 1\n      name: Front",
    programs: str = "",
    rainbird_extra: str = "",
) -> str:
    # C2 and C61 reject RMT keys in the receiver schema, before our check.
    receiver = ["  pin: GPIO4", f"  idle: {idle}", f"  filter: {filter_us}"]
    if receive_symbols is not None:
        receiver.append(f"  receive_symbols: {receive_symbols}")
    if rx_rmt is not None:
        receiver.append(f"  rmt_symbols: {rx_rmt}")
    tx_lines = ["  pin: GPIO5", f"  carrier_duty_percent: {duty}"]
    if non_blocking is not None:
        tx_lines.append(f"  non_blocking: {'true' if non_blocking else 'false'}")
    if tx_rmt is not None:
        tx_lines.append(f"  rmt_symbols: {tx_rmt}")
    transmitter_yaml = ""
    if transmitter:
        transmitter_yaml = "remote_transmitter:\n" + "\n".join(tx_lines) + "\n"
    listen = "  listen_only: true\n" if listen_only else ""
    program_yaml = f"  programs:\n{programs}\n" if programs else ""
    return f"""\
esphome:
  name: rainbird-config-test
  min_version: 2026.9.0

esp32:
  variant: {variant}

logger:

wifi:
  ssid: test-ssid
  password: test-password

ota:
  - platform: esphome

external_components:
  - source:
      type: local
      path: {COMPONENTS}
    components:
      - rainbird

{transmitter_yaml}remote_receiver:
{chr(10).join(receiver)}

rainbird:
  model: {model}
{listen}{rainbird_extra}  zones:
{zones}
{program_yaml}"""


def _expect_fail(tmp_path: Path, needle: str, **kwargs) -> None:
    path = tmp_path / "fail.yaml"
    path.write_text(_yaml(**kwargs))
    result = _run(path, tmp_path)
    blob = result.stdout + result.stderr
    assert result.returncode != 0, blob
    assert needle in blob, blob


@pytest.mark.parametrize("path", COMMITTED, ids=lambda path: path.name)
def test_committed_configs(path: Path):
    result = _run(path, REPO)
    assert result.returncode == 0, result.stdout + result.stderr


def test_listen_only_without_transmitter(tmp_path: Path):
    path = tmp_path / "listen.yaml"
    path.write_text(_yaml(transmitter=False, listen_only=True))
    result = _run(path, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr


def test_carrier_must_be_baseband(tmp_path: Path):
    _expect_fail(tmp_path, "carrier_duty_percent: 100%", duty="50%")


def test_transmitter_must_be_non_blocking(tmp_path: Path):
    _expect_fail(tmp_path, "non_blocking: true", non_blocking=False)


def test_receive_symbols_omitted(tmp_path: Path):
    _expect_fail(tmp_path, "receive_symbols: 384", receive_symbols=None)


def test_receive_symbols_too_small(tmp_path: Path):
    _expect_fail(tmp_path, "receive_symbols: 384", receive_symbols=192)


def test_original_esp32_rmt_symbols(tmp_path: Path):
    _expect_fail(tmp_path, "rmt_symbols: 384", rx_rmt=64)


def test_idle_too_long_and_too_short(tmp_path: Path):
    _expect_fail(tmp_path, "1ms", idle="50ms")
    _expect_fail(tmp_path, "1ms", idle="100us")


def test_filter_eats_half_bits(tmp_path: Path):
    _expect_fail(tmp_path, "filter: 50us", filter_us="150us")


def test_rmt_symbol_sum(tmp_path: Path):
    _expect_fail(tmp_path, "RMT symbol memory", tx_rmt=256)


@pytest.mark.parametrize(
    ("variant", "needle"),
    [("esp32s2", "ESP32-S2"), ("esp32c2", "ESP32-C2"), ("esp32c61", "ESP32-C61")],
)
def test_unsupported_variants(tmp_path: Path, variant: str, needle: str):
    kwargs = {}
    if variant in ("esp32c2", "esp32c61"):
        kwargs = {
            "receive_symbols": None,
            "rx_rmt": None,
            "non_blocking": None,
        }
    _expect_fail(tmp_path, needle, variant=variant, **kwargs)


def test_unimplemented_and_unknown_model(tmp_path: Path):
    _expect_fail(tmp_path, "not implemented yet", model="ESP-ME")
    _expect_fail(tmp_path, "Unknown model", model="ESP-NOPE")


def test_zone_and_program_limits(tmp_path: Path):
    _expect_fail(
        tmp_path,
        "Zone 13",
        zones="    - zone: 13\n      name: Too many",
    )
    _expect_fail(
        tmp_path,
        "listed more than once",
        zones="    - zone: 1\n      name: Front\n    - zone: 1\n      name: Again",
    )
    _expect_fail(
        tmp_path,
        "Program D",
        programs="    - program: D\n      name: Missing",
    )
