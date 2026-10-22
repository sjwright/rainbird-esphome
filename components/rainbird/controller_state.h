#pragma once

#include <cstddef>
#include <cstdint>

#include "sip_commands.h"

namespace esphome::rainbird {

// Sized for the ESP-TM2. There is one C++ model, so the store matches its limits.
static constexpr uint8_t STATE_ZONES = 12;
static constexpr uint8_t STATE_PROGRAMS = 3;
static constexpr uint8_t STATE_STARTS = 4;
static constexpr uint8_t STATE_QUEUE = 11;

// What the controller reports, one entry per kind of value. The poll plan polls by field,
// the page map (Model::locate) places schedule fields, and each entity kind shows one.
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
  ZONE_FAULT,
  RUN_TIME,
  WATERING_DAY,
  FREQUENCY,
  CYCLE_DAYS,
  DAYS_UNTIL_NEXT,
  START_TIME,
  START_ENABLED,
  STATION_DELAY,
  GLOBAL_SENSOR_BYPASS,
  ZONE_SENSOR_BYPASS,
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

struct QueueItem {
  uint8_t zone{0};
  uint8_t flags{0};
  uint16_t seconds{0};
};

// The only copy of controller data. A cell is changed when a reply brings a new value,
// and stays changed until the hub clears the flags after publishing.
class ControllerState {
 public:
  void clear_changed();
  // Zone 0 on a per-zone field means any zone.
  bool changed(Field field, uint8_t zone = 0, uint8_t program = 0, uint8_t index = 0) const;
  // One schedule page field, as Model::locate places it and read_codec decodes it.
  void set_page_field(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value);

  void set_zone_running(uint8_t zone, bool on);
  bool zone_running(uint8_t zone, bool *on) const;

  void set_zone_available(uint8_t zone, bool on);
  bool zone_available(uint8_t zone, bool *on) const;

  void set_clock(uint8_t hour, uint8_t minute, uint8_t second);
  bool clock(uint8_t *hour, uint8_t *minute, uint8_t *second) const;
  void set_date(uint8_t day, uint8_t month, uint16_t year);
  // The controller reported a date that is not a real date. The date is unknown until a valid one arrives.
  void forget_date();
  bool date(uint8_t *day, uint8_t *month, uint16_t *year) const;

  void set_rain_delay(uint16_t days);
  bool rain_delay(uint16_t *days) const;
  void set_rain_sensor(uint8_t state);
  bool rain_sensor(uint8_t *state) const;
  void set_irrigation(uint8_t state);
  bool irrigation(uint8_t *state) const;

  void set_active_zone(uint8_t zone);
  bool active_zone(uint8_t *zone) const;
  void set_program_byte(uint8_t byte);
  bool program_byte(uint8_t *byte) const;
  Activity activity() const;
  void set_remaining_seconds(uint16_t seconds);
  bool remaining_seconds(uint16_t *seconds) const;
  // True when a reply carried the remaining time since clear_changed(), changed or not.
  // A paused controller repeats the same value, and the local countdown re-anchors on it.
  bool remaining_refreshed() const { return this->remaining_refreshed_; }
  void set_queued_count(uint8_t count);
  bool queued_count(uint8_t *count) const;

  void set_queue(const QueueItem *items, uint8_t count);
  uint8_t queue_count() const { return this->queue_count_; }
  // True once a queue list (3B 01) has been read, even an empty one.
  bool queue_known() const { return this->queue_known_; }
  bool queue_item(uint8_t index, QueueItem *out) const;

  void set_seasonal(uint8_t program, uint16_t percent);
  bool seasonal(uint8_t program, uint16_t *percent) const;

  void set_schedule_changed(uint32_t seconds_since_2000);
  bool schedule_changed(uint32_t *seconds_since_2000) const;

  void set_run_minutes(uint8_t zone, uint8_t program, uint16_t minutes);
  bool run_minutes(uint8_t zone, uint8_t program, uint16_t *minutes) const;
  void set_watering_day(uint8_t program, uint8_t index, bool on);
  bool watering_day(uint8_t program, uint8_t index, bool *on) const;
  void set_frequency(uint8_t program, uint8_t frequency);
  bool frequency(uint8_t program, uint8_t *frequency) const;
  void set_cycle_days(uint8_t program, uint8_t days);
  bool cycle_days(uint8_t program, uint8_t *days) const;
  void set_days_until(uint8_t program, uint8_t days);
  bool days_until(uint8_t program, uint8_t *days) const;
  // minutes is negative when the slot is off (wire FFFF). An off slot does not invent a time.
  void set_start_minutes(uint8_t program, uint8_t index, int32_t minutes);
  bool start_minutes(uint8_t program, uint8_t index, int32_t *minutes) const;
  void set_start_enabled(uint8_t program, uint8_t index, bool on);
  bool start_enabled(uint8_t program, uint8_t index, bool *on) const;

  void set_station_delay(uint16_t seconds);
  bool station_delay(uint16_t *seconds) const;
  // True when the rain sensor is ignored, controller-wide or for one zone.
  void set_global_bypass(bool bypassed);
  bool global_bypass(bool *bypassed) const;
  void set_zone_bypass(uint8_t zone, bool bypassed);
  bool zone_bypass(uint8_t zone, bool *bypassed) const;

 private:
  struct Cell {
    int32_t value{0};
    bool known{false};
    bool changed{false};
  };

  // Every value kept. A slot has one cell, or one per zone, program and day or start slot.
  enum class Slot : uint8_t {
    RUNNING,
    AVAILABLE,
    TIME,  // seconds since midnight
    DATE,  // yyyymmdd
    RAIN_DELAY,
    RAIN_SENSOR,
    IRRIGATION,
    ACTIVE_ZONE,
    PROGRAM_BYTE,
    REMAINING,
    QUEUED_COUNT,
    SEASONAL,
    SCHEDULE_CHANGED,
    RUN_MINUTES,
    WATERING_DAY,
    FREQUENCY,
    CYCLE_DAYS,
    DAYS_UNTIL,
    START_MINUTES,
    START_ENABLED,
    STATION_DELAY,
    GLOBAL_BYPASS,
    ZONE_BYPASS,
    COUNT,
  };

  // The cell for one value, or null when the zone, program or index is out of range.
  Cell *cell_(Slot slot, uint8_t zone = 0, uint8_t program = 0, uint8_t index = 0);
  const Cell *cell_(Slot slot, uint8_t zone = 0, uint8_t program = 0, uint8_t index = 0) const;
  void set_(Slot slot, uint8_t zone, uint8_t program, uint8_t index, int32_t value);
  bool get_(Slot slot, uint8_t zone, uint8_t program, uint8_t index, int32_t *value) const;
  // get_() converted to the getter's type. A bool is true for any non-zero value.
  template<typename T> bool read_(Slot slot, uint8_t zone, uint8_t program, uint8_t index, T *out) const;
  // Zone 0 on a per-zone slot means any zone.
  bool slot_changed_(Slot slot, uint8_t zone, uint8_t program, uint8_t index) const;

  // One cell per value. controller_state.cpp checks this against the slot shapes.
  static constexpr size_t CELLS = 3 * STATE_ZONES + STATE_ZONES * STATE_PROGRAMS + 4 * STATE_PROGRAMS +
                                  7 * STATE_PROGRAMS + 2 * STATE_PROGRAMS * STATE_STARTS + 12;
  Cell cells_[CELLS]{};
  bool remaining_refreshed_{false};
  QueueItem queue_[STATE_QUEUE]{};
  uint8_t queue_count_{0};
  bool queue_known_{false};
  bool queue_changed_{false};
};

}  // namespace esphome::rainbird
