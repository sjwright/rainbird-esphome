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

#ifdef USE_SWITCH
class RainbirdSwitch final : public switch_::Switch, public Component, public Parented<RainbirdComponent> {
 public:
  void setup() override;
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);
  EntityKind kind() const { return this->kind_; }
  uint8_t zone() const { return this->zone_; }
  uint8_t program() const { return this->program_; }
  uint8_t index() const { return this->index_; }

 protected:
  void write_state(bool state) override;

  EntityKind kind_{EntityKind::VALVE};
  uint8_t zone_{NO_ZONE};
  uint8_t program_{NO_PROGRAM};
  uint8_t index_{0};
};
#endif

#ifdef USE_NUMBER
class RainbirdNumber final : public number::Number, public Component, public Parented<RainbirdComponent> {
 public:
  void setup() override;
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);
  void set_initial_value(float initial_value);
  void set_restore_value(bool restore_value);
  void set_command(bool command);
  EntityKind kind() const { return this->kind_; }
  uint8_t zone() const { return this->zone_; }
  uint8_t program() const { return this->program_; }
  uint8_t index() const { return this->index_; }
  // False for local settings (run duration), which the controller never reports.
  bool is_command() const { return this->command_; }

 protected:
  void control(float value) override;

  EntityKind kind_{EntityKind::RUN_DURATION};
  uint8_t zone_{NO_ZONE};
  uint8_t program_{NO_PROGRAM};
  uint8_t index_{0};
  float initial_value_{0};
  bool has_initial_value_{false};
  bool restore_value_{false};
  // Local run durations are the exception that does not go through submit().
  bool command_{true};
  ESPPreferenceObject pref_{};
};
#endif

#ifdef USE_BUTTON
class RainbirdButton final : public button::Button, public Parented<RainbirdComponent> {
 public:
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);
  EntityKind kind() const { return this->kind_; }
  uint8_t zone() const { return this->zone_; }
  uint8_t program() const { return this->program_; }
  uint8_t index() const { return this->index_; }

 protected:
  void press_action() override;

  EntityKind kind_{EntityKind::PROGRAM_START};
  uint8_t zone_{NO_ZONE};
  uint8_t program_{NO_PROGRAM};
  uint8_t index_{0};
};
#endif

#ifdef USE_SELECT
class RainbirdSelect final : public select::Select, public Parented<RainbirdComponent> {
 public:
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);
  EntityKind kind() const { return this->kind_; }
  uint8_t program() const { return this->program_; }

 protected:
  void control(size_t index) override;

  EntityKind kind_{EntityKind::FREQUENCY};
  uint8_t zone_{NO_ZONE};
  uint8_t program_{NO_PROGRAM};
  uint8_t index_{0};
};
#endif

#ifdef USE_DATETIME_TIME
class RainbirdTime final : public datetime::TimeEntity, public Parented<RainbirdComponent> {
 public:
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);
  EntityKind kind() const { return this->kind_; }
  uint8_t program() const { return this->program_; }
  uint8_t index() const { return this->index_; }
  // Minutes since midnight, once this entity has a value. An off slot keeps the last one.
  bool minutes(uint16_t *out) const;
  void show(uint8_t hour, uint8_t minute);

 protected:
  void control(const datetime::TimeCall &call) override;

  EntityKind kind_{EntityKind::START_TIME};
  uint8_t zone_{NO_ZONE};
  uint8_t program_{NO_PROGRAM};
  uint8_t index_{0};
};
#endif

#ifdef USE_DATETIME_DATETIME
class RainbirdDateTime final : public datetime::DateTimeEntity, public Parented<RainbirdComponent> {
 public:
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);
  EntityKind kind() const { return this->kind_; }
  void show(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);

 protected:
  void control(const datetime::DateTimeCall &call) override;

  EntityKind kind_{EntityKind::CONTROLLER_CLOCK};
  uint8_t zone_{NO_ZONE};
  uint8_t program_{NO_PROGRAM};
  uint8_t index_{0};
};
#endif

}  // namespace esphome::rainbird
