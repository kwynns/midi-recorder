#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "midi_recorder/MidiParser.hpp"

namespace midi_recorder {

class SeekableByteStream {
 public:
  virtual ~SeekableByteStream() = default;
  virtual std::size_t write(const std::uint8_t* data, std::size_t length) = 0;
  virtual bool seek(std::uint32_t position) = 0;
  virtual std::uint32_t position() = 0;
  virtual void flush() = 0;
};

// Encodes the low 28 bits of value as a Standard MIDI File variable-length
// quantity. Returns the number of bytes placed in output (1..4).
std::size_t encodeVariableLength(std::uint32_t value,
                                 std::uint8_t output[4]);

class SmfRecorder {
 public:
  static constexpr std::uint16_t kTicksPerQuarterNote = 960;
  static constexpr std::uint32_t kMicrosecondsPerQuarterNote = 500000;
  static constexpr std::uint32_t kMaximumDeltaTicks = 0x0FFFFFFF;

  bool begin(SeekableByteStream& stream, std::uint64_t startedAtUs);
  // Updates the time origin after an already-open recorder has been armed but
  // before it has accepted its first event.
  bool setStartTime(std::uint64_t startedAtUs);
  bool record(const MidiMessageView& message);

  // Makes the on-card file independently valid by replacing its End Of Track
  // marker, patching the MTrk length, and flushing it. Uncommitted bytes remain
  // buffered in RAM until this is called.
  bool checkpoint();

  // Writes a final End Of Track at stoppedAtUs so the MIDI file duration equals
  // the complete recording duration, even if the last note ended earlier.
  bool finish(std::uint64_t stoppedAtUs);

  bool active() const { return active_; }
  bool ok() const { return ok_; }
  std::uint32_t recordedEventCount() const { return recordedEventCount_; }
  std::uint32_t skippedEventCount() const { return skippedEventCount_; }
  std::size_t pendingByteCount() const { return pending_.size(); }
  std::uint64_t startedAtUs() const { return startedAtUs_; }

 private:
  static constexpr std::uint32_t kTrackLengthOffset = 18;
  static constexpr std::uint32_t kTrackDataOffset = 22;

  bool appendEventDelta(std::uint64_t absoluteTick);
  void appendVariableLength(std::uint32_t value);
  void appendByte(std::uint8_t value) { pending_.push_back(value); }
  void appendBytes(const std::uint8_t* data, std::size_t length);
  bool writeAll(const std::uint8_t* data, std::size_t length);
  bool commitWithEndMarker(std::uint64_t endDeltaTicks, bool finalCommit);
  bool patchTrackLength(std::uint32_t endPosition);
  std::uint64_t timestampToTick(std::uint64_t timestampUs) const;

  SeekableByteStream* stream_ = nullptr;
  std::vector<std::uint8_t> pending_;
  std::uint64_t startedAtUs_ = 0;
  std::uint64_t lastEventTick_ = 0;
  std::uint32_t committedDataEnd_ = kTrackDataOffset;
  std::uint32_t recordedEventCount_ = 0;
  std::uint32_t skippedEventCount_ = 0;
  bool active_ = false;
  bool ok_ = false;
};

}  // namespace midi_recorder
