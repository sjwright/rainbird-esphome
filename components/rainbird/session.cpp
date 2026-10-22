#include "session.h"

#include <algorithm>

#include "elapsed.h"

namespace esphome::rainbird {

namespace {

// Optional commands probed with 04 at startup (PLAN 6.3). The probe answers are
// the only capability source; the model id is used for the D12 gate alone.
constexpr uint8_t PROBED[] = {0x4C, 0x4A, 0x42, 0x4B, 0x3D, 0x49, 0x3A};

// Three requests in a row failing after their retries means the link is down.
constexpr uint8_t FAILURES_FOR_DOWN = 3;

// The slow group runs on the first cycle and then every fourth (PLAN 6.4).
constexpr uint32_t SLOW_POLL_EVERY = 4;

// Without 4C the clock is read as 10, then 12. A 12 this soon after a 10 completes the reading.
constexpr uint32_t CLOCK_PAIR_MS = 5000;

constexpr uint32_t tag(TagKind kind, uint8_t detail) { return (static_cast<uint32_t>(kind) << 8) | detail; }

TagKind tag_kind(uint32_t value) { return static_cast<TagKind>((value >> 8) & 0xFF); }

}  // namespace

const char *control_result_text(ControlResult result) {
  switch (result) {
    case ControlResult::QUEUED:
      return "queued";
    case ControlResult::REFUSED_NOT_READY:
      return "the controller is not identified, or this write is off";
    case ControlResult::REFUSED_LINK_DOWN:
      return "the controller is not answering";
    case ControlResult::REFUSED_EXPERIMENTAL:
      return "needs experimental: true";
    case ControlResult::REFUSED_UNSUPPORTED:
      return "the controller does not support it";
    case ControlResult::REFUSED_RANGE:
      return "out of range";
    case ControlResult::REFUSED_NO_ZONE:
      return "the controller says it does not have that zone";
    case ControlResult::REFUSED_NOT_RUNNING:
      return "that zone is not running";
    case ControlResult::REFUSED_QUEUED_ONLY:
      return "that zone is only queued, and there is no command to remove one zone";
    case ControlResult::REFUSED_BUSY:
      return "the link could not take another request";
  }
  return "refused";
}

Session::Session(const Model &model, const SessionConfig &config)
    : model_(model), config_(config), link_(config.timing), editor_(model) {}

bool Session::anything_changed() const {
  const LinkChanges &link = this->link_changes_;
  return this->state_touched_ || this->identity_changed_ || link.connected || link.response || link.errors;
}

void Session::clear_changed() {
  this->state_touched_ = false;
  this->state_.clear_changed();
  this->identity_changed_ = false;
  this->link_changes_ = LinkChanges{};
}

bool Session::take_event(SessionEvent *out) {
  if (out == nullptr || this->event_count_ == 0) {
    return false;
  }
  *out = this->events_[0];
  for (size_t i = 0; i + 1 < this->event_count_; i++) {
    this->events_[i] = this->events_[i + 1];
  }
  this->event_count_--;
  return true;
}

void Session::push_event_(const SessionEvent &event) {
  if (this->event_count_ >= EVENT_CAP) {
    // The hub drains events every pass, so this only drops under misuse. Keep the newest.
    for (size_t i = 0; i + 1 < EVENT_CAP; i++) {
      this->events_[i] = this->events_[i + 1];
    }
    this->event_count_ = EVENT_CAP - 1;
  }
  this->events_[this->event_count_++] = event;
}

void Session::set_stage_(SessionStage stage) {
  if (this->stage_ == stage) {
    return;
  }
  this->stage_ = stage;
  SessionEvent event;
  event.kind = SessionEventKind::STAGE;
  event.stage = stage;
  this->push_event_(event);
}

bool Session::enqueue_(const ByteBuf &request, Priority priority, TagKind kind, uint8_t detail, Delivery delivery) {
  return this->link_.enqueue(request, priority, tag(kind, detail), delivery);
}

void Session::enqueue_list_(const RequestList &list, Priority priority, TagKind kind, size_t *count) {
  for (uint8_t i = 0; i < list.count; i++) {
    if (this->enqueue_(list.items[i], priority, kind) && count != nullptr) {
      (*count)++;
    }
  }
}

void Session::start(uint32_t now_ms) {
  (void) now_ms;
  if (this->config_.listen_only) {
    this->set_stage_(SessionStage::PASSIVE);
    return;
  }
  if (!this->config_.polling) {
    this->set_stage_(SessionStage::MANUAL);
    return;
  }
  this->identify_();
}

void Session::identify_() {
  this->set_stage_(SessionStage::IDENTIFY);
  this->enqueue_(request_model(), Priority::FOLLOW_UP, TagKind::IDENTIFY);
}

void Session::restart_() {
  // What was learned at startup belongs to the controller that answered then, and so does
  // its state: a command after this is decided on what the new controller reports. Entities
  // keep showing the old values until new ones arrive, because nothing can be unpublished.
  if (this->editor_.active()) {
    this->fail_edit_("the controller stopped answering");
  }
  this->state_ = ControllerState{};
  this->identity_ = Identity{};
  this->time_pending_ = false;
  this->link_.drop_queued(Priority::POLL);
  this->link_.drop_queued(Priority::FOLLOW_UP);
  this->caps_ = Capabilities{};
  this->probes_done_ = false;
  this->writes_permitted_ = false;
  this->schedule_read_once_ = false;
  this->schedule_stale_ = false;
  this->event_known_ = false;
  this->poll_count_ = 0;
  this->identify_();
}

ControlResult Session::refresh(uint32_t now_ms) {
  if (this->stage_ != SessionStage::IDLE && this->stage_ != SessionStage::RUNNING) {
    return ControlResult::REFUSED_NOT_READY;
  }
  this->poll_count_ = 0;  // the next cycle includes the slow group
  this->schedule_stale_ = true;
  this->poll(now_ms);
  return ControlResult::QUEUED;
}

void Session::check_silence(uint32_t now_ms) {
  if (this->stage_ == SessionStage::PASSIVE && this->frame_seen_ &&
      elapsed(now_ms, this->last_frame_at_, this->config_.silence_ms)) {
    this->set_connected_(false, now_ms);
  }
}

void Session::poll(uint32_t now_ms) {
  switch (this->stage_) {
    case SessionStage::PASSIVE:
    case SessionStage::MANUAL:
      return;
    case SessionStage::IDLE:
      this->identify_();
      return;
    case SessionStage::IDENTIFY:
    case SessionStage::PROBE:
    case SessionStage::SCHEDULE:
      return;
    case SessionStage::RUNNING:
      break;
  }
  // A controller gone this long may not be the same one when it answers again: it could have
  // been replaced or reflashed. Start over, so its model id, probes and schedule are read again.
  if (this->link_status_.connected_known && !this->link_status_.connected &&
      elapsed(now_ms, this->down_since_, this->config_.reidentify_after_ms)) {
    this->restart_();
    return;
  }
  // One cycle at a time. A cycle still queued or in flight is not doubled up.
  if (this->link_.queued(Priority::POLL) > 0) {
    return;
  }
  RequestList list;
  this->model_.short_poll(this->caps_, this->config_.used, &list);
  this->enqueue_list_(list, Priority::POLL, TagKind::POLL);
  if (this->poll_count_ % SLOW_POLL_EVERY == 0) {
    this->model_.slow_poll(this->caps_, this->config_.used, &list);
    this->enqueue_list_(list, Priority::POLL, TagKind::POLL);
  }
  this->poll_count_++;
  if (this->schedule_due_(now_ms)) {
    this->read_schedule_(now_ms, Priority::POLL, nullptr);
  }
}

bool Session::schedule_due_(uint32_t now_ms) const {
  if (!this->schedule_read_once_) {
    return false;
  }
  return this->schedule_stale_ || elapsed(now_ms, this->last_schedule_read_, this->config_.refresh_interval_ms);
}

void Session::read_schedule_(uint32_t now_ms, Priority priority, size_t *count) {
  size_t wanted = 0;
  size_t queued = 0;
  for (uint16_t page = 0; page <= 0xFF; page++) {
    if (!this->config_.pages.has(static_cast<uint8_t>(page))) {
      continue;
    }
    wanted++;
    if (this->enqueue_(request_read_page(static_cast<uint8_t>(page)), priority, TagKind::SCHEDULE)) {
      queued++;
    }
  }
  if (count != nullptr) {
    *count += queued;
  }
  this->last_schedule_read_ = now_ms;
  this->schedule_read_once_ = true;
  // A page the queue refused is read again on the next poll, not a refresh_interval later.
  // A page read that times out does the same (handle_()).
  this->schedule_stale_ = queued < wanted;
}

void Session::tick(uint32_t now_ms) {
  this->link_.tick(now_ms);
  Completion done;
  while (this->link_.pop_completion(&done)) {
    this->handle_(done, now_ms);
  }
  if (this->link_.link_errors() != this->link_status_.errors) {
    this->link_status_.errors = this->link_.link_errors();
    this->link_changes_.errors = true;
  }
  this->link_status_.replies = this->link_.replies();
  this->link_status_.duplicates = this->link_.duplicates();
  this->link_status_.unexpected = this->link_.unexpected();
}

bool Session::next_transmission(uint32_t now_ms, Transmission *out) {
  return this->link_.next_transmission(now_ms, out);
}

void Session::note_clock_reading_(uint32_t at_ms) {
  this->clock_read_at_ = at_ms;
  if (++this->clock_serial_ == 0) {
    this->clock_serial_ = 1;
  }
}

void Session::on_frame(const uint8_t *payload, size_t size, uint32_t now_ms) {
  this->last_frame_at_ = now_ms;
  this->frame_seen_ = true;
  if (this->stage_ == SessionStage::PASSIVE) {
    this->set_connected_(true, now_ms);
  }
  // Passive, every valid reply is the controller answering the LNK2, so all of it is state.
  // Active, only the answer to the request in flight is: a late or duplicate reply is older
  // than what that request brings, and must not replace it.
  bool passive = this->stage_ == SessionStage::PASSIVE;
  Reply reply;
  bool parsed = parse_reply(payload, size, &reply);
  bool answer = passive || this->link_.on_frame(payload, size, parsed ? &reply : nullptr, now_ms);
  if (answer && parsed) {
    this->model_.apply_reply(reply, &this->state_);
    this->state_touched_ = true;
    switch (reply.kind) {
      case ReplyKind::MODEL:
        this->identity_.model = reply.model;
        this->identity_.model_known = true;
        this->identity_changed_ = true;
        break;
      case ReplyKind::FIRMWARE:
        this->identity_.firmware = reply.firmware;
        this->identity_.firmware_known = true;
        this->identity_changed_ = true;
        break;
      case ReplyKind::SERIAL_NUMBER:
        this->identity_.serial = reply.serial;
        this->identity_.serial_known = true;
        this->identity_changed_ = true;
        break;
      case ReplyKind::COMBINED:
        this->note_clock_reading_(now_ms);
        break;
      case ReplyKind::TIME:
        this->time_pending_ = true;
        this->time_at_ = now_ms;
        break;
      case ReplyKind::DATE:
        // The pair is one reading, timed by the 10 reply.
        if (this->time_pending_ && !elapsed(now_ms, this->time_at_, CLOCK_PAIR_MS)) {
          this->note_clock_reading_(this->time_at_);
        }
        this->time_pending_ = false;
        break;
      case ReplyKind::EVENT:
        // A new schedule timestamp means the schedule was edited (SPEC 8.11).
        if (reply.event.event == 1) {
          if (this->event_known_ && reply.event.seconds_since_2000 != this->event_value_) {
            this->schedule_stale_ = true;
          }
          this->event_known_ = true;
          this->event_value_ = reply.event.seconds_since_2000;
        }
        break;
      default:
        break;
    }
  }
  if (!passive) {
    this->tick(now_ms);
  }
}

ControlResult Session::send_raw(const uint8_t *payload, size_t size, uint32_t now_ms) {
  if (this->stage_ == SessionStage::PASSIVE) {
    return ControlResult::REFUSED_NOT_READY;
  }
  if (!this->config_.experimental) {
    return ControlResult::REFUSED_EXPERIMENTAL;
  }
  if (payload == nullptr || size == 0 || size > ByteBuf::CAP) {
    return ControlResult::REFUSED_RANGE;
  }
  ByteBuf request;
  for (size_t i = 0; i < size; i++) {
    request.push(payload[i]);
  }
  // Never retried. If the controller acted and the reply was lost, a retry would run the command again.
  if (!this->enqueue_(request, Priority::USER, TagKind::RAW, 0, Delivery::NO_RETRY)) {
    return ControlResult::REFUSED_BUSY;
  }
  this->hold_ = true;
  this->hold_from_ = now_ms;
  return ControlResult::QUEUED;
}

void Session::note_unsupported_(uint8_t opcode) {
  CommandSupport support;
  support.command = opcode;
  support.supported = false;
  this->model_.note_support(support, &this->caps_);
}

void Session::set_connected_(bool connected, uint32_t now_ms) {
  if (!this->link_status_.connected_known || this->link_status_.connected != connected) {
    this->link_changes_.connected = true;
    if (!connected) {
      this->down_since_ = now_ms;
      // Commands do not outlive a disconnect: one queued now could run minutes later, or on
      // a different controller. The edit loses its next step with them.
      this->link_.drop_queued(Priority::USER);
      if (this->editor_.active()) {
        this->fail_edit_("the controller stopped answering");
      }
    }
    SessionEvent event;
    event.kind = connected ? SessionEventKind::LINK_UP : SessionEventKind::LINK_DOWN;
    this->push_event_(event);
  }
  this->link_status_.connected_known = true;
  this->link_status_.connected = connected;
}

void Session::handle_(const Completion &done, uint32_t now_ms) {
  if (done.outcome == Outcome::FAILED) {
    if (this->link_.consecutive_failures() >= FAILURES_FOR_DOWN) {
      this->set_connected_(false, now_ms);
    }
  } else {
    this->set_connected_(true, now_ms);
    if (!this->link_status_.response_known || this->link_status_.response_ms != done.response_ms) {
      this->link_changes_.response = true;
    }
    this->link_status_.response_known = true;
    this->link_status_.response_ms = done.response_ms;
  }
  if (done.outcome == Outcome::NAK) {
    SessionEvent event;
    event.kind = SessionEventKind::NAK;
    event.opcode = done.opcode;
    event.code = done.reply.nak.code;
    this->push_event_(event);
    // NAK bit 0 is "command not supported" (SPEC 3.3). Drop that capability.
    if (nak_is_unsupported(done.reply.nak.code)) {
      this->note_unsupported_(done.opcode);
    }
  }

  switch (tag_kind(done.tag)) {
    case TagKind::IDENTIFY:
      if (done.outcome == Outcome::FAILED) {
        this->set_stage_(SessionStage::IDLE);
        return;
      }
      if (done.outcome == Outcome::REPLY && done.reply.kind == ReplyKind::MODEL) {
        uint16_t id = done.reply.model.model_id;
        bool extra = std::find(this->config_.accept_ids.begin(), this->config_.accept_ids.end(), id) !=
                     this->config_.accept_ids.end();
        this->writes_permitted_ = this->model_.accepts(id) || extra;
      }
      this->set_stage_(SessionStage::PROBE);
      this->enqueue_(request_firmware(), Priority::FOLLOW_UP, TagKind::INFO);
      this->enqueue_(request_serial(), Priority::FOLLOW_UP, TagKind::INFO);
      this->enqueue_(request_stations(0), Priority::FOLLOW_UP, TagKind::INFO);
      this->probes_pending_ = 0;
      this->probe_answered_ = false;
      this->probe_refused_ = false;
      for (uint8_t command : PROBED) {
        // Only a queued probe completes. One the queue refused counts as unanswered.
        if (this->enqueue_(request_support(command), Priority::FOLLOW_UP, TagKind::PROBE, command)) {
          this->probes_pending_++;
        }
      }
      if (this->probes_pending_ == 0) {
        this->finish_probes_(now_ms);
      }
      return;
    case TagKind::PROBE:
      if (done.outcome == Outcome::REPLY && done.reply.kind == ReplyKind::SUPPORT) {
        this->model_.note_support(done.reply.support, &this->caps_);
        this->probe_answered_ = true;
      } else if (done.outcome == Outcome::NAK) {
        this->probe_refused_ = true;
      }
      if (this->probes_pending_ > 0 && --this->probes_pending_ == 0) {
        this->finish_probes_(now_ms);
      }
      return;
    case TagKind::SCHEDULE:
      // A NAK is an answer: that page does not exist. A timeout is read again on the next poll.
      if (done.outcome == Outcome::FAILED) {
        this->schedule_stale_ = true;
      }
      if (this->stage_ == SessionStage::SCHEDULE && this->schedule_pending_ > 0 && --this->schedule_pending_ == 0) {
        this->begin_running_(now_ms);
      }
      return;
    case TagKind::INFO:
    case TagKind::POLL:
    case TagKind::RAW:
      return;
    case TagKind::CONTROL:
      // The short poll runs only after an ack. A NAK is the answer; sending the command again is not.
      if (done.outcome == Outcome::REPLY) {
        this->follow_up_(now_ms);
      }
      return;
    case TagKind::PAGE:
      this->handle_page_(done);
      return;
  }
}

void Session::finish_probes_(uint32_t now_ms) {
  this->probes_done_ = true;
  this->caps_assumed_ = !this->probe_answered_ && this->probe_refused_;
  if (this->caps_assumed_) {
    // 04 itself is refused: a controller without it. Try every optional command, and a NAK
    // clears the ones that fail. Probes that only timed out say nothing, so nothing is assumed:
    // the fallback polls (10, 12, 36, 3E, 48) work on every model.
    for (uint8_t command : PROBED) {
      CommandSupport support;
      support.command = command;
      support.supported = true;
      this->model_.note_support(support, &this->caps_);
    }
  }
  this->schedule_pending_ = 0;
  this->read_schedule_(now_ms, Priority::FOLLOW_UP, &this->schedule_pending_);
  if (this->schedule_pending_ > 0) {
    this->set_stage_(SessionStage::SCHEDULE);
  } else {
    this->begin_running_(now_ms);
  }
}

void Session::begin_running_(uint32_t now_ms) {
  this->set_stage_(SessionStage::RUNNING);
  // First data now, rather than one update_interval later.
  this->poll(now_ms);
}

bool Session::watering_() const {
  for (uint8_t zone = 1; zone <= STATE_ZONES; zone++) {
    bool on = false;
    if (this->state_.zone_running(zone, &on) && on) {
      return true;
    }
  }
  uint8_t active = 0;
  if (this->state_.active_zone(&active) && active != 0) {
    return true;
  }
  uint16_t remaining = 0;
  return this->state_.remaining_seconds(&remaining) && remaining != 0;
}

ControlResult Session::unsupported_() const {
  // Before the probes finish, "not supported" would be a guess.
  return this->probes_done_ ? ControlResult::REFUSED_UNSUPPORTED : ControlResult::REFUSED_NOT_READY;
}

bool Session::link_down_() const { return this->link_status_.connected_known && !this->link_status_.connected; }

bool Session::ready_for_command_() const {
  if (!this->writes_permitted_) {
    return false;
  }
  return this->stage_ == SessionStage::PROBE || this->stage_ == SessionStage::SCHEDULE ||
         this->stage_ == SessionStage::RUNNING;
}

bool Session::enqueue_command_(const ByteBuf &request, uint32_t now_ms) {
  if (request.size == 0) {
    return false;
  }
  // One copy, never retried: the controller may act on every copy it hears, and a lost reply
  // does not mean it did nothing.
  if (!this->enqueue_(request, Priority::USER, TagKind::CONTROL, request.data[0], Delivery::ONCE)) {
    return false;
  }
  this->hold_ = true;
  this->hold_from_ = now_ms;
  return true;
}

void Session::follow_up_(uint32_t now_ms) {
  this->hold_ = true;
  this->hold_from_ = now_ms;
  // Commands in a row (time, then date) share one follow-up. It still reads after the last
  // of them, because commands go ahead of FOLLOW_UP.
  if (this->link_.queued(Priority::FOLLOW_UP) > 0) {
    return;
  }
  RequestList list;
  this->model_.short_poll(this->caps_, this->config_.used, &list);
  this->enqueue_list_(list, Priority::FOLLOW_UP, TagKind::POLL);
}

bool Session::zone_missing_(uint8_t zone) const {
  bool available = true;
  return this->state_.zone_available(zone, &available) && !available;
}

bool Session::next_queued_zone_(uint8_t skip, uint8_t *zone) const {
  if (zone == nullptr || !this->state_.queue_known()) {
    return false;
  }
  uint8_t active = 0;
  this->state_.active_zone(&active);
  for (uint8_t i = 0; i < this->state_.queue_count(); i++) {
    QueueItem item;
    if (!this->state_.queue_item(i, &item) || item.zone == 0) {
      continue;
    }
    if (item.zone == skip || item.zone == active) {
      continue;
    }
    *zone = item.zone;
    return true;
  }
  return false;
}

ControlResult Session::control(const Control &command, uint32_t now_ms) {
  if (this->link_down_()) {
    return ControlResult::REFUSED_LINK_DOWN;
  }
  if (!this->ready_for_command_()) {
    return ControlResult::REFUSED_NOT_READY;
  }
  const ModelLimits &limits = this->model_.limits();
  const uint8_t zones = limits.zones;
  const uint8_t programs = limits.programs;
  ByteBuf request;
  switch (command.op) {
    case ControlOp::RUN_ZONE:
    case ControlOp::QUEUE_ZONE:
    case ControlOp::VALVE_ON: {
      bool queue =
          command.op == ControlOp::QUEUE_ZONE || (command.op == ControlOp::VALVE_ON && this->config_.valve_queue);
      if (command.zone < 1 || command.zone > zones || command.value < 1 || command.value > limits.run_max_minutes) {
        return ControlResult::REFUSED_RANGE;
      }
      if (this->zone_missing_(command.zone)) {
        return ControlResult::REFUSED_NO_ZONE;
      }
      if (queue && !this->caps_.queue_run) {
        return this->unsupported_();
      }
      uint8_t minutes = static_cast<uint8_t>(command.value);
      request = queue ? request_queue_station(0, command.zone, minutes) : request_run_station(command.zone, minutes);
      break;
    }
    case ControlOp::VALVE_OFF: {
      bool running = false;
      if (!this->state_.zone_running(command.zone, &running) || !running) {
        for (uint8_t i = 0; i < this->state_.queue_count(); i++) {
          QueueItem item;
          if (this->state_.queue_item(i, &item) && item.zone == command.zone) {
            return ControlResult::REFUSED_QUEUED_ONLY;
          }
        }
        return ControlResult::REFUSED_NOT_RUNNING;
      }
      uint8_t next = 0;
      if (this->config_.valve_queue && this->config_.experimental && this->caps_.advance &&
          this->next_queued_zone_(command.zone, &next)) {
        request = request_advance(next);
      } else {
        request = request_stop();
      }
      break;
    }
    case ControlOp::RUN_PROGRAM:
      if (command.program >= programs) {
        return ControlResult::REFUSED_RANGE;
      }
      request = request_run_program(command.program);
      break;
    case ControlOp::STOP:
      request = request_stop();
      break;
    case ControlOp::ADVANCE: {
      if (!this->config_.experimental) {
        return ControlResult::REFUSED_EXPERIMENTAL;
      }
      if (!this->caps_.advance) {
        return this->unsupported_();
      }
      uint8_t next = 0;
      if (!this->next_queued_zone_(0, &next)) {
        return ControlResult::REFUSED_NOT_RUNNING;
      }
      request = request_advance(next);
      break;
    }
    case ControlOp::SET_SEASONAL:
      if (command.program >= programs || command.value < limits.seasonal_min_percent ||
          command.value > limits.seasonal_max_percent) {
        return ControlResult::REFUSED_RANGE;
      }
      request = request_set_water_budget(command.program, command.value);
      break;
    case ControlOp::SET_RAIN_DELAY:
      if (command.value > limits.rain_delay_max_days) {
        return ControlResult::REFUSED_RANGE;
      }
      request = request_set_rain_delay(command.value);
      break;
    case ControlOp::SET_IRRIGATION:
      if (!this->config_.experimental) {
        return ControlResult::REFUSED_EXPERIMENTAL;
      }
      if (!this->caps_.controller_state) {
        return this->unsupported_();
      }
      request = request_set_controller_state(command.value == 0 ? 0 : 1);
      break;
    case ControlOp::TEST_ZONES:
      if (!this->config_.experimental) {
        return ControlResult::REFUSED_EXPERIMENTAL;
      }
      if (!this->caps_.test_stations) {
        return this->unsupported_();
      }
      if (command.value < 1 || command.value > limits.run_max_minutes) {
        return ControlResult::REFUSED_RANGE;
      }
      request = request_test_stations(static_cast<uint8_t>(command.value));
      break;
    case ControlOp::SET_CLOCK: {
      PackedDate date;
      date.day = command.day;
      date.month = command.month;
      date.year = command.year;
      if (command.hour > 23 || command.minute > 59 || command.second > 59 || !date_valid(date)) {
        return ControlResult::REFUSED_RANGE;
      }
      // Both or neither: a new time without its date is a clock a day out near midnight.
      if (this->link_.space() < 2) {
        return ControlResult::REFUSED_BUSY;
      }
      this->enqueue_command_(request_set_time(command.hour, command.minute, command.second), now_ms);
      request = request_set_date(date);
      break;
    }
  }
  if (!this->enqueue_command_(request, now_ms)) {
    return ControlResult::REFUSED_BUSY;
  }
  return ControlResult::QUEUED;
}

void Session::fail_edit_(const char *reason) {
  this->editor_.cancel();
  SessionEvent event;
  event.kind = SessionEventKind::EDIT_FAILED;
  event.reason = reason;
  this->push_event_(event);
  // A write whose acknowledgement was lost may still have landed. Read the schedule again so
  // the entities show what the controller holds.
  this->schedule_stale_ = true;
}

ControlResult Session::edit_schedule(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value,
                                     uint32_t now_ms) {
  (void) now_ms;
  // Page writes wait until the link is running. A write during the first schedule read
  // would race that read, and allow_writes stays off unless the config opts in.
  if (!this->config_.allow_writes || this->stage_ != SessionStage::RUNNING || !this->writes_permitted_) {
    return ControlResult::REFUSED_NOT_READY;
  }
  if (this->link_down_()) {
    return ControlResult::REFUSED_LINK_DOWN;
  }
  if (this->editor_.active()) {
    return ControlResult::REFUSED_BUSY;
  }
  ByteBuf read;
  if (!this->editor_.start(field, zone, program, index, value, &read)) {
    return ControlResult::REFUSED_RANGE;
  }
  if (!this->enqueue_(read, Priority::USER, TagKind::PAGE, this->editor_.page())) {
    this->editor_.cancel();
    return ControlResult::REFUSED_BUSY;
  }
  return ControlResult::QUEUED;
}

void Session::handle_page_(const Completion &done) {
  if (!this->editor_.active()) {
    return;
  }
  ByteBuf next;
  switch (this->editor_.on_reply(done, &next)) {
    case ScheduleEditor::Step::WRITE: {
      if (!this->enqueue_(next, Priority::USER, TagKind::PAGE, this->editor_.page(), Delivery::ONCE)) {
        this->fail_edit_("the page write could not be queued");
        return;
      }
      // Logged before it goes: the hub drains events ahead of the next transmission.
      SessionEvent event;
      event.kind = SessionEventKind::PAGE_WRITE;
      this->push_event_(event);
      return;
    }
    case ScheduleEditor::Step::CONFIRM:
      // The write moves the schedule timestamp (4A 01). That change is ours, and the confirming
      // read covers it, so the next timestamp is taken as it is rather than re-reading every page.
      this->event_known_ = false;
      if (!this->enqueue_(next, Priority::USER, TagKind::PAGE, this->editor_.page())) {
        this->fail_edit_("the page was written, but the confirming read could not be queued");
      }
      return;
    case ScheduleEditor::Step::DONE:
      return;
    case ScheduleEditor::Step::FAILED:
      this->fail_edit_(this->editor_.reason());
      return;
  }
}

uint32_t Session::desired_interval(uint32_t now_ms) const {
  if (this->stage_ != SessionStage::RUNNING) {
    return this->config_.update_interval_ms;
  }
  if (this->watering_() || (this->hold_ && !elapsed(now_ms, this->hold_from_, this->config_.fast_hold_ms))) {
    return std::min(this->config_.fast_interval_ms, this->config_.update_interval_ms);
  }
  return this->config_.update_interval_ms;
}

}  // namespace esphome::rainbird
