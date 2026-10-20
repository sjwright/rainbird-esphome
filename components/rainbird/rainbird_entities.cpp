#include "rainbird_entities.h"

#include <cmath>

#ifdef USE_SWITCH
namespace esphome::rainbird {

void RainbirdSwitch::setup() {
  // Multiple inheritance can make HasLoopOverride true, which would call loop() every cycle.
  this->disable_loop();
  // No publish here. The state comes from the controller, and until it reports, it is unknown.
}

void RainbirdSwitch::set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index) {
  this->kind_ = static_cast<EntityKind>(kind);
  this->zone_ = zone;
  this->program_ = program;
  this->index_ = index;
}

void RainbirdSwitch::write_state(bool state) {
  WriteIntent intent{this->kind_, this->zone_, this->program_, this->index_, state ? 1.0f : 0.0f};
  // Publish nothing here, accepted or refused. on_turn_on/on_turn_off run from published states,
  // so publishing the request and then taking it back would fire both. The controller's
  // reply sets the state. A refused toggle reverts in Home Assistant because no state arrives.
  this->get_parent()->submit(intent);
}

}  // namespace esphome::rainbird
#endif

#ifdef USE_NUMBER
namespace esphome::rainbird {

void RainbirdNumber::setup() {
  this->disable_loop();
  if (this->command_) {
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

void RainbirdNumber::set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index) {
  this->kind_ = static_cast<EntityKind>(kind);
  this->zone_ = zone;
  this->program_ = program;
  this->index_ = index;
}

void RainbirdNumber::set_initial_value(float initial_value) {
  this->initial_value_ = initial_value;
  this->has_initial_value_ = true;
}

void RainbirdNumber::set_restore_value(bool restore_value) { this->restore_value_ = restore_value; }

void RainbirdNumber::set_command(bool command) { this->command_ = command; }

void RainbirdNumber::control(float value) {
  if (!this->command_) {
    // The controller does not store run durations, so this stays off submit() (PLAN 5.1).
    this->publish_state(value);
    if (this->restore_value_) {
      this->pref_.save(&value);
    }
    return;
  }
  WriteIntent intent{this->kind_, this->zone_, this->program_, this->index_, value};
  if (!this->get_parent()->submit(intent) && !std::isnan(this->state)) {
    // Number::publish_state does not drop repeats, so the previous value undoes the change.
    // With no controller value yet there is nothing to restore, so the state stays unknown.
    this->publish_state(this->state);
  }
}

}  // namespace esphome::rainbird
#endif

#ifdef USE_BUTTON
namespace esphome::rainbird {

void RainbirdButton::set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index) {
  this->kind_ = static_cast<EntityKind>(kind);
  this->zone_ = zone;
  this->program_ = program;
  this->index_ = index;
}

void RainbirdButton::press_action() {
  WriteIntent intent{this->kind_, this->zone_, this->program_, this->index_, 0.0f};
  this->get_parent()->submit(intent);
}

}  // namespace esphome::rainbird
#endif
