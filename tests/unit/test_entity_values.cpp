#include "doctest.h"

#include "countdown.h"
#include "entity_values.h"
#include "hex.h"
#include "model_esp_tm2.h"

#include <string>

using namespace esphome::rainbird;

static void apply(ControllerState &state, const char *hex) {
  EspTm2Model model;
  auto bytes = from_hex(hex);
  Reply reply;
  REQUIRE(parse_reply(bytes.data(), bytes.size(), &reply));
  model.apply_reply(reply, &state);
}

static EntityBinding bind(EntityKind kind, uint8_t zone = NO_ZONE, uint8_t program = NO_PROGRAM, uint8_t index = 0) {
  return EntityBinding{kind, zone, program, index};
}

// What the session would hand the hub, built by hand.
struct Shown {
  ControllerState state;
  Identity identity;
  LinkStatus link;
  bool identity_changed{false};
  LinkChanges link_changes{};

  Readout view() const { return Readout{state, identity, link, identity_changed, link_changes}; }
};

static TextContext yaml_text() {
  TextContext context;
  context.zone = [](uint8_t zone) { return zone == 1 ? std::string("Front lawn") : "Zone " + std::to_string(zone); };
  context.program = [](uint8_t program) { return program == 0 ? std::string("Lawns") : std::string("Beds"); };
  context.model = "ESP-TM2";
  return context;
}

TEST_CASE("every kind has a row, and write paths match the YAML side") {
  for (uint8_t id = 1; id <= static_cast<uint8_t>(EntityKind::CONTROLLER_CLOCK); id++) {
    CHECK(static_cast<uint8_t>(kind_info(static_cast<EntityKind>(id)).kind) == id);
  }
  CHECK(kind_info(EntityKind::RUN_DURATION).write == WritePath::LOCAL);
  CHECK(kind_info(EntityKind::FREQUENCY).write == WritePath::SCHEDULE_PAGE);
  CHECK(kind_info(EntityKind::FREQUENCY).field == Field::FREQUENCY);
}

TEST_CASE("each binding asks the poll plan for its fields and schedule pages") {
  EspTm2Model model;
  SessionConfig config;
  note_binding(model, bind(EntityKind::VALVE, 1), &config);
  note_binding(model, bind(EntityKind::QUEUED_ZONES), &config);
  note_binding(model, bind(EntityKind::QUEUE), &config);
  note_binding(model, bind(EntityKind::RUN_DURATION, 1), &config);
  CHECK(config.used.has(Field::ZONES_RUNNING));
  CHECK(config.used.has(Field::QUEUED_COUNT));
  CHECK(config.used.has(Field::QUEUE));
  CHECK_FALSE(config.used.has(Field::CLOCK));
  // WATERING shows the active zone when someone else polls it, but does not poll it itself.
  SessionConfig watering;
  note_binding(model, bind(EntityKind::WATERING), &watering);
  CHECK_FALSE(watering.used.has(Field::ACTIVE_ZONE));

  // Advance needs the queue list to know which zone is next.
  SessionConfig advance;
  note_binding(model, bind(EntityKind::ADVANCE), &advance);
  CHECK(advance.used.has(Field::QUEUE));

  // So does a queue-mode valve, which advances when it is turned off.
  SessionConfig queue_mode;
  queue_mode.valve_queue = true;
  queue_mode.experimental = true;
  note_binding(model, bind(EntityKind::VALVE, 1), &queue_mode);
  CHECK(queue_mode.used.has(Field::QUEUE));

  // A config with only local settings polls nothing.
  SessionConfig local;
  note_binding(model, bind(EntityKind::RUN_DURATION, 1), &local);
  note_binding(model, bind(EntityKind::PROGRAM_START, NO_ZONE, 0), &local);
  note_binding(model, bind(EntityKind::MODEL), &local);
  CHECK(local.used.bits == 0);

  // Only the pages an entity shows are read: one zone's run time is one page.
  SessionConfig pages;
  note_binding(model, bind(EntityKind::RUN_TIME, 1, 0), &pages);
  note_binding(model, bind(EntityKind::RUN_TIME, 2, 2), &pages);
  note_binding(model, bind(EntityKind::RUN_TIME, 12, 1), &pages);
  note_binding(model, bind(EntityKind::FREQUENCY, NO_ZONE, 1), &pages);
  note_binding(model, bind(EntityKind::START_TIME, NO_ZONE, 2, 3), &pages);
  note_binding(model, bind(EntityKind::ZONE_BYPASS, 5), &pages);
  CHECK(pages.pages.has(0x80));
  CHECK_FALSE(pages.pages.has(0x81));
  CHECK(pages.pages.has(0x85));
  CHECK(pages.pages.has(0x11));
  CHECK_FALSE(pages.pages.has(0x10));
  CHECK(pages.pages.has(0x62));
  CHECK(pages.pages.has(0x20));
  CHECK_FALSE(pages.pages.has(0x00));
}

TEST_CASE("nothing is shown until the controller has said it") {
  Shown shown;
  bool on = true;
  float number = 1;
  std::string text;
  CHECK_FALSE(bool_value(bind(EntityKind::VALVE, 1), shown.view(), &on));
  CHECK_FALSE(bool_value(bind(EntityKind::WATERING), shown.view(), &on));
  CHECK_FALSE(bool_value(bind(EntityKind::RAIN_SENSOR), shown.view(), &on));
  CHECK_FALSE(bool_value(bind(EntityKind::CONNECTED), shown.view(), &on));
  CHECK_FALSE(number_value(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 0), shown.view(), &number));
  CHECK_FALSE(number_value(bind(EntityKind::RESPONSE_TIME), shown.view(), &number));
  CHECK_FALSE(text_value(bind(EntityKind::ACTIVE_PROGRAM), shown.view(), yaml_text(), &text));
  CHECK_FALSE(text_value(bind(EntityKind::QUEUE), shown.view(), yaml_text(), &text));
  CHECK_FALSE(text_value(bind(EntityKind::MODEL), shown.view(), yaml_text(), &text));
}

TEST_CASE("zone, watering, sensor and enabled states come from 3F and 4C") {
  Shown shown;
  apply(shown.state, "BF 00 02 00 00 00");
  bool on = false;
  REQUIRE(bool_value(bind(EntityKind::VALVE, 2), shown.view(), &on));
  CHECK(on);
  REQUIRE(bool_value(bind(EntityKind::ZONE_RUNNING, 1), shown.view(), &on));
  CHECK_FALSE(on);
  REQUIRE(bool_value(bind(EntityKind::WATERING), shown.view(), &on));
  CHECK(on);
  CHECK(kind_changed(bind(EntityKind::VALVE, 2), shown.view()));
  CHECK(kind_changed(bind(EntityKind::WATERING), shown.view()));

  shown.state.clear_changed();
  apply(shown.state, "CC 14 0B 23 08 17 E7 00 03 01 01 FF FF 00 00 00");
  REQUIRE(bool_value(bind(EntityKind::RAIN_SENSOR), shown.view(), &on));
  CHECK(on);
  REQUIRE(bool_value(bind(EntityKind::IRRIGATION_ENABLED), shown.view(), &on));
  CHECK(on);
  CHECK_FALSE(kind_changed(bind(EntityKind::VALVE, 2), shown.view()));
  CHECK(kind_changed(bind(EntityKind::RAIN_SENSOR), shown.view()));
  // 4C also carries the active zone, which WATERING shows.
  CHECK(kind_changed(bind(EntityKind::WATERING), shown.view()));
}

TEST_CASE("program, remaining time and queue come from 3B") {
  Shown shown;
  apply(shown.state, "BB 00 05 00 05 D0 00 00 01 00 00 01");
  float number = 0;
  REQUIRE(number_value(bind(EntityKind::REMAINING_TIME), shown.view(), &number));
  CHECK(number == 1488);
  REQUIRE(number_value(bind(EntityKind::ACTIVE_ZONE), shown.view(), &number));
  CHECK(number == 1);
  REQUIRE(number_value(bind(EntityKind::QUEUED_ZONES), shown.view(), &number));
  CHECK(number == 5);
  CHECK(kind_changed(bind(EntityKind::QUEUED_ZONES), shown.view()));
  bool on = false;
  REQUIRE(bool_value(bind(EntityKind::PROGRAM_RUNNING, NO_ZONE, 0), shown.view(), &on));
  CHECK(on);
  REQUIRE(bool_value(bind(EntityKind::PROGRAM_RUNNING, NO_ZONE, 1), shown.view(), &on));
  CHECK_FALSE(on);
  std::string text;
  REQUIRE(text_value(bind(EntityKind::ACTIVE_PROGRAM), shown.view(), yaml_text(), &text));
  CHECK(text == "Lawns");

  apply(shown.state, "BB 01 21 05 D0 22 04 B0 00 00 00");
  REQUIRE(text_value(bind(EntityKind::QUEUE), shown.view(), yaml_text(), &text));
  CHECK(text == "Front lawn 24:48, Zone 2 20:00");

  Shown manual;
  apply(manual.state, "BB 00 01 00 01 67 00 00 03 FF 00 01");
  REQUIRE(text_value(bind(EntityKind::ACTIVE_PROGRAM), manual.view(), yaml_text(), &text));
  CHECK(text == "Manual");
  REQUIRE(bool_value(bind(EntityKind::PROGRAM_RUNNING, NO_ZONE, 0), manual.view(), &on));
  CHECK_FALSE(on);

  Shown idle;
  apply(idle.state, "BB 00 00 00 00 00 00 00 00 FF 00 00");
  REQUIRE(text_value(bind(EntityKind::ACTIVE_PROGRAM), idle.view(), yaml_text(), &text));
  CHECK(text == "Idle");
  apply(idle.state, "BB 01 00 00 00");
  REQUIRE(text_value(bind(EntityKind::QUEUE), idle.view(), yaml_text(), &text));
  CHECK(text == "Empty");
}

TEST_CASE("seasonal adjust comes from each program's 30 reply") {
  Shown shown;
  apply(shown.state, "B0 01 00 50");
  float number = 0;
  REQUIRE(number_value(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 1), shown.view(), &number));
  CHECK(number == 80);
  CHECK_FALSE(number_value(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 0), shown.view(), &number));
  CHECK(kind_changed(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 1), shown.view()));
  CHECK_FALSE(kind_changed(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 0), shown.view()));
}

TEST_CASE("link and identity entities follow their own change flags") {
  Shown shown;
  shown.link.connected_known = true;
  shown.link.connected = true;
  shown.link.response_known = true;
  shown.link.response_ms = 30;
  shown.link.errors = 2;
  bool on = false;
  float number = 0;
  REQUIRE(bool_value(bind(EntityKind::CONNECTED), shown.view(), &on));
  CHECK(on);
  REQUIRE(number_value(bind(EntityKind::RESPONSE_TIME), shown.view(), &number));
  CHECK(number == 30);
  REQUIRE(number_value(bind(EntityKind::LINK_ERRORS), shown.view(), &number));
  CHECK(number == 2);
  CHECK_FALSE(kind_changed(bind(EntityKind::CONNECTED), shown.view()));
  // A new response time republishes only the response time, not the unchanged error count.
  shown.link_changes.response = true;
  CHECK(kind_changed(bind(EntityKind::RESPONSE_TIME), shown.view()));
  CHECK_FALSE(kind_changed(bind(EntityKind::LINK_ERRORS), shown.view()));
  CHECK_FALSE(kind_changed(bind(EntityKind::CONNECTED), shown.view()));
  shown.link_changes.connected = true;
  CHECK(kind_changed(bind(EntityKind::CONNECTED), shown.view()));
  CHECK_FALSE(kind_changed(bind(EntityKind::MODEL), shown.view()));

  shown.identity.model_known = true;
  shown.identity.model.model_id = 0x000A;
  shown.identity.model.protocol_major = 9;
  shown.identity.model.protocol_minor = 12;
  shown.identity_changed = true;
  std::string text;
  CHECK(kind_changed(bind(EntityKind::MODEL), shown.view()));
  REQUIRE(text_value(bind(EntityKind::MODEL), shown.view(), yaml_text(), &text));
  CHECK(text == "ESP-TM2 (000A), protocol 9.12");
}

TEST_CASE("identity and duration text") {
  Identity identity;
  CHECK(model_text(identity, "ESP-TM2").empty());
  identity.model_known = true;
  identity.model.model_id = 0x000A;
  identity.model.protocol_major = 9;
  identity.model.protocol_minor = 12;
  CHECK(model_text(identity, "ESP-TM2") == "ESP-TM2 (000A), protocol 9.12");
  identity.firmware_known = true;
  identity.firmware.major = 1;
  identity.firmware.minor = 47;
  CHECK(firmware_text(identity) == "1.47.0");
  identity.serial_known = true;
  identity.serial.bytes[6] = 0x89;
  identity.serial.bytes[7] = 0x63;
  CHECK(serial_text(identity) == "0000000000008963");
  CHECK(duration_text(59) == "0:59");
  CHECK(duration_text(1488) == "24:48");
  CHECK(duration_text(4000) == "1:06:40");
}

TEST_CASE("schedule entities read the page fields they are bound to") {
  Shown shown;
  apply(shown.state, "A0 00 10 6A 06 01 00 64 01");
  bool on = true;
  REQUIRE(bool_value(bind(EntityKind::WATERING_DAY, NO_ZONE, 0, 1), shown.view(), &on));
  CHECK_FALSE(on);
  REQUIRE(bool_value(bind(EntityKind::WATERING_DAY, NO_ZONE, 0, 0), shown.view(), &on));
  CHECK(on);
  uint8_t frequency = 9;
  REQUIRE(select_index(bind(EntityKind::FREQUENCY, NO_ZONE, 0), shown.view(), &frequency));
  CHECK(frequency == FREQ_CYCLIC);
  float number = 0;
  REQUIRE(number_value(bind(EntityKind::CYCLE_DAYS, NO_ZONE, 0), shown.view(), &number));
  CHECK(number == 6);
  REQUIRE(number_value(bind(EntityKind::DAYS_UNTIL_NEXT, NO_ZONE, 0), shown.view(), &number));
  CHECK(number == 1);
  CHECK(kind_changed(bind(EntityKind::WATERING_DAY, NO_ZONE, 0, 0), shown.view()));

  apply(shown.state, "A0 00 00 00 00 04 00");
  REQUIRE(number_value(bind(EntityKind::STATION_DELAY), shown.view(), &number));
  CHECK(number == 0);
  REQUIRE(bool_value(bind(EntityKind::GLOBAL_BYPASS), shown.view(), &on));
  CHECK_FALSE(on);

  apply(shown.state, "A0 00 60 00 F0 FF FF FF FF FF FF");
  uint8_t hour = 0;
  uint8_t minute = 0;
  REQUIRE(start_time(bind(EntityKind::START_TIME, NO_ZONE, 0, 0), shown.view(), &hour, &minute));
  CHECK(hour == 4);
  CHECK(minute == 0);
  // An off slot has no time to show.
  CHECK_FALSE(start_time(bind(EntityKind::START_TIME, NO_ZONE, 0, 1), shown.view(), &hour, &minute));

  bool fault = true;
  CHECK_FALSE(bool_value(bind(EntityKind::ZONE_FAULT, 1), shown.view(), &fault));
}

TEST_CASE("the controller clock needs both its time and a real date") {
  Shown shown;
  WallClock clock;
  CHECK_FALSE(controller_clock(shown.view(), &clock));
  apply(shown.state, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00");
  REQUIRE(controller_clock(shown.view(), &clock));
  CHECK(clock.year == 2023);
  CHECK(clock.hour == 20);
  apply(shown.state, "CC 14 0B 23 08 07 E7 00 03 00 01 FF FF 00 00 00");
  CHECK_FALSE(controller_clock(shown.view(), &clock));
}

TEST_CASE("the countdown re-anchors on every reading and shows a steady 0 once") {
  Countdown countdown;
  CHECK(countdown.reading(0, 0));  // the first reading always shows
  CHECK_FALSE(countdown.reading(0, 3000));
  CHECK(countdown.reading(120, 6000));
  CHECK(countdown.remaining(6000) == 120);
  CHECK(countdown.remaining(9999) == 117);
  // Paused: the same value again re-anchors rather than counting on.
  CHECK(countdown.reading(120, 15000));
  CHECK(countdown.remaining(15000) == 120);
  CHECK(countdown.remaining(15000 + 200000) == 0);
  CHECK(countdown.reading(0, 20000));
  CHECK_FALSE(countdown.reading(0, 23000));
}
