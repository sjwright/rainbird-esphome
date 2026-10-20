I may be able to add to (or reinforce) your research; in my case I have an ESP-TM2 with LNK2. I'm a complete newb at this, but hopefully this will help someone else going down the same path. I traced the signal path through the LNK2 circuit and determined:

| Pin | Function | Description |
|-----|----------|-------------|
| J1 | 24V AC | |
| J2 | Controller TX | Data from Rain Bird controller to ESP32 (via optocoupler 1) |
| J3 | Signal return | Common return for the communication interface |
| J4 | Controller RX | Data from ESP32 to Rain Bird controller (via optocoupler 2) |
| J5 | 24V AC |  |

Replicating the module requires an ESP32 development board, a 24V AC to 5V DC power converter, two VO617A-2 optocouplers for isolated bidirectional communication, and a couple of resistors. The 5-pin connector on the Rain Bird controller is a fairly standard JST compatible arrangement. I was able to use a flylead from my parts bin. I'll get the exact part if someone wants it.

A simpler option could be to flash the ESP32 chip with your own firmware, although I haven't established whether its security configuration permits this. I do know that the factory firmware is flash-encryped. If it does work then it's a viable option if wifi reception isn't your main issue and you're willing to risk destroying it. As ESP32 boards go they're horribly overpriced, but probably cheaper in the long run if buying random parts on Mouser isn't your idea of fun.

The pins for flashing are exposed on a castellated edge of the circuit board. Counting from the top (connector up):

| Pin | Function |
|-----|----------|
| 1   | EN       |
| 4   | GND      |
| 6   | GPIO1    |
| 7   | GPIO3    |
| 8   | 3V3      |
| 9   | GPIO0    |

As the firmware is flash-encrypted, so a conventional flash dump won't provide usable firmware for disassembly. Thankfully it seems it might not need to be. The LNK2 sends standard Rain Bird commands to the controller, and they're the same ones `pyrainbird` uses. Each one is wrapped in a simple frame and sent as Manchester-coded serial at 2400 bit/s.

I scoped the data pins on the LNK2's ESP32 chip and got this AI analysis:

-----

**The signal itself**
- It's Manchester encoded at 2400 bit/s, so each bit is split into two halves of about 208 µs. Only two pulse widths ever appear: 208 µs and 416 µs.
- A high-then-low pair is a 1 and low-then-high is a 0, with the most significant bit first. The line idles low between frames.
- The module transmits on GPIO26. Replies come back inverted at opto 1's emitter and start with an extra half-bit.

**Frame format**
- **Requests, module to controller:** `F5 F5 1C <len> <payload…> <chk> 1D`
- **Replies, controller to module:** `F5 F5 F5 1C <len> <payload…> <chk> 1D`, so three `F5` bytes instead of two.
- `len` is the number of payload bytes, and `chk` is the low byte of len plus the payload sum. Every frame we captured checked out.
- Requests with a 2-byte payload are always sent twice back to back. 1-byte and 4-byte ones are sent once.
- The controller sends exactly one reply about 30 ms after each request. Replies are the request opcode plus 0x80, or `01 <opcode>` to acknowledge a set command.

**Commands and replies**
- **Start a zone:** `39 00 01 06` runs zone 1 for 6 minutes, and the controller acknowledges with `01 39`. The second capture also had a `4B 00 01 01` for zone 1 for 1 minute, probably from a different screen in the app.
- **Stop:** `40`, acknowledged with `01 40`.
- **Status poll:** the module runs its own fixed poll every 12.5–15 s, and more often after a command. In order, it sends `12`, `3F 00/01`, `3D 00–02`, `36`, `3E`, `4A 01`, `30 00–02`, `4C`, `3B 00`, `0B` and `10`.
- **Example replies:**
  - `92 13 A7 EA` is the date, 19/10/2026.
  - `90 0C 0A 28` is the time, 12:10:40.
  - `BF 00 01 …` means zone 1 is active.
  - `CC …` and `BB …` both include the time remaining, for example 0x0167 = 359 s.
  - `B0 0p 00 64` is a 100% water budget for program p.
- **Unknown:** `3D`, which takes and echoes a page number. It isn't in `pyrainbird`.

**What that means**
On the wire, the module behaves like a SIP protocol bridge: standard Rain Bird commands and replies, plus the framing and its own status polling. A replacement would need a Manchester transmitter and receiver, the framing and checksum, and `pyrainbird`'s command set. There's nothing proprietary on top as far as we've seen.