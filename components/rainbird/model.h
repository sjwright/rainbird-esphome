#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "controller_state.h"
#include "sip_commands.h"

namespace esphome::rainbird {

struct ModelLimits {
  uint8_t zones;
  uint8_t programs;
  uint16_t seasonal_min_percent;
  uint16_t seasonal_max_percent;
  uint16_t rain_delay_max_days;
  uint8_t run_max_minutes;  // 39, 4B and 3A take minutes in one byte
};

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

struct RequestList {
  static constexpr size_t CAP = 24;
  ByteBuf items[CAP]{};
  uint8_t count{0};

  bool add(const ByteBuf &buf) {
    if (this->count >= CAP) {
      return false;
    }
    this->items[this->count++] = buf;
    return true;
  }
};

enum class CodecKind : uint8_t {
  NONE = 0,
  U8,
  U16_BE,
  START_SLOT,  // minutes after midnight, negative means the slot is off (FFFF)
  // The enabled switch of a start slot. 0 writes FFFF. Nonzero keeps a slot that is
  // already on and refuses one that is off: enabling needs a time, so the caller
  // writes START_TIME with the minutes instead.
  SLOT_ENABLED,
  BIT,
  GLOBAL_BYPASS,  // logical on (bypassed) is wire 0x01
};

struct PageField {
  uint8_t page{0};
  uint8_t offset{0};
  uint8_t width{0};
  uint8_t mask{0xFF};
};

// The logical value of one page field: minutes, days, 0/1 for a day bit or a bypass, -1 for an
// off start slot. False when the field does not fit in `size`.
bool read_codec(CodecKind kind, const PageField &field, const uint8_t *data, size_t size, int32_t *value);
// The inverse. Other bytes and other bits of the same byte are left as they are.
bool write_codec(CodecKind kind, const PageField &field, int32_t value, uint8_t *data, size_t size);

}  // namespace esphome::rainbird
