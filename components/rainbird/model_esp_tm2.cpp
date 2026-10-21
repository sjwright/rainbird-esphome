#include "model_esp_tm2.h"

#include <cstring>

namespace esphome::rainbird {

namespace {

constexpr uint16_t MODEL_IDS[] = {0x0005, 0x000A, 0x010A, 0x0014, 0x0015};
constexpr const char *FREQUENCY_NAMES[] = {"Custom days", "Cyclic", "Odd days", "Even days"};
constexpr ModelLimits LIMITS{"ESP-TM2", STATE_ZONES, STATE_PROGRAMS, STATE_STARTS};

// Entity day index is Monday=0 … Sunday=6. The wire mask's bit 0 is Sunday.
constexpr uint8_t DAY_MASKS[] = {DAY_MONDAY, DAY_TUESDAY,  DAY_WEDNESDAY, DAY_THURSDAY,
                                 DAY_FRIDAY, DAY_SATURDAY, DAY_SUNDAY};

// Every field read from a schedule page, and the bindings it fans out over. locate() says
// where each one is; a page reply reads whichever of them land on that page.
// START_ENABLED is not listed: it is the same bytes as START_TIME, and ControllerState
// derives it from the time.
struct PageFieldSpan {
  Field field;
  bool per_zone;
  bool per_program;
  uint8_t indices;
};

constexpr PageFieldSpan PAGE_FIELDS[] = {
    {Field::STATION_DELAY, false, false, 1},
    {Field::GLOBAL_SENSOR_BYPASS, false, false, 1},
    {Field::ZONE_SENSOR_BYPASS, true, false, 1},
    {Field::WATERING_DAY, false, true, 7},
    {Field::CYCLE_DAYS, false, true, 1},
    {Field::DAYS_UNTIL_NEXT, false, true, 1},
    {Field::FREQUENCY, false, true, 1},
    {Field::START_TIME, false, true, STATE_STARTS},
    {Field::RUN_TIME, true, true, 1},
};

bool status_group(FieldSet used) {
  return used.has(Field::CLOCK) || used.has(Field::RAIN_DELAY) || used.has(Field::RAIN_SENSOR) ||
         used.has(Field::IRRIGATION_ENABLED);
}

bool schedule_interest(FieldSet used) {
  if (used.has(Field::SCHEDULE_CHANGED) || used.has(Field::START_ENABLED)) {
    return true;
  }
  for (const PageFieldSpan &span : PAGE_FIELDS) {
    if (used.has(span.field)) {
      return true;
    }
  }
  return false;
}

void apply_running_mask(ControllerState *state, uint8_t page, const uint8_t mask[4]) {
  if (page != 0) {
    return;
  }
  for (uint8_t zone = 1; zone <= STATE_ZONES; zone++) {
    state->set_zone_running(zone, station_bit(mask, 0, zone));
  }
}

void apply_available_mask(ControllerState *state, uint8_t page, const uint8_t mask[4]) {
  if (page != 0) {
    return;
  }
  for (uint8_t zone = 1; zone <= STATE_ZONES; zone++) {
    state->set_zone_available(zone, station_bit(mask, 0, zone));
  }
}

void apply_date(ControllerState *state, const PackedDate &date) {
  if (date_valid(date)) {
    state->set_date(date.day, date.month, date.year);
  } else {
    state->forget_date();
  }
}

}  // namespace

bool read_codec(const ValueCodec &codec, const PageField &field, const uint8_t *data, size_t size, int32_t *value) {
  if (data == nullptr || value == nullptr || field.width == 0 ||
      static_cast<size_t>(field.offset) + field.width > size) {
    return false;
  }
  const uint8_t *at = data + field.offset;
  switch (codec.kind) {
    case CodecKind::U8:
    case CodecKind::GLOBAL_BYPASS:
    case CodecKind::ZONE_BYPASS:
    case CodecKind::BIT:
      if (field.width != 1 || (codec.kind == CodecKind::BIT && field.mask == 0)) {
        return false;
      }
      if (codec.kind == CodecKind::GLOBAL_BYPASS) {
        *value = at[0] == GLOBAL_SENSOR_BYPASSED ? 1 : 0;
      } else if (codec.kind == CodecKind::ZONE_BYPASS) {
        *value = at[0] == ZONE_SENSOR_BYPASSED ? 1 : 0;
      } else if (codec.kind == CodecKind::BIT) {
        *value = (at[0] & field.mask) != 0 ? 1 : 0;
      } else {
        *value = at[0];
      }
      return true;
    case CodecKind::U16_BE:
    case CodecKind::START_SLOT:
    case CodecKind::SLOT_ENABLED: {
      if (field.width != 2) {
        return false;
      }
      uint16_t word = be16(at[0], at[1]);
      if (codec.kind == CodecKind::START_SLOT) {
        *value = word == START_TIME_OFF ? -1 : word;
      } else if (codec.kind == CodecKind::SLOT_ENABLED) {
        *value = word == START_TIME_OFF ? 0 : 1;
      } else {
        *value = word;
      }
      return true;
    }
    case CodecKind::NONE:
      return false;
  }
  return false;
}

bool write_codec(const ValueCodec &codec, const PageField &field, int32_t value, uint8_t *data, size_t size) {
  if (data == nullptr || static_cast<size_t>(field.offset) + field.width > size || field.width == 0) {
    return false;
  }
  uint8_t *at = data + field.offset;
  switch (codec.kind) {
    case CodecKind::U8:
    case CodecKind::GLOBAL_BYPASS:
    case CodecKind::ZONE_BYPASS:
      if (field.width != 1) {
        return false;
      }
      if (codec.kind == CodecKind::GLOBAL_BYPASS) {
        *at = value != 0 ? GLOBAL_SENSOR_BYPASSED : GLOBAL_SENSOR_OBEYED;
      } else if (codec.kind == CodecKind::ZONE_BYPASS) {
        *at = value != 0 ? ZONE_SENSOR_BYPASSED : ZONE_SENSOR_OBEYED;
      } else if (value < 0 || value > 0xFF) {
        return false;  // refused, not truncated: 256 would be written as 0
      } else {
        *at = static_cast<uint8_t>(value);
      }
      return true;
    case CodecKind::BIT:
      if (field.width != 1 || field.mask == 0) {
        return false;
      }
      if (value != 0) {
        *at = static_cast<uint8_t>(*at | field.mask);
      } else {
        *at = static_cast<uint8_t>(*at & ~field.mask);
      }
      return true;
    case CodecKind::U16_BE:
    case CodecKind::START_SLOT:
      if (field.width != 2) {
        return false;
      }
      if (codec.kind == CodecKind::START_SLOT && value < 0) {
        at[0] = 0xFF;
        at[1] = 0xFF;
        return true;
      }
      if (value < 0 || value > 0xFFFF || (codec.kind == CodecKind::START_SLOT && value >= 24 * 60)) {
        return false;
      }
      at[0] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
      at[1] = static_cast<uint8_t>(value);
      return true;
    case CodecKind::SLOT_ENABLED:
      if (field.width != 2) {
        return false;
      }
      if (value == 0) {
        at[0] = 0xFF;
        at[1] = 0xFF;
        return true;
      }
      // Turning a slot on needs a time. The page has none for an off slot, so
      // that write is refused rather than inventing one.
      return !(at[0] == 0xFF && at[1] == 0xFF);
    case CodecKind::NONE:
      return false;
  }
  return false;
}

const ModelLimits &EspTm2Model::limits() const { return LIMITS; }

bool EspTm2Model::accepts(uint16_t model_id) const {
  for (uint16_t id : MODEL_IDS) {
    if (id == model_id) {
      return true;
    }
  }
  return false;
}

void EspTm2Model::note_support(const CommandSupport &support, Capabilities *caps) const {
  if (caps == nullptr) {
    return;
  }
  switch (support.command) {
    case 0x4C:
      caps->combined = support.supported;
      break;
    case 0x4A:
      caps->event_timestamp = support.supported;
      break;
    case 0x42:
      caps->advance = support.supported;
      break;
    case 0x4B:
      caps->queue_run = support.supported;
      break;
    case 0x3D:
      caps->station_error = support.supported;
      break;
    case 0x49:
      caps->controller_state = support.supported;
      break;
    case 0x3A:
      caps->test_stations = support.supported;
      break;
    default:
      break;
  }
}

void EspTm2Model::short_poll(const Capabilities &caps, FieldSet used, RequestList *out) const {
  if (out == nullptr) {
    return;
  }
  out->count = 0;
  // Each row of the poll plan is sent only when a configured field needs it.
  if (used.has(Field::ZONES_RUNNING)) {
    out->add(request_active_stations(0));
  }
  if (status_group(used)) {
    if (caps.combined) {
      out->add(request_combined_state());
    } else {
      // 0005 has no 4C. The probe result is what selects this list, not the model id.
      if (used.has(Field::CLOCK)) {
        out->add(request_time());
        out->add(request_date());
      }
      if (used.has(Field::RAIN_DELAY)) {
        out->add(request_rain_delay());
      }
      if (used.has(Field::RAIN_SENSOR)) {
        out->add(request_rain_sensor());
      }
      if (used.has(Field::IRRIGATION_ENABLED)) {
        out->add(request_irrigation_state());
      }
    }
  }
  // 3B 00 carries the running zone, program, time left and the queued count.
  if (used.has(Field::ACTIVE_ZONE) || used.has(Field::ACTIVE_PROGRAM) || used.has(Field::REMAINING_TIME) ||
      used.has(Field::QUEUED_COUNT) || used.has(Field::QUEUE)) {
    out->add(request_queue(0));
  }
  // 3B 01 is the queue list itself.
  if (used.has(Field::QUEUE)) {
    out->add(request_queue(1));
  }
}

void EspTm2Model::slow_poll(const Capabilities &caps, FieldSet used, RequestList *out) const {
  if (out == nullptr) {
    return;
  }
  out->count = 0;
  if (used.has(Field::SEASONAL_ADJUST)) {
    for (uint8_t program = 0; program < STATE_PROGRAMS; program++) {
      out->add(request_water_budget(program));
    }
  }
  if (caps.event_timestamp && schedule_interest(used)) {
    out->add(request_event_timestamp(1));
  }
  if (caps.station_error && used.has(Field::ZONE_FAULT)) {
    out->add(request_station_error(0));
  }
}

std::optional<PageField> EspTm2Model::locate(Field field, uint8_t zone, uint8_t program, uint8_t index) const {
  PageField located;
  switch (field) {
    case Field::RUN_TIME:
      if (zone < 1 || zone > STATE_ZONES || program >= STATE_PROGRAMS) {
        return std::nullopt;
      }
      located.page = run_time_page(zone);
      located.offset = run_time_offset(zone, program);
      located.width = 2;
      return located;
    case Field::WATERING_DAY:
      if (program >= STATE_PROGRAMS || index >= 7) {
        return std::nullopt;
      }
      located.page = program_settings_page(program);
      located.offset = 0;
      located.width = 1;
      located.mask = DAY_MASKS[index];
      return located;
    case Field::CYCLE_DAYS:
    case Field::DAYS_UNTIL_NEXT:
    case Field::FREQUENCY:
      // Bytes 3 and 4 are never located, so an edit leaves them as the controller had them.
      if (program >= STATE_PROGRAMS) {
        return std::nullopt;
      }
      located.page = program_settings_page(program);
      located.offset = field == Field::CYCLE_DAYS ? 1 : field == Field::DAYS_UNTIL_NEXT ? 2 : 5;
      located.width = 1;
      return located;
    case Field::START_TIME:
    case Field::START_ENABLED:
      if (program >= STATE_PROGRAMS || index >= STATE_STARTS) {
        return std::nullopt;
      }
      located.page = start_time_page(program);
      located.offset = static_cast<uint8_t>(index * 2);
      located.width = 2;
      return located;
    case Field::STATION_DELAY:
      located.page = 0x00;
      located.offset = 0;
      located.width = 2;
      return located;
    case Field::GLOBAL_SENSOR_BYPASS:
      located.page = 0x00;
      located.offset = 3;
      located.width = 1;
      return located;
    case Field::ZONE_SENSOR_BYPASS:
      if (zone < 1 || zone > STATE_ZONES) {
        return std::nullopt;
      }
      located.page = 0x20;
      located.offset = static_cast<uint8_t>(zone - 1);
      located.width = 1;
      return located;
    default:
      return std::nullopt;
  }
}

ValueCodec EspTm2Model::codec(Field field) const {
  ValueCodec codec;
  switch (field) {
    case Field::RUN_TIME:
    case Field::STATION_DELAY:
      codec.kind = CodecKind::U16_BE;
      break;
    case Field::START_TIME:
      codec.kind = CodecKind::START_SLOT;
      break;
    case Field::START_ENABLED:
      codec.kind = CodecKind::SLOT_ENABLED;
      break;
    case Field::WATERING_DAY:
      codec.kind = CodecKind::BIT;
      break;
    case Field::GLOBAL_SENSOR_BYPASS:
      codec.kind = CodecKind::GLOBAL_BYPASS;
      break;
    case Field::ZONE_SENSOR_BYPASS:
      codec.kind = CodecKind::ZONE_BYPASS;
      break;
    case Field::FREQUENCY:
    case Field::CYCLE_DAYS:
    case Field::DAYS_UNTIL_NEXT:
      codec.kind = CodecKind::U8;
      break;
    default:
      codec.kind = CodecKind::NONE;
      break;
  }
  return codec;
}

OptionList EspTm2Model::frequency_options() const {
  return OptionList{FREQUENCY_NAMES, static_cast<uint8_t>(sizeof(FREQUENCY_NAMES) / sizeof(FREQUENCY_NAMES[0]))};
}

void EspTm2Model::apply_page_(const SchedulePage &page, ControllerState *state) const {
  for (const PageFieldSpan &span : PAGE_FIELDS) {
    const uint8_t zones = span.per_zone ? STATE_ZONES : 1;
    const uint8_t programs = span.per_program ? STATE_PROGRAMS : 1;
    for (uint8_t z = 0; z < zones; z++) {
      const uint8_t zone = span.per_zone ? static_cast<uint8_t>(z + 1) : 0;
      for (uint8_t program = 0; program < programs; program++) {
        for (uint8_t index = 0; index < span.indices; index++) {
          std::optional<PageField> located = this->locate(span.field, zone, program, index);
          int32_t value = 0;
          // A short reply carries only some fields; the rest stay as they were.
          if (located.has_value() && located->page == page.page &&
              read_codec(this->codec(span.field), *located, page.data, page.size, &value)) {
            state->set_page_field(span.field, zone, program, index, value);
          }
        }
      }
    }
  }
}

void EspTm2Model::apply_reply(const Reply &reply, ControllerState *state) const {
  if (state == nullptr) {
    return;
  }
  switch (reply.kind) {
    case ReplyKind::ACTIVE:
      apply_running_mask(state, reply.stations.page, reply.stations.mask);
      break;
    case ReplyKind::STATIONS:
      apply_available_mask(state, reply.stations.page, reply.stations.mask);
      break;
    case ReplyKind::COMBINED: {
      const CombinedState &combined = reply.combined;
      state->set_clock(combined.time.hour, combined.time.minute, combined.time.second);
      apply_date(state, combined.date);
      state->set_rain_delay(combined.rain_delay_days);
      state->set_rain_sensor(combined.sensor);
      state->set_irrigation(combined.irrigation_state);
      // The TM2 budgets per program, and only 30/B0 replies fill those cells. The 4C
      // field is a controller-wide value (FFFF on every capture), so it is not program A's.
      state->set_remaining_seconds(combined.remaining_seconds);
      state->set_active_zone(combined.active_zone);
      break;
    }
    case ReplyKind::QUEUE_NOW:
      state->set_queued_count(reply.queue_now.queued);
      state->set_remaining_seconds(reply.queue_now.remaining_seconds);
      state->set_active_zone(reply.queue_now.zone);
      state->set_program_byte(reply.queue_now.program.none ? QUEUE_PROGRAM_NONE : reply.queue_now.program.index);
      break;
    case ReplyKind::QUEUE_LIST: {
      QueueItem items[STATE_QUEUE];
      uint8_t count = reply.queue_list.count;
      if (count > STATE_QUEUE) {
        count = STATE_QUEUE;
      }
      for (uint8_t i = 0; i < count; i++) {
        items[i].zone = reply.queue_list.slots[i].zone;
        items[i].flags = reply.queue_list.slots[i].flags;
        items[i].seconds = reply.queue_list.slots[i].seconds;
      }
      state->set_queue(items, count);
      break;
    }
    case ReplyKind::TIME:
      state->set_clock(reply.time.hour, reply.time.minute, reply.time.second);
      break;
    case ReplyKind::DATE:
      apply_date(state, reply.date);
      break;
    case ReplyKind::RAIN_DELAY:
      state->set_rain_delay(reply.rain_delay.days);
      break;
    case ReplyKind::RAIN_SENSOR:
      state->set_rain_sensor(reply.rain_sensor.state);
      break;
    case ReplyKind::IRRIGATION:
      state->set_irrigation(reply.irrigation.state);
      break;
    case ReplyKind::BUDGET:
      state->set_seasonal(reply.budget.program, reply.budget.percent);
      break;
    case ReplyKind::EVENT:
      if (reply.event.event == 1) {
        state->set_schedule_changed(reply.event.seconds_since_2000);
      }
      break;
    case ReplyKind::SCHEDULE:
      if (reply.schedule.page > 0xFF) {
        break;
      }
      this->apply_page_(reply.schedule, state);
      break;
    case ReplyKind::ACK:
    case ReplyKind::NAK:
    case ReplyKind::MODEL:
    case ReplyKind::SUPPORT:
    case ReplyKind::SERIAL_NUMBER:
    case ReplyKind::FIRMWARE:
    case ReplyKind::RAW:
    case ReplyKind::NONE:
      break;
  }
}

bool EspTm2Model::patch(uint8_t page, const uint8_t *data, size_t size, Field field, uint8_t zone, uint8_t program,
                        uint8_t index, int32_t value, uint8_t *out, size_t *out_size) const {
  if (data == nullptr || out == nullptr || out_size == nullptr || size > *out_size) {
    return false;
  }
  std::optional<PageField> located = this->locate(field, zone, program, index);
  if (!located.has_value() || located->page != page) {
    return false;
  }
  std::memcpy(out, data, size);
  if (!write_codec(this->codec(field), *located, value, out, size)) {
    return false;
  }
  *out_size = size;
  return true;
}

}  // namespace esphome::rainbird
