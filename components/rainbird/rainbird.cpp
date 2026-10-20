#include "rainbird.h"

#include "esphome/core/log.h"

#include "log_tag.h"

namespace esphome::rainbird {

ESPHOME_LOG_TAG(TAG, "rainbird");

void RainbirdComponent::setup() {
  // The link enables loop() while a request is queued. Nothing is queued yet.
  this->disable_loop();
}

void RainbirdComponent::loop() {
  // rainbird_link runs here once transmitting exists.
}

void RainbirdComponent::update() {
  // Polling enqueues a cycle once the link can transmit. The interval is already set.
}

bool RainbirdComponent::submit(const WriteIntent &intent) {
  // Commands wait for the link. Local run durations are stored on the number and never get here.
  ESP_LOGW(TAG,
           "Refused command (kind %u zone %u program %u index %u value %.0f): "
           "the controller link is not implemented yet",
           static_cast<unsigned>(intent.kind), static_cast<unsigned>(intent.zone),
           static_cast<unsigned>(intent.program), static_cast<unsigned>(intent.index), intent.value);
  return false;
}

void RainbirdComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Rain Bird %s\n"
                "  Link: not implemented yet\n"
                "  Listen only: %s\n"
                "  Valve mode: %s\n"
                "  Schedule writes: %s\n"
                "  Experimental: %s\n"
                "  Zones: %u\n"
                "  Programs: %u\n"
                "  Max drift: %u ms\n"
                "  Schedule refresh: %u ms",
                this->model_name_, YESNO(this->listen_only_), this->valve_queue_ ? "queue" : "start",
                YESNO(this->allow_writes_), YESNO(this->experimental_), static_cast<unsigned>(this->zones_.size()),
                static_cast<unsigned>(this->programs_.size()), static_cast<unsigned>(this->max_drift_ms_),
                static_cast<unsigned>(this->refresh_interval_ms_));
  LOG_UPDATE_INTERVAL(this);
#ifdef USE_TIME
  ESP_LOGCONFIG(TAG, "  Time: %s", YESNO(this->time_ != nullptr));
#endif
  if (this->monitor_receiver_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Monitor receiver: yes");
  }
  if (!this->accept_ids_.empty()) {
    ESP_LOGCONFIG(TAG, "  Extra model ids: %u", static_cast<unsigned>(this->accept_ids_.size()));
  }
  if (!this->nak_triggers_.empty() || !this->frame_triggers_.empty()) {
    ESP_LOGCONFIG(TAG, "  Triggers: %u NAK, %u frame", static_cast<unsigned>(this->nak_triggers_.size()),
                  static_cast<unsigned>(this->frame_triggers_.size()));
  }
  // The wire pointers are unused until the link exists. Read them so the fields stay live.
  if (!this->listen_only_ && this->transmitter_ == nullptr) {
    ESP_LOGE(TAG, "No transmitter");
  }
  if (this->receiver_ == nullptr) {
    ESP_LOGE(TAG, "No receiver");
  }
}

}  // namespace esphome::rainbird
