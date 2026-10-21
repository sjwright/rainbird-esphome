# Host build for the pure C++ layers. No ESPHome include path, so a protocol
# file that includes ESPHome fails here (PLAN.md 5.1).
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -Wall -Wextra -Werror -I../../components/rainbird -DCAPTURES_DIR=\"../captures\"

SRCS = \
	../../components/rainbird/manchester.cpp \
	../../components/rainbird/sip_frame.cpp \
	../../components/rainbird/sip_commands.cpp \
	../../components/rainbird/controller_state.cpp \
	../../components/rainbird/model_esp_tm2.cpp \
	../../components/rainbird/link_core.cpp \
	../../components/rainbird/session.cpp \
	../../components/rainbird/clock_sync.cpp \
	../../components/rainbird/entity_values.cpp

TESTS = test_main.cpp test_manchester.cpp test_frame.cpp test_commands.cpp test_model.cpp test_replay.cpp test_link.cpp test_session.cpp test_clock_sync.cpp test_entity_values.cpp

HDRS = \
	../../components/rainbird/manchester.h \
	../../components/rainbird/sip_frame.h \
	../../components/rainbird/sip_commands.h \
	../../components/rainbird/controller_state.h \
	../../components/rainbird/model.h \
	../../components/rainbird/model_esp_tm2.h \
	../../components/rainbird/link_core.h \
	../../components/rainbird/session.h \
	../../components/rainbird/clock_sync.h \
	../../components/rainbird/entity_kind.h \
	../../components/rainbird/entity_values.h \
	doctest.h hex.h vectors.h fixture.h fake_controller.h

.PHONY: test clean

test: test_runner
	./test_runner

test_runner: $(TESTS) $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(TESTS) $(SRCS)

# Fills in a fixture's expect lines. Used by tools/raw_to_fixture.py.
decode_fixture: decode_fixture.cpp $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ decode_fixture.cpp $(SRCS)

clean:
	rm -f test_runner decode_fixture
