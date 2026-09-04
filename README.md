# CoreS3 MIDI Recorder

A standalone, touchscreen MIDI recorder for the **M5Stack CoreS3** and
**M5Stack Unit MIDI (U187)**. It records the Unit's DIN MIDI input to standard
format-0 `.MID` files on the CoreS3 microSD card.

The recorder is transport-armed: tapping `RECORD` prepares the file, but the
timeline stays at `00:00:00.0` until the Digitakt sends MIDI Start (`FA`) or
Continue (`FB`). This gives the MIDI file a clean time-zero at Digitakt PLAY.

## Controls

The CoreS3 display is a touchscreen; this app deliberately leaves the physical
power button out of the recording workflow.

- `RECORD`: arms a new recording and waits for Digitakt PLAY.
- `CANCEL`: closes and removes an armed file if PLAY has not arrived.
- `STOP / SAVE`: finalizes the running recording.
- The cyan dot in the upper-right flashes when MIDI messages arrive.

While recording, the display shows elapsed `HH:MM:SS.t`, the captured-event count,
and any parser or input-overflow warnings. Files are placed in `/MIDI` as
`REC0001.MID`, `REC0002.MID`, and so on; existing files are never overwritten.

## Hardware setup

1. Insert a FAT32-formatted microSD card. M5Stack specifies up to 16 GB for the
   CoreS3 card slot.
2. Connect Unit MIDI to CoreS3 **Port C**. The firmware uses CoreS3 RX GPIO 18,
   TX GPIO 17, at the MIDI-standard 31,250 baud.
3. Connect the Digitakt/MRCC recorder feed to the Unit's MIDI **IN**.
4. Set the Unit switch to **`BYPASS`**. The
   [official Unit MIDI page](https://docs.m5stack.com/en/unit/Unit-MIDI) confirms
   that BYPASS presents MIDI IN to the CoreS3's RX pin. It also creates a
   hardware MIDI THRU path to the Unit's OUT jack, so leave OUT disconnected if
   you do not need it. The firmware never echoes input to output, so it will not
   create a second software copy.

CoreS3 has a 320×240 capacitive touchscreen and built-in microSD, as documented
on the [official CoreS3 page](https://docs.m5stack.com/en/core/CoreS3). M5Stack's
[Unit MIDI tutorial](https://docs.m5stack.com/en/arduino/projects/unit/unit_midi)
confirms the GPIO 18/17 UART mapping and 31,250 baud connection.

## Digitakt II setup

On the Digitakt II:

1. Open `SETTINGS > MIDI CONFIG > SYNC` and turn `TRANSPORT SEND` on.
2. Open `SETTINGS > MIDI CONFIG > PORT CONFIG`.
3. Set `OUT PORT FUNCTIONALITY` to `MIDI`.
4. Set `OUTPUT TO` to `MIDI` or `MIDI+USB`.

`CLOCK SEND` is optional for this version. The incoming transport message starts
the recorder, while event times come from the CoreS3's monotonic microsecond
clock. These menu names are from the
[Digitakt II user manual](https://www.elektron.se/wp-content/uploads/2025/06/Digitakt-2-User-Manual_ENG_OS1.15_250625.pdf).

## Record a take

1. Power on the CoreS3 and wait for `READY`.
2. Tap `RECORD`. The screen changes to `ARMED` and says `Waiting for Digitakt
   PLAY`.
3. Press PLAY on the Digitakt. The state changes to `RECORDING` and the duration
   begins at zero.
4. Tap `STOP / SAVE` when the take is finished. Wait for `SAVED` before removing
   the card or switching off the CoreS3.

MIDI Stop (`FC`) does not save or close the file in this version. After stopping
the Digitakt, use the touchscreen's `STOP / SAVE` button so an accidental
transport stop cannot end the take.

If PLAY does nothing, check `TRANSPORT SEND`, verify that the Digitakt/MRCC route
reaches Unit MIDI IN, and look for the cyan MIDI activity dot.

## Build and upload

The project uses PlatformIO and pins the same ESP32 platform family recommended
by M5Stack for CoreS3.

```sh
pio run
pio run --target upload
pio device monitor
```

Or open this directory with the PlatformIO extension in VS Code and use its
Build and Upload buttons. USB serial monitoring runs at 115,200 baud.

Run the hardware-independent parser and MIDI-file tests on macOS/Linux with:

```sh
make test
```

## Recording format and limits

- SMF format 0, one track, 960 PPQN.
- Channel messages, running status, poly/channel pressure, pitch bend, CC/NRPN
  streams, program changes, and complete SysEx messages are preserved.
- Timing is written against a fixed 120 BPM map so elapsed seconds remain exact
  and stay aligned with a separately recorded stereo take. It does not infer a
  musical tempo map from MIDI Clock yet.
- Live-only system messages such as MIDI Clock, Start, Continue, Stop, Active
  Sensing, and Song Position are parsed but are not embedded in the `.MID` file.
- Only MIDI actually routed to the Unit is recorded. Route both transport and
  channel-event data to the recorder output on an MRCC; audio-track trigs are not
  MIDI notes unless the Digitakt is configured to transmit corresponding MIDI.
- SysEx messages larger than 32 KiB are skipped and shown as a warning.
- The file is buffered and checkpointed once per second. Always use `STOP / SAVE`
  when possible; removing power during the few milliseconds of an SD write can
  still damage the current file.

Timing-sensitive UART capture runs separately from display and SD work, and a
4,096-entry queue (one timestamped MIDI byte per entry) absorbs card-write
pauses. Any queue overflow is shown as a warning instead of being silently
ignored.
