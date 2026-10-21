#include "controller_state.h"

namespace esphome::rainbird {

void ControllerState::Cell::set(int32_t next) {
  if (this->known && this->value == next) {
    return;
  }
  this->value = next;
  this->known = true;
  this->changed = true;
}

void ControllerState::Cell::forget() {
  if (!this->known) {
    return;
  }
  this->known = false;
  this->changed = true;
}

bool ControllerState::Cell::get(int32_t *out) const {
  if (!this->known || out == nullptr) {
    return false;
  }
  *out = this->value;
  return true;
}

int ControllerState::zone_index_(uint8_t zone) {
  if (zone < 1 || zone > STATE_ZONES) {
    return -1;
  }
  return zone - 1;
}

int ControllerState::program_index_(uint8_t program) {
  if (program >= STATE_PROGRAMS) {
    return -1;
  }
  return program;
}

ControllerState::Cell *ControllerState::cell_at_(Cell *cells, int index, int count) {
  if (cells == nullptr || index < 0 || index >= count) {
    return nullptr;
  }
  return &cells[index];
}

const ControllerState::Cell *ControllerState::cell_at_(const Cell *cells, int index, int count) const {
  if (cells == nullptr || index < 0 || index >= count) {
    return nullptr;
  }
  return &cells[index];
}

void ControllerState::clear_changed() {
  for (auto &cell : this->running_)
    cell.changed = false;
  for (auto &cell : this->available_)
    cell.changed = false;
  this->hour_.changed = false;
  this->minute_.changed = false;
  this->second_.changed = false;
  this->date_packed_.changed = false;
  this->rain_delay_.changed = false;
  this->rain_sensor_.changed = false;
  this->irrigation_.changed = false;
  this->active_zone_.changed = false;
  this->program_byte_.changed = false;
  this->remaining_.changed = false;
  this->remaining_refreshed_ = false;
  this->queued_count_.changed = false;
  for (auto &cell : this->seasonal_)
    cell.changed = false;
  this->schedule_changed_.changed = false;
  for (uint8_t zone = 0; zone < STATE_ZONES; zone++) {
    for (auto &cell : this->run_minutes_[zone])
      cell.changed = false;
    this->zone_bypass_[zone].changed = false;
  }
  for (uint8_t program = 0; program < STATE_PROGRAMS; program++) {
    for (auto &cell : this->watering_day_[program])
      cell.changed = false;
    this->frequency_[program].changed = false;
    this->cycle_days_[program].changed = false;
    this->days_until_[program].changed = false;
    for (auto &cell : this->start_minutes_[program])
      cell.changed = false;
    for (auto &cell : this->start_enabled_[program])
      cell.changed = false;
  }
  this->station_delay_.changed = false;
  this->global_bypass_.changed = false;
  this->queue_changed_ = false;
}

bool ControllerState::changed(Field field, uint8_t zone, uint8_t program, uint8_t index) const {
  int zi = zone_index_(zone);
  int pi = program_index_(program);
  switch (field) {
    case Field::ZONES_RUNNING:
      if (zone == 0) {
        for (const Cell &cell : this->running_) {
          if (cell.changed) {
            return true;
          }
        }
        return false;
      }
      return zi >= 0 && this->running_[zi].changed;
    case Field::CLOCK:
      return this->hour_.changed || this->minute_.changed || this->second_.changed || this->date_packed_.changed;
    case Field::RAIN_DELAY:
      return this->rain_delay_.changed;
    case Field::RAIN_SENSOR:
      return this->rain_sensor_.changed;
    case Field::IRRIGATION_ENABLED:
      return this->irrigation_.changed;
    case Field::ACTIVE_ZONE:
      return this->active_zone_.changed;
    case Field::ACTIVE_PROGRAM:
      return this->program_byte_.changed;
    case Field::REMAINING_TIME:
      return this->remaining_.changed;
    case Field::QUEUE:
      return this->queue_changed_;
    case Field::QUEUED_COUNT:
      return this->queued_count_.changed;
    case Field::SEASONAL_ADJUST:
      return pi >= 0 && this->seasonal_[pi].changed;
    case Field::SCHEDULE_CHANGED:
      return this->schedule_changed_.changed;
    case Field::RUN_TIME:
      return zi >= 0 && pi >= 0 && this->run_minutes_[zi][pi].changed;
    case Field::WATERING_DAY:
      return pi >= 0 && index < 7 && this->watering_day_[pi][index].changed;
    case Field::FREQUENCY:
      return pi >= 0 && this->frequency_[pi].changed;
    case Field::CYCLE_DAYS:
      return pi >= 0 && this->cycle_days_[pi].changed;
    case Field::DAYS_UNTIL_NEXT:
      return pi >= 0 && this->days_until_[pi].changed;
    case Field::START_TIME:
      return pi >= 0 && index < STATE_STARTS && this->start_minutes_[pi][index].changed;
    case Field::START_ENABLED:
      return pi >= 0 && index < STATE_STARTS && this->start_enabled_[pi][index].changed;
    case Field::STATION_DELAY:
      return this->station_delay_.changed;
    case Field::GLOBAL_SENSOR_BYPASS:
      return this->global_bypass_.changed;
    case Field::ZONE_SENSOR_BYPASS:
      if (zone == 0) {
        for (const Cell &cell : this->zone_bypass_) {
          if (cell.changed) {
            return true;
          }
        }
        return false;
      }
      return zi >= 0 && this->zone_bypass_[zi].changed;
    case Field::ZONE_FAULT:
    case Field::COUNT:
      return false;
  }
  return false;
}

void ControllerState::set_page_field(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value) {
  switch (field) {
    case Field::STATION_DELAY:
      this->set_station_delay(static_cast<uint16_t>(value));
      break;
    case Field::GLOBAL_SENSOR_BYPASS:
      this->set_global_bypass(value != 0);
      break;
    case Field::ZONE_SENSOR_BYPASS:
      this->set_zone_bypass(zone, value != 0);
      break;
    case Field::WATERING_DAY:
      this->set_watering_day(program, index, value != 0);
      break;
    case Field::CYCLE_DAYS:
      this->set_cycle_days(program, static_cast<uint8_t>(value));
      break;
    case Field::DAYS_UNTIL_NEXT:
      this->set_days_until(program, static_cast<uint8_t>(value));
      break;
    case Field::FREQUENCY:
      this->set_frequency(program, static_cast<uint8_t>(value));
      break;
    case Field::START_TIME:
      this->set_start_minutes(program, index, value);
      break;
    case Field::START_ENABLED:
      this->set_start_enabled(program, index, value != 0);
      break;
    case Field::RUN_TIME:
      this->set_run_minutes(zone, program, static_cast<uint16_t>(value));
      break;
    default:
      break;
  }
}

void ControllerState::set_zone_running(uint8_t zone, bool on) {
  Cell *cell = this->cell_at_(this->running_, zone_index_(zone), STATE_ZONES);
  if (cell != nullptr) {
    cell->set(on ? 1 : 0);
  }
}

bool ControllerState::zone_running(uint8_t zone, bool *on) const {
  const Cell *cell = this->cell_at_(this->running_, zone_index_(zone), STATE_ZONES);
  int32_t value = 0;
  if (cell == nullptr || !cell->get(&value) || on == nullptr) {
    return false;
  }
  *on = value != 0;
  return true;
}

void ControllerState::set_zone_available(uint8_t zone, bool on) {
  Cell *cell = this->cell_at_(this->available_, zone_index_(zone), STATE_ZONES);
  if (cell != nullptr) {
    cell->set(on ? 1 : 0);
  }
}

bool ControllerState::zone_available(uint8_t zone, bool *on) const {
  const Cell *cell = this->cell_at_(this->available_, zone_index_(zone), STATE_ZONES);
  int32_t value = 0;
  if (cell == nullptr || !cell->get(&value) || on == nullptr) {
    return false;
  }
  *on = value != 0;
  return true;
}

void ControllerState::set_clock(uint8_t hour, uint8_t minute, uint8_t second) {
  this->hour_.set(hour);
  this->minute_.set(minute);
  this->second_.set(second);
}

bool ControllerState::clock(uint8_t *hour, uint8_t *minute, uint8_t *second) const {
  int32_t h = 0;
  int32_t m = 0;
  int32_t s = 0;
  if (hour == nullptr || minute == nullptr || second == nullptr || !this->hour_.get(&h) || !this->minute_.get(&m) ||
      !this->second_.get(&s)) {
    return false;
  }
  *hour = static_cast<uint8_t>(h);
  *minute = static_cast<uint8_t>(m);
  *second = static_cast<uint8_t>(s);
  return true;
}

void ControllerState::set_date(uint8_t day, uint8_t month, uint16_t year) {
  this->date_packed_.set(static_cast<int32_t>(year) * 10000 + month * 100 + day);
}

void ControllerState::forget_date() { this->date_packed_.forget(); }

bool ControllerState::date(uint8_t *day, uint8_t *month, uint16_t *year) const {
  int32_t packed = 0;
  if (day == nullptr || month == nullptr || year == nullptr || !this->date_packed_.get(&packed)) {
    return false;
  }
  *year = static_cast<uint16_t>(packed / 10000);
  *month = static_cast<uint8_t>((packed / 100) % 100);
  *day = static_cast<uint8_t>(packed % 100);
  return true;
}

void ControllerState::set_rain_delay(uint16_t days) { this->rain_delay_.set(days); }

bool ControllerState::rain_delay(uint16_t *days) const {
  int32_t value = 0;
  if (days == nullptr || !this->rain_delay_.get(&value)) {
    return false;
  }
  *days = static_cast<uint16_t>(value);
  return true;
}

void ControllerState::set_rain_sensor(uint8_t state) { this->rain_sensor_.set(state); }

bool ControllerState::rain_sensor(uint8_t *state) const {
  int32_t value = 0;
  if (state == nullptr || !this->rain_sensor_.get(&value)) {
    return false;
  }
  *state = static_cast<uint8_t>(value);
  return true;
}

void ControllerState::set_irrigation(uint8_t state) { this->irrigation_.set(state); }

bool ControllerState::irrigation(uint8_t *state) const {
  int32_t value = 0;
  if (state == nullptr || !this->irrigation_.get(&value)) {
    return false;
  }
  *state = static_cast<uint8_t>(value);
  return true;
}

void ControllerState::set_active_zone(uint8_t zone) { this->active_zone_.set(zone); }

bool ControllerState::active_zone(uint8_t *zone) const {
  int32_t value = 0;
  if (zone == nullptr || !this->active_zone_.get(&value)) {
    return false;
  }
  *zone = static_cast<uint8_t>(value);
  return true;
}

void ControllerState::set_program_byte(uint8_t byte) { this->program_byte_.set(byte); }

bool ControllerState::program_byte(uint8_t *byte) const {
  int32_t value = 0;
  if (byte == nullptr || !this->program_byte_.get(&value)) {
    return false;
  }
  *byte = static_cast<uint8_t>(value);
  return true;
}

Activity ControllerState::activity() const {
  int32_t program = 0;
  int32_t zone = 0;
  if (!this->program_byte_.get(&program) || !this->active_zone_.get(&zone)) {
    return Activity::UNKNOWN;
  }
  // 0xFF is manual or idle. Keep it in step with QUEUE_PROGRAM_NONE.
  if (static_cast<uint8_t>(program) != 0xFF) {
    return Activity::PROGRAM;
  }
  if (zone == 0) {
    return Activity::IDLE;
  }
  return Activity::MANUAL;
}

void ControllerState::set_remaining_seconds(uint16_t seconds) {
  this->remaining_.set(seconds);
  this->remaining_refreshed_ = true;
}

bool ControllerState::remaining_seconds(uint16_t *seconds) const {
  int32_t value = 0;
  if (seconds == nullptr || !this->remaining_.get(&value)) {
    return false;
  }
  *seconds = static_cast<uint16_t>(value);
  return true;
}

void ControllerState::set_queued_count(uint8_t count) { this->queued_count_.set(count); }

bool ControllerState::queued_count(uint8_t *count) const {
  int32_t value = 0;
  if (count == nullptr || !this->queued_count_.get(&value)) {
    return false;
  }
  *count = static_cast<uint8_t>(value);
  return true;
}

void ControllerState::set_queue(const QueueItem *items, uint8_t count) {
  if (count > STATE_QUEUE) {
    count = STATE_QUEUE;
  }
  bool same = this->queue_known_ && this->queue_count_ == count;
  if (same && items != nullptr) {
    for (uint8_t i = 0; i < count; i++) {
      const QueueItem &have = this->queue_[i];
      const QueueItem &next = items[i];
      if (have.zone != next.zone || have.flags != next.flags || have.seconds != next.seconds) {
        same = false;
        break;
      }
    }
  }
  if (items == nullptr && count != 0) {
    return;
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
  Cell *cell = this->cell_at_(this->seasonal_, program_index_(program), STATE_PROGRAMS);
  if (cell != nullptr) {
    cell->set(percent);
  }
}

bool ControllerState::seasonal(uint8_t program, uint16_t *percent) const {
  const Cell *cell = this->cell_at_(this->seasonal_, program_index_(program), STATE_PROGRAMS);
  int32_t value = 0;
  if (cell == nullptr || percent == nullptr || !cell->get(&value)) {
    return false;
  }
  *percent = static_cast<uint16_t>(value);
  return true;
}

void ControllerState::set_schedule_changed(uint32_t seconds_since_2000) {
  this->schedule_changed_.set(static_cast<int32_t>(seconds_since_2000));
}

bool ControllerState::schedule_changed(uint32_t *seconds_since_2000) const {
  int32_t value = 0;
  if (seconds_since_2000 == nullptr || !this->schedule_changed_.get(&value)) {
    return false;
  }
  *seconds_since_2000 = static_cast<uint32_t>(value);
  return true;
}

void ControllerState::set_run_minutes(uint8_t zone, uint8_t program, uint16_t minutes) {
  int zi = zone_index_(zone);
  int pi = program_index_(program);
  if (zi < 0 || pi < 0) {
    return;
  }
  this->run_minutes_[zi][pi].set(minutes);
}

bool ControllerState::run_minutes(uint8_t zone, uint8_t program, uint16_t *minutes) const {
  int zi = zone_index_(zone);
  int pi = program_index_(program);
  int32_t value = 0;
  if (zi < 0 || pi < 0 || minutes == nullptr || !this->run_minutes_[zi][pi].get(&value)) {
    return false;
  }
  *minutes = static_cast<uint16_t>(value);
  return true;
}

void ControllerState::set_watering_day(uint8_t program, uint8_t index, bool on) {
  int pi = program_index_(program);
  if (pi < 0 || index >= 7) {
    return;
  }
  this->watering_day_[pi][index].set(on ? 1 : 0);
}

bool ControllerState::watering_day(uint8_t program, uint8_t index, bool *on) const {
  int pi = program_index_(program);
  int32_t value = 0;
  if (pi < 0 || index >= 7 || on == nullptr || !this->watering_day_[pi][index].get(&value)) {
    return false;
  }
  *on = value != 0;
  return true;
}

void ControllerState::set_frequency(uint8_t program, uint8_t frequency) {
  Cell *cell = this->cell_at_(this->frequency_, program_index_(program), STATE_PROGRAMS);
  if (cell != nullptr) {
    cell->set(frequency);
  }
}

bool ControllerState::frequency(uint8_t program, uint8_t *frequency) const {
  const Cell *cell = this->cell_at_(this->frequency_, program_index_(program), STATE_PROGRAMS);
  int32_t value = 0;
  if (cell == nullptr || frequency == nullptr || !cell->get(&value)) {
    return false;
  }
  *frequency = static_cast<uint8_t>(value);
  return true;
}

void ControllerState::set_cycle_days(uint8_t program, uint8_t days) {
  Cell *cell = this->cell_at_(this->cycle_days_, program_index_(program), STATE_PROGRAMS);
  if (cell != nullptr) {
    cell->set(days);
  }
}

bool ControllerState::cycle_days(uint8_t program, uint8_t *days) const {
  const Cell *cell = this->cell_at_(this->cycle_days_, program_index_(program), STATE_PROGRAMS);
  int32_t value = 0;
  if (cell == nullptr || days == nullptr || !cell->get(&value)) {
    return false;
  }
  *days = static_cast<uint8_t>(value);
  return true;
}

void ControllerState::set_days_until(uint8_t program, uint8_t days) {
  Cell *cell = this->cell_at_(this->days_until_, program_index_(program), STATE_PROGRAMS);
  if (cell != nullptr) {
    cell->set(days);
  }
}

bool ControllerState::days_until(uint8_t program, uint8_t *days) const {
  const Cell *cell = this->cell_at_(this->days_until_, program_index_(program), STATE_PROGRAMS);
  int32_t value = 0;
  if (cell == nullptr || days == nullptr || !cell->get(&value)) {
    return false;
  }
  *days = static_cast<uint8_t>(value);
  return true;
}

void ControllerState::set_start_minutes(uint8_t program, uint8_t index, int32_t minutes) {
  int pi = program_index_(program);
  if (pi < 0 || index >= STATE_STARTS) {
    return;
  }
  this->start_minutes_[pi][index].set(minutes);
  this->start_enabled_[pi][index].set(minutes >= 0 ? 1 : 0);
}

bool ControllerState::start_minutes(uint8_t program, uint8_t index, int32_t *minutes) const {
  int pi = program_index_(program);
  if (pi < 0 || index >= STATE_STARTS || minutes == nullptr) {
    return false;
  }
  return this->start_minutes_[pi][index].get(minutes);
}

void ControllerState::set_start_enabled(uint8_t program, uint8_t index, bool on) {
  int pi = program_index_(program);
  if (pi < 0 || index >= STATE_STARTS) {
    return;
  }
  this->start_enabled_[pi][index].set(on ? 1 : 0);
}

bool ControllerState::start_enabled(uint8_t program, uint8_t index, bool *on) const {
  int pi = program_index_(program);
  int32_t value = 0;
  if (pi < 0 || index >= STATE_STARTS || on == nullptr || !this->start_enabled_[pi][index].get(&value)) {
    return false;
  }
  *on = value != 0;
  return true;
}

void ControllerState::set_station_delay(uint16_t seconds) { this->station_delay_.set(seconds); }

bool ControllerState::station_delay(uint16_t *seconds) const {
  int32_t value = 0;
  if (seconds == nullptr || !this->station_delay_.get(&value)) {
    return false;
  }
  *seconds = static_cast<uint16_t>(value);
  return true;
}

void ControllerState::set_global_bypass(bool bypassed) { this->global_bypass_.set(bypassed ? 1 : 0); }

bool ControllerState::global_bypass(bool *bypassed) const {
  int32_t value = 0;
  if (bypassed == nullptr || !this->global_bypass_.get(&value)) {
    return false;
  }
  *bypassed = value != 0;
  return true;
}

void ControllerState::set_zone_bypass(uint8_t zone, bool bypassed) {
  Cell *cell = this->cell_at_(this->zone_bypass_, zone_index_(zone), STATE_ZONES);
  if (cell != nullptr) {
    cell->set(bypassed ? 1 : 0);
  }
}

bool ControllerState::zone_bypass(uint8_t zone, bool *bypassed) const {
  const Cell *cell = this->cell_at_(this->zone_bypass_, zone_index_(zone), STATE_ZONES);
  int32_t value = 0;
  if (cell == nullptr || bypassed == nullptr || !cell->get(&value)) {
    return false;
  }
  *bypassed = value != 0;
  return true;
}

}  // namespace esphome::rainbird
