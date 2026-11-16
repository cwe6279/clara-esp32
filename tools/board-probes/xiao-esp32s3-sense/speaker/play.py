"""Play a WAV file through the board's speaker: python play.py clip.wav [gain_db]
Resamples to 16 kHz mono with ffmpeg if needed (ffmpeg must be installed)."""
import serial, struct, subprocess, sys, time
path = sys.argv[1]; gain = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-ac", "1", "-ar", "16000",
                      "-af", f"volume={gain}dB", "-f", "s16le", "-"], capture_output=True, check=True).stdout
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.1)
t0 = time.time(); chunk = 640                    # 20 ms per packet
for i in range(0, len(raw), chunk):
    part = raw[i:i + chunk]
    s.write(b"PCM!" + struct.pack("<I", len(part)) + part)
    ahead = (i + chunk) / 32000 - (time.time() - t0)
    if ahead > 0.25: time.sleep(ahead - 0.25)    # stay about 0.25 s ahead of playback
print(f"sent {len(raw) / 32000:.1f} s of audio")
