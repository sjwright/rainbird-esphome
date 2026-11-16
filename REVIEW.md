# REVIEW: a living document of issues

Last assessed on 16 November 2026. HEAD is `257ce56`, and the working tree
on top of it is uncommitted.

## Big

### The GPIO receiver

Phase 0 is built and nothing in the firmware calls it. `BurstAssembler` and
`tests/unit/test_burst_assembler.cpp` are in the tree. The Saleae capture
round-trips, missed edges did not invent frames, and jitter of ±25 µs per
edge still decoded. Failures started at ±30 µs. `remote_receiver` is still
the receive path. Phases 1 to 4 have not started.

PLAN-GPIO.md proposes replacing `remote_receiver` with a GPIO edge interrupt.
The code shows what `remote_receiver` costs today:

- About 200 lines of `wire.py` re-implement ESP-IDF's RMT block accounting per
  chip variant (`_RMT_LAYOUT`, `_UNSUPPORTED`, `_check_rmt_memory`,
  `_check_receiver`). That table needs an edit for every new chip and can go
  stale with an ESP-IDF change.
- `RainbirdComponent::release_receiver_pads_()` works around
  `remote_receiver` not moving an original-ESP32 RTC pad onto the digital
  input, and depends on the hub's setup priority sitting just below DATA.
- `codegen.py` has a `hasattr(remote_base, "add_listener")` shim for the
  listener API that changed in 2026.10. That coupling has already broken once
  ("Fix for 2026.10").
- Replies are capped at 42 bytes on the original ESP32.

Phase 1 waits on a soak of the current receive path. Changing it before that
would make a new fault hard to place. It also waits on the latency histogram
on a C3, PLAN-GPIO section 10.1. If that spread is over about 8 µs, phase 4
comes before phase 1 is worth building.

## Small

### No reproducible dev environment

`pyproject.toml` has no `[project]` table and no dependency group. `uv.lock`
is an empty stub with `requires-python = ">=3.15"`. The local `.venv` has
Python and nothing else, so `uv run pytest` fails. The `host` job runs
`pip install ruff yamllint pytest clang-format esphome==2026.9.1`, so the
linters and pytest are unpinned. A new clang-format release can change
formatting and turn CI red with no code change. The compile job pins ESPHome
to 2026.9.0, 2026.9.1 and 2026.10.0. That pin is not a lockfile, and CI does
not run `uv sync`.

**Suggested change:** a `dev` dependency group in `pyproject.toml` (ruff,
yamllint, pytest, clang-format, esphome), `requires-python` set to what ESPHome
supports, a real `uv.lock`, and `uv sync` in CI. That pins the linters ands
gives a one-command local setup.

### Tag a release

The docs match the project. There is no tag, so `examples/full.yaml` still
tracks `main`. Tag one when configs should pin it.