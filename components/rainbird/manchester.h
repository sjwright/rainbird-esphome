#pragma once

#include <cstdint>
#include <vector>

namespace esphome::rainbird {

// 2400 bit/s Manchester. One bit is 416 µs, two halves of 208 µs.
// Windows are ±30% using integer math. 271–290 µs is in neither window.
// A pulse in neither window ends the attempt, unless it is the idle tail.
static constexpr int32_t HALF_BIT_US = 208;
static constexpr int32_t BIT_US = HALF_BIT_US * 2;
static constexpr int32_t ONE_HALF_MIN_US = HALF_BIT_US * 70 / 100;   // 145
static constexpr int32_t ONE_HALF_MAX_US = HALF_BIT_US * 130 / 100;  // 270
static constexpr int32_t TWO_HALF_MIN_US = BIT_US * 70 / 100;        // 291
static constexpr int32_t TWO_HALF_MAX_US = BIT_US * 130 / 100;       // 540

// Positive timings are mark (high), negative are space (low), matching remote_base.
// Returns 1, 2, or 0 when the width is neither.
int manchester_halves(int32_t us);

enum class Polarity : uint8_t { NORMAL = 0, INVERTED = 1 };
enum class Phase : uint8_t { ALIGNED = 0, EXTRA_HALF = 1 };

// Three good frames lock a receiver; five bad frames in a row unlock it.
// One lock per receiver. Nothing here is global, so several hubs can listen at once.
static constexpr uint8_t POLARITY_LOCK_GOOD = 3;
static constexpr uint8_t POLARITY_UNLOCK_BAD = 5;

class PolarityLock {
 public:
  void note_good(Polarity polarity, Phase phase);
  void note_bad();

  bool locked() const { return this->locked_; }
  Polarity polarity() const { return this->polarity_; }
  Phase phase() const { return this->phase_; }

 private:
  bool locked_{false};
  bool seen_{false};
  uint8_t good_{0};
  uint8_t bad_{0};
  Polarity polarity_{Polarity::NORMAL};
  Phase phase_{Phase::ALIGNED};
};

struct ManchesterBits {
  std::vector<uint8_t> bits;  // 1 or 0, MSB of each byte first once framed
  bool ended_early{false};    // a mid-burst width outside both windows, or two equal half-bits
};

// EXTRA_HALF prepends one high half-bit before polarity is applied. Replies on the
// optocoupler start that way, and the two directions are inverted from each other,
// so a search tries all four combinations until a receiver locks.
std::vector<int32_t> manchester_encode(const uint8_t *data, size_t size, Polarity polarity, Phase phase);
ManchesterBits manchester_decode_bits(const int32_t *timings, size_t count, Polarity polarity, Phase phase);

}  // namespace esphome::rainbird
