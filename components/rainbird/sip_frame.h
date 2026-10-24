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
// F5 F5 F5 1C, length, checksum and 1D around a reply's payload.
static constexpr size_t REPLY_FRAME_OVERHEAD = 7;

// (len + sum(payload)) & 0xFF. The length byte is included.
uint8_t frame_checksum(uint8_t length, const uint8_t *payload);

struct Frame {
  uint8_t payload[MAX_PAYLOAD]{};
  uint8_t length{0};
};

// Request: F5 F5 1C. Reply: F5 F5 F5 1C. Returns 0 if the payload is too long or out is short.
size_t encode_frame(const uint8_t *payload, size_t length, bool reply, uint8_t *out, size_t cap);

// Every frame in one finished burst of bits (1 or 0, MSB first). Hunts for F5 F5 1C at every
// bit; a reply's extra leading F5 sits in front of that same sequence, so one pattern covers
// both. The end byte is taken by count, because 1D is legal inside the payload. A frame cut
// off by the end of the burst is dropped.
std::vector<Frame> find_frames(const std::vector<uint8_t> &bits);

struct BurstDecode {
  std::vector<Frame> frames;
  Polarity polarity{Polarity::NORMAL};
  Phase phase{Phase::ALIGNED};
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
