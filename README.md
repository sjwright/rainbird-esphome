# rainbird-esphome

An [ESPHome](https://esphome.io) external component for controlling a Rain Bird
ESP-TM2 irrigation controller by talking to it directly over its TX/RX serial
interface. No Rain Bird LNK WiFi module or cloud service is needed.

> Work in progress, not yet tested on a controller. The firmware identifies the
> controller, probes what it supports, polls it and publishes read-only status:
> zones running, watering, rain sensor, irrigation enabled, seasonal adjust,
> queue, active program, time remaining, controller identity and link
> diagnostics. With `listen_only: true` it decodes the same
> status passively from an attached LNK2. Commands (starting zones, programs,
> rain delay) are refused until the next step is built, so it cannot run
> irrigation. See PLAN.md section 12 for the schedule.

## Captures

`tools/raw_to_fixture.py` turns `remote_receiver` `dump: raw` log lines into
replay fixtures under `tests/captures/`, which `make -C tests/unit test` runs
through the decoder. `synthetic-replies.txt` is generated, not captured; real
captures from a controller replace it.

## Usage

```yaml
external_components:
  - source: github://sjwright/rainbird-esphome
    components: [rainbird]
```

## Acknowledgements

The protocol knowledge comes from
[pyrainbird](https://github.com/allenporter/pyrainbird).
