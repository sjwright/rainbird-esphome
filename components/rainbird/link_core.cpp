#include "link_core.h"

#include <cstring>

#include "elapsed.h"

namespace esphome::rainbird {

uint32_t frame_air_ms(size_t bytes) {
  // One bit is 1/2400 s = 5/12 ms.
  return static_cast<uint32_t>((bytes * 8 * 5 + 11) / 12);
}

LinkCore::LinkCore(const LinkTiming &timing) : timing_(timing) {}

bool LinkCore::enqueue(const ByteBuf &request, Priority priority, uint32_t tag, Delivery delivery) {
  if (request.size == 0 || request.size > MAX_PAYLOAD || this->count_ >= QUEUE_CAP) {
    return false;
  }
  Pending &slot = this->queue_[this->count_++];
  slot.request = request;
  slot.priority = priority;
  slot.tag = tag;
  slot.attempts = 0;
  slot.delivery = delivery;
  return true;
}

size_t LinkCore::queued(Priority priority) const {
  size_t count = 0;
  for (size_t i = 0; i < this->count_; i++) {
    if (this->queue_[i].priority == priority) {
      count++;
    }
  }
  if (this->busy_ && this->current_.priority == priority) {
    count++;
  }
  return count;
}

void LinkCore::drop_queued(Priority priority) {
  size_t kept = 0;
  for (size_t i = 0; i < this->count_; i++) {
    if (this->queue_[i].priority != priority) {
      this->queue_[kept++] = this->queue_[i];
    }
  }
  this->count_ = kept;
}

bool LinkCore::take_next_(Pending *out) {
  if (this->count_ == 0) {
    return false;
  }
  // Highest priority first; within a priority, the oldest. A retry is put at the front.
  size_t best = 0;
  for (size_t i = 1; i < this->count_; i++) {
    if (static_cast<uint8_t>(this->queue_[i].priority) < static_cast<uint8_t>(this->queue_[best].priority)) {
      best = i;
    }
  }
  *out = this->queue_[best];
  for (size_t i = best; i + 1 < this->count_; i++) {
    this->queue_[i] = this->queue_[i + 1];
  }
  this->count_--;
  return true;
}

bool LinkCore::push_front_(const Pending &pending) {
  if (this->count_ >= QUEUE_CAP) {
    return false;
  }
  for (size_t i = this->count_; i > 0; i--) {
    this->queue_[i] = this->queue_[i - 1];
  }
  this->queue_[0] = pending;
  this->count_++;
  return true;
}

void LinkCore::complete_(const Completion &completion) {
  if (this->done_count_ >= DONE_CAP) {
    // The session drains completions every loop, so this only drops under misuse. Keep the newest.
    for (size_t i = 0; i + 1 < DONE_CAP; i++) {
      this->done_[i] = this->done_[i + 1];
    }
    this->done_count_ = DONE_CAP - 1;
  }
  this->done_[this->done_count_++] = completion;
}

void LinkCore::fail_(const Pending &pending) {
  Completion failed;
  failed.tag = pending.tag;
  failed.outcome = Outcome::FAILED;
  failed.opcode = pending.request.data[0];
  failed.attempts = pending.attempts;
  this->complete_(failed);
}

bool LinkCore::pop_completion(Completion *out) {
  if (out == nullptr || this->done_count_ == 0) {
    return false;
  }
  *out = this->done_[0];
  for (size_t i = 0; i + 1 < this->done_count_; i++) {
    this->done_[i] = this->done_[i + 1];
  }
  this->done_count_--;
  return true;
}

void LinkCore::tick(uint32_t now_ms) {
  if (!this->busy_ || !elapsed(now_ms, this->sent_at_, this->first_copy_ms_ + this->timing_.timeout_ms)) {
    return;
  }
  this->busy_ = false;
  this->link_errors_++;
  this->ready_from_ = now_ms;
  this->ready_delay_ = this->timing_.gap_ms;
  uint8_t retries = this->current_.delivery == Delivery::NORMAL ? this->timing_.retries : 0;
  if (this->current_.attempts <= retries && this->push_front_(this->current_)) {
    return;
  }
  if (this->consecutive_failures_ < 255) {
    this->consecutive_failures_++;
  }
  this->fail_(this->current_);
}

bool LinkCore::next_transmission(uint32_t now_ms, Transmission *out) {
  if (out == nullptr || this->busy_ || !elapsed(now_ms, this->ready_from_, this->ready_delay_)) {
    return false;
  }
  Pending next;
  if (!this->take_next_(&next)) {
    return false;
  }
  *out = Transmission{};
  size_t first = encode_frame(next.request.data, next.request.size, false, out->bytes, sizeof(out->bytes));
  if (first == 0) {
    // enqueue() bounds the size, so this should not happen. Fail the request rather than
    // drop it: whoever queued it is waiting for a completion.
    this->fail_(next);
    return false;
  }
  out->size = first;
  if (this->timing_.repeat_short && next.delivery != Delivery::ONCE && next.request.size == 2) {
    // The second copy follows with no gap. Encoding both as one byte run keeps it gapless.
    std::memcpy(out->bytes + first, out->bytes, first);
    out->size = first * 2;
  }
  out->first_copy_ms = frame_air_ms(first);
  out->total_ms = frame_air_ms(out->size);

  next.attempts++;
  this->current_ = next;
  this->busy_ = true;
  this->sent_at_ = now_ms;
  this->first_copy_ms_ = out->first_copy_ms;
  this->total_ms_ = out->total_ms;
  return true;
}

bool LinkCore::on_frame(const uint8_t *payload, size_t size, uint32_t now_ms) {
  Reply reply;
  bool parsed = parse_reply(payload, size, &reply);
  return this->on_frame(payload, size, parsed ? &reply : nullptr, now_ms);
}

bool LinkCore::on_frame(const uint8_t *payload, size_t size, const Reply *parsed, uint32_t now_ms) {
  if (payload == nullptr || size == 0 || size > MAX_PAYLOAD) {
    return false;
  }
  if (this->busy_ && reply_matches(this->current_.request.data, this->current_.request.size, payload, size)) {
    Completion done;
    done.tag = this->current_.tag;
    done.opcode = this->current_.request.data[0];
    done.attempts = this->current_.attempts;
    uint32_t since_sent = now_ms - this->sent_at_;
    done.response_ms = since_sent >= this->first_copy_ms_ ? since_sent - this->first_copy_ms_ : 0;
    if (parsed == nullptr) {
      // Right opcode, wrong length. Leave the request waiting; the timeout retries it.
      this->unexpected_++;
      return false;
    }
    done.reply = *parsed;
    done.outcome = done.reply.kind == ReplyKind::NAK ? Outcome::NAK : Outcome::REPLY;
    this->replies_++;
    this->busy_ = false;
    this->consecutive_failures_ = 0;
    // Do not start the next request while our duplicate copy may still be on the wire.
    if (since_sent < this->total_ms_) {
      this->ready_from_ = this->sent_at_;
      this->ready_delay_ = this->total_ms_ + this->timing_.gap_ms;
    } else {
      this->ready_from_ = now_ms;
      this->ready_delay_ = this->timing_.gap_ms;
    }
    std::memcpy(this->last_reply_, payload, size);
    this->last_reply_size_ = size;
    this->last_reply_at_ = now_ms;
    this->complete_(done);
    return true;
  }
  // A second identical reply soon after is the answer to the duplicate copy (SPEC 8.5).
  if (size == this->last_reply_size_ && std::memcmp(payload, this->last_reply_, size) == 0 &&
      !elapsed(now_ms, this->last_reply_at_, this->timing_.timeout_ms)) {
    this->duplicates_++;
    return false;
  }
  this->unexpected_++;
  return false;
}

}  // namespace esphome::rainbird
