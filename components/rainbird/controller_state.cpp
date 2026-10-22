#include "controller_state.h"

#include <type_traits>

namespace esphome::rainbird {

namespace {

// How many cells a slot has along each axis. 0 means the slot is not split that way.
struct Shape {
  uint8_t zones;
  uint8_t programs;
  uint8_t indexes;
};

// One row per Slot, in Slot order.
constexpr Shape SHAPES[] = {
    {STATE_ZONES, 0, 0},                // RUNNING
    {STATE_ZONES, 0, 0},                // AVAILABLE
    {0, 0, 0},                          // TIME
    {0, 0, 0},                          // DATE
    {0, 0, 0},                          // RAIN_DELAY
    {0, 0, 0},                          // RAIN_SENSOR
    {0, 0, 0},                          // IRRIGATION
    {0, 0, 0},                          // ACTIVE_ZONE
    {0, 0, 0},                          // PROGRAM_BYTE
    {0, 0, 0},                          // REMAINING
    {0, 0, 0},                          // QUEUED_COUNT
    {0, STATE_PROGRAMS, 0},             // SEASONAL
    {0, 0, 0},                          // SCHEDULE_CHANGED
    {STATE_ZONES, STATE_PROGRAMS, 0},   // RUN_MINUTES
    {0, STATE_PROGRAMS, 7},             // WATERING_DAY
    {0, STATE_PROGRAMS, 0},             // FREQUENCY
    {0, STATE_PROGRAMS, 0},             // CYCLE_DAYS
    {0, STATE_PROGRAMS, 0},             // DAYS_UNTIL
    {0, STATE_PROGRAMS, STATE_STARTS},  // START_MINUTES
    {0, STATE_PROGRAMS, STATE_STARTS},  // START_ENABLED
    {0, 0, 0},                          // STATION_DELAY
    {0, 0, 0},                          // GLOBAL_BYPASS
    {STATE_ZONES, 0, 0},                // ZONE_BYPASS
};

constexpr size_t extent(uint8_t count) { return count == 0 ? 1 : count; }

constexpr size_t cells_in(const Shape &shape) {
  return extent(shape.zones) * extent(shape.programs) * extent(shape.indexes);
}

// Where each slot's cells start in the cell array.
constexpr size_t first_cell(size_t slot) {
  size_t total = 0;
  for (size_t i = 0; i < slot; i++) {
    total += cells_in(SHAPES[i]);
  }
  return total;
}

constexpr size_t SLOTS = sizeof(SHAPES) / sizeof(SHAPES[0]);

}  // namespace

const ControllerState::Cell *ControllerState::cell_(Slot slot, uint8_t zone, uint8_t program, uint8_t index) const {
  static_assert(SLOTS == static_cast<size_t>(Slot::COUNT), "one shape per slot");
  static_assert(first_cell(SLOTS) == CELLS, "CELLS matches the slot shapes");
  const size_t s = static_cast<size_t>(slot);
  if (s >= SLOTS) {
    return nullptr;
  }
  const Shape &shape = SHAPES[s];
  size_t zi = 0;
  size_t pi = 0;
  size_t ii = 0;
  if (shape.zones != 0) {
    if (zone < 1 || zone > shape.zones) {
      return nullptr;
    }
    zi = zone - 1;
  }
  if (shape.programs != 0) {
    if (program >= shape.programs) {
      return nullptr;
    }
    pi = program;
  }
  if (shape.indexes != 0) {
    if (index >= shape.indexes) {
      return nullptr;
    }
    ii = index;
  }
  const size_t offset = (zi * extent(shape.programs) + pi) * extent(shape.indexes) + ii;
  return &this->cells_[first_cell(s) + offset];
}

ControllerState::Cell *ControllerState::cell_(Slot slot, uint8_t zone, uint8_t program, uint8_t index) {
  return const_cast<Cell *>(static_cast<const ControllerState *>(this)->cell_(slot, zone, program, index));
}

void ControllerState::set_(Slot slot, uint8_t zone, uint8_t program, uint8_t index, int32_t value) {
  Cell *cell = this->cell_(slot, zone, program, index);
  if (cell == nullptr || (cell->known && cell->value == value)) {
    return;
  }
  cell->value = value;
  cell->known = true;
  cell->changed = true;
}

bool ControllerState::get_(Slot slot, uint8_t zone, uint8_t program, uint8_t index, int32_t *value) const {
  const Cell *cell = this->cell_(slot, zone, program, index);
  if (cell == nullptr || !cell->known || value == nullptr) {
    return false;
  }
  *value = cell->value;
  return true;
}

bool ControllerState::slot_changed_(Slot slot, uint8_t zone, uint8_t program, uint8_t index) const {
  const Shape &shape = SHAPES[static_cast<size_t>(slot)];
  if (shape.zones != 0 && zone == 0) {
    for (uint8_t z = 1; z <= shape.zones; z++) {
      if (this->slot_changed_(slot, z, program, index)) {
        return true;
      }
    }
    return false;
  }
  const Cell *cell = this->cell_(slot, zone, program, index);
  return cell != nullptr && cell->changed;
}

template<typename T>
bool ControllerState::read_(Slot slot, uint8_t zone, uint8_t program, uint8_t index, T *out) const {
  int32_t value = 0;
  if (out == nullptr || !this->get_(slot, zone, program, index, &value)) {
    return false;
  }
  if constexpr (std::is_same_v<T, bool>) {
    *out = value != 0;
  } else {
    *out = static_cast<T>(value);
  }
  return true;
}

void ControllerState::clear_changed() {
  for (Cell &cell : this->cells_) {
    cell.changed = false;
  }
  this->remaining_refreshed_ = false;
  this->queue_changed_ = false;
}

bool ControllerState::changed(Field field, uint8_t zone, uint8_t program, uint8_t index) const {
  switch (field) {
    case Field::ZONES_RUNNING:
      return this->slot_changed_(Slot::RUNNING, zone, 0, 0);
    case Field::CLOCK:
      return this->slot_changed_(Slot::TIME, 0, 0, 0) || this->slot_changed_(Slot::DATE, 0, 0, 0);
    case Field::RAIN_DELAY:
      return this->slot_changed_(Slot::RAIN_DELAY, 0, 0, 0);
    case Field::RAIN_SENSOR:
      return this->slot_changed_(Slot::RAIN_SENSOR, 0, 0, 0);
    case Field::IRRIGATION_ENABLED:
      return this->slot_changed_(Slot::IRRIGATION, 0, 0, 0);
    case Field::ACTIVE_ZONE:
      return this->slot_changed_(Slot::ACTIVE_ZONE, 0, 0, 0);
    case Field::ACTIVE_PROGRAM:
      return this->slot_changed_(Slot::PROGRAM_BYTE, 0, 0, 0);
    case Field::REMAINING_TIME:
      return this->slot_changed_(Slot::REMAINING, 0, 0, 0);
    case Field::QUEUE:
      return this->queue_changed_;
    case Field::QUEUED_COUNT:
      return this->slot_changed_(Slot::QUEUED_COUNT, 0, 0, 0);
    case Field::SEASONAL_ADJUST:
      return this->slot_changed_(Slot::SEASONAL, 0, program, 0);
    case Field::SCHEDULE_CHANGED:
      return this->slot_changed_(Slot::SCHEDULE_CHANGED, 0, 0, 0);
    case Field::RUN_TIME:
      return this->slot_changed_(Slot::RUN_MINUTES, zone, program, 0);
    case Field::WATERING_DAY:
      return this->slot_changed_(Slot::WATERING_DAY, 0, program, index);
    case Field::FREQUENCY:
      return this->slot_changed_(Slot::FREQUENCY, 0, program, 0);
    case Field::CYCLE_DAYS:
      return this->slot_changed_(Slot::CYCLE_DAYS, 0, program, 0);
    case Field::DAYS_UNTIL_NEXT:
      return this->slot_changed_(Slot::DAYS_UNTIL, 0, program, 0);
    case Field::START_TIME:
      return this->slot_changed_(Slot::START_MINUTES, 0, program, index);
    case Field::START_ENABLED:
      return this->slot_changed_(Slot::START_ENABLED, 0, program, index);
    case Field::STATION_DELAY:
      return this->slot_changed_(Slot::STATION_DELAY, 0, 0, 0);
    case Field::GLOBAL_SENSOR_BYPASS:
      return this->slot_changed_(Slot::GLOBAL_BYPASS, 0, 0, 0);
    case Field::ZONE_SENSOR_BYPASS:
      return this->slot_changed_(Slot::ZONE_BYPASS, zone, 0, 0);
    case Field::ZONE_FAULT:
    case Field::COUNT:
      return false;
  }
  return false;
}

void ControllerState::set_page_field(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value) {
  switch (field) {
    case Field::STATION_DELAY:
      this->set_(Slot::STATION_DELAY, 0, 0, 0, value);
      break;
    case Field::GLOBAL_SENSOR_BYPASS:
      this->set_(Slot::GLOBAL_BYPASS, 0, 0, 0, value != 0);
      break;
    case Field::ZONE_SENSOR_BYPASS:
      this->set_(Slot::ZONE_BYPASS, zone, 0, 0, value != 0);
      break;
    case Field::WATERING_DAY:
      this->set_(Slot::WATERING_DAY, 0, program, index, value != 0);
      break;
    case Field::CYCLE_DAYS:
      this->set_(Slot::CYCLE_DAYS, 0, program, 0, value);
      break;
    case Field::DAYS_UNTIL_NEXT:
      this->set_(Slot::DAYS_UNTIL, 0, program, 0, value);
      break;
    case Field::FREQUENCY:
      this->set_(Slot::FREQUENCY, 0, program, 0, value);
      break;
    case Field::START_TIME:
      this->set_start_minutes(program, index, value);
      break;
    case Field::START_ENABLED:
      this->set_(Slot::START_ENABLED, 0, program, index, value != 0);
      break;
    case Field::RUN_TIME:
      this->set_(Slot::RUN_MINUTES, zone, program, 0, value);
      break;
    default:
      break;
  }
}

void ControllerState::set_zone_running(uint8_t zone, bool on) { this->set_(Slot::RUNNING, zone, 0, 0, on); }
bool ControllerState::zone_running(uint8_t zone, bool *on) const { return this->read_(Slot::RUNNING, zone, 0, 0, on); }

void ControllerState::set_zone_available(uint8_t zone, bool on) { this->set_(Slot::AVAILABLE, zone, 0, 0, on); }
bool ControllerState::zone_available(uint8_t zone, bool *on) const {
  return this->read_(Slot::AVAILABLE, zone, 0, 0, on);
}

void ControllerState::set_clock(uint8_t hour, uint8_t minute, uint8_t second) {
  this->set_(Slot::TIME, 0, 0, 0, hour * 3600 + minute * 60 + second);
}

bool ControllerState::clock(uint8_t *hour, uint8_t *minute, uint8_t *second) const {
  int32_t seconds = 0;
  if (hour == nullptr || minute == nullptr || second == nullptr || !this->get_(Slot::TIME, 0, 0, 0, &seconds)) {
    return false;
  }
  *hour = static_cast<uint8_t>(seconds / 3600);
  *minute = static_cast<uint8_t>((seconds / 60) % 60);
  *second = static_cast<uint8_t>(seconds % 60);
  return true;
}

void ControllerState::set_date(uint8_t day, uint8_t month, uint16_t year) {
  this->set_(Slot::DATE, 0, 0, 0, static_cast<int32_t>(year) * 10000 + month * 100 + day);
}

void ControllerState::forget_date() {
  Cell *cell = this->cell_(Slot::DATE);
  if (cell->known) {
    cell->known = false;
    cell->changed = true;
  }
}

bool ControllerState::date(uint8_t *day, uint8_t *month, uint16_t *year) const {
  int32_t packed = 0;
  if (day == nullptr || month == nullptr || year == nullptr || !this->get_(Slot::DATE, 0, 0, 0, &packed)) {
    return false;
  }
  *year = static_cast<uint16_t>(packed / 10000);
  *month = static_cast<uint8_t>((packed / 100) % 100);
  *day = static_cast<uint8_t>(packed % 100);
  return true;
}

void ControllerState::set_rain_delay(uint16_t days) { this->set_(Slot::RAIN_DELAY, 0, 0, 0, days); }
bool ControllerState::rain_delay(uint16_t *days) const { return this->read_(Slot::RAIN_DELAY, 0, 0, 0, days); }

void ControllerState::set_rain_sensor(uint8_t state) { this->set_(Slot::RAIN_SENSOR, 0, 0, 0, state); }
bool ControllerState::rain_sensor(uint8_t *state) const { return this->read_(Slot::RAIN_SENSOR, 0, 0, 0, state); }

void ControllerState::set_irrigation(uint8_t state) { this->set_(Slot::IRRIGATION, 0, 0, 0, state); }
bool ControllerState::irrigation(uint8_t *state) const { return this->read_(Slot::IRRIGATION, 0, 0, 0, state); }

void ControllerState::set_active_zone(uint8_t zone) { this->set_(Slot::ACTIVE_ZONE, 0, 0, 0, zone); }
bool ControllerState::active_zone(uint8_t *zone) const { return this->read_(Slot::ACTIVE_ZONE, 0, 0, 0, zone); }

void ControllerState::set_program_byte(uint8_t byte) { this->set_(Slot::PROGRAM_BYTE, 0, 0, 0, byte); }
bool ControllerState::program_byte(uint8_t *byte) const { return this->read_(Slot::PROGRAM_BYTE, 0, 0, 0, byte); }

Activity ControllerState::activity() const {
  uint8_t program = 0;
  uint8_t zone = 0;
  if (!this->program_byte(&program) || !this->active_zone(&zone)) {
    return Activity::UNKNOWN;
  }
  if (program != QUEUE_PROGRAM_NONE) {
    return Activity::PROGRAM;
  }
  return zone == 0 ? Activity::IDLE : Activity::MANUAL;
}

void ControllerState::set_remaining_seconds(uint16_t seconds) {
  this->set_(Slot::REMAINING, 0, 0, 0, seconds);
  this->remaining_refreshed_ = true;
}
bool ControllerState::remaining_seconds(uint16_t *seconds) const {
  return this->read_(Slot::REMAINING, 0, 0, 0, seconds);
}

void ControllerState::set_queued_count(uint8_t count) { this->set_(Slot::QUEUED_COUNT, 0, 0, 0, count); }
bool ControllerState::queued_count(uint8_t *count) const { return this->read_(Slot::QUEUED_COUNT, 0, 0, 0, count); }

void ControllerState::set_queue(const QueueItem *items, uint8_t count) {
  if (count > STATE_QUEUE) {
    count = STATE_QUEUE;
  }
  if (items == nullptr && count != 0) {
    return;
  }
  bool same = this->queue_known_ && this->queue_count_ == count;
  for (uint8_t i = 0; same && i < count; i++) {
    const QueueItem &have = this->queue_[i];
    const QueueItem &next = items[i];
    same = have.zone == next.zone && have.flags == next.flags && have.seconds == next.seconds;
  }
  if (same) {
    return;
  }
  this->queue_count_ = count;
  for (uint8_t i = 0; i < count; i++) {
    this->queue_[i] = items[i];
  }
  this->queue_known_ = true;
  this->queue_changed_ = true;
}

bool ControllerState::queue_item(uint8_t index, QueueItem *out) const {
  if (!this->queue_known_ || out == nullptr || index >= this->queue_count_) {
    return false;
  }
  *out = this->queue_[index];
  return true;
}

void ControllerState::set_seasonal(uint8_t program, uint16_t percent) {
  this->set_(Slot::SEASONAL, 0, program, 0, percent);
}
bool ControllerState::seasonal(uint8_t program, uint16_t *percent) const {
  return this->read_(Slot::SEASONAL, 0, program, 0, percent);
}

void ControllerState::set_schedule_changed(uint32_t seconds_since_2000) {
  this->set_(Slot::SCHEDULE_CHANGED, 0, 0, 0, static_cast<int32_t>(seconds_since_2000));
}
bool ControllerState::schedule_changed(uint32_t *seconds_since_2000) const {
  return this->read_(Slot::SCHEDULE_CHANGED, 0, 0, 0, seconds_since_2000);
}

void ControllerState::set_run_minutes(uint8_t zone, uint8_t program, uint16_t minutes) {
  this->set_(Slot::RUN_MINUTES, zone, program, 0, minutes);
}
bool ControllerState::run_minutes(uint8_t zone, uint8_t program, uint16_t *minutes) const {
  return this->read_(Slot::RUN_MINUTES, zone, program, 0, minutes);
}

void ControllerState::set_watering_day(uint8_t program, uint8_t index, bool on) {
  this->set_(Slot::WATERING_DAY, 0, program, index, on);
}
bool ControllerState::watering_day(uint8_t program, uint8_t index, bool *on) const {
  return this->read_(Slot::WATERING_DAY, 0, program, index, on);
}

void ControllerState::set_frequency(uint8_t program, uint8_t frequency) {
  this->set_(Slot::FREQUENCY, 0, program, 0, frequency);
}
bool ControllerState::frequency(uint8_t program, uint8_t *frequency) const {
  return this->read_(Slot::FREQUENCY, 0, program, 0, frequency);
}

void ControllerState::set_cycle_days(uint8_t program, uint8_t days) {
  this->set_(Slot::CYCLE_DAYS, 0, program, 0, days);
}
bool ControllerState::cycle_days(uint8_t program, uint8_t *days) const {
  return this->read_(Slot::CYCLE_DAYS, 0, program, 0, days);
}

void ControllerState::set_days_until(uint8_t program, uint8_t days) {
  this->set_(Slot::DAYS_UNTIL, 0, program, 0, days);
}
bool ControllerState::days_until(uint8_t program, uint8_t *days) const {
  return this->read_(Slot::DAYS_UNTIL, 0, program, 0, days);
}

void ControllerState::set_start_minutes(uint8_t program, uint8_t index, int32_t minutes) {
  this->set_(Slot::START_MINUTES, 0, program, index, minutes);
  this->set_(Slot::START_ENABLED, 0, program, index, minutes >= 0);
}
bool ControllerState::start_minutes(uint8_t program, uint8_t index, int32_t *minutes) const {
  return this->read_(Slot::START_MINUTES, 0, program, index, minutes);
}

void ControllerState::set_start_enabled(uint8_t program, uint8_t index, bool on) {
  this->set_(Slot::START_ENABLED, 0, program, index, on);
}
bool ControllerState::start_enabled(uint8_t program, uint8_t index, bool *on) const {
  return this->read_(Slot::START_ENABLED, 0, program, index, on);
}

void ControllerState::set_station_delay(uint16_t seconds) { this->set_(Slot::STATION_DELAY, 0, 0, 0, seconds); }
bool ControllerState::station_delay(uint16_t *seconds) const {
  return this->read_(Slot::STATION_DELAY, 0, 0, 0, seconds);
}

void ControllerState::set_global_bypass(bool bypassed) { this->set_(Slot::GLOBAL_BYPASS, 0, 0, 0, bypassed); }
bool ControllerState::global_bypass(bool *bypassed) const {
  return this->read_(Slot::GLOBAL_BYPASS, 0, 0, 0, bypassed);
}

void ControllerState::set_zone_bypass(uint8_t zone, bool bypassed) {
  this->set_(Slot::ZONE_BYPASS, zone, 0, 0, bypassed);
}
bool ControllerState::zone_bypass(uint8_t zone, bool *bypassed) const {
  return this->read_(Slot::ZONE_BYPASS, zone, 0, 0, bypassed);
}

}  // namespace esphome::rainbird
