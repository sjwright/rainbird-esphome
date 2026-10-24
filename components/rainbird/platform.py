"""Every `platform: rainbird` module, generated from the kind table (PLAN 5.3).

An entry names the hub, optionally a zone or a program, and one or more
entity keys:

    binary_sensor:
      - platform: rainbird
        watering:
          name: Watering
      - platform: rainbird
        zone: 1
        running:
          name: Front lawn running

A key means a zone, program or controller entity depending on which of
`zone:` and `program:` the entry has. This is the same thing the hub's
`zones:` and `programs:` generate, written by hand.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_NAME

from .classes import RainbirdComponent
from .codegen import build_entity
from .const import CONF_DAY, CONF_PROGRAM, CONF_RAINBIRD_ID, CONF_SLOT, CONF_ZONE
from .entity_kinds import (
    DAYS,
    KINDS,
    NO_PROGRAM,
    NO_ZONE,
    FanOut,
    Scope,
    platform_keys,
    resolve_kind,
)
from .hub_schema import program_letter
from .models import ESP_TM2, program_index
from .schemas import entity_schema


def _day_index(value):
    text = cv.string(value).lower()
    for index, day in enumerate(DAYS):
        if day.lower() == text:
            return index
    names = ", ".join(DAYS)
    raise cv.Invalid(f"day must be one of {names}")


def _resolve(domain: str, key: str, config: dict):
    return resolve_kind(domain, key, zone=CONF_ZONE in config, program=CONF_PROGRAM in config)


def _check_scopes(domain: str):
    def validator(config):
        keys = [key for key in platform_keys(domain) if key in config]
        if not keys:
            raise cv.Invalid(f"Add one of: {', '.join(platform_keys(domain))}")
        uses_zone = uses_program = False
        needs_day = needs_slot = False
        for key in keys:
            kind = _resolve(domain, key, config)
            if kind is None:
                scopes = {k.scope for k in KINDS if k.domain == domain and k.key == key}
                need = "zone" if Scope.ZONE in scopes else "program"
                raise cv.Invalid(f"{key} needs {need}")
            uses_zone |= kind.scope is Scope.ZONE
            # Run times are zone entities, one per program, so program: belongs on the entry.
            uses_program |= kind.scope is Scope.PROGRAM or kind.fan_out is FanOut.PROGRAM
            needs_day |= kind.fan_out is FanOut.DAY
            needs_slot |= kind.fan_out is FanOut.SLOT
        # Reject a selector nothing uses rather than ignore it silently.
        if CONF_ZONE in config and not uses_zone:
            raise cv.Invalid(f"zone is not used by {', '.join(keys)}. Remove zone.")
        if CONF_PROGRAM in config and not uses_program:
            raise cv.Invalid(f"program is not used by {', '.join(keys)}. Remove program.")
        if uses_program and CONF_PROGRAM not in config:
            raise cv.Invalid(f"{', '.join(keys)} needs program")
        if needs_day and CONF_DAY not in config:
            raise cv.Invalid(f"{', '.join(keys)} needs day")
        if CONF_DAY in config and not needs_day:
            raise cv.Invalid(f"day is not used by {', '.join(keys)}. Remove day.")
        if needs_slot and CONF_SLOT not in config:
            raise cv.Invalid(f"{', '.join(keys)} needs slot")
        if CONF_SLOT in config and not needs_slot:
            raise cv.Invalid(f"slot is not used by {', '.join(keys)}. Remove slot.")
        return config

    return validator


def platform_schema(domain: str):
    fields = {
        cv.GenerateID(CONF_RAINBIRD_ID): cv.use_id(RainbirdComponent),
        cv.Optional(CONF_ZONE): cv.int_range(min=1, max=ESP_TM2.zones),
        cv.Optional(CONF_PROGRAM): program_letter,
        cv.Optional(CONF_DAY): _day_index,
        cv.Optional(CONF_SLOT): cv.int_range(min=1, max=ESP_TM2.starts),
    }
    for key in platform_keys(domain):
        # Kinds sharing a key (zone and program `running`) share an entity schema.
        kind = next(k for k in KINDS if k.domain == domain and k.key == key)
        fields[cv.Optional(key)] = cv.maybe_simple_value(entity_schema(kind), key=CONF_NAME)
    return cv.All(cv.Schema(fields), _check_scopes(domain))


async def platform_to_code(domain: str, config: dict) -> None:
    parent = await cg.get_variable(config[CONF_RAINBIRD_ID])
    for key in platform_keys(domain):
        if key not in config:
            continue
        kind = _resolve(domain, key, config)
        zone = config[CONF_ZONE] if kind.scope is Scope.ZONE else NO_ZONE
        if kind.scope is Scope.PROGRAM or kind.fan_out is FanOut.PROGRAM:
            program = program_index(config[CONF_PROGRAM])
        else:
            program = NO_PROGRAM
        if kind.fan_out is FanOut.DAY:
            index = config[CONF_DAY]
        elif kind.fan_out is FanOut.SLOT:
            index = config[CONF_SLOT] - 1
        else:
            index = 0
        await build_entity(parent, kind, config[key], zone, program, index)
