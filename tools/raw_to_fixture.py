#!/usr/bin/env python3
"""Turn `remote_receiver` `dump: raw` log lines into a replay fixture.

Capture with `dump: raw` on the receiver (examples/sniffer.yaml), save the
`esphome logs` output to a file, then:

    python3 tools/raw_to_fixture.py session.log -o tests/captures/s4-app-session.txt \
        --note "LNK2 fitted, app: run zone 1, stop, rain delay 2"

Each `Received Raw:` dump becomes one `burst:` line. Long bursts continue on
following `remote.raw` lines that start with two spaces; those are joined.
The dumper leaves out the idle sample remote_receiver appends, so a burst
ends with the long run that holds the frame's last half-bit, exactly as the
listener sees it apart from that sample.

With --decode (the default) the bursts are run through the component's own
decoder (tests/unit/decode_fixture, built with make) to fill in `expect:`
lines. Check them before committing: a fixture records what the decoder
found, and only a person can say whether that was right.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
UNIT = REPO / "tests" / "unit"

_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_NUMBER = re.compile(r"-?\d+")
_START = "Received Raw:"
_TAG = "[remote.raw"


def parse_log(lines: list[str]) -> list[list[int]]:
    """Return one list of timings per `Received Raw:` dump."""
    bursts: list[list[int]] = []
    current: list[int] | None = None
    for raw in lines:
        line = _ANSI.sub("", raw).rstrip("\n")
        if _START in line:
            current = [int(n) for n in _NUMBER.findall(line.split(_START, 1)[1])]
            bursts.append(current)
            continue
        # A continuation directly follows its dump line, e.g.
        # "[I][remote.raw:041]:   -416, 416, ...". Only the part after "]:" is data.
        body = line.rsplit("]:", 1)[-1]
        if current is not None and _TAG in line and body.startswith("  "):
            current.extend(int(n) for n in _NUMBER.findall(body))
        else:
            current = None
    return [burst for burst in bursts if burst]


def render(bursts: list[list[int]], note: str | None, receiver: str | None) -> str:
    out = []
    if note:
        out.extend(f"# {line}" for line in note.splitlines())
    if receiver:
        out.append(f"receiver: {receiver}")
    for burst in bursts:
        out.append("burst: " + ", ".join(str(t) for t in burst))
    return "\n".join(out) + "\n"


def decode(path: Path) -> str:
    """Run the C++ decoder over a fixture and return it with expect lines."""
    subprocess.run(["make", "-s", "-C", str(UNIT), "decode_fixture"], check=True)
    result = subprocess.run(
        [str(UNIT / "decode_fixture"), str(path)], check=True, capture_output=True, text=True
    )
    return result.stdout


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=Path, help="saved `esphome logs` output")
    parser.add_argument("-o", "--output", type=Path, required=True, help="fixture to write")
    parser.add_argument("--note", help="comment written at the top of the fixture")
    parser.add_argument("--receiver", help="which receiver was dumped, e.g. rx or monitor")
    parser.add_argument(
        "--no-decode", action="store_true", help="write bursts only, without expect lines"
    )
    args = parser.parse_args(argv)

    bursts = parse_log(args.log.read_text(errors="replace").splitlines())
    if not bursts:
        print(f"{args.log}: no 'Received Raw:' lines found", file=sys.stderr)
        return 1
    args.output.write_text(render(bursts, args.note, args.receiver))
    if not args.no_decode:
        args.output.write_text(decode(args.output))
    print(f"{args.output}: {len(bursts)} bursts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
