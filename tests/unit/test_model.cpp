#include "doctest.h"

#include "hex.h"
#include "model_esp_tm2.h"

#include <cstring>
#include <initializer_list>
#include <string>

using namespace esphome::rainbird;

static bool load(const char *hex, Reply *out) {
  auto bytes = from_hex(hex);
  return parse_reply(bytes.data(), bytes.size(), out);
}

// REQUIRE returns from the test case, so the parse check stays here rather than in load().
#define APPLY(model, hex, state)           \
  do {                                     \
    Reply loaded_;                         \
    REQUIRE(load((hex), &loaded_));        \
    (model).apply_reply(loaded_, (state)); \
  } while (0)

static void expect_requests(const RequestList &list, std::initializer_list<const char *> hexes) {
  REQUIRE(list.count == hexes.size());
  size_t index = 0;
  for (const char *hex : hexes) {
    auto expect = from_hex(hex);
    INFO(hex);
    CHECK(list.items[index].size == expect.size());
    CHECK(to_hex(list.items[index].data, list.items[index].size) == to_hex(expect.data(), expect.size()));
    index++;
  }
}

TEST_CASE("ESP-TM2 accepts its model ids only") {
  EspTm2Model model;
  CHECK(model.limits().zones == 12);
  CHECK(model.limits().programs == 3);
  CHECK(model.limits().starts == 4);
  CHECK(std::string(model.limits().name) == "ESP-TM2");
  CHECK(model.accepts(0x0005));
  CHECK(model.accepts(0x000A));
  CHECK(model.accepts(0x010A));
  CHECK(model.accepts(0x0014));
  CHECK(model.accepts(0x0015));
  CHECK_FALSE(model.accepts(0x0006));

  OptionList options = model.frequency_options();
  REQUIRE(options.count == 4);
  CHECK(std::string(options.names[0]) == "Custom days");
  CHECK(std::string(options.names[1]) == "Cyclic");
  CHECK(std::string(options.names[3]) == "Even days");
}

TEST_CASE("combined state keeps seconds and ignores FFFF seasonal adjust") {
  EspTm2Model model;
  ControllerState state;
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00", &state);
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  REQUIRE(state.clock(&hour, &minute, &second));
  CHECK(hour == 20);
  CHECK(minute == 11);
  CHECK(second == 35);
  uint8_t day = 0;
  uint8_t month = 0;
  uint16_t year = 0;
  REQUIRE(state.date(&day, &month, &year));
  CHECK(day == 8);
  CHECK(month == 1);
  CHECK(year == 2023);
  uint16_t rain = 0;
  REQUIRE(state.rain_delay(&rain));
  CHECK(rain == 3);
  uint8_t sensor = 0xFF;
  REQUIRE(state.rain_sensor(&sensor));
  CHECK(sensor == 0);
  uint8_t irrigation = 0;
  REQUIRE(state.irrigation(&irrigation));
  CHECK(irrigation == 1);
  uint16_t percent = 0;
  CHECK_FALSE(state.seasonal(0, &percent));
  uint16_t remaining = 1;
  REQUIRE(state.remaining_seconds(&remaining));
  CHECK(remaining == 0);
  CHECK(state.changed(Field::CLOCK));
  CHECK(state.changed(Field::REMAINING_TIME));

  state.clear_changed();
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00", &state);
  CHECK_FALSE(state.changed(Field::CLOCK));
  CHECK_FALSE(state.changed(Field::REMAINING_TIME));

  ControllerState seconds;
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 01 67 00", &seconds);
  REQUIRE(seconds.remaining_seconds(&remaining));
  CHECK(remaining == 359);
  CHECK(remaining != 0x6701);
}

TEST_CASE("queue FF is idle or manual, and seconds are big-endian") {
  EspTm2Model model;
  ControllerState idle;
  APPLY(model, "BB 00 00 00 00 00 00 00 00 FF 00 00", &idle);
  CHECK(idle.activity() == Activity::IDLE);
  uint8_t program = 0;
  REQUIRE(idle.program_byte(&program));
  CHECK(program == 0xFF);
  uint8_t zone = 1;
  REQUIRE(idle.active_zone(&zone));
  CHECK(zone == 0);

  ControllerState running;
  APPLY(model, "BB 00 05 00 05 D0 00 00 01 00 00 01", &running);
  CHECK(running.activity() == Activity::PROGRAM);
  REQUIRE(running.program_byte(&program));
  CHECK(program == 0);
  uint16_t remaining = 0;
  REQUIRE(running.remaining_seconds(&remaining));
  CHECK(remaining == 1488);
  CHECK(remaining != 0xD005);
  uint8_t queued = 0;
  REQUIRE(running.queued_count(&queued));
  CHECK(queued == 5);

  ControllerState manual;
  APPLY(model, "BB 00 01 00 00 3C 00 00 01 FF 00 01", &manual);
  CHECK(manual.activity() == Activity::MANUAL);
  REQUIRE(manual.remaining_seconds(&remaining));
  CHECK(remaining == 60);
  REQUIRE(manual.active_zone(&zone));
  CHECK(zone == 1);

  ControllerState list;
  APPLY(model, "BB 01 21 05 D0 22 04 B0 23 01 A4 24 04 B0 25 02 58 00", &list);
  CHECK(list.queue_count() == 5);
  QueueItem item;
  REQUIRE(list.queue_item(0, &item));
  CHECK(item.zone == 1);
  CHECK(item.flags == 0x20);
  CHECK(item.seconds == 1488);
  REQUIRE(list.queue_item(2, &item));
  CHECK(item.zone == 3);
  CHECK(item.seconds == 420);
  REQUIRE(list.queue_item(4, &item));
  CHECK(item.zone == 5);
  CHECK(item.seconds == 600);
  CHECK(list.changed(Field::QUEUE));
}

TEST_CASE("schedule pages fill run times, days and start slots") {
  EspTm2Model model;
  ControllerState state;
  APPLY(model, "A0 00 00 00 00 04 00", &state);
  uint16_t delay = 1;
  REQUIRE(state.station_delay(&delay));
  CHECK(delay == 0);
  uint16_t rain = 0;
  REQUIRE(state.rain_delay(&rain));
  CHECK(rain == 4);
  uint8_t bypass = 0xFF;
  REQUIRE(state.global_bypass(&bypass));
  CHECK(bypass == GLOBAL_SENSOR_OBEYED);

  APPLY(model, "A0 00 10 6A 06 01 00 64 01", &state);
  bool on = false;
  REQUIRE(state.watering_day(0, 0, &on));
  CHECK(on);
  REQUIRE(state.watering_day(0, 1, &on));
  CHECK_FALSE(on);
  REQUIRE(state.watering_day(0, 6, &on));
  CHECK_FALSE(on);
  uint8_t frequency = 0;
  REQUIRE(state.frequency(0, &frequency));
  CHECK(frequency == FREQ_CYCLIC);
  uint8_t cycle = 0;
  REQUIRE(state.cycle_days(0, &cycle));
  CHECK(cycle == 6);
  uint8_t until = 0;
  REQUIRE(state.days_until(0, &until));
  CHECK(until == 1);
  uint8_t raw[8];
  uint8_t raw_size = 0;
  REQUIRE(state.copy_page(0x10, raw, sizeof(raw), &raw_size));
  CHECK(raw_size == 6);
  CHECK(raw[3] == 0x00);
  CHECK(raw[4] == 0x64);

  APPLY(model, "A0 00 60 00 F0 FF FF FF FF FF FF", &state);
  int32_t minutes = 0;
  REQUIRE(state.start_minutes(0, 0, &minutes));
  CHECK(minutes == 240);
  REQUIRE(state.start_enabled(0, 0, &on));
  CHECK(on);
  REQUIRE(state.start_minutes(0, 1, &minutes));
  CHECK(minutes == -1);
  REQUIRE(state.start_enabled(0, 1, &on));
  CHECK_FALSE(on);

  APPLY(model, "A0 00 80 00 19 00 01 00 00 00 14 00 02 00 00", &state);
  uint16_t run = 0;
  REQUIRE(state.run_minutes(1, 0, &run));
  CHECK(run == 25);
  REQUIRE(state.run_minutes(1, 1, &run));
  CHECK(run == 1);
  REQUIRE(state.run_minutes(2, 0, &run));
  CHECK(run == 20);
  REQUIRE(state.run_minutes(2, 2, &run));
  CHECK(run == 0);
}

TEST_CASE("locate maps zones onto run-time pages") {
  EspTm2Model model;
  auto first = model.locate(Field::RUN_TIME, 1, 0, 0);
  REQUIRE(first.has_value());
  CHECK(first->page == 0x80);
  CHECK(first->offset == 0);
  CHECK(first->width == 2);

  auto second = model.locate(Field::RUN_TIME, 2, 1, 0);
  REQUIRE(second.has_value());
  CHECK(second->page == 0x80);
  CHECK(second->offset == 8);

  auto zone11 = model.locate(Field::RUN_TIME, 11, 0, 0);
  REQUIRE(zone11.has_value());
  CHECK(zone11->page == 0x85);
  CHECK(zone11->offset == 0);

  auto zone12 = model.locate(Field::RUN_TIME, 12, 2, 0);
  REQUIRE(zone12.has_value());
  CHECK(zone12->page == 0x85);
  CHECK(zone12->offset == 10);

  CHECK_FALSE(model.locate(Field::RUN_TIME, 13, 0, 0).has_value());
  CHECK_FALSE(model.locate(Field::RUN_TIME, 0, 0, 0).has_value());

  auto monday = model.locate(Field::WATERING_DAY, 0, 0, 0);
  REQUIRE(monday.has_value());
  CHECK(monday->page == 0x10);
  CHECK(monday->mask == DAY_MONDAY);
  auto sunday = model.locate(Field::WATERING_DAY, 0, 0, 6);
  REQUIRE(sunday.has_value());
  CHECK(sunday->mask == DAY_SUNDAY);
}

TEST_CASE("a program page edit leaves bytes 3 and 4 alone") {
  EspTm2Model model;
  ControllerState state;
  APPLY(model, "A0 00 10 6A 06 01 00 64 01", &state);
  uint8_t page[8];
  uint8_t size = 0;
  REQUIRE(state.copy_page(0x10, page, sizeof(page), &size));

  uint8_t edited[8];
  size_t edited_size = sizeof(edited);
  REQUIRE(model.patch(0x10, page, size, Field::WATERING_DAY, 0, 0, 0, 0, edited, &edited_size));
  CHECK(edited_size == 6);
  CHECK(edited[0] == static_cast<uint8_t>(0x6A & ~DAY_MONDAY));
  CHECK(edited[1] == 0x06);
  CHECK(edited[2] == 0x01);
  CHECK(edited[3] == 0x00);
  CHECK(edited[4] == 0x64);
  CHECK(edited[5] == FREQ_CYCLIC);

  edited_size = sizeof(edited);
  REQUIRE(model.patch(0x10, page, size, Field::FREQUENCY, 0, 0, 0, FREQ_ODD, edited, &edited_size));
  CHECK(edited[0] == 0x6A);
  CHECK(edited[3] == 0x00);
  CHECK(edited[4] == 0x64);
  CHECK(edited[5] == FREQ_ODD);

  uint8_t starts[8];
  std::memset(starts, 0, sizeof(starts));
  edited_size = sizeof(edited);
  REQUIRE(model.patch(0x60, starts, 8, Field::START_TIME, 0, 0, 1, -1, edited, &edited_size));
  CHECK(edited[2] == 0xFF);
  CHECK(edited[3] == 0xFF);
  CHECK(edited[0] == 0x00);
}

TEST_CASE("short poll uses 4C only when the probe said it is supported") {
  EspTm2Model model;
  FieldSet used;
  used.add(Field::ZONES_RUNNING);
  used.add(Field::CLOCK);
  used.add(Field::RAIN_DELAY);
  used.add(Field::RAIN_SENSOR);
  used.add(Field::IRRIGATION_ENABLED);
  used.add(Field::ACTIVE_ZONE);
  used.add(Field::REMAINING_TIME);

  Capabilities upgraded;
  CommandSupport supported;
  supported.command = 0x4C;
  supported.supported = true;
  model.note_support(supported, &upgraded);
  RequestList with_combined;
  model.short_poll(upgraded, used, &with_combined);
  expect_requests(with_combined, {"3F 00", "4C", "3B 00"});

  // 0005 does not implement 4C. The probe is the only reason this list changes.
  CHECK(model.accepts(0x0005));
  Capabilities original;
  supported.supported = false;
  model.note_support(supported, &original);
  RequestList fallback;
  model.short_poll(original, used, &fallback);
  expect_requests(fallback, {"3F 00", "10", "12", "36", "3E", "48", "3B 00"});

  FieldSet valves;
  valves.add(Field::ZONES_RUNNING);
  RequestList only_zones;
  model.short_poll(upgraded, valves, &only_zones);
  expect_requests(only_zones, {"3F 00"});
}

TEST_CASE("slow poll and schedule pages follow the fields in use") {
  EspTm2Model model;
  FieldSet used;
  used.add(Field::SEASONAL_ADJUST);
  used.add(Field::RUN_TIME);
  used.add(Field::ZONE_FAULT);
  used.add(Field::SCHEDULE_CHANGED);

  Capabilities caps;
  CommandSupport event;
  event.command = 0x4A;
  event.supported = true;
  model.note_support(event, &caps);
  CommandSupport faults;
  faults.command = 0x3D;
  faults.supported = true;
  model.note_support(faults, &caps);

  RequestList slow;
  model.slow_poll(caps, used, &slow);
  expect_requests(slow, {"30 00", "30 01", "30 02", "4A 01", "3D 00"});

  caps.event_timestamp = false;
  caps.station_error = false;
  RequestList quiet;
  model.slow_poll(caps, used, &quiet);
  expect_requests(quiet, {"30 00", "30 01", "30 02"});

  FieldSet runs;
  runs.add(Field::RUN_TIME);
  RequestList pages;
  model.schedule_pages(runs, (1u << 0) | (1u << 1), 0, &pages);
  expect_requests(pages, {"20 00 80"});

  RequestList split;
  model.schedule_pages(runs, (1u << 0) | (1u << 2), 0, &split);
  expect_requests(split, {"20 00 80", "20 00 81"});

  RequestList last;
  model.schedule_pages(runs, 1u << 11, 0, &last);
  expect_requests(last, {"20 00 85"});

  FieldSet frequency;
  frequency.add(Field::FREQUENCY);
  RequestList programs;
  model.schedule_pages(frequency, 0, 0x1, &programs);
  expect_requests(programs, {"20 00 10"});
}
