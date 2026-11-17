#include "session.h"

#include <algorithm>

#include "elapsed.h"

namespace esphome::rainbird {

namespace {

// The optional commands are probed with 04 at startup, in CAPABILITY_OPCODES order (model.h,
// PLAN 6.3). The probe answers are the only capability source; the model id is used for the
// D12 gate alone.

// One bit per capability, for probes NAKed and not yet settled.
constexpr uint8_t probe_bit(uint8_t opcode) {
  for (uint8_t i = 0; i < CAPABILITY_COUNT; i++) {
    if (CAPABILITY_OPCODES[i] == opcode) {
      return static_cast<uint8_t>(1u << i);
    }
  }
  return 0;
}

static_assert(CAPABILITY_COUNT <= 8, "probes_refused_ holds one bit per capability");

// Three requests in a row failing after their retries means a link that was up is down.
constexpr uint8_t FAILURES_FOR_DOWN = 3;

// The slow group runs on the first cycle and then every fourth (PLAN 6.4).
constexpr uint32_t SLOW_POLL_EVERY = 4;

// Without 4C the clock is read as 10, then 12. A 12 this soon after a 10 completes the reading.
constexpr uint32_t CLOCK_PAIR_MS = 5000;

constexpr uint32_t tag(TagKind kind, uint8_t detail) { return (static_cast<uint32_t>(kind) << 8) | detail; }

TagKind tag_kind(uint32_t value) { return static_cast<TagKind>((value >> 8) & 0xFF); }

uint8_t tag_detail(uint32_t value) { return static_cast<uint8_t>(value & 0xFF); }

// 31 sets a program's seasonal adjust. The short poll does not read it back (30 is in the slow group).
constexpr uint8_t OPCODE_SET_WATER_BUDGET = 0x31;
constexpr uint8_t OPCODE_SET_TIME = 0x11;

}  // namespace

const char *control_result_text(ControlResult result) {
  switch (result) {
    case ControlResult::QUEUED:
      return "queued";
    case ControlResult::REFUSED_NOT_READY:
      return "not ready yet: not identified, a probe or the queue unread, or this write is off";
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

Session::Session(const EspTm2Model &model, const SessionConfig &config)
    : model_(model), config_(config), link_(config.timing), editor_(model) {}

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
  if (!this->config_.handshake) {
    this->send_identify_();
    return;
  }
  // A fresh challenge each attempt. Zero is avoided: T() maps it to FFFFFFFF.
  uint32_t challenge = this->config_.random != nullptr ? this->config_.random() : 0;
  if (this->config_.random == nullptr) {
    challenge = 0x12345678u + 0x9E3779B9u * this->handshake_count_++;
  }
  if (challenge == 0) {
    challenge = 1;
  }
  this->handshake_challenge_ = challenge;
  this->set_stage_(SessionStage::HANDSHAKE);
  this->enqueue_(request_handshake(challenge), Priority::FOLLOW_UP, TagKind::HANDSHAKE, 1);
}

void Session::send_identify_() {
  this->set_stage_(SessionStage::IDENTIFY);
  this->enqueue_(request_model(), Priority::FOLLOW_UP, TagKind::IDENTIFY);
}

void Session::handle_handshake_(const Completion &done) {
  SessionEvent event;
  event.kind = SessionEventKind::HANDSHAKE;
  if (done.outcome == Outcome::FAILED) {
    // Nothing answered. Like a silent 02: wait for the next poll, then start again.
    this->set_stage_(SessionStage::IDLE);
    return;
  }
  if (tag_detail(done.tag) == 1) {
    if (done.outcome == Outcome::REPLY && done.reply.kind == ReplyKind::HANDSHAKE) {
      if (done.reply.handshake.answer != handshake_answer(this->handshake_challenge_)) {
        // Carry on: what matters is whether the controller accepts our answer to its challenge.
        event.code = 1;
        event.reason = "the controller's answer to our challenge is not the expected one";
        this->push_event_(event);
      }
      this->enqueue_(request_handshake_answer(handshake_answer(done.reply.handshake.challenge)), Priority::FOLLOW_UP,
                     TagKind::HANDSHAKE, 2);
      return;
    }
    // A NAK or any other answer: this controller has no handshake. Identify as before.
    event.reason = "the controller does not use one";
    this->push_event_(event);
    this->send_identify_();
    return;
  }
  if (done.outcome == Outcome::NAK) {
    event.code = 1;
    event.reason = "the controller refused our answer";
    this->push_event_(event);
    this->set_stage_(SessionStage::IDLE);
    return;
  }
  event.reason = done.reply.kind == ReplyKind::HANDSHAKE_DONE ? "complete" : "complete (no 87)";
  this->push_event_(event);
  this->send_identify_();
}

void Session::restart_() {
  // What was learned at startup belongs to the controller that answered then, and so does
  // its state: a command after this is decided on what the new controller reports.
  this->editor_.drop_waiting();
  if (this->editor_.active()) {
    this->fail_edit_("the controller stopped answering");
  }
  this->forget_state_(false);
  this->identity_ = Identity{};
  this->time_pending_ = false;
  this->link_.drop_queued(Priority::POLL);
  this->link_.drop_queued(Priority::FOLLOW_UP);
  this->caps_ = Capabilities{};
  this->probe_retry_ = false;
  this->date_after_time_ = false;
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
  this->probe_retry_ = true;
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
  // Cleared once it runs out, so a hold from weeks ago cannot come back when millis() wraps.
  if (this->hold_ && elapsed(now_ms, this->hold_from_, this->config_.fast_hold_ms)) {
    this->hold_ = false;
  }
  switch (this->stage_) {
    case SessionStage::PASSIVE:
    case SessionStage::MANUAL:
      return;
    case SessionStage::IDLE:
      this->identify_();
      return;
    case SessionStage::HANDSHAKE:
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
    if (this->probe_retry_) {
      this->probe_retry_ = false;
      this->retry_probes_();
    }
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
    this->revision_++;
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
    this->revision_++;
    switch (reply.kind) {
      case ReplyKind::ACTIVE:
        if (reply.stations.page == 0) {
          std::fill_n(this->unconfirmed_starts_, STATE_ZONES, false);
        }
        break;
      case ReplyKind::MODEL:
        this->identity_.model = reply.model;
        this->identity_.model_known = true;
        break;
      case ReplyKind::FIRMWARE:
        this->identity_.firmware = reply.firmware;
        this->identity_.firmware_known = true;
        break;
      case ReplyKind::SERIAL_NUMBER:
        this->identity_.serial = reply.serial;
        this->identity_.serial_known = true;
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

void Session::retry_probes_() {
  // A probe that timed out says nothing about the command, so it is asked again, once.
  for (uint8_t command : CAPABILITY_OPCODES) {
    if (!this->caps_.known(command)) {
      this->enqueue_(request_support(command), Priority::POLL, TagKind::PROBE, command);
    }
  }
}

void Session::set_connected_(bool connected, uint32_t now_ms) {
  if (!this->link_status_.connected_known || this->link_status_.connected != connected) {
    this->revision_++;
    if (!connected) {
      this->down_since_ = now_ms;
      // Commands do not outlive a disconnect: one queued now could run minutes later, or on
      // a different controller. The edit loses its next step with them.
      this->link_.drop_queued(Priority::USER);
      this->date_after_time_ = false;  // its time was dropped with the other commands
      this->editor_.drop_waiting();    // and so are the edits waiting behind the one in flight
      // What was read is no longer known to be true: a zone shown running may have stopped.
      // Everything is read again when the controller answers, the schedule included.
      this->forget_state_(true);
      this->schedule_stale_ = this->schedule_read_once_;
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

void Session::forget_state_(bool keep_zones) {
  ControllerState fresh;
  // The count only ever moves on, so whoever watches it never sees an old number come round again.
  fresh.remaining_readings = this->state_.remaining_readings + 1;
  if (keep_zones) {
    // Which zones the controller has is read once, at startup. A restart reads it again.
    for (size_t i = 0; i < STATE_ZONES; i++) {
      fresh.available[i] = this->state_.available[i];
    }
  }
  this->state_ = fresh;
  this->time_pending_ = false;
  std::fill_n(this->unconfirmed_starts_, STATE_ZONES, false);
}

void Session::handle_(const Completion &done, uint32_t now_ms) {
  if (done.outcome == Outcome::FAILED) {
    // A controller that has never answered is down at the first request that fails, retries
    // included: that is a wiring fault to report now, not three polls later. One that has
    // answered gets three in a row, so a single lost reply does not drop what was read.
    if (!this->link_status_.connected_known || this->link_.consecutive_failures() >= FAILURES_FOR_DOWN) {
      this->set_connected_(false, now_ms);
    }
  } else {
    this->set_connected_(true, now_ms);
    if (!this->link_status_.response_known || this->link_status_.response_ms != done.response_ms) {
      this->revision_++;
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
      this->caps_.set(done.opcode, false);
    }
  }

  switch (tag_kind(done.tag)) {
    case TagKind::HANDSHAKE:
      this->handle_handshake_(done);
      return;
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
      this->probes_refused_ = 0;
      for (uint8_t command : CAPABILITY_OPCODES) {
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
        // reply_matches() has checked that the 84 names the command this probe asked about.
        this->caps_.set(done.reply.support.command, done.reply.support.supported);
        this->probe_answered_ = true;
      } else if (done.outcome == Outcome::NAK) {
        this->probe_refused_ = true;
        this->probes_refused_ |= probe_bit(tag_detail(done.tag));
        if (this->probes_pending_ == 0) {
          this->settle_refused_probes_();  // a retry, after startup has decided
        }
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
      return;
    case TagKind::RAW:
      if (done.outcome == Outcome::FAILED) {
        this->no_reply_(done.opcode);
      }
      return;
    case TagKind::ZONE_START:
      if (done.outcome != Outcome::NAK && !this->link_down_()) {
        this->unconfirmed_starts_[tag_detail(done.tag) - 1] = true;
      }
      [[fallthrough]];
    case TagKind::CONTROL:
      // Reported, because nothing else says so: the command went once and nobody answered.
      if (done.outcome == Outcome::FAILED) {
        this->no_reply_(done.opcode);
      }
      if (done.opcode == OPCODE_SET_TIME && this->date_after_time_) {
        // A time the controller did not take, or may not have, is not followed by its date:
        // a new date on the old time is a clock a day out near midnight.
        this->date_after_time_ = false;
        if (done.outcome == Outcome::REPLY) {
          this->enqueue_command_(request_set_date(this->date_to_set_), now_ms);
        }
      }
      // The short poll runs only after an ack. A NAK is the answer; sending the command again is not.
      if (done.outcome == Outcome::REPLY) {
        this->follow_up_(now_ms);
        // The new seasonal adjust would otherwise show only after the next slow poll. The tag holds the program.
        if (done.opcode == OPCODE_SET_WATER_BUDGET) {
          this->enqueue_(request_water_budget(tag_detail(done.tag)), Priority::FOLLOW_UP, TagKind::POLL);
        }
      }
      return;
    case TagKind::PAGE:
      this->handle_page_(done);
      return;
  }
}

void Session::no_reply_(uint8_t opcode) {
  SessionEvent event;
  event.kind = SessionEventKind::NO_REPLY;
  event.opcode = opcode;
  this->push_event_(event);
}

void Session::settle_refused_probes_() {
  // A NAK refuses 04 itself, not the command it asked about. With no 84 reply at all, this is a
  // controller without 04: try the command, and its own NAK clears it. Alongside 84 replies, a
  // NAK is taken as "no". Probes that only timed out say nothing: they stay unknown, the
  // fallback polls (10, 12, 36, 3E, 48) work on every model, and retry_probes_() asks again.
  this->caps_assumed_ = !this->probe_answered_ && this->probe_refused_;
  for (uint8_t command : CAPABILITY_OPCODES) {
    if ((this->probes_refused_ & probe_bit(command)) != 0) {
      this->caps_.set(command, this->caps_assumed_);
    }
  }
  this->probes_refused_ = 0;
}

void Session::finish_probes_(uint32_t now_ms) {
  this->settle_refused_probes_();
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
  // Probes that timed out are asked again with the next slow group, not straight away.
  this->probe_retry_ = true;
}

// Whether to poll fast. Broader than the `watering` sensor (bool_value in entity_values.cpp):
// time left also counts, so a reply that shows no zone running yet, while the
// controller still counts down, keeps polling fast. A fast poll too many costs one
// cycle; a slow one too early misses the next zone starting.
bool Session::watering_() const {
  for (const std::optional<bool> &running : this->state_.running) {
    if (running.value_or(false)) {
      return true;
    }
  }
  return this->state_.active_zone.value_or(0) != 0 || this->state_.remaining_seconds.value_or(0) != 0;
}

ControlResult Session::unsupported_(Capability capability) const {
  // Until the probe has an answer, "not supported" would be a guess.
  return this->caps_.known(capability) ? ControlResult::REFUSED_UNSUPPORTED : ControlResult::REFUSED_NOT_READY;
}

bool Session::link_down_() const { return this->link_status_.connected_known && !this->link_status_.connected; }

bool Session::ready_for_command_() const {
  if (!this->writes_permitted_) {
    return false;
  }
  return this->stage_ == SessionStage::PROBE || this->stage_ == SessionStage::SCHEDULE ||
         this->stage_ == SessionStage::RUNNING;
}

bool Session::enqueue_command_(const ByteBuf &request, uint32_t now_ms, uint8_t detail, TagKind kind) {
  if (request.size == 0) {
    return false;
  }
  // One copy, never retried: the controller may act on every copy it hears, and a lost reply
  // does not mean it did nothing.
  if (!this->enqueue_(request, Priority::USER, kind, detail, Delivery::ONCE)) {
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

bool Session::zone_missing_(uint8_t zone) const { return !at(this->state_.available, zone - 1).value_or(true); }

bool Session::queued_(uint8_t zone) const {
  const std::optional<QueueList> &queue = this->state_.queue;
  for (uint8_t i = 0; queue.has_value() && i < queue->count; i++) {
    if (queue->slots[i].zone == zone) {
      return true;
    }
  }
  return false;
}

bool Session::next_queued_zone_(uint8_t skip, uint8_t *zone) const {
  const std::optional<QueueList> &queue = this->state_.queue;
  uint8_t active = this->state_.active_zone.value_or(0);
  for (uint8_t i = 0; queue.has_value() && i < queue->count; i++) {
    uint8_t queued = queue->slots[i].zone;
    if (queued != skip && queued != active) {
      *zone = queued;
      return true;
    }
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
  uint8_t program = 0;  // the program a command is for, kept in its tag
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
      if (queue && !this->caps_.has(Capability::QUEUE_RUN)) {
        return this->unsupported_(Capability::QUEUE_RUN);
      }
      uint8_t minutes = static_cast<uint8_t>(command.value);
      request = queue ? request_queue_station(0, command.zone, minutes) : request_run_station(command.zone, minutes);
      if (!queue) {
        return this->enqueue_command_(request, now_ms, command.zone, TagKind::ZONE_START) ? ControlResult::QUEUED
                                                                                          : ControlResult::REFUSED_BUSY;
      }
      break;
    }
    case ControlOp::VALVE_OFF: {
      if (command.zone < 1 || command.zone > zones) {
        return ControlResult::REFUSED_RANGE;
      }
      uint32_t start_tag = tag(TagKind::ZONE_START, command.zone);
      size_t cancelled = this->link_.cancel_queued(start_tag);
      // Cancel intent separately from reported state. An unsent start needs no stop; one
      // already on the wire may be watering before its first status reading arrives.
      if (this->unconfirmed_starts_[command.zone - 1] || this->link_.in_flight(start_tag)) {
        request = request_stop();
        break;
      }
      if (!at(this->state_.running, command.zone - 1).value_or(false)) {
        if (cancelled > 0) {
          return ControlResult::QUEUED;
        }
        return this->queued_(command.zone) ? ControlResult::REFUSED_QUEUED_ONLY : ControlResult::REFUSED_NOT_RUNNING;
      }
      uint8_t next = 0;
      bool advance = this->config_.valve_queue && this->config_.experimental;
      // Until the 42 probe is answered, 40 could cancel zones a supported 42 would keep.
      if (advance && !this->caps_.known(Capability::ADVANCE)) {
        return ControlResult::REFUSED_NOT_READY;
      }
      if (advance && this->caps_.has(Capability::ADVANCE)) {
        // Until the queue has been read, 40 could cancel zones queued behind this one.
        if (!this->state_.queue.has_value()) {
          return ControlResult::REFUSED_NOT_READY;
        }
        // Nothing else queued: stopping is turning this zone off.
        request = this->next_queued_zone_(command.zone, &next) ? request_advance(next) : request_stop();
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
      if (!this->caps_.has(Capability::ADVANCE)) {
        return this->unsupported_(Capability::ADVANCE);
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
      program = command.program;
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
      if (!this->caps_.has(Capability::CONTROLLER_STATE)) {
        return this->unsupported_(Capability::CONTROLLER_STATE);
      }
      request = request_set_controller_state(command.value == 0 ? 0 : 1);
      break;
    case ControlOp::TEST_ZONES:
      if (!this->config_.experimental) {
        return ControlResult::REFUSED_EXPERIMENTAL;
      }
      if (!this->caps_.has(Capability::TEST_STATIONS)) {
        return this->unsupported_(Capability::TEST_STATIONS);
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
      // One clock set at a time: the time in flight sends its own date once it is acknowledged (handle_).
      if (this->date_after_time_) {
        return ControlResult::REFUSED_BUSY;
      }
      if (!this->enqueue_command_(request_set_time(command.hour, command.minute, command.second), now_ms)) {
        return ControlResult::REFUSED_BUSY;
      }
      this->date_after_time_ = true;
      this->date_to_set_ = date;
      return ControlResult::QUEUED;
    }
  }
  if (!this->enqueue_command_(request, now_ms, program)) {
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
  this->start_next_edit_();
}

void Session::start_next_edit_() {
  ByteBuf read;
  if (!this->editor_.next(&read)) {
    return;
  }
  if (!this->enqueue_(read, Priority::USER, TagKind::PAGE, this->editor_.page())) {
    // Reported, because each caller was told its edit was queued. Nothing would start the
    // rest later either, so they go too.
    this->editor_.cancel();
    this->editor_.drop_waiting();
    SessionEvent event;
    event.kind = SessionEventKind::EDIT_FAILED;
    event.reason = "the link could not take a page read, and the edits waiting were dropped";
    this->push_event_(event);
  }
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
  // Off and the second odd code are pyrainbird's reading, not seen on a TM2 (SPEC pages 10-12).
  if (field == Field::FREQUENCY && value > FREQ_EVEN && !this->config_.experimental) {
    return ControlResult::REFUSED_EXPERIMENTAL;
  }
  ByteBuf read;
  switch (this->editor_.ask(PageChange{field, zone, program, index, value}, &read)) {
    case ScheduleEditor::Asked::READ:
      if (!this->enqueue_(read, Priority::USER, TagKind::PAGE, this->editor_.page())) {
        this->editor_.cancel();
        return ControlResult::REFUSED_BUSY;
      }
      return ControlResult::QUEUED;
    case ScheduleEditor::Asked::JOINED:
    case ScheduleEditor::Asked::WAITING:
      return ControlResult::QUEUED;
    case ScheduleEditor::Asked::FULL:
      return ControlResult::REFUSED_BUSY;
    case ScheduleEditor::Asked::RANGE:
      break;
  }
  return ControlResult::REFUSED_RANGE;
}

void Session::handle_page_(const Completion &done) {
  if (!this->editor_.active()) {
    return;
  }
  ByteBuf next;
  ScheduleEditor::Step step = this->editor_.on_reply(done, &next);
  if ((step == ScheduleEditor::Step::WRITE || step == ScheduleEditor::Step::UNCHANGED) && this->editor_.refused() > 0) {
    // The rest of the page still goes ahead; whoever asked for this one was told it was queued.
    SessionEvent event;
    event.kind = SessionEventKind::EDIT_FAILED;
    event.reason = "a field could not be changed on its page; the other changes to that page were kept";
    this->push_event_(event);
  }
  switch (step) {
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
    case ScheduleEditor::Step::UNCHANGED:
      this->start_next_edit_();
      return;
    case ScheduleEditor::Step::FAILED:
      this->fail_edit_(this->editor_.reason());
      return;
  }
}

uint32_t Session::desired_interval(uint32_t now_ms) const {
  // With the link down, the watering values are the last ones read and may never change again.
  if (this->stage_ != SessionStage::RUNNING || this->link_down_()) {
    return this->config_.update_interval_ms;
  }
  if (this->watering_() || (this->hold_ && !elapsed(now_ms, this->hold_from_, this->config_.fast_hold_ms))) {
    return std::min(this->config_.fast_interval_ms, this->config_.update_interval_ms);
  }
  return this->config_.update_interval_ms;
}

}  // namespace esphome::rainbird
