#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::rainbird {

// Sized for the ESP-TM2. There is one C++ model, so the store matches its limits.
static constexpr uint8_t STATE_ZONES = 12;
static constexpr uint8_t STATE_PROGRAMS = 3;
static constexpr uint8_t STATE_STARTS = 4;
static constexpr uint8_t STATE_QUEUE = 11;

// Names match the fields the poll plan and locate() use. Entity rows point at these later.
enum class Field : uint8_t {
  ZONES_RUNNING = 0,
  CLOCK,
  RAIN_DELAY,
  RAIN_SENSOR,
  IRRIGATION_ENABLED,
  ACTIVE_ZONE,
  ACTIVE_PROGRAM,
  REMAINING_TIME,
  QUEUE,
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
  AVAILABLE_ZONES,
  COUNT,
};

// FF with a zone running is a manual run. FF with zone 0 is idle.
enum class Activity : uint8_t { UNKNOWN = 0, IDLE, MANUAL, PROGRAM };

struct FieldSet {
  uint32_t bits{0};

  void add(Field field) { this->bits |= 1u << static_cast<uint8_t>(field); }
  bool has(Field field) const { return (this->bits & (1u << static_cast<uint8_t>(field))) != 0; }
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
  bool changed(Field field, uint8_t zone = 0, uint8_t program = 0, uint8_t index = 0) const;

  void set_zone_running(uint8_t zone, bool on);
  bool zone_running(uint8_t zone, bool *on) const;

  void set_zone_available(uint8_t zone, bool on);
  bool zone_available(uint8_t zone, bool *on) const;

  void set_clock(uint8_t hour, uint8_t minute, uint8_t second);
  bool clock(uint8_t *hour, uint8_t *minute, uint8_t *second) const;
  void set_date(uint8_t day, uint8_t month, uint16_t year);
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
  void set_queued_count(uint8_t count);
  bool queued_count(uint8_t *count) const;

  void set_queue(const QueueItem *items, uint8_t count);
  uint8_t queue_count() const { return this->queue_count_; }
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
  void set_global_bypass(uint8_t wire);
  bool global_bypass(uint8_t *wire) const;
  void set_zone_bypass(uint8_t zone, uint8_t wire);
  bool zone_bypass(uint8_t zone, uint8_t *wire) const;

  void set_page(uint8_t page, const uint8_t *data, uint8_t size);
  bool copy_page(uint8_t page, uint8_t *data, size_t cap, uint8_t *size) const;
  bool page_changed(uint8_t page) const;

 private:
  struct Cell {
    int32_t value{0};
    bool known{false};
    bool changed{false};

    void set(int32_t next);
    bool get(int32_t *out) const;
  };

  struct PageImage {
    uint8_t page{0};
    uint8_t data[40]{};
    uint8_t size{0};
    bool known{false};
    bool changed{false};
  };

  static int zone_index(uint8_t zone);
  static int program_index(uint8_t program);
  Cell *cell_at(Cell *cells, int index, int count);
  const Cell *cell_at(const Cell *cells, int index, int count) const;

  Cell running_[STATE_ZONES]{};
  Cell available_[STATE_ZONES]{};
  Cell hour_{};
  Cell minute_{};
  Cell second_{};
  Cell date_packed_{};
  Cell rain_delay_{};
  Cell rain_sensor_{};
  Cell irrigation_{};
  Cell active_zone_{};
  Cell program_byte_{};
  Cell remaining_{};
  Cell queued_count_{};
  Cell seasonal_[STATE_PROGRAMS]{};
  Cell schedule_changed_{};
  Cell run_minutes_[STATE_ZONES][STATE_PROGRAMS]{};
  Cell watering_day_[STATE_PROGRAMS][7]{};
  Cell frequency_[STATE_PROGRAMS]{};
  Cell cycle_days_[STATE_PROGRAMS]{};
  Cell days_until_[STATE_PROGRAMS]{};
  Cell start_minutes_[STATE_PROGRAMS][STATE_STARTS]{};
  Cell start_enabled_[STATE_PROGRAMS][STATE_STARTS]{};
  Cell station_delay_{};
  Cell global_bypass_{};
  Cell zone_bypass_[STATE_ZONES]{};
  QueueItem queue_[STATE_QUEUE]{};
  uint8_t queue_count_{0};
  bool queue_known_{false};
  bool queue_changed_{false};
  PageImage pages_[16]{};
};

}  // namespace esphome::rainbird
