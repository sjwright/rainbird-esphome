# Plan: our own GPIO receiver, and what to do about the transmitter

Status: **proposal for discussion. Nothing here is built.**
Written 25 October 2026, against ESPHome 2026.9.1 and ESP-IDF 5.5.4.
Updated 26 October 2026: single-core chips (8.1).

This plan replaces `remote_receiver` with a small receiver of our own, built on
a GPIO edge interrupt. It weighs four ways to transmit and recommends keeping
`remote_transmitter` for now. Every caveat known today is in section 8. The
open questions for discussion are in section 11.

---

## 1. Summary

| | Today | Proposed |
|---|---|---|
| Receiving | `remote_receiver` (RMT peripheral) | Our GPIO edge interrupt, then the existing decoder |
| Transmitting | `remote_transmitter` (RMT) | Unchanged in phase 1. Optionally our own RMT channel later (section 5) |
| User YAML | `remote_receiver` and `remote_transmitter` blocks with exact settings | `receiver_pin:` and optional `monitor_pin:` on the hub, plus the transmitter |
| Longest reply | 42 bytes on the original ESP32 (the RMT memory) | Set by a RAM buffer we size, on every chip |
| Chips | ESP32, S3, C3, C5, C6, H2, P4 | The same, plus S2 (only transmitting needs RMT, and the S2's suffices). Listen-only on any ESP32 |

**What gets better.** Most of `wire.py` goes. The RTC pad workaround goes. The
reply-length limit goes, which is what blocks ESP-ME3 schedule reads on the
original ESP32. Users configure two pins rather than copying magic numbers.

**What we take on.** About 150 lines of C++ that touch hardware: an interrupt
handler, a ring buffer and the loop integration. The burst assembly behind it
is pure code and is host-tested. Timing precision becomes ours to prove. RMT
timestamps in hardware; an interrupt adds latency and jitter. The tolerances
say this is comfortable (section 8.1), but that needs a scope, not a belief.
The single-core chips (C3, C5, C6, H2, and the S2 this plan adds) are the hard
case: our interrupt shares the one core with WiFi and everything else.

**Recommendation.** Do the receiver. Keep `remote_transmitter`. Decide on our
own transmitter channel after the receiver has been proven on hardware.

---

## 2. Background

### 2.1 The line

From SPEC.md section 1:

- Manchester at 2400 bit/s. A half-bit is 208.3 µs, so only two pulse widths
  ever appear: ~208 µs and ~417 µs.
- High-then-low is 1, MSB first. The line idles low between frames.
- Replies arrive inverted relative to requests, and start with an extra
  half-bit. The decoder locks polarity and phase after three good frames
  (`PolarityLock`). None of that changes in this plan.
- The decoder (`manchester_decode_bits`) takes **signed pulse durations in
  microseconds**: positive is high, negative is low. That is the format
  `remote_receiver` delivers, so it is the seam this plan cuts at. Everything
  from the decoder up stays as it is.

Pulse classification windows (`manchester.h`):

| Pulse | Nominal | Accepted | Margin |
|---|---|---|---|
| One half | 208.3 µs | 145–270 µs | −63 / +62 µs |
| Two halves | 416.7 µs | 291–540 µs | −126 / +123 µs |

271–290 µs is in neither window and ends the decode attempt.

### 2.2 How we use `remote_*` today

| Place | What it does |
|---|---|
| `__init__.py` | `DEPENDENCIES = ["remote_receiver"]` |
| `hub_schema.py` | `transmitter_id`, `receiver_id`, `monitor_receiver_id`; `_apply_wire_ids` picks defaults |
| `codegen.py` | `_add_listener` registers a `RainbirdReceiver` on each receiver; `_receiver_gpio` digs the pin out of the receiver's config for the pad workaround |
| `rainbird.h/.cpp` | `RainbirdReceiver` (a `RemoteReceiverListener`) with its own `ReceiverDecoder`; `transmit_()` encodes and calls `remote_transmitter`; `release_receiver_pads_()` |
| `wire.py` | 433 lines: per-variant RMT block budgets, `receive_symbols`, `rmt_symbols`, `idle`, `filter`, `carrier_duty_percent`, `non_blocking`, other RMT users (LED strips), unsupported variants |
| `manchester.cpp` | `trailing_idle()` and the "idle swallowed the last half" rule, both shaped by how `remote_receiver` ends a burst |
| `tools/raw_to_fixture.py` | Turns `remote_receiver`'s `dump: raw` log lines into replay fixtures |
| Examples, test YAML, 12 tests in `test_config.py` | Configure and check the above |

### 2.3 Problems `remote_receiver` causes, with evidence

1. **Reply length.** The original ESP32 cannot refill RMT memory while
   receiving, so a whole reply must fit in hardware memory: 512 symbols across
   8 blocks, one of which the transmitter needs. Worst case is one symbol per
   bit (36 bytes of `FF` gave 651 timings, about 326 symbols:
   `examples/sniffer.md`). We cap replies at 42 bytes. An ESP-ME3 schedule
   reply is about 126 bytes.
2. **RTC pads.** `remote_receiver` hands the GPIO number to the RMT driver and
   never runs the pin's setup, so an RTC-capable pad on the original ESP32
   stays on the RTC mux and the receiver sees a flat line. Found on the bench
   on 21 October (`examples/sniffer.md`). The hub now calls `rtc_gpio_deinit`
   after `remote_receiver` sets up.
3. **Configuration burden.** Users must set `idle: 4ms`, `filter: 50us`,
   `receive_symbols: 384` and, on the original ESP32, `rmt_symbols: 384`. Get
   one wrong and replies are silently lost, which is why `wire.py` exists.
4. **RMT budgeting.** RMT memory is shared with LED strips and IR. `wire.py`
   rounds every user up to whole blocks per variant (`_check_rmt_memory`). Two
   receivers (controller and monitor) do not fit an original ESP32
   (`examples/sniffer.yaml` tells people to use an S3).
5. **Variant exclusions.** The S2 is refused because 256 symbols cannot hold a
   reply.
6. **Burst-end artefacts.** `remote_receiver` ends a burst on an idle timeout,
   merges the last half-bit into the idle pulse and appends a synthetic
   opposite-level sample. `manchester.cpp` reconstructs the last bit from
   that, and `raw_to_fixture.py` has to put the sample back because
   `dump: raw` omits it.
7. **Upstream churn.** The RMT driver was rewritten for ESP-IDF 5, and
   `remote_transmitter`'s `non_blocking` default flipped in ESPHome 2025.11.

---

## 3. Goals and non-goals

**Goals**

- Receive frames of any length up to `MAX_PAYLOAD` on every supported chip.
- Configure the receiver with a pin and nothing else.
- Keep the decoder, polarity lock, framing, link and session unchanged.
- Keep everything after the interrupt host-testable, including a replay of
  the real Saleae capture.
- Delete more code than we add, counting `wire.py` and its tests.

**Non-goals**

- ESP8266, RP2040 or LibreTiny. The receiver would port, but the transmitter
  and the rest of the component are not being tested there.
- Changing the wire protocol, the polarity rules or the link timing.
- Replacing the transmitter in phase 1.

---

## 4. Receiver design

### 4.1 Data flow

```
 GPIO edge
   │  (interrupt, IRAM)
   ▼
 EdgeStore ring buffer ── timestamp|level per edge, single producer
   │  (loop, drains)
   ▼
 BurstAssembler ── pure: glitch filter, idle split, signed durations   ◄─ host tests
   │  one burst
   ▼
 ReceiverDecoder (existing) ── polarity lock, Manchester, frames         ◄─ host tests
   │  frames
   ▼
 RainbirdComponent::deliver_frame_() (existing)
```

The interrupt does as little as possible: read the level, read the clock,
write one word, wake the loop. Filtering and burst splitting happen in the
loop, in a pure class, so they can be unit-tested and replayed. This is a
deliberate difference from ESPHome's own interrupt receiver (section 4.10),
which filters inside the interrupt.

### 4.2 The interrupt handler

Sketch, not final code:

```cpp
struct EdgeStore {
  ISRInternalGPIOPin pin;
  uint32_t *edges;              // timestamp in µs with the level in bit 0
  uint32_t mask;                // capacity - 1, a power of two
  std::atomic<uint32_t> head{0};  // written only by the interrupt
  std::atomic<uint32_t> tail{0};  // written only by the loop
  std::atomic<bool> overflow{false};
  Component *wake;              // the hub
};

static void IRAM_ATTR edge_isr(EdgeStore *s) {
  uint32_t level = s->pin.digital_read() ? 1u : 0u;  // level after the edge
  uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  uint32_t head = s->head.load(std::memory_order_relaxed);
  if (head - s->tail.load(std::memory_order_acquire) > s->mask) {
    s->overflow.store(true, std::memory_order_relaxed);
    return;
  }
  s->edges[head & s->mask] = (now & ~1u) | level;
  s->head.store(head + 1, std::memory_order_release);
  s->wake->enable_loop_soon_any_context();
}
```

Points to settle in review:

- **Level after the edge, read in the interrupt.** If two edges arrive within
  one interrupt latency, one is lost. Reading the level, rather than toggling
  a remembered one, keeps the parity right after a miss. The lost pulse then
  shows as a merged pulse, which the decoder rejects as out of window. The
  frame is lost and the link retries it. That is the same outcome as a
  corrupted RMT capture.
- **2 µs resolution.** Packing the level into bit 0 loses the clock's lowest
  bit. A half-bit is 208 µs, so this costs nothing.
- **Clock.** `esp_timer_get_time()` is IRAM-resident when
  `CONFIG_ESP_TIMER_IN_IRAM` is set (`esp_timer_impl.h`). ESPHome's own
  `millis()` relies on this from interrupt handlers (`esp32/hal.cpp`). Check
  that ESPHome's generated sdkconfig keeps it on.
- **`enable_loop_soon_any_context()`** exists in ESPHome 2026.9 and is documented
  as interrupt-safe. It sets a flag; it does not allocate or log.
- **Atomics.** On the dual-core original ESP32 and S3, the interrupt and the
  loop may run on different cores. `std::atomic` with acquire/release on the
  indices gives the ordering. On single-core chips the interrupt preempts the
  loop instead, and the same atomics are still correct. ESPHome's own receiver
  uses `volatile`, which does not formally order the buffer write against the
  index write.

### 4.3 Attaching the interrupt

Use ESPHome's pin type, not raw ESP-IDF:

```cpp
this->rx_pin_->setup();   // gpio_config: input, pulls, and rtc_gpio_deinit on RTC pads
this->store_.pin = this->rx_pin_->to_isr();
this->rx_pin_->attach_interrupt(edge_isr, &this->store_, gpio::INTERRUPT_ANY_EDGE);
```

- `pin->setup()` calls ESP-IDF's `gpio_config()`, which calls
  `rtc_gpio_deinit()` for RTC-capable pads (`esp_driver_gpio/src/gpio.c`,
  around line 373). That is exactly what `release_receiver_pads_()` does by
  hand today, so the workaround can go. Prove it on the same G26 → G27
  loopback that found the problem.
- `inverted:` on the pin schema works as usual. `digital_read()` on the
  interrupt pin honours it.
- ESPHome installs the shared GPIO interrupt service on first use with
  `ESP_INTR_FLAG_LEVEL3` and **without** `ESP_INTR_FLAG_IRAM`
  (`esp32/gpio.cpp`, around line 93). See concern 8.2.

### 4.4 Burst assembly (pure)

```cpp
class BurstAssembler {
 public:
  // Feed edges in order. Returns true when a burst has closed.
  void add_edge(uint32_t time_us, bool level_after);
  // The loop calls this with the current time; an idle gap closes the burst.
  bool check_idle(uint32_t now_us);
  // Signed durations, positive high, in the format manchester_decode_bits takes.
  const std::vector<int32_t> &burst() const;
  void clear();
};
```

Rules:

- **Idle split.** No edge for `IDLE_US` (4 ms, as today's `idle: 4ms`) closes
  the burst. 4 ms is about 10 bit times, far below the controller's ~30 ms
  turnaround, and far above the longest legal pulse (417 µs).
- **Glitch filter.** A pulse shorter than `FILTER_US` (50 µs, as today) is
  merged into its neighbours, and the levels either side are joined. This is
  ESPHome's rule, moved out of the interrupt.
- **Unsigned time.** Durations use unsigned subtraction, so the 71-minute
  wrap of a 32-bit µs clock is harmless. Idle gaps are clamped.
- **The last half-bit.** The line idles low. A frame ending in a 1 bit
  (high-then-low, and `FRAME_END` `1D` ends in 1) has its final low half
  merged into idle, so it has no edge at its end. This is a property of the
  line, not of `remote_receiver`, so the decoder still has to infer the last
  half. The assembler ends each burst with one synthetic idle-level sample,
  exactly as `remote_receiver` does, so `manchester_decode_bits` keeps
  working unchanged. Simplifying `trailing_idle()` is a separate later step,
  once the replay tests cover it.

### 4.5 Loop integration

The hub disables its loop when the link is idle (`after_activity_()`). A
reply, or any frame in listen-only mode, has to wake it:

1. The interrupt calls `enable_loop_soon_any_context()` on the hub.
2. `loop()` drains the ring buffer into the `BurstAssembler`.
3. While a burst is open, the hub holds a `HighFrequencyLoopRequester`, so
   the idle check runs every millisecond or so rather than every 16 ms.
   `remote_receiver` holds one permanently; we would hold it only while a
   burst is open.
4. When the burst closes, decode it, deliver the frames, release the
   requester.
5. `after_activity_()` keeps the loop enabled while the receiver has edges
   pending or a burst open.

Without step 3, the end of every reply is noticed up to 16 ms late. That is
harmless to correctness, but it would add to `response_ms` and to the link's
hold timing.

### 4.6 Monitor receiver

The monitor (J4, the requests another master sends) becomes a second
`EdgeStore`, `BurstAssembler` and `ReceiverDecoder` on its own pin. Its
interrupt is a second handler on the same GPIO interrupt service. It no
longer needs an RMT channel, so an original ESP32 can run both receivers.

### 4.7 Buffer sizing and memory

| Frame | Bits | Worst-case edges | Buffer (4 bytes per edge) |
|---|---|---|---|
| TM2 reply, 42 bytes | 336 | 672 | 2.7 KB |
| `MAX_PAYLOAD` 64 + 7 | 568 | 1136 | 4.5 KB |
| 255 + 7 (ESP-ME3 `0C` replies) | 2096 | 4192 | 16.4 KB |

- Size the ring to a power of two from `MAX_PAYLOAD` at compile time: 2048
  edges, 8 KB, today. With a second receiver, double it.
- The assembler's burst vector reserves the same worst case once at setup,
  so nothing allocates per frame.
- If `MAX_PAYLOAD` rises to 255 for the ME3, this is 16 KB per receiver. That
  is affordable on every supported chip but worth saying out loud. An
  alternative is to decode incrementally and keep only the current frame.

### 4.8 Diagnostics

`remote_receiver`'s `dump: raw` is how protocol captures are made today
(`examples/sniffer.yaml`, `tools/raw_to_fixture.py`). Options, in order of
preference:

1. Users who need raw dumps add a stand-alone `remote_receiver` with
   `dump: raw` **on the same pin**, alongside ours. The ESP32 GPIO matrix can
   route one pad's input to both the RMT and the GPIO interrupt, so this
   should coexist. To be proved on the bench before we rely on it.
2. Our own raw log line at `VERY_VERBOSE`, in the exact `Received Raw:` format,
   so `raw_to_fixture.py` keeps working.
3. Counters in the existing 10-minute link-stats log line: edges, bursts,
   glitches filtered, overflows, bursts that decoded to no frame. These are
   wanted whatever else is chosen, because they are how the soak tests in
   section 9 are judged.

### 4.9 Code removed and added (receiver only)

| Removed | Added |
|---|---|
| `RainbirdReceiver` and its listener registration | `EdgeStore` and the interrupt handler (~40 lines) |
| `release_receiver_pads_()`, `receiver_pins_`, `add_receiver_pin` | `BurstAssembler` (~60 lines, pure) |
| `codegen._add_listener`, `codegen._receiver_gpio` | Loop drain, wake-up and requester handling (~40 lines) |
| `receiver_id`, `monitor_receiver_id`, `_apply_wire_ids` receiver part | `receiver_pin`, `monitor_pin` schema and codegen (~20 lines) |
| `wire._check_receiver`, receive-side `_rmt_users` and block budgeting | Migration errors for the old keys (~10 lines) |
| ~10 receiver tests in `test_config.py` | Assembler unit tests, Saleae replay, jitter fuzz, config tests |

### 4.10 Reference: ESPHome's own interrupt receiver

`remote_receiver` already has an interrupt path, used on ESP8266, LibreTiny,
RP2040 and ESP32 variants without RMT (`remote_receiver.cpp`, guarded by
`!SOC_RMT_SUPPORTED`). We cannot select it on an RMT chip, because the choice
is made at compile time, but it is a proven design to borrow from:

- It filters glitches in the interrupt by deferring each pulse's commit
  until the next edge proves it was not a glitch.
- It splits bursts on idle in the interrupt, and closes a trailing burst from
  `loop()`.
- It holds a `HighFrequencyLoopRequester` permanently.
- Its buffer indices are `volatile`, not atomic.

We would keep its shape but move filtering and splitting out of the interrupt
into the pure assembler, for testability.

---

## 5. Transmitter options

Transmitting does not have the receiver's problems. RMT refills while
transmitting on every RMT chip, so frame length is not limited, and RMT
timing is exact. The case for changing it is tidiness and precise
"transmission finished" timestamps, not correctness.

### T1. Keep `remote_transmitter` (recommended for now)

- No new code. Hardware-exact timing.
- Users still write a `remote_transmitter` block with
  `carrier_duty_percent: 100%` and `non_blocking: true`. `wire.py` keeps
  `_check_transmitter` for those two settings.
- RMT budgeting shrinks to one transmitter channel. Propose deleting
  `_check_rmt_memory` entirely: ESP-IDF fails loudly at boot if the channel
  cannot be allocated, and a lone transmitter always fits. This is up for
  discussion (question 4).

### T2. Our own RMT transmitter channel through ESP-IDF

- `rmt_new_tx_channel`, a copy encoder, `rmt_transmit` without blocking, idle
  level low at the end of transmission, no carrier. Roughly 60–80 lines.
- The hub takes `transmitter_pin:` and the user writes nothing else.
- `on_trans_done` gives the exact end of the transmission, which would make
  the link's timeout base and `response_ms` exact rather than "when `loop()`
  sent it".
- Costs: we own an ESP-IDF API that changed in 5.0 and may change again, and
  the RMT channel allocation and its error paths. It works under the Arduino
  framework too, because ESPHome's Arduino builds sit on ESP-IDF 5, and
  `remote_transmitter` already uses the same driver under both.
- Does not help chips without RMT (C2, C61, H4, H21).

### T3. GPIO from a hardware timer interrupt

- A `gptimer` alarm every 208.3 µs sets the next half-bit level. About 4,800
  interrupts a second for the ~60 ms of a two-copy request.
- Works on every chip, including those without RMT.
- **Risk: our jitter goes onto the wire, and we do not know the controller's
  tolerance.** On our receiving side we know the windows exactly; on the
  controller's we know nothing. A flash write mid-frame (concern 8.2) would
  stall the timer interrupt and stretch a pulse, corrupting the frame. The
  checksum protects the controller from acting on it, but a command frame
  sent once (`Delivery::ONCE`) would simply be lost.
- Not recommended unless chips without RMT become a goal.

### T4. Stream half-bits through SPI or I2S

- Clock the half-bit pattern out of a serial peripheral by DMA, one SPI bit
  per half-bit at 4,800 Hz. The ESP32's SPI clock divider reaches that
  (the minimum is a few hundred Hz), but check each variant's divider and
  the driver's minimum before relying on it.
- Exact timing, no interrupts. But it uses a whole SPI host and its pins for
  a 2400 bit/s line, and it is the most unusual code of the four.
- Rejected for complexity.

**Recommendation: T1 now. Revisit T2 after the receiver ships,** judged on
whether exact transmit timestamps or the simpler YAML are worth owning an RMT
channel.

---

## 6. Configuration changes

### Before

```yaml
remote_transmitter:
  pin: GPIO26
  carrier_duty_percent: 100%
  non_blocking: true

remote_receiver:
  pin: GPIO27
  idle: 4ms
  filter: 50us
  receive_symbols: 384
  rmt_symbols: 384      # original ESP32 only

rainbird:
  model: ESP-TM2
```

### After (T1)

```yaml
remote_transmitter:
  pin: GPIO26
  carrier_duty_percent: 100%
  non_blocking: true

rainbird:
  model: ESP-TM2
  receiver_pin: GPIO27
  # monitor_pin: GPIO25   # optional, logs another master's requests
```

- `receiver_pin` and `monitor_pin` use ESPHome's internal GPIO input pin
  schema, so `inverted:`, `mode:` and pull-ups work as on any pin.
- `idle` and `filter` are protocol constants, not settings. Proposal: no YAML
  knobs (question 2).
- `listen_only: true` needs no transmitter at all, so a listen-only hub needs
  no RMT and runs on any ESP32, including the C2 and C61.

### Migration

Nothing is released, so this is a clean break with clear errors:

- `receiver_id:` → "receiver_id is gone: set `receiver_pin:` to the GPIO and
  remove the remote_receiver block."
- `monitor_receiver_id:` → the same message for `monitor_pin:`.
- `DEPENDENCIES` drops `remote_receiver`. `remote_base` stays auto-loaded for
  the transmitter type.

---

## 7. Changes by file

| File | Change |
|---|---|
| `components/rainbird/rx_gpio.h/.cpp` (new) | `EdgeStore`, the interrupt handler, setup |
| `components/rainbird/burst.h/.cpp` (new) | `BurstAssembler`, pure |
| `rainbird.h/.cpp` | Drop `RainbirdReceiver`, `release_receiver_pads_`, receiver pointers. Add pins, two receivers, loop drain, wake-up, requester |
| `hub_schema.py` | `receiver_pin`, `monitor_pin`, migration errors; drop receiver ids |
| `codegen.py` | `cg.gpio_pin_expression` for the pins; drop `_add_listener`, `_receiver_gpio` |
| `wire.py` | Keep the transmitter checks and a narrowed variant check. Delete everything receiver-related, probably the RMT budget too |
| `__init__.py` | Drop the `remote_receiver` dependency |
| `manchester.cpp` | No change in phase 1. `trailing_idle()` may simplify later |
| `tests/unit/test_burst.cpp` (new) | Assembler rules, Saleae replay, jitter and glitch fuzz |
| `tests/unit/Makefile` | Add the new pure sources |
| `tests/python/test_config.py` | Remove receiver RMT tests; add pin and migration tests |
| `examples/*.yaml`, `examples/sniffer.md`, test YAML | New keys; sniffer section rewritten |
| `tools/raw_to_fixture.py` | Unchanged if option 4.8.2 keeps the log format; otherwise a variant for our format |
| `SPEC.md`, `PLAN.md` | Update the physical-layer and receiver sections |

---

## 8. Caveats and concerns

Each item says what could go wrong, how likely it looks, how to measure it,
and what we would do about it.

### 8.1 Timing jitter: the main technical risk

**What.** RMT stamps edges in hardware. Our timestamp is taken when the
interrupt runs, after a latency that varies. A pulse's measured width is
`(true width) + (latency at its end) − (latency at its start)`, so what
matters is the **spread** of latency, not its average.

**Budget.** A one-half pulse tolerates −63 / +62 µs. Edges also suffer the
line's own distortion, mainly the optocoupler's unequal turn-on and turn-off
times, which lengthen one polarity and shorten the other by the same amount.
RMT sees that distortion too. So, roughly:

```
optocoupler asymmetry + interrupt latency spread  <  ~60 µs
```

**Expectation.** ESP32 GPIO interrupt latency is typically a few µs with a
small spread. The tail comes from code that runs with interrupts masked:
WiFi and especially Bluetooth controller critical sections on the original
ESP32, cache-disabled flash windows (8.2), and other level-3-or-higher
interrupts. Tens of µs is plausible in the tail; hundreds is not expected.

**Single-core chips are the worst case.** The C3, C5, C6 and H2 (and the S2)
have one application core. On the original ESP32, S3 and P4, the hub's
interrupt can sit on the core WiFi does not use, so WiFi's interrupt-masked
sections do not delay it. On a single-core chip they all run on the same core:
the WiFi and BLE drivers, their interrupts, FreeRTOS critical sections and the
loop. Every masked stretch on the chip lands in our latency tail. The cores
are also slower (160 MHz on the C3 and C6, 96 MHz on the H2, against 240 MHz),
so each masked stretch lasts longer. RMT hides all of this today: it
timestamps edges in hardware, and on the C3 only its refill interrupt depends
on latency.

So the dual-core results do not carry over. The C3 is the chip to measure
first and the one the acceptance criteria (9.4) are judged on. It is also the
most common single-core board. The first move in "If it fails" below, moving
to the other core, does not exist on these chips.

**Measure.**

- Host: a jitter fuzz test (section 9.1) finds how much random per-edge
  jitter the decoder survives on real captured edges. That turns "should be
  fine" into a number.
- Bench: toggle a spare GPIO at the top of the interrupt handler, capture it
  with the logic analyser alongside the input line, and plot the latency
  histogram. Under load: WiFi traffic at DEBUG logging, BLE tracker on, and
  API clients connected. Do it on a C3 and on the original ESP32, C3 first.

**If it fails.** In order: on dual-core chips, move the hub's setup (and so
the interrupt service installation) to the core without WiFi; drop BLE (on
the original ESP32, and on any single-core chip); widen nothing (the gap between the windows is fixed by the protocol);
consider a time-slot decoder (section 10, phase 4), which tolerates far more
per-edge jitter; fall back to `remote_receiver`.

### 8.2 Flash writes mask our interrupt

**What.** ESPHome installs the GPIO interrupt service without
`ESP_INTR_FLAG_IRAM`. While the flash cache is disabled (any flash write or
erase), such interrupts are held off. Edges in that window are lost. The
interrupt runs once when the window ends, with the wrong timestamp.

**When.** NVS writes when preferences are saved (ESPHome batches these, and
our run-duration numbers with `restore_value` cause them), WiFi driver NVS
writes, and OTA. A sector erase can take tens of ms, longer than a whole
reply.

**Effect.** A reply overlapping a flash write fails its checksum or decodes
to nothing. Polls are retried. A command reply (`Delivery::ONCE`) is reported
as failed, exactly as a lost reply is today: "the controller may have acted".

**Can we avoid it?** Not cleanly.

- Installing the GPIO service ourselves with the IRAM flag before ESPHome
  does would make ESPHome's own `attach_interrupt` fail for every other
  component (it treats `ESP_ERR_INVALID_STATE` as an error and returns
  without adding the handler).
- A dedicated `esp_intr_alloc` for the GPIO source conflicts with the shared
  service.
- RMT is not immune either. On chips that refill (C3, S3), the refill
  interrupt runs in a cache-disabled window only if the RMT driver was built
  IRAM-safe (`CONFIG_RMT_ISR_IRAM_SAFE`), which has not been checked. Only
  the original ESP32's whole-frame capture is immune.

**Measure.** A soak test that writes preferences in a loop while polling,
counting lost frames (section 9.2).

**Accept** if the loss rate is in line with ordinary line noise, and say so
in the docs.

### 8.3 Shared GPIO interrupt service

All GPIO interrupts on a core go through one dispatcher, which checks each
registered pin. Other components with pin interrupts (pulse counters, buttons
with `on_press` interrupts, rotary encoders) add their handlers' run time to
our latency. Note it in the docs. Measure with the latency histogram if a
user reports trouble.

### 8.4 Optocoupler edges and glitches

RMT has a hardware glitch filter ahead of capture. We filter in software,
after the interrupt. A slow optocoupler edge crossing the input threshold
without hysteresis can produce a burst of very short pulses: each costs an
interrupt and a buffer slot, then the filter merges them. Harmless in
principle. But it makes the analogue edge quality matter more than it does
with RMT.

- Scope the real optocoupler output: rise and fall times, ringing.
- On chips that support it (S3, C3, C6 and later), consider enabling input
  hysteresis on the pin.
- Choose the optocoupler load resistor for clean edges. That belongs in the
  hardware notes anyway.

### 8.5 Loop scheduling changes response timing

Today `response_ms` is measured when `remote_receiver`'s loop hands over a
finished burst: the end of the reply, plus the 4 ms idle, plus loop latency.
With our receiver and a high-frequency requester it is about the same. If the
requester is not held, it becomes up to 16 ms worse.

We could do better: the interrupt knows the reply's first and last edge
exactly. Passing those times into the link would make `response_ms` the true
turnaround, and the duplicate-reply hold exact. That changes `LinkCore`'s
inputs, so it is a separate step (question 6), not part of phase 1.

### 8.6 Memory

8 KB per receiver at today's `MAX_PAYLOAD`, 16 KB at 255 (section 4.7). Fine
on every supported chip; more than the RMT path, which uses the RMT
peripheral's own memory plus a smaller RAM buffer.

### 8.7 Losing `remote_receiver`'s tooling

`dump: raw`, `raw_to_fixture.py` and the sniffer recipe all assume
`remote_receiver`. Section 4.8 lists the replacements. The same-pin
coexistence option must be proven before the sniffer docs promise it.

### 8.8 What we now own

- An interrupt handler and a lock-free ring buffer: small, but the kind of
  code where a mistake shows as rare, unreproducible frame loss.
- ESPHome's GPIO interrupt API. It has been stable for years, but it is now
  a dependency of ours rather than of `remote_receiver`.
- The interrupt is the one part host tests cannot reach. Keep it to the
  sketch in 4.2 and put every decision in the assembler.

### 8.9 What does not get better

- The polarity lock, the extra half-bit and the inverted replies are physics.
  They stay.
- The transmitter keeps its RMT channel and its YAML block (T1).
- The idle tail: the line idles low, so the last half-bit of a frame ending
  in a 1 still has no closing edge. The decoder still infers it.

### 8.10 Scope creep

With a GPIO receiver, ESP8266 and RP2040 support looks close. It is not: the
transmitter, `wire.py` variant rules, memory budgets and every test assume
the ESP32. Keep it out of scope unless someone asks for it.

---

## 9. Test and validation plan

### 9.1 Host tests (no hardware)

1. **Assembler rules.** Idle split, glitch merge, levels joined across a
   merged glitch, the synthetic idle sample at the end, unsigned clock wrap,
   overflow behaviour.
2. **Round trip.** `manchester_encode` → edges → assembler → decoder → the
   original frame, for every SPEC vector and both polarities and phases.
3. **Real edges.** `tests/captures/saleae/capture.csv` is a Saleae Logic
   capture of a real LNK2 session, with edge timestamps for both lines. Feed
   it straight into the assembler, so the first end-to-end test uses real
   controller edges, not synthesized ones. Check the frames against the ones
   WIRE.md records.
4. **Jitter fuzz.** Add uniform random jitter of ±N µs to every edge of the
   captured and synthesized bursts. Find the largest N with no decode
   failures over, say, 100,000 frames. This number is the acceptance target
   for 9.2.
5. **Missed edges.** Delete random edges and edge pairs; check the decoder
   rejects the frame rather than producing a wrong one.

### 9.2 Bench (dev board, no controller)

1. **Loopback on RTC pads.** G26 → G27 jumper, as on 21 October, without the
   pad workaround. Proves `pin->setup()` handles the RTC mux.
2. **A/B against `remote_receiver`.** Both on the same pin, if coexistence
   works (4.8.1). Send the sniffer sequence in a loop for hours; compare frame
   counts. Any difference is a finding.
3. **Latency histogram** (8.1), idle and under load, on a C3 and on the
   original ESP32. The C3 goes first: as a single-core chip it is the worst
   case.
4. **Flash-write soak** (8.2): a test number with `restore_value: true`
   changed every few seconds while polling a loopback.
5. **Long frames.** The 36 bytes of `FF` worst case, then 64 and 255 bytes,
   on the original ESP32 and a C3.
6. **Inverted loopback**, as in `examples/sniffer.md`, to keep the polarity
   lock covered.

### 9.3 Field (real controller)

1. Passive sniff with the LNK2 fitted: receiver and monitor on our pins,
   `listen_only: true`. Compare decoded frames with what the Rain Bird app
   did.
2. Active, through the optocouplers: a day of polling. The link-stats
   counters must show no rise in lost replies against a `remote_receiver`
   build on the same hardware.

### 9.4 Acceptance criteria (to agree before starting)

- Host jitter tolerance at least 3× the worst latency spread measured on the
  bench under load, on any chip. Expect the C3 to set that number.
- No decode failures across 24 hours of loopback polling with WiFi up, on
  the C3 and the original ESP32.
- A loss rate under flash-write stress at or below the `remote_receiver`
  build's on the same bench.
- `wire.py` and the receiver tests shrink by more than the new code grows.

---

## 10. Phasing and effort

| Phase | Work | Effort | Exit |
|---|---|---|---|
| 0 | `BurstAssembler`, host tests, Saleae replay, jitter fuzz. No firmware change | 0.5–1 day | A measured jitter tolerance to discuss |
| 1 | Interrupt, ring buffer, loop integration, counters, on a branch alongside `remote_receiver` | 1 day | 9.2 passes |
| 2 | YAML switch, migration errors, `wire.py` and test cleanup, examples, sniffer docs | 0.5–1 day | CI green; 9.3.1 passes |
| 3 (optional) | T2: our own RMT transmitter channel, exact transmit-done time | 1–1.5 days | Decided after phase 2 |
| 4 (optional) | Time-slot decoder: sample at bit-clock positions recovered from mid-bit transitions, instead of classifying pulse widths. Tolerates far more per-edge jitter | ~1 day | Only if 8.1 margins are thin |

Bench and field time is extra and depends on hardware access.

Phase 1 is the only time both receivers coexist in the code. They should not
coexist in a release: two receive paths means two sets of bugs and docs.

---

## 11. Open questions for discussion

1. **Is the timing risk acceptable** on the strength of host fuzzing plus a
   bench histogram, or do we want field data from a `remote_receiver` build
   first, as a baseline?
2. **YAML knobs.** Hard-code `idle` and `filter` as protocol constants, or
   expose them for awkward optocoupler builds?
3. **Transmitter.** T1 forever, or plan for T2? Is "users write only pins"
   worth owning an RMT channel?
4. **RMT budget check.** Delete `_check_rmt_memory` once only a transmitter
   uses RMT, and rely on ESP-IDF's boot-time error? Or keep a small check for
   people with LED strips?
5. **Flash-write losses** (8.2). Is "same as line noise, retried" acceptable
   for commands sent once?
6. **Interrupt timestamps into the link.** Worth changing `LinkCore`'s inputs
   for exact `response_ms` and an exact duplicate-reply hold, or leave link
   timing on `millis()`?
7. **Raw dumps.** Rely on a stand-alone `remote_receiver` on the same pin, or
   keep our own raw log line in `remote_receiver`'s format?
8. **Listen-only on chips without RMT.** Advertise it, or leave the variant
   list as it is to keep testing manageable?
9. **The ESP-ME3 timing.** Does this land before or after multi-model work?
   It unblocks ME3 schedule reads on the original ESP32, but ME3 support does
   not depend on it if small `0C` requests turn out to work.
10. **Single-core chips failing alone.** If the original ESP32 passes 9.4
    and the C3 does not, what then? Build the time-slot decoder (phase 4)
    before release; drop the single-core chips (they work today on
    `remote_receiver`); or keep `remote_receiver` for them, against the rule
    in section 10 that the two receivers do not coexist in a release?

---

## 12. Alternatives considered

| Alternative | Why not |
|---|---|
| Keep `remote_receiver`, require C3/S3 for long replies | Zero code, but keeps every other problem in 2.3, and excludes the board this project was brought up on |
| Use `remote_receiver`'s interrupt path on RMT chips | Chosen at compile time by `SOC_RMT_SUPPORTED`. Not selectable |
| Our own RMT receiver through ESP-IDF directly | Removes the YAML, but keeps the RMT memory limit on the original ESP32, which is the main problem |
| RMT receiver with DMA | Only the S3 and P4 have RMT DMA |
| UART receive | Manchester has no start and stop bits, so it does not map onto UART framing |
| Sample the line with I2S or SPI and decode in software | Exact and interrupt-free, but heavy: a whole peripheral, a DMA stream and a bit-level decoder for a 2400 bit/s line |

---

## Appendix: references

**This repository**

- `components/rainbird/manchester.h/.cpp`: windows, `trailing_idle()`
- `components/rainbird/sip_frame.cpp`: `decode_burst`, `ReceiverDecoder`
- `components/rainbird/rainbird.cpp`: `RainbirdReceiver::on_receive`,
  `release_receiver_pads_`, `transmit_`
- `components/rainbird/wire.py`: every RMT rule
- `examples/sniffer.md`: the RTC pad finding and the worst-case symbol count
- `tests/captures/saleae/capture.csv`: the real LNK2 edge capture

**ESPHome 2026.9.1**

- `components/remote_receiver/remote_receiver.cpp`: the interrupt receiver
  for chips without RMT
- `components/esp32/gpio.cpp`: `attach_interrupt`, the
  `ESP_INTR_FLAG_LEVEL3` service, `ISRInternalGPIOPin`
- `components/esp32/hal.cpp`: `millis()` from interrupt handlers
- `core/component.h`: `enable_loop_soon_any_context()`
- `core/helpers.h`: `HighFrequencyLoopRequester`

**ESP-IDF 5.5.4**

- `components/esp_driver_gpio/src/gpio.c`: `gpio_config()` calls
  `rtc_gpio_deinit()` on RTC pads
- `components/esp_timer/private_include/esp_timer_impl.h`:
  `CONFIG_ESP_TIMER_IN_IRAM`
