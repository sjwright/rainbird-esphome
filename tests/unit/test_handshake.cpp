#include "doctest.h"

#include "fake_controller.h"
#include "hex.h"
#include "model_esp_tm2.h"
#include "session.h"
#include "sip_commands.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace esphome::rainbird;

namespace {

SessionConfig handshake_config() {
  SessionConfig config;
  config.used.add(Field::ZONES_RUNNING);
  config.handshake = true;  // random stays null: the first challenge is 12345678
  return config;
}

// The worked example: A = B = 12345678, T(12345678) = 8ACF17EB.
FakeController handshaking_tm2() {
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["06 12 34 56 78"] = "86 8A CF 17 EB 12 34 56 78";
  fake.replies["07 8A CF 17 EB"] = "87";
  return fake;
}

bool take_handshake_event(Session &session, SessionEvent *out) {
  SessionEvent event;
  while (session.take_event(&event)) {
    if (event.kind == SessionEventKind::HANDSHAKE) {
      *out = event;
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("handshake transform matches the worked example") {
  CHECK(handshake_answer(0x12345678) == 0x8ACF17EB);
  // An all-zero register is replaced, so the answer is never zero.
  CHECK(handshake_answer(0) != 0);
}

TEST_CASE("handshake requests and replies encode and parse") {
  ByteBuf challenge = request_handshake(0x12345678);
  CHECK(to_hex(challenge.data, challenge.size) == "06 12 34 56 78");
  ByteBuf answer = request_handshake_answer(0x8ACF17EB);
  CHECK(to_hex(answer.data, answer.size) == "07 8A CF 17 EB");

  auto reply = from_hex("86 8A CF 17 EB 12 34 56 78");
  Reply parsed;
  REQUIRE(parse_reply(reply.data(), reply.size(), &parsed));
  CHECK(parsed.kind == ReplyKind::HANDSHAKE);
  CHECK(parsed.handshake.answer == 0x8ACF17EB);
  CHECK(parsed.handshake.challenge == 0x12345678);
  CHECK(reply_matches(challenge.data, challenge.size, reply.data(), reply.size()));

  auto done = from_hex("87");
  REQUIRE(parse_reply(done.data(), done.size(), &parsed));
  CHECK(parsed.kind == ReplyKind::HANDSHAKE_DONE);
  CHECK(reply_matches(answer.data, answer.size, done.data(), done.size()));

  auto short_reply = from_hex("86 8A CF 17 EB");
  CHECK_FALSE(parse_reply(short_reply.data(), short_reply.size(), &parsed));
}

TEST_CASE("with the handshake on, startup exchanges challenges before identifying") {
  EspTm2Model model;
  Session session(model, handshake_config());
  FakeController fake = handshaking_tm2();
  session.start(0);
  run_session(session, fake, 0, 10000);

  REQUIRE(fake.requests.size() >= 3);
  CHECK(fake.requests[0] == "06 12 34 56 78");
  CHECK(fake.requests[1] == "07 8A CF 17 EB");
  CHECK(fake.requests[2] == "02");
  CHECK(session.stage() == SessionStage::RUNNING);
  CHECK(session.identity().model_known);

  SessionEvent event;
  REQUIRE(take_handshake_event(session, &event));
  CHECK(event.code == 0);
  CHECK(std::string(event.reason) == "complete");
}

TEST_CASE("a controller that refuses 06 is identified without the handshake") {
  EspTm2Model model;
  Session session(model, handshake_config());
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["06 12 34 56 78"] = "00 06 01";
  session.start(0);
  run_session(session, fake, 0, 10000);

  REQUIRE(fake.requests.size() >= 2);
  CHECK(fake.requests[0] == "06 12 34 56 78");
  CHECK(fake.requests[1] == "02");
  CHECK(std::find(fake.requests.begin(), fake.requests.end(), "07 8A CF 17 EB") == fake.requests.end());
  CHECK(session.stage() == SessionStage::RUNNING);
}

TEST_CASE("a wrong answer from the controller is reported, and the handshake carries on") {
  EspTm2Model model;
  Session session(model, handshake_config());
  FakeController fake = handshaking_tm2();
  fake.replies["06 12 34 56 78"] = "86 00 00 00 01 12 34 56 78";
  session.start(0);
  run_session(session, fake, 0, 10000);

  REQUIRE(fake.requests.size() >= 3);
  CHECK(fake.requests[1] == "07 8A CF 17 EB");
  CHECK(session.stage() == SessionStage::RUNNING);
  SessionEvent event;
  REQUIRE(take_handshake_event(session, &event));
  CHECK(event.code == 1);
}

TEST_CASE("a refused answer leaves the session idle, and the next poll starts over with a new challenge") {
  EspTm2Model model;
  Session session(model, handshake_config());
  FakeController fake = handshaking_tm2();
  fake.replies["07 8A CF 17 EB"] = "00 07 04";
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);

  CHECK(session.stage() == SessionStage::IDLE);
  CHECK(std::find(fake.requests.begin(), fake.requests.end(), "02") == fake.requests.end());
  SessionEvent event;
  REQUIRE(take_handshake_event(session, &event));
  CHECK(event.code == 1);

  session.poll(now);
  run_session(session, fake, now, 10000);
  REQUIRE(fake.requests.size() >= 3);
  CHECK(fake.requests[2].rfind("06 ", 0) == 0);
  CHECK(fake.requests[2] != "06 12 34 56 78");
}

TEST_CASE("a silent controller during the handshake leaves the session idle") {
  EspTm2Model model;
  Session session(model, handshake_config());
  FakeController silent;
  session.start(0);
  run_session(session, silent, 0, 5000);
  CHECK(session.stage() == SessionStage::IDLE);
  REQUIRE_FALSE(silent.requests.empty());
  CHECK(silent.requests[0] == "06 12 34 56 78");
}
