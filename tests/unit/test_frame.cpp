#include "doctest.h"

#include "hex.h"
#include "sip_frame.h"
#include "vectors.h"

#include <vector>

using namespace esphome::rainbird;

static std::vector<Frame> frames_from_bytes(const uint8_t *data, size_t size) {
  FrameParser parser;
  for (size_t i = 0; i < size; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      parser.push_bit(((data[i] >> bit) & 0x01) != 0);
    }
  }
  parser.finish();
  std::vector<Frame> frames;
  Frame frame;
  while (parser.pop(&frame)) {
    frames.push_back(frame);
  }
  return frames;
}

static void check_payload(const Frame &frame, const std::vector<uint8_t> &payload) {
  REQUIRE(frame.length == payload.size());
  for (size_t i = 0; i < payload.size(); i++) {
    CHECK(frame.payload[i] == payload[i]);
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
      CHECK_FALSE(burst.ended_early);
      REQUIRE(burst.frames.size() == 1);
      check_payload(burst.frames[0], payload);
    }
  }

  auto reply = manchester_encode(frame, n, Polarity::INVERTED, Phase::EXTRA_HALF);
  auto timings = receiver_tail(reply, true);
  auto burst = decode_burst(timings.data(), timings.size(), Polarity::INVERTED, Phase::EXTRA_HALF);
  CHECK_FALSE(burst.ended_early);
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
