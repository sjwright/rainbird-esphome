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
  file layout, C++ and Python idioms, entity schemas, actions, conditions,
  triggers and `dump_config()` output.
- **Fully implement the ESP-TM2** (every TM2 variant). Other Rain Bird models
  get registry entries and C++ stubs so they can be added later without
  restructuring.
- **Be safe by default.** Commands that SPEC.md marks as unverified, mainly
  schedule writes, stay disabled until the user turns them on.

## Non-goals (v1)

- Implementing other Rain Bird models. They are stubbed only.
- Platforms other than ESP32. The physical layer is behind an interface, so
  ESP8266 or RP2040 could be added later.
- Weather-based adjustment logic. Home Assistant automations can drive the
  seasonal-adjust and rain-delay entities instead.
- Reflashing the LNK2 itself.

---

## 1. Key decisions

These are open for review. Each one shapes the YAML in section 3.

| # | Decision | Why |
|---|----------|-----|
| D1 | **`model: ESP-TM2`, not `type:`** | ESPHome uses `model:` for hardware variants (`dht`, `ili9xxx`, `ssd1306`). The value is case-insensitive, so `esp-tm2` also works. The TM2 sub-variant (`0005` / `000A` / `010A` / `0014` / `0015`) is detected at runtime with command `02`, and optional commands are enabled to match. |
| D2 | **Zones and programs are declared on the hub**, and their entities are generated from `zone_defaults` / `program_defaults` | This follows core's `sprinkler` component (`valves:` with `valve_switch`, `run_duration_number`, …). Naming a zone once gives you its switch, duration, run times and so on, which is the efficiency asked for. Sprinkler's keys take only a name string. Ours take either a name string (`valve_switch: "Front lawn"`) or a full entity schema, using `cv.maybe_simple_value(..., key=CONF_NAME)`. |
| D3 | **Controller-wide entities use normal platform blocks** (`sensor: - platform: rainbird`) with one key per entity | This follows core hub components such as `ld2410`. Users can split them into packages, and they look like any other ESPHome entity. |
| D4 | **Entity keys are named `<thing>_<domain>`.** A singular key (`valve_switch`) creates one entity. A plural key (`run_time_numbers`, `watering_day_switches`, `start_time_datetimes`) creates one entity per program, day or slot. | The same convention as `sprinkler`. The key name tells you both the entity type and how many entities it creates. |
| D5 | **Zone valves are `switch` entities** | Matches `sprinkler` and the existing Home Assistant Rain Bird integration. ESPHome's `valve` domain is an option to revisit later. |
| D6 | **Schedule fields use typed entities, never free text.** Watering days are 7 switches per program. Each start-time slot is a `datetime` time entity plus an "enabled" switch, since `FFFF` (off) can't be shown as a time. | Nothing on the write path parses text, and each entity maps to one byte or one u16 on the page. All of them are opt-in, so the entity count stays low unless asked for. |
| D7 | **Names can contain the placeholders `{zone}`, `{program}`, `{day}` and `{slot}`** | One template in `zone_defaults` or `program_defaults` names the same entity everywhere. Single braces don't clash with ESPHome's `${substitutions}`. |
| D8 | **IDs are generated predictably**: `<zone id>_<key>`, e.g. `front_lawn_valve_switch` | Lambdas and automations can reference any generated entity without declaring it. |
| D9 | **ESP32 only** (variants with RMT), with RMT for transmit and a GPIO interrupt for receive | RMT gives exact Manchester timing at no CPU cost. Interrupt-based receive handles frames of any length on every ESP32 variant (section 6.1). Variants without RMT (ESP32-C2, ESP32-C61) are rejected at config validation, using the same list `remote_transmitter` uses. |
| D10 | **Unverified commands are off by default**: `schedule: allow_writes: false`, `experimental: false`. `rainbird.send_raw` also needs `experimental: true`. | SPEC.md section 8 lists unverified commands. Until they are confirmed, the matching entities can be read but not written. A raw-send action left in a config would bypass both gates. |
| D11 | **An unrecognised model ID makes the hub read-only** | If `02` returns an ID outside the model's list, status polling continues but every control and write command is refused. That stops a TM2 page map from being written to a different controller. `accept_model_ids:` overrides this for TM2 variants missing from the list. |

---

## 2. Using the component

The repository is **https://github.com/sjwright/rainbird-esphome**. It uses
the layout ESPHome expects for external components: the component lives in
`components/rainbird/`, next to a README and example configs.

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

The smallest working config:

```yaml
rainbird:
  model: ESP-TM2
  tx_pin: GPIO26
  rx_pin: GPIO27
  zones:
    - { zone: 1, name: Front lawn }
    - { zone: 2, name: Back lawn }
```

That gives:

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
  variant: esp32             # `board:` is no longer recommended; any ESP32 with RMT works
  # framework:               # ESP-IDF is the default; Arduino is opt-in
  #   type: arduino

logger:
  level: DEBUG
  logs:
    rainbird.phy: INFO       # set to VERBOSE to see every decoded frame

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

# ---------------------------------------------------------------------------
# Hub
# ---------------------------------------------------------------------------
rainbird:
  id: rainbird_controller
  model: ESP-TM2
  # accept_model_ids: [0x000A]   # only for a TM2 variant missing from the built-in list

  # Wiring (see WIRE.md). Any free GPIO works; avoid strapping pins.
  tx_pin: GPIO26             # → optocoupler 2 → J4 (controller RX). Supports `inverted:`
  rx_pin:                    # ← optocoupler 1 ← J2 (controller TX)
    number: GPIO27
    mode:
      input: true
      pullup: false          # fit an external pull-up of a few kΩ on the optocoupler output
  rx_polarity: auto          # auto | normal | inverted

  # Development aids (normally left out). See examples/sniffer.yaml.
  # listen_only: true        # never transmit; decode traffic from an attached LNK2
  # monitor_pin: GPIO25      # second input on J4, to decode the LNK2's requests too
  # monitor_polarity: auto   # locked separately from rx_polarity
  # log_raw_edges: false     # VERBOSE: pulse timings of every frame, for test fixtures

  # Link and polling
  update_interval: 15s       # idle poll cycle (the LNK2 polls every 12.5–15 s)
  active_update_interval: 3s # used while watering and just after a command
  request_timeout: 400ms     # from end of the first copy of a request
  retries: 2
  repeat_short_requests: true   # send 2-byte requests twice, as the LNK2 does
  rx_buffer_size: 2048       # edges per receive pin

  # What turning a zone switch on does: start (39, replaces the running zone)
  # or queue (4B, adds it to the end of the queue)
  valve_switch_mode: start

  # Time source, used for clock sync and to convert controller-local timestamps
  time_id: ha_time
  clock_sync:
    max_drift: 30s           # also synced at boot and after a UTC-offset change

  schedule:
    refresh_interval: 1h     # re-read pages; also re-read when 4A 01 changes
    allow_writes: false      # D10: schedule entities are read-only until enabled

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
      sensor_bypass_switch:  # turns on an entity zone_defaults doesn't create
        name: "{zone} ignores rain"
    - zone: 5
      name: Drip line
      valve_switch:
        icon: mdi:water-pump
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
  # Triggers
  # -------------------------------------------------------------------------
  on_zone_started:
    - logger.log:
        format: "Zone %u started (%s)"
        args: [zone, zone_name.c_str()]
  on_zone_stopped:
    - logger.log:
        format: "Zone %u stopped"
        args: [zone]
  on_nak:
    - logger.log:
        format: "Controller NAK for 0x%02X, code 0x%02X"
        args: [command, code]
  # on_frame:                # raw payload of every reply, for protocol work
  #   - lambda: ESP_LOGD("rb", "%s", format_hex_pretty(payload).c_str());

# ---------------------------------------------------------------------------
# Controller-wide entities. Every key is optional. `rainbird_id` is only
# needed when there is more than one hub.
# ---------------------------------------------------------------------------
sensor:
  - platform: rainbird
    rainbird_id: rainbird_controller
    remaining_time:
      name: Time remaining     # seconds; counts down locally between polls
    active_zone:
      name: Active zone        # 0 = none
    queued_zones:
      name: Zones queued
    schedule_changed:
      name: Schedule last changed   # timestamp, from 4A 01; needs time_id
    response_time:
      name: Controller response time
      entity_category: diagnostic
    link_errors:
      name: Controller link errors  # timeouts + bad frames + RX overflows
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

number:
  - platform: rainbird
    rain_delay:
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

# ---------------------------------------------------------------------------
# Home Assistant actions (optional). rainbird.send_raw is deliberately not
# here; it belongs in examples/sniffer.yaml.
# ---------------------------------------------------------------------------
api:
  actions:
    - action: run_zone
      variables:
        zone: int
        minutes: int
      then:
        - rainbird.zone.start:
            zone: !lambda "return zone;"
            duration: !lambda "return minutes * 60 * 1000;"
    - action: queue_zone
      variables:
        zone: int
        minutes: int
      then:
        - rainbird.zone.queue:
            zone: !lambda "return zone;"
            duration: !lambda "return minutes * 60 * 1000;"
    - action: set_rain_delay
      variables:
        days: int
      then:
        - rainbird.set_rain_delay:
            days: !lambda "return days;"
```

---

## 4. Configuration reference

### 4.1 Hub (`rainbird:`)

`MULTI_CONF = True`: several hubs are allowed (one per controller).

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `id` | ID | auto | `RainbirdComponent` |
| `model` | enum | **required** | `ESP-TM2` is implemented. Every other model is rejected with "not implemented yet" (section 7). |
| `accept_model_ids` | list of int | – | Extra model IDs to treat as this model (D11) |
| `tx_pin` | output pin schema | required unless `listen_only` | `inverted:` is honoured |
| `rx_pin` | input pin schema | **required** | |
| `rx_polarity` | `auto` / `normal` / `inverted` | `auto` | Locked on its own pin (6.1) |
| `listen_only` | bool | `false` | Never transmits. State is decoded from replies to another master, such as an LNK2. |
| `monitor_pin` | input pin schema | – | Optional second receiver on J4, so requests from another master are decoded too |
| `monitor_polarity` | `auto` / `normal` / `inverted` | `auto` | Locked separately from `rx_polarity`. The two directions are known to differ. |
| `log_raw_edges` | bool | `false` | Logs every frame's pulse timings at VERBOSE level, for building replay test fixtures |
| `rx_buffer_size` | int | `2048` | Edges held per receive pin. Overflows are counted in `link_errors`. |
| `update_interval` | time | `15s` | Idle poll cycle (section 6.4) |
| `active_update_interval` | time | `3s` | Poll cycle while watering or for 30 s after a command |
| `request_timeout` | time | `400ms` | From the end of the first copy of a request. The longest reply (42 bytes) is about 140 ms on the wire, plus about 30 ms of controller turnaround. Tighten it after phase 2 measures the real turnaround. |
| `retries` | int | `2` | For timeouts and bad frames. A NAK is never retried. |
| `repeat_short_requests` | bool | `true` | Copies the LNK2 habit of sending 2-byte requests twice (SPEC 2.3) |
| `valve_switch_mode` | `start` / `queue` | `start` | Whether turning a zone switch on sends `39` or `4B`. Also changes what "off" does (6.5). |
| `time_id` | ID of `time` | – | Needed by `clock_sync` and by the `schedule_changed` sensor |
| `clock_sync.max_drift` | time | `30s` | Sync when the controller drifts further than this. Also syncs at boot and after a UTC-offset change. Needs `time_id`. |
| `schedule.refresh_interval` | time | `1h` | Full re-read of the schedule pages. The `4A 01` timestamp triggers re-reads in between. |
| `schedule.allow_writes` | bool | `false` | Lets schedule entities write `21 …` pages |
| `experimental` | bool | `false` | Allows `49`, `3A`, `42` and `rainbird.send_raw` |
| `zone_defaults`, `zones` | see 4.2 | all 12 zones | `zones: []` creates none |
| `program_defaults`, `programs` | see 4.3 | all programs | `programs: []` creates none |
| `on_zone_started` / `on_zone_stopped` | automation | | Variables `zone` (uint8), `zone_name` (std::string) |
| `on_nak` | automation | | Variables `command`, `code` |
| `on_frame` | automation | | Variable `payload` (`std::vector<uint8_t>`) for every valid reply |

### 4.2 Zones

> **Turning off a zone switch sends `40`, which stops every zone.** That is the
> only stop command the controller has. Turning off a zone that is only
> queued does nothing. Details, and the experimental `queue`-mode
> alternative, are in 6.5.

**Zone keys:**

| Key | Default | Notes |
|-----|---------|-------|
| `zone` | **required** | 1 to the model's maximum (12 on the TM2). Each zone number can appear only once. |
| `id` | `<hub id>_zone<N>` | `RainbirdZone` object, usable in actions and conditions |
| `name` | `Zone N` | Fills the `{zone}` placeholder |
| `disabled` | `false` | Generates no entities for this zone |
| `device_id` | – | Passed to every entity of the zone, to group a zone as an ESPHome sub-device |

**Generated entities** (one key per entity, or one per program for plural
keys):

| Key | Domain | Generated by default | Default name | Source → write | Notes |
|-----|--------|----------------------|--------------|----------------|-------|
| `valve_switch` | switch | yes | `{zone}` | `3F` → `39`/`4B`. Off: `40` (or `42`, 6.5). | |
| `run_duration_number` | number (min, 1–255) | yes | `{zone} duration` | local | Duration used by `valve_switch` and `queue_button`. 255 is the `39` limit. |
| `running_binary_sensor` | binary_sensor | no | `{zone} running` | `3F` | Same data as the switch state, for read-only dashboards |
| `queue_button` | button | no | `{zone} queue` | → `4B` | |
| `run_time_numbers` | number ×programs (min) | no | `{zone} {program} run time` | pages `80`–`85` → `21 00 8N` | Writing needs `allow_writes` |
| `sensor_bypass_switch` | switch | no | `{zone} ignores rain sensor` | page `20` → `21 00 20` | Writing needs `allow_writes` |
| `fault_binary_sensor` | binary_sensor | no | `{zone} fault` | `3D` | Stays unavailable until `3D` is decoded (SPEC 8.4) |

**How each entity's settings are worked out:**

1. If the zone sets the key to `false`, the entity is not generated.
2. Otherwise, the zone's own settings for that key are merged over the
   `zone_defaults` entry. A zone that sets a key absent from `zone_defaults`
   gets that entity anyway.
3. The placeholders in the name are filled in.
4. A key may be given as a plain string, as in core's `sprinkler`. It is then
   read as `{name: <string>}`, so `valve_switch: "{zone} valve"` works.
5. If no `id` is given, the generated ID is `<zone id>_<key>`. Plural keys
   add a suffix: the program letter, day or slot number, e.g.
   `front_lawn_run_time_numbers_a`. These are ESPHome config IDs for
   lambdas, not the deprecated entity `object_id`, which the component never
   sets or reads.
6. The result is checked against the platform's normal entity schema, so
   every standard option (`icon`, `internal`, `entity_category`,
   `disabled_by_default`, `device_id`, `on_turn_on`, `filters`, …) works.

### 4.3 Programs

**Program keys:**

| Key | Default | Notes |
|-----|---------|-------|
| `program` | **required** | `A`–`C` on the TM2. Stored as 0–2. |
| `id` | `<hub id>_program_<a>` | `RainbirdProgram` |
| `name` | `Program A` | Fills `{program}`, and is used in the text of `active_program` and `queue` |

**Generated entities:**

| Key | Domain | Generated by default | Default name | Source → write |
|-----|--------|----------------------|--------------|----------------|
| `start_button` | button | yes | `Run {program}` | → `38` |
| `seasonal_adjust_number` | number (%, 10–200) | yes | `{program} seasonal adjust` | `30` → `31`. The 10 % floor is a pyrainbird docstring, not a verified limit; phase 4 probes it (SPEC 8.14). |
| `running_binary_sensor` | binary_sensor | no | `{program} running` | `3B 00` byte 9 |
| `frequency_select` | select | no | `{program} frequency` | page `1P` byte 5. Options: `Custom days`, `Cyclic`, `Odd days`, `Even days`. |
| `watering_day_switches` | switch ×7 | no | `{program} {day}` | page `1P` byte 0, one bit each. Used by `Custom days`. |
| `cycle_days_number` | number (days) | no | `{program} cycle days` | page `1P` byte 1 |
| `days_until_next_number` | number (days) | no | `{program} days until next run` | page `1P` byte 2 |
| `start_time_datetimes` | datetime (time) ×4 | no | `{program} start {slot}` | page `6P`, one u16 each |
| `start_time_switches` | switch ×4 | no | `{program} start {slot} enabled` | page `6P`. Off = `FFFF`. Must be enabled together with `start_time_datetimes`. |

Every write to page `1P` or `6P` needs `allow_writes`. Bytes 3
(`permanentDaysOff`) and 4 (unknown) of page `1P` have no entities; they are
always copied from the last read (6.5).

### 4.4 Platform keys (controller-wide)

| Platform | Keys |
|----------|------|
| `sensor` | `remaining_time`, `active_zone`, `queued_zones`, `schedule_changed`, `response_time`, `link_errors` |
| `binary_sensor` | `watering`, `rain_sensor`, `irrigation_enabled`, `connected` |
| `text_sensor` | `active_program`, `queue`, `model`, `firmware_version`, `serial_number` |
| `number` | `rain_delay`, `station_delay`, `test_duration` |
| `switch` | `rain_sensor_bypass`, `irrigation_enabled` |
| `button` | `stop`, `advance`, `test_all_zones`, `sync_clock`, `refresh` |
| `datetime` | `controller_clock` |

Each key takes that domain's normal entity schema. Device classes, units,
icons and entity categories are set the way core sets them.

### 4.5 Actions, conditions and triggers

**Actions**

| Action | Arguments | Command |
|--------|-----------|---------|
| `rainbird.zone.start` | `zone` (zone ID or templatable number), `duration` (templatable time) | `39` |
| `rainbird.zone.queue` | same | `4B` |
| `rainbird.program.start` | `program` (program ID or templatable letter/index) | `38` |
| `rainbird.stop` | `id` (hub, optional) | `40` |
| `rainbird.advance` | `zone` (optional; defaults to the next queued zone from `3B 01`) | `42` (experimental) |
| `rainbird.test_all_zones` | `duration` | `3A` (experimental) |
| `rainbird.set_rain_delay` | `days` (templatable) | `37` |
| `rainbird.set_seasonal_adjust` | `program`, `percent` (templatable) | `31` |
| `rainbird.sync_clock` | | `11` + `13` |
| `rainbird.refresh` | `schedule: true/false` | Immediate poll |
| `rainbird.send_raw` | `data` (templatable bytes) | Any payload; the reply arrives through `on_frame`. **Experimental only.** Compile fails without `experimental: true`. |

Durations are rounded **up** to whole minutes and clamped to 1–255.

**Conditions:** `rainbird.is_watering`, `rainbird.zone.is_running: <zone>`,
`rainbird.program.is_running: <program>`.

**Triggers:** see 4.1.

---

## 5. Repository and code layout

ESPHome core files are flat inside a component folder, with one subfolder per
entity platform that has its own C++ files. Shared C++ sits at the component
root and uses name prefixes rather than extra folders. That way ESPHome's
file copier picks every file up without trouble.

```
rainbird-esphome/
├── README.md, LICENSE, SPEC.md, PLAN.md, WIRE.md
├── components/rainbird/
│   ├── __init__.py              # hub schema, zones/programs expansion, actions, to_code
│   ├── models.py                # Python model registry: limits, implemented flag, C++ class
│   ├── rainbird.h/.cpp          # RainbirdComponent: scheduler, poll plan, state, publishing
│   ├── rainbird_zone.h          # RainbirdZone, RainbirdProgram (entity holders + helpers)
│   ├── automation.h             # actions, conditions, triggers
│   ├── controller_state.h       # plain state structs (no ESPHome deps)
│   ├── sip_frame.h/.cpp         # streaming frame decoder + encoder, checksum (pure C++)
│   ├── sip_commands.h/.cpp      # request builders + reply parsers (pure C++)
│   ├── manchester.h/.cpp        # duration ↔ bits codec (pure C++)
│   ├── phy.h                    # abstract Phy interface
│   ├── phy_esp32.h/.cpp         # RMT TX + GPIO-ISR RX
│   ├── model.h                  # abstract Model + LegacyProgramModel base
│   ├── model_esp_tm2.h/.cpp     # implemented
│   ├── model_stubs.h            # EspMe, Lcr (ESP-RZXe/ST8x), EspMe3: declared, unimplemented
│   ├── sensor/__init__.py
│   ├── binary_sensor/__init__.py
│   ├── text_sensor/__init__.py
│   ├── number/__init__.py, rainbird_number.h/.cpp
│   ├── switch/__init__.py, rainbird_switch.h/.cpp
│   ├── button/__init__.py, rainbird_button.h/.cpp
│   ├── select/__init__.py, rainbird_select.h/.cpp
│   └── datetime/__init__.py, rainbird_datetime.h/.cpp
├── examples/
│   ├── minimal.yaml
│   ├── full.yaml                # the YAML in section 3
│   └── sniffer.yaml             # listen_only + monitor_pin + send_raw, for protocol work
├── tools/
│   └── edges_to_fixture.py      # turns log_raw_edges output into tests/captures files
├── tests/
│   ├── unit/                    # host-compiled tests of the pure C++ layers
│   │   ├── Makefile
│   │   ├── doctest.h
│   │   ├── test_frame.cpp, test_manchester.cpp, test_commands.cpp, test_replay.cpp
│   │   └── vectors.h            # every example in SPEC.md as a test vector
│   ├── captures/                # real pulse-timing captures from phase 1 onwards
│   └── components/rainbird/
│       ├── common.yaml          # the component config, pins as substitutions
│       ├── test.esp32-idf.yaml  # core naming: test.<platform>-<framework>.yaml
│       ├── test.esp32-ard.yaml
│       ├── test.esp32-c3-idf.yaml   # RISC-V variant with smaller RMT memory
│       └── validate.esp32-idf.yaml  # config-only checks, never compiled
└── .github/workflows/ci.yaml
```

The test files follow core's component-test layout: a shared `common.yaml`,
included as a package from one file per platform and framework, with pins
passed in as substitutions. Each one loads the component from the checkout
with `external_components: [{ source: { type: local, path: ../../../components } }]`,
so CI tests the code in the PR rather than a published tag.

### 5.1 Style conventions (as core uses them)

These conventions were checked against the ESPHome developer docs and the
2026.9.1 source on 2026-10-20 (section 11).

**C++**
- Namespace `esphome::rainbird`. The namespace must match the component name.
- `this->` everywhere, members with a trailing underscore, `snake_case`
  methods, and `final` on concrete classes, as core does.
- **Log tags** are declared with `ESPHOME_LOG_TAG(TAG, "rainbird.phy");` at
  namespace scope in each `.cpp`. That macro arrives in 2026.10.0, so a shared
  header carries core's documented fallback for older releases:
  ```cpp
  #ifndef ESPHOME_LOG_TAG
  #define ESPHOME_LOG_TAG(name, tag) static const char *const name = tag
  #endif
  ```
  `TAG` is only passed to log macros; it is never used as a scheduler name or
  to build strings.
- **`dump_config()`** combines related fields into one `ESP_LOGCONFIG` call
  with `\n` separators, within the 512-byte buffer. It uses `LOG_PIN` and
  `LOG_UPDATE_INTERVAL`, and sets up no strings with `str_sprintf`.
- **Logging levels.** No logging for entity state changes; the base classes
  already log published states at VERBOSE. No logging in the interrupt
  handler or on every loop pass. Frame dumps are VERBOSE, through
  `format_hex_pretty_to` into a stack buffer. They move to `ESP_LOGV_HEX`
  once `min_version` includes it.
- `SUB_SENSOR(...)`, `SUB_BINARY_SENSOR`, `SUB_TEXT_SENSOR`, `SUB_SWITCH`,
  `SUB_NUMBER`, `SUB_BUTTON` and `SUB_SELECT` setters, each with its include
  inside `#ifdef USE_SENSOR` (etc.) guards, exactly as `ld2410` does. Domains
  the user doesn't configure then cost nothing.
- `Parented<RainbirdComponent>` for writeable entities.
- **Status** uses `status_set_warning()` / `status_clear_warning()` while the
  link is down. The named form of `status_momentary_warning` is deprecated
  (removal in 2027.5.0) and is not used.
- **Timekeeping** in `loop()` uses `App.get_loop_component_start_time()`, not
  `millis()`. Any timer IDs are `static constexpr uint32_t`, never strings.
- **Persisted state** (the remembered time of a start-time slot that is off)
  uses `global_preferences->make_preference<T>()`. Its key is a hash of hub
  ID, program and slot, so it stays stable across builds. A failed `load()`
  is handled as the normal case.
- clang-format using ESPHome's `.clang-format`.

**Python**
- `DOMAIN = "rainbird"`. Core CI is starting to require it, and it's harmless
  on older releases.
- `CONF_*` constants from `esphome.const` wherever one exists (`CONF_MODEL`,
  `CONF_TX_PIN`, `CONF_RX_PIN`, `CONF_TIME_ID`, `CONF_ZONE`, …).
- `CONF_RAINBIRD_ID = "rainbird_id"` for platforms, with
  `cv.GenerateID(CONF_RAINBIRD_ID): cv.use_id(RainbirdComponent)`.
- `CODEOWNERS = ["@sjwright"]`, `MULTI_CONF = True`.
- `AUTO_LOAD` for every entity domain the hub can generate: `switch`,
  `number`, `button`, `binary_sensor`, `select`, `datetime`.
- **Validation:** `cv.only_on_esp32`, plus a check against
  `esp32_rmt.VARIANTS_NO_RMT`, the list `remote_transmitter` uses.
- **Entity schemas** come from the domain helpers: `switch.switch_schema(...)`,
  `number.number_schema(..., unit_of_measurement=...)`,
  `button.button_schema(...)`, `select.select_schema(...)`,
  `datetime.time_schema(...)` / `datetime.datetime_schema(...)`. The entities
  are created with `new_switch`, `new_number(config, min_value=, max_value=,
  step=)` and so on, and parented with `cg.register_parented()`.
- **RMT driver:** `to_code` calls
  `esp32.include_builtin_idf_component("esp_driver_rmt")`. ESPHome now leaves
  that driver out of builds by default, and `remote_transmitter` re-enables
  it the same way.
- **Pins** are wired with `cg.gpio_pin_expression()` and `cg.add()` for now.
  Core's newer `cg.add_pin()` / `cg.add_variable()` helpers need 2026.11.0.
- **Not used yet, to adopt later:** core's `sub_switches(...)` and
  `ApplySwitch` / `ApplyNumber` / `ApplyButton` / `ApplySelect` helpers. They
  let a simple child entity call one hub method without its own C++ class, but
  they are only on ESPHome's `dev` branch. Section 11 lists when to adopt
  them.
- **No PlatformIO-specific hooks** (`cg.add_library`, `platformio_options`).
  ESPHome is deprecating the PlatformIO toolchain (removal in 2027.2.0), and
  this component needs no libraries.
- ruff formatting.

**Entities:** names in sentence case, matching current ESPHome guidance.
Names never repeat the device name; ESPHome and Home Assistant add it.

### 5.2 Main C++ classes

```cpp
namespace esphome::rainbird {

class RainbirdComponent : public PollingComponent {
 public:
  void setup() override;          // PHY init, queue identification requests
  void loop() override;           // drain RX, run the scheduler, timeouts; the only place that transmits
  void update() override;         // enqueue a poll cycle if none is pending; never transmits
  void dump_config() override;

  // Control API used by entities, actions and the HA API
  void start_zone(uint8_t zone, uint16_t minutes);
  void queue_zone(uint8_t zone, uint16_t minutes);
  void start_program(uint8_t program);
  void stop_all();
  void advance();
  void test_all_zones(uint8_t minutes);
  void set_rain_delay(uint16_t days);
  void set_seasonal_adjust(uint8_t program, uint16_t percent);
  void set_clock(const ESPTime &t);
  void edit_schedule(const ScheduleEdit &edit);   // read-modify-write, 6.5
  void send_raw(std::vector<uint8_t> payload);
  void request_refresh(bool schedule);

  const ControllerState &state() const { return this->state_; }

  SUB_SENSOR(remaining_time) SUB_SENSOR(active_zone) /* … */
  SUB_BINARY_SENSOR(watering) /* … */

 protected:
  Phy *phy_;
  Model *model_;
  bool writes_permitted_{false};  // false until 02 returns an accepted model ID (D11)
  std::vector<RainbirdZone *> zones_;
  std::vector<RainbirdProgram *> programs_;
  RequestQueue queue_;            // priority: user > follow-up > poll
  ControllerState state_;         // last known values, std::optional per field
};

}  // namespace esphome::rainbird
```

`ControllerState`, `sip_frame`, `sip_commands` and `manchester` use only the
standard library. That keeps them unit-testable on the host and reusable by
other models.

---

## 6. Design details

### 6.1 Physical layer (`phy_esp32`)

**Transmit, using RMT** through the RMT TX driver API (`driver/rmt_tx.h`). It
is the only RMT API in ESP-IDF 5.x and 6.x, and it is what Arduino-ESP32 3.x
builds on. The current ESPHome defaults are ESP-IDF 5.5.5 and Arduino 3.3.11.
The driver component has to be added back explicitly (see 5.1).

- Resolution is 1 MHz. Each bit becomes one RMT symbol of 208 + 208 µs:
  high then low for 1, low then high for 0.
- The idle level is low. Pin `inverted:` is mapped to the RMT output-invert
  flag.
- The whole frame, including the duplicate copy when
  `repeat_short_requests` applies, is encoded once and sent as one
  transaction, with no CPU involvement while it goes out.

**Receive, using a GPIO interrupt** (one per receive pin).

- The interrupt is attached the way core's `pulse_meter` does it:
  `this->rx_isr_ = this->rx_pin_->to_isr();` and
  `this->rx_pin_->attach_interrupt(&RainbirdPhyEsp32::rx_isr, this, gpio::INTERRUPT_ANY_EDGE);`.
- The handler is `static void IRAM_ATTR`. It reads the level with
  `isr_pin.digital_read()` and timestamps the edge with
  `esp_timer_get_time()`. ESPHome's own `micros()`, which core ISRs use, is
  also IRAM-safe on ESP32. `esp_timer_get_time()` is preferred because it is
  the ESP-IDF primitive and doesn't depend on which framework is in use.
- Each edge is stored as a `uint32_t` (timestamp with the level in the low
  bit) in a lock-free single-producer, single-consumer ring of
  `rx_buffer_size` entries (8 KB at the default).
- A full ring drops the edge, increments an overflow counter and marks the
  frame in progress as bad. Overflows show up in `link_errors` and in the
  logs.
- In normal use only one reply is in flight (≤ ~700 edges). The headroom is
  for sniffing back-to-back LNK2 traffic through a `loop()` stall during
  WiFi or OTA activity.
- **Receive is never paused while transmitting.** TX and RX are separate
  wires. The controller starts replying about 30 ms after the first copy of
  a request, while the duplicate copy is still going out.
- **Waking the main loop.** On the first edge after an idle period, the
  handler calls the hub's `enable_loop_soon_any_context()`, ESPHome's
  documented ISR-safe wake, and nothing else. The PHY belongs to the hub, so
  this is the hub controlling its own loop, as the docs require. It doesn't call it on every
  edge, which would flood the main loop with wake-ups.

**Decoding** (pure `manchester.cpp` + `sip_frame.cpp`):

1. Classify each interval as 1 or 2 half-bits, allowing ±30 % on the
   208/416 µs widths. Anything else resets the decoder to hunting for a
   preamble.
2. Expand the intervals into a stream of half-bit levels. When the line has
   been idle, the idle level is added at the start to cover the leading
   edge.
3. Until polarity is locked, run 4 decoders in parallel: both polarities ×
   both phase offsets. The phase offset absorbs the "extra half-bit" seen on
   replies.
4. Decode high-low = 1 and low-high = 0, MSB first, into a **streaming frame
   parser**. It hunts for `F5 F5 1C` or `F5 F5 F5 1C`, then reads `len`
   bytes, the checksum and `1D`.
5. **Frames are split by length, not by idle gaps.** As soon as a frame
   validates, the parser starts hunting for the next preamble on the very
   next bit. The LNK2's duplicate 2-byte requests have no gap between them,
   so a gap-only splitter would glue them together. A gap of 2 ms or more
   only resets a half-finished frame.
6. **Polarity locks per pin.** Each receive pin (`rx_pin`, `monitor_pin`)
   keeps its own polarity and phase state, and locks after 3 consecutive
   good frames when set to `auto`. The two directions are known to have
   opposite polarity at the LNK2's own pins. Five bad frames in a row
   unlock it again.

`log_raw_edges` logs each frame's interval list at VERBOSE level.
`tools/edges_to_fixture.py` turns those logs into files in `tests/captures/`
for the replay tests (section 8).

### 6.2 Link and transaction layer

- **`update()` only enqueues.** `PollingComponent::update()` adds a poll
  cycle to the queue unless one is already pending. `loop()` is the only
  place that transmits, so a poll can never collide with a user command in
  flight.
- **Switching to the fast poll** while watering and back uses
  `set_update_interval()` and restarts the poller. There is no second timer.
- **`loop()` only runs when there is work.** It is enabled while a request is
  queued or in flight, or while received edges are waiting to be decoded.
  When the queue and both receive rings are empty, it calls `disable_loop()`.
  `update()`, the control API and the interrupt handler turn it back on with
  `enable_loop()` or `enable_loop_soon_any_context()`. This is the pattern the
  developer docs recommend for interrupt-fed components.
- **Nothing blocks.** Transmit is a non-blocking RMT transaction, and
  decoding works through whatever edges are buffered. Every `loop()` pass
  stays far below ESPHome's 50 ms blocking warning.
- The `remaining_time` countdown uses `set_interval` with a `constexpr`
  numeric ID. It runs only while a zone is running and is cancelled when
  watering stops.
- **One request in flight at a time.** Each step is: send, then wait up to
  `request_timeout` after the end of the first copy, then match the reply. A
  reply matches if its opcode is the request opcode + `0x80`, or an ACK/NAK
  whose echo byte is the request opcode.
- **Duplicate requests.** With `repeat_short_requests`, 2-byte requests go
  twice with no gap. A second identical reply within the window is dropped
  and counted, which answers SPEC question 5.
- **Errors.** A timeout or bad frame is retried up to `retries` times, then
  counted as a link error. Three link errors in a row set `connected` off,
  and polling backs off to `update_interval`.
- **NAKs** are logged and raise `on_nak`. The code is a bitmask (SPEC 3.3),
  so the command is marked unsupported for the session when **bit 0 is set**
  (`code & 0x01`). `00 02 03` means "not supported" and "bad length". A NAK
  without bit 0 is logged as a likely encoding bug.
- **Queue priority:** user commands, then follow-up reads, then the poll
  cycle. There is a gap of at least 20 ms between transactions.
- **Follow-up reads.** After any ACKed control command, `3F 00`, `4C` and
  `3B 00` are queued straight away, and the fast poll runs for 30 s. Entities
  show the confirmed state within about 500 ms; they don't publish
  optimistic state.
- **Write gate (D11).** Control and write commands are refused, with a
  warning, until `02` has returned a model ID that the model accepts. This
  covers 37, 38, 39, 31, 40, 42, 49, 4B, 3A, 11, 13, 21 and raw sends.

### 6.3 Startup sequence

1. `02` model and protocol. If the model ID doesn't match the configured
   model and isn't in `accept_model_ids`, an error is logged, the hub stays
   read-only (D11), and `model` reports the mismatch.
2. `0B` firmware, `05` serial, `03 00` available zones.
3. `04` probes for `4C`, `4A`, `42`, `4B` and `3D`. The model's capability
   set is narrowed to what the controller actually supports.
4. Full schedule read: page `00`, `10`–`12`, `60`–`62`, `80`–`85`, `20`.
5. Clock check and sync if needed. Then the normal poll cycle starts.

Zones that the `03` mask reports as missing are marked unavailable, and a
warning is logged once.

### 6.4 Poll plan (ESP-TM2)

| Cycle | Requests | Notes |
|-------|----------|-------|
| Every poll | `4C`, `3F 00`, `3B 00` | `4C` replaces `10`, `12`, `36`, `3E` and `48` on upgraded TM2s |
| Every poll, original TM2 (`0005`) | `10`, `12`, `36`, `3E`, `48`, `3F 00`, `3B 00` | No `4C` on this variant |
| While watering | Add `3B 01` | Feeds the queue text sensor |
| Every 4th poll | `30 00`–`02`, `4A 01`, `3D 00` | Budgets and schedule-change detection |
| `4A 01` changed, or `schedule.refresh_interval` passed | Schedule pages | |
| On demand | anything | `refresh` button, actions |

One full idle cycle takes about 3 × 200 ms, so the bus is free more than 95 %
of the time. `remaining_time` counts down once a second on the ESP between
polls, so Home Assistant shows a smooth countdown.

### 6.5 Entity behaviour notes

**Zone switches**

- **On** sends `39` (or `4B` in `queue` mode) with that zone's
  `run_duration_number` value. The switch turns on once `3F` confirms it.
- **Off, `start` mode:** sends `40` (stop all) if this zone is running.
- **Off, `queue` mode:** for the running zone, sends `42` with the next queued
  zone's number (advance) so the
  rest of the queue carries on. This only applies with `experimental: true`
  on a controller that supports `42`; otherwise it sends `40`.
- **Off for a zone that is only queued:** nothing is sent and a warning is
  logged. The TM2 has no "remove from queue" command.
- **Switching on another zone in `start` mode** replaces the running zone, as
  the controller does. The other switch turns off after the next poll.

**Gated writes.** Writes to `allow_writes`-gated or `experimental`-gated
entities, or any write blocked by the model gate, log a warning and
republish the controller's current value. Home Assistant shows the change
bouncing back, which is visible and harmless.

**Schedule edits** always work on a fresh copy of the page:

1. Re-read the page.
2. Change only the bytes behind the edited entity. Every other byte is
   copied from that read, including page `1P` byte 3 (`permanentDaysOff`)
   and byte 4 (unknown), which have no entities. A page is never rebuilt
   from entity state.
3. **Log the old and new page in hex at INFO level** before sending
   `21 00 PP …`. The log is then a backup and undo record from the first
   write onwards.
4. Re-read to confirm. A mismatch logs an error and publishes the value read
   back.

**Start-time slots.** Each slot has a time entity and an enabled switch.

- Turning the switch off writes `FFFF`. The time entity keeps its last value,
  restored across reboots.
- Turning the switch on writes the time entity's value.
- Changing the time while the slot is on writes it straight away. While the
  slot is off, the new time is only stored locally.

**`active_program`** shows the program name, `Manual` (`FF` with a zone
running) or `Idle`.

**`schedule_changed`.** The `4A 01` value is seconds since 2000-01-01 in
*controller local time* (SPEC 5.3). It is converted to a UTC timestamp using
the timezone of `time_id`, so the sensor needs `time_id` and config
validation fails without it. Whether the controller really stores local time
is still an open question (SPEC 8.11); phase 3 checks it against a
front-panel edit at a known time.

**Rain sensor.** Polarity is unconfirmed (SPEC 8.1). The binary sensor
publishes `value != 0` as on. Users can add `filters: [invert]`, and the
default is fixed once confirmed.

---

## 7. Model abstraction and stubs

### 7.1 Python registry (`models.py`)

```python
MODELS = {
    "ESP-TM2":  Model(EspTm2Model, implemented=True,  zones=12, programs=3, starts=4,
                      model_ids=[0x0005, 0x000A, 0x010A, 0x0014, 0x0015]),
    "ESP-ME":   Model(EspMeModel,  implemented=False, zones=22, programs=4, starts=6,
                      model_ids=[0x0007, 0x0107]),
    "ESP-RZXE": Model(LcrModel,    implemented=False, zones=8,  programs=0, starts=6,
                      model_ids=[0x0003, 0x0103]),
    "ST8X-WF":  Model(LcrModel,    implemented=False, zones=8,  programs=0, starts=6,
                      model_ids=[0x0006, 0x0008]),
    "ESP-ME3":  Model(EspMe3Model, implemented=False, zones=22, programs=4, starts=6,
                      model_ids=[0x0009]),
}
```

The registry is used to:

- reject models that aren't implemented, with a link to the issue tracker;
- check zone numbers, program letters and other ranges against the model's
  limits;
- pick the C++ model class to create;
- give the runtime model-ID check (D11) its list of accepted IDs.

### 7.2 C++ `Model` interface

```cpp
class Model {
 public:
  virtual const char *name() const = 0;
  virtual bool accepts(uint16_t model_id) const = 0;   // D11 gate
  virtual uint8_t max_zones() const = 0;
  virtual uint8_t max_programs() const = 0;
  virtual uint8_t start_times_per_program() const = 0;
  virtual bool supports(Capability c) const = 0;    // COMBINED_STATE, EVENT_TIMESTAMP, …
  virtual void build_poll_cycle(PollContext &ctx) = 0;
  virtual void schedule_pages(std::vector<uint8_t> &pages) const = 0;
  virtual bool parse_reply(const std::vector<uint8_t> &payload, ControllerState &state) = 0;
  virtual bool apply_schedule_edit(const ScheduleEdit &edit,
                                   const std::vector<uint8_t> &current_page,
                                   std::vector<uint8_t> &new_page) = 0;
};

// Legacy SIP program-based controllers (TM2, ME). Page layout is derived from
// the limits passed in, never from another model's constants.
class LegacyProgramModel : public Model {
 protected:
  LegacyProgramModel(const ModelLimits &limits, std::vector<uint16_t> model_ids);
};
```

- **`EspTm2Model`** is a `LegacyProgramModel` built with the TM2 limits and
  IDs, plus TM2-specific poll plans and queue parsing.
- **`EspMeModel`** is declared as a sibling `LegacyProgramModel` with its own
  limits (4 programs, 6 starts, 22 zones, pages `80`–`8A`). It is a sibling
  rather than a subclass of `EspTm2Model`, so turning on `implemented` can't
  silently inherit TM2-specific behaviour.
- **`LcrModel`** (per-zone schedules) and **`EspMe3Model`** (Universal/CDT
  protocol) are declared only.
- **Schedule encodings belong to the model, never the entity layer.** These
  include the frequency enum order, the start-time codec and the "slot off"
  value. The model exposes `frequency_options()` and
  `encode_start_time()` / `decode_start_time()`, and `frequency_select` takes
  its options from the model. The LCR family differs on all three:
  - frequency order is custom / odd / even / cyclic, against the TM2's
    custom / cyclic / odd / even;
  - start times have 10-minute resolution;
  - an unused slot is `0x90`, not `FFFF`.

  This way the LCR stub can't inherit TM2 encodings by accident.
- `apply_schedule_edit` takes the current page bytes, so the
  read-modify-write rule in 6.5 is part of the interface.

---

## 8. Testing and CI

| Layer | How it's tested |
|-------|-----------------|
| Frame codec, checksum | Host unit tests with doctest. Vectors are every frame in SPEC.md, plus corrupted copies with bad length, checksum or end byte. Also tests two frames back to back with no gap, and a reply that overlaps the end of a duplicate request on the other pin. |
| Manchester codec | Host tests. Synthesise duration streams from bytes, add ±20 % jitter, invert them, add the leading half-bit, then decode. Also round-trip with the encoder. |
| Replay | `test_replay.cpp` feeds every file in `tests/captures/` (real pulse timings from `log_raw_edges`) through the full pipeline. Expected payloads are stored beside each capture. This catches assumptions the synthetic tests share with the encoder. |
| SIP parsers | Host tests using every `[test]` and `[wire]` example in SPEC.md, such as the `CC`, `BB` page 0 and page 1, `A0` pages and `CA` timestamp captures. Also tests that schedule edits preserve the bytes they don't touch. Vectors taken from pyrainbird's raw `decode_schedule` output have 1 added to the zone numbers (SPEC 5.6). |
| Schema and codegen | `esphome compile` on every `tests/components/rainbird/test.*.yaml`, and `esphome config` on `validate.*.yaml` and `examples/*.yaml`. Negative tests check that `send_raw` without `experimental`, an unimplemented model, and an ESP32-C2 target are all rejected. |
| Lint | ruff, clang-format check, yamllint |
| Hardware | Phases 1–2 below, on the real controller |

**Host test build:** `tests/unit/Makefile` compiles the pure C++ sources with
the tests:

```
g++ -std=gnu++17 -Wall -Wextra -Werror -I components/rainbird
```

Run it with:

```bash
make -C tests/unit test
```

CI runs the same command.

GitHub Actions runs every row except hardware on each push and PR.

**ESPHome version matrix:**
- `min_version` (currently 2026.9.0) and the latest stable release. Both must
  pass.
- A weekly scheduled build against ESPHome `dev`. It may fail, but each
  failure is an early warning of upcoming API changes.

The ESPHome version is installed with `pip install esphome==<version>`. Builds
use ESPHome's default toolchain; PlatformIO is not invoked directly.

Release tags (`v0.x.y`) are created only when CI passes.

---

## 9. Delivery phases

Each phase ends with something usable and settles specific open questions
from SPEC.md section 8. A question counts as **settled once the answer is
known**, whether the controller accepts the command or not.

| Phase | Deliverable | Exit criteria | Settles |
|-------|-------------|---------------|---------|
| **0. Scaffold** | Repo layout, empty hub that compiles, CI green (host tests + compile), `examples/minimal.yaml` | `make -C tests/unit test` and `esphome compile` pass in CI for both frameworks | – |
| **1. Sniffer** | PHY receive, streaming frame decode, `listen_only` + `monitor_pin` with polarity locked per pin, `on_frame`, `log_raw_edges`, first `tests/captures/` | With the LNK2 still connected, both directions decode for 1 h, duplicates included, with 100 % valid checksums and zero RX overflows | 13 (polarity and timing on both pins), 5 (whether duplicates get two replies), 4 (`3D` layout) if a fault is set up |
| **2. Link** | PHY transmit, transaction layer, identification and model gate, `connected`, `response_time`, `link_errors`, model/firmware/serial | LNK2 removed. 24 h of polling with fewer than 0.1 % retries and no unrecovered errors. Measured turnaround used to tighten `request_timeout`. | 12 (model ID), 5 again (A/B test of `repeat_short_requests`) |
| **3. Status** | All read-only controller and zone entities, `remaining_time` countdown, `queue` and `active_program` text, `schedule_changed` | Values match the controller display through a full program run | 1 (rain sensor, by opening the sensor input), 2 (`48` vs OFF), 10 (queue high bits), 11 (`4A` changes on a front-panel edit, and whether it is local time) |
| **4. Control** | Zone switches, run durations, queue, stop, run program, rain delay, seasonal adjust, clock sync and `controller_clock`, actions and conditions | `39`, `4B`, `38`, `40`, `37`, `31`, `11`, `13` acknowledged and confirmed by a follow-up read. `49`, `3A` and `42` each tried behind `experimental` until it is known whether and how they work. `42` follows the probe order in SPEC 5.4. Seasonal adjust probed at 5 % and 0 % on a program with no start times. Every NAK seen while probing is recorded with its code. | 3 (`49`), 6 (`3A`), 7 (`42`), 14 (seasonal adjust minimum), 15 (NAK code bits) |
| **5. Schedule read** | Schedule pages, program and zone schedule entities (read-only) | Entities match the front panel for all 3 programs. A distinctive run time set at the front panel on one odd and one even zone shows up in the expected page and slot. | 9 (program page byte 4), page-to-zone mapping |
| **6. Schedule write** | `allow_writes`, read-modify-write with hex logging | Each page type written and verified on hardware, including the inferred `8N` and `20` layouts. Untouched bytes confirmed unchanged. | 8 |
| **7. Release** | README (wiring, YAML, entity list), `examples/full.yaml`, v1.0.0 tag | A fresh user can build from the README alone | – |

As each question is settled, SPEC.md is updated, the matching `[inferred]`
tag becomes `[wire]`, and the default or `experimental` gate is adjusted.

---

## 10. Risks

| Risk | Mitigation |
|------|------------|
| The controller is fussy about transmit timing or the idle level | RMT gives exact timing. Phase 1 measures the LNK2's real waveform first, and `inverted:` on the pin handles polarity. |
| Optocoupler edges are slow enough to skew pulse widths | ±30 % classification. Use an external pull-up of a few kΩ rather than the internal ~45 kΩ one. |
| Edges are lost during WiFi or OTA activity | IRAM interrupt handler using `esp_timer_get_time()`, an 8 KB ring per pin, and overflow counting, so losses show as `link_errors` rather than mystery checksum failures. Retries cover the rest. |
| A bad schedule write corrupts the controller's programs | Writes are off by default. The model gate (D11) applies. Writes are read-modify-write with confirmation and logged in hex before sending, and each page type is tested on its own. |
| Generated entity count is high (12 zones × up to 7, 3 programs × up to 21) | Only valve and duration per zone, and run button and seasonal adjust per program, are generated by default. Everything else is opt-in. |
| ESPHome API churn, especially the `datetime` platform, the RMT driver and code generation helpers | CI builds against both `min_version` and the latest stable release, plus a weekly build against `dev` that is allowed to fail (section 11). Compatibility shims are used where the docs give one (`ESPHOME_LOG_TAG`). Free-text `text` entities are not used at all. |
| Two hubs or an LNK2 on the same controller | Not supported while transmitting. `listen_only` is the only supported way to share the bus. |

---

## 11. ESPHome compatibility

This section was checked on **2026-10-20** against the user docs at
esphome.io, the developer docs at developers.esphome.io, and the ESPHome
source at tag `2026.9.1`, the latest stable release at that date. Re-check it
before each minor release of this component.

### 11.1 Baseline

- **ESPHome `min_version: 2026.9.0`.** CI tests nothing older, so nothing
  older is claimed. It can be lowered later if a user needs it and CI passes.
- **ESP32 with ESP-IDF by default.** ESP-IDF is now ESPHome's default
  framework for every ESP32 variant (recommended version 5.5.5). Arduino
  (3.3.11) is opt-in and only exists for ESP32, -S2, -S3 and -C3, so it is
  tested but secondary.
- **`esp32: variant:`** in every example. The docs say `board:` is "no longer
  recommended".

### 11.2 Current practices this plan follows

| Area | Current practice | Where in this plan |
|------|------------------|--------------------|
| External components | `components/<name>/` layout. `github://user/repo@ref` shorthand or the full `type: git` form. `refresh` defaults to `1day`; `never` suits a pinned tag. | 2, 5 |
| HA actions | `api: actions:` with `action:` keys. `services:` still works but is the old name. Array variable types such as `int[]` are supported. | 3 |
| OTA | `ota: - platform: esphome` (list form) | 3 |
| Sub-devices | `esphome: devices:` / `areas:`, with `device_id` on each entity | 4.2 (`device_id` per zone) |
| Loop control | `disable_loop()` when idle, `enable_loop_soon_any_context()` from interrupts | 6.1, 6.2 |
| Scheduler | `App.get_loop_component_start_time()` instead of `millis()`; numeric timer IDs; nothing that blocks for 50 ms or more | 5.1, 6.2 |
| Logging | `ESPHOME_LOG_TAG` with fallback; combined `dump_config` lines; no state-change logs | 5.1 |
| Status | `status_set_warning()` without a name argument | 5.1 |
| RMT | `include_builtin_idf_component("esp_driver_rmt")`; RMT TX driver API; reject variants without RMT | 5.1, 6.1 |
| GPIO interrupts | `to_isr()` + `attach_interrupt(..., gpio::INTERRUPT_ANY_EDGE)` with an `IRAM_ATTR` static handler | 6.1 |
| Entity IDs | No use of entity `object_id` (deprecated in 2026) | 4.2 |
| Optional domains | `#ifdef USE_<DOMAIN>` guards around `SUB_*` members | 5.1 |
| Tests | `tests/components/<name>/test.<platform>-<framework>.yaml` plus `common.yaml` and `validate.*.yaml` | 5, 8 |
| Toolchain | Nothing PlatformIO-specific; ESPHome's PlatformIO toolchain is deprecated (removal in 2027.2.0) | 5.1 |

### 11.3 Newer APIs deliberately not used yet

These exist only in releases newer than `min_version`, or only on `dev`. Each
can be adopted once `min_version` reaches the release that ships it.

| API | Available from | Adopt when |
|-----|----------------|------------|
| `ESPHOME_LOG_TAG` | 2026.10.0 | Used now through the documented `#ifndef` fallback. Drop the fallback when `min_version` ≥ 2026.10.0. |
| `cg.add_pin()` / `cg.add_variable()` | 2026.11.0 | Replace `cg.gpio_pin_expression()` + `cg.add()` |
| `sub_switches()` / `ApplySwitch` etc. | `dev` only (not in 2026.9.1) | Possibly for simple child entities such as buttons that make one hub call. Zone entities keep their own classes, because they carry zone state. |
| `DOMAIN` lint requirement | `dev` | Already defined, so nothing to change |
| `ESP_LOGV_HEX` | Not stated in the docs; check the release notes | Replace `format_hex_pretty_to` buffers once `min_version` includes it |

### 11.4 Sources

- [External components](https://esphome.io/components/external_components/)
- [ESP32 platform](https://esphome.io/components/esp32/) (default framework, `variant` vs `board`, toolchain deprecation)
- [Core configuration](https://esphome.io/components/esphome/) (`min_version`, `devices`, `areas`)
- [Native API](https://esphome.io/components/api/) (`actions:`)
- [Datetime](https://esphome.io/components/datetime/)
- [Sprinkler](https://esphome.io/components/sprinkler/)
- [Component architecture](https://developers.esphome.io/architecture/components/) and [advanced topics](https://developers.esphome.io/architecture/components/advanced) (loop control, scheduler, preferences)
- [Logging best practices](https://developers.esphome.io/architecture/logging/best_practices/)
- [Component tests](https://developers.esphome.io/architecture/ci/component_tests/)
- ESPHome source at `2026.9.1`: `remote_transmitter/__init__.py` (RMT driver include), `pulse_meter/pulse_meter_sensor.cpp` (interrupt pattern), `esp32/__init__.py` (framework versions), `switch/__init__.py` (no `sub_switches` in the release), `ld2410/ld2410.h` (`SUB_*` with `#ifdef` guards)

---

## Appendix: review notes

This revision folds in two external reviews. Some suggestions were changed or
declined, and the reasons are recorded here.

| Suggestion | Outcome |
|------------|---------|
| Separate polarity lock per receive pin | Adopted (6.1, `monitor_polarity`) |
| Split frames by length; keep receive running during transmit | Adopted (6.1) |
| Refuse transmits on a model-ID mismatch | **Changed.** Status polling continues, because reads can't hurt the controller and they give a mismatched user something to report. Control and writes are refused, and `accept_model_ids` is the escape hatch (D11). |
| Four time entities for start times | **Changed.** Time entities can't show "off" (`FFFF`), so each slot also gets an enabled switch (D6). |
| Replace free-text watering days | Adopted as 7 switches. The `text` platform is no longer used. |
| Program-page writes must keep bytes 3 and 4 | Adopted. Now part of the `Model` interface (`apply_schedule_edit` takes the current page). |
| `request_timeout` 400 ms, measured from the first copy | Adopted |
| IRAM-safe timestamps, overflow counting | Adopted (`esp_timer_get_time()`, `rx_buffer_size`, `link_errors`) |
| `update()` only enqueues; use `set_update_interval()` | Adopted (6.2) |
| Phase 4 exit for `49` / `3A` / `42` | Adopted. A question is settled once the answer is known. |
| `send_raw` bypasses the gates | Adopted. It needs `experimental` and lives only in the sniffer example. |
| Say what happens when `programs:` is omitted | Adopted, and the same rule now applies to `zones:` |
| Seasonal adjust 10–200 % | Adopted. The reviewer later pointed out that 10 % is only a docstring, and suggested 0–200 until a NAK proves otherwise. **Kept 10 %**, because a NAK isn't the only possible outcome: 0 % could be accepted and quietly stop a program. Phase 4 now probes lower values (SPEC 8.14). |
| Don't copy pyrainbird's program-page write template | Adopted. SPEC 5.6 now warns that the template hard-codes byte 4. |
| `42` should send a real zone number, not `00` | Adopted. "Next zone" sends the next queued zone. The probe order is in SPEC 5.4. |
| Zone off-by-one in pyrainbird's schedule decoder | **Partly adopted.** It isn't an upstream bug: `decode_schedule` is 0-based and `Schedule` adds 1 (`data.py`). Test vectors copied from the raw decoder output still need the +1, so that is noted in SPEC 5.6 and section 8. The hardware mapping check is now in phase 5. |
| Schedule encodings belong to the model (for the LCR stub) | Adopted (7.2) |
| NAK bit meanings are docs-only | Adopted. NAKs are recorded during the phase 4 probes (SPEC 8.15). |
| Make "off stops everything" prominent | Adopted (sections 2 and 4.2, and the YAML comment) |
| Log the page in hex before every write | Adopted (6.5) |
| Don't derive `EspMeModel` from `EspTm2Model` | Adopted, via a `LegacyProgramModel` base built from limits |
| Host test build command | Adopted (section 8) |
| `run_time_numbers` breaks the naming convention | **Changed.** Kept the name and made plural keys an explicit rule (D4). Moving run times under programs was declined: zones are where users think about durations, and the zone is already the naming unit. |
| Test the NAK code as a bitmask | Adopted (6.2) |
| Time zone of the `schedule_changed` timestamp | Adopted. Converted using `time_id`, and verification added to phase 3. The 6-hour gap in the SPEC capture is the event being earlier in the day, not a time-zone offset. The capture is from a US controller, where the offset would have been 8 hours. |
| `queue`-mode "off" sends `42` | Adopted behind `experimental` (6.5) |
| Replay harness of real captures | Adopted, from `log_raw_edges` captures. WIRE.md only has decoded hex, so it can't drive the replay tests. |
