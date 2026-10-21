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

// Drains events up to the first of `kind`.
static bool take_event_of(Session &session, SessionEventKind kind, SessionEvent *out = nullptr) {
  SessionEvent event;
  while (session.take_event(&event)) {
    if (event.kind == kind) {
      if (out != nullptr) {
        *out = event;
      }
      return true;
    }
  }
  return false;
}

TEST_CASE("startup identifies, probes, reads the schedule in use, then polls") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING, Field::CLOCK, Field::RUN_TIME});
  config.pages.add(0x80);  // zone 1's run times only
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
  CHECK(session.link().connected_known);
  CHECK(session.link().connected);
  CHECK(session.link().response_known);
  CHECK(session.link().response_ms == 30);
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
  CHECK(session.link().errors == 3);

  session.poll(now);
  CHECK(session.stage() == SessionStage::IDENTIFY);
}

TEST_CASE("three failed requests in a row mark the link down; any answer brings it back") {
  EspTm2Model model;
  Session session(model, config_with({Field::ZONES_RUNNING}));
  FakeController fake = FakeController::upgraded_tm2();
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  REQUIRE(session.link().connected);

  fake.replies.clear();
  for (int i = 0; i < 3; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 5000);
  }
  CHECK_FALSE(session.link().connected);

  fake = FakeController::upgraded_tm2();
  session.poll(now);
  run_session(session, fake, now, 5000);
  CHECK(session.link().connected);
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
  CHECK(session.send_raw(from_hex("02").data(), 1, 0) == ControlResult::REFUSED_NOT_READY);

  auto bf = from_hex("BF 00 02 00 00 00");
  session.on_frame(bf.data(), bf.size(), 100);
  bool on = false;
  REQUIRE(session.state().zone_running(2, &on));
  CHECK(on);
  CHECK(session.link().connected);

  // update() may never run in listen_only, so silence has its own check.
  session.check_silence(100 + 59999);
  CHECK(session.link().connected);
  session.check_silence(100 + 60000);
  CHECK_FALSE(session.link().connected);
}

TEST_CASE("with polling off only send_raw transmits, and only with experimental") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.polling = false;
  Session plain(model, config);
  plain.start(0);
  CHECK(plain.send_raw(from_hex("02").data(), 1, 0) == ControlResult::REFUSED_EXPERIMENTAL);

  config.experimental = true;
  Session session(model, config);
  session.start(0);
  CHECK(session.stage() == SessionStage::MANUAL);
  session.poll(0);
  Transmission tx;
  CHECK_FALSE(session.next_transmission(0, &tx));
  REQUIRE(session.send_raw(from_hex("02").data(), 1, 0) == ControlResult::QUEUED);
  REQUIRE(session.next_transmission(0, &tx));
  CHECK(to_hex(tx.bytes, tx.size) == "F5 F5 1C 01 02 03 1D");
}

TEST_CASE("send_raw is sent once when nothing answers") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.polling = false;
  config.experimental = true;
  Session session(model, config);
  FakeController silent;
  session.start(0);
  REQUIRE(session.send_raw(from_hex("42").data(), 1, 0) == ControlResult::QUEUED);
  run_session(session, silent, 0, 5000);
  CHECK(silent.requests == std::vector<std::string>{"42"});
}

TEST_CASE("polling is fast while a zone runs and after a command, otherwise update_interval") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.experimental = true;
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

  REQUIRE(session.send_raw(from_hex("4C").data(), 1, now) == ControlResult::QUEUED);
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
  off.op = ControlOp::VALVE_OFF;
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
  for (bool experimental : {true, false}) {
    SessionConfig config = config_with({Field::ZONES_RUNNING, Field::QUEUE});
    config.valve_queue = true;
    config.experimental = experimental;
    Running live(config);
    REQUIRE(live.session.capabilities().advance);
    Session &session = live.session;
    uint32_t &now = live.now;
    auto running = from_hex("BF 00 01 00 00 00");
    session.on_frame(running.data(), running.size(), now);
    auto queued = from_hex("BB 01 21 05 D0 22 04 B0 00 00 00");
    session.on_frame(queued.data(), queued.size(), now);
    Control off;
    off.op = ControlOp::VALVE_OFF;
    off.zone = 1;
    FakeController fake;
    REQUIRE(session.control(off, now) == ControlResult::QUEUED);
    run_session(session, fake, now, 5000);
    // 42 is experimental. Without it, turning the valve off stops everything.
    CHECK(fake.requests == std::vector<std::string>{experimental ? "42 02" : "40"});
  }
}

TEST_CASE("experimental commands stay off until the config and the probe allow them") {
  // This script answers 04 49 with "not supported" and 04 3A with "supported".
  Control irrigation;
  irrigation.op = ControlOp::SET_IRRIGATION;
  irrigation.value = 0;
  Control test_all;
  test_all.op = ControlOp::TEST_ZONES;
  test_all.value = 2;

  Running plain(config_with({Field::IRRIGATION_ENABLED}));
  CHECK(plain.session.control(irrigation, plain.now) == ControlResult::REFUSED_EXPERIMENTAL);
  CHECK(plain.session.control(test_all, plain.now) == ControlResult::REFUSED_EXPERIMENTAL);

  SessionConfig config = config_with({Field::IRRIGATION_ENABLED});
  config.experimental = true;
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  CHECK_FALSE(session.capabilities().controller_state);
  REQUIRE(session.capabilities().test_stations);
  CHECK(session.control(irrigation, now) == ControlResult::REFUSED_UNSUPPORTED);
  FakeController fake;
  REQUIRE(session.control(test_all, now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"3A 02"});
}

TEST_CASE("a schedule edit changes one field and checks the page that comes back") {
  SessionConfig config = config_with({Field::FREQUENCY});
  config.allow_writes = true;
  config.pages.add(0x10);
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

  REQUIRE(take_event_of(session, SessionEventKind::PAGE_WRITE));
  const PageEditLog &log = session.page_write();
  CHECK(log.page == 0x10);
  CHECK(log.before[5] == FREQ_CYCLIC);
  CHECK(log.after[5] == FREQ_CUSTOM);
  CHECK(log.before[3] == log.after[3]);
  CHECK(log.before[4] == log.after[4]);

  // The write is queued and has not been sent. Answer it, then answer the confirm read with the new page.
  fake.replies["21 00 10 6A 06 01 00 64 00"] = "01 21";
  fake.replies["20 00 10"] = "A0 00 10 6A 06 01 00 64 00";
  now = run_session(session, fake, now, 5000);
  CHECK_FALSE(take_event_of(session, SessionEventKind::EDIT_FAILED));
  CHECK(std::find(fake.requests.begin(), fake.requests.end(), "21 00 10 6A 06 01 00 64 00") != fake.requests.end());
}

TEST_CASE("a new schedule timestamp re-reads the schedule on the next poll") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::FREQUENCY});
  config.pages.add(0x10);
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

// The next transmission, once the gap after whatever came before has passed.
static Transmission next_after_gap(Session &session, uint32_t *now) {
  Transmission tx;
  for (int i = 0; i < 200 && !session.next_transmission(*now, &tx); i++) {
    *now += 5;
    session.tick(*now);
  }
  return tx;
}

TEST_CASE("a 2-byte command goes as one copy; polls and send_raw keep the LNK2's two") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t now = live.now;
  Control run;
  run.op = ControlOp::RUN_PROGRAM;
  run.program = 0;
  REQUIRE(session.control(run, now) == ControlResult::QUEUED);
  Transmission tx = next_after_gap(session, &now);
  CHECK(to_hex(tx.bytes, tx.size) == "F5 F5 1C 02 38 00 3A 1D");

  SessionConfig manual = config_with({Field::ZONES_RUNNING});
  manual.polling = false;
  manual.experimental = true;
  Session raw(live.model, manual);
  raw.start(0);
  REQUIRE(raw.send_raw(from_hex("3F 00").data(), 2, 0) == ControlResult::QUEUED);
  REQUIRE(raw.next_transmission(0, &tx));
  CHECK(to_hex(tx.bytes, tx.size) == "F5 F5 1C 02 3F 00 41 1D F5 F5 1C 02 3F 00 41 1D");
}

TEST_CASE("commands in a row share one follow-up poll, sent after the last of them") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t &now = live.now;
  Control time;
  time.op = ControlOp::SET_TIME;
  time.hour = 12;
  Control date;
  date.op = ControlOp::SET_DATE;
  date.day = 21;
  date.month = 10;
  date.year = 2026;
  REQUIRE(session.control(time, now) == ControlResult::QUEUED);
  REQUIRE(session.control(date, now) == ControlResult::QUEUED);
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["11 0C 00 00"] = "01 11";
  fake.replies["13 15 A7 EA"] = "01 13";
  fake.requests.clear();
  now = run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"11 0C 00 00", "13 15 A7 EA", "3F 00"});
}

TEST_CASE("each complete clock reading moves the clock serial on once") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::CLOCK});
  config.listen_only = true;
  Session session(model, config);
  session.start(0);
  CHECK(session.clock_serial() == 0);

  auto cc = from_hex("CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00");
  session.on_frame(cc.data(), cc.size(), 100);
  CHECK(session.clock_serial() == 1);
  CHECK(session.clock_read_at() == 100);

  // Without 4C: 10, then 12. The pair is one reading, timed by the 10.
  auto time = from_hex("90 0C 0A 28");
  auto date = from_hex("92 13 A7 EA");
  session.on_frame(time.data(), time.size(), 200);
  CHECK(session.clock_serial() == 1);
  session.on_frame(date.data(), date.size(), 300);
  CHECK(session.clock_serial() == 2);
  CHECK(session.clock_read_at() == 200);

  // A 12 on its own is not a reading.
  session.on_frame(date.data(), date.size(), 400);
  CHECK(session.clock_serial() == 2);
}

TEST_CASE("a schedule page that timed out is read again on the next poll; a NAKed one is not") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::FREQUENCY, Field::START_TIME});
  config.pages.add(0x10);
  config.pages.add(0x60);
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies.erase("20 00 10");         // silent: the read times out
  fake.replies["20 00 60"] = "00 20 01";  // not supported
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 20000);
  REQUIRE(session.stage() == SessionStage::RUNNING);

  fake.replies["20 00 10"] = "A0 00 10 6A 06 01 00 64 01";
  fake.requests.clear();
  session.poll(now);
  now = run_session(session, fake, now, 5000);
  CHECK(seen(fake, "20 00 10"));
  CHECK(seen(fake, "20 00 60"));  // the whole schedule is read again, NAKed page included

  fake.requests.clear();
  session.poll(now);
  run_session(session, fake, now, 5000);
  CHECK_FALSE(seen(fake, "20 00 10"));
  CHECK_FALSE(seen(fake, "20 00 60"));
}

TEST_CASE("a confirming read the queue refuses ends the edit, and the next edit can start") {
  SessionConfig config = config_with({Field::FREQUENCY});
  config.allow_writes = true;
  config.experimental = true;  // send_raw fills the queue below
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, now) == ControlResult::QUEUED);

  Transmission tx = next_after_gap(session, &now);
  REQUIRE(to_hex(first_payload(tx).data(), first_payload(tx).size()) == "20 00 10");
  now += tx.first_copy_ms + 30;
  auto page = from_hex("A0 00 10 6A 06 01 00 64 01");
  session.on_frame(page.data(), page.size(), now);
  tx = next_after_gap(session, &now);
  REQUIRE(to_hex(first_payload(tx).data(), first_payload(tx).size()) == "21 00 10 6A 06 01 00 64 00");

  // The write is in flight. Fill the queue so the confirming read has nowhere to go.
  auto model_query = from_hex("02");
  while (session.send_raw(model_query.data(), 1, now) == ControlResult::QUEUED) {
  }
  now += tx.first_copy_ms + 30;
  auto ack = from_hex("01 21");
  session.on_frame(ack.data(), ack.size(), now);
  SessionEvent failed;
  REQUIRE(take_event_of(session, SessionEventKind::EDIT_FAILED, &failed));
  CHECK(std::string(failed.reason).find("confirming read") != std::string::npos);

  FakeController fake = FakeController::upgraded_tm2();
  now = run_session(session, fake, now, 20000);
  CHECK(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, now) == ControlResult::QUEUED);
}

TEST_CASE("after five minutes down, the session starts again at identification") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t &now = live.now;
  REQUIRE(session.writes_permitted());
  FakeController silent;
  for (int i = 0; i < 3; i++) {
    session.poll(now);
    now = run_session(session, silent, now, 5000);
  }
  REQUIRE_FALSE(session.link().connected);

  session.poll(now + 60000);
  CHECK(session.stage() == SessionStage::RUNNING);
  now = run_session(session, silent, now + 60000, 5000);

  session.poll(now + 300000);
  CHECK(session.stage() == SessionStage::IDENTIFY);
  CHECK_FALSE(session.writes_permitted());
  CHECK_FALSE(session.capabilities().combined);
  // The old controller's state goes too, so nothing is decided on it.
  bool on = false;
  CHECK_FALSE(session.state().zone_running(1, &on));

  FakeController fake = FakeController::upgraded_tm2();
  run_session(session, fake, now + 300000, 20000);
  CHECK(session.stage() == SessionStage::RUNNING);
  CHECK(session.writes_permitted());
  CHECK(session.capabilities().combined);
}

TEST_CASE("events report each stage, the link going up and refusals, once each") {
  EspTm2Model model;
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["3F 00"] = "00 3F 02";  // a NAK that is not "unsupported"
  Session session(model, config_with({Field::ZONES_RUNNING}));
  session.start(0);
  run_session(session, fake, 0, 10000);

  std::vector<SessionEventKind> kinds;
  std::vector<SessionStage> stages;
  SessionEvent event;
  SessionEvent nak;
  while (session.take_event(&event)) {
    kinds.push_back(event.kind);
    if (event.kind == SessionEventKind::STAGE) {
      stages.push_back(event.stage);
    }
    if (event.kind == SessionEventKind::NAK) {
      nak = event;
    }
  }
  CHECK(stages == std::vector<SessionStage>{SessionStage::IDENTIFY, SessionStage::PROBE, SessionStage::RUNNING});
  CHECK(std::count(kinds.begin(), kinds.end(), SessionEventKind::LINK_UP) == 1);
  CHECK(nak.opcode == 0x3F);
  CHECK(nak.code == 0x02);
  CHECK_FALSE(session.take_event(&event));
}

TEST_CASE("probes the queue refuses do not leave the session waiting for them") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.experimental = true;
  Session session(model, config);
  session.start(0);
  Transmission tx;
  REQUIRE(session.next_transmission(0, &tx));  // 02 in flight
  auto query = from_hex("02");
  while (session.send_raw(query.data(), 1, 0) == ControlResult::QUEUED) {
  }
  auto model_reply = from_hex("82 00 0A 09 0C");
  session.on_frame(model_reply.data(), model_reply.size(), 100);
  CHECK(session.stage() == SessionStage::RUNNING);
  // No probe was answered or refused, so nothing optional is assumed.
  CHECK_FALSE(session.capabilities().combined);
}

TEST_CASE("probes that only time out assume nothing; the fallback polls run") {
  EspTm2Model model;
  FakeController fake = FakeController::upgraded_tm2();
  for (const char *probe : {"04 4C", "04 4A", "04 42", "04 4B", "04 3D", "04 49", "04 3A"}) {
    fake.replies.erase(probe);
  }
  Session session(model, config_with({Field::CLOCK}));
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 30000);
  REQUIRE(session.stage() == SessionStage::RUNNING);
  CHECK_FALSE(session.capabilities().combined);
  CHECK_FALSE(session.capabilities().advance);
  fake.requests.clear();
  session.poll(now);
  run_session(session, fake, now, 5000);
  CHECK(seen(fake, "10"));
  CHECK_FALSE(seen(fake, "4C"));
}

TEST_CASE("refresh reads the slow group and the schedule pages again") {
  SessionConfig config = config_with({Field::FREQUENCY, Field::SEASONAL_ADJUST});
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  FakeController fake = FakeController::upgraded_tm2();
  session.poll(now);  // the second cycle: no slow group, no pages
  now = run_session(session, fake, now, 5000);
  CHECK_FALSE(seen(fake, "30 00"));

  fake.requests.clear();
  REQUIRE(session.refresh(now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(seen(fake, "30 00"));
  CHECK(seen(fake, "20 00 10"));
}

TEST_CASE("valves start or queue their zone by valve_queue, and missing zones are refused") {
  for (bool queue : {false, true}) {
    SessionConfig config = config_with({Field::ZONES_RUNNING});
    config.valve_queue = queue;
    Running live(config);
    Session &session = live.session;
    Control on;
    on.op = ControlOp::VALVE_ON;
    on.zone = 2;
    on.value = 6;
    FakeController fake;
    REQUIRE(session.control(on, live.now) == ControlResult::QUEUED);
    run_session(session, fake, live.now, 5000);
    CHECK(fake.requests == std::vector<std::string>{queue ? "4B 00 02 06" : "39 00 02 06"});

    // The script's 03 00 reply says zones 1 to 6 are fitted.
    on.zone = 7;
    CHECK(session.control(on, live.now) == ControlResult::REFUSED_NO_ZONE);
  }
}
