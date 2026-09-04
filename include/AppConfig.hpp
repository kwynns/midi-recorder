#pragma once

#include <cstddef>
#include <cstdint>

namespace app_config {

// M5Stack CoreS3 Port C / M-Bus UART pins used by Unit MIDI (U187).
constexpr int kMidiRxPin = 18;
constexpr int kMidiTxPin = 17;
constexpr std::uint32_t kMidiBaud = 31250;
constexpr std::size_t kMidiUartBufferBytes = 4096;
constexpr std::size_t kTimestampQueueItems = 4096;

// CoreS3 built-in microSD SPI pins.
constexpr int kSdSckPin = 36;
constexpr int kSdMisoPin = 35;
constexpr int kSdMosiPin = 37;
constexpr int kSdChipSelectPin = 4;
constexpr std::uint32_t kSdFrequencyHz = 25000000;

constexpr std::uint32_t kCheckpointIntervalMs = 1000;
constexpr std::size_t kCheckpointBufferBytes = 8192;
constexpr std::uint32_t kUiRefreshIntervalMs = 100;
constexpr std::size_t kMaximumSysExBytes = 32768;

}  // namespace app_config

