#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "sip_commands.h"

namespace esphome::rainbird {

// Sized for the ESP-TM2. There is one C++ model, so the store matches its limits.
static constexpr uint8_t STATE_ZONES = 12;
static constexpr uint8_t STATE_PROGRAMS = 3;
static constexpr uint8_t STATE_STARTS = 4;

// What the controller reports, one entry per kind of value. The poll plan polls by field,
// the page map (EspTm2Model::locate) places schedule fields, and each entity kind shows one.
enum class Field : uint8_t {
  ZONES_RUNNING = 0,
  CLOCK,
  RAIN_DELAY,
  RAIN_SENSOR,
  IRRIGATION_ENABLED,
  ACTIVE_ZONE,
  ACTIVE_PROGRAM,
  REMAINING_TIME,
  QUEUE,         // the queue list, 3B 01
  QUEUED_COUNT,  // how many zones are queued, 3B 00
  SEASONAL_ADJUST,
  SCHEDULE_CHANGED,
  RUN_TIME,
  WATERING_DAY,
  FREQUENCY,
  CYCLE_DAYS,
  DAYS_UNTIL_NEXT,
  START_TIME,
  START_ENABLED,
  STATION_DELAY,
  GLOBAL_SENSOR_BYPASS,
  COUNT,
};

// No field: a kind that shows nothing from the controller.
static constexpr Field NO_FIELD = Field::COUNT;

// FF with a zone running is a manual run. FF with zone 0 is idle.
enum class Activity : uint8_t { UNKNOWN = 0, IDLE, MANUAL, PROGRAM };

struct FieldSet {
  uint32_t bits{0};

  void add(Field field) { this->bits |= 1u << static_cast<uint8_t>(field); }
  bool has(Field field) const { return (this->bits & (1u << static_cast<uint8_t>(field))) != 0; }
};

// Schedule page numbers, 00 to FF.
struct PageSet {
  uint32_t bits[8]{};

  void add(uint8_t page) { this->bits[page / 32] |= 1u << (page % 32); }
  bool has(uint8_t page) const { return (this->bits[page / 32] & (1u << (page % 32))) != 0; }
};

// The value at `index`, or none when the index is out of range. Zone N is index N - 1, so zone 0
// is out of range too.
template<typename T, size_t N> std::optional<T> at(const std::optional<T> (&values)[N], size_t index) {
  return index < N ? values[index] : std::nullopt;
}

// What the controller has reported: the only copy of controller data. Each value is empty until
// a reply brings it. Arrays are by zone (zone N at N - 1), program (0 is A), then day or slot.
struct ControllerState {
  std::optional<bool> running[STATE_ZONES]{};
  std::optional<bool> available[STATE_ZONES]{};  // 03 00: the zones this controller has
  std::optional<ClockTime> time;
  std::optional<PackedDate> date;  // a real date only
  std::optional<uint16_t> rain_delay_days;
  std::optional<uint8_t> rain_sensor;
  std::optional<uint8_t> irrigation;
  std::optional<uint8_t> active_zone;  // 0 is none
  std::optional<uint8_t> program;      // 0 is A; QUEUE_PROGRAM_NONE is manual or idle
  std::optional<uint16_t> remaining_seconds;
  // Counts every reply that carried the remaining time, changed or not. A paused controller
  // repeats its value, and the local countdown re-anchors on each reading.
  uint32_t remaining_readings{0};
  std::optional<uint8_t> queued_count;
  std::optional<QueueList> queue;  // 3B 01; set once one has been read, even an empty one
  std::optional<uint16_t> seasonal[STATE_PROGRAMS]{};
  std::optional<uint32_t> schedule_changed;  // seconds since 2000 on the controller's clock

  // Schedule pages.
  std::optional<uint16_t> run_minutes[STATE_ZONES][STATE_PROGRAMS]{};
  std::optional<bool> watering_day[STATE_PROGRAMS][7]{};  // Monday is 0
  std::optional<uint8_t> frequency[STATE_PROGRAMS]{};
  std::optional<uint8_t> cycle_days[STATE_PROGRAMS]{};
  std::optional<uint8_t> days_until[STATE_PROGRAMS]{};
  // Minutes after midnight, negative when the slot is off (wire FFFF). An off slot has no time.
  std::optional<int32_t> start_minutes[STATE_PROGRAMS][STATE_STARTS]{};
  std::optional<uint16_t> station_delay;
  std::optional<bool> global_bypass;  // the rain sensor is ignored controller-wide

  Activity activity() const;
  // One schedule page field, as EspTm2Model::locate places it and read_codec decodes it.
  void set_page_field(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value);
};

}  // namespace esphome::rainbird
