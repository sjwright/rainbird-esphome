#include "controller_state.h"

namespace esphome::rainbird {

Activity ControllerState::activity() const {
  if (!this->program.has_value() || !this->active_zone.has_value()) {
    return Activity::UNKNOWN;
  }
  if (*this->program != QUEUE_PROGRAM_NONE) {
    return Activity::PROGRAM;
  }
  return *this->active_zone == 0 ? Activity::IDLE : Activity::MANUAL;
}

void ControllerState::set_page_field(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value) {
  // EspTm2Model::locate has already checked the zone, program and index for this field.
  switch (field) {
    case Field::STATION_DELAY:
      this->station_delay = static_cast<uint16_t>(value);
      break;
    case Field::GLOBAL_SENSOR_BYPASS:
      this->global_bypass = value != 0;
      break;
    case Field::WATERING_DAY:
      this->watering_day[program][index] = value != 0;
      break;
    case Field::CYCLE_DAYS:
      this->cycle_days[program] = static_cast<uint8_t>(value);
      break;
    case Field::DAYS_UNTIL_NEXT:
      this->days_until[program] = static_cast<uint8_t>(value);
      break;
    case Field::FREQUENCY:
      this->frequency[program] = static_cast<uint8_t>(value);
      break;
    case Field::START_TIME:
      this->start_minutes[program][index] = value;
      break;
    case Field::RUN_TIME:
      this->run_minutes[zone - 1][program] = static_cast<uint16_t>(value);
      break;
    default:
      break;
  }
}

}  // namespace esphome::rainbird
