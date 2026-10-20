#include "entity_values.h"

#include <cstdio>

namespace esphome::rainbird {

void add_fields(EntityKind kind, FieldSet *used) {
  if (used == nullptr) {
    return;
  }
  switch (kind) {
    case EntityKind::VALVE:
    case EntityKind::ZONE_RUNNING:
    case EntityKind::WATERING:
      used->add(Field::ZONES_RUNNING);
      break;
    case EntityKind::SEASONAL_ADJUST:
      used->add(Field::SEASONAL_ADJUST);
      break;
    case EntityKind::PROGRAM_RUNNING:
    case EntityKind::ACTIVE_PROGRAM:
      used->add(Field::ACTIVE_PROGRAM);
      break;
    case EntityKind::REMAINING_TIME:
      used->add(Field::REMAINING_TIME);
      break;
    case EntityKind::ACTIVE_ZONE:
    case EntityKind::QUEUED_ZONES:  // the queued count is in 3B 00 with the active zone
      used->add(Field::ACTIVE_ZONE);
      break;
    case EntityKind::QUEUE:
      used->add(Field::QUEUE);
      break;
    case EntityKind::RAIN_SENSOR:
      used->add(Field::RAIN_SENSOR);
      break;
    case EntityKind::IRRIGATION_ENABLED:
      used->add(Field::IRRIGATION_ENABLED);
      break;
    case EntityKind::SCHEDULE_CHANGED:
      used->add(Field::SCHEDULE_CHANGED);
      break;
    case EntityKind::RUN_DURATION:   // local setting
    case EntityKind::PROGRAM_START:  // command only
    case EntityKind::RESPONSE_TIME:  // link diagnostics
    case EntityKind::LINK_ERRORS:
    case EntityKind::CONNECTED:
    case EntityKind::MODEL:  // read once at startup
    case EntityKind::FIRMWARE_VERSION:
    case EntityKind::SERIAL_NUMBER:
      break;
  }
}

bool kind_changed(EntityKind kind, const ControllerState &state, uint8_t zone, uint8_t program) {
  switch (kind) {
    case EntityKind::VALVE:
    case EntityKind::ZONE_RUNNING:
      return state.changed(Field::ZONES_RUNNING, zone);
    case EntityKind::WATERING:
      for (uint8_t z = 1; z <= STATE_ZONES; z++) {
        if (state.changed(Field::ZONES_RUNNING, z)) {
          return true;
        }
      }
      return state.changed(Field::ACTIVE_ZONE);
    case EntityKind::SEASONAL_ADJUST:
      return state.changed(Field::SEASONAL_ADJUST, 0, program);
    case EntityKind::PROGRAM_RUNNING:
    case EntityKind::ACTIVE_PROGRAM:
      // "Manual" depends on the zone as well as the program byte.
      return state.changed(Field::ACTIVE_PROGRAM) || state.changed(Field::ACTIVE_ZONE);
    case EntityKind::REMAINING_TIME:
      return state.changed(Field::REMAINING_TIME);
    case EntityKind::ACTIVE_ZONE:
      return state.changed(Field::ACTIVE_ZONE);
    case EntityKind::QUEUED_ZONES:
    case EntityKind::QUEUE:
      return state.changed(Field::QUEUE);
    case EntityKind::RAIN_SENSOR:
      return state.changed(Field::RAIN_SENSOR);
    case EntityKind::IRRIGATION_ENABLED:
      return state.changed(Field::IRRIGATION_ENABLED);
    case EntityKind::SCHEDULE_CHANGED:
      return state.changed(Field::SCHEDULE_CHANGED);
    default:
      return false;
  }
}

bool bool_value(EntityKind kind, const ControllerState &state, uint8_t zone, uint8_t program, bool *out) {
  if (out == nullptr) {
    return false;
  }
  switch (kind) {
    case EntityKind::VALVE:
    case EntityKind::ZONE_RUNNING:
      return state.zone_running(zone, out);
    case EntityKind::WATERING: {
      bool any_known = false;
      for (uint8_t z = 1; z <= STATE_ZONES; z++) {
        bool on = false;
        if (state.zone_running(z, &on)) {
          any_known = true;
          if (on) {
            *out = true;
            return true;
          }
        }
      }
      uint8_t active = 0;
      if (state.active_zone(&active)) {
        *out = active != 0;
        return true;
      }
      if (any_known) {
        *out = false;
      }
      return any_known;
    }
    case EntityKind::PROGRAM_RUNNING: {
      Activity activity = state.activity();
      uint8_t byte = 0;
      if (activity == Activity::UNKNOWN || !state.program_byte(&byte)) {
        return false;
      }
      *out = activity == Activity::PROGRAM && byte == program;
      return true;
    }
    case EntityKind::RAIN_SENSOR: {
      // Polarity is unconfirmed (SPEC 8.1): non-zero is published as on. Users can invert.
      uint8_t value = 0;
      if (!state.rain_sensor(&value)) {
        return false;
      }
      *out = value != 0;
      return true;
    }
    case EntityKind::IRRIGATION_ENABLED: {
      uint8_t value = 0;
      if (!state.irrigation(&value)) {
        return false;
      }
      *out = value != 0;
      return true;
    }
    default:
      return false;
  }
}

bool number_value(EntityKind kind, const ControllerState &state, uint8_t zone, uint8_t program, float *out) {
  (void) zone;
  if (out == nullptr) {
    return false;
  }
  switch (kind) {
    case EntityKind::SEASONAL_ADJUST: {
      uint16_t percent = 0;
      if (!state.seasonal(program, &percent)) {
        return false;
      }
      *out = percent;
      return true;
    }
    case EntityKind::REMAINING_TIME: {
      uint16_t seconds = 0;
      if (!state.remaining_seconds(&seconds)) {
        return false;
      }
      *out = seconds;
      return true;
    }
    case EntityKind::ACTIVE_ZONE: {
      uint8_t active = 0;
      if (!state.active_zone(&active)) {
        return false;
      }
      *out = active;
      return true;
    }
    case EntityKind::QUEUED_ZONES: {
      uint8_t count = 0;
      if (!state.queued_count(&count)) {
        return false;
      }
      *out = count;
      return true;
    }
    default:
      return false;
  }
}

std::string duration_text(uint32_t seconds) {
  char buf[16];
  if (seconds >= 3600) {
    std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", static_cast<unsigned>(seconds / 3600),
                  static_cast<unsigned>((seconds / 60) % 60), static_cast<unsigned>(seconds % 60));
  } else {
    std::snprintf(buf, sizeof(buf), "%u:%02u", static_cast<unsigned>(seconds / 60),
                  static_cast<unsigned>(seconds % 60));
  }
  return buf;
}

bool active_program_text(const ControllerState &state, const Names &names, std::string *out) {
  if (out == nullptr) {
    return false;
  }
  uint8_t byte = 0;
  switch (state.activity()) {
    case Activity::UNKNOWN:
      return false;
    case Activity::IDLE:
      *out = "Idle";
      return true;
    case Activity::MANUAL:
      *out = "Manual";
      return true;
    case Activity::PROGRAM:
      if (!state.program_byte(&byte)) {
        return false;
      }
      *out = names.program ? names.program(byte) : std::string("Program ") + static_cast<char>('A' + byte);
      return true;
  }
  return false;
}

bool queue_text(const ControllerState &state, const Names &names, std::string *out) {
  if (out == nullptr) {
    return false;
  }
  if (!state.queue_known()) {
    return false;
  }
  QueueItem item;
  std::string text;
  for (uint8_t i = 0; i < state.queue_count(); i++) {
    if (!state.queue_item(i, &item)) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += names.zone ? names.zone(item.zone) : "Zone " + std::to_string(item.zone);
    text += " ";
    text += duration_text(item.seconds);
  }
  *out = text.empty() ? "Empty" : text;
  return true;
}

std::string model_text(const Identity &identity, const char *model_name) {
  if (!identity.model_known) {
    return "";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s (%04X), protocol %u.%u", model_name == nullptr ? "Rain Bird" : model_name,
                static_cast<unsigned>(identity.model.model_id), static_cast<unsigned>(identity.model.protocol_major),
                static_cast<unsigned>(identity.model.protocol_minor));
  return buf;
}

std::string firmware_text(const Identity &identity) {
  if (!identity.firmware_known) {
    return "";
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u", static_cast<unsigned>(identity.firmware.major),
                static_cast<unsigned>(identity.firmware.minor), static_cast<unsigned>(identity.firmware.patch));
  return buf;
}

std::string serial_text(const Identity &identity) {
  if (!identity.serial_known) {
    return "";
  }
  static const char *digits = "0123456789ABCDEF";
  std::string out;
  for (uint8_t byte : identity.serial.bytes) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0F]);
  }
  return out;
}

}  // namespace esphome::rainbird
