"""Record the board's mic at three distances with spoken cues; write WAVs and print levels."""
import serial, struct, subprocess, sys, time, wave, threading
import numpy as np
OUT = sys.argv[1]
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
pcm = bytearray(); marks = []; lock = threading.Lock(); stop = False
def reader():
    buf = b""
    while not stop:
        buf += s.read(8192)
        while True:
            i = buf.find(b"PCM!")
            if i < 0 or len(buf) < i + 8: buf = buf[max(0, len(buf) - 7):] if i < 0 else buf; break
            n = struct.unpack("<I", buf[i+4:i+8])[0]
            if n > 4096: buf = buf[i+4:]; continue
            if len(buf) < i + 8 + n: break
            with lock: pcm.extend(buf[i+8:i+8+n])
            buf = buf[i+8+n:]
threading.Thread(target=reader, daemon=True).start()
def say(t): subprocess.run(["spd-say", "-w", "-r", "-10", t])
def pos(): 
    with lock: return len(pcm) // 2
time.sleep(2.5)
say("Microphone test. Stay quiet for five seconds.")
time.sleep(1.0)
a = pos(); time.sleep(5); marks.append(("quiet", a, pos()))
for name, where in (("far", "Go far away, across the room, about four meters."),
                    ("medium", "Now move to about one and a half meters."),
                    ("close", "Now come close, about thirty centimeters.")):
    say(where + " After the beep, read the sentence, twice, in your normal voice.")
    time.sleep(7)
    subprocess.run(["pw-play", "/usr/share/sounds/freedesktop/stereo/bell.oga"], stderr=subprocess.DEVNULL)
    time.sleep(0.6)
    a = pos(); time.sleep(8); marks.append((name, a, pos()))
say("Done. Thank you.")
stop = True
x = np.frombuffer(bytes(pcm), dtype=np.int16).astype(np.float64)
x -= np.mean(x)
def db(v): return 20 * np.log10(max(v, 1e-9) / 32768)
def write(name, seg):
    with wave.open(f"{OUT}/mic-{name}.wav", "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(seg.astype(np.int16).tobytes())
noise = None
print("levels after the board's filter and gain")
print(f"{'clip':8} {'rms dBFS':>9} {'peak dBFS':>10} {'loud 10% dBFS':>14} {'above quiet':>12} {'clipped':>8}")
for name, a, b in marks:
    seg = x[a:b]; write(name, seg)
    frames = seg[: len(seg) // 320 * 320].reshape(-1, 320); fr = np.sqrt((frames ** 2).mean(1))
    loud = np.percentile(fr, 90); rms = np.sqrt((seg ** 2).mean())
    if name == "quiet": noise = rms
    clip = int((np.abs(seg) >= 32700).sum())
    print(f"{name:8} {db(rms):9.1f} {db(np.abs(seg).max()):10.1f} {db(loud):14.1f} {db(loud) - db(noise):11.1f}dB {clip:8}")
write("all", x)
