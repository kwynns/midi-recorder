#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace midi_recorder {

enum class MidiMessageKind : std::uint8_t {
  Channel,
  SystemExclusive,
  SystemCommon,
  Realtime,
};

struct MidiMessageView {
  MidiMessageKind kind;
  const std::uint8_t* bytes;
  std::size_t length;
  std::uint64_t timestampUs;
};

class MidiParser {
 public:
  using Callback = void (*)(void* context, const MidiMessageView& message);

  explicit MidiParser(std::size_t maximumSysExBytes = 32768);

  void reset();
  void feed(std::uint8_t byte, std::uint64_t timestampUs, Callback callback,
            void* context);

  std::uint32_t malformedByteCount() const { return malformedByteCount_; }
  std::uint32_t oversizedSysExCount() const { return oversizedSysExCount_; }

 private:
  static std::uint8_t channelDataLength(std::uint8_t status);
  static std::uint8_t systemDataLength(std::uint8_t status);

  void beginMessage(std::uint8_t status, std::uint64_t timestampUs,
                    MidiMessageKind kind, std::uint8_t dataLength,
                    Callback callback, void* context);
  void emit(MidiMessageKind kind, const std::uint8_t* bytes,
            std::size_t length, std::uint64_t timestampUs, Callback callback,
            void* context) const;

  const std::size_t maximumSysExBytes_;
  std::vector<std::uint8_t> sysEx_;
  std::uint8_t runningStatus_ = 0;
  std::uint8_t pending_[3] = {};
  std::uint8_t pendingLength_ = 0;
  std::uint8_t pendingExpected_ = 0;
  MidiMessageKind pendingKind_ = MidiMessageKind::Channel;
  std::uint64_t pendingTimestampUs_ = 0;
  std::uint64_t sysExTimestampUs_ = 0;
  bool inSysEx_ = false;
  bool sysExOverflow_ = false;
  std::uint32_t malformedByteCount_ = 0;
  std::uint32_t oversizedSysExCount_ = 0;
};

}  // namespace midi_recorder

