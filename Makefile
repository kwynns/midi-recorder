CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2

NATIVE_TEST := build/native/midi_recorder_tests
NATIVE_SOURCES := \
	tests/native/test_main.cpp \
	src/MidiParser.cpp \
	src/SmfRecorder.cpp

.PHONY: test clean

test: $(NATIVE_TEST)
	./$(NATIVE_TEST)

$(NATIVE_TEST): $(NATIVE_SOURCES) include/midi_recorder/MidiParser.hpp include/midi_recorder/SmfRecorder.hpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Iinclude $(NATIVE_SOURCES) -o $@

clean:
	rm -rf build/native

