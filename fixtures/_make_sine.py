#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerates the sine fixture WAVs.
#
# Default set (no args):
#   sine_44100_16.wav   2 s mono 44.1 kHz S16_LE 440 Hz at -12 dBFS
#   sine_44100_24.wav   same content packed as S24_3LE
#   sine_96000_24.wav   2 s mono 96 kHz S24_3LE 440 Hz at -12 dBFS
#   sine_192000_24.wav  2 s mono 192 kHz S24_3LE 440 Hz at -12 dBFS
#
# Single-shot mode for ad-hoc fixtures:
#   python3 _make_sine.py --rate 96000 --bits 24 --out path.wav
#
# Files emitted via the standard library `wave` module: a basic 44-byte
# PCM RIFF/WAVE header followed by interleaved little-endian samples. No
# LIST/INFO, no fact chunk, no padding chunks — the data chunk is the
# tail of the file. Loopback bit-perfect comparison reads the data chunk
# directly via `transporter::engine::load_wav`.

import argparse
import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))

DURATION_S = 2.0
FREQ_HZ = 440.0
DB = -12.0
AMPLITUDE = 10.0 ** (DB / 20.0)  # ~0.2512


def samples(rate_hz):
    n = int(rate_hz * DURATION_S)
    for i in range(n):
        yield AMPLITUDE * math.sin(2.0 * math.pi * FREQ_HZ * i / rate_hz)


def write_s16(path, rate_hz):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate_hz)
        frames = bytearray()
        for s in samples(rate_hz):
            v = int(round(s * 32767.0))
            if v > 32767:
                v = 32767
            if v < -32768:
                v = -32768
            frames += struct.pack("<h", v)
        w.writeframes(bytes(frames))


def write_s24_3le(path, rate_hz):
    # `wave` writes raw little-endian PCM at sampwidth=3 as S24_3LE (tightly
    # packed 3 bytes/sample). It writes a basic PCM (tag 0x0001) header.
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(3)
        w.setframerate(rate_hz)
        frames = bytearray()
        for s in samples(rate_hz):
            v = int(round(s * (2 ** 23 - 1)))
            if v > (2 ** 23 - 1):
                v = 2 ** 23 - 1
            if v < -(2 ** 23):
                v = -(2 ** 23)
            if v < 0:
                v += 1 << 24
            frames += bytes(((v >> 0) & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF))
        w.writeframes(bytes(frames))


def write_one(path, rate_hz, bits):
    if bits == 16:
        write_s16(path, rate_hz)
    elif bits == 24:
        write_s24_3le(path, rate_hz)
    else:
        raise SystemExit("unsupported bits=%d (16 or 24 only)" % bits)


def write_default_set():
    write_s16(os.path.join(HERE, "sine_44100_16.wav"), 44100)
    write_s24_3le(os.path.join(HERE, "sine_44100_24.wav"), 44100)
    write_s24_3le(os.path.join(HERE, "sine_96000_24.wav"), 96000)
    write_s24_3le(os.path.join(HERE, "sine_192000_24.wav"), 192000)
    print("wrote sine_44100_16.wav, sine_44100_24.wav, "
          "sine_96000_24.wav, sine_192000_24.wav", file=sys.stderr)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rate", type=int, default=None,
                   help="sample rate in Hz (single-shot mode)")
    p.add_argument("--bits", type=int, default=None, choices=[16, 24],
                   help="bit depth (single-shot mode)")
    p.add_argument("--out", type=str, default=None,
                   help="output path (single-shot mode)")
    args = p.parse_args()

    any_shot = args.rate is not None or args.bits is not None or args.out is not None
    if not any_shot:
        write_default_set()
        return
    if args.rate is None or args.bits is None or args.out is None:
        raise SystemExit("single-shot mode requires --rate, --bits, --out")
    write_one(args.out, args.rate, args.bits)
    print("wrote %s (%d Hz, %d bit)" % (args.out, args.rate, args.bits), file=sys.stderr)


if __name__ == "__main__":
    main()
