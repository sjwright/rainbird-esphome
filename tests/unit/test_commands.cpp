#include "doctest.h"

#include "hex.h"
#include "sip_commands.h"

#include <cstring>

using namespace esphome::rainbird;

static void expect_buf(const ByteBuf &buf, const char *hex) {
  auto expect = from_hex(hex);
  REQUIRE(buf.size == expect.size());
  CHECK(to_hex(buf.data, buf.size) == to_hex(expect.data(), expect.size()));
}

TEST_CASE("request builders match the spec bytes") {
  expect_buf(request_model(), "02");
  expect_buf(request_stations(0), "03 00");
  expect_buf(request_support(0x82), "04 82");
  expect_buf(request_serial(), "05");
  expect_buf(request_firmware(), "0B");
  expect_buf(request_time(), "10");
  expect_buf(request_set_time(0x16, 0x2F, 0x13), "11 16 2F 13");
  expect_buf(request_date(), "12");
  PackedDate date;
  REQUIRE(decode_packed_date(0x09, 0x17, 0xE7, &date));
  expect_buf(request_set_date(date), "13 09 17 E7");
  expect_buf(request_active_stations(0), "3F 00");
  expect_buf(request_combined_state(), "4C");
  expect_buf(request_queue(0), "3B 00");
  expect_buf(request_queue(1), "3B 01");
  expect_buf(request_irrigation_state(), "48");
  expect_buf(request_rain_sensor(), "3E");
  expect_buf(request_event_timestamp(1), "4A 01");
  expect_buf(request_rain_delay(), "36");
  expect_buf(request_run_station(1, 6), "39 00 01 06");
  expect_buf(request_queue_station(0, 1, 1), "4B 00 01 01");
  expect_buf(request_run_program(0), "38 00");
  expect_buf(request_stop(), "40");
  expect_buf(request_advance(8), "42 08");
  expect_buf(request_test_stations(2), "3A 02");
  expect_buf(request_set_controller_state(1), "49 01");
  expect_buf(request_water_budget(0), "30 00");
  expect_buf(request_set_water_budget(0, 80), "31 00 00 50");
  expect_buf(request_set_rain_delay(15), "37 00 0F");
  expect_buf(request_read_page(0x10), "20 00 10");
}

TEST_CASE("identification and clock replies") {
  ModelVersion model;
  auto bytes = from_hex("82 00 06 09 0C");
  REQUIRE(parse_model(bytes.data(), bytes.size(), &model));
  CHECK(model.model_id == 0x0006);
  CHECK(model.protocol_major == 9);
  CHECK(model.protocol_minor == 12);

  StationMask stations;
  bytes = from_hex("83 00 3F 00 00 00");
  REQUIRE(parse_stations(bytes.data(), bytes.size(), &stations));
  for (uint8_t zone = 1; zone <= 6; zone++) {
    CHECK(station_bit(stations.mask, 0, zone));
  }
  CHECK_FALSE(station_bit(stations.mask, 0, 7));
  CHECK_FALSE(station_bit(stations.mask, 0, 8));

  CommandSupport support;
  bytes = from_hex("84 82 01");
  REQUIRE(parse_support(bytes.data(), bytes.size(), &support));
  CHECK(support.command == 0x82);
  CHECK(support.supported);
  bytes = from_hex("84 82 00");
  REQUIRE(parse_support(bytes.data(), bytes.size(), &support));
  CHECK_FALSE(support.supported);

  SerialNumber serial;
  bytes = from_hex("85 00 00 00 00 00 00 89 63");
  REQUIRE(parse_serial(bytes.data(), bytes.size(), &serial));
  CHECK(serial.bytes[6] == 0x89);
  CHECK(serial.bytes[7] == 0x63);

  FirmwareVersion firmware;
  bytes = from_hex("8B 01 2F 00 00");
  REQUIRE(parse_firmware(bytes.data(), bytes.size(), &firmware));
  CHECK(firmware.major == 1);
  CHECK(firmware.minor == 47);
  CHECK(firmware.patch == 0);

  ClockTime time;
  bytes = from_hex("90 0C 0A 28");
  REQUIRE(parse_time(bytes.data(), bytes.size(), &time));
  CHECK(time.hour == 12);
  CHECK(time.minute == 10);
  CHECK(time.second == 40);

  PackedDate packed;
  bytes = from_hex("92 13 A7 EA");
  REQUIRE(parse_date(bytes.data(), bytes.size(), &packed));
  CHECK(packed.day == 19);
  CHECK(packed.month == 10);
  CHECK(packed.year == 2026);
  uint8_t day = 0;
  uint8_t my = 0;
  uint8_t yy = 0;
  encode_packed_date(packed, &day, &my, &yy);
  CHECK(day == 0x13);
  CHECK(my == 0xA7);
  CHECK(yy == 0xEA);

  // Real calendar dates only, in the years the packed form holds.
  auto real = [](uint16_t year, uint8_t month, uint8_t day) { return date_valid(PackedDate{day, month, year}); };
  CHECK(real(2024, 2, 29));
  CHECK_FALSE(real(2023, 2, 29));
  CHECK_FALSE(real(2000 + 100, 2, 29));
  CHECK(real(2000, 2, 29));
  CHECK_FALSE(real(2026, 4, 31));
  CHECK(real(2026, 12, 31));
  CHECK_FALSE(real(1999, 6, 1));
  CHECK(real(4095, 6, 1));
  CHECK_FALSE(real(4096, 6, 1));

  // Any date bytes parse; the model decides what an impossible date means.
  bytes = from_hex("92 00 07 EA");
  REQUIRE(parse_date(bytes.data(), bytes.size(), &packed));
  CHECK_FALSE(date_valid(packed));
  CombinedState impossible;
  bytes = from_hex("CC 14 0B 23 08 07 E7 00 03 00 01 FF FF 00 00 00");
  REQUIRE(parse_combined_state(bytes.data(), bytes.size(), &impossible));
  CHECK_FALSE(date_valid(impossible.date));
  CHECK(impossible.rain_delay_days == 3);
}

TEST_CASE("ack, nak and live status") {
  Ack ack;
  auto bytes = from_hex("01 39");
  REQUIRE(parse_ack(bytes.data(), bytes.size(), &ack));
  CHECK(ack.echo == 0x39);
  bytes = from_hex("01 40");
  REQUIRE(parse_ack(bytes.data(), bytes.size(), &ack));
  CHECK(ack.echo == 0x40);

  Nak nak;
  bytes = from_hex("00 02 03");
  REQUIRE(parse_nak(bytes.data(), bytes.size(), &nak));
  CHECK(nak.echo == 0x02);
  CHECK(nak.code == 0x03);
  CHECK(nak_is_unsupported(nak.code));
  CHECK_FALSE(nak_is_unsupported(0x02));

  StationMask active;
  bytes = from_hex("BF 00 01 00 00 00");
  REQUIRE(parse_active_stations(bytes.data(), bytes.size(), &active));
  CHECK(station_bit(active.mask, 0, 1));
  CHECK_FALSE(station_bit(active.mask, 0, 2));

  CombinedState combined;
  bytes = from_hex("CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00");
  REQUIRE(parse_combined_state(bytes.data(), bytes.size(), &combined));
  CHECK(combined.time.hour == 20);
  CHECK(combined.time.minute == 11);
  CHECK(combined.time.second == 35);
  CHECK(combined.date.day == 8);
  CHECK(combined.date.month == 1);
  CHECK(combined.date.year == 2023);
  CHECK(combined.rain_delay_days == 3);
  CHECK(combined.sensor == 0);
  CHECK(combined.irrigation_state == 1);
  CHECK(combined.seasonal_adjust == 0xFFFF);
  CHECK(combined.remaining_seconds == 0);
  CHECK(combined.active_zone == 0);

  QueueNow idle;
  bytes = from_hex("BB 00 00 00 00 00 00 00 00 FF 00 00");
  REQUIRE(parse_queue_now(bytes.data(), bytes.size(), &idle));
  CHECK(idle.queued == 0);
  CHECK(idle.zone == 0);
  CHECK(idle.program == QUEUE_PROGRAM_NONE);
  CHECK(idle.active == 0);

  QueueNow running;
  bytes = from_hex("BB 00 05 00 05 D0 00 00 01 00 00 01");
  REQUIRE(parse_queue_now(bytes.data(), bytes.size(), &running));
  CHECK(running.queued == 5);
  CHECK(running.remaining_seconds == 1488);
  CHECK(running.zone == 1);
  CHECK(running.program == 0);
  CHECK(running.active == 0x01);

  // Above 4 must stay as itself. Mapping it to 0 would look like program A.
  QueueNow other;
  bytes = from_hex("BB 00 01 00 00 3C 00 00 01 05 00 01");
  REQUIRE(parse_queue_now(bytes.data(), bytes.size(), &other));
  CHECK(other.program == 5);

  QueueList list;
  bytes = from_hex(
      "BB 01 21 05 D0 22 04 B0 23 01 A4 24 04 B0 25 02 58 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00");
  REQUIRE(parse_queue_list(bytes.data(), bytes.size(), &list));
  REQUIRE(list.count == 5);
  const uint8_t zones[] = {1, 2, 3, 4, 5};
  const uint16_t seconds[] = {1488, 1200, 420, 1200, 600};
  for (int i = 0; i < 5; i++) {
    CHECK(list.slots[i].zone == zones[i]);
    CHECK(list.slots[i].flags == 0x20);
    CHECK(list.slots[i].seconds == seconds[i]);
  }
  // No slot at all, or a slot cut short, is not a queue: an empty queue is a slot of zone 0.
  bytes = from_hex("BB 01");
  CHECK_FALSE(parse_queue_list(bytes.data(), bytes.size(), &list));
  bytes = from_hex("BB 01 21 05 D0 22 04");
  CHECK_FALSE(parse_queue_list(bytes.data(), bytes.size(), &list));
  bytes = from_hex("BB 01 00 00 00");
  REQUIRE(parse_queue_list(bytes.data(), bytes.size(), &list));
  CHECK(list.count == 0);

  IrrigationState irrigation;
  bytes = from_hex("C8 01");
  REQUIRE(parse_irrigation_state(bytes.data(), bytes.size(), &irrigation));
  CHECK(irrigation.state == 1);

  RainSensorState sensor;
  bytes = from_hex("BE 00");
  REQUIRE(parse_rain_sensor(bytes.data(), bytes.size(), &sensor));
  CHECK(sensor.state == 0);
  bytes = from_hex("BE 01");
  REQUIRE(parse_rain_sensor(bytes.data(), bytes.size(), &sensor));
  CHECK(sensor.state == 1);

  EventTimestamp stamp;
  bytes = from_hex("CA 01 2B 48 31 63");
  REQUIRE(parse_event_timestamp(bytes.data(), bytes.size(), &stamp));
  CHECK(stamp.event == 1);
  CHECK(stamp.seconds_since_2000 == 726151523);

  RainDelay delay;
  bytes = from_hex("B6 00 03");
  REQUIRE(parse_rain_delay(bytes.data(), bytes.size(), &delay));
  CHECK(delay.days == 3);

  WaterBudget budget;
  bytes = from_hex("B0 00 00 64");
  REQUIRE(parse_water_budget(bytes.data(), bytes.size(), &budget));
  CHECK(budget.program == 0);
  CHECK(budget.percent == 100);
}

TEST_CASE("schedule pages keep their raw bytes; the page helpers do the zone arithmetic") {
  // What the bytes mean is the model's page map (test_model.cpp). sip_commands keeps them raw.
  SchedulePage page;
  auto bytes = from_hex("A0 00 10 6A 06 01 00 64 01");
  REQUIRE(parse_schedule_page(bytes.data(), bytes.size(), &page));
  CHECK(page.page == 0x10);
  CHECK(page.size == 6);
  CHECK(page.data[0] == 0x6A);
  CHECK(page.data[4] == 0x64);
  CHECK(page.data[5] == FREQ_CYCLIC);

  CHECK(run_time_page(1) == 0x80);
  CHECK(run_time_page(2) == 0x80);
  CHECK(run_time_page(11) == 0x85);
  CHECK(run_time_page(12) == 0x85);
  CHECK(run_time_page(0) == 0);
  CHECK(run_time_offset(1, 0) == 0);
  CHECK(run_time_offset(1, 2) == 4);
  CHECK(run_time_offset(2, 0) == 6);
  CHECK(run_time_offset(2, 2) == 10);
  CHECK(program_settings_page(2) == 0x12);
  CHECK(start_time_page(1) == 0x61);

  ByteBuf write;
  REQUIRE(request_write_page(0x10, page.data, page.size, &write));
  CHECK(to_hex(write.data, write.size) == "21 00 10 6A 06 01 00 64 01");
}
