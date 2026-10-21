#include "doctest.h"

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

static Names yaml_names() {
  Names names;
  names.zone = [](uint8_t zone) { return zone == 1 ? std::string("Front lawn") : "Zone " + std::to_string(zone); };
  names.program = [](uint8_t program) { return program == 0 ? std::string("Lawns") : std::string("Beds"); };
  return names;
}

TEST_CASE("each kind asks the poll plan for the fields it reads") {
  FieldSet used;
  add_fields(EntityKind::VALVE, &used);
  add_fields(EntityKind::QUEUED_ZONES, &used);
  add_fields(EntityKind::QUEUE, &used);
  add_fields(EntityKind::RUN_DURATION, &used);
  CHECK(used.has(Field::ZONES_RUNNING));
  CHECK(used.has(Field::ACTIVE_ZONE));
  CHECK(used.has(Field::QUEUE));
  CHECK_FALSE(used.has(Field::CLOCK));

  // Advance needs the queue list to know which zone is next.
  FieldSet advance;
  add_fields(EntityKind::ADVANCE, &advance);
  CHECK(advance.has(Field::QUEUE));

  // A config with only local settings polls nothing.
  FieldSet local;
  add_fields(EntityKind::RUN_DURATION, &local);
  add_fields(EntityKind::PROGRAM_START, &local);
  add_fields(EntityKind::MODEL, &local);
  CHECK(local.bits == 0);
}

TEST_CASE("nothing is shown until the controller has said it") {
  ControllerState state;
  bool on = true;
  float number = 1;
  std::string text;
  CHECK_FALSE(bool_value(EntityKind::VALVE, state, 1, NO_PROGRAM, &on));
  CHECK_FALSE(bool_value(EntityKind::WATERING, state, NO_ZONE, NO_PROGRAM, &on));
  CHECK_FALSE(bool_value(EntityKind::RAIN_SENSOR, state, NO_ZONE, NO_PROGRAM, &on));
  CHECK_FALSE(number_value(EntityKind::SEASONAL_ADJUST, state, NO_ZONE, 0, &number));
  CHECK_FALSE(active_program_text(state, yaml_names(), &text));
  CHECK_FALSE(queue_text(state, yaml_names(), &text));
}

TEST_CASE("zone, watering, sensor and enabled states come from 3F and 4C") {
  ControllerState state;
  apply(state, "BF 00 02 00 00 00");
  bool on = false;
  REQUIRE(bool_value(EntityKind::VALVE, state, 2, NO_PROGRAM, &on));
  CHECK(on);
  REQUIRE(bool_value(EntityKind::ZONE_RUNNING, state, 1, NO_PROGRAM, &on));
  CHECK_FALSE(on);
  REQUIRE(bool_value(EntityKind::WATERING, state, NO_ZONE, NO_PROGRAM, &on));
  CHECK(on);
  CHECK(kind_changed(EntityKind::VALVE, state, 2, NO_PROGRAM));
  CHECK(kind_changed(EntityKind::WATERING, state, NO_ZONE, NO_PROGRAM));

  state.clear_changed();
  apply(state, "CC 14 0B 23 08 17 E7 00 03 01 01 FF FF 00 00 00");
  REQUIRE(bool_value(EntityKind::RAIN_SENSOR, state, NO_ZONE, NO_PROGRAM, &on));
  CHECK(on);
  REQUIRE(bool_value(EntityKind::IRRIGATION_ENABLED, state, NO_ZONE, NO_PROGRAM, &on));
  CHECK(on);
  CHECK_FALSE(kind_changed(EntityKind::VALVE, state, 2, NO_PROGRAM));
  CHECK(kind_changed(EntityKind::RAIN_SENSOR, state, NO_ZONE, NO_PROGRAM));
}

TEST_CASE("program, remaining time and queue come from 3B") {
  ControllerState state;
  apply(state, "BB 00 05 00 05 D0 00 00 01 00 00 01");
  float number = 0;
  REQUIRE(number_value(EntityKind::REMAINING_TIME, state, NO_ZONE, NO_PROGRAM, &number));
  CHECK(number == 1488);
  REQUIRE(number_value(EntityKind::ACTIVE_ZONE, state, NO_ZONE, NO_PROGRAM, &number));
  CHECK(number == 1);
  REQUIRE(number_value(EntityKind::QUEUED_ZONES, state, NO_ZONE, NO_PROGRAM, &number));
  CHECK(number == 5);
  bool on = false;
  REQUIRE(bool_value(EntityKind::PROGRAM_RUNNING, state, NO_ZONE, 0, &on));
  CHECK(on);
  REQUIRE(bool_value(EntityKind::PROGRAM_RUNNING, state, NO_ZONE, 1, &on));
  CHECK_FALSE(on);
  std::string text;
  REQUIRE(active_program_text(state, yaml_names(), &text));
  CHECK(text == "Lawns");

  apply(state, "BB 01 21 05 D0 22 04 B0 00 00 00");
  REQUIRE(queue_text(state, yaml_names(), &text));
  CHECK(text == "Front lawn 24:48, Zone 2 20:00");

  ControllerState manual;
  apply(manual, "BB 00 01 00 01 67 00 00 03 FF 00 01");
  REQUIRE(active_program_text(manual, yaml_names(), &text));
  CHECK(text == "Manual");
  REQUIRE(bool_value(EntityKind::PROGRAM_RUNNING, manual, NO_ZONE, 0, &on));
  CHECK_FALSE(on);

  ControllerState idle;
  apply(idle, "BB 00 00 00 00 00 00 00 00 FF 00 00");
  REQUIRE(active_program_text(idle, yaml_names(), &text));
  CHECK(text == "Idle");
  apply(idle, "BB 01 00 00 00");
  REQUIRE(queue_text(idle, yaml_names(), &text));
  CHECK(text == "Empty");
}

TEST_CASE("seasonal adjust comes from each program's 30 reply") {
  ControllerState state;
  apply(state, "B0 01 00 50");
  float number = 0;
  REQUIRE(number_value(EntityKind::SEASONAL_ADJUST, state, NO_ZONE, 1, &number));
  CHECK(number == 80);
  CHECK_FALSE(number_value(EntityKind::SEASONAL_ADJUST, state, NO_ZONE, 0, &number));
  CHECK(kind_changed(EntityKind::SEASONAL_ADJUST, state, NO_ZONE, 1));
  CHECK_FALSE(kind_changed(EntityKind::SEASONAL_ADJUST, state, NO_ZONE, 0));
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
  ControllerState state;
  apply(state, "A0 00 10 6A 06 01 00 64 01");
  bool on = true;
  REQUIRE(bool_value(EntityKind::WATERING_DAY, state, NO_ZONE, 0, &on, 1));
  CHECK_FALSE(on);
  REQUIRE(bool_value(EntityKind::WATERING_DAY, state, NO_ZONE, 0, &on, 0));
  CHECK(on);
  uint8_t frequency = 9;
  REQUIRE(select_index(EntityKind::FREQUENCY, state, NO_ZONE, 0, &frequency));
  CHECK(frequency == FREQ_CYCLIC);
  float number = 0;
  REQUIRE(number_value(EntityKind::CYCLE_DAYS, state, NO_ZONE, 0, &number));
  CHECK(number == 6);
  REQUIRE(number_value(EntityKind::DAYS_UNTIL_NEXT, state, NO_ZONE, 0, &number));
  CHECK(number == 1);
  CHECK(kind_changed(EntityKind::WATERING_DAY, state, NO_ZONE, 0, 0));

  apply(state, "A0 00 00 00 00 04 00");
  REQUIRE(number_value(EntityKind::STATION_DELAY, state, NO_ZONE, NO_PROGRAM, &number));
  CHECK(number == 0);
  REQUIRE(number_value(EntityKind::RAIN_DELAY, state, NO_ZONE, NO_PROGRAM, &number));
  CHECK(number == 4);
  REQUIRE(bool_value(EntityKind::GLOBAL_BYPASS, state, NO_ZONE, NO_PROGRAM, &on));
  CHECK_FALSE(on);

  FieldSet used;
  add_fields(EntityKind::RUN_TIME, &used);
  add_fields(EntityKind::ZONE_FAULT, &used);
  add_fields(EntityKind::CONTROLLER_CLOCK, &used);
  CHECK(used.has(Field::RUN_TIME));
  CHECK(used.has(Field::ZONE_FAULT));
  CHECK(used.has(Field::CLOCK));
  bool fault = true;
  CHECK_FALSE(bool_value(EntityKind::ZONE_FAULT, state, 1, NO_PROGRAM, &fault));
}
