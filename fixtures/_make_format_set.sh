#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerates Phase-2 per-format fixtures from sine_44100_16.wav.
# Each fixture is 2.0 s, 44.1 kHz mono, 440 Hz at -12 dBFS — except Opus,
# which is forced to 48 kHz because libopusfile reports 48 kHz output
# regardless of the source rate.
#
# Embedded tags:
#   artist=transporter test
#   album=phase2 fixtures
#   title=sine 440Hz
#   track=1
#   date=2024
#
# Run:  bash fixtures/_make_format_set.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/sine_44100_16.wav"

if [ ! -f "$SRC" ]; then
    python3 "$HERE/_make_sine.py"
fi

META=(
    -metadata "artist=transporter test"
    -metadata "album=phase2 fixtures"
    -metadata "title=sine 440Hz"
    -metadata "track=1"
    -metadata "date=2024"
)

# AIFF: PCM big-endian by default (no `-c:a pcm_s16be` needed; AIFF muxer
# defaults to PCM). FFmpeg's AIFF output uses PCM s16be by default for s16
# input.
ffmpeg -y -loglevel error -i "$SRC" "${META[@]}" \
    -c:a pcm_s16be "$HERE/sine_44100_16.aiff"

ffmpeg -y -loglevel error -i "$SRC" "${META[@]}" \
    -c:a flac "$HERE/sine_44100_16.flac"

ffmpeg -y -loglevel error -i "$SRC" "${META[@]}" \
    -c:a alac "$HERE/sine_44100_16.m4a"

ffmpeg -y -loglevel error -i "$SRC" "${META[@]}" \
    -c:a libmp3lame -b:a 192k "$HERE/sine_44100_16.mp3"

ffmpeg -y -loglevel error -i "$SRC" "${META[@]}" \
    -c:a libvorbis -q:a 5 "$HERE/sine_44100_16.ogg"

# Opus: native 48 kHz output. libopus encoder rejects rates other than its
# fixed grid; resample on encode. Decode side will report 48 kHz.
ffmpeg -y -loglevel error -i "$SRC" -ar 48000 "${META[@]}" \
    -c:a libopus -b:a 96k "$HERE/sine_44100_16.opus"

# Phase 3 rate-switch fixture: same content resampled to 48 kHz, encoded as
# FLAC. Used pairwise with sine_44100_16.flac to exercise auto rate-switch
# between tracks.
META48=(
    -metadata "artist=transporter test"
    -metadata "album=phase3 fixtures"
    -metadata "title=sine 440Hz 48k"
    -metadata "track=1"
    -metadata "date=2024"
)
ffmpeg -y -loglevel error -i "$SRC" -ar 48000 "${META48[@]}" \
    -c:a flac "$HERE/sine_48000_16.flac"

echo "wrote: aiff flac m4a mp3 ogg opus flac@48k" >&2
