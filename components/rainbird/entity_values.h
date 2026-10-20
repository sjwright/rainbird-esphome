#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "controller_state.h"
#include "entity_kind.h"
#include "session.h"

namespace esphome::rainbird {

// What each entity kind reads and how it is shown. Pure, so the hub only
// publishes and the rules here run in host tests.

// Adds the ControllerState fields a kind reads, for the poll plan (PLAN 6.4).
void add_fields(EntityKind kind, FieldSet *used);

// True when a value the entity shows may have changed since the last clear_changed().
bool kind_changed(EntityKind kind, const ControllerState &state, uint8_t zone, uint8_t program);

bool bool_value(EntityKind kind, const ControllerState &state, uint8_t zone, uint8_t program, bool *out);
bool number_value(EntityKind kind, const ControllerState &state, uint8_t zone, uint8_t program, float *out);

// Zone and program display names, from the YAML. Fall back to "Zone N" / "Program A".
struct Names {
  std::function<std::string(uint8_t zone)> zone;
  std::function<std::string(uint8_t program)> program;
};

// "Lawns", "Manual" or "Idle".
bool active_program_text(const ControllerState &state, const Names &names, std::string *out);
// "Front lawn 24:48, Back lawn 20:00", or "Empty".
bool queue_text(const ControllerState &state, const Names &names, std::string *out);
// "ESP-TM2 (000A), protocol 9.12"
std::string model_text(const Identity &identity, const char *model_name);
// "1.47.0"
std::string firmware_text(const Identity &identity);
// The 8 serial bytes in hex, "0000000000008963".
std::string serial_text(const Identity &identity);
// 1488 -> "24:48", 4000 -> "1:06:40".
std::string duration_text(uint32_t seconds);

}  // namespace esphome::rainbird
