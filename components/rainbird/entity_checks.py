"""Final validation of the entities on a hub: one control per binding, and the entities a control needs.

Hand-written `platform: rainbird` entries and the hub's generated entities are checked
together. Both are compared by binding (bindings.py), the same value codegen builds from.
"""

import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PLATFORM, CONF_TIME_ID
import esphome.final_validate as fv

from .bindings import Binding, entry_bindings
from .const import (
    CONF_GENERATED,
    CONF_PROGRAM,
    CONF_RAINBIRD_ID,
    CONF_ZONE,
    GENERATED_CONFIG,
    GENERATED_INDEX,
    GENERATED_KIND,
)
from .entity_kinds import KINDS_BY_ID, NO_PROGRAM, NO_ZONE, FanOut, KindId, Scope, Write
from .models import ESP_TM2

BindingKey = tuple[int, int, int, int]

# The domains whose platform entries can hold controls.
_CONTROL_DOMAINS = ("switch", "number", "button", "select", "datetime")


def declaring(ident):
    """The config block that declares an id."""
    full = fv.full_config.get()
    path = full.get_path_for_id(ident)[:-1]
    return full.get_config_for_path(path)


def _key(kind_id: KindId) -> str:
    return KINDS_BY_ID[int(kind_id)].key


def _generated_key(item: dict) -> BindingKey:
    return (item[GENERATED_KIND], item[CONF_ZONE], item[CONF_PROGRAM], item[GENERATED_INDEX])


def _label(domain: str, entry: dict, binding: Binding) -> str:
    kind = binding.kind
    where = (
        f"zone {binding.zone}" if kind.scope is Scope.ZONE else f"program {entry.get(CONF_PROGRAM)}"
    )
    if kind.fan_out is FanOut.SLOT:
        where += f" slot {binding.index + 1}"
    elif kind.fan_out is FanOut.DAY:
        where += f" day {binding.index}"
    return f"{domain} {kind.key} for {where}"


def _control_bindings(entry: dict, domain: str, model) -> list[tuple[BindingKey, str]]:
    """(binding key, label) for each control a hand-written platform entry creates.

    Only controls count: two switches for one valve is a mistake, while a
    second read-only sensor for the same value is harmless.
    """
    return [
        (binding.key, _label(domain, entry, binding))
        for _, binding in entry_bindings(domain, entry, model)
        if binding.kind.write is not Write.NONE
    ]


def _platform_entries(hub_id):
    """(domain, entry) for every hand-written control entry on this hub."""
    full = fv.full_config.get()
    for domain in _CONTROL_DOMAINS:
        entries = full.get(domain) or []
        if isinstance(entries, dict):
            entries = [entries]
        for entry in entries:
            if not isinstance(entry, dict) or entry.get(CONF_PLATFORM) != "rainbird":
                continue
            if entry.get(CONF_RAINBIRD_ID) != hub_id:
                continue
            yield domain, entry


def _check_duplicates(config: dict, domain: str, hub: dict, model) -> None:
    """One entity per binding. Hand-written entries cannot repeat the hub's or each other's."""
    mine = _control_bindings(config, domain, model)
    if not mine:
        return
    counts: dict[BindingKey, int] = {}
    generated_ids: dict[BindingKey, str] = {}
    for item in hub.get(CONF_GENERATED, []):
        binding = _generated_key(item)
        counts[binding] = counts.get(binding, 0) + 1
        generated_ids[binding] = str(item[GENERATED_CONFIG][CONF_ID])
    for entry_domain, entry in _platform_entries(config[CONF_RAINBIRD_ID]):
        for binding, _ in _control_bindings(entry, entry_domain, model):
            counts[binding] = counts.get(binding, 0) + 1
    for binding, label in mine:
        if counts.get(binding, 0) <= 1:
            continue
        if binding in generated_ids:
            raise cv.Invalid(
                f"The rainbird hub already creates this {label} "
                f"(id {generated_ids[binding]}). Remove this entry, or turn the hub's "
                "copy off with `false` under that zone or program."
            )
        raise cv.Invalid(f"This {label} is defined more than once.")


def check_dependencies(hub: dict) -> None:
    """Controls that use another entity's value need that entity on the same hub.

    Without it the control only logs a warning when used, so the config fails here instead.
    """
    model = ESP_TM2
    bindings = {_generated_key(item) for item in hub.get(CONF_GENERATED, [])}
    for domain, entry in _platform_entries(hub[CONF_ID]):
        bindings.update(binding for binding, _ in _control_bindings(entry, domain, model))
    kinds = {binding[0] for binding in bindings}
    if KindId.TEST_ALL in kinds and KindId.TEST_DURATION not in kinds:
        raise cv.Invalid(
            "The test_all_zones button needs a test_duration number on the same rainbird hub: "
            "the button runs each zone for that many minutes."
        )
    for kind_id, zone, program, index in sorted(bindings):
        if kind_id in (KindId.VALVE, KindId.QUEUE_ZONE):
            if (KindId.RUN_DURATION, zone, NO_PROGRAM, 0) not in bindings:
                control = "valve switch" if kind_id == KindId.VALVE else "queue button"
                raise cv.Invalid(
                    f"The zone {zone} {control} needs that zone's run_duration number on the same "
                    "rainbird hub: it runs the zone for that many minutes. To show only whether the "
                    "zone runs, use its running binary sensor."
                )
        if (
            kind_id == KindId.START_ENABLED
            and (KindId.START_TIME, NO_ZONE, program, index) not in bindings
        ):
            letter = model.letters[program] if program < len(model.letters) else "?"
            raise cv.Invalid(
                f"The program {letter} start {index + 1} enabled switch needs that slot's start_time "
                "entity on the same rainbird hub: turning a slot on writes the time it shows."
            )


def validate_entity_platform(domain: str):
    """Final validator for a platform module: what the entry needs from its hub, and duplicates."""

    def validator(config: dict) -> None:
        _validate_entity_platform(config, domain)

    return validator


def _validate_entity_platform(config: dict, domain: str) -> None:
    hub = declaring(config[CONF_RAINBIRD_ID])
    if (
        domain == "text_sensor"
        and _key(KindId.SCHEDULE_CHANGED) in config
        and CONF_TIME_ID not in hub
    ):
        raise cv.Invalid(
            "schedule_changed needs time_id on the rainbird hub: the controller stamps it in "
            "local time, and the time zone converts it."
        )
    if domain == "text_sensor" and _key(KindId.ENDS_AT) in config and CONF_TIME_ID not in hub:
        raise cv.Invalid(
            "ends_at needs time_id on the rainbird hub: the controller reports time left, and the "
            "clock turns that into the time it ends."
        )
    if domain == "button" and _key(KindId.SYNC_CLOCK) in config and CONF_TIME_ID not in hub:
        raise cv.Invalid(
            "sync_clock needs time_id on the rainbird hub: it sets the controller to that clock's local time."
        )
    _check_duplicates(config, domain, hub, ESP_TM2)
