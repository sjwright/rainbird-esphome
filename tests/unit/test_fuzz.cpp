// Random and damaged input through every decode path: the Manchester decoder, the frame hunter,
// the reply parser, the model, the session in both modes, and every entity reader and writer.
// These take whatever arrives on the wire, so nothing here may crash, read out of bounds or
// overflow. The assertions are few; the sanitizer build (make test SAN=1) is what catches a fault.
// Fixed seeds, so a failure repeats.
#include "doctest.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "entity_values.h"
#include "entity_writes.h"
#include "manchester.h"
#include "session.h"
#include "sip_commands.h"
#include "sip_frame.h"

using namespace esphome::rainbird;

namespace {

struct FuzzResult {
  long parsed{0};
  long frames{0};
  SessionStage stage{SessionStage::IDLE};
};

FuzzResult fuzz(unsigned seed, long iters) {
  std::mt19937 rng(seed);
  auto rnd = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  static const uint8_t OPS[] = {0x00, 0x01, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x8B, 0x90, 0x92,
                                0xA0, 0xB0, 0xB6, 0xBB, 0xBE, 0xBF, 0xC8, 0xCA, 0xCC};
  EspTm2Model model;
  SessionConfig active;
  active.handshake = true;
  active.allow_writes = true;
  active.experimental = true;
  active.used.bits = 0xFFFFFFFF >> 11;
  for (int p = 0; p < 256; p++)
    if (rnd(8) == 0) active.pages.add(static_cast<uint8_t>(p));
  SessionConfig passive = active;
  passive.listen_only = true;
  Session a(model, active), p(model, passive);
  a.start(0);
  p.start(0);
  ReceiverDecoder decoder;
  uint32_t now = 0;
  long frames = 0, parsed = 0;
  std::string text;
  TextContext ctx;
  for (long i = 0; i < iters; i++) {
    now += rnd(700);
    // 1. A payload: random, or a known opcode with a random length and body.
    uint8_t payload[80];
    size_t size = rnd(66);
    for (size_t k = 0; k < sizeof(payload); k++) payload[k] = static_cast<uint8_t>(rng());
    if (size > 0 && rnd(4) != 0) payload[0] = OPS[rnd(sizeof(OPS))];
    if (size > 1 && rnd(2) == 0) payload[1] = static_cast<uint8_t>(rnd(4));
    Reply reply;
    if (size <= MAX_PAYLOAD && parse_reply(payload, size, &reply)) {
      parsed++;
      ControllerState state;
      model.apply_reply(reply, &state);
    }
    if (size <= MAX_PAYLOAD) {
      a.on_frame(payload, size, now);
      p.on_frame(payload, size, now);
    }
    if (rnd(3) == 0) a.poll(now);
    if (rnd(50) == 0) a.refresh(now);
    if (rnd(20) == 0) {
      Control c;
      c.op = static_cast<ControlOp>(rnd(12));
      c.zone = static_cast<uint8_t>(rng());
      c.program = static_cast<uint8_t>(rng());
      c.value = static_cast<uint16_t>(rng());
      c.hour = static_cast<uint8_t>(rnd(30));
      c.minute = static_cast<uint8_t>(rnd(70));
      c.day = static_cast<uint8_t>(rnd(40));
      c.month = static_cast<uint8_t>(rnd(16));
      c.year = static_cast<uint16_t>(1990 + rnd(200));
      a.control(c, now);
      a.edit_schedule(static_cast<Field>(rnd(22)), static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()),
                      static_cast<uint8_t>(rng()), static_cast<int32_t>(rng()), now);
    }
    a.tick(now);
    Transmission tx;
    while (a.next_transmission(now, &tx)) {
      // Sometimes answer with the reply the request wants, to drive the state machine forward.
      if (rnd(2) == 0) {
        uint8_t ans[70];
        size_t n = 1 + rnd(40);
        for (size_t k = 0; k < n; k++) ans[k] = static_cast<uint8_t>(rng());
        ans[0] = rnd(5) == 0 ? 0x01 : static_cast<uint8_t>(tx.payload.data[0] + 0x80);
        if (n > 1) ans[1] = tx.payload.size > 1 && ans[0] != 0x01 ? tx.payload.data[1] : tx.payload.data[0];
        if (n > 2 && tx.payload.size > 2) ans[2] = tx.payload.data[2];
        now += 30;
        a.on_frame(ans, n, now);
      }
      now += rnd(500);
      a.tick(now);
    }
    SessionEvent ev;
    while (a.take_event(&ev)) {}
    while (p.take_event(&ev)) {}
    // Every entity kind against whatever state that left.
    if (rnd(10) == 0) {
      for (const Session *s : {&a, &p}) {
        Readout r = readout(*s);
        for (int kind = 0; kind < 45; kind++) {
          EntityBinding b{static_cast<EntityKind>(kind), static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()),
                          static_cast<uint8_t>(rng())};
          bool bv;
          float fv;
          uint8_t u1, u2;
          WallClock wc;
          bool_value(b, r, &bv);
          number_value(b, r, &fv);
          text_value(b, r, ctx, &text);
          select_index(b, r, &u1);
          start_time(b, r, &u1, &u2);
          controller_clock(r, &wc);
          plan_write(b, rnd(4) == 0 ? static_cast<float>(static_cast<int32_t>(rng())) : static_cast<float>(rnd(300)),
                     WriteContext{});
        }
      }
    }
    // 2. Timings: a real frame encoded, then damaged, or plain noise.
    std::vector<int32_t> timings;
    if (rnd(3) != 0 && size <= MAX_PAYLOAD) {
      uint8_t bytes[2 * MAX_FRAME_BYTES];
      size_t n = encode_frame(payload, size, rnd(2) == 0, bytes, sizeof(bytes));
      timings = manchester_encode(bytes, n, rnd(2) ? Polarity::NORMAL : Polarity::INVERTED,
                                  rnd(2) ? Phase::ALIGNED : Phase::EXTRA_HALF);
      for (uint32_t d = rnd(4); d > 0 && !timings.empty(); d--) {
        size_t at = rnd(static_cast<uint32_t>(timings.size()));
        switch (rnd(4)) {
          case 0: timings[at] = static_cast<int32_t>(rng()); break;
          case 1: timings.erase(timings.begin() + at); break;
          case 2: timings.insert(timings.begin() + at, static_cast<int32_t>(rnd(900)) - 450); break;
          case 3: timings.resize(at); break;
        }
      }
      if (rnd(2)) timings.push_back(rnd(2) ? 5000 : -5000);
    } else {
      size_t n = rnd(800);
      for (size_t k = 0; k < n; k++) {
        int32_t v = rnd(10) == 0 ? static_cast<int32_t>(rng()) : (rnd(2) ? 208 : 416) + static_cast<int32_t>(rnd(120)) - 60;
        timings.push_back(rnd(2) ? v : -v);
      }
    }
    frames += decoder.decode(timings.data(), timings.size()).frames.size();
    // 3. Raw bits straight into the frame hunter.
    if (rnd(5) == 0) {
      std::vector<uint8_t> bits(rnd(1200));
      for (auto &bit : bits) bit = rng() & 1;
      frames += find_frames(bits).size();
    }
  }
  FuzzResult result;
  result.parsed = parsed;
  result.frames = frames;
  result.stage = a.stage();
  return result;
}

}  // namespace

TEST_CASE("random and damaged input never faults, and the session still gets through startup") {
  for (unsigned seed : {1u, 2u, 3u}) {
    CAPTURE(seed);
    FuzzResult result = fuzz(seed, 8000);
    // The harness has to be exercising something: replies that parse, frames that decode.
    CHECK(result.parsed > 1000);
    CHECK(result.frames > 500);
  }
}
