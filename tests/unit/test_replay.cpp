#include "doctest.h"

#include "fixture.h"
#include "sip_frame.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace esphome::rainbird;

#ifndef CAPTURES_DIR
#define CAPTURES_DIR "../captures"
#endif

// Every fixture in tests/captures/ runs through the same decoder the device
// uses, one ReceiverDecoder per file so the polarity lock carries across
// bursts as it does on a receiver.
TEST_CASE("captured bursts decode to their recorded frames") {
  std::vector<std::string> paths;
  for (const auto &entry : std::filesystem::directory_iterator(CAPTURES_DIR)) {
    if (entry.is_regular_file() && entry.path().extension() == ".txt") {
      paths.push_back(entry.path().string());
    }
  }
  std::sort(paths.begin(), paths.end());
  REQUIRE_FALSE(paths.empty());

  for (const std::string &path : paths) {
    Fixture fixture = read_fixture(path);
    INFO(path);
    REQUIRE(fixture.error.empty());
    REQUIRE_FALSE(fixture.bursts.empty());
    ReceiverDecoder decoder;
    for (const FixtureBurst &burst : fixture.bursts) {
      INFO("burst on line " << burst.line);
      REQUIRE(burst.has_expect);
      BurstDecode decoded = decoder.decode(burst.timings.data(), burst.timings.size());
      if (burst.expect_none) {
        CHECK(decoded.frames.empty());
        continue;
      }
      REQUIRE(decoded.frames.size() == burst.expect.size());
      for (size_t i = 0; i < burst.expect.size(); i++) {
        const Frame &frame = decoded.frames[i];
        CHECK(to_hex(frame.payload, frame.length) == to_hex(burst.expect[i].data(), burst.expect[i].size()));
      }
    }
  }
}
