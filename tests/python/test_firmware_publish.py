"""Exercise the firmware's publication method without the numeric-sensor domain."""

from pathlib import Path
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]
CXX = shutil.which("c++")


@pytest.mark.skipif(CXX is None, reason="a C++ compiler is not installed")
def test_ends_at_updates_without_numeric_sensors(tmp_path: Path):
    # Compile the actual method so an accidental USE_SENSOR guard fails at runtime.
    # The harness substitutes only the session and the ESPHome publication callback.
    source = (REPO / "components/rainbird/rainbird.cpp").read_text()
    start = source.index("void RainbirdComponent::publish_remaining_()")
    end = source.index("void RainbirdComponent::tick_countdown_()", start)
    method = source[start:end]
    harness = """\
#include <cassert>
#include "controller_state.h"
using namespace esphome::rainbird;

struct FakeSession {
  ControllerState value;
  const ControllerState &state() const { return value; }
};

struct RainbirdComponent {
  FakeSession *session_;
  uint32_t remaining_readings_{0};
  unsigned publications{0};
  uint16_t last_seconds{0};
  void publish_remaining_();
  void publish_ends_at_(uint16_t seconds) {
    publications++;
    last_seconds = seconds;
  }
};
"""
    main = """\
int main() {
  FakeSession session;
  RainbirdComponent hub{&session};
  hub.publish_remaining_();
  assert(hub.publications == 0);
  session.value.remaining_seconds = 120;
  session.value.remaining_readings++;
  hub.publish_remaining_();
  assert(hub.publications == 1 && hub.last_seconds == 120);
  hub.publish_remaining_();
  assert(hub.publications == 1);
  session.value.remaining_seconds = 0;
  session.value.remaining_readings++;
  hub.publish_remaining_();
  assert(hub.publications == 2 && hub.last_seconds == 0);
}
"""
    cpp = tmp_path / "ends_at.cpp"
    binary = tmp_path / "ends_at"
    cpp.write_text(harness + method + main)
    subprocess.run(
        [
            CXX,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DUSE_TEXT_SENSOR",
            "-DUSE_TIME",
            f"-I{REPO / 'components/rainbird'}",
            str(cpp),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
