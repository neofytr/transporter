# transporter

A minimal, bit-perfect Linux music player.

Bare-metal ALSA-direct playback. No sound server in the audio path, no resampling, no DSP, no software volume on the bit-perfect path. Wayland-native GUI with a software renderer — no GPU required.

## Features

- Bit-perfect ALSA-direct output (`hw:` device, no PipeWire / PulseAudio in path)
- Hard exclusive device ownership — kernel-level lock, no sharing
- Format support: FLAC, WAV, AIFF, MP3, Ogg Vorbis, Opus, ALAC (M4A)
- Native sample-rate switching — DAC follows the track, no resampling ever
- Gapless playback via background preloading; format mismatch falls through gracefully
- Real-time audio thread (`SCHED_FIFO` priority 80, `mlockall`, CPU affinity)
- Pipeline view — live per-stage telemetry: formats, ring fill, xrun count, RT mode, bit-perfect verdict with per-condition breakdown
- Live FFT spectrum analyser (1024-point, 32 log-spaced bars, 20 Hz–20 kHz)
- Waveform amplitude envelope overlaid on seek bar
- Time-synced LRC lyrics display
- Album art thumbnails in library (cover.jpg / cover.png probed automatically)
- SQLite-backed music library with background scanner
- Play queue with shuffle and repeat (none / one / all)
- Mini/compact mode (480×72 strip)
- MPRIS (`org.mpris.MediaPlayer2`) for media-key and D-Bus control
- Wayland-native — no X11, no GPU driver stack (wl_shm software render)

## Keyboard shortcuts

| Key | Action |
|---|---|
| Space | Play / Pause |
| ← / → | Seek −5 s / +5 s |
| Ctrl+← / Ctrl+→ | Previous / Next track |
| + / = | Volume +5% |
| − | Volume −5% |
| M | Toggle mute |
| F1–F4 | Switch view (Main / Library / Pipeline / Queue) |
| Tab | Cycle views |

XF86 media keys (play/pause, stop, next, prev, mute, vol up/down) are also handled.

## Dependencies

Install on Arch:

```
sudo pacman -S meson ninja pkg-config \
  flac mpg123 libvorbis opus \
  alsa-lib \
  wayland wayland-protocols libxkbcommon \
  dbus sdbus-cpp \
  sqlite \
  libpng libjpeg-turbo
```

Install on Debian / Ubuntu:

```
sudo apt install \
  meson ninja-build pkg-config \
  libflac-dev libmpg123-dev libvorbis-dev libopus-dev \
  libasound2-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  libdbus-1-dev libsdbus-c++-dev \
  libsqlite3-dev \
  libpng-dev libjpeg-dev
```

Dear ImGui, toml++, doctest, and the Apple ALAC decoder are bundled under `third_party/`.

## Build

```
meson setup build
ninja -C build
```

Binary: `build/transporter`.

System install:

```
sudo ninja -C build install
```

## Realtime audio (recommended)

The audio thread requests `SCHED_FIFO` priority 80 with `mlockall` and CPU affinity. Grant it without root:

```
sudo gpasswd -a "$USER" audio
```

Create `/etc/security/limits.d/99-transporter.conf`:

```
@audio  -  rtprio  95
@audio  -  memlock unlimited
```

Log out and back in. transporter detects the granted capability at startup; falls back to `SCHED_OTHER` and reports the actual mode in the Pipeline view.

## Usage

```
transporter [OPTIONS] [FILE]
```

Play a file:

```
transporter /path/to/track.flac
```

Open the library browser (no file argument):

```
transporter
```

Pin a specific DAC:

```
transporter --device hw:CARD=Topping_E70,DEV=0
```

Headless playback (no GUI, exits when done):

```
transporter --no-gui /path/to/track.flac
```

### Config file

`~/.config/transporter/config.toml`

```toml
[device]
preferred = "hw:CARD=Topping_E70,DEV=0"

[library]
roots = ["/home/user/Music"]

[dbus]
enabled = true
```

## License

GPL-3.0-or-later. See `LICENSE`.
