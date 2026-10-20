#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::rainbird {

// Quirk helpers live here so call sites do not repeat the wire arithmetic (PLAN 5.1).

struct ByteBuf {
  // Matches MAX_PAYLOAD in sip_frame.h, so any frame payload, including send_raw's, fits.
  static constexpr size_t CAP = 64;
  uint8_t data[CAP]{};
  uint8_t size{0};

  bool push(uint8_t byte) {
    if (this->size >= CAP) {
      return false;
    }
    this->data[this->size++] = byte;
    return true;
  }

  bool push_be16(uint16_t value) {
    return this->push(static_cast<uint8_t>(value >> 8)) && this->push(static_cast<uint8_t>(value));
  }
};

uint16_t be16(uint8_t hi, uint8_t lo);

// Big-endian seconds. The combined-state docs say minutes; captures of 0x0167 after a
// 6-minute start are 359 seconds. This returns the raw big-endian value as seconds.
uint16_t queue_seconds(uint8_t hi, uint8_t lo);

// Page 0 program byte. 0xFF is manual or idle, not program 0.
// pyrainbird maps every value above 4 to 0, which erases that case.
static constexpr uint8_t QUEUE_PROGRAM_NONE = 0xFF;

struct QueueProgram {
  bool none{false};
  uint8_t index{0};
};

QueueProgram queue_program(uint8_t byte);

// NAK code bits from the pyrainbird docs. SPEC question 15 is still open: the TM2
// may not use these meanings. Bit 0 is the one the link treats as "unsupported".
bool nak_is_unsupported(uint8_t code);
bool nak_is_bad_length(uint8_t code);
bool nak_is_incompatible(uint8_t code);
bool nak_is_checksum(uint8_t code);

// Run-time pages 0x80–0x85 hold two 1-based zones each. Zone 1 is page 0x80.
// Zone 0 has no page; the functions return 0.
uint8_t run_time_page(uint8_t zone);
// Byte offset of that zone's big-endian minutes for program 0=A, 1=B, 2=C.
uint8_t run_time_offset(uint8_t zone, uint8_t program);
uint8_t program_settings_page(uint8_t program);
uint8_t start_time_page(uint8_t program);

// Bit 0 of the days mask is Sunday.
static constexpr uint8_t DAY_SUNDAY = 0x01;
static constexpr uint8_t DAY_MONDAY = 0x02;
static constexpr uint8_t DAY_TUESDAY = 0x04;
static constexpr uint8_t DAY_WEDNESDAY = 0x08;
static constexpr uint8_t DAY_THURSDAY = 0x10;
static constexpr uint8_t DAY_FRIDAY = 0x20;
static constexpr uint8_t DAY_SATURDAY = 0x40;

static constexpr uint8_t FREQ_CUSTOM = 0;
static constexpr uint8_t FREQ_CYCLIC = 1;
static constexpr uint8_t FREQ_ODD = 2;
static constexpr uint8_t FREQ_EVEN = 3;

static constexpr uint16_t START_TIME_OFF = 0xFFFF;

// Page 00 byte 3: 0 obeys the sensor, 1 bypasses it.
// Page 20 is the opposite: 1 obeys, 0 bypasses. No page 20 capture exists yet.
static constexpr uint8_t GLOBAL_SENSOR_OBEYED = 0x00;
static constexpr uint8_t GLOBAL_SENSOR_BYPASSED = 0x01;
bool station_obeys_rain_sensor(uint8_t page20_byte);

// Absolute zone, page 0 covering zones 1–32. Bit 0 of each byte is the lowest zone.
bool station_bit(const uint8_t mask[4], uint8_t page, uint8_t zone);

struct PackedDate {
  uint8_t day{0};
  uint8_t month{0};
  uint16_t year{0};
};

bool decode_packed_date(uint8_t day, uint8_t my, uint8_t yy, PackedDate *out);
void encode_packed_date(const PackedDate &date, uint8_t *day, uint8_t *my, uint8_t *yy);

// --- requests ---

ByteBuf request_model();
ByteBuf request_stations(uint8_t page);
ByteBuf request_support(uint8_t command);
ByteBuf request_serial();
ByteBuf request_firmware();
ByteBuf request_time();
ByteBuf request_set_time(uint8_t hour, uint8_t minute, uint8_t second);
ByteBuf request_date();
ByteBuf request_set_date(const PackedDate &date);
ByteBuf request_active_stations(uint8_t page);
ByteBuf request_combined_state();
ByteBuf request_queue(uint8_t page);
ByteBuf request_irrigation_state();
ByteBuf request_rain_sensor();
ByteBuf request_event_timestamp(uint8_t event);
ByteBuf request_station_error(uint8_t page);
ByteBuf request_rain_delay();
ByteBuf request_run_station(uint16_t zone, uint8_t minutes);
ByteBuf request_queue_station(uint8_t page, uint8_t zone, uint8_t minutes);
ByteBuf request_run_program(uint8_t program);
ByteBuf request_stop();
ByteBuf request_advance(uint8_t zone);
ByteBuf request_test_stations(uint8_t minutes);
ByteBuf request_set_controller_state(uint8_t state);
ByteBuf request_water_budget(uint8_t program);
ByteBuf request_set_water_budget(uint8_t program, uint16_t percent);
ByteBuf request_set_rain_delay(uint16_t days);
ByteBuf request_read_page(uint8_t page);
// False when the data does not fit. The page byte is the low byte of the u16 page number.
bool request_write_page(uint8_t page, const uint8_t *data, size_t size, ByteBuf *out);

// --- replies ---

struct ModelVersion {
  uint16_t model_id{0};
  uint8_t protocol_major{0};
  uint8_t protocol_minor{0};
};

struct StationMask {
  uint8_t page{0};
  uint8_t mask[4]{};
};

struct CommandSupport {
  uint8_t command{0};
  bool supported{false};
};

struct SerialNumber {
  uint8_t bytes[8]{};
};

struct FirmwareVersion {
  uint8_t major{0};
  uint8_t minor{0};
  uint16_t patch{0};
};

struct ClockTime {
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t second{0};
};

struct Ack {
  uint8_t echo{0};
};

struct Nak {
  uint8_t echo{0};
  uint8_t code{0};
};

// Bytes are kept raw. Captures show irrigation state 1 while idle, and the rain-sensor
// docs disagree about which value means rain. The model decides later.
struct CombinedState {
  ClockTime time{};
  PackedDate date{};
  uint16_t rain_delay_days{0};
  uint8_t sensor{0};
  uint8_t irrigation_state{0};
  uint16_t seasonal_adjust{0};
  uint16_t remaining_seconds{0};
  uint8_t active_zone{0};
};

struct QueueNow {
  uint8_t queued{0};
  uint16_t remaining_seconds{0};
  uint8_t zone{0};
  QueueProgram program{};
  uint8_t active{0};  // 0x01 running, 0x00 idle
};

struct QueueSlot {
  uint8_t zone{0};   // low 5 bits
  uint8_t flags{0};  // high 3 bits, 0x20 in the program A capture
  uint16_t seconds{0};
};

struct QueueList {
  QueueSlot slots[11]{};
  uint8_t count{0};
};

struct EventTimestamp {
  uint8_t event{0};
  uint32_t seconds_since_2000{0};
};

struct RainDelay {
  uint16_t days{0};
};

struct WaterBudget {
  uint8_t program{0};
  uint16_t percent{0};
};

struct IrrigationState {
  uint8_t state{0};
};

struct RainSensorState {
  uint8_t state{0};
};

struct SchedulePage {
  uint16_t page{0};
  uint8_t data[40]{};
  uint8_t size{0};
};

struct GlobalSettings {
  uint16_t station_delay_seconds{0};
  uint8_t rain_delay_days{0};
  uint8_t rain_sensor{0};
};

struct ProgramSettings {
  uint8_t program{0};
  uint8_t days{0};
  uint8_t period{0};
  uint8_t synchro{0};
  uint8_t permanent_days_off{0};
  uint8_t unknown{0};  // byte 4, written back unchanged
  uint8_t frequency{0};
};

struct StartTimes {
  uint8_t program{0};
  uint16_t minutes[4]{};
};

enum class ProgramField : uint8_t {
  DAYS = 0,
  PERIOD = 1,
  SYNCHRO = 2,
  PERMANENT_OFF = 3,
  FREQUENCY = 5,
};

bool parse_model(const uint8_t *payload, size_t size, ModelVersion *out);
bool parse_stations(const uint8_t *payload, size_t size, StationMask *out);
bool parse_support(const uint8_t *payload, size_t size, CommandSupport *out);
bool parse_serial(const uint8_t *payload, size_t size, SerialNumber *out);
bool parse_firmware(const uint8_t *payload, size_t size, FirmwareVersion *out);
bool parse_time(const uint8_t *payload, size_t size, ClockTime *out);
bool parse_date(const uint8_t *payload, size_t size, PackedDate *out);
bool parse_ack(const uint8_t *payload, size_t size, Ack *out);
bool parse_nak(const uint8_t *payload, size_t size, Nak *out);
bool parse_active_stations(const uint8_t *payload, size_t size, StationMask *out);
bool parse_combined_state(const uint8_t *payload, size_t size, CombinedState *out);
bool parse_queue_now(const uint8_t *payload, size_t size, QueueNow *out);
bool parse_queue_list(const uint8_t *payload, size_t size, QueueList *out);
bool parse_irrigation_state(const uint8_t *payload, size_t size, IrrigationState *out);
bool parse_rain_sensor(const uint8_t *payload, size_t size, RainSensorState *out);
bool parse_event_timestamp(const uint8_t *payload, size_t size, EventTimestamp *out);
bool parse_rain_delay(const uint8_t *payload, size_t size, RainDelay *out);
bool parse_water_budget(const uint8_t *payload, size_t size, WaterBudget *out);
bool parse_schedule_page(const uint8_t *payload, size_t size, SchedulePage *out);
bool parse_global_page(const SchedulePage &page, GlobalSettings *out);
bool parse_program_page(const SchedulePage &page, ProgramSettings *out);
bool parse_start_times(const SchedulePage &page, StartTimes *out);
bool run_time_minutes(const SchedulePage &page, uint8_t zone, uint8_t program, uint16_t *minutes);

// Copies bytes 3 and 4 as they stand. There is no field for byte 4: the ME/TM2 doc
// hard-codes 0x64 there, and that would throw away the controller's value.
bool patch_program_field(uint8_t *data, size_t size, ProgramField field, uint8_t value);

// One parsed reply. Only the member named by kind is set. The model maps this onto
// controller state and does not look at the raw opcode again.
enum class ReplyKind : uint8_t {
  NONE = 0,
  ACK,
  NAK,
  MODEL,
  STATIONS,
  SUPPORT,
  SERIAL_NUMBER,  // not SERIAL: Arduino.h defines SERIAL as 0x0
  FIRMWARE,
  TIME,
  DATE,
  ACTIVE,
  COMBINED,
  QUEUE_NOW,
  QUEUE_LIST,
  IRRIGATION,
  RAIN_SENSOR,
  EVENT,
  RAIN_DELAY,
  BUDGET,
  SCHEDULE,
  RAW,
};

struct RawReply {
  uint8_t opcode{0};
  uint8_t data[64]{};
  uint8_t size{0};
};

struct Reply {
  ReplyKind kind{ReplyKind::NONE};
  Ack ack{};
  Nak nak{};
  ModelVersion model{};
  StationMask stations{};
  CommandSupport support{};
  SerialNumber serial{};
  FirmwareVersion firmware{};
  ClockTime time{};
  PackedDate date{};
  CombinedState combined{};
  QueueNow queue_now{};
  QueueList queue_list{};
  IrrigationState irrigation{};
  RainSensorState rain_sensor{};
  EventTimestamp event{};
  RainDelay rain_delay{};
  WaterBudget budget{};
  SchedulePage schedule{};
  RawReply raw{};
};

// False when a known opcode is the wrong length. An unknown opcode is RAW.
bool parse_reply(const uint8_t *payload, size_t size, Reply *out);

// True when a reply answers this request: the request opcode + 0x80, or an ACK or
// NAK echoing it. Paged requests also check the selector the reply echoes (3B/BB
// page, 3F/BF page, 03/83 page, 30/B0 program, 4A/CA event, 04/84 command,
// 20/A0 page), so a late reply to an earlier request is not taken for this one.
bool reply_matches(const uint8_t *request, size_t request_size, const uint8_t *reply, size_t reply_size);

}  // namespace esphome::rainbird
