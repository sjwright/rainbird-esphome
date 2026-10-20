#include "manchester.h"

namespace esphome::rainbird {

int manchester_halves(int32_t us) {
  if (us < 0) {
    us = -us;
  }
  if (us >= ONE_HALF_MIN_US && us <= ONE_HALF_MAX_US) {
    return 1;
  }
  if (us >= TWO_HALF_MIN_US && us <= TWO_HALF_MAX_US) {
    return 2;
  }
  return 0;
}

void PolarityLock::note_good(Polarity polarity, Phase phase) {
  if (this->locked_) {
    // Only a frame at the locked polarity clears the miss streak.
    if (this->polarity_ == polarity && this->phase_ == phase) {
      this->bad_ = 0;
    }
    return;
  }
  if (this->seen_ && this->polarity_ == polarity && this->phase_ == phase) {
    this->good_++;
  } else {
    this->polarity_ = polarity;
    this->phase_ = phase;
    this->seen_ = true;
    this->good_ = 1;
  }
  if (this->good_ >= POLARITY_LOCK_GOOD) {
    this->locked_ = true;
  }
}

void PolarityLock::note_bad() {
  this->good_ = 0;
  this->seen_ = false;
  if (!this->locked_) {
    return;
  }
  this->bad_++;
  if (this->bad_ >= POLARITY_UNLOCK_BAD) {
    this->locked_ = false;
    this->bad_ = 0;
  }
}

namespace {

class Emitter {
 public:
  explicit Emitter(Polarity polarity) : invert_(polarity == Polarity::INVERTED) {}

  void half(bool high) {
    if (this->invert_) {
      high = !high;
    }
    int sign = high ? 1 : -1;
    if (this->have_ && this->sign_ == sign) {
      this->us_ += HALF_BIT_US;
      return;
    }
    this->flush();
    this->sign_ = sign;
    this->us_ = HALF_BIT_US;
    this->have_ = true;
  }

  std::vector<int32_t> take() {
    this->flush();
    return this->out_;
  }

 private:
  void flush() {
    if (!this->have_) {
      return;
    }
    this->out_.push_back(this->sign_ * this->us_);
    this->have_ = false;
  }

  bool invert_;
  bool have_{false};
  int sign_{1};
  int32_t us_{0};
  std::vector<int32_t> out_;
};

}  // namespace

std::vector<int32_t> manchester_encode(const uint8_t *data, size_t size, Polarity polarity, Phase phase) {
  Emitter emitter(polarity);
  if (data == nullptr) {
    return emitter.take();
  }
  // The extra half is a leading high in logical polarity. Inversion flips it with the data.
  if (phase == Phase::EXTRA_HALF) {
    emitter.half(true);
  }
  for (size_t i = 0; i < size; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      bool one = ((data[i] >> bit) & 0x01) != 0;
      // High then low is 1. Low then high is 0. Same levels merge into a 416 µs pulse.
      if (one) {
        emitter.half(true);
        emitter.half(false);
      } else {
        emitter.half(false);
        emitter.half(true);
      }
    }
  }
  return emitter.take();
}

namespace {

// remote_receiver times out on a steady level and reports that stretch as one
// long pulse. The last Manchester half shares that level, so it is absorbed
// into the pulse. decode_rmt_ then appends one more idle sample at the
// opposite level. Every remaining entry is that tail, not another bit.
bool trailing_idle(const int32_t *timings, size_t count, size_t at) {
  if (at >= count) {
    return false;
  }
  for (size_t i = at; i < count; i++) {
    if (manchester_halves(timings[i]) != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

ManchesterBits manchester_decode_bits(const int32_t *timings, size_t count, Polarity polarity, Phase phase) {
  ManchesterBits out;
  if (timings == nullptr || count == 0) {
    return out;
  }
  std::vector<uint8_t> levels;
  bool stopped_on_idle = false;
  for (size_t i = 0; i < count; i++) {
    int halves = manchester_halves(timings[i]);
    if (halves == 0) {
      stopped_on_idle = trailing_idle(timings, count, i);
      if (!stopped_on_idle) {
        out.ended_early = true;
      }
      break;
    }
    bool high = timings[i] > 0;
    if (polarity == Polarity::INVERTED) {
      high = !high;
    }
    for (int h = 0; h < halves; h++) {
      levels.push_back(high ? 1 : 0);
    }
  }
  size_t index = 0;
  if (phase == Phase::EXTRA_HALF) {
    if (levels.empty()) {
      return out;
    }
    index = 1;
  }
  while (index + 1 < levels.size()) {
    uint8_t first = levels[index];
    uint8_t second = levels[index + 1];
    // A Manchester bit is a transition. Two halves at the same level are not a bit.
    if (first == second) {
      out.ended_early = true;
      break;
    }
    out.bits.push_back(first == 1 ? 1 : 0);
    index += 2;
  }
  // The idle swallowed the last half. The half still in hand is the first half
  // of that bit, and Manchester's other half is the opposite level.
  if (stopped_on_idle && !out.ended_early && index + 1 == levels.size()) {
    out.bits.push_back(levels[index] == 1 ? 1 : 0);
  }
  return out;
}

}  // namespace esphome::rainbird
