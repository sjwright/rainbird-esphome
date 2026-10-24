#pragma once

#include <cstdint>

namespace esphome::rainbird {

// One value per row of entity_kinds.py (KindId). tests/python/test_entity_kinds.py
// checks the two stay in step.
enum class EntityKind : uint8_t {
  VALVE = 1,
  RUN_DURATION = 2,
  PROGRAM_START = 3,
  SEASONAL_ADJUST = 4,
  ZONE_RUNNING = 5,
  PROGRAM_RUNNING = 6,
  REMAINING_TIME = 7,
  ACTIVE_ZONE = 8,
  QUEUED_ZONES = 9,
  RESPONSE_TIME = 10,
  LINK_ERRORS = 11,
  WATERING = 12,
  RAIN_SENSOR = 13,
  IRRIGATION_ENABLED = 14,
  CONNECTED = 15,
  ACTIVE_PROGRAM = 16,
  QUEUE = 17,
  MODEL = 18,
  FIRMWARE_VERSION = 19,
  SERIAL_NUMBER = 20,
  SCHEDULE_CHANGED = 21,
  QUEUE_ZONE = 22,
  STOP = 23,
  ADVANCE = 24,
  TEST_ALL = 25,
  SYNC_CLOCK = 26,
  REFRESH = 27,
  RAIN_DELAY = 28,
  STATION_DELAY = 29,
  TEST_DURATION = 30,
  CYCLE_DAYS = 31,
  DAYS_UNTIL_NEXT = 32,
  RUN_TIME = 33,
  IRRIGATION_SET = 34,
  GLOBAL_BYPASS = 35,
  WATERING_DAY = 36,
  START_ENABLED = 37,
  FREQUENCY = 38,
  START_TIME = 39,
  CONTROLLER_CLOCK = 40,
};

// Zone 0 and program 255 mean "this binding does not use that index".
// Program 0 is program A, so 0 cannot mean "none". Keep in step with entity_kinds.py.
static constexpr uint8_t NO_ZONE = 0;
static constexpr uint8_t NO_PROGRAM = 255;

}  // namespace esphome::rainbird
