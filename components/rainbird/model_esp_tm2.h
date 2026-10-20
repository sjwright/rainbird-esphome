#pragma once

#include "model.h"

namespace esphome::rainbird {

class EspTm2Model final : public Model {
 public:
  const ModelLimits &limits() const override;
  bool accepts(uint16_t model_id) const override;
  void short_poll(const Capabilities &caps, FieldSet used, RequestList *out) const override;
  void slow_poll(const Capabilities &caps, FieldSet used, RequestList *out) const override;
  void schedule_pages(FieldSet used, uint16_t zone_mask, uint8_t program_mask, RequestList *out) const override;
  std::optional<PageField> locate(Field field, uint8_t zone, uint8_t program, uint8_t index) const override;
  ValueCodec codec(Field field) const override;
  OptionList frequency_options() const override;
  void apply_reply(const Reply &reply, ControllerState *state) const override;
  void note_support(const CommandSupport &support, Capabilities *caps) const override;
  bool patch(uint8_t page, const uint8_t *data, size_t size, Field field, uint8_t zone, uint8_t program, uint8_t index,
             int32_t value, uint8_t *out, size_t *out_size) const override;
};

}  // namespace esphome::rainbird
