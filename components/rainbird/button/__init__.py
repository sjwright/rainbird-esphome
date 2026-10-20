"""`button: - platform: rainbird`. Generated from the kind table; see platform.py."""

from ..platform import platform_schema, platform_to_code
from ..wire import validate_entity_platform

CONFIG_SCHEMA = platform_schema("button")

# Model limits and duplicates need the hub config, which final validation has.
FINAL_VALIDATE_SCHEMA = validate_entity_platform("button")


async def to_code(config):
    await platform_to_code("button", config)
