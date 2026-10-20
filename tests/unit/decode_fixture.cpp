// Prints a fixture's bursts back with the frames the decoder finds, as
// `expect:` lines. tools/raw_to_fixture.py runs this to fill in expectations;
// a person then checks them before the fixture is committed.
//
//   ./decode_fixture tests/captures/session.txt

#include "fixture.h"
#include "sip_frame.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace esphome::rainbird;

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s FIXTURE\n", argv[0]);
    return 2;
  }
  Fixture fixture = read_fixture(argv[1]);
  if (!fixture.error.empty()) {
    std::fprintf(stderr, "%s: %s\n", argv[1], fixture.error.c_str());
    return 1;
  }
  // Comments and the receiver line are carried over unchanged.
  std::ifstream in(argv[1]);
  std::string line;
  while (std::getline(in, line)) {
    std::string text = fixture_trim(line);
    if (text.rfind("burst:", 0) == 0 || text.rfind("expect:", 0) == 0) {
      break;
    }
    std::printf("%s\n", line.c_str());
  }
  ReceiverDecoder decoder;
  for (const FixtureBurst &burst : fixture.bursts) {
    std::printf("burst: ");
    for (size_t i = 0; i < burst.timings.size(); i++) {
      std::printf(i == 0 ? "%d" : ", %d", static_cast<int>(burst.timings[i]));
    }
    std::printf("\n");
    BurstDecode decoded = decoder.decode(burst.timings.data(), burst.timings.size());
    if (decoded.frames.empty()) {
      std::printf("expect: none\n");
    }
    for (const Frame &frame : decoded.frames) {
      std::printf("expect: %s\n", to_hex(frame.payload, frame.length).c_str());
    }
  }
  return 0;
}
