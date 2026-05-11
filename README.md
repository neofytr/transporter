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
- Play queue with shuffle and repeat (none / one / all); shows title, artist, and duration from library
- Session persistence — queue, shuffle, and repeat state survive restarts
- M3U / M3U8 playlist support in headless mode (`--no-gui`)
- Mini/compact mode (480×72 strip) with progress bar
- MPRIS (`org.mpris.MediaPlayer2`) — playback, volume, shuffle, loop status all wired; `playerctl` and status-bar applets work
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
| S | Toggle shuffle |
| R | Cycle repeat mode (off → track → playlist) |
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
# hw: string for the DAC to claim at startup. Saved automatically when you
# pick a device in the GUI; you rarely need to set this manually.
preferred = "hw:CARD=Topping_E70,DEV=0"

[audio]
# ALSA period size in milliseconds and number of periods per ring.
# Total latency ≈ period_ms × period_count. Defaults: 12 ms × 4 = ~50 ms.
period_ms = 12
period_count = 4
# RT scheduling policy: "auto" tries SCHED_FIFO, falls back to SCHED_OTHER;
# "fifo" requires RT capability (fails hard if not granted); "other" disables RT.
rt_policy = "auto"

[library]
roots = ["/home/user/Music"]
# Glob patterns to skip during scanning (e.g. hidden dirs, sample packs).
ignore_patterns = [".*", "Samples"]

[theme]
# Follow Hyprland border/gap colours from ~/.config/hypr/hyprland.conf.
follow_hyprland = true
# Override with a custom theme TOML file (disables Hyprland detection when set).
# override_file = "~/.config/transporter/theme.toml"

[ui]
# View shown at startup: "main", "library", or "pipeline".
default_view = "main"

[dbus]
enabled = true
```

## License

GPL-3.0-or-later. See `LICENSE`.
