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
    REPO / "examples/sniffer.yaml",
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
    extra: str = "",
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
{program_yaml}{extra}"""


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


def test_omitted_hub_id_is_rainbird(tmp_path: Path):
    path = tmp_path / "auto-id.yaml"
    path.write_text(_yaml())
    result = _run(path, tmp_path)
    blob = result.stdout + result.stderr
    assert result.returncode == 0, blob
    assert "rainbird_zone1_valve_switch" in blob
    assert "rainbirdcomponent" not in blob


def test_second_hub_id_is_rainbird_2(tmp_path: Path):
    path = tmp_path / "two-hubs.yaml"
    path.write_text(
        f"""\
esphome:
  name: rainbird-two
  min_version: 2026.9.0

esp32:
  variant: esp32c3

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

# The C3 has two 48-symbol receive blocks. Two receivers get one each.
remote_receiver:
  - id: rx_a
    pin: GPIO4
    idle: 4ms
    filter: 50us
    receive_symbols: 384
    rmt_symbols: 48
  - id: rx_b
    pin: GPIO5
    idle: 4ms
    filter: 50us
    receive_symbols: 384
    rmt_symbols: 48

rainbird:
  - model: ESP-TM2
    listen_only: true
    receiver_id: rx_a
    zones:
      - zone: 1
        name: Front
    programs: []
  - model: ESP-TM2
    listen_only: true
    receiver_id: rx_b
    zones:
      - zone: 1
        name: Back
    programs: []
"""
    )
    result = _run(path, tmp_path)
    blob = result.stdout + result.stderr
    assert result.returncode == 0, blob
    assert "rainbird_zone1_valve_switch" in blob
    assert "rainbird_2_zone1_valve_switch" in blob
    assert "rainbirdcomponent" not in blob


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


SEND_RAW = """
interval:
  - interval: 60s
    then:
      - rainbird.send_raw: [0x02]
"""


def test_send_raw_needs_experimental(tmp_path: Path):
    _expect_fail(tmp_path, "experimental: true", extra=SEND_RAW)
    path = tmp_path / "raw.yaml"
    path.write_text(_yaml(rainbird_extra="  experimental: true\n", extra=SEND_RAW))
    result = _run(path, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    path.write_text(
        _yaml(
            transmitter=False,
            listen_only=True,
            rainbird_extra="  experimental: true\n",
            extra=SEND_RAW,
        )
    )
    result = _run(path, tmp_path)
    blob = result.stdout + result.stderr
    assert result.returncode != 0, blob
    assert "listen_only" in blob, blob


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


@pytest.mark.parametrize("letter", ["AB", "BC", '""'])
def test_platform_program_must_be_one_letter(tmp_path: Path, letter: str):
    extra = f"""
button:
  - platform: rainbird
    program: {letter}
    start:
      name: Run it
"""
    _expect_fail(tmp_path, "program letter", programs="    []", extra=extra)


def test_hand_written_entity_cannot_repeat_the_hubs(tmp_path: Path):
    extra = """
switch:
  - platform: rainbird
    zone: 1
    valve:
      name: Front again
"""
    _expect_fail(tmp_path, "already creates this switch valve for zone 1", extra=extra)


def test_hand_written_entities_cannot_repeat_each_other(tmp_path: Path):
    extra = """
switch:
  - platform: rainbird
    zone: 3
    valve:
      name: Side
  - platform: rainbird
    zone: 3
    valve:
      name: Side again
number:
  - platform: rainbird
    zone: 3
    run_duration:
      name: Side duration
"""
    _expect_fail(tmp_path, "defined more than once", extra=extra)


def test_number_platform_rejects_keys_it_would_ignore(tmp_path: Path):
    extra = """
number:
  - platform: rainbird
    zone: 3
    program: A
    run_duration:
      name: Side duration
"""
    _expect_fail(tmp_path, "Remove program", programs="    []", extra=extra)


def test_controller_numbers_have_no_initial_or_restored_value(tmp_path: Path):
    programs = "    - program: A\n      seasonal_adjust_number:\n        restore_value: true"
    _expect_fail(tmp_path, "restore_value", programs=programs)


def test_accept_model_ids_are_16_bit(tmp_path: Path):
    _expect_fail(tmp_path, "accept_model_ids", rainbird_extra="  accept_model_ids: [0x10000]\n")


def test_clock_sync_is_off_unless_configured(tmp_path: Path):
    path = tmp_path / "clock.yaml"
    time = "time:\n  - platform: sntp\n    id: ha_time\n"
    path.write_text(_yaml(rainbird_extra="  time_id: ha_time\n", extra=time))
    result = _run(path, tmp_path)
    blob = result.stdout + result.stderr
    assert result.returncode == 0, blob
    assert "clock_sync:" not in blob, blob
    # A bare `clock_sync:` and `schedule:` mean "use the defaults".
    path.write_text(
        _yaml(rainbird_extra="  time_id: ha_time\n  clock_sync:\n  schedule:\n", extra=time)
    )
    result = _run(path, tmp_path)
    blob = result.stdout + result.stderr
    assert result.returncode == 0, blob
    assert "max_drift: 30s" in blob, blob


def test_rmt_blocks_round_up(tmp_path: Path):
    # 390 + 120 symbols is under 512, but ESP-IDF needs 7 + 2 blocks of 64, and there are 8.
    _expect_fail(tmp_path, "RMT symbol memory", rx_rmt=390, tx_rmt=120)


def test_c3_receiver_fits_the_receive_blocks(tmp_path: Path):
    # 128 symbols is 3 blocks. A C3 receiver can use only its 2 (96 symbols).
    _expect_fail(tmp_path, "at most 2 blocks", variant="esp32c3", rx_rmt=128)
    path = tmp_path / "c3.yaml"
    path.write_text(_yaml(variant="esp32c3", rx_rmt=96))
    result = _run(path, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr


def test_max_drift_has_a_floor(tmp_path: Path):
    time = "time:\n  - platform: sntp\n    id: ha_time\n"
    _expect_fail(
        tmp_path,
        "max_drift",
        rainbird_extra="  time_id: ha_time\n  clock_sync:\n    max_drift: 2s\n",
        extra=time,
    )


def test_sync_clock_needs_time_id(tmp_path: Path):
    extra = """
button:
  - platform: rainbird
    sync_clock:
      name: Sync controller clock
"""
    _expect_fail(tmp_path, "sync_clock needs time_id", extra=extra)


def test_test_all_zones_needs_a_test_duration(tmp_path: Path):
    extra = """
button:
  - platform: rainbird
    test_all_zones:
      name: Test all zones
"""
    _expect_fail(
        tmp_path,
        "needs a test_duration number",
        rainbird_extra="  experimental: true\n",
        extra=extra,
    )
    passing = tmp_path / "pass.yaml"
    passing.write_text(
        _yaml(
            rainbird_extra="  experimental: true\n",
            extra=extra
            + "\nnumber:\n  - platform: rainbird\n    test_duration:\n      name: Test minutes\n",
        )
    )
    result = _run(passing, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr


def test_start_slot_switch_needs_its_time(tmp_path: Path):
    programs = "    - program: A\n      start_time_switches: true"
    _expect_fail(
        tmp_path,
        "program A start 1 enabled switch needs that slot's start_time",
        programs=programs,
        rainbird_extra="  schedule:\n    allow_writes: true\n",
    )
    passing = tmp_path / "pass.yaml"
    passing.write_text(
        _yaml(
            programs=programs + "\n      start_time_datetimes: true",
            rainbird_extra="  schedule:\n    allow_writes: true\n",
        )
    )
    result = _run(passing, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr


def test_ha_actions_package_validates(tmp_path: Path):
    path = tmp_path / "actions.yaml"
    extra = f"""
api:

packages:
  actions: !include {REPO / "packages/ha-actions.yaml"}

number:
  - platform: rainbird
    rain_delay:
      id: rain_delay
      name: Rain delay

button:
  - platform: rainbird
    stop:
      id: stop_watering
      name: Stop watering
"""
    path.write_text(_yaml(extra=extra))
    result = _run(path, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr


def test_valve_needs_its_run_duration(tmp_path: Path):
    zones = "    - zone: 1\n      name: Front\n      run_duration_number: false"
    _expect_fail(tmp_path, "zone 1 valve switch needs that zone's run_duration", zones=zones)
    extra = """
button:
  - platform: rainbird
    zone: 2
    queue:
      name: Queue side
"""
    _expect_fail(tmp_path, "zone 2 queue button needs that zone's run_duration", extra=extra)
