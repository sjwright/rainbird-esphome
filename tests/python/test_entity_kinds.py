import importlib.util
from pathlib import Path

from rainbird.entity_kinds import KINDS, KindId, Scope, Write, platform_keys, resolve_kind
from rainbird.models import ESP_TM2

REPO = Path(__file__).resolve().parents[2]


def _generator(name: str = "gen_kinds"):
    spec = importlib.util.spec_from_file_location(name, REPO / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_generated_headers_are_current():
    # entity_kind.h and model_limits.h are written from the Python tables. A stale one means
    # the C++ hub and the YAML side disagree about a kind or a limit.
    stale = [path.name for path in _generator().stale()]
    assert not stale, f"{stale} out of date. Run: python3 tools/gen_kinds.py"


def test_readme_entity_tables_are_current():
    # The README's entity tables are written from the kind table, so it cannot list an
    # entity that does not exist or miss one that does.
    assert not _generator("gen_docs").stale(), "Run: python3 tools/gen_docs.py"


def test_schedule_page_kinds_name_the_field_they_edit():
    # A schedule-page write edits its kind's field, so every such kind needs one.
    for kind in KINDS:
        if kind.write is Write.SCHEDULE_PAGE:
            assert kind.field, kind.key


def test_every_kind_id_is_used_once():
    ids = [int(kind.kind_id) for kind in KINDS]
    assert len(ids) == len(set(ids))
    assert set(ids) == {int(kind) for kind in KindId}


def test_running_means_zone_or_program_by_selector():
    assert "running" in platform_keys("binary_sensor")
    zone = resolve_kind("binary_sensor", "running", zone=True, program=False)
    program = resolve_kind("binary_sensor", "running", zone=False, program=True)
    assert zone is not None and zone.scope is Scope.ZONE
    assert program is not None and program.scope is Scope.PROGRAM
    assert resolve_kind("binary_sensor", "running", zone=False, program=False) is None
    watering = resolve_kind("binary_sensor", "watering", zone=False, program=False)
    assert watering is not None and watering.scope is Scope.CONTROLLER


def test_number_limits_come_from_the_model():
    # The session refuses a command outside the model's ranges; the entity offers the same range.
    limits = {kind.kind_id: kind.limits for kind in KINDS}
    assert limits[KindId.SEASONAL_ADJUST][:2] == (
        ESP_TM2.seasonal_min_percent,
        ESP_TM2.seasonal_max_percent,
    )
    assert limits[KindId.RAIN_DELAY][1] == ESP_TM2.rain_delay_max_days
    assert limits[KindId.RUN_DURATION][1] == ESP_TM2.run_max_minutes
    assert limits[KindId.TEST_DURATION][1] == ESP_TM2.run_max_minutes
