#include "rainbird.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#endif

#include "clock_sync.h"
#include "entity_values.h"
#include "log_tag.h"
#include "rainbird_entities.h"
#include "sip_commands.h"

#include <cmath>
#include <ctime>

namespace esphome::rainbird {

ESPHOME_LOG_TAG(TAG, "rainbird");

namespace {

constexpr uint32_t COUNTDOWN_INTERVAL_ID = 1;
constexpr uint32_t SILENCE_INTERVAL_ID = 2;
constexpr uint32_t SILENCE_CHECK_MS = 5000;
// A clock set the controller did not take is tried again after this long, not on every reading.
constexpr uint32_t CLOCK_SYNC_RETRY_MS = 60000;
// 39 and 4B take whole minutes in one byte.
constexpr uint32_t MAX_RUN_MINUTES = 255;
// 4A timestamps count from 2000-01-01 (SPEC 5.3).
constexpr time_t SECONDS_1970_TO_2000 = 946684800;

int32_t rounded(float value) {
  if (value >= 0)
    return static_cast<int32_t>(value + 0.5f);
  return static_cast<int32_t>(value - 0.5f);
}

bool schedule_field(EntityKind kind, Field *field) {
  if (field == nullptr)
    return false;
  switch (kind) {
    case EntityKind::RUN_TIME:
      *field = Field::RUN_TIME;
      return true;
    case EntityKind::WATERING_DAY:
      *field = Field::WATERING_DAY;
      return true;
    case EntityKind::FREQUENCY:
      *field = Field::FREQUENCY;
      return true;
    case EntityKind::CYCLE_DAYS:
      *field = Field::CYCLE_DAYS;
      return true;
    case EntityKind::DAYS_UNTIL_NEXT:
      *field = Field::DAYS_UNTIL_NEXT;
      return true;
    case EntityKind::START_TIME:
      *field = Field::START_TIME;
      return true;
    case EntityKind::STATION_DELAY:
      *field = Field::STATION_DELAY;
      return true;
    case EntityKind::GLOBAL_BYPASS:
      *field = Field::GLOBAL_SENSOR_BYPASS;
      return true;
    case EntityKind::ZONE_BYPASS:
      *field = Field::ZONE_SENSOR_BYPASS;
      return true;
    default:
      return false;
  }
}

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
  config.allow_writes = this->allow_writes_;
  config.accept_ids = this->accept_ids_;
  // The fields configured entities read decide what is polled (PLAN 6.4).
  // Schedule masks stay 0 until an entity needs a page, and 0 means every zone or program.
#ifdef USE_SWITCH
  for (auto *entity : this->switches_)
    this->note_binding_(entity->kind(), entity->zone(), entity->program(), &config);
#endif
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_)
    this->note_binding_(entity->kind(), entity->zone(), entity->program(), &config);
#endif
#ifdef USE_BUTTON
  for (auto *entity : this->buttons_)
    this->note_binding_(entity->kind(), entity->zone(), entity->program(), &config);
#endif
#ifdef USE_SELECT
  for (auto *entity : this->selects_)
    this->note_binding_(entity->kind(), NO_ZONE, entity->program(), &config);
#endif
#ifdef USE_DATETIME_TIME
  for (auto *entity : this->times_)
    this->note_binding_(entity->kind(), NO_ZONE, entity->program(), &config);
#endif
#ifdef USE_DATETIME_DATETIME
  for (auto *entity : this->datetimes_)
    this->note_binding_(entity->kind(), NO_ZONE, NO_PROGRAM, &config);
#endif
#ifdef USE_SENSOR
  for (auto &binding : this->sensors_)
    this->note_binding_(binding.kind, binding.zone, binding.program, &config);
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &binding : this->binary_sensors_)
    this->note_binding_(binding.kind, binding.zone, binding.program, &config);
#endif
#ifdef USE_TEXT_SENSOR
  for (auto &binding : this->text_sensors_)
    this->note_binding_(binding.kind, binding.zone, binding.program, &config);
#endif
  if (this->clock_sync_)
    config.used.add(Field::CLOCK);
  this->session_ = std::make_unique<Session>(this->model_, config);
  this->session_->start(millis());
  this->last_stage_ = this->session_->stage();
  this->release_receiver_pads_();
  if (this->listen_only_) {
    // With `update_interval: never`, update() never runs, so silence has its own timer.
    this->set_interval(SILENCE_INTERVAL_ID, SILENCE_CHECK_MS, [this]() {
      this->session_->check_silence(millis());
      this->after_activity_();
    });
  }
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
  // The page write is queued by the read that just finished. Log it before it goes out.
  this->log_page_edit_();
  Transmission tx;
  if (this->session_->next_transmission(now, &tx))
    this->transmit_(tx);
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

  const char *reason = nullptr;
  if (this->session_->take_edit_failure(&reason) && reason != nullptr)
    ESP_LOGE(TAG, "Schedule edit failed: %s", reason);

  // A clock command queued here waits for the next loop. Calling loop() from here would recurse.
  this->check_clock_();

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
  if (!controller) {
    // The monitor hears another master's requests. They are logged for protocol work and go
    // nowhere else: on_frame and on_nak report the controller's replies (PLAN 4.5).
    ESP_LOGD(TAG, "%s %s", which, format_hex_pretty_to(pretty, frame.payload, frame.length));
    return;
  }
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
  if (this->session_) {
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

void RainbirdComponent::note_binding_(EntityKind kind, uint8_t zone, uint8_t program, SessionConfig *config) {
  add_fields(kind, &config->used);
  // One zone's run time must not pull every zone's page. Other schedule pages are one per program.
  if (kind == EntityKind::RUN_TIME && zone >= 1 && zone <= 16)
    config->zone_mask |= static_cast<uint16_t>(1u << (zone - 1));
  // In queue mode, turning a valve off moves on to the next queued zone, which the queue list names.
  if (kind == EntityKind::VALVE && this->valve_queue_ && this->experimental_)
    config->used.add(Field::QUEUE);
  if (program != NO_PROGRAM && program < 8 &&
      (kind == EntityKind::WATERING_DAY || kind == EntityKind::FREQUENCY || kind == EntityKind::CYCLE_DAYS ||
       kind == EntityKind::DAYS_UNTIL_NEXT || kind == EntityKind::START_TIME || kind == EntityKind::START_ENABLED)) {
    config->program_mask |= static_cast<uint8_t>(1u << program);
  }
}

bool RainbirdComponent::zone_run_minutes_(uint8_t zone, uint8_t *minutes) const {
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_) {
    if (entity->kind() != EntityKind::RUN_DURATION || entity->zone() != zone || std::isnan(entity->state))
      continue;
    int32_t value = rounded(entity->state);
    if (value < 1)
      value = 1;
    if (value > 255)
      value = 255;
    *minutes = static_cast<uint8_t>(value);
    return true;
  }
#else
  (void) zone;
#endif
  (void) minutes;
  return false;
}

bool RainbirdComponent::test_minutes_(uint8_t *minutes) const {
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_) {
    if (entity->kind() != EntityKind::TEST_DURATION || std::isnan(entity->state))
      continue;
    int32_t value = rounded(entity->state);
    if (value < 1)
      value = 1;
    if (value > 255)
      value = 255;
    *minutes = static_cast<uint8_t>(value);
    return true;
  }
#endif
  (void) minutes;
  return false;
}

bool RainbirdComponent::start_minutes_(uint8_t program, uint8_t index, uint16_t *minutes) const {
#ifdef USE_DATETIME_TIME
  for (auto *entity : this->times_) {
    if (entity->kind() == EntityKind::START_TIME && entity->program() == program && entity->index() == index)
      return entity->minutes(minutes);
  }
#else
  (void) program;
  (void) index;
#endif
  (void) minutes;
  return false;
}

bool RainbirdComponent::start_slot_is_on(uint8_t program, uint8_t index) const {
  if (!this->session_)
    return false;
  bool on = false;
  return this->session_->state().start_enabled(program, index, &on) && on;
}

ControlResult RainbirdComponent::queue_control_(const Control &command) {
  ControlResult result = this->session_->control(command, millis());
  if (result != ControlResult::QUEUED)
    ESP_LOGW(TAG, "Refused command: %s", control_result_text(result));
  return result;
}

bool RainbirdComponent::queue_clock_(uint8_t hour, uint8_t minute, uint8_t second, uint8_t day, uint8_t month,
                                     uint16_t year) {
  if (!this->session_)
    return false;
  uint32_t now = millis();
  Control time;
  time.op = ControlOp::SET_TIME;
  time.hour = hour;
  time.minute = minute;
  time.second = second;
  Control date;
  date.op = ControlOp::SET_DATE;
  date.day = day;
  date.month = month;
  date.year = year;
  ControlResult time_result = this->session_->control(time, now);
  ControlResult date_result = this->session_->control(date, now);
  if (time_result != ControlResult::QUEUED)
    ESP_LOGW(TAG, "Refused time set: %s", control_result_text(time_result));
  if (date_result != ControlResult::QUEUED)
    ESP_LOGW(TAG, "Refused date set: %s", control_result_text(date_result));
  if (time_result == ControlResult::QUEUED || date_result == ControlResult::QUEUED) {
    this->clock_synced_ = true;
    this->clock_sync_at_ = now;
  }
  return time_result == ControlResult::QUEUED && date_result == ControlResult::QUEUED;
}

bool RainbirdComponent::run_zone(int32_t zone, uint32_t duration_ms, bool queue) {
  if (!this->session_) {
    ESP_LOGW(TAG, "Refused command: the link has not started");
    return false;
  }
  // Refused, not clamped. A negative minutes value in a template wraps to a huge duration,
  // and clamping that would water for over four hours.
  if (zone < 1 || zone > 255) {
    ESP_LOGW(TAG, "Refused zone run: %d is not a zone number", static_cast<int>(zone));
    return false;
  }
  if (duration_ms == 0 || duration_ms > MAX_RUN_MINUTES * 60000u) {
    ESP_LOGW(TAG, "Refused zone run: %u ms is outside 1 ms to %u minutes", static_cast<unsigned>(duration_ms),
             static_cast<unsigned>(MAX_RUN_MINUTES));
    return false;
  }
  Control command;
  command.op = queue ? ControlOp::QUEUE_ZONE : ControlOp::RUN_ZONE;
  command.zone = static_cast<uint8_t>(zone);
  // Whole minutes, so a part minute rounds up.
  command.value = static_cast<uint16_t>((duration_ms + 59999u) / 60000u);
  if (this->queue_control_(command) != ControlResult::QUEUED)
    return false;
  this->loop();
  return true;
}

bool RainbirdComponent::set_controller_clock(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                             uint8_t second) {
  if (!this->queue_clock_(hour, minute, second, day, month, year))
    return false;
  this->loop();
  return true;
}

bool RainbirdComponent::submit(const WriteIntent &intent) {
  if (!this->session_) {
    ESP_LOGW(TAG, "Refused command: the link has not started");
    return false;
  }
  bool queued = false;
  switch (intent.kind) {
    case EntityKind::VALVE: {
      Control command;
      command.zone = intent.zone;
      command.queue_mode = this->valve_queue_;
      command.experimental = this->experimental_;
      if (intent.value == 0) {
        command.op = ControlOp::STOP_ZONE;
      } else {
        uint8_t minutes = 0;
        if (!this->zone_run_minutes_(intent.zone, &minutes)) {
          ESP_LOGW(TAG, "Zone %u has no run duration", static_cast<unsigned>(intent.zone));
          return false;
        }
        command.op = this->valve_queue_ ? ControlOp::QUEUE_ZONE : ControlOp::RUN_ZONE;
        command.value = minutes;
      }
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::QUEUE_ZONE: {
      uint8_t minutes = 0;
      if (!this->zone_run_minutes_(intent.zone, &minutes)) {
        ESP_LOGW(TAG, "Zone %u has no run duration", static_cast<unsigned>(intent.zone));
        return false;
      }
      Control command;
      command.op = ControlOp::QUEUE_ZONE;
      command.zone = intent.zone;
      command.value = minutes;
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::PROGRAM_START: {
      Control command;
      command.op = ControlOp::RUN_PROGRAM;
      command.program = intent.program;
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::STOP: {
      Control command;
      command.op = ControlOp::STOP;
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::ADVANCE: {
      Control command;
      command.op = ControlOp::ADVANCE;
      command.experimental = this->experimental_;
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::TEST_ALL: {
      uint8_t minutes = 0;
      if (!this->test_minutes_(&minutes)) {
        ESP_LOGW(TAG, "Test all zones needs a test duration");
        return false;
      }
      Control command;
      command.op = ControlOp::TEST_ZONES;
      command.value = minutes;
      command.experimental = this->experimental_;
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::REFRESH: {
      Control command;
      command.op = ControlOp::REFRESH;
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::SEASONAL_ADJUST: {
      Control command;
      command.op = ControlOp::SET_SEASONAL;
      command.program = intent.program;
      command.value = static_cast<uint16_t>(rounded(intent.value));
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::RAIN_DELAY: {
      Control command;
      command.op = ControlOp::SET_RAIN_DELAY;
      command.value = static_cast<uint16_t>(rounded(intent.value));
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::IRRIGATION_SET: {
      Control command;
      command.op = ControlOp::SET_IRRIGATION;
      command.value = intent.value != 0 ? 1 : 0;
      command.experimental = this->experimental_;
      queued = this->queue_control_(command) == ControlResult::QUEUED;
      break;
    }
    case EntityKind::SYNC_CLOCK: {
#ifdef USE_TIME
      if (this->time_ == nullptr) {
        ESP_LOGW(TAG, "Sync needs time_id");
        return false;
      }
      ESPTime local = this->time_->now();
      if (!local.is_valid()) {
        ESP_LOGW(TAG, "Sync needs a valid clock");
        return false;
      }
      queued = this->queue_clock_(local.hour, local.minute, local.second, local.day_of_month, local.month, local.year);
#else
      ESP_LOGW(TAG, "Sync needs time_id");
      return false;
#endif
      break;
    }
    case EntityKind::START_ENABLED: {
      ControlResult result;
      if (intent.value == 0) {
        result =
            this->session_->edit_schedule(Field::START_ENABLED, NO_ZONE, intent.program, intent.index, 0, millis());
      } else {
        // Turning a slot on writes the time entity's minutes. START_ENABLED=1 would invent a time.
        uint16_t minutes = 0;
        if (!this->start_minutes_(intent.program, intent.index, &minutes)) {
          ESP_LOGW(TAG, "That start slot has no time set");
          return false;
        }
        result =
            this->session_->edit_schedule(Field::START_TIME, NO_ZONE, intent.program, intent.index, minutes, millis());
      }
      queued = result == ControlResult::QUEUED;
      if (!queued)
        ESP_LOGW(TAG, "Refused schedule edit: %s", control_result_text(result));
      break;
    }
    default: {
      Field field;
      if (!schedule_field(intent.kind, &field)) {
        ESP_LOGW(TAG, "Refused command: kind %u is not writable", static_cast<unsigned>(intent.kind));
        return false;
      }
      ControlResult result = this->session_->edit_schedule(field, intent.zone, intent.program, intent.index,
                                                           rounded(intent.value), millis());
      queued = result == ControlResult::QUEUED;
      if (!queued)
        ESP_LOGW(TAG, "Refused schedule edit: %s", control_result_text(result));
      break;
    }
  }
  if (!queued)
    return false;
  // The action runs outside the component phase, and loop() is off while the link is idle.
  this->loop();
  return true;
}

void RainbirdComponent::log_page_edit_() {
  PageEditLog edit;
  if (!this->session_->take_page_log(&edit))
    return;
  char before[160];
  char after[160];
  ESP_LOGI(TAG, "Schedule page %02X before: %s", edit.page, format_hex_pretty_to(before, edit.before, edit.size));
  ESP_LOGI(TAG, "Schedule page %02X write:  %s", edit.page, format_hex_pretty_to(after, edit.after, edit.size));
}

void RainbirdComponent::check_clock_() {
#ifdef USE_TIME
  if (!this->clock_sync_ || this->time_ == nullptr || !this->session_)
    return;
  // Once per controller reading, and only while commands can reach the controller.
  uint32_t serial = this->session_->clock_serial();
  if (serial == 0 || serial == this->clock_checked_serial_)
    return;
  if (this->session_->stage() != SessionStage::RUNNING || !this->session_->writes_permitted() ||
      !this->session_->connected())
    return;
  this->clock_checked_serial_ = serial;
  uint32_t now = millis();
  if (this->clock_synced_ && now - this->clock_sync_at_ < CLOCK_SYNC_RETRY_MS)
    return;
  ESPTime local = this->time_->now();
  if (!local.is_valid())
    return;
  const ControllerState &state = this->session_->state();
  WallClock controller;
  if (!state.clock(&controller.hour, &controller.minute, &controller.second))
    return;
  bool date_valid = state.date(&controller.day, &controller.month, &controller.year);
  WallClock here{local.year, local.month, local.day_of_month, local.hour, local.minute, local.second};
  // A UTC-offset change moves local time by an hour, so daylight saving shows up as drift here.
  // WAIT: local midnight is close; the next reading checks again.
  if (check_clock(controller, date_valid, now - this->session_->clock_read_at(), here, this->max_drift_ms_) !=
      ClockCheck::SYNC)
    return;
  ESP_LOGI(TAG, "Setting the controller clock to %04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned>(here.year),
           static_cast<unsigned>(here.month), static_cast<unsigned>(here.day), static_cast<unsigned>(here.hour),
           static_cast<unsigned>(here.minute), static_cast<unsigned>(here.second));
  this->queue_clock_(here.hour, here.minute, here.second, here.day, here.month, here.year);
#endif
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
    if (kind_changed(entity->kind(), state, entity->zone(), entity->program(), entity->index()) &&
        bool_value(entity->kind(), state, entity->zone(), entity->program(), &value, entity->index())) {
      entity->publish_state(value);
    }
  }
#endif
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_) {
    float value = 0;
    if (entity->is_command() &&
        kind_changed(entity->kind(), state, entity->zone(), entity->program(), entity->index()) &&
        number_value(entity->kind(), state, entity->zone(), entity->program(), &value, entity->index())) {
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
    if (kind_changed(binding.kind, state, binding.zone, binding.program, binding.index) &&
        bool_value(binding.kind, state, binding.zone, binding.program, &value, binding.index)) {
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
        // Every reading re-anchors the countdown, not only a new value: a paused controller repeats
        // the same value, and counting on from the old anchor would run down to 0. A steady 0 is
        // not published again.
        if (state.remaining_refreshed() &&
            number_value(binding.kind, state, binding.zone, binding.program, &value, binding.index) &&
            (value > 0 || kind_changed(binding.kind, state, binding.zone, binding.program, binding.index))) {
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
        if (kind_changed(binding.kind, state, binding.zone, binding.program, binding.index) &&
            number_value(binding.kind, state, binding.zone, binding.program, &value, binding.index)) {
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
        publish = kind_changed(binding.kind, state, binding.zone, binding.program, binding.index) &&
                  active_program_text(state, names, &text);
        break;
      case EntityKind::QUEUE:
        publish = kind_changed(binding.kind, state, binding.zone, binding.program, binding.index) &&
                  queue_text(state, names, &text);
        break;
      case EntityKind::SCHEDULE_CHANGED:
        publish = kind_changed(binding.kind, state, binding.zone, binding.program, binding.index) &&
                  this->schedule_changed_text_(&text);
        break;
      default:
        break;
    }
    if (publish) {
      binding.entity->publish_state(text);
    }
  }
#endif
#ifdef USE_SELECT
  for (auto *entity : this->selects_) {
    uint8_t index = 0;
    if (kind_changed(entity->kind(), state, NO_ZONE, entity->program()) &&
        select_index(entity->kind(), state, NO_ZONE, entity->program(), &index)) {
      entity->publish_state(index);
    }
  }
#endif
#ifdef USE_DATETIME_TIME
  for (auto *entity : this->times_) {
    int32_t minutes = 0;
    if (!kind_changed(entity->kind(), state, NO_ZONE, entity->program(), entity->index()) ||
        !start_slot_minutes(state, entity->program(), entity->index(), &minutes) || minutes < 0 ||
        minutes > 23 * 60 + 59)
      continue;
    entity->show(static_cast<uint8_t>(minutes / 60), static_cast<uint8_t>(minutes % 60));
  }
#endif
#ifdef USE_DATETIME_DATETIME
  for (auto *entity : this->datetimes_) {
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t day = 0;
    uint8_t month = 0;
    uint16_t year = 0;
    if (kind_changed(entity->kind(), state, NO_ZONE, NO_PROGRAM) && state.clock(&hour, &minute, &second) &&
        state.date(&day, &month, &year)) {
      entity->show(year, month, day, hour, minute, second);
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
                "  Commands: %s\n"
                "  Listen only: %s\n"
                "  Valve mode: %s\n"
                "  Schedule writes: %s\n"
                "  Experimental: %s\n"
                "  Zones: %u\n"
                "  Programs: %u\n"
                "  Schedule refresh: %u ms",
                this->model_name_, this->session_ ? stage_text(this->session_->stage()) : "not started",
                this->listen_only_ ? "off (listen only)" : "on", YESNO(this->listen_only_),
                this->valve_queue_ ? "queue" : "start", YESNO(this->allow_writes_), YESNO(this->experimental_),
                static_cast<unsigned>(this->zones_.size()), static_cast<unsigned>(this->programs_.size()),
                static_cast<unsigned>(this->refresh_interval_ms_));
  if (this->clock_sync_) {
    ESP_LOGCONFIG(TAG, "  Clock sync: max drift %u ms", static_cast<unsigned>(this->max_drift_ms_));
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
