#include "rainbird_entities.h"

#include <cmath>

#ifdef USE_SWITCH
namespace esphome::rainbird {

void RainbirdSwitch::setup() {
  // Multiple inheritance can make HasLoopOverride true, which would call loop() every cycle.
  this->disable_loop();
  // No publish here. The state comes from the controller, and until it reports, it is unknown.
}

void RainbirdSwitch::write_state(bool state) {
  // Publish nothing here, accepted or refused. on_turn_on/on_turn_off run from published states,
  // so publishing the request and then taking it back would fire both. The controller's
  // reply sets the state. A refused toggle reverts in Home Assistant because no state arrives.
  this->get_parent()->submit(this->binding_, state ? 1.0f : 0.0f);
}

}  // namespace esphome::rainbird
#endif

#ifdef USE_NUMBER
namespace esphome::rainbird {

void RainbirdNumber::setup() {
  this->disable_loop();
  if (this->is_command()) {
    // Controller-backed: unknown until the controller reports. Publishing the minimum
    // here would show, for example, a 10% seasonal adjust nobody set.
    return;
  }
  float value = this->traits.get_min_value();
  if (this->restore_value_) {
    this->pref_ = this->make_entity_preference<float>();
    float restored;
    if (this->pref_.load(&restored)) {
      value = restored;
    } else if (this->has_initial_value_) {
      value = this->initial_value_;
    }
  } else if (this->has_initial_value_) {
    value = this->initial_value_;
  }
  this->publish_state(value);
}

void RainbirdNumber::set_initial_value(float initial_value) {
  this->initial_value_ = initial_value;
  this->has_initial_value_ = true;
}

void RainbirdNumber::set_restore_value(bool restore_value) { this->restore_value_ = restore_value; }

void RainbirdNumber::control(float value) {
  if (!this->is_command()) {
    // The controller does not store run durations, so this stays off submit() (PLAN 5.1).
    this->publish_state(value);
    if (this->restore_value_) {
      this->pref_.save(&value);
    }
    return;
  }
  if (!this->get_parent()->submit(this->binding_, value) && !std::isnan(this->state)) {
    // Number::publish_state does not drop repeats, so the previous value undoes the change.
    // With no controller value yet there is nothing to restore, so the state stays unknown.
    this->publish_state(this->state);
  }
}

}  // namespace esphome::rainbird
#endif

#ifdef USE_BUTTON
namespace esphome::rainbird {

void RainbirdButton::press_action() { this->get_parent()->submit(this->binding_, 0.0f); }

}  // namespace esphome::rainbird
#endif

#ifdef USE_SELECT
namespace esphome::rainbird {

void RainbirdSelect::control(size_t index) {
  // Publish nothing. Queued is not written: the confirming read publishes the new option, and a
  // failed edit leaves the controller's option showing.
  this->get_parent()->submit(this->binding_, static_cast<float>(index));
}

}  // namespace esphome::rainbird
#endif

#ifdef USE_DATETIME_TIME
namespace esphome::rainbird {

void RainbirdTime::show(uint8_t hour, uint8_t minute) {
  this->hour_ = hour;
  this->minute_ = minute;
  this->second_ = 0;
  this->publish_state();
}

bool RainbirdTime::minutes(uint16_t *out) const {
  if (out == nullptr || !this->has_state())
    return false;
  *out = static_cast<uint16_t>(this->hour_ * 60 + this->minute_);
  return true;
}

void RainbirdTime::control(const datetime::TimeCall &call) {
  // A missing part keeps what this entity already shows. With no state yet, the call has to carry it.
  if (!call.get_hour().has_value() && !this->has_state())
    return;
  uint8_t hour = call.get_hour().value_or(this->hour_);
  uint8_t minute = call.get_minute().value_or(this->minute_);
  if (this->get_parent()->start_slot_is_on(this->binding_.program, this->binding_.index)) {
    // Publish nothing, as with the switches: the confirming read shows the new time, and a
    // failed edit leaves the controller's time showing.
    this->get_parent()->submit(this->binding_, static_cast<float>(hour * 60 + minute));
    return;
  }
  // An off slot has no time on the controller (the page holds FFFF). The time stays here, and
  // the slot's enable switch writes it when it turns the slot on.
  this->show(hour, minute);
}

}  // namespace esphome::rainbird
#endif

#ifdef USE_DATETIME_DATETIME
namespace esphome::rainbird {

void RainbirdDateTime::show(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
  this->year_ = year;
  this->month_ = month;
  this->day_ = day;
  this->hour_ = hour;
  this->minute_ = minute;
  this->second_ = second;
  this->publish_state();
}

void RainbirdDateTime::control(const datetime::DateTimeCall &call) {
  uint16_t year = call.get_year().value_or(this->year_);
  uint8_t month = call.get_month().value_or(this->month_);
  uint8_t day = call.get_day().value_or(this->day_);
  uint8_t hour = call.get_hour().value_or(this->hour_);
  uint8_t minute = call.get_minute().value_or(this->minute_);
  uint8_t second = call.get_second().value_or(this->second_);
  this->get_parent()->set_controller_clock(year, month, day, hour, minute, second);
}

}  // namespace esphome::rainbird
#endif
