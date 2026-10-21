#pragma once

#include <cstdint>

namespace esphome::rainbird {

// Clock sync policy (PLAN 6.5), without ESPHome, so it runs in host tests.

// A wall-clock reading with no time zone: the controller's clock, or local time from time_id.
struct WallClock {
  uint16_t year{0};
  uint8_t month{0};
  uint8_t day{0};
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t second{0};
};

// Seconds since 2000-01-01 00:00 on the same wall clock.
int64_t wall_seconds(const WallClock &clock);

enum class ClockCheck : uint8_t {
  IN_STEP,  // within max_drift
  SYNC,     // write the local time, then the local date
  WAIT,     // a sync is due, but local midnight is too close (see check_clock)
};

// No sync this close to local midnight, in seconds either side.
static constexpr uint32_t CLOCK_MIDNIGHT_GUARD_S = 5;

// `controller` is a reading that arrived `age_ms` ago; the controller clock has run on since.
// `date_valid` is false when the controller reported a date that is not a real date.
// Date and time are compared as one instant, so 23:59:50 and 00:00:10 the next day are
// 20 seconds apart. A sync writes time and date as two commands, and the controller could
// pass midnight between them, so near midnight a due sync waits for a later reading.
ClockCheck check_clock(const WallClock &controller, bool date_valid, uint32_t age_ms, const WallClock &local,
                       uint32_t max_drift_ms);

}  // namespace esphome::rainbird
