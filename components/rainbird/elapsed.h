#pragma once

#include <cstdint>

namespace esphome::rainbird {

// True once `delay` ms have passed since `from`. Unsigned arithmetic stays right across the
// 49-day millis() wrap and after any idle spell, so every wait in this component is written this way.
inline bool elapsed(uint32_t now, uint32_t from, uint32_t delay) { return now - from >= delay; }

}  // namespace esphome::rainbird
