#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::rainbird {

// Turns timestamped line edges into bursts of signed pulse widths, the form
// manchester_decode_bits takes (PLAN-GPIO 4.4). Pure, so it runs in host tests and can replay a
// logic-analyser capture. This is phase 0 of PLAN-GPIO.md: nothing in the firmware feeds it
// yet. remote_receiver still does this job on the device.
class BurstAssembler {
 public:
  // No edge for this long ends a burst. About ten bit times: far above the longest legal pulse
  // (416 µs) and far below the controller's ~30 ms turnaround. The same as `idle: 4ms` today.
  static constexpr uint32_t IDLE_US = 4000;
  // A pulse shorter than this is a glitch and is merged away. The same as `filter: 50us` today.
  static constexpr uint32_t FILTER_US = 50;
  // Room for a 64-byte payload and its framing with every bit as two half-bit pulses (1136),
  // and spare. A burst that runs past it is cut off there.
  static constexpr size_t MAX_PULSES = 2048;

  BurstAssembler() { this->open_.reserve(MAX_PULSES); }

  // One edge, in time order: the clock in microseconds and the line level after it. True when
  // this edge came after an idle gap and so closed the burst before it: take burst() before
  // the next call. The edge itself starts the next burst. The clock may wrap.
  bool add_edge(uint32_t time_us, bool level_after);
  // Call from the loop with the current time. True when the open burst has gone idle and
  // closed: take burst() before the next call.
  bool check_idle(uint32_t now_us);
  // The burst that closed last. Positive widths are high, negative low, in microseconds. It
  // ends with one idle-level sample of IDLE_US, as remote_receiver's bursts do: the line's
  // last half-bit runs into the idle with no edge between, and the decoder infers it from that.
  const std::vector<int32_t> &burst() const { return this->closed_; }

  // True while a burst is open, so the caller knows to keep calling check_idle().
  bool open() const { return this->have_edge_; }

  // Since construction, for the link statistics.
  uint32_t glitches() const { return this->glitches_; }    // pulses under FILTER_US merged away
  uint32_t overflows() const { return this->overflows_; }  // bursts cut off at MAX_PULSES

 private:
  void close_();

  std::vector<int32_t> open_{};
  std::vector<int32_t> closed_{};
  bool have_edge_{false};  // a burst is open: last_time_ and level_ are its latest edge
  bool overflowed_{false};
  uint32_t last_time_{0};
  bool level_{false};  // the line level since last_time_
  uint32_t glitches_{0};
  uint32_t overflows_{0};
};

}  // namespace esphome::rainbird
