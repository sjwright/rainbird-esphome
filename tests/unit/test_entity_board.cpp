#include "doctest.h"

#include "entity_board.h"
#include "hex.h"
#include "model_esp_tm2.h"

#include <string>
#include <vector>

using namespace esphome::rainbird;

namespace {

void apply(ControllerState &state, const char *hex) {
  EspTm2Model model;
  auto bytes = from_hex(hex);
  Reply reply;
  REQUIRE(parse_reply(bytes.data(), bytes.size(), &reply));
  model.apply_reply(reply, &state);
}

EntityBinding bind(EntityKind kind, uint8_t zone = NO_ZONE, uint8_t program = NO_PROGRAM, uint8_t index = 0) {
  return EntityBinding{kind, zone, program, index};
}

// What the session would hand the hub, built by hand.
struct Shown {
  ControllerState state;
  Identity identity;
  LinkStatus link;

  Readout view() const { return Readout{state, identity, link}; }
};

// Entities are names here. The sink writes down what the hub would have been told to do.
struct Recorder final : BoardSink {
  std::vector<std::string> log;

  static const char *name(void *entity) { return static_cast<const char *>(entity); }

  void show_bool(Domain domain, void *entity, bool value) override {
    this->log.push_back(std::string(domain == Domain::SWITCH ? "switch " : "binary ") + name(entity) +
                        (value ? " on" : " off"));
  }
  void show_number(Domain domain, void *entity, float value) override {
    this->log.push_back(std::string(domain == Domain::NUMBER ? "number " : "sensor ") + name(entity) + " " +
                        std::to_string(static_cast<int>(value)));
  }
  void show_option(void *entity, uint8_t index) override {
    this->log.push_back(std::string("select ") + name(entity) + " " + std::to_string(index));
  }
  void show_time(void *entity, uint8_t hour, uint8_t minute) override {
    this->log.push_back(std::string("time ") + name(entity) + " " + std::to_string(hour) + ":" +
                        std::to_string(minute));
  }
  void show_clock(void *entity, const WallClock &clock) override {
    this->log.push_back(std::string("clock ") + name(entity) + " " + std::to_string(clock.year) + " " +
                        std::to_string(clock.hour) + ":" + std::to_string(clock.minute));
  }
  void show_text(void *entity, const std::string &text) override {
    this->log.push_back(std::string("text ") + name(entity) + " " + text);
  }
  void show_unknown(Domain domain, void *entity) override {
    const char *what = domain == Domain::NUMBER ? "number " : domain == Domain::SENSOR ? "sensor " : "binary ";
    this->log.push_back(std::string("unknown ") + what + name(entity));
  }

  std::vector<std::string> take() {
    std::vector<std::string> out;
    out.swap(this->log);
    return out;
  }
};

void *entity(const char *name) { return const_cast<char *>(name); }

using Log = std::vector<std::string>;

}  // namespace

TEST_CASE("nothing is shown until the controller says it, and an unchanged value is shown once") {
  EntityBoard board;
  board.add(Domain::SWITCH, bind(EntityKind::VALVE, 1), entity("valve1"));
  board.add(Domain::BINARY_SENSOR, bind(EntityKind::ZONE_RUNNING, 2), entity("running2"));
  board.add(Domain::NUMBER, bind(EntityKind::RAIN_DELAY), entity("rain_delay"));
  board.add(Domain::SENSOR, bind(EntityKind::ACTIVE_ZONE), entity("active_zone"));
  board.add(Domain::TEXT_SENSOR, bind(EntityKind::ACTIVE_PROGRAM), entity("program"));
  board.add(Domain::BUTTON, bind(EntityKind::STOP), entity("stop"));
  Shown shown;
  Recorder sink;
  TextContext context;

  board.publish(shown.view(), context, sink);
  CHECK(sink.take().empty());

  apply(shown.state, "BF 00 01 00 00 00");                    // zone 1 running
  apply(shown.state, "BB 00 05 00 05 D0 00 00 01 00 00 01");  // program A, zone 1
  board.publish(shown.view(), context, sink);
  CHECK(sink.take() ==
        Log{"switch valve1 on", "binary running2 off", "sensor active_zone 1", "text program Program A"});

  // The same reading again publishes nothing.
  board.publish(shown.view(), context, sink);
  CHECK(sink.take().empty());

  apply(shown.state, "BF 00 02 00 00 00");  // zone 2 now
  apply(shown.state, "B6 00 03");           // rain delay 3 days
  board.publish(shown.view(), context, sink);
  CHECK(sink.take() == Log{"switch valve1 off", "binary running2 on", "number rain_delay 3"});
}

TEST_CASE("selects, times and the clock are shown from the schedule and clock replies") {
  EntityBoard board;
  board.add(Domain::SELECT, bind(EntityKind::FREQUENCY, NO_ZONE, 0), entity("frequency_a"));
  board.add(Domain::TIME, bind(EntityKind::START_TIME, NO_ZONE, 0, 0), entity("start_a1"));
  board.add(Domain::TIME, bind(EntityKind::START_TIME, NO_ZONE, 0, 1), entity("start_a2"));
  board.add(Domain::DATETIME, bind(EntityKind::CONTROLLER_CLOCK), entity("clock"));
  Shown shown;
  Recorder sink;
  TextContext context;
  apply(shown.state, "A0 00 10 6A 06 01 00 64 01");
  apply(shown.state, "A0 00 60 01 68 FF FF FF FF FF FF");  // slot 1 at 06:00, the rest off
  apply(shown.state, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 01 67 01");
  board.publish(shown.view(), context, sink);
  // An off slot has no time, so its entity is left alone.
  CHECK(sink.take() == Log{"select frequency_a 1", "time start_a1 6:0", "clock clock 2023 20:11"});
}

TEST_CASE("the remaining-time sensor is left to the countdown") {
  EntityBoard board;
  board.add(Domain::SENSOR, bind(EntityKind::REMAINING_TIME), entity("remaining"));
  board.add(Domain::TEXT_SENSOR, bind(EntityKind::ENDS_AT), entity("ends_at"));
  Shown shown;
  Recorder sink;
  TextContext context;
  apply(shown.state, "BB 00 05 00 05 D0 00 00 01 00 00 01");
  board.publish(shown.view(), context, sink);
  CHECK(sink.take().empty());

  // The hub finds them by domain and kind.
  std::vector<std::string> found;
  board.for_each(Domain::SENSOR, EntityKind::REMAINING_TIME,
                 [&](void *entity) { found.push_back(Recorder::name(entity)); });
  board.for_each(Domain::TEXT_SENSOR, EntityKind::ENDS_AT,
                 [&](void *entity) { found.push_back(Recorder::name(entity)); });
  CHECK(found == Log{"remaining", "ends_at"});

  // It still goes to unknown with the rest when the link is lost.
  board.link_lost(sink);
  CHECK(sink.take() == Log{"unknown sensor remaining"});
}

TEST_CASE("a lost link shows unknown where an entity can, and the next reading is published again") {
  EntityBoard board;
  board.add(Domain::SWITCH, bind(EntityKind::VALVE, 1), entity("valve1"));
  board.add(Domain::BINARY_SENSOR, bind(EntityKind::ZONE_RUNNING, 1), entity("running1"));
  board.add(Domain::BINARY_SENSOR, bind(EntityKind::CONNECTED), entity("connected"));
  board.add(Domain::NUMBER, bind(EntityKind::RAIN_DELAY), entity("rain_delay"));
  board.add(Domain::SENSOR, bind(EntityKind::ACTIVE_ZONE), entity("active_zone"));
  board.add(Domain::SENSOR, bind(EntityKind::LINK_ERRORS), entity("link_errors"));
  board.add(Domain::SELECT, bind(EntityKind::FREQUENCY, NO_ZONE, 0), entity("frequency_a"));
  board.add(Domain::TEXT_SENSOR, bind(EntityKind::ACTIVE_PROGRAM), entity("program"));
  board.add(Domain::TEXT_SENSOR, bind(EntityKind::MODEL), entity("model"));
  Shown shown;
  Recorder sink;
  TextContext context;
  context.model = "ESP-TM2";
  auto read_everything = [&]() {
    apply(shown.state, "BF 00 01 00 00 00");
    apply(shown.state, "BB 00 05 00 05 D0 00 00 01 00 00 01");
    apply(shown.state, "B6 00 03");
    apply(shown.state, "A0 00 10 6A 06 01 00 64 01");
    shown.identity.model_known = true;
    shown.identity.model.model_id = 0x000A;
    shown.link.connected_known = true;
    shown.link.connected = true;
  };
  read_everything();
  board.publish(shown.view(), context, sink);
  REQUIRE(sink.take().size() == 9);

  // The session forgets what it read and reports the link down.
  shown.state = ControllerState{};
  shown.identity = Identity{};
  shown.link.connected = false;
  board.link_lost(sink);
  // Only what the controller reported, and only where the entity has an unknown state. The
  // switch, select and text sensor keep their last value. `connected` and the link error count
  // are the hub's own, so they stay too.
  CHECK(sink.take() == Log{"unknown binary running1", "unknown number rain_delay", "unknown sensor active_zone"});
  board.publish(shown.view(), context, sink);
  CHECK(sink.take() == Log{"binary connected off"});

  // The controller answers again with the values it had before. Every one is published, the
  // switch and the select included, because nothing shown is known to be current.
  read_everything();
  board.publish(shown.view(), context, sink);
  CHECK(sink.take() == Log{"switch valve1 on", "binary running1 on", "binary connected on", "number rain_delay 3",
                           "sensor active_zone 1", "select frequency_a 1", "text program Program A"});
}

TEST_CASE("an entity that shows a value of its own has the next reading published") {
  EntityBoard board;
  board.add(Domain::TIME, bind(EntityKind::START_TIME, NO_ZONE, 0, 0), entity("start_a1"));
  Shown shown;
  Recorder sink;
  TextContext context;
  apply(shown.state, "A0 00 60 01 68 FF FF FF FF FF FF");
  board.publish(shown.view(), context, sink);
  REQUIRE(sink.take() == Log{"time start_a1 6:0"});

  // The entity was set by hand while its slot was off, so it no longer shows 06:00.
  board.forget(entity("start_a1"));
  board.publish(shown.view(), context, sink);
  CHECK(sink.take() == Log{"time start_a1 6:0"});
}

TEST_CASE("every binding reaches the poll plan, buttons and text sensors included") {
  EntityBoard board;
  board.add(Domain::BUTTON, bind(EntityKind::ADVANCE), entity("advance"));
  board.add(Domain::TEXT_SENSOR, bind(EntityKind::SCHEDULE_CHANGED), entity("changed"));
  board.add(Domain::NUMBER, bind(EntityKind::RUN_TIME, 3, 1), entity("run_time"));
  EspTm2Model model;
  SessionConfig config;
  board.for_each_binding([&](const EntityBinding &binding) { note_binding(model, binding, &config); });
  CHECK(config.used.has(Field::QUEUE));
  CHECK(config.used.has(Field::SCHEDULE_CHANGED));
  CHECK(config.used.has(Field::RUN_TIME));
  CHECK(config.pages.has(0x81));
}
