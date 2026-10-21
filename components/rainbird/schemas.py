"""Entity schemas shared by the hub expander and the platform modules."""

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
import esphome.config_validation as cv
from esphome.const import (
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_INITIAL_VALUE,
    CONF_MODE,
    CONF_RESTORE_VALUE,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_SECOND,
)

from .entity_kinds import EntityKind, KindId, Write

_NUMBER_UNITS = {
    "min": UNIT_MINUTE,
    "%": UNIT_PERCENT,
    "s": UNIT_SECOND,
    "d": "d",
}


def _opt(value):
    """None in the kind table means "not set", which the schema helpers spell UNDEFINED."""
    return cv.UNDEFINED if value is None else value


def build_entity_schema(kind: EntityKind, cls):
    if kind.domain == "switch":
        # A valve is not a GPIO output, so inverted would swap the command.
        return switch.switch_schema(
            cls,
            block_inverted=True,
            icon=_opt(kind.icon),
            entity_category=_opt(kind.entity_category),
        )
    if kind.domain == "button":
        return button.button_schema(
            cls, icon=_opt(kind.icon), entity_category=_opt(kind.entity_category)
        )
    if kind.domain == "sensor":
        return sensor.sensor_schema(
            cls,
            unit_of_measurement=_opt(kind.unit),
            icon=_opt(kind.icon),
            accuracy_decimals=_opt(kind.accuracy),
            device_class=_opt(kind.device_class),
            state_class=_opt(kind.state_class),
            entity_category=_opt(kind.entity_category),
        )
    if kind.domain == "binary_sensor":
        return binary_sensor.binary_sensor_schema(
            cls,
            icon=_opt(kind.icon),
            entity_category=_opt(kind.entity_category),
            device_class=_opt(kind.device_class),
        )
    if kind.domain == "text_sensor":
        return text_sensor.text_sensor_schema(
            cls,
            device_class=_opt(kind.device_class),
            entity_category=_opt(kind.entity_category),
            icon=_opt(kind.icon),
        )
    if kind.domain == "number":
        unit = _NUMBER_UNITS.get(kind.unit)
        if unit is None:
            raise ValueError(f"No unit for {kind.key}")
        minimum, maximum, _step = kind.limits

        def _initial(config):
            if CONF_INITIAL_VALUE in config:
                value = config[CONF_INITIAL_VALUE]
                if value < minimum or value > maximum:
                    raise cv.Invalid(f"initial_value must be between {minimum} and {maximum}")
            return config

        schema = number.number_schema(
            cls,
            unit_of_measurement=unit,
            icon=_opt(kind.icon),
            entity_category=_opt(kind.entity_category),
        )
        if kind.mode is not None:
            schema = schema.extend(
                {
                    cv.Optional(CONF_MODE, default=kind.mode.upper()): cv.enum(
                        number.NUMBER_MODES, upper=True
                    )
                }
            )
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
    if kind.domain == "select":
        return select.select_schema(
            cls, icon=_opt(kind.icon), entity_category=_opt(kind.entity_category)
        )
    if kind.domain == "datetime":
        if kind.kind_id == KindId.START_TIME:
            schema = datetime.time_schema(cls)
        else:
            schema = datetime.datetime_schema(cls)
        # The datetime helpers take no icon or category, so set the defaults the way they would.
        defaults = {}
        if kind.icon is not None:
            defaults[cv.Optional(CONF_ICON, default=kind.icon)] = cv.icon
        if kind.entity_category is not None:
            defaults[cv.Optional(CONF_ENTITY_CATEGORY, default=kind.entity_category)] = (
                cv.entity_category
            )
        return schema.extend(defaults)
    raise ValueError(f"No schema for domain {kind.domain}")
