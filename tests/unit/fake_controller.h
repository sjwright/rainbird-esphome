#pragma once

// A scripted ESP-TM2 for link and session tests. Requests map to replies from
// SPEC.md; a missing entry means the controller stays silent.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "hex.h"
#include "link_core.h"
#include "session.h"

struct FakeController {
  std::map<std::string, std::string> replies;
  std::vector<std::string> requests;  // every request seen, in order, as hex
  uint32_t turnaround_ms{30};

  static FakeController upgraded_tm2() {
    FakeController fake;
    fake.replies = {
        {"02", "82 00 0A 09 0C"},
        {"0B", "8B 01 2F 00 00"},
        {"05", "85 00 00 00 00 00 00 89 63"},
        {"03 00", "83 00 3F 00 00 00"},
        {"04 4C", "84 4C 01"},
        {"04 4A", "84 4A 01"},
        {"04 42", "84 42 01"},
        {"04 4B", "84 4B 01"},
        {"04 3D", "84 3D 00"},
        {"04 49", "84 49 00"},
        {"04 3A", "84 3A 01"},
        {"3F 00", "BF 00 01 00 00 00"},
        {"4C", "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 01 67 01"},
        {"3B 00", "BB 00 05 00 05 D0 00 00 01 00 00 01"},
        {"3B 01", "BB 01 21 05 D0 22 04 B0 23 01 A4 24 04 B0 25 02 58 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
                  "00 00 00"},
        {"30 00", "B0 00 00 64"},
        {"30 01", "B0 01 00 50"},
        {"30 02", "B0 02 00 50"},
        {"4A 01", "CA 01 2B 48 31 63"},
        {"20 00 10", "A0 00 10 6A 06 01 00 64 01"},
        {"20 00 11", "A0 00 11 7F 03 00 00 2D 00"},
        {"20 00 12", "A0 00 12 00 03 00 00 64 00"},
        {"20 00 80", "A0 00 80 00 19 00 01 00 00 00 14 00 02 00 00"},
    };
    return fake;
  }
};

// The payload of the first frame in a transmission: F5 F5 1C len payload chk 1D.
inline std::vector<uint8_t> first_payload(const esphome::rainbird::Transmission &tx) {
  if (tx.size < 4) {
    return {};
  }
  uint8_t length = tx.bytes[3];
  return std::vector<uint8_t>(tx.bytes + 4, tx.bytes + 4 + length);
}

// Runs the session against the fake until it has been quiet for a while or
// `limit_ms` has passed. Returns the clock. `poll_every_ms` calls poll() like
// PollingComponent; 0 means never.
inline uint32_t run_session(esphome::rainbird::Session &session, FakeController &fake, uint32_t now,
                            uint32_t limit_ms, uint32_t poll_every_ms = 0) {
  using namespace esphome::rainbird;
  uint32_t end = now + limit_ms;
  uint32_t next_poll = now + poll_every_ms;
  while (static_cast<int32_t>(end - now) > 0) {
    if (poll_every_ms != 0 && static_cast<int32_t>(now - next_poll) >= 0) {
      session.poll(now);
      next_poll = now + poll_every_ms;
    }
    session.tick(now);
    Transmission tx;
    if (session.next_transmission(now, &tx)) {
      std::vector<uint8_t> request = first_payload(tx);
      std::string key = to_hex(request.data(), request.size());
      fake.requests.push_back(key);
      auto it = fake.replies.find(key);
      if (it != fake.replies.end()) {
        now += tx.first_copy_ms + fake.turnaround_ms;
        std::vector<uint8_t> reply = from_hex(it->second.c_str());
        session.on_frame(reply.data(), reply.size(), now);
      }
      continue;
    }
    if (poll_every_ms == 0 && !session.busy()) {
      break;
    }
    now += 5;
  }
  return now;
}
