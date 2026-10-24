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

std::vector<Frame> find_frames(const std::vector<uint8_t> &bits) {
  auto byte_at = [&bits](size_t bit) {
    uint8_t value = 0;
    for (size_t i = 0; i < 8; i++) {
      value = static_cast<uint8_t>((value << 1) | bits[bit + i]);
    }
    return value;
  };
  std::vector<Frame> frames;
  size_t at = 0;
  // F5 F5 1C and the length byte, then the length decides the rest.
  while (at + 32 <= bits.size()) {
    uint8_t length = byte_at(at + 24);
    // 24-bit preamble, then length, payload, checksum and end.
    size_t total = 24 + 8 * (static_cast<size_t>(length) + 3);
    if (byte_at(at) != FRAME_PREAMBLE || byte_at(at + 8) != FRAME_PREAMBLE || byte_at(at + 16) != FRAME_START ||
        length > MAX_PAYLOAD || at + total > bits.size()) {
      at++;
      continue;
    }
    Frame frame{};
    frame.length = length;
    for (uint8_t i = 0; i < length; i++) {
      frame.payload[i] = byte_at(at + 32 + static_cast<size_t>(i) * 8);
    }
    uint8_t checksum = byte_at(at + 32 + static_cast<size_t>(length) * 8);
    uint8_t end = byte_at(at + 40 + static_cast<size_t>(length) * 8);
    if (checksum != frame_checksum(length, frame.payload) || end != FRAME_END) {
      // A preamble inside the bytes this one claimed may still start a real frame.
      at++;
      continue;
    }
    frames.push_back(frame);
    at += total;
  }
  return frames;
}

BurstDecode decode_burst(const int32_t *timings, size_t count, Polarity polarity, Phase phase) {
  BurstDecode out;
  out.polarity = polarity;
  out.phase = phase;
  out.frames = find_frames(manchester_decode_bits(timings, count, polarity, phase).bits);
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
