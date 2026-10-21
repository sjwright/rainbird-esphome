#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "rainbird.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#include "esphome/core/preferences.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_DATETIME_TIME
#include "esphome/components/datetime/time_entity.h"
#endif
#ifdef USE_DATETIME_DATETIME
#include "esphome/components/datetime/datetime_entity.h"
#endif

namespace esphome::rainbird {

// The binding every Rainbird control carries: its kind, and the zone, program and day or slot.
// Codegen sets it; the hub reads it to poll and publish, and the entity passes it to submit().
class BoundEntity {
 public:
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index) {
    this->binding_ = EntityBinding{static_cast<EntityKind>(kind), zone, program, index};
  }
  const EntityBinding &binding() const { return this->binding_; }

 protected:
  EntityBinding binding_{};
};

#ifdef USE_SWITCH
class RainbirdSwitch final : public switch_::Switch,
                             public Component,
                             public Parented<RainbirdComponent>,
                             public BoundEntity {
 public:
  void setup() override;

 protected:
  void write_state(bool state) override;
};
#endif

#ifdef USE_NUMBER
class RainbirdNumber final : public number::Number,
                             public Component,
                             public Parented<RainbirdComponent>,
                             public BoundEntity {
 public:
  void setup() override;
  void set_initial_value(float initial_value);
  void set_restore_value(bool restore_value);
  // False for local settings (run and test durations), which the controller never holds.
  bool is_command() const { return kind_info(this->binding_.kind).write != WritePath::LOCAL; }

 protected:
  void control(float value) override;

  float initial_value_{0};
  bool has_initial_value_{false};
  bool restore_value_{false};
  ESPPreferenceObject pref_{};
};
#endif

#ifdef USE_BUTTON
class RainbirdButton final : public button::Button, public Parented<RainbirdComponent>, public BoundEntity {
 protected:
  void press_action() override;
};
#endif

#ifdef USE_SELECT
class RainbirdSelect final : public select::Select, public Parented<RainbirdComponent>, public BoundEntity {
 protected:
  void control(size_t index) override;
};
#endif

#ifdef USE_DATETIME_TIME
class RainbirdTime final : public datetime::TimeEntity, public Parented<RainbirdComponent>, public BoundEntity {
 public:
  // Minutes since midnight, once this entity has a value. An off slot keeps the last one.
  bool minutes(uint16_t *out) const;
  void show(uint8_t hour, uint8_t minute);

 protected:
  void control(const datetime::TimeCall &call) override;
};
#endif

#ifdef USE_DATETIME_DATETIME
class RainbirdDateTime final : public datetime::DateTimeEntity, public Parented<RainbirdComponent>, public BoundEntity {
 public:
  void show(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);

 protected:
  void control(const datetime::DateTimeCall &call) override;
};
#endif

}  // namespace esphome::rainbird
