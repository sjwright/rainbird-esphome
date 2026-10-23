"""Code generation for the hub and every entity it registers."""

import esphome.automation as automation
import esphome.codegen as cg
from esphome.components import (
    binary_sensor,
    button,
    datetime,
    number,
    select,
    sensor,
    switch,
    text_sensor,
)
from esphome.components.remote_base import CONF_RECEIVER_ID, CONF_TRANSMITTER_ID
from esphome.const import (
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

from .classes import RainbirdReceiver
from .const import (
    CONF_ACCEPT_MODEL_IDS,
    CONF_ALLOW_WRITES,
    CONF_CLOCK_SYNC,
    CONF_EXPERIMENTAL,
    CONF_GENERATED,
    CONF_LISTEN_ONLY,
    CONF_MAX_DRIFT,
    CONF_MONITOR_RECEIVER_ID,
    CONF_ON_FRAME,
    CONF_ON_NAK,
    CONF_PROGRAM,
    CONF_PROGRAMS,
    CONF_REFRESH_INTERVAL,
    CONF_SCHEDULE,
    CONF_VALVE_SWITCH_MODE,
    CONF_ZONE,
    CONF_ZONES,
    GENERATED_CONFIG,
    GENERATED_INDEX,
    GENERATED_KIND,
)
from .entity_kinds import FREQUENCY_OPTIONS, KINDS_BY_ID, EntityKind, KindId, Write
from .models import program_index


async def build_entity(parent, kind: EntityKind, entity_config, zone, program, index):
    if kind.domain == "number" and kind.write is Write.LOCAL:
        # A local setting: no parent and nothing polled. The hub indexes it for the commands that read it.
        minimum, maximum, step = kind.limits
        var = await number.new_number(
            entity_config, min_value=minimum, max_value=maximum, step=step
        )
        await cg.register_component(var, entity_config)
        if CONF_INITIAL_VALUE in entity_config:
            cg.add(var.set_initial_value(entity_config[CONF_INITIAL_VALUE]))
        if CONF_RESTORE_VALUE in entity_config:
            cg.add(var.set_restore_value(entity_config[CONF_RESTORE_VALUE]))
        cg.add(var.set_binding(int(kind.kind_id), zone, program, index))
        cg.add(parent.add_setting(var))
        return var
    if kind.domain == "switch":
        var = await switch.new_switch(entity_config)
    elif kind.domain == "number":
        minimum, maximum, step = kind.limits
        var = await number.new_number(
            entity_config, min_value=minimum, max_value=maximum, step=step
        )
    elif kind.domain == "button":
        var = await button.new_button(entity_config)
    elif kind.domain == "select":
        var = await select.new_select(entity_config, options=list(FREQUENCY_OPTIONS))
    elif kind.domain == "datetime":
        var = await datetime.new_datetime(entity_config)
    elif kind.domain in ("binary_sensor", "sensor", "text_sensor"):
        # Read-only: the hub publishes to the base class, so there is no entity class
        # and no parent pointer, only a binding the hub reads when state changes.
        builder = {
            "binary_sensor": binary_sensor.new_binary_sensor,
            "sensor": sensor.new_sensor,
            "text_sensor": text_sensor.new_text_sensor,
        }[kind.domain]
        var = await builder(entity_config)
        cg.add(getattr(parent, f"add_{kind.domain}")(var, int(kind.kind_id), zone, program, index))
        return var
    else:
        raise ValueError(f"No builder for {kind.domain}")
    await cg.register_parented(var, parent)
    cg.add(var.set_binding(int(kind.kind_id), zone, program, index))
    if kind.domain in ("switch", "number"):
        # Switches and numbers also show what the controller reports.
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


async def hub_to_code(config):
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
        cg.add(program_var.set_program(program_index(program[CONF_PROGRAM])))
        cg.add(program_var.set_name(program[CONF_NAME]))
        cg.add(var.add_program(program_var))
    for item in config[CONF_GENERATED]:
        await build_entity(
            var,
            KINDS_BY_ID[item[GENERATED_KIND]],
            item[GENERATED_CONFIG],
            item[CONF_ZONE],
            item[CONF_PROGRAM],
            item[GENERATED_INDEX],
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
