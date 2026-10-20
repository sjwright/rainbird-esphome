"""Entity schemas shared by the hub expander and the platform modules."""

from esphome.components import button, number, switch
import esphome.config_validation as cv
from esphome.const import (
    CONF_INITIAL_VALUE,
    CONF_RESTORE_VALUE,
    UNIT_MINUTE,
    UNIT_PERCENT,
)

from .entity_kinds import EntityKind, Write


def build_entity_schema(kind: EntityKind, cls):
    if kind.domain == "switch":
        # A valve is not a GPIO output, so inverted would swap the command.
        return switch.switch_schema(cls, block_inverted=True)
    if kind.domain == "button":
        return button.button_schema(cls)
    if kind.domain == "number":
        unit = UNIT_MINUTE if kind.unit == "min" else UNIT_PERCENT
        minimum, maximum, _step = kind.limits

        def _initial(config):
            if CONF_INITIAL_VALUE in config:
                value = config[CONF_INITIAL_VALUE]
                if value < minimum or value > maximum:
                    raise cv.Invalid(f"initial_value must be between {minimum} and {maximum}")
            return config

        schema = number.number_schema(cls, unit_of_measurement=unit)
        if kind.write is not Write.LOCAL:
            # A controller-backed number shows only what the controller reports.
            # initial_value or restore_value would show a value nobody read.
            return schema
        return cv.All(
            schema.extend(
                {
                    cv.Optional(CONF_INITIAL_VALUE): cv.float_,
                    cv.Optional(CONF_RESTORE_VALUE): cv.boolean,
                }
            ),
            _initial,
        )
    raise ValueError(f"No schema for domain {kind.domain}")
