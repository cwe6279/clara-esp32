# Seeed XIAO ESP32S3 Sense: probes and calibration

A candidate board for a headless character: camera and microphone, no display, no speaker.
These four small ESP-IDF projects check its hardware and calibrate it before a board class is
written. Each is standalone: `idf.py set-target esp32s3 && idf.py -p /dev/ttyACM0 flash`.
Back up the factory image first (`esptool read-flash 0 0x800000 stock.bin`).

| Project | What it does |
|---|---|
| `probe` | Prints chip, flash, PSRAM, the camera sensor's ID, I2C devices, mic level, microSD |
| `photo` | Takes one full-resolution JPEG at each reset and prints it as base64 over USB |
| `video` | Streams JPEG frames over USB; `viewer.py` shows them at http://127.0.0.1:8099/ with size buttons |
| `mic`   | Streams the microphone over USB after a 100 Hz high-pass and a fixed gain; `record.py` runs a cued far / medium / close test |
| `speaker` | MAX98357A bring-up: a chime, then 440 Hz at -30, -20, -12 and -6 dBFS; then plays audio sent over USB (`play.py clip.wav`) |

## What the board has (measured 2026-11-16)

- ESP32-S3 rev 0.2, two cores at 240 MHz, 8 MB octal PSRAM, 8 MB flash.
- 2.4 GHz Wi-Fi and Bluetooth 5 LE only. No Classic Bluetooth, so it cannot send audio to a
  Bluetooth speaker; speech out needs an I2S amplifier (e.g. MAX98357A) on free header pins.
- Camera: OV3660 (3 MP) at I2C 0x3C. Ordinary lens of about 65 to 70 degrees, no fisheye; sharp
  from arm's length out, soft up close; cool white balance indoors.
- Microphone: PDM, clock GPIO42, data GPIO41.
- microSD slot (SPI: CS 21, SCK 7, MISO 8, MOSI 9).

Camera pins (esp32-camera): XCLK 10, SIOD 40, SIOC 39, D7..D0 = 48, 11, 12, 14, 16, 18, 17, 15,
VSYNC 38, HREF 47, PCLK 13, no PWDN or RESET.

## Camera

Frame rate the camera delivers on the board (JPEG quality 12):

| Size | fps | Average frame |
|---|---|---|
| 320 x 240 | 27.8 | 5 KB |
| 640 x 480 | 23.5 | 19 KB |
| 800 x 600 | 24.1 | 26 KB |
| 1280 x 720 | 17.4 | 43 KB |
| 1600 x 1200 | 13.9 | 94 KB |
| 2048 x 1536 | 11.1 | 147 KB |

The built-in USB link carries about 1 MB/s: all of VGA's 24 fps, about 5.5 fps at full size.

For a character's looks, **1600 x 1200** is the choice. 1280 x 720 is a crop of the sensor and
cuts off the top and bottom of the view; 1600 x 1200 shows the full 4:3 frame. A dense 31B
Gemma 4 on a LAN Ollama server described either size in the same time, because it resizes
every picture itself (about 460 vs 550 image tokens):

| Prompt | Time per look (model loaded) |
|---|---|
| Two or three sentences, thinking on | about 12 s (some 450 hidden tokens) |
| Two or three sentences, `think: false` | about 2 s |
| An itemized list (people, held, objects, text, setting), `think: false` | about 4 s |

The short prompt drops details; the itemized one names nearly every object in view. Keep the
model loaded: a cold load is about 17 s, and Ollama unloads after 5 idle minutes by default.

## Microphone calibration

The raw microphone is quiet and dominated by rumble: speech at 30 cm around -58 dBFS, and 86 %
of the energy below 100 Hz. The `mic` project fixes that on the board with a 2nd-order
Butterworth high-pass at 100 Hz followed by a fixed gain (default 30 dB; send one byte 0..60
over USB to change it). Reading "Hey Clara, what's on my calendar today?" at three distances:

| Where | rms | Peak | Above the quiet room |
|---|---|---|---|
| Quiet room | -51.2 dBFS | -38.2 | 0 dB |
| Far, about 4 m | -36.3 dBFS | -15.5 | 19.8 dB |
| Medium, about 1.5 m | -33.5 dBFS | -11.6 | 22.3 dB |
| Close, about 30 cm | -26.3 dBFS | -3.3 | 29.5 dB |

30 dB is the most the fixed gain should be: close speech peaks 3 dB short of clipping.

Next, one piece at a time: settle the gain (27 dB, or 30 dB with a limiter); then a light
automatic gain capped at about +10 dB for far speech, slow-moving and held during silence.
Echo cancellation and interference come once the board has a speaker; the automatic gain must
then sit after echo cancellation.

## Speaker: MAX98357A (wiring planned, not yet tested)

The board has no sound output, so speech goes out through a MAX98357A I2S class-D amplifier.
The firmware already drives this pairing (PDM mic plus I2S amplifier, `NoAudioCodecSimplexPdm`).

| MAX98357A | XIAO pin | GPIO |
|---|---|---|
| BCLK | D0 | 1 |
| LRC | D1 | 2 |
| DIN | D3 | 4 |
| VIN | 5V | - |
| GND | GND | - |
| SD, GAIN | not connected (mono mix of left and right, 9 dB gain) | - |

D2 (GPIO3) is skipped because it is a strapping pin; D4/D5 stay free for I2C, D6/D7 for serial,
and D8 to D10 belong to the microSD slot. The speaker goes across the amplifier's + and -
outputs, never to ground. From 5 V the amplifier gives up to about 3 W into 4 ohm and 1.4 W
into 8 ohm at full scale; for a small speaker keep the level at -6 dBFS or below (about a
quarter of that power) or feed VIN from 3V3.

The speaker chosen is a 50 mm full-range driver, 8 ohm, 3 W: above anything this amplifier can
deliver into 8 ohm, so VIN at 5 V and full scale are safe. A bare driver sounds thin; a small
closed box or even a baffle around it gives the voice its low end.

## Monitoring mode (design, not built)

For a watching job rather than conversation (noticing someone arrive, minding a room), the
board itself decides when a picture is worth sending. For the assistant, looks stay on demand.

1. The camera keeps running at 1600 x 1200. A couple of times a second the board decodes one
   frame at 1/8 scale (esp32-camera's JPEG decoder) and reduces it to a 32 x 24 grey grid.
2. It compares that grid with the one from the last picture sent. The talker's host-side gate
   gives the scale: a still room about 1 %, a raised hand 4 to 8 %, a person walking in 10 %+.
3. Past the threshold it sends a full picture, with three guards:
   - **Settle first:** wait until the movement has stopped for about 0.5 s, so the picture is
     not blurred mid-move.
   - **Minimum gap:** about 10 s between pictures, so a busy scene cannot trigger a stream.
   - **Patterns, not brightness:** normalise each grid's brightness and contrast before
     comparing, so a lamp or an exposure change does not count.

Cost: a few percent of one core and almost no memory; nothing crosses Wi-Fi and no model is
called until there is a picture. The threshold needs one tuning session on the board in the
room where it will sit: walk in, sit down, hold something up, leave, and read the scores.
