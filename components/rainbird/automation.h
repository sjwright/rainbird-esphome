#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include "rainbird.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::rainbird {

// Trigger variable names match the YAML lambdas: command, code, payload.
class NakTrigger : public Trigger<uint8_t, uint8_t> {};
class NoReplyTrigger : public Trigger<uint8_t> {};
class FrameTrigger : public Trigger<std::vector<uint8_t>> {};

// rainbird.send_raw.
template<typename... Ts> class RainbirdSendRawAction final : public Action<Ts...>, public Parented<RainbirdComponent> {
 public:
  void set_data_template(std::vector<uint8_t> (*func)(Ts...)) { this->func_ = func; }
  void set_data_static(const uint8_t *data, size_t length) {
    this->data_ = data;
    this->length_ = length;
  }

  void play(const Ts &...x) override {
    if (this->func_ != nullptr) {
      this->parent_->send_raw(this->func_(x...));
      return;
    }
    this->parent_->send_raw(this->data_, this->length_);
  }

 protected:
  std::vector<uint8_t> (*func_)(Ts...){nullptr};
  const uint8_t *data_{nullptr};
  size_t length_{0};
};

// rainbird.zone.start and rainbird.zone.queue. The hub checks the zone and duration,
// because either can come from a template.
template<typename... Ts> class RainbirdZoneAction final : public Action<Ts...>, public Parented<RainbirdComponent> {
 public:
  // int32_t, so a template's out-of-range zone is refused rather than wrapped into a valid one.
  TEMPLATABLE_VALUE(int32_t, zone)
  TEMPLATABLE_VALUE(uint32_t, duration_ms)
  void set_zone_entity(RainbirdZone *zone) { this->zone_entity_ = zone; }
  void set_queue(bool queue) { this->queue_ = queue; }

  void play(const Ts &...x) override {
    int32_t zone = this->zone_entity_ != nullptr ? this->zone_entity_->get_zone() : this->zone_.value(x...);
    this->parent_->run_zone(zone, this->duration_ms_.value(x...), this->queue_);
  }

 protected:
  RainbirdZone *zone_entity_{nullptr};
  bool queue_{false};
};

}  // namespace esphome::rainbird
