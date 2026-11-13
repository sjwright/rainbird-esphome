#include "doctest.h"

#include "entity_writes.h"

#include <cmath>

using namespace esphome::rainbird;

namespace {

// Zone 1 runs for 10 minutes, zone 2 has a duration entity with no value yet, zone 3 has none.
// The test duration is 2 minutes. Program A slot 1 shows 06:30; no other slot has a time.
WriteContext context() {
  WriteContext context;
  context.run_duration = [](uint8_t zone, float *minutes) {
    if (zone == 1) {
      *minutes = 10.0f;
      return true;
    }
    if (zone == 2) {
      *minutes = NAN;
      return true;
    }
    return false;
  };
  context.test_duration = [](float *minutes) {
    *minutes = 2.0f;
    return true;
  };
  context.slot_time = [](uint8_t program, uint8_t index, uint16_t *minutes) {
    if (program != 0 || index != 0) {
      return false;
    }
    *minutes = 6 * 60 + 30;
    return true;
  };
  return context;
}

EntityBinding bind(EntityKind kind, uint8_t zone = NO_ZONE, uint8_t program = NO_PROGRAM, uint8_t index = 0) {
  return EntityBinding{kind, zone, program, index};
}

}  // namespace

TEST_CASE("a valve on runs its zone for the run duration; off needs no duration") {
  WritePlan on = plan_write(bind(EntityKind::VALVE, 1), 1.0f, context());
  REQUIRE(on.action == WriteAction::CONTROL);
  CHECK(on.control.op == ControlOp::VALVE_ON);
  CHECK(on.control.zone == 1);
  CHECK(on.control.value == 10);

  WritePlan off = plan_write(bind(EntityKind::VALVE, 3), 0.0f, context());
  REQUIRE(off.action == WriteAction::CONTROL);
  CHECK(off.control.op == ControlOp::VALVE_OFF);
  CHECK(off.control.zone == 3);

  WritePlan queue = plan_write(bind(EntityKind::QUEUE_ZONE, 1), 0.0f, context());
  REQUIRE(queue.action == WriteAction::CONTROL);
  CHECK(queue.control.op == ControlOp::QUEUE_ZONE);
  CHECK(queue.control.value == 10);
}

TEST_CASE("a zone with no run duration, or one with no value yet, is refused") {
  CHECK(plan_write(bind(EntityKind::VALVE, 3), 1.0f, context()).action == WriteAction::REFUSED);
  CHECK(plan_write(bind(EntityKind::VALVE, 2), 1.0f, context()).action == WriteAction::REFUSED);
  CHECK(plan_write(bind(EntityKind::QUEUE_ZONE, 3), 0.0f, context()).action == WriteAction::REFUSED);
  // No lookups at all, as in a build with no number entities.
  WritePlan bare = plan_write(bind(EntityKind::VALVE, 1), 1.0f, WriteContext{});
  CHECK(bare.action == WriteAction::REFUSED);
  CHECK(bare.refusal != nullptr);
  CHECK(plan_write(bind(EntityKind::TEST_ALL), 0.0f, WriteContext{}).action == WriteAction::REFUSED);
}

TEST_CASE("a minutes setting is rounded and held to the one byte the command takes") {
  uint8_t minutes = 0;
  CHECK(whole_minutes(10.4f, &minutes));
  CHECK(minutes == 10);
  CHECK(whole_minutes(10.5f, &minutes));
  CHECK(minutes == 11);
  CHECK(whole_minutes(0.0f, &minutes));
  CHECK(minutes == 1);
  CHECK(whole_minutes(-5.0f, &minutes));
  CHECK(minutes == 1);
  CHECK(whole_minutes(1e12f, &minutes));
  CHECK(minutes == 255);
  CHECK_FALSE(whole_minutes(NAN, &minutes));
}

TEST_CASE("buttons become their commands") {
  WritePlan start = plan_write(bind(EntityKind::PROGRAM_START, NO_ZONE, 2), 0.0f, context());
  REQUIRE(start.action == WriteAction::CONTROL);
  CHECK(start.control.op == ControlOp::RUN_PROGRAM);
  CHECK(start.control.program == 2);
  CHECK(plan_write(bind(EntityKind::STOP), 0.0f, context()).control.op == ControlOp::STOP);
  CHECK(plan_write(bind(EntityKind::ADVANCE), 0.0f, context()).control.op == ControlOp::ADVANCE);
  WritePlan test = plan_write(bind(EntityKind::TEST_ALL), 0.0f, context());
  REQUIRE(test.action == WriteAction::CONTROL);
  CHECK(test.control.op == ControlOp::TEST_ZONES);
  CHECK(test.control.value == 2);
  CHECK(plan_write(bind(EntityKind::REFRESH), 0.0f, context()).action == WriteAction::REFRESH);
  CHECK(plan_write(bind(EntityKind::SYNC_CLOCK), 0.0f, context()).action == WriteAction::SYNC_CLOCK);
}

TEST_CASE("numbers the controller holds are rounded, and never wrapped into range") {
  WritePlan seasonal = plan_write(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 1), 79.6f, context());
  REQUIRE(seasonal.action == WriteAction::CONTROL);
  CHECK(seasonal.control.op == ControlOp::SET_SEASONAL);
  CHECK(seasonal.control.program == 1);
  CHECK(seasonal.control.value == 80);
  WritePlan delay = plan_write(bind(EntityKind::RAIN_DELAY), 3.0f, context());
  REQUIRE(delay.action == WriteAction::CONTROL);
  CHECK(delay.control.op == ControlOp::SET_RAIN_DELAY);
  CHECK(delay.control.value == 3);
  // -1 as uint16 would be 65535, and 65636 would be 100.
  CHECK(plan_write(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 0), -1.0f, context()).action == WriteAction::REFUSED);
  CHECK(plan_write(bind(EntityKind::SEASONAL_ADJUST, NO_ZONE, 0), 65636.0f, context()).action ==
        WriteAction::REFUSED);
  CHECK(plan_write(bind(EntityKind::RAIN_DELAY), 1e12f, context()).action == WriteAction::REFUSED);
  CHECK(plan_write(bind(EntityKind::RAIN_DELAY), NAN, context()).action == WriteAction::REFUSED);
  WritePlan irrigation = plan_write(bind(EntityKind::IRRIGATION_SET), 1.0f, context());
  CHECK(irrigation.control.op == ControlOp::SET_IRRIGATION);
  CHECK(irrigation.control.value == 1);
}

TEST_CASE("turning a start slot on writes the time its entity shows; off writes the slot off") {
  WritePlan on = plan_write(bind(EntityKind::START_ENABLED, NO_ZONE, 0, 0), 1.0f, context());
  REQUIRE(on.action == WriteAction::EDIT);
  CHECK(on.edit.field == Field::START_TIME);
  CHECK(on.edit.program == 0);
  CHECK(on.edit.index == 0);
  CHECK(on.edit.value == 390);

  WritePlan off = plan_write(bind(EntityKind::START_ENABLED, NO_ZONE, 0, 0), 0.0f, context());
  REQUIRE(off.action == WriteAction::EDIT);
  CHECK(off.edit.field == Field::START_ENABLED);
  CHECK(off.edit.value == 0);

  // Slot 2 has no time to write.
  CHECK(plan_write(bind(EntityKind::START_ENABLED, NO_ZONE, 0, 1), 1.0f, context()).action == WriteAction::REFUSED);
  // Off needs none.
  CHECK(plan_write(bind(EntityKind::START_ENABLED, NO_ZONE, 0, 1), 0.0f, context()).action == WriteAction::EDIT);
}

TEST_CASE("schedule-page kinds edit the field the kind table names") {
  WritePlan run = plan_write(bind(EntityKind::RUN_TIME, 5, 1), 25.0f, context());
  REQUIRE(run.action == WriteAction::EDIT);
  CHECK(run.edit.field == Field::RUN_TIME);
  CHECK(run.edit.zone == 5);
  CHECK(run.edit.program == 1);
  CHECK(run.edit.value == 25);
  WritePlan day = plan_write(bind(EntityKind::WATERING_DAY, NO_ZONE, 2, 6), 1.0f, context());
  REQUIRE(day.action == WriteAction::EDIT);
  CHECK(day.edit.field == Field::WATERING_DAY);
  CHECK(day.edit.index == 6);
  CHECK(plan_write(bind(EntityKind::FREQUENCY, NO_ZONE, 0), 3.0f, context()).edit.field == Field::FREQUENCY);
  CHECK(plan_write(bind(EntityKind::START_TIME, NO_ZONE, 0, 1), 390.0f, context()).edit.field == Field::START_TIME);
  CHECK(plan_write(bind(EntityKind::STATION_DELAY), 1e9f, context()).action == WriteAction::REFUSED);
}

TEST_CASE("read-only, local and unknown kinds are refused") {
  CHECK(plan_write(bind(EntityKind::ZONE_RUNNING, 1), 1.0f, context()).action == WriteAction::REFUSED);
  CHECK(plan_write(bind(EntityKind::RUN_DURATION, 1), 5.0f, context()).action == WriteAction::REFUSED);
  CHECK(plan_write(bind(static_cast<EntityKind>(0)), 1.0f, context()).action == WriteAction::REFUSED);
  CHECK(plan_write(bind(static_cast<EntityKind>(200)), 1.0f, context()).action == WriteAction::REFUSED);
}

TEST_CASE("a zone run from an action is refused outside its ranges, never clamped") {
  WritePlan run = plan_zone_run(3, 6 * 60000, false);
  REQUIRE(run.action == WriteAction::CONTROL);
  CHECK(run.control.op == ControlOp::RUN_ZONE);
  CHECK(run.control.zone == 3);
  CHECK(run.control.value == 6);
  CHECK(plan_zone_run(3, 6 * 60000, true).control.op == ControlOp::QUEUE_ZONE);
  // A part minute rounds up.
  CHECK(plan_zone_run(1, 1, false).control.value == 1);
  CHECK(plan_zone_run(1, 60001, false).control.value == 2);
  CHECK(plan_zone_run(1, 255u * 60000u, false).control.value == 255);
  CHECK(plan_zone_run(1, 255u * 60000u + 1, false).action == WriteAction::REFUSED);
  CHECK(plan_zone_run(1, 0, false).action == WriteAction::REFUSED);
  // -1 minutes in a template, as uint32 milliseconds.
  CHECK(plan_zone_run(1, static_cast<uint32_t>(-60000), false).action == WriteAction::REFUSED);
  CHECK(plan_zone_run(0, 60000, false).action == WriteAction::REFUSED);
  CHECK(plan_zone_run(-1, 60000, false).action == WriteAction::REFUSED);
  CHECK(plan_zone_run(256, 60000, false).action == WriteAction::REFUSED);
}
