#include "rainbird.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#endif

#include "entity_values.h"
#include "log_tag.h"
#include "rainbird_entities.h"
#include "sip_commands.h"

#include <ctime>

namespace esphome::rainbird {

ESPHOME_LOG_TAG(TAG, "rainbird");

namespace {

constexpr uint32_t COUNTDOWN_INTERVAL_ID = 1;
// 4A timestamps count from 2000-01-01 (SPEC 5.3).
constexpr time_t SECONDS_1970_TO_2000 = 946684800;

const char *stage_text(SessionStage stage) {
  switch (stage) {
    case SessionStage::PASSIVE:
      return "listening";
    case SessionStage::MANUAL:
      return "manual (update_interval: never)";
    case SessionStage::IDLE:
      return "waiting for the controller";
    case SessionStage::IDENTIFY:
      return "identifying";
    case SessionStage::PROBE:
      return "probing";
    case SessionStage::SCHEDULE:
      return "reading the schedule";
    case SessionStage::RUNNING:
      return "running";
  }
  return "?";
}

}  // namespace

void RainbirdComponent::setup() {
  SessionConfig config;
  config.listen_only = this->listen_only_;
  this->base_interval_ = this->get_update_interval();
  config.polling = this->base_interval_ != SCHEDULER_DONT_RUN;
  config.update_interval_ms = this->base_interval_;
  config.refresh_interval_ms = this->refresh_interval_ms_;
  config.accept_ids = this->accept_ids_;
  // The fields configured entities read decide what is polled (PLAN 6.4).
#ifdef USE_SWITCH
  for (auto *entity : this->switches_) {
    add_fields(entity->kind(), &config.used);
  }
#endif
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_) {
    add_fields(entity->kind(), &config.used);
  }
#endif
#ifdef USE_SENSOR
  for (auto &binding : this->sensors_) {
    add_fields(binding.kind, &config.used);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &binding : this->binary_sensors_) {
    add_fields(binding.kind, &config.used);
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto &binding : this->text_sensors_) {
    add_fields(binding.kind, &config.used);
  }
#endif
  this->session_ = std::make_unique<Session>(this->model_, config);
  this->session_->start(millis());
  this->last_stage_ = this->session_->stage();
  this->release_receiver_pads_();
  this->after_activity_();
}

void RainbirdComponent::release_receiver_pads_() {
#ifdef USE_ESP32
  // remote_receiver hands the GPIO number to the RMT driver and never runs the
  // pin's setup(), which is what moves an original-ESP32 RTC pad onto the
  // digital input. rtc_gpio_deinit does that move. ESP-IDF only declares it
  // when SOC_RTCIO_PIN_COUNT > 0. C2 and C3 are 0, and they have no such mux.
  // This runs after remote_receiver setup, because the hub's priority is just below DATA.
  for (uint8_t pin : this->receiver_pins_) {
    auto gpio = static_cast<gpio_num_t>(pin);
#if SOC_RTCIO_PIN_COUNT > 0
    if (rtc_gpio_is_valid_gpio(gpio))
      rtc_gpio_deinit(gpio);
#endif
    gpio_input_enable(gpio);
  }
#endif
}

void RainbirdComponent::loop() {
  if (!this->session_) {
    this->disable_loop();
    return;
  }
  uint32_t now = App.get_loop_component_start_time();
  this->session_->tick(now);
  Transmission tx;
  if (this->session_->next_transmission(now, &tx)) {
    this->transmit_(tx);
  }
  this->after_activity_();
}

void RainbirdComponent::update() {
  if (!this->session_) {
    return;
  }
  this->session_->poll(millis());
  this->after_activity_();
}

void RainbirdComponent::after_activity_() {
  this->publish_changed_();

  SessionStage stage = this->session_->stage();
  if (stage != this->last_stage_) {
    ESP_LOGD(TAG, "Link: %s", stage_text(stage));
    if (stage == SessionStage::RUNNING) {
      const Identity &identity = this->session_->identity();
      ESP_LOGI(TAG, "Controller %s, firmware %s", model_text(identity, this->model_name_).c_str(),
               firmware_text(identity).c_str());
      if (!this->session_->writes_permitted()) {
        ESP_LOGW(TAG, "Model id is not one %s accepts; commands stay off. See accept_model_ids.", this->model_name_);
      }
    }
    this->last_stage_ = stage;
  }
  // Log the link going up or down once, not on every failed request.
  if (this->session_->connected_known() && (!this->link_logged_ || this->link_up_ != this->session_->connected())) {
    this->link_logged_ = true;
    this->link_up_ = this->session_->connected();
    if (this->link_up_) {
      ESP_LOGI(TAG, "Controller link up");
      this->status_clear_warning();
    } else {
      ESP_LOGW(TAG, "Controller link down");
      this->status_set_warning();
    }
  }

  // Fast while watering or just after a command (PLAN 6.3). One poller, no second timer.
  if (this->base_interval_ != SCHEDULER_DONT_RUN) {
    uint32_t want = this->session_->desired_interval(millis());
    if (want != this->get_update_interval()) {
      this->set_update_interval(want);
      this->start_poller();
    }
  }

  // loop() only runs while the link has work: a request queued, in flight or finishing.
  if (this->session_->busy()) {
    this->enable_loop();
  } else {
    this->disable_loop();
  }
}

void RainbirdComponent::transmit_(const Transmission &tx) {
  if (this->transmitter_ == nullptr) {
    return;
  }
  char pretty[format_hex_pretty_size(MAX_PAYLOAD)];
  ESP_LOGD(TAG, "Send %s", format_hex_pretty_to(pretty, tx.bytes + 4, tx.bytes[3]));
  // The transmitter pin's inverted: flag is the electrical polarity. Bytes stay normal.
  std::vector<int32_t> timings = manchester_encode(tx.bytes, tx.size, Polarity::NORMAL, Phase::ALIGNED);
  auto call = this->transmitter_->transmit();
  remote_base::RemoteTransmitData *data = call.get_data();
  data->set_carrier_frequency(0);
  data->set_data(timings);
  call.perform();
}

bool RainbirdReceiver::on_receive(remote_base::RemoteReceiveData data) {
  if (this->parent_ == nullptr) {
    return false;
  }
  const remote_base::RawTimings &raw = data.get_raw_data();
  bool was_locked = this->decoder_.lock().locked();
  BurstDecode decoded = this->decoder_.decode(raw.data(), raw.size());
  const PolarityLock &lock = this->decoder_.lock();
  if (!was_locked && lock.locked()) {
    ESP_LOGD(TAG, "%s polarity locked: %s, %s", this->name_,
             lock.polarity() == Polarity::INVERTED ? "inverted" : "normal",
             lock.phase() == Phase::EXTRA_HALF ? "extra half-bit" : "aligned");
  } else if (was_locked && !lock.locked()) {
    ESP_LOGD(TAG, "%s polarity unlocked", this->name_);
  }
  if (decoded.frames.empty()) {
    return false;
  }
  for (const Frame &frame : decoded.frames) {
    this->parent_->deliver_frame_(frame, this->name_, this->controller_);
  }
  return true;
}

void RainbirdComponent::deliver_frame_(const Frame &frame, const char *which, bool controller) {
  char pretty[format_hex_pretty_size(MAX_PAYLOAD)];
  ESP_LOGV(TAG, "%s %s", which, format_hex_pretty_to(pretty, frame.payload, frame.length));
  std::vector<uint8_t> payload(frame.payload, frame.payload + frame.length);
  for (FrameTrigger *trigger : this->frame_triggers_) {
    trigger->trigger(payload);
  }
  Nak nak;
  if (parse_nak(frame.payload, frame.length, &nak)) {
    for (NakTrigger *trigger : this->nak_triggers_) {
      trigger->trigger(nak.echo, nak.code);
    }
  }
  // Only the controller's receiver feeds state. The monitor hears requests.
  if (controller && this->session_) {
    this->session_->on_frame(frame.payload, frame.length, millis());
    this->after_activity_();
  }
}

bool RainbirdComponent::send_raw(const uint8_t *data, size_t size) {
  if (!this->experimental_) {
    ESP_LOGW(TAG, "send_raw needs experimental: true");
    return false;
  }
  if (this->transmitter_ == nullptr || !this->session_) {
    ESP_LOGW(TAG, "send_raw needs a transmitter");
    return false;
  }
  if (size == 0 || size > MAX_PAYLOAD) {
    ESP_LOGW(TAG, "send_raw payload is %u bytes; it must be 1 to %u", static_cast<unsigned>(size),
             static_cast<unsigned>(MAX_PAYLOAD));
    return false;
  }
  // Queued on the link like any request, so it never collides with a poll in flight.
  if (!this->session_->send_raw(data, size, millis())) {
    ESP_LOGW(TAG, "send_raw could not be queued");
    return false;
  }
  this->after_activity_();
  // loop() is off while the link is idle. The action runs from the scheduler,
  // outside the component phase, so send the queued frame in this turn.
  this->loop();
  return true;
}

bool RainbirdComponent::submit(const WriteIntent &intent) {
  // Controls are PLAN step S7. The model gate (D12) applies once they exist.
  ESP_LOGW(TAG,
           "Refused command (kind %u zone %u program %u index %u value %.0f): "
           "controller commands are not built yet",
           static_cast<unsigned>(intent.kind), static_cast<unsigned>(intent.zone),
           static_cast<unsigned>(intent.program), static_cast<unsigned>(intent.index), intent.value);
  return false;
}

std::string RainbirdComponent::zone_name_(uint8_t zone) const {
  for (const RainbirdZone *entry : this->zones_) {
    if (entry->get_zone() == zone && entry->get_name()[0] != '\0') {
      return entry->get_name();
    }
  }
  return "Zone " + std::to_string(zone);
}

std::string RainbirdComponent::program_name_(uint8_t program) const {
  for (const RainbirdProgram *entry : this->programs_) {
    if (entry->get_program() == program && entry->get_name()[0] != '\0') {
      return entry->get_name();
    }
  }
  return std::string("Program ") + static_cast<char>('A' + program);
}

bool RainbirdComponent::schedule_changed_text_(std::string *out) const {
#ifdef USE_TIME
  uint32_t seconds = 0;
  if (this->time_ == nullptr || !this->session_->state().schedule_changed(&seconds)) {
    return false;
  }
  // The controller stamps its own wall clock (SPEC 8.11). Read those fields as
  // local time in time_id's zone to get the instant, then publish it in UTC.
  ESPTime local = ESPTime::from_epoch_utc(SECONDS_1970_TO_2000 + static_cast<time_t>(seconds));
  local.recalc_timestamp_local();
  time_t utc = local.timestamp;
  struct tm tm{};
  gmtime_r(&utc, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
  *out = buf;
  return true;
#else
  (void) out;
  return false;
#endif
}

void RainbirdComponent::publish_changed_() {
  const ControllerState &state = this->session_->state();
  bool diagnostics = this->session_->diagnostics_changed();
  bool identity = this->session_->identity_changed();
  Names names;
  names.zone = [this](uint8_t zone) { return this->zone_name_(zone); };
  names.program = [this](uint8_t program) { return this->program_name_(program); };

#ifdef USE_SWITCH
  for (auto *entity : this->switches_) {
    bool value = false;
    if (kind_changed(entity->kind(), state, entity->zone(), entity->program()) &&
        bool_value(entity->kind(), state, entity->zone(), entity->program(), &value)) {
      entity->publish_state(value);
    }
  }
#endif
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_) {
    float value = 0;
    if (entity->is_command() && kind_changed(entity->kind(), state, entity->zone(), entity->program()) &&
        number_value(entity->kind(), state, entity->zone(), entity->program(), &value)) {
      entity->publish_state(value);
    }
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &binding : this->binary_sensors_) {
    if (binding.kind == EntityKind::CONNECTED) {
      if (diagnostics && this->session_->connected_known()) {
        binding.entity->publish_state(this->session_->connected());
      }
      continue;
    }
    bool value = false;
    if (kind_changed(binding.kind, state, binding.zone, binding.program) &&
        bool_value(binding.kind, state, binding.zone, binding.program, &value)) {
      binding.entity->publish_state(value);
    }
  }
#endif
#ifdef USE_SENSOR
  for (auto &binding : this->sensors_) {
    float value = 0;
    switch (binding.kind) {
      case EntityKind::RESPONSE_TIME:
        if (diagnostics && this->session_->response_known()) {
          binding.entity->publish_state(this->session_->response_ms());
        }
        break;
      case EntityKind::LINK_ERRORS:
        if (diagnostics) {
          binding.entity->publish_state(this->session_->link_errors());
        }
        break;
      case EntityKind::REMAINING_TIME:
        if (kind_changed(binding.kind, state, binding.zone, binding.program) &&
            number_value(binding.kind, state, binding.zone, binding.program, &value)) {
          binding.entity->publish_state(value);
          // Count down locally between polls, so Home Assistant sees time pass smoothly.
          this->countdown_from_ = static_cast<uint16_t>(value);
          this->countdown_at_ = millis();
          if (value > 0 && !this->countdown_running_) {
            this->countdown_running_ = true;
            this->set_interval(COUNTDOWN_INTERVAL_ID, 1000, [this]() { this->publish_countdown_(); });
          } else if (value <= 0 && this->countdown_running_) {
            this->countdown_running_ = false;
            this->cancel_interval(COUNTDOWN_INTERVAL_ID);
          }
        }
        break;
      default:
        if (kind_changed(binding.kind, state, binding.zone, binding.program) &&
            number_value(binding.kind, state, binding.zone, binding.program, &value)) {
          binding.entity->publish_state(value);
        }
        break;
    }
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto &binding : this->text_sensors_) {
    std::string text;
    bool publish = false;
    const Identity &who = this->session_->identity();
    switch (binding.kind) {
      case EntityKind::MODEL:
        text = model_text(who, this->model_name_);
        publish = identity && !text.empty();
        break;
      case EntityKind::FIRMWARE_VERSION:
        text = firmware_text(who);
        publish = identity && !text.empty();
        break;
      case EntityKind::SERIAL_NUMBER:
        text = serial_text(who);
        publish = identity && !text.empty();
        break;
      case EntityKind::ACTIVE_PROGRAM:
        publish = kind_changed(binding.kind, state, binding.zone, binding.program) &&
                  active_program_text(state, names, &text);
        break;
      case EntityKind::QUEUE:
        publish = kind_changed(binding.kind, state, binding.zone, binding.program) && queue_text(state, names, &text);
        break;
      case EntityKind::SCHEDULE_CHANGED:
        publish =
            kind_changed(binding.kind, state, binding.zone, binding.program) && this->schedule_changed_text_(&text);
        break;
      default:
        break;
    }
    if (publish) {
      binding.entity->publish_state(text);
    }
  }
#endif
  this->session_->state().clear_changed();
  this->session_->clear_changed();
}

void RainbirdComponent::publish_countdown_() {
#ifdef USE_SENSOR
  uint32_t elapsed = (millis() - this->countdown_at_) / 1000;
  uint32_t remaining = this->countdown_from_ > elapsed ? this->countdown_from_ - elapsed : 0;
  for (auto &binding : this->sensors_) {
    if (binding.kind == EntityKind::REMAINING_TIME) {
      binding.entity->publish_state(remaining);
    }
  }
  if (remaining == 0) {
    this->countdown_running_ = false;
    this->cancel_interval(COUNTDOWN_INTERVAL_ID);
  }
#endif
}

void RainbirdComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Rain Bird %s\n"
                "  Link: %s\n"
                "  Commands: not implemented yet\n"
                "  Listen only: %s\n"
                "  Valve mode: %s\n"
                "  Schedule writes: %s\n"
                "  Experimental: %s\n"
                "  Zones: %u\n"
                "  Programs: %u\n"
                "  Schedule refresh: %u ms",
                this->model_name_, this->session_ ? stage_text(this->session_->stage()) : "not started",
                YESNO(this->listen_only_), this->valve_queue_ ? "queue" : "start", YESNO(this->allow_writes_),
                YESNO(this->experimental_), static_cast<unsigned>(this->zones_.size()),
                static_cast<unsigned>(this->programs_.size()), static_cast<unsigned>(this->refresh_interval_ms_));
  if (this->clock_sync_) {
    ESP_LOGCONFIG(TAG, "  Clock sync: max drift %u ms (not built yet)", static_cast<unsigned>(this->max_drift_ms_));
  } else {
    ESP_LOGCONFIG(TAG, "  Clock sync: off");
  }
  LOG_UPDATE_INTERVAL(this);
#ifdef USE_TIME
  ESP_LOGCONFIG(TAG, "  Time: %s", YESNO(this->time_ != nullptr));
#endif
  if (this->monitor_receiver_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Monitor receiver: yes");
  }
  if (!this->accept_ids_.empty()) {
    ESP_LOGCONFIG(TAG, "  Extra model ids: %u", static_cast<unsigned>(this->accept_ids_.size()));
  }
  if (!this->listen_only_ && this->transmitter_ == nullptr) {
    ESP_LOGE(TAG, "No transmitter");
  }
  if (this->receiver_ == nullptr) {
    ESP_LOGE(TAG, "No receiver");
  }
}

}  // namespace esphome::rainbird
