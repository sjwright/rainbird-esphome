# Rain Bird ESP-TM2 serial protocol

This document collects everything currently known about talking to a Rain Bird
ESP-TM2 controller through its LNK2 accessory port. It is the reference for
the `rainbird` ESPHome component in this repository.

## Sources and confidence

Four sources feed this document:

- **WIRE.md**: oscilloscope captures of a real LNK2 module talking to an
  ESP-TM2. It covers the physical layer and framing, and it confirms that the
  payloads are ordinary Rain Bird SIP commands.
- **ESP-ME3 teardown** by **adidooo**:
  [post #16](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/16)
  in the Home Assistant community thread "Reverse engineering accessory port
  Rain Bird ESP-RZX", 6 November 2026. adidooo traced an LNK2 board (firmware
  3.97) and captured its traffic with an ESP-ME3 (model `0009`, firmware
  2.12). The ME3 uses a different link layer but the same SIP commands. It is
  the main source for the LNK2 circuit in section 1 and for section 9.
- **Session handshake** by **biggestchungus**:
  [post #18](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/18)
  in the same thread, 8 November 2026. It gives the `06` / `07` challenge
  exchange and the T() transform in 5.0. Section 1.5 also cites post #18,
  and [post #14](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/14)
  by **banoz**, on the LNK2's firmware.
- **pyrainbird** (commit `aac99b0`): a Python library that sends the same SIP
  commands to the LNK2 over WiFi, wrapped in JSON-RPC (`tunnelSip`). Its
  command schema (`pyrainbird/resources/sipcommands.yaml`), decoders
  (`pyrainbird/rainbird.py`), docs (`docs/*.md`) and test captures of real
  device replies (`tests/testdata/*.yaml`) supply the command set. Later
  commits, read up to `2b23d42` (17 November 2026), add a device emulator
  (`pyrainbird/testing/`). Cited where used: the escape table (2.4), the manual
  run limit (5.4), the program frequency codes (pages `10`–`12`) and the queue
  program bits (5.3). The emulator's own defaults are not evidence of what a
  TM2 does.

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
| **[reported]** | Posted or told to us by someone else, with no capture of our own or pyrainbird source yet. The source is named where it is used. |
| **[ME3]** | From adidooo's ESP-ME3 teardown ([post #16](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/16)). Measured on an ME3, so it applies to the TM2 only where the hardware is shared |

Where sources disagree, the conflict is called out with **⚠ Conflict**.

---

## 1. Physical layer [wire]

### 1.1 Connector

The ESP-TM2 accessory connector is a 5-pin header with 2.54 mm pitch. A JST XH
flylead fits it. WIRE.md calls the pins J1–J5. On the LNK2 board they are pads
1–5 of connector J1, in the same order. The ESP-ME3 uses the same pinout.
**[ME3]**

| Pin | Function | Direction | Idle level vs J3, ESP-ME3 **[ME3]** |
|-----|----------|-----------|-------------------------------------|
| J1 | 24 V AC | supply | about 34.5 V AC |
| J2 | Controller TX | controller → module, through optocoupler 1 | 1.1 V DC with the LNK2 fitted (its LED lit); 0 V with nothing fitted |
| J3 | COM | return for both data lines | 0 V (reference) |
| J4 | Controller RX | module → controller, through optocoupler 2 | 3.2 V DC (the controller's pull-up) |
| J5 | 24 V AC | supply | about 10 V AC |

The controller side of the data lines, as measured on the ME3 **[ME3]**:

- J1 to J5 is about 28 V AC.
- **J3 is not one leg of the 24 V AC.** It sits between the two AC legs, so
  don't assume it is earth. Keep both data lines isolated from the ESP32, as
  the LNK2 does.
- **J2 drives the receive LED itself.** The controller supplies the current and
  limits it, so the LED goes straight across J2 and J3 with no series resistor.
  The current is not yet known.
- **J4 is pulled up inside the controller**, to about 3.2 V. The transmit
  optocoupler's transistor pulls it to J3.

The TM2 hasn't been measured. Its idle levels may differ, because its line
idles in a different state (1.4). See open question 16.

A replacement needs a 24 V AC → 5 V DC supply, an ESP32, two optocouplers and a
few resistors (1.3).

### 1.2 Line coding

- **Manchester**, 2400 bit/s. One bit lasts 416.7 µs and is split into two
  208.3 µs halves. Only two pulse widths ever appear on the line: ~208 µs and
  ~416 µs.
- **High then low = 1**, **low then high = 0** (G.E. Thomas convention).
- **MSB first.**
- The line **idles low** between frames.
- No UART-style start or stop bits were reported. Treat each frame as a
  continuous Manchester bitstream of whole bytes. **[inferred]**

### 1.3 The LNK2 interface circuit [ME3]

adidooo's ME3 teardown ([post #16](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/16)) traced the LNK2 board. WIRE.md's trace of a TM2's LNK2 agrees
where they overlap: two VO617A optocouplers, with the module transmitting on
GPIO26. So it is probably the same board. **[inferred]**

| | Optocoupler 1: receive | Optocoupler 2: transmit |
|---|---|---|
| LNK2 part | U2 | U3 |
| LED | Across J2 and J3, driven by the controller. No series resistor. | 3V3 → LED → 200 Ω → GPIO26 |
| Transistor | Collector on 3V3, emitter straight to **GPIO25**, no resistor (emitter follower) | Across J4 and J3 |
| Sense | Current out of J2 reads high at GPIO25 | GPIO26 low lights the LED and pulls J4 to J3 |

- Both paths are **non-inverting**: GPIO26 high leaves J4 high, and current
  out of J2 reads high at GPIO25.
- The transmit LED current is about (3.3 − 1.2) / 200 ≈ 10 mA.
- Both optocouplers sit on the solder side, directly under the J1 pads. U2 is
  mounted the other way round to U3.
- ⚠ **Conflict:** WIRE.md reads the parts as **VO617A-2** (current transfer
  ratio 63–125 %). adidooo's teardown reads **VO617A-4** (160–320 %). They may be
  different board revisions. For a build, prefer a higher grade (-3 or -4).
  The controller limits the receive LED current to a value no one has
  measured, so a higher ratio leaves more margin.

**For our own interface** the receive side does not have to copy the LNK2:

- PLAN.md's build puts the receive transistor's output on a pull-up of a few
  kΩ. That inverts relative to the LNK2's emitter follower. The receiver locks
  polarity on its own (1.4), so either works.
- Whichever topology is used, give the transistor a real load resistor. The
  LNK2's emitter follower has nothing pulling GPIO25 down, so its falling edge
  depends on leakage. adidooo fits a 1 kΩ pull-down on their own build
  (2.2 kΩ if the idle level sits below about 2.5 V). Slow edges skew the
  208 µs half-bits against the receiver's ±30 % tolerance.

### 1.4 Signal polarity at the ESP32

These observations are at the LNK2's own ESP32 pins:

- The module transmits on **GPIO26** and receives on **GPIO25**. **[ME3]**
- Replies arrive **inverted** at optocoupler 1's emitter (GPIO25) compared
  with the module's transmit polarity.
- Replies **start with an extra half-bit** before the first data bit.

A receiver should not hard-code polarity or the starting phase. The `F5`
preamble bytes (section 2) give it a known pattern to lock onto first.
**[inferred]**

Since the transmit path does not invert (1.3), GPIO26 idling low (1.2) means the
transmit LED is lit between frames, which holds J4 at J3. The ME3's UART is the
opposite: GPIO26 idles high and J4 sits at 3.2 V. **[inferred]** A meter on J4
with the LNK2 fitted and idle would settle it (open question 16).

### 1.5 The LNK2 board

- **Board:** PWBA 691040. **[ME3]**
- **Parts:** an ESP32-WROOM-32D module (U5, ESP32-D0WD), a switching
  regulator U1 with a 10 µH inductor L1, a pairing button S1 and LED D3.
  **[ME3]**
- **The regulator is not isolated.** The LNK2's ESP32 ground sits on the
  rectified 24 V AC. **[ME3]** A scope or logic analyser clipped to the LNK2's
  own pins must float: run it from a battery or an ungrounded supply, and keep
  its other leads off earth. Otherwise the rectified AC shorts through earth.
  Don't assume J3 is earth either (1.1). Float the instrument for any
  measurement on this port unless you have checked.
- **Debug pads:** the ESP32 programming pins are on a castellated edge,
  counted from the top with the connector up: 1 EN, 4 GND, 6 GPIO1 (TX0),
  7 GPIO3 (RX0), 8 3V3, 9 GPIO0.
- **Firmware:** flash-encrypted on the WIRE.md unit, so a flash dump is not
  usable. The ME3 unit was the same, with the key eFuse read-protected.
  **[ME3]** Two other posters in the same thread disagree. banoz has an
  identical-looking LNK2 with readable firmware
  ([post #14](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/14)), and biggestchungus says unencrypted firmware can be
  read by a fragile method ([post #18](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/18)). **[reported]** Whether the LNK2 can be reflashed with ESPHome is unknown.
- **Controller firmware updates:** the LNK2's cloud connection can trigger a
  controller firmware update, according to biggestchungus
  ([post #18](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/18)). **[reported]** An update could change
  the session handshake (5.0). Once an ESPHome interface replaces the LNK2,
  keep the LNK2 off the controller.

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
`len` payload bytes rather than scan for `1D`. **[inferred]** `1D` does not in
fact appear raw inside a frame: the controller escapes it (2.4). **⚠ Conflict**
with the earlier reading of this section, which assumed it could.

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

### 2.4 Byte stuffing [wire] [code] [ME3]

After the `1C` start byte, the controller sends a `1B`, `1C` or `1D` as `1B`
followed by a code. The length and checksum count the original bytes.

| Byte | On the wire | Seen |
|------|-------------|------|
| `1C` | `1B 01` | pyrainbird and the ME3; not yet from a TM2 |
| `1B` | `1B 02` | pyrainbird and the ME3; not yet from a TM2 |
| `1D` | `1B 03` | **[wire]** 13 Nov 2026 |

⚠ **Conflict** with this section's earlier reading, "the byte minus `1A`",
which swaps the first two (`1B → 1B 01`, `1C → 1B 02`). That rule was fitted
to the one `1D` sample and nothing else. Three things favour the table above:

- pyrainbird's frame code (`pyrainbird/testing/crypto.py`, #650) has it. The
  commits that later corrected that file's preamble from the LNK2 firmware
  (#667) and aligned its docs with the firmware left it as it was. Their
  firmware details include the controller's three `F5` bytes, which match
  our captures.
- 9.2 has the same table for the ME3's UART link, from adidooo's captures.
  pyrainbird says the LNK2's UART mode uses the same stuffing as its
  Manchester mode.
- An arbitrary order is an odd thing to make up. Someone inventing a rule
  would more likely have picked the arithmetic one.

The hub uses the table above both ways. With one escaped byte in a frame, the
wrong table fails the checksum, so the first escaped `1B` or `1C` in a reply
settles it: `4C` at :27 or :28 seconds does it. With a `1B` and a `1C` both
in a frame, the two readings give the same checksum.

Seen once, in a `4C` reply with the clock at 12:56:29
(`tests/captures/tm2-escaped-reply.txt`):

```
wire:     F5 F5 F5 1C 10 CC 0C 38 1B 03 0D B7 EA 00 00 01 01 FF FF 00 00 00 EB 1D
payload:  CC 0C 38 1D 0D B7 EA 00 00 01 01 FF FF 00 00 00          (16 bytes, chk EB)
```

The frame is one byte longer than `len` says, and `EB` is the checksum of the
payload with `1D`, not of the bytes on the wire. One sample also fits another rule:
XOR with `1E` gives `1D → 03` and `1C → 02`, but `1B → 05`.

Unknown: whether requests must be escaped too. The hub escapes them, with the
same table, and logs the first time it sends each escape, so a NAK or a timeout
right after that log line points here. It decodes a reply plain first, then
with escapes undone, and takes either only with a good checksum and end byte.

**Relevance:** `4C` carries the time, so without this its reply fails at
:27–:29 seconds, and for three minutes every hour at xx:27–xx:29. Any field
that reaches 27–29 (minutes, days, run times, schedule bytes) is affected the
same way.

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
  `32p + 8n + 1` to `32p + 8n + 8`. Example: mask bytes `3F 00 00 00` =
  stations 1–6. **[test]** So a mask is effectively a little-endian u32, the
  one exception to big-endian. The ME3 reads it the same way: `BF 00 04 00 00 00`
  while station 3 ran. **[ME3]**

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
(open question 15). One real log backs code `04`: a `39` with a run time too
long, NAKed with `04` (pyrainbird issue #178, controller not stated).
pyrainbird's emulator uses `01`, `02` and `04` the same way.

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
| Max run time | 21 600 s (6 h). The manual run command `39` carries 1–255 minutes, but a manual run stops at 100 (5.4). |
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

### 5.0 Session handshake

#### `06` / `07` challenge exchange [reported]

Not in pyrainbird. Posted by biggestchungus in
[post #18](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/18)
of the Home Assistant thread, 8 November 2026. It came into use here on
13 November 2026, after a TM2 on our own interface refused every command, `02`
and `12` included, with NAK `01`. Not yet confirmed on the wire. Each side picks a 32-bit challenge, most significant byte
first, and the other answers with T() of it:

```
→ 06 AA AA AA AA          our challenge A
← 86 T(A) BB BB BB BB     the controller's answer, then its challenge B
→ 07 T(B)                 our answer
← 87                      done
```

T() is 13 steps of a 32-bit LFSR. Each step takes the parity of `x & 0x80718611`,
shifts `x` left by one (dropping the top bit) with that parity as the new low
bit, and replaces an all-zero result with `FFFFFFFF`. Example: T(`12345678`) =
`8ACF17EB`. (Both the algorithm and the example are from biggestchungus.)

T() is linear over GF(2): T(a ⊕ b) = T(a) ⊕ T(b), apart from the all-zero
case. Flipping one input bit flips 3–11 output bits, about 7 on average.
**[inferred]** (computed from the definition)

The ME3's LNK2 runs an exchange of the same shape. Over 356 handshakes
captured by adidooo
([post #16](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/16)),
the controller's reply was always `86` plus 8 bytes, and the final reply was
always `87`. **[ME3]** adidooo could not find the transform and reported that the answer looked random. Their analysis
treated the module's answer (`07`) as a function of both A and B. It is T(B)
alone, and A is independent of B, so compared against A it does look random.
That fits their numbers: about 48 % of bits flipping per input bit, and no
linear structure. **[inferred]** (This analysis of adidooo's results is ours.)
Whether the ME3 uses the same mask and step
count is unconfirmed (section 9).

**Relevance:** the hub runs it before `02` (`handshake: true`, the default).
A controller that refuses `06` is identified without it. A wrong T(A) from the
controller is logged and the exchange carries on; a NAK of `07` sends the session
back to idle, and the next poll starts again with a new challenge. The LNK2
capture in WIRE.md starts mid-session, so it shows none of this. A controller
firmware update could change T(); see 1.5.

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
| `QZ >> 5` | The program plus 1: `1` is A, `2` B, `3` C, `0` a manual run **[code]** |
| `TT TT` | Run time in seconds, big-endian |

Example **[test]**: `BB 01 21 05 D0 22 04 B0 23 01 A4 24 04 B0 25 02 58 00…`
= zone 1 for 1488 s (running), zone 2 for 1200 s, zone 3 for 420 s, zone 4 for
1200 s, zone 5 for 600 s.

The top three bits come from the Rain Bird app's queue parser, as cited in
pyrainbird #670 (`((program + 1) & 7) << 5 | zone`). The capture above, all
`0x20`, is program A, which fits. A manual-run entry has not been captured.

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
**[inferred]** adidooo's ME3 teardown backs this up. It labels `C8 00` as OFF and
`C8 01` as AUTO, and read `C8 01` while a manual run was watering. **[ME3]**
Confirm on the TM2 by switching it to OFF.

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
overriding the current program. **[doc]** The one-byte field holds 255
minutes, but a manual run stops at **100** **[reported]**:

- On an ESP-RZXe with an LNK2, 100 minutes works and 101 fails (pyrainbird
  issue #618). Home Assistant core issue #105007 reports the same limit, with
  the controller not stated.
- The Rain Bird app's manual run picker stops at 1 h 40 min.
- pyrainbird now refuses a `39` over 6000 seconds for every mains controller,
  the three TM2 model IDs included (`max_manual_run_seconds` in
  `models.yaml`, #658).

Not yet tried on a TM2. The hub caps `39`, `4B` and `3A` at 100 minutes; a
101-minute `39` with `send_raw` would settle it, expecting NAK code `04`
(3.3).

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

Nothing in pyrainbird ever sends `00`, but the ME3's LNK2 does: it sends
`42 00` and gets `01 42`. **[ME3]** So `00` is probably "advance to the next
zone", and a zone number picks one. **[inferred]** What remains unknown is
whether a zone named is the one to jump to or the one to skip.

To find out, try these in order with a queue running, reading `3B 00` after
each:

1. `00`;
2. the next queued zone (from `3B 01`);
3. the currently running zone.

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
**[inferred]** adidooo's ME3 teardown lists the byte as minutes and got `01 3A`.
**[ME3]**

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
| 5 | frequency | `0` custom days, `1` cyclic, `2` odd days, `3` even days. pyrainbird's decoder adds `4` off and `5` odd days **[code]** |

Codes `4` and `5` come from pyrainbird #660, which reads them for every
program-based SIP controller, the TM2 included. It gives `2` as "odd, or odd
excluding the 31st", so one of the two odd codes may skip the 31st. Neither
has been seen from a TM2, and the TM2 front panel may not offer "off". The
hub shows both and writes them only with `experimental: true`.

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
10. ~~**Queue page 1 high bits**~~: answered from the app's parser (5.3). A
    capture of a queued manual run (top bits `0`) would confirm it.
11. **`4A`**: other event IDs, whether event 1 really changes when the
    schedule is edited at the front panel, and whether the timestamp is
    controller local time or UTC.
12. **Model ID** of the target unit (`02`): `0005` or an upgraded variant?
13. **Line polarity and timing** at the replacement interface's own GPIO pins.
14. **Seasonal adjust minimum** (`31`): does the controller accept values below
    10 %, and what does 0 % do?
15. **NAK code bits**: do the codes the TM2 actually returns match the
    documented bit meanings (3.3)? Record every NAK seen while probing. `04`
    for an out-of-range value has been seen on some controller (3.3).
16. **TM2 idle levels** (1.1, 1.4): with the LNK2 fitted and idle, measure J2
    and J4 against J3 with a meter. Does J4 sit at J3, as inferred, or at about
    3.2 V, as on the ME3?
17. **Receive LED current** (1.3): how much current does J2 deliver into an
    LED? Measure it with a meter in series with optocoupler 1's LED. It
    decides which optocoupler grade and load resistor give clean edges.
18. **Escape table** (2.4): capture a reply with an escaped `1B` or `1C`, such
    as `4C` at :27 or :28 seconds.
19. **Manual run limit** (5.4): does a TM2 refuse a 101-minute `39`?
20. **Frequency codes `4` and `5`** (pages `10`–`12`): can the TM2 set or
    report them, and which odd code skips the 31st?

---

## 9. ESP-ME3 notes (for future support)

This section collects what adidooo's ESP-ME3 teardown
([post #16](https://community.home-assistant.io/t/reverse-engineering-accessory-port-rain-bird-esp-rzx/998452/16)) found, as a starting point for ME3 support. Everything
here is **[ME3]** unless it is tagged otherwise. It comes from one person's
captures. We have not checked any of it.

### 9.1 What is shared with the TM2

- The connector and pinout (1.1).
- The LNK2 board and its interface circuit (1.3, 1.5).
- The SIP payloads (sections 3 and 5): the same opcodes, reply = request +
  `0x80`, ACK `01 <op>`, NAK `00 <op> …`.
- The handshake opcodes `06` / `86` / `07` / `87` (5.0).

What differs is the link layer (9.2) and how schedules are stored (9.5).

### 9.2 Link layer

- **UART**, 9600 baud, 8N1, LSB first, 3.3 V logic. Idles high. Not inverted
  at the LNK2's ESP32 pins.
- The module is the master. The controller answers 6–40 ms after each
  request, and the LNK2 sends a request about every 100 ms.
- The ME3 does not answer this component's Manchester link. adidooo
  fitted it in place of the LNK2, polled `02` and `3F`, and
  power-cycled the controller. There was no reply, and no edges at all.

Frame, in both directions:

```
1C <len> 00 <payload…> <crc> 1D
```

| Field | Size | Meaning |
|-------|------|---------|
| `1C` | 1 | Start of frame |
| `len` | 1 | Number of payload bytes. The `00` after it is not counted. |
| `00` | 1 | adidooo calls it a role byte. `00` in both directions; its meaning is unknown. |
| `payload` | `len` | SIP command or reply |
| `crc` | 1 | CRC-8 over `len 00 payload`: polynomial `0x07`, initial value `00`. No reflection and no final XOR **[inferred]**, which matches the captured `87` frame. |
| `1D` | 1 | End of frame |

Bytes between `1C` and `1D` are stuffed after the CRC is computed:

| On the wire | Means |
|-------------|-------|
| `1B 01` | `1C` |
| `1B 02` | `1B` |
| `1B 03` | `1D` |

Compared with the TM2: there is no `F5` preamble, the check is a CRC instead
of a sum, and the frame has the extra `00`. Because `1D` can't appear inside a
stuffed frame, a receiver can scan for it.

Worked examples, computed from the rules above **[inferred]**:

| Purpose | Frame |
|---------|-------|
| Stop irrigation | `1C 01 00 40 AC 1D` |
| Run station 3 for 1 minute | `1C 04 00 39 00 03 01 93 1D` |
| Run station 3 for 27 minutes (`1B` is stuffed) | `1C 04 00 39 00 03 1B 02 D5 1D` |
| Handshake complete: captured, and it checks | `1C 01 00 87 F7 1D` |

### 9.3 Handshake

The controller ignores the command set until the handshake completes, as on
the TM2 (5.0). The captures match 5.0's shape: the controller's reply is
always `86` plus 8 bytes, and the final reply is always `87`.

- **First test for ME3 support:** run biggestchungus's T() from 5.0, mask
  `80718611` and 13 steps, against an ME3.
- **If it fails:** T() is linear (5.0). Every captured handshake gives two
  input/output pairs, (A, T(A)) and (B, T(B)), and 32 independent pairs
  determine a linear map. **[inferred]**
- **Unexplained:** adidooo saw op `06` in only 54 % of the module's first
  frames, with about 60 other opcodes in the rest. It could be a capture or
  un-stuffing problem.

### 9.4 Commands

These are the same as on the TM2 unless listed.

| Op | ME3 | Compared with the TM2 |
|----|-----|-----------------------|
| `02` | `82 00 09 02 0C`: model `0009`, version 2.12 | TM2 model IDs are in 4.1 |
| `03` | `03 00` → `83 00 FF 1F 00 00`: 13 stations | Same layout |
| `0B` | `8B 03 0B 00 01 B1 00 19 00`: 8 data bytes, layout unknown | The TM2 sends 4 (5.1) |
| `30` | Programs 0–3 | The TM2 has 3 programs |
| `3A` | `3A <minutes>` → `01 3A` | Settles the minutes question for the ME3 (5.4) |
| `3B` | Pages 0–3. Page 0 is the running state and pages 1–3 the queue. The LNK2 reads 2 and 3 only while watering. | WIRE.md shows only page 0 polled |
| `3D` | Polled with pages 0–2. Unknown here too. | Same |
| `42` | `42 00` → `01 42` | 5.4 |
| `48` | `C8 00` = OFF, `C8 01` = AUTO | Supports 5.3's reading |
| `4B` | `4B <station u16> <minutes>` | 5.4 reads it as `4B PP ZZ MM`. The bytes are the same for stations up to 255. |
| `4C` | Not polled | The TM2's main status poll |
| `62` | Polled; reply not described | Not seen on the TM2 |
| `63` | `63` → `E3 <installed 0/1> 00`: flow sensor fitted | The TM2 has no flow sensor input (section 6) |
| `64` | `64 <installed 0/1> 00` → `01 64` | Same |
| `0C` | Object protocol (9.5) | Not used on the TM2 (section 6) |
| `20` / `21` | Not seen in the captures. Schedules go through `0C`. | The TM2's schedule pages (5.6) |

The LNK2's poll runs about every 13 s, one request at a time:

```
12        date
10        time
0B        firmware version
03 00     available stations
30 00–03  water budget, programs A–D
3B 00     running state
3B 01     queue
62        unknown
63        flow sensor
3F 00     active stations 1–32
3F 01     active stations 33–64
3D 00–02  station errors (?)
36        rain delay
3E        rain sensor
4A 01     event timestamp
48        irrigation state
0C        object protocol
```

On top of that:

- `3B 02` and `3B 03` while watering.
- Object `0D` (rain sensor active) every 15–20 s.
- The event log about every 2 minutes.
- The whole configuration at startup and after any "module" write (9.5).

### 9.5 Object protocol (`0C` / `8C`)

Programs and schedules are read and written as numbered objects, carried in a
`0C` request and an `8C` reply. pyrainbird calls `0C` UniversalMessageRequest
(section 6).

**Routing.** adidooo's figure of the full envelope was not readable, so
where these fields sit in the frame is not known.

| Destination | Source | Use | Read (request / reply) | Write (request / reply) |
|-------------|--------|-----|------------------------|-------------------------|
| `08 05 00 00` | `0C 00 00 00` | Configuration | `07` / `08` | `05` / `06` |
| `01 01 00 00` | `0C 00 00 00` | "Module" | `03` / `04` | `01` / `02` |
| `01 01 00 00` | `0E 00 00 00` | Event log | | |

**Body, configuration routing:**

```
read:   07 0B <n> <descr> × n                       → 08 0B <n> (<descr> <type> <values>) × n
write:  05 0B <n> (<descr> <type> <values>) × n     → 06 0B 00
```

- `descr`: the object number (u16), a dimension count (u8), then one
  (start u16, end u16) pair per dimension.
- `type`: `01` = u8, `02` = u16, `04` = u32.
- Everything in the body is **little-endian**, unlike the big-endian SIP
  fields.

**Objects:**

| Object | Dimensions | Meaning |
|--------|------------|---------|
| `0A` | zone 0–21 | Cycle minutes (cycle and soak) |
| `0B` | zone 0–21 | Soak minutes |
| `0D` | none | Rain sensor active (1) or bypassed (0) |
| `10` | program 0–3 × day 0–6 | Watering days in custom mode, Sunday first |
| `11` | program 0–3 | Days left before the next cyclic watering |
| `12` | program 0–3 | Cyclic interval in days |
| `13` | program 0–3 | Mode: 0 custom, 2 odd, 3 even, 4 cyclic |
| `14` | none | Rain delay in days (the same value as `36`) |
| `15` | program × zone 0–21 | Run time in seconds (u32) |
| `18` | program 0–3 | Seasonal adjust % (u16, the same value as `30`) |
| `1D` | program × `[0]` × start 0–5 | Start times in minutes from midnight; 1440 = none |

From the dimensions: 4 programs, up to 22 zones and 6 start times per program.

**Not stored on the controller:** zone names, the "wired" flag per zone, soil
type, automatic seasonal adjust, the calendar, notifications and WiFi. The
app, the LNK2 or the cloud keeps these.

### 9.6 Hardware

The ME3 uses the same connector and the same optocoupler arrangement, so a TM2
interface board should work electrically **[inferred]**. Two things change:

- **Transport:** ESPHome's `uart` at 9600 8N1 on the same two pins, in place
  of `remote_transmitter` and `remote_receiver`.
- **Polarity:** the UART needs both ESP32 pins idle-high, with no inversion end
  to end. A receive stage with a pull-up (1.3) inverts, so set `inverted: true`
  on that pin, or change the stage to an emitter follower with a pull-down.

adidooo's own build used:

- two PC817C optocouplers (any current transfer ratio of about 100–300 %);
- 220 Ω on the transmit LED, about 9.5 mA;
- a 1 kΩ pull-down on the receive output (2.2 kΩ if the idle level sits below
  about 2.5 V);
- `uart` at 9600 8N1, with no inversion.

### 9.7 What ME3 support would take here [inferred]

- **A UART transport** beside the Manchester one. Today the Manchester side is
  tied in at three points:
  - `sip_frame` holds the F5 framing and the additive checksum;
  - `LinkCore` includes it and times replies with Manchester airtime
    (`frame_air_ms`);
  - `RainbirdComponent` talks to `remote_transmitter` and `remote_receiver`.
- **An ME3 model** beside `model_esp_tm2`. `Session` includes the TM2 model
  directly today. The ME3 model would have 4 programs and use the object
  protocol in place of pages `20` / `21`.
- **The handshake**, once T() is confirmed for the ME3 (9.3).

### 9.8 Open questions for the ME3

1. Does the ME3 use the same T()?
2. What is the `00` byte after `len`?
3. What is the layout of the `0B` reply?
4. What does `62` read?
5. Where do the routing fields sit in a `0C` envelope?
6. Why was only 54 % of the module's first frames op `06`?

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
