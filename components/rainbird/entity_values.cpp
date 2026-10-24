#include "entity_values.h"

#include <cstdio>

namespace esphome::rainbird {

namespace {

// One row per EntityKind, in enum order. tests/python/test_entity_kinds.py reads the
// WritePath column, so keep one row per line in this shape.
constexpr KindInfo KINDS[] = {
    {EntityKind::VALVE, Field::ZONES_RUNNING, WritePath::COMMAND},
    {EntityKind::RUN_DURATION, NO_FIELD, WritePath::LOCAL},
    {EntityKind::PROGRAM_START, NO_FIELD, WritePath::COMMAND},
    {EntityKind::SEASONAL_ADJUST, Field::SEASONAL_ADJUST, WritePath::COMMAND},
    {EntityKind::ZONE_RUNNING, Field::ZONES_RUNNING, WritePath::NONE},
    // "Manual" depends on the zone as well as the program byte. 3B 00 carries both.
    {EntityKind::PROGRAM_RUNNING, Field::ACTIVE_PROGRAM, WritePath::NONE},
    {EntityKind::REMAINING_TIME, Field::REMAINING_TIME, WritePath::NONE},
    {EntityKind::ACTIVE_ZONE, Field::ACTIVE_ZONE, WritePath::NONE},
    {EntityKind::QUEUED_ZONES, Field::QUEUED_COUNT, WritePath::NONE},
    {EntityKind::RESPONSE_TIME, NO_FIELD, WritePath::NONE},
    {EntityKind::LINK_ERRORS, NO_FIELD, WritePath::NONE},
    // Any zone running, or an active zone when another entity polls it.
    {EntityKind::WATERING, Field::ZONES_RUNNING, WritePath::NONE},
    {EntityKind::RAIN_SENSOR, Field::RAIN_SENSOR, WritePath::NONE},
    {EntityKind::IRRIGATION_ENABLED, Field::IRRIGATION_ENABLED, WritePath::NONE},
    {EntityKind::CONNECTED, NO_FIELD, WritePath::NONE},
    {EntityKind::ACTIVE_PROGRAM, Field::ACTIVE_PROGRAM, WritePath::NONE},
    {EntityKind::QUEUE, Field::QUEUE, WritePath::NONE},
    {EntityKind::MODEL, NO_FIELD, WritePath::NONE},
    {EntityKind::FIRMWARE_VERSION, NO_FIELD, WritePath::NONE},
    {EntityKind::SERIAL_NUMBER, NO_FIELD, WritePath::NONE},
    {EntityKind::SCHEDULE_CHANGED, Field::SCHEDULE_CHANGED, WritePath::NONE},
    {EntityKind::QUEUE_ZONE, NO_FIELD, WritePath::COMMAND},
    {EntityKind::STOP, NO_FIELD, WritePath::COMMAND},
    // The zone to advance to comes from the queue list (3B 01).
    {EntityKind::ADVANCE, Field::QUEUE, WritePath::COMMAND},
    {EntityKind::TEST_ALL, NO_FIELD, WritePath::COMMAND},
    {EntityKind::SYNC_CLOCK, NO_FIELD, WritePath::COMMAND},
    {EntityKind::REFRESH, NO_FIELD, WritePath::COMMAND},
    {EntityKind::RAIN_DELAY, Field::RAIN_DELAY, WritePath::COMMAND},
    {EntityKind::STATION_DELAY, Field::STATION_DELAY, WritePath::SCHEDULE_PAGE},
    {EntityKind::TEST_DURATION, NO_FIELD, WritePath::LOCAL},
    {EntityKind::CYCLE_DAYS, Field::CYCLE_DAYS, WritePath::SCHEDULE_PAGE},
    {EntityKind::DAYS_UNTIL_NEXT, Field::DAYS_UNTIL_NEXT, WritePath::SCHEDULE_PAGE},
    {EntityKind::RUN_TIME, Field::RUN_TIME, WritePath::SCHEDULE_PAGE},
    {EntityKind::IRRIGATION_SET, Field::IRRIGATION_ENABLED, WritePath::COMMAND},
    {EntityKind::GLOBAL_BYPASS, Field::GLOBAL_SENSOR_BYPASS, WritePath::SCHEDULE_PAGE},
    {EntityKind::WATERING_DAY, Field::WATERING_DAY, WritePath::SCHEDULE_PAGE},
    {EntityKind::START_ENABLED, Field::START_ENABLED, WritePath::SCHEDULE_PAGE},
    {EntityKind::FREQUENCY, Field::FREQUENCY, WritePath::SCHEDULE_PAGE},
    {EntityKind::START_TIME, Field::START_TIME, WritePath::SCHEDULE_PAGE},
    {EntityKind::CONTROLLER_CLOCK, Field::CLOCK, WritePath::COMMAND},
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

constexpr KindInfo UNKNOWN_KIND{EntityKind::VALVE, NO_FIELD, WritePath::NONE};

}  // namespace

const KindInfo &kind_info(EntityKind kind) {
  size_t index = static_cast<size_t>(kind) - 1;
  return index < KIND_COUNT ? KINDS[index] : UNKNOWN_KIND;
}

void note_binding(const EspTm2Model &model, const EntityBinding &binding, SessionConfig *config) {
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

Readout readout(const Session &session) { return Readout{session.state(), session.identity(), session.link()}; }

namespace {

// Copies a known value out, converted to the publishing type. False while it is unknown.
template<typename T, typename U> bool put(const std::optional<T> &value, U *out) {
  if (!value.has_value()) {
    return false;
  }
  *out = static_cast<U>(*value);
  return true;
}

std::optional<uint16_t> run_minutes(const ControllerState &state, uint8_t zone, uint8_t program) {
  return zone >= 1 && zone <= STATE_ZONES ? at(state.run_minutes[zone - 1], program) : std::nullopt;
}

std::optional<int32_t> start_minutes(const ControllerState &state, uint8_t program, uint8_t index) {
  return program < STATE_PROGRAMS ? at(state.start_minutes[program], index) : std::nullopt;
}

}  // namespace

bool bool_value(const EntityBinding &binding, const Readout &readout, bool *out) {
  const ControllerState &state = readout.state;
  switch (binding.kind) {
    case EntityKind::VALVE:
    case EntityKind::ZONE_RUNNING:
      return put(at(state.running, binding.zone - 1), out);
    case EntityKind::WATERING: {
      // What the controller says is running: a zone bit, or an active zone. Narrower than
      // Session::watering_(), which also counts time left so fast polling never stops early;
      // a sensor shows only what the controller reports as running.
      bool any_known = false;
      for (const std::optional<bool> &running : state.running) {
        any_known |= running.has_value();
        if (running.value_or(false)) {
          *out = true;
          return true;
        }
      }
      if (state.active_zone.has_value()) {
        *out = *state.active_zone != 0;
        return true;
      }
      *out = false;
      return any_known;
    }
    case EntityKind::PROGRAM_RUNNING:
      if (state.activity() == Activity::UNKNOWN) {
        return false;
      }
      *out = state.activity() == Activity::PROGRAM && state.program == binding.program;
      return true;
    case EntityKind::RAIN_SENSOR:
      // Polarity is unconfirmed (SPEC 8.1): non-zero is published as on. Users can invert.
      return put(state.rain_sensor, out);
    case EntityKind::IRRIGATION_ENABLED:
    case EntityKind::IRRIGATION_SET:
      return put(state.irrigation, out);
    case EntityKind::WATERING_DAY:
      return binding.program < STATE_PROGRAMS && put(at(state.watering_day[binding.program], binding.index), out);
    case EntityKind::START_ENABLED: {
      std::optional<int32_t> minutes = start_minutes(state, binding.program, binding.index);
      if (!minutes.has_value()) {
        return false;
      }
      *out = *minutes >= 0;
      return true;
    }
    case EntityKind::GLOBAL_BYPASS:
      return put(state.global_bypass, out);
    case EntityKind::CONNECTED:
      if (!readout.link.connected_known) {
        return false;
      }
      *out = readout.link.connected;
      return true;
    default:
      return false;
  }
}

bool number_value(const EntityBinding &binding, const Readout &readout, float *out) {
  const ControllerState &state = readout.state;
  switch (binding.kind) {
    case EntityKind::SEASONAL_ADJUST:
      return put(at(state.seasonal, binding.program), out);
    case EntityKind::REMAINING_TIME:
      return put(state.remaining_seconds, out);
    case EntityKind::ACTIVE_ZONE:
      return put(state.active_zone, out);
    case EntityKind::QUEUED_ZONES:
      return put(state.queued_count, out);
    case EntityKind::RAIN_DELAY:
      return put(state.rain_delay_days, out);
    case EntityKind::STATION_DELAY:
      return put(state.station_delay, out);
    case EntityKind::CYCLE_DAYS:
      return put(at(state.cycle_days, binding.program), out);
    case EntityKind::DAYS_UNTIL_NEXT:
      return put(at(state.days_until, binding.program), out);
    case EntityKind::RUN_TIME:
      return put(run_minutes(state, binding.zone, binding.program), out);
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
    case EntityKind::SCHEDULE_CHANGED:
      return context.timestamp && readout.state.schedule_changed.has_value() &&
             context.timestamp(*readout.state.schedule_changed, out);
    default:
      return false;
  }
}

bool select_index(const EntityBinding &binding, const Readout &readout, uint8_t *out) {
  return binding.kind == EntityKind::FREQUENCY && put(at(readout.state.frequency, binding.program), out);
}

bool start_time(const EntityBinding &binding, const Readout &readout, uint8_t *hour, uint8_t *minute) {
  std::optional<int32_t> minutes = start_minutes(readout.state, binding.program, binding.index);
  if (binding.kind != EntityKind::START_TIME || !minutes.has_value() || *minutes < 0 || *minutes >= 24 * 60) {
    return false;
  }
  *hour = static_cast<uint8_t>(*minutes / 60);
  *minute = static_cast<uint8_t>(*minutes % 60);
  return true;
}

bool controller_clock(const Readout &readout, WallClock *out) {
  const ControllerState &state = readout.state;
  if (!state.time.has_value() || !state.date.has_value()) {
    return false;
  }
  *out = WallClock{state.date->year, state.date->month,  state.date->day,
                   state.time->hour, state.time->minute, state.time->second};
  return true;
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
  switch (state.activity()) {
    case Activity::UNKNOWN:
      return false;
    case Activity::IDLE:
      *out = "Idle";
      return true;
    case Activity::MANUAL:
      *out = "Manual";
      return true;
    case Activity::PROGRAM: {
      uint8_t program = *state.program;
      *out = context.program ? context.program(program) : std::string("Program ") + static_cast<char>('A' + program);
      return true;
    }
  }
  return false;
}

bool queue_text(const ControllerState &state, const TextContext &context, std::string *out) {
  if (!state.queue.has_value()) {
    return false;
  }
  std::string text;
  for (uint8_t i = 0; i < state.queue->count; i++) {
    const QueueSlot &item = state.queue->slots[i];
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
