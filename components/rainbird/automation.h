#pragma once

#include "esphome/core/automation.h"

#include <cstdint>
#include <vector>

namespace esphome::rainbird {

// Trigger variable names match the YAML lambdas: command, code, payload.
class NakTrigger : public Trigger<uint8_t, uint8_t> {};
class FrameTrigger : public Trigger<std::vector<uint8_t>> {};

}  // namespace esphome::rainbird
