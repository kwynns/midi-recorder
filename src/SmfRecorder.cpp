#include "midi_recorder/SmfRecorder.hpp"

#include <algorithm>

namespace midi_recorder {

namespace {

constexpr std::uint8_t kFileHeader[] = {
    'M', 'T', 'h', 'd',  // Header chunk
    0x00, 0x00, 0x00, 0x06,
    0x00, 0x00,  // Format 0
    0x00, 0x01,  // One track
    static_cast<std::uint8_t>(SmfRecorder::kTicksPerQuarterNote >> 8),
    static_cast<std::uint8_t>(SmfRecorder::kTicksPerQuarterNote & 0xFF),
    'M', 'T', 'r', 'k',  // Track chunk
    0x00, 0x00, 0x00, 0x00,
};

constexpr char kTrackName[] = "M5 MIDI Recorder";

}  // namespace

std::size_t encodeVariableLength(std::uint32_t value,
                                 std::uint8_t output[4]) {
  value &= SmfRecorder::kMaximumDeltaTicks;
  std::uint32_t packed = value & 0x7F;
  while ((value >>= 7) != 0) {
    packed <<= 8;
    packed |= (value & 0x7F) | 0x80;
  }

  std::size_t length = 0;
  while (true) {
    output[length++] = static_cast<std::uint8_t>(packed & 0xFF);
    if ((packed & 0x80) == 0) {
      break;
    }
    packed >>= 8;
  }
  return length;
}

bool SmfRecorder::begin(SeekableByteStream& stream,
                        std::uint64_t startedAtUs) {
  stream_ = &stream;
  pending_.clear();
  pending_.reserve(8192);
  startedAtUs_ = startedAtUs;
  lastEventTick_ = 0;
  committedDataEnd_ = kTrackDataOffset;
  recordedEventCount_ = 0;
  skippedEventCount_ = 0;
  active_ = true;
  ok_ = true;

  if (!stream_->seek(0) || !writeAll(kFileHeader, sizeof(kFileHeader))) {
    active_ = false;
    ok_ = false;
    return false;
  }

  // Track name.
  appendByte(0x00);
  appendByte(0xFF);
  appendByte(0x03);
  appendVariableLength(sizeof(kTrackName) - 1);
  appendBytes(reinterpret_cast<const std::uint8_t*>(kTrackName),
              sizeof(kTrackName) - 1);

  // A deliberately time-based tempo map. Events are converted from absolute
  // microseconds at 960 PPQN / 120 BPM, keeping them locked to wall time and
  // therefore to a separately recorded stereo file. The displayed tempo is not
  // inferred from MIDI Clock; that can be added as a later tempo-map feature.
  appendByte(0x00);
  appendByte(0xFF);
  appendByte(0x51);
  appendByte(0x03);
  appendByte(static_cast<std::uint8_t>(kMicrosecondsPerQuarterNote >> 16));
  appendByte(static_cast<std::uint8_t>(kMicrosecondsPerQuarterNote >> 8));
  appendByte(static_cast<std::uint8_t>(kMicrosecondsPerQuarterNote));

  return checkpoint();
}

bool SmfRecorder::setStartTime(std::uint64_t startedAtUs) {
  if (!active_ || !ok_ || recordedEventCount_ != 0 || lastEventTick_ != 0 ||
      !pending_.empty()) {
    return false;
  }
  startedAtUs_ = startedAtUs;
  return true;
}

void SmfRecorder::appendBytes(const std::uint8_t* data, std::size_t length) {
  pending_.insert(pending_.end(), data, data + length);
}

void SmfRecorder::appendVariableLength(std::uint32_t value) {
  std::uint8_t encoded[4];
  const std::size_t length = encodeVariableLength(value, encoded);
  appendBytes(encoded, length);
}

bool SmfRecorder::appendEventDelta(std::uint64_t absoluteTick) {
  if (absoluteTick < lastEventTick_) {
    absoluteTick = lastEventTick_;
  }

  std::uint64_t delta = absoluteTick - lastEventTick_;
  while (delta > kMaximumDeltaTicks) {
    // Split exceptionally long silence with an empty sequencer-specific meta
    // event. This keeps every delta legal without changing playback.
    appendVariableLength(kMaximumDeltaTicks);
    appendByte(0xFF);
    appendByte(0x7F);
    appendByte(0x00);
    delta -= kMaximumDeltaTicks;
  }
  appendVariableLength(static_cast<std::uint32_t>(delta));
  lastEventTick_ = absoluteTick;
  return true;
}

std::uint64_t SmfRecorder::timestampToTick(std::uint64_t timestampUs) const {
  if (timestampUs <= startedAtUs_) {
    return 0;
  }
  const std::uint64_t elapsedUs = timestampUs - startedAtUs_;
  const std::uint64_t wholeQuarters =
      elapsedUs / kMicrosecondsPerQuarterNote;
  const std::uint64_t remainderUs =
      elapsedUs % kMicrosecondsPerQuarterNote;
  return wholeQuarters * kTicksPerQuarterNote +
         (remainderUs * kTicksPerQuarterNote +
          kMicrosecondsPerQuarterNote / 2) /
             kMicrosecondsPerQuarterNote;
}

bool SmfRecorder::record(const MidiMessageView& message) {
  if (!active_ || !ok_ || message.bytes == nullptr || message.length == 0) {
    return false;
  }

  if (message.kind == MidiMessageKind::Channel) {
    appendEventDelta(timestampToTick(message.timestampUs));
    appendBytes(message.bytes, message.length);
    ++recordedEventCount_;
    return true;
  }

  if (message.kind == MidiMessageKind::SystemExclusive &&
      message.bytes[0] == 0xF0 && message.length >= 2) {
    appendEventDelta(timestampToTick(message.timestampUs));
    appendByte(0xF0);
    appendVariableLength(static_cast<std::uint32_t>(message.length - 1));
    appendBytes(message.bytes + 1, message.length - 1);
    ++recordedEventCount_;
    return true;
  }

  // SMF uses 0xFF for meta events and does not directly encode live MIDI Clock,
  // Start/Continue/Stop, Active Sensing, Reset, or system-common messages.
  ++skippedEventCount_;
  return true;
}

bool SmfRecorder::writeAll(const std::uint8_t* data, std::size_t length) {
  if (!ok_ || stream_ == nullptr) {
    return false;
  }
  if (length == 0) {
    return true;
  }
  if (stream_->write(data, length) != length) {
    ok_ = false;
    return false;
  }
  return true;
}

bool SmfRecorder::patchTrackLength(std::uint32_t endPosition) {
  if (endPosition < kTrackDataOffset) {
    ok_ = false;
    return false;
  }
  const std::uint32_t trackLength = endPosition - kTrackDataOffset;
  const std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(trackLength >> 24),
      static_cast<std::uint8_t>(trackLength >> 16),
      static_cast<std::uint8_t>(trackLength >> 8),
      static_cast<std::uint8_t>(trackLength),
  };
  return stream_->seek(kTrackLengthOffset) && writeAll(bytes, sizeof(bytes)) &&
         stream_->seek(endPosition);
}

bool SmfRecorder::commitWithEndMarker(std::uint64_t endDeltaTicks,
                                      bool finalCommit) {
  if (!active_ || !ok_ || stream_ == nullptr ||
      !stream_->seek(committedDataEnd_)) {
    ok_ = false;
    return false;
  }

  if (!writeAll(pending_.data(), pending_.size())) {
    return false;
  }
  committedDataEnd_ += static_cast<std::uint32_t>(pending_.size());
  pending_.clear();

  std::vector<std::uint8_t> endMarker;
  endMarker.reserve(8);
  while (endDeltaTicks > kMaximumDeltaTicks) {
    std::uint8_t encoded[4];
    const std::size_t length =
        encodeVariableLength(kMaximumDeltaTicks, encoded);
    endMarker.insert(endMarker.end(), encoded, encoded + length);
    endMarker.push_back(0xFF);
    endMarker.push_back(0x7F);
    endMarker.push_back(0x00);
    endDeltaTicks -= kMaximumDeltaTicks;
  }
  std::uint8_t encoded[4];
  const std::size_t encodedLength =
      encodeVariableLength(static_cast<std::uint32_t>(endDeltaTicks), encoded);
  endMarker.insert(endMarker.end(), encoded, encoded + encodedLength);
  endMarker.push_back(0xFF);
  endMarker.push_back(0x2F);
  endMarker.push_back(0x00);

  if (!writeAll(endMarker.data(), endMarker.size())) {
    return false;
  }
  const std::uint32_t endPosition = stream_->position();
  if (!patchTrackLength(endPosition)) {
    ok_ = false;
    return false;
  }
  stream_->flush();

  if (finalCommit) {
    active_ = false;
  }
  return true;
}

bool SmfRecorder::checkpoint() {
  const bool succeeded = commitWithEndMarker(0, false);
  if (!succeeded) {
    active_ = false;
  }
  return succeeded;
}

bool SmfRecorder::finish(std::uint64_t stoppedAtUs) {
  if (!active_ || !ok_) {
    return false;
  }
  const std::uint64_t stoppedAtTick =
      std::max(lastEventTick_, timestampToTick(stoppedAtUs));
  const bool succeeded =
      commitWithEndMarker(stoppedAtTick - lastEventTick_, true);
  if (!succeeded) {
    active_ = false;
  }
  return succeeded;
}

}  // namespace midi_recorder
