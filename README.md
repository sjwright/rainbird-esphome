# rainbird-esphome

An [ESPHome](https://esphome.io) external component for controlling a Rain Bird
ESP-TM2 irrigation controller by talking to it directly over its TX/RX serial
interface. No Rain Bird LNK WiFi module or cloud service is needed.

> Work in progress, not yet tested on a controller. The firmware identifies the
> controller, probes what it supports, polls it and publishes its status: zones
> running, watering, rain sensor, irrigation enabled, seasonal adjust, queue,
> active program, time remaining, controller identity and link diagnostics.
> It runs and stops zones and programs, sets rain delay and seasonal adjust,
> and keeps the controller clock in step. Schedule entities read the
> controller's programs; editing them needs `schedule: allow_writes: true`.
> Commands not yet confirmed on a TM2 (`42`, `3A`, `49`, `send_raw`) need
> `experimental: true`. With `listen_only: true` it decodes the same status
> passively from an attached LNK2 and sends nothing. See PLAN.md section 12
> for the schedule.

## Usage

```yaml
external_components:
  - source: github://sjwright/rainbird-esphome
    components: [rainbird]
```

## Acknowledgements

I did the initial feasibility study and LNK2 hardware analysis.

After incoming signals were being received, control over the Saleae Logic probe was handed over to Grok Bot for logging and signal analysis.

The protocol knowledge comes from [pyrainbird](https://github.com/allenporter/pyrainbird).

Development has been a collaborative effort between myself, Grok 4.7 and Opus 5.5, with additional code reviews by GPT 6 Sol, Kimi K3 and Muse Wonder.