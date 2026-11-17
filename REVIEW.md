# REVIEW: a living document of issues

Last assessed on 17 November 2026 at `6fb6b47`, with nothing uncommitted.
That commit is a refactor with no change in behaviour: the edits waiting for a
schedule page moved from `Session` into `ScheduleEditor`, `Capabilities` into
its own `capabilities.h`, the codec declarations into `model_esp_tm2.h`, the
`send_raw` and zone actions into `automation.h`, and `schemas.py` became
`entity_schema.py`. Every issue below was checked again against it and still
stands; the one under "To confirm on hardware" still needs the controller.

At that state the 174 host tests pass, plain and under ASan and UBSan, and the
82 pytest cases pass against ESPHome 2026.10.2, as do ruff, clang-format and
yamllint. `test.esp32-idf.yaml` and `test.esp32-ard.yaml` compile, and
`validate.esp32-idf.yaml` passes `esphome config`. `test.esp32-c3-idf.yaml`
and `test.ends-at-only.yaml` were not compiled this time.

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
- Replies on the original ESP32 are capped by RMT memory. The receiver now
  takes 448 symbols, 7 of the chip's 8 blocks, which holds the longest escaped
  TM2 queue reply and leaves no room to grow.

Phase 1 waits on a soak of the current receive path. Changing it before that
would make a new fault hard to place. It also waits on the latency histogram
on a C3, PLAN-GPIO section 10.1. If that spread is over about 8 µs, phase 4
comes before phase 1 is worth building.

### Tag a release

The docs match the project. There is no tag, so the README's quick start and
`examples/full.yaml` still track `main`, and `full.yaml` sets `refresh: 1d`. A
user's next compile therefore builds whatever `main` holds that day. For
firmware that opens water valves, a tag to pin is worth having before anyone
else runs it.

## Small

### No reproducible dev environment

`pyproject.toml` has no `[project]` table and no dependency group. `uv.lock`
is an empty stub with `requires-python = ">=3.15"`. The local `.venv` has
Python and nothing else, so `uv run pytest` fails, and ruff and clang-format
are not installed locally at all: lint only ever runs in CI. The `host` job
runs `pip install ruff yamllint pytest clang-format esphome==2026.9.1`, so the
linters and pytest are unpinned. A new clang-format release can change
formatting and turn CI red with no code change. The compile job pins ESPHome
to 2026.9.0, 2026.9.1 and 2026.10.0. That pin is not a lockfile, and CI does
not run `uv sync`.

**Suggested change:** a `dev` dependency group in `pyproject.toml` (ruff,
yamllint, pytest, clang-format, esphome), `requires-python` set to what ESPHome
supports, a real `uv.lock`, and `uv sync` in CI. That pins the linters and
gives a one-command local setup.

### CI does not build against the current stable ESPHome

The compile matrix is 2026.9.0, 2026.9.1 and 2026.10.0. The local install is
already 2026.10.2. A new stable release is only covered by the weekly `dev`
job, which builds one config. PLAN.md section 10 says CI builds against "the
latest stable release", which the matrix does not do.

**Suggested change:** one more matrix entry that installs ESPHome unpinned, so
the scheduled run fails when a stable release breaks the component.

### Polling carries on in full while the link is down

With the controller silent, `Session::poll()` still queues the whole short
cycle every `update_interval`, and each request is sent three times before it
fails. That is a few seconds of transmitting per interval and a `Send` line
for each attempt at debug level, until the five-minute restart.

**Suggested change:** while the link is down, send one request with no
retries, and go back to the full cycle when it is answered.

### PLAN-GPIO.md still uses the old reply size

Sections 1, 2.3, 4.7 and 6 give the longest reply as 42 bytes and the receiver
settings as 384 symbols. An escaped queue reply reaches 64 bytes on the wire,
and the settings are now 448. The buffer-sizing table in 4.7 matters most: it
sizes the new receiver's buffer from the 42-byte figure.

## To confirm on hardware

### A 2-byte command sent as one copy

The LNK2 sends every 2-byte request twice, back to back (SPEC 2.3, question
5). The hub copies that for reads and sends commands once, because the
controller may act on each copy. That leaves `38` (run program), a default
entity, going out as a single 2-byte frame, along with `42`, `3A` and `49`.
Nothing in the repo records the controller answering one sent that way. If it
does not, the log now says "No reply to 38" and `on_no_reply` runs.

### The escape table and the 100-minute run limit

Both now follow pyrainbird's later commits rather than a capture (SPEC 2.4 and
5.4). The hub escapes `1C` as `1B 01` and `1B` as `1B 02`, the other way
round from before, and refuses manual runs and the zone test over 100
minutes. The log notes the first `1B` or `1C` the controller escapes, which
settles the table. If instead `4C` gets no reply at :27 or :28 seconds, the old
order was right. A 101-minute `39` with `send_raw` would settle the limit.

