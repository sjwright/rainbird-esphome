#include "doctest.h"

#include "hex.h"
#include "manchester.h"

#include <vector>

using namespace esphome::rainbird;

static std::vector<int32_t> jitter(const std::vector<int32_t> &timings, bool up) {
  std::vector<int32_t> out;
  for (int32_t us : timings) {
    int sign = us < 0 ? -1 : 1;
    int32_t mag = us < 0 ? -us : us;
    int32_t scaled = up ? mag * 6 / 5 : mag * 4 / 5;
    out.push_back(sign * scaled);
  }
  return out;
}

static std::vector<int32_t> invert(const std::vector<int32_t> &timings) {
  std::vector<int32_t> out;
  for (int32_t us : timings) {
    out.push_back(-us);
  }
  return out;
}

TEST_CASE("half-bit windows are ±30%") {
  CHECK(manchester_halves(145) == 1);
  CHECK(manchester_halves(144) == 0);
  CHECK(manchester_halves(270) == 1);
  CHECK(manchester_halves(271) == 0);
  CHECK(manchester_halves(291) == 2);
  CHECK(manchester_halves(290) == 0);
  CHECK(manchester_halves(540) == 2);
  CHECK(manchester_halves(541) == 0);
  CHECK(manchester_halves(208) == 1);
  CHECK(manchester_halves(-208) == 1);
  CHECK(manchester_halves(416) == 2);
  CHECK(manchester_halves(1000) == 0);
}

TEST_CASE("F5 encodes to merged half-bits") {
  const uint8_t f5 = 0xF5;
  auto timings = manchester_encode(&f5, 1, Polarity::NORMAL, Phase::ALIGNED);
  // 11110101, high-low = 1. Adjacent halves at the same level become one 416 µs pulse.
  const int32_t expect[] = {208, -208, 208, -208, 208, -208, 208, -416, 416, -416, 416, -208};
  REQUIRE(timings.size() == sizeof(expect) / sizeof(expect[0]));
  for (size_t i = 0; i < timings.size(); i++) {
    CHECK(timings[i] == expect[i]);
  }
}

TEST_CASE("extra half-bit merges into the leading high") {
  const uint8_t f5 = 0xF5;
  auto timings = manchester_encode(&f5, 1, Polarity::NORMAL, Phase::EXTRA_HALF);
  CHECK(timings[0] == 416);
}

TEST_CASE("round trip survives jitter, inversion and a leading half-bit") {
  auto payload = from_hex("39 00 01 06");
  auto clean = manchester_encode(payload.data(), payload.size(), Polarity::NORMAL, Phase::ALIGNED);

  auto check = [&](const std::vector<int32_t> &timings, Polarity polarity, Phase phase) {
    auto bits = manchester_decode_bits(timings.data(), timings.size(), polarity, phase);
    CHECK_FALSE(bits.ended_early);
    REQUIRE(bits.bits.size() == payload.size() * 8);
    for (size_t i = 0; i < payload.size(); i++) {
      uint8_t byte = 0;
      for (int bit = 0; bit < 8; bit++) {
        byte = static_cast<uint8_t>((byte << 1) | bits.bits[i * 8 + bit]);
      }
      CHECK(byte == payload[i]);
    }
  };

  check(clean, Polarity::NORMAL, Phase::ALIGNED);
  check(jitter(clean, true), Polarity::NORMAL, Phase::ALIGNED);
  check(jitter(clean, false), Polarity::NORMAL, Phase::ALIGNED);
  check(invert(clean), Polarity::INVERTED, Phase::ALIGNED);

  std::vector<int32_t> led = clean;
  led.insert(led.begin(), HALF_BIT_US);
  check(led, Polarity::NORMAL, Phase::EXTRA_HALF);

  auto reply = manchester_encode(payload.data(), payload.size(), Polarity::INVERTED, Phase::EXTRA_HALF);
  check(reply, Polarity::INVERTED, Phase::EXTRA_HALF);
  check(jitter(reply, true), Polarity::INVERTED, Phase::EXTRA_HALF);
}

TEST_CASE("a width outside both windows ends the attempt") {
  auto payload = from_hex("40");
  auto timings = manchester_encode(payload.data(), payload.size(), Polarity::NORMAL, Phase::ALIGNED);
  timings.insert(timings.begin() + 2, 1000);
  auto bits = manchester_decode_bits(timings.data(), timings.size(), Polarity::NORMAL, Phase::ALIGNED);
  CHECK(bits.ended_early);
  CHECK(bits.bits.size() < 8);
}

TEST_CASE("polarity locks after three good frames and unlocks after five bad ones") {
  PolarityLock lock;
  lock.note_good(Polarity::NORMAL, Phase::ALIGNED);
  lock.note_good(Polarity::NORMAL, Phase::ALIGNED);
  CHECK_FALSE(lock.locked());
  lock.note_good(Polarity::NORMAL, Phase::ALIGNED);
  CHECK(lock.locked());
  CHECK(lock.polarity() == Polarity::NORMAL);
  CHECK(lock.phase() == Phase::ALIGNED);

  for (int i = 0; i < 4; i++) {
    lock.note_bad();
  }
  CHECK(lock.locked());
  // A good frame at the other polarity must not clear the miss streak.
  lock.note_good(Polarity::INVERTED, Phase::EXTRA_HALF);
  lock.note_bad();
  CHECK_FALSE(lock.locked());

  PolarityLock again;
  again.note_good(Polarity::NORMAL, Phase::ALIGNED);
  again.note_good(Polarity::NORMAL, Phase::ALIGNED);
  again.note_bad();
  again.note_good(Polarity::NORMAL, Phase::ALIGNED);
  again.note_good(Polarity::NORMAL, Phase::ALIGNED);
  CHECK_FALSE(again.locked());
  again.note_good(Polarity::NORMAL, Phase::ALIGNED);
  CHECK(again.locked());

  PolarityLock shifted;
  shifted.note_good(Polarity::INVERTED, Phase::EXTRA_HALF);
  shifted.note_good(Polarity::NORMAL, Phase::ALIGNED);
  shifted.note_good(Polarity::NORMAL, Phase::ALIGNED);
  CHECK_FALSE(shifted.locked());
  shifted.note_good(Polarity::NORMAL, Phase::ALIGNED);
  CHECK(shifted.locked());
  CHECK(shifted.polarity() == Polarity::NORMAL);
}
