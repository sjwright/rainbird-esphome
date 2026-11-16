#include "entity_board.h"

namespace esphome::rainbird {

namespace {

// True when `value` differs from what was last published, and records it as published.
template<typename V> bool is_new(std::optional<V> &shown, const V &value) {
  if (shown == value)
    return false;
  shown = value;
  return true;
}

// Shows something the controller reports, as opposed to a local setting or the link's own state.
bool from_controller(const EntityBinding &binding) { return kind_info(binding.kind).field != NO_FIELD; }

}  // namespace

void EntityBoard::add(Domain domain, const EntityBinding &binding, void *entity) {
  if (domain == Domain::TEXT_SENSOR) {
    this->texts_.push_back(Text{binding, entity});
  } else {
    this->entries_.push_back(Entry{domain, binding, entity});
  }
}

void EntityBoard::publish(const Readout &readout, const TextContext &context, BoardSink &sink) {
  for (Entry &entry : this->entries_) {
    switch (entry.domain) {
      case Domain::SWITCH:
      case Domain::BINARY_SENSOR: {
        bool value = false;
        if (bool_value(entry.binding, readout, &value) && is_new(entry.shown, static_cast<double>(value)))
          sink.show_bool(entry.domain, entry.entity, value);
        break;
      }
      case Domain::NUMBER:
      case Domain::SENSOR: {
        float value = 0;
        if (entry.binding.kind != EntityKind::REMAINING_TIME && number_value(entry.binding, readout, &value) &&
            is_new(entry.shown, static_cast<double>(value)))
          sink.show_number(entry.domain, entry.entity, value);
        break;
      }
      case Domain::SELECT: {
        uint8_t index = 0;
        if (select_index(entry.binding, readout, &index) && is_new(entry.shown, static_cast<double>(index)))
          sink.show_option(entry.entity, index);
        break;
      }
      case Domain::TIME: {
        uint8_t hour = 0;
        uint8_t minute = 0;
        if (start_time(entry.binding, readout, &hour, &minute) &&
            is_new(entry.shown, static_cast<double>(hour * 60 + minute)))
          sink.show_time(entry.entity, hour, minute);
        break;
      }
      case Domain::DATETIME: {
        WallClock clock;
        if (entry.binding.kind == EntityKind::CONTROLLER_CLOCK && controller_clock(readout, &clock) &&
            is_new(entry.shown, static_cast<double>(wall_seconds(clock))))
          sink.show_clock(entry.entity, clock);
        break;
      }
      case Domain::BUTTON:
      case Domain::TEXT_SENSOR:
        break;
    }
  }
  for (Text &text : this->texts_) {
    std::string value;
    if (text_value(text.binding, readout, context, &value) && is_new(text.shown, value))
      sink.show_text(text.entity, value);
  }
}

void EntityBoard::link_lost(BoardSink &sink) {
  for (Entry &entry : this->entries_) {
    switch (entry.domain) {
      case Domain::NUMBER:
      case Domain::SENSOR:
      case Domain::BINARY_SENSOR:
        if (from_controller(entry.binding)) {
          entry.shown.reset();
          sink.show_unknown(entry.domain, entry.entity);
        }
        break;
      case Domain::SWITCH:
      case Domain::SELECT:
      case Domain::TIME:
      case Domain::DATETIME:
        // No way to show unknown: the last value stays, and the next reading is published.
        if (from_controller(entry.binding))
          entry.shown.reset();
        break;
      case Domain::BUTTON:
      case Domain::TEXT_SENSOR:
        break;
    }
  }
  for (Text &text : this->texts_) {
    if (from_controller(text.binding))
      text.shown.reset();
  }
}

void EntityBoard::forget(void *entity) {
  for (Entry &entry : this->entries_) {
    if (entry.entity == entity)
      entry.shown.reset();
  }
  for (Text &text : this->texts_) {
    if (text.entity == entity)
      text.shown.reset();
  }
}

}  // namespace esphome::rainbird
