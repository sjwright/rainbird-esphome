#pragma once

#include <cstddef>
#include <cstdint>

#include "controller_state.h"
#include "link_core.h"
#include "model_esp_tm2.h"
#include "sip_commands.h"

namespace esphome::rainbird {

// The bytes of one schedule write, for the log line that goes out before it.
struct PageEditLog {
  uint8_t page{0};
  uint8_t size{0};
  uint8_t before[MAX_PAGE_BYTES]{};
  uint8_t after[MAX_PAGE_BYTES]{};
};

// One schedule edit at a time (PLAN 6.5): read the page, change one field, write the page
// back, read it again. Every step works from the reply it is given, never from a page kept
// earlier. Pure: the session queues each request this asks for and reports the outcome.
class ScheduleEditor {
 public:
  explicit ScheduleEditor(const EspTm2Model &model) : model_(model) {}

  // What on_reply() asks for next.
  enum class Step : uint8_t {
    WRITE,    // queue `next`, the page write, once. write_log() has its bytes.
    CONFIRM,  // queue `next`, the read back. The write was acknowledged.
    DONE,     // the read back matches what was written
    FAILED,   // reason() says why
  };

  bool active() const { return this->active_; }
  uint8_t page() const { return this->page_; }
  const char *reason() const { return this->reason_; }
  const PageEditLog &write_log() const { return this->log_; }

  // Starts an edit, and fills `read` with the page read to queue. False when the field is not
  // on a page or the binding is out of range.
  bool start(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value, ByteBuf *read);
  // The completion of the request this editor asked for last.
  Step on_reply(const Completion &done, ByteBuf *next);
  // Ends the edit without a reply: a step the queue refused, or a lost link.
  void cancel() { this->active_ = false; }

 private:
  enum class Phase : uint8_t { READING, WRITING, CONFIRMING };

  Step fail_(const char *reason);

  const EspTm2Model &model_;
  bool active_{false};
  Phase phase_{Phase::READING};
  Field field_{Field::RUN_TIME};
  uint8_t zone_{0};
  uint8_t program_{0};
  uint8_t index_{0};
  int32_t value_{0};
  uint8_t page_{0};
  const char *reason_{nullptr};
  PageEditLog log_{};
};

}  // namespace esphome::rainbird
