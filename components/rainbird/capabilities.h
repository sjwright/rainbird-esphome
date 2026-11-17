#pragma once

#include <cstdint>

namespace esphome::rainbird {

// The optional commands, each probed with 04 at startup (PLAN 6.3). This is the one list of
// them: a row here and in CAPABILITY_OPCODES is probed, tracked and logged with the rest.
enum class Capability : uint8_t {
  COMBINED = 0,      // 4C
  EVENT_TIMESTAMP,   // 4A
  ADVANCE,           // 42
  QUEUE_RUN,         // 4B
  CONTROLLER_STATE,  // 49, irrigation on/off
  TEST_STATIONS,     // 3A
  COUNT,
};

static constexpr uint8_t CAPABILITY_COUNT = static_cast<uint8_t>(Capability::COUNT);
// The command each capability is, in enum order. The probes go out in this order.
static constexpr uint8_t CAPABILITY_OPCODES[CAPABILITY_COUNT] = {0x4C, 0x4A, 0x42, 0x4B, 0x49, 0x3A};

// UNKNOWN until the probe has an answer: a probe that timed out says nothing about the command.
enum class Support : uint8_t { UNKNOWN = 0, NO, YES };

// Probe results. The model id is not a feature table (PLAN 6.3).
struct Capabilities {
  Support support[CAPABILITY_COUNT]{};

  bool has(Capability capability) const { return this->support[static_cast<uint8_t>(capability)] == Support::YES; }
  bool known(Capability capability) const {
    return this->support[static_cast<uint8_t>(capability)] != Support::UNKNOWN;
  }
  void set(Capability capability, bool supported) {
    this->support[static_cast<uint8_t>(capability)] = supported ? Support::YES : Support::NO;
  }

  // By command. An opcode that is not an optional command is not known and setting it does nothing.
  static bool find(uint8_t opcode, Capability *out) {
    for (uint8_t i = 0; i < CAPABILITY_COUNT; i++) {
      if (CAPABILITY_OPCODES[i] == opcode) {
        *out = static_cast<Capability>(i);
        return true;
      }
    }
    return false;
  }
  bool known(uint8_t opcode) const {
    Capability capability;
    return find(opcode, &capability) && this->known(capability);
  }
  void set(uint8_t opcode, bool supported) {
    Capability capability;
    if (find(opcode, &capability)) {
      this->set(capability, supported);
    }
  }
};

}  // namespace esphome::rainbird
