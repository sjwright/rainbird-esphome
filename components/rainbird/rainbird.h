#pragma once

#include "esphome/components/remote_base/remote_base.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#include "automation.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <cstdint>
#include <vector>

namespace esphome::rainbird {

// Values match KindId in entity_kinds.py.
enum class EntityKind : uint8_t {
  VALVE = 1,
  RUN_DURATION = 2,
  PROGRAM_START = 3,
  SEASONAL_ADJUST = 4,
};

// Zone 0 and program 255 mean "this binding does not use that index".
// Program 0 is program A, so 0 cannot mean "none". Keep in step with entity_kinds.py.
static constexpr uint8_t NO_ZONE = 0;
static constexpr uint8_t NO_PROGRAM = 255;

// One write, whatever the entity domain. Local run durations do not use this.
struct WriteIntent {
  EntityKind kind;
  uint8_t zone;
  uint8_t program;
  uint8_t index;
  float value;
};

class RainbirdZone {
 public:
  void set_zone(uint8_t zone) { this->zone_ = zone; }
  uint8_t get_zone() const { return this->zone_; }

 protected:
  uint8_t zone_{0};
};

class RainbirdProgram {
 public:
  void set_program(uint8_t program) { this->program_ = program; }
  uint8_t get_program() const { return this->program_; }

 protected:
  uint8_t program_{0};
};

class RainbirdComponent final : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  bool submit(const WriteIntent &intent);

  void set_model_name(const char *model_name) { this->model_name_ = model_name; }
  void set_listen_only(bool listen_only) { this->listen_only_ = listen_only; }
  void set_allow_writes(bool allow_writes) { this->allow_writes_ = allow_writes; }
  void set_experimental(bool experimental) { this->experimental_ = experimental; }
  void set_valve_queue(bool valve_queue) { this->valve_queue_ = valve_queue; }
  void set_max_drift(uint32_t max_drift_ms) { this->max_drift_ms_ = max_drift_ms; }
  void set_refresh_interval(uint32_t refresh_interval_ms) { this->refresh_interval_ms_ = refresh_interval_ms; }
  void set_transmitter(remote_base::RemoteTransmitterBase *transmitter) { this->transmitter_ = transmitter; }
  void set_receiver(remote_base::RemoteReceiverBase *receiver) { this->receiver_ = receiver; }
  void set_monitor_receiver(remote_base::RemoteReceiverBase *receiver) { this->monitor_receiver_ = receiver; }
  void add_accept_model_id(uint16_t model_id) { this->accept_ids_.push_back(model_id); }
  void add_zone(RainbirdZone *zone) { this->zones_.push_back(zone); }
  void add_program(RainbirdProgram *program) { this->programs_.push_back(program); }
  void add_nak_trigger(NakTrigger *trigger) { this->nak_triggers_.push_back(trigger); }
  void add_frame_trigger(FrameTrigger *trigger) { this->frame_triggers_.push_back(trigger); }

#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
#endif

 protected:
  const char *model_name_{""};
  remote_base::RemoteTransmitterBase *transmitter_{nullptr};
  remote_base::RemoteReceiverBase *receiver_{nullptr};
  remote_base::RemoteReceiverBase *monitor_receiver_{nullptr};
  std::vector<RainbirdZone *> zones_{};
  std::vector<RainbirdProgram *> programs_{};
  std::vector<NakTrigger *> nak_triggers_{};
  std::vector<FrameTrigger *> frame_triggers_{};
  std::vector<uint16_t> accept_ids_{};
  bool listen_only_{false};
  bool allow_writes_{false};
  bool experimental_{false};
  bool valve_queue_{false};
  uint32_t max_drift_ms_{30000};
  uint32_t refresh_interval_ms_{3600000};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
};

}  // namespace esphome::rainbird
