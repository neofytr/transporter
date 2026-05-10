# Test fixtures

Hand-generated audio files used by engine integration and unit tests.

Two generators live here:

- `_make_sine.py` — base WAV fixtures (Phase 1). Standard library only.
- `_make_format_set.sh` — Phase-2 per-format fixtures, derived from
  `sine_44100_16.wav` via `ffmpeg`.

Regenerate everything with:

    python3 fixtures/_make_sine.py
    bash   fixtures/_make_format_set.sh

| File | Rate | Channels | Decoder format | Length | Content |
|---|---|---|---|---|---|
| `sine_44100_16.wav` | 44100 | 1 | `S16_LE` | 2.0 s | 440 Hz, -12 dBFS |
| `sine_44100_24.wav` | 44100 | 1 | `S24_3LE` | 2.0 s | 440 Hz, -12 dBFS |
| `sine_96000_24.wav` | 96000 | 1 | `S24_3LE` | 2.0 s | 440 Hz, -12 dBFS |
| `sine_192000_24.wav` | 192000 | 1 | `S24_3LE` | 2.0 s | 440 Hz, -12 dBFS |
| `sine_44100_16.aiff` | 44100 | 1 | `S16_LE` (BE on disk; decoder swaps) | 2.0 s | 440 Hz |
| `sine_44100_16.flac` | 44100 | 1 | `S16_LE` | 2.0 s | 440 Hz |
| `sine_44100_16.m4a` | 44100 | 1 | `S16_LE` (ALAC) | 2.0 s | 440 Hz |
| `sine_44100_16.mp3` | 44100 | 1 | `S16_LE` (MPEG L3, 192 kbps) | ~2.0 s | 440 Hz |
| `sine_44100_16.ogg` | 44100 | 1 | `FLOAT_LE` (Vorbis q5) | ~2.0 s | 440 Hz |
| `sine_44100_16.opus` | 48000 | 1 | `FLOAT_LE` (Opus 96 kbps) | ~2.0 s | 440 Hz |

Tags embedded by `_make_format_set.sh`:

    artist  = transporter test
    album   = phase2 fixtures
    title   = sine 440Hz
    track   = 1
    date    = 2024

WAV files do not carry tags — the WAV writer used by `_make_sine.py` does
not emit a `LIST/INFO` chunk. The unit test for WAV asserts empty tags.

The 24-bit WAVs at 44.1k / 96k / 192k feed the bit-perfect loopback harness
(see `tests/integration/bit_perfect_loopback.cpp`). `sine_44100_24.wav` also
drives the format-mismatch refusal test against a synthetic device whose
accepted format set excludes `S24_3LE`.

`_make_sine.py` accepts `--rate` and `--bits` for ad-hoc fixtures:

    python3 fixtures/_make_sine.py --rate 88200 --bits 24 --out /tmp/sine.wav

The Opus fixture is 48 kHz because libopusfile always reports its output
at 48 kHz (Opus is internally 48 kHz). The decoder declares 48 kHz; the
engine's format-match step will refuse to play if the DAC won't accept it.
This is by design — bit-perfect promises mean no silent resampling.
