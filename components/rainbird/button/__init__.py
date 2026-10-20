import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_NAME

from .. import (
    CONF_PROGRAM,
    CONF_RAINBIRD_ID,
    RainbirdButton,
    RainbirdComponent,
    build_entity,
    program_letter,
)
from ..entity_kinds import KINDS_BY_KEY, NO_ZONE
from ..schemas import build_entity_schema
from ..wire import validate_entity_platform

START = KINDS_BY_KEY["start"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RAINBIRD_ID): cv.use_id(RainbirdComponent),
        cv.Required(CONF_PROGRAM): program_letter,
        cv.Required("start"): cv.maybe_simple_value(
            build_entity_schema(START, RainbirdButton), key=CONF_NAME
        ),
    }
)

FINAL_VALIDATE_SCHEMA = validate_entity_platform("button")


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RAINBIRD_ID])
    await build_entity(
        parent,
        START,
        config["start"],
        NO_ZONE,
        "ABCD".index(config[CONF_PROGRAM]),
        0,
    )
