#!/usr/bin/env python3
"""Decode a stt_bucket Ogg/Opus stream and print, per channel, the dominant
tone in each 100 ms window ('.' = silence).

Uses ffmpeg: libopusfile (and so mod_opusfile) rejects channel mapping family 255
with OP_EIMPL, while ffmpeg/libavcodec decodes it."""
import math, subprocess, sys, wave, tempfile, os
from array import array

def decode(path):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.wav")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-c:a", "pcm_s16le", "-ar", "48000", out], check=True)
        w = wave.open(out)
        ch = w.getnchannels()
        pcm = array("h", w.readframes(w.getnframes()))
    return ch, pcm

def dominant(x, rate=48000, cands=(440, 660, 880, 1320, 1760)):
    if max((abs(v) for v in x), default=0) < 500:
        return "."
    best, bf = 0, 0
    for f in cands:  # Goertzel at the candidate tones
        w = 2 * math.pi * f / rate; c = 2 * math.cos(w); s1 = s2 = 0.0
        for v in x:
            s0 = v + c * s1 - s2; s2, s1 = s1, s0
        p = s1 * s1 + s2 * s2 - c * s1 * s2
        if p > best:
            best, bf = p, f
    return str(bf)

path = sys.argv[1]
ch, pcm = decode(path)
frames = len(pcm) // ch
win = 4800  # 100 ms at 48 kHz (libopusfile always decodes at 48 kHz)
print(f"{path}: {ch} channels, {frames / 48000:.2f} s")
for c in range(ch):
    row = []
    for start in range(0, frames - win + 1, win):
        row.append(dominant([pcm[(start + i) * ch + c] for i in range(0, win, 2)], rate=24000))
    print(f"ch{c}: " + " ".join(f"{r:>4}" for r in row))
