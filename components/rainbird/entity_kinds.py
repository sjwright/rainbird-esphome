"""Every entity the component can create is one row (PLAN.md 5.3).

Hub keys, platform keys, default names and generated ids are derived from this
table. ``kind_id`` values are the C++ ``EntityKind`` enum in ``rainbird.h``.
S0 registers the entities a config gets with no extra YAML. Later steps add rows.
"""

from dataclasses import dataclass
from enum import Enum, IntEnum


class Scope(Enum):
    CONTROLLER = "controller"
    ZONE = "zone"
    PROGRAM = "program"


class FanOut(Enum):
    NONE = "none"
    PROGRAM = "program"
    DAY = "day"
    SLOT = "slot"


class Write(Enum):
    NONE = "none"
    LOCAL = "local"
    COMMAND = "command"
    SCHEDULE_PAGE = "schedule_page"


class Gate(Enum):
    NONE = "none"
    ALLOW_WRITES = "allow_writes"
    EXPERIMENTAL = "experimental"


class KindId(IntEnum):
    VALVE = 1
    RUN_DURATION = 2
    PROGRAM_START = 3
    SEASONAL_ADJUST = 4


# Zone 0 and program 255 mean "this binding does not use that index".
# Zones are 1-based. Program 0 is program A, so 0 cannot mean "none".
NO_ZONE = 0
NO_PROGRAM = 255

DAYS = (
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
    "Sunday",
)

_DOMAIN_SUFFIX = {
    "switch": "switch",
    "number": "number",
    "button": "button",
    "binary_sensor": "binary_sensor",
    "sensor": "sensor",
    "text_sensor": "text_sensor",
    "select": "select",
    "datetime": "datetime",
}

_PLURAL_SUFFIX = {
    "switch": "switches",
    "number": "numbers",
    "button": "buttons",
    "binary_sensor": "binary_sensors",
    "sensor": "sensors",
    "text_sensor": "text_sensors",
    "select": "selects",
    "datetime": "datetimes",
}


@dataclass(frozen=True)
class EntityKind:
    kind_id: int
    key: str
    domain: str
    scope: Scope
    default_name: str
    fan_out: FanOut = FanOut.NONE
    default: bool = False
    write: Write = Write.NONE
    gate: Gate = Gate.NONE
    limits: tuple[int, int, int] | None = None
    unit: str | None = None
    # Set when the hub key is not `<key>_<domain>` (start_time_switches).
    key_override: str | None = None


def hub_key(kind: EntityKind) -> str:
    if kind.key_override:
        return kind.key_override
    suffixes = _PLURAL_SUFFIX if kind.fan_out is not FanOut.NONE else _DOMAIN_SUFFIX
    return f"{kind.key}_{suffixes[kind.domain]}"


KINDS = (
    EntityKind(
        KindId.VALVE,
        "valve",
        "switch",
        Scope.ZONE,
        "{zone}",
        default=True,
        write=Write.COMMAND,
    ),
    EntityKind(
        KindId.RUN_DURATION,
        "run_duration",
        "number",
        Scope.ZONE,
        "{zone} duration",
        default=True,
        write=Write.LOCAL,
        limits=(1, 255, 1),
        unit="min",
    ),
    EntityKind(
        KindId.PROGRAM_START,
        "start",
        "button",
        Scope.PROGRAM,
        "Run {program}",
        default=True,
        write=Write.COMMAND,
    ),
    EntityKind(
        KindId.SEASONAL_ADJUST,
        "seasonal_adjust",
        "number",
        Scope.PROGRAM,
        "{program} seasonal adjust",
        default=True,
        write=Write.COMMAND,
        limits=(10, 200, 1),
        unit="%",
    ),
)

KINDS_BY_KEY = {kind.key: kind for kind in KINDS}
KINDS_BY_ID = {int(kind.kind_id): kind for kind in KINDS}
