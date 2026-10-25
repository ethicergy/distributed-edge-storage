# Host build: unit tests, cluster simulation and tools.
#
#   make test      build and run every C++ host test
#   make sim       build the interactive 4-node cluster simulator
#   make test-pi   Raspberry Pi package tests (incl. end-to-end via sim)
#   make check     all of the above: the one command that must be green
#   make firmware  build all firmware images with PlatformIO
#   make clean
#
# Everything directly under src/ is platform independent C++17 and is
# compiled here. Arduino / ESP-IDF code lives in src/platform/ and is
# built only by PlatformIO.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
# Fault injection is compiled into host builds so the cluster tests
# can exercise lost and duplicated packets.
CPPFLAGS += -Iinclude -Isim -DEDGE_FAULT_INJECTION=1

BUILD    := build/host
SOURCES  := $(wildcard src/*.cpp)
OBJECTS  := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(SOURCES))
SIM_SRCS := $(wildcard sim/*.cpp)
SIM_OBJS := $(patsubst sim/%.cpp,$(BUILD)/sim/%.o,$(SIM_SRCS))
TESTS    := $(patsubst test/%.cpp,$(BUILD)/test/%,$(wildcard test/*_test.cpp))
HEADERS  := $(wildcard include/*.h sim/*.h test/*.h)

PIO      ?= $(shell command -v pio 2>/dev/null || echo .venv/bin/pio)

.PHONY: all test sim test-pi check firmware clean

all: test sim

$(BUILD)/obj/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/sim/%.o: sim/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test/%: test/%.cpp $(OBJECTS) $(SIM_OBJS) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(OBJECTS) $(SIM_OBJS) -o $@

test: $(TESTS)
	@pass=0; fail=0; failed=""; \
	for t in $(TESTS); do \
		name=$$(basename $$t); \
		if $$t > $(BUILD)/$$name.log 2>&1; then \
			pass=$$((pass+1)); printf "  PASS  %s\n" $$name; \
		else \
			fail=$$((fail+1)); failed="$$failed $$name"; \
			printf "  FAIL  %s\n" $$name; tail -n 20 $(BUILD)/$$name.log; \
		fi; \
	done; \
	echo "----------------------------------------"; \
	echo "  $$pass passed, $$fail failed"; \
	test $$fail -eq 0

sim: $(BUILD)/edge_sim

$(BUILD)/edge_sim: tools/edge_sim.cpp $(OBJECTS) $(SIM_OBJS) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(OBJECTS) $(SIM_OBJS) -o $@ -lutil

test-pi: sim
	cd pi && python3 -m unittest discover -s tests -v

check: test test-pi

firmware:
	$(PIO) run -e node-a -e node-b -e node-c -e node-d \
	           -e node-a-fi -e node-b-fi -e node-c-fi -e node-d-fi
	scripts/check_images.sh

clean:
	rm -rf build
