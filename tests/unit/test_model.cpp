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
  CHECK(model.accepts(0x0005));
  CHECK(model.accepts(0x000A));
  CHECK(model.accepts(0x010A));
  CHECK(model.accepts(0x0014));
  CHECK(model.accepts(0x0015));
  CHECK_FALSE(model.accepts(0x0006));
}

TEST_CASE("combined state keeps seconds and ignores FFFF seasonal adjust") {
  EspTm2Model model;
  ControllerState state;
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00", &state);
  REQUIRE(state.time.has_value());
  CHECK(state.time->hour == 20);
  CHECK(state.time->minute == 11);
  CHECK(state.time->second == 35);
  REQUIRE(state.date.has_value());
  CHECK(state.date->day == 8);
  CHECK(state.date->month == 1);
  CHECK(state.date->year == 2023);
  CHECK(state.rain_delay_days == 3);
  CHECK(state.rain_sensor == 0);
  CHECK(state.irrigation == 1);
  CHECK_FALSE(state.seasonal[0].has_value());
  CHECK(state.remaining_seconds == 0);

  ControllerState seconds;
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 01 67 00", &seconds);
  CHECK(seconds.remaining_seconds == 359);
}

TEST_CASE("a date that is not a real date is unknown, and the rest of the reply still applies") {
  EspTm2Model model;
  ControllerState state;
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00", &state);
  // Month 0.
  APPLY(model, "CC 14 0B 23 08 07 E7 00 05 00 01 FF FF 00 00 00", &state);
  CHECK_FALSE(state.date.has_value());
  CHECK(state.rain_delay_days == 5);

  APPLY(model, "92 13 A7 EA", &state);
  CHECK(state.date.has_value());
  APPLY(model, "92 00 A7 EA", &state);
  CHECK_FALSE(state.date.has_value());
}

TEST_CASE("every remaining-time reading is counted, changed or not") {
  EspTm2Model model;
  ControllerState state;
  CHECK(state.remaining_readings == 0);
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 01 67 00", &state);
  CHECK(state.remaining_readings == 1);
  // A paused controller repeats the value.
  APPLY(model, "CC 14 0B 25 08 17 E7 00 03 00 01 FF FF 01 67 00", &state);
  CHECK(state.remaining_readings == 2);
  APPLY(model, "BB 00 01 00 01 67 00 00 01 FF 00 01", &state);
  CHECK(state.remaining_readings == 3);
}

TEST_CASE("queue FF is idle or manual, and seconds are big-endian") {
  EspTm2Model model;
  ControllerState idle;
  APPLY(model, "BB 00 00 00 00 00 00 00 00 FF 00 00", &idle);
  CHECK(idle.activity() == Activity::IDLE);
  CHECK(idle.program == 0xFF);
  CHECK(idle.active_zone == 0);

  ControllerState running;
  APPLY(model, "BB 00 05 00 05 D0 00 00 01 00 00 01", &running);
  CHECK(running.activity() == Activity::PROGRAM);
  CHECK(running.program == 0);
  CHECK(running.remaining_seconds == 1488);
  CHECK(running.queued_count == 5);

  ControllerState manual;
  APPLY(model, "BB 00 01 00 00 3C 00 00 01 FF 00 01", &manual);
  CHECK(manual.activity() == Activity::MANUAL);
  CHECK(manual.remaining_seconds == 60);
  CHECK(manual.active_zone == 1);

  ControllerState list;
  APPLY(model, "BB 01 21 05 D0 22 04 B0 23 01 A4 24 04 B0 25 02 58 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", &list);
  REQUIRE(list.queue.has_value());
  CHECK(list.queue->count == 5);
  CHECK(list.queue->slots[0].zone == 1);
  CHECK(list.queue->slots[0].flags == 0x20);
  CHECK(list.queue->slots[0].seconds == 1488);
  CHECK(list.queue->slots[2].zone == 3);
  CHECK(list.queue->slots[2].seconds == 420);
  CHECK(list.queue->slots[4].zone == 5);
  CHECK(list.queue->slots[4].seconds == 600);
}

TEST_CASE("schedule pages fill run times, days and start slots") {
  EspTm2Model model;
  ControllerState state;
  APPLY(model, "A0 00 00 00 00 04 00", &state);
  CHECK(state.station_delay == 0);
  // Byte 2 is also the rain delay, but that comes from 4C or 36. Page fields are the editable ones.
  CHECK_FALSE(state.rain_delay_days.has_value());
  CHECK(state.global_bypass == false);

  APPLY(model, "A0 00 10 6A 06 01 00 64 01", &state);
  CHECK(state.watering_day[0][0] == true);
  CHECK(state.watering_day[0][1] == false);
  CHECK(state.watering_day[0][6] == false);
  CHECK(state.frequency[0] == FREQ_CYCLIC);
  CHECK(state.cycle_days[0] == 6);
  CHECK(state.days_until[0] == 1);

  APPLY(model, "A0 00 60 00 F0 FF FF FF FF FF FF", &state);
  CHECK(state.start_minutes[0][0] == 240);
  CHECK(state.start_minutes[0][1] == -1);

  APPLY(model, "A0 00 80 00 19 00 01 00 00 00 14 00 02 00 00", &state);
  CHECK(state.run_minutes[0][0] == 25);
  CHECK(state.run_minutes[0][1] == 1);
  CHECK(state.run_minutes[1][0] == 20);
  CHECK(state.run_minutes[1][2] == 0);
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
  const uint8_t page[6] = {0x6A, 0x06, 0x01, 0x00, 0x64, 0x01};
  const uint8_t size = sizeof(page);

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

  // Values that do not fit their field are refused, not truncated.
  edited_size = sizeof(edited);
  CHECK_FALSE(model.patch(0x10, page, size, Field::CYCLE_DAYS, 0, 0, 0, 256, edited, &edited_size));
  edited_size = sizeof(edited);
  CHECK_FALSE(model.patch(0x10, page, size, Field::FREQUENCY, 0, 0, 0, -1, edited, &edited_size));
  edited_size = sizeof(edited);
  CHECK_FALSE(model.patch(0x60, starts, 8, Field::START_TIME, 0, 0, 1, 24 * 60, edited, &edited_size));
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
  used.add(Field::SCHEDULE_CHANGED);

  Capabilities caps;
  CommandSupport event;
  event.command = 0x4A;
  event.supported = true;
  model.note_support(event, &caps);

  RequestList slow;
  model.slow_poll(caps, used, &slow);
  expect_requests(slow, {"30 00", "30 01", "30 02", "4A 01"});

  caps.event_timestamp = false;
  RequestList quiet;
  model.slow_poll(caps, used, &quiet);
  expect_requests(quiet, {"30 00", "30 01", "30 02"});
}

TEST_CASE("reading a page and editing it use the same map") {
  // locate() is the only place that knows where a field lives. Write a value with it,
  // read it back with it, and the two must agree for every field, zone, program and slot.
  EspTm2Model model;
  const Field fields[] = {Field::RUN_TIME,  Field::WATERING_DAY, Field::CYCLE_DAYS,    Field::DAYS_UNTIL_NEXT,
                          Field::FREQUENCY, Field::START_TIME,   Field::STATION_DELAY, Field::GLOBAL_SENSOR_BYPASS};
  for (Field field : fields) {
    for (uint8_t zone = 0; zone <= STATE_ZONES; zone++) {
      for (uint8_t program = 0; program < STATE_PROGRAMS; program++) {
        for (uint8_t index = 0; index < 7; index++) {
          auto located = model.locate(field, zone, program, index);
          if (!located.has_value()) {
            continue;
          }
          uint8_t page[MAX_PAGE_BYTES] = {};
          int32_t written = field == Field::START_TIME ? 365 : 1;
          REQUIRE(write_codec(model.codec(field), *located, written, page, sizeof(page)));
          int32_t read = -2;
          REQUIRE(read_codec(model.codec(field), *located, page, sizeof(page), &read));
          CHECK(read == written);
        }
      }
    }
  }
}

TEST_CASE("a non-FFFF 4C budget is not program A's") {
  EspTm2Model model;
  ControllerState state;
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 00 50 00 00 00", &state);
  CHECK_FALSE(state.seasonal[0].has_value());
  APPLY(model, "B0 00 00 64", &state);
  CHECK(state.seasonal[0] == 100);
  APPLY(model, "CC 14 0B 23 08 17 E7 00 03 00 01 00 50 00 00 00", &state);
  CHECK(state.seasonal[0] == 100);
}

TEST_CASE("the start-slot switch turns a slot off but never writes a time") {
  EspTm2Model model;
  // Slot 1 starts at 04:00, slot 2 is off.
  const uint8_t starts[8] = {0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t edited[8];
  size_t edited_size = sizeof(edited);

  REQUIRE(model.patch(0x60, starts, 8, Field::START_ENABLED, 0, 0, 0, 0, edited, &edited_size));
  CHECK(edited[0] == 0xFF);
  CHECK(edited[1] == 0xFF);

  // Turning on a slot that is already on keeps its time.
  edited_size = sizeof(edited);
  REQUIRE(model.patch(0x60, starts, 8, Field::START_ENABLED, 0, 0, 0, 1, edited, &edited_size));
  CHECK(edited[0] == 0x00);
  CHECK(edited[1] == 0xF0);

  // Turning on an off slot needs a time, so it is refused. Before, this wrote 00:01.
  edited_size = sizeof(edited);
  CHECK_FALSE(model.patch(0x60, starts, 8, Field::START_ENABLED, 0, 0, 1, 1, edited, &edited_size));

  // START_TIME is how a slot gets a time.
  edited_size = sizeof(edited);
  REQUIRE(model.patch(0x60, starts, 8, Field::START_TIME, 0, 0, 1, 360, edited, &edited_size));
  CHECK(edited[2] == 0x01);
  CHECK(edited[3] == 0x68);
}

TEST_CASE("the queue field polls both queue pages") {
  EspTm2Model model;
  Capabilities caps;
  FieldSet queue;
  queue.add(Field::QUEUE);
  RequestList list;
  model.short_poll(caps, queue, &list);
  expect_requests(list, {"3B 00", "3B 01"});
}

TEST_CASE("probes for 49 and 3A are recorded") {
  EspTm2Model model;
  Capabilities caps;
  CommandSupport support;
  support.supported = true;
  support.command = 0x49;
  model.note_support(support, &caps);
  support.command = 0x3A;
  model.note_support(support, &caps);
  CHECK(caps.controller_state);
  CHECK(caps.test_stations);
  support.supported = false;
  model.note_support(support, &caps);
  CHECK_FALSE(caps.test_stations);
}
