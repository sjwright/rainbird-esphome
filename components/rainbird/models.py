"""Rain Bird model registry.

Stubs live here and only here. A config that names an unimplemented model fails
at validation, and there is no C++ class for it (PLAN.md 7.1).
"""

from dataclasses import dataclass

from .errors import ConfigError

ISSUES_URL = "https://github.com/sjwright/rainbird-esphome/issues"
_IMPLEMENTED = "ESP-TM2"

# Program A is wire program 0. A model uses the first `programs` letters.
PROGRAM_LETTERS = "ABCDEFGH"


def program_index(letter: str) -> int:
    """Wire program number for a validated letter: A is 0."""
    return PROGRAM_LETTERS.index(letter.upper())


@dataclass(frozen=True)
class Model:
    name: str
    implemented: bool
    zones: int
    programs: int
    starts: int
    model_ids: tuple[int, ...]

    @property
    def letters(self) -> str:
        return PROGRAM_LETTERS[: self.programs]


def _model(
    name: str,
    *,
    implemented: bool,
    zones: int,
    programs: int,
    starts: int,
    model_ids: tuple[int, ...],
) -> Model:
    return Model(name, implemented, zones, programs, starts, model_ids)


# model_ids are the 02 reply values accepted for that model (PLAN D12).
MODELS = {
    model.name: model
    for model in (
        _model(
            "ESP-TM2",
            implemented=True,
            zones=12,
            programs=3,
            starts=4,
            model_ids=(0x0005, 0x000A, 0x010A, 0x0014, 0x0015),
        ),
        _model(
            "ESP-ME",
            implemented=False,
            zones=22,
            programs=4,
            starts=6,
            model_ids=(0x0007, 0x0107),
        ),
        _model(
            "ESP-RZXE",
            implemented=False,
            zones=8,
            programs=0,
            starts=6,
            model_ids=(0x0003, 0x0103),
        ),
        _model(
            "ST8X-WF",
            implemented=False,
            zones=8,
            programs=0,
            starts=6,
            model_ids=(0x0006, 0x0008),
        ),
        _model(
            "ESP-ME3",
            implemented=False,
            zones=22,
            programs=4,
            starts=6,
            model_ids=(0x0009,),
        ),
    )
}


def resolve_model(name: str) -> Model:
    """Return the registry entry for a YAML model value. Case-insensitive."""
    key = str(name).upper()
    model = MODELS.get(key)
    if model is None:
        known = ", ".join(MODELS)
        raise ConfigError(f"Unknown model '{name}'. Known models: {known}.")
    if not model.implemented:
        raise ConfigError(
            f"{model.name} is not implemented yet. {_IMPLEMENTED} is the only "
            f"model this component builds. {ISSUES_URL}"
        )
    return model


# Every letter some model in the registry uses. A platform entry is checked against
# its hub's model in final validation, once the hub is known.
ANY_MODEL_LETTERS = PROGRAM_LETTERS[: max(model.programs for model in MODELS.values())]
