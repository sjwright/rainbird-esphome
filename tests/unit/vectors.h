#pragma once

// Payloads from SPEC.md, without the F5/1C frame. `reply` is the controller direction.
struct SpecVector {
  const char *name;
  const char *hex;
  bool reply;
};

inline const SpecVector SPEC_VECTORS[] = {
    {"run zone 1 for 6 min", "39 00 01 06", false},
    {"stop", "40", false},
    {"ack of stop", "01 40", true},
    {"ack of run zone", "01 39", true},
    {"model 0006 protocol 9.12", "82 00 06 09 0C", true},
    {"stations 1-6", "83 00 3F 00 00 00", true},
    {"command 82 supported", "84 82 01", true},
    {"serial", "85 00 00 00 00 00 00 89 63", true},
    {"firmware 1.47.0", "8B 01 2F 00 00", true},
    {"time 12:10:40", "90 0C 0A 28", true},
    {"set time 22:47:19", "11 16 2F 13", false},
    {"date 19/10/2026", "92 13 A7 EA", true},
    {"set date 9 Jan 2023", "13 09 17 E7", false},
    {"zone 1 active", "BF 00 01 00 00 00", true},
    {"combined state", "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00", true},
    {"queue idle", "BB 00 00 00 00 00 00 00 00 FF 00 00", true},
    {"queue running", "BB 00 05 00 05 D0 00 00 01 00 00 01", true},
    {"queue list", "BB 01 21 05 D0 22 04 B0 23 01 A4 24 04 B0 25 02 58 00", true},
    {"irrigation state 1", "C8 01", true},
    {"schedule timestamp", "CA 01 2B 48 31 63", true},
    {"nak", "00 02 03", true},
    {"rain delay 3 days", "B6 00 03", true},
    {"water budget 100", "B0 00 00 64", true},
    {"set water budget 80", "31 00 00 50", false},
    {"set rain delay 15", "37 00 0F", false},
    {"queue zone 1 for 1 min", "4B 00 01 01", false},
    {"global page", "A0 00 00 00 00 04 00", true},
    {"program A page", "A0 00 10 6A 06 01 00 64 01", true},
    {"start times 04:00", "A0 00 60 00 F0 FF FF FF FF FF FF", true},
    {"run times page 80", "A0 00 80 00 19 00 01 00 00 00 14 00 02 00 00", true},
    // Cited by SPEC 5.4 as the pyrainbird advance_zone test value.
    {"advance zone 8", "42 08", false},
};

inline constexpr int SPEC_VECTOR_COUNT = sizeof(SPEC_VECTORS) / sizeof(SPEC_VECTORS[0]);
