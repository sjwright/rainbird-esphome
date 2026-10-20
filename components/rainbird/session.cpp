#include "session.h"

#include <algorithm>

namespace esphome::rainbird {

namespace {

// Optional commands probed with 04 at startup (PLAN 6.3). The probe answers are
// the only capability source; the model id is used for the D12 gate alone.
constexpr uint8_t PROBED[] = {0x4C, 0x4A, 0x42, 0x4B, 0x3D, 0x49, 0x3A};

constexpr uint32_t tag(TagKind kind, uint8_t detail) { return (static_cast<uint32_t>(kind) << 8) | detail; }

TagKind tag_kind(uint32_t value) { return static_cast<TagKind>((value >> 8) & 0xFF); }

// Unsigned elapsed time is correct across the millis() wrap and any idle spell.
bool elapsed(uint32_t now, uint32_t from, uint32_t delay) { return now - from >= delay; }

}  // namespace

Session::Session(const Model &model, const SessionConfig &config)
    : model_(model), config_(config), link_(config.timing) {}

void Session::clear_changed() {
  this->identity_changed_ = false;
  this->diagnostics_changed_ = false;
}

void Session::enqueue(const ByteBuf &request, Priority priority, TagKind kind, uint8_t detail) {
  this->link_.enqueue(request, priority, tag(kind, detail));
}

void Session::enqueue_list(const RequestList &list, Priority priority, TagKind kind, size_t *count) {
  for (uint8_t i = 0; i < list.count; i++) {
    if (this->link_.enqueue(list.items[i], priority, tag(kind, 0)) && count != nullptr) {
      (*count)++;
    }
  }
}

void Session::start(uint32_t now_ms) {
  (void) now_ms;
  if (this->config_.listen_only) {
    this->stage_ = SessionStage::PASSIVE;
    return;
  }
  if (!this->config_.polling) {
    this->stage_ = SessionStage::MANUAL;
    return;
  }
  this->identify();
}

void Session::identify() {
  this->stage_ = SessionStage::IDENTIFY;
  this->enqueue(request_model(), Priority::FOLLOW_UP, TagKind::IDENTIFY);
}

void Session::poll(uint32_t now_ms) {
  switch (this->stage_) {
    case SessionStage::PASSIVE:
      if (this->frame_seen_ && elapsed(now_ms, this->last_frame_at_, this->config_.silence_ms)) {
        this->set_connected(false);
      }
      return;
    case SessionStage::MANUAL:
      return;
    case SessionStage::IDLE:
      this->identify();
      return;
    case SessionStage::IDENTIFY:
    case SessionStage::PROBE:
    case SessionStage::SCHEDULE:
      return;
    case SessionStage::RUNNING:
      break;
  }
  // One cycle at a time. A cycle still queued or in flight is not doubled up.
  if (this->link_.queued(Priority::POLL) > 0) {
    return;
  }
  RequestList list;
  this->model_.short_poll(this->caps_, this->config_.used, &list);
  this->enqueue_list(list, Priority::POLL, TagKind::POLL);
  // The slow group runs on the first cycle and then every fourth (PLAN 6.4).
  if (this->poll_count_ % 4 == 0) {
    this->model_.slow_poll(this->caps_, this->config_.used, &list);
    this->enqueue_list(list, Priority::POLL, TagKind::POLL);
  }
  this->poll_count_++;
  if (this->schedule_due(now_ms)) {
    this->read_schedule(now_ms, Priority::POLL, nullptr);
  }
}

bool Session::schedule_due(uint32_t now_ms) const {
  if (!this->schedule_read_once_) {
    return false;
  }
  return this->schedule_stale_ || elapsed(now_ms, this->last_schedule_read_, this->config_.refresh_interval_ms);
}

void Session::read_schedule(uint32_t now_ms, Priority priority, size_t *count) {
  RequestList pages;
  this->model_.schedule_pages(this->config_.used, this->config_.zone_mask, this->config_.program_mask, &pages);
  this->enqueue_list(pages, priority, TagKind::SCHEDULE, count);
  this->last_schedule_read_ = now_ms;
  this->schedule_read_once_ = true;
  this->schedule_stale_ = false;
}

void Session::tick(uint32_t now_ms) {
  this->link_.tick(now_ms);
  Completion done;
  while (this->link_.pop_completion(&done)) {
    this->handle(done, now_ms);
  }
  if (this->link_.link_errors() != this->last_link_errors_) {
    this->last_link_errors_ = this->link_.link_errors();
    this->diagnostics_changed_ = true;
  }
}

bool Session::next_transmission(uint32_t now_ms, Transmission *out) {
  return this->link_.next_transmission(now_ms, out);
}

void Session::on_frame(const uint8_t *payload, size_t size, uint32_t now_ms) {
  this->last_frame_at_ = now_ms;
  this->frame_seen_ = true;
  if (this->stage_ == SessionStage::PASSIVE) {
    this->set_connected(true);
  }
  // Every valid reply updates state, matched or not. In listen_only mode these
  // are the replies to the LNK2's own polls.
  Reply reply;
  if (parse_reply(payload, size, &reply)) {
    this->model_.apply_reply(reply, &this->state_);
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
  if (this->stage_ != SessionStage::PASSIVE) {
    this->link_.on_frame(payload, size, now_ms);
    this->tick(now_ms);
  }
}

bool Session::send_raw(const uint8_t *payload, size_t size, uint32_t now_ms) {
  if (this->stage_ == SessionStage::PASSIVE || payload == nullptr || size == 0 || size > ByteBuf::CAP) {
    return false;
  }
  ByteBuf request;
  for (size_t i = 0; i < size; i++) {
    request.push(payload[i]);
  }
  if (!this->link_.enqueue(request, Priority::USER, tag(TagKind::RAW, 0))) {
    return false;
  }
  this->hold_ = true;
  this->hold_from_ = now_ms;
  return true;
}

void Session::note_unsupported(uint8_t opcode) {
  CommandSupport support;
  support.command = opcode;
  support.supported = false;
  this->model_.note_support(support, &this->caps_);
}

void Session::set_connected(bool connected) {
  if (!this->connected_known_ || this->connected_ != connected) {
    this->diagnostics_changed_ = true;
  }
  this->connected_known_ = true;
  this->connected_ = connected;
}

void Session::handle(const Completion &done, uint32_t now_ms) {
  if (done.outcome == Outcome::FAILED) {
    // Three requests in a row failing after their retries means the link is down.
    if (this->link_.consecutive_failures() >= 3) {
      this->set_connected(false);
    }
  } else {
    this->set_connected(true);
    this->response_known_ = true;
    this->response_ms_ = done.response_ms;
    this->diagnostics_changed_ = true;
  }
  // NAK bit 0 is "command not supported" (SPEC 3.3). Drop that capability.
  if (done.outcome == Outcome::NAK && nak_is_unsupported(done.reply.nak.code)) {
    this->note_unsupported(done.opcode);
  }

  switch (tag_kind(done.tag)) {
    case TagKind::IDENTIFY:
      if (done.outcome == Outcome::FAILED) {
        this->stage_ = SessionStage::IDLE;
        return;
      }
      if (done.outcome == Outcome::REPLY && done.reply.kind == ReplyKind::MODEL) {
        uint16_t id = done.reply.model.model_id;
        bool extra = std::find(this->config_.accept_ids.begin(), this->config_.accept_ids.end(), id) !=
                     this->config_.accept_ids.end();
        this->writes_permitted_ = this->model_.accepts(id) || extra;
      }
      this->stage_ = SessionStage::PROBE;
      this->enqueue(request_firmware(), Priority::FOLLOW_UP, TagKind::INFO);
      this->enqueue(request_serial(), Priority::FOLLOW_UP, TagKind::INFO);
      this->enqueue(request_stations(0), Priority::FOLLOW_UP, TagKind::INFO);
      this->probes_pending_ = 0;
      this->probe_answered_ = false;
      for (uint8_t command : PROBED) {
        this->enqueue(request_support(command), Priority::FOLLOW_UP, TagKind::PROBE, command);
        this->probes_pending_++;
      }
      return;
    case TagKind::PROBE:
      if (done.outcome == Outcome::REPLY && done.reply.kind == ReplyKind::SUPPORT) {
        this->model_.note_support(done.reply.support, &this->caps_);
        this->probe_answered_ = true;
      }
      if (this->probes_pending_ > 0 && --this->probes_pending_ == 0) {
        this->finish_probes(now_ms);
      }
      return;
    case TagKind::SCHEDULE:
      if (this->stage_ == SessionStage::SCHEDULE && this->schedule_pending_ > 0 && --this->schedule_pending_ == 0) {
        this->begin_running(now_ms);
      }
      return;
    case TagKind::INFO:
    case TagKind::POLL:
    case TagKind::RAW:
      return;
  }
}

void Session::finish_probes(uint32_t now_ms) {
  if (!this->probe_answered_) {
    // No 04 answer at all: try every optional command. A NAK clears the ones that fail.
    for (uint8_t command : PROBED) {
      CommandSupport support;
      support.command = command;
      support.supported = true;
      this->model_.note_support(support, &this->caps_);
    }
  }
  this->schedule_pending_ = 0;
  this->read_schedule(now_ms, Priority::FOLLOW_UP, &this->schedule_pending_);
  if (this->schedule_pending_ > 0) {
    this->stage_ = SessionStage::SCHEDULE;
  } else {
    this->begin_running(now_ms);
  }
}

void Session::begin_running(uint32_t now_ms) {
  this->stage_ = SessionStage::RUNNING;
  // First data now, rather than one update_interval later.
  this->poll(now_ms);
}

bool Session::watering() const {
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

uint32_t Session::desired_interval(uint32_t now_ms) const {
  if (this->stage_ != SessionStage::RUNNING) {
    return this->config_.update_interval_ms;
  }
  if (this->watering() || (this->hold_ && !elapsed(now_ms, this->hold_from_, this->config_.fast_hold_ms))) {
    return std::min(this->config_.fast_interval_ms, this->config_.update_interval_ms);
  }
  return this->config_.update_interval_ms;
}

}  // namespace esphome::rainbird
