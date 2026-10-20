import pytest

from rainbird.entity_kinds import EntityKind, FanOut, Scope
from rainbird.errors import ConfigError
from rainbird.expand import expand, fill_name
from rainbird.models import resolve_model

HUB = "rainbird_controller"


def run(**kwargs):
    model = kwargs.pop("model", resolve_model("ESP-TM2"))
    args = {
        "zones": None,
        "programs": None,
        "zone_defaults": None,
        "program_defaults": None,
        "zone_defaults_set": False,
        "program_defaults_set": False,
    }
    args.update(kwargs)
    return expand(model, HUB, **args)


def test_fill_name_leaves_substitutions():
    assert fill_name("${name} {zone}", zone="Front") == "${name} Front"


def test_default_expansion_is_thirty_entities():
    result = run()
    assert len(result.zones) == 12
    assert [zone.number for zone in result.zones] == list(range(1, 13))
    assert [program.letter for program in result.programs] == ["A", "B", "C"]
    assert len(result.entities) == 30
    assert result.entities[0].id == f"{HUB}_zone1_valve_switch"
    assert result.entities[0].name == "Zone 1"
    assert result.entities[0].zone == 1
    assert result.entities[0].program == 255
    duration = result.entities[1]
    assert duration.id == f"{HUB}_zone1_run_duration_number"
    assert duration.name == "Zone 1 duration"
    assert result.entities[-2].id == f"{HUB}_program_c_start_button"
    assert result.entities[-2].name == "Run Program C"
    seasonal = result.entities[-1]
    assert seasonal.id == f"{HUB}_program_c_seasonal_adjust_number"
    assert seasonal.name == "Program C seasonal adjust"
    assert seasonal.program == 2
    assert seasonal.zone == 0
    assert len({entity.id for entity in result.entities}) == 30


def test_explicit_ids_and_names():
    result = run(
        zones=[{"zone": 1, "id": "front_lawn", "name": "Front lawn"}],
        programs=[{"program": "A", "id": "lawns", "name": "Lawns"}],
    )
    ids = [entity.id for entity in result.entities]
    assert ids == [
        "front_lawn_valve_switch",
        "front_lawn_run_duration_number",
        "lawns_start_button",
        "lawns_seasonal_adjust_number",
    ]
    assert result.entities[0].name == "Front lawn"
    assert result.entities[2].name == "Run Lawns"
    assert result.entities[3].name == "Lawns seasonal adjust"


def test_false_drops_only_that_entity():
    result = run(
        zones=[{"zone": 1, "name": "Front", "valve_switch": False}],
        programs=[],
        program_defaults_set=True,
    )
    ids = [entity.id for entity in result.entities]
    assert ids == [f"{HUB}_zone1_run_duration_number"]


def test_string_shorthand_is_a_name():
    result = run(
        zones=[{"zone": 1, "valve_switch": "Sprinkler"}],
        programs=[],
        program_defaults_set=True,
    )
    assert result.entities[0].name == "Sprinkler"
    assert result.entities[0].id == f"{HUB}_zone1_valve_switch"


def test_zone_overrides_merge_over_defaults():
    result = run(
        zone_defaults={"run_duration_number": {"name": "{zone} duration", "initial_value": 10}},
        zone_defaults_set=True,
        zones=[
            {
                "zone": 1,
                "name": "Front lawn",
                "run_duration_number": {"initial_value": 5},
            }
        ],
        programs=[],
        program_defaults_set=True,
    )
    assert len(result.entities) == 1
    entity = result.entities[0]
    assert entity.name == "Front lawn duration"
    assert entity.options["initial_value"] == 5


def test_set_defaults_suppress_implicit_kinds():
    result = run(
        zones=[{"zone": 1, "name": "Front"}],
        zone_defaults={},
        zone_defaults_set=True,
        programs=[],
        program_defaults_set=True,
    )
    assert result.entities == ()


def test_zone_can_add_a_key_missing_from_defaults():
    result = run(
        zones=[{"zone": 1, "name": "Front", "valve_switch": "Tap"}],
        zone_defaults={},
        zone_defaults_set=True,
        programs=[],
        program_defaults_set=True,
    )
    assert [entity.name for entity in result.entities] == ["Tap"]


def test_disabled_zone_keeps_the_zone_and_drops_entities():
    result = run(
        zones=[{"zone": 1, "disabled": True}, {"zone": 2, "name": "Back"}],
        programs=[],
        program_defaults_set=True,
    )
    assert [zone.number for zone in result.zones] == [1, 2]
    assert result.zones[0].disabled is True
    assert [entity.zone for entity in result.entities] == [2, 2]


def test_empty_lists_create_nothing():
    result = run(zones=[], programs=[])
    assert result.zones == ()
    assert result.programs == ()
    assert result.entities == ()


def test_duplicate_zone_and_program():
    with pytest.raises(ConfigError, match="Zone 1 is listed more than once"):
        run(zones=[{"zone": 1}, {"zone": 1}], programs=[])
    with pytest.raises(ConfigError, match="Program A is listed more than once"):
        run(programs=[{"program": "A"}, {"program": "a"}], zones=[])


def test_zone_outside_model_and_bad_program():
    with pytest.raises(ConfigError, match="Zone 13"):
        run(zones=[{"zone": 13}], programs=[])
    with pytest.raises(ConfigError, match="Program D"):
        run(programs=[{"program": "D"}], zones=[])


@pytest.mark.parametrize("letter", ["AB", "BC", "ABC", ""])
def test_program_must_be_one_letter(letter):
    # `in` on a string is a substring test. These used to alias programs A and B.
    with pytest.raises(ConfigError, match="is not valid for ESP-TM2"):
        run(programs=[{"program": letter}], zones=[])


def test_unknown_key_and_bool_zone():
    with pytest.raises(ConfigError, match="Unknown zone setting 'colour'"):
        run(zones=[{"zone": 1, "colour": "green"}], programs=[])
    with pytest.raises(ConfigError, match="zone must be an integer"):
        run(zones=[{"zone": True}], programs=[])


def test_defaults_cannot_set_id():
    with pytest.raises(ConfigError, match="cannot set id"):
        run(
            zone_defaults={"valve_switch": {"id": "shared"}},
            zone_defaults_set=True,
            programs=[],
            program_defaults_set=True,
        )


def test_entity_id_override_and_device_id():
    result = run(
        zones=[
            {
                "zone": 1,
                "id": "front",
                "device_id": "front_device",
                "valve_switch": {"id": "my_valve", "name": "Tap"},
            }
        ],
        programs=[],
        program_defaults_set=True,
    )
    valve = result.entities[0]
    assert valve.id == "my_valve"
    assert valve.options["device_id"] == "front_device"
    assert result.entities[1].options["device_id"] == "front_device"


def test_lowercase_program_letter():
    result = run(programs=[{"program": "b"}], zones=[], zone_defaults_set=True)
    assert result.programs[0].letter == "B"
    assert result.programs[0].index == 1
    assert result.entities[0].id == f"{HUB}_program_b_start_button"


def test_program_fan_out():
    kind = EntityKind(
        99,
        "run_time",
        "number",
        Scope.ZONE,
        "{zone} {program}",
        fan_out=FanOut.PROGRAM,
        default=True,
    )
    result = run(
        zones=[{"zone": 1, "id": "front", "name": "Front"}],
        kinds=(kind,),
    )
    assert [entity.id for entity in result.entities] == [
        "front_run_time_numbers_a",
        "front_run_time_numbers_b",
        "front_run_time_numbers_c",
    ]
    assert [entity.program for entity in result.entities] == [0, 1, 2]
    assert [entity.name for entity in result.entities] == [
        "Front Program A",
        "Front Program B",
        "Front Program C",
    ]
    assert [entity.zone for entity in result.entities] == [1, 1, 1]


def test_program_fan_out_with_no_programs():
    kind = EntityKind(
        99,
        "run_time",
        "number",
        Scope.ZONE,
        "{zone} {program}",
        fan_out=FanOut.PROGRAM,
        default=True,
    )
    result = run(
        zones=[{"zone": 1}],
        programs=[],
        kinds=(kind,),
    )
    assert result.entities == ()


def test_day_and_slot_fan_out():
    days = EntityKind(
        98,
        "watering_day",
        "switch",
        Scope.PROGRAM,
        "{program} {day}",
        fan_out=FanOut.DAY,
        default=True,
    )
    slots = EntityKind(
        97,
        "start_time",
        "datetime",
        Scope.PROGRAM,
        "{program} start {slot}",
        fan_out=FanOut.SLOT,
        default=True,
    )
    day_result = run(
        zones=[],
        zone_defaults_set=True,
        programs=[{"program": "A", "id": "lawns", "name": "Lawns"}],
        kinds=(days,),
    )
    assert len(day_result.entities) == 7
    assert day_result.entities[0].id == "lawns_watering_day_switches_monday"
    assert day_result.entities[0].name == "Lawns Monday"
    assert day_result.entities[0].index == 0
    assert day_result.entities[6].id == "lawns_watering_day_switches_sunday"
    assert day_result.entities[6].index == 6
    slot_result = run(
        zones=[],
        zone_defaults_set=True,
        programs=[{"program": "A", "name": "Lawns"}],
        kinds=(slots,),
    )
    assert [entity.index for entity in slot_result.entities] == [0, 1, 2, 3]
    assert slot_result.entities[0].id == f"{HUB}_program_a_start_time_datetimes_1"
    assert slot_result.entities[3].name == "Lawns start 4"
    assert slot_result.entities[0].program == 0


def test_resolve_model():
    model = resolve_model("esp-tm2")
    assert model.name == "ESP-TM2"
    assert model.zones == 12
    assert model.letters == "ABC"
    with pytest.raises(ConfigError, match="not implemented yet"):
        resolve_model("ESP-ME")
    with pytest.raises(ConfigError, match="Unknown model"):
        resolve_model("ESP-NOPE")
