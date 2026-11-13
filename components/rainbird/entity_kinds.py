"""Every entity the component can create is one row (PLAN.md 5.3).

Hub keys, platform keys, default names and generated ids are derived from this
table. So is the C++ side: ``tools/gen_kinds.py`` writes the ``EntityKind`` enum and
the kind table (``entity_kind.h``) from it. Run it after changing a row.
"""

from dataclasses import dataclass
from enum import Enum, IntEnum

from .models import ESP_TM2


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


class KindId(IntEnum):
    VALVE = 1
    RUN_DURATION = 2
    PROGRAM_START = 3
    SEASONAL_ADJUST = 4
    ZONE_RUNNING = 5
    PROGRAM_RUNNING = 6
    REMAINING_TIME = 7
    ACTIVE_ZONE = 8
    QUEUED_ZONES = 9
    RESPONSE_TIME = 10
    LINK_ERRORS = 11
    WATERING = 12
    RAIN_SENSOR = 13
    IRRIGATION_ENABLED = 14
    CONNECTED = 15
    ACTIVE_PROGRAM = 16
    QUEUE = 17
    MODEL = 18
    FIRMWARE_VERSION = 19
    SERIAL_NUMBER = 20
    SCHEDULE_CHANGED = 21
    QUEUE_ZONE = 22
    STOP = 23
    ADVANCE = 24
    TEST_ALL = 25
    SYNC_CLOCK = 26
    REFRESH = 27
    RAIN_DELAY = 28
    STATION_DELAY = 29
    TEST_DURATION = 30
    CYCLE_DAYS = 31
    DAYS_UNTIL_NEXT = 32
    RUN_TIME = 33
    IRRIGATION_SET = 34
    GLOBAL_BYPASS = 35
    WATERING_DAY = 36
    START_ENABLED = 37
    FREQUENCY = 38
    START_TIME = 39
    CONTROLLER_CLOCK = 40
    ENDS_AT = 41


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

# The select index is the wire byte: FREQ_CUSTOM to FREQ_EVEN in sip_commands.h.
FREQUENCY_OPTIONS = ("Custom days", "Cyclic", "Odd days", "Even days")

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
    limits: tuple[int, int, int] | None = None
    unit: str | None = None
    # Set when the hub key is not `<key>_<domain>` (start_time_switches).
    key_override: str | None = None
    # Entity defaults, as core components set them. Plain strings, so this
    # module stays importable without ESPHome.
    device_class: str | None = None
    state_class: str | None = None
    entity_category: str | None = None
    accuracy: int | None = None
    icon: str | None = None
    # Number display mode: "slider" or "box". None is ESPHome's default, auto.
    mode: str | None = None
    # What the kind shows and polls: a C++ `Field` name from controller_state.h. For a
    # schedule-page kind it is also the field a write edits. None: nothing from the controller.
    field: str | None = None
    # One line for the README's entity tables (tools/gen_docs.py).
    doc: str = ""


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
        field="ZONES_RUNNING",
        doc="On runs the zone for its run duration. Off stops all watering.",
    ),
    EntityKind(
        KindId.RUN_DURATION,
        "run_duration",
        "number",
        Scope.ZONE,
        "{zone} duration",
        default=True,
        write=Write.LOCAL,
        limits=(1, ESP_TM2.run_max_minutes, 1),
        unit="min",
        doc="Minutes the valve switch and queue button run the zone for.",
    ),
    EntityKind(
        KindId.PROGRAM_START,
        "start",
        "button",
        Scope.PROGRAM,
        "Run {program}",
        default=True,
        write=Write.COMMAND,
        doc="Runs the program now.",
    ),
    EntityKind(
        KindId.SEASONAL_ADJUST,
        "seasonal_adjust",
        "number",
        Scope.PROGRAM,
        "{program} seasonal adjust",
        default=True,
        write=Write.COMMAND,
        limits=(ESP_TM2.seasonal_min_percent, ESP_TM2.seasonal_max_percent, 1),
        unit="%",
        field="SEASONAL_ADJUST",
    ),
    # Read-only status. Zone and program rows are generated by the
    # hub's zones/programs like the rows above; controller rows are platform-only.
    EntityKind(
        KindId.ZONE_RUNNING,
        "running",
        "binary_sensor",
        Scope.ZONE,
        "{zone} running",
        device_class="running",
        field="ZONES_RUNNING",
        doc="The same state as the valve switch, read-only.",
    ),
    EntityKind(
        KindId.PROGRAM_RUNNING,
        "running",
        "binary_sensor",
        Scope.PROGRAM,
        "{program} running",
        device_class="running",
        field="ACTIVE_PROGRAM",
    ),
    EntityKind(
        KindId.REMAINING_TIME,
        "remaining_time",
        "sensor",
        Scope.CONTROLLER,
        "Time remaining",
        unit="s",
        device_class="duration",
        state_class="measurement",
        accuracy=0,
        icon="mdi:timer-sand",
        field="REMAINING_TIME",
        doc="Counts down every second between polls.",
    ),
    EntityKind(
        KindId.ACTIVE_ZONE,
        "active_zone",
        "sensor",
        Scope.CONTROLLER,
        "Active zone",
        accuracy=0,
        icon="mdi:sprinkler-variant",
        field="ACTIVE_ZONE",
        doc="0 when nothing is running.",
    ),
    EntityKind(
        KindId.QUEUED_ZONES,
        "queued_zones",
        "sensor",
        Scope.CONTROLLER,
        "Zones queued",
        accuracy=0,
        icon="mdi:format-list-numbered",
        field="QUEUED_COUNT",
    ),
    EntityKind(
        KindId.RESPONSE_TIME,
        "response_time",
        "sensor",
        Scope.CONTROLLER,
        "Controller response time",
        unit="ms",
        device_class="duration",
        state_class="measurement",
        entity_category="diagnostic",
        accuracy=0,
    ),
    EntityKind(
        KindId.LINK_ERRORS,
        "link_errors",
        "sensor",
        Scope.CONTROLLER,
        "Controller link errors",
        state_class="total_increasing",
        entity_category="diagnostic",
        accuracy=0,
        icon="mdi:alert-circle-outline",
        doc="Requests that timed out, retries included.",
    ),
    EntityKind(
        KindId.WATERING,
        "watering",
        "binary_sensor",
        Scope.CONTROLLER,
        "Watering",
        device_class="running",
        field="ZONES_RUNNING",
    ),
    EntityKind(
        KindId.RAIN_SENSOR,
        "rain_sensor",
        "binary_sensor",
        Scope.CONTROLLER,
        "Rain sensor",
        device_class="moisture",
        field="RAIN_SENSOR",
        doc="Polarity is unconfirmed; add `filters: [invert]` if it reads backwards.",
    ),
    EntityKind(
        KindId.IRRIGATION_ENABLED,
        "irrigation_enabled",
        "binary_sensor",
        Scope.CONTROLLER,
        "Irrigation enabled",
        icon="mdi:water-check",
        field="IRRIGATION_ENABLED",
    ),
    EntityKind(
        KindId.CONNECTED,
        "connected",
        "binary_sensor",
        Scope.CONTROLLER,
        "Controller connected",
        device_class="connectivity",
        entity_category="diagnostic",
    ),
    EntityKind(
        KindId.ACTIVE_PROGRAM,
        "active_program",
        "text_sensor",
        Scope.CONTROLLER,
        "Active program",
        icon="mdi:calendar-clock",
        field="ACTIVE_PROGRAM",
        doc="The program's name, `Manual` or `Idle`.",
    ),
    EntityKind(
        KindId.QUEUE,
        "queue",
        "text_sensor",
        Scope.CONTROLLER,
        "Queue",
        icon="mdi:format-list-bulleted",
        field="QUEUE",
        doc="For example `Front lawn 24:48, Back lawn 20:00`, or `Empty`.",
    ),
    EntityKind(
        KindId.MODEL,
        "model",
        "text_sensor",
        Scope.CONTROLLER,
        "Controller model",
        entity_category="diagnostic",
        icon="mdi:information-outline",
    ),
    EntityKind(
        KindId.FIRMWARE_VERSION,
        "firmware_version",
        "text_sensor",
        Scope.CONTROLLER,
        "Controller firmware",
        entity_category="diagnostic",
        icon="mdi:chip",
    ),
    EntityKind(
        KindId.SERIAL_NUMBER,
        "serial_number",
        "text_sensor",
        Scope.CONTROLLER,
        "Controller serial",
        entity_category="diagnostic",
        icon="mdi:barcode",
    ),
    # A text_sensor, not a sensor: a float sensor only holds an epoch to about
    # two minutes. ISO 8601 text with the timestamp device class is exact.
    EntityKind(
        KindId.SCHEDULE_CHANGED,
        "schedule_changed",
        "text_sensor",
        Scope.CONTROLLER,
        "Schedule last changed",
        device_class="timestamp",
        field="SCHEDULE_CHANGED",
        doc="Needs `time_id`.",
    ),
    # Opt-in controls and schedule rows (PLAN 4.2–4.4). None of these are
    # generated unless a zone, program or platform entry asks for them.
    EntityKind(
        KindId.QUEUE_ZONE,
        "queue",
        "button",
        Scope.ZONE,
        "Queue {zone}",
        write=Write.COMMAND,
        icon="mdi:playlist-plus",
        doc="Adds the zone to the queue for its run duration.",
    ),
    EntityKind(
        KindId.STOP,
        "stop",
        "button",
        Scope.CONTROLLER,
        "Stop watering",
        write=Write.COMMAND,
        icon="mdi:stop",
        doc="Stops all watering.",
    ),
    EntityKind(
        KindId.ADVANCE,
        "advance",
        "button",
        Scope.CONTROLLER,
        "Next zone",
        write=Write.COMMAND,
        icon="mdi:skip-next",
        field="QUEUE",
        doc="Moves on to the next queued zone. Needs `experimental: true`.",
    ),
    EntityKind(
        KindId.TEST_ALL,
        "test_all_zones",
        "button",
        Scope.CONTROLLER,
        "Test all zones",
        write=Write.COMMAND,
        entity_category="diagnostic",
        icon="mdi:test-tube",
        doc="Runs every zone for the test duration. Needs `experimental: true`.",
    ),
    EntityKind(
        KindId.SYNC_CLOCK,
        "sync_clock",
        "button",
        Scope.CONTROLLER,
        "Sync controller clock",
        write=Write.COMMAND,
        entity_category="config",
        icon="mdi:clock-check",
        doc="Needs `time_id`.",
    ),
    EntityKind(
        KindId.REFRESH,
        "refresh",
        "button",
        Scope.CONTROLLER,
        "Refresh from controller",
        write=Write.COMMAND,
        entity_category="diagnostic",
        icon="mdi:refresh",
        doc="Reads everything again, the schedule included.",
    ),
    EntityKind(
        KindId.RAIN_DELAY,
        "rain_delay",
        "number",
        Scope.CONTROLLER,
        "Rain delay",
        write=Write.COMMAND,
        limits=(0, ESP_TM2.rain_delay_max_days, 1),
        unit="d",
        entity_category="config",
        icon="mdi:weather-rainy",
        field="RAIN_DELAY",
    ),
    EntityKind(
        KindId.STATION_DELAY,
        "station_delay",
        "number",
        Scope.CONTROLLER,
        "Delay between zones",
        write=Write.SCHEDULE_PAGE,
        # The TM2 manual: "from 1 second to 9 hours". 0 is no delay. The page field is u16 seconds.
        limits=(0, 9 * 3600, 1),
        unit="s",
        entity_category="config",
        icon="mdi:timer-sand",
        mode="slider",
        field="STATION_DELAY",
    ),
    EntityKind(
        KindId.TEST_DURATION,
        "test_duration",
        "number",
        Scope.CONTROLLER,
        "Test duration",
        write=Write.LOCAL,
        limits=(1, ESP_TM2.run_max_minutes, 1),
        unit="min",
        icon="mdi:timer-outline",
        doc="Minutes per zone for the test button.",
    ),
    EntityKind(
        KindId.CYCLE_DAYS,
        "cycle_days",
        "number",
        Scope.PROGRAM,
        "{program} cycle days",
        write=Write.SCHEDULE_PAGE,
        limits=(0, 255, 1),
        unit="d",
        field="CYCLE_DAYS",
    ),
    EntityKind(
        KindId.DAYS_UNTIL_NEXT,
        "days_until_next",
        "number",
        Scope.PROGRAM,
        "{program} days until next run",
        write=Write.SCHEDULE_PAGE,
        limits=(0, 255, 1),
        unit="d",
        field="DAYS_UNTIL_NEXT",
    ),
    EntityKind(
        KindId.RUN_TIME,
        "run_time",
        "number",
        Scope.ZONE,
        "{zone} {program} run time",
        fan_out=FanOut.PROGRAM,
        write=Write.SCHEDULE_PAGE,
        limits=(0, 65535, 1),
        unit="min",
        key_override="run_time_numbers",
        field="RUN_TIME",
        doc="One per program; a platform entry also takes `program:`.",
    ),
    EntityKind(
        KindId.IRRIGATION_SET,
        "irrigation_enabled",
        "switch",
        Scope.CONTROLLER,
        "Irrigation enabled",
        write=Write.COMMAND,
        icon="mdi:water-check",
        field="IRRIGATION_ENABLED",
        doc="Needs `experimental: true`.",
    ),
    EntityKind(
        KindId.GLOBAL_BYPASS,
        "rain_sensor_bypass",
        "switch",
        Scope.CONTROLLER,
        "Ignore rain sensor",
        write=Write.SCHEDULE_PAGE,
        entity_category="config",
        icon="mdi:weather-rainy",
        field="GLOBAL_SENSOR_BYPASS",
    ),
    EntityKind(
        KindId.WATERING_DAY,
        "watering_day",
        "switch",
        Scope.PROGRAM,
        "{program} {day}",
        fan_out=FanOut.DAY,
        write=Write.SCHEDULE_PAGE,
        field="WATERING_DAY",
        doc="One per day; a platform entry also takes `day:`.",
    ),
    EntityKind(
        KindId.START_ENABLED,
        "start_time_enabled",
        "switch",
        Scope.PROGRAM,
        "{program} start {slot} enabled",
        fan_out=FanOut.SLOT,
        write=Write.SCHEDULE_PAGE,
        key_override="start_time_switches",
        field="START_ENABLED",
        doc="One per start slot; a platform entry also takes `slot:`. Turning one on writes the time its `start_time` shows.",
    ),
    EntityKind(
        KindId.FREQUENCY,
        "frequency",
        "select",
        Scope.PROGRAM,
        "{program} frequency",
        write=Write.SCHEDULE_PAGE,
        field="FREQUENCY",
        doc="`Custom days`, `Cyclic`, `Odd days` or `Even days`.",
    ),
    EntityKind(
        KindId.START_TIME,
        "start_time",
        "datetime",
        Scope.PROGRAM,
        "{program} start {slot}",
        fan_out=FanOut.SLOT,
        write=Write.SCHEDULE_PAGE,
        field="START_TIME",
        doc="One per start slot; a platform entry also takes `slot:`.",
    ),
    # When the time remaining runs out, as one timestamp Home Assistant counts down to by
    # itself. remaining_time shows the same thing as a number that changes every second.
    EntityKind(
        KindId.ENDS_AT,
        "ends_at",
        "text_sensor",
        Scope.CONTROLLER,
        "Watering ends",
        device_class="timestamp",
        icon="mdi:timer-sand-complete",
        field="REMAINING_TIME",
        doc="When the time remaining runs out. Changes once per zone. Needs `time_id`.",
    ),
    EntityKind(
        KindId.CONTROLLER_CLOCK,
        "controller_clock",
        "datetime",
        Scope.CONTROLLER,
        "Controller clock",
        write=Write.COMMAND,
        entity_category="config",
        icon="mdi:clock-outline",
        field="CLOCK",
        doc="Setting it sets the controller's time and date.",
    ),
)

KINDS_BY_ID = {int(kind.kind_id): kind for kind in KINDS}


def kinds_for_domain(domain: str) -> tuple[EntityKind, ...]:
    return tuple(kind for kind in KINDS if kind.domain == domain)


def find_kind(domain: str, key: str, scope: Scope) -> EntityKind | None:
    """The kind a platform key means in a scope. `running` is a zone or a program kind."""
    for kind in KINDS:
        if kind.domain == domain and kind.key == key and kind.scope is scope:
            return kind
    return None


def platform_keys(domain: str) -> tuple[str, ...]:
    """The keys a `platform: rainbird` entry of this domain accepts, in table order."""
    keys: list[str] = []
    for kind in KINDS:
        if kind.domain == domain and kind.key not in keys:
            keys.append(kind.key)
    return tuple(keys)


def resolve_kind(domain: str, key: str, *, zone: bool, program: bool) -> EntityKind | None:
    """The kind a platform entry's key means, given whether it has zone: or program:."""
    if zone and (kind := find_kind(domain, key, Scope.ZONE)) is not None:
        return kind
    if program and (kind := find_kind(domain, key, Scope.PROGRAM)) is not None:
        return kind
    return find_kind(domain, key, Scope.CONTROLLER)
