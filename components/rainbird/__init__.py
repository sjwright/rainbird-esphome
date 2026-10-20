"""Hub schema, entity generation and codegen.

ESPHome validates each domain as soon as that domain is loaded, so generated
entities cannot be appended to ``switch:`` and still be checked. They are
validated here with the same schema the platform modules use, then built from
the hub. Hand-written ``platform: rainbird`` blocks are what ``packages``,
``!extend`` and ``!remove`` edit.
"""

import esphome.automation as automation
import esphome.codegen as cg

# Aliased so the switch/number/button platforms don't replace these when they load.
import esphome.components.button as button_comp
import esphome.components.number as number_comp
from esphome.components.remote_base import (
    CONF_RECEIVER_ID,
    CONF_TRANSMITTER_ID,
    RemoteReceiverBase,
    RemoteReceiverListener,
    RemoteTransmitterBase,
)
import esphome.components.switch as switch_comp
import esphome.components.time as time_comp
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA,
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_MODEL,
    CONF_NAME,
    CONF_RESTORE_VALUE,
    CONF_TIME_ID,
    CONF_TRIGGER_ID,
)
from esphome.core import ID

from .entity_kinds import KINDS_BY_ID, EntityKind, Write
from .errors import ConfigError
from .expand import expand
from .models import resolve_model
from .schemas import build_entity_schema
from .wire import validate_hub

DOMAIN = "rainbird"
CODEOWNERS = ["@sjwright"]
MULTI_CONF = True
DEPENDENCIES = ["remote_receiver"]
# Later steps add binary_sensor, select and datetime as those rows appear.
AUTO_LOAD = ["switch", "number", "button"]

CONF_RAINBIRD_ID = "rainbird_id"
CONF_LISTEN_ONLY = "listen_only"
CONF_MONITOR_RECEIVER_ID = "monitor_receiver_id"
CONF_ACCEPT_MODEL_IDS = "accept_model_ids"
CONF_VALVE_SWITCH_MODE = "valve_switch_mode"
CONF_CLOCK_SYNC = "clock_sync"
CONF_MAX_DRIFT = "max_drift"
CONF_SCHEDULE = "schedule"
CONF_REFRESH_INTERVAL = "refresh_interval"
CONF_ALLOW_WRITES = "allow_writes"
CONF_EXPERIMENTAL = "experimental"
CONF_ZONE_DEFAULTS = "zone_defaults"
CONF_ZONES = "zones"
CONF_PROGRAM_DEFAULTS = "program_defaults"
CONF_PROGRAMS = "programs"
CONF_ZONE = "zone"
CONF_PROGRAM = "program"
CONF_DISABLED = "disabled"
CONF_ON_NAK = "on_nak"
CONF_ON_FRAME = "on_frame"
CONF_GENERATED = "generated"

rainbird_ns = cg.esphome_ns.namespace("rainbird")
RainbirdComponent = rainbird_ns.class_("RainbirdComponent", cg.PollingComponent)
RainbirdZone = rainbird_ns.class_("RainbirdZone")
RainbirdProgram = rainbird_ns.class_("RainbirdProgram")
RainbirdSwitch = rainbird_ns.class_(
    "RainbirdSwitch", switch_comp.Switch, cg.Component, cg.Parented.template(RainbirdComponent)
)
RainbirdNumber = rainbird_ns.class_(
    "RainbirdNumber", number_comp.Number, cg.Component, cg.Parented.template(RainbirdComponent)
)
RainbirdButton = rainbird_ns.class_(
    "RainbirdButton", button_comp.Button, cg.Parented.template(RainbirdComponent)
)
RainbirdReceiver = rainbird_ns.class_("RainbirdReceiver", RemoteReceiverListener)
RainbirdSendRawAction = rainbird_ns.class_(
    "RainbirdSendRawAction", automation.Action, cg.Parented.template(RainbirdComponent)
)
NakTrigger = rainbird_ns.class_("NakTrigger", automation.Trigger.template(cg.uint8, cg.uint8))
FrameTrigger = rainbird_ns.class_(
    "FrameTrigger", automation.Trigger.template(cg.std_vector.template(cg.uint8))
)

_DOMAIN_CLASS = {
    "switch": RainbirdSwitch,
    "number": RainbirdNumber,
    "button": RainbirdButton,
}


def entity_schema(kind: EntityKind):
    return build_entity_schema(kind, _DOMAIN_CLASS[kind.domain])


def program_letter(value):
    """A–D covers every model in the registry. The hub's model narrows it later."""
    text = cv.string(value).upper()
    if text not in "ABCD":
        raise cv.Invalid(f"Program {text} is not a program letter (A–D)")
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


CLOCK_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MAX_DRIFT, default="30s"): cv.positive_time_period_milliseconds,
    }
)
SCHEDULE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_REFRESH_INTERVAL, default="1h"): cv.positive_time_period_milliseconds,
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
    clock_set = CONF_CLOCK_SYNC in config
    if not clock_set:
        config[CONF_CLOCK_SYNC] = CLOCK_SCHEMA({})
    elif CONF_TIME_ID not in config:
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
                "kind_id": int(entity.kind.kind_id),
                "zone": entity.zone,
                "program": entity.program,
                "index": entity.index,
                "config": validated,
            }
        )
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
            cv.Optional(CONF_ACCEPT_MODEL_IDS): cv.ensure_list(cv.hex_int),
            cv.Optional(CONF_VALVE_SWITCH_MODE, default="start"): cv.one_of(
                "start", "queue", lower=True
            ),
            cv.Optional(CONF_TIME_ID): cv.use_id(time_comp.RealTimeClock),
            cv.Optional(CONF_CLOCK_SYNC): CLOCK_SCHEMA,
            cv.Optional(CONF_SCHEDULE): SCHEDULE_SCHEMA,
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

FINAL_VALIDATE_SCHEMA = validate_hub


async def build_entity(parent, kind: EntityKind, entity_config, zone, program, index):
    if kind.domain == "switch":
        var = await switch_comp.new_switch(entity_config)
        await cg.register_component(var, entity_config)
    elif kind.domain == "number":
        minimum, maximum, step = kind.limits
        var = await number_comp.new_number(
            entity_config, min_value=minimum, max_value=maximum, step=step
        )
        await cg.register_component(var, entity_config)
        if CONF_INITIAL_VALUE in entity_config:
            cg.add(var.set_initial_value(entity_config[CONF_INITIAL_VALUE]))
        if CONF_RESTORE_VALUE in entity_config:
            cg.add(var.set_restore_value(entity_config[CONF_RESTORE_VALUE]))
        cg.add(var.set_command(kind.write != Write.LOCAL))
    elif kind.domain == "button":
        var = await button_comp.new_button(entity_config)
    else:
        raise ValueError(f"No builder for {kind.domain}")
    await cg.register_parented(var, parent)
    cg.add(var.set_binding(int(kind.kind_id), zone, program, index))
    return var


def _payload(value):
    if isinstance(value, list):
        data = cv.Schema([cv.hex_uint8_t])(value)
    else:
        raise cv.Invalid("data must be a list of bytes")
    if len(data) > 64:
        raise cv.Invalid("data is longer than 64 bytes")
    return data


async def _add_listener(parent, parent_id: str, receiver_id, suffix: str, name: str):
    listener = cg.new_Pvariable(
        ID(f"{parent_id}_{suffix}", is_declaration=True, type=RainbirdReceiver)
    )
    cg.add(listener.set_parent(parent))
    cg.add(listener.set_name(name))
    receiver = await cg.get_variable(receiver_id)
    cg.add(receiver.register_listener(listener))


@automation.register_action(
    "rainbird.send_raw",
    RainbirdSendRawAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(RainbirdComponent),
            cv.Required(CONF_DATA): cv.templatable(_payload),
        },
        key=CONF_DATA,
    ),
    synchronous=True,
)
async def send_raw_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    data = config[CONF_DATA]
    if cg.is_template(data):
        templ = await cg.templatable(data, args, cg.std_vector.template(cg.uint8))
        cg.add(var.set_data_template(templ))
    else:
        arr_id = ID(f"{action_id}_data", is_declaration=True, type=cg.uint8)
        arr = cg.static_const_array(arr_id, cg.ArrayInitializer(*data))
        cg.add(var.set_data_static(arr, len(data)))
    return var


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_model_name(config[CONF_MODEL]))
    cg.add(var.set_listen_only(config[CONF_LISTEN_ONLY]))
    cg.add(var.set_experimental(config[CONF_EXPERIMENTAL]))
    cg.add(var.set_valve_queue(config[CONF_VALVE_SWITCH_MODE] == "queue"))
    cg.add(var.set_allow_writes(config[CONF_SCHEDULE][CONF_ALLOW_WRITES]))
    cg.add(var.set_max_drift(config[CONF_CLOCK_SYNC][CONF_MAX_DRIFT]))
    cg.add(var.set_refresh_interval(config[CONF_SCHEDULE][CONF_REFRESH_INTERVAL]))
    if not config[CONF_LISTEN_ONLY]:
        cg.add(var.set_transmitter(await cg.get_variable(config[CONF_TRANSMITTER_ID])))
    cg.add(var.set_receiver(await cg.get_variable(config[CONF_RECEIVER_ID])))
    await _add_listener(var, config[CONF_ID].id, config[CONF_RECEIVER_ID], "rx", "Receiver")
    if CONF_MONITOR_RECEIVER_ID in config:
        cg.add(var.set_monitor_receiver(await cg.get_variable(config[CONF_MONITOR_RECEIVER_ID])))
        await _add_listener(
            var, config[CONF_ID].id, config[CONF_MONITOR_RECEIVER_ID], "monitor", "Monitor"
        )
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    for model_id in config.get(CONF_ACCEPT_MODEL_IDS, []):
        cg.add(var.add_accept_model_id(model_id))
    for zone in config[CONF_ZONES]:
        zone_var = cg.new_Pvariable(zone[CONF_ID])
        cg.add(zone_var.set_zone(zone[CONF_ZONE]))
        cg.add(var.add_zone(zone_var))
    for program in config[CONF_PROGRAMS]:
        program_var = cg.new_Pvariable(program[CONF_ID])
        cg.add(program_var.set_program("ABCD".index(program[CONF_PROGRAM])))
        cg.add(var.add_program(program_var))
    for item in config[CONF_GENERATED]:
        kind = KINDS_BY_ID[item["kind_id"]]
        await build_entity(
            var,
            kind,
            item["config"],
            item["zone"],
            item["program"],
            item["index"],
        )
    for conf in config.get(CONF_ON_NAK, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        await automation.build_automation(
            trigger, [(cg.uint8, "command"), (cg.uint8, "code")], conf
        )
        cg.add(var.add_nak_trigger(trigger))
    for conf in config.get(CONF_ON_FRAME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        await automation.build_automation(
            trigger, [(cg.std_vector.template(cg.uint8), "payload")], conf
        )
        cg.add(var.add_frame_trigger(trigger))
