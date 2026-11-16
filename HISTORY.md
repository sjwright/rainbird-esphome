# History: how `rainbird` was planned and built

This is the record, not the reference. [PLAN.md](PLAN.md) describes the component as it
is; this file keeps what was planned before it was built, the order it was built in, and
the design reviews. Nothing here is kept up to date. Where it disagrees with PLAN.md or
the code, they are right.

Section numbers match the sections these were in PLAN.md, so older notes that cite
"PLAN section 9" or "step S6" still lead somewhere.

---

## 9. Delivery phases

Each phase ends with something usable and settles specific open questions
from SPEC.md section 8. A question counts as **settled once the answer is
known**, whether the controller accepts the command or not.

| Phase | Deliverable | Exit criteria | Settles |
|-------|-------------|---------------|---------|
| **0. Scaffold** | Repo layout, `entity_kinds.py` skeleton, empty hub that compiles against `remote_transmitter` / `remote_receiver`, CI green (host tests, pytest, compile), `examples/minimal.yaml` | All CI jobs pass for both frameworks | – |
| **1. Sniffer** | Receive adapter, decoder with per-receiver polarity lock, `listen_only` + `monitor_receiver_id`, `on_frame`, `examples/sniffer.yaml`, first `tests/captures/` from `dump: raw` | With the LNK2 still connected, both directions decode for 1 h, duplicates included, with 100 % valid checksums and no receiver overflow warnings | 13 (polarity and timing on both receivers), 5 (whether duplicates get two replies), 4 (`3D` layout) if a fault is set up |
| **2. Link** | Transmit through `remote_transmitter`, transport, identification and model gate, `04` probing, `connected`, `response_time`, `link_errors`, model/firmware/serial | LNK2 removed. 24 h of polling with fewer than 0.1 % retries and no unrecovered errors. Measured turnaround fixes the internal timeout constant. | 12 (model ID), 5 again (A/B test of duplicate requests) |
| **3. Status** | All read-only controller and zone entities, publish-on-change, `remaining_time` countdown, `queue` and `active_program` text, `schedule_changed` | Values match the controller display through a full program run | 1 (rain sensor, by opening the sensor input), 2 (`48` vs OFF), 10 (queue high bits), 11 (`4A` changes on a front-panel edit, and whether it is local time) |
| **4. Control** | Zone switches, run durations, queue, stop, run program, rain delay, seasonal adjust, clock sync and `controller_clock`, `rainbird.zone.*` actions, `packages/ha-actions.yaml` | `39`, `4B`, `38`, `40`, `37`, `31`, `11`, `13` acknowledged and confirmed by a follow-up read. `49`, `3A` and `42` each tried behind `experimental` until it is known whether and how they work. `42` follows the probe order in SPEC 5.4. Seasonal adjust probed at 5 % and 0 % on a program with no start times. Every NAK seen while probing is recorded with its code. | 3 (`49`), 6 (`3A`), 7 (`42`), 14 (seasonal adjust minimum), 15 (NAK code bits) |
| **5. Schedule read** | Page fields, program and zone schedule entities (read-only) | Entities match the front panel for all 3 programs. A distinctive run time set at the front panel on one odd and one even zone shows up in the expected page and slot. | 9 (program page byte 4), page-to-zone mapping |
| **6. Schedule write** | `allow_writes`, generic read-modify-write with hex logging | Each page type written and verified on hardware, including the inferred `8N` and `20` layouts. Untouched bytes confirmed unchanged. | 8 |
| **7. Release** | README (wiring, YAML, entity list generated from `entity_kinds.py`), `examples/full.yaml`, v1.0.0 tag | A fresh user can build from the README alone | – |

As each question is settled, SPEC.md is updated, the matching `[inferred]`
tag becomes `[wire]`, and the default or `experimental` gate is adjusted.

---

## 12. Schedule of work

Section 9's phases are the hardware gates. This section is the working order.

**Principles**

- All hardware-independent code is finished and tested on the host first.
- Each visit to the controller tests exactly one new layer.
- The code for the next step is written while hardware waits run: the
  1-hour sniff, the 24-hour soak and the program run.

There are three kinds of session:

- **Desk:** code and CI. No hardware needed.
- **Bench:** a dev board, or the interface board on the bench.
- **On-site:** something physical at the controller: wiring, the rain-sensor
  jumper, the OFF position, front-panel edits. Once the board is installed
  and on WiFi, firmware updates (OTA) and logs (`esphome logs`) work over the
  network. So on-site sessions are only for physical actions, and decoding
  and analysis happen at the desk.

### 12.1 Steps

| Step | Kind | Work | Exit | Phase |
|------|------|------|------|-------|
| **S0 Foundation** | desk | First commit of the docs, GitHub repo, CI (host tests, pytest, `esphome compile` for both frameworks and the version matrix). Empty hub, final validation of `transmitter_id` / `receiver_id` against 6.1, `entity_kinds.py` rows for the default entities, `models.py`, `examples/minimal.yaml`. No protocol code. | CI green | 0 |
| **S1 Codecs** | desk | `manchester`, `sip_frame`, `sip_commands` and the quirk helpers. Every SPEC frame and reply is a vector, plus a burst with two frames and no gap, a reply with the extra leading half-bit, and both polarities. | `make -C tests/unit test` passes | 0 |
| **S2 Model and state** | desk | `ControllerState`, `EspTm2Model` (`apply_reply`, `locate`, codecs, short and slow poll from the set of fields in use). Tests cover the `CC`, `BB` and `A0` captures, `FF` = manual, big-endian seconds, zone-to-page offsets, a `1P` edit that leaves bytes 3 and 4 alone, and the no-`4C` fallback poll for `0005`. | Host tests pass | 0 |
| **H1 Hardware** (alongside S0–S2) | bench | Order the parts (12.3). Build the **receive path first**: 24 V AC supply, optocoupler 1 to the receiver pin, and a tap lead so J2 and J4 can be heard with the LNK2 still fitted. The transmit path (optocoupler 2) is only needed from S5. | Both receive paths checked with a meter or logic analyser | – |
| **S3 Bench loopback** | desk + bench | Receive adapter, per-receiver polarity lock, `listen_only`, `monitor_receiver_id`, `on_frame`, `examples/sniffer.yaml`. On one dev board, wire the transmitter pin to the receiver pin and send frames from `send_raw`. Then repeat through the optocouplers once they're built, to see the real inversion. | Looped-back frames decode with valid checksums | 1 |
| **S4 Sniffer** | on-site, then desk | **Passive only.** The LNK2 stays connected. (a) Listen for 1 hour. (b) **App-driven capture:** in the Rain Bird app, run a zone, queue a zone, stop, run a program, advance, test all zones, set the rain delay, set seasonal adjust (including the lowest value the app allows), turn irrigation off and on, and edit one of each schedule field. Note the time of each action. Commit the `Received Raw:` bursts to `tests/captures/` and decode them at the desk. A checksum miss here is a codec bug; go back to S1 rather than starting the transmitter. | 100 % valid checksums in both directions. SPEC.md updated with every answer the captures give. | 1 |
| **S5 Link** | desk, then on-site | `rainbird_link`: one request in flight, duplicate short requests in one transmit buffer, retries, model gate, `04` probes, diagnostics. Swap the LNK2 out. The TM2 keeps watering on its own programs, and the LNK2 can go back in at any time. Start the 24-hour soak, and write S6's publishers while it runs. | Soak passes. Timeout constant set from the measured turnaround. | 2 |
| **S6 Status** | desk, then one on-site visit | Publish-on-change, `remaining_time` countdown, `queue`, `active_program`, `schedule_changed`. **Visit checklist:** run a program with zone times turned down; open the rain-sensor input; switch the controller to OFF; edit the schedule at the front panel at a noted time. That settles SPEC questions 1, 2, 10 and 11 in one visit. | Entities match the panel. SPEC.md updated before any control code is written. **v0.1.0** | 3 |
| **S7 Control** | desk, then on-site | `submit()`, the five entity classes, the three actions, `packages/ha-actions.yaml`. Prove `39`, `4B`, `38`, `40`, `37`, `31`, `11` and `13` with follow-up reads. One experimental session covers only what S4's captures **didn't** settle about `49`, `3A`, `42` and the seasonal-adjust floor. Use the SPEC 5.4 order, a program with no start times, and record every NAK. | Every control confirmed by read-back. **v0.2.0** | 4 |
| **S8 Schedule read** (can overlap S7) | desk, then on-site | The opt-in schedule rows in `entity_kinds.py`; the poll plan picks up their pages. Check all three programs against the panel. Set one odd and one even zone to a run time you'll recognise, and confirm the page and slot. No `21` writes. | Matches the panel; page-to-zone mapping confirmed. **v0.3.0** | 5 |
| **S9 Schedule write** | desk, then on-site | `allow_writes` and the generic read-modify-write, with the encodings checked against the app writes captured in S4. **One page type per session, in this order:** `00` (station delay, rain-sensor bypass), `6P` (a start time), `1P` (one watering day), `8N` (one run time), `20` (one zone bypass). Read the hex log back before moving to the next page type. Stop if a confirming read differs. | Every page type written and confirmed; untouched bytes unchanged | 6 |
| **S10 Release** | desk | README entity tables generated from `entity_kinds.py`, `examples/full.yaml`, wiring guide. | CI green → **v1.0.0** | 7 |

S1 and S2 land as separate changes. S8 can overlap S7, because both sit on
the same state layer. S4, S5, S6, S7 and S9 each need the controller, and
each starts only after the previous step's exit line is true.

### 12.2 Effort and calendar

| Kind | Sessions |
|------|----------|
| Desk | about 17–20 |
| Bench | about 3–4, including H1 |
| On-site | about 8–11. S9 is the longest at one page type per session. |
| Unattended waits | 1 h sniff, 24 h soak, parts shipping (3–10 days) |

At a hobby pace of about three sessions a week, plan for **8–10 weeks** to
v1.0.0. Parts shipping overlaps S0–S2, so it costs nothing. The calendar is
set by on-site availability, not by coding: v0.1.0 (read-only status) is
about 4 weeks in.

### 12.3 Before starting

- **Repository:** `sjwright/rainbird-esphome` created on GitHub, and an OK
  for the first commit.
- **Board:** an **ESP32-S3** dev board is recommended. It has RX ping-pong
  and 4 receive channels, so the two-receiver sniffer setup has room to
  spare. A C3 or C6 also works. The original ESP32 is tight for sniffing.
  The S2 is unsupported (6.1). Pick a board with an external-antenna
  connector if WiFi is weak where the controller is; the WIRE.md author
  mentions LNK2 WiFi reception problems.
- **Parts:**
  - 2× VO617A optocouplers and resistors;
  - a small 24 V AC → 5 V supply module;
  - two 5-pin JST-compatible leads (one for the tap);
  - strongly recommended: an inexpensive 8-channel USB logic analyser.
- **Rain Bird app** working through the existing LNK2, for S4.
- **Safety for S7 and S9:** work at a time when watering does no harm, or
  disconnect the valve common wire. Before S9's first write, save the full
  schedule dump from the log as a backup.

### 12.4 Build notes (S0–S6 code)

The code for S0 to S6 is built and host-tested; nothing has run on a
controller yet. Where the build changed the design above:

- **The link is two pure classes plus a thin hub.**
  - `LinkCore` (`link_core.h`) is the transport: queue, one request in
    flight, timeouts from the end of the first copy, retries, reply matching
    with selector checks, and duplicate counting.
  - `Session` (`session.h`) is startup, probes, the poll plan, the model
    gate and diagnostics.
  - `RainbirdComponent` only sends, receives and publishes.

  Both pure classes run against a scripted controller in
  `tests/unit/test_session.cpp`.
- **All waits are "elapsed since a start time ≥ a delay" in unsigned
  arithmetic.** A signed comparison against a stored deadline refuses to
  send for up to 24.8 days after a long idle spell or the millis() wrap; the
  host tests caught that.
- **Startup probes all seven optional commands** (`4C 4A 42 4B 3D 49 3A`).
  If `04` itself is refused with a NAK, every optional command is tried, and
  a NAK with bit 0 switches that one off. Probes that only time out assume
  nothing, so the fallback polls run.
- **`update_interval: never` puts the hub in manual mode**: no
  identification and no polling, only `send_raw`. The bench loopback uses
  this.
- **In `listen_only` mode every valid reply updates state** (in active mode
  only the matched one does), so status
  entities work passively alongside an LNK2. `connected` there means "a
  valid frame in the last 60 s".
- **`schedule_changed` is a `text_sensor`** with ISO 8601 text and device
  class `timestamp`. A float sensor only holds an epoch to about two
  minutes.
- **All six entity platforms are generated from `entity_kinds.py`**
  (`platform.py`). A key resolves to a zone, program or controller kind by
  the entry's `zone:` / `program:`.
- **The duplicate-entity check covers controls only**; a second read-only
  sensor for the same value is harmless.
- **Replay tooling:**
  - `tools/raw_to_fixture.py` converts `dump: raw` logs.
  - `tests/unit/decode_fixture` fills in the expected frames.
  - `tests/unit/test_replay.cpp` runs every file in `tests/captures/`.
  - `synthetic-replies.txt` is generated; S4's real captures replace it.

---

## Appendix: review notes

The plan went through four rounds of external review before the code was written. Some suggestions
were changed or declined, and the reasons are recorded here. Rows marked
*superseded* were adopted at the time and later replaced.

### Round 1: plan review

| Suggestion | Outcome |
|------------|---------|
| Separate polarity lock per receive pin | Adopted. Now per receiver (6.2). |
| Split frames by length; keep receive running during transmit | Adopted (6.2). The transmitter and receiver are now separate ESPHome components, so receive is never paused. |
| Refuse transmits on a model-ID mismatch | **Changed.** Status polling continues, because reads can't hurt the controller and they give a mismatched user something to report. Control and writes are refused, and `accept_model_ids` is the escape hatch (D12). |
| Four time entities for start times | **Changed.** Time entities can't show "off" (`FFFF`), so each slot also gets an enabled switch (D7). |
| Replace free-text watering days | Adopted as 7 switches. The `text` platform is not used. |
| Program-page writes must keep bytes 3 and 4 | Adopted. Now built into the generic read-modify-write (6.5). |
| `request_timeout` 400 ms, measured from the first copy | Adopted, as an internal constant (4.1) |
| IRAM-safe timestamps, overflow counting | *Superseded* by D10: `remote_receiver` owns the interrupt and reports overflows. |
| `update()` only enqueues; use `set_update_interval()` | Adopted (6.3) |
| Phase 4 exit for `49` / `3A` / `42` | Adopted. A question is settled once the answer is known. |
| `send_raw` bypasses the gates | Adopted. It needs `experimental` and lives only in the sniffer example. |
| Say what happens when `programs:` is omitted | Adopted, and the same rule now applies to `zones:` |
| Make "off stops everything" prominent | Adopted (sections 2 and 4.2, and the YAML comment) |
| Log the page in hex before every write | Adopted (6.5) |
| Don't derive `EspMeModel` from `EspTm2Model` | *Superseded*: there are no C++ stub classes now (7.1). |
| Host test build command | Adopted (section 8) |
| `run_time_numbers` breaks the naming convention | **Changed.** Kept, and plural keys became an explicit rule (D5). Moving run times under programs was declined: zones are where users think about durations. |
| Test the NAK code as a bitmask | Adopted, as `nak_is_unsupported()` (5.1 rule 5) |
| Time zone of the `schedule_changed` timestamp | Adopted. Converted with `ESPTime` and `time_id`; verified in phase 3. The 6-hour gap in the SPEC capture is the event being earlier in the day, not a time-zone offset. |
| `queue`-mode "off" sends `42` | Adopted behind `experimental` (6.5) |
| Replay harness of real captures | Adopted, now from `remote_receiver`'s `dump: raw` |

### Round 2: pyrainbird cross-check

| Suggestion | Outcome |
|------------|---------|
| Seasonal adjust minimum | **Kept 10 %** although it is only a docstring: 0 % might be accepted and quietly stop a program. Phase 4 probes lower values (SPEC 8.14). |
| Don't copy pyrainbird's program-page write template | Adopted. SPEC 5.6 warns that it hard-codes byte 4. |
| `42` should send a real zone number, not `00` | Adopted. The probe order is in SPEC 5.4. |
| Zone off-by-one in pyrainbird's schedule decoder | **Partly adopted.** pyrainbird's decoder is 0-based and its `Schedule` adds 1, so it's not an upstream bug. Test vectors from the raw decoder still need the +1 (SPEC 5.6, section 8). |
| Schedule encodings belong to the model | Adopted (7.2) |
| NAK bit meanings are docs-only | Adopted (SPEC 8.15) |

### Round 3: simplification and reuse

| Suggestion | Outcome |
|------------|---------|
| Use `remote_transmitter` / `remote_receiver` instead of our own physical layer | **Adopted** (D10). Agent 1 argued that `remote_receiver` can't hold a 42-byte reply on a C3. The ESP-IDF 5.5.4 driver shows RX ping-pong on the C3, S3, C6 and others, and an original ESP32 has room for 384 receive symbols. Agent 2 argued that `remote_receiver` splits on gaps. Our decoder splits by length inside each burst, so that doesn't matter. The **ESP32-S2** really can't fit a worst-case reply and is rejected. |
| Create a `remote_transmitter` internally to keep `tx_pin` on the hub | Declined. It drives another component's internals and hides the wire setup. Bus-style references (`transmitter_id`, like `uart_id`) are the native pattern. |
| Keep our own GPIO interrupt for receive | Declined. Shown unnecessary by the RX ping-pong check above. |
| Don't use core `sprinkler` | Agreed: it would be a second scheduler on top of the controller's |
| One class per domain, bound by kind and index | Adopted (5.4) |
| Parsing only in `sip_commands`; the model maps typed replies | Adopted (5.1 rule 1, 7.2) |
| Follow-up reads from the model's short poll (`0005` has no `4C`) | Adopted (6.3, 6.4) |
| No C++ stub classes until a second model is built | Adopted. Stubs live in the registry (7.1). |
| One Python table drives the hub expander and the platforms | Adopted (5.3) |
| Ship the HA actions as a package | Adopted (`packages/ha-actions.yaml`) |
| Persist an off slot's time with `TimeEntityRestoreState` | Declined. RAM-only is simpler and keeps `ControllerState` the single source of truth (6.5). |
| Layer ownership and include direction | Adopted (5.1 rules 1–2) |
| Protocol quirks as named functions | Adopted (5.1 rule 5) |
| Single zone-reference resolver | Adopted (4.5) |
| Publish only on change | Adopted, through changed-field tracking (5.1 rule 3) |
| `on_time_sync` as an optional clock-sync trigger | Adopted as a documented option; the built-in drift check stays (6.5) |
| Make `switch` vs `valve` final | Adopted (D6) |
| No statics or globals | Adopted (5.1 rule 6) |
| Generated IDs are API | Adopted (D9), with a snapshot test |
| Replace custom actions and conditions with entity ones | Adopted (D13, 4.5): 3 custom actions remain, down from 11, and there are no custom conditions |
| Derive the poll plan from configured entities; `04` as the only capability source | Adopted (6.3, 6.4) |
| Option count | Reduced: timeouts, retries, duplicates, fast poll and polarity are internal constants (4.1) |

### Round 4: schedule of work

| Suggestion | Outcome |
|------------|---------|
| Split the host work into scaffold, codecs, and model/state, each a separate change (Agent 1) | Adopted (S0–S2) |
| Write the next step's code during the hardware waits (both agents) | Adopted (12, principles) |
| "A checksum miss in the sniffer is a codec bug, not a reason to start the transmitter" (Agent 1) | Adopted (S4) |
| One status visit settles questions 1, 2, 10 and 11, with a short program run (Agent 1) | Adopted (S6 checklist) |
| Schedule writes one page type per session, in the order `00`, `6P`, `1P`, `8N`, `20` (Agent 1) | Adopted (S9) |
| Build the receive path first; transmit can wait until the link step (Agent 2) | Adopted (H1) |
| Hobby-pace estimate of 8–9 weeks (Agent 2) | Adopted as 8–10 weeks. My earlier 5–6 weeks assumed near-daily sessions. |
| The controller must be within reach of the bench (Agent 2) | **Changed.** Once installed and on WiFi, OTA and network logs make most work remote. Only physical actions need a visit. |
| Removing the LNK2 removes the household's irrigation control (Agent 2) | **Corrected.** Only app control is lost: the TM2 keeps running its own programs, and the LNK2 can be refitted at any time. |
| Probe `49`, `3A`, `42` and the seasonal floor on the controller (Agent 1) | **Changed.** S4's app-driven capture records the official app sending these, so S7's experimental session only covers what that didn't settle. |
| Bench loopback before the first controller visit (mine) | Kept (S3): RMT round trips are cheaper to debug on the bench. |
