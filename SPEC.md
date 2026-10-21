# Rain Bird ESP-TM2 serial protocol

This document collects everything currently known about talking to a Rain Bird
ESP-TM2 controller through its LNK2 accessory port. It is the reference for
the `rainbird` ESPHome component in this repository.

## Sources and confidence

Two sources feed this document:

- **WIRE.md**: oscilloscope captures of a real LNK2 module talking to an
  ESP-TM2. It covers the physical layer and framing, and it confirms that the
  payloads are ordinary Rain Bird SIP commands.
- **pyrainbird** (commit `aac99b0`): a Python library that sends the same SIP
  commands to the LNK2 over WiFi, wrapped in JSON-RPC (`tunnelSip`). Its
  command schema (`pyrainbird/resources/sipcommands.yaml`), decoders
  (`pyrainbird/rainbird.py`), docs (`docs/*.md`) and test captures of real
  device replies (`tests/testdata/*.yaml`) supply the command set.

The LNK2 is a bridge. The hex string pyrainbird puts in a `tunnelSip` request
is exactly the payload the LNK2 sends down the wire inside a serial frame, and
the reply payload comes back unchanged. Everything pyrainbird knows about
command payloads therefore applies directly to the serial port.

Each fact below carries a confidence tag:

| Tag | Meaning |
|-----|---------|
| **[wire]** | Seen in the WIRE.md captures on a real ESP-TM2 + LNK2 |
| **[test]** | Real device reply recorded in pyrainbird's test data (over WiFi) |
| **[code]** | Implemented in pyrainbird's encoder or decoder, but no capture backs the detail |
| **[doc]** | Stated only in pyrainbird's docs or in the command names it extracted from the Rain Bird app |
| **[inferred]** | Worked out in this document from the evidence; not stated anywhere |

Where sources disagree, the conflict is called out with **⚠ Conflict**.

---

## 1. Physical layer [wire]

### 1.1 Connector

The ESP-TM2 accessory connector is a 5-pin, JST-compatible header.

| Pin | Function | Notes |
|-----|----------|-------|
| J1 | 24 V AC | Powers the module |
| J2 | Controller TX | Controller → module, through optocoupler 1 |
| J3 | Signal return | Common return for both data lines |
| J4 | Controller RX | Module → controller, through optocoupler 2 |
| J5 | 24 V AC | Powers the module |

The LNK2 isolates both data lines with VO617A-2 optocouplers. A replacement
needs a 24 V AC → 5 V DC supply, an ESP32, two optocouplers and a few
resistors.

### 1.2 Line coding

- **Manchester**, 2400 bit/s. One bit lasts 416.7 µs and is split into two
  208.3 µs halves. Only two pulse widths ever appear on the line: ~208 µs and
  ~416 µs.
- **High then low = 1**, **low then high = 0** (G.E. Thomas convention).
- **MSB first.**
- The line **idles low** between frames.
- No UART-style start or stop bits were reported. Treat each frame as a
  continuous Manchester bitstream of whole bytes. **[inferred]**

### 1.3 Signal polarity at the ESP32

These observations are at the LNK2's own ESP32 pins:

- The module transmits on **GPIO26**.
- Replies arrive **inverted** at optocoupler 1's emitter compared with the
  module's transmit polarity.
- Replies **start with an extra half-bit** before the first data bit.

A receiver should not hard-code polarity or the starting phase. The `F5`
preamble bytes (section 2) give it a known pattern to lock onto first.
**[inferred]**

### 1.4 LNK2 debug pads (for reference only)

The LNK2 board exposes its ESP32 programming pins on a castellated edge,
counted from the top with the connector up: 1 EN, 4 GND, 6 GPIO1 (TX0),
7 GPIO3 (RX0), 8 3V3, 9 GPIO0. Its firmware is flash-encrypted, so a flash
dump is not usable. Whether the LNK2 can be reflashed with ESPHome is unknown.

---

## 2. Link layer [wire]

### 2.1 Frame format

```
Request  (module → controller):     F5 F5    1C <len> <payload…> <chk> 1D
Reply    (controller → module):     F5 F5 F5 1C <len> <payload…> <chk> 1D
```

| Field | Size | Meaning |
|-------|------|---------|
| `F5` | 2 bytes in requests, 3 in replies | Preamble / sync |
| `1C` | 1 | Start of frame |
| `len` | 1 | Number of payload bytes |
| `payload` | `len` | SIP command or reply (section 4 onwards) |
| `chk` | 1 | `(len + sum(payload)) & 0xFF` |
| `1D` | 1 | End of frame |

Every captured frame passed this checksum. The receiver should read exactly
`len` payload bytes rather than scan for `1D`, because `1D` can also appear in
the payload or checksum. **[inferred]**

pyrainbird's `tunnelSip` `length` parameter is the same number as `len`, and
its `data` hex string is the `payload`.

### 2.2 Worked examples [inferred]

These were computed from the rule above, not captured.

| Purpose | Frame |
|---------|-------|
| Run zone 1 for 6 min | `F5 F5 1C 04 39 00 01 06 44 1D` |
| Stop irrigation | `F5 F5 1C 01 40 41 1D` |
| ACK of stop | `F5 F5 F5 1C 02 01 40 43 1D` |

### 2.3 Transaction rules [wire]

- The protocol is strictly request/response. The controller sends exactly one
  reply about **30 ms** after each request and never sends anything
  unprompted.
- A reply opcode is the request opcode + `0x80` (`12` → `92`). Set commands
  are answered with ACK `01 <opcode>` instead (section 3.3).
- The LNK2 always sends **requests with a 2-byte payload twice, back to
  back**. Requests with 1-byte and 4-byte payloads go once. The captures don't
  show why. It could be a workaround for a receiver quirk in the controller.
  Whether the controller answers both copies is not stated. Test both ways
  before relying on either.
- Keep one request in flight at a time. pyrainbird notes that controllers only
  handle one client request at a time. **[doc]**

---

## 3. Payload conventions

### 3.1 Encoding [code]

- The first payload byte is the opcode.
- Multi-byte integers are **big-endian**.
- **Stations (zones) are numbered from 1.** Station 0 means "none".
- **Programs are numbered from 0** on the wire: 0 = A, 1 = B, 2 = C.
  pyrainbird names them `PGM A`, `PGM B` and so on (`data.py`).
- Zone bitmasks are 4 bytes per page and cover 32 stations. Within each byte,
  **bit 0 is the lowest station**. Byte *n* of page *p* covers stations
  `32p + 8n + 1` to `32p + 8n + 8`. Example: `3F 00 00 00` = stations 1–6.
  **[test]**

### 3.2 Packed date [test] [wire]

Dates use 3 bytes: `DD MY YY`.

- `DD`: day of month.
- High nibble of the 2nd byte: month (1–12).
- Low 12 bits of bytes 2–3: year.

Example: `13 A7 EA` → day `0x13` = 19, month `0xA` = 10, year `0x7EA` = 2026,
so 19/10/2026.

### 3.3 Generic replies

**`01` ACK** [test]

```
01 <echo>
```

The controller accepted the command `<echo>`. Example: `01 39`.

**`00` NAK** [test] [doc]

```
00 <echo> <code>
```

The controller rejected `<echo>`. `<code>` is a bitmask. The frame layout is
in pyrainbird's schema, but the bit meanings below come only from its docs
(open question 15):

| Bit | Value | Meaning |
|-----|-------|---------|
| 0 | `01` | Command not supported |
| 1 | `02` | Bad length |
| 2 | `04` | Incompatible data (out of range) |
| 3 | `08` | Checksum error |

Example: `00 02 03`.

---

## 4. ESP-TM2 identification and limits

### 4.1 Model IDs [code]

Model IDs are returned by `02` (section 5.1).

| Model ID | pyrainbird code | Notes |
|----------|-----------------|-------|
| `0005` | `ESP_TM2` | Original TM2. pyrainbird lists no `4C` combined state, no `4A` event timestamp and no `42` advance. |
| `000A` | `ESP_TM2v2` | "Upgraded TM2" |
| `010A` | `ESP_TM2v3` | "Upgraded TM2" |
| `0014` | `TM2R` | "Upgraded TM2" |
| `0015` | `TRU` | "Upgraded TM2" |

The LNK2 in WIRE.md polls `4C` and `4A`, so a TM2 fitted with an LNK2 is
probably an upgraded model. **[inferred]**

### 4.2 Limits [code]

From `models.yaml` and `docs/PROTOCOL_ESP_ME_TM2.md`:

| Limit | Value |
|-------|-------|
| Stations | up to 12 (use `03` to find which are fitted) |
| Programs | 3 (A, B, C) |
| Start times per program | 4 |
| Run-time schedule pages | 6 (`0x80`–`0x85`, 2 stations each) |
| Rain delay | 0–14 days |
| Max run time | 21 600 s (6 h). The manual run command `39` can only carry 1–255 minutes. |
| Seasonal adjust (water budget) | per program. `models.yaml` sets a maximum of 200 %. The only stated minimum is 10 %, in the docstring of pyrainbird's `set_water_budget`; nothing enforces it (open question 14). |

### 4.3 Feature flags [doc]

From pyrainbird's feature matrix, TM2 / Upgraded TM2:

| Feature | TM2 | Upgraded TM2 |
|---------|-----|--------------|
| Program-based schedules | yes | yes |
| Stacked manual runs (`4B`) | yes | yes |
| Combined state (`4C`) | no | yes |
| Schedule timestamp (`4A`) | no | yes |
| Advance station (`42`) | no | yes |
| Manual queue | no | yes |
| Seasonal adjust per program | yes | yes |
| Seasonal adjust per zone (`32`) | no | no |
| Sensor bypass per zone | yes | yes |
| Global water budget is ignored | yes | yes |

`04` (CommandSupportRequest) can confirm each command on a specific unit at
runtime.

---

## 5. Command reference

Request and reply payloads are shown as hex bytes without the frame. Byte
offsets start at 0, the opcode.

Each command lists its **Relevance**: what it means for irrigation or
configuration, and what it is good for in this project.

### 5.1 Identification and capabilities

#### `02` ModelAndVersionRequest → `82` [test] [code]

```
→ 02
← 82 MM MM VJ VN
```

| Offset | Field | Meaning |
|--------|-------|---------|
| 1–2 | modelID | See 4.1 |
| 3 | protocolRevisionMajor | SIP protocol major version |
| 4 | protocolRevisionMinor | SIP protocol minor version |

Example: `82 00 06 09 0C` = model `0006` (an ST8x), protocol 9.12.

**Relevance:** Identifies the controller variant and decides which optional
commands to use. Read it once at boot. Text sensor: model / protocol.

#### `03` AvailableStationsRequest → `83` [test] [code]

```
→ 03 PP              (PP = page; 00 for stations 1–32)
← 83 PP MM MM MM MM  (bitmask, see 3.1)
```

Example: `83 00 3F 00 00 00` = stations 1–6 fitted.

**Relevance:** Tells you which zones are physically available. Use it to warn
about configured zones that are not fitted and refuse commands for them, and
to skip unused zones when reading the schedule. Read it at boot.

#### `04` CommandSupportRequest → `84` [test] [code]

```
→ 04 CC
← 84 CC SS           (SS: 01 = supported, 00 = not)
```

Example: `84 82 01`.

**Relevance:** Capability probe for optional commands (`4C`, `4A`, `42`,
`4B`, `3D` and so on). Run it at boot and keep the result. Diagnostic only.

#### `05` SerialNumberRequest → `85` [test] [code]

```
→ 05
← 85 + 8 bytes serial number
```

Example: `85 00 00 00 00 00 00 89 63`.

**Relevance:** Device identity, for a diagnostic text sensor. Not seen in the
LNK2 poll.

#### `0B` ControllerFirmwareVersionRequest → `8B` [wire] [test]

```
→ 0B
← 8B MJ MN PP PP
```

| Offset | Field |
|--------|-------|
| 1 | major |
| 2 | minor |
| 3–4 | patch (u16) |

Example: `8B 01 2F 00 00` = firmware 1.47.0.

**Relevance:** Controller firmware version, for a diagnostic text sensor. The
LNK2 polls this.

### 5.2 Clock

The controller schedules irrigation from its own clock, so keeping it right
matters.

#### `10` CurrentTimeRequest → `90` [wire] [test]

```
→ 10
← 90 HH MM SS        (24 h)
```

Example: `90 0C 0A 28` = 12:10:40.

#### `11` SetCurrentTimeRequest → `01 11` [test]

```
→ 11 HH MM SS
← 01 11
```

Example: `11 16 2F 13` sets 22:47:19.

#### `12` CurrentDateRequest → `92` [wire] [test]

```
→ 12
← 92 DD MY YY        (packed date, 3.2)
```

#### `13` SetCurrentDateRequest → `01 13` [test]

```
→ 13 DD MY YY
← 01 13
```

Example: `13 09 17 E7` sets 9 Jan 2023.

**Relevance (10–13):** Shows the controller clock and lets ESPHome set it from
Home Assistant or SNTP. That fixes drift and daylight-saving changes, which
would otherwise need fixing at the front panel. Entities: a "Controller time" text sensor, a
"Sync clock" button, and optionally an automatic sync. The Rain Bird app reads
the value back after each set. **[doc]**

### 5.3 Live irrigation status

#### `3F` CurrentStationsActiveRequest → `BF` [wire] [test]

```
→ 3F PP              (PP = page; LNK2 polls 00 and 01)
← BF PP MM MM MM MM  (bitmask of stations running now)
```

Example: `BF 00 01 00 00 00` = zone 1 running.

**Relevance:** The main "which zone is watering" signal. Entities: one binary
sensor per zone, and the state of the per-zone switches.

#### `4C` CombinedControllerStateRequest → `CC` [wire] [test] (Upgraded TM2)

```
→ 4C
← CC hh mm ss DD MY YY RR RR SS IS AA AA TT TT ZZ
```

| Offset | Field | Meaning |
|--------|-------|---------|
| 1–3 | hour, minute, second | Controller clock |
| 4–6 | packed date | 3.2 |
| 7–8 | delaySetting | Rain delay days remaining (u16) |
| 9 | sensorState | Rain sensor (see `3E`) |
| 10 | irrigationState | See `48` |
| 11–12 | seasonalAdjust | `FFFF` on TM2, which uses per-program budgets (`30`) instead |
| 13–14 | remainingRuntime | **Seconds** left on the running zone (u16) |
| 15 | activeStation | Running zone, 0 = none |

Example **[test]**: `CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00` =
20:11:35 on 8 Jan 2023, rain delay 3 days, sensor 0, irrigation state 1, no
zone running.

⚠ **Conflict:** pyrainbird's docs say `remainingRuntime` is in minutes. In the
WIRE.md capture it was `0x0167` = 359 right after a 6-minute zone start, which
can only be seconds. **Use seconds.**

**Relevance:** A single poll for most of the dashboard: clock, rain delay,
rain sensor, irrigation enabled, active zone and time remaining. Use it as the
main status poll when supported.

#### `3B` CurrentQueueRequest → `BB` [wire] [test] [code]

This is the running zone plus the queue of zones waiting to run.

**Page 0: what is running now**

```
→ 3B 00
← BB 00 NN 00 TT TT 00 00 ZZ PP 00 AA     (12 bytes)
```

| Offset | Field | Meaning |
|--------|-------|---------|
| 2 | NN | Number of zones queued, including the running one **[inferred]** (5 when 5 entries are on page 1) |
| 4–5 | remaining | **Seconds** left on the running zone **[test]** **[wire]** |
| 8 | zone | Running zone, 0 = none |
| 9 | program | 0 = A, 1 = B, 2 = C. `FF` = none, i.e. a manual run or idle **[inferred]** |
| 11 | AA | `01` while running, `00` when idle **[inferred]** |
| 3, 6, 7, 10 | ? | Always `00` in the captures |

Examples **[test]**:

- Idle: `BB 00 00 00 00 00 00 00 00 FF 00 00`
- Running program A, zone 1, 1488 s left: `BB 00 05 00 05 D0 00 00 01 00 00 01`

pyrainbird maps any program number above 4 to 0, which loses the
`FF` = none case. Decode the byte directly.

**Page 1: the queue**

```
→ 3B 01
← BB 01 [QZ TT TT] × 11                   (35 bytes)
```

| Part | Meaning |
|------|---------|
| `QZ & 0x1F` | Zone number. Zone 0 ends the list. |
| `QZ & 0xE0` | Always `0x20` in the capture of program A. It may encode the program. **[inferred]** |
| `TT TT` | Run time in seconds, big-endian |

Example **[test]**: `BB 01 21 05 D0 22 04 B0 23 01 A4 24 04 B0 25 02 58 00…`
= zone 1 for 1488 s (running), zone 2 for 1200 s, zone 3 for 420 s, zone 4 for
1200 s, zone 5 for 600 s.

⚠ **Conflict:** `docs/PROTOCOL_ESP_ME_TM2.md` says the upgraded TM2 uses a
byte-swapped, 4-byte-per-entry queue format. The TM2 test captures and
pyrainbird's own decoder for 12- and 35-byte replies use the big-endian
3-byte format shown above. **Trust the captures.** The other format matches
ME3 replies (7-byte page 0).

**Relevance:** The running program, the time left and what runs next. Entities:
"Active program", "Time remaining" and "Queue" sensors. The LNK2 polls page 0.

#### `48` CurrentIrrigationStateRequest → `C8` [test] [code]

```
→ 48
← C8 SS
```

⚠ **Conflict:** pyrainbird's docs say `1` = irrigating and `0` = idle. Every
capture shows `1` while nothing is running: `C8 01` with an empty queue, and
`CC … 01 …` with no active zone. This is most likely **"irrigation enabled"**:
the controller is not switched off, so automatic schedules will run.
**[inferred]** Confirm by switching the controller to OFF.

**Relevance:** Shows whether automatic watering is enabled. Binary sensor
"Irrigation enabled". Also available as byte 10 of `CC`.

#### `3E` CurrentRainSensorStateRequest → `BE` [wire] [test]

```
→ 3E
← BE SS
```

⚠ **Conflict:** pyrainbird's code, used by the Home Assistant integration,
treats a non-zero value as "rain detected". Its protocol doc says the
opposite: 0 = rain detected, 1 = no rain. Test on the real unit by opening the
sensor input (remove the jumper) and watching the value change.

**Relevance:** Rain sensor binary sensor. Also available as byte 9 of `CC`.

#### `4A` ControllerEventTimestampRequest → `CA` [wire] [test] (Upgraded TM2)

```
→ 4A EE              (EE = event ID; the LNK2 polls 01)
← CA EE TT TT TT TT
```

The timestamp is **seconds since 2000-01-01 00:00**, apparently in controller
local time. **[inferred]** In the test capture, `CA 01 2B 48 31 63` decodes to
2023-01-04 12:45:23, and the combined state in the same capture reads
2023-01-04 18:40.

What event 1 is isn't documented. pyrainbird's feature list has a "schedule
timestamp" feature for the upgraded TM2, so event 1 is probably **when the
schedule last changed**. **[inferred]** The app would use it to decide when to
re-read the schedule. Other event IDs are unknown.

**Relevance:** A cheap way to tell when the schedule was changed at the
controller's buttons, so ESPHome only re-reads the schedule pages when needed.
Entity: "Schedule last changed" timestamp sensor.

#### `3D` CurrentStationErrorRequest → (`BD`?) [wire] [doc]

```
→ 3D PP              (LNK2 polls 00, 01, 02)
← ?? PP …            (echoes the page; the rest is unknown)
```

The name comes from the Rain Bird app's command list, where it is described as
"Get station error (auto-increment)". It is not in pyrainbird's schema and no
reply was decoded. It is probably a per-station fault report, such as a
shorted solenoid wire, which the TM2 displays as an error. **[inferred]**

**Relevance:** Possibly zone fault binary sensors. Until it is decoded, log
the raw reply. A capture taken with a deliberately shorted or open zone would
settle it.

#### `36` RainDelayGetRequest → `B6` [wire] [test]

```
→ 36
← B6 DD DD           (u16 days)
```

Example: `B6 00 03` = 3 days.

**Relevance:** Rain delay remaining, in days. Also available as bytes 7–8 of
`CC`.

### 5.4 Manual control

#### `39` ManuallyRunStationRequest → `01 39` [wire] [code]

```
→ 39 ZZ ZZ MM        (zone u16, minutes u8)
← 01 39
```

Example: `39 00 01 06` runs zone 1 for 6 minutes.

The run replaces whatever is running now; pyrainbird describes it as
overriding the current program. **[doc]** Runs are limited to 1–255 minutes
by the one-byte field.

**Relevance:** Zone switches. Turning a zone on sends `39`, and turning it off
sends `40`. Pair each zone with a "run duration" number entity.

#### `4B` StackManuallyRunStationRequest → `01 4B` [wire] [code]

```
→ 4B PP ZZ MM        (page, zone within page, minutes)
← 01 4B
```

Example: `4B 00 01 01` queues zone 1 for 1 minute.

The zone is **added to the end of the queue** instead of replacing what is
running. **[doc]** For zones above 32, set `PP` to the 32-zone page; on a TM2
`PP` is always `00`.

**Relevance:** "Queue zone" action, for building a run sequence from Home
Assistant. Expose it as an ESPHome action or service rather than a switch.

#### `38` ManuallyRunProgramRequest → `01 38` [test] [code]

```
→ 38 PP              (0 = A, 1 = B, 2 = C)
← 01 38
```

**Relevance:** "Run program A/B/C" buttons.

#### `40` StopIrrigationRequest → `01 40` [wire] [code]

```
→ 40
← 01 40
```

Stops all watering. Whether it also clears the queue is unconfirmed; check
`3B 01` afterwards.

**Relevance:** "Stop all" button, and the off action for every zone switch.

#### `42` AdvanceStationRequest → `01 42` [code] (Upgraded TM2)

```
→ 42 PP
← 01 42
```

This skips ahead in the running sequence. The parameter is a zone number in
every source:

- pyrainbird's `advance_zone(param)` is documented as "Advance to the specified
  zone" and passes the integer straight through. Its tests send `42 08` and
  `advance_zone(3)`.
- The command table extracted from the app calls the byte "station".

Nothing in pyrainbird ever sends `00`. What remains unknown is whether the
zone named is the one to jump to or the one to skip. **[inferred]**

To find out, try these in order with a queue running:

1. the next queued zone (from `3B 01`);
2. the currently running zone;
3. `00`.

**Relevance:** "Advance to next zone" button, sending the next queued zone's
number.

#### `3A` TestStationsRequest → `01 3A` [code]

```
→ 3A NN
← 01 3A
```

This runs every zone in turn: the TM2 "Test all stations" function.

⚠ **Conflict:** pyrainbird's protocol doc says `NN` is the **minutes per
station**, but pyrainbird's API passes a zone number (`test_zone(zone)`). The
TM2 front-panel test asks for minutes, so minutes is more likely.
**[inferred]**

**Relevance:** "Test all zones" button, with a minutes number entity.

#### `49` CurrentControllerStateSet → `01 49` [doc]

```
→ 49 SS
← 01 49
```

The parameter is described only as "state". This is probably the setter that
pairs with `48`, switching automatic irrigation on (`01`) or off (`00`).
**[inferred]**

**Relevance:** If confirmed, an "Irrigation enabled" switch. Test with care,
and read the result back with `48`.

### 5.5 Settings

#### `30` WaterBudgetRequest → `B0` [wire] [test]

```
→ 30 PP              (program 0–2; the LNK2 polls 00, 01, 02)
← B0 PP SS SS        (u16 percent)
```

Example: `B0 00 00 64` = program A at 100 %.

#### `31` WaterBudgetSetRequest → `01 31` [test] [code]

```
→ 31 PP SS SS
← 01 31
```

Example: `31 00 00 50` sets program A to 80 %. LCR-series controllers use
`PP = FF` for a single global value; the TM2 is per program.

**Relevance (30/31):** Seasonal adjust per program. It scales every run time
in that program. Entity: a "Program A/B/C seasonal adjust" number from 10 to
200 %, step 1. The lower bound is unverified (open question 14). Home Assistant automations could drive it from weather data,
which is the job the LNK2's cloud weather service did.

#### `37` RainDelaySetRequest → `01 37` [test] [code]

```
→ 37 DD DD           (u16 days, 0–14)
← 01 37
```

Example: `37 00 0F` = 15 days. That example is out of range for a TM2 and
should NAK. The app reads the value back with `36` after setting it. **[doc]**

**Relevance:** Rain delay number entity, 0–14 days. 0 cancels the delay.

### 5.6 Schedule (`20` read, `21` write)

The TM2 keeps its schedule in **pages**:

```
→ 20 00 PP           read a page
← A0 00 PP <data>
```

The reply echoes the page as a u16. Program pages, start-time pages and
run-time pages are listed below.

| Page | Contents |
|------|----------|
| `00` | Global settings |
| `10`–`12` | Program A–C settings |
| `20` | Per-station rain sensor bypass |
| `60`–`62` | Program A–C start times |
| `80`–`85` | Run times, 2 stations per page |

Writes use the same page layout:

```
→ 21 00 PP <data>
← 01 21
```

pyrainbird only implements writes for the LCR series, so the TM2 write
layouts below come from `docs/PROTOCOL_ESP_ME_TM2.md` **[doc]** or follow by
symmetry with the read layout **[inferred]**. Read the page back after every
write.

#### Page `00`: global settings [test]

```
← A0 00 00 DD DD RR SS
```

| Offset in data | Field | Meaning |
|----------------|-------|---------|
| 0–1 | stationDelay | Delay between zones, in seconds per the doc (u16) |
| 2 | rainDelay | Rain delay days (matches `36` / `CC`) |
| 3 | rainSensor | `00` = sensor obeyed, `01` = sensor bypassed **[doc]** |

Example: `A0 00 00 00 00 04 00` = no station delay, 4-day rain delay, sensor
active.

Write **[doc]**: `21 00 00 DD DD RR SS`.

**Relevance:** "Station delay" number, and a global "Rain sensor bypass"
switch.

#### Pages `10`–`12`: program settings [test]

```
← A0 00 1P WW CC RR OO XX FF
```

| Offset | Field | Meaning |
|--------|-------|---------|
| 0 | daysOfWeekMask | Bit 0 = Sunday … bit 6 = Saturday. Used when the frequency is custom. |
| 1 | period | Cycle length in days. Used when the frequency is cyclic. |
| 2 | synchro | Days left until the next cyclic run |
| 3 | permanentDaysOff | Days-of-week mask of days never to water, for odd, even and cyclic programs **[doc]** |
| 4 | ? | See below |
| 5 | frequency | `0` custom days, `1` cyclic, `2` odd days, `3` even days |

Example: `A0 00 10 6A 06 01 00 64 01` = program A, cyclic every 6 days, next
run in 1 day. Its days mask `0x6A` (Mon, Wed, Fri, Sat) is ignored because
the frequency is cyclic.

⚠ **Byte 4** is called `seasonalAdjust` in the ME/TM2 doc and `reserved` in
pyrainbird's decoder. Captures show `0x64` and `0x2D` (45), and neither
matches the `B0` water budget read at the same time (100, 80, 80). Its meaning
is unknown. The doc says the app always writes `64`.

Write **[doc]**: `21 00 1P WW CC RR OO 64 FF`.

⚠ **Don't use this template as written.** It hard-codes byte 4 as `64`,
throwing away whatever the controller holds there. That contradicts
pyrainbird's own decoder, which treats the byte as `reserved`. Build every
write from a fresh read of the page, change only the edited fields, and write
bytes 3 and 4 back exactly as read. pyrainbird has no TM2 schedule encoder to
borrow: `SetScheduleRequest` is fixed at 14 bytes, and `encode_zone_schedule`
builds an LCR-series zone page.

**Relevance:** For each program:
- a "Frequency" select (custom, cyclic, odd, even)
- seven day-of-week switches
- a "Cycle days" number
- a "Days until next cyclic run" number
- a "Permanent days off" mask

#### Pages `60`–`62`: start times [test]

```
← A0 00 6P T1 T1 T2 T2 T3 T3 T4 T4
```

Each slot is a u16 of **minutes after midnight**. `FFFF` means the slot is
off. The TM2 has 4 slots.

Example: `A0 00 60 00 F0 FF FF FF FF FF FF` = program A starts at 04:00 only.

Write **[doc]**: `21 00 6P T1 T1 T2 T2 T3 T3 T4 T4`.

**Relevance:** Four start-time slots per program. Each slot is an ESPHome
`datetime` time entity plus an "enabled" switch, because a time entity can't
show `FFFF` (off).

#### Pages `80`–`85`: run times [test]

```
← A0 00 8N [A_pA A_pB A_pC] [B_pA B_pB B_pC]    (6 × u16 minutes)
```

Each page holds two stations:
- station A = `2N + 1`
- station B = `2N + 2`

so page `80` holds zones 1 and 2, and page `85` holds zones 11 and 12. Each
station has one run time per program (A, B, C) in minutes, and 0 means the
zone is not in that program.

Example: `A0 00 80 00 19 00 01 00 00 00 14 00 02 00 00` =
- zone 1: A 25 min, B 1 min, C off
- zone 2: A 20 min, B 2 min, C off

**Zone numbering in pyrainbird.** pyrainbird's `decode_schedule` returns
these two stations as `zone: 0` and `zone: 1`. The library adds 1 later
(`data.py`, `"zone": zone + 1`), so its public schedule is 1-based like
everything else here. Test vectors copied from the raw decoder output in
`tests/testdata/schedule*.yaml` must add 1 to every zone. Phase 5 of the plan
checks the mapping on hardware.

Write **[inferred]**: `21 00 8N` followed by the same 12 bytes. This is not
documented anywhere; try it on one page and read it back.

**Relevance:** One "Zone N run time, program X" number per zone and program:
up to 36 entities. Probably best grouped or created only for zones that are
fitted.

#### Page `20`: per-station rain sensor bypass [doc]

```
← A0 00 20 S1 S2 … Sn       (one byte per station)
```

`01` means the station obeys the rain sensor, and `00` means it is bypassed.
Note that this polarity is the **opposite** of page `00` byte 3. No capture
exists, and the reply length for a TM2 is unknown. The write is presumably
`21 00 20 S1…Sn`. **[inferred]**

**Relevance:** One "Zone N ignores rain sensor" switch per zone.

---

## 6. Commands that don't apply to the ESP-TM2

These exist in the Rain Bird command set but are for other controller families
or are entirely undocumented. Probe them with `04` before use.

| Opcode | Name | Why it's excluded |
|--------|------|-------------------|
| `09` | AlternateBitRateSupportRequest | Name only. Might allow a faster line rate; payload unknown. |
| `0C` / `8C` | UniversalMessageRequest | CDT/UPT transport used by ESP-ME3, RC2, ARC8, LX. The TM2 uses legacy SIP. |
| `2B` | SetTimeZoneRequest | Name only, no layout. |
| `32` / `B2`, `33` | Zones seasonal adjust get/set | The TM2 has no per-zone seasonal adjust. |
| `4D` / `CD` | IrrigationStatisticsRequest | Name only; listed as unsupported on the TM2. |
| `60`–`65` | Flow learning / flow monitor | The TM2 has no flow sensor input. |
| `70` | LogEntriesRequest | Name only, no layout. |

---

## 7. Observed LNK2 behaviour [wire]

### 7.1 Poll cycle

The LNK2 runs a fixed poll every **12.5–15 s**, and more often just after a
command. Each cycle sends, in this order:

```
12       date
3F 00    active stations 1–32
3F 01    active stations 33–64
3D 00    station errors (?), page 0
3D 01    page 1
3D 02    page 2
36       rain delay
3E       rain sensor
4A 01    schedule timestamp (?)
30 00    water budget, program A
30 01    program B
30 02    program C
4C       combined state
3B 00    running zone / queue
0B       firmware version
10       time
```

pyrainbird's device registry lists almost the same set as the Rain Bird app's
poll list for the upgraded TM2. **[doc]**

### 7.2 Features the LNK2 or cloud provided that are not on the wire

The LNK2 answers these JSON-RPC methods itself; they never reach the
controller:

- `getWifiParams`, `getNetworkStatus`, `getServerMode`: WiFi and cloud status.
- `getSettings`, `getWeatherAdjustmentMask`, `getZipCode`, `getProgramInfo`:
  location, soil type per program, weather opt-out mask per program, and a
  global weather disable.

Weather-based adjustment and custom zone and program names lived in the Rain
Bird cloud. Weather adjustment presumably reached the controller through the
`31` water-budget and `37` rain-delay commands documented above, since the
TM2 has no other adjustment input. **[inferred]** With ESPHome,
Home Assistant takes over that role. Zone and program names belong in the
ESPHome YAML.

---

## 8. Open questions

These need checking against a real controller. Captures from the ESPHome
component's debug log will settle most of them.

1. **Rain sensor polarity** (`3E`, `CC` byte 9): which value means rain?
2. **`48` / `CC` irrigationState**: confirm that it means "enabled" by setting
   the controller to OFF.
3. **`49`**: does `49 00` / `49 01` switch automatic irrigation off and on?
4. **`3D`**: reply opcode and layout. Capture with a faulty zone.
5. **2-byte requests sent twice**: needed or not? Does the controller reply
   twice?
6. **`3A`**: is the parameter minutes or a zone?
7. **`42`**: is the zone number the zone to jump to or the zone to skip?
   (Probe order in 5.4.)
8. **Schedule writes** (`21`) for pages `00`, `1P`, `6P`, `8N` and `20`: test
   one page at a time and read back.
9. **Program page byte 4**: what is it?
10. **Queue page 1 high bits** (`QZ & 0xE0`): is it the program number?
11. **`4A`**: other event IDs, whether event 1 really changes when the
    schedule is edited at the front panel, and whether the timestamp is
    controller local time or UTC.
12. **Model ID** of the target unit (`02`): `0005` or an upgraded variant?
13. **Line polarity and timing** at the replacement interface's own GPIO pins.
14. **Seasonal adjust minimum** (`31`): does the controller accept values below
    10 %, and what does 0 % do?
15. **NAK code bits**: do the codes the TM2 actually returns match the
    documented bit meanings (3.3)? Record every NAK seen while probing.

---

## Appendix: proposed ESPHome entities

This list covers every documented item. Items tagged † depend on an open
question in section 8.

| Entity | Type | Source | Write |
|--------|------|--------|-------|
| Zone N | switch (×12) | `3F` | on `39`, off `40` |
| Zone N run duration | number, minutes (×12) | ESPHome setting | – |
| Zone N active | binary_sensor (×12) | `3F` | – |
| Zone N available | (entity availability) | `03` | – |
| Zone N fault † | binary_sensor (×12) | `3D` | – |
| Zone N rain sensor bypass † | switch (×12) | page `20` | `21 00 20` |
| Active zone | sensor | `CC` / `3B 00` | – |
| Active program | text_sensor | `3B 00` | – |
| Time remaining | sensor, seconds | `CC` / `3B 00` | – |
| Queue | text_sensor | `3B 01` | – |
| Queue zone | action/service | – | `4B` |
| Run program A/B/C | button (×3) | – | `38` |
| Stop all | button | – | `40` |
| Advance zone † | button | – | `42` |
| Test all zones | button + number | – | `3A` † |
| Irrigation enabled † | binary_sensor / switch | `48` / `CC` | `49` † |
| Rain sensor † | binary_sensor | `3E` / `CC` | – |
| Rain sensor bypass (global) | switch | page `00` | `21 00 00` |
| Rain delay | number, 0–14 d | `36` / `CC` | `37` |
| Seasonal adjust A/B/C | number, % (×3) | `30` | `31` |
| Station delay | number, seconds | page `00` | `21 00 00` |
| Program X frequency | select (×3) | page `1P` | `21 00 1P` |
| Program X watering days | switch ×7 or text (×3) | page `1P` | `21 00 1P` |
| Program X cycle days / days until next run | number (×3 each) | page `1P` | `21 00 1P` |
| Program X start time 1–4 | time / text (×12) | page `6P` | `21 00 6P` |
| Zone N program X run time | number, minutes (×36) | page `8N` | `21 00 8N` † |
| Controller time | text_sensor | `10` + `12` / `CC` | – |
| Sync clock | button (plus optional automatic sync) | – | `11` + `13` |
| Schedule last changed † | timestamp sensor | `4A 01` | – |
| Model / protocol | text_sensor | `02` | – |
| Firmware | text_sensor | `0B` | – |
| Serial number | text_sensor | `05` | – |
| Link OK / errors / NAKs | binary_sensor / sensor | component | – |
