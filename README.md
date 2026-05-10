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

## Bit-perfect verification (snd-aloop loopback)

To prove end-to-end bit-perfect output, transporter can play through ALSA's
virtual loopback card and capture the result for byte-comparison against
the source.

    sudo modprobe snd-aloop                  # load the loopback driver
    aplay -L | grep -i loopback              # confirm Loopback card appears

The verification harness:

    ./build/tests/integration/test_bit_perfect_loopback \
        Loopback fixtures/sine_44100_16.wav
    ./build/tests/integration/test_bit_perfect_loopback \
        Loopback fixtures/sine_96000_24.wav
    ./build/tests/integration/test_bit_perfect_loopback \
        Loopback fixtures/sine_192000_24.wav

Each invocation must exit 0. A non-zero exit means the digital path mutated
the bytes between source and capture — that is a bug, file an issue.

Stress test (5 minutes; assume PipeWire has been paused on the target DAC):

    # terminal 1: drive playback
    ./build/tests/integration/test_xrun_stress hw:CARD=DAC,DEV=0 300
    # terminal 2: load the system
    stress-ng --cpu $(($(nproc) - 1)) --vm 2 --vm-bytes 256M --io 2 --timeout 5m

A tuned RT setup should report `xrun_total <= 1`; without RT, expect
multiple xruns.

## License

GPL-3.0-or-later. See `LICENSE`.
