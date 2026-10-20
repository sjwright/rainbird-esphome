"""`binary_sensor: - platform: rainbird`. Generated from the kind table; see platform.py."""

from ..platform import platform_schema, platform_to_code
from ..wire import validate_entity_platform

CONFIG_SCHEMA = platform_schema("binary_sensor")

# Model limits and duplicates need the hub config, which final validation has.
FINAL_VALIDATE_SCHEMA = validate_entity_platform("binary_sensor")


async def to_code(config):
    await platform_to_code("binary_sensor", config)
