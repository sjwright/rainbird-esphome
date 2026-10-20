# rainbird-esphome

An [ESPHome](https://esphome.io) external component for controlling a Rain Bird
ESP-TM2 irrigation controller by talking to it directly over its TX/RX serial
interface. No Rain Bird LNK WiFi module or cloud service is needed.

> Work in progress.

## Usage

```yaml
external_components:
  - source: github://sjwright/rainbird-esphome
    components: [rainbird]
```

## Acknowledgements

The protocol knowledge comes from
[pyrainbird](https://github.com/allenporter/pyrainbird).
