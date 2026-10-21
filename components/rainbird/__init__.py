"""Hub schema, entity generation and codegen.

ESPHome validates each domain as soon as that domain is loaded, so generated
entities cannot be appended to ``switch:`` and still be checked. They are
validated here with the same schema the platform modules use, then built from
the hub. Hand-written ``platform: rainbird`` blocks are what ``packages``,
``!extend`` and ``!remove`` edit.
"""

import esphome.automation as automation
import esphome.codegen as cg

# Aliased so the rainbird platform modules don't replace these when they load.
import esphome.components.binary_sensor as binary_sensor_comp
import esphome.components.button as button_comp
import esphome.components.datetime as datetime_comp
import esphome.components.number as number_comp
from esphome.components.remote_base import (
    CONF_RECEIVER_ID,
    CONF_TRANSMITTER_ID,
    RemoteReceiverBase,
    RemoteReceiverListener,
    RemoteTransmitterBase,
)
import esphome.components.select as select_comp
import esphome.components.sensor as sensor_comp
import esphome.components.switch as switch_comp
import esphome.components.text_sensor as text_sensor_comp
import esphome.components.time as time_comp
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA,
    CONF_DURATION,
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_MODEL,
    CONF_NAME,
    CONF_NUMBER,
    CONF_PIN,
    CONF_RESTORE_VALUE,
    CONF_TIME_ID,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE, ID

from .entity_kinds import FREQUENCY_OPTIONS, KINDS_BY_ID, EntityKind, KindId, Write
from .errors import ConfigError
from .expand import expand
from .models import resolve_model
from .schemas import build_entity_schema
from .wire import validate_hub

DOMAIN = "rainbird"
CODEOWNERS = ["@sjwright"]
MULTI_CONF = True
DEPENDENCIES = ["remote_receiver"]
# The domains the hub can generate from zones/programs. sensor and text_sensor
# entities are only hand-written, so those domains load with their platforms.
AUTO_LOAD = ["switch", "number", "button", "binary_sensor", "select", "datetime"]

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
RainbirdSelect = rainbird_ns.class_(
    "RainbirdSelect", select_comp.Select, cg.Parented.template(RainbirdComponent)
)
RainbirdTime = rainbird_ns.class_(
    "RainbirdTime", datetime_comp.TimeEntity, cg.Parented.template(RainbirdComponent)
)
RainbirdDateTime = rainbird_ns.class_(
    "RainbirdDateTime", datetime_comp.DateTimeEntity, cg.Parented.template(RainbirdComponent)
)
RainbirdZoneAction = rainbird_ns.class_(
    "RainbirdZoneAction", automation.Action, cg.Parented.template(RainbirdComponent)
)
RainbirdReceiver = rainbird_ns.class_("RainbirdReceiver", RemoteReceiverListener)
RainbirdSendRawAction = rainbird_ns.class_(
    "RainbirdSendRawAction", automation.Action, cg.Parented.template(RainbirdComponent)
)
NakTrigger = rainbird_ns.class_("NakTrigger", automation.Trigger.template(cg.uint8, cg.uint8))
FrameTrigger = rainbird_ns.class_(
    "FrameTrigger", automation.Trigger.template(cg.std_vector.template(cg.uint8))
)

# Read-only kinds use the base classes; the hub publishes to them.
_DOMAIN_CLASS = {
    "switch": RainbirdSwitch,
    "number": RainbirdNumber,
    "button": RainbirdButton,
    "select": RainbirdSelect,
    "binary_sensor": binary_sensor_comp.BinarySensor,
    "sensor": sensor_comp.Sensor,
    "text_sensor": text_sensor_comp.TextSensor,
}


def entity_schema(kind: EntityKind):
    if kind.kind_id == KindId.START_TIME:
        cls = RainbirdTime
    elif kind.kind_id == KindId.CONTROLLER_CLOCK:
        cls = RainbirdDateTime
    else:
        cls = _DOMAIN_CLASS[kind.domain]
    return build_entity_schema(kind, cls)


def program_letter(value):
    """A–D covers every model in the registry. The hub's model narrows it later."""
    text = cv.string(value).upper()
    # Exact match: `in` on a string is a substring test, so "AB" or "" would pass.
    if len(text) != 1 or text not in "ABCD":
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
    elif kind.domain == "select":
        var = await select_comp.new_select(entity_config, options=list(FREQUENCY_OPTIONS))
    elif kind.domain == "datetime":
        var = await datetime_comp.new_datetime(entity_config)
    elif kind.domain in ("binary_sensor", "sensor", "text_sensor"):
        # Read-only: the hub publishes to the base class, so there is no entity class
        # and no parent pointer, only a binding the hub reads when state changes.
        builder = {
            "binary_sensor": binary_sensor_comp.new_binary_sensor,
            "sensor": sensor_comp.new_sensor,
            "text_sensor": text_sensor_comp.new_text_sensor,
        }[kind.domain]
        var = await builder(entity_config)
        cg.add(getattr(parent, f"add_{kind.domain}")(var, int(kind.kind_id), zone, program, index))
        return var
    else:
        raise ValueError(f"No builder for {kind.domain}")
    await cg.register_parented(var, parent)
    cg.add(var.set_binding(int(kind.kind_id), zone, program, index))
    if kind.domain in ("switch", "number"):
        # Controller-backed switches and numbers also show what the controller reports.
        cg.add(getattr(parent, f"add_{kind.domain}")(var))
    elif kind.domain == "button":
        # Buttons show nothing, but some need fields polled: advance reads the queue list.
        cg.add(parent.add_button(var))
    elif kind.domain == "select":
        cg.add(parent.add_select(var))
    elif kind.kind_id == KindId.START_TIME:
        cg.add(parent.add_time(var))
    elif kind.kind_id == KindId.CONTROLLER_CLOCK:
        cg.add(parent.add_datetime(var))
    return var


def _payload(value):
    if isinstance(value, list):
        data = cv.Schema([cv.hex_uint8_t])(value)
    else:
        raise cv.Invalid("data must be a list of bytes")
    if len(data) > 64:
        raise cv.Invalid("data is longer than 64 bytes")
    return data


async def _add_listener(
    parent, parent_id: str, receiver_id, suffix: str, name: str, controller: bool
):
    listener = cg.new_Pvariable(
        ID(f"{parent_id}_{suffix}", is_declaration=True, type=RainbirdReceiver)
    )
    cg.add(listener.set_parent(parent))
    cg.add(listener.set_name(name))
    # Only the receiver on the controller's TX line feeds state; the monitor just logs.
    cg.add(listener.set_controller(controller))
    receiver = await cg.get_variable(receiver_id)
    cg.add(receiver.register_listener(listener))


def _zone_action_schema():
    return cv.Schema(
        {
            cv.GenerateID(): cv.use_id(RainbirdComponent),
            cv.Required(CONF_ZONE): cv.Any(
                cv.use_id(RainbirdZone), cv.templatable(cv.int_range(min=1, max=22))
            ),
            cv.Required(CONF_DURATION): cv.templatable(cv.positive_time_period_milliseconds),
        }
    )


async def _zone_action_to_code(config, action_id, template_arg, args, queue: bool):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_queue(queue))
    zone = config[CONF_ZONE]
    if isinstance(zone, ID):
        cg.add(var.set_zone_entity(await cg.get_variable(zone)))
    else:
        # int32, so the hub can refuse a template's out-of-range zone instead of wrapping it.
        cg.add(var.set_zone(await cg.templatable(zone, args, cg.int32)))
    cg.add(var.set_duration_ms(await cg.templatable(config[CONF_DURATION], args, cg.uint32)))
    return var


@automation.register_action(
    "rainbird.zone.start",
    RainbirdZoneAction,
    _zone_action_schema(),
    synchronous=True,
)
async def zone_start_to_code(config, action_id, template_arg, args):
    return await _zone_action_to_code(config, action_id, template_arg, args, False)


@automation.register_action(
    "rainbird.zone.queue",
    RainbirdZoneAction,
    _zone_action_schema(),
    synchronous=True,
)
async def zone_queue_to_code(config, action_id, template_arg, args):
    return await _zone_action_to_code(config, action_id, template_arg, args, True)


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


def _receiver_gpio(receiver_id):
    """GPIO number for a remote_receiver id, if this config declares it."""
    wanted = getattr(receiver_id, "id", receiver_id)
    for conf in CORE.config.get("remote_receiver", []):
        found = conf.get(CONF_ID)
        if getattr(found, "id", found) != wanted:
            continue
        pin = conf.get(CONF_PIN)
        if isinstance(pin, dict):
            return pin.get(CONF_NUMBER)
        return pin
    return None


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_model_name(config[CONF_MODEL]))
    cg.add(var.set_listen_only(config[CONF_LISTEN_ONLY]))
    cg.add(var.set_experimental(config[CONF_EXPERIMENTAL]))
    cg.add(var.set_valve_queue(config[CONF_VALVE_SWITCH_MODE] == "queue"))
    cg.add(var.set_allow_writes(config[CONF_SCHEDULE][CONF_ALLOW_WRITES]))
    if CONF_CLOCK_SYNC in config:
        cg.add(var.set_clock_sync(config[CONF_CLOCK_SYNC][CONF_MAX_DRIFT]))
    cg.add(var.set_refresh_interval(config[CONF_SCHEDULE][CONF_REFRESH_INTERVAL]))
    if not config[CONF_LISTEN_ONLY]:
        cg.add(var.set_transmitter(await cg.get_variable(config[CONF_TRANSMITTER_ID])))
    cg.add(var.set_receiver(await cg.get_variable(config[CONF_RECEIVER_ID])))
    if (pin := _receiver_gpio(config[CONF_RECEIVER_ID])) is not None:
        cg.add(var.add_receiver_pin(pin))
    await _add_listener(var, config[CONF_ID].id, config[CONF_RECEIVER_ID], "rx", "Receiver", True)
    if CONF_MONITOR_RECEIVER_ID in config:
        cg.add(var.set_monitor_receiver(await cg.get_variable(config[CONF_MONITOR_RECEIVER_ID])))
        if (pin := _receiver_gpio(config[CONF_MONITOR_RECEIVER_ID])) is not None:
            cg.add(var.add_receiver_pin(pin))
        await _add_listener(
            var, config[CONF_ID].id, config[CONF_MONITOR_RECEIVER_ID], "monitor", "Monitor", False
        )
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    for model_id in config.get(CONF_ACCEPT_MODEL_IDS, []):
        cg.add(var.add_accept_model_id(model_id))
    for zone in config[CONF_ZONES]:
        zone_var = cg.new_Pvariable(zone[CONF_ID])
        cg.add(zone_var.set_zone(zone[CONF_ZONE]))
        cg.add(zone_var.set_name(zone[CONF_NAME]))
        cg.add(var.add_zone(zone_var))
    for program in config[CONF_PROGRAMS]:
        program_var = cg.new_Pvariable(program[CONF_ID])
        cg.add(program_var.set_program("ABCD".index(program[CONF_PROGRAM])))
        cg.add(program_var.set_name(program[CONF_NAME]))
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
