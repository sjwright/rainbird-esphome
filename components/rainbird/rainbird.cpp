#include "rainbird.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#endif

#include "automation.h"
#include "clock_sync.h"
#include "elapsed.h"
#include "entity_writes.h"
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
// 4A timestamps count from 2000-01-01 (SPEC 5.3).
constexpr time_t SECONDS_1970_TO_2000 = 946684800;
// Room for a whole page as "AA.BB.CC…".
constexpr size_t PAGE_HEX_SIZE = format_hex_pretty_size(MAX_PAGE_BYTES);

const char *stage_text(SessionStage stage) {
  switch (stage) {
    case SessionStage::PASSIVE:
      return "listening";
    case SessionStage::MANUAL:
      return "manual (update_interval: never)";
    case SessionStage::IDLE:
      return "waiting for the controller";
    case SessionStage::HANDSHAKE:
      return "handshaking";
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
  config.experimental = this->experimental_;
  config.handshake = this->handshake_;
  config.random = random_uint32;
  config.valve_queue = this->valve_queue_;
  config.accept_ids = this->accept_ids_;
  // What the configured entities show decides what is polled and which schedule pages are read (PLAN 6.4).
  this->board_.for_each_binding([&](const EntityBinding &binding) { note_binding(this->model_, binding, &config); });
  if (this->clock_sync_)
    config.used.add(Field::CLOCK);
#ifdef USE_SENSOR
  this->board_.for_each(Domain::SENSOR, EntityKind::REMAINING_TIME, [this](void *entity) {
    this->remaining_sensors_.push_back(static_cast<sensor::Sensor *>(entity));
  });
#endif
#ifdef USE_TEXT_SENSOR
  this->board_.for_each(Domain::TEXT_SENSOR, EntityKind::ENDS_AT, [this](void *entity) {
    this->ends_at_sensors_.push_back(static_cast<text_sensor::TextSensor *>(entity));
  });
#endif

  this->text_.zone = [this](uint8_t zone) { return this->zone_name_(zone); };
  this->text_.program = [this](uint8_t program) { return this->program_name_(program); };
  this->text_.model = this->model_name_;
  this->text_.timestamp = [this](uint32_t seconds, std::string *out) { return this->local_timestamp_(seconds, out); };

  this->writes_.run_duration = [this](uint8_t zone, float *minutes) { return this->run_minutes_(zone, minutes); };
  this->writes_.test_duration = [this](float *minutes) { return this->test_minutes_(minutes); };
  this->writes_.slot_time = [this](uint8_t program, uint8_t index, uint16_t *minutes) {
    return this->slot_minutes_(program, index, minutes);
  };

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

#ifdef USE_SWITCH
void RainbirdComponent::add_switch(RainbirdSwitch *entity) {
  this->board_.add(Domain::SWITCH, entity->binding(), entity);
}
#endif

#ifdef USE_NUMBER
void RainbirdComponent::add_number(RainbirdNumber *entity) {
  this->board_.add(Domain::NUMBER, entity->binding(), entity);
}

void RainbirdComponent::add_setting(RainbirdSetting *setting) {
  const EntityBinding &binding = setting->binding();
  if (binding.kind == EntityKind::RUN_DURATION && binding.zone >= 1 && binding.zone <= STATE_ZONES)
    this->run_durations_[binding.zone - 1] = setting;
  if (binding.kind == EntityKind::TEST_DURATION)
    this->test_duration_ = setting;
}
#endif

#ifdef USE_BUTTON
void RainbirdComponent::add_button(RainbirdButton *entity) {
  this->board_.add(Domain::BUTTON, entity->binding(), entity);
}
#endif

#ifdef USE_SELECT
void RainbirdComponent::add_select(RainbirdSelect *entity) {
  this->board_.add(Domain::SELECT, entity->binding(), entity);
}
#endif

#ifdef USE_DATETIME_TIME
void RainbirdComponent::add_time(RainbirdTime *entity) {
  const EntityBinding &binding = entity->binding();
  this->board_.add(Domain::TIME, binding, entity);
  if (binding.program < STATE_PROGRAMS && binding.index < STATE_STARTS)
    this->start_times_[binding.program][binding.index] = entity;
}
#endif

#ifdef USE_DATETIME_DATETIME
void RainbirdComponent::add_datetime(RainbirdDateTime *entity) {
  this->board_.add(Domain::DATETIME, entity->binding(), entity);
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
  this->publish_();
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
        this->log_heard_();
        this->status_set_warning();
        this->show_unknown_();
        break;
      case SessionEventKind::NAK:
        // Bit 0 is "not supported", and the session has dropped that capability. Bit 2 is a value
        // out of range, which a real 39 NAK has shown. Any other code is more likely a request this
        // component got wrong (SPEC 3.3, question 15).
        if (nak_is_unsupported(event.code)) {
          ESP_LOGI(TAG, "The controller does not support %02X", event.opcode);
        } else if (event.code == NAK_OUT_OF_RANGE) {
          ESP_LOGW(TAG, "The controller refused %02X with code 04: a value is out of range", event.opcode);
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
      case SessionEventKind::NO_REPLY:
        ESP_LOGW(TAG,
                 "No reply to %02X. It was sent once and is not sent again: the controller may have acted on it. "
                 "The next poll shows what it did.",
                 event.opcode);
        for (NoReplyTrigger *trigger : this->no_reply_triggers_) {
          trigger->trigger(event.opcode);
        }
        break;
      case SessionEventKind::HANDSHAKE:
        if (event.code != 0) {
          ESP_LOGW(TAG, "Handshake: %s", event.reason);
        } else {
          ESP_LOGI(TAG, "Handshake: %s", event.reason);
        }
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
  // "4C unknown, " is the longest entry.
  char supports[CAPABILITY_COUNT * 12 + 1] = "";
  size_t length = 0;
  for (uint8_t i = 0; i < CAPABILITY_COUNT && length < sizeof(supports); i++) {
    auto capability = static_cast<Capability>(i);
    length += std::snprintf(supports + length, sizeof(supports) - length, "%s%02X %s", i == 0 ? "" : ", ",
                            CAPABILITY_OPCODES[i], !caps.known(capability) ? "unknown" : YESNO(caps.has(capability)));
  }
  ESP_LOGI(TAG, "Controller supports: %s%s", supports,
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
    if (!at(this->session_->state().available, zone->get_zone() - 1).value_or(true)) {
      ESP_LOGW(TAG, "Zone %u (%s) is not on this controller", static_cast<unsigned>(zone->get_zone()),
               this->zone_name_(zone->get_zone()).c_str());
    }
  }
}

void RainbirdComponent::log_heard_() {
  // Tells a dead wire from a signal that does not decode, which are different faults to chase.
  if (this->bursts_heard_ == 0) {
    ESP_LOGW(TAG, "  The receiver has heard nothing since boot. Check the wiring and the receiver pin.");
  } else if (this->bursts_decoded_ == 0) {
    ESP_LOGW(TAG, "  The receiver has heard %u bursts and none held a valid frame. Check idle, filter and the pin.",
             static_cast<unsigned>(this->bursts_heard_));
  } else {
    ESP_LOGW(TAG, "  The receiver has heard %u bursts since boot, %u with a valid frame.",
             static_cast<unsigned>(this->bursts_heard_), static_cast<unsigned>(this->bursts_decoded_));
  }
}

void RainbirdComponent::log_link_stats_() {
  // Duplicates answer SPEC question 5: does the controller answer both copies of a 2-byte request?
  const LinkStatus &link = this->session_->link();
  ESP_LOGI(TAG, "Link since boot: %u replies, %u duplicate replies, %u unexpected, %u timeouts",
           static_cast<unsigned>(link.replies), static_cast<unsigned>(link.duplicates),
           static_cast<unsigned>(link.unexpected), static_cast<unsigned>(link.errors));
  ESP_LOGI(TAG, "Receiver since boot: %u bursts, %u with a valid frame", static_cast<unsigned>(this->bursts_heard_),
           static_cast<unsigned>(this->bursts_decoded_));
}

void RainbirdComponent::transmit_(const Transmission &tx) {
  if (this->transmitter_ == nullptr) {
    return;
  }
  char pretty[format_hex_pretty_size(MAX_PAYLOAD)];
  ESP_LOGD(TAG, "Send %s", format_hex_pretty_to(pretty, tx.payload.data, tx.payload.size));
  this->note_escapes_(tx.payload, true);
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
  if (this->controller_) {
    this->parent_->bursts_heard_++;
    if (!decoded.frames.empty())
      this->parent_->bursts_decoded_++;
  }
  if (decoded.frames.empty()) {
    return false;
  }
  for (const Frame &frame : decoded.frames) {
    this->parent_->deliver_frame_(frame, this->name_, this->controller_);
  }
  return true;
}

void RainbirdComponent::note_escapes_(const ByteBuf &payload, bool sent) {
  // The length and checksum are escaped too, so recompute what encode_frame escaped.
  uint8_t used = 0;
  auto note = [&used](uint8_t byte) {
    if (frame_needs_escape(byte))
      used |= frame_escape_bit(byte);
  };
  note(payload.size);
  for (uint8_t i = 0; i < payload.size; i++)
    note(payload.data[i]);
  note(frame_checksum(payload.size, payload.data));
  this->note_escapes_(used, sent);
}

void RainbirdComponent::note_escapes_(uint8_t escapes, bool sent) {
  // Logged once per byte and direction: only 1D -> 1B 03 in a reply has been seen (SPEC 2.4).
  // The first 1B or 1C either way settles the rest of the table.
  uint8_t &logged = sent ? this->escapes_sent_logged_ : this->escapes_received_logged_;
  uint8_t fresh = escapes & static_cast<uint8_t>(~logged);
  for (uint8_t byte = FRAME_ESCAPE; byte <= FRAME_END; byte++) {
    if ((fresh & frame_escape_bit(byte)) == 0)
      continue;
    if (sent) {
      ESP_LOGI(TAG,
               "Sending %02X escaped as 1B %02X for the first time. A NAK or no answer to this request "
               "would mean requests are not escaped.",
               byte, frame_escape_code(byte));
    } else {
      ESP_LOGI(TAG, "The controller escaped %02X as 1B %02X (first time seen)", byte, frame_escape_code(byte));
    }
  }
  logged |= fresh;
}

void RainbirdComponent::deliver_frame_(const Frame &frame, const char *which, bool controller) {
  char pretty[format_hex_pretty_size(MAX_PAYLOAD)];
  this->note_escapes_(frame.escaped, false);
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

bool RainbirdComponent::run_minutes_(uint8_t zone, float *minutes) const {
#ifdef USE_NUMBER
  if (zone < 1 || zone > STATE_ZONES || this->run_durations_[zone - 1] == nullptr)
    return false;
  *minutes = this->run_durations_[zone - 1]->state;
  return true;
#else
  (void) zone;
  (void) minutes;
  return false;
#endif
}

bool RainbirdComponent::test_minutes_(float *minutes) const {
#ifdef USE_NUMBER
  if (this->test_duration_ == nullptr)
    return false;
  *minutes = this->test_duration_->state;
  return true;
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
  return program < STATE_PROGRAMS && at(this->session_->state().start_minutes[program], index).value_or(-1) >= 0;
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
  WritePlan plan = plan_zone_run(zone, duration_ms, queue);
  if (plan.action != WriteAction::CONTROL) {
    ESP_LOGW(TAG, "Refused zone run (zone %d, %u ms): %s", static_cast<int>(zone), static_cast<unsigned>(duration_ms),
             plan.refusal);
    return false;
  }
  return this->submitted_(this->session_->control(plan.control, millis()));
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
  // What the write means is decided in entity_writes.cpp. This only carries it out.
  uint32_t now = millis();
  WritePlan plan = plan_write(binding, value, this->writes_);
  switch (plan.action) {
    case WriteAction::CONTROL:
      return this->submitted_(this->session_->control(plan.control, now));
    case WriteAction::EDIT:
      return this->submitted_(this->session_->edit_schedule(plan.edit.field, plan.edit.zone, plan.edit.program,
                                                            plan.edit.index, plan.edit.value, now));
    case WriteAction::REFRESH:
      return this->submitted_(this->session_->refresh(now));
    case WriteAction::SYNC_CLOCK:
      return this->sync_clock_();
    case WriteAction::REFUSED:
      break;
  }
  ESP_LOGW(TAG, "Refused (kind %u, zone %u): %s", static_cast<unsigned>(binding.kind),
           static_cast<unsigned>(binding.zone), plan.refusal);
  return false;
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
  if (!state.time.has_value())
    return;
  WallClock controller{0, 0, 0, state.time->hour, state.time->minute, state.time->second};
  bool date_valid = state.date.has_value();
  if (date_valid) {
    controller.year = state.date->year;
    controller.month = state.date->month;
    controller.day = state.date->day;
  }
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

void RainbirdComponent::publish_() {
  // Most passes follow a frame or a loop with nothing new; skip walking every entity then.
  if (this->session_->revision() == this->published_revision_)
    return;
  this->published_revision_ = this->session_->revision();
  // Which values go out is decided in entity_board.cpp. The show_ methods below carry it out.
  this->board_.publish(readout(*this->session_), this->text_, *this);
  this->publish_remaining_();
}

// Each pointer comes back as the class its add_ method put on the board.
void RainbirdComponent::show_bool(Domain domain, void *entity, bool value) {
#ifdef USE_SWITCH
  if (domain == Domain::SWITCH)
    static_cast<RainbirdSwitch *>(entity)->publish_state(value);
#endif
#ifdef USE_BINARY_SENSOR
  if (domain == Domain::BINARY_SENSOR)
    static_cast<binary_sensor::BinarySensor *>(entity)->publish_state(value);
#endif
  (void) domain;
  (void) entity;
  (void) value;
}

void RainbirdComponent::show_number(Domain domain, void *entity, float value) {
#ifdef USE_NUMBER
  if (domain == Domain::NUMBER)
    static_cast<RainbirdNumber *>(entity)->publish_state(value);
#endif
#ifdef USE_SENSOR
  if (domain == Domain::SENSOR)
    static_cast<sensor::Sensor *>(entity)->publish_state(value);
#endif
  (void) domain;
  (void) entity;
  (void) value;
}

void RainbirdComponent::show_option(void *entity, uint8_t index) {
#ifdef USE_SELECT
  static_cast<RainbirdSelect *>(entity)->publish_state(index);
#else
  (void) entity;
  (void) index;
#endif
}

void RainbirdComponent::show_time(void *entity, uint8_t hour, uint8_t minute) {
#ifdef USE_DATETIME_TIME
  static_cast<RainbirdTime *>(entity)->show(hour, minute);
#else
  (void) entity;
  (void) hour;
  (void) minute;
#endif
}

void RainbirdComponent::show_clock(void *entity, const WallClock &clock) {
#ifdef USE_DATETIME_DATETIME
  static_cast<RainbirdDateTime *>(entity)->show(clock.year, clock.month, clock.day, clock.hour, clock.minute,
                                                clock.second);
#else
  (void) entity;
  (void) clock;
#endif
}

void RainbirdComponent::show_text(void *entity, const std::string &text) {
#ifdef USE_TEXT_SENSOR
  static_cast<text_sensor::TextSensor *>(entity)->publish_state(text);
#else
  (void) entity;
  (void) text;
#endif
}

void RainbirdComponent::show_unknown(Domain domain, void *entity) {
#ifdef USE_NUMBER
  if (domain == Domain::NUMBER)
    static_cast<RainbirdNumber *>(entity)->publish_state(NAN);
#endif
#ifdef USE_SENSOR
  if (domain == Domain::SENSOR)
    static_cast<sensor::Sensor *>(entity)->publish_state(NAN);
#endif
#ifdef USE_BINARY_SENSOR
  if (domain == Domain::BINARY_SENSOR)
    static_cast<binary_sensor::BinarySensor *>(entity)->invalidate_state();
#endif
  (void) domain;
  (void) entity;
}

void RainbirdComponent::publish_remaining_() {
  const ControllerState &state = this->session_->state();
  if (state.remaining_readings == this->remaining_readings_ || !state.remaining_seconds.has_value())
    return;
  this->remaining_readings_ = state.remaining_readings;
  uint16_t seconds = *state.remaining_seconds;
  // The timestamp also works in firmware with no numeric sensors.
  this->publish_ends_at_(seconds);
#ifdef USE_SENSOR
  if (this->remaining_sensors_.empty() || !this->countdown_.reading(seconds, millis()))
    return;
  this->publish_remaining_sensors_(seconds);
  if (seconds > 0) {
    // Count down locally between polls. Restarting the interval lines its ticks up with this reading.
    this->set_interval(COUNTDOWN_INTERVAL_ID, 1000, [this]() { this->tick_countdown_(); });
  } else {
    this->cancel_interval(COUNTDOWN_INTERVAL_ID);
  }
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

void RainbirdComponent::publish_ends_at_(uint16_t seconds) {
#if defined(USE_TEXT_SENSOR) && defined(USE_TIME)
  if (this->ends_at_sensors_.empty() || this->time_ == nullptr)
    return;
  // Without a clock there is no end time to give. The next reading tries again.
  if (!this->time_->now().is_valid())
    return;
  if (this->ends_at_.reading(seconds, static_cast<int64_t>(this->time_->timestamp_now())))
    this->show_ends_at_();
#else
  (void) seconds;
#endif
}

void RainbirdComponent::show_ends_at_() {
#ifdef USE_TEXT_SENSOR
  // Nothing running is an empty string, which Home Assistant's timestamp class shows as unknown.
  char buf[32] = "";
  if (this->ends_at_.end() != 0) {
    time_t end = static_cast<time_t>(this->ends_at_.end());
    struct tm tm{};
    gmtime_r(&end, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
  }
  for (text_sensor::TextSensor *entity : this->ends_at_sensors_)
    entity->publish_state(buf);
#endif
}

// The link is down and the session has dropped what it read (SessionEventKind::LINK_DOWN).
// Which entities go to unknown is decided in entity_board.cpp.
void RainbirdComponent::show_unknown_() {
  this->board_.link_lost(*this);
#ifdef USE_SENSOR
  // The local countdown would otherwise run on to zero by itself.
  this->cancel_interval(COUNTDOWN_INTERVAL_ID);
  this->countdown_ = Countdown{};
#endif
#ifdef USE_TEXT_SENSOR
  if (this->ends_at_.clear())
    this->show_ends_at_();
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
                "  Handshake: %s\n"
                "  Zones: %u\n"
                "  Programs: %u\n"
                "  Schedule refresh: %u ms",
                this->model_name_, this->session_ ? stage_text(this->session_->stage()) : "not started",
                this->listen_only_ ? "off (listen only)" : "on", YESNO(this->listen_only_),
                this->valve_queue_ ? "queue" : "start", YESNO(this->allow_writes_), YESNO(this->experimental_),
                YESNO(this->handshake_), static_cast<unsigned>(this->zones_.size()),
                static_cast<unsigned>(this->programs_.size()), static_cast<unsigned>(this->refresh_interval_ms_));
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
