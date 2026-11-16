#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "clock_sync.h"
#include "entity_values.h"

namespace esphome::rainbird {

// Every entity on a hub, and what each one shows (PLAN 5.3, 6.5): which values to publish
// after a reading, and what a lost link does to each. Pure, so these rules run in host tests.
// The hub registers its entities here and carries out what the board tells it through a sink.

// The kind of ESPHome entity, which decides how a value is read and whether it can show unknown.
enum class Domain : uint8_t {
  SWITCH,
  NUMBER,
  BUTTON,  // shows nothing, but its binding still adds to the poll plan
  SELECT,
  TIME,
  DATETIME,
  SENSOR,
  BINARY_SENSOR,
  TEXT_SENSOR,
};

// What the board asks the hub to do. `entity` is the pointer the entity was added with, handed
// back untouched: the hub casts it to the class that domain uses.
class BoardSink {
 public:
  virtual void show_bool(Domain domain, void *entity, bool value) = 0;     // a switch or binary sensor
  virtual void show_number(Domain domain, void *entity, float value) = 0;  // a number or sensor
  virtual void show_option(void *entity, uint8_t index) = 0;               // a select
  virtual void show_time(void *entity, uint8_t hour, uint8_t minute) = 0;
  virtual void show_clock(void *entity, const WallClock &clock) = 0;  // a datetime
  virtual void show_text(void *entity, const std::string &text) = 0;  // a text sensor
  // A number or sensor as NaN, a binary sensor with no state. Never called for another domain.
  virtual void show_unknown(Domain domain, void *entity) = 0;

 protected:
  ~BoardSink() = default;
};

class EntityBoard {
 public:
  void add(Domain domain, const EntityBinding &binding, void *entity);

  // Every entity's binding, for the poll plan.
  template<typename F> void for_each_binding(F &&visit) const {
    for (const Entry &entry : this->entries_)
      visit(entry.binding);
    for (const Text &text : this->texts_)
      visit(text.binding);
  }
  // The entities of one domain and kind, as the pointers they were added with.
  template<typename F> void for_each(Domain domain, EntityKind kind, F &&visit) const {
    for (const Entry &entry : this->entries_) {
      if (entry.domain == domain && entry.binding.kind == kind)
        visit(entry.entity);
    }
    if (domain != Domain::TEXT_SENSOR)
      return;
    for (const Text &text : this->texts_) {
      if (text.binding.kind == kind)
        visit(text.entity);
    }
  }

  // Shows every value that is known and differs from what its entity shows. Nothing is shown
  // until the controller says it. The remaining-time sensor is left out: it counts down between
  // readings, which the hub's timer does (countdown.h).
  void publish(const Readout &readout, const TextContext &context, BoardSink &sink);

  // The link is down and the session has dropped what it read, so nothing here is known to be
  // true any more. An entity that can show unknown does: a sensor or number as NaN, a binary
  // sensor with no state. Switches, selects, times and text sensors have no such state and keep
  // their last value; `connected` says how far to trust them. Either way the first reading
  // after the link returns is published, even one equal to the old value. Local settings and
  // the link's own diagnostics (connected, response time, link errors) are left as they are.
  void link_lost(BoardSink &sink);

  // The entity now shows something the board did not publish (a time kept on the device for an
  // off start slot), so the next value from the controller is published whatever it is.
  void forget(void *entity);

 private:
  struct Entry {
    Domain domain;
    EntityBinding binding;
    void *entity;
    std::optional<double> shown{};  // the value last published, so an unchanged one is not sent again
  };
  struct Text {
    EntityBinding binding;
    void *entity;
    std::optional<std::string> shown{};
  };

  std::vector<Entry> entries_{};
  std::vector<Text> texts_{};
};

}  // namespace esphome::rainbird
