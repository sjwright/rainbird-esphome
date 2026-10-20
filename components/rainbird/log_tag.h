#pragma once

// ESPHOME_LOG_TAG arrives in 2026.10.0. Drop this once min_version includes it.
#ifndef ESPHOME_LOG_TAG
#define ESPHOME_LOG_TAG(name, tag) static const char *const name = tag
#endif
