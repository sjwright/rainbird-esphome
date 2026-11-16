#include "burst_assembler.h"

namespace esphome::rainbird {

void BurstAssembler::close_() {
  // The line has sat at level_ since the last edge. That stretch holds the frame's last
  // half-bit and the idle after it, and its real length is not known or needed.
  this->open_.push_back(this->level_ ? static_cast<int32_t>(IDLE_US) : -static_cast<int32_t>(IDLE_US));
  this->closed_.swap(this->open_);
  this->open_.clear();
  this->have_edge_ = false;
  this->overflowed_ = false;
}

bool BurstAssembler::check_idle(uint32_t now_us) {
  // Unsigned subtraction, so the 71-minute wrap of a 32-bit microsecond clock does no harm.
  if (!this->have_edge_ || now_us - this->last_time_ < IDLE_US) {
    return false;
  }
  this->close_();
  return true;
}

bool BurstAssembler::add_edge(uint32_t time_us, bool level_after) {
  if (!this->have_edge_) {
    this->have_edge_ = true;
    this->last_time_ = time_us;
    this->level_ = level_after;
    return false;
  }
  uint32_t width = time_us - this->last_time_;
  if (width >= IDLE_US) {
    // The loop did not get to check_idle() before the next burst began. This edge starts it.
    this->close_();
    this->have_edge_ = true;
    this->last_time_ = time_us;
    this->level_ = level_after;
    return true;
  }
  if (level_after == this->level_) {
    // Not a change of level: an edge was missed, or a pulse came and went before the level
    // was read. Nothing to record; the current pulse runs on.
    return false;
  }
  if (width < FILTER_US) {
    // A glitch. Drop it and join the levels either side, as if the line had never moved.
    this->glitches_++;
    if (this->open_.empty()) {
      // The burst began with it, so there is no burst yet.
      this->have_edge_ = false;
      return false;
    }
    int32_t before = this->open_.back();
    this->open_.pop_back();
    this->last_time_ -= static_cast<uint32_t>(before < 0 ? -before : before);
    this->level_ = level_after;
    return false;
  }
  if (this->open_.size() + 1 >= MAX_PULSES) {
    // Leave room for the idle sample. The burst is cut off here and fails its checksum.
    if (!this->overflowed_) {
      this->overflowed_ = true;
      this->overflows_++;
    }
  } else {
    this->open_.push_back(this->level_ ? static_cast<int32_t>(width) : -static_cast<int32_t>(width));
  }
  this->last_time_ = time_us;
  this->level_ = level_after;
  return false;
}

}  // namespace esphome::rainbird
