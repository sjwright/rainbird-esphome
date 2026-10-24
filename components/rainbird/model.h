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

// Probe results. The model id is not a feature table (PLAN 6.3).
struct Capabilities {
  bool combined{false};          // 4C
  bool event_timestamp{false};   // 4A
  bool advance{false};           // 42
  bool queue_run{false};         // 4B
  bool controller_state{false};  // 49, irrigation on/off
  bool test_stations{false};     // 3A
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
