# Design: `rainbird` ESPHome component

This describes how the component is built: an ESPHome external component that
replaces the Rain Bird LNK2 WiFi module and talks to the controller directly
over its accessory port, using the protocol in [SPEC.md](SPEC.md).

It began as the implementation plan, and the file keeps that name because code
comments cite its sections ("PLAN 6.3"). Section numbers are stable for that
reason. It is kept in step with the code: where the two disagree, that is a
bug in this file.

- Using the component: [README.md](README.md).
- The order it was built in, the hardware phases and the design reviews:
  [HISTORY.md](HISTORY.md).

## Goals

- **Import straight from GitHub** as an `external_components` source.
- **One short YAML block per controller.** Each irrigation zone is named once,
  and all of that zone's entities are generated from the name. Every
  generated entity can still be customised or turned off.
- **Look and behave like a core ESPHome component.** That covers config keys,
  file layout, C++ and Python idioms, entity schemas, actions and
  `dump_config()` output. Wherever ESPHome already provides a piece, use it
  rather than writing our own.
- **Fully implement the ESP-TM2** (every TM2 variant). It is the only model;
  a config that names another gets a clear error (section 7).
- **Be safe by default.** Commands that SPEC.md marks as unverified, mainly
  schedule writes, stay disabled until the user turns them on.

## Non-goals (v1)

- Implementing other Rain Bird models.
- Platforms other than ESP32. ESPHome's `remote_transmitter` and
  `remote_receiver` also run on ESP8266, RP2040 and LibreTiny, so other
  platforms may work. They are untested. On those platforms the transmitter
  blocks, and a duplicated request takes about 53 ms, which is over ESPHome's
  50 ms blocking warning.
- Weather-based adjustment logic. Home Assistant automations can drive the
  seasonal-adjust and rain-delay entities instead.
- Reflashing the LNK2 itself.

---

## 1. Key decisions

Every decision here is final unless marked "open". Each one shapes the YAML a
user writes (README.md, `examples/full.yaml`).

| # | Decision | Why |
|---|----------|-----|
| D1 | **`model: ESP-TM2`, not `type:`** | ESPHome uses `model:` for hardware variants (`dht`, `ili9xxx`, `ssd1306`). The value is case-insensitive, so `esp-tm2` also works. Which TM2 variant is fitted is found at runtime (6.3). |
| D2 | **Zones and programs are declared on the hub.** Their entities are generated from `zone_defaults` / `program_defaults`. | This follows core's `sprinkler` component (`valves:` with `valve_switch`, `run_duration_number`, …). Naming a zone once gives you its switch, duration, run times and so on, which is the efficiency asked for. Sprinkler's keys take only a name string; ours take either a name string or a full entity schema (`cv.maybe_simple_value(..., key=CONF_NAME)`). |
| D3 | **`zones:` and `programs:` are shorthand for ordinary platform entries.** One Python table of entity kinds (5.3) drives both the hub expansion and the platform modules. | There is one code path per entity kind. A zone's entities can also be written by hand as `switch: - platform: rainbird` with `zone: 4`, and they work with ESPHome's own `packages`, `!extend` and `!remove`. Adding an entity kind is one table row. |
| D4 | **Controller-wide entities use normal platform blocks** (`sensor: - platform: rainbird`) with one key per entity | This follows core hub components such as `ld2410`. |
| D5 | **Entity keys are named `<thing>_<domain>` on the hub.** A singular key (`valve_switch`) creates one entity. A plural key (`run_time_numbers`, `watering_day_switches`, `start_time_datetimes`) creates one per program, day or slot. In platform blocks the domain suffix is dropped (`valve`, `run_time`). | The hub mixes domains, so the suffix tells you the entity type. Inside a `switch:` block it would be redundant. |
| D6 | **Zone valves are `switch` entities. This is final.** | Changing the domain after release would break every user's entity IDs and automations, so it is decided now. ESPHome's `valve` domain promises a per-valve "close", and the TM2 can only stop all watering, so `valve` would misrepresent the hardware. `switch` also matches `sprinkler` and the Home Assistant Rain Bird integration. |
| D7 | **Schedule fields use typed entities, never free text.** Watering days are 7 switches per program. Each start-time slot is a `datetime` time entity plus an "enabled" switch, since `FFFF` (off) can't be shown as a time. | Nothing on the write path parses text, and each entity maps to bits or bytes of one page. All are opt-in. |
| D8 | **Names can contain the placeholders `{zone}`, `{program}`, `{day}` and `{slot}`** | One template in `zone_defaults` or `program_defaults` names the same entity everywhere. Single braces don't clash with ESPHome's `${substitutions}`. |
| D9 | **Generated IDs are public API from the first release**: `<zone id>_<key>`, e.g. `front_lawn_valve_switch` | Users will write lambdas and automations against them, so changing the scheme is a breaking change. A snapshot test (section 8) guards it. |
| D10 | **The wire layer is ESPHome's `remote_transmitter` and `remote_receiver`**, referenced by `transmitter_id` and `receiver_id`. | This is how core's `climate_ir` platforms do it, and how bus-based components take a `uart_id`. ESPHome already provides non-blocking RMT transmit with the carrier off, RMT receive with its own interrupt handler, buffering, overflow warnings and raw dumps, and it maintains that code across ESP-IDF releases. The rainbird component never touches RMT or IRAM. It does call `rtc_gpio_deinit` on the receiver pins after `remote_receiver` setup, because that component leaves original-ESP32 RTC pads on the RTC mux (6.1). Section 6.1 checks the limits. |
| D11 | **Unverified commands are off by default**: `schedule: allow_writes: false`, `experimental: false`. `rainbird.send_raw` also needs `experimental: true`. | SPEC.md section 8 lists the unverified commands. A raw-send action left in a config would bypass both gates. |
| D12 | **An unrecognised model ID makes the hub read-only** | If `02` returns an ID outside the model's list, status polling continues but every control and write command is refused. `accept_model_ids:` overrides this. |
| D13 | **Prefer ESPHome's own entity actions, conditions and triggers.** Custom actions exist only where a runtime argument is needed. | `button.press`, `number.set`, `switch.is_on` and `on_turn_on` already cover stopping, rain delay, program runs, state checks and "zone started". What's left: `rainbird.zone.start`/`queue` (zone number from Home Assistant), `rainbird.send_raw`, and the `on_nak`/`on_frame` triggers (4.5). |
| D14 | **Layering and ownership rules** (5.1) are fixed now | Wire format, protocol semantics, scheduling and entities each have exactly one owner. Every write passes through one function. `ControllerState` is the only copy of controller data. |

---

## 2. Using the component

[README.md](README.md) is the user documentation: what to build, the smallest
config, every hub option, how zones and programs generate entities, and the
entity tables. Two points of design sit behind it:

- **Releases are tags.** Every release is to be tagged `vMAJOR.MINOR.PATCH`,
  and a config should pin one (`github://sjwright/rainbird-esphome@v…` with
  `refresh: never`). Until the first tag exists the examples track `main`.
  The minimum ESPHome version appears as `esphome: min_version` in the
  examples.
- **`transmitter_id` and `receiver_id` resolve by themselves** when the config
  has one transmitter and one receiver, as they do for any ESPHome component.
  If a transmitter or receiver setting won't work for this protocol, config
  validation names it and gives the value to use (6.1).

---

## 3. Example YAML

The examples are files, so CI can validate them:

| File | What it is |
|------|------------|
| `examples/minimal.yaml` | The smallest config that validates |
| `examples/full.yaml` | Every option and every entity, with comments |
| `examples/sniffer.yaml` | `listen_only`, a monitor receiver, `dump: raw` and `send_raw`, for protocol work |
| `packages/ha-actions.yaml` | Home Assistant actions (`run_zone`, `queue_zone`, `set_rain_delay`, `stop_watering`), includable as a remote package |

Only the zone actions in the package are custom; the rest use ESPHome's own
entity actions (4.5).

---

## 4. Configuration reference

### 4.1 Hub (`rainbird:`)

`MULTI_CONF = True`: several hubs are allowed (one per controller).

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `id` | ID | auto | `RainbirdComponent` |
| `model` | string | **required** | `ESP-TM2`, the only model (section 7). Anything else is rejected with a link to the issue tracker. |
| `transmitter_id` | ID of `remote_transmitter` | the only one | Checked at final validation (6.1) |
| `receiver_id` | ID of `remote_receiver` | the only one | Checked at final validation (6.1) |
| `listen_only` | bool | `false` | No transmitter is used. State is decoded from replies to another master, such as an LNK2. |
| `monitor_receiver_id` | ID of `remote_receiver` | – | A second receiver on J4, so requests from another master are decoded too. Development only. |
| `accept_model_ids` | list of int | – | Extra model IDs to treat as this model (D12) |
| `update_interval` | time | `15s` | Idle poll cycle (section 6.4). `never`: manual mode, where nothing is sent except `send_raw`. |
| `valve_switch_mode` | `start` / `queue` | `start` | Whether turning a zone switch on sends `39` or `4B`. Also changes what "off" does (6.5). |
| `time_id` | ID of `time` | – | Needed by `clock_sync`, `sync_clock`, `schedule_changed` and `ends_at` |
| `clock_sync` | mapping | absent: no sync | The block has to be present for the clock to be synced; `time_id` alone does not. |
| `clock_sync.max_drift` | time | `30s` | Sync when the controller drifts further than this, at least 5 s. Checked on every clock reading, from boot on; a UTC-offset change moves local time an hour, so it shows as drift. Needs `time_id`. |
| `schedule.refresh_interval` | time | `1h` | Full re-read of the schedule pages that configured entities use, at least 1 min. The `4A 01` timestamp triggers re-reads in between, except for the change our own write makes. |
| `schedule.allow_writes` | bool | `false` | Lets schedule entities write `21 …` pages |
| `experimental` | bool | `false` | Allows `49`, `3A`, `42` and `rainbird.send_raw` |
| `handshake` | bool | `true` | Runs the `06`/`07` challenge exchange before `02` (SPEC 5.0). A controller that refuses `06` is identified without it. |
| `zone_defaults`, `zones` | see 4.2 | all 12 zones | `zones: []` creates none |
| `program_defaults`, `programs` | see 4.3 | all programs | `programs: []` creates none |
| `on_nak` | automation | | Variables `command`, `code` |
| `on_frame` | automation | | Variable `payload` (`std::vector<uint8_t>`) for every valid reply. The monitor receiver's requests are only logged. |

**Internal constants, not options.** Only one that turns out to need tuning per
installation would become an option.

| Constant | Value | Notes |
|----------|---------------|-------|
| Request timeout | 400 ms | From the end of the first copy of a request. The longest reply (42 bytes) is about 140 ms on the wire, plus about 30 ms of controller turnaround. |
| Retries | 2 | For timeouts and bad frames on reads. A NAK is never retried. Commands, schedule writes and `send_raw` are never retried, so a lost reply cannot run a command the controller already acted on. |
| Duplicate 2-byte requests | on | Copies the LNK2 habit (SPEC 2.3) for reads and `send_raw`. Commands and schedule writes go as one copy, because the controller may act on each. |
| Fast poll interval | 3 s | While watering, and for 30 s after a command |
| Gap between transactions | 20 ms | |
| Receive polarity | auto, per receiver | Locked separately for each receiver (6.1) |

### 4.2 Zones

> **Turning off a zone switch sends `40`, which stops every zone.** That is the
> only stop command the controller has. Turning off a zone that is only
> queued does nothing. Details, and the experimental `queue`-mode
> alternative, are in 6.5.

**Zone keys:**

| Key | Default | Notes |
|-----|---------|-------|
| `zone` | **required** | 1 to the model's maximum (12 on the TM2). Each zone number can appear only once. |
| `id` | `<hub id>_zone<N>` | Prefix for the zone's generated entity IDs (D9). Also accepted by `rainbird.zone.start`. |
| `name` | `Zone N` | Fills the `{zone}` placeholder, and is used in the `queue` text |
| `disabled` | `false` | Generates no entities for this zone |
| `device_id` | – | Passed to every entity of the zone, to group a zone as an ESPHome sub-device |

**Generated entities.** The table of zone entities is in README.md, generated
from `entity_kinds.py` (5.3). Where each one's value comes from:

| Platform key | Source → write |
|--------------|----------------|
| `valve` | `3F` → `39`/`4B`. Off: `40` (or `42`, 6.5). |
| `run_duration` | A local setting. Used by `valve` and `queue`. 255 is the `39` limit. |
| `running` | `3F` |
| `queue` | → `4B` |
| `run_time` (+ `program:`) | pages `80`–`85` → `21 00 8N` |

Two zone entities were planned and are not built: a per-zone rain-sensor
bypass (page `20`, whose sense only pyrainbird's docs describe, so nothing
writes it until a capture shows it) and a zone fault sensor (`3D`, still
undecoded, SPEC 8.4).

**How each entity's settings are worked out** (`expand.py`):

1. With no `zone_defaults`, a zone gets the kinds marked default. With
   `zone_defaults`, it gets exactly the keys listed there.
2. If the zone sets a key to `false`, that entity is not generated.
3. Otherwise, the zone's own settings for that key are merged over the
   `zone_defaults` entry. A zone that sets a key absent from `zone_defaults`
   gets that entity anyway.
4. A key given as a plain string is read as `{name: <string>}`, as in core's
   `sprinkler`. `true` is an empty mapping.
5. The placeholders in the name are filled in.
6. If no `id` is given, the generated ID is `<zone id>_<key>`. Plural keys
   add a suffix: the program letter, day or slot number, e.g.
   `front_lawn_run_time_numbers_a`. These are ESPHome config IDs for
   lambdas, not the deprecated entity `object_id`, which the component never
   sets or reads. `zone_defaults` cannot set an `id`.
7. The result is checked against the same entity schema the platform module
   uses, so every standard option (`icon`, `internal`, `entity_category`,
   `disabled_by_default`, `device_id`, `on_turn_on`, `filters`, …) works.
   ESPHome validates a domain as soon as that domain is loaded, so the
   entities cannot be appended to `switch:`; the hub validates them, keeps
   them in its own config and builds them itself.

**Writing zone entities by hand** as `platform: rainbird` entries gives
exactly the same result, and those are what `packages`, `!extend` and
`!remove` edit. A hand-written control that repeats one the hub generates, or
another hand-written one, fails validation; a second read-only sensor for the
same value is harmless and allowed.

A valve switch or queue button needs its zone's `run_duration` number, a
start-slot switch needs that slot's `start_time`, and `test_all_zones` needs
`test_duration`: each control runs with the other entity's value. Final
validation fails a config that leaves one out, rather than letting the
control refuse every press (`entity_checks.py`).

### 4.3 Programs

**Program keys:**

| Key | Default | Notes |
|-----|---------|-------|
| `program` | **required** | `A`–`C` on the TM2. Stored as 0–2. |
| `id` | `<hub id>_program_<a>` | Prefix for the program's generated entity IDs |
| `name` | `Program A` | Fills `{program}`, and is used in the text of `active_program` and `queue` |

**Generated entities.** The table is in README.md. Where each one's value
comes from:

| Platform key | Source → write |
|--------------|----------------|
| `start` | → `38` |
| `seasonal_adjust` | `30` → `31`. 5–200 %, from the TM2 manual. |
| `running` | `3B 00` byte 9 |
| `frequency` | page `1P` byte 5. The option index is the wire byte. |
| `watering_day` (+ `day:`) | page `1P` byte 0, one bit each. Used by `Custom days`. |
| `cycle_days` | page `1P` byte 1 |
| `days_until_next` | page `1P` byte 2 |
| `start_time` (+ `slot:`) | page `6P`, one u16 each |
| `start_time_enabled` (+ `slot:`) | page `6P`. Off = `FFFF`. Needs that slot's `start_time`. |

Every write to page `1P` or `6P` needs `allow_writes`. Bytes 3
(`permanentDaysOff`) and 4 (unknown) of page `1P` have no entities; they are
always copied from a fresh read (6.5).

### 4.4 Platform keys (controller-wide)

The table of controller entities is in README.md. Each key takes that
domain's normal entity schema. Device classes, units, icons and entity
categories are set the way core sets them, from the kind table.

### 4.5 Actions, conditions and triggers

**Custom actions.** There are only three, because each needs a runtime
argument that no entity can take:

| Action | Arguments | Command |
|--------|-----------|---------|
| `rainbird.zone.start` | `zone` (zone ID or templatable number), `duration` (templatable time), `id` (hub, optional) | `39` |
| `rainbird.zone.queue` | same | `4B` |
| `rainbird.send_raw` | `data` (templatable bytes) | Any payload; the reply arrives through `on_frame`. **Experimental only.** Validation fails without `experimental: true`. |

Durations are rounded **up** to whole minutes. A zone outside 1–255 or a
duration outside 1 ms to 255 minutes is refused, never clamped: a negative
minutes value in a template wraps to a huge duration, and clamping that would
water for over four hours (`plan_zone_run()` in `entity_writes.cpp`).

**Everything else uses ESPHome's own automations:**

| Task | Use |
|------|-----|
| Stop watering | `button.press: <stop id>` |
| Run a program | `button.press: <start button id>` |
| Set rain delay or seasonal adjust | `number.set` |
| Sync the clock | `button.press: <sync_clock id>`, or `datetime.datetime.set` on `controller_clock` |
| Re-read the controller | `button.press: <refresh id>` |
| "Is watering?" | `binary_sensor.is_on: <watering id>` |
| "Is zone running?" | `switch.is_on: front_lawn_valve_switch` |
| "Zone started/stopped" | `on_turn_on` / `on_turn_off` on the valve switch |
| "Program running?" | `binary_sensor.is_on` on its `running_binary_sensor` |

An entity can be `internal: true` if it's only wanted for automations.

**Custom triggers:** `on_nak` and `on_frame` (4.1). They report protocol
events that no entity represents.

---

## 5. Repository and code layout

```
rainbird-esphome/
├── README.md                    # user documentation; entity tables generated
├── PLAN.md, HISTORY.md          # this design, and how it was planned and built
├── SPEC.md, WIRE.md             # the protocol and the circuit
├── components/rainbird/
│   ├── __init__.py              # component metadata; wires the modules below together
│   ├── classes.py               # the C++ classes codegen names, and each kind's class
│   ├── hub_schema.py            # hub schema; validates the entities zones/programs generate
│   ├── codegen.py               # hub and entity code generation
│   ├── actions.py               # zone.start, zone.queue, send_raw
│   ├── const.py                 # config keys shared by the Python modules
│   ├── entity_kinds.py          # THE table of entity kinds (5.3), used by hub and platforms
│   ├── expand.py                # zones/programs → entity declarations (no ESPHome import)
│   ├── bindings.py              # a platform entry's key → kind, zone, program, index
│   ├── models.py                # the model: limits and program letters
│   ├── platform.py, schemas.py  # the platform modules and entity schemas, from the kind table
│   ├── wire.py                  # final validation: transmitter, receivers, RMT memory
│   ├── entity_checks.py         # final validation: duplicate controls, dependencies
│   ├── automation.h             # on_nak / on_frame triggers
│   ├── rainbird.h/.cpp          # hub: ESPHome glue, publishing, logging, clock sync, actions
│   ├── rainbird_entities.h/.cpp # one class per writeable domain (5.4)
│   ├── entity_kind.h            # generated: the EntityKind enum and the kind table rows
│   ├── model_limits.h           # generated: zone, program and value limits
│   ├── entity_values.h/.cpp     # pure: the kind table, poll plan per binding, values to show
│   ├── entity_writes.h/.cpp     # pure: what a write from an entity or action becomes
│   ├── session.h/.cpp           # pure: startup, probes, polls, commands, edits, events
│   ├── schedule_editor.h/.cpp   # pure: one schedule edit: read, patch, write, confirm
│   ├── link_core.h/.cpp         # pure: transport queue, timeouts, retries, reply matching
│   ├── controller_state.h/.cpp  # pure: field IDs and one optional cell per value
│   ├── clock_sync.h/.cpp        # pure: when the controller clock needs setting
│   ├── countdown.h              # pure: the remaining-time countdown and the end time
│   ├── elapsed.h                # pure: wrap-safe elapsed-time check
│   ├── manchester.h/.cpp        # pure: raw timings ↔ bits, polarity/phase lock
│   ├── sip_frame.h/.cpp         # pure: frame encoder, frame hunter, checksum, escapes
│   ├── sip_commands.h/.cpp      # pure: request builders, typed reply parsers, quirk helpers
│   ├── model.h                  # pure: limits, capabilities, page fields and codecs
│   ├── model_esp_tm2.h/.cpp     # pure: the ESP-TM2
│   └── sensor/, binary_sensor/, text_sensor/, number/, switch/, button/, select/,
│       datetime/                # each an __init__.py generated from the kind table
├── packages/
│   └── ha-actions.yaml          # api: actions, includable as a remote package
├── examples/
│   ├── minimal.yaml, full.yaml
│   └── sniffer.yaml, sniffer.md # listen_only + monitor receiver + dump: raw; bench notes
├── tools/
│   ├── gen_kinds.py             # entity_kinds.py, models.py → entity_kind.h, model_limits.h
│   ├── gen_docs.py              # entity_kinds.py → the entity tables in README.md
│   └── raw_to_fixture.py        # "Received Raw:" log lines → tests/captures files
├── tests/
│   ├── unit/                    # host-compiled tests of the pure C++ layers (section 8)
│   ├── python/                  # pytest: expansion, IDs, generated files, config validation
│   ├── captures/                # pulse-timing captures for the replay test
│   └── components/rainbird/
│       ├── common.yaml          # the component config, pins as substitutions
│       ├── test.esp32-idf.yaml  # core naming: test.<platform>-<framework>.yaml
│       ├── test.esp32-ard.yaml
│       ├── test.esp32-c3-idf.yaml   # RMT receive with ping-pong (6.1)
│       └── validate.esp32-idf.yaml  # config-only checks, never compiled
└── .github/workflows/ci.yaml
```

**Why the entity classes are at the component root.** The hub creates zone
and program entities even when the user writes no `switch: - platform:
rainbird` block. Sources inside `switch/` are only compiled when that platform
is loaded, so the classes would be missing. The platform folders therefore
hold only `__init__.py`. The C++ sits at the root behind `#ifdef USE_SWITCH`
(etc.) guards, and the hub `AUTO_LOAD`s the domains it generates.

The test files follow core's component-test layout: a shared `common.yaml`,
included as a package from one file per platform and framework, with pins
passed in as substitutions. Each one loads the component from the checkout
with `external_components: [{ source: { type: local, path: ../../../components } }]`,
so CI tests the code in the PR rather than a published tag.

### 5.1 Architecture rules

These were fixed before the first commit, because they are cheap to follow
from the start and expensive to retrofit.

```
remote_transmitter / remote_receiver           (ESPHome)
        │ raw timings (+mark / −space µs)
        ▼
manchester ──► sip_frame ──► sip_commands      (pure C++: wire format)
                                   │ typed replies
                                   ▼
                     model  ──►  ControllerState   (pure C++: meaning)
                                   │ changed fields
                                   ▼
link_core ◄── session ──► entity_values/_writes  (pure C++: behaviour)
                 ▲  │ events, readout
                 │  ▼
          RainbirdComponent ──► entities          (ESPHome)
```

1. **One owner per job.**
   - `sip_commands` owns the wire format: bytes ↔ typed structs.
   - The `Model` owns meaning: limits, which requests to poll, the page map
     (`locate()`, used for reading pages and editing them) and value codecs.
   - `Session` owns the controller conversation and `ControllerState`, and
     reports what happened through one event queue.
   - `LinkCore` owns scheduling: queue, timeouts and retries.
   - `entity_values` owns the kind table and what each entity shows and
     polls; `entity_writes` owns what each write becomes. The table itself is
     generated from `entity_kinds.py` (5.3). `RainbirdComponent` only
     publishes, logs and carries a write plan out.
   - Replies are parsed in exactly one place; the model never re-parses
     bytes.
2. **Includes only point down the stack.** The pure layers (everything in
   the diagram except the ESPHome rows) include only the standard library.
   The host tests enforce this, because they compile those files without
   ESPHome.
3. **`ControllerState` is the only copy of controller data.** Each value is
   empty until a reply brings it, and is emptied again when the link goes
   down. `Session::revision()` moves on whenever anything may have changed;
   the hub then walks its entities and publishes only a value that differs
   from the one it last published, so nothing is republished when a poll
   changes nothing. The only values outside it are local settings that the
   controller doesn't hold (run durations, test duration).
4. **Every write from an entity goes through one function**,
   `RainbirdComponent::submit()`. What the write means is decided by the pure
   `plan_write()`; `submit()` hands the plan to the session. The model gate
   (D12), `allow_writes`, `experimental` and capability checks live in
   `Session::control()` and `Session::edit_schedule()` and nowhere else.
5. **Protocol quirks are named functions in `sip_commands`, written once.**
   Examples: `nak_is_unsupported(code)` (bit 0), `QUEUE_PROGRAM_NONE`
   (`FF` = manual or idle), and `run_time_page(zone)` /
   `run_time_offset(zone, program)` for the 1-based zone mapping. Call sites never repeat the arithmetic.
6. **No statics or globals.** `MULTI_CONF` allows several hubs, so all state
   hangs off component instances. That includes decoder state, which is kept
   per receiver.

### 5.2 Style conventions (as core uses them)

These were checked against the ESPHome developer docs and the 2026.9.1 source
on 2026-10-20 (section 11).

**C++**
- Namespace `esphome::rainbird`. The namespace must match the component name.
- `this->` everywhere, members with a trailing underscore, `snake_case`
  methods, and `final` on concrete classes, as core does.
- **The log tag** is declared with `ESPHOME_LOG_TAG(TAG, "rainbird");` at
  namespace scope in `rainbird.cpp`, the only file that logs. That macro arrives in 2026.10.0, so a shared
  header carries core's documented fallback for older releases:
  ```cpp
  #ifndef ESPHOME_LOG_TAG
  #define ESPHOME_LOG_TAG(name, tag) static const char *const name = tag
  #endif
  ```
  `TAG` is only passed to log macros.
- **`dump_config()`** combines related fields into one `ESP_LOGCONFIG` call
  with `\n` separators, within the 512-byte buffer.
- **Logging levels.** No logging for entity state changes; the base classes
  already log published states at VERBOSE. Frame dumps are VERBOSE, through
  `format_hex_pretty_to` into a stack buffer. They move to `ESP_LOGV_HEX`
  once `min_version` includes it.
- Read-only entities are ESPHome's own classes, registered with the hub
  together with their binding (`add_sensor(entity, kind, zone, program,
  index)`). Each domain's code sits inside `#ifdef USE_SENSOR` (etc.) guards.
- **Status** uses `status_set_warning()` / `status_clear_warning()` while the
  link is down. The named form of `status_momentary_warning` is deprecated
  (removal in 2027.5.0) and is not used.
- **Timekeeping** in `loop()` uses `App.get_loop_component_start_time()`, not
  `millis()`. Timer IDs are `static constexpr uint32_t`, never strings.
- **Time conversion** uses ESPHome's `ESPTime` helpers and the `time_id`
  component's timezone. There is no date arithmetic of our own beyond
  unpacking the wire format.
- clang-format using ESPHome's `.clang-format`.

**Python**
- `DOMAIN = "rainbird"`. Core CI is starting to require it, and it's harmless
  on older releases.
- `CONF_*` constants from `esphome.const` wherever one exists.
  `remote_base.CONF_TRANSMITTER_ID` / `CONF_RECEIVER_ID` are used for the wire
  references.
- `CONF_RAINBIRD_ID = "rainbird_id"` for platforms, with
  `cv.GenerateID(CONF_RAINBIRD_ID): cv.use_id(RainbirdComponent)`.
- `CODEOWNERS = ["@sjwright"]`, `MULTI_CONF = True`,
  `DEPENDENCIES = ["remote_receiver"]`.
- `AUTO_LOAD` for every entity domain the hub can generate: `switch`,
  `number`, `button`, `binary_sensor`, `select`, `datetime`.
- **Final validation** (`FINAL_VALIDATE_SCHEMA`) reads the referenced
  transmitter and receiver configs with `fv.full_config` and
  `get_declaring_config`, the way components that use `uart` check the bus
  settings (6.1). The entity checks that run in the same pass are in
  `entity_checks.py`.
- **Entity schemas** come from the domain helpers: `switch.switch_schema(...)`,
  `number.number_schema(..., unit_of_measurement=...)`,
  `button.button_schema(...)`, `select.select_schema(...)`,
  `datetime.time_schema(...)` / `datetime.datetime_schema(...)`. The entities
  are created with `new_switch`, `new_number(config, min_value=, max_value=,
  step=)` and so on, and parented with `cg.register_parented()`.
- The hub is wired to the wire layer with
  `cg.add(hub.set_transmitter(await cg.get_variable(...)))` and a
  `RainbirdReceiver` listener per receiver. From 2026.10 the listener list is
  sized at codegen, so it is added with `remote_base.add_listener()` where
  that exists and `register_listener()` otherwise.
- **Not used yet, to adopt later** (11.3): `cg.add_pin()`,
  `cg.add_variable()`, `sub_switches()` and the `Apply*` classes.
- **No PlatformIO-specific hooks** (`cg.add_library`, `platformio_options`).
- ruff formatting.

**Entities:** names in sentence case, matching current ESPHome guidance.
Names never repeat the device name; ESPHome and Home Assistant add it.

### 5.3 The entity-kind table (`entity_kinds.py`)

Every entity the component can create is one row:

```python
EntityKind(
    KindId.RUN_TIME,              # the number the C++ EntityKind enum uses
    "run_time",                   # platform key
    "number",                     # domain
    Scope.ZONE,                   # CONTROLLER | ZONE | PROGRAM
    "{zone} {program} run time",  # default name
    fan_out=FanOut.PROGRAM,       # NONE | PROGRAM | DAY | SLOT
    write=Write.SCHEDULE_PAGE,    # NONE | LOCAL | COMMAND | SCHEDULE_PAGE
    limits=(0, 65535, 1),
    unit="min",
    key_override="run_time_numbers",  # hub key, when it is not <key>_<domain>
    field="RUN_TIME",             # the ControllerState field it shows and edits
    doc="One per program; a platform entry also takes `program:`.",
)
```

Rows also carry the entity defaults (`device_class`, `state_class`,
`entity_category`, `icon`, `accuracy`, `mode`) as plain strings, so the module
imports without ESPHome and the host tests can load it.

The table drives everything else:

- the hub's `zone_defaults` / `program_defaults` keys, including their
  `_<domain>` and plural spellings (`hub_key()`);
- the platform schemas and their keys (`platform.py`, `schemas.py`);
- the expansion of zones and programs into entity declarations (`expand.py`);
- the binding passed to each entity (`kind`, `zone`, `program`, `index`),
  worked out in one place (`bindings.py`) for codegen and for the duplicate
  and dependency checks alike;
- the C++ side: `tools/gen_kinds.py` writes the `EntityKind` enum and the
  rows of the C++ kind table into `entity_kind.h`, so the two tables cannot
  disagree. Number limits come from `models.py`, which the same tool writes
  into `model_limits.h` for the session's range checks;
- the poll plan: which `Field`s are used decides which requests are polled
  (6.4);
- the entity tables in README.md (`tools/gen_docs.py`).

A Python test fails when a generated file is out of date, so CI catches a row
changed without running the tools.

Adding an entity kind means one row, a run of both tools and, if it needs a
new field, one parser case and one reader in `entity_values.cpp`.

Whether a kind needs `experimental` or `allow_writes` is not in the table: the
session decides that per command (5.1 rule 4).

### 5.4 C++ classes

**One class per writeable domain.** `write_state()` / `control()` are pure
virtual, so each writeable domain needs one subclass. Each class carries a
binding (`kind`, `zone`, `program`, `index`) and forwards writes to
`RainbirdComponent::submit()`:

- `RainbirdSwitch`: valve, watering day, start-time enabled, global
  rain-sensor bypass, irrigation enabled
- `RainbirdNumber`: run time, seasonal adjust, cycle days, days until next
  run, rain delay, station delay
- `RainbirdSetting`: run duration and test duration. These are local numbers the
  controller never holds, so they have no parent and submit nothing.
- `RainbirdButton`: queue, program start, stop, advance, test all zones, sync
  clock, refresh
- `RainbirdSelect`: frequency
- `RainbirdTime`: a start time
- `RainbirdDateTime`: the controller clock

Read-only entities use the base classes directly: `sensor::Sensor`,
`binary_sensor::BinarySensor`, `text_sensor::TextSensor`.

Local settings (`RainbirdSetting`) restore their value the same way
core's `template` number does with `restore_value`. When ESPHome replaces the
preference-hash helpers (removal is scheduled for 2027.1.0), we follow
whatever core `template` number moves to.

`RainbirdComponent` is a `PollingComponent`. `setup()` builds the session's
config from the registered bindings and starts it; `update()` only enqueues a
poll cycle; `loop()` runs the link and is disabled when the link is idle;
after every frame, loop and update it drains the session's events, publishes
what changed and checks the clock. It owns one `EspTm2Model` and one
`Session`, which owns `LinkCore` and `ControllerState`.

---

## 6. Design details

### 6.1 Wire layer: `remote_transmitter` and `remote_receiver`

**Transmit.** The hub holds a `remote_base::RemoteTransmitterBase *`. To send
what `Session::next_transmission()` returns:

1. Call `transmit()` and `get_data()`, then `set_carrier_frequency(0)`.
2. Set the data to `manchester_encode()`'s timings: one 208 µs mark and one
   208 µs space per bit, ordered by the bit's value, with adjacent equal
   levels merged.
3. Call `perform()`.

Both copies of a duplicated 2-byte request go into the same data buffer, so
they leave back to back without depending on how `send_times` behaves on a
given ESP-IDF version. The transmitter is non-blocking on ESP32, and the
RMT encoder refills hardware memory from RAM, so frame length doesn't matter.
The end-of-transmit time is calculated from the bit count, so no completion
callback is needed. Polarity comes from the transmitter's pin `inverted:`.

**Receive.** A small adapter implements
`remote_base::RemoteReceiverListener::on_receive(RemoteReceiveData)` and is
registered on the receiver. `remote_receiver` calls it from its own `loop()`
with raw timings: positive values are marks and negative values are spaces,
in µs. Each call is one burst, ended by `idle`. The adapter passes the
timings to that receiver's decoder (6.2) and returns `true` when it decodes a
valid frame. A second adapter, with its own decoder state, serves
`monitor_receiver_id`.

On the original ESP32, `remote_receiver` passes the bare GPIO number to the RMT
driver and never runs the pin's `setup()`. That `setup()` is what moves an RTC
pad (GPIO 0, 2, 4, 12 to 15, 25 to 27, and 32 to 39) off the RTC mux, so a
receiver on one of those pins can see a flat line. After `remote_receiver`
setup, `release_receiver_pads_()` calls `rtc_gpio_deinit` and
`gpio_input_enable` on each receiver pin. ESP-IDF declares `rtc_gpio_deinit`
only when `SOC_RTCIO_PIN_COUNT > 0`. In 5.5.5 that count is 0 on the C2 and
the C3, which have no RTC mux, so the call is compiled out there.
`gpio_input_enable` only turns the input buffer on. Drop the workaround once
ESPHome fixes the receiver and `min_version` includes that release.

**Will the receive buffer hold a whole reply?** Checked against the ESP-IDF
5.5.4 RMT driver source on this machine. In the worst case a Manchester byte
is 16 level runs, or 8 RMT symbols, so the longest reply (42 bytes) needs up
to 336 symbols.

| Variant | RMT receive refills mid-frame (ping-pong) | Needed |
|---------|-------------------------------------------|--------|
| ESP32-S3, -C3, -C5, -C6, -H2, -P4 | yes | `receive_symbols: 384`; `rmt_symbols` default is fine |
| ESP32 (original) | no | also `rmt_symbols: 384` (6 of 8 memory blocks; transmit keeps 64 symbols) |
| ESP32-S2 | no, and only 256 symbols in total | **rejected**: a worst-case reply can't fit |
| ESP32-C2, -C61 | no RMT | **rejected** |

**Final validation** looks at the referenced components' configs and fails
with the exact fix when:

- the transmitter has a carrier (`carrier_duty_percent` below 100 % with a
  carrier frequency);
- the transmitter is blocking (`non_blocking: false`);
- the receiver's `receive_symbols` is below 384;
- on an original ESP32, the receiver's `rmt_symbols` is below 384;
- `idle` is outside 1–20 ms. It must be longer than any in-frame pulse
  (416 µs) and shorter than the controller's ~30 ms turnaround;
- `filter` is over 100 µs, which would eat 208 µs half-bits;
- `rmt_symbols` across all RMT users exceeds the chip's memory, which
  ESP-IDF would otherwise only report at boot;
- the variant is S2, C2 or C61.

**Debugging comes for free.** `dump: raw` on the receiver logs every burst as
`Received Raw: …`. `tools/raw_to_fixture.py` turns those lines into replay
fixtures. A second `remote_receiver` on J4 is the sniffer's monitor input.

### 6.2 Decoding (pure `manchester.cpp` + `sip_frame.cpp`)

Input is one burst of raw timings. Each burst is decoded on its own: a valid
frame never contains a gap as long as `idle`, so a frame can't be split
across bursts.

1. Classify each duration as 1 or 2 half-bits, allowing ±30 % on the
   208/416 µs widths. Anything else ends the burst, unless it is the idle
   tail `remote_receiver` appends, which also swallows the last half-bit.
2. Expand into a stream of half-bit levels. The sign gives the level.
3. Until polarity is locked, decode the burst four ways, both polarities ×
   both phase offsets, and keep the one that yields the most frames. The
   phase offset absorbs the "extra half-bit" seen on replies.
4. Decode high-low = 1 and low-high = 0, MSB first, into bits. The frame
   hunter (`find_frames()`) looks for `F5 F5 1C` at every bit; a reply's
   extra leading `F5` sits in front of that same sequence. It then reads
   `len` bytes, the checksum and `1D`: plain first, then with byte-stuffing
   undone (SPEC 2.4). A frame cut off by the end of the burst is dropped.
5. **Frames are split by length, not by gaps.** As soon as a frame
   validates, the hunter looks for the next preamble on the very next bit.
   The duplicate 2-byte requests arrive in one burst with no gap, and both
   are recovered.
6. **Polarity locks per receiver**, after 3 consecutive good frames. The two
   directions are known to have opposite polarity at the LNK2's own pins.
   Five bad frames in a row unlock it again.

### 6.3 Transport (`LinkCore`, `Session`) and startup

The link is two pure classes and a thin hub. `LinkCore` is the transport: the
queue, one request in flight, timeouts, retries, reply matching and duplicate
counting. `Session` is the conversation: startup, probes, the poll plan, the
model gate, commands, schedule edits and events. `RainbirdComponent` sends,
receives and publishes. Both pure classes run against a scripted controller
in `tests/unit/test_session.cpp`.

Every wait is "elapsed since a start time ≥ a delay" in unsigned arithmetic
(`elapsed.h`). A signed comparison against a stored deadline refuses to send
for up to 24.8 days after a long idle spell or the millis() wrap.

- **`update()` only enqueues.** `PollingComponent::update()` adds a poll
  cycle unless one is pending. Only `loop()` transmits, so a poll can never
  collide with a command in flight.
- **Fast poll** while watering and back uses `set_update_interval()` and
  restarts the poller. There is no second timer.
- **`loop()` only runs when there is work**: while a request is queued or in
  flight. When the queue is empty, it calls `disable_loop()`. `update()`,
  `submit()` and the receive adapter turn it back on with `enable_loop()`.
  The receive adapter already runs in loop context, so no interrupt-safe
  wake is needed.
- **One request in flight at a time.** Each step is: send, then wait up to
  the timeout after the end of the first copy, then match the reply. A reply
  matches if its opcode is the request opcode + `0x80`, or an ACK/NAK whose
  echo byte is the request opcode. Paged requests also check the selector
  the reply echoes (`reply_matches()`), so a late reply to an earlier page
  is not taken for this one.
- **Duplicate replies.** A second identical reply within the window is
  dropped and counted, which answers SPEC question 5.
- **Errors.** A timeout or bad frame is retried, then counted as a link
  error. Commands and schedule writes go as one copy and are never retried:
  the controller may act on every copy it hears, and a lost reply does not
  mean it did nothing. `send_raw` is never retried either (4.1). Three
  failures in a row set `connected` off, and polling stays at
  `update_interval`. While it is off, commands and schedule edits are
  refused, and any still queued are dropped: a command must not run minutes
  later, or on a different controller. `ControllerState` is emptied at the
  same moment, keeping only which zones exist: a zone shown running may have
  stopped since. The hub shows what it can as unknown (6.5), and everything,
  the schedule included, is read again when the controller answers. After
  five minutes down, the hub starts again at step 1 of the
  startup below, in case the controller was replaced or reflashed.
- **Only the answer to the request in flight updates state** in active mode.
  A late or duplicate reply is older than what that request brings.
- **A schedule edit checks each answer's kind**: the page itself for the read
  and the confirming read, an ACK for the write. It works from that reply's
  bytes, never a page kept from an earlier read. Another master or the front
  panel can still change the page between the read and the write; the
  controller has no conditional write, so the confirming read is the check.
- **A clock set is time, then date**: `11` goes first, and `13` only once
  `11` is acknowledged. A new date on a time the controller did not take
  would be a clock a day out near midnight. One clock set at a time.
- **Schedule edits run one at a time and wait their turn.** An edit asked
  for while another runs is held, up to eight of them; a later value for a
  field already waiting replaces the earlier one. Each reads its page
  afresh, so two edits to one page do not undo each other, and one that
  fails does not stop those behind it. A lost link drops them all.
- **NAKs** are logged and raise `on_nak`. The command is marked unsupported
  for the session when `nak_is_unsupported(code)` is true (bit 0 set). A NAK
  without bit 0 is logged as a likely encoding bug.
- **Queue priority:** user commands, then follow-up reads, then the poll
  cycle.
- **Follow-up reads** after any ACKed control command are the model's *short
  poll* (6.4), the same list the fast poll uses. That way a `0005` without
  `4C` gets its own fallback reads. Commands in a row, such as the clock's
  time and date, share one follow-up. The fast poll then runs for 30 s.
  Entities show the confirmed state within about 500 ms; nothing is
  published optimistically.
- The `remaining_time` countdown uses `set_interval` with a `constexpr`
  numeric ID, only while a zone is running.

**Startup**

0. The handshake, when `handshake` is on (SPEC 5.0): `06` with our
   challenge, then `07` with our answer to the controller's. A controller
   that refuses `06` has none, and startup carries on.
1. `02` model and protocol. If the model ID isn't accepted (D12), an error
   is logged, the hub stays read-only, and `model` reports the mismatch.
2. `0B` firmware, `05` serial, `03 00` available zones.
3. `04` probes for the six optional commands (`4C`, `4A`, `42`, `4B`,
   `49`, `3A`). If `04` itself is refused with a NAK and no probe is
   answered, every optional command is assumed until its own NAK (bit 0)
   switches it off. A probe that only times out assumes nothing: the
   fallback polls run, and it is asked again with a later slow poll.
   **The probe results are the
   only source of capability information.** The model ID is used for the
   D12 gate and for nothing else, so there are no per-variant feature tables
   in code.
4. Read the schedule pages that configured entities use.
5. The normal poll cycle starts. With `clock_sync`, each clock reading from
   then on is checked (6.5).

On reaching running, the log shows the model and firmware, what each `04`
probe answered, and the schedule pages read. Every 10 minutes it shows the
link counters: replies, duplicate replies (SPEC question 5), unexpected
frames and timeouts.

**Modes without startup.** `update_interval: never` is manual mode: no
identification and no polling, only `send_raw`; the bench loopback uses it.
`listen_only` never transmits: every valid reply updates state (in active
mode only the matched one does), so status entities work passively alongside
an LNK2, and `connected` means "a valid frame in the last 60 s".

Zones that the `03` mask reports as missing get a warning when the link
reaches running, and commands for them are refused. ESPHome has no
per-entity availability, so their entities stay as they are.

After five minutes with the link down, startup runs again from step 1 with
the old capabilities, model gate and controller state cleared.

### 6.4 Poll plan

The model maps each `ControllerState` field to the request that fills it.
The hub polls only for fields that a configured entity uses:

| Fields | Request | When |
|--------|---------|------|
| Zones running | `3F 00` | Short poll |
| Clock, rain delay, rain sensor, irrigation enabled | `4C` if supported, else `10`, `12`, `36`, `3E`, `48` for the ones in use | Short poll |
| Running zone, program, time left, queued count | `3B 00` | Short poll |
| Queue entries | `3B 01` | Short poll, if the `queue` text, the `advance` button or a queue-mode valve (with `experimental`) is configured |
| Seasonal adjust | `30 00`–`02` | Slow poll: the first cycle, then every 4th |
| Schedule timestamp | `4A 01` | Slow poll, if supported and any schedule entity or `schedule_changed` exists |
| Schedule pages | `20 00 PP` for the pages that configured entities use | At startup, when `4A 01` changes, every `refresh_interval`, on `refresh`, and after the link returns |

The **short poll** is the first four rows. A config with only zone switches
polls one request per cycle and never reads a schedule page. One full idle
cycle is a handful of requests at about 150–200 ms each, so the bus is free
more than 95 % of the time. One cycle runs at a time: `update()` does not
queue another while one is pending.

### 6.5 Entity behaviour notes

**Zone switches**

- **On** sends `39` (or `4B` in `queue` mode) with that zone's
  `run_duration_number` value. The switch turns on once `3F` confirms it.
- **Off, `start` mode:** sends `40` (stop all) if this zone is running.
- **Off, `queue` mode:** for the running zone, sends `42` with the next queued
  zone's number (advance) so the rest of the queue carries on. This only
  applies with `experimental: true` on a controller that supports `42`;
  otherwise it sends `40`.
- **Off for a zone that is only queued:** nothing is sent and a warning is
  logged. The TM2 has no "remove from queue" command.
- **Switching on another zone in `start` mode** replaces the running zone, as
  the controller does. The other switch turns off after the next poll.

**Refused writes.** A write that is refused, at once or after a NAK or a
failed schedule edit, logs a warning and publishes nothing new: the entity
still holds the controller's value, which never changed. Home Assistant drops
its pending change, so a toggle springs back. Numbers also republish their
value, because `Number::publish_state` does not drop repeats; a switch's
does, so republishing a switch would change nothing. A failed schedule edit
also re-reads the schedule, in case a write whose acknowledgement was lost
landed after all.

**Schedule edits.** Every schedule entity is bound to a *page field*: page,
byte offset, width and bit mask, plus a value encoding from the model. One
generic routine (`ScheduleEditor`, driven by the session) handles every
schedule write, one edit at a time (6.3):

1. Re-read the page.
2. Change only the bits of the edited field. Every other byte is kept from
   that read, including page `1P` byte 3 (`permanentDaysOff`) and byte 4
   (unknown). A page is never rebuilt from entity state.
3. **Log the old and new page in hex at INFO level** before sending
   `21 00 PP …`. The log is then a backup and undo record from the first
   write onwards.
4. Re-read to confirm. A mismatch logs an error and publishes the value read
   back.

**Start-time slots.** Each slot has a time entity and an enabled switch.

- Turning the switch off writes the model's "off" value (`FFFF` on the TM2).
- Turning the switch on writes the time entity's value.
- Changing the time while the slot is on writes it straight away. While the
  slot is off, the new time is held in RAM only.
- After a reboot, an off slot's time entity has no value until it is set.
  This keeps the single-source-of-truth rule without exceptions.

**Link loss.** When the link goes down the session forgets what it read
(6.3), and the hub shows every entity that can as unknown: sensors and
controller-backed numbers publish NaN, binary sensors lose their state, and
`ends_at` goes empty. Switches, selects, times and the other text sensors
have no such state and keep their last value; `connected` says how far to
trust them. Local settings and the link diagnostics are left alone. The first
reading after the link returns is published even when it equals the old one.

**`remaining_time` and `ends_at`.** `remaining_time` counts down locally every
second between readings (`Countdown`), re-anchored on each one because a
paused controller repeats its value. `ends_at` is the same thing as one
timestamp, now plus the time left, which Home Assistant counts down to by
itself. It is republished only when it moves by more than two seconds
(`EndTime`): the controller paused, or another zone started. It needs
`time_id`.

**`active_program`** shows the program name, `Manual` (`FF` with a zone
running) or `Idle`.

**`schedule_changed`.** The `4A 01` value is seconds since 2000-01-01 in
*controller local time* (SPEC 5.3). It is converted to a UTC timestamp with
`ESPTime` and the timezone of `time_id`, so the sensor needs `time_id`, and
config validation fails without it. Whether the controller really stores
local time is still an open question (SPEC 8.11).

**Clock sync** stays built in, because only C++ can compare the controller
clock against `time_id` before writing. Each new reading is compared once,
moved on by its age, with date and time as one instant, so a few seconds
either side of midnight never looks like a day. A sync writes `11` and then
`13`; it waits while local midnight is within 5 s, so the controller cannot
pass midnight between the two. After a sync, the next one waits a minute.
Users who want more frequent syncs can add `time: on_time_sync:` →
`button.press` on `sync_clock`.

**Rain sensor.** Polarity is unconfirmed (SPEC 8.1). The binary sensor
publishes `value != 0` as on. Users can add `filters: [invert]`, and the
default is fixed once confirmed.

---

## 7. The model

There is one model, the ESP-TM2, and the code says so directly rather than
through an interface nobody else implements.

### 7.1 Python (`models.py`)

`ESP_TM2` is a `Model`: its name, 12 zones, 4 start slots, program letters
`ABC`, and the value ranges from the manual (seasonal adjust 5–200 %, rain
delay up to 14 days, runs up to 255 minutes). It is used to:

- reject any other `model:` value, with a link to the issue tracker;
- check zone numbers, program letters and number ranges at validation;
- give the C++ side the same limits, through the generated `model_limits.h`.

The model IDs a TM2 can report (`0005`, `000A`, `010A`, `0014`, `0015`) are in
`model_esp_tm2.cpp`, which checks the `02` reply for the D12 gate.

### 7.2 C++ (`EspTm2Model`)

`EspTm2Model` is a concrete class with no virtuals. `Session`,
`ScheduleEditor` and `entity_values` take it by reference. It owns meaning:

- `limits()` and `accepts(model_id)`;
- `short_poll()` and `slow_poll()`: the requests for the fields in use (6.4);
- `locate(field, zone, program, index)`: the page map. Reading a page and
  editing one both use it and nothing else, so the two cannot disagree;
- `codec(field)` with `read_codec()` / `write_codec()`: the value encodings,
  such as minutes ↔ start slot with `FFFF` for off;
- `apply_reply()`: maps an already-parsed `Reply` onto `ControllerState`.
  Parsing happens only in `sip_commands` (5.1 rule 1);
- `note_support()`: records a `04` probe answer in `Capabilities`;
- `patch()`: copies a page and changes one field, for the schedule editor.

`ControllerState` is sized for the TM2 (`STATE_ZONES`, `STATE_PROGRAMS`,
`STATE_STARTS`, from `model_limits.h`).

**A second model.** The other Rain Bird families differ in zone and program
counts, and the LCR family in encodings too (custom/odd/even/cyclic order,
10-minute start resolution, `0x90` for off). Adding one means a second model
class and, at that point, an interface over the methods above; `model.h`
already holds the types they share. No stub exists for a model nobody has
tested, in Python or C++: it would encode guesses.

---

## 8. Testing and CI

| Layer | How it's tested |
|-------|-----------------|
| Frame codec, checksum | Host unit tests with doctest. Vectors are every frame in SPEC.md, plus corrupted copies with bad length, checksum or end byte. Also two frames back to back in one burst with no gap. |
| Manchester codec | Host tests. Synthesise raw timings (+mark / −space) from bytes, add jitter, invert them, add the leading half-bit, then decode. Also round-trip with the encoder. |
| Replay | `test_replay.cpp` feeds every file in `tests/captures/` (`Received Raw:` bursts from `dump: raw`, converted by `tools/raw_to_fixture.py`) through the full pipeline. Expected payloads are stored beside each capture. |
| SIP parsers and model | Host tests using the `[test]` and `[wire]` examples in SPEC.md. Also tests that a schedule edit changes only the bits of its field, and that every quirk helper (5.1 rule 5) is right. |
| Link and session | `test_link.cpp`, `test_session.cpp`, `test_handshake.cpp`: the pure classes against a scripted controller (`fake_controller.h`), with time passed in. |
| Entity reads and writes | `test_entity_values.cpp` and `test_entity_writes.cpp`: what each kind shows, and what each write becomes, including every refusal. |
| Untrusted input | `test_fuzz.cpp`: random and damaged timings, bits and payloads through every decode path, the session and the entity layer, with fixed seeds. |
| Expansion and IDs | pytest on `entity_kinds.py` and the hub expansion: defaults merge, `false`, string shorthand, placeholders, generated IDs (D9). |
| Generated files | pytest fails when `entity_kind.h`, `model_limits.h` or the README tables are out of date (5.3). |
| Schema and codegen | `esphome compile` on every `tests/components/rainbird/test.*.yaml`, and `esphome config` on `validate.*.yaml` and the examples. `test_config.py` checks that each final-validation rule (6.1) fires, and that `send_raw` without `experimental`, an unknown model, and S2/C2 targets are rejected. It is skipped where `esphome` is not installed. |
| Lint | ruff, clang-format check, yamllint |
| Hardware | On the real controller; HISTORY.md has the phases |

**Host test build.** `tests/unit/Makefile` compiles the pure C++ sources and
the tests to object files, so only what changed is rebuilt:

```bash
make -C tests/unit -j test          # g++ -std=gnu++17 -Wall -Wextra -Werror
make -C tests/unit -j test SAN=1    # the same under AddressSanitizer and UBSan
```

Because no ESPHome include path is given, this also enforces 5.1 rule 2. The
sanitizer build is what gives the fuzz test its teeth: a fault there is
usually silent in the plain build.

GitHub Actions runs every row except hardware on each push and PR: both host
builds, pytest, the linters, and the compile matrix.

**ESPHome version matrix:**
- `min_version` (currently 2026.9.0) and the stable releases since. All must
  pass.
- A weekly scheduled build against ESPHome `dev`. It fails the scheduled run
  when it breaks, which is the early warning of an upcoming API change.

The ESPHome version is installed with `pip install esphome==<version>`.

Release tags (`v0.x.y`) are created only when CI passes.

---

## 9. Delivery phases

The hardware phases, their exit criteria and the SPEC questions each one
settles are in [HISTORY.md](HISTORY.md). As a question is settled, SPEC.md is
updated, the matching `[inferred]` tag becomes `[wire]`, and the default or
`experimental` gate is adjusted.

---

## 10. Risks

| Risk | Mitigation |
|------|------------|
| The controller is fussy about transmit timing or the idle level | RMT gives exact timing. Phase 1 measures the LNK2's real waveform first, and `inverted:` on the transmitter pin handles polarity. |
| Optocoupler edges are slow enough to skew pulse widths | ±30 % classification. Use an external pull-up of a few kΩ rather than the internal ~45 kΩ one. |
| RMT memory runs out on an original ESP32 (512 symbols) | Receive 384 + transmit 64 fits. A monitor receiver needs 384 too, so the sniffer setup with two receivers needs an S3. Final validation adds up every RMT user's `rmt_symbols` and fails early. S3, C3 and C6 don't have this constraint, thanks to RX ping-pong. |
| `remote_receiver` or `remote_transmitter` change behaviour in a future ESPHome | CI against `min_version`, the latest stable release and weekly `dev`. Final validation pins the settings we depend on. Replay tests use the receiver's own raw format, so a format change shows up immediately. |
| A bad schedule write corrupts the controller's programs | Writes are off by default. The model gate (D12) applies. Every write goes through one read-modify-write routine with confirmation and hex logging, and each page type is tested on its own. |
| Generated entity count is high (12 zones × up to 7, 3 programs × up to 21) | Only valve and duration per zone, and run button and seasonal adjust per program, are generated by default. Everything else is opt-in. |
| ESPHome API churn in code generation helpers and the `datetime` platform | Same CI matrix. Compatibility shims are used where the docs give one (`ESPHOME_LOG_TAG`). |
| Two hubs or an LNK2 on the same controller | Not supported while transmitting. `listen_only` is the only supported way to share the bus. |

---

## 11. ESPHome compatibility

This section was checked on **2026-10-20** against the user docs at
esphome.io, the developer docs at developers.esphome.io, the ESPHome source
at tag `2026.9.1` (the latest stable release at that date), and the ESP-IDF
5.5.4 RMT driver. Re-check it before each minor release of this component.

### 11.1 Baseline

- **ESPHome `min_version: 2026.9.0`.** CI tests nothing older, so nothing
  older is claimed. It can be lowered later if a user needs it and CI passes.
- **ESP32 with ESP-IDF by default.** ESP-IDF is now ESPHome's default
  framework for every ESP32 variant (recommended version 5.5.5). Arduino
  (3.3.11) is opt-in and only exists for ESP32, -S2, -S3 and -C3, so it is
  tested but secondary.
- **`esp32: variant:`** in every example. The docs say `board:` is "no longer
  recommended".
- **Supported variants:** every ESP32 with RMT except the S2 (6.1).

### 11.2 Current practices this plan follows

| Area | Current practice | Where in this plan |
|------|------------------|--------------------|
| External components | `components/<name>/` layout. `github://user/repo@ref` shorthand or the full `type: git` form. `refresh` defaults to `1day`; `never` suits a pinned tag. | 2, 5 |
| Remote packages | `packages:` with `github://user/repo/path@ref` | 2, 3 |
| Wire layer | `remote_base`: `CONF_TRANSMITTER_ID` / `CONF_RECEIVER_ID`, `RemoteTransmitterBase::transmit()` → `get_data()` → `mark`/`space` → `perform()`, `RemoteReceiverListener::on_receive(RemoteReceiveData)`, `register_listener`. This is the `climate_ir` pattern. | 6.1 |
| HA actions | `api: actions:` with `action:` keys. `services:` still works but is the old name. | 3 |
| Entity automations | `button.press`, `number.set`, `switch.is_on`, `on_turn_on` instead of custom equivalents | 4.5 |
| OTA | `ota: - platform: esphome` (list form) | 3 |
| Sub-devices | `esphome: devices:` / `areas:`, with `device_id` on each entity | 4.2 |
| Final validation | `FINAL_VALIDATE_SCHEMA` checking the referenced components, as `uart` users do | 5.2, 6.1 |
| Loop control | `disable_loop()` when idle, `enable_loop()` when work arrives | 6.3 |
| Scheduler | `App.get_loop_component_start_time()` instead of `millis()`; numeric timer IDs; nothing that blocks for 50 ms or more | 5.2, 6.3 |
| Logging | `ESPHOME_LOG_TAG` with fallback; combined `dump_config` lines; no state-change logs | 5.2 |
| Status | `status_set_warning()` without a name argument | 5.2 |
| Entity IDs | No use of entity `object_id` (deprecated in 2026) | 4.2 |
| Optional domains | `#ifdef USE_<DOMAIN>` guards | 5.2 |
| Tests | `tests/components/<name>/test.<platform>-<framework>.yaml` plus `common.yaml` and `validate.*.yaml` | 5, 8 |
| Toolchain | Nothing PlatformIO-specific; ESPHome's PlatformIO toolchain is deprecated (removal in 2027.2.0) | 5.2 |

### 11.3 Newer APIs deliberately not used yet

These exist only in releases newer than `min_version`, or only on `dev`. Each
can be adopted once `min_version` reaches the release that ships it.

| API | Available from | Adopt when |
|-----|----------------|------------|
| `ESPHOME_LOG_TAG` | 2026.10.0 | Used now through the documented `#ifndef` fallback. Drop the fallback when `min_version` ≥ 2026.10.0. |
| `cg.add_pin()` / `cg.add_variable()` | 2026.11.0 | Replace `cg.get_variable()` + `cg.add()` |
| `sub_switches()` / `ApplySwitch` etc. | `dev` only (not in 2026.9.1) | Possibly for buttons that make one hub call. Zone entities keep `Rainbird*` classes, because they carry a binding. |
| `DOMAIN` lint requirement | `dev` | Already defined, so nothing to change |
| `ESP_LOGV_HEX` | Not stated in the docs; check the release notes | Replace `format_hex_pretty_to` buffers once `min_version` includes it |

### 11.4 Sources

- [External components](https://esphome.io/components/external_components/)
- [Packages](https://esphome.io/components/packages/)
- [ESP32 platform](https://esphome.io/components/esp32/) (default framework, `variant` vs `board`, toolchain deprecation)
- [Core configuration](https://esphome.io/components/esphome/) (`min_version`, `devices`, `areas`)
- [Native API](https://esphome.io/components/api/) (`actions:`)
- [Datetime](https://esphome.io/components/datetime/)
- [Sprinkler](https://esphome.io/components/sprinkler/)
- [Component architecture](https://developers.esphome.io/architecture/components/) and [advanced topics](https://developers.esphome.io/architecture/components/advanced) (loop control, scheduler, preferences)
- [Logging best practices](https://developers.esphome.io/architecture/logging/best_practices/)
- [Component tests](https://developers.esphome.io/architecture/ci/component_tests/)
- ESPHome source at `2026.9.1`:
  - `remote_base/__init__.py` and `remote_base.h` (transmit/listen API);
  - `remote_receiver/__init__.py` and `remote_receiver_rmt.cpp` (RMT only on RMT chips, `receive_symbols`, `idle` limits);
  - `remote_transmitter/__init__.py` and `remote_transmitter_rmt.cpp` (carrier off at 100 %, non-blocking, inversion);
  - `climate_ir/__init__.py` (the `transmitter_id` / `receiver_id` pattern);
  - `esp32/__init__.py` (framework versions);
  - `ld2410/ld2410.h` (`SUB_*` with `#ifdef` guards).
- ESP-IDF 5.5.4 `esp_driver_rmt/src/rmt_rx.c` and `soc_caps.h` (RX ping-pong per variant)

---

## 12. Schedule of work

The working order (steps S0 to S10), the effort estimate, the parts list and
the build notes are in [HISTORY.md](HISTORY.md), with the notes from the four
design reviews. What the build notes recorded about the design is now in the
sections above.
