#pragma once

// Reads a capture fixture from tests/captures/. Shared by test_replay.cpp and
// decode_fixture.cpp. The format is line based:
//
//   # comment
//   receiver: rx                 informational
//   burst: 208, -208, 416, ...   one remote_receiver burst, as `dump: raw` logs it
//   expect: 92 13 A7 EA          a frame payload that burst must decode to (in order)
//   expect: none                 the burst must decode to no frame
//
// Every burst needs at least one expect line. tools/raw_to_fixture.py writes
// them by running decode_fixture, and a person checks them before committing.

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hex.h"

struct FixtureBurst {
  std::vector<int32_t> timings;
  std::vector<std::vector<uint8_t>> expect;  // frame payloads
  bool expect_none{false};
  bool has_expect{false};
  int line{0};
};

struct Fixture {
  std::string path;
  std::vector<FixtureBurst> bursts;
  std::string error;  // empty when the file parsed
};

inline std::string fixture_trim(const std::string &text) {
  size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(start, end - start + 1);
}

inline std::vector<int32_t> fixture_timings(const std::string &text) {
  std::vector<int32_t> out;
  std::string number;
  auto flush = [&]() {
    if (!number.empty() && number != "-") {
      out.push_back(static_cast<int32_t>(std::stol(number)));
    }
    number.clear();
  };
  for (char c : text) {
    if (c == '-' && number.empty()) {
      number.push_back(c);
    } else if (c >= '0' && c <= '9') {
      number.push_back(c);
    } else {
      flush();
    }
  }
  flush();
  return out;
}

inline Fixture read_fixture(const std::string &path) {
  Fixture fixture;
  fixture.path = path;
  std::ifstream in(path);
  if (!in) {
    fixture.error = "cannot open";
    return fixture;
  }
  std::string raw;
  int line = 0;
  while (std::getline(in, raw)) {
    line++;
    std::string text = fixture_trim(raw);
    if (text.empty() || text[0] == '#') {
      continue;
    }
    size_t colon = text.find(':');
    if (colon == std::string::npos) {
      fixture.error = "line " + std::to_string(line) + ": expected key: value";
      return fixture;
    }
    std::string key = fixture_trim(text.substr(0, colon));
    std::string value = fixture_trim(text.substr(colon + 1));
    if (key == "burst") {
      FixtureBurst burst;
      burst.timings = fixture_timings(value);
      burst.line = line;
      fixture.bursts.push_back(burst);
    } else if (key == "expect") {
      if (fixture.bursts.empty()) {
        fixture.error = "line " + std::to_string(line) + ": expect before any burst";
        return fixture;
      }
      FixtureBurst &burst = fixture.bursts.back();
      burst.has_expect = true;
      if (value == "none") {
        burst.expect_none = true;
      } else {
        burst.expect.push_back(from_hex(value.c_str()));
      }
    } else if (key != "receiver") {
      fixture.error = "line " + std::to_string(line) + ": unknown key '" + key + "'";
      return fixture;
    }
  }
  return fixture;
}
