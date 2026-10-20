"""Config errors raised before ESPHome's validator is available.

The hub schema turns these into ``cv.Invalid``. Unit tests catch them directly,
so this module does not import ESPHome.
"""


class ConfigError(Exception):
    def __init__(self, message: str) -> None:
        super().__init__(message)
        self.message = message
