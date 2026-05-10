# transporter

A minimal, bit-perfect Linux music player.

Bare-metal ALSA-direct playback. No sound server in the audio path, no resampling, no DSP, no software volume on the bit-perfect path. Wayland-native, Hyprland-themed.

## Status

Pre-implementation. Design is settled; code is on the way.

- `docs/architecture.md` — runtime design (modules, threads, public engine API).
- `docs/phases.md` — implementation phases with goals and acceptance criteria.
- `docs/spec/locked.md` — formal specification with rationale for every decision.

## Build

Pending Phase 0 — Meson + Ninja.

```
meson setup build
meson compile -C build
./build/transporter --version
```

## Realtime audio (recommended)

For lowest-jitter playback the audio thread runs `SCHED_FIFO` priority 80 with
`mlockall` and CPU affinity. By default the kernel does not grant unprivileged
processes RT scheduling. The standard fix on Debian / Ubuntu / Arch is to add
the user to the `audio` group and bump RT / memlock limits:

    sudo gpasswd -a "$USER" audio
    sudo install -m 0644 packaging/limits-transporter.conf \
         /etc/security/limits.d/99-transporter.conf

The supplied `packaging/limits-transporter.conf`:

    @audio  -  rtprio  95
    @audio  -  memlock unlimited

Log out and back in for the new group / limits to apply. transporter will
detect the granted capability at startup and run RT; otherwise it falls back
to `SCHED_OTHER` + `nice -10` and reports the actual mode in the Pipeline view.

## License

GPL-3.0-or-later. See `LICENSE`.
