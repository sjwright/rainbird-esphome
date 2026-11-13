#pragma once

#include <cstdint>
#include <functional>

#include "controller_state.h"
#include "entity_values.h"
#include "session.h"

namespace esphome::rainbird {

// What a write from an entity or an action becomes (PLAN 5.3, 6.5): the other half of
// entity_values. Pure, so the rules run in host tests and the hub only carries the plan out.

// Local values a write reads: the settings kept on this device, and what a start slot's
// time entity shows. Each returns false when there is no such entity or it has no value yet.
struct WriteContext {
  std::function<bool(uint8_t zone, float *minutes)> run_duration;
  std::function<bool(float *minutes)> test_duration;
  std::function<bool(uint8_t program, uint8_t index, uint16_t *minutes)> slot_time;
};

enum class WriteAction : uint8_t {
  REFUSED,     // `refusal` says why
  CONTROL,     // Session::control(control)
  EDIT,        // Session::edit_schedule(edit)
  REFRESH,     // Session::refresh()
  SYNC_CLOCK,  // set the controller clock from time_id, which only the hub has
};

struct ScheduleEdit {
  Field field{Field::RUN_TIME};
  uint8_t zone{NO_ZONE};
  uint8_t program{0};
  uint8_t index{0};
  int32_t value{0};
};

struct WritePlan {
  WriteAction action{WriteAction::REFUSED};
  Control control{};
  ScheduleEdit edit{};
  const char *refusal{nullptr};  // a static string
};

// 39, 4B and 3A take whole minutes in one byte.
static constexpr uint32_t MAX_RUN_MINUTES = TM2_RUN_MAX_MINUTES;

// To the nearest whole number, halves away from zero.
int32_t rounded(float value);
// A local minutes setting as the one byte 39, 4B and 3A take: rounded, then held to 1-255.
// False for NaN, a setting with no value.
bool whole_minutes(float value, uint8_t *minutes);

// Every write from an entity. `value` is the switch state, number, option index or minutes.
WritePlan plan_write(const EntityBinding &binding, float value, const WriteContext &context);
// rainbird.zone.start / queue. The values may come from a template: anything outside zone
// 1-255 or 1 ms to 255 minutes is refused, never clamped. A part minute rounds up.
WritePlan plan_zone_run(int32_t zone, uint32_t duration_ms, bool queue);

}  // namespace esphome::rainbird
