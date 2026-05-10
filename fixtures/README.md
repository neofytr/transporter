# Test fixtures

Hand-generated PCM WAV files used by the engine integration and unit tests.
Regenerate with:

    python3 fixtures/_make_sine.py

| File | Rate | Channels | Format | Length | Content |
|---|---|---|---|---|---|
| `sine_44100_16.wav` | 44100 Hz | 1 | `S16_LE` (PCM) | 2.0 s | 440 Hz sine, -12 dBFS |
| `sine_44100_24.wav` | 44100 Hz | 1 | `S24_3LE` (PCM, 3 bytes/sample) | 2.0 s | 440 Hz sine, -12 dBFS |

The 24-bit fixture exists to drive the format-mismatch refusal test against a
synthetic device whose accepted format set excludes `S24_3LE`. Both files use
the basic `WAVE_FORMAT_PCM` tag (no `WAVE_FORMAT_EXTENSIBLE`); the parser
also handles `EXTENSIBLE` with PCM / IEEE_FLOAT GUIDs but no fixture exercises
that path yet.

`_make_sine.py` uses only the Python standard library (`wave`, `struct`,
`math`), so the script runs without extra dependencies on any current Linux
distribution.
