"""The C++ classes codegen names, and which class each entity kind uses."""

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
from esphome.components.remote_base import RemoteReceiverListener

from .entity_kinds import EntityKind, KindId, Write

rainbird_ns = cg.esphome_ns.namespace("rainbird")
RainbirdComponent = rainbird_ns.class_("RainbirdComponent", cg.PollingComponent)
RainbirdZone = rainbird_ns.class_("RainbirdZone")
RainbirdProgram = rainbird_ns.class_("RainbirdProgram")
RainbirdSwitch = rainbird_ns.class_(
    "RainbirdSwitch", switch.Switch, cg.Parented.template(RainbirdComponent)
)
RainbirdNumber = rainbird_ns.class_(
    "RainbirdNumber", number.Number, cg.Parented.template(RainbirdComponent)
)
# A local setting restores its value in setup(), so it is a component.
RainbirdSetting = rainbird_ns.class_("RainbirdSetting", number.Number, cg.Component)
RainbirdButton = rainbird_ns.class_(
    "RainbirdButton", button.Button, cg.Parented.template(RainbirdComponent)
)
RainbirdSelect = rainbird_ns.class_(
    "RainbirdSelect", select.Select, cg.Parented.template(RainbirdComponent)
)
RainbirdTime = rainbird_ns.class_(
    "RainbirdTime", datetime.TimeEntity, cg.Parented.template(RainbirdComponent)
)
RainbirdDateTime = rainbird_ns.class_(
    "RainbirdDateTime", datetime.DateTimeEntity, cg.Parented.template(RainbirdComponent)
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
    "binary_sensor": binary_sensor.BinarySensor,
    "sensor": sensor.Sensor,
    "text_sensor": text_sensor.TextSensor,
}


def entity_class(kind: EntityKind):
    if kind.kind_id == KindId.START_TIME:
        return RainbirdTime
    if kind.kind_id == KindId.CONTROLLER_CLOCK:
        return RainbirdDateTime
    if kind.domain == "number" and kind.write is Write.LOCAL:
        return RainbirdSetting
    return _DOMAIN_CLASS[kind.domain]
