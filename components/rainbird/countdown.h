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

}  // namespace esphome::rainbird
