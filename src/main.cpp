#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <M5Unified.h>
#include <esp_timer.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "AppConfig.hpp"
#include "midi_recorder/MidiParser.hpp"
#include "midi_recorder/SmfRecorder.hpp"

namespace {

using midi_recorder::MidiMessageView;
using midi_recorder::MidiMessageKind;
using midi_recorder::MidiParser;
using midi_recorder::SeekableByteStream;
using midi_recorder::SmfRecorder;

enum class AppState : std::uint8_t {
  NoCard,
  Ready,
  Armed,
  Recording,
  Saving,
  Saved,
  Error,
};

struct CapturedMidiByte {
  std::uint64_t timestampUs;
  std::uint8_t byte;
};

struct Rect {
  std::int16_t x;
  std::int16_t y;
  std::int16_t width;
  std::int16_t height;

  bool contains(std::int16_t pointX, std::int16_t pointY) const {
    return pointX >= x && pointX < x + width && pointY >= y &&
           pointY < y + height;
  }
};

constexpr Rect kRecordButton{12, 181, 142, 48};
constexpr Rect kStopButton{166, 181, 142, 48};

constexpr std::uint16_t kBackground = 0x0841;
constexpr std::uint16_t kPanel = 0x10C3;
constexpr std::uint16_t kMuted = 0x9CF3;
constexpr std::uint16_t kReadyGreen = 0x2626;
constexpr std::uint16_t kRecordRed = 0xE9E4;
constexpr std::uint16_t kButtonDisabled = 0x2945;
constexpr std::uint16_t kWarning = 0xFD20;

class SdByteStream final : public SeekableByteStream {
 public:
  bool open(const char* path) {
    close();
    file_ = SD.open(path, FILE_WRITE);
    return static_cast<bool>(file_);
  }

  void close() {
    if (file_) {
      file_.close();
    }
  }

  bool isOpen() const { return static_cast<bool>(file_); }

  std::size_t write(const std::uint8_t* data,
                    std::size_t length) override {
    return file_ ? file_.write(data, length) : 0;
  }

  bool seek(std::uint32_t position) override {
    return file_ && file_.seek(position);
  }

  std::uint32_t position() override {
    return file_ ? static_cast<std::uint32_t>(file_.position()) : 0;
  }

  void flush() override {
    if (file_) {
      file_.flush();
    }
  }

 private:
  File file_;
};

AppState appState = AppState::NoCard;
MidiParser midiParser(app_config::kMaximumSysExBytes);
SmfRecorder recorder;
SdByteStream midiFile;
QueueHandle_t midiQueue = nullptr;
TaskHandle_t midiCaptureTaskHandle = nullptr;
bool midiInputReady = false;

std::atomic<std::uint32_t> droppedCaptureBytes{0};
std::atomic<std::uint32_t> uartReceiveErrors{0};
std::uint32_t droppedBytesAtStart = 0;
std::uint32_t uartErrorsAtStart = 0;
std::uint32_t malformedBytesAtStart = 0;
std::uint32_t oversizedSysExAtStart = 0;
std::uint32_t finalWarningCount = 0;

std::uint64_t recordingStartedAtUs = 0;
std::uint64_t recordingStoppedAtUs = 0;
std::uint64_t stopBoundaryUs = 0;
std::uint64_t armRequestedAtUs = 0;
std::uint64_t lastMidiMessageAtUs = 0;
std::uint32_t lastCheckpointAtMs = 0;
std::uint32_t lastUiRefreshAtMs = 0;
std::uint64_t sdFreeBytes = 0;

char activePath[32] = {};
char statusDetail[64] = "Insert a microSD card";
bool fullRedrawNeeded = true;

std::uint64_t monotonicUs() {
  return static_cast<std::uint64_t>(esp_timer_get_time());
}

const char* baseName(const char* path);
void setStatusDetail(const char* text);
void drawFullScreen();

std::uint32_t currentWarningCount() {
  return droppedCaptureBytes.load(std::memory_order_relaxed) -
             droppedBytesAtStart +
         uartReceiveErrors.load(std::memory_order_relaxed) -
             uartErrorsAtStart +
         midiParser.malformedByteCount() - malformedBytesAtStart +
         midiParser.oversizedSysExCount() - oversizedSysExAtStart;
}

void midiCaptureTask(void*) {
  for (;;) {
    const int value = Serial2.read();
    if (value < 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    const CapturedMidiByte captured{
        monotonicUs(), static_cast<std::uint8_t>(value)};
    if (xQueueSend(midiQueue, &captured, 0) != pdTRUE) {
      droppedCaptureBytes.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void beginOnMidiTransport(std::uint64_t timestampUs) {
  if (!recorder.setStartTime(timestampUs)) {
    midiFile.close();
    SD.remove(activePath);
    activePath[0] = '\0';
    appState = AppState::Error;
    setStatusDetail("Could not start MIDI timeline");
    fullRedrawNeeded = true;
    return;
  }

  recordingStartedAtUs = timestampUs;
  recordingStoppedAtUs = 0;
  stopBoundaryUs = 0;
  armRequestedAtUs = 0;
  lastCheckpointAtMs = millis();
  appState = AppState::Recording;
  setStatusDetail(baseName(activePath));
  fullRedrawNeeded = true;
}

void onMidiMessage(void*, const MidiMessageView& message) {
  lastMidiMessageAtUs = message.timestampUs;
  if (appState == AppState::Armed &&
      message.timestampUs >= armRequestedAtUs &&
      message.kind == MidiMessageKind::Realtime && message.length == 1 &&
      (message.bytes[0] == 0xFA || message.bytes[0] == 0xFB)) {
    beginOnMidiTransport(message.timestampUs);
    return;
  }
  if (appState != AppState::Recording ||
      message.timestampUs < recordingStartedAtUs ||
      (stopBoundaryUs != 0 && message.timestampUs > stopBoundaryUs)) {
    return;
  }
  recorder.record(message);
}

void drainMidiQueue() {
  if (midiQueue == nullptr) {
    return;
  }
  CapturedMidiByte captured{};
  while (xQueueReceive(midiQueue, &captured, 0) == pdTRUE) {
    midiParser.feed(captured.byte, captured.timestampUs, onMidiMessage,
                    nullptr);
  }
}

const char* baseName(const char* path) {
  const char* slash = std::strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}

void setStatusDetail(const char* text) {
  std::snprintf(statusDetail, sizeof(statusDetail), "%s", text);
}

bool chooseNextFilename(char* output, std::size_t outputSize) {
  for (unsigned int number = 1; number <= 9999; ++number) {
    std::snprintf(output, outputSize, "/MIDI/REC%04u.MID", number);
    if (!SD.exists(output)) {
      return true;
    }
  }
  output[0] = '\0';
  return false;
}

void updateFreeSpace() {
  const std::uint64_t total = SD.totalBytes();
  const std::uint64_t used = SD.usedBytes();
  sdFreeBytes = total > used ? total - used : 0;
}

bool initializeSdCard() {
  if (midiFile.isOpen()) {
    midiFile.close();
  }

  SD.end();
  SPI.begin(app_config::kSdSckPin, app_config::kSdMisoPin,
            app_config::kSdMosiPin, app_config::kSdChipSelectPin);
  if (!SD.begin(app_config::kSdChipSelectPin, SPI,
                app_config::kSdFrequencyHz) ||
      SD.cardType() == CARD_NONE) {
    appState = AppState::NoCard;
    setStatusDetail("Insert card, then tap RETRY");
    fullRedrawNeeded = true;
    return false;
  }

  if (!SD.exists("/MIDI") && !SD.mkdir("/MIDI")) {
    appState = AppState::Error;
    setStatusDetail("Could not create /MIDI");
    fullRedrawNeeded = true;
    return false;
  }

  updateFreeSpace();
  appState = AppState::Ready;
  setStatusDetail("MIDI IN ready");
  fullRedrawNeeded = true;
  return true;
}

void enterWriteError() {
  finalWarningCount = currentWarningCount();
  armRequestedAtUs = 0;
  midiFile.close();
  recordingStoppedAtUs = monotonicUs();
  appState = AppState::Error;
  setStatusDetail("SD write failed; check the card");
  fullRedrawNeeded = true;
}

void armRecording(std::uint64_t requestedAtUs) {
  if (appState == AppState::Armed || appState == AppState::Recording ||
      appState == AppState::Saving) {
    return;
  }
  if (!midiInputReady) {
    appState = AppState::Error;
    setStatusDetail("MIDI input is unavailable");
    fullRedrawNeeded = true;
    return;
  }
  armRequestedAtUs = requestedAtUs;
  droppedBytesAtStart =
      droppedCaptureBytes.load(std::memory_order_relaxed);
  uartErrorsAtStart = uartReceiveErrors.load(std::memory_order_relaxed);
  malformedBytesAtStart = midiParser.malformedByteCount();
  oversizedSysExAtStart = midiParser.oversizedSysExCount();
  finalWarningCount = 0;
  if (appState == AppState::NoCard || appState == AppState::Error) {
    if (!initializeSdCard()) {
      armRequestedAtUs = 0;
      return;
    }
  }
  if (!chooseNextFilename(activePath, sizeof(activePath))) {
    armRequestedAtUs = 0;
    appState = AppState::Error;
    setStatusDetail("REC0001-REC9999 already exist");
    fullRedrawNeeded = true;
    return;
  }
  if (!midiFile.open(activePath)) {
    armRequestedAtUs = 0;
    appState = AppState::NoCard;
    setStatusDetail("Could not open file; tap RETRY");
    fullRedrawNeeded = true;
    return;
  }

  recordingStartedAtUs = 0;
  recordingStoppedAtUs = 0;
  stopBoundaryUs = 0;

  if (!recorder.begin(midiFile, 0)) {
    midiFile.close();
    SD.remove(activePath);
    activePath[0] = '\0';
    armRequestedAtUs = 0;
    appState = AppState::Error;
    setStatusDetail("Could not write MIDI header");
    fullRedrawNeeded = true;
    return;
  }

  appState = AppState::Armed;
  setStatusDetail("Waiting for sequencer PLAY");
  fullRedrawNeeded = true;
  drainMidiQueue();
}

void stopRecording() {
  if (appState == AppState::Armed) {
    midiFile.close();
    SD.remove(activePath);
    activePath[0] = '\0';
    recordingStartedAtUs = 0;
    recordingStoppedAtUs = 0;
    stopBoundaryUs = 0;
    armRequestedAtUs = 0;
    appState = AppState::Ready;
    setStatusDetail("Arm cancelled");
    fullRedrawNeeded = true;
    return;
  }
  if (appState != AppState::Recording) {
    return;
  }

  // Let the capture task empty bytes already reaching the UART, then define
  // the take boundary. This intentionally gives STOP a tiny 2 ms tail rather
  // than cutting off the final partial MIDI message.
  vTaskDelay(pdMS_TO_TICKS(2));
  stopBoundaryUs = monotonicUs();
  drainMidiQueue();

  appState = AppState::Saving;
  fullRedrawNeeded = true;
  recordingStoppedAtUs = stopBoundaryUs;
  drawFullScreen();

  if (!recorder.finish(recordingStoppedAtUs)) {
    enterWriteError();
    return;
  }

  finalWarningCount = currentWarningCount();
  midiFile.close();
  updateFreeSpace();
  appState = AppState::Saved;
  setStatusDetail(baseName(activePath));
  fullRedrawNeeded = true;
}

void formatDuration(std::uint64_t durationUs, char* output,
                    std::size_t outputSize) {
  const std::uint64_t totalTenths = durationUs / 100000;
  const std::uint64_t hours = totalTenths / 36000;
  const std::uint64_t minutes = (totalTenths / 600) % 60;
  const std::uint64_t seconds = (totalTenths / 10) % 60;
  const std::uint64_t tenths = totalTenths % 10;
  std::snprintf(output, outputSize, "%02llu:%02llu:%02llu.%llu",
                static_cast<unsigned long long>(hours),
                static_cast<unsigned long long>(minutes),
                static_cast<unsigned long long>(seconds),
                static_cast<unsigned long long>(tenths));
}

std::uint64_t displayedDurationUs() {
  if (recordingStartedAtUs == 0) {
    return 0;
  }
  if (appState == AppState::Recording) {
    return monotonicUs() - recordingStartedAtUs;
  }
  if (recordingStoppedAtUs >= recordingStartedAtUs) {
    return recordingStoppedAtUs - recordingStartedAtUs;
  }
  return 0;
}

void drawButton(const Rect& rect, const char* label, bool enabled,
                std::uint16_t enabledColor) {
  const std::uint16_t fill = enabled ? enabledColor : kButtonDisabled;
  M5.Display.fillRoundRect(rect.x, rect.y, rect.width, rect.height, 9, fill);
  M5.Display.drawRoundRect(rect.x, rect.y, rect.width, rect.height, 9,
                           enabled ? TFT_WHITE : 0x52AA);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(enabled ? TFT_WHITE : kMuted, fill);
  M5.Display.drawString(label, rect.x + rect.width / 2,
                        rect.y + rect.height / 2);
}

void drawDynamicContent() {
  const std::uint64_t nowUs = monotonicUs();
  char duration[24];
  formatDuration(displayedDurationUs(), duration, sizeof(duration));

  M5.Display.fillRect(0, 68, 320, 53, kBackground);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(4);
  M5.Display.setTextColor(TFT_WHITE, kBackground);
  M5.Display.drawString(duration, 160, 94);

  char stats[72];
  const std::uint32_t liveWarnings = currentWarningCount();
  const std::uint32_t displayedWarnings =
      appState == AppState::Saved ? finalWarningCount : liveWarnings;
  if (appState == AppState::Armed) {
    std::snprintf(stats, sizeof(stats), "Press PLAY on the sequencer");
  } else if (appState == AppState::Recording ||
             appState == AppState::Saving || appState == AppState::Saved) {
    if (displayedWarnings != 0) {
      std::snprintf(stats, sizeof(stats), "Events %lu   Warnings %lu",
                    static_cast<unsigned long>(recorder.recordedEventCount()),
                    static_cast<unsigned long>(displayedWarnings));
    } else {
      std::snprintf(stats, sizeof(stats), "Events %lu",
                    static_cast<unsigned long>(recorder.recordedEventCount()));
    }
  } else {
    std::snprintf(stats, sizeof(stats), "Free %.1f MB",
                  static_cast<double>(sdFreeBytes) / (1024.0 * 1024.0));
  }

  M5.Display.fillRect(0, 124, 320, 23, kBackground);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  const bool showWarningColor =
      (appState == AppState::Recording || appState == AppState::Saving ||
       appState == AppState::Saved) &&
      displayedWarnings != 0;
  M5.Display.setTextColor(showWarningColor ? kWarning : kMuted, kBackground);
  M5.Display.drawString(stats, 160, 135);

  // A small cyan pulse confirms that parsed MIDI is arriving without forcing a
  // redraw for every event.
  const bool midiActive = nowUs >= lastMidiMessageAtUs &&
                          nowUs - lastMidiMessageAtUs < 120000;
  M5.Display.fillCircle(298, 20, 5, midiActive ? TFT_CYAN : kButtonDisabled);
}

void drawFullScreen() {
  M5.Display.fillScreen(kBackground);

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, kBackground);
  M5.Display.drawString("MIDI RECORDER", 12, 10);

  const char* stateText = "NO CARD";
  std::uint16_t stateColor = kWarning;
  switch (appState) {
    case AppState::Ready:
      stateText = "READY";
      stateColor = kReadyGreen;
      break;
    case AppState::Recording:
      stateText = "RECORDING";
      stateColor = kRecordRed;
      break;
    case AppState::Armed:
      stateText = "ARMED";
      stateColor = kWarning;
      break;
    case AppState::Saving:
      stateText = "SAVING";
      stateColor = kWarning;
      break;
    case AppState::Saved:
      stateText = "SAVED";
      stateColor = kReadyGreen;
      break;
    case AppState::Error:
      stateText = "ERROR";
      stateColor = kRecordRed;
      break;
    case AppState::NoCard:
      break;
  }

  M5.Display.fillRoundRect(12, 40, 296, 23, 7, kPanel);
  M5.Display.fillCircle(25, 51, 5, stateColor);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, kPanel);
  M5.Display.drawString(stateText, 37, 51);
  M5.Display.setTextDatum(middle_right);
  M5.Display.setTextColor(kMuted, kPanel);
  M5.Display.drawString(statusDetail, 300, 51);

  const bool canRecord = appState == AppState::Ready ||
                         appState == AppState::Saved ||
                         appState == AppState::NoCard ||
                         appState == AppState::Error;
  const bool canStop =
      appState == AppState::Armed || appState == AppState::Recording;
  const char* recordLabel =
      (appState == AppState::NoCard || appState == AppState::Error)
          ? "RETRY SD"
          : "RECORD";
  drawButton(kRecordButton, recordLabel, canRecord, kRecordRed);
  drawButton(kStopButton,
             appState == AppState::Armed ? "CANCEL" : "STOP / SAVE", canStop,
             0x3186);
  drawDynamicContent();
  fullRedrawNeeded = false;
}

void refreshUiIfNeeded() {
  const std::uint32_t nowMs = millis();
  if (fullRedrawNeeded) {
    drawFullScreen();
    lastUiRefreshAtMs = nowMs;
    return;
  }
  if (nowMs - lastUiRefreshAtMs >= app_config::kUiRefreshIntervalMs) {
    drawDynamicContent();
    lastUiRefreshAtMs = nowMs;
  }
}

void handleTouch() {
  const auto touch = M5.Touch.getDetail();
  if (!touch.wasClicked()) {
    return;
  }

  if (kRecordButton.contains(touch.x, touch.y)) {
    armRecording(monotonicUs());
  } else if (kStopButton.contains(touch.x, touch.y)) {
    stopRecording();
  }
}

void serviceCheckpoint() {
  if (appState != AppState::Recording) {
    return;
  }

  const std::uint32_t nowMs = millis();
  if (recorder.pendingByteCount() < app_config::kCheckpointBufferBytes &&
      nowMs - lastCheckpointAtMs < app_config::kCheckpointIntervalMs) {
    return;
  }

  if (!recorder.checkpoint()) {
    enterWriteError();
    return;
  }
  lastCheckpointAtMs = nowMs;
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.output_power = true;
  M5.begin(config);
  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  }
  M5.Display.setBrightness(128);
  M5.Display.setTextWrap(false);

  Serial.begin(115200);
  Serial.println("M5 CoreS3 MIDI Recorder starting");

  Serial2.setRxBufferSize(app_config::kMidiUartBufferBytes);
  Serial2.begin(app_config::kMidiBaud, SERIAL_8N1, app_config::kMidiRxPin,
                app_config::kMidiTxPin);
  Serial2.onReceiveError([](hardwareSerial_error_t) {
    uartReceiveErrors.fetch_add(1, std::memory_order_relaxed);
  });

  midiQueue = xQueueCreate(app_config::kTimestampQueueItems,
                           sizeof(CapturedMidiByte));
  if (midiQueue == nullptr ||
      xTaskCreatePinnedToCore(midiCaptureTask, "midi-rx", 3072, nullptr, 3,
                              &midiCaptureTaskHandle, 0) != pdPASS) {
    appState = AppState::Error;
    setStatusDetail("Could not start MIDI input");
    drawFullScreen();
    return;
  }
  midiInputReady = true;

  initializeSdCard();
  drawFullScreen();
}

void loop() {
  M5.update();
  handleTouch();
  drainMidiQueue();
  serviceCheckpoint();
  refreshUiIfNeeded();
  delay(1);
}
