"""Import the pure modules without loading the ESPHome hub.

``rainbird/__init__.py`` imports ESPHome. Putting a stub package in
``sys.modules`` first lets tests import ``expand`` and ``models`` on a
Python that does not have ESPHome installed.
"""

from pathlib import Path
import sys
import types

_root = Path(__file__).resolve().parents[2] / "components" / "rainbird"
if "rainbird" not in sys.modules:
    package = types.ModuleType("rainbird")
    package.__path__ = [str(_root)]
    package.__package__ = "rainbird"
    sys.modules["rainbird"] = package
