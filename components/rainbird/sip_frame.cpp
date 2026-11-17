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
  size_t i = 0;
  bool fits = true;
  auto put = [&](uint8_t byte) {
    if (i >= cap) {
      fits = false;
      return;
    }
    out[i++] = byte;
  };
  auto put_escaped = [&](uint8_t byte) {
    if (frame_needs_escape(byte)) {
      put(FRAME_ESCAPE);
      put(frame_escape_code(byte));
    } else {
      put(byte);
    }
  };
  put(FRAME_PREAMBLE);
  put(FRAME_PREAMBLE);
  if (reply) {
    put(FRAME_PREAMBLE);
  }
  put(FRAME_START);
  put_escaped(static_cast<uint8_t>(length));
  for (size_t p = 0; p < length; p++) {
    put_escaped(payload[p]);
  }
  put_escaped(frame_checksum(static_cast<uint8_t>(length), payload));
  put(FRAME_END);
  return fits ? i : 0;
}

namespace {

uint8_t byte_at(const std::vector<uint8_t> &bits, size_t bit) {
  uint8_t value = 0;
  for (size_t i = 0; i < 8; i++) {
    value = static_cast<uint8_t>((value << 1) | bits[bit + i]);
  }
  return value;
}

// One frame whose length byte starts at `bit`. Returns the bits it used, or 0.
size_t read_frame(const std::vector<uint8_t> &bits, size_t bit, bool unescape, Frame *frame) {
  size_t at = bit;
  uint8_t escaped = 0;
  uint8_t plain = 0;
  auto next = [&](uint8_t *out) {
    if (at + 8 > bits.size()) {
      return false;
    }
    uint8_t byte = byte_at(bits, at);
    at += 8;
    if (unescape && byte == FRAME_ESCAPE) {
      if (at + 8 > bits.size()) {
        return false;
      }
      uint8_t code = byte_at(bits, at);
      at += 8;
      byte = frame_unescape(code);
      if (byte == 0) {
        return false;  // 1B followed by something that is not 01, 02 or 03
      }
      escaped |= frame_escape_bit(byte);
    } else if (frame_needs_escape(byte)) {
      plain |= frame_escape_bit(byte);
    }
    *out = byte;
    return true;
  };
  uint8_t length = 0;
  if (!next(&length) || length > MAX_PAYLOAD) {
    return 0;
  }
  *frame = Frame{};
  frame->length = length;
  for (uint8_t i = 0; i < length; i++) {
    if (!next(&frame->payload[i])) {
      return 0;
    }
  }
  uint8_t checksum = 0;
  if (!next(&checksum) || checksum != frame_checksum(length, frame->payload)) {
    return 0;
  }
  if (at + 8 > bits.size() || byte_at(bits, at) != FRAME_END) {
    return 0;
  }
  at += 8;
  frame->escaped = escaped;
  frame->plain = plain;
  return at - bit;
}

}  // namespace

std::vector<Frame> find_frames(const std::vector<uint8_t> &bits) {
  std::vector<Frame> frames;
  size_t at = 0;
  // F5 F5 1C, then the length decides the rest.
  while (at + 32 <= bits.size()) {
    if (byte_at(bits, at) != FRAME_PREAMBLE || byte_at(bits, at + 8) != FRAME_PREAMBLE ||
        byte_at(bits, at + 16) != FRAME_START) {
      at++;
      continue;
    }
    Frame frame{};
    // Plain first, so a controller that does not escape still decodes as it did before.
    size_t used = read_frame(bits, at + 24, false, &frame);
    if (used == 0) {
      used = read_frame(bits, at + 24, true, &frame);
    }
    if (used == 0) {
      // A preamble inside the bytes this one claimed may still start a real frame.
      at++;
      continue;
    }
    frames.push_back(frame);
    at += 24 + used;
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
