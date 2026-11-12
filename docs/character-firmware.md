# Character firmware: minting your own from the Clara fork

This fork of [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (branch `clara-face`) turns a
small ESP32-S3 board with a screen, a mic and a speaker into a character's ears, voice and face.
The board does the edge of the loop: wake word, audio in and out, a face that blinks, looks
around, moves its lips to the voice, reacts to touch and sleeps on battery. Everything that
thinks runs on a host computer (the [talker](https://github.com/cwe6279/animation) voice loop with
`--satellite`: a desktop, or a headless Raspberry Pi Zero 2 W as the permanent host), which the
board reaches over Wi-Fi.

Clara is the first character. This guide is for making the second: a different face, voice,
name, temperament and timings, on the same code. It assumes you have never seen the talker
repo and have built an ESP-IDF project at least once.

## What is the board's and what is the character's

| Board (keep) | Character (replace) |
| --- | --- |
| `main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/esp32-s3-touch-amoled-1.8-v2.cc`: panel, touch, codec, power chip, buttons, the idle timer | `clara_*.c` in the same folder: 28 generated images (eye layers, lid masks, expression cuts, mouths) |
| `clara_face_display.cc/.h`: the face engine (drawing, gaze, blink, lips, touch, sleep) | `clara_face_layout.h`: generated sizes and positions |
| `main/application.cc`, `main/protocols/websocket_protocol.cc`, `main/audio/audio_service.*`: the satellite protocol additions | `main/assets/common/*.ogg` you add: reaction clips in the character's voice |
| `main/boards/common/axp2101.*`, `board.*`, `wifi_board.cc`: battery voltage in the status | `sdkconfig.defaults`: wake phrases, server address, temperament, timings |
| `scripts/build.py --wake-word custom`, `scripts/build_default_assets.py`: several wake phrases, an 8 s recognition window | `CMakeLists.txt` `PROJECT_VER`: bump it so the board accepts your build over the air |

The face engine is named after Clara but has nothing of hers in it: every number comes from the
generated layout header, every image symbol goes through `FACE_SYM(name)`, and the reactions
are chosen by a config option. A second character is a second set of generated files and a
different `sdkconfig.defaults`.

## What you need

- The board: Waveshare ESP32-S3-Touch-AMOLED-1.8 **V2** (368x448 AMOLED, ES8311 codec,
  AXP2101 power chip, a battery). Other xiaozhi boards would need their own `*_face_display`
  class; the engine assumes an LVGL display and a touch panel.
- ESP-IDF `release/v6.1` at `~/esp/esp-idf`, this repo at `~/esp/xiaozhi-esp32`, and a
  USB-C to USB-C cable (USB-A to C cables powered the board but never enumerated it).
- The talker repo, for the host and for the art tool. Its `.venv` has numpy, Pillow and pygame.
- A character in talker's format: a folder under `faces/` with `face.json`, six mouth PNGs and a
  textured eye (`eye/iris.png`, `lid-upper.png`, `lid-lower.png`, optionally `sclera.png`,
  `pupilMap.png`, `highlight.png`). Clara's `faces/clara/` is the reference.

## 1. The face

The board draws the face from layers the host renders once:

- **sclera**: the eye at rest with its iris removed, alpha = the eye disc (RGB565A8)
- **iris**: the iris and pupil as a disc that moves with the gaze (RGB565A8)
- **lid0..lid4**: what the lids cover at five openings, from open to shut; these also mask
  everything outside the eye disc (A8, drawn black)
- **emo_happy / angry / annoyed / sad**: the expression's lid cut, under the blink (A8)
- **mouth_sil / pp / ee / ah / aa / oo**: the six mouth shapes on black (RGB565), one per
  viseme id the host sends on the audio frames

The tool makes all of them, and the layout header, from the face folder:

```
cd ~/Projects/animation
.venv/bin/python tools/satellite_face_art.py faces/<name> \
    --out ~/esp/xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2 \
    --lvgl-image ~/esp/xiaozhi-esp32/scripts/Image_Converter/LVGLImage.py \
    --png-dir /tmp/<name>-art        # optional: keep the PNGs to look at
```

It uses the same eye model and lid geometry as the host face, so the board shows what the
desktop shows. The character's 16:9 canvas is scaled by `--scale` (0.6 for a 1280x720 canvas on
this panel) and centred; eye size, iris size, gaze range and positions are derived from
`face.json` and written to `<name>_face_layout.h`. LVGL's own `LVGLImage.py` writes the C
arrays, run with the ESP-IDF python env because it needs `pypng` and `lz4`.

Then point the engine at your set: in `clara_face_display.cc` change the one include,
`#include "clara_face_layout.h"`, to your header, and delete Clara's `clara_*.c` images so two
sets of symbols do not meet in the link. (Renaming the engine files themselves is cosmetic; the
class name does not appear anywhere a character would notice.)

Images are plain C arrays in the app partition. Clara's set is about 350 KB; the app partition
has roughly 8% free with her in it, so a bigger face needs the partition table changed.

**Checking the art before flashing.** With `--png-dir` you get the layers as PNGs. The masks are
black with alpha = covered. A quick way to see them as the board will: composite sclera, then
iris at `FACE_IRIS_REST`, then darken by each mask's alpha. The lid levels are spaced by how
much of the eye stays visible (all, three quarters, half, a quarter, none); do not space them
by blink amount, the lid maps close most of the eye in the first third of a blink.

## 2. Name and wake phrases

The wake word is Espressif's English speech-command model (MultiNet) listening for a list of
phrases, so no training is needed, but it is weaker than a true wake-word model and a lone
two-syllable name is its hard case. In `sdkconfig.defaults`:

```
CONFIG_CUSTOM_WAKE_WORD="clara;klara;clarra;claira;hey clara;hi clara"
CONFIG_CUSTOM_WAKE_WORD_DISPLAY="Clara"
CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=15
```

Give it the name, two or three spellings of how people actually say it (the model works from
a pronunciation of the spelling: Clara said "CLAIR-a" was only caught once "claira" was in the
list), and the "hey" and "hi" forms, which a longer phrase makes reliable. Any phrase wakes the
board and all of them show as the display name. The threshold is a percentage; 15 catches more
and false-wakes more, 25 is strict. The phrases and threshold live in the assets partition, not
the app: flash `build/generated_assets.bin` at `0x800000` after changing them.

## 3. Temperament and reaction clips

Touch reactions are the character's, not the board's. `CONFIG_CLARA_TOUCH_COMPOSED=y` (Clara,
a serious assistant): a tap is a blink, a glance at the finger and "Hm?"; a hold is a patient
look; a rub or three quick taps gets a half-lidded look, eyes drifting up, a throat-clear and
"Let's keep it professional." Off (playful): wide eyes on a tap, a contented squint on a hold,
and a slow ha-ha with a giggle. A two-second hold puts either to sleep; a tap wakes.

Motion reactions follow the same split. Composed: a blink, a glance up and "Hm?" when picked
up, the tickle look and "Gently, please." when shaken. Playful: wide eyes when picked up,
swimming eyes and "Whoa! Easy!" when shaken. Either: a blink when set down, eyes shut while
lying face down, and the gaze drifting toward the side that went up while tilted.

The clips are OGG files in `main/assets/common/`: `hm.ogg`, `mmm.ogg`, `giggle.ogg`,
`professional.ogg`, `gently.ogg`, `whoa.ogg`. The build embeds every OGG in that folder and names it
`Lang::Sounds::OGG_<NAME>` in the generated `main/assets/lang_config.h`; delete that header to
force regeneration when you add a file. The face engine plays them in `Say(...)`, only while
the board is idle (in a conversation the host owns the audio), at most once per 8 s each, with
the wake word paused for 3.5 s so the character's own voice is not taken for its name.

Making a clip in the character's voice, with ElevenLabs v3 and its audio tags (the key needs
text-to-speech only):

```
curl -s -X POST "https://api.elevenlabs.io/v1/text-to-speech/<voice id>?output_format=mp3_44100_64" \
  -H "xi-api-key: $ELEVEN_LABS_API" -H "Content-Type: application/json" \
  -d '{"text":"[clears throat] Let'"'"'s keep it professional.","model_id":"eleven_v3"}' -o professional.mp3
ffmpeg -i professional.mp3 \
  -af 'silenceremove=start_periods=1:start_threshold=-45dB,areverse,silenceremove=start_periods=1:start_threshold=-45dB,areverse' \
  -ac 1 -ar 48000 -c:a libopus -b:a 24k -frame_duration 60 -application voip professional.ogg
```

48 kHz mono Opus in OGG with 60 ms frames is what the other clips are. To change which clip a
reaction plays, or add a reaction, edit `OnTouch`, `Tickled`, `OnMotion` and the `Say` calls in
`clara_face_display.cc`; a new temperament is a new `CONFIG_` option in `main/Kconfig.projbuild`
and a branch in `Tick` where `kComposed` is tested.

## 4. Timings, server, version

In `sdkconfig.defaults` (all under "Clara face" in `menuconfig`):

| Option | Default | What |
| --- | --- | --- |
| `CONFIG_OTA_URL` | `http://talker-host.local:8030/xiaozhi/ota/` | the talker host: set it to your host's address (the board asks it for the WebSocket address at boot) |
| `CONFIG_CLARA_LISTEN_SETTLE_MS` | 15000 | mic open to eyes open at start-up (the lids part two thirds of the way in) |
| `CONFIG_CLARA_SLEEP_SECONDS` | 60 | idle on battery before the panel sleeps (ears stay on) |
| `CONFIG_CLARA_SHUTDOWN_SECONDS` | 240 | idle on battery before the board powers off; -1 never |
| `CONFIG_CLARA_FACE_SUBTITLES` | n | what was heard and said, as a line at the bottom |
| `CONFIG_CLARA_TOUCH_COMPOSED` | y | the temperament, above |
| `CONFIG_WAKE_WORD_WHILE_SPEAKING` | n | upstream's barge-in on the wake word; off, it cut replies on any noise |
| `CONFIG_STARTUP_WAKE_WORD_HINT` | n | upstream's spoken "say ... to wake me"; the face is the hint |

`CMakeLists.txt` `PROJECT_VER`: the board only takes an over-the-air image with a higher
version, so bump it for every build you mean to ship that way.

## 5. Build and flash

```
cd ~/esp/xiaozhi-esp32
source ~/esp/esp-idf/export.sh
python scripts/build.py --wake-word custom waveshare/esp32-s3-touch-amoled-1.8-v2
```

`--wake-word custom` is a fork addition that selects the MultiNet model and reads your phrase
list. The build writes `build/xiaozhi.bin` (the app), `build/generated_assets.bin` (the models,
fonts and the wake phrase list) and `build/merged-binary.bin` (everything).

First flash, which erases the saved Wi-Fi too:

```
esptool --chip esp32s3 --port /dev/ttyACM0 erase-flash
esptool --chip esp32s3 --port /dev/ttyACM0 -b 921600 --after watchdog-reset \
    write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 build/merged-binary.bin
```

After that, the app alone (Wi-Fi survives), and the assets only when the wake phrases or the
threshold changed:

```
esptool ... write-flash ... 0x20000 build/xiaozhi.bin
esptool ... write-flash ... 0x800000 build/generated_assets.bin
```

Wi-Fi setup is the board's own: it opens a hotspot `Xiaozhi-XXXX`, you join it and enter the
network at `192.168.4.1`. The same page has the server address, for a board built without it.

Things that cost an afternoon each:

- The board has a battery, so unplugging does not reset it. Download mode: hold PWR until
  dark, hold BOOT, tap PWR, release BOOT after two seconds. A short PWR press is harmless.
- A USB reset re-enumerates the port (`/dev/ttyACM0` may come back as `ttyACM1`). Open the
  serial port with DTR and RTS both asserted or you drop the chip into download mode. A udev
  rule `SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"` saves `sudo`.
- `--after watchdog-reset` is the reset that works; the default hard reset does not.
- The boot log is the fastest way to find out what the board is doing: the fork logs power
  (USB or battery, timer armed or off), "eyes open", every touch, every reaction, and the
  battery level at power-off.

## 6. The host side

The board is a client of talker's satellite server (`talker/satellite.py`), started by
`python voice_loop.py --face <name> --satellite ...`. What travels, beyond the stock xiaozhi
protocol (`docs/websocket.md`):

- The board's `hello` carries `"clara_face": true`; the server then puts a six-byte header on
  every audio frame: `0xCF`, version 1, an emotion id, and one mouth id per 20 ms of the 60 ms
  frame. The firmware carries it in the packet timestamp (bit 31 set) and fires
  `Display::SetFaceFrame` as that frame reaches the codec, so lips follow playback.
- `{"type":"llm","emotion":"happy"}` sets the expression; the server's names map in
  `SetEmotion` (talker's "annoyed" travels as "confused").
- `{"type":"face","look":"left","hold_ms":2500}` points the gaze (left, right, up, down,
  ahead, or `{"x":..,"y":..}` in -1..1, the viewer's left and up negative). On the host, saying
  "look left" is handled before the brain sees it.
- The device status the server asks for on connect carries the battery voltage next to the
  percentage; the host puts a line about the board into the character's context.
- `{"type":"event","kind":"picked_up","text":"picked up"}` goes the other way: the board
  opens the channel if it is closed and idle, and sends one event (picked up, set down, shaken,
  tilted, face down, face up, upside down), then nothing of that kind for
  `CONFIG_CLARA_MOTION_COOLDOWN_S`. The host makes it an event prompt for the character and
  drops it if she is busy. `CONFIG_CLARA_MOTION_EVENTS=n` keeps the reactions local.
- The host closes the channel 30 s after the last reply; that is when the board's idle starts.

Three timing rules the board imposes, each learned the hard way: pace the audio (the board
keeps about 1.2 s and drops the rest), hold the first frames 150 ms after `tts start`, and mute
the mic 400 ms after `listen start`. They are in the server already.

## 7. Inside the face engine

Everything that draws runs in the LVGL task on a 20 ms timer (`Tick`), in this order: a
hand-made sleep (lids drift shut, then the mouth and status icons hide; the screen stays
awake because the panel's sleep command silences the touch controller), then readiness (eyes
shut until the wake word runs and `CLARA_LISTEN_SETTLE_MS` has passed, lids parting two thirds
of the way in), then the mouth from the frame now playing, the gaze (a wandering target, or a
finger, or a look the host asked for), the blink, the touch reactions, and the expression.
`SetFaceFrame`, `SetEmotion` and `SetLook` come from other tasks and only store atomics.

Touch arrives as LVGL events on the screen object (`OnTouch`): press, pressing, release. The
power-save timer in the board file owns battery sleep (`SetPowerSaveMode`); a tap while the
panel sleeps on battery hands it a wake, so the idle countdown restarts rather than being
bypassed. `Say` plays a clip and pauses the ears; `EarsBack` restores them.

The IMU (`clara_motion.cc`) is the pattern for any sensor: its own task reads the chip (the
QMI8658, on the same I2C bus as the touch and power chips), low-passes gravity out of the
accelerometer, and decides "picked up", "set down", "shaken" and the orientation there. It
reports through two callbacks the board file wires up: an event, which the board hands to the
face (`Motion`, an atomic that `Tick` reacts to as `OnMotion`) and, sampled, to the host
(`Application::SendBoardEvent`); and the pose ten times a second (`Pose`, for the eyes).
Nothing in that task touches LVGL. The chip's axes are mapped to face axes (right, up, out of
the screen) in `MapAxes`; the task logs the pose every 10 s, so a different board is a matter
of standing it up, tilting it, reading the log and editing that function. The thresholds are
the constants at the top of the file.

## 8. Known limits

- Standby on battery is Wi-Fi plus the wake-word model, roughly 80 to 120 mA; the CPU cannot
  clock down without stopping the wake word, so the real saving is the power-off at four minutes.
- The panel's sleep command (SLPIN) also silences the touch controller. Battery sleep uses it
  anyway (the wake word and USB are the wakers there); a hand-made sleep does not.
- The AXP2101 does not answer on I2C for a while after a cold power-on; the init waits for it,
  which was the long cold start.
- Partial redraws must be aligned to even pixels on this panel or the face jerks.
- The built-in speaker is tiny; the host shapes the voice for it, and a bigger 8 ohm speaker on
  header H2 is the real fix. The mic is quiet and clean; whispers need the host's satellite
  defaults.

## 9. Toward its own project

The fork is already two things: a board port and a character engine. When this becomes its
own repository, the split is: the engine (`*_face_display.*`, the protocol additions in
`application.cc`, `websocket_protocol.cc`, `audio_service.*`, `display.h`) as a component;
per-board files as today; per-character folders holding a generated art set, a layout header,
the reaction clips and an `sdkconfig` fragment; and the art tool moving over from talker along
with the eye model it depends on.
