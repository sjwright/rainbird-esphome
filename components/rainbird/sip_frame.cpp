#include "sip_frame.h"

namespace esphome::rainbird {

uint8_t frame_checksum(uint8_t length, const uint8_t *payload) {
  uint16_t sum = length;
  if (payload != nullptr) {
    for (uint8_t i = 0; i < length; i++) {
      sum += payload[i];
    }
  }
  return static_cast<uint8_t>(sum);
}

size_t encode_frame(const uint8_t *payload, size_t length, bool reply, uint8_t *out, size_t cap) {
  if (out == nullptr || length > MAX_PAYLOAD || (length > 0 && payload == nullptr)) {
    return 0;
  }
  size_t size = (reply ? 3 : 2) + 1 + 1 + length + 1 + 1;
  if (cap < size) {
    return 0;
  }
  size_t i = 0;
  out[i++] = FRAME_PREAMBLE;
  out[i++] = FRAME_PREAMBLE;
  if (reply) {
    out[i++] = FRAME_PREAMBLE;
  }
  out[i++] = FRAME_START;
  out[i++] = static_cast<uint8_t>(length);
  for (size_t p = 0; p < length; p++) {
    out[i++] = payload[p];
  }
  out[i++] = frame_checksum(static_cast<uint8_t>(length), payload);
  out[i++] = FRAME_END;
  return i;
}

void FrameParser::reset() {
  this->bits_.clear();
  this->ready_.clear();
  this->scan_from_ = 0;
  this->finishing_ = false;
}

void FrameParser::push_bit(bool one) {
  this->bits_.push_back(one ? 1 : 0);
  this->pump();
}

void FrameParser::finish() {
  this->finishing_ = true;
  this->pump();
  this->finishing_ = false;
}

bool FrameParser::pop(Frame *out) {
  if (out == nullptr || this->ready_.empty()) {
    return false;
  }
  *out = this->ready_.front();
  this->ready_.erase(this->ready_.begin());
  return true;
}

uint8_t FrameParser::byte_at(size_t bit) const {
  uint8_t value = 0;
  for (size_t i = 0; i < 8; i++) {
    value = static_cast<uint8_t>((value << 1) | this->bits_[bit + i]);
  }
  return value;
}

bool FrameParser::find_preamble(size_t *at) {
  size_t size = this->bits_.size();
  if (size < 24 || this->scan_from_ + 24 > size) {
    return false;
  }
  uint32_t window = 0;
  for (size_t b = 0; b < 23; b++) {
    window = (window << 1) | this->bits_[this->scan_from_ + b];
  }
  for (size_t i = this->scan_from_; i + 24 <= size; i++) {
    window = ((window << 1) | this->bits_[i + 23]) & 0xFFFFFF;
    if (window == 0xF5F51C) {
      // Stay on this offset. An incomplete frame finds it again in one step.
      this->scan_from_ = i;
      *at = i;
      return true;
    }
  }
  // No preamble starts before the last 23 bits.
  this->scan_from_ = size - 23;
  return false;
}

void FrameParser::pump() {
  while (true) {
    size_t at = 0;
    if (!this->find_preamble(&at)) {
      // Bits that cannot start a preamble are dropped once there are many of them.
      if (this->scan_from_ > 4096) {
        this->bits_.erase(this->bits_.begin(),
                          this->bits_.begin() + static_cast<std::vector<uint8_t>::difference_type>(this->scan_from_));
        this->scan_from_ = 0;
      }
      return;
    }
    if (this->bits_.size() < at + 32) {
      if (!this->finishing_) {
        return;
      }
      this->scan_from_ = at + 1;
      continue;
    }
    uint8_t length = this->byte_at(at + 24);
    if (length > MAX_PAYLOAD) {
      this->scan_from_ = at + 1;
      continue;
    }
    // 24-bit preamble, then len, payload, checksum and end.
    size_t total = 24 + 8 * (static_cast<size_t>(length) + 3);
    if (this->bits_.size() < at + total) {
      if (!this->finishing_) {
        return;
      }
      this->scan_from_ = at + 1;
      continue;
    }
    Frame frame{};
    frame.length = length;
    for (uint8_t i = 0; i < length; i++) {
      frame.payload[i] = this->byte_at(at + 32 + static_cast<size_t>(i) * 8);
    }
    uint8_t checksum = this->byte_at(at + 32 + static_cast<size_t>(length) * 8);
    uint8_t end = this->byte_at(at + 40 + static_cast<size_t>(length) * 8);
    if (checksum != frame_checksum(length, frame.payload) || end != FRAME_END) {
      this->scan_from_ = at + 1;
      continue;
    }
    this->ready_.push_back(frame);
    this->bits_.erase(this->bits_.begin(),
                      this->bits_.begin() + static_cast<std::vector<uint8_t>::difference_type>(at + total));
    this->scan_from_ = 0;
  }
}

BurstDecode decode_burst(const int32_t *timings, size_t count, Polarity polarity, Phase phase) {
  BurstDecode out;
  out.polarity = polarity;
  out.phase = phase;
  ManchesterBits decoded = manchester_decode_bits(timings, count, polarity, phase);
  out.ended_early = decoded.ended_early;
  FrameParser parser;
  for (uint8_t bit : decoded.bits) {
    parser.push_bit(bit != 0);
  }
  parser.finish();
  Frame frame;
  while (parser.pop(&frame)) {
    out.frames.push_back(frame);
  }
  return out;
}

BurstDecode ReceiverDecoder::decode(const int32_t *timings, size_t count) {
  BurstDecode decoded;
  if (this->lock_.locked()) {
    decoded = decode_burst(timings, count, this->lock_.polarity(), this->lock_.phase());
  } else {
    decoded = decode_burst_unlocked(timings, count);
  }
  if (decoded.frames.empty()) {
    this->lock_.note_bad();
    return decoded;
  }
  for (size_t i = 0; i < decoded.frames.size(); i++) {
    this->lock_.note_good(decoded.polarity, decoded.phase);
  }
  return decoded;
}

BurstDecode decode_burst_unlocked(const int32_t *timings, size_t count) {
  const Polarity polarities[] = {Polarity::NORMAL, Polarity::NORMAL, Polarity::INVERTED, Polarity::INVERTED};
  const Phase phases[] = {Phase::ALIGNED, Phase::EXTRA_HALF, Phase::ALIGNED, Phase::EXTRA_HALF};
  BurstDecode best = decode_burst(timings, count, polarities[0], phases[0]);
  for (int i = 1; i < 4; i++) {
    BurstDecode candidate = decode_burst(timings, count, polarities[i], phases[i]);
    if (candidate.frames.size() > best.frames.size()) {
      best = candidate;
    }
  }
  return best;
}

}  // namespace esphome::rainbird
