#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "manchester.h"

namespace esphome::rainbird {

static constexpr uint8_t FRAME_PREAMBLE = 0xF5;
static constexpr uint8_t FRAME_START = 0x1C;
static constexpr uint8_t FRAME_END = 0x1D;
static constexpr size_t MAX_PAYLOAD = 64;
static constexpr size_t MAX_FRAME_BYTES = 4 + 1 + MAX_PAYLOAD + 1 + 1;

// (len + sum(payload)) & 0xFF. The length byte is included.
uint8_t frame_checksum(uint8_t length, const uint8_t *payload);

struct Frame {
  uint8_t payload[MAX_PAYLOAD]{};
  uint8_t length{0};
};

// Request: F5 F5 1C. Reply: F5 F5 F5 1C. Returns 0 if the payload is too long or out is short.
size_t encode_frame(const uint8_t *payload, size_t length, bool reply, uint8_t *out, size_t cap);

// Hunts for F5 F5 1C on every bit. A reply's extra leading F5 sits in front of that
// same sequence, so one pattern covers both. The end byte is taken by count, because
// 1D is legal inside the payload.
class FrameParser {
 public:
  void reset();
  void push_bit(bool one);
  // A short burst will not grow more bits. Incomplete candidates are dropped here.
  void finish();
  bool pop(Frame *out);

 private:
  void pump();
  bool find_preamble(size_t *at);
  uint8_t byte_at(size_t bit) const;

  std::vector<uint8_t> bits_;
  std::vector<Frame> ready_;
  // Offsets before scan_from_ cannot start a preamble. Each new bit therefore
  // costs one rolling-window step, not a rescan of the whole buffer.
  size_t scan_from_{0};
  bool finishing_{false};
};

struct BurstDecode {
  std::vector<Frame> frames;
  Polarity polarity{Polarity::NORMAL};
  Phase phase{Phase::ALIGNED};
  bool ended_early{false};
};

BurstDecode decode_burst(const int32_t *timings, size_t count, Polarity polarity, Phase phase);

// Both polarities and both phases. The pair that yields the most frames wins.
// A tie keeps the earlier pair: normal polarity, no extra half-bit.
BurstDecode decode_burst_unlocked(const int32_t *timings, size_t count);

// One of these per receiver. While locked, only that polarity and phase are tried.
// A frame at the other polarity must count as a miss, so searching all four again
// would hide the misses that are supposed to unlock it.
class ReceiverDecoder {
 public:
  BurstDecode decode(const int32_t *timings, size_t count);
  const PolarityLock &lock() const { return this->lock_; }

 private:
  PolarityLock lock_{};
};

}  // namespace esphome::rainbird
