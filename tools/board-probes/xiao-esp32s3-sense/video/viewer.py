"""Reads the board's USB stream and serves it as MJPEG at http://127.0.0.1:8099/ with live stats."""
import serial, struct, threading, time, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
latest = {"jpg": None, "n": 0}; lines = []; fps = {"v": 0.0}
cond = threading.Condition()

port = {}

def reader():
    s = serial.Serial("/dev/ttyACM0", 115200, timeout=1); port["s"] = s
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
    buf = b""; times = []
    while True:
        buf += s.read(65536)
        while True:
            i = min([x for x in (buf.find(b"FRM!"), buf.find(b"TXT!")) if x >= 0], default=-1)
            if i < 0: buf = buf[-3:]; break
            if len(buf) < i + 8: buf = buf[i:]; break
            n = struct.unpack("<I", buf[i + 4:i + 8])[0]
            if n > 2_000_000: buf = buf[i + 4:]; continue
            if len(buf) < i + 8 + n: buf = buf[i:]; break
            kind, body, buf = buf[i:i + 4], buf[i + 8:i + 8 + n], buf[i + 8 + n:]
            if kind == b"TXT!":
                line = body.decode(errors="replace"); lines.append(line); print(line, flush=True)
            elif body[:2] == b"\xff\xd8":
                now = time.time(); times.append(now); times[:] = [t for t in times if now - t < 2]
                fps["v"] = len(times) / 2
                with cond:
                    latest["jpg"] = body; latest["n"] += 1; cond.notify_all()

PAGE = b"""<!doctype html><meta charset=utf-8><title>XIAO camera</title>
<body style="background:#111;color:#ddd;font:15px sans-serif;margin:16px">
<div style="margin-bottom:8px"><button onclick="fetch('/size?0')">320x240</button> <button onclick="fetch('/size?1')">640x480</button> <button onclick="fetch('/size?2')">800x600</button> <button onclick="fetch('/size?3')">1280x720</button> <button onclick="fetch('/size?4')">1600x1200</button> <button onclick="fetch('/size?5')">2048x1536</button> </div><img src="/stream" style="max-width:100%;border:1px solid #333"><pre id=s></pre>
<script>setInterval(async()=>{document.getElementById('s').textContent=await (await fetch('/stats')).text()},500)</script>"""

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == "/stream":
            self.send_response(200); self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=f"); self.end_headers()
            seen = 0
            try:
                while True:
                    with cond:
                        cond.wait_for(lambda: latest["n"] != seen, timeout=5); seen = latest["n"]; jpg = latest["jpg"]
                    if jpg:
                        self.wfile.write(b"--f\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpg) + jpg + b"\r\n")
            except Exception:
                return
        elif self.path.startswith("/size?"):
            port["s"].write(self.path[-1].encode()); self.send_response(204); self.end_headers()
        elif self.path == "/stats":
            body = (f"desktop receives {fps['v']:.1f} fps, frame {len(latest['jpg'] or b'') // 1024} KB\n" + "\n".join(lines[-12:])).encode()
            self.send_response(200); self.send_header("Content-Type", "text/plain"); self.end_headers(); self.wfile.write(body)
        else:
            self.send_response(200); self.send_header("Content-Type", "text/html"); self.end_headers(); self.wfile.write(PAGE)

threading.Thread(target=reader, daemon=True).start()
ThreadingHTTPServer(("127.0.0.1", 8099), H).serve_forever()
