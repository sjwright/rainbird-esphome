#include "doctest.h"

#include "clock_sync.h"

using namespace esphome::rainbird;

static WallClock at(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
  WallClock clock;
  clock.year = year;
  clock.month = month;
  clock.day = day;
  clock.hour = hour;
  clock.minute = minute;
  clock.second = second;
  return clock;
}

TEST_CASE("wall seconds count days across months, leap years and new year") {
  CHECK(wall_seconds(at(2000, 1, 1, 0, 0, 0)) == 0);
  CHECK(wall_seconds(at(2000, 3, 1, 0, 0, 0)) == 60 * 86400);  // 2000 is a leap year
  CHECK(wall_seconds(at(2026, 10, 21, 0, 0, 0)) == 845856000);
  CHECK(wall_seconds(at(2023, 12, 31, 23, 59, 59)) + 1 == wall_seconds(at(2024, 1, 1, 0, 0, 0)));
  CHECK(wall_seconds(at(2024, 1, 1, 0, 0, 0)) == 757382400);
}

TEST_CASE("drift within max_drift leaves the clock alone; beyond it syncs") {
  WallClock controller = at(2026, 10, 21, 12, 0, 0);
  CHECK(check_clock(controller, true, 0, at(2026, 10, 21, 12, 0, 20), 30000) == ClockCheck::IN_STEP);
  CHECK(check_clock(controller, true, 0, at(2026, 10, 21, 12, 0, 31), 30000) == ClockCheck::SYNC);
  CHECK(check_clock(controller, true, 0, at(2026, 10, 21, 11, 59, 29), 30000) == ClockCheck::SYNC);
  // Daylight saving moves local time an hour.
  CHECK(check_clock(controller, true, 0, at(2026, 10, 21, 13, 0, 0), 30000) == ClockCheck::SYNC);
}

TEST_CASE("the reading is moved on by its age before comparing") {
  // Read a minute ago. The controller clock has run on since, so it is in step.
  WallClock controller = at(2026, 10, 21, 12, 0, 0);
  CHECK(check_clock(controller, true, 60000, at(2026, 10, 21, 12, 1, 0), 30000) == ClockCheck::IN_STEP);
  CHECK(check_clock(controller, true, 0, at(2026, 10, 21, 12, 1, 0), 30000) == ClockCheck::SYNC);
}

TEST_CASE("a few seconds either side of midnight are a few seconds, not a day") {
  // The controller is 20 s behind, so it is still yesterday when local time is past midnight.
  CHECK(check_clock(at(2026, 10, 20, 23, 59, 50), true, 0, at(2026, 10, 21, 0, 0, 10), 30000) == ClockCheck::IN_STEP);
  // And 20 s ahead.
  CHECK(check_clock(at(2026, 10, 21, 0, 0, 10), true, 0, at(2026, 10, 20, 23, 59, 50), 30000) == ClockCheck::IN_STEP);
  // The right time on the wrong day is a day of drift.
  CHECK(check_clock(at(2026, 10, 20, 12, 0, 0), true, 0, at(2026, 10, 21, 12, 0, 0), 30000) == ClockCheck::SYNC);
}

TEST_CASE("a date that is not a real date syncs") {
  CHECK(check_clock(at(2000, 0, 0, 12, 0, 0), false, 0, at(2026, 10, 21, 12, 0, 0), 30000) == ClockCheck::SYNC);
}

TEST_CASE("a due sync waits while local midnight is near") {
  // Time and date are two commands. The controller could pass midnight between them.
  WallClock wrong_day = at(2026, 10, 19, 12, 0, 0);
  CHECK(check_clock(wrong_day, true, 0, at(2026, 10, 20, 23, 59, 54), 30000) == ClockCheck::SYNC);
  CHECK(check_clock(wrong_day, true, 0, at(2026, 10, 20, 23, 59, 55), 30000) == ClockCheck::WAIT);
  CHECK(check_clock(wrong_day, true, 0, at(2026, 10, 21, 0, 0, 4), 30000) == ClockCheck::WAIT);
  CHECK(check_clock(wrong_day, true, 0, at(2026, 10, 21, 0, 0, 5), 30000) == ClockCheck::SYNC);
  CHECK(check_clock(wrong_day, false, 0, at(2026, 10, 21, 0, 0, 0), 30000) == ClockCheck::WAIT);
}
