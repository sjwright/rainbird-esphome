#include "doctest.h"

#include "hex.h"
#include "model_limits.h"
#include "sip_frame.h"
#include "vectors.h"

#include <algorithm>
#include <vector>

using namespace esphome::rainbird;

static std::vector<Frame> frames_from_bytes(const uint8_t *data, size_t size) {
  std::vector<uint8_t> bits;
  for (size_t i = 0; i < size; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      bits.push_back((data[i] >> bit) & 0x01);
    }
  }
  return find_frames(bits);
}

static void check_payload(const Frame &frame, const std::vector<uint8_t> &payload) {
  REQUIRE(frame.length == payload.size());
  for (size_t i = 0; i < payload.size(); i++) {
    CHECK(frame.payload[i] == payload[i]);
  }
}

TEST_CASE("escaped TM2 queue replies fit the configured RMT capacity") {
  // 7452 seconds is 138 minutes at 90 percent seasonal adjust. Both bytes escape.
  std::vector<uint8_t> payload{0xBB, 1};
  for (uint8_t zone = 1; zone <= 11; zone++) {
    payload.insert(payload.end(), {static_cast<uint8_t>(0x20 | zone), 0x1D, 0x1C});
  }
  uint8_t frame[MAX_FRAME_BYTES];
  size_t size = encode_frame(payload.data(), payload.size(), true, frame, sizeof(frame));
  REQUIRE(size == 64);
  auto pulses = manchester_encode(frame, size, Polarity::INVERTED, Phase::EXTRA_HALF);
  CHECK((pulses.size() + 1) / 2 > 384);
  CHECK((pulses.size() + 1) / 2 <= TM2_RECEIVE_SYMBOLS);
  CHECK(decode_burst_unlocked(pulses.data(), 384 * 2).frames.empty());
  auto decoded = decode_burst_unlocked(pulses.data(), pulses.size());
  REQUIRE(decoded.frames.size() == 1);
  check_payload(decoded.frames[0], payload);

  // Bound every valid queue, rather than just this example. Adding pulse counts for
  // separate segments overestimates their concatenation, where equal levels can merge.
  auto byte_pulses = [](uint8_t byte) {
    uint8_t bytes[] = {byte, 0};
    size_t count = 1;
    if (frame_needs_escape(byte)) {
      bytes[0] = FRAME_ESCAPE;
      bytes[1] = byte - FRAME_ESCAPE_OFFSET;
      count = 2;
    }
    return manchester_encode(bytes, count, Polarity::NORMAL, Phase::ALIGNED).size();
  };
  size_t max_time = 0;
  size_t max_zone = 0;
  for (unsigned byte = 0; byte < 256; byte++) {
    max_time = std::max(max_time, byte_pulses(byte));
    if ((byte & 0x1F) >= 1 && (byte & 0x1F) <= TM2_ZONES) {
      max_zone = std::max(max_zone, byte_pulses(byte));
    }
  }
  const uint8_t header[] = {0xF5, 0xF5, 0xF5, 0x1C, 35, 0xBB, 1};
  const uint8_t end[] = {FRAME_END};
  size_t fixed = manchester_encode(header, sizeof(header), Polarity::NORMAL, Phase::ALIGNED).size() +
                 manchester_encode(end, sizeof(end), Polarity::NORMAL, Phase::ALIGNED).size();
  for (size_t occupied = 0; occupied <= 11; occupied++) {
    // Empty slots are zero-filled; the checksum may escape too. Reserve four extra pulses
    // for phase and the receiver's leading/trailing idle samples.
    size_t bound = fixed + occupied * (max_zone + 2 * max_time) + (11 - occupied) * 3 * byte_pulses(0) + max_time + 4;
    CHECK((bound + 1) / 2 <= TM2_RECEIVE_SYMBOLS);
  }
}

TEST_CASE("worked examples match the spec frames") {
  struct Golden {
    const char *payload;
    bool reply;
    const char *frame;
  };
  const Golden rows[] = {
      {"39 00 01 06", false, "F5 F5 1C 04 39 00 01 06 44 1D"},
      {"40", false, "F5 F5 1C 01 40 41 1D"},
      {"01 40", true, "F5 F5 F5 1C 02 01 40 43 1D"},
  };
  for (const auto &row : rows) {
    auto payload = from_hex(row.payload);
    auto expect = from_hex(row.frame);
    uint8_t frame[MAX_FRAME_BYTES];
    size_t n = encode_frame(payload.data(), payload.size(), row.reply, frame, sizeof(frame));
    REQUIRE(n == expect.size());
    CHECK(to_hex(frame, n) == to_hex(expect.data(), expect.size()));
    auto parsed = frames_from_bytes(frame, n);
    REQUIRE(parsed.size() == 1);
    check_payload(parsed[0], payload);
  }
}

TEST_CASE("every spec payload round-trips as a frame") {
  for (int i = 0; i < SPEC_VECTOR_COUNT; i++) {
    auto payload = from_hex(SPEC_VECTORS[i].hex);
    uint8_t frame[MAX_FRAME_BYTES];
    size_t n = encode_frame(payload.data(), payload.size(), SPEC_VECTORS[i].reply, frame, sizeof(frame));
    REQUIRE_MESSAGE(n > 0, SPEC_VECTORS[i].name);
    auto parsed = frames_from_bytes(frame, n);
    REQUIRE_MESSAGE(parsed.size() == 1, SPEC_VECTORS[i].name);
    check_payload(parsed[0], payload);
  }
}

TEST_CASE("1D inside the payload is not the end of the frame") {
  auto payload = from_hex("39 1D 01 06");
  uint8_t frame[MAX_FRAME_BYTES];
  size_t n = encode_frame(payload.data(), payload.size(), false, frame, sizeof(frame));
  auto parsed = frames_from_bytes(frame, n);
  REQUIRE(parsed.size() == 1);
  check_payload(parsed[0], payload);
}

TEST_CASE("bad checksum, end byte or length yields no frame") {
  auto bad_sum = from_hex("F5 F5 1C 01 40 40 1D");
  CHECK(frames_from_bytes(bad_sum.data(), bad_sum.size()).empty());

  auto bad_end = from_hex("F5 F5 1C 01 40 41 00");
  CHECK(frames_from_bytes(bad_end.data(), bad_end.size()).empty());

  auto bad_len = from_hex("F5 F5 1C 02 40 41 00 1D");
  CHECK(frames_from_bytes(bad_len.data(), bad_len.size()).empty());

  std::vector<uint8_t> too_long = from_hex("F5 F5 1C 41");
  too_long.insert(too_long.end(), 67, 0x00);
  CHECK(frames_from_bytes(too_long.data(), too_long.size()).empty());
}

TEST_CASE("a bad frame does not swallow the next one") {
  auto bytes = from_hex("F5 F5 1C 01 40 40 1D F5 F5 1C 04 39 00 01 06 44 1D");
  auto parsed = frames_from_bytes(bytes.data(), bytes.size());
  REQUIRE(parsed.size() == 1);
  check_payload(parsed[0], from_hex("39 00 01 06"));
}

TEST_CASE("two frames back to back with no gap") {
  // LNK2 sends a 2-byte request twice in one burst. 3F 00 checksum is 0x41.
  auto bytes = from_hex("F5 F5 1C 02 3F 00 41 1D F5 F5 1C 02 3F 00 41 1D");
  auto parsed = frames_from_bytes(bytes.data(), bytes.size());
  REQUIRE(parsed.size() == 2);
  check_payload(parsed[0], from_hex("3F 00"));
  check_payload(parsed[1], from_hex("3F 00"));

  auto timings = manchester_encode(bytes.data(), bytes.size(), Polarity::NORMAL, Phase::ALIGNED);
  auto burst = decode_burst(timings.data(), timings.size(), Polarity::NORMAL, Phase::ALIGNED);
  REQUIRE(burst.frames.size() == 2);
  check_payload(burst.frames[0], from_hex("3F 00"));
  check_payload(burst.frames[1], from_hex("3F 00"));
}

TEST_CASE("unlocked decode finds inverted replies with a leading half-bit") {
  auto payload = from_hex("01 40");
  uint8_t frame[MAX_FRAME_BYTES];
  size_t n = encode_frame(payload.data(), payload.size(), true, frame, sizeof(frame));
  auto timings = manchester_encode(frame, n, Polarity::INVERTED, Phase::EXTRA_HALF);
  auto burst = decode_burst_unlocked(timings.data(), timings.size());
  CHECK(burst.polarity == Polarity::INVERTED);
  CHECK(burst.phase == Phase::EXTRA_HALF);
  REQUIRE(burst.frames.size() == 1);
  check_payload(burst.frames[0], payload);

  auto normal = manchester_encode(frame, n, Polarity::NORMAL, Phase::ALIGNED);
  auto straight = decode_burst_unlocked(normal.data(), normal.size());
  CHECK(straight.polarity == Polarity::NORMAL);
  CHECK(straight.phase == Phase::ALIGNED);
  REQUIRE(straight.frames.size() == 1);
}

static std::vector<int32_t> receiver_tail(std::vector<int32_t> timings, bool append_opposite) {
  int sign = timings.back() < 0 ? -1 : 1;
  timings.back() = sign * 4000;
  if (append_opposite) {
    timings.push_back(-sign * 4000);
  }
  return timings;
}

TEST_CASE("remote_receiver-shaped bursts still decode") {
  auto payload = from_hex("02");
  uint8_t frame[MAX_FRAME_BYTES];
  size_t n = encode_frame(payload.data(), payload.size(), false, frame, sizeof(frame));
  REQUIRE(n > 0);

  const Polarity polarities[] = {Polarity::NORMAL, Polarity::INVERTED};
  for (Polarity polarity : polarities) {
    auto clean = manchester_encode(frame, n, polarity, Phase::ALIGNED);
    for (bool append_opposite : {false, true}) {
      auto timings = receiver_tail(clean, append_opposite);
      const char *which = polarity == Polarity::INVERTED ? "inverted" : "normal";
      INFO(which);
      auto burst = decode_burst(timings.data(), timings.size(), polarity, Phase::ALIGNED);
      REQUIRE(burst.frames.size() == 1);
      check_payload(burst.frames[0], payload);
    }
  }

  auto reply = manchester_encode(frame, n, Polarity::INVERTED, Phase::EXTRA_HALF);
  auto timings = receiver_tail(reply, true);
  auto burst = decode_burst(timings.data(), timings.size(), Polarity::INVERTED, Phase::EXTRA_HALF);
  REQUIRE(burst.frames.size() == 1);
  check_payload(burst.frames[0], payload);
}

TEST_CASE("looped-back bytes decode and lock that receiver's polarity") {
  // send_raw emits a request at normal polarity. A jumper from the transmitter
  // pin to the receiver pin brings those same timings back.
  auto payload = from_hex("02");
  uint8_t frame[MAX_FRAME_BYTES];
  size_t n = encode_frame(payload.data(), payload.size(), false, frame, sizeof(frame));
  REQUIRE(n > 0);
  auto timings = manchester_encode(frame, n, Polarity::NORMAL, Phase::ALIGNED);

  ReceiverDecoder decoder;
  for (int i = 0; i < 3; i++) {
    BurstDecode decoded = decoder.decode(timings.data(), timings.size());
    REQUIRE(decoded.frames.size() == 1);
    check_payload(decoded.frames[0], payload);
  }
  CHECK(decoder.lock().locked());
  CHECK(decoder.lock().polarity() == Polarity::NORMAL);
  CHECK(decoder.lock().phase() == Phase::ALIGNED);

  auto inverted = manchester_encode(frame, n, Polarity::INVERTED, Phase::ALIGNED);
  BurstDecode missed = decoder.decode(inverted.data(), inverted.size());
  CHECK(missed.frames.empty());
  CHECK(decoder.lock().locked());

  ReceiverDecoder other;
  for (int i = 0; i < 3; i++) {
    BurstDecode decoded = other.decode(inverted.data(), inverted.size());
    REQUIRE(decoded.frames.size() == 1);
    check_payload(decoded.frames[0], payload);
  }
  CHECK(other.lock().polarity() == Polarity::INVERTED);
  for (int i = 0; i < 4; i++) {
    other.decode(timings.data(), timings.size());
    CHECK(other.lock().locked());
  }
  other.decode(timings.data(), timings.size());
  CHECK_FALSE(other.lock().locked());
}

TEST_CASE("a frame after thousands of junk bits still decodes") {
  // Junk is alternating bits, which never contain F5 F5 1C.
  std::vector<uint8_t> stream(700, 0x55);
  auto payload = from_hex("40");
  uint8_t frame[MAX_FRAME_BYTES];
  size_t n = encode_frame(payload.data(), payload.size(), false, frame, sizeof(frame));
  REQUIRE(n > 0);
  stream.insert(stream.end(), frame, frame + n);
  auto frames = frames_from_bytes(stream.data(), stream.size());
  REQUIRE(frames.size() == 1);
  check_payload(frames[0], payload);
}

TEST_CASE("a preamble inside a rejected frame is still found") {
  // The first frame claims more payload than follows and fails its checksum. The
  // second frame starts inside the bytes the first one claimed.
  auto second = from_hex("12");
  uint8_t good[MAX_FRAME_BYTES];
  size_t n = encode_frame(second.data(), second.size(), false, good, sizeof(good));
  REQUIRE(n > 0);
  std::vector<uint8_t> stream = {0xF5, 0xF5, 0x1C, 0x05, 0x01};
  stream.insert(stream.end(), good, good + n);
  auto frames = frames_from_bytes(stream.data(), stream.size());
  REQUIRE(frames.size() == 1);
  check_payload(frames[0], second);
}

TEST_CASE("1B, 1C and 1D are escaped on the way out") {
  uint8_t frame[MAX_FRAME_BYTES];
  // Set time 12:56:29. 29 is 1D, the end byte.
  const uint8_t set_time[] = {0x11, 0x0C, 0x38, 0x1D};
  size_t size = encode_frame(set_time, sizeof(set_time), false, frame, sizeof(frame));
  CHECK(to_hex(frame, size) == "F5 F5 1C 04 11 0C 38 1B 03 76 1D");

  const uint8_t all[] = {0x1B, 0x1C, 0x1D};
  size = encode_frame(all, sizeof(all), false, frame, sizeof(frame));
  // Checksum 03 + 1B + 1C + 1D = 57.
  CHECK(to_hex(frame, size) == "F5 F5 1C 03 1B 01 1B 02 1B 03 57 1D");

  // A length of 1D is escaped as well.
  uint8_t long_payload[0x1D] = {};
  size = encode_frame(long_payload, sizeof(long_payload), false, frame, sizeof(frame));
  CHECK(to_hex(frame, 6) == "F5 F5 1C 1B 03 00");
}

TEST_CASE("escaped frames decode, checked by their checksum") {
  // The 4C reply recorded on 13 Nov 2026 with the clock at 12:56:29.
  auto wire = from_hex("F5 F5 F5 1C 10 CC 0C 38 1B 03 0D B7 EA 00 00 01 01 FF FF 00 00 00 EB 1D");
  std::vector<Frame> frames = frames_from_bytes(wire.data(), wire.size());
  REQUIRE(frames.size() == 1);
  check_payload(frames[0], from_hex("CC 0C 38 1D 0D B7 EA 00 00 01 01 FF FF 00 00 00"));
  CHECK(frames[0].escaped == frame_escape_bit(0x1D));

  // What encode_frame writes, find_frames reads back.
  const uint8_t all[] = {0x1B, 0x1C, 0x1D, 0x00, 0x1B};
  uint8_t frame[MAX_FRAME_BYTES];
  size_t size = encode_frame(all, sizeof(all), true, frame, sizeof(frame));
  frames = frames_from_bytes(frame, size);
  REQUIRE(frames.size() == 1);
  check_payload(frames[0], std::vector<uint8_t>(all, all + sizeof(all)));
  CHECK(frames[0].escaped == (frame_escape_bit(0x1B) | frame_escape_bit(0x1C) | frame_escape_bit(0x1D)));

  // A plain frame still decodes plain, with no escapes noted.
  auto plain = from_hex("F5 F5 F5 1C 10 CC 0C 38 1E 0D B7 EA 00 00 01 01 FF FF 00 00 00 EC 1D");
  frames = frames_from_bytes(plain.data(), plain.size());
  REQUIRE(frames.size() == 1);
  CHECK(frames[0].escaped == 0);

  // A wrong checksum is still refused, escaped or not.
  auto bad = from_hex("F5 F5 F5 1C 10 CC 0C 38 1B 03 0D B7 EA 00 00 01 01 FF FF 00 00 00 EC 1D");
  CHECK(frames_from_bytes(bad.data(), bad.size()).empty());
  // 1B followed by anything but 01, 02 or 03 is not an escape.
  auto odd = from_hex("F5 F5 F5 1C 02 01 1B 05 21 1D");
  CHECK(frames_from_bytes(odd.data(), odd.size()).empty());
}
