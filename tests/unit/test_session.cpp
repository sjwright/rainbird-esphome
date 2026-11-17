#include "doctest.h"

#include "fake_controller.h"
#include "model_esp_tm2.h"
#include "session.h"

#include <algorithm>
#include <map>
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

// The next transmission, once the gap after whatever came before has passed.
static Transmission next_after_gap(Session &session, uint32_t *now) {
  Transmission tx;
  for (int i = 0; i < 200 && !session.next_transmission(*now, &tx); i++) {
    *now += 5;
    session.tick(*now);
  }
  return tx;
}

TEST_CASE("startup identifies, probes, reads the schedule in use, then polls") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING, Field::CLOCK, Field::RUN_TIME});
  config.pages.add(0x80);  // zone 1's run times only
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();

  session.start(0);
  run_session(session, fake, 0, 10000);

  std::vector<std::string> expect = {"02",    "0B",    "05",    "03 00",    "04 4C", "04 4A", "04 42",
                                     "04 4B", "04 49", "04 3A", "20 00 80", "3F 00", "4C"};
  REQUIRE(fake.requests.size() >= expect.size());
  CHECK(std::vector<std::string>(fake.requests.begin(), fake.requests.begin() + expect.size()) == expect);
  CHECK(session.stage() == SessionStage::RUNNING);
  CHECK(session.writes_permitted());
  CHECK(session.capabilities().has(Capability::COMBINED));
  CHECK(session.capabilities().has(Capability::ADVANCE));
  CHECK_FALSE(session.capabilities().has(Capability::CONTROLLER_STATE));
  CHECK(session.capabilities().has(Capability::TEST_STATIONS));

  REQUIRE(session.identity().model_known);
  CHECK(session.identity().model.model_id == 0x000A);
  CHECK(session.identity().firmware.minor == 47);
  CHECK(session.identity().serial_known);

  CHECK(session.state().running[0] == true);
  CHECK(session.state().available[5] == true);
  CHECK(session.state().run_minutes[0][0] == 25);
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
  for (const char *probe : {"04 4C", "04 4A", "04 42", "04 4B", "04 49", "04 3A"}) {
    fake.replies[probe] = std::string("00 04 01");
  }
  fake.replies["4C"] = "00 4C 01";  // 0005 has no 4C
  Session session(model, config_with({Field::CLOCK, Field::RAIN_DELAY}));
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  CHECK_FALSE(session.capabilities().has(Capability::COMBINED));  // the 4C NAK cleared it
  CHECK(session.capabilities().has(Capability::EVENT_TIMESTAMP));

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
  CHECK(session.state().running[1] == true);
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

  // A zone was running when the link went down. That last reading cannot hold the fast poll.
  fake.replies["3F 00"] = "BF 00 01 00 00 00";
  session.poll(now);
  now = run_session(session, fake, now, 2000);
  REQUIRE(session.desired_interval(now) == 3000);
  fake.replies.clear();
  for (int i = 0; i < 3 && session.link().connected; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 5000);
  }
  REQUIRE_FALSE(session.link().connected);
  CHECK(session.desired_interval(now) == 15000);
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

TEST_CASE("manual runs and the zone test stop at 100 minutes") {
  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.experimental = true;
  Running live(config);
  Control run;
  run.op = ControlOp::RUN_ZONE;
  run.zone = 1;
  run.value = 101;
  CHECK(live.session.control(run, live.now) == ControlResult::REFUSED_RANGE);
  run.op = ControlOp::QUEUE_ZONE;
  CHECK(live.session.control(run, live.now) == ControlResult::REFUSED_RANGE);
  Control test_all;
  test_all.op = ControlOp::TEST_ZONES;
  test_all.value = 101;
  CHECK(live.session.control(test_all, live.now) == ControlResult::REFUSED_RANGE);
  run.op = ControlOp::RUN_ZONE;
  run.value = 100;
  CHECK(live.session.control(run, live.now) == ControlResult::QUEUED);
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

TEST_CASE("valve off cancels every unsent start for its zone without stopping another zone") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Control on;
  on.op = ControlOp::VALVE_ON;
  on.zone = 2;
  on.value = 10;
  REQUIRE(live.session.control(on, live.now) == ControlResult::QUEUED);
  REQUIRE(live.session.control(on, live.now) == ControlResult::QUEUED);
  on.zone = 3;
  REQUIRE(live.session.control(on, live.now) == ControlResult::QUEUED);
  Control off;
  off.op = ControlOp::VALVE_OFF;
  off.zone = 2;
  CHECK(live.session.control(off, live.now) == ControlResult::QUEUED);
  CHECK(live.session.state().running[0] == true);
  CHECK(live.session.state().running[1] == false);
  FakeController silent;
  run_session(live.session, silent, live.now, 5000);
  CHECK(silent.requests == std::vector<std::string>{"39 00 03 0A"});
}

TEST_CASE("valve off stops a transmitted start before status arrives, with or without its ACK") {
  // Off can arrive during transmission, after the ACK, or after its timeout.
  for (int when = 0; when < 3; when++) {
    CAPTURE(when);
    Running live(config_with({Field::ZONES_RUNNING}));
    Session &session = live.session;
    Control on;
    on.op = ControlOp::VALVE_ON;
    on.zone = 2;
    on.value = 10;
    REQUIRE(session.control(on, live.now) == ControlResult::QUEUED);
    Transmission tx = next_after_gap(session, &live.now);
    REQUIRE(to_hex(tx.payload.data, tx.payload.size) == "39 00 02 0A");
    if (when == 1) {
      auto ack = from_hex("01 39");
      live.now += tx.first_copy_ms + 30;
      session.on_frame(ack.data(), ack.size(), live.now);
    } else if (when == 2) {
      live.now += tx.first_copy_ms + 400;
      session.tick(live.now);
    }
    Control off;
    off.op = ControlOp::VALVE_OFF;
    off.zone = 2;
    REQUIRE(session.control(off, live.now) == ControlResult::QUEUED);
    // Pending intent must never be published as the controller's state.
    CHECK(session.state().running[1] == false);
    FakeController fake = FakeController::upgraded_tm2();
    fake.replies["40"] = "01 40";
    run_session(session, fake, live.now, 5000);
    REQUIRE_FALSE(fake.requests.empty());
    CHECK(fake.requests[0] == "40");
    CHECK(std::count(fake.requests.begin(), fake.requests.end(), "40") == 1);
  }
}

TEST_CASE("a refused start or a fresh idle reading leaves nothing to cancel") {
  for (const char *reply : {"01 39", "00 39 02"}) {
    Running live(config_with({Field::ZONES_RUNNING}));
    Control on;
    on.op = ControlOp::RUN_ZONE;
    on.zone = 2;
    on.value = 10;
    REQUIRE(live.session.control(on, live.now) == ControlResult::QUEUED);
    FakeController fake = FakeController::upgraded_tm2();
    fake.replies["39 00 02 0A"] = reply;
    live.now = run_session(live.session, fake, live.now, 5000);
    Control off;
    off.op = ControlOp::VALVE_OFF;
    off.zone = 2;
    CHECK(live.session.control(off, live.now) == ControlResult::REFUSED_NOT_RUNNING);
  }
}

TEST_CASE("queue mode advances to the next queued zone when that is allowed") {
  for (bool experimental : {true, false}) {
    SessionConfig config = config_with({Field::ZONES_RUNNING, Field::QUEUE});
    config.valve_queue = true;
    config.experimental = experimental;
    Running live(config);
    REQUIRE(live.session.capabilities().has(Capability::ADVANCE));
    Session &session = live.session;
    uint32_t &now = live.now;
    // The script has zone 1 running and zones 1 to 5 in the queue.
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

TEST_CASE("queue mode refuses a valve off until the queue has been read") {
  SessionConfig config = config_with({Field::ZONES_RUNNING});  // no queue list polled
  config.valve_queue = true;
  config.experimental = true;
  Running live(config);
  REQUIRE(live.session.capabilities().has(Capability::ADVANCE));
  REQUIRE_FALSE(live.session.state().queue.has_value());
  Control off;
  off.op = ControlOp::VALVE_OFF;
  off.zone = 1;
  CHECK(live.session.control(off, live.now) == ControlResult::REFUSED_NOT_READY);
}

TEST_CASE("queue mode stops when the running zone is the only one queued") {
  SessionConfig config = config_with({Field::ZONES_RUNNING, Field::QUEUE});
  config.valve_queue = true;
  config.experimental = true;
  EspTm2Model model;
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["3B 01"] = "BB 01 21 05 D0 00 00 00";
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  REQUIRE(session.state().queue.has_value());
  Control off;
  off.op = ControlOp::VALVE_OFF;
  off.zone = 1;
  REQUIRE(session.control(off, now) == ControlResult::QUEUED);
  FakeController silent;
  run_session(session, silent, now, 5000);
  CHECK(silent.requests == std::vector<std::string>{"40"});
}

TEST_CASE("queue mode refuses a valve off until the 42 probe has an answer") {
  SessionConfig config = config_with({Field::ZONES_RUNNING, Field::QUEUE});
  config.valve_queue = true;
  config.experimental = true;
  EspTm2Model model;
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies.erase("04 42");
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 30000);
  REQUIRE(session.state().queue.has_value());
  REQUIRE_FALSE(session.capability_known(0x42));
  Control off;
  off.op = ControlOp::VALVE_OFF;
  off.zone = 1;
  CHECK(session.control(off, now) == ControlResult::REFUSED_NOT_READY);

  // The controller says no: 40 is the documented command.
  fake.replies["04 42"] = "84 42 00";
  REQUIRE(session.refresh(now) == ControlResult::QUEUED);
  now = run_session(session, fake, now, 5000);
  REQUIRE(session.capability_known(0x42));
  REQUIRE(session.control(off, now) == ControlResult::QUEUED);
  FakeController silent;
  run_session(session, silent, now, 5000);
  CHECK(silent.requests == std::vector<std::string>{"40"});
}

TEST_CASE("without 04, a probe that timed out is not assumed with the refused ones") {
  EspTm2Model model;
  FakeController fake = FakeController::upgraded_tm2();
  for (const char *probe : {"04 4C", "04 42", "04 4B", "04 49", "04 3A"}) {
    fake.replies[probe] = std::string("00 04 01");
  }
  fake.replies.erase("04 4A");
  Session session(model, config_with({Field::ZONES_RUNNING}));
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 30000);
  REQUIRE(session.stage() == SessionStage::RUNNING);
  CHECK(session.capabilities_assumed());
  CHECK(session.capabilities().has(Capability::QUEUE_RUN));
  CHECK(session.capability_known(0x4B));
  CHECK_FALSE(session.capabilities().has(Capability::EVENT_TIMESTAMP));
  CHECK_FALSE(session.capability_known(0x4A));

  // Its retry is refused like the others, so it is assumed too.
  fake.replies["04 4A"] = "00 04 01";
  REQUIRE(session.refresh(now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(session.capability_known(0x4A));
  CHECK(session.capabilities().has(Capability::EVENT_TIMESTAMP));
}

TEST_CASE("a probe that timed out is unknown, not unsupported, and is asked again") {
  EspTm2Model model;
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies.erase("04 4B");
  Session session(model, config_with({Field::ZONES_RUNNING}));
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 30000);
  REQUIRE(session.stage() == SessionStage::RUNNING);
  REQUIRE(session.writes_permitted());
  CHECK_FALSE(session.capability_known(0x4B));
  CHECK(session.capability_known(0x4C));
  Control queue;
  queue.op = ControlOp::QUEUE_ZONE;
  queue.zone = 1;
  queue.value = 5;
  CHECK(session.control(queue, now) == ControlResult::REFUSED_NOT_READY);

  // The next slow group asks once more (three tries on the link). Later cycles do not.
  fake.requests.clear();
  for (int i = 0; i < 9; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 3000);
  }
  CHECK(std::count(fake.requests.begin(), fake.requests.end(), "04 4B") == 3);
  CHECK_FALSE(session.capability_known(0x4B));

  // Refresh asks again, and an answer settles it.
  fake.replies["04 4B"] = "84 4B 01";
  REQUIRE(session.refresh(now) == ControlResult::QUEUED);
  now = run_session(session, fake, now, 5000);
  CHECK(session.capability_known(0x4B));
  CHECK(session.control(queue, now) == ControlResult::QUEUED);
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
  CHECK_FALSE(session.capabilities().has(Capability::CONTROLLER_STATE));
  REQUIRE(session.capabilities().has(Capability::TEST_STATIONS));
  CHECK(session.control(irrigation, now) == ControlResult::REFUSED_UNSUPPORTED);
  FakeController fake;
  REQUIRE(session.control(test_all, now) == ControlResult::QUEUED);
  run_session(session, fake, now, 5000);
  CHECK(fake.requests == std::vector<std::string>{"3A 02"});
}

TEST_CASE("frequencies 4 and 5, seen only in pyrainbird, need experimental to write") {
  SessionConfig config = config_with({Field::FREQUENCY});
  config.allow_writes = true;
  config.pages.add(0x10);
  Running plain(config);
  CHECK(plain.session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_OFF, plain.now) ==
        ControlResult::REFUSED_EXPERIMENTAL);
  CHECK(plain.session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_ODD_ALT, plain.now) ==
        ControlResult::REFUSED_EXPERIMENTAL);
  CHECK(plain.session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_EVEN, plain.now) == ControlResult::QUEUED);

  config.experimental = true;
  Running trying(config);
  CHECK(trying.session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_OFF, trying.now) == ControlResult::QUEUED);
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
  Transmission tx = next_after_gap(session, &now);
  REQUIRE(tx.size > 0);
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

TEST_CASE("a clock set is time then date, sharing one follow-up poll") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t &now = live.now;
  Control clock;
  clock.op = ControlOp::SET_CLOCK;
  clock.hour = 12;
  clock.day = 21;
  clock.month = 10;
  clock.year = 2026;
  REQUIRE(session.control(clock, now) == ControlResult::QUEUED);
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
  CHECK_FALSE(session.capabilities().has(Capability::COMBINED));
  // The old controller's state and identity go too, so nothing is decided on them.
  CHECK_FALSE(session.state().running[0].has_value());
  CHECK_FALSE(session.identity().model_known);

  FakeController fake = FakeController::upgraded_tm2();
  run_session(session, fake, now + 300000, 20000);
  CHECK(session.stage() == SessionStage::RUNNING);
  CHECK(session.writes_permitted());
  CHECK(session.capabilities().has(Capability::COMBINED));
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
  CHECK_FALSE(session.capabilities().has(Capability::COMBINED));
}

TEST_CASE("probes that only time out assume nothing; the fallback polls run") {
  EspTm2Model model;
  FakeController fake = FakeController::upgraded_tm2();
  for (const char *probe : {"04 4C", "04 4A", "04 42", "04 4B", "04 49", "04 3A"}) {
    fake.replies.erase(probe);
  }
  Session session(model, config_with({Field::CLOCK}));
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 30000);
  REQUIRE(session.stage() == SessionStage::RUNNING);
  CHECK_FALSE(session.capabilities().has(Capability::COMBINED));
  CHECK_FALSE(session.capabilities().has(Capability::ADVANCE));
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

TEST_CASE("with the link down, commands are refused and queued ones are dropped") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t &now = live.now;
  Control run;
  run.op = ControlOp::RUN_ZONE;
  run.value = 5;
  for (uint8_t zone = 1; zone <= 4; zone++) {
    run.zone = zone;
    REQUIRE(session.control(run, now) == ControlResult::QUEUED);
  }
  FakeController silent;
  now = run_session(session, silent, now, 10000);
  // Three failures in a row take the link down. The fourth command does not wait for it to come back.
  CHECK_FALSE(session.link().connected);
  CHECK(silent.requests == std::vector<std::string>{"39 00 01 05", "39 00 02 05", "39 00 03 05"});
  run.zone = 1;
  CHECK(session.control(run, now) == ControlResult::REFUSED_LINK_DOWN);
}

TEST_CASE("a reply nobody asked for does not change state") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  // Nothing is in flight. A late BF saying zone 3 runs is older than the next poll's answer.
  auto late = from_hex("BF 00 04 00 00 00");
  session.on_frame(late.data(), late.size(), live.now);
  CHECK(session.state().running[2] == false);
}

TEST_CASE("a schedule edit fails, and writes nothing, when a read is answered without the page") {
  SessionConfig config = config_with({Field::FREQUENCY});
  config.allow_writes = true;
  config.pages.add(0x10);

  // The first read gets an ACK instead of the page.
  Running reading(config);
  REQUIRE(reading.session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, reading.now) == ControlResult::QUEUED);
  FakeController fake;
  fake.replies["20 00 10"] = "01 20";
  run_session(reading.session, fake, reading.now, 5000);
  SessionEvent failed;
  REQUIRE(take_event_of(reading.session, SessionEventKind::EDIT_FAILED, &failed));
  CHECK(std::string(failed.reason).find("did not return that page") != std::string::npos);
  CHECK(fake.requests == std::vector<std::string>{"20 00 10"});

  // The confirming read gets an ACK instead of the page.
  Running confirming(config);
  Session &session = confirming.session;
  uint32_t now = confirming.now;
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, now) == ControlResult::QUEUED);
  Transmission tx = next_after_gap(session, &now);
  now += tx.first_copy_ms + 30;
  auto page = from_hex("A0 00 10 6A 06 01 00 64 01");
  session.on_frame(page.data(), page.size(), now);
  FakeController after;
  after.replies["21 00 10 6A 06 01 00 64 00"] = "01 21";
  after.replies["20 00 10"] = "01 20";
  run_session(session, after, now, 5000);
  REQUIRE(take_event_of(session, SessionEventKind::EDIT_FAILED, &failed));
  CHECK(std::string(failed.reason).find("did not return that page") != std::string::npos);
}

TEST_CASE("a command queued behind a clock set does not lose its date") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  Control clock;
  clock.op = ControlOp::SET_CLOCK;
  clock.hour = 12;
  clock.day = 21;
  clock.month = 10;
  clock.year = 2026;
  REQUIRE(session.control(clock, live.now) == ControlResult::QUEUED);
  Control delay;
  delay.op = ControlOp::SET_RAIN_DELAY;
  delay.value = 2;
  REQUIRE(session.control(delay, live.now) == ControlResult::QUEUED);
  // A second clock set waits: the time already queued sends its own date.
  Control other = clock;
  other.day = 22;
  CHECK(session.control(other, live.now) == ControlResult::REFUSED_BUSY);

  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["11 0C 00 00"] = "01 11";
  fake.replies["37 00 02"] = "01 37";
  fake.replies["13 15 A7 EA"] = "01 13";
  fake.requests.clear();
  uint32_t now = run_session(session, fake, live.now, 5000);
  CHECK(seen(fake, "37 00 02"));
  CHECK(seen(fake, "13 15 A7 EA"));
  CHECK_FALSE(seen(fake, "13 16 A7 EA"));
  CHECK(session.control(other, now) == ControlResult::QUEUED);
}

TEST_CASE("a clock set sends the date only once the time is acknowledged") {
  Control clock;
  clock.op = ControlOp::SET_CLOCK;
  clock.hour = 12;
  clock.day = 21;
  clock.month = 10;
  clock.year = 2026;
  for (const char *answer : {"", "00 11 02"}) {  // a timeout, then a NAK
    Running live(config_with({Field::ZONES_RUNNING}));
    Session &session = live.session;
    REQUIRE(session.control(clock, live.now) == ControlResult::QUEUED);
    FakeController fake = FakeController::upgraded_tm2();
    if (answer[0] != '\0') {
      fake.replies["11 0C 00 00"] = answer;
    }
    fake.requests.clear();
    run_session(session, fake, live.now, 5000);
    CHECK(seen(fake, "11 0C 00 00"));
    CHECK_FALSE(seen(fake, "13 15 A7 EA"));
  }

  SessionConfig config = config_with({Field::ZONES_RUNNING});
  config.experimental = true;  // send_raw fills the queue
  Running live(config);
  Session &session = live.session;
  auto query = from_hex("02");
  while (session.send_raw(query.data(), 1, live.now) == ControlResult::QUEUED) {
  }
  CHECK(session.control(clock, live.now) == ControlResult::REFUSED_BUSY);
  FakeController fake = FakeController::upgraded_tm2();
  run_session(session, fake, live.now, 30000);
  CHECK_FALSE(seen(fake, "11 0C 00 00"));

  clock.month = 2;
  clock.day = 30;
  CHECK(session.control(clock, live.now) == ControlResult::REFUSED_RANGE);
}

TEST_CASE("answered requests leave the error count alone") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t errors = session.link().errors;
  FakeController fake = FakeController::upgraded_tm2();
  session.poll(live.now);
  run_session(session, fake, live.now, 5000);
  CHECK(session.link().errors == errors);
  CHECK(session.link().connected);
  CHECK(session.link().replies > 0);
}

TEST_CASE("our own schedule write does not set off a full schedule re-read") {
  SessionConfig config = config_with({Field::FREQUENCY});
  config.allow_writes = true;
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, now) == ControlResult::QUEUED);
  Transmission tx = next_after_gap(session, &now);
  now += tx.first_copy_ms + 30;
  auto page = from_hex("A0 00 10 6A 06 01 00 64 01");
  session.on_frame(page.data(), page.size(), now);
  // The write, then the confirming read, which now returns the written page.
  FakeController fake = FakeController::upgraded_tm2();
  fake.replies["21 00 10 6A 06 01 00 64 00"] = "01 21";
  fake.replies["20 00 10"] = "A0 00 10 6A 06 01 00 64 00";
  now = run_session(session, fake, now, 5000);
  REQUIRE_FALSE(take_event_of(session, SessionEventKind::EDIT_FAILED));

  // The write moved the timestamp. That change is ours, so the page is not read again for it.
  fake.replies["4A 01"] = "CA 01 2B 48 40 00";
  fake.requests.clear();
  for (int i = 0; i < 5; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 3000);
  }
  CHECK(seen(fake, "4A 01"));
  CHECK_FALSE(seen(fake, "20 00 10"));

  // A later change is someone else's, at the panel or another master: read it.
  fake.replies["4A 01"] = "CA 01 2B 48 50 00";
  fake.requests.clear();
  for (int i = 0; i < 5; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 3000);
  }
  CHECK(seen(fake, "20 00 10"));
}

TEST_CASE("before the probes finish, a command that needs one is not ready, not unsupported") {
  EspTm2Model model;
  Session session(model, config_with({Field::ZONES_RUNNING}));
  FakeController fake = FakeController::upgraded_tm2();
  session.start(0);
  Transmission tx;
  REQUIRE(session.next_transmission(0, &tx));  // 02
  auto identified = from_hex("82 00 0A 09 0C");
  session.on_frame(identified.data(), identified.size(), 60);
  REQUIRE(session.stage() == SessionStage::PROBE);
  Control queue;
  queue.op = ControlOp::QUEUE_ZONE;
  queue.zone = 1;
  queue.value = 5;
  CHECK(session.control(queue, 60) == ControlResult::REFUSED_NOT_READY);
}

TEST_CASE("seasonal adjust takes the TM2's 5 to 200 percent") {
  Running live(config_with({Field::SEASONAL_ADJUST}));
  Control seasonal;
  seasonal.op = ControlOp::SET_SEASONAL;
  seasonal.program = 0;
  seasonal.value = 4;
  CHECK(live.session.control(seasonal, live.now) == ControlResult::REFUSED_RANGE);
  seasonal.value = 201;
  CHECK(live.session.control(seasonal, live.now) == ControlResult::REFUSED_RANGE);
  seasonal.value = 5;
  CHECK(live.session.control(seasonal, live.now) == ControlResult::QUEUED);
}

TEST_CASE("a seasonal adjust is read back after the ack, not a slow poll later") {
  Running live(config_with({Field::SEASONAL_ADJUST}));
  Session &session = live.session;
  uint32_t &now = live.now;
  FakeController fake;
  fake.replies["31 01 00 64"] = "01 31";
  fake.replies["30 01"] = "B0 01 00 64";
  Control seasonal;
  seasonal.op = ControlOp::SET_SEASONAL;
  seasonal.program = 1;
  seasonal.value = 100;
  REQUIRE(session.control(seasonal, now) == ControlResult::QUEUED);
  now = run_session(session, fake, now, 5000);
  REQUIRE(fake.requests.size() >= 2);
  CHECK(fake.requests[0] == "31 01 00 64");
  CHECK(seen(fake, "30 01"));
  CHECK(session.state().seasonal[1] == 100);
}

TEST_CASE("a refused seasonal adjust is not read back") {
  Running live(config_with({Field::SEASONAL_ADJUST}));
  FakeController fake;
  fake.replies["31 00 00 64"] = "00 31 02";
  Control seasonal;
  seasonal.op = ControlOp::SET_SEASONAL;
  seasonal.program = 0;
  seasonal.value = 100;
  REQUIRE(live.session.control(seasonal, live.now) == ControlResult::QUEUED);
  run_session(live.session, fake, live.now, 5000);
  CHECK_FALSE(seen(fake, "30 00"));
}

TEST_CASE("the revision moves on when there is something to publish, and only then") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t revision = session.revision();
  session.tick(live.now + 1);
  CHECK(session.revision() == revision);
  FakeController fake = FakeController::upgraded_tm2();
  session.poll(live.now);
  run_session(session, fake, live.now, 5000);
  CHECK(session.revision() != revision);
}

// A controller that keeps schedule pages and takes writes to them, so edits in a row see each other.
struct PageController {
  std::map<uint8_t, std::vector<uint8_t>> pages{{0x10, from_hex("6A 06 01 00 64 01")}};
  std::vector<std::string> writes;    // the bytes of each page written, in order
  std::vector<std::string> requests;  // every request, as hex

  uint32_t run(Session &session, uint32_t now, uint32_t limit_ms) {
    uint32_t end = now + limit_ms;
    while (static_cast<int32_t>(end - now) > 0) {
      session.tick(now);
      Transmission tx;
      if (!session.next_transmission(now, &tx)) {
        if (!session.busy()) {
          break;
        }
        now += 5;
        continue;
      }
      std::vector<uint8_t> request = first_payload(tx);
      this->requests.push_back(to_hex(request.data(), request.size()));
      std::vector<uint8_t> reply;
      if (request.size() == 3 && request[0] == 0x20 && this->pages.count(request[2]) != 0) {
        reply = {0xA0, 0x00, request[2]};
        const std::vector<uint8_t> &page = this->pages[request[2]];
        reply.insert(reply.end(), page.begin(), page.end());
      } else if (request.size() > 3 && request[0] == 0x21 && this->pages.count(request[2]) != 0) {
        std::vector<uint8_t> &page = this->pages[request[2]];
        page.assign(request.begin() + 3, request.end());
        this->writes.push_back(to_hex(page.data(), page.size()));
        reply = from_hex("01 21");
      } else {
        continue;  // anything else goes unanswered
      }
      now += tx.first_copy_ms + 30;
      session.on_frame(reply.data(), reply.size(), now);
    }
    return now;
  }

  size_t count(const std::string &request) const {
    return static_cast<size_t>(std::count(this->requests.begin(), this->requests.end(), request));
  }
};

TEST_CASE("schedule edits to one page that arrive together go out as one write") {
  SessionConfig config = config_with({Field::WATERING_DAY, Field::FREQUENCY});
  config.allow_writes = true;
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  // 6A is Monday, Wednesday, Friday and Saturday. Monday off, Wednesday off, then the frequency
  // twice: the second value replaces the first.
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 0, 0, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 2, 0, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_ODD, now) == ControlResult::QUEUED);
  // Out of range is still refused at once, not queued and failed later.
  CHECK(session.edit_schedule(Field::WATERING_DAY, 0, 0, 7, 0, now) == ControlResult::REFUSED_RANGE);

  PageController controller;
  now = controller.run(session, now, 20000);
  CHECK(controller.writes == std::vector<std::string>{"60 06 01 00 64 02"});
  CHECK(controller.count("20 00 10") == 2);  // one read, one confirming read
  CHECK_FALSE(take_event_of(session, SessionEventKind::EDIT_FAILED));
  CHECK(session.state().watering_day[0][0] == std::optional<bool>(false));
  CHECK(session.state().watering_day[0][2] == std::optional<bool>(false));
  CHECK(session.state().frequency[0] == std::optional<uint8_t>(FREQ_ODD));
}

TEST_CASE("an edit that arrives after its page was read waits its turn, and sees that write") {
  SessionConfig config = config_with({Field::WATERING_DAY});
  config.allow_writes = true;
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 0, 0, now) == ControlResult::QUEUED);
  Transmission tx = next_after_gap(session, &now);
  REQUIRE(to_hex(first_payload(tx).data(), first_payload(tx).size()) == "20 00 10");
  now += tx.first_copy_ms + 30;
  auto page = from_hex("A0 00 10 6A 06 01 00 64 01");
  session.on_frame(page.data(), page.size(), now);

  // The page is patched and its write is queued. Wednesday cannot join it now.
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 2, 0, now) == ControlResult::QUEUED);
  PageController controller;
  now = controller.run(session, now, 20000);
  CHECK(controller.writes == std::vector<std::string>{"68 06 01 00 64 01", "60 06 01 00 64 01"});
  CHECK_FALSE(take_event_of(session, SessionEventKind::EDIT_FAILED));
}

TEST_CASE("an edit that changes nothing reads the page and writes nothing") {
  SessionConfig config = config_with({Field::WATERING_DAY, Field::FREQUENCY});
  config.allow_writes = true;
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  // Monday is already on and the frequency is already cyclic.
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 0, 1, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_CYCLIC, now) == ControlResult::QUEUED);
  PageController controller;
  now = controller.run(session, now, 20000);
  CHECK(controller.requests == std::vector<std::string>{"20 00 10"});
  CHECK(controller.writes.empty());
  CHECK_FALSE(take_event_of(session, SessionEventKind::PAGE_WRITE));
  CHECK_FALSE(take_event_of(session, SessionEventKind::EDIT_FAILED));
  CHECK_FALSE(session.busy());

  // The next edit starts as usual.
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 0, 0, now) == ControlResult::QUEUED);
  controller.run(session, now, 20000);
  CHECK(controller.writes == std::vector<std::string>{"68 06 01 00 64 01"});
}

TEST_CASE("a change its field cannot hold is reported, and the rest of the page is still written") {
  SessionConfig config = config_with({Field::CYCLE_DAYS, Field::FREQUENCY});
  config.allow_writes = true;
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  REQUIRE(session.edit_schedule(Field::FREQUENCY, 0, 0, 0, FREQ_EVEN, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::CYCLE_DAYS, 0, 0, 0, 300, now) == ControlResult::QUEUED);  // one byte
  PageController controller;
  controller.run(session, now, 20000);
  CHECK(controller.writes == std::vector<std::string>{"6A 06 01 00 64 03"});
  SessionEvent failed;
  REQUIRE(take_event_of(session, SessionEventKind::EDIT_FAILED, &failed));
  CHECK(std::string(failed.reason).find("could not be changed") != std::string::npos);
}

TEST_CASE("edits to different pages go one page at a time, oldest first") {
  SessionConfig config = config_with({Field::RUN_TIME, Field::WATERING_DAY});
  config.allow_writes = true;
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  // Zones 1 and 2 share page 80; zone 3 is on page 81. Asked for in this order.
  REQUIRE(session.edit_schedule(Field::RUN_TIME, 1, 0, 0, 10, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::RUN_TIME, 3, 1, 0, 7, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 0, 0, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::RUN_TIME, 2, 2, 0, 5, now) == ControlResult::QUEUED);
  // Zone 3 again while it waits: the later value is the one written.
  REQUIRE(session.edit_schedule(Field::RUN_TIME, 3, 1, 0, 9, now) == ControlResult::QUEUED);

  PageController controller;
  controller.pages[0x80] = from_hex("00 19 00 01 00 00 00 14 00 02 00 00");
  controller.pages[0x81] = from_hex("00 00 00 00 00 00 00 00 00 00 00 00");
  controller.run(session, now, 30000);
  CHECK(controller.writes == std::vector<std::string>{
                                 "00 0A 00 01 00 00 00 14 00 02 00 05",  // page 80: zone 1 A and zone 2 C together
                                 "68 06 01 00 64 01",                    // page 10
                                 "00 00 00 09 00 00 00 00 00 00 00 00",  // page 81, moved to the back by the repeat
                             });
  CHECK_FALSE(take_event_of(session, SessionEventKind::EDIT_FAILED));
}

TEST_CASE("an edit that fails does not stop the ones waiting behind it") {
  SessionConfig config = config_with({Field::WATERING_DAY, Field::START_ENABLED});
  config.allow_writes = true;
  config.pages.add(0x10);
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  // Page 60 is never answered, so the first edit's read times out.
  REQUIRE(session.edit_schedule(Field::START_ENABLED, 0, 0, 0, 0, now) == ControlResult::QUEUED);
  REQUIRE(session.edit_schedule(Field::WATERING_DAY, 0, 0, 0, 0, now) == ControlResult::QUEUED);
  PageController controller;
  now = controller.run(session, now, 20000);
  CHECK(take_event_of(session, SessionEventKind::EDIT_FAILED));
  CHECK(controller.writes == std::vector<std::string>{"68 06 01 00 64 01"});
}

TEST_CASE("every schedule field can wait at once, and a lost link drops them with the page in flight") {
  SessionConfig config = config_with({Field::RUN_TIME});
  config.allow_writes = true;
  Running live(config);
  Session &session = live.session;
  uint32_t &now = live.now;
  // All 92 fields a TM2 has. None is refused for want of room.
  int queued = 0;
  auto edit = [&](Field field, uint8_t zone, uint8_t program, uint8_t index) {
    queued += session.edit_schedule(field, zone, program, index, 1, now) == ControlResult::QUEUED ? 1 : 0;
  };
  for (uint8_t zone = 1; zone <= 12; zone++) {
    for (uint8_t program = 0; program < 3; program++) {
      edit(Field::RUN_TIME, zone, program, 0);
    }
  }
  for (uint8_t program = 0; program < 3; program++) {
    for (uint8_t day = 0; day < 7; day++) {
      edit(Field::WATERING_DAY, 0, program, day);
    }
    edit(Field::CYCLE_DAYS, 0, program, 0);
    edit(Field::DAYS_UNTIL_NEXT, 0, program, 0);
    edit(Field::FREQUENCY, 0, program, 0);
    for (uint8_t slot = 0; slot < 4; slot++) {
      edit(Field::START_TIME, 0, program, slot);
      edit(Field::START_ENABLED, 0, program, slot);
    }
  }
  edit(Field::STATION_DELAY, 0, 0, 0);
  edit(Field::GLOBAL_SENSOR_BYPASS, 0, 0, 0);
  CHECK(queued == 92);

  // Nothing answers. The first page read fails after its retries; with the link still up, the
  // next page waiting then tries its own read. Three failed requests in a row mark the link
  // down, and what is still waiting goes with it: nothing more is sent.
  FakeController silent;
  now = run_session(session, silent, now, 60000);
  CHECK(session.link().connected == false);
  size_t reads = 0;
  for (const std::string &request : silent.requests) {
    reads += request.rfind("20 00 ", 0) == 0 ? 1 : 0;
  }
  CHECK(reads == 9);  // three pages, three attempts each
  CHECK(session.edit_schedule(Field::RUN_TIME, 1, 0, 0, 10, now) == ControlResult::REFUSED_LINK_DOWN);
  CHECK_FALSE(session.busy());
}

TEST_CASE("a command nobody answers is reported, and is not sent again") {
  Running live(config_with({Field::ZONES_RUNNING}));
  Session &session = live.session;
  uint32_t &now = live.now;
  SessionEvent event;
  while (session.take_event(&event)) {
  }
  Control run;
  run.op = ControlOp::RUN_PROGRAM;
  run.program = 1;
  REQUIRE(session.control(run, now) == ControlResult::QUEUED);
  FakeController silent;
  now = run_session(session, silent, now, 5000);
  CHECK(silent.requests == std::vector<std::string>{"38 01"});
  REQUIRE(take_event_of(session, SessionEventKind::NO_REPLY, &event));
  CHECK(event.opcode == 0x38);
  // One lost reply is not a lost link.
  CHECK(session.link().connected);

  // A poll that nobody answers is counted, not reported.
  session.poll(now);
  run_session(session, silent, now, 5000);
  CHECK_FALSE(take_event_of(session, SessionEventKind::NO_REPLY));
}

TEST_CASE("a controller that has never answered is down at the first failed request") {
  EspTm2Model model;
  Session session(model, config_with({Field::ZONES_RUNNING}));
  FakeController silent;
  session.start(0);
  CHECK_FALSE(session.link().connected_known);
  run_session(session, silent, 0, 5000);
  CHECK(silent.requests.size() == 3);  // one request and its two retries
  CHECK(session.link().connected_known);
  CHECK_FALSE(session.link().connected);
  CHECK(take_event_of(session, SessionEventKind::LINK_DOWN));
}

TEST_CASE("a lost link forgets what was read, keeps which zones exist, and reads it all again") {
  EspTm2Model model;
  SessionConfig config = config_with({Field::ZONES_RUNNING, Field::FREQUENCY});
  config.pages.add(0x10);
  Session session(model, config);
  FakeController fake = FakeController::upgraded_tm2();
  session.start(0);
  uint32_t now = run_session(session, fake, 0, 10000);
  REQUIRE(session.state().running[0] == std::optional<bool>(true));
  REQUIRE(session.state().frequency[0].has_value());
  REQUIRE(session.state().available[0] == std::optional<bool>(true));
  uint32_t readings = session.state().remaining_readings;

  fake.replies.clear();
  for (int i = 0; i < 3; i++) {
    session.poll(now);
    now = run_session(session, fake, now, 5000);
  }
  REQUIRE_FALSE(session.link().connected);
  // A zone shown running may have stopped since, so nothing is shown as known.
  CHECK_FALSE(session.state().running[0].has_value());
  CHECK_FALSE(session.state().frequency[0].has_value());
  // 03 00 is only read at startup, and commands check it.
  CHECK(session.state().available[0] == std::optional<bool>(true));
  // The count moves on, never back, so a watcher cannot mistake a new reading for an old one.
  CHECK(session.state().remaining_readings > readings);

  fake = FakeController::upgraded_tm2();
  fake.requests.clear();
  session.poll(now);
  now = run_session(session, fake, now, 5000);
  session.poll(now);
  run_session(session, fake, now, 5000);
  CHECK(session.link().connected);
  CHECK(session.state().running[0] == std::optional<bool>(true));
  // The schedule too, without waiting for refresh_interval.
  CHECK(seen(fake, "20 00 10"));
  CHECK(session.state().frequency[0].has_value());
}
