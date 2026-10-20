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

void FrameParser::erase_at(size_t index) {
  this->bits_.erase(this->bits_.begin() + static_cast<std::vector<uint8_t>::difference_type>(index));
}

uint8_t FrameParser::byte_at(size_t bit) const {
  uint8_t value = 0;
  for (size_t i = 0; i < 8; i++) {
    value = static_cast<uint8_t>((value << 1) | this->bits_[bit + i]);
  }
  return value;
}

bool FrameParser::find_preamble(size_t *at) const {
  if (this->bits_.size() < 24) {
    return false;
  }
  for (size_t i = 0; i + 24 <= this->bits_.size(); i++) {
    uint32_t window = 0;
    for (size_t b = 0; b < 24; b++) {
      window = (window << 1) | this->bits_[i + b];
    }
    if (window == 0xF5F51C) {
      *at = i;
      return true;
    }
  }
  return false;
}

void FrameParser::pump() {
  while (true) {
    if (this->bits_.size() > 4096) {
      this->bits_.erase(this->bits_.begin(), this->bits_.end() - 64);
    }
    size_t at = 0;
    if (!this->find_preamble(&at)) {
      return;
    }
    if (this->bits_.size() < at + 32) {
      if (!this->finishing_) {
        return;
      }
      this->erase_at(at);
      continue;
    }
    uint8_t length = this->byte_at(at + 24);
    if (length > MAX_PAYLOAD) {
      this->erase_at(at);
      continue;
    }
    // 24-bit preamble, then len, payload, checksum and end.
    size_t total = 24 + 8 * (static_cast<size_t>(length) + 3);
    if (this->bits_.size() < at + total) {
      if (!this->finishing_) {
        return;
      }
      this->erase_at(at);
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
      this->erase_at(at);
      continue;
    }
    this->ready_.push_back(frame);
    this->bits_.erase(this->bits_.begin(),
                      this->bits_.begin() + static_cast<std::vector<uint8_t>::difference_type>(at + total));
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
