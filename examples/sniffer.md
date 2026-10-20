# Bench loopback

21 October 2026, original ESP32 devkit. `sniffer.yaml` was flashed through the CP2102 on `/dev/cu.usbserial-0001`. The chip is an ESP32-D0WDQ6 revision 1.0 with 4 MB of flash, MAC `9C:9C:1F:CA:F9:3C`, running ESPHome 2026.9.1. WiFi is still the placeholder network `example`, so the board never joins. The log is the serial port at 115200.

GPIO26, the transmitter, was jumpered to GPIO27, the receiver. On this board those are the pins marked G26 and G27. One second after boot the hub sends payload `02`, the model query. Nothing answers, so the link retries twice. Three copies go out, about 460 ms apart. `update_interval: never`, so that send is the only traffic.

The first image logged `Send 02` three times and then silence. No `Received Raw`, no `Frame 02`. Sampling the pads during a later transmit counted 91 edges on GPIO26 and 91 edges on GPIO27. The jumper was carrying the waveform. The RMT receiver was not watching the pad.

GPIO26 and GPIO27 are RTC pads on this chip. `remote_receiver` sets up its RMT channel and never calls `rtc_gpio_deinit`, so the digital input misses the pad and the receiver reports a flat line. The hub now records the receiver GPIO and, after `remote_receiver` setup, releases an RTC pad with `rtc_gpio_deinit` and enables the digital input. See `RainbirdComponent::release_receiver_pads_`.

With that in the image, the same three transmits decoded. The log showed `Frame 02`, a `Received Raw:` line starting `208, -208, 208, -208`, and on the third good frame `Receiver polarity locked: normal, aligned`. A bad checksum still does not reach `on_frame`.

There is no `api:` block, and WiFi never connects, so the "Send model query" button cannot be pressed. The boot action is what transmits. This was a direct jumper, no optocoupler, which is why the lock came up normal and aligned. The optocoupler path is still untested.
