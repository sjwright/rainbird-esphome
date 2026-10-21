"""Config keys used across the hub, the platform modules and final validation.

Plain strings with no ESPHome import, so the host tests can load the modules that
use them. Keys ESPHome itself defines (id, name, model, time_id, ...) come from
``esphome.const`` where ESPHome is available.
"""

DOMAIN = "rainbird"

CONF_ACCEPT_MODEL_IDS = "accept_model_ids"
CONF_ALLOW_WRITES = "allow_writes"
CONF_CLOCK_SYNC = "clock_sync"
CONF_DAY = "day"
CONF_DISABLED = "disabled"
CONF_EXPERIMENTAL = "experimental"
CONF_GENERATED = "generated"
CONF_LISTEN_ONLY = "listen_only"
CONF_MAX_DRIFT = "max_drift"
CONF_MONITOR_RECEIVER_ID = "monitor_receiver_id"
CONF_ON_FRAME = "on_frame"
CONF_ON_NAK = "on_nak"
CONF_PROGRAM = "program"
CONF_PROGRAM_DEFAULTS = "program_defaults"
CONF_PROGRAMS = "programs"
CONF_RAINBIRD_ID = "rainbird_id"
CONF_REFRESH_INTERVAL = "refresh_interval"
CONF_SCHEDULE = "schedule"
CONF_SLOT = "slot"
CONF_VALVE_SWITCH_MODE = "valve_switch_mode"
CONF_ZONE = "zone"
CONF_ZONE_DEFAULTS = "zone_defaults"
CONF_ZONES = "zones"

# The hub's generated entities, kept in its config for final validation and codegen.
GENERATED_KIND = "kind_id"
GENERATED_INDEX = "index"
GENERATED_CONFIG = "config"

ACTION_SEND_RAW = "rainbird.send_raw"
ACTION_ZONE_START = "rainbird.zone.start"
ACTION_ZONE_QUEUE = "rainbird.zone.queue"
