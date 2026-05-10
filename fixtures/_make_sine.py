#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerates the fixture WAVs:
#   sine_44100_16.wav   2 s mono 44.1 kHz S16_LE 440 Hz at -12 dBFS
#   sine_44100_24.wav   same content packed as S24_3LE
#
# Run from repo root:  python3 fixtures/_make_sine.py

import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))

RATE = 44100
DURATION_S = 2.0
FREQ_HZ = 440.0
DB = -12.0
AMPLITUDE = 10.0 ** (DB / 20.0)  # ~0.2512


def samples():
    n = int(RATE * DURATION_S)
    for i in range(n):
        yield AMPLITUDE * math.sin(2.0 * math.pi * FREQ_HZ * i / RATE)


def write_s16(path):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for s in samples():
            v = int(round(s * 32767.0))
            if v > 32767:
                v = 32767
            if v < -32768:
                v = -32768
            frames += struct.pack("<h", v)
        w.writeframes(bytes(frames))


def write_s24_3le(path):
    # `wave` writes raw little-endian PCM at sampwidth=3 as S24_3LE (tightly
    # packed 3 bytes/sample). It writes a basic PCM (tag 0x0001) header.
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(3)
        w.setframerate(RATE)
        frames = bytearray()
        for s in samples():
            v = int(round(s * (2 ** 23 - 1)))
            if v > (2 ** 23 - 1):
                v = 2 ** 23 - 1
            if v < -(2 ** 23):
                v = -(2 ** 23)
            if v < 0:
                v += 1 << 24
            frames += bytes(((v >> 0) & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF))
        w.writeframes(bytes(frames))


def main():
    write_s16(os.path.join(HERE, "sine_44100_16.wav"))
    write_s24_3le(os.path.join(HERE, "sine_44100_24.wav"))
    print("wrote sine_44100_16.wav and sine_44100_24.wav", file=sys.stderr)


if __name__ == "__main__":
    main()
