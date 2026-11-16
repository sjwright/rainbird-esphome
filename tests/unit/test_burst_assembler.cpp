#include "doctest.h"

#include "burst_assembler.h"
#include "hex.h"
#include "manchester.h"
#include "sip_frame.h"
#include "vectors.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace esphome::rainbird;

#ifndef CAPTURES_DIR
#define CAPTURES_DIR "../captures"
#endif

namespace {

struct Edge {
  uint32_t time_us;
  bool level;  // the line level after the edge
};

using Burst = std::vector<int32_t>;

// The edges a line makes for these pulse widths, the first at `start`. The line idles at the
// level opposite the first pulse. A last pulse at the idle level has no edge at its end: it
// runs into the idle, as a frame's final half-bit does on the wire.
std::vector<Edge> edges_of(const Burst &timings, uint32_t start) {
  std::vector<Edge> edges;
  if (timings.empty()) {
    return edges;
  }
  bool idle = timings[0] < 0;
  uint32_t time = start;
  for (size_t i = 0; i < timings.size(); i++) {
    bool level = timings[i] > 0;
    edges.push_back(Edge{time, level});
    time += static_cast<uint32_t>(timings[i] < 0 ? -timings[i] : timings[i]);
    if (i + 1 == timings.size() && level != idle) {
      edges.push_back(Edge{time, idle});
    }
  }
  return edges;
}

// Every burst the assembler closes for these edges. `loop_us` is how often the loop looks for
// idle; 0 is never, which leaves it to the next burst's first edge and one check at the end.
std::vector<Burst> assemble(BurstAssembler &assembler, const std::vector<Edge> &edges, uint32_t loop_us = 0) {
  std::vector<Burst> bursts;
  for (size_t i = 0; i < edges.size(); i++) {
    if (loop_us != 0 && i > 0) {
      for (uint32_t now = edges[i - 1].time_us + loop_us; static_cast<int32_t>(edges[i].time_us - now) > 0;
           now += loop_us) {
        if (assembler.check_idle(now)) {
          bursts.push_back(assembler.burst());
        }
      }
    }
    if (assembler.add_edge(edges[i].time_us, edges[i].level)) {
      bursts.push_back(assembler.burst());
    }
  }
  if (!edges.empty() && assembler.check_idle(edges.back().time_us + BurstAssembler::IDLE_US)) {
    bursts.push_back(assembler.burst());
  }
  return bursts;
}

std::vector<std::string> frames_of(const Burst &burst, ReceiverDecoder *decoder = nullptr) {
  ReceiverDecoder fresh;
  BurstDecode decoded = (decoder != nullptr ? *decoder : fresh).decode(burst.data(), burst.size());
  std::vector<std::string> out;
  for (const Frame &frame : decoded.frames) {
    out.push_back(to_hex(frame.payload, frame.length));
  }
  return out;
}

// One line of the Saleae capture: the edges, in microseconds from the start of the capture.
// Channel 0 is the module's requests and channel 1 the controller's replies (WIRE.md).
std::vector<Edge> saleae_edges(int channel) {
  std::vector<Edge> edges;
  std::ifstream in(std::string(CAPTURES_DIR) + "/saleae/capture.csv");
  std::string line;
  std::getline(in, line);  // the header
  int last = -1;
  while (std::getline(in, line)) {
    double seconds = 0;
    int levels[2] = {0, 0};
    if (std::sscanf(line.c_str(), "%lf,%d,%d", &seconds, &levels[0], &levels[1]) != 3) {
      continue;
    }
    if (last != -1 && levels[channel] != last) {
      edges.push_back(Edge{static_cast<uint32_t>(seconds * 1e6 + 0.5), levels[channel] != 0});
    }
    last = levels[channel];
  }
  return edges;
}

// The capture as the assembler's bursts, and the frames each decodes to with nothing disturbed.
struct Captured {
  std::vector<std::vector<Edge>> bursts;  // the edges of each burst, both lines
  std::vector<std::vector<std::string>> frames;
};

const Captured &captured() {
  static Captured capture = []() {
    Captured out;
    for (int channel = 0; channel < 2; channel++) {
      std::vector<Edge> edges = saleae_edges(channel);
      std::vector<Edge> burst;
      for (size_t i = 0; i < edges.size(); i++) {
        if (!burst.empty() && edges[i].time_us - burst.back().time_us >= BurstAssembler::IDLE_US) {
          out.bursts.push_back(burst);
          burst.clear();
        }
        burst.push_back(edges[i]);
      }
      if (!burst.empty()) {
        out.bursts.push_back(burst);
      }
    }
    for (const std::vector<Edge> &burst : out.bursts) {
      BurstAssembler assembler;
      std::vector<Burst> assembled = assemble(assembler, burst);
      out.frames.push_back(assembled.size() == 1 ? frames_of(assembled[0]) : std::vector<std::string>{});
    }
    return out;
  }();
  return capture;
}

// How many of the captured bursts no longer decode to their frames when every edge is moved
// by up to `jitter_us` either way.
size_t failures_with_jitter(int jitter_us, int rounds, std::mt19937 &random) {
  const Captured &capture = captured();
  std::uniform_int_distribution<int> shift(-jitter_us, jitter_us);
  size_t failures = 0;
  for (int round = 0; round < rounds; round++) {
    for (size_t i = 0; i < capture.bursts.size(); i++) {
      std::vector<Edge> edges = capture.bursts[i];
      for (Edge &edge : edges) {
        edge.time_us += static_cast<uint32_t>(shift(random));
      }
      BurstAssembler assembler;
      std::vector<Burst> assembled = assemble(assembler, edges);
      if (assembled.size() != 1 || frames_of(assembled[0]) != capture.frames[i]) {
        failures++;
      }
    }
  }
  return failures;
}

}  // namespace

TEST_CASE("pulses between edges become signed widths, and idle closes the burst") {
  BurstAssembler assembler;
  // The line idles low: up at 1000, down at 1208, up at 1624, down at 1832.
  CHECK_FALSE(assembler.add_edge(1000, true));
  CHECK(assembler.open());
  CHECK_FALSE(assembler.add_edge(1208, false));
  CHECK_FALSE(assembler.add_edge(1624, true));
  CHECK_FALSE(assembler.add_edge(1832, false));
  CHECK_FALSE(assembler.check_idle(1832 + BurstAssembler::IDLE_US - 1));
  REQUIRE(assembler.check_idle(1832 + BurstAssembler::IDLE_US));
  // The last entry is the idle the line went into, not a pulse that was timed.
  CHECK(assembler.burst() == Burst{208, -416, 208, -static_cast<int32_t>(BurstAssembler::IDLE_US)});
  CHECK_FALSE(assembler.open());
  CHECK_FALSE(assembler.check_idle(100000));
}

TEST_CASE("an edge after an idle gap closes the burst the loop did not get to") {
  BurstAssembler assembler;
  assembler.add_edge(1000, true);
  assembler.add_edge(1208, false);
  // No check_idle() in between. The next burst's first edge closes this one and starts its own.
  REQUIRE(assembler.add_edge(40000, true));
  CHECK(assembler.burst() == Burst{208, -4000});
  CHECK(assembler.open());
  assembler.add_edge(40416, false);
  REQUIRE(assembler.check_idle(50000));
  CHECK(assembler.burst() == Burst{416, -4000});
}

TEST_CASE("a glitch is merged away and the levels either side are joined") {
  BurstAssembler assembler;
  assembler.add_edge(1000, true);
  assembler.add_edge(1208, false);
  // 20 µs of high in the middle of a 416 µs low.
  assembler.add_edge(1400, true);
  assembler.add_edge(1420, false);
  assembler.add_edge(1624, true);
  assembler.add_edge(1832, false);
  REQUIRE(assembler.check_idle(10000));
  CHECK(assembler.burst() == Burst{208, -416, 208, -4000});
  CHECK(assembler.glitches() == 1);

  // A glitch on an idle line starts no burst.
  assembler.add_edge(20000, true);
  assembler.add_edge(20030, false);
  CHECK_FALSE(assembler.open());
  CHECK_FALSE(assembler.check_idle(30000));
  CHECK(assembler.glitches() == 2);
}

TEST_CASE("an edge that does not change the level is not a pulse") {
  BurstAssembler assembler;
  assembler.add_edge(1000, true);
  assembler.add_edge(1208, false);
  assembler.add_edge(1300, false);  // the rising edge before it was missed
  assembler.add_edge(1624, true);
  assembler.add_edge(1832, false);
  REQUIRE(assembler.check_idle(10000));
  CHECK(assembler.burst() == Burst{208, -416, 208, -4000});
}

TEST_CASE("the microsecond clock can wrap in the middle of a burst or of the idle") {
  BurstAssembler assembler;
  uint32_t start = 0xFFFFFFFFu - 300;
  assembler.add_edge(start, true);
  assembler.add_edge(start + 208, false);
  assembler.add_edge(start + 624, true);  // past the wrap
  assembler.add_edge(start + 832, false);
  CHECK_FALSE(assembler.check_idle(start + 832 + 3999));
  REQUIRE(assembler.check_idle(start + 832 + 4000));
  CHECK(assembler.burst() == Burst{208, -416, 208, -4000});
}

TEST_CASE("a burst longer than the buffer is cut off, counted, and the next one is whole") {
  BurstAssembler assembler;
  uint32_t time = 1000;
  bool level = true;
  for (size_t i = 0; i < BurstAssembler::MAX_PULSES + 500; i++) {
    assembler.add_edge(time, level);
    time += 208;
    level = !level;
  }
  REQUIRE(assembler.check_idle(time + 10000));
  CHECK(assembler.burst().size() == BurstAssembler::MAX_PULSES);
  CHECK(assembler.overflows() == 1);

  time += 20000;
  assembler.add_edge(time, true);
  assembler.add_edge(time + 208, false);
  REQUIRE(assembler.check_idle(time + 10000));
  CHECK(assembler.burst() == Burst{208, -4000});
  CHECK(assembler.overflows() == 1);
}

TEST_CASE("every SPEC frame survives encode, edges, assembler and decoder, at both polarities and phases") {
  const Polarity polarities[] = {Polarity::NORMAL, Polarity::INVERTED};
  const Phase phases[] = {Phase::ALIGNED, Phase::EXTRA_HALF};
  for (int i = 0; i < SPEC_VECTOR_COUNT; i++) {
    const SpecVector &vector = SPEC_VECTORS[i];
    INFO(vector.name);
    std::vector<uint8_t> payload = from_hex(vector.hex);
    uint8_t frame[MAX_FRAME_BYTES];
    size_t size = encode_frame(payload.data(), payload.size(), vector.reply, frame, sizeof(frame));
    REQUIRE(size > 0);
    for (Polarity polarity : polarities) {
      for (Phase phase : phases) {
        Burst sent = manchester_encode(frame, size, polarity, phase);
        // Once with a loop that checks for idle every millisecond, once with none.
        for (uint32_t loop_us : {1000u, 0u}) {
          BurstAssembler assembler;
          std::vector<Burst> bursts = assemble(assembler, edges_of(sent, 5000), loop_us);
          REQUIRE(bursts.size() == 1);
          CHECK(frames_of(bursts[0]) == std::vector<std::string>{to_hex(payload.data(), payload.size())});
        }
      }
    }
  }
}

TEST_CASE("a real LNK2 session decodes from its logic-analyser edges") {
  // tests/captures/saleae/capture.csv: 111 seconds of an LNK2 polling a TM2 and running zone 1.
  std::vector<std::string> requests;
  std::vector<std::string> replies;
  size_t bursts[2] = {0, 0};
  for (int channel = 0; channel < 2; channel++) {
    std::vector<Edge> edges = saleae_edges(channel);
    REQUIRE(edges.size() > 20000);
    BurstAssembler assembler;
    ReceiverDecoder decoder;  // one per line, so the polarity lock carries as on a receiver
    for (const Burst &burst : assemble(assembler, edges, 1000)) {
      bursts[channel]++;
      std::vector<std::string> frames = frames_of(burst, &decoder);
      CHECK_FALSE(frames.empty());
      (channel == 0 ? requests : replies)
          .insert((channel == 0 ? requests : replies).end(), frames.begin(), frames.end());
    }
    CHECK(assembler.glitches() == 0);
    CHECK(assembler.overflows() == 0);
  }
  // 135 request bursts, 74 of them a 2-byte request sent twice, and one reply to each burst.
  CHECK(bursts[0] == 135);
  CHECK(bursts[1] == 135);
  CHECK(requests.size() == 209);
  REQUIRE(replies.size() == 135);

  // Every reply answers the request before it. A doubled request gets one reply.
  size_t reply = 0;
  for (size_t i = 0; i < requests.size(); i++) {
    if (i + 1 < requests.size() && requests[i].size() == 5 && requests[i + 1] == requests[i]) {
      continue;  // the first copy of a pair
    }
    REQUIRE(reply < replies.size());
    std::vector<uint8_t> request_bytes = from_hex(requests[i].c_str());
    std::vector<uint8_t> reply_bytes = from_hex(replies[reply].c_str());
    INFO(requests[i] << " -> " << replies[reply]);
    // By opcode only. The LNK2 also asks for pages the TM2 does not have (3F 01, 3D 02), and the
    // controller answers those with page FF, which reply_matches() would rightly not pair up.
    REQUIRE(reply_bytes.size() >= 2);
    bool answers = reply_bytes[0] == static_cast<uint8_t>(request_bytes[0] + 0x80) ||
                   (reply_bytes[0] == 0x01 && reply_bytes[1] == request_bytes[0]);
    CHECK(answers);
    reply++;
  }
  CHECK(reply == replies.size());

  // The frames WIRE.md records from this session.
  auto has = [](const std::vector<std::string> &frames, const char *frame) {
    return std::find(frames.begin(), frames.end(), frame) != frames.end();
  };
  CHECK(has(requests, "39 00 01 06"));
  CHECK(has(replies, "01 39"));
  CHECK(has(requests, "40"));
  CHECK(has(replies, "01 40"));
  CHECK(has(replies, "92 13 A7 EA"));
  CHECK(has(replies, "B0 00 00 64"));
}

TEST_CASE("the captured session still decodes with every edge moved by interrupt jitter") {
  REQUIRE(captured().bursts.size() == 270);
  for (const std::vector<std::string> &frames : captured().frames) {
    REQUIRE_FALSE(frames.empty());
  }
  std::mt19937 random(20261116);
  // The largest jitter, in steps of 5 µs, at which nothing fails: 270 bursts, 20 rounds each.
  int tolerated = 0;
  for (int jitter = 5; jitter <= 100; jitter += 5) {
    if (failures_with_jitter(jitter, 20, random) != 0) {
      break;
    }
    tolerated = jitter;
  }
  MESSAGE("every captured burst decodes with up to +/-" << tolerated << " us of jitter on each edge");
  // PLAN-GPIO 9.4 compares this with the latency spread measured on the bench. The pulse
  // windows are +/-30% (manchester.h), so two edges out by 25 µs each still land inside.
  CHECK(tolerated >= 25);
}

TEST_CASE("a burst with edges missing gives no frame, or the frames it had, never a wrong one") {
  const Captured &capture = captured();
  std::mt19937 random(7);
  size_t lost = 0;
  size_t kept = 0;
  for (int round = 0; round < 10; round++) {
    for (size_t i = 0; i < capture.bursts.size(); i++) {
      std::vector<Edge> edges = capture.bursts[i];
      // One edge gone, or two in a row, somewhere in the burst.
      std::uniform_int_distribution<size_t> where(0, edges.size() - 2);
      size_t at = where(random);
      edges.erase(edges.begin() + at, edges.begin() + at + 1 + (round % 2));
      BurstAssembler assembler;
      for (const Burst &burst : assemble(assembler, edges)) {
        for (const std::string &frame : frames_of(burst)) {
          // A doubled request can lose one copy and keep the other.
          bool known = std::find(capture.frames[i].begin(), capture.frames[i].end(), frame) != capture.frames[i].end();
          CHECK(known);
          kept++;
        }
      }
      lost++;
    }
  }
  CHECK(lost == 2700);
  MESSAGE(kept << " frames still decoded from " << lost << " damaged bursts, none of them wrong");
}
