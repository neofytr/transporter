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

## License

GPL-3.0-or-later. See `LICENSE`.
