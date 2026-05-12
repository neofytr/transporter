# Architecture

This document describes the runtime architecture of `transporter` and supplements `docs/spec/locked.md`. It covers the thread model, public engine API, project layout, and forbidden cross-cuts.

## High-level shape

`transporter` is a single binary. Internally it has five modules organized so that the audio engine has zero dependency on the frontend:

```
                ┌─────────────────────────────┐
                │           main()            │
                │   argv parse, mode dispatch │
                └──────────────┬──────────────┘
                               │ wires up
        ┌──────────┬───────────┼───────────┬──────────────┐
        ▼          ▼           ▼           ▼              ▼
    ┌───────┐  ┌─────────┐  ┌─────────┐  ┌────────┐  ┌────────────┐
    │ tui/  │  │ library │  │ config/ │  │ dbus/  │  │  hotplug/  │
    │(notc- │  │   /     │  │         │  │ MPRIS+ │  │            │
    │urses) │  └────┬────┘  └───┬─────┘  └────┬───┘  └─────┬──────┘
    └───┬───┘       │           │              │             │
        │           │           │              │             │
        └───────────┼───────────┼──────────────┼─────────────┘
                    │           │              │
                    ▼           ▼              ▼
                ┌─────────────────────────┐
                │      engine/  (lib)      │  ← no UI deps; reusable
                │  decoder, format, alsa,  │
                │  ring, rt, fsm, spectrum │
                └────────────┬─────────────┘
                             │
                       ┌─────┴─────┐
                       │ libasound │
                       └─────┬─────┘
                             │
                       ┌─────┴─────┐
                       │  USB DAC  │
                       └───────────┘
```

`engine/` is the only module on the audio path. Everything else is control or presentation. `tui/` and the daemon path share the same engine surface — when launched without a tty (or with `--daemon`), `main()` skips the notcurses init and runs MPRIS-only.

## Thread model

| Thread | Schedule | Cardinality | Responsibilities |
|---|---|---|---|
| Main | `SCHED_OTHER`, default nice | 1 | argv parse, mode dispatch, wiring, shutdown |
| TUI render | `SCHED_OTHER`, default nice | 1 (TUI mode only) | notcurses frame loop ~30 Hz active, ~5 Hz idle |
| TUI input | `SCHED_OTHER`, default nice | 1 (TUI mode only) | notcurses keypress/mouse blocking read; posts to TUI command queue |
| Audio | `SCHED_FIFO` priority 80 + `mlockall` + affinity (soft fallback to `SCHED_OTHER`) | 1 | `snd_pcm_writei` loop; consumes decoded frames from a lock-free ring; reports xrun events |
| Decode | `SCHED_OTHER`, default nice | 1 | Reads file, decodes to PCM, pushes to ring buffer |
| Library scanner | `SCHED_OTHER`, low priority | 1 + inotify | Walks library dirs, reads tags, upserts SQLite; reacts to filesystem events |
| DBus | `SCHED_OTHER` | 1 | MPRIS / custom DBus method dispatch |
| Hotplug | `SCHED_OTHER` | 1 | udev watcher for ALSA card add / remove |
| Spectrum / VU consumer | `SCHED_OTHER` | 0 or 1 (RAII attach) | FFT + dBFS smoothing for live viz; only spawned while Now Playing is active |
| Waveform pre-decode | `SCHED_OTHER` | 0 or 1 (per-track lazy) | Background decode + peak/RMS envelope build; LRU 20 |

Total typical threads: 7–9. No thread pools. No fork. No anonymous helper threads.

### Inter-thread communication

- **Control → Audio.** Lock-free SPSC ring of small command messages (`load_track`, `play`, `pause`, `stop`, `set_volume`, …). Audio thread polls non-blockingly each cycle.
- **Decode → Audio.** Lock-free SPSC ring of PCM frames. Pre-allocated at engine init, sized for ~250 ms of headroom at 192k/24/2ch. Audio thread reads; decode thread writes.
- **Audio → Spectrum/VU consumer.** Lock-free SPSC ring of channel samples. **Allocated only while a consumer handle is alive** (`engine.attach_spectrum_consumer()`). Audio thread checks an atomic `consumer_active` before writing — when no consumer attached, the tap is fully no-op.
- **Audio → Telemetry.** Lock-free SPSC ring of trace events / xrun reports. Drained on a control thread.
- **Engine ↔ TUI / DBus / etc.** Snapshot pattern: subscribers register callbacks; engine dispatches them on a worker thread (never on audio thread).

## Project layout

```
transporter/
├── src/
│   ├── engine/         # audio engine library — no UI deps
│   │   ├── alsa/       # libasound wrappers, capability discovery
│   │   ├── decoder/    # one .cpp per format
│   │   ├── format/     # negotiation, mismatch checking
│   │   ├── ring/       # lock-free SPSC ring buffer
│   │   ├── rt/         # RT thread setup, mlockall, affinity, fallback
│   │   ├── trace/      # lock-free trace buffer
│   │   ├── spectrum/   # RAII-attached audio-thread tap for viz consumers
│   │   ├── device/     # DeviceFingerprint, hotplug glue
│   │   ├── fsm.cpp     # state machine
│   │   └── engine.cpp  # public API impl
│   ├── library/        # SQLite tag DB + scanner thread + inotify watcher
│   │   ├── schema.cpp  # DDL + migrations
│   │   ├── scanner.cpp # walk + upsert + inotify
│   │   └── queries.hpp # all SQL strings (centralized)
│   ├── tui/            # notcurses-driven terminal frontend
│   │   ├── notcurses_ctx.cpp/hpp   # init/teardown, capability probe
│   │   ├── app.cpp/hpp             # top-level state, event loop, page registry
│   │   ├── input.cpp/hpp           # vi-modal keymap, command palette
│   │   ├── theme.cpp/hpp           # dominant-color accent extraction
│   │   ├── session.cpp/hpp         # save/restore queue + position + page
│   │   ├── components/             # player_bar, seekbar, spectrum, vu_meter,
│   │   │                           # cover, transport, list, toast, banner
│   │   ├── pages/                  # library, album_detail, queue, now_playing,
│   │   │                           # pipeline, settings, search_overlay,
│   │   │                           # dac_popup, help_overlay
│   │   └── dsp/                    # waveform_envelope, spectrum_consumer, vu_consumer
│   ├── dbus/           # MPRIS + custom interface
│   ├── hotplug/        # libudev watcher
│   ├── config/         # TOML config loader
│   └── main.cpp        # argv parsing, mode dispatch, wiring
├── include/
│   └── transporter/
│       ├── engine/     # public engine headers
│       │   ├── engine.hpp
│       │   ├── device.hpp
│       │   ├── format.hpp
│       │   ├── error.hpp
│       │   ├── ring.hpp
│       │   ├── rt.hpp
│       │   ├── telemetry.hpp
│       │   └── trace.hpp
│       ├── library/
│       │   └── library.hpp
│       ├── hotplug/
│       │   └── monitor.hpp
│       ├── config/
│       │   └── config.hpp
│       └── dbus/
│           └── service.hpp
├── tests/
│   ├── unit/
│   ├── integration/    # registered with `meson test`; hardware-gated tests SKIP-77 gracefully
│   └── fixtures/       # known-good audio files at various rates / depths
├── docs/
│   ├── spec/
│   ├── architecture.md (this)
│   └── phases.md
├── third_party/        # vendored deps (alac, doctest, kissfft, tomlpp)
├── packaging/
├── meson.build
├── meson_options.txt
├── LICENSE
└── README.md
```

## Public engine API (sketch)

The engine module exposes a small surface. TUI / DBus / scripts use only this. Headers under `include/transporter/engine/`.

```cpp
// error.hpp
namespace transporter::engine {
    struct Error {
        enum Code {
            DeviceBusy,        // -EBUSY: another app has the DAC
            DeviceMissing,     // -ENODEV: DAC unplugged
            FormatRefused,     // file's native format outside DAC capability
            DecoderError,      // file unreadable / corrupt
            NotPermitted,      // RT setup failed and policy is hard-RT
            Internal,
        };
        Code code;
        std::string message;
    };
}

// device.hpp
namespace transporter::engine {
    struct DeviceId {
        std::string alsa_name;                  // "hw:CARD=USBDAC,DEV=0"
        std::optional<std::string> usb_serial;
    };

    struct DeviceCapability {
        std::vector<int> supported_rates;
        std::vector<SampleFormat> supported_formats;
        int min_channels, max_channels;
        bool has_hardware_volume;
    };

    std::vector<DeviceId> list_devices();
    std::expected<DeviceCapability, Error> probe(const DeviceId&);
}

// engine.hpp
namespace transporter::engine {
    enum class State { Idle, Loading, Playing, Paused, Error, Disconnected };

    struct PlaybackInfo {
        int rate;
        SampleFormat format;
        int channels;
        std::chrono::milliseconds position;
        std::chrono::milliseconds duration;
        bool bit_perfect;     // current effective state
        bool rt_active;       // SCHED_FIFO actually granted?
    };

    class Engine {
    public:
        static std::expected<std::unique_ptr<Engine>, Error> create(DeviceId);
        ~Engine();

        // Transport (control-thread, non-blocking)
        std::expected<void, Error> load(std::filesystem::path);
        std::expected<void, Error> play();
        void pause();
        void stop();
        void seek(std::chrono::milliseconds);

        // Volume — bit-perfect mode disables digital path
        void set_hw_volume_pct(int);             // 0–100, no-op if !has_hardware_volume
        int  get_hw_volume_pct() const;
        void set_digital_volume_active(bool);    // engaging digital volume downgrades bit-perfect verdict
        bool digital_volume_active() const;

        // State
        State state() const;
        std::optional<PlaybackInfo> playback_info() const;
        PipelineSnapshot pipeline_snapshot() const;

        // Live-viz tap (RAII-attached spectrum/VU consumer)
        struct SpectrumHandle { /* opaque; destructor releases the ring */ };
        SpectrumHandle attach_spectrum_consumer();

        // Subscriptions (callbacks fire on a worker thread, never RT)
        struct Event { /* state change, xrun, hotplug, ... */ };
        using EventCallback = std::function<void(const Event&)>;
        SubscriptionId subscribe(EventCallback);
        void unsubscribe(SubscriptionId);
    };
}
```

The exact shape will evolve, but the *seams* should not: Engine is owned, transport methods return `std::expected`, subscribers go to a worker thread, viz consumers are RAII-attached so they cannot leak audio-thread cost.

## Data flows

### Track load

```
TUI → Engine::load(path)
    → state := Loading
    → pick decoder by extension / magic bytes
    → Decode thread: open file, read header, fill PCM ring with first frames
    → Format: check decoder's native format ∈ DAC capability
    → If mismatch and resampler disabled:
            state := Error
            emit FormatRefused
            return
    → If first track or rate-change:
            snd_pcm_drop  / snd_pcm_close  / snd_pcm_open(new params)
    → Audio thread: snd_pcm_prepare; ready
    → state := Paused
    → emit TrackLoaded(track_info)
```

### Playback

```
TUI → Engine::play()
    → state := Playing
    → Audio thread: enter snd_pcm_writei loop; pull frames from ring
    → If spectrum consumer attached: tap channel-0 samples into the spec ring
    → Decode thread: keep ring fed; on EOF, mark stream-complete
    → On stream-complete + ring drained:
            state := Idle
            emit TrackEnded
```

### Gapless transition (same rate)

```
On Engine::load(new_path) when new.rate == current.rate AND gapless_pending:
    Decode thread swaps to new file without ALSA close
    Snapshot: gapless_pending = false; pipeline counters carry over
    Audio thread continues writei without re-prepare
    Inaudible boundary (≤ 1 ms gap)
```

### Format change between tracks

```
On Engine::load(new_path) when new.rate != current.rate:
    snd_pcm_drop(handle)        // discard pending frames
    snd_pcm_close(handle)
    handle = snd_pcm_open(hw:..., new.rate, new.format)
    snd_pcm_prepare(handle)
    // short silence here is the DAC re-locking; we do not fill it
```

### Hotplug — DAC unplugged mid-playback

```
Path A: hotplug/ thread sees udev REMOVE matching our DeviceFingerprint
    → engine: state := Disconnected
    → emit DeviceDisconnected
    → audio thread idles

Path B: audio thread's snd_pcm_writei returns -ENODEV (USB unplugged faster than udev)
    → check_run_finish_ classifies the error as device-gone
    → engine: state := Disconnected (idempotent with Path A)
    → emit DeviceDisconnected

On reconnect (either path):
    hotplug/ thread sees udev ADD matching our DeviceFingerprint
    → emit DeviceReturned
    → engine: re-probe capabilities, re-open hw:, snd_pcm_prepare
    → state := previous (Playing or Paused)
```

### MPRIS Play command

```
DBus thread receives org.mpris.MediaPlayer2.Player.Play
    → Engine::play()        // same path as TUI
```

## RT discipline

The audio thread is the **only** RT-disciplined thread.

**Forbidden in the audio thread:**
- Heap allocation (`new`, `malloc`, mutating `std::string`, growing `std::vector`)
- Lock acquisition that can block (mutexes, futexes)
- Syscalls outside the RT-safe list:
  - **Allowed:** `snd_pcm_writei`, `snd_pcm_avail_update`, `snd_pcm_recover`, `clock_gettime(CLOCK_MONOTONIC)`, `pthread_self`
  - **Forbidden:** `open`, `close`, `read`, `write` (other than ALSA), `mmap`, anything touching `/proc` or `/sys`
- Logging via stdio — use the lock-free trace ring
- C++ exceptions — use sentinel returns or `std::expected`

**Required:**
- All buffers allocated at init or via RAII-attached handles (spectrum consumer).
- Memory locked via `mlockall(MCL_CURRENT | MCL_FUTURE)` at engine create.
- One `std::atomic<int>` per state value the audio thread reads (so the control thread can update without locks).

The trace ring is drained by a "telemetry" capability of the control thread, not by the audio thread. The spectrum ring (when present) is drained by the spectrum consumer thread — never on the audio thread.

## Module boundaries (and forbidden cross-cuts)

- `engine/` does NOT depend on `tui/`, `library/`, `dbus/`, `hotplug/`. It is reusable as a library.
- `tui/` depends on `engine/`, `library/`. Does NOT touch `dbus/` or `hotplug/` directly (engine is the only seam).
- `dbus/` depends on `engine/`. Does NOT touch `tui/` or `library/`.
- `hotplug/` depends on `engine/` (it produces device events the engine consumes).
- `library/` depends on the decoder trait from `engine/decoder/` for tag reading. Does NOT depend on the playback path.
- `config/` is pure: input is TOML text, output is a config struct.
- `main.cpp` wires everything together. It is the ONLY place that touches every module.

## Error handling

- Public engine API uses `std::expected<T, Error>` everywhere mutation can fail.
- Internal engine code may use `std::expected` or a strongly-typed sentinel return.
- **No exceptions cross the engine API boundary.** Inside the engine, exceptions are forbidden on the audio path; allowed on the control path only if they don't escape.
- The audio thread translates negative ALSA error codes to engine `Error::Code` values via a lookup table.

## Configuration

`~/.config/transporter/config.toml` (XDG Base Directory). Provisional schema:

```toml
[device]
preferred = "USBDAC"        # USB serial or ALSA card name; engine picks first match

[audio]
period_ms = 12              # override per-period wall-clock
period_count = 4            # override period count
rt_policy = "auto"          # "auto" | "fifo" | "other"
rt_priority = 80
rt_affinity = -1            # -1 = no affinity; otherwise core index

[library]
paths = ["~/Music"]         # multiple roots supported
ignore = [".*", "*.tmp"]

[ui]
mouse = true                # --no-mouse overrides
desktop_notifications = false   # libnotify integration
default_enter = "replace"   # "replace" | "append"
spectrum = true             # show spectrum on Now Playing
vu_meter = true             # show VU on Now Playing
pipeline_refresh_hz = 10

[dbus]
mpris_art_forward = true    # write embedded picture to /tmp for mpris:artUrl
```

Final schema is locked in `docs/spec/locked.md` once the TUI overhaul concludes.

---

## See also

- `CLAUDE.md` — design intent and public-repo discipline (top-level)
- `docs/spec/locked.md` — formal spec with rationale
- `docs/spec/open-questions.md` — unresolved items
- `docs/phases.md` — implementation phase plan (T0–T12 overhaul + forward engine work)
