"""`datetime: - platform: rainbird`. Generated from the kind table; see platform.py."""

from ..entity_checks import validate_entity_platform
from ..platform import platform_schema, platform_to_code

CONFIG_SCHEMA = platform_schema("datetime")

# Model limits and duplicates need the hub config, which final validation has.
FINAL_VALIDATE_SCHEMA = validate_entity_platform("datetime")


async def to_code(config):
    await platform_to_code("datetime", config)
