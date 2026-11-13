# Host build for the pure C++ layers. No ESPHome include path, so a protocol
# file that includes ESPHome fails here (PLAN.md 5.1).
#
#   make test          build what changed and run the tests
#   make test SAN=1    the same under AddressSanitizer and UBSan (its own build directory)
#   make -j test       compile in parallel
CXX ?= g++
COMPONENT = ../../components/rainbird
CXXFLAGS ?= -std=gnu++17 -Wall -Wextra -Werror
CPPFLAGS += -I$(COMPONENT) -DCAPTURES_DIR=\"../captures\" -MMD -MP

ifeq ($(SAN),1)
BUILD = build-san
CXXFLAGS += -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined
LDFLAGS += -fsanitize=address,undefined
else
BUILD = build
endif

# The pure layers, by name. A file that needs ESPHome does not belong in this list.
SRCS = manchester sip_frame sip_commands controller_state model_esp_tm2 link_core session \
	schedule_editor clock_sync entity_values entity_writes

TESTS = $(basename $(wildcard test_*.cpp))

SRC_OBJS = $(SRCS:%=$(BUILD)/src_%.o)
TEST_OBJS = $(TESTS:%=$(BUILD)/%.o)

.PHONY: test clean

test: $(BUILD)/test_runner
	./$(BUILD)/test_runner

$(BUILD)/test_runner: $(TEST_OBJS) $(SRC_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

# Fills in a fixture's expect lines. Used by tools/raw_to_fixture.py.
decode_fixture: $(BUILD)/decode_fixture.o $(SRC_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/src_%.o: $(COMPONENT)/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf build build-san test_runner decode_fixture

-include $(wildcard $(BUILD)/*.d)
