#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "clock_sync.h"
#include "controller_state.h"
#include "entity_kind.h"
#include "model.h"
#include "session.h"

namespace esphome::rainbird {

// What each entity kind shows, what it polls and how it writes (PLAN 5.3). Pure, so the
// hub only publishes and the rules here run in host tests. entity_kinds.py is the same
// table for the YAML side; tests/python/test_entity_kinds.py checks the write paths agree.

// How a kind writes. The same names as Write in entity_kinds.py.
enum class WritePath : uint8_t { NONE, LOCAL, COMMAND, SCHEDULE_PAGE };

// Where a kind's value comes from, and so which change flag republishes it.
enum class Source : uint8_t {
  STATE,     // ControllerState: `field` and `also` changed
  LINK,      // Session::link()
  IDENTITY,  // Session::identity()
};

struct KindInfo {
  EntityKind kind;
  // Shown and polled. For a schedule-page kind, also the field a write edits. NO_FIELD: none.
  Field field;
  // Also shown: a change republishes the entity, but this kind does not poll it.
  Field also;
  Source source;
  WritePath write;
};

const KindInfo &kind_info(EntityKind kind);

// One entity: its kind, and the zone, program and day or slot it is for.
struct EntityBinding {
  EntityKind kind{EntityKind::VALVE};
  uint8_t zone{NO_ZONE};
  uint8_t program{NO_PROGRAM};
  uint8_t index{0};  // the watering day (Monday 0) or start slot (0-based), else 0
};

// Adds what an entity shows to the poll plan (PLAN 6.4): the fields to poll and the schedule
// pages to read. Set config->experimental and valve_queue first: queue-mode valves need the queue list.
void note_binding(const Model &model, const EntityBinding &binding, SessionConfig *config);

// Everything an entity can show, and the change flags for what is not in ControllerState.
struct Readout {
  const ControllerState &state;
  const Identity &identity;
  const LinkStatus &link;
  bool identity_changed{false};
  LinkChanges link_changes{};
};

Readout readout(const Session &session);

// Zone, program and model names from the YAML, and local time for the controller's timestamps.
struct TextContext {
  std::function<std::string(uint8_t zone)> zone;        // "Zone N" when unset
  std::function<std::string(uint8_t program)> program;  // "Program A" when unset
  const char *model{"Rain Bird"};
  // Seconds since 2000 on the controller's clock to ISO 8601 UTC. Unset: no schedule_changed text.
  std::function<bool(uint32_t seconds_since_2000, std::string *out)> timestamp;
};

// True when a value the entity shows may have changed since the last clear_changed().
bool kind_changed(const EntityBinding &binding, const Readout &readout);

// The value to publish. False while it is unknown: nothing is shown until the controller says it.
bool bool_value(const EntityBinding &binding, const Readout &readout, bool *out);
bool number_value(const EntityBinding &binding, const Readout &readout, float *out);
bool text_value(const EntityBinding &binding, const Readout &readout, const TextContext &context, std::string *out);
// Frequency select. The value is the option index, which is the page byte.
bool select_index(const EntityBinding &binding, const Readout &readout, uint8_t *out);
// A start slot's time. False while it is unknown or the slot is off: an off slot has no time.
bool start_time(const EntityBinding &binding, const Readout &readout, uint8_t *hour, uint8_t *minute);
// The controller clock, date and time together.
bool controller_clock(const Readout &readout, WallClock *out);

// "Lawns", "Manual" or "Idle".
bool active_program_text(const ControllerState &state, const TextContext &context, std::string *out);
// "Front lawn 24:48, Back lawn 20:00", or "Empty".
bool queue_text(const ControllerState &state, const TextContext &context, std::string *out);
// "ESP-TM2 (000A), protocol 9.12"
std::string model_text(const Identity &identity, const char *model_name);
// "1.47.0"
std::string firmware_text(const Identity &identity);
// The 8 serial bytes in hex, "0000000000008963".
std::string serial_text(const Identity &identity);
// 1488 -> "24:48", 4000 -> "1:06:40".
std::string duration_text(uint32_t seconds);

}  // namespace esphome::rainbird
