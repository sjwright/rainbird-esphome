import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_NAME

from .. import (
    CONF_PROGRAM,
    CONF_RAINBIRD_ID,
    CONF_ZONE,
    RainbirdComponent,
    RainbirdNumber,
    build_entity,
    program_letter,
)
from ..entity_kinds import KINDS_BY_KEY, NO_PROGRAM, NO_ZONE
from ..schemas import build_entity_schema
from ..wire import validate_entity_platform

RUN_DURATION = KINDS_BY_KEY["run_duration"]
SEASONAL = KINDS_BY_KEY["seasonal_adjust"]


def _one_number(config):
    has_duration = "run_duration" in config
    has_seasonal = "seasonal_adjust" in config
    if has_duration == has_seasonal:
        raise cv.Invalid("Specify exactly one of run_duration or seasonal_adjust")
    if has_duration and CONF_ZONE not in config:
        raise cv.Invalid("run_duration needs zone")
    if has_seasonal and CONF_PROGRAM not in config:
        raise cv.Invalid("seasonal_adjust needs program")
    # Reject a key the entity would ignore rather than build something else silently.
    if has_duration and CONF_PROGRAM in config:
        raise cv.Invalid("run_duration is per zone. Remove program.")
    if has_seasonal and CONF_ZONE in config:
        raise cv.Invalid("seasonal_adjust is per program. Remove zone.")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_RAINBIRD_ID): cv.use_id(RainbirdComponent),
            cv.Optional(CONF_ZONE): cv.int_range(min=1, max=22),
            cv.Optional(CONF_PROGRAM): program_letter,
            cv.Optional("run_duration"): cv.maybe_simple_value(
                build_entity_schema(RUN_DURATION, RainbirdNumber), key=CONF_NAME
            ),
            cv.Optional("seasonal_adjust"): cv.maybe_simple_value(
                build_entity_schema(SEASONAL, RainbirdNumber), key=CONF_NAME
            ),
        }
    ),
    _one_number,
)

FINAL_VALIDATE_SCHEMA = validate_entity_platform("number")


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RAINBIRD_ID])
    if "run_duration" in config:
        await build_entity(
            parent,
            RUN_DURATION,
            config["run_duration"],
            config[CONF_ZONE],
            NO_PROGRAM,
            0,
        )
        return
    await build_entity(
        parent,
        SEASONAL,
        config["seasonal_adjust"],
        NO_ZONE,
        "ABCD".index(config[CONF_PROGRAM]),
        0,
    )
