# Host build for the pure C++ layers. No ESPHome include path, so a protocol
# file that includes ESPHome fails here (PLAN.md 5.1).
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -Wall -Wextra -Werror -I../../components/rainbird

SRCS = \
	../../components/rainbird/manchester.cpp \
	../../components/rainbird/sip_frame.cpp \
	../../components/rainbird/sip_commands.cpp \
	../../components/rainbird/controller_state.cpp \
	../../components/rainbird/model_esp_tm2.cpp

TESTS = test_main.cpp test_manchester.cpp test_frame.cpp test_commands.cpp test_model.cpp

HDRS = \
	../../components/rainbird/manchester.h \
	../../components/rainbird/sip_frame.h \
	../../components/rainbird/sip_commands.h \
	../../components/rainbird/controller_state.h \
	../../components/rainbird/model.h \
	../../components/rainbird/model_esp_tm2.h \
	doctest.h hex.h vectors.h

.PHONY: test clean

test: test_runner
	./test_runner

test_runner: $(TESTS) $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(TESTS) $(SRCS)

clean:
	rm -f test_runner
