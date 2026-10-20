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

namespace esphome::rainbird {

#ifdef USE_SWITCH
class RainbirdSwitch final : public switch_::Switch, public Component, public Parented<RainbirdComponent> {
 public:
  void setup() override;
  void set_binding(uint8_t kind, uint8_t zone, uint8_t program, uint8_t index);

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

 protected:
  void press_action() override;

  EntityKind kind_{EntityKind::PROGRAM_START};
  uint8_t zone_{NO_ZONE};
  uint8_t program_{NO_PROGRAM};
  uint8_t index_{0};
};
#endif

}  // namespace esphome::rainbird
