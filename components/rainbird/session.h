#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "controller_state.h"
#include "link_core.h"
#include "model.h"
#include "schedule_editor.h"
#include "sip_commands.h"

namespace esphome::rainbird {

// Everything the hub does with the controller apart from ESPHome itself
// (PLAN 6.3, 6.4): startup, capability probes, the poll plan, the model gate,
// commands, schedule edits and diagnostics. Replies are applied to ControllerState
// here; the hub sends what next_transmission() returns, publishes what changed and
// reports what take_event() hands over.

struct SessionConfig {
  bool listen_only{false};
  // False with `update_interval: never`: nothing is sent except send_raw.
  bool polling{true};
  bool experimental{false};  // 42, 3A, 49 and send_raw
  // Valve switches queue their zone (4B) instead of starting it (39), and turning one off
  // moves on to the next queued zone (42, with experimental) instead of stopping everything.
  bool valve_queue{false};
  bool allow_writes{false};  // schedule page writes (21)
  uint32_t update_interval_ms{15000};
  uint32_t fast_interval_ms{3000};
  uint32_t fast_hold_ms{30000};  // fast polling after a command
  uint32_t refresh_interval_ms{3600000};
  uint32_t silence_ms{60000};            // listen-only: no frame for this long is "not connected"
  uint32_t reidentify_after_ms{300000};  // link down this long: start again at identification
  FieldSet used{};                       // fields that configured entities show
  PageSet pages{};                       // schedule pages that configured entities show
  std::vector<uint16_t> accept_ids{};    // accept_model_ids
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

// What the link has seen of the controller, for the diagnostic entities.
struct LinkStatus {
  bool connected_known{false};
  bool connected{false};
  bool response_known{false};
  uint32_t response_ms{0};  // the last reply, from the end of the first request copy
  uint32_t errors{0};       // requests that timed out, retries included
  // Counted for the log, not published. Duplicates answer SPEC question 5: does the
  // controller answer both copies of a 2-byte request?
  uint32_t replies{0};
  uint32_t duplicates{0};
  uint32_t unexpected{0};
};

// Which LinkStatus values changed since clear_changed(). Each entity republishes only for its own:
// a reply that leaves the error count alone does not republish it.
struct LinkChanges {
  bool connected{false};
  bool response{false};
  bool errors{false};
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
enum class TagKind : uint8_t {
  IDENTIFY = 1,
  INFO = 2,
  PROBE = 3,
  SCHEDULE = 4,
  POLL = 5,
  RAW = 6,
  CONTROL = 7,  // a command that may already have been acted on; sent once. The detail is its program.
  PAGE = 8,     // one schedule edit: read, write, read back
};

// A control command. Minutes, percent and days share `value`. Clock fields are separate.
enum class ControlOp : uint8_t {
  RUN_ZONE,    // 39: start now
  QUEUE_ZONE,  // 4B: run after whatever is running
  VALVE_ON,    // a valve switch: QUEUE_ZONE with valve_queue, otherwise RUN_ZONE
  VALVE_OFF,   // a valve switch: stop, or advance past it with valve_queue and experimental
  RUN_PROGRAM,
  STOP,
  ADVANCE,
  SET_SEASONAL,
  SET_RAIN_DELAY,
  SET_IRRIGATION,
  TEST_ZONES,
  SET_CLOCK,  // 11 then 13: both are queued or neither is
};

struct Control {
  ControlOp op{ControlOp::STOP};
  uint8_t zone{0};
  uint8_t program{0};
  uint16_t value{0};
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t second{0};
  uint8_t day{0};
  uint8_t month{0};
  uint16_t year{0};
};

enum class ControlResult : uint8_t {
  QUEUED,
  REFUSED_NOT_READY,     // not identified, model refused, listen-only, or writes off
  REFUSED_LINK_DOWN,     // the controller is not answering
  REFUSED_EXPERIMENTAL,  // 42, 3A, 49 and send_raw
  REFUSED_UNSUPPORTED,   // the 04 probe said no
  REFUSED_RANGE,
  REFUSED_NO_ZONE,      // 03 00 says the controller does not have that zone
  REFUSED_NOT_RUNNING,  // valve off, and this zone is not the one running
  REFUSED_QUEUED_ONLY,  // queued, and there is no "remove one zone" command
  REFUSED_BUSY,         // a schedule edit is already in flight, or the queue is full
};

const char *control_result_text(ControlResult result);

// Something for the hub to report. take_event() hands each over once, in order.
enum class SessionEventKind : uint8_t {
  STAGE,        // stage() changed; `stage` is the new one
  LINK_UP,      // the first reply, or the first after the link was down
  LINK_DOWN,    // three requests in a row failed, or listen-only heard nothing for silence_ms
  NAK,          // the controller refused `opcode` with `code`
  PAGE_WRITE,   // a schedule page write is queued and page_write() has its bytes. Log it before it goes.
  EDIT_FAILED,  // a schedule edit stopped; `reason` says why
};

struct SessionEvent {
  SessionEventKind kind{SessionEventKind::STAGE};
  SessionStage stage{SessionStage::IDLE};
  uint8_t opcode{0};
  uint8_t code{0};
  const char *reason{nullptr};  // a static string
};

class Session {
 public:
  Session(const Model &model, const SessionConfig &config);

  void start(uint32_t now_ms);
  // PollingComponent::update(). Enqueues a poll cycle unless one is pending.
  void poll(uint32_t now_ms);
  // The refresh button: everything read again, status, slow group and schedule pages, with
  // the next poll cycle. That is now, unless one is still running. Identification again if that failed.
  ControlResult refresh(uint32_t now_ms);
  // listen_only: no frame for silence_ms marks the controller not connected. The hub calls
  // this on its own timer, because with `update_interval: never` poll() never runs.
  void check_silence(uint32_t now_ms);
  // Timeouts and completions. Call every loop before next_transmission().
  void tick(uint32_t now_ms);
  bool next_transmission(uint32_t now_ms, Transmission *out);
  // Every valid frame from the controller's receiver.
  void on_frame(const uint8_t *payload, size_t size, uint32_t now_ms);
  // Experimental: any payload, never retried. Needs experimental.
  ControlResult send_raw(const uint8_t *payload, size_t size, uint32_t now_ms);
  // One control command, sent once. A lost reply must not run it again.
  ControlResult control(const Control &command, uint32_t now_ms);
  // Read the page, change one field, write it back, read it again. One at a time.
  ControlResult edit_schedule(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value,
                              uint32_t now_ms);

  // The next event, oldest first. False when there are none.
  bool take_event(SessionEvent *out);
  // The bytes of the latest schedule write, for a PAGE_WRITE event.
  const PageEditLog &page_write() const { return this->editor_.write_log(); }

  // Fast while watering or just after a command, otherwise update_interval.
  uint32_t desired_interval(uint32_t now_ms) const;
  // True while anything is queued, in flight or waiting to be handled.
  bool busy() const { return !this->link_.idle(); }

  SessionStage stage() const { return this->stage_; }
  const ControllerState &state() const { return this->state_; }
  const Identity &identity() const { return this->identity_; }
  const LinkStatus &link() const { return this->link_status_; }
  const Capabilities &capabilities() const { return this->caps_; }
  // True when 04 was refused, so every optional command is assumed until a NAK says otherwise.
  bool capabilities_assumed() const { return this->caps_assumed_; }
  // The schedule pages read, for the log.
  const PageSet &pages() const { return this->config_.pages; }
  // D12: control and write commands are allowed only for an accepted model id.
  bool writes_permitted() const { return this->writes_permitted_; }

  // Changes with every complete clock reading (4C, or 10 then 12), so the caller can act once
  // per reading. 0 until the first. The values are in state(); clock_read_at() is when it arrived.
  uint32_t clock_serial() const { return this->clock_serial_; }
  uint32_t clock_read_at() const { return this->clock_read_at_; }

  // Change flags for identity() and link(). ControllerState keeps its own per field.
  bool identity_changed() const { return this->identity_changed_; }
  const LinkChanges &link_changes() const { return this->link_changes_; }
  // True when anything may need publishing: a reply was applied, or identity() or link() changed.
  bool anything_changed() const;
  // After publishing: clears these and every ControllerState cell.
  void clear_changed();

 private:
  bool enqueue_(const ByteBuf &request, Priority priority, TagKind kind, uint8_t detail = 0,
                Delivery delivery = Delivery::NORMAL);
  void enqueue_list_(const RequestList &list, Priority priority, TagKind kind, size_t *count = nullptr);
  void push_event_(const SessionEvent &event);
  void set_stage_(SessionStage stage);
  void set_connected_(bool connected, uint32_t now_ms);
  void identify_();
  void restart_();
  void handle_(const Completion &done, uint32_t now_ms);
  void finish_probes_(uint32_t now_ms);
  void begin_running_(uint32_t now_ms);
  bool schedule_due_(uint32_t now_ms) const;
  void read_schedule_(uint32_t now_ms, Priority priority, size_t *count);
  void note_unsupported_(uint8_t opcode);
  void note_clock_reading_(uint32_t at_ms);
  bool watering_() const;
  bool ready_for_command_() const;
  bool link_down_() const;
  ControlResult unsupported_() const;
  bool enqueue_command_(const ByteBuf &request, uint32_t now_ms, uint8_t program = 0);
  void follow_up_(uint32_t now_ms);
  bool next_queued_zone_(uint8_t skip, uint8_t *zone) const;
  bool zone_missing_(uint8_t zone) const;
  void fail_edit_(const char *reason);
  void handle_page_(const Completion &done);

  const Model &model_;
  SessionConfig config_;
  LinkCore link_;
  ControllerState state_{};
  Identity identity_{};
  LinkStatus link_status_{};
  Capabilities caps_{};
  SessionStage stage_{SessionStage::IDLE};

  bool writes_permitted_{false};
  size_t probes_pending_{0};
  bool probe_answered_{false};  // some 04 got an 84 reply
  bool probe_refused_{false};   // some 04 got a NAK
  bool caps_assumed_{false};
  bool probes_done_{false};
  bool state_touched_{false};  // a reply was applied since clear_changed()
  size_t schedule_pending_{0};
  uint32_t poll_count_{0};
  uint32_t last_schedule_read_{0};
  bool schedule_read_once_{false};
  bool schedule_stale_{false};
  bool event_known_{false};
  uint32_t event_value_{0};
  uint32_t hold_from_{0};
  bool hold_{false};

  uint32_t down_since_{0};
  uint32_t last_frame_at_{0};
  bool frame_seen_{false};
  bool identity_changed_{false};
  LinkChanges link_changes_{};
  uint32_t clock_serial_{0};
  uint32_t clock_read_at_{0};
  bool time_pending_{false};  // a 10 reply waiting for its 12
  uint32_t time_at_{0};

  static constexpr size_t EVENT_CAP = 8;
  SessionEvent events_[EVENT_CAP]{};
  size_t event_count_{0};

  ScheduleEditor editor_;
};

}  // namespace esphome::rainbird
