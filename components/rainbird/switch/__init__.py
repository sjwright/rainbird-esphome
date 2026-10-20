import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_NAME

from .. import CONF_RAINBIRD_ID, CONF_ZONE, RainbirdComponent, RainbirdSwitch, build_entity
from ..entity_kinds import KINDS_BY_KEY, NO_PROGRAM
from ..schemas import build_entity_schema
from ..wire import validate_entity_platform

VALVE = KINDS_BY_KEY["valve"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RAINBIRD_ID): cv.use_id(RainbirdComponent),
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=22),
        cv.Required("valve"): cv.maybe_simple_value(
            build_entity_schema(VALVE, RainbirdSwitch), key=CONF_NAME
        ),
    }
)

# Model limits need the hub config, which is only available in final validation.
FINAL_VALIDATE_SCHEMA = validate_entity_platform


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RAINBIRD_ID])
    await build_entity(parent, VALVE, config["valve"], config[CONF_ZONE], NO_PROGRAM, 0)
