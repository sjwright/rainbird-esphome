"""Which entity a `platform: rainbird` entry's key makes: its kind, zone, program and day or slot.

Codegen builds each entity from this, and final validation's duplicate and dependency
checks compare the same values, so the two cannot disagree. No ESPHome import, so the host
tests can load it.
"""

from dataclasses import dataclass

from .const import CONF_DAY, CONF_PROGRAM, CONF_SLOT, CONF_ZONE
from .entity_kinds import (
    NO_PROGRAM,
    NO_ZONE,
    EntityKind,
    FanOut,
    Scope,
    platform_keys,
    resolve_kind,
)
from .models import Model


@dataclass(frozen=True)
class Binding:
    kind: EntityKind
    zone: int
    program: int
    index: int

    @property
    def key(self) -> tuple[int, int, int, int]:
        """What makes two bindings the same entity. The C++ EntityBinding holds the same four."""
        return (int(self.kind.kind_id), self.zone, self.program, self.index)


def uses_program(kind: EntityKind) -> bool:
    """Program kinds, and zone kinds with one entity per program (run times)."""
    return kind.scope is Scope.PROGRAM or kind.fan_out is FanOut.PROGRAM


def entry_bindings(domain: str, entry: dict, model: Model) -> list[tuple[str, Binding]]:
    """(key, binding) for each entity key in a validated platform entry, in table order.

    The entry has passed platform_schema: `zone` is a number, `program` a letter,
    `day` an index from Monday and `slot` a 1-based number, each present when a key needs it.
    """
    out = []
    for key in platform_keys(domain):
        if key not in entry:
            continue
        kind = resolve_kind(domain, key, zone=CONF_ZONE in entry, program=CONF_PROGRAM in entry)
        if kind is None:
            continue
        zone = entry[CONF_ZONE] if kind.scope is Scope.ZONE else NO_ZONE
        program = NO_PROGRAM
        if uses_program(kind):
            program = model.letters.index(str(entry[CONF_PROGRAM]).upper())
        if kind.fan_out is FanOut.DAY:
            index = entry[CONF_DAY]
        elif kind.fan_out is FanOut.SLOT:
            index = entry[CONF_SLOT] - 1
        else:
            index = 0
        out.append((key, Binding(kind, zone, program, index)))
    return out
