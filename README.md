# rainbird-esphome

An [ESPHome](https://esphome.io) external component for controlling a Rain Bird
ESP-TM2 irrigation controller by talking to it directly over its TX/RX serial
interface. No Rain Bird LNK WiFi module or cloud service is needed.

It identifies the controller, polls it and publishes what it finds as Home
Assistant entities. It runs and stops zones and programs, sets rain delay and
seasonal adjust, keeps the controller clock in step, and can read and edit the
controller's watering schedule.

> **Status: early.** It runs against the author's ESP-TM2. Not every command has
> been confirmed there: the ones still unconfirmed sit behind `experimental: true`,
> and schedule editing is off until you set `schedule: allow_writes: true`.
> [SPEC.md](SPEC.md) section 8 lists what is still open. There is no tagged
> release yet, so the examples track `main`.

## What you need

- An ESP32 with the RMT peripheral: ESP32, S3, C3, C5, C6, H2 or P4. The S2, C2
  and C61 are rejected at validation.
- An interface board in place of the LNK2: two optocouplers, a few resistors and
  a 24 V AC to 5 V supply. [WIRE.md](WIRE.md) has the connector pinout and the
  circuit.
- ESPHome 2026.9.0 or newer.

## Quick start

```yaml
external_components:
  - source: github://sjwright/rainbird-esphome@main
    components: [rainbird]

remote_transmitter:
  pin: GPIO26                 # to the controller's RX, through an optocoupler
  carrier_duty_percent: 100%  # no carrier
  non_blocking: true

remote_receiver:
  pin: GPIO27                 # from the controller's TX, through an optocoupler
  idle: 4ms
  filter: 50us
  receive_symbols: 384
  rmt_symbols: 384            # original ESP32 only; leave out on an S3, C3 or C6

rainbird:
  model: ESP-TM2
  zones:
    - zone: 1
      name: Front lawn
    - zone: 2
      name: Back lawn
```

That gives each listed zone a valve switch and a run-duration number, and each
program (A, B and C) a run button and a seasonal-adjust number. If a transmitter
or receiver setting will not work for this protocol, validation says which and
what to set.

[examples/minimal.yaml](examples/minimal.yaml) is that config as a complete
file. [examples/full.yaml](examples/full.yaml) uses every option and entity.
[examples/sniffer.yaml](examples/sniffer.yaml) listens to a controller that
still has its LNK2 fitted.

> **Turning off any zone switch stops all watering.** The controller has one
> stop command and it stops everything. Turning off a zone that is only queued
> does nothing.

## Hub options

| Key | Default | What it does |
|-----|---------|--------------|
| `model` | required | `ESP-TM2`, the only model so far. Not case-sensitive. |
| `transmitter_id`, `receiver_id` | the only one of each | The `remote_transmitter` and `remote_receiver` to use. |
| `update_interval` | `15s` | How often the controller is polled when idle. It is polled every 3 s while watering and for 30 s after a command. `never` sends nothing except `rainbird.send_raw`. |
| `valve_switch_mode` | `start` | `start`: a zone switch starts its zone now, replacing whatever runs. `queue`: it adds the zone to the queue. |
| `time_id` | none | A `time` component. Needed by `clock_sync`, `sync_clock`, `schedule_changed` and `ends_at`. |
| `clock_sync` | off | Present: the controller clock is set whenever it drifts from `time_id`. `max_drift` (default `30s`, at least `5s`) is how far. Daylight saving shows up as drift, so it is followed too. |
| `schedule.refresh_interval` | `1h` | How often the schedule pages in use are read again, at least `1min`. A change made at the controller is picked up sooner. |
| `schedule.allow_writes` | `false` | Lets the schedule entities change the controller's programs. Every write is logged in hex, before and after. |
| `experimental` | `false` | Allows the commands not yet confirmed on a TM2: next zone, test all zones, irrigation on/off and `rainbird.send_raw`. |
| `handshake` | `true` | Exchanges challenges with the controller before identifying it. A controller that has no handshake is identified without it. |
| `accept_model_ids` | none | Extra model ids to treat as this model. An id the component does not know leaves the hub read-only. |
| `listen_only` | `false` | Sends nothing. Status is decoded from the controller's replies to another master, such as an LNK2. Needs no transmitter. |
| `monitor_receiver_id` | none | A second receiver on the controller's RX line, to log another master's requests. For protocol work. |
| `zone_defaults`, `zones` | all 12 zones | See below. `zones: []` creates none. |
| `program_defaults`, `programs` | all 3 programs | See below. `programs: []` creates none. |
| `on_nak` | | Runs when the controller refuses a request. Variables `command` and `code`. |
| `on_no_reply` | | Runs when a command got no answer at all. It is not sent again. Variable `command`. |
| `on_frame` | | Runs for every valid reply. Variable `payload`, a `std::vector<uint8_t>`. |

## Zones and programs

Name a zone or program once on the hub and its entities are generated from the
name.

```yaml
rainbird:
  model: ESP-TM2
  zone_defaults:
    valve_switch: "{zone}"
    run_duration_number:
      name: "{zone} duration"
      initial_value: 10
      restore_value: true
    running_binary_sensor: true
  zones:
    - zone: 1
      id: front_lawn
      name: Front lawn
    - zone: 2
      name: Back lawn
      running_binary_sensor: false    # not for this zone
  programs:
    - program: A
      name: Lawns
      watering_day_switches: true     # only for this program
```

- **Which entities.** With no `zone_defaults`, each zone gets the entities marked
  "by default" in the tables below. With `zone_defaults`, each zone gets exactly
  the keys listed there. A zone can add a key of its own, or set one to `false`.
  Programs and `program_defaults` work the same way.
- **Values.** A key takes `true`, a name, or the entity's full ESPHome schema
  (`icon`, `entity_category`, `on_turn_on`, `filters` and so on). A zone's own
  settings are merged over the defaults.
- **Names.** `{zone}`, `{program}`, `{day}` and `{slot}` in a name are filled in.
- **Ids.** A zone's id defaults to `<hub id>_zone<N>` and a program's to
  `<hub id>_program_<letter>`. Each generated entity's id is the zone or program
  id, an underscore and the hub key: `front_lawn_valve_switch`. Keys that make
  several entities add the program letter, day or slot:
  `front_lawn_run_time_numbers_a`.
- **Zone keys:** `zone` (1 to 12, required), `name`, `id`, `disabled` (no
  entities for this zone) and `device_id` (passed to every entity of the zone).
- **Program keys:** `program` (`A` to `C`, required), `name` and `id`.

Any generated entity can be written by hand instead, and the result is the same:

```yaml
switch:
  - platform: rainbird
    zone: 4
    valve:
      name: Veggie patch
number:
  - platform: rainbird
    zone: 4
    run_duration:
      name: Veggie patch duration
```

Some controls use another entity's value, and validation fails if that entity is
missing: a valve switch or queue button needs its zone's `run_duration`, a
`start_time_enabled` switch needs that slot's `start_time`, and
`test_all_zones` needs `test_duration`.

## Entities

In the "Writes" column, `command` entities send a controller command,
`schedule page` entities edit the controller's schedule and need
`schedule: allow_writes: true`, and `kept on the device` entities are settings
the controller never sees.

<!-- entities:start (generated by tools/gen_docs.py; do not edit by hand) -->

### Zone entities

Set under a zone or in `zone_defaults` by hub key, or written by hand as a
`platform: rainbird` entry with `zone:` and the platform key.

| Hub key | Platform key | Type | By default | Default name | Writes | Notes |
|---|---|---|---|---|---|---|
| `valve_switch` | `valve` | switch | yes | `{zone}` | command | On runs the zone for its run duration. Off stops all watering. |
| `run_duration_number` | `run_duration` | number, 1 to 255 min | yes | `{zone} duration` | kept on the device | Minutes the valve switch and queue button run the zone for. |
| `running_binary_sensor` | `running` | binary_sensor |  | `{zone} running` | read-only | The same state as the valve switch, read-only. |
| `queue_button` | `queue` | button |  | `Queue {zone}` | command | Adds the zone to the queue for its run duration. |
| `run_time_numbers` | `run_time` | number, 0 to 65535 min |  | `{zone} {program} run time` | schedule page | One per program; a platform entry also takes `program:`. |

### Program entities

Set under a program or in `program_defaults` by hub key, or written by hand with
`program:` and the platform key.

| Hub key | Platform key | Type | By default | Default name | Writes | Notes |
|---|---|---|---|---|---|---|
| `start_button` | `start` | button | yes | `Run {program}` | command | Runs the program now. |
| `seasonal_adjust_number` | `seasonal_adjust` | number, 5 to 200 % | yes | `{program} seasonal adjust` | command |  |
| `running_binary_sensor` | `running` | binary_sensor |  | `{program} running` | read-only |  |
| `cycle_days_number` | `cycle_days` | number, 0 to 255 d |  | `{program} cycle days` | schedule page |  |
| `days_until_next_number` | `days_until_next` | number, 0 to 255 d |  | `{program} days until next run` | schedule page |  |
| `watering_day_switches` | `watering_day` | switch |  | `{program} {day}` | schedule page | One per day; a platform entry also takes `day:`. |
| `start_time_switches` | `start_time_enabled` | switch |  | `{program} start {slot} enabled` | schedule page | One per start slot; a platform entry also takes `slot:`. Turning one on writes the time its `start_time` shows. |
| `frequency_select` | `frequency` | select |  | `{program} frequency` | schedule page | `Custom days`, `Cyclic`, `Odd days` or `Even days`. |
| `start_time_datetimes` | `start_time` | datetime |  | `{program} start {slot}` | schedule page | One per start slot; a platform entry also takes `slot:`. |

### Controller entities

These are only written by hand, as `platform: rainbird` entries under the type's domain.

| Platform key | Type | Default name | Writes | Notes |
|---|---|---|---|---|
| `remaining_time` | sensor, s | `Time remaining` | read-only | Counts down every second between polls. |
| `active_zone` | sensor | `Active zone` | read-only | 0 when nothing is running. |
| `queued_zones` | sensor | `Zones queued` | read-only |  |
| `response_time` | sensor, ms | `Controller response time` | read-only |  |
| `link_errors` | sensor | `Controller link errors` | read-only | Requests that timed out, retries included. |
| `watering` | binary_sensor | `Watering` | read-only |  |
| `rain_sensor` | binary_sensor | `Rain sensor` | read-only | Polarity is unconfirmed; add `filters: [invert]` if it reads backwards. |
| `irrigation_enabled` | binary_sensor | `Irrigation enabled` | read-only |  |
| `connected` | binary_sensor | `Controller connected` | read-only |  |
| `active_program` | text_sensor | `Active program` | read-only | The program's name, `Manual` or `Idle`. |
| `queue` | text_sensor | `Queue` | read-only | For example `Front lawn 24:48, Back lawn 20:00`, or `Empty`. |
| `model` | text_sensor | `Controller model` | read-only |  |
| `firmware_version` | text_sensor | `Controller firmware` | read-only |  |
| `serial_number` | text_sensor | `Controller serial` | read-only |  |
| `schedule_changed` | text_sensor | `Schedule last changed` | read-only | Needs `time_id`. |
| `stop` | button | `Stop watering` | command | Stops all watering. |
| `advance` | button | `Next zone` | command | Moves on to the next queued zone. Needs `experimental: true`. |
| `test_all_zones` | button | `Test all zones` | command | Runs every zone for the test duration. Needs `experimental: true`. |
| `sync_clock` | button | `Sync controller clock` | command | Needs `time_id`. |
| `refresh` | button | `Refresh from controller` | command | Reads everything again, the schedule included. |
| `rain_delay` | number, 0 to 14 d | `Rain delay` | command |  |
| `station_delay` | number, 0 to 32400 s | `Delay between zones` | schedule page |  |
| `test_duration` | number, 1 to 255 min | `Test duration` | kept on the device | Minutes per zone for the test button. |
| `irrigation_enabled` | switch | `Irrigation enabled` | command | Needs `experimental: true`. |
| `rain_sensor_bypass` | switch | `Ignore rain sensor` | schedule page |  |
| `ends_at` | text_sensor | `Watering ends` | read-only | When the time remaining runs out. Changes once per zone. Needs `time_id`. |
| `controller_clock` | datetime | `Controller clock` | command | Setting it sets the controller's time and date. |

<!-- entities:end -->

## Actions

Most things are done with ESPHome's own entity actions: `button.press` to stop
or run a program, `number.set` for rain delay, `switch.turn_on` for a zone.
Three actions take arguments no entity can:

| Action | Arguments | What it does |
|--------|-----------|--------------|
| `rainbird.zone.start` | `zone` (a zone id or number), `duration` | Starts the zone now. |
| `rainbird.zone.queue` | the same | Adds the zone to the queue. |
| `rainbird.send_raw` | `data` (a list of bytes) | Sends any request once. The reply arrives through `on_frame`. Needs `experimental: true`. |

Both zone arguments can be templates. A duration is rounded up to whole minutes.
A zone outside 1 to 255 or a duration outside 1 ms to 255 minutes is refused,
never clamped.

[packages/ha-actions.yaml](packages/ha-actions.yaml) defines Home Assistant
actions (`run_zone`, `queue_zone`, `set_rain_delay`, `stop_watering`) on top of
these. Include it as a package; `examples/full.yaml` shows how.

## Behaviour worth knowing

- **Nothing is shown until the controller says it.** A switch turns on when the
  controller reports the zone running, not when you press it. A refused command
  logs a warning and the entity stays as it was.
- **Commands are sent once.** A command whose reply is lost is not sent again,
  because the controller may already have acted on it. The log says so and
  `on_no_reply` runs; the next poll shows what happened.
- **When the controller stops answering**, sensors and numbers go to unknown,
  binary sensors lose their state, and waiting commands are dropped. Switches,
  selects, times and most text sensors cannot show unknown and keep their last
  value; the `connected` sensor says whether to trust them. Everything is read
  again when the controller answers.
- **Schedule edits go a page at a time.** Each one reads the page, changes the
  field, writes the page back and reads it again. Edits to the same page made
  together, such as a script setting several watering days, share one read and
  one write. Setting a field to the value it already has writes nothing. Any
  other edit waits its turn, and none is dropped for want of room.
- **A controller that never answers is reported at once.** The first request
  that gets no reply marks the link down, and the log says whether the receiver
  heard nothing or heard a signal it could not decode. Once the controller has
  answered, it takes three failed requests in a row.
- **An unknown controller is read-only.** If the controller reports a model id
  the component does not recognise, status still works and every command is
  refused, until `accept_model_ids` lists the id.
- **`remaining_time` changes every second while watering.** `ends_at` shows the
  same thing as one timestamp, which Home Assistant counts down to by itself.

## Development

```bash
make -C tests/unit -j test         # host tests of the protocol and session code
make -C tests/unit -j test SAN=1   # the same under AddressSanitizer and UBSan
pytest                             # config expansion and validation; uses esphome if installed
```

Two files are generated from `components/rainbird/entity_kinds.py` and
`models.py`, and the tests fail if they are out of date:

```bash
python3 tools/gen_kinds.py   # entity_kind.h and model_limits.h
python3 tools/gen_docs.py    # the entity tables above
```

## More

- [WIRE.md](WIRE.md): the connector, the interface circuit and the signal.
- [SPEC.md](SPEC.md): the serial protocol, with what is confirmed and what is not.
- [PLAN.md](PLAN.md): how the component is designed. Code comments cite its
  sections.
- [HISTORY.md](HISTORY.md): the build schedule and the design review notes.
- [PLAN-GPIO.md](PLAN-GPIO.md): a proposal to replace `remote_receiver`.
- [examples/sniffer.md](examples/sniffer.md): bench notes from the first loopback.

## Acknowledgements

I did the initial feasibility study and LNK2 hardware analysis.

After incoming signals were being received, control over the Saleae Logic probe was handed over to Grok Bot for logging and signal analysis.

The protocol knowledge comes from [pyrainbird](https://github.com/allenporter/pyrainbird).

Development has been a collaborative effort between myself, Grok 4.7 and Opus 5.5, with additional code reviews by GPT 6 Sol, Kimi K3 and Muse Wonder.