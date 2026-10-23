# Implementation plan: `rainbird` ESPHome component

This plan describes how to build an ESPHome external component that replaces
the Rain Bird LNK2 WiFi module. The component talks to the controller directly
over its accessory port, using the protocol in [SPEC.md](SPEC.md).

## Goals

- **Import straight from GitHub** as an `external_components` source.
- **One short YAML block per controller.** Each irrigation zone is named once,
  and all of that zone's entities are generated from the name. Every
  generated entity can still be customised or turned off.
- **Look and behave like a core ESPHome component.** That covers config keys,
  file layout, C++ and Python idioms, entity schemas, actions and
  `dump_config()` output. Wherever ESPHome already provides a piece, use it
  rather than writing our own.
- **Fully implement the ESP-TM2** (every TM2 variant). Other Rain Bird models
  are stubbed in the model registry, so a config that names one gets a clear
  "not implemented" error, and the C++ `Model` interface is the place to add
  them.
- **Be safe by default.** Commands that SPEC.md marks as unverified, mainly
  schedule writes, stay disabled until the user turns them on.

## Non-goals (v1)

- Implementing other Rain Bird models. They are stubbed only.
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

Every decision here is final unless marked "open". Each one shapes the YAML in
section 3.

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
| D9 | **Generated IDs are public API from v0.1.0**: `<zone id>_<key>`, e.g. `front_lawn_valve_switch` | Users will write lambdas and automations against them, so changing the scheme is a breaking change. A snapshot test (section 8) guards it. |
| D10 | **The wire layer is ESPHome's `remote_transmitter` and `remote_receiver`**, referenced by `transmitter_id` and `receiver_id`. | This is how core's `climate_ir` platforms do it, and how bus-based components take a `uart_id`. ESPHome already provides non-blocking RMT transmit with the carrier off, RMT receive with its own interrupt handler, buffering, overflow warnings and raw dumps, and it maintains that code across ESP-IDF releases. The rainbird component never touches RMT or IRAM. It does call `rtc_gpio_deinit` on the receiver pins after `remote_receiver` setup, because that component leaves original-ESP32 RTC pads on the RTC mux (6.1). Section 6.1 checks the limits. |
| D11 | **Unverified commands are off by default**: `schedule: allow_writes: false`, `experimental: false`. `rainbird.send_raw` also needs `experimental: true`. | SPEC.md section 8 lists the unverified commands. A raw-send action left in a config would bypass both gates. |
| D12 | **An unrecognised model ID makes the hub read-only** | If `02` returns an ID outside the model's list, status polling continues but every control and write command is refused. `accept_model_ids:` overrides this. |
| D13 | **Prefer ESPHome's own entity actions, conditions and triggers.** Custom actions exist only where a runtime argument is needed. | `button.press`, `number.set`, `switch.is_on` and `on_turn_on` already cover stopping, rain delay, program runs, state checks and "zone started". What's left: `rainbird.zone.start`/`queue` (zone number from Home Assistant), `rainbird.send_raw`, and the `on_nak`/`on_frame` triggers (4.5). |
| D14 | **Layering and ownership rules** (5.1) are fixed now | Wire format, protocol semantics, scheduling and entities each have exactly one owner. Every write passes through one function. `ControllerState` is the only copy of controller data. |

---

## 2. Using the component

The repository is **https://github.com/sjwright/rainbird-esphome**. It uses
the layout ESPHome expects for external components: the component lives in
`components/rainbird/`, next to a README, example configs and reusable
packages.

**Recommended: a pinned release.** A tag never moves, so there is nothing to
re-check:

```yaml
external_components:
  - source: github://sjwright/rainbird-esphome@v0.1.0
    components: [rainbird]
    refresh: never
```

**The same, written out in full.** Use this form for a fork or a private
mirror:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/sjwright/rainbird-esphome
      ref: v0.1.0
    components: [rainbird]
    refresh: never
```

**Tracking development.** This uses the default branch, checked for updates
once a day (ESPHome's default `refresh`):

```yaml
external_components:
  - source: github://sjwright/rainbird-esphome@main
    components: [rainbird]
    refresh: 1d
```

**Working on the component itself:**
`source: { type: local, path: ../rainbird-esphome/components }`.

Every release is tagged `vMAJOR.MINOR.PATCH`. The release notes state the
minimum ESPHome version, which also appears as `esphome: min_version` in the
examples.

The smallest working config, on an original ESP32:

```yaml
remote_transmitter:
  pin: GPIO26                # → optocoupler 2 → J4 (controller RX)
  carrier_duty_percent: 100% # plain baseband, no carrier

remote_receiver:
  pin: GPIO27                # ← optocoupler 1 ← J2 (controller TX)
  idle: 4ms
  receive_symbols: 384       # the longest reply needs up to 336
  rmt_symbols: 384           # original ESP32 only (6.1)

rainbird:
  model: ESP-TM2
  zones:
    - { zone: 1, name: Front lawn }
    - { zone: 2, name: Back lawn }
```

With only one transmitter and one receiver in the config, `transmitter_id` and
`receiver_id` resolve automatically, as they do for any ESPHome component. If
a transmitter or receiver setting won't work for this protocol, config
validation names it and gives the value to use (6.1).

That config gives:

- each listed zone a valve switch and a run-duration number;
- each program a "Run" button and a seasonal-adjust number. When `programs:`
  is left out, all of the model's programs are created with default names
  ("Program A" to "Program C"). Write `programs: []` to create none.

Zones work the same way. Leaving out `zones:` creates zones 1–12 named
"Zone 1" to "Zone 12", and `zones: []` creates none. Add platform blocks
(section 3) for controller-wide status.

Home Assistant entity IDs follow from the entity names together with the
node's `friendly_name`. For example, a node with friendly name "Irrigation"
and a zone named "Front lawn" gives `switch.irrigation_front_lawn`.

> **Turning off any zone switch stops all watering.** The controller only has
> a stop-all command (`40`). See 6.5 for the details, including the
> experimental `queue`-mode alternative.

**Home Assistant actions without copying YAML.** The repository ships a
package that defines the `api:` actions. It can be included straight from
GitHub:

```yaml
packages:
  rainbird_actions: github://sjwright/rainbird-esphome/packages/ha-actions.yaml@v0.1.0
```

---

## 3. Full proposed YAML

This example uses every option and entity, with comments.

```yaml
substitutions:
  name: irrigation
  friendly_name: Irrigation

esphome:
  name: ${name}
  friendly_name: ${friendly_name}
  min_version: 2026.9.0      # the oldest release CI builds against (section 11)

esp32:
  variant: esp32             # `board:` is no longer recommended. ESP32-S2 is not supported (6.1).
  # framework:               # ESP-IDF is the default; Arduino is opt-in
  #   type: arduino

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

ota:
  - platform: esphome

time:
  - platform: homeassistant
    id: ha_time

external_components:
  - source: github://sjwright/rainbird-esphome@v0.1.0
    components: [rainbird]
    refresh: never

packages:
  rainbird_actions: github://sjwright/rainbird-esphome/packages/ha-actions.yaml@v0.1.0

# ---------------------------------------------------------------------------
# Wire layer: ESPHome's own components (see WIRE.md for the circuit).
# Any free GPIO works; avoid strapping pins.
# ---------------------------------------------------------------------------
remote_transmitter:
  id: rb_tx
  pin: GPIO26                # → optocoupler 2 → J4 (controller RX). `inverted:` works
  carrier_duty_percent: 100% # required: no carrier
  non_blocking: true         # the ESP32 default; stated to silence the warning

remote_receiver:
  id: rb_rx
  pin:                       # ← optocoupler 1 ← J2 (controller TX)
    number: GPIO27
    mode:
      input: true
      pullup: false          # fit an external pull-up of a few kΩ on the optocoupler output
  idle: 4ms                  # ends a burst; frames themselves are split by length
  filter: 50us
  receive_symbols: 384
  rmt_symbols: 384           # original ESP32: hardware memory must hold a whole reply
  # dump: raw                # pulse timings for test fixtures (section 8)

# ---------------------------------------------------------------------------
# Hub
# ---------------------------------------------------------------------------
rainbird:
  id: rainbird_controller
  model: ESP-TM2
  transmitter_id: rb_tx
  receiver_id: rb_rx
  # accept_model_ids: [0x000A]   # only for a TM2 variant missing from the built-in list

  update_interval: 15s       # idle poll cycle (the LNK2 polls every 12.5–15 s)

  # What turning a zone switch on does: start (39, replaces the running zone)
  # or queue (4B, adds it to the end of the queue)
  valve_switch_mode: start

  # Time source, used for clock sync and to convert controller-local timestamps
  time_id: ha_time
  clock_sync:
    max_drift: 30s           # checked on every clock reading; daylight saving shows as drift

  schedule:
    refresh_interval: 1h     # re-read pages; also re-read when 4A 01 changes
    allow_writes: false      # D11: schedule entities are read-only until enabled

  experimental: false        # enables 49, 3A, 42 and rainbird.send_raw

  # -------------------------------------------------------------------------
  # Zones
  # -------------------------------------------------------------------------
  # NOTE: turning off any zone switch stops ALL watering (command 40).
  #
  # Defaults apply to every zone. Any key listed here is generated for every
  # zone, and `{zone}` is replaced by that zone's name. With no zone_defaults,
  # each zone gets valve_switch and run_duration_number.
  zone_defaults:
    valve_switch:
      name: "{zone}"
      icon: mdi:sprinkler-variant
    run_duration_number:
      name: "{zone} duration"
      initial_value: 10        # minutes, used by the valve switch and queue button
      restore_value: true
      entity_category: config
    running_binary_sensor:
      name: "{zone} running"
      disabled_by_default: true
    run_time_numbers:        # plural: one per program, "{program}" available
      name: "{zone} {program} run time"
      entity_category: config
    queue_button:
      name: "{zone} queue"
      disabled_by_default: true
    fault_binary_sensor:     # unavailable until 3D is decoded (SPEC 8.4)
      name: "{zone} fault"
      disabled_by_default: true

  zones:
    - { zone: 1,  id: front_lawn, name: Front lawn }
    - { zone: 2,  id: back_lawn,  name: Back lawn }
    - { zone: 3,  name: Side path }
    - zone: 4
      id: veggies
      name: Veggie patch
      run_duration_number:
        initial_value: 5     # merged over the zone_defaults entry
      sensor_bypass_switch: "{zone} ignores rain"   # name-only shorthand
    - zone: 5
      name: Drip line
      valve_switch:
        icon: mdi:water-pump
        on_turn_on:          # ESPHome's own trigger replaces "zone started"
          - logger.log: "Drip line started"
      run_time_numbers: false   # turns off an entity for this zone only
    - { zone: 6,  name: Pots }
    - { zone: 7,  name: Front beds }
    - { zone: 8,  name: Back beds }
    - { zone: 9,  name: Orchard }
    - { zone: 10, name: Nature strip }
    - { zone: 11, name: Greenhouse }
    - { zone: 12, name: Spare, disabled: true }   # no entities generated

  # -------------------------------------------------------------------------
  # Programs (A, B, C on the TM2)
  # -------------------------------------------------------------------------
  # `{program}` is replaced by the program's name. With no program_defaults,
  # each program gets start_button and seasonal_adjust_number.
  program_defaults:
    start_button:
      name: "Run {program}"
    running_binary_sensor:
      name: "{program} running"
    seasonal_adjust_number:
      name: "{program} seasonal adjust"     # 10–200 %
    frequency_select:
      name: "{program} frequency"
      entity_category: config
    watering_day_switches:   # plural: 7, "{day}" = Monday … Sunday
      name: "{program} {day}"
      entity_category: config
    cycle_days_number:
      name: "{program} cycle days"
      entity_category: config
    days_until_next_number:
      name: "{program} days until next run"
      entity_category: config
    start_time_datetimes:    # plural: 4 time entities, "{slot}" = 1–4
      name: "{program} start {slot}"
      entity_category: config
    start_time_switches:     # plural: 4, slot on/off; required with start_time_datetimes
      name: "{program} start {slot} enabled"
      entity_category: config

  programs:
    - { program: A, id: lawns,  name: Lawns }
    - { program: B, id: beds,   name: Garden beds }
    - { program: C, id: drip,   name: Drip }

  # -------------------------------------------------------------------------
  # Triggers with no entity equivalent
  # -------------------------------------------------------------------------
  on_nak:
    - logger.log:
        format: "Controller NAK for 0x%02X, code 0x%02X"
        args: [command, code]
  # on_frame:                # payload of every valid reply, for protocol work
  #   - lambda: ESP_LOGD("rb", "%s", format_hex_pretty(payload).c_str());

# ---------------------------------------------------------------------------
# Controller-wide entities. Every key is optional. `rainbird_id` is only
# needed when there is more than one hub.
# ---------------------------------------------------------------------------
sensor:
  - platform: rainbird
    remaining_time:
      name: Time remaining     # seconds; counts down locally between polls
    active_zone:
      name: Active zone        # 0 = none
    queued_zones:
      name: Zones queued
    response_time:
      name: Controller response time
      entity_category: diagnostic
    link_errors:
      name: Controller link errors  # timeouts and bad frames
      entity_category: diagnostic

binary_sensor:
  - platform: rainbird
    watering:
      name: Watering           # device_class: running
    rain_sensor:
      name: Rain sensor        # device_class: moisture; use `filters: [invert]` if reversed
    irrigation_enabled:
      name: Irrigation enabled
    connected:
      name: Controller connected   # device_class: connectivity, diagnostic

text_sensor:
  - platform: rainbird
    active_program:
      name: Active program     # "Lawns", "Manual" or "Idle"
    queue:
      name: Queue              # "Front lawn 24:48, Back lawn 20:00, …"
    model:
      name: Controller model   # "ESP-TM2 (000A), protocol 9.12"
    firmware_version:
      name: Controller firmware
    serial_number:
      name: Controller serial
    schedule_changed:
      name: Schedule last changed   # ISO 8601 timestamp, from 4A 01; needs time_id

number:
  - platform: rainbird
    rain_delay:
      id: rain_delay           # used by the HA action package
      name: Rain delay         # 0–14 days (37)
    station_delay:
      name: Delay between zones   # seconds; needs schedule.allow_writes
    test_duration:
      name: Test duration      # minutes per zone for the test button

switch:
  - platform: rainbird
    rain_sensor_bypass:
      name: Ignore rain sensor    # page 00; needs schedule.allow_writes
    irrigation_enabled:
      name: Irrigation            # 49; needs experimental: true

button:
  - platform: rainbird
    stop:
      id: stop_watering
      name: Stop watering      # 40
    advance:
      name: Next zone          # 42 + next queued zone; upgraded TM2 + experimental
    test_all_zones:
      name: Test all zones     # 3A; experimental
    sync_clock:
      name: Sync controller clock
    refresh:
      name: Refresh from controller
      entity_category: diagnostic

datetime:
  - platform: rainbird
    controller_clock:
      name: Controller clock   # type: datetime; setting it sends 11 + 13
```

**`packages/ha-actions.yaml`** (shipped in the repository) defines the Home
Assistant actions. Only the zone actions are custom; the rest use ESPHome's
own entity actions:

```yaml
api:
  actions:
    - action: run_zone
      variables: { zone: int, minutes: int }
      then:
        - rainbird.zone.start:
            zone: !lambda "return zone;"
            duration: !lambda "return minutes * 60 * 1000;"
    - action: queue_zone
      variables: { zone: int, minutes: int }
      then:
        - rainbird.zone.queue:
            zone: !lambda "return zone;"
            duration: !lambda "return minutes * 60 * 1000;"
    - action: set_rain_delay
      variables: { days: int }
      then:
        - number.set: { id: rain_delay, value: !lambda "return days;" }
    - action: stop_watering
      then:
        - button.press: stop_watering
```

---

## 4. Configuration reference

### 4.1 Hub (`rainbird:`)

`MULTI_CONF = True`: several hubs are allowed (one per controller).

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `id` | ID | auto | `RainbirdComponent` |
| `model` | enum | **required** | `ESP-TM2` is implemented. Every other model in the registry is rejected with "not implemented yet" (section 7). |
| `transmitter_id` | ID of `remote_transmitter` | the only one | Checked at final validation (6.1) |
| `receiver_id` | ID of `remote_receiver` | the only one | Checked at final validation (6.1) |
| `listen_only` | bool | `false` | No transmitter is used. State is decoded from replies to another master, such as an LNK2. |
| `monitor_receiver_id` | ID of `remote_receiver` | – | A second receiver on J4, so requests from another master are decoded too. Development only. |
| `accept_model_ids` | list of int | – | Extra model IDs to treat as this model (D12) |
| `update_interval` | time | `15s` | Idle poll cycle (section 6.4) |
| `valve_switch_mode` | `start` / `queue` | `start` | Whether turning a zone switch on sends `39` or `4B`. Also changes what "off" does (6.5). |
| `time_id` | ID of `time` | – | Needed by `clock_sync` and by the `schedule_changed` sensor |
| `clock_sync.max_drift` | time | `30s` | Sync when the controller drifts further than this, at least 5 s. Checked on every clock reading, from boot on; a UTC-offset change moves local time an hour, so it shows as drift. Needs `time_id`. |
| `schedule.refresh_interval` | time | `1h` | Full re-read of the schedule pages that configured entities use, at least 1 min. The `4A 01` timestamp triggers re-reads in between, except for the change our own write makes. |
| `schedule.allow_writes` | bool | `false` | Lets schedule entities write `21 …` pages |
| `experimental` | bool | `false` | Allows `49`, `3A`, `42` and `rainbird.send_raw` |
| `zone_defaults`, `zones` | see 4.2 | all 12 zones | `zones: []` creates none |
| `program_defaults`, `programs` | see 4.3 | all programs | `programs: []` creates none |
| `on_nak` | automation | | Variables `command`, `code` |
| `on_frame` | automation | | Variable `payload` (`std::vector<uint8_t>`) for every valid reply. The monitor receiver's requests are only logged. |

**Internal constants, not options.** These start at the values below. Phase 2
measures the real behaviour, then the constants are fixed. Only one that turns
out to need tuning per installation would become an option.

| Constant | Initial value | Notes |
|----------|---------------|-------|
| Request timeout | 400 ms | From the end of the first copy of a request. The longest reply (42 bytes) is about 140 ms on the wire, plus about 30 ms of controller turnaround. |
| Retries | 2 | For timeouts and bad frames on reads. A NAK is never retried. Commands, schedule writes and `send_raw` are never retried, so a lost reply cannot run a command the controller already acted on. |
| Duplicate 2-byte requests | on | Copies the LNK2 habit (SPEC 2.3) for reads and `send_raw`. Commands and schedule writes go as one copy, because the controller may act on each. Phase 2 A/B-tests whether it's needed. |
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

**Generated entities** (one key per entity, or one per program for plural
keys):

| Hub key | Platform key | Domain | Generated by default | Default name | Source → write | Notes |
|---------|--------------|--------|----------------------|--------------|----------------|-------|
| `valve_switch` | `valve` | switch | yes | `{zone}` | `3F` → `39`/`4B`. Off: `40` (or `42`, 6.5). | |
| `run_duration_number` | `run_duration` | number (min, 1–255) | yes | `{zone} duration` | local setting | Used by `valve_switch` and `queue_button`. 255 is the `39` limit. |
| `running_binary_sensor` | `running` | binary_sensor | no | `{zone} running` | `3F` | Same data as the switch, for read-only dashboards |
| `queue_button` | `queue` | button | no | `{zone} queue` | → `4B` | |
| `run_time_numbers` | `run_time` (+ `program:`) | number ×programs (min) | no | `{zone} {program} run time` | pages `80`–`85` → `21 00 8N` | Writing needs `allow_writes` |
| `sensor_bypass_switch` | `sensor_bypass` | switch | no | `{zone} ignores rain sensor` | page `20` → `21 00 20` | Writing needs `allow_writes` |
| `fault_binary_sensor` | `fault` | binary_sensor | no | `{zone} fault` | `3D` | Stays unavailable until `3D` is decoded (SPEC 8.4) |

**How each entity's settings are worked out:**

1. If the zone sets the key to `false`, the entity is not generated.
2. Otherwise, the zone's own settings for that key are merged over the
   `zone_defaults` entry. A zone that sets a key absent from `zone_defaults`
   gets that entity anyway.
3. A key given as a plain string is read as `{name: <string>}`, as in core's
   `sprinkler`.
4. The placeholders in the name are filled in.
5. If no `id` is given, the generated ID is `<zone id>_<key>`. Plural keys
   add a suffix: the program letter, day or slot number, e.g.
   `front_lawn_run_time_numbers_a`. These are ESPHome config IDs for
   lambdas, not the deprecated entity `object_id`, which the component never
   sets or reads.
6. The result becomes an ordinary platform entry. For example,
   `valve_switch` on zone 1 becomes
   `switch: - platform: rainbird, zone: 1, valve: {...}`. It is checked
   against that platform's normal entity schema, so every standard option
   (`icon`, `internal`, `entity_category`, `disabled_by_default`,
   `device_id`, `on_turn_on`, `filters`, …) works.

**Writing zone entities by hand** gives exactly the same result:

```yaml
switch:
  - platform: rainbird
    zone: 4
    valve:
      name: Veggie patch
    sensor_bypass:
      name: Veggie patch ignores rain
number:
  - platform: rainbird
    zone: 4
    run_duration:
      name: Veggie patch duration
  - platform: rainbird
    zone: 4
    program: A
    run_time:
      name: Veggie patch lawns run time
```

A valve switch or queue button needs its zone's `run_duration` number, a
start-slot switch needs that slot's `start_time`, and `test_all_zones` needs
`test_duration`: each control runs with the other entity's value. Final
validation fails a config that leaves one out, rather than letting the
control refuse every press.

### 4.3 Programs

**Program keys:**

| Key | Default | Notes |
|-----|---------|-------|
| `program` | **required** | `A`–`C` on the TM2. Stored as 0–2. |
| `id` | `<hub id>_program_<a>` | Prefix for the program's generated entity IDs |
| `name` | `Program A` | Fills `{program}`, and is used in the text of `active_program` and `queue` |

**Generated entities:**

| Hub key | Platform key | Domain | Generated by default | Default name | Source → write |
|---------|--------------|--------|----------------------|--------------|----------------|
| `start_button` | `start` | button | yes | `Run {program}` | → `38` |
| `seasonal_adjust_number` | `seasonal_adjust` | number (%, 10–200) | yes | `{program} seasonal adjust` | `30` → `31`. The 10 % floor is a pyrainbird docstring, not a verified limit; phase 4 probes it (SPEC 8.14). |
| `running_binary_sensor` | `running` | binary_sensor | no | `{program} running` | `3B 00` byte 9 |
| `frequency_select` | `frequency` | select | no | `{program} frequency` | page `1P` byte 5. Options come from the model: `Custom days`, `Cyclic`, `Odd days`, `Even days`. |
| `watering_day_switches` | `watering_day` (+ `day:`) | switch ×7 | no | `{program} {day}` | page `1P` byte 0, one bit each. Used by `Custom days`. |
| `cycle_days_number` | `cycle_days` | number (days) | no | `{program} cycle days` | page `1P` byte 1 |
| `days_until_next_number` | `days_until_next` | number (days) | no | `{program} days until next run` | page `1P` byte 2 |
| `start_time_datetimes` | `start_time` (+ `slot:`) | datetime (time) ×4 | no | `{program} start {slot}` | page `6P`, one u16 each |
| `start_time_switches` | `start_time_enabled` (+ `slot:`) | switch ×4 | no | `{program} start {slot} enabled` | page `6P`. Off = `FFFF`. Must be enabled together with `start_time_datetimes`. |

Every write to page `1P` or `6P` needs `allow_writes`. Bytes 3
(`permanentDaysOff`) and 4 (unknown) of page `1P` have no entities; they are
always copied from a fresh read (6.5).

### 4.4 Platform keys (controller-wide)

| Platform | Keys |
|----------|------|
| `sensor` | `remaining_time`, `active_zone`, `queued_zones`, `response_time`, `link_errors` |
| `binary_sensor` | `watering`, `rain_sensor`, `irrigation_enabled`, `connected` |
| `text_sensor` | `active_program`, `queue`, `model`, `firmware_version`, `serial_number`, `schedule_changed` (ISO 8601, device class `timestamp`) |
| `number` | `rain_delay`, `station_delay`, `test_duration` |
| `switch` | `rain_sensor_bypass`, `irrigation_enabled` |
| `button` | `stop`, `advance`, `test_all_zones`, `sync_clock`, `refresh` |
| `datetime` | `controller_clock` |

Each key takes that domain's normal entity schema. Device classes, units,
icons and entity categories are set the way core sets them.

### 4.5 Actions, conditions and triggers

**Custom actions.** There are only three, because each needs a runtime
argument that no entity can take:

| Action | Arguments | Command |
|--------|-----------|---------|
| `rainbird.zone.start` | `zone` (zone ID or templatable number), `duration` (templatable time), `id` (hub, optional) | `39` |
| `rainbird.zone.queue` | same | `4B` |
| `rainbird.send_raw` | `data` (templatable bytes) | Any payload; the reply arrives through `on_frame`. **Experimental only.** Validation fails without `experimental: true`. |

Durations are rounded **up** to whole minutes and clamped to 1–255. Zone
references in both zone actions go through one resolver, so the validation
rules are the same everywhere.

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
├── README.md, LICENSE, SPEC.md, PLAN.md, WIRE.md
├── components/rainbird/
│   ├── __init__.py              # component metadata; wires the modules below together
│   ├── classes.py               # the C++ classes codegen names, and each kind's class
│   ├── hub_schema.py            # hub schema; validates the entities zones/programs generate
│   ├── codegen.py               # hub and entity code generation
│   ├── actions.py               # zone_start, zone_queue, send_raw
│   ├── const.py                 # config keys shared by the Python modules
│   ├── entity_kinds.py          # THE table of entity kinds (5.3), used by hub and platforms
│   ├── expand.py                # zones/programs → entity declarations (no ESPHome import)
│   ├── models.py                # model registry: limits, accepted IDs, program letters
│   ├── platform.py, schemas.py  # the platform modules and entity schemas, from the kind table
│   ├── wire.py                  # final validation: wire settings, model limits, dependencies
│   ├── automation.h             # on_nak / on_frame triggers
│   ├── rainbird.h/.cpp          # hub: ESPHome glue, publishing, logging, clock sync, actions
│   ├── rainbird_entities.h/.cpp # one class per writeable domain (5.4)
│   ├── entity_kind.h            # pure: the EntityKind ids, in step with entity_kinds.py
│   ├── entity_values.h/.cpp     # pure: the C++ kind table, poll plan per binding, values to show
│   ├── session.h/.cpp           # pure: startup, probes, polls, commands, events
│   ├── schedule_editor.h/.cpp   # pure: one schedule edit: read, patch, write, confirm
│   ├── link_core.h/.cpp         # pure: transport queue, timeouts, retries, reply matching
│   ├── controller_state.h/.cpp  # pure: field IDs, one cell per value, changed flags
│   ├── clock_sync.h/.cpp        # pure: when the controller clock needs setting
│   ├── countdown.h              # pure: the remaining-time countdown
│   ├── elapsed.h                # pure: wrap-safe elapsed-time check
│   ├── manchester.h/.cpp        # pure: raw timings ↔ bits, polarity/phase lock
│   ├── sip_frame.h/.cpp         # pure: streaming frame parser + encoder, checksum
│   ├── sip_commands.h/.cpp      # pure: request builders, typed reply parsers, quirk helpers
│   ├── model.h                  # pure: abstract Model (limits, polls, page map, codecs)
│   ├── model_esp_tm2.h/.cpp     # pure: the one implementation
│   ├── sensor/__init__.py
│   ├── binary_sensor/__init__.py
│   ├── text_sensor/__init__.py
│   ├── number/__init__.py
│   ├── switch/__init__.py
│   ├── button/__init__.py
│   ├── select/__init__.py
│   └── datetime/__init__.py
├── packages/
│   └── ha-actions.yaml          # api: actions, includable as a remote package
├── examples/
│   ├── minimal.yaml
│   ├── full.yaml                # the YAML in section 3
│   └── sniffer.yaml             # listen_only + monitor receiver + dump: raw + send_raw
├── tools/
│   └── raw_to_fixture.py        # "Received Raw:" log lines → tests/captures files
├── tests/
│   ├── unit/                    # host-compiled tests of the pure C++ layers
│   │   ├── Makefile
│   │   ├── doctest.h
│   │   ├── test_frame.cpp, test_manchester.cpp, test_commands.cpp, test_replay.cpp
│   │   └── vectors.h            # every example in SPEC.md as a test vector
│   ├── python/                  # pytest: expansion of zones/programs, ID snapshot
│   ├── captures/                # real pulse-timing captures from phase 1 onwards
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

These are fixed now, because they are cheap to follow from the first commit
and expensive to retrofit.

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
link_core ◄── session ──► entity_values          (pure C++: behaviour)
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
   - `entity_values` owns the kind table: what each entity shows, polls and
     writes. `RainbirdComponent` only publishes and logs.
   - Replies are parsed in exactly one place; the model never re-parses
     bytes.
2. **Includes only point down the stack.** The pure layers (`manchester`,
   `sip_*`, `model*`, `controller_state`) include only the standard library.
   The host tests enforce this, because they compile those files without
   ESPHome.
3. **`ControllerState` is the only copy of controller data.** Entities never
   cache values. A reply updates fields and marks them changed. The hub then
   publishes only the entities bound to changed fields, so nothing is
   republished when a poll changes nothing. The only values outside it are
   local settings that the controller doesn't hold (run durations, test
   duration).
4. **Every write from an entity goes through one function**,
   `RainbirdComponent::submit()`, which picks the command. The model gate
   (D12), `allow_writes`, `experimental` and capability checks live in
   `Session::control()` and `Session::edit_schedule()` and nowhere else.
5. **Protocol quirks are named functions in `sip_commands`, written once.**
   Examples: `nak_is_unsupported(code)` (bit 0), `queue_program(byte)`
   (`FF` = manual or idle), `queue_seconds()` (big-endian seconds), and
   `run_time_page(zone)` / `run_time_offset(zone, program)` for the 1-based
   zone mapping. Call sites never repeat the arithmetic.
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
- **Log tags** are declared with `ESPHOME_LOG_TAG(TAG, "rainbird.link");` at
  namespace scope in each `.cpp`. That macro arrives in 2026.10.0, so a shared
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
- `SUB_SENSOR`, `SUB_BINARY_SENSOR` and `SUB_TEXT_SENSOR` setters for the
  read-only controller-wide entities, each include inside `#ifdef USE_SENSOR`
  (etc.) guards, as `ld2410` does.
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
  settings (6.1).
- **Entity schemas** come from the domain helpers: `switch.switch_schema(...)`,
  `number.number_schema(..., unit_of_measurement=...)`,
  `button.button_schema(...)`, `select.select_schema(...)`,
  `datetime.time_schema(...)` / `datetime.datetime_schema(...)`. The entities
  are created with `new_switch`, `new_number(config, min_value=, max_value=,
  step=)` and so on, and parented with `cg.register_parented()`.
- The hub is wired to the wire layer with
  `cg.add(hub.set_transmitter(await cg.get_variable(...)))` and
  `remote_base.register_listener(hub_receiver_adapter, config)`.
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
    key="run_time",               # platform key; hub key = run_time_numbers
    domain="number",
    scope=Scope.ZONE,             # CONTROLLER | ZONE | PROGRAM
    fan_out=FanOut.PROGRAM,       # NONE | PROGRAM | DAY | SLOT
    default=False,                # generated by zone_defaults when absent?
    default_name="{zone} {program} run time",
    field=Field.RUN_TIME,         # ControllerState field it reads
    write=Write.SCHEDULE_PAGE,    # NONE | LOCAL | COMMAND | SCHEDULE_PAGE
    gate=Gate.ALLOW_WRITES,       # NONE | ALLOW_WRITES | EXPERIMENTAL
    schema=lambda: number.number_schema(RainbirdNumber, unit_of_measurement=UNIT_MINUTE,
                                        entity_category=ENTITY_CATEGORY_CONFIG),
    limits=(0, 255, 1),
)
```

The table drives everything else:

- the hub's `zone_defaults` / `program_defaults` keys, including their
  `_<domain>` and plural spellings;
- the platform schemas and their keys;
- the expansion of zones and programs into platform entries;
- the C++ binding passed to each entity (`kind`, `zone`, `program`, `index`);
- the poll plan: which `Field`s are used decides which requests are polled
  (6.4);
- the docs tables in 4.2–4.4, generated by a script rather than hand-edited.

Adding an entity kind means one row and, if it needs a new field, one parser
case. There are no hand-written branches per key.

### 5.4 C++ classes

**One class per writeable domain.** `write_state()` / `control()` are pure
virtual, so each writeable domain needs one subclass. Each class carries a
binding (`kind`, `zone`, `program`, `index`) and forwards writes to
`RainbirdComponent::submit()`, which chooses the command:

- `RainbirdSwitch`: valve, sensor bypass, watering day, start-time enabled,
  global rain-sensor bypass, irrigation enabled
- `RainbirdNumber`: run time, seasonal adjust, cycle days, days until next
  run, rain delay, station delay
- `RainbirdSetting`: run duration and test duration. These are local numbers the
  controller never holds, so they have no parent and submit nothing.
- `RainbirdButton`: queue, program start, stop, advance, test all zones, sync
  clock, refresh
- `RainbirdSelect`: frequency
- `RainbirdDateTime`: start time (time) and controller clock (datetime)

Read-only entities use the base classes directly: `sensor::Sensor`,
`binary_sensor::BinarySensor`, `text_sensor::TextSensor`.

Local settings (`RainbirdSetting`) restore their value the same way
core's `template` number does with `restore_value`. When ESPHome replaces the
preference-hash helpers (removal is scheduled for 2027.1.0), we follow
whatever core `template` number moves to.

```cpp
namespace esphome::rainbird {

class RainbirdComponent final : public PollingComponent {
 public:
  void setup() override;          // queue identification requests
  void loop() override;           // run the link; disabled when idle
  void update() override;         // enqueue a poll cycle if none is pending
  void dump_config() override;

  // The single write path (5.1 rule 4)
  bool submit(const EntityBinding &binding, float value);

  // Entity registration: each entity carries its binding; the hub polls for it
  // and publishes to it.
  void add_switch(RainbirdSwitch *entity);
  void add_sensor(sensor::Sensor *entity, uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);
  /* … one per domain … */

 protected:
  void handle_events_();           // log what the session reports
  void publish_changed_();         // kind_changed() and *_value() per binding

  EspTm2Model model_;
  std::unique_ptr<Session> session_;  // owns LinkCore and ControllerState
};

}  // namespace esphome::rainbird
```

---

## 6. Design details

### 6.1 Wire layer: `remote_transmitter` and `remote_receiver`

**Transmit.** `RainbirdLink` holds a `remote_base::RemoteTransmitterBase *`.
To send a frame:

1. Call `transmit()` and `get_data()`, then `set_carrier_frequency(0)`.
2. Append one `mark(208)` / `space(208)` pair per bit, ordered by the bit's
   value. Adjacent equal levels are merged.
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
   208/416 µs widths. Anything else ends the current frame attempt.
2. Expand into a stream of half-bit levels. The sign gives the level.
3. Until polarity is locked, run 4 decoders in parallel: both polarities ×
   both phase offsets. The phase offset absorbs the "extra half-bit" seen on
   replies.
4. Decode high-low = 1 and low-high = 0, MSB first, into a **streaming frame
   parser**. It hunts for `F5 F5 1C` or `F5 F5 F5 1C`, then reads `len`
   bytes, the checksum and `1D`.
5. **Frames are split by length, not by gaps.** As soon as a frame
   validates, the parser hunts for the next preamble on the very next bit.
   The duplicate 2-byte requests arrive in one burst with no gap, and both
   are recovered.
6. **Polarity locks per receiver**, after 3 consecutive good frames. The two
   directions are known to have opposite polarity at the LNK2's own pins.
   Five bad frames in a row unlock it again.

### 6.3 Transport (`rainbird_link`) and startup

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
  echo byte is the request opcode.
- **Duplicate replies.** A second identical reply within the window is
  dropped and counted, which answers SPEC question 5.
- **Errors.** A timeout or bad frame is retried, then counted as a link
  error. Commands and schedule writes go as one copy and are never retried:
  the controller may act on every copy it hears, and a lost reply does not
  mean it did nothing. `send_raw` is never retried either (4.1). Three
  failures in a row set `connected` off, and polling backs off. While it is
  off, commands and schedule edits are refused, and any still queued are
  dropped: a command must not run minutes later, or on a different
  controller. After five minutes down, the hub starts again at step 1 of the
  startup below, in case the controller was replaced or reflashed.
- **Only the answer to the request in flight updates state** in active mode.
  A late or duplicate reply is older than what that request brings.
- **A schedule edit checks each answer's kind**: the page itself for the read
  and the confirming read, an ACK for the write. It works from that reply's
  bytes, never a page kept from an earlier read. Another master or the front
  panel can still change the page between the read and the write; the
  controller has no conditional write, so the confirming read is the check.
- **A clock set is queued whole**: `11` and `13` together, or neither.
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

1. `02` model and protocol. If the model ID isn't accepted (D12), an error
   is logged, the hub stays read-only, and `model` reports the mismatch.
2. `0B` firmware, `05` serial, `03 00` available zones.
3. `04` probes for every optional command the configured entities need
   (`4C`, `4A`, `42`, `4B`, `3D`, `49`, `3A`). **The probe results are the
   only source of capability information.** The model ID is used for the
   D12 gate and for nothing else, so there are no per-variant feature tables
   in code.
4. Read the schedule pages that configured entities use.
5. Clock check and sync if needed. Then the normal poll cycle starts.

On reaching running, the log shows the model and firmware, what each `04`
probe answered, and the schedule pages read. Every 10 minutes it shows the
link counters: replies, duplicate replies (SPEC question 5), unexpected
frames and timeouts.

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
| Clock, rain delay, rain sensor, irrigation enabled | `4C` if supported, else `10`, `12`, `36`, `3E`, `48` | Short poll |
| Running zone, program, time left | `3B 00` | Short poll |
| Queue entries | `3B 01` | While watering, if `queue` or `queued_zones` is configured |
| Seasonal adjust | `30 00`–`02` | Every 4th poll |
| Schedule timestamp | `4A 01` | Every 4th poll, if any schedule entity or `schedule_changed` exists |
| Zone faults | `3D 00` | Every 4th poll, if any `fault_binary_sensor` exists |
| Schedule pages | `20 00 PP` for the pages that configured entities use | At startup, when `4A 01` changes, and every `refresh_interval` |

The **short poll** is the first three rows. A config with only zone switches
polls two or three requests per cycle and never reads a schedule page. One
full idle cycle takes about 3 × 200 ms, so the bus is free more than 95 % of
the time.

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
generic routine in the hub handles every schedule write:

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

**`active_program`** shows the program name, `Manual` (`FF` with a zone
running) or `Idle`.

**`schedule_changed`.** The `4A 01` value is seconds since 2000-01-01 in
*controller local time* (SPEC 5.3). It is converted to a UTC timestamp with
`ESPTime` and the timezone of `time_id`, so the sensor needs `time_id`, and
config validation fails without it. Whether the controller really stores
local time is still an open question (SPEC 8.11); phase 3 checks it.

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

## 7. Model abstraction and stubs

### 7.1 Python registry (`models.py`): where the stubs live

```python
MODELS = {
    "ESP-TM2":  Model(implemented=True,  zones=12, programs=3, starts=4,
                      model_ids=[0x0005, 0x000A, 0x010A, 0x0014, 0x0015]),
    "ESP-ME":   Model(implemented=False, zones=22, programs=4, starts=6,
                      model_ids=[0x0007, 0x0107]),
    "ESP-RZXE": Model(implemented=False, zones=8,  programs=0, starts=6,
                      model_ids=[0x0003, 0x0103]),
    "ST8X-WF":  Model(implemented=False, zones=8,  programs=0, starts=6,
                      model_ids=[0x0006, 0x0008]),
    "ESP-ME3":  Model(implemented=False, zones=22, programs=4, starts=6,
                      model_ids=[0x0009]),
}
```

The registry is used to:

- reject models that aren't implemented, with a link to the issue tracker;
- check zone numbers, program letters and other ranges against the model's
  limits;
- pick the C++ model to create;
- give the runtime model-ID check (D12) its list of accepted IDs.

The other models are stubbed here and only here. Empty C++ classes would be
dead code, and they would encode guesses about models nobody has tested.

### 7.2 C++ `Model` interface

```cpp
class Model {
 public:
  virtual ~Model() = default;
  virtual const ModelLimits &limits() const = 0;
  virtual bool accepts(uint16_t model_id) const = 0;              // D12 gate
  virtual void short_poll(const Capabilities &caps, FieldSet used, RequestList &out) const = 0;
  virtual void slow_poll(const Capabilities &caps, FieldSet used, RequestList &out) const = 0;
  virtual std::optional<PageField> locate(Field field, uint8_t zone, uint8_t program, uint8_t index) const = 0;
  virtual const ValueCodec &codec(Field field) const = 0;         // e.g. minutes ↔ time, FFFF = off
  virtual const std::vector<const char *> &frequency_options() const = 0;
  virtual void apply_reply(const sip::Reply &reply, ControllerState &state) const = 0;
};
```

There is one implementation, `EspTm2Model`.

- **The model receives typed replies.** `apply_reply` maps an already-parsed
  `sip::Reply` onto `ControllerState` fields; parsing happens only in
  `sip_commands` (5.1 rule 1).
- **Schedule writes have no per-model code.** `locate()` gives the page and
  bits for a field, and the hub's generic read-modify-write does the rest.
- **Encodings come from the model.** These include the frequency option
  order, start-time conversion and the "slot off" value. The LCR family
  differs on all three (custom/odd/even/cyclic order, 10-minute start
  resolution, `0x90` for off). A future LCR model supplies its own, and the
  entity layer needs no changes.

---

## 8. Testing and CI

| Layer | How it's tested |
|-------|-----------------|
| Frame codec, checksum | Host unit tests with doctest. Vectors are every frame in SPEC.md, plus corrupted copies with bad length, checksum or end byte. Also two frames back to back in one burst with no gap. |
| Manchester codec | Host tests. Synthesise raw timings (+mark / −space) from bytes, add ±20 % jitter, invert them, add the leading half-bit, then decode. Also round-trip with the encoder. |
| Replay | `test_replay.cpp` feeds every file in `tests/captures/` (real `Received Raw:` bursts from `dump: raw`) through the full pipeline. Expected payloads are stored beside each capture. |
| SIP parsers and model | Host tests using every `[test]` and `[wire]` example in SPEC.md. Also tests that a schedule edit changes only the bits of its field, and that every quirk helper (5.1 rule 5) is right. Vectors taken from pyrainbird's raw `decode_schedule` output have 1 added to the zone numbers (SPEC 5.6). |
| Expansion and IDs | pytest on `entity_kinds.py` and the hub expansion: defaults merge, `false`, string shorthand, placeholders. A **snapshot of every generated ID** for `examples/full.yaml` guards D9. |
| Schema and codegen | `esphome compile` on every `tests/components/rainbird/test.*.yaml`, and `esphome config` on `validate.*.yaml` and `examples/*.yaml`. Negative tests check that each final-validation rule (6.1) fires, and that `send_raw` without `experimental`, an unimplemented model, and S2/C2 targets are rejected. |
| Lint | ruff, clang-format check, yamllint |
| Hardware | Phases 1–2 below, on the real controller |

**Host test build:** `tests/unit/Makefile` compiles the pure C++ sources with
the tests:

```
g++ -std=gnu++17 -Wall -Wextra -Werror -I components/rainbird
```

Because no ESPHome include path is given, this also enforces 5.1 rule 2. Run
it with:

```bash
make -C tests/unit test
```

CI runs the same command, plus `pytest tests/python`.

GitHub Actions runs every row except hardware on each push and PR.

**ESPHome version matrix:**
- `min_version` (currently 2026.9.0) and the latest stable release. Both must
  pass.
- A weekly scheduled build against ESPHome `dev`. It may fail, but each
  failure is an early warning of upcoming API changes.

The ESPHome version is installed with `pip install esphome==<version>`.

Release tags (`v0.x.y`) are created only when CI passes.

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

The plan has been through three rounds of external review. Some suggestions
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
