#include "midi_recorder/MidiParser.hpp"

namespace midi_recorder {

MidiParser::MidiParser(std::size_t maximumSysExBytes)
    : maximumSysExBytes_(maximumSysExBytes) {
  sysEx_.reserve(maximumSysExBytes_ < 1024 ? maximumSysExBytes_ : 1024);
}

void MidiParser::reset() {
  sysEx_.clear();
  runningStatus_ = 0;
  pendingLength_ = 0;
  pendingExpected_ = 0;
  pendingTimestampUs_ = 0;
  sysExTimestampUs_ = 0;
  inSysEx_ = false;
  sysExOverflow_ = false;
  malformedByteCount_ = 0;
  oversizedSysExCount_ = 0;
}

std::uint8_t MidiParser::channelDataLength(std::uint8_t status) {
  const std::uint8_t family = status & 0xF0;
  return (family == 0xC0 || family == 0xD0) ? 1 : 2;
}

std::uint8_t MidiParser::systemDataLength(std::uint8_t status) {
  switch (status) {
    case 0xF1:  // MIDI Time Code quarter-frame
    case 0xF3:  // Song Select
      return 1;
    case 0xF2:  // Song Position Pointer
      return 2;
    default:
      return 0;
  }
}

void MidiParser::emit(MidiMessageKind kind, const std::uint8_t* bytes,
                      std::size_t length, std::uint64_t timestampUs,
                      Callback callback, void* context) const {
  if (callback == nullptr) {
    return;
  }
  const MidiMessageView view{kind, bytes, length, timestampUs};
  callback(context, view);
}

void MidiParser::beginMessage(std::uint8_t status,
                              std::uint64_t timestampUs,
                              MidiMessageKind kind,
                              std::uint8_t dataLength, Callback callback,
                              void* context) {
  pending_[0] = status;
  pendingLength_ = 1;
  pendingExpected_ = static_cast<std::uint8_t>(dataLength + 1);
  pendingKind_ = kind;
  pendingTimestampUs_ = timestampUs;

  if (pendingExpected_ == 1) {
    emit(pendingKind_, pending_, pendingLength_, pendingTimestampUs_, callback,
         context);
    pendingLength_ = 0;
    pendingExpected_ = 0;
  }
}

void MidiParser::feed(std::uint8_t byte, std::uint64_t timestampUs,
                      Callback callback, void* context) {
  // Realtime messages can appear between any two bytes, including inside SysEx,
  // and never disturb the message currently being assembled.
  if (byte >= 0xF8) {
    emit(MidiMessageKind::Realtime, &byte, 1, timestampUs, callback, context);
    return;
  }

  if (inSysEx_) {
    if (byte == 0xF7) {
      if (!sysExOverflow_) {
        if (sysEx_.size() < maximumSysExBytes_) {
          sysEx_.push_back(byte);
          emit(MidiMessageKind::SystemExclusive, sysEx_.data(), sysEx_.size(),
               sysExTimestampUs_, callback, context);
        } else {
          sysExOverflow_ = true;
        }
      }
      if (sysExOverflow_) {
        ++oversizedSysExCount_;
      }
      sysEx_.clear();
      inSysEx_ = false;
      sysExOverflow_ = false;
      return;
    }

    if ((byte & 0x80) != 0) {
      // A non-realtime status before EOX aborts a malformed SysEx. Process the
      // new status normally so one bad packet cannot desynchronise the stream.
      ++malformedByteCount_;
      sysEx_.clear();
      inSysEx_ = false;
      sysExOverflow_ = false;
    } else {
      if (!sysExOverflow_) {
        if (sysEx_.size() < maximumSysExBytes_) {
          sysEx_.push_back(byte);
        } else {
          sysExOverflow_ = true;
        }
      }
      return;
    }
  }

  if ((byte & 0x80) != 0) {
    if (pendingLength_ != 0) {
      ++malformedByteCount_;
    }
    pendingLength_ = 0;
    pendingExpected_ = 0;

    if (byte >= 0x80 && byte <= 0xEF) {
      runningStatus_ = byte;
      beginMessage(byte, timestampUs, MidiMessageKind::Channel,
                   channelDataLength(byte), callback, context);
      return;
    }

    runningStatus_ = 0;
    if (byte == 0xF0) {
      sysEx_.clear();
      sysEx_.push_back(byte);
      sysExTimestampUs_ = timestampUs;
      inSysEx_ = true;
      sysExOverflow_ = maximumSysExBytes_ == 0;
      return;
    }

    beginMessage(byte, timestampUs, MidiMessageKind::SystemCommon,
                 systemDataLength(byte), callback, context);
    return;
  }

  if (pendingLength_ == 0) {
    if (runningStatus_ == 0) {
      ++malformedByteCount_;
      return;
    }
    beginMessage(runningStatus_, timestampUs, MidiMessageKind::Channel,
                 channelDataLength(runningStatus_), callback, context);
  }

  if (pendingLength_ < sizeof(pending_)) {
    pending_[pendingLength_++] = byte;
  }

  if (pendingLength_ == pendingExpected_) {
    emit(pendingKind_, pending_, pendingLength_, pendingTimestampUs_, callback,
         context);
    pendingLength_ = 0;
    pendingExpected_ = 0;
  }
}

}  // namespace midi_recorder
