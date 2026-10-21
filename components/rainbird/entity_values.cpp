#include "entity_values.h"

#include <cstdio>

namespace esphome::rainbird {

namespace {

// One row per EntityKind, in enum order. tests/python/test_entity_kinds.py reads the
// WritePath column, so keep one row per line in this shape.
constexpr KindInfo KINDS[] = {
    {EntityKind::VALVE, Field::ZONES_RUNNING, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::RUN_DURATION, NO_FIELD, NO_FIELD, Source::STATE, WritePath::LOCAL},
    {EntityKind::PROGRAM_START, NO_FIELD, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::SEASONAL_ADJUST, Field::SEASONAL_ADJUST, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::ZONE_RUNNING, Field::ZONES_RUNNING, NO_FIELD, Source::STATE, WritePath::NONE},
    // "Manual" depends on the zone as well as the program byte. 3B 00 carries both.
    {EntityKind::PROGRAM_RUNNING, Field::ACTIVE_PROGRAM, Field::ACTIVE_ZONE, Source::STATE, WritePath::NONE},
    {EntityKind::REMAINING_TIME, Field::REMAINING_TIME, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::ACTIVE_ZONE, Field::ACTIVE_ZONE, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::QUEUED_ZONES, Field::QUEUED_COUNT, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::RESPONSE_TIME, NO_FIELD, NO_FIELD, Source::LINK, WritePath::NONE},
    {EntityKind::LINK_ERRORS, NO_FIELD, NO_FIELD, Source::LINK, WritePath::NONE},
    // Any zone running, or an active zone when another entity polls it.
    {EntityKind::WATERING, Field::ZONES_RUNNING, Field::ACTIVE_ZONE, Source::STATE, WritePath::NONE},
    {EntityKind::RAIN_SENSOR, Field::RAIN_SENSOR, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::IRRIGATION_ENABLED, Field::IRRIGATION_ENABLED, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::CONNECTED, NO_FIELD, NO_FIELD, Source::LINK, WritePath::NONE},
    {EntityKind::ACTIVE_PROGRAM, Field::ACTIVE_PROGRAM, Field::ACTIVE_ZONE, Source::STATE, WritePath::NONE},
    {EntityKind::QUEUE, Field::QUEUE, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::MODEL, NO_FIELD, NO_FIELD, Source::IDENTITY, WritePath::NONE},
    {EntityKind::FIRMWARE_VERSION, NO_FIELD, NO_FIELD, Source::IDENTITY, WritePath::NONE},
    {EntityKind::SERIAL_NUMBER, NO_FIELD, NO_FIELD, Source::IDENTITY, WritePath::NONE},
    {EntityKind::SCHEDULE_CHANGED, Field::SCHEDULE_CHANGED, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::QUEUE_ZONE, NO_FIELD, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::STOP, NO_FIELD, NO_FIELD, Source::STATE, WritePath::COMMAND},
    // The zone to advance to comes from the queue list (3B 01).
    {EntityKind::ADVANCE, Field::QUEUE, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::TEST_ALL, NO_FIELD, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::SYNC_CLOCK, NO_FIELD, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::REFRESH, NO_FIELD, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::RAIN_DELAY, Field::RAIN_DELAY, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::STATION_DELAY, Field::STATION_DELAY, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::TEST_DURATION, NO_FIELD, NO_FIELD, Source::STATE, WritePath::LOCAL},
    {EntityKind::CYCLE_DAYS, Field::CYCLE_DAYS, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::DAYS_UNTIL_NEXT, Field::DAYS_UNTIL_NEXT, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::RUN_TIME, Field::RUN_TIME, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::IRRIGATION_SET, Field::IRRIGATION_ENABLED, NO_FIELD, Source::STATE, WritePath::COMMAND},
    {EntityKind::GLOBAL_BYPASS, Field::GLOBAL_SENSOR_BYPASS, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::ZONE_BYPASS, Field::ZONE_SENSOR_BYPASS, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::WATERING_DAY, Field::WATERING_DAY, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::START_ENABLED, Field::START_ENABLED, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    // Polled when the probe said 3D works. There is no decoder yet, so the sensor stays unknown.
    {EntityKind::ZONE_FAULT, Field::ZONE_FAULT, NO_FIELD, Source::STATE, WritePath::NONE},
    {EntityKind::FREQUENCY, Field::FREQUENCY, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::START_TIME, Field::START_TIME, NO_FIELD, Source::STATE, WritePath::SCHEDULE_PAGE},
    {EntityKind::CONTROLLER_CLOCK, Field::CLOCK, NO_FIELD, Source::STATE, WritePath::COMMAND},
};

constexpr size_t KIND_COUNT = sizeof(KINDS) / sizeof(KINDS[0]);

constexpr bool rows_in_order() {
  for (size_t i = 0; i < KIND_COUNT; i++) {
    if (static_cast<size_t>(KINDS[i].kind) != i + 1) {
      return false;
    }
  }
  return true;
}

static_assert(rows_in_order(), "KINDS has one row per EntityKind, in enum order");

constexpr KindInfo UNKNOWN_KIND{EntityKind::VALVE, NO_FIELD, NO_FIELD, Source::STATE, WritePath::NONE};

}  // namespace

const KindInfo &kind_info(EntityKind kind) {
  size_t index = static_cast<size_t>(kind) - 1;
  return index < KIND_COUNT ? KINDS[index] : UNKNOWN_KIND;
}

void note_binding(const Model &model, const EntityBinding &binding, SessionConfig *config) {
  if (config == nullptr) {
    return;
  }
  const KindInfo &info = kind_info(binding.kind);
  if (info.field != NO_FIELD) {
    config->used.add(info.field);
    // Schedule fields live on pages, and only the pages an entity shows are read.
    std::optional<PageField> located = model.locate(info.field, binding.zone, binding.program, binding.index);
    if (located.has_value()) {
      config->pages.add(located->page);
    }
  }
  // In queue mode, turning a valve off moves on to the next queued zone, which the queue list names.
  if (binding.kind == EntityKind::VALVE && config->valve_queue && config->experimental) {
    config->used.add(Field::QUEUE);
  }
}

Readout readout(const Session &session) {
  return Readout{session.state(), session.identity(), session.link(), session.identity_changed(),
                 session.link_changed()};
}

bool kind_changed(const EntityBinding &binding, const Readout &readout) {
  const KindInfo &info = kind_info(binding.kind);
  switch (info.source) {
    case Source::LINK:
      return readout.link_changed;
    case Source::IDENTITY:
      return readout.identity_changed;
    case Source::STATE:
      break;
  }
  auto changed = [&](Field field) {
    return field != NO_FIELD && readout.state.changed(field, binding.zone, binding.program, binding.index);
  };
  return changed(info.field) || changed(info.also);
}

bool bool_value(const EntityBinding &binding, const Readout &readout, bool *out) {
  if (out == nullptr) {
    return false;
  }
  const ControllerState &state = readout.state;
  switch (binding.kind) {
    case EntityKind::VALVE:
    case EntityKind::ZONE_RUNNING:
      return state.zone_running(binding.zone, out);
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
      *out = activity == Activity::PROGRAM && byte == binding.program;
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
    case EntityKind::IRRIGATION_ENABLED:
    case EntityKind::IRRIGATION_SET: {
      uint8_t value = 0;
      if (!state.irrigation(&value)) {
        return false;
      }
      *out = value != 0;
      return true;
    }
    case EntityKind::WATERING_DAY:
      return state.watering_day(binding.program, binding.index, out);
    case EntityKind::START_ENABLED:
      return state.start_enabled(binding.program, binding.index, out);
    case EntityKind::GLOBAL_BYPASS:
      return state.global_bypass(out);
    case EntityKind::ZONE_BYPASS:
      return state.zone_bypass(binding.zone, out);
    case EntityKind::CONNECTED:
      if (!readout.link.connected_known) {
        return false;
      }
      *out = readout.link.connected;
      return true;
    case EntityKind::ZONE_FAULT:
      // 3D has no decoder yet (SPEC 8.4). Stay unknown rather than invent a layout.
      return false;
    default:
      return false;
  }
}

bool number_value(const EntityBinding &binding, const Readout &readout, float *out) {
  if (out == nullptr) {
    return false;
  }
  const ControllerState &state = readout.state;
  switch (binding.kind) {
    case EntityKind::SEASONAL_ADJUST: {
      uint16_t percent = 0;
      if (!state.seasonal(binding.program, &percent)) {
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
    case EntityKind::RAIN_DELAY: {
      uint16_t days = 0;
      if (!state.rain_delay(&days)) {
        return false;
      }
      *out = days;
      return true;
    }
    case EntityKind::STATION_DELAY: {
      uint16_t seconds = 0;
      if (!state.station_delay(&seconds)) {
        return false;
      }
      *out = seconds;
      return true;
    }
    case EntityKind::CYCLE_DAYS: {
      uint8_t days = 0;
      if (!state.cycle_days(binding.program, &days)) {
        return false;
      }
      *out = days;
      return true;
    }
    case EntityKind::DAYS_UNTIL_NEXT: {
      uint8_t days = 0;
      if (!state.days_until(binding.program, &days)) {
        return false;
      }
      *out = days;
      return true;
    }
    case EntityKind::RUN_TIME: {
      uint16_t minutes = 0;
      if (!state.run_minutes(binding.zone, binding.program, &minutes)) {
        return false;
      }
      *out = minutes;
      return true;
    }
    case EntityKind::RESPONSE_TIME:
      if (!readout.link.response_known) {
        return false;
      }
      *out = static_cast<float>(readout.link.response_ms);
      return true;
    case EntityKind::LINK_ERRORS:
      *out = static_cast<float>(readout.link.errors);
      return true;
    default:
      return false;
  }
}

bool text_value(const EntityBinding &binding, const Readout &readout, const TextContext &context, std::string *out) {
  if (out == nullptr) {
    return false;
  }
  switch (binding.kind) {
    case EntityKind::MODEL:
      *out = model_text(readout.identity, context.model);
      return !out->empty();
    case EntityKind::FIRMWARE_VERSION:
      *out = firmware_text(readout.identity);
      return !out->empty();
    case EntityKind::SERIAL_NUMBER:
      *out = serial_text(readout.identity);
      return !out->empty();
    case EntityKind::ACTIVE_PROGRAM:
      return active_program_text(readout.state, context, out);
    case EntityKind::QUEUE:
      return queue_text(readout.state, context, out);
    case EntityKind::SCHEDULE_CHANGED: {
      uint32_t seconds = 0;
      return context.timestamp && readout.state.schedule_changed(&seconds) && context.timestamp(seconds, out);
    }
    default:
      return false;
  }
}

bool select_index(const EntityBinding &binding, const Readout &readout, uint8_t *out) {
  if (out == nullptr || binding.kind != EntityKind::FREQUENCY) {
    return false;
  }
  return readout.state.frequency(binding.program, out);
}

bool start_time(const EntityBinding &binding, const Readout &readout, uint8_t *hour, uint8_t *minute) {
  int32_t minutes = 0;
  if (hour == nullptr || minute == nullptr || binding.kind != EntityKind::START_TIME ||
      !readout.state.start_minutes(binding.program, binding.index, &minutes) || minutes < 0 || minutes >= 24 * 60) {
    return false;
  }
  *hour = static_cast<uint8_t>(minutes / 60);
  *minute = static_cast<uint8_t>(minutes % 60);
  return true;
}

bool controller_clock(const Readout &readout, WallClock *out) {
  return out != nullptr && readout.state.clock(&out->hour, &out->minute, &out->second) &&
         readout.state.date(&out->day, &out->month, &out->year);
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

bool active_program_text(const ControllerState &state, const TextContext &context, std::string *out) {
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
      *out = context.program ? context.program(byte) : std::string("Program ") + static_cast<char>('A' + byte);
      return true;
  }
  return false;
}

bool queue_text(const ControllerState &state, const TextContext &context, std::string *out) {
  if (out == nullptr || !state.queue_known()) {
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
    text += context.zone ? context.zone(item.zone) : "Zone " + std::to_string(item.zone);
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
