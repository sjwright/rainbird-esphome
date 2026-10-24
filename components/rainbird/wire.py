"""Final validation for the wire layer (PLAN.md 6.1) and model limits."""

from esphome.components import esp32
from esphome.components.remote_base import CONF_RECEIVER_ID, CONF_TRANSMITTER_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PLATFORM, CONF_RMT_SYMBOLS, CONF_TIME_ID
from esphome.core import CORE
import esphome.final_validate as fv

from .const import (
    ACTION_SEND_RAW,
    CONF_DAY,
    CONF_EXPERIMENTAL,
    CONF_GENERATED,
    CONF_LISTEN_ONLY,
    CONF_MONITOR_RECEIVER_ID,
    CONF_PROGRAM,
    CONF_RAINBIRD_ID,
    CONF_SLOT,
    CONF_ZONE,
    GENERATED_CONFIG,
    GENERATED_INDEX,
    GENERATED_KIND,
)
from .entity_kinds import (
    KINDS_BY_ID,
    NO_PROGRAM,
    NO_ZONE,
    FanOut,
    KindId,
    Scope,
    Write,
    platform_keys,
    resolve_kind,
)
from .models import ESP_TM2

CONF_RECEIVE_SYMBOLS = "receive_symbols"
CONF_NON_BLOCKING = "non_blocking"
CONF_CARRIER_DUTY_PERCENT = "carrier_duty_percent"
CONF_IDLE = "idle"
CONF_FILTER = "filter"

# A 42-byte reply is up to 336 RMT symbols. 384 leaves a little spare.
MIN_RECEIVE_SYMBOLS = 384
# Original ESP32 cannot refill the receiver mid-frame, so the hardware block
# itself has to hold the reply. Ping-pong chips keep a smaller block.
ORIGINAL_RX_SYMBOLS = 384
MIN_IDLE_US = 1_000
MAX_IDLE_US = 20_000
MAX_FILTER_US = 100

# (memory blocks, blocks a receiver can use, symbols per block), from ESP-IDF
# soc_caps.h. ESP-IDF rounds rmt_symbols up to whole blocks per channel. A
# receiver's blocks must come from the receive channels, which are the last
# ones in the group. A transmitter can use any free block. receive_symbols is
# the RAM buffer, not this.
_RMT_LAYOUT = {
    esp32.VARIANT_ESP32: (8, 8, 64),
    esp32.VARIANT_ESP32S3: (8, 4, 48),
    esp32.VARIANT_ESP32C3: (4, 2, 48),
    esp32.VARIANT_ESP32C5: (4, 2, 48),
    esp32.VARIANT_ESP32C6: (4, 2, 48),
    esp32.VARIANT_ESP32H2: (4, 2, 48),
    esp32.VARIANT_ESP32P4: (8, 4, 48),
}
CONF_USE_DMA = "use_dma"

_SUPPORTED = "ESP32, S3, C3, C5, C6, H2 or P4"
_UNSUPPORTED = {
    esp32.VARIANT_ESP32S2: (
        "ESP32-S2 has 256 RMT symbols, which cannot hold a 42-byte reply "
        f"(336 symbols). Use an {_SUPPORTED}."
    ),
    esp32.VARIANT_ESP32C2: (f"ESP32-C2 has no RMT peripheral. Use an {_SUPPORTED}."),
    esp32.VARIANT_ESP32C61: (f"ESP32-C61 has no RMT peripheral. Use an {_SUPPORTED}."),
    esp32.VARIANT_ESP32H4: (
        f"ESP32-H4 has no usable RMT in the current ESP-IDF. Use an {_SUPPORTED}."
    ),
    esp32.VARIANT_ESP32H21: (
        f"ESP32-H21 has no usable RMT in the current ESP-IDF. Use an {_SUPPORTED}."
    ),
}


def _label(config: dict, fallback: str) -> str:
    ident = config.get(CONF_ID)
    name = getattr(ident, "id", None)
    return name or fallback


def _declaring(ident):
    full = fv.full_config.get()
    path = full.get_path_for_id(ident)[:-1]
    return full.get_config_for_path(path)


def _check_receiver(rx: dict, variant: str | None) -> None:
    name = _label(rx, "remote_receiver")
    if CORE.is_esp32:
        symbols = rx.get(CONF_RECEIVE_SYMBOLS)
        if symbols is None or symbols < MIN_RECEIVE_SYMBOLS:
            raise cv.Invalid(
                f"remote_receiver '{name}' has receive_symbols {symbols}. "
                f"Set receive_symbols: {MIN_RECEIVE_SYMBOLS} so a 42-byte reply "
                "(up to 336 symbols) fits."
            )
        if variant == esp32.VARIANT_ESP32:
            block = rx.get(CONF_RMT_SYMBOLS, 0)
            if block < ORIGINAL_RX_SYMBOLS:
                raise cv.Invalid(
                    f"remote_receiver '{name}' has rmt_symbols {block}. The original "
                    "ESP32 does not refill RMT memory mid-frame, so set "
                    f"rmt_symbols: {ORIGINAL_RX_SYMBOLS}. Leave the transmitter at 64."
                )
    idle = rx.get(CONF_IDLE)
    if idle is not None:
        idle_us = idle.total_microseconds
        if idle_us < MIN_IDLE_US or idle_us > MAX_IDLE_US:
            raise cv.Invalid(
                f"remote_receiver '{name}' has idle {idle}. Set idle between 1ms and "
                "20ms (4ms is a good value): longer than a 416µs bit and shorter than "
                "the controller's ~30ms turnaround."
            )
    filt = rx.get(CONF_FILTER)
    if filt is not None and filt.total_microseconds > MAX_FILTER_US:
        raise cv.Invalid(
            f"remote_receiver '{name}' has filter {filt}. Set filter: 50us. "
            "A filter above 100us eats the 208µs half-bits."
        )


def _check_transmitter(tx: dict) -> None:
    name = _label(tx, "remote_transmitter")
    duty = tx.get(CONF_CARRIER_DUTY_PERCENT)
    if duty is not None and int(duty) != 100:
        raise cv.Invalid(
            f"remote_transmitter '{name}' has carrier_duty_percent {duty}%. "
            "Set carrier_duty_percent: 100% (Rain Bird is baseband, no carrier)."
        )
    if tx.get(CONF_NON_BLOCKING) is False:
        raise cv.Invalid(
            f"remote_transmitter '{name}' has non_blocking: false. "
            "Set non_blocking: true so transmitting does not block the main loop."
        )


def _rmt_users(full) -> list[tuple[str, int, bool, bool]]:
    """(name, rmt_symbols, is_receiver, uses_dma) for every RMT channel in the config."""
    users: list[tuple[str, int, bool, bool]] = []
    for domain in ("remote_transmitter", "remote_receiver"):
        entries = full.get(domain) or []
        if isinstance(entries, dict):
            entries = [entries]
        for entry in entries:
            if isinstance(entry, dict) and CONF_RMT_SYMBOLS in entry:
                users.append(
                    (
                        f"{domain} {_label(entry, domain)}",
                        entry[CONF_RMT_SYMBOLS],
                        domain == "remote_receiver",
                        bool(entry.get(CONF_USE_DMA)),
                    )
                )
    lights = full.get("light") or []
    if isinstance(lights, dict):
        lights = [lights]
    for entry in lights:
        if not isinstance(entry, dict):
            continue
        if entry.get(CONF_PLATFORM) != "esp32_rmt_led_strip":
            continue
        if CONF_RMT_SYMBOLS not in entry:
            continue
        users.append(
            (
                f"light {_label(entry, 'light')}",
                entry[CONF_RMT_SYMBOLS],
                False,
                bool(entry.get(CONF_USE_DMA)),
            )
        )
    return users


def _check_rmt_memory(variant: str) -> None:
    """Count whole blocks the way ESP-IDF allocates them, not raw symbols."""
    blocks, rx_blocks, per_block = _RMT_LAYOUT[variant]
    users = _rmt_users(fv.full_config.get())

    def need(symbols: int, dma: bool) -> int:
        # A DMA channel always takes one block; otherwise rmt_symbols rounds up.
        return 1 if dma else -(-symbols // per_block)

    for name, symbols, is_rx, dma in users:
        if is_rx and need(symbols, dma) > rx_blocks:
            raise cv.Invalid(
                f"RMT symbol memory: {name} has rmt_symbols {symbols}, which needs "
                f"{need(symbols, dma)} blocks of {per_block}. A receiver on this chip can use at "
                f"most {rx_blocks} blocks ({rx_blocks * per_block} symbols). Lower rmt_symbols; "
                "receive_symbols is the RAM buffer and can stay at 384."
            )
    used_rx = sum(need(s, d) for _, s, rx, d in users if rx)
    used = sum(need(s, d) for _, s, _, d in users)
    parts = ", ".join(f"{name} {need(s, d)}" for name, s, _, d in users)
    if used_rx > rx_blocks:
        raise cv.Invalid(
            f"RMT symbol memory: receivers need {used_rx} blocks of {per_block} symbols, "
            f"and this chip has {rx_blocks} for receiving ({parts}). Lower rmt_symbols "
            f"(each {per_block} symbols is one block) or remove a receiver."
        )
    if used > blocks:
        raise cv.Invalid(
            f"RMT symbol memory on this chip is {blocks} blocks of {per_block} symbols. "
            f"This config needs {used} ({parts}). ESP-IDF rounds rmt_symbols up to whole "
            "blocks, so lower rmt_symbols. On the original ESP32, receive 384 plus "
            "transmit 64 fits."
        )


def _collect_send_raw(node, found: list) -> None:
    if isinstance(node, dict):
        action = node.get(ACTION_SEND_RAW)
        if isinstance(action, dict):
            found.append(action)
        for value in node.values():
            _collect_send_raw(value, found)
    elif isinstance(node, list):
        for item in node:
            _collect_send_raw(item, found)


def _check_send_raw(config: dict) -> None:
    """send_raw skips the model gate, so the config has to opt in."""
    hub_id = config.get(CONF_ID)
    found: list = []
    _collect_send_raw(fv.full_config.get(), found)
    for action in found:
        if action.get(CONF_ID) != hub_id:
            continue
        if config.get(CONF_LISTEN_ONLY):
            raise cv.Invalid(
                "rainbird.send_raw needs a transmitter. listen_only does not have one."
            )
        if not config.get(CONF_EXPERIMENTAL):
            raise cv.Invalid("rainbird.send_raw needs experimental: true on the rainbird hub.")


def validate_hub(config: dict) -> None:
    """Check the referenced transmitter and receiver against the wire rules."""
    variant = None
    if CORE.is_esp32:
        variant = esp32.get_esp32_variant()
        if variant in _UNSUPPORTED:
            raise cv.Invalid(_UNSUPPORTED[variant])
        if variant not in _RMT_LAYOUT:
            raise cv.Invalid(
                f"{variant} is not supported. Rain Bird needs an ESP32 with RMT "
                f"and room for a 42-byte reply. Use an {_SUPPORTED}."
            )

    seen = set()
    for key in (CONF_RECEIVER_ID, CONF_MONITOR_RECEIVER_ID):
        if key not in config:
            continue
        ident = config[key]
        if ident in seen:
            continue
        seen.add(ident)
        _check_receiver(_declaring(ident), variant)

    if not config.get(CONF_LISTEN_ONLY) and CONF_TRANSMITTER_ID in config:
        _check_transmitter(_declaring(config[CONF_TRANSMITTER_ID]))
    _check_send_raw(config)
    _check_dependencies(config)

    if variant is None:
        return
    _check_rmt_memory(variant)


def _key(kind_id: KindId) -> str:
    return KINDS_BY_ID[int(kind_id)].key


def _generated_binding(item: dict) -> tuple[int, int, int, int]:
    return (item[GENERATED_KIND], item[CONF_ZONE], item[CONF_PROGRAM], item[GENERATED_INDEX])


def _entry_bindings(entry: dict, domain: str, model) -> list[tuple[tuple[int, int, int, int], str]]:
    """(binding, label) for each control a hand-written platform entry creates.

    Only controls count: two switches for one valve is a mistake, while a
    second read-only sensor for the same value is harmless.
    """
    out = []
    zone = entry.get(CONF_ZONE)
    program = entry.get(CONF_PROGRAM)
    program_index = NO_PROGRAM
    if isinstance(program, str) and len(program) == 1 and program.upper() in model.letters:
        program_index = model.letters.index(program.upper())
    for key in platform_keys(domain):
        if key not in entry:
            continue
        kind = resolve_kind(domain, key, zone=zone is not None, program=program is not None)
        if kind is None or kind.write is Write.NONE:
            continue
        index = 0
        if kind.fan_out is FanOut.DAY:
            index = entry.get(CONF_DAY, 0)
        elif kind.fan_out is FanOut.SLOT and isinstance(entry.get(CONF_SLOT), int):
            index = entry[CONF_SLOT] - 1
        uses_program = kind.scope is Scope.PROGRAM or kind.fan_out is FanOut.PROGRAM
        binding = (
            int(kind.kind_id),
            zone if kind.scope is Scope.ZONE else NO_ZONE,
            program_index if uses_program else NO_PROGRAM,
            index,
        )
        where = f"zone {zone}" if kind.scope is Scope.ZONE else f"program {program}"
        if kind.fan_out is FanOut.SLOT:
            where += f" slot {entry.get(CONF_SLOT)}"
        elif kind.fan_out is FanOut.DAY:
            where += f" day {index}"
        out.append((binding, f"{domain} {kind.key} for {where}"))
    return out


# The domains whose platform entries can hold controls.
_CONTROL_DOMAINS = ("switch", "number", "button", "select", "datetime")


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
    mine = _entry_bindings(config, domain, model)
    if not mine:
        return
    counts: dict[tuple[int, int, int, int], int] = {}
    generated_ids: dict[tuple[int, int, int, int], str] = {}
    for item in hub.get(CONF_GENERATED, []):
        binding = _generated_binding(item)
        counts[binding] = counts.get(binding, 0) + 1
        generated_ids[binding] = str(item[GENERATED_CONFIG][CONF_ID])
    for entry_domain, entry in _platform_entries(config[CONF_RAINBIRD_ID]):
        for binding, _ in _entry_bindings(entry, entry_domain, model):
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


def _check_dependencies(hub: dict) -> None:
    """Controls that use another entity's value need that entity on the same hub.

    Without it the control only logs a warning when used, so the config fails here instead.
    """
    model = ESP_TM2
    bindings = {_generated_binding(item) for item in hub.get(CONF_GENERATED, [])}
    for domain, entry in _platform_entries(hub[CONF_ID]):
        bindings.update(binding for binding, _ in _entry_bindings(entry, domain, model))
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
    """Final validator for a platform module: model limits and duplicate entities."""

    def validator(config: dict) -> None:
        _validate_entity_platform(config, domain)

    return validator


def _validate_entity_platform(config: dict, domain: str) -> None:
    hub = _declaring(config[CONF_RAINBIRD_ID])
    if (
        domain == "text_sensor"
        and _key(KindId.SCHEDULE_CHANGED) in config
        and CONF_TIME_ID not in hub
    ):
        raise cv.Invalid(
            "schedule_changed needs time_id on the rainbird hub: the controller stamps it in "
            "local time, and the time zone converts it."
        )
    if domain == "button" and _key(KindId.SYNC_CLOCK) in config and CONF_TIME_ID not in hub:
        raise cv.Invalid(
            "sync_clock needs time_id on the rainbird hub: it sets the controller to that clock's local time."
        )
    _check_duplicates(config, domain, hub, ESP_TM2)
