#include "sip_commands.h"

namespace esphome::rainbird {

uint16_t be16(uint8_t hi, uint8_t lo) { return (static_cast<uint16_t>(hi) << 8) | lo; }

uint16_t queue_seconds(uint8_t hi, uint8_t lo) { return be16(hi, lo); }

QueueProgram queue_program(uint8_t byte) {
  QueueProgram out;
  out.none = byte == QUEUE_PROGRAM_NONE;
  out.index = byte;
  return out;
}

bool nak_is_unsupported(uint8_t code) { return (code & 0x01) != 0; }

uint8_t run_time_page(uint8_t zone) {
  if (zone == 0) {
    return 0;
  }
  return static_cast<uint8_t>(0x80 + (zone - 1) / 2);
}

uint8_t run_time_offset(uint8_t zone, uint8_t program) {
  if (zone == 0) {
    return 0;
  }
  uint8_t station = (zone - 1) % 2;
  return static_cast<uint8_t>(station * 6 + program * 2);
}

uint8_t program_settings_page(uint8_t program) { return static_cast<uint8_t>(0x10 + program); }
uint8_t start_time_page(uint8_t program) { return static_cast<uint8_t>(0x60 + program); }

bool station_bit(const uint8_t mask[4], uint8_t page, uint8_t zone) {
  if (mask == nullptr || zone == 0) {
    return false;
  }
  uint16_t first = static_cast<uint16_t>(page) * 32 + 1;
  if (zone < first || zone >= first + 32) {
    return false;
  }
  uint8_t index = static_cast<uint8_t>(zone - first);
  return (mask[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

bool decode_packed_date(uint8_t day, uint8_t my, uint8_t yy, PackedDate *out) {
  if (out == nullptr) {
    return false;
  }
  out->day = day;
  out->month = my >> 4;
  out->year = (static_cast<uint16_t>(my & 0x0F) << 8) | yy;
  return date_valid(*out);
}

bool date_valid(const PackedDate &date) {
  static constexpr uint8_t DAYS_IN_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (date.year < 2000 || date.year > 0x0FFF || date.month < 1 || date.month > 12 || date.day < 1) {
    return false;
  }
  bool leap = (date.year % 4 == 0 && date.year % 100 != 0) || date.year % 400 == 0;
  uint8_t days = DAYS_IN_MONTH[date.month - 1] + (date.month == 2 && leap ? 1 : 0);
  return date.day <= days;
}

void encode_packed_date(const PackedDate &date, uint8_t *day, uint8_t *my, uint8_t *yy) {
  *day = date.day;
  *my = static_cast<uint8_t>((date.month << 4) | ((date.year >> 8) & 0x0F));
  *yy = static_cast<uint8_t>(date.year);
}

namespace {

bool exact(const uint8_t *payload, size_t size, size_t expect, uint8_t opcode) {
  return payload != nullptr && size == expect && payload[0] == opcode;
}

ByteBuf op(uint8_t opcode) {
  ByteBuf buf;
  buf.push(opcode);
  return buf;
}

ByteBuf op_byte(uint8_t opcode, uint8_t arg) {
  ByteBuf buf;
  buf.push(opcode);
  buf.push(arg);
  return buf;
}

}  // namespace

ByteBuf request_model() { return op(0x02); }
ByteBuf request_stations(uint8_t page) { return op_byte(0x03, page); }
ByteBuf request_support(uint8_t command) { return op_byte(0x04, command); }
ByteBuf request_serial() { return op(0x05); }
ByteBuf request_firmware() { return op(0x0B); }
ByteBuf request_time() { return op(0x10); }

ByteBuf request_set_time(uint8_t hour, uint8_t minute, uint8_t second) {
  ByteBuf buf;
  buf.push(0x11);
  buf.push(hour);
  buf.push(minute);
  buf.push(second);
  return buf;
}

ByteBuf request_date() { return op(0x12); }

ByteBuf request_set_date(const PackedDate &date) {
  ByteBuf buf;
  uint8_t day = 0;
  uint8_t my = 0;
  uint8_t yy = 0;
  encode_packed_date(date, &day, &my, &yy);
  buf.push(0x13);
  buf.push(day);
  buf.push(my);
  buf.push(yy);
  return buf;
}

ByteBuf request_active_stations(uint8_t page) { return op_byte(0x3F, page); }
ByteBuf request_combined_state() { return op(0x4C); }
ByteBuf request_queue(uint8_t page) { return op_byte(0x3B, page); }
ByteBuf request_irrigation_state() { return op(0x48); }
ByteBuf request_rain_sensor() { return op(0x3E); }
ByteBuf request_event_timestamp(uint8_t event) { return op_byte(0x4A, event); }
ByteBuf request_station_error(uint8_t page) { return op_byte(0x3D, page); }
ByteBuf request_rain_delay() { return op(0x36); }

ByteBuf request_run_station(uint16_t zone, uint8_t minutes) {
  ByteBuf buf;
  buf.push(0x39);
  buf.push_be16(zone);
  buf.push(minutes);
  return buf;
}

ByteBuf request_queue_station(uint8_t page, uint8_t zone, uint8_t minutes) {
  ByteBuf buf;
  buf.push(0x4B);
  buf.push(page);
  buf.push(zone);
  buf.push(minutes);
  return buf;
}

ByteBuf request_run_program(uint8_t program) { return op_byte(0x38, program); }
ByteBuf request_stop() { return op(0x40); }
ByteBuf request_advance(uint8_t zone) { return op_byte(0x42, zone); }
ByteBuf request_test_stations(uint8_t minutes) { return op_byte(0x3A, minutes); }
ByteBuf request_set_controller_state(uint8_t state) { return op_byte(0x49, state); }
ByteBuf request_water_budget(uint8_t program) { return op_byte(0x30, program); }

ByteBuf request_set_water_budget(uint8_t program, uint16_t percent) {
  ByteBuf buf;
  buf.push(0x31);
  buf.push(program);
  buf.push_be16(percent);
  return buf;
}

ByteBuf request_set_rain_delay(uint16_t days) {
  ByteBuf buf;
  buf.push(0x37);
  buf.push_be16(days);
  return buf;
}

ByteBuf request_read_page(uint8_t page) {
  ByteBuf buf;
  buf.push(0x20);
  buf.push(0x00);
  buf.push(page);
  return buf;
}

bool request_write_page(uint8_t page, const uint8_t *data, size_t size, ByteBuf *out) {
  if (out == nullptr || (size > 0 && data == nullptr) || size + 3 > ByteBuf::CAP) {
    return false;
  }
  *out = ByteBuf{};
  out->push(0x21);
  out->push(0x00);
  out->push(page);
  for (size_t i = 0; i < size; i++) {
    out->push(data[i]);
  }
  return true;
}

bool parse_model(const uint8_t *payload, size_t size, ModelVersion *out) {
  if (out == nullptr || !exact(payload, size, 5, 0x82)) {
    return false;
  }
  out->model_id = be16(payload[1], payload[2]);
  out->protocol_major = payload[3];
  out->protocol_minor = payload[4];
  return true;
}

bool parse_stations(const uint8_t *payload, size_t size, StationMask *out) {
  if (out == nullptr || !exact(payload, size, 6, 0x83)) {
    return false;
  }
  out->page = payload[1];
  for (int i = 0; i < 4; i++) {
    out->mask[i] = payload[2 + i];
  }
  return true;
}

bool parse_support(const uint8_t *payload, size_t size, CommandSupport *out) {
  if (out == nullptr || !exact(payload, size, 3, 0x84)) {
    return false;
  }
  out->command = payload[1];
  out->supported = payload[2] == 0x01;
  return true;
}

bool parse_serial(const uint8_t *payload, size_t size, SerialNumber *out) {
  if (out == nullptr || !exact(payload, size, 9, 0x85)) {
    return false;
  }
  for (int i = 0; i < 8; i++) {
    out->bytes[i] = payload[1 + i];
  }
  return true;
}

bool parse_firmware(const uint8_t *payload, size_t size, FirmwareVersion *out) {
  if (out == nullptr || !exact(payload, size, 5, 0x8B)) {
    return false;
  }
  out->major = payload[1];
  out->minor = payload[2];
  out->patch = be16(payload[3], payload[4]);
  return true;
}

bool parse_time(const uint8_t *payload, size_t size, ClockTime *out) {
  if (out == nullptr || !exact(payload, size, 4, 0x90)) {
    return false;
  }
  out->hour = payload[1];
  out->minute = payload[2];
  out->second = payload[3];
  return true;
}

bool parse_date(const uint8_t *payload, size_t size, PackedDate *out) {
  if (out == nullptr || !exact(payload, size, 4, 0x92)) {
    return false;
  }
  decode_packed_date(payload[1], payload[2], payload[3], out);
  return true;
}

bool parse_ack(const uint8_t *payload, size_t size, Ack *out) {
  if (out == nullptr || !exact(payload, size, 2, 0x01)) {
    return false;
  }
  out->echo = payload[1];
  return true;
}

bool parse_nak(const uint8_t *payload, size_t size, Nak *out) {
  if (out == nullptr || !exact(payload, size, 3, 0x00)) {
    return false;
  }
  out->echo = payload[1];
  out->code = payload[2];
  return true;
}

bool parse_active_stations(const uint8_t *payload, size_t size, StationMask *out) {
  if (out == nullptr || !exact(payload, size, 6, 0xBF)) {
    return false;
  }
  out->page = payload[1];
  for (int i = 0; i < 4; i++) {
    out->mask[i] = payload[2 + i];
  }
  return true;
}

bool parse_combined_state(const uint8_t *payload, size_t size, CombinedState *out) {
  if (out == nullptr || !exact(payload, size, 16, 0xCC)) {
    return false;
  }
  out->time.hour = payload[1];
  out->time.minute = payload[2];
  out->time.second = payload[3];
  decode_packed_date(payload[4], payload[5], payload[6], &out->date);
  out->rain_delay_days = be16(payload[7], payload[8]);
  out->sensor = payload[9];
  out->irrigation_state = payload[10];
  out->seasonal_adjust = be16(payload[11], payload[12]);
  out->remaining_seconds = queue_seconds(payload[13], payload[14]);
  out->active_zone = payload[15];
  return true;
}

bool parse_queue_now(const uint8_t *payload, size_t size, QueueNow *out) {
  if (out == nullptr || !exact(payload, size, 12, 0xBB) || payload[1] != 0x00) {
    return false;
  }
  out->queued = payload[2];
  out->remaining_seconds = queue_seconds(payload[4], payload[5]);
  out->zone = payload[8];
  out->program = queue_program(payload[9]);
  out->active = payload[11];
  return true;
}

bool parse_queue_list(const uint8_t *payload, size_t size, QueueList *out) {
  // Whole 3-byte slots, at least one. A slot with zone 0 ends the list. The TM2 sends all 11
  // (SPEC 5.3), but a shorter list of whole slots reads the same way.
  if (out == nullptr || payload == nullptr || size < 5 || (size - 2) % 3 != 0 || payload[0] != 0xBB ||
      payload[1] != 0x01) {
    return false;
  }
  *out = QueueList{};
  size_t index = 2;
  while (index + 3 <= size && out->count < 11) {
    uint8_t raw = payload[index];
    if ((raw & 0x1F) == 0) {
      break;
    }
    QueueSlot &slot = out->slots[out->count];
    slot.zone = raw & 0x1F;
    slot.flags = raw & 0xE0;
    slot.seconds = queue_seconds(payload[index + 1], payload[index + 2]);
    out->count++;
    index += 3;
  }
  return true;
}

bool parse_irrigation_state(const uint8_t *payload, size_t size, IrrigationState *out) {
  if (out == nullptr || !exact(payload, size, 2, 0xC8)) {
    return false;
  }
  out->state = payload[1];
  return true;
}

bool parse_rain_sensor(const uint8_t *payload, size_t size, RainSensorState *out) {
  if (out == nullptr || !exact(payload, size, 2, 0xBE)) {
    return false;
  }
  out->state = payload[1];
  return true;
}

bool parse_event_timestamp(const uint8_t *payload, size_t size, EventTimestamp *out) {
  if (out == nullptr || !exact(payload, size, 6, 0xCA)) {
    return false;
  }
  out->event = payload[1];
  out->seconds_since_2000 = (static_cast<uint32_t>(payload[2]) << 24) | (static_cast<uint32_t>(payload[3]) << 16) |
                            (static_cast<uint32_t>(payload[4]) << 8) | payload[5];
  return true;
}

bool parse_rain_delay(const uint8_t *payload, size_t size, RainDelay *out) {
  if (out == nullptr || !exact(payload, size, 3, 0xB6)) {
    return false;
  }
  out->days = be16(payload[1], payload[2]);
  return true;
}

bool parse_water_budget(const uint8_t *payload, size_t size, WaterBudget *out) {
  if (out == nullptr || !exact(payload, size, 4, 0xB0)) {
    return false;
  }
  out->program = payload[1];
  out->percent = be16(payload[2], payload[3]);
  return true;
}

bool parse_schedule_page(const uint8_t *payload, size_t size, SchedulePage *out) {
  if (out == nullptr || payload == nullptr || size < 3 || payload[0] != 0xA0) {
    return false;
  }
  if (size - 3 > sizeof(out->data)) {
    return false;
  }
  *out = SchedulePage{};
  out->page = be16(payload[1], payload[2]);
  out->size = static_cast<uint8_t>(size - 3);
  for (size_t i = 0; i < out->size; i++) {
    out->data[i] = payload[3 + i];
  }
  return true;
}

bool parse_reply(const uint8_t *payload, size_t size, Reply *out) {
  if (out == nullptr || payload == nullptr || size == 0) {
    return false;
  }
  *out = Reply{};
  switch (payload[0]) {
    case 0x01:
      if (!parse_ack(payload, size, &out->ack)) {
        return false;
      }
      out->kind = ReplyKind::ACK;
      return true;
    case 0x00:
      if (!parse_nak(payload, size, &out->nak)) {
        return false;
      }
      out->kind = ReplyKind::NAK;
      return true;
    case 0x82:
      if (!parse_model(payload, size, &out->model)) {
        return false;
      }
      out->kind = ReplyKind::MODEL;
      return true;
    case 0x83:
      if (!parse_stations(payload, size, &out->stations)) {
        return false;
      }
      out->kind = ReplyKind::STATIONS;
      return true;
    case 0x84:
      if (!parse_support(payload, size, &out->support)) {
        return false;
      }
      out->kind = ReplyKind::SUPPORT;
      return true;
    case 0x85:
      if (!parse_serial(payload, size, &out->serial)) {
        return false;
      }
      out->kind = ReplyKind::SERIAL_NUMBER;
      return true;
    case 0x8B:
      if (!parse_firmware(payload, size, &out->firmware)) {
        return false;
      }
      out->kind = ReplyKind::FIRMWARE;
      return true;
    case 0x90:
      if (!parse_time(payload, size, &out->time)) {
        return false;
      }
      out->kind = ReplyKind::TIME;
      return true;
    case 0x92:
      if (!parse_date(payload, size, &out->date)) {
        return false;
      }
      out->kind = ReplyKind::DATE;
      return true;
    case 0xBF:
      if (!parse_active_stations(payload, size, &out->stations)) {
        return false;
      }
      out->kind = ReplyKind::ACTIVE;
      return true;
    case 0xCC:
      if (!parse_combined_state(payload, size, &out->combined)) {
        return false;
      }
      out->kind = ReplyKind::COMBINED;
      return true;
    case 0xBB:
      if (size >= 2 && payload[1] == 0x00 && parse_queue_now(payload, size, &out->queue_now)) {
        out->kind = ReplyKind::QUEUE_NOW;
        return true;
      }
      if (size >= 2 && payload[1] == 0x01 && parse_queue_list(payload, size, &out->queue_list)) {
        out->kind = ReplyKind::QUEUE_LIST;
        return true;
      }
      return false;
    case 0xC8:
      if (!parse_irrigation_state(payload, size, &out->irrigation)) {
        return false;
      }
      out->kind = ReplyKind::IRRIGATION;
      return true;
    case 0xBE:
      if (!parse_rain_sensor(payload, size, &out->rain_sensor)) {
        return false;
      }
      out->kind = ReplyKind::RAIN_SENSOR;
      return true;
    case 0xCA:
      if (!parse_event_timestamp(payload, size, &out->event)) {
        return false;
      }
      out->kind = ReplyKind::EVENT;
      return true;
    case 0xB6:
      if (!parse_rain_delay(payload, size, &out->rain_delay)) {
        return false;
      }
      out->kind = ReplyKind::RAIN_DELAY;
      return true;
    case 0xB0:
      if (!parse_water_budget(payload, size, &out->budget)) {
        return false;
      }
      out->kind = ReplyKind::BUDGET;
      return true;
    case 0xA0:
      if (!parse_schedule_page(payload, size, &out->schedule)) {
        return false;
      }
      out->kind = ReplyKind::SCHEDULE;
      return true;
    default:
      break;
  }
  if (size > sizeof(out->raw.data)) {
    return false;
  }
  out->kind = ReplyKind::RAW;
  out->raw.opcode = payload[0];
  out->raw.size = static_cast<uint8_t>(size);
  for (size_t i = 0; i < size; i++) {
    out->raw.data[i] = payload[i];
  }
  return true;
}

bool reply_matches(const uint8_t *request, size_t request_size, const uint8_t *reply, size_t reply_size) {
  if (request == nullptr || reply == nullptr || request_size == 0 || reply_size == 0) {
    return false;
  }
  const uint8_t opcode = request[0];
  if (reply[0] == 0x01) {
    return reply_size >= 2 && reply[1] == opcode;
  }
  if (reply[0] == 0x00) {
    return reply_size >= 3 && reply[1] == opcode;
  }
  if (reply[0] != static_cast<uint8_t>(opcode + 0x80)) {
    return false;
  }
  switch (opcode) {
    case 0x03:  // 83 PP
    case 0x30:  // B0 PP
    case 0x3B:  // BB PP
    case 0x3F:  // BF PP
    case 0x4A:  // CA EE
    case 0x04:  // 84 CC
      return request_size >= 2 && reply_size >= 2 && reply[1] == request[1];
    case 0x20:  // 20 00 PP -> A0 00 PP
      return request_size >= 3 && reply_size >= 3 && reply[1] == request[1] && reply[2] == request[2];
    default:
      return true;
  }
}

}  // namespace esphome::rainbird
