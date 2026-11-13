#include "entity_writes.h"

#include <cmath>

namespace esphome::rainbird {

namespace {

WritePlan refused(const char *why) {
  WritePlan plan;
  plan.action = WriteAction::REFUSED;
  plan.refusal = why;
  return plan;
}

WritePlan control(ControlOp op, uint8_t zone = 0, uint8_t program = 0, uint16_t value = 0) {
  WritePlan plan;
  plan.action = WriteAction::CONTROL;
  plan.control.op = op;
  plan.control.zone = zone;
  plan.control.program = program;
  plan.control.value = value;
  return plan;
}

WritePlan edit(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value) {
  WritePlan plan;
  plan.action = WriteAction::EDIT;
  plan.edit.field = field;
  plan.edit.zone = zone;
  plan.edit.program = program;
  plan.edit.index = index;
  plan.edit.value = value;
  return plan;
}

WritePlan action(WriteAction what) {
  WritePlan plan;
  plan.action = what;
  return plan;
}

}  // namespace

int32_t rounded(float value) {
  if (value >= 0)
    return static_cast<int32_t>(value + 0.5f);
  return static_cast<int32_t>(value - 0.5f);
}

bool whole_minutes(float value, uint8_t *minutes) {
  if (std::isnan(value))
    return false;
  // Held in float first: a huge value must not overflow the conversion.
  if (value < 1.0f)
    value = 1.0f;
  if (value > static_cast<float>(MAX_RUN_MINUTES))
    value = static_cast<float>(MAX_RUN_MINUTES);
  *minutes = static_cast<uint8_t>(rounded(value));
  return true;
}

WritePlan plan_write(const EntityBinding &binding, float value, const WriteContext &context) {
  // A number with no value. Switches, buttons and selects never pass one.
  if (std::isnan(value))
    return refused("no value");
  switch (binding.kind) {
    case EntityKind::VALVE:
    case EntityKind::QUEUE_ZONE: {
      // Whether a valve starts or queues its zone, and what turning it off does, is the session's
      // valve_queue setting.
      if (binding.kind == EntityKind::VALVE && value == 0)
        return control(ControlOp::VALVE_OFF, binding.zone);
      float setting = 0;
      uint8_t minutes = 0;
      if (!context.run_duration || !context.run_duration(binding.zone, &setting) || !whole_minutes(setting, &minutes))
        return refused("that zone has no run duration");
      return control(binding.kind == EntityKind::VALVE ? ControlOp::VALVE_ON : ControlOp::QUEUE_ZONE, binding.zone, 0,
                     minutes);
    }
    case EntityKind::PROGRAM_START:
      return control(ControlOp::RUN_PROGRAM, 0, binding.program);
    case EntityKind::STOP:
      return control(ControlOp::STOP);
    case EntityKind::ADVANCE:
      return control(ControlOp::ADVANCE);
    case EntityKind::TEST_ALL: {
      float setting = 0;
      uint8_t minutes = 0;
      if (!context.test_duration || !context.test_duration(&setting) || !whole_minutes(setting, &minutes))
        return refused("test all zones needs a test duration");
      return control(ControlOp::TEST_ZONES, 0, 0, minutes);
    }
    case EntityKind::SEASONAL_ADJUST:
    case EntityKind::RAIN_DELAY: {
      // The session checks the model's range. Outside uint16 is out of every range, and
      // must not wrap into one.
      if (value < 0 || value > 65535.0f)
        return refused("out of range");
      int32_t whole = rounded(value);
      if (binding.kind == EntityKind::RAIN_DELAY)
        return control(ControlOp::SET_RAIN_DELAY, 0, 0, static_cast<uint16_t>(whole));
      return control(ControlOp::SET_SEASONAL, 0, binding.program, static_cast<uint16_t>(whole));
    }
    case EntityKind::IRRIGATION_SET:
      return control(ControlOp::SET_IRRIGATION, 0, 0, value != 0 ? 1 : 0);
    case EntityKind::REFRESH:
      return action(WriteAction::REFRESH);
    case EntityKind::SYNC_CLOCK:
      return action(WriteAction::SYNC_CLOCK);
    case EntityKind::START_ENABLED: {
      if (value == 0)
        return edit(Field::START_ENABLED, NO_ZONE, binding.program, binding.index, 0);
      // Turning a slot on writes the time its time entity shows. START_ENABLED=1 would invent one.
      uint16_t minutes = 0;
      if (!context.slot_time || !context.slot_time(binding.program, binding.index, &minutes))
        return refused("that start slot has no time set");
      return edit(Field::START_TIME, NO_ZONE, binding.program, binding.index, minutes);
    }
    default: {
      const KindInfo &info = kind_info(binding.kind);
      if (info.write != WritePath::SCHEDULE_PAGE)
        return refused("that entity is not writable");
      // Held to what a page field could ever take, so the conversion cannot overflow. The page
      // codec then refuses anything its field does not hold.
      if (value < 0 || value > 65535.0f)
        return refused("out of range");
      return edit(info.field, binding.zone, binding.program, binding.index, rounded(value));
    }
  }
}

WritePlan plan_zone_run(int32_t zone, uint32_t duration_ms, bool queue) {
  // Refused, not clamped. A negative minutes value in a template wraps to a huge duration,
  // and clamping that would water for over four hours.
  if (zone < 1 || zone > 255)
    return refused("that is not a zone number");
  if (duration_ms == 0 || duration_ms > MAX_RUN_MINUTES * 60000u)
    return refused("the duration is outside 1 ms to 255 minutes");
  // Whole minutes, so a part minute rounds up.
  return control(queue ? ControlOp::QUEUE_ZONE : ControlOp::RUN_ZONE, static_cast<uint8_t>(zone), 0,
                 static_cast<uint16_t>((duration_ms + 59999u) / 60000u));
}

}  // namespace esphome::rainbird
