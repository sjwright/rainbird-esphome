"""Turn a hub's zones and programs into entity declarations.

This stays free of ESPHome so the id rules can be tested on the host. The hub
schema then runs each declaration through the real entity schema.

ESPHome validates a domain as soon as that domain is loaded, so the hub cannot
append entries to ``switch:`` and still have them validated. Generated entities
are checked with the same schema the platform modules use, then built from the
hub. Hand-written ``platform: rainbird`` blocks are what ``packages``,
``!extend`` and ``!remove`` edit.
"""

from collections.abc import Sequence
from dataclasses import dataclass

from .const import CONF_DISABLED, CONF_PROGRAM, CONF_ZONE
from .entity_kinds import (
    DAYS,
    KINDS,
    NO_PROGRAM,
    NO_ZONE,
    EntityKind,
    FanOut,
    Scope,
    hub_key,
)
from .errors import ConfigError
from .models import Model

# ESPHome's own keys are plain strings here: this module has no ESPHome import.
_ZONE_FIELDS = {CONF_ZONE, "id", "name", CONF_DISABLED, "device_id"}
_PROGRAM_FIELDS = {CONF_PROGRAM, "id", "name"}


@dataclass(frozen=True)
class ZoneDecl:
    number: int
    id: str
    name: str
    disabled: bool
    device_id: object | None
    raw: dict


@dataclass(frozen=True)
class ProgramDecl:
    index: int
    letter: str
    id: str
    name: str
    raw: dict


@dataclass(frozen=True)
class EntityDecl:
    kind: EntityKind
    id: str
    name: str
    zone: int
    program: int
    index: int
    options: dict


@dataclass(frozen=True)
class Expansion:
    zones: tuple[ZoneDecl, ...]
    programs: tuple[ProgramDecl, ...]
    entities: tuple[EntityDecl, ...]


def fill_name(
    template: str,
    *,
    zone: str | None = None,
    program: str | None = None,
    day: str | None = None,
    slot: str | None = None,
) -> str:
    """Replace ``{zone}``-style tokens. Single braces, so ``${substitutions}`` stay."""
    values = {
        "{zone}": zone,
        "{program}": program,
        "{day}": day,
        "{slot}": slot,
    }
    for token, value in values.items():
        if value is not None:
            template = template.replace(token, value)
    return template


def expand(
    model: Model,
    hub_id: str,
    *,
    zones: list | None,
    programs: list | None,
    zone_defaults: dict | None,
    program_defaults: dict | None,
    zone_defaults_set: bool,
    program_defaults_set: bool,
    kinds: Sequence[EntityKind] = KINDS,
) -> Expansion:
    # Absent defaults mean "generate the kinds marked default". An empty
    # mapping means the user listed nothing, so only keys a zone sets itself apply.
    zone_defaults = _defaults(zone_defaults, zone_defaults_set, "zone_defaults")
    program_defaults = _defaults(program_defaults, program_defaults_set, "program_defaults")
    zone_decls = _zones(model, hub_id, zones, kinds)
    program_decls = _programs(model, hub_id, programs, kinds)
    _reject_unknown(zone_defaults, Scope.ZONE, kinds, "zone_defaults")
    _reject_unknown(program_defaults, Scope.PROGRAM, kinds, "program_defaults")
    _reject_default_id(zone_defaults, "zone_defaults")
    _reject_default_id(program_defaults, "program_defaults")

    entities: list[EntityDecl] = []
    seen: set[str] = set()
    for zone in zone_decls:
        if zone.disabled:
            continue
        entities.extend(
            _scope_entities(
                kinds,
                Scope.ZONE,
                owner_id=zone.id,
                raw=zone.raw,
                defaults=zone_defaults,
                defaults_omitted=not zone_defaults_set,
                zone_number=zone.number,
                zone_name=zone.name,
                program=None,
                programs=program_decls,
                model=model,
                seen=seen,
                device_id=zone.device_id,
            )
        )
    for program in program_decls:
        entities.extend(
            _scope_entities(
                kinds,
                Scope.PROGRAM,
                owner_id=program.id,
                raw=program.raw,
                defaults=program_defaults,
                defaults_omitted=not program_defaults_set,
                zone_number=NO_ZONE,
                zone_name=None,
                program=program,
                programs=program_decls,
                model=model,
                seen=seen,
                device_id=None,
            )
        )
    return Expansion(tuple(zone_decls), tuple(program_decls), tuple(entities))


def _defaults(value: dict | None, was_set: bool, label: str) -> dict | None:
    if not was_set:
        return None
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ConfigError(f"{label} must be a mapping")
    return value


def _zones(
    model: Model, hub_id: str, zones: list | None, kinds: Sequence[EntityKind]
) -> tuple[ZoneDecl, ...]:
    if zones is None:
        zones = [{CONF_ZONE: number} for number in range(1, model.zones + 1)]
    elif isinstance(zones, dict):
        zones = [zones]
    if not isinstance(zones, list):
        raise ConfigError("zones must be a list")

    decls: list[ZoneDecl] = []
    seen: set[int] = set()
    for raw in zones:
        if not isinstance(raw, dict):
            raise ConfigError("Each zone must be a mapping")
        _reject_unknown(raw, Scope.ZONE, kinds, "zone")
        number = raw.get(CONF_ZONE)
        if isinstance(number, bool) or not isinstance(number, int):
            raise ConfigError("zone must be an integer")
        if not 1 <= number <= model.zones:
            raise ConfigError(f"Zone {number} is outside 1–{model.zones} for {model.name}")
        if number in seen:
            raise ConfigError(f"Zone {number} is listed more than once")
        seen.add(number)
        zone_id = raw.get("id") or f"{hub_id}_zone{number}"
        if not isinstance(zone_id, str):
            raise ConfigError(f"Zone {number} id must be a string")
        name = raw.get("name") or f"Zone {number}"
        if not isinstance(name, str):
            raise ConfigError(f"Zone {number} name must be a string")
        disabled = bool(raw.get(CONF_DISABLED, False))
        decls.append(ZoneDecl(number, zone_id, name, disabled, raw.get("device_id"), raw))
    return tuple(decls)


def _programs(
    model: Model, hub_id: str, programs: list | None, kinds: Sequence[EntityKind]
) -> tuple[ProgramDecl, ...]:
    if programs is None:
        programs = [{CONF_PROGRAM: letter} for letter in model.letters]
    elif isinstance(programs, dict):
        programs = [programs]
    if not isinstance(programs, list):
        raise ConfigError("programs must be a list")

    decls: list[ProgramDecl] = []
    seen: set[str] = set()
    for raw in programs:
        if not isinstance(raw, dict):
            raise ConfigError("Each program must be a mapping")
        _reject_unknown(raw, Scope.PROGRAM, kinds, "program")
        letter = raw.get(CONF_PROGRAM)
        if not isinstance(letter, str):
            raise ConfigError("program must be a letter")
        letter = letter.upper()
        # Exact match: `in` on a string is a substring test, so "AB" or "" would pass.
        if len(letter) != 1 or letter not in model.letters:
            raise ConfigError(
                f"Program {letter} is not valid for {model.name} "
                f"(programs {', '.join(model.letters)})"
            )
        if letter in seen:
            raise ConfigError(f"Program {letter} is listed more than once")
        seen.add(letter)
        index = model.letters.index(letter)
        program_id = raw.get("id") or f"{hub_id}_program_{letter.lower()}"
        if not isinstance(program_id, str):
            raise ConfigError(f"Program {letter} id must be a string")
        name = raw.get("name") or f"Program {letter}"
        if not isinstance(name, str):
            raise ConfigError(f"Program {letter} name must be a string")
        decls.append(ProgramDecl(index, letter, program_id, name, raw))
    return tuple(decls)


def _scope_entities(
    kinds: Sequence[EntityKind],
    scope: Scope,
    *,
    owner_id: str,
    raw: dict,
    defaults: dict | None,
    defaults_omitted: bool,
    zone_number: int,
    zone_name: str | None,
    program: ProgramDecl | None,
    programs: tuple[ProgramDecl, ...],
    model: Model,
    seen: set[str],
    device_id: object | None,
) -> list[EntityDecl]:
    entities: list[EntityDecl] = []
    for kind in kinds:
        if kind.scope is not scope:
            continue
        key = hub_key(kind)
        if not _selected(key, raw, defaults, defaults_omitted, kind.default):
            continue
        merged = _merge(key, raw, defaults)
        for item in _fanout(kind, programs, model):
            entities.append(
                _declare(
                    kind,
                    owner_id,
                    merged,
                    zone_number=zone_number,
                    zone_name=zone_name,
                    program=program,
                    item=item,
                    seen=seen,
                    device_id=device_id,
                )
            )
    return entities


def _selected(
    key: str,
    raw: dict,
    defaults: dict | None,
    defaults_omitted: bool,
    is_default: bool,
) -> bool:
    if key in raw:
        return raw[key] is not False
    if defaults is not None and key in defaults:
        return defaults[key] is not False
    return defaults_omitted and is_default


def _merge(key: str, raw: dict, defaults: dict | None) -> dict:
    base: dict = {}
    if defaults is not None and key in defaults and defaults[key] is not False:
        base = _as_mapping(defaults[key], key)
    if "id" in base:
        raise ConfigError(f"{key} in defaults cannot set id")
    if key not in raw or raw[key] is False:
        return base
    return {**base, **_as_mapping(raw[key], key)}


def _as_mapping(value: object, key: str) -> dict:
    if value is True:
        return {}
    if isinstance(value, str):
        return {"name": value}
    if isinstance(value, dict):
        return dict(value)
    raise ConfigError(f"{key} must be a name, a mapping, or false")


def _fanout(kind: EntityKind, programs: tuple[ProgramDecl, ...], model: Model):
    if kind.fan_out is FanOut.NONE:
        return (None,)
    if kind.fan_out is FanOut.PROGRAM:
        return programs
    if kind.fan_out is FanOut.DAY:
        return tuple(enumerate(DAYS))
    if kind.fan_out is FanOut.SLOT:
        return tuple(range(1, model.starts + 1))
    raise ConfigError(f"Unknown fan-out for {kind.key}")


def _declare(
    kind: EntityKind,
    owner_id: str,
    merged: dict,
    *,
    zone_number: int,
    zone_name: str | None,
    program: ProgramDecl | None,
    item: object,
    seen: set[str],
    device_id: object | None,
) -> EntityDecl:
    program_name = program.name if program is not None else None
    program_index = program.index if program is not None else NO_PROGRAM
    day = None
    slot = None
    suffix = ""
    index = 0
    if isinstance(item, ProgramDecl):
        program_name = item.name
        program_index = item.index
        suffix = f"_{item.letter.lower()}"
    elif isinstance(item, tuple):
        index, day = item
        suffix = f"_{str(day).lower()}"
    elif isinstance(item, int):
        slot = str(item)
        index = item - 1
        suffix = f"_{item}"

    options = {key: value for key, value in merged.items() if key not in ("id", "name")}
    entity_id = merged.get("id") or f"{owner_id}_{hub_key(kind)}{suffix}"
    if not isinstance(entity_id, str):
        raise ConfigError(f"{hub_key(kind)} id must be a string")
    if entity_id in seen:
        raise ConfigError(f"Id {entity_id} is generated more than once")
    seen.add(entity_id)
    name = fill_name(
        str(merged.get("name", kind.default_name)),
        zone=zone_name,
        program=program_name,
        day=day,
        slot=slot,
    )
    options["id"] = entity_id
    options["name"] = name
    if device_id is not None and "device_id" not in options:
        options["device_id"] = device_id
    return EntityDecl(kind, entity_id, name, zone_number, program_index, index, options)


def _reject_unknown(
    raw: dict | None, scope: Scope, kinds: Sequence[EntityKind], label: str
) -> None:
    if not raw:
        return
    fields = _ZONE_FIELDS if scope is Scope.ZONE else _PROGRAM_FIELDS
    if label in ("zone_defaults", "program_defaults"):
        fields = set()
    known = {hub_key(kind) for kind in kinds if kind.scope is scope}
    unknown = sorted(set(raw) - fields - known)
    if unknown:
        keys = ", ".join(sorted(known)) or "(none)"
        raise ConfigError(f"Unknown {label} setting '{unknown[0]}'. Expected one of: {keys}.")


def _reject_default_id(defaults: dict | None, label: str) -> None:
    if not defaults:
        return
    for key, value in defaults.items():
        if isinstance(value, dict) and "id" in value:
            raise ConfigError(f"{key} in {label} cannot set id")
