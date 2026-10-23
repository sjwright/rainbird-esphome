"""Rain Bird ESP-TM2 hub. The schema is in hub_schema.py and the code generation in codegen.py."""

from . import actions  # noqa: F401  registers the hub's actions
from .classes import RainbirdComponent  # noqa: F401  the hub class, for cv.use_id elsewhere
from .codegen import hub_to_code
from .hub_schema import CONFIG_SCHEMA  # noqa: F401
from .wire import validate_hub

CODEOWNERS = ["@sjwright"]
MULTI_CONF = True
DEPENDENCIES = ["remote_receiver"]
# The domains the hub can generate from zones/programs. sensor and text_sensor
# entities are only hand-written, so those domains load with their platforms.
AUTO_LOAD = ["switch", "number", "button", "binary_sensor", "select", "datetime"]

FINAL_VALIDATE_SCHEMA = validate_hub


async def to_code(config):
    await hub_to_code(config)
