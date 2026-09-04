#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "midi_recorder/MidiParser.hpp"
#include "midi_recorder/SmfRecorder.hpp"

namespace {

using midi_recorder::MidiMessageKind;
using midi_recorder::MidiMessageView;
using midi_recorder::MidiParser;
using midi_recorder::SeekableByteStream;
using midi_recorder::SmfRecorder;

int failureCount = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "          \
                << #condition << '\n';                                          \
      ++failureCount;                                                           \
    }                                                                           \
  } while (false)

struct CapturedMessage {
  MidiMessageKind kind;
  std::vector<std::uint8_t> bytes;
  std::uint64_t timestampUs;
};

void captureMessage(void* context, const MidiMessageView& message) {
  auto* messages = static_cast<std::vector<CapturedMessage>*>(context);
  messages->push_back({message.kind,
                       std::vector<std::uint8_t>(message.bytes,
                                                 message.bytes + message.length),
                       message.timestampUs});
}

class MemoryStream final : public SeekableByteStream {
 public:
  std::size_t write(const std::uint8_t* source,
                    std::size_t length) override {
    if (position_ + length > bytes.size()) {
      bytes.resize(position_ + length);
    }
    std::copy(source, source + length, bytes.begin() + position_);
    position_ += length;
    return length;
  }

  bool seek(std::uint32_t position) override {
    if (position > bytes.size()) {
      return false;
    }
    position_ = position;
    return true;
  }

  std::uint32_t position() override {
    return static_cast<std::uint32_t>(position_);
  }

  void flush() override { ++flushCount; }

  std::vector<std::uint8_t> bytes;
  std::uint32_t flushCount = 0;

 private:
  std::size_t position_ = 0;
};

std::uint32_t readBigEndian32(const std::vector<std::uint8_t>& bytes,
                              std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

bool containsSequence(const std::vector<std::uint8_t>& haystack,
                      const std::vector<std::uint8_t>& needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(),
                     needle.end()) != haystack.end();
}

void testVariableLengthEncoding() {
  struct Case {
    std::uint32_t input;
    std::vector<std::uint8_t> expected;
  };
  const std::vector<Case> cases = {
      {0, {0x00}},
      {127, {0x7F}},
      {128, {0x81, 0x00}},
      {16383, {0xFF, 0x7F}},
      {16384, {0x81, 0x80, 0x00}},
      {0x0FFFFFFF, {0xFF, 0xFF, 0xFF, 0x7F}},
  };

  for (const auto& test : cases) {
    std::uint8_t output[4] = {};
    const std::size_t length =
        midi_recorder::encodeVariableLength(test.input, output);
    CHECK(length == test.expected.size());
    CHECK(std::equal(output, output + length, test.expected.begin()));
  }
}

void testChannelAndRunningStatus() {
  MidiParser parser;
  std::vector<CapturedMessage> messages;
  parser.feed(0x90, 100, captureMessage, &messages);
  parser.feed(0x3C, 110, captureMessage, &messages);
  parser.feed(0x64, 120, captureMessage, &messages);
  parser.feed(0x3D, 200, captureMessage, &messages);
  parser.feed(0x00, 210, captureMessage, &messages);
  parser.feed(0xC1, 300, captureMessage, &messages);
  parser.feed(0x0A, 310, captureMessage, &messages);

  CHECK(messages.size() == 3);
  CHECK(messages[0].kind == MidiMessageKind::Channel);
  CHECK(messages[0].bytes == std::vector<std::uint8_t>({0x90, 0x3C, 0x64}));
  CHECK(messages[0].timestampUs == 100);
  CHECK(messages[1].bytes == std::vector<std::uint8_t>({0x90, 0x3D, 0x00}));
  CHECK(messages[1].timestampUs == 200);
  CHECK(messages[2].bytes == std::vector<std::uint8_t>({0xC1, 0x0A}));
}

void testRealtimeInterleaving() {
  MidiParser parser;
  std::vector<CapturedMessage> messages;
  parser.feed(0x90, 100, captureMessage, &messages);
  parser.feed(0x3C, 110, captureMessage, &messages);
  parser.feed(0xF8, 115, captureMessage, &messages);
  parser.feed(0x64, 120, captureMessage, &messages);

  CHECK(messages.size() == 2);
  CHECK(messages[0].kind == MidiMessageKind::Realtime);
  CHECK(messages[0].bytes == std::vector<std::uint8_t>({0xF8}));
  CHECK(messages[1].kind == MidiMessageKind::Channel);
  CHECK(messages[1].bytes == std::vector<std::uint8_t>({0x90, 0x3C, 0x64}));
}

void testSysExAndRealtime() {
  MidiParser parser;
  std::vector<CapturedMessage> messages;
  const std::uint8_t input[] = {0xF0, 0x7D, 0x01, 0xF8, 0x02, 0xF7};
  for (std::size_t index = 0; index < sizeof(input); ++index) {
    parser.feed(input[index], 100 + index, captureMessage, &messages);
  }

  CHECK(messages.size() == 2);
  CHECK(messages[0].kind == MidiMessageKind::Realtime);
  CHECK(messages[1].kind == MidiMessageKind::SystemExclusive);
  CHECK(messages[1].bytes ==
        std::vector<std::uint8_t>({0xF0, 0x7D, 0x01, 0x02, 0xF7}));
  CHECK(messages[1].timestampUs == 100);
}

void testSystemCommonClearsRunningStatus() {
  MidiParser parser;
  std::vector<CapturedMessage> messages;
  const std::uint8_t input[] = {0x90, 0x3C, 0x64, 0xF2,
                                0x00, 0x00, 0x3D, 0x64};
  for (std::size_t index = 0; index < sizeof(input); ++index) {
    parser.feed(input[index], index, captureMessage, &messages);
  }

  CHECK(messages.size() == 2);
  CHECK(messages[0].kind == MidiMessageKind::Channel);
  CHECK(messages[1].kind == MidiMessageKind::SystemCommon);
  CHECK(messages[1].bytes == std::vector<std::uint8_t>({0xF2, 0x00, 0x00}));
  CHECK(parser.malformedByteCount() == 2);
}

void testOversizedSysExIsDropped() {
  MidiParser parser(4);
  std::vector<CapturedMessage> messages;
  const std::uint8_t input[] = {0xF0, 0x01, 0x02, 0x03, 0xF7};
  for (std::size_t index = 0; index < sizeof(input); ++index) {
    parser.feed(input[index], index, captureMessage, &messages);
  }
  CHECK(messages.empty());
  CHECK(parser.oversizedSysExCount() == 1);
}

void testSmfRecordingAndCheckpoint() {
  MemoryStream stream;
  SmfRecorder recorder;
  constexpr std::uint64_t startUs = 1000000;
  CHECK(recorder.begin(stream, startUs));
  CHECK(stream.bytes.size() > 22);
  CHECK(std::memcmp(stream.bytes.data(), "MThd", 4) == 0);
  CHECK(std::memcmp(stream.bytes.data() + 14, "MTrk", 4) == 0);
  CHECK(readBigEndian32(stream.bytes, 18) == stream.bytes.size() - 22);
  CHECK(containsSequence(stream.bytes, {0x00, 0xFF, 0x2F, 0x00}));

  const std::uint8_t noteOn[] = {0x90, 0x3C, 0x64};
  const std::uint8_t program[] = {0xC0, 0x05};
  const std::uint8_t noteOff[] = {0x80, 0x3C, 0x00};
  CHECK(recorder.record({MidiMessageKind::Channel, noteOn, sizeof(noteOn),
                         startUs}));
  CHECK(recorder.record({MidiMessageKind::Channel, program, sizeof(program),
                         startUs + 250000}));
  CHECK(recorder.record({MidiMessageKind::Channel, noteOff, sizeof(noteOff),
                         startUs + 500000}));
  CHECK(recorder.checkpoint());
  CHECK(readBigEndian32(stream.bytes, 18) == stream.bytes.size() - 22);
  CHECK(containsSequence(stream.bytes,
                         {0x00, 0x90, 0x3C, 0x64, 0x83, 0x60, 0xC0, 0x05,
                          0x83, 0x60, 0x80, 0x3C, 0x00}));

  CHECK(recorder.finish(startUs + 750000));
  CHECK(!recorder.active());
  CHECK(recorder.recordedEventCount() == 3);
  CHECK(readBigEndian32(stream.bytes, 18) == stream.bytes.size() - 22);
  CHECK(stream.bytes.size() >= 5);
  CHECK(std::equal(stream.bytes.end() - 5, stream.bytes.end(),
                   std::vector<std::uint8_t>({0x83, 0x60, 0xFF, 0x2F, 0x00})
                       .begin()));
}

void testArmedStartTimeAndSysEx() {
  MemoryStream stream;
  SmfRecorder recorder;
  CHECK(recorder.begin(stream, 0));
  CHECK(recorder.setStartTime(5000000));

  const std::uint8_t clock[] = {0xF8};
  const std::uint8_t sysEx[] = {0xF0, 0x7D, 0x01, 0xF7};
  CHECK(recorder.record(
      {MidiMessageKind::Realtime, clock, sizeof(clock), 5000000}));
  CHECK(recorder.record({MidiMessageKind::SystemExclusive, sysEx,
                         sizeof(sysEx), 5000000}));
  CHECK(recorder.finish(5000000));
  CHECK(recorder.recordedEventCount() == 1);
  CHECK(recorder.skippedEventCount() == 1);
  CHECK(containsSequence(stream.bytes, {0x00, 0xF0, 0x03, 0x7D, 0x01, 0xF7}));
}

struct ArmedRecorderHarness {
  SmfRecorder* recorder;
  std::uint64_t armedAtUs;
  bool armed = true;
  bool recording = false;
};

void routeArmedMessage(void* context, const MidiMessageView& message) {
  auto* harness = static_cast<ArmedRecorderHarness*>(context);
  if (harness->armed && message.timestampUs >= harness->armedAtUs &&
      message.kind == MidiMessageKind::Realtime &&
      message.length == 1 &&
      (message.bytes[0] == 0xFA || message.bytes[0] == 0xFB)) {
    CHECK(harness->recorder->setStartTime(message.timestampUs));
    harness->armed = false;
    harness->recording = true;
    return;
  }
  if (harness->recording) {
    CHECK(harness->recorder->record(message));
  }
}

void testTransportArmingIntegration() {
  MemoryStream stream;
  SmfRecorder recorder;
  MidiParser parser;
  ArmedRecorderHarness harness{&recorder, 1000000};
  CHECK(recorder.begin(stream, 0));

  // Notes before transport are intentionally ignored. Start establishes time
  // zero; the first post-Start note must be written at delta zero.
  const struct TimedByte {
    std::uint8_t byte;
    std::uint64_t timestampUs;
  } bytes[] = {
      {0x90, 900000}, {0x30, 900100}, {0x50, 900200}, {0xFA, 999999},
      {0xF8, 1000000}, {0xFB, 1000100}, {0x90, 1000200},
      {0x3C, 1000300}, {0x64, 1000400},
  };
  for (const auto& byte : bytes) {
    parser.feed(byte.byte, byte.timestampUs, routeArmedMessage, &harness);
  }

  CHECK(!harness.armed);
  CHECK(harness.recording);
  CHECK(recorder.recordedEventCount() == 1);
  CHECK(recorder.finish(1500000));
  CHECK(containsSequence(stream.bytes, {0x00, 0x90, 0x3C, 0x64}));
}

}  // namespace

int main() {
  testVariableLengthEncoding();
  testChannelAndRunningStatus();
  testRealtimeInterleaving();
  testSysExAndRealtime();
  testSystemCommonClearsRunningStatus();
  testOversizedSysExIsDropped();
  testSmfRecordingAndCheckpoint();
  testArmedStartTimeAndSysEx();
  testTransportArmingIntegration();

  if (failureCount != 0) {
    std::cerr << failureCount << " test(s) failed\n";
    return 1;
  }
  std::cout << "All native MIDI recorder tests passed\n";
  return 0;
}
