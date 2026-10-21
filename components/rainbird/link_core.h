#pragma once

#include <cstddef>
#include <cstdint>

#include "sip_commands.h"
#include "sip_frame.h"

namespace esphome::rainbird {

// The transport (PLAN 6.3), without ESPHome: one request in flight, a priority
// queue, timeouts from the end of the first copy, retries, reply matching and
// duplicate-reply counting. The hub passes the time in and sends whatever
// next_transmission() returns, so all of this runs in host tests.

enum class Priority : uint8_t { USER = 0, FOLLOW_UP = 1, POLL = 2 };

struct LinkTiming {
  uint32_t timeout_ms{400};  // after the end of the first copy (SPEC 2.3: reply ~30 ms later)
  uint8_t retries{2};        // extra sends after a timeout, for Delivery::NORMAL. A NAK is not retried.
  uint32_t gap_ms{20};       // quiet time between transactions
  bool repeat_short{true};   // send 2-byte requests twice, back to back, as the LNK2 does
};

// How one request goes out.
enum class Delivery : uint8_t {
  // Retried after a timeout. A 2-byte request goes as two back-to-back copies, as the LNK2 sends it.
  NORMAL,
  // The same copies, never retried. send_raw: the bytes go out as the LNK2 would send them, once.
  NO_RETRY,
  // One copy, never retried. Commands and page writes: the controller may act on every copy it
  // hears, and a lost reply does not mean it did nothing.
  ONCE,
};

// Frames to send now, both copies already concatenated with no gap.
struct Transmission {
  uint8_t bytes[2 * MAX_FRAME_BYTES]{};
  size_t size{0};
  uint32_t first_copy_ms{0};  // air time of the first copy
  uint32_t total_ms{0};       // air time of everything in bytes
};

enum class Outcome : uint8_t { REPLY, NAK, FAILED };

struct Completion {
  uint32_t tag{0};
  Outcome outcome{Outcome::FAILED};
  uint8_t opcode{0};
  Reply reply{};            // set for REPLY and NAK
  uint32_t response_ms{0};  // end of the first copy to the reply
  uint8_t attempts{0};
};

// Air time at 2400 bit/s, rounded up to whole milliseconds.
uint32_t frame_air_ms(size_t bytes);

class LinkCore {
 public:
  static constexpr size_t QUEUE_CAP = 32;
  static constexpr size_t DONE_CAP = 4;

  explicit LinkCore(const LinkTiming &timing = LinkTiming{});

  // False when the queue is full or the request is empty or too long.
  bool enqueue(const ByteBuf &request, Priority priority, uint32_t tag, Delivery delivery = Delivery::NORMAL);
  // Times out the request in flight. Call before next_transmission().
  void tick(uint32_t now_ms);
  // True when out holds frames to send now.
  bool next_transmission(uint32_t now_ms, Transmission *out);
  // Every valid frame from the controller's receiver. True if it answered the request in flight.
  bool on_frame(const uint8_t *payload, size_t size, uint32_t now_ms);
  bool pop_completion(Completion *out);

  bool in_flight() const { return this->busy_; }
  size_t queued() const { return this->count_; }
  size_t queued(Priority priority) const;
  // Nothing queued, nothing in flight, nothing waiting to be collected.
  bool idle() const { return !this->busy_ && this->count_ == 0 && this->done_count_ == 0; }

  uint32_t link_errors() const { return this->link_errors_; }
  uint32_t duplicates() const { return this->duplicates_; }
  uint32_t unexpected() const { return this->unexpected_; }
  uint8_t consecutive_failures() const { return this->consecutive_failures_; }

 private:
  struct Pending {
    ByteBuf request{};
    Priority priority{Priority::POLL};
    uint32_t tag{0};
    uint8_t attempts{0};
    Delivery delivery{Delivery::NORMAL};
  };

  bool take_next(Pending *out);
  bool push_front(const Pending &pending);
  void complete(const Completion &completion);
  void fail(const Pending &pending);

  LinkTiming timing_;
  Pending queue_[QUEUE_CAP]{};
  size_t count_{0};
  Completion done_[DONE_CAP]{};
  size_t done_count_{0};

  Pending current_{};
  bool busy_{false};
  // Every wait is "elapsed since a start time >= a delay", in unsigned arithmetic.
  // That stays right across the 49-day millis() wrap and after any idle spell.
  uint32_t sent_at_{0};
  uint32_t first_copy_ms_{0};
  uint32_t total_ms_{0};
  uint32_t ready_from_{0};
  uint32_t ready_delay_{0};

  uint8_t last_reply_[MAX_PAYLOAD]{};
  size_t last_reply_size_{0};
  uint32_t last_reply_at_{0};

  uint32_t link_errors_{0};
  uint32_t duplicates_{0};
  uint32_t unexpected_{0};
  uint8_t consecutive_failures_{0};
};

}  // namespace esphome::rainbird
