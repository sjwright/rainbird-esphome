"""Final validation for the wire layer (PLAN.md 6.1) and model limits."""

from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PLATFORM, CONF_RMT_SYMBOLS
from esphome.core import CORE
import esphome.final_validate as fv

from .models import MODELS

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

# channels * words per channel, from ESP-IDF soc_caps. This is the hardware
# pool mem_block_symbols is allocated from. receive_symbols is RAM, not this.
_RMT_SYMBOLS = {
    esp32.VARIANT_ESP32: 8 * 64,
    esp32.VARIANT_ESP32S3: 8 * 48,
    esp32.VARIANT_ESP32C3: 4 * 48,
    esp32.VARIANT_ESP32C5: 4 * 48,
    esp32.VARIANT_ESP32C6: 4 * 48,
    esp32.VARIANT_ESP32H2: 4 * 48,
    esp32.VARIANT_ESP32P4: 8 * 48,
}

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


def _rmt_users(full) -> list[tuple[str, int]]:
    users: list[tuple[str, int]] = []
    for domain in ("remote_transmitter", "remote_receiver"):
        entries = full.get(domain) or []
        if isinstance(entries, dict):
            entries = [entries]
        for entry in entries:
            if isinstance(entry, dict) and CONF_RMT_SYMBOLS in entry:
                users.append((f"{domain} {_label(entry, domain)}", entry[CONF_RMT_SYMBOLS]))
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
        users.append((f"light {_label(entry, 'light')}", entry[CONF_RMT_SYMBOLS]))
    return users


def _collect_send_raw(node, found: list) -> None:
    if isinstance(node, dict):
        action = node.get("rainbird.send_raw")
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
        if config.get("listen_only"):
            raise cv.Invalid(
                "rainbird.send_raw needs a transmitter. listen_only does not have one."
            )
        if not config.get("experimental"):
            raise cv.Invalid("rainbird.send_raw needs experimental: true on the rainbird hub.")


def validate_hub(config: dict) -> None:
    """Check the referenced transmitter and receiver against the wire rules."""
    variant = None
    if CORE.is_esp32:
        variant = esp32.get_esp32_variant()
        if variant in _UNSUPPORTED:
            raise cv.Invalid(_UNSUPPORTED[variant])
        if variant not in _RMT_SYMBOLS:
            raise cv.Invalid(
                f"{variant} is not supported. Rain Bird needs an ESP32 with RMT "
                f"and room for a 42-byte reply. Use an {_SUPPORTED}."
            )

    seen = set()
    for key in ("receiver_id", "monitor_receiver_id"):
        if key not in config:
            continue
        ident = config[key]
        if ident in seen:
            continue
        seen.add(ident)
        _check_receiver(_declaring(ident), variant)

    if not config.get("listen_only") and "transmitter_id" in config:
        _check_transmitter(_declaring(config["transmitter_id"]))
    _check_send_raw(config)

    if variant is None:
        return
    users = _rmt_users(fv.full_config.get())
    total = sum(symbols for _, symbols in users)
    limit = _RMT_SYMBOLS[variant]
    if total > limit:
        parts = ", ".join(f"{name} {symbols}" for name, symbols in users)
        raise cv.Invalid(
            f"RMT symbol memory on this chip is {limit}. This config uses {total} "
            f"({parts}). Lower rmt_symbols so the total is {limit} or less. "
            "receive_symbols is the RAM buffer and can stay at 384; rmt_symbols "
            "is the hardware block. On the original ESP32, receive 384 plus "
            "transmit 64 fits."
        )


def validate_entity_platform(config: dict) -> None:
    """Reject a hand-written zone or program the selected model does not have."""
    hub = _declaring(config["rainbird_id"])
    model = MODELS[str(hub["model"]).upper()]
    zone = config.get("zone")
    program = config.get("program")
    if isinstance(program, str):
        program = program.upper()
    if zone is not None and not 1 <= zone <= model.zones:
        raise cv.Invalid(f"Zone {zone} is outside 1–{model.zones} for {model.name}")
    if program is not None and program not in model.letters:
        raise cv.Invalid(
            f"Program {program} is not valid for {model.name} (programs {', '.join(model.letters)})"
        )
