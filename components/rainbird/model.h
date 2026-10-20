#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "controller_state.h"
#include "sip_commands.h"

namespace esphome::rainbird {

struct ModelLimits {
  const char *name;
  uint8_t zones;
  uint8_t programs;
  uint8_t starts;
};

// Probe results. The model id is not a feature table (PLAN 6.3).
struct Capabilities {
  bool combined{false};          // 4C
  bool event_timestamp{false};   // 4A
  bool advance{false};           // 42
  bool queue_run{false};         // 4B
  bool station_error{false};     // 3D
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
  ZONE_BYPASS,    // logical on (ignores the sensor) is wire 0x00, the opposite of page 00
};

struct ValueCodec {
  CodecKind kind{CodecKind::NONE};
};

struct PageField {
  uint8_t page{0};
  uint8_t offset{0};
  uint8_t width{0};
  uint8_t mask{0xFF};
};

struct OptionList {
  const char *const *names;
  uint8_t count;
};

class Model {
 public:
  virtual ~Model() = default;
  virtual const ModelLimits &limits() const = 0;
  virtual bool accepts(uint16_t model_id) const = 0;
  virtual void short_poll(const Capabilities &caps, FieldSet used, RequestList *out) const = 0;
  virtual void slow_poll(const Capabilities &caps, FieldSet used, RequestList *out) const = 0;
  // zone_mask bit 0 is zone 1. program_mask bit 0 is program A. A zero mask means every zone or program.
  virtual void schedule_pages(FieldSet used, uint16_t zone_mask, uint8_t program_mask, RequestList *out) const = 0;
  virtual std::optional<PageField> locate(Field field, uint8_t zone, uint8_t program, uint8_t index) const = 0;
  virtual ValueCodec codec(Field field) const = 0;
  virtual OptionList frequency_options() const = 0;
  virtual void apply_reply(const Reply &reply, ControllerState *state) const = 0;
  virtual void note_support(const CommandSupport &support, Capabilities *caps) const = 0;
  // Copy the page, then change one field. Every other byte stays, including program-page bytes 3 and 4.
  virtual bool patch(uint8_t page, const uint8_t *data, size_t size, Field field, uint8_t zone, uint8_t program,
                     uint8_t index, int32_t value, uint8_t *out, size_t *out_size) const = 0;
};

bool write_codec(const ValueCodec &codec, const PageField &field, int32_t value, uint8_t *data, size_t size);

}  // namespace esphome::rainbird
