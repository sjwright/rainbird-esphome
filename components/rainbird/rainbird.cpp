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
#include "elapsed.h"
#include "log_tag.h"
#include "rainbird_entities.h"
#include "sip_commands.h"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace esphome::rainbird {

ESPHOME_LOG_TAG(TAG, "rainbird");

namespace {

constexpr uint32_t COUNTDOWN_INTERVAL_ID = 1;
constexpr uint32_t SILENCE_INTERVAL_ID = 2;
constexpr uint32_t SILENCE_CHECK_MS = 5000;
constexpr uint32_t STATS_INTERVAL_ID = 3;
// Link counters in the log. Often enough to read during a soak, rarely enough to stay quiet.
constexpr uint32_t STATS_LOG_MS = 10 * 60 * 1000;
// A clock set the controller did not take is tried again after this long, not on every reading.
constexpr uint32_t CLOCK_SYNC_RETRY_MS = 60000;
// 39, 4B and 3A take whole minutes in one byte.
constexpr uint32_t MAX_RUN_MINUTES = 255;
// 4A timestamps count from 2000-01-01 (SPEC 5.3).
constexpr time_t SECONDS_1970_TO_2000 = 946684800;
// Room for a whole page as "AA.BB.CC…".
constexpr size_t PAGE_HEX_SIZE = format_hex_pretty_size(MAX_PAGE_BYTES);

int32_t rounded(float value) {
  if (value >= 0)
    return static_cast<int32_t>(value + 0.5f);
  return static_cast<int32_t>(value - 0.5f);
}

// A local minutes setting as the one byte 39, 4B and 3A take.
bool whole_minutes(float value, uint8_t *minutes) {
  if (std::isnan(value))
    return false;
  int32_t whole = rounded(value);
  if (whole < 1)
    whole = 1;
  if (whole > static_cast<int32_t>(MAX_RUN_MINUTES))
    whole = MAX_RUN_MINUTES;
  *minutes = static_cast<uint8_t>(whole);
  return true;
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

// Every registered entity's binding, writable and read-only alike.
template<typename F> void RainbirdComponent::for_each_binding_(F &&visit) const {
#ifdef USE_SWITCH
  for (auto *entity : this->switches_)
    visit(entity->binding());
#endif
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_)
    visit(entity->binding());
#endif
#ifdef USE_BUTTON
  for (auto *entity : this->buttons_)
    visit(entity->binding());
#endif
#ifdef USE_SELECT
  for (auto *entity : this->selects_)
    visit(entity->binding());
#endif
#ifdef USE_DATETIME_TIME
  for (auto *entity : this->times_)
    visit(entity->binding());
#endif
#ifdef USE_DATETIME_DATETIME
  for (auto *entity : this->datetimes_)
    visit(entity->binding());
#endif
#ifdef USE_SENSOR
  for (const auto &bound : this->sensors_)
    visit(bound.binding);
#endif
#ifdef USE_BINARY_SENSOR
  for (const auto &bound : this->binary_sensors_)
    visit(bound.binding);
#endif
#ifdef USE_TEXT_SENSOR
  for (const auto &bound : this->text_sensors_)
    visit(bound.binding);
#endif
}

void RainbirdComponent::setup() {
  SessionConfig config;
  config.listen_only = this->listen_only_;
  this->base_interval_ = this->get_update_interval();
  config.polling = this->base_interval_ != SCHEDULER_DONT_RUN;
  config.update_interval_ms = this->base_interval_;
  config.refresh_interval_ms = this->refresh_interval_ms_;
  config.allow_writes = this->allow_writes_;
  config.experimental = this->experimental_;
  config.valve_queue = this->valve_queue_;
  config.accept_ids = this->accept_ids_;
  // What the configured entities show decides what is polled and which schedule pages are read (PLAN 6.4).
  this->for_each_binding_([&](const EntityBinding &binding) { note_binding(this->model_, binding, &config); });
  if (this->clock_sync_)
    config.used.add(Field::CLOCK);
#ifdef USE_SENSOR
  for (const auto &bound : this->sensors_) {
    if (bound.binding.kind == EntityKind::REMAINING_TIME)
      this->remaining_sensors_.push_back(bound.entity);
  }
#endif

  this->text_.zone = [this](uint8_t zone) { return this->zone_name_(zone); };
  this->text_.program = [this](uint8_t program) { return this->program_name_(program); };
  this->text_.model = this->model_name_;
  this->text_.timestamp = [this](uint32_t seconds, std::string *out) { return this->local_timestamp_(seconds, out); };

  this->session_ = std::make_unique<Session>(this->model_, config);
  this->session_->start(millis());
  this->release_receiver_pads_();
  if (!this->listen_only_) {
    this->set_interval(STATS_INTERVAL_ID, STATS_LOG_MS, [this]() { this->log_link_stats_(); });
  }
  if (this->listen_only_) {
    // With `update_interval: never`, update() never runs, so silence has its own timer.
    this->set_interval(SILENCE_INTERVAL_ID, SILENCE_CHECK_MS, [this]() {
      this->session_->check_silence(millis());
      this->after_activity_();
    });
  }
  this->after_activity_();
}

#ifdef USE_NUMBER
void RainbirdComponent::add_setting(RainbirdSetting *setting) {
  const EntityBinding &binding = setting->binding();
  if (binding.kind == EntityKind::RUN_DURATION && binding.zone >= 1 && binding.zone <= STATE_ZONES)
    this->run_durations_[binding.zone - 1] = setting;
  if (binding.kind == EntityKind::TEST_DURATION)
    this->test_duration_ = setting;
}
#endif

#ifdef USE_DATETIME_TIME
void RainbirdComponent::add_time(RainbirdTime *entity) {
  this->times_.push_back(entity);
  const EntityBinding &binding = entity->binding();
  if (binding.program < STATE_PROGRAMS && binding.index < STATE_STARTS)
    this->start_times_[binding.program][binding.index] = entity;
}
#endif

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
  // A page write queued by the read that just finished is logged before it goes out.
  this->handle_events_();
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
  this->handle_events_();
  this->publish_changed_();
  this->check_clock_();

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

void RainbirdComponent::handle_events_() {
  SessionEvent event;
  while (this->session_->take_event(&event)) {
    switch (event.kind) {
      case SessionEventKind::STAGE:
        ESP_LOGD(TAG, "Link: %s", stage_text(event.stage));
        if (event.stage == SessionStage::RUNNING)
          this->log_controller_();
        break;
      case SessionEventKind::LINK_UP:
        ESP_LOGI(TAG, "Controller link up");
        this->status_clear_warning();
        break;
      case SessionEventKind::LINK_DOWN:
        ESP_LOGW(TAG, "Controller link down");
        this->status_set_warning();
        break;
      case SessionEventKind::NAK:
        // Bit 0 is "not supported", and the session has dropped that capability. Any other code
        // is more likely a request this component got wrong (SPEC 3.3, question 15).
        if (nak_is_unsupported(event.code)) {
          ESP_LOGI(TAG, "The controller does not support %02X", event.opcode);
        } else {
          ESP_LOGW(TAG, "The controller refused %02X with code %02X", event.opcode, event.code);
        }
        break;
      case SessionEventKind::PAGE_WRITE: {
        const PageEditLog &edit = this->session_->page_write();
        char before[PAGE_HEX_SIZE];
        char after[PAGE_HEX_SIZE];
        ESP_LOGI(TAG, "Schedule page %02X before: %s", edit.page, format_hex_pretty_to(before, edit.before, edit.size));
        ESP_LOGI(TAG, "Schedule page %02X write:  %s", edit.page, format_hex_pretty_to(after, edit.after, edit.size));
        break;
      }
      case SessionEventKind::EDIT_FAILED:
        ESP_LOGE(TAG, "Schedule edit failed: %s", event.reason);
        break;
    }
  }
}

void RainbirdComponent::log_controller_() {
  const Identity &identity = this->session_->identity();
  ESP_LOGI(TAG, "Controller %s, firmware %s", model_text(identity, this->model_name_).c_str(),
           firmware_text(identity).c_str());
  if (!this->session_->writes_permitted()) {
    ESP_LOGW(TAG, "Model id is not one %s accepts; commands stay off. See accept_model_ids.", this->model_name_);
  }
  // The 04 answers decide the poll plan and which commands are offered (PLAN 6.3).
  const Capabilities &caps = this->session_->capabilities();
  ESP_LOGI(TAG, "Controller supports: 4C %s, 4A %s, 42 %s, 4B %s, 3D %s, 49 %s, 3A %s%s", YESNO(caps.combined),
           YESNO(caps.event_timestamp), YESNO(caps.advance), YESNO(caps.queue_run), YESNO(caps.station_error),
           YESNO(caps.controller_state), YESNO(caps.test_stations),
           this->session_->capabilities_assumed() ? " (04 refused: assumed until a NAK says otherwise)" : "");
  // "00 10 60 80" fits the most a TM2 reads: 14 pages.
  char pages[3 * 16 + 1] = "";
  size_t used = 0;
  for (uint16_t page = 0; page <= 0xFF && used + 3 < sizeof(pages); page++) {
    if (this->session_->pages().has(static_cast<uint8_t>(page)))
      used += std::snprintf(pages + used, sizeof(pages) - used, "%s%02X", used == 0 ? "" : " ",
                            static_cast<unsigned>(page));
  }
  ESP_LOGI(TAG, "Schedule pages read: %s", used == 0 ? "none" : pages);
  // 03 00 at startup says which zones the controller has.
  for (const RainbirdZone *zone : this->zones_) {
    bool available = true;
    if (this->session_->state().zone_available(zone->get_zone(), &available) && !available) {
      ESP_LOGW(TAG, "Zone %u (%s) is not on this controller", static_cast<unsigned>(zone->get_zone()),
               this->zone_name_(zone->get_zone()).c_str());
    }
  }
}

void RainbirdComponent::log_link_stats_() {
  // Duplicates answer SPEC question 5: does the controller answer both copies of a 2-byte request?
  const LinkStatus &link = this->session_->link();
  ESP_LOGI(TAG, "Link since boot: %u replies, %u duplicate replies, %u unexpected, %u timeouts",
           static_cast<unsigned>(link.replies), static_cast<unsigned>(link.duplicates),
           static_cast<unsigned>(link.unexpected), static_cast<unsigned>(link.errors));
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
  // The triggers take a vector, so the copy is made only when an automation will use it.
  if (!this->frame_triggers_.empty()) {
    std::vector<uint8_t> payload(frame.payload, frame.payload + frame.length);
    for (FrameTrigger *trigger : this->frame_triggers_) {
      trigger->trigger(payload);
    }
  }
  Nak nak;
  if (!this->nak_triggers_.empty() && parse_nak(frame.payload, frame.length, &nak)) {
    for (NakTrigger *trigger : this->nak_triggers_) {
      trigger->trigger(nak.echo, nak.code);
    }
  }
  if (this->session_) {
    this->session_->on_frame(frame.payload, frame.length, millis());
    this->after_activity_();
  }
}

bool RainbirdComponent::submitted_(ControlResult result) {
  if (result != ControlResult::QUEUED) {
    ESP_LOGW(TAG, "Refused: %s", control_result_text(result));
    return false;
  }
  // loop() sends it. It sleeps while the link is idle, so wake it.
  this->enable_loop();
  return true;
}

bool RainbirdComponent::send_raw(const uint8_t *data, size_t size) {
  if (this->transmitter_ == nullptr || !this->session_) {
    ESP_LOGW(TAG, "send_raw needs a transmitter");
    return false;
  }
  // Queued on the link like any request, so it never collides with a poll in flight.
  return this->submitted_(this->session_->send_raw(data, size, millis()));
}

bool RainbirdComponent::run_minutes_(uint8_t zone, uint8_t *minutes) const {
#ifdef USE_NUMBER
  return zone >= 1 && zone <= STATE_ZONES && this->run_durations_[zone - 1] != nullptr &&
         whole_minutes(this->run_durations_[zone - 1]->state, minutes);
#else
  (void) zone;
  (void) minutes;
  return false;
#endif
}

bool RainbirdComponent::test_minutes_(uint8_t *minutes) const {
#ifdef USE_NUMBER
  return this->test_duration_ != nullptr && whole_minutes(this->test_duration_->state, minutes);
#else
  (void) minutes;
  return false;
#endif
}

bool RainbirdComponent::slot_minutes_(uint8_t program, uint8_t index, uint16_t *minutes) const {
#ifdef USE_DATETIME_TIME
  return program < STATE_PROGRAMS && index < STATE_STARTS && this->start_times_[program][index] != nullptr &&
         this->start_times_[program][index]->minutes(minutes);
#else
  (void) program;
  (void) index;
  (void) minutes;
  return false;
#endif
}

bool RainbirdComponent::start_slot_is_on(uint8_t program, uint8_t index) const {
  if (!this->session_)
    return false;
  bool on = false;
  return this->session_->state().start_enabled(program, index, &on) && on;
}

bool RainbirdComponent::queue_clock_(uint8_t hour, uint8_t minute, uint8_t second, uint8_t day, uint8_t month,
                                     uint16_t year) {
  Control clock;
  clock.op = ControlOp::SET_CLOCK;
  clock.hour = hour;
  clock.minute = minute;
  clock.second = second;
  clock.day = day;
  clock.month = month;
  clock.year = year;
  uint32_t now = millis();
  ControlResult result = this->session_->control(clock, now);
  if (result != ControlResult::QUEUED) {
    ESP_LOGW(TAG, "Refused clock set: %s", control_result_text(result));
    return false;
  }
  this->clock_set_queued_ = true;
  this->clock_set_at_ = now;
  return true;
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
  return this->submitted_(this->session_->control(command, millis()));
}

bool RainbirdComponent::set_controller_clock(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                             uint8_t second) {
  if (!this->session_ || !this->queue_clock_(hour, minute, second, day, month, year))
    return false;
  this->enable_loop();
  return true;
}

bool RainbirdComponent::sync_clock_() {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    ESPTime local = this->time_->now();
    if (local.is_valid())
      return this->set_controller_clock(local.year, local.month, local.day_of_month, local.hour, local.minute,
                                        local.second);
    ESP_LOGW(TAG, "Sync needs a valid clock");
    return false;
  }
#endif
  ESP_LOGW(TAG, "Sync needs time_id");
  return false;
}

bool RainbirdComponent::submit(const EntityBinding &binding, float value) {
  if (!this->session_) {
    ESP_LOGW(TAG, "Refused command: the link has not started");
    return false;
  }
  uint32_t now = millis();
  Control command;
  switch (binding.kind) {
    case EntityKind::VALVE:
    case EntityKind::QUEUE_ZONE: {
      // Whether a valve starts or queues its zone, and what turning it off does, is the session's
      // valve_queue setting.
      command.zone = binding.zone;
      if (binding.kind == EntityKind::VALVE && value == 0) {
        command.op = ControlOp::VALVE_OFF;
        break;
      }
      uint8_t minutes = 0;
      if (!this->run_minutes_(binding.zone, &minutes)) {
        ESP_LOGW(TAG, "Zone %u has no run duration", static_cast<unsigned>(binding.zone));
        return false;
      }
      command.op = binding.kind == EntityKind::VALVE ? ControlOp::VALVE_ON : ControlOp::QUEUE_ZONE;
      command.value = minutes;
      break;
    }
    case EntityKind::PROGRAM_START:
      command.op = ControlOp::RUN_PROGRAM;
      command.program = binding.program;
      break;
    case EntityKind::STOP:
      command.op = ControlOp::STOP;
      break;
    case EntityKind::ADVANCE:
      command.op = ControlOp::ADVANCE;
      break;
    case EntityKind::TEST_ALL: {
      uint8_t minutes = 0;
      if (!this->test_minutes_(&minutes)) {
        ESP_LOGW(TAG, "Test all zones needs a test duration");
        return false;
      }
      command.op = ControlOp::TEST_ZONES;
      command.value = minutes;
      break;
    }
    case EntityKind::SEASONAL_ADJUST:
      command.op = ControlOp::SET_SEASONAL;
      command.program = binding.program;
      command.value = static_cast<uint16_t>(rounded(value));
      break;
    case EntityKind::RAIN_DELAY:
      command.op = ControlOp::SET_RAIN_DELAY;
      command.value = static_cast<uint16_t>(rounded(value));
      break;
    case EntityKind::IRRIGATION_SET:
      command.op = ControlOp::SET_IRRIGATION;
      command.value = value != 0 ? 1 : 0;
      break;
    case EntityKind::REFRESH:
      return this->submitted_(this->session_->refresh(now));
    case EntityKind::SYNC_CLOCK:
      return this->sync_clock_();
    case EntityKind::START_ENABLED: {
      if (value == 0) {
        return this->submitted_(
            this->session_->edit_schedule(Field::START_ENABLED, NO_ZONE, binding.program, binding.index, 0, now));
      }
      // Turning a slot on writes the time its time entity shows. START_ENABLED=1 would invent one.
      uint16_t minutes = 0;
      if (!this->slot_minutes_(binding.program, binding.index, &minutes)) {
        ESP_LOGW(TAG, "That start slot has no time set");
        return false;
      }
      return this->submitted_(
          this->session_->edit_schedule(Field::START_TIME, NO_ZONE, binding.program, binding.index, minutes, now));
    }
    default: {
      const KindInfo &info = kind_info(binding.kind);
      if (info.write != WritePath::SCHEDULE_PAGE) {
        ESP_LOGW(TAG, "Refused command: kind %u is not writable", static_cast<unsigned>(binding.kind));
        return false;
      }
      return this->submitted_(
          this->session_->edit_schedule(info.field, binding.zone, binding.program, binding.index, rounded(value), now));
    }
  }
  return this->submitted_(this->session_->control(command, now));
}

void RainbirdComponent::check_clock_() {
#ifdef USE_TIME
  if (!this->clock_sync_ || this->time_ == nullptr)
    return;
  // Once per controller reading, and only while commands can reach the controller.
  uint32_t serial = this->session_->clock_serial();
  if (serial == 0 || serial == this->clock_checked_serial_)
    return;
  if (this->session_->stage() != SessionStage::RUNNING || !this->session_->writes_permitted() ||
      !this->session_->link().connected)
    return;
  this->clock_checked_serial_ = serial;
  uint32_t now = millis();
  if (this->clock_set_queued_) {
    if (!elapsed(now, this->clock_set_at_, CLOCK_SYNC_RETRY_MS))
      return;
    // Cleared once it runs out, so a set from weeks ago cannot hold off a sync when millis() wraps.
    this->clock_set_queued_ = false;
  }
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

bool RainbirdComponent::local_timestamp_(uint32_t seconds_since_2000, std::string *out) const {
#ifdef USE_TIME
  if (this->time_ == nullptr || out == nullptr) {
    return false;
  }
  // The controller stamps its own wall clock (SPEC 8.11). Read those fields as
  // local time in time_id's zone to get the instant, then publish it in UTC.
  ESPTime local = ESPTime::from_epoch_utc(SECONDS_1970_TO_2000 + static_cast<time_t>(seconds_since_2000));
  local.recalc_timestamp_local();
  time_t utc = local.timestamp;
  struct tm tm{};
  gmtime_r(&utc, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
  *out = buf;
  return true;
#else
  (void) seconds_since_2000;
  (void) out;
  return false;
#endif
}

void RainbirdComponent::publish_changed_() {
  // Most passes follow a frame or a loop with nothing new; skip walking every entity then.
  if (!this->session_->anything_changed())
    return;
  const Readout shown = readout(*this->session_);
#ifdef USE_SWITCH
  for (auto *entity : this->switches_) {
    bool value = false;
    if (kind_changed(entity->binding(), shown) && bool_value(entity->binding(), shown, &value))
      entity->publish_state(value);
  }
#endif
#ifdef USE_NUMBER
  for (auto *entity : this->numbers_) {
    float value = 0;
    if (kind_changed(entity->binding(), shown) && number_value(entity->binding(), shown, &value))
      entity->publish_state(value);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &bound : this->binary_sensors_) {
    bool value = false;
    if (kind_changed(bound.binding, shown) && bool_value(bound.binding, shown, &value))
      bound.entity->publish_state(value);
  }
#endif
#ifdef USE_SENSOR
  for (auto &bound : this->sensors_) {
    float value = 0;
    // Remaining time has its own countdown rules (publish_remaining_).
    if (bound.binding.kind != EntityKind::REMAINING_TIME && kind_changed(bound.binding, shown) &&
        number_value(bound.binding, shown, &value))
      bound.entity->publish_state(value);
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto &bound : this->text_sensors_) {
    std::string text;
    if (kind_changed(bound.binding, shown) && text_value(bound.binding, shown, this->text_, &text))
      bound.entity->publish_state(text);
  }
#endif
#ifdef USE_SELECT
  for (auto *entity : this->selects_) {
    uint8_t index = 0;
    if (kind_changed(entity->binding(), shown) && select_index(entity->binding(), shown, &index))
      entity->publish_state(index);
  }
#endif
#ifdef USE_DATETIME_TIME
  for (auto *entity : this->times_) {
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (kind_changed(entity->binding(), shown) && start_time(entity->binding(), shown, &hour, &minute))
      entity->show(hour, minute);
  }
#endif
#ifdef USE_DATETIME_DATETIME
  for (auto *entity : this->datetimes_) {
    WallClock clock;
    if (kind_changed(entity->binding(), shown) && controller_clock(shown, &clock))
      entity->show(clock.year, clock.month, clock.day, clock.hour, clock.minute, clock.second);
  }
#endif
  this->publish_remaining_(shown.state);
  this->session_->clear_changed();
}

void RainbirdComponent::publish_remaining_(const ControllerState &state) {
#ifdef USE_SENSOR
  uint16_t seconds = 0;
  if (this->remaining_sensors_.empty() || !state.remaining_refreshed() || !state.remaining_seconds(&seconds))
    return;
  if (!this->countdown_.reading(seconds, millis()))
    return;
  this->publish_remaining_sensors_(seconds);
  if (seconds > 0) {
    // Count down locally between polls. Restarting the interval lines its ticks up with this reading.
    this->set_interval(COUNTDOWN_INTERVAL_ID, 1000, [this]() { this->tick_countdown_(); });
  } else {
    this->cancel_interval(COUNTDOWN_INTERVAL_ID);
  }
#else
  (void) state;
#endif
}

void RainbirdComponent::tick_countdown_() {
  uint32_t remaining = this->countdown_.remaining(millis());
  this->publish_remaining_sensors_(remaining);
  if (remaining == 0)
    this->cancel_interval(COUNTDOWN_INTERVAL_ID);
}

void RainbirdComponent::publish_remaining_sensors_(uint32_t seconds) {
#ifdef USE_SENSOR
  for (sensor::Sensor *entity : this->remaining_sensors_)
    entity->publish_state(seconds);
#else
  (void) seconds;
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
