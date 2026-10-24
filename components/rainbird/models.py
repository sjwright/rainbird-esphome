"""The Rain Bird model this component builds.

The ESP-TM2 is the only one. Its accepted model ids are in model_esp_tm2.cpp, which
checks the 02 reply (PLAN D12).
"""

from dataclasses import dataclass

from .errors import ConfigError

ISSUES_URL = "https://github.com/sjwright/rainbird-esphome/issues"


@dataclass(frozen=True)
class Model:
    name: str
    zones: int
    starts: int
    # Program A is wire program 0.
    letters: str


ESP_TM2 = Model("ESP-TM2", zones=12, starts=4, letters="ABC")


def program_index(letter: str) -> int:
    """Wire program number for a validated letter: A is 0."""
    return ESP_TM2.letters.index(letter.upper())


def resolve_model(name: str) -> Model:
    """The model for a YAML model value. Case-insensitive."""
    if str(name).upper() != ESP_TM2.name:
        raise ConfigError(
            f"Unknown model '{name}'. {ESP_TM2.name} is the only model this component builds. {ISSUES_URL}"
        )
    return ESP_TM2
