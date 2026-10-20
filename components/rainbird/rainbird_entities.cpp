#include "rainbird_entities.h"

#ifdef USE_SWITCH
namespace esphome::rainbird {

void RainbirdSwitch::setup() {
  // Multiple inheritance can make HasLoopOverride true, which would call loop() every cycle.
  this->disable_loop();
  this->publish_state(false);
}

void RainbirdSwitch::set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index) {
  this->kind_ = static_cast<EntityKind>(kind);
  this->zone_ = zone;
  this->program_ = program;
  this->index_ = index;
}

void RainbirdSwitch::write_state(bool state) {
  WriteIntent intent{this->kind_, this->zone_, this->program_, this->index_, state ? 1.0f : 0.0f};
  if (this->get_parent()->submit(intent)) {
    return;
  }
  // publish_state drops a repeat of the current level, so a refused command is announced and then taken back.
  bool previous = this->state;
  this->publish_state(state);
  if (state != previous) {
    this->publish_state(previous);
  }
}

}  // namespace esphome::rainbird
#endif

#ifdef USE_NUMBER
namespace esphome::rainbird {

void RainbirdNumber::setup() {
  this->disable_loop();
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
  if (!this->get_parent()->submit(intent)) {
    // Number::publish_state does not drop repeats, so the previous value undoes the change.
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
