"""The hub's configuration schema, and the entities its zones and programs generate.

ESPHome validates each domain as soon as that domain is loaded, so generated
entities cannot be appended to ``switch:`` and still be checked. They are
validated here with the same schema the platform modules use, then built from
the hub. Hand-written ``platform: rainbird`` blocks are what ``packages``,
``!extend`` and ``!remove`` edit.
"""

import esphome.automation as automation
from esphome.components import time as time_comp
from esphome.components.remote_base import (
    CONF_RECEIVER_ID,
    CONF_TRANSMITTER_ID,
    RemoteReceiverBase,
    RemoteTransmitterBase,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODEL, CONF_NAME, CONF_TIME_ID, CONF_TRIGGER_ID

from .classes import FrameTrigger, NakTrigger, RainbirdComponent, RainbirdProgram, RainbirdZone
from .const import (
    CONF_ACCEPT_MODEL_IDS,
    CONF_ALLOW_WRITES,
    CONF_CLOCK_SYNC,
    CONF_DISABLED,
    CONF_EXPERIMENTAL,
    CONF_GENERATED,
    CONF_LISTEN_ONLY,
    CONF_MAX_DRIFT,
    CONF_MONITOR_RECEIVER_ID,
    CONF_ON_FRAME,
    CONF_ON_NAK,
    CONF_PROGRAM,
    CONF_PROGRAM_DEFAULTS,
    CONF_PROGRAMS,
    CONF_REFRESH_INTERVAL,
    CONF_SCHEDULE,
    CONF_VALVE_SWITCH_MODE,
    CONF_ZONE,
    CONF_ZONE_DEFAULTS,
    CONF_ZONES,
    GENERATED_CONFIG,
    GENERATED_INDEX,
    GENERATED_KIND,
)
from .errors import ConfigError
from .expand import expand
from .models import ANY_MODEL_LETTERS, resolve_model
from .schemas import entity_schema


def program_letter(value):
    """Any letter some model uses. The hub's model narrows it in final validation."""
    text = cv.string(value).upper()
    # Exact match: `in` on a string is a substring test, so "AB" or "" would pass.
    if len(text) != 1 or text not in ANY_MODEL_LETTERS:
        letters = f"{ANY_MODEL_LETTERS[0]}–{ANY_MODEL_LETTERS[-1]}"
        raise cv.Invalid(f"Program {text} is not a program letter ({letters})")
    return text


def _mapping_or_list(value):
    if isinstance(value, (dict, list)):
        return value
    raise cv.Invalid("Expected a list or a mapping")


def _optional_mapping(value):
    # `zone_defaults:` with no value arrives as None. That is an empty mapping:
    # no implicit entities, only keys a zone sets itself.
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise cv.Invalid("Expected a mapping")
    return value


def _bare_mapping(schema):
    """Accept `key:` with no value as an empty mapping, like zone_defaults."""

    def validator(value):
        return schema({} if value is None else value)

    return validator


# Both clocks read in whole seconds, and a set lands a fraction of a second late, so a
# limit under a few seconds would set the clock on every reading.
MIN_MAX_DRIFT = cv.TimePeriod(seconds=5)

CLOCK_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MAX_DRIFT, default="30s"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(min=MIN_MAX_DRIFT)
        ),
    }
)
# Each refresh reads every schedule page in use, up to 14, ahead of status polls.
# More often than this would crowd the status polls out.
MIN_REFRESH_INTERVAL = cv.TimePeriod(minutes=1)

SCHEDULE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_REFRESH_INTERVAL, default="1h"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(min=MIN_REFRESH_INTERVAL)
        ),
        cv.Optional(CONF_ALLOW_WRITES, default=False): cv.boolean,
    }
)
ZONE_DECL_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ID): cv.declare_id(RainbirdZone),
        cv.Required(CONF_ZONE): cv.int_,
        cv.Required(CONF_NAME): cv.string,
        cv.Optional(CONF_DISABLED, default=False): cv.boolean,
    }
)
PROGRAM_DECL_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ID): cv.declare_id(RainbirdProgram),
        cv.Required(CONF_PROGRAM): cv.string,
        cv.Required(CONF_NAME): cv.string,
    }
)


def _assign_hub_id(config) -> str:
    """Omitted hub ids are ``rainbird``, then ``rainbird_2``.

    Zone and program ids are ``<hub id>_zone<N>``. Those strings are config
    API, so they stay on a fixed prefix instead of ESPHome's type name.
    """
    ident = config[CONF_ID]
    if ident.id is not None:
        return ident.id
    from esphome.config import path_context

    path = path_context.get()
    index = path[-1] if path and isinstance(path[-1], int) else 0
    ident.id = "rainbird" if index == 0 else f"rainbird_{index + 1}"
    return ident.id


def _apply_wire_ids(config):
    # GenerateID auto-picks the only transmitter. listen_only must not require one.
    extra = {}
    if not config.get(CONF_LISTEN_ONLY) and CONF_TRANSMITTER_ID not in config:
        extra[cv.GenerateID(CONF_TRANSMITTER_ID)] = cv.use_id(RemoteTransmitterBase)
    if CONF_RECEIVER_ID not in config:
        extra[cv.GenerateID(CONF_RECEIVER_ID)] = cv.use_id(RemoteReceiverBase)
    if not extra:
        return config
    return cv.Schema(extra, extra=cv.ALLOW_EXTRA)(config)


def _validate(config):
    # Clock sync writes the controller's clock, so it runs only when the block is
    # present. time_id alone is also used to convert schedule timestamps.
    if CONF_CLOCK_SYNC in config and CONF_TIME_ID not in config:
        raise cv.Invalid("clock_sync needs time_id")
    if CONF_SCHEDULE not in config:
        config[CONF_SCHEDULE] = SCHEDULE_SCHEMA({})

    hub_id = _assign_hub_id(config)
    try:
        model = resolve_model(config[CONF_MODEL])
        expansion = expand(
            model,
            hub_id,
            zones=config.get(CONF_ZONES),
            programs=config.get(CONF_PROGRAMS),
            zone_defaults=config.get(CONF_ZONE_DEFAULTS),
            program_defaults=config.get(CONF_PROGRAM_DEFAULTS),
            zone_defaults_set=CONF_ZONE_DEFAULTS in config,
            program_defaults_set=CONF_PROGRAM_DEFAULTS in config,
        )
    except ConfigError as err:
        raise cv.Invalid(err.message) from err

    config[CONF_MODEL] = model.name
    config.pop(CONF_ZONE_DEFAULTS, None)
    config.pop(CONF_PROGRAM_DEFAULTS, None)
    config[CONF_ZONES] = [
        ZONE_DECL_SCHEMA(
            {
                CONF_ID: zone.id,
                CONF_ZONE: zone.number,
                CONF_NAME: zone.name,
                CONF_DISABLED: zone.disabled,
            }
        )
        for zone in expansion.zones
    ]
    config[CONF_PROGRAMS] = [
        PROGRAM_DECL_SCHEMA(
            {
                CONF_ID: program.id,
                CONF_PROGRAM: program.letter,
                CONF_NAME: program.name,
            }
        )
        for program in expansion.programs
    ]

    generated = []
    for entity in expansion.entities:
        try:
            validated = entity_schema(entity.kind)(entity.options)
        except cv.Invalid as err:
            raise cv.Invalid(f"{entity.id}: {err}") from err
        generated.append(
            {
                GENERATED_KIND: int(entity.kind.kind_id),
                CONF_ZONE: entity.zone,
                CONF_PROGRAM: entity.program,
                GENERATED_INDEX: entity.index,
                GENERATED_CONFIG: validated,
            }
        )
    # Kept in the config, not CORE.data: ESPHome's ID pass walks the config, and it is what
    # names each generated entity's id and lets automations refer to it.
    config[CONF_GENERATED] = generated
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RainbirdComponent),
            cv.Required(CONF_MODEL): cv.string,
            cv.Optional(CONF_LISTEN_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_TRANSMITTER_ID): cv.use_id(RemoteTransmitterBase),
            cv.Optional(CONF_RECEIVER_ID): cv.use_id(RemoteReceiverBase),
            cv.Optional(CONF_MONITOR_RECEIVER_ID): cv.use_id(RemoteReceiverBase),
            cv.Optional(CONF_ACCEPT_MODEL_IDS): cv.ensure_list(cv.hex_uint16_t),
            cv.Optional(CONF_VALVE_SWITCH_MODE, default="start"): cv.one_of(
                "start", "queue", lower=True
            ),
            cv.Optional(CONF_TIME_ID): cv.use_id(time_comp.RealTimeClock),
            cv.Optional(CONF_CLOCK_SYNC): _bare_mapping(CLOCK_SCHEMA),
            cv.Optional(CONF_SCHEDULE): _bare_mapping(SCHEDULE_SCHEMA),
            cv.Optional(CONF_EXPERIMENTAL, default=False): cv.boolean,
            cv.Optional(CONF_ZONE_DEFAULTS): _optional_mapping,
            cv.Optional(CONF_ZONES): _mapping_or_list,
            cv.Optional(CONF_PROGRAM_DEFAULTS): _optional_mapping,
            cv.Optional(CONF_PROGRAMS): _mapping_or_list,
            cv.Optional(CONF_ON_NAK): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(NakTrigger)}
            ),
            cv.Optional(CONF_ON_FRAME): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FrameTrigger)}
            ),
        }
    ).extend(cv.polling_component_schema("15s")),
    _apply_wire_ids,
    _validate,
)
