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

// One change to one schedule field.
struct PageChange {
  Field field{Field::RUN_TIME};
  uint8_t zone{0};
  uint8_t program{0};
  uint8_t index{0};
  int32_t value{0};

  bool same_field(const PageChange &other) const {
    return this->field == other.field && this->zone == other.zone && this->program == other.program &&
           this->index == other.index;
  }
};

// One schedule page at a time (PLAN 6.5): read the page, change the fields asked for, write
// the page back, read it again. Changes to the same page go out as one write, and a page that
// already holds every value is not written at all. Every step works from the reply it is
// given, never from a page kept earlier. Changes for another page wait their turn here. Pure:
// the session queues each request this asks for and reports the outcome.
class ScheduleEditor {
 public:
  // More than the fields any one page holds (ten on a program page), so a page's changes always fit.
  static constexpr size_t MAX_CHANGES = 12;
  // Changes waiting for the page in flight, one per field. A TM2 has 92 schedule fields
  // (2 global, 10 and 8 per program, 36 run times), so this never fills.
  static constexpr size_t MAX_WAITING = 96;

  explicit ScheduleEditor(const EspTm2Model &model) : model_(model) {}

  // What on_reply() asks for next.
  enum class Step : uint8_t {
    WRITE,      // queue `next`, the page write, once. write_log() has its bytes.
    CONFIRM,    // queue `next`, the read back. The write was acknowledged.
    DONE,       // the read back matches what was written
    UNCHANGED,  // the page already holds every value asked for. Nothing was written.
    FAILED,     // reason() says why
  };

  // What ask() did with a change.
  enum class Asked : uint8_t {
    READ,     // nothing was in flight: its page edit started. Queue `read`.
    JOINED,   // its page is being read: it shares that read and that write
    WAITING,  // another page is in flight, or its page was already read: it waits its turn
    FULL,     // no room left to wait
    RANGE,    // the field is not on a page, or the binding is out of range
  };

  // True while a page is in flight. Nothing waits unless one is.
  bool active() const { return this->active_; }
  size_t waiting() const { return this->waiting_count_; }
  uint8_t page() const { return this->page_; }
  const char *reason() const { return this->reason_; }
  const PageEditLog &write_log() const { return this->log_; }
  // How many of the changes the page would not take (a value its field cannot hold, or turning
  // on a start slot that has no time). Known once the read is answered; the rest are still written.
  size_t refused() const { return this->refused_; }

  // One change, wherever it belongs: it starts a page, joins the one being read, or waits. A
  // later value for a field already waiting replaces the earlier one and goes to the back: it
  // is the latest thing asked for. Each page is read afresh, so two changes to one page that
  // go out in turn do not undo each other.
  Asked ask(const PageChange &change, ByteBuf *read);
  // Once the page in flight is done, failed or cancelled: starts the oldest page waiting and
  // fills `read`. Every other change waiting for that page shares the read and the write, in
  // the order asked. False when nothing waits.
  bool next(ByteBuf *read);
  // Forgets every change waiting. The page in flight, if any, carries on.
  void drop_waiting() { this->waiting_count_ = 0; }

  // Starts an edit, and fills `read` with the page read to queue. False when the field is not
  // on a page or the binding is out of range. ask() uses this; it leaves the waiting changes alone.
  bool start(const PageChange &change, ByteBuf *read);
  bool start(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value, ByteBuf *read) {
    return this->start(PageChange{field, zone, program, index, value}, read);
  }
  // Another change for the page being edited, taken while its read is still out. Changes apply
  // in the order asked, and asking for a field again moves it to the end with the later value.
  // False when it is for another page or the read has been answered: that one waits its turn.
  bool add(const PageChange &change);
  // The completion of the request this editor asked for last.
  Step on_reply(const Completion &done, ByteBuf *next);
  // Ends the page in flight without a reply: a step the queue refused, or a lost link. What
  // waits stays waiting, for next().
  void cancel() { this->active_ = false; }

 private:
  enum class Phase : uint8_t { READING, WRITING, CONFIRMING };

  Step fail_(const char *reason);
  void remove_waiting_(size_t index);

  const EspTm2Model &model_;
  bool active_{false};
  Phase phase_{Phase::READING};
  PageChange changes_[MAX_CHANGES]{};
  size_t count_{0};
  size_t refused_{0};
  uint8_t page_{0};
  const char *reason_{nullptr};
  PageEditLog log_{};
  PageChange waiting_[MAX_WAITING]{};  // oldest first
  size_t waiting_count_{0};
};

}  // namespace esphome::rainbird
