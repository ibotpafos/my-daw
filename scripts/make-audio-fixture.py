#!/usr/bin/env python3
"""Generate a quiet, original 48 kHz WAV for repeatable local playback checks."""
from pathlib import Path
import math, struct, wave
root = Path(__file__).resolve().parents[1]
path = root / 'build' / 'Audio check.wav'
path.parent.mkdir(exist_ok=True)
rate = 48000
with wave.open(str(path), 'wb') as out:
    out.setparams((2, 2, rate, 0, 'NONE', 'not compressed'))
    block = bytearray()
    for frame in range(rate * 12):
        t = frame / rate
        envelope = min(1, t / 0.05, (12 - t) / 0.1)
        frequency = [220, 261.6256, 329.6276, 293.6648][int(t) % 4]
        pulse = math.exp(-3 * (t % 1))
        sample = int(32767 * 0.025 * envelope * pulse * math.sin(2 * math.pi * frequency * t))
        block += struct.pack('<hh', sample, sample)
    out.writeframes(block)
print(path)
