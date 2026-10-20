#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "controller_state.h"
#include "link_core.h"
#include "model.h"
#include "sip_commands.h"

namespace esphome::rainbird {

// Everything the hub does with the controller apart from ESPHome itself
// (PLAN 6.3, 6.4): startup, capability probes, the poll plan, the model gate
// and diagnostics. Replies are applied to ControllerState here; the hub only
// sends what next_transmission() returns and publishes what changed.

struct SessionConfig {
  bool listen_only{false};
  // False with `update_interval: never`: nothing is sent except send_raw.
  bool polling{true};
  uint32_t update_interval_ms{15000};
  uint32_t fast_interval_ms{3000};
  uint32_t fast_hold_ms{30000};  // fast polling after a command
  uint32_t refresh_interval_ms{3600000};
  uint32_t silence_ms{60000};          // listen-only: no frame for this long is "not connected"
  FieldSet used{};                     // fields that configured entities read
  uint16_t zone_mask{0};               // zones that read schedule pages, 0 = all
  uint8_t program_mask{0};             // programs that read schedule pages, 0 = all
  std::vector<uint16_t> accept_ids{};  // accept_model_ids
  LinkTiming timing{};
};

struct Identity {
  bool model_known{false};
  bool firmware_known{false};
  bool serial_known{false};
  ModelVersion model{};
  FirmwareVersion firmware{};
  SerialNumber serial{};
};

enum class SessionStage : uint8_t {
  PASSIVE,   // listen_only: decode only
  MANUAL,    // polling off: only send_raw transmits
  IDLE,      // not identified yet, or identification failed; the next poll retries
  IDENTIFY,  // 02 in flight
  PROBE,     // 0B, 05, 03 and the 04 probes
  SCHEDULE,  // first read of the schedule pages in use
  RUNNING,
};

// Request tags. The high byte says why the request was sent.
enum class TagKind : uint8_t { IDENTIFY = 1, INFO = 2, PROBE = 3, SCHEDULE = 4, POLL = 5, RAW = 6 };

class Session {
 public:
  Session(const Model &model, const SessionConfig &config);

  void start(uint32_t now_ms);
  // PollingComponent::update(). Enqueues a poll cycle unless one is pending.
  void poll(uint32_t now_ms);
  // Timeouts and completions. Call every loop before next_transmission().
  void tick(uint32_t now_ms);
  bool next_transmission(uint32_t now_ms, Transmission *out);
  // Every valid frame from the controller's receiver.
  void on_frame(const uint8_t *payload, size_t size, uint32_t now_ms);
  // Experimental raw send. The experimental gate is the hub's; this only queues it.
  bool send_raw(const uint8_t *payload, size_t size, uint32_t now_ms);

  // Fast while watering or just after a command, otherwise update_interval.
  uint32_t desired_interval(uint32_t now_ms) const;
  // True while anything is queued, in flight or waiting to be handled.
  bool busy() const { return !this->link_.idle(); }

  SessionStage stage() const { return this->stage_; }
  ControllerState &state() { return this->state_; }
  const ControllerState &state() const { return this->state_; }
  const Identity &identity() const { return this->identity_; }
  const Capabilities &capabilities() const { return this->caps_; }
  // D12: control and write commands are allowed only for an accepted model id.
  bool writes_permitted() const { return this->writes_permitted_; }

  bool connected_known() const { return this->connected_known_; }
  bool connected() const { return this->connected_; }
  bool response_known() const { return this->response_known_; }
  uint32_t response_ms() const { return this->response_ms_; }
  uint32_t link_errors() const { return this->link_.link_errors(); }
  uint32_t duplicates() const { return this->link_.duplicates(); }

  // Change flags for values outside ControllerState. The hub clears them after publishing.
  bool identity_changed() const { return this->identity_changed_; }
  bool diagnostics_changed() const { return this->diagnostics_changed_; }
  void clear_changed();

 private:
  void enqueue(const ByteBuf &request, Priority priority, TagKind kind, uint8_t detail = 0);
  void enqueue_list(const RequestList &list, Priority priority, TagKind kind, size_t *count = nullptr);
  void identify();
  void handle(const Completion &done, uint32_t now_ms);
  void finish_probes(uint32_t now_ms);
  void begin_running(uint32_t now_ms);
  bool schedule_due(uint32_t now_ms) const;
  void read_schedule(uint32_t now_ms, Priority priority, size_t *count);
  void note_unsupported(uint8_t opcode);
  void set_connected(bool connected);
  bool watering() const;

  const Model &model_;
  SessionConfig config_;
  LinkCore link_;
  ControllerState state_{};
  Identity identity_{};
  Capabilities caps_{};
  SessionStage stage_{SessionStage::IDLE};

  bool writes_permitted_{false};
  size_t probes_pending_{0};
  bool probe_answered_{false};
  size_t schedule_pending_{0};
  uint32_t poll_count_{0};
  uint32_t last_schedule_read_{0};
  bool schedule_read_once_{false};
  bool schedule_stale_{false};
  bool event_known_{false};
  uint32_t event_value_{0};
  uint32_t hold_from_{0};
  bool hold_{false};

  bool connected_known_{false};
  bool connected_{false};
  bool response_known_{false};
  uint32_t response_ms_{0};
  uint32_t last_link_errors_{0};
  uint32_t last_frame_at_{0};
  bool frame_seen_{false};
  bool identity_changed_{false};
  bool diagnostics_changed_{false};
};

}  // namespace esphome::rainbird
