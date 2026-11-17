"""Original droid-style status sounds for a headless character (24 kHz mono WAVs).
Building blocks: sine sweeps with vibrato, quick random pitch hops (the 'droid talk'), a ring-
modulated burble, and short envelopes so nothing lingers into the microphone."""
import wave, numpy as np
SR = 24000
rng = np.random.default_rng(7)

def env(n, a=0.005, r=0.04):
    e = np.ones(n); na, nr = int(a * SR), int(r * SR)
    e[:na] = np.linspace(0, 1, na); e[-nr:] = np.linspace(1, 0, nr); return e

def sweep(f0, f1, dur, curve=1.0, vib_hz=0.0, vib_depth=0.0, shape="sine"):
    t = np.arange(int(dur * SR)) / SR
    f = f0 + (f1 - f0) * (t / dur) ** curve
    if vib_hz: f = f * (1 + vib_depth * np.sin(2 * np.pi * vib_hz * t))
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph)
    if shape == "soft-square": y = np.tanh(3 * y)
    return y * env(len(t))

def hops(freqs, step, glide=0.25, shape="sine"):
    """Pitch jumps with a short glide between them: the 'chattering' sound."""
    n = int(step * SR); f = []
    for i, fr in enumerate(freqs):
        prev = freqs[i - 1] if i else fr
        g = int(n * glide)
        f.extend(np.linspace(prev, fr, g)); f.extend([fr] * (n - g))
    f = np.array(f); ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph) if shape == "sine" else np.tanh(3 * np.sin(ph))
    return y * env(len(f))

def burble(dur, base=600, rate=28):
    t = np.arange(int(dur * SR)) / SR
    steps = rng.uniform(0.6, 1.8, int(dur * rate) + 2)
    f = base * steps[(t * rate).astype(int)]
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph) * (0.6 + 0.4 * np.sin(2 * np.pi * 7 * t))
    return y * env(len(t), 0.01, 0.08)

def click():
    n = int(0.03 * SR); y = rng.uniform(-1, 1, n) * np.exp(-np.arange(n) / (0.004 * SR))
    return np.concatenate([y, np.zeros(int(0.05 * SR)), 0.7 * y])

def gap(s): return np.zeros(int(s * SR))

SOUNDS = {
    "wake":            np.concatenate([sweep(900, 2600, 0.12, 0.6), gap(0.03), sweep(1800, 3200, 0.08)]),
    "listening-ends":  sweep(2200, 1500, 0.07),
    "thinking":        burble(0.7, 700),
    "shutter":         click(),
    "recording-start": np.concatenate([hops([1200], 0.09), gap(0.05), hops([1800], 0.12)]),
    "recording-stop":  np.concatenate([hops([1800], 0.09), gap(0.05), hops([1200], 0.12)]),
    "happy":           hops([1400, 2200, 1700, 2800, 2400, 3300], 0.06, 0.4),
    "question":        np.concatenate([hops([1300, 1600, 1250], 0.07, 0.3), sweep(1500, 3000, 0.22, 2.2)]),
    "error":           sweep(1400, 220, 0.55, 0.5, vib_hz=9, vib_depth=0.06, shape="soft-square"),
    "connected":       np.concatenate([hops([1500, 2000, 2500], 0.06, 0.2), gap(0.04), sweep(2500, 3400, 0.1)]),
    "chatter":         hops(list(rng.choice([900, 1200, 1500, 1800, 2200, 2700, 3200], 14)), 0.045, 0.35),
    "sad":             sweep(1100, 500, 0.6, 1.4, vib_hz=6, vib_depth=0.04),
}

if __name__ == "__main__":
    for name, y in SOUNDS.items():
        y = 0.5 * y / max(1e-9, np.abs(y).max())          # -6 dBFS peak: loud enough, never clips
        with wave.open(f"{name}.wav", "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
            w.writeframes((y * 32767).astype(np.int16).tobytes())
        print(f"{name:16} {len(y) / SR:.2f} s")
