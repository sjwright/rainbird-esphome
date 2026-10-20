#pragma once

#include <cstdint>
#include <string>
#include <vector>

inline std::vector<uint8_t> from_hex(const char *text) {
  std::vector<uint8_t> out;
  int high = -1;
  for (const char *p = text; *p != '\0'; p++) {
    char c = *p;
    int nibble;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      nibble = c - 'A' + 10;
    } else {
      continue;
    }
    if (high < 0) {
      high = nibble;
    } else {
      out.push_back(static_cast<uint8_t>((high << 4) | nibble));
      high = -1;
    }
  }
  return out;
}

inline std::string to_hex(const uint8_t *data, size_t size) {
  static const char *digits = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < size; i++) {
    if (i != 0) {
      out.push_back(' ');
    }
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0F]);
  }
  return out;
}
