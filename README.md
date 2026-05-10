# transporter

A minimal, bit-perfect Linux music player.

Bare-metal ALSA-direct playback. No sound server in the audio path, no resampling, no DSP, no software volume on the bit-perfect path. Wayland-native, Hyprland-themed.

## Features

- Bit-perfect ALSA-direct output (`hw:` device, no PipeWire / PulseAudio in path)
- Hard exclusive device ownership — kernel-level lock, no sharing
- Format auto-detection: FLAC, WAV, AIFF, MP3, Ogg Vorbis, Opus, ALAC (M4A)
- Native sample-rate switching — DAC follows the track, no resampling ever
- Real-time audio thread (`SCHED_FIFO` priority 80, `mlockall`, CPU affinity)
- Hyprland theme integration — reads `~/.config/hypr/hyprland.conf` at startup
- Pipeline view — live per-stage telemetry: formats, ring fill, xrun count, RT mode, bit-perfect verdict with per-condition breakdown
- MPRIS (`org.mpris.MediaPlayer2`) for media-key and D-Bus control
- SQLite-backed library with background scanner

## Dependencies

Install on Debian / Ubuntu:

```
sudo apt install \
  meson ninja-build pkg-config \
  libflac-dev libmpg123-dev libvorbis-dev libopus-dev \
  libasound2-dev \
  libwayland-dev libwayland-egl-backend-dev wayland-protocols \
  libegl-dev libgl-dev libxkbcommon-dev \
  libdbus-1-dev \
  libsqlite3-dev
```

Dear ImGui, toml++, doctest, and the Apple ALAC decoder are bundled under `third_party/`.

## Build

```
meson setup build
ninja -C build
```

The binary is at `build/transporter`.

To install system-wide:

```
sudo ninja -C build install
```

## Realtime audio (recommended)

For lowest-jitter playback the audio thread runs `SCHED_FIFO` priority 80 with
`mlockall` and CPU affinity. The kernel does not grant unprivileged processes RT
scheduling by default. Fix on Debian / Ubuntu / Arch:

```
sudo gpasswd -a "$USER" audio
```

Create `/etc/security/limits.d/99-transporter.conf`:

```
@audio  -  rtprio  95
@audio  -  memlock unlimited
```

Log out and back in. transporter detects the granted capability at startup and runs
RT; otherwise it falls back to `SCHED_OTHER` and reports the actual scheduling mode
in the Pipeline view.

## Usage

```
transporter [OPTIONS] [FILE]
```

Play a file directly:

```
transporter /path/to/track.flac
```

Open with no file to use the library browser.

Pin a specific DAC (useful when multiple output devices are present):

```
transporter --device hw:CARD=Topping_E70,DEV=0 /path/to/track.flac
```

Run without a GUI (headless, exits when playback ends):

```
transporter --no-gui /path/to/track.flac
```

### Config file

`~/.config/transporter/config.toml` — created on first run if absent.

Minimal example:

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
