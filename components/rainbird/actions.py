"""The hub's actions: rainbird.zone_start, rainbird.zone_queue and rainbird.send_raw."""

import esphome.automation as automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_DATA, CONF_DURATION, CONF_ID
from esphome.core import ID

from .classes import RainbirdComponent, RainbirdSendRawAction, RainbirdZone, RainbirdZoneAction
from .const import ACTION_SEND_RAW, ACTION_ZONE_QUEUE, ACTION_ZONE_START, CONF_ZONE


def _payload(value):
    if isinstance(value, list):
        data = cv.Schema([cv.hex_uint8_t])(value)
    else:
        raise cv.Invalid("data must be a list of bytes")
    if len(data) > 64:
        raise cv.Invalid("data is longer than 64 bytes")
    return data


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
    ACTION_ZONE_START,
    RainbirdZoneAction,
    _zone_action_schema(),
    synchronous=True,
)
async def zone_start_to_code(config, action_id, template_arg, args):
    return await _zone_action_to_code(config, action_id, template_arg, args, False)


@automation.register_action(
    ACTION_ZONE_QUEUE,
    RainbirdZoneAction,
    _zone_action_schema(),
    synchronous=True,
)
async def zone_queue_to_code(config, action_id, template_arg, args):
    return await _zone_action_to_code(config, action_id, template_arg, args, True)


@automation.register_action(
    ACTION_SEND_RAW,
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
