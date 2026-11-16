"""Play the XIAO's speech from its USB cable on this computer, until the amplifier is fitted.
The board sends "SPK!"+len+16-bit mono 24 kHz samples; console log lines are printed as text.
    python usb_speaker.py [/dev/ttyACM0]"""
import serial, struct, subprocess, sys
port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
s = serial.Serial(port, 115200, timeout=0.05)
player = subprocess.Popen(["pw-cat", "--playback", "--rate", "24000", "--channels", "1", "--format", "s16",
                           "--latency", "60ms", "--raw", "-"], stdin=subprocess.PIPE)
buf = b""
while True:
    buf += s.read(8192)
    while True:
        i = buf.find(b"SPK!")
        if i < 0:
            cut = buf.rfind(b"\n")                       # plain console text: print whole lines
            if cut >= 0:
                sys.stdout.write(buf[:cut + 1].decode(errors="replace")); sys.stdout.flush(); buf = buf[cut + 1:]
            buf = buf[-4096:]
            break
        if i:
            sys.stdout.write(buf[:i].decode(errors="replace")); sys.stdout.flush()
        if len(buf) < i + 8:
            buf = buf[i:]; break
        n = struct.unpack("<I", buf[i + 4:i + 8])[0]
        if n > 32768 or n % 2:
            buf = buf[i + 4:]; continue
        if len(buf) < i + 8 + n:
            buf = buf[i:]; break
        player.stdin.write(buf[i + 8:i + 8 + n]); player.stdin.flush()
        buf = buf[i + 8 + n:]
