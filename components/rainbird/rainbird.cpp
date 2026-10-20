#include "rainbird.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "log_tag.h"
#include "sip_commands.h"

namespace esphome::rainbird {

ESPHOME_LOG_TAG(TAG, "rainbird");

void RainbirdComponent::setup() {
  // The link enables loop() while a request is queued. Nothing is queued yet.
  // Received frames are decoded from the receiver's own loop, so this stays off.
  this->disable_loop();
}

bool RainbirdReceiver::on_receive(remote_base::RemoteReceiveData data) {
  if (this->parent_ == nullptr) {
    return false;
  }
  const remote_base::RawTimings &raw = data.get_raw_data();
  bool was_locked = this->decoder_.lock().locked();
  BurstDecode decoded = this->decoder_.decode(raw.data(), raw.size());
  const PolarityLock &lock = this->decoder_.lock();
  if (!was_locked && lock.locked()) {
    ESP_LOGD(TAG, "%s polarity locked: %s, %s", this->name_,
             lock.polarity() == Polarity::INVERTED ? "inverted" : "normal",
             lock.phase() == Phase::EXTRA_HALF ? "extra half-bit" : "aligned");
  } else if (was_locked && !lock.locked()) {
    ESP_LOGD(TAG, "%s polarity unlocked", this->name_);
  }
  if (decoded.frames.empty()) {
    return false;
  }
  for (const Frame &frame : decoded.frames) {
    this->parent_->deliver_frame_(frame, this->name_);
  }
  return true;
}

void RainbirdComponent::deliver_frame_(const Frame &frame, const char *which) {
  char pretty[format_hex_pretty_size(MAX_PAYLOAD)];
  ESP_LOGV(TAG, "%s %s", which, format_hex_pretty_to(pretty, frame.payload, frame.length));
  std::vector<uint8_t> payload(frame.payload, frame.payload + frame.length);
  for (FrameTrigger *trigger : this->frame_triggers_) {
    trigger->trigger(payload);
  }
  Nak nak;
  if (parse_nak(frame.payload, frame.length, &nak)) {
    for (NakTrigger *trigger : this->nak_triggers_) {
      trigger->trigger(nak.echo, nak.code);
    }
  }
}

bool RainbirdComponent::send_raw(const uint8_t *data, size_t size) {
  if (!this->experimental_) {
    ESP_LOGW(TAG, "send_raw needs experimental: true");
    return false;
  }
  if (this->transmitter_ == nullptr) {
    ESP_LOGW(TAG, "send_raw needs a transmitter");
    return false;
  }
  if (size > MAX_PAYLOAD) {
    ESP_LOGW(TAG, "send_raw payload is %u bytes; the maximum is %u", static_cast<unsigned>(size),
             static_cast<unsigned>(MAX_PAYLOAD));
    return false;
  }
  uint8_t frame[MAX_FRAME_BYTES];
  size_t framed = encode_frame(data, size, false, frame, sizeof(frame));
  if (framed == 0) {
    return false;
  }
  // The transmitter pin's inverted: flag is the electrical polarity. Bytes stay normal.
  std::vector<int32_t> timings = manchester_encode(frame, framed, Polarity::NORMAL, Phase::ALIGNED);
  auto call = this->transmitter_->transmit();
  remote_base::RemoteTransmitData *tx = call.get_data();
  tx->set_carrier_frequency(0);
  tx->set_data(timings);
  call.perform();
  return true;
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
                "  Receive: listening\n"
                "  Commands: not implemented yet\n"
                "  Listen only: %s\n"
                "  Valve mode: %s\n"
                "  Schedule writes: %s\n"
                "  Experimental: %s\n"
                "  Zones: %u\n"
                "  Programs: %u\n"
                "  Schedule refresh: %u ms",
                this->model_name_, YESNO(this->listen_only_), this->valve_queue_ ? "queue" : "start",
                YESNO(this->allow_writes_), YESNO(this->experimental_), static_cast<unsigned>(this->zones_.size()),
                static_cast<unsigned>(this->programs_.size()), static_cast<unsigned>(this->refresh_interval_ms_));
  if (this->clock_sync_) {
    ESP_LOGCONFIG(TAG, "  Clock sync: max drift %u ms", static_cast<unsigned>(this->max_drift_ms_));
  } else {
    ESP_LOGCONFIG(TAG, "  Clock sync: off");
  }
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
  // send_raw uses the transmitter. The receivers are registered separately, in to_code.
  if (!this->listen_only_ && this->transmitter_ == nullptr) {
    ESP_LOGE(TAG, "No transmitter");
  }
  if (this->receiver_ == nullptr) {
    ESP_LOGE(TAG, "No receiver");
  }
}

}  // namespace esphome::rainbird
