#pragma once

#include <cstdint>

namespace esphome::rainbird {

// The remaining-time sensor counts down between controller readings (PLAN 6.3), so Home
// Assistant sees time pass smoothly. Pure, so the rules run in host tests; the hub owns the timer.
class Countdown {
 public:
  // A controller reading at now_ms. True when the sensor should show it. Every non-zero
  // reading re-anchors the count, because a paused controller repeats its value and counting
  // on from the old anchor would run down to 0. A steady 0 is shown once.
  bool reading(uint16_t seconds, uint32_t now_ms) {
    bool show = !this->known_ || seconds > 0 || this->from_ != 0;
    this->known_ = true;
    this->from_ = seconds;
    this->at_ = now_ms;
    return show;
  }

  // Seconds left at now_ms, counting down from the last reading.
  uint32_t remaining(uint32_t now_ms) const {
    uint32_t gone = (now_ms - this->at_) / 1000;
    return this->from_ > gone ? this->from_ - gone : 0;
  }

 private:
  bool known_{false};
  uint16_t from_{0};
  uint32_t at_{0};
};

// When the time remaining runs out, as a wall-clock instant: one value Home Assistant can count
// down to by itself, in place of a sensor that changes every second. Pure, like Countdown.
class EndTime {
 public:
  // Two readings of a steady countdown land within a second of each other. More than this apart
  // and the end really has moved: the controller paused, or another zone started.
  static constexpr int64_t TOLERANCE_S = 2;

  // A controller reading of `seconds` left, taken at `now` (seconds since the epoch). True when
  // the end to show has changed; end() is then the new one.
  bool reading(uint16_t seconds, int64_t now) {
    if (seconds == 0) {
      return this->set_(0);
    }
    int64_t end = now + seconds;
    int64_t moved = end > this->end_ ? end - this->end_ : this->end_ - end;
    if (this->end_ != 0 && moved <= TOLERANCE_S) {
      return false;
    }
    return this->set_(end);
  }
  // Nothing is known any more (the link went down). True when that changes what is shown.
  bool clear() { return this->set_(0); }

  // Seconds since the epoch, or 0 when nothing is running.
  int64_t end() const { return this->end_; }

 private:
  bool set_(int64_t end) {
    bool changed = !this->known_ || end != this->end_;
    this->known_ = true;
    this->end_ = end;
    return changed;
  }

  bool known_{false};
  int64_t end_{0};
};

}  // namespace esphome::rainbird
