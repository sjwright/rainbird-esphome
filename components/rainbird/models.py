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
    # Value ranges, from the controller's manual. The number entities offer these and the
    # session refuses a command outside them; tools/gen_kinds.py writes them into model_limits.h.
    seasonal_min_percent: int
    seasonal_max_percent: int
    rain_delay_max_days: int
    # Manual runs (39, 4B) and the zone test (3A), in minutes. The byte holds 255, but
    # pyrainbird caps manual runs at 100 on every mains controller, the TM2 included, and an
    # ESP-RZXe refuses 101 (SPEC 5.4).
    run_max_minutes: int
    # Escaped queue replies, including Manchester phase and the idle tail, fit this many symbols.
    receive_symbols: int


ESP_TM2 = Model(
    "ESP-TM2",
    zones=12,
    starts=4,
    letters="ABC",
    seasonal_min_percent=5,  # "from 5% up to 200%"
    seasonal_max_percent=200,
    rain_delay_max_days=14,  # "Suspend watering for up to 14 days"
    run_max_minutes=100,  # the Rain Bird app's manual run stops at 1 h 40 min
    receive_symbols=448,
)


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
