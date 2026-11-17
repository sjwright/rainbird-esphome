#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "capabilities.h"
#include "controller_state.h"
#include "model.h"
#include "sip_commands.h"

namespace esphome::rainbird {

// The logical value of one page field: minutes, days, 0/1 for a day bit or a bypass, -1 for an
// off start slot. False when the field does not fit in `size`.
bool read_codec(CodecKind kind, const PageField &field, const uint8_t *data, size_t size, int32_t *value);
// The inverse. Other bytes and other bits of the same byte are left as they are.
bool write_codec(CodecKind kind, const PageField &field, int32_t value, uint8_t *data, size_t size);

// The ESP-TM2: its limits, poll plan, page map and reply handling.
class EspTm2Model {
 public:
  const ModelLimits &limits() const;
  bool accepts(uint16_t model_id) const;
  void short_poll(const Capabilities &caps, FieldSet used, RequestList *out) const;
  void slow_poll(const Capabilities &caps, FieldSet used, RequestList *out) const;
  // The page map: where a schedule field lives. Reading a page and editing one use this
  // and nothing else, so the two cannot disagree. No value: not a page field, or out of range.
  std::optional<PageField> locate(Field field, uint8_t zone, uint8_t program, uint8_t index) const;
  CodecKind codec(Field field) const;
  void apply_reply(const Reply &reply, ControllerState *state) const;
  // Copy the page, then change one field. Every other byte stays, including program-page bytes 3 and 4.
  bool patch(uint8_t page, const uint8_t *data, size_t size, Field field, uint8_t zone, uint8_t program, uint8_t index,
             int32_t value, uint8_t *out, size_t *out_size) const;

 private:
  void apply_page_(const SchedulePage &page, ControllerState *state) const;
};

}  // namespace esphome::rainbird
