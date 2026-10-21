#include "doctest.h"

#include "fake_controller.h"
#include "model_esp_tm2.h"
#include "session.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace esphome::rainbird;

static SessionConfig config_with(std::initializer_list<Field> fields) {
  SessionConfig config;
  for (Field field : fields) {
    config.used.add(field);
  }
  return config;
}

static bool seen(const FakeController &fake, const std::string &request) {
  return std::find(fake.requests.begin(), fake.requests.end(), request) != fake.requests.end();
}

TEST_CASE("startup identifies, probes, reads the schedule in use, then polls") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING, Field::CLOCK, Field::RUN_TIME});
  config.zone_mask = 1u << 0;  // zone 1's run times only: page 80
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();

  session.start(0);
  run_session(session, fake, 0, 10000);

  std::vector<std::string> expect = {"02",    "0B",    "05",    "03 00", "04 4C",    "04 4A", "04 42",
                                     "04 4B", "04 3D", "04 49", "04 3A", "20 00 80", "3F 00", "4C"};
  REQUIRE(fake.requests.size() >= expect.size());
  CHECK(std::vector<std::string>(fake.requests.begin(), fake.requests.begin() + expect.size()) == expect);
  CHECK(session.stage() == SessionStage::RUNNING);
  CHECK(session.writes_permitted());
  CHECK(session.capabilities().combined);
  CHECK(session.capabilities().advance);
  CHECK_FALSE(session.capabilities().station_error);
  CHECK_FALSE(session.capabilities().controller_state);
  CHECK(session.capabilities().test_stations);

  REQUIRE(session.identity().model_known);
  CHECK(session.identity().model.model_id == 0x000A);
  CHECK(session.identity().firmware.minor == 47);
  CHECK(session.identity().serial_known);
  CHECK(session.identity_changed());

  bool on = false;
  REQUIRE(session.state().zone_running(1, &on));
  CHECK(on);
  REQUIRE(session.state().zone_available(6, &on));
  CHECK(on);
  uint16_t minutes = 0;
  REQUIRE(session.state().run_minutes(1, 0, &minutes));
  CHECK(minutes == 25);
  CHECK(session.connected_known());
  CHECK(session.connected());
  CHECK(session.response_known());
  CHECK(session.response_ms() == 30);
}

TEST_CASE("an unknown model id keeps the hub read-only unless accept_model_ids lists it") {
  EspTm2Model model;
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["02"] = "82 00 06 09 0C";  // an ST8x

  Session plain(model, config_with({Field::ZONES_RUNNING}));
  plain.start(0);
  run_session(plain, fake, 0, 10000);
  CHECK(plain.stage() == SessionStage::RUNNING);
  CHECK_FALSE(plain.writes_permitted());

  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.accept_ids = {0x0006};
  Session accepting(model, config);
  accepting.start(0);
  run_session(accepting, fake, 0, 10000);
  CHECK(accepting.writes_permitted());
}

TEST_CASE("a silent controller leaves the session idle, then the next poll tries again") {
  EspTm2Model model;
  Session session(model, config_with({Field::ZONES_RUNNING}));
  FakeController silent;
  session.start(0);
  uint32_t now = run_session(session, silent, 0, 5000);
  CHECK(session.stage() == SessionStage::IDLE);
  CHECK(silent.requests == std::vector<std::string>{"02", "02", "02"});
  CHECK(session.link_errors() == 3);

  session.poll(now);
  CHECK(session.stage() == SessionStage::IDENTIFY);
}

TEST_CASE("three failed requests in a row mark the link down; any answer brings it back") {
  EspTm2Model model;
  Session session(model, config_with({Field::ZONES_RUNNING}));
  FakeController fake = FakeController::upgraded_tm2();
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  REQUIRE(session.connected());

  fake.replies.clear();
  for (int i = 0; i < 3; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 5000);
  }
  CHECK_FALSE(session.connected());

  fake = FakeController::upgraded_tm2();
  session.poll(now);
  run_session(session, fake, now, 5000);
  CHECK(session.connected());
}

TEST_CASE("without 04 every optional command is tried; a NAK then switches it off") {
  EspTm2Model model;
  FakeController fake = FakeController::upgraded_tm2();
  for (const char *probe : {"04 4C", "04 4A", "04 42", "04 4B", "04 3D", "04 49", "04 3A"}) {
    fake.replies[probe] = std::string("00 04 01");
  }
  fake.replies["4C"] = "00 4C 01";  // 0005 has no 4C
  Session session(model, config_with({Field::CLOCK, Field::RAIN_DELAY}));
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  CHECK_FALSE(session.capabilities().combined);  // the 4C NAK cleared it
  CHECK(session.capabilities().event_timestamp);

  fake.requests.clear();
  session.poll(now);
  run_session(session, fake, now, 5000);
  CHECK(seen(fake, "10"));
  CHECK(seen(fake, "12"));
  CHECK(seen(fake, "36"));
  CHECK_FALSE(seen(fake, "4C"));
}

TEST_CASE("listen_only never transmits and decodes what it hears") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.listen_only = true;
  Session session(model, config);
  session.start(0);
  CHECK(session.stage() == SessionStage::PASSIVE);
  session.poll(0);
  Transmission tx;
  CHECK_FALSE(session.next_transmission(0, &tx));
  CHECK_FALSE(session.send_raw(from_hex("02").data(), 1, 0));

  auto bf = from_hex("BF 00 02 00 00 00");
  session.on_frame(bf.data(), bf.size(), 100);
  bool on = false;
  REQUIRE(session.state().zone_running(2, &on));
  CHECK(on);
  CHECK(session.connected());

  session.poll(100 + 60000);
  CHECK_FALSE(session.connected());
}

TEST_CASE("with polling off only send_raw transmits") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.polling = false;
  Session session(model, config);
  session.start(0);
  CHECK(session.stage() == SessionStage::MANUAL);
  session.poll(0);
  Transmission tx;
  CHECK_FALSE(session.next_transmission(0, &tx));
  REQUIRE(session.send_raw(from_hex("02").data(), 1, 0));
  REQUIRE(session.next_transmission(0, &tx));
  CHECK(to_hex(tx.bytes, tx.size) == "F5 F5 1C 01 02 03 1D");
}

TEST_CASE("send_raw is sent once when nothing answers") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.polling = false;
  Session session(model, config);
  FakeController silent;
  session.start(0);
  REQUIRE(session.send_raw(from_hex("42").data(), 1, 0));
  run_session(session, silent, 0, 5000);
  CHECK(silent.requests == std::vector<std::string>{"42"});
}

TEST_CASE("polling is fast while a zone runs and after a command, otherwise update_interval") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["3F 00"] = "BF 00 00 00 00 00";
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  CHECK(session.desired_interval(now) == 15000);

  fake.replies["3F 00"] = "BF 00 01 00 00 00";
  session.poll(now);
  now = run_session(session, fake, now, 2000);
  CHECK(session.desired_interval(now) == 3000);

  fake.replies["3F 00"] = "BF 00 00 00 00 00";
  session.poll(now);
  now = run_session(session, fake, now, 2000);
  CHECK(session.desired_interval(now) == 15000);

  REQUIRE(session.send_raw(from_hex("4C").data(), 1, now));
  CHECK(session.desired_interval(now + 29999) == 3000);
  CHECK(session.desired_interval(now + 30000) == 15000);
  // A hold from a month ago does not look active again after the clock moves on.
  CHECK(session.desired_interval(now + 2592000000u) == 15000);
}

TEST_CASE("a poll cycle is not doubled while one is still pending") {
  EspTm2Model model;
  Session session(model, config_with({Field::ZONES_RUNNING, Field::CLOCK}));
  FakeController fake = FakeController::upgraded_tm2();
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  fake.requests.clear();
  session.poll(now);
  session.poll(now);
  run_session(session, fake, now, 5000);
  CHECK(std::count(fake.requests.begin(), fake.requests.end(), "3F 00") == 1);
}

struct Running {
  EspTm2Model model;
  Session session;
  uint32_t now{0};

  explicit Running(SessionConfig config) : session(model, config) {
    FakeController fake = FakeController::upgraded_tm2();
    this->session.start(0);
    this->now = run_session(this->session, fake, 0, 10000);
  }
};

TEST_CASE("a zone run is sent once, then a short poll follows the ack") {
  Running live(config_with({Field::ZONES_RUNNING}));
  REQUIRE(live.session.stage() == SessionStage::RUNNING);
  REQUIRE(live.session.writes_permitted());
  Session &session = live.session;
  uint32_t &now = live.now;
  FakeController fake;
  Control run;
  run.op = ControlOp::RUN_ZONE;
  run.zone = 1;
  run.value = 6;
  REQUIRE(session.control(run, now) == ControlResult::QUEUED);
  now = run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"39 00 01 06"});

  fake.replies["39 00 01 06"] = "01 39";
  fake.requests.clear();
  REQUIRE(session.control(run, now) == ControlResult::QUEUED);
  now = run_session(session, fake, now, 5000);
  REQUIRE(fake.requests.size() >= 2);
  CHECK(fake.requests[0] == "39 00 01 06");
  CHECK(fake.requests[1] == "3F 00");
}

TEST_CASE("turning a zone off stops only when that zone is running") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t &now = live.now;
  Control off;
  off.op = ControlOp::STOP_ZONE;
  // The scripted poll leaves zone 1 running and the rest off.
  off.zone = 2;
  CHECK(session.control(off, now) == ControlResult::REFUSED_NOT_RUNNING);
  off.zone = 1;
  FakeController fake;
  REQUIRE(session.control(off, now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"40"});
}

TEST_CASE("queue mode advances to the next queued zone when that is allowed") {
  Running live(config_with({Field::ZONES_RUNNING, Field::QUEUE}));
  REQUIRE(live.session.capabilities().advance);
  Session &session = live.session;
  uint32_t &now = live.now;
  auto running = from_hex("BF 00 01 00 00 00");
  session.on_frame(running.data(), running.size(), now);
  auto queued = from_hex("BB 01 21 05 D0 22 04 B0 00 00 00");
  session.on_frame(queued.data(), queued.size(), now);
  Control off;
  off.op = ControlOp::STOP_ZONE;
  off.zone = 1;
  off.queue_mode = true;
  off.experimental = true;
  FakeController fake;
  REQUIRE(session.control(off, now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"42 02"});

  off.experimental = false;
  fake.requests.clear();
  REQUIRE(session.control(off, now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"40"});
}

TEST_CASE("experimental commands stay off until the config and the probe allow them") {
  Running live(config_with({Field::IRRIGATION_ENABLED}));
  Session &session = live.session;
  uint32_t &now = live.now;
  // This script answers 04 49 with "not supported" and 04 3A with "supported".
  CHECK_FALSE(session.capabilities().controller_state);
  REQUIRE(session.capabilities().test_stations);
  Control irrigation;
  irrigation.op = ControlOp::SET_IRRIGATION;
  irrigation.value = 0;
  CHECK(session.control(irrigation, now) == ControlResult::REFUSED_EXPERIMENTAL);
  irrigation.experimental = true;
  CHECK(session.control(irrigation, now) == ControlResult::REFUSED_UNSUPPORTED);

  Control test_all;
  test_all.op = ControlOp::TEST_ZONES;
  test_all.value = 2;
  CHECK(session.control(test_all, now) == ControlResult::REFUSED_EXPERIMENTAL);
  test_all.experimental = true;
  FakeController fake;
  REQUIRE(session.control(test_all, now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"3A 02"});
}

TEST_CASE("a schedule edit changes one field and checks the page that comes back") {
  SessionConfig config = config_with({Field::FREQUENCY});
  config.allow_writes = true;
  config.program_mask = 0x1;
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, now) == ControlResult::QUEUED);

  FakeController fake;
  fake.replies["20 00 10"] = "A0 00 10 6A 06 01 00 64 01";
  now += 25;  // the gap after the poll that left the session idle
  session.tick(now);
  Transmission tx;
  REQUIRE(session.next_transmission(now, &tx));
  std::vector<uint8_t> request = first_payload(tx);
  CHECK(to_hex(request.data(), request.size()) == "20 00 10");
  now += tx.first_copy_ms + fake.turnaround_ms;
  std::vector<uint8_t> reply = from_hex("A0 00 10 6A 06 01 00 64 01");
  session.on_frame(reply.data(), reply.size(), now);

  PageEditLog log;
  REQUIRE(session.take_page_log(&log));
  CHECK(log.page == 0x10);
  CHECK(log.before[5] == FREQ_CYCLIC);
  CHECK(log.after[5] == FREQ_CUSTOM);
  CHECK(log.before[3] == log.after[3]);
  CHECK(log.before[4] == log.after[4]);

  // The write is queued and has not been sent. Answer it, then answer the confirm read with the new page.
  fake.replies["21 00 10 6A 06 01 00 64 00"] = "01 21";
  fake.replies["20 00 10"] = "A0 00 10 6A 06 01 00 64 00";
  now = run_session(session, fake, now, 5000);
  const char *reason = nullptr;
  CHECK_FALSE(session.take_edit_failure(&reason));
  CHECK(std::find(fake.requests.begin(), fake.requests.end(), "21 00 10 6A 06 01 00 64 00") != fake.requests.end());
}

TEST_CASE("clock drift across midnight counts the short gap") {
  CHECK_FALSE(clock_drifted(12, 0, 0, 12, 0, 20, 30000));
  CHECK(clock_drifted(12, 0, 0, 12, 0, 31, 30000));
  CHECK_FALSE(clock_drifted(0, 0, 10, 23, 59, 50, 30000));
  CHECK(clock_drifted(0, 0, 10, 23, 59, 0, 30000));
}

TEST_CASE("a new schedule timestamp re-reads the schedule on the next poll") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::FREQUENCY});
  config.program_mask = 0x1;
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  CHECK(seen(fake, "20 00 10"));
  CHECK(seen(fake, "4A 01"));  // the first poll cycle includes the slow group

  // Same timestamp: no re-read.
  fake.requests.clear();
  for (int i = 0; i < 4; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 3000);
  }
  CHECK(seen(fake, "4A 01"));
  CHECK_FALSE(seen(fake, "20 00 10"));

  // Edited at the front panel: the timestamp moves, and the page is read again.
  fake.replies["4A 01"] = "CA 01 2B 48 40 00";
  fake.requests.clear();
  for (int i = 0; i < 5; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 3000);
  }
  CHECK(seen(fake, "20 00 10"));
}
