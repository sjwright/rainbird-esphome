#pragma once

#include "esphome/components/remote_base/remote_base.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#include "automation.h"
#include "countdown.h"
#include "entity_kind.h"
#include "entity_values.h"
#include "model_esp_tm2.h"
#include "session.h"
#include "sip_frame.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome::rainbird {

class RainbirdZone {
 public:
  void set_zone(uint8_t zone) { this->zone_ = zone; }
  uint8_t get_zone() const { return this->zone_; }
  void set_name(const char *name) { this->name_ = name; }
  const char *get_name() const { return this->name_; }

 protected:
  uint8_t zone_{0};
  const char *name_{""};
};

class RainbirdProgram {
 public:
  void set_program(uint8_t program) { this->program_ = program; }
  uint8_t get_program() const { return this->program_; }
  void set_name(const char *name) { this->name_ = name; }
  const char *get_name() const { return this->name_; }

 protected:
  uint8_t program_{0};
  const char *name_{""};
};

class RainbirdComponent;
class RainbirdSwitch;
class RainbirdNumber;
class RainbirdButton;
class RainbirdSelect;
class RainbirdTime;
class RainbirdDateTime;

// One listener per remote_receiver, each with its own polarity lock.
class RainbirdReceiver : public remote_base::RemoteReceiverListener {
 public:
  void set_parent(RainbirdComponent *parent) { this->parent_ = parent; }
  void set_name(const char *name) { this->name_ = name; }
  // The receiver on J2 hears the controller. The monitor (J4) only logs.
  void set_controller(bool controller) { this->controller_ = controller; }
  bool on_receive(remote_base::RemoteReceiveData data) override;

 private:
  RainbirdComponent *parent_{nullptr};
  const char *name_{"Receiver"};
  bool controller_{true};
  ReceiverDecoder decoder_{};
};

class RainbirdComponent final : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  // After remote_receiver (DATA). Its setup claims the pin; release_receiver_pads_
  // has to run once that is done.
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void update() override;
  void dump_config() override;

  // Every write from an entity. `value` is the switch state, number, option index or minutes.
  bool submit(const EntityBinding &binding, float value);
  // rainbird.zone.start / queue. The values come straight from the action, so they may come from a
  // template: anything outside zone 1-255 or 1 ms to 255 minutes is refused, never clamped.
  bool run_zone(int32_t zone, uint32_t duration_ms, bool queue);
  // Controller clock entity and the sync button. Sends 11 and 13.
  bool set_controller_clock(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);
  // True when the start slot is known to be on. An unknown slot is treated as off.
  bool start_slot_is_on(uint8_t program, uint8_t index) const;
  // Experimental. Queues one request on the link. A lost reply is not retried.
  bool send_raw(const uint8_t *data, size_t size);
  bool send_raw(const std::vector<uint8_t> &data) { return this->send_raw(data.data(), data.size()); }

  void set_model_name(const char *model_name) { this->model_name_ = model_name; }
  void set_listen_only(bool listen_only) { this->listen_only_ = listen_only; }
  void set_allow_writes(bool allow_writes) { this->allow_writes_ = allow_writes; }
  void set_experimental(bool experimental) { this->experimental_ = experimental; }
  void set_valve_queue(bool valve_queue) { this->valve_queue_ = valve_queue; }
  // Only called when the YAML has a clock_sync block. time_id alone does not sync.
  void set_clock_sync(uint32_t max_drift_ms) {
    this->clock_sync_ = true;
    this->max_drift_ms_ = max_drift_ms;
  }
  void set_refresh_interval(uint32_t refresh_interval_ms) { this->refresh_interval_ms_ = refresh_interval_ms; }
  void set_transmitter(remote_base::RemoteTransmitterBase *transmitter) { this->transmitter_ = transmitter; }
  void set_receiver(remote_base::RemoteReceiverBase *receiver) { this->receiver_ = receiver; }
  void add_receiver_pin(uint8_t pin) { this->receiver_pins_.push_back(pin); }
  void set_monitor_receiver(remote_base::RemoteReceiverBase *receiver) { this->monitor_receiver_ = receiver; }
  void add_accept_model_id(uint16_t model_id) { this->accept_ids_.push_back(model_id); }
  void add_zone(RainbirdZone *zone) { this->zones_.push_back(zone); }
  void add_program(RainbirdProgram *program) { this->programs_.push_back(program); }
  void add_nak_trigger(NakTrigger *trigger) { this->nak_triggers_.push_back(trigger); }
  void add_frame_trigger(FrameTrigger *trigger) { this->frame_triggers_.push_back(trigger); }

  // Every entity registers here with its binding: the hub polls for it and publishes to it.
#ifdef USE_SWITCH
  void add_switch(RainbirdSwitch *entity) { this->switches_.push_back(entity); }
#endif
#ifdef USE_NUMBER
  void add_number(RainbirdNumber *entity);
#endif
#ifdef USE_BUTTON
  void add_button(RainbirdButton *entity) { this->buttons_.push_back(entity); }
#endif
#ifdef USE_SELECT
  void add_select(RainbirdSelect *entity) { this->selects_.push_back(entity); }
#endif
#ifdef USE_DATETIME_TIME
  void add_time(RainbirdTime *entity);
#endif
#ifdef USE_DATETIME_DATETIME
  void add_datetime(RainbirdDateTime *entity) { this->datetimes_.push_back(entity); }
#endif
#ifdef USE_SENSOR
  void add_sensor(sensor::Sensor *entity, uint8_t kind, uint8_t zone, uint8_t program, uint8_t index) {
    this->sensors_.push_back({entity, {static_cast<EntityKind>(kind), zone, program, index}});
  }
#endif
#ifdef USE_BINARY_SENSOR
  void add_binary_sensor(binary_sensor::BinarySensor *entity, uint8_t kind, uint8_t zone, uint8_t program,
                         uint8_t index) {
    this->binary_sensors_.push_back({entity, {static_cast<EntityKind>(kind), zone, program, index}});
  }
#endif
#ifdef USE_TEXT_SENSOR
  void add_text_sensor(text_sensor::TextSensor *entity, uint8_t kind, uint8_t zone, uint8_t program, uint8_t index) {
    this->text_sensors_.push_back({entity, {static_cast<EntityKind>(kind), zone, program, index}});
  }
#endif

#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
#endif

 protected:
  friend class RainbirdReceiver;

  // A read-only entity is ESPHome's own class, so its binding is kept here.
  template<typename T> struct Bound {
    T *entity;
    EntityBinding binding;
  };

  template<typename F> void for_each_binding_(F &&visit) const;
  void deliver_frame_(const Frame &frame, const char *which, bool controller);
  void transmit_(const Transmission &tx);
  void release_receiver_pads_();
  void after_activity_();
  void handle_events_();
  void log_controller_();
  void publish_changed_();
  void publish_remaining_(const ControllerState &state);
  void publish_remaining_sensors_(uint32_t seconds);
  void tick_countdown_();
  void check_clock_();
  bool submitted_(ControlResult result);
  bool sync_clock_();
  bool queue_clock_(uint8_t hour, uint8_t minute, uint8_t second, uint8_t day, uint8_t month, uint16_t year);
  bool run_minutes_(uint8_t zone, uint8_t *minutes) const;
  bool test_minutes_(uint8_t *minutes) const;
  bool slot_minutes_(uint8_t program, uint8_t index, uint16_t *minutes) const;
  std::string zone_name_(uint8_t zone) const;
  std::string program_name_(uint8_t program) const;
  bool local_timestamp_(uint32_t seconds_since_2000, std::string *out) const;

  const char *model_name_{""};
  remote_base::RemoteTransmitterBase *transmitter_{nullptr};
  remote_base::RemoteReceiverBase *receiver_{nullptr};
  std::vector<uint8_t> receiver_pins_{};
  remote_base::RemoteReceiverBase *monitor_receiver_{nullptr};
  std::vector<RainbirdZone *> zones_{};
  std::vector<RainbirdProgram *> programs_{};
  std::vector<NakTrigger *> nak_triggers_{};
  std::vector<FrameTrigger *> frame_triggers_{};
  std::vector<uint16_t> accept_ids_{};
#ifdef USE_SWITCH
  std::vector<RainbirdSwitch *> switches_{};
#endif
#ifdef USE_NUMBER
  std::vector<RainbirdNumber *> numbers_{};
  // Local settings that commands read, by zone. Indexed when they register, so a command never searches.
  RainbirdNumber *run_durations_[STATE_ZONES]{};
  RainbirdNumber *test_duration_{nullptr};
#endif
#ifdef USE_BUTTON
  std::vector<RainbirdButton *> buttons_{};
#endif
#ifdef USE_SELECT
  std::vector<RainbirdSelect *> selects_{};
#endif
#ifdef USE_DATETIME_TIME
  std::vector<RainbirdTime *> times_{};
  // Turning a start slot on writes the time its time entity shows.
  RainbirdTime *start_times_[STATE_PROGRAMS][STATE_STARTS]{};
#endif
#ifdef USE_DATETIME_DATETIME
  std::vector<RainbirdDateTime *> datetimes_{};
#endif
#ifdef USE_SENSOR
  std::vector<Bound<sensor::Sensor>> sensors_{};
  std::vector<sensor::Sensor *> remaining_sensors_{};
#endif
#ifdef USE_BINARY_SENSOR
  std::vector<Bound<binary_sensor::BinarySensor>> binary_sensors_{};
#endif
#ifdef USE_TEXT_SENSOR
  std::vector<Bound<text_sensor::TextSensor>> text_sensors_{};
#endif
  bool listen_only_{false};
  bool allow_writes_{false};
  bool experimental_{false};
  bool valve_queue_{false};
  bool clock_sync_{false};
  uint32_t max_drift_ms_{30000};
  uint32_t refresh_interval_ms_{3600000};
  // A clock set was queued at clock_set_at_. Queued, not confirmed: the next reading shows that.
  bool clock_set_queued_{false};
  uint32_t clock_set_at_{0};
  uint32_t clock_checked_serial_{0};  // the controller clock reading last compared
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

  EspTm2Model model_{};
  std::unique_ptr<Session> session_{};
  TextContext text_{};
  Countdown countdown_{};
  uint32_t base_interval_{0};
};

template<typename... Ts> class RainbirdSendRawAction final : public Action<Ts...>, public Parented<RainbirdComponent> {
 public:
  void set_data_template(std::vector<uint8_t> (*func)(Ts...)) { this->func_ = func; }
  void set_data_static(const uint8_t *data, size_t length) {
    this->data_ = data;
    this->length_ = length;
  }

  void play(const Ts &...x) override {
    if (this->func_ != nullptr) {
      this->parent_->send_raw(this->func_(x...));
      return;
    }
    this->parent_->send_raw(this->data_, this->length_);
  }

 protected:
  std::vector<uint8_t> (*func_)(Ts...){nullptr};
  const uint8_t *data_{nullptr};
  size_t length_{0};
};

// rainbird.zone.start and rainbird.zone.queue. The hub checks the zone and duration,
// because either can come from a template.
template<typename... Ts> class RainbirdZoneAction final : public Action<Ts...>, public Parented<RainbirdComponent> {
 public:
  // int32_t, so a template's out-of-range zone is refused rather than wrapped into a valid one.
  TEMPLATABLE_VALUE(int32_t, zone)
  TEMPLATABLE_VALUE(uint32_t, duration_ms)
  void set_zone_entity(RainbirdZone *zone) { this->zone_entity_ = zone; }
  void set_queue(bool queue) { this->queue_ = queue; }

  void play(const Ts &...x) override {
    int32_t zone = this->zone_entity_ != nullptr ? this->zone_entity_->get_zone() : this->zone_.value(x...);
    this->parent_->run_zone(zone, this->duration_ms_.value(x...), this->queue_);
  }

 protected:
  RainbirdZone *zone_entity_{nullptr};
  bool queue_{false};
};

}  // namespace esphome::rainbird
