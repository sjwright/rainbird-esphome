#include "clock_sync.h"

namespace esphome::rainbird {

namespace {

constexpr int64_t SECONDS_PER_DAY = 86400;

// Days from 2000-01-01 to a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int64_t days_since_2000(int32_t year, int32_t month, int32_t day) {
  year -= month <= 2 ? 1 : 0;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const int32_t year_of_era = year - era * 400;
  const int32_t day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const int32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  // 730425 is days_from_civil(2000, 1, 1) counted from 0000-03-01.
  return static_cast<int64_t>(era) * 146097 + day_of_era - 730425;
}

}  // namespace

int64_t wall_seconds(const WallClock &clock) {
  return days_since_2000(clock.year, clock.month, clock.day) * SECONDS_PER_DAY + clock.hour * 3600 + clock.minute * 60 +
         clock.second;
}

ClockCheck check_clock(const WallClock &controller, bool date_valid, uint32_t age_ms, const WallClock &local,
                       uint32_t max_drift_ms) {
  bool due = !date_valid;
  if (date_valid) {
    int64_t now_on_controller = wall_seconds(controller) + (age_ms + 500) / 1000;
    int64_t gap = wall_seconds(local) - now_on_controller;
    if (gap < 0) {
      gap = -gap;
    }
    due = gap * 1000 > max_drift_ms;
  }
  if (!due) {
    return ClockCheck::IN_STEP;
  }
  int64_t second_of_day = local.hour * 3600 + local.minute * 60 + local.second;
  if (second_of_day < CLOCK_MIDNIGHT_GUARD_S || second_of_day >= SECONDS_PER_DAY - CLOCK_MIDNIGHT_GUARD_S) {
    return ClockCheck::WAIT;
  }
  return ClockCheck::SYNC;
}

}  // namespace esphome::rainbird
