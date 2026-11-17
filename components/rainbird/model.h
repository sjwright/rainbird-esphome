#pragma once

#include <cstddef>
#include <cstdint>

#include "sip_commands.h"

namespace esphome::rainbird {

// The types a model works in: its limits, the requests it plans, and where and how a schedule
// field sits on its page. The one model is EspTm2Model (model_esp_tm2.h).

struct ModelLimits {
  uint8_t zones;
  uint8_t programs;
  uint16_t seasonal_min_percent;
  uint16_t seasonal_max_percent;
  uint16_t rain_delay_max_days;
  uint8_t run_max_minutes;  // 39, 4B and 3A take minutes in one byte
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

}  // namespace esphome::rainbird
