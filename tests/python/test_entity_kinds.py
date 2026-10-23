from pathlib import Path
import re

from rainbird.entity_kinds import KINDS, KindId, Scope, platform_keys, resolve_kind

COMPONENT = Path(__file__).resolve().parents[2] / "components" / "rainbird"
HEADER = COMPONENT / "entity_kind.h"
TABLE = COMPONENT / "entity_values.cpp"
MODEL = COMPONENT / "model_esp_tm2.cpp"


def test_cpp_enum_matches_kind_ids():
    # The C++ hub switches on these numbers, so the two lists must agree exactly.
    text = HEADER.read_text()
    body = text[text.index("enum class EntityKind") :]
    body = body[: body.index("};")]
    cpp = {name: int(value) for name, value in re.findall(r"(\w+)\s*=\s*(\d+)", body)}
    py = {kind.name: int(kind.value) for kind in KindId}
    assert cpp == py


def test_cpp_kind_table_writes_the_same_way():
    # entity_values.cpp has one KindInfo row per kind. Its WritePath decides what submit() does
    # and whether a number is a local setting, so it must match the YAML side's Write.
    text = TABLE.read_text()
    text = text[text.index("constexpr KindInfo KINDS[]") :]
    text = text[: text.index("};")]
    rows = re.findall(r"\{EntityKind::(\w+), [^{}]*WritePath::(\w+)\}", text)
    cpp = dict(rows)
    assert len(cpp) == len(rows), "a kind has two rows"
    py = {kind.kind_id.name: kind.write.name for kind in KINDS}
    assert cpp == py


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


def test_number_limits_match_the_model():
    # The session refuses a command outside the model's ranges; the entity offers the same range.
    text = MODEL.read_text()
    consts = {name: int(value) for name, value in re.findall(r"constexpr \w+ (\w+) = (\d+);", text)}
    limits = {kind.kind_id: kind.limits for kind in KINDS}
    seasonal = limits[KindId.SEASONAL_ADJUST]
    assert (seasonal[0], seasonal[1]) == (
        consts["SEASONAL_MIN_PERCENT"],
        consts["SEASONAL_MAX_PERCENT"],
    )
    assert limits[KindId.RAIN_DELAY][1] == consts["RAIN_DELAY_MAX_DAYS"]
    assert limits[KindId.RUN_DURATION][1] == consts["RUN_MAX_MINUTES"]
    assert limits[KindId.TEST_DURATION][1] == consts["RUN_MAX_MINUTES"]
