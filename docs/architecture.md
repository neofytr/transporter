# Architecture

This document describes the runtime architecture of `transporter` and supplements `docs/spec/locked.md`. It covers the thread model, public engine API, project layout, and forbidden cross-cuts.

## High-level shape

`transporter` is a single binary. Internally it has six modules organized so that the audio engine has zero dependency on UI:

```
                ┌─────────────────────────────┐
                │           main()            │
                │   argv parse, mode dispatch │
                └──────────────┬──────────────┘
                               │ wires up
        ┌──────────┬───────────┼───────────┬──────────────┐
        ▼          ▼           ▼           ▼              ▼
    ┌───────┐  ┌─────────┐  ┌──────┐  ┌────────┐  ┌────────────┐
    │ gui/  │  │ library │  │theme/│  │ dbus/  │  │  hotplug/  │
    │       │  │   /     │  │      │  │ MPRIS+ │  │            │
    └───┬───┘  └────┬────┘  └───┬──┘  └────┬───┘  └─────┬──────┘
        │           │           │          │             │
        └───────────┼───────────┼──────────┼─────────────┘
                    │           │          │
                    ▼           ▼          ▼
                ┌─────────────────────────┐
                │      engine/  (lib)      │  ← no UI deps; reusable
                │  decoder, format, alsa,  │
                │  ring, rt, fsm           │
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

`engine/` is the only module on the audio path. Everything else is control or presentation.

## Thread model

| Thread | Schedule | Cardinality | Responsibilities |
|---|---|---|---|
| Main / UI | `SCHED_OTHER`, default nice | 1 | Wayland event loop, Dear ImGui frame loop, user input |
| Audio | `SCHED_FIFO` priority 80 + `mlockall` + affinity (soft fallback to `SCHED_OTHER`) | 1 | `snd_pcm_writei` loop; consumes decoded frames from a lock-free ring; reports xrun events |
| Decode | `SCHED_OTHER`, default nice | 1 | Reads file, decodes to PCM, pushes to ring buffer |
| Library scanner | `SCHED_OTHER`, low priority | 1 | Walks library dirs, reads tags, upserts SQLite |
| DBus | `SCHED_OTHER` | 1 | MPRIS / custom DBus method dispatch |
| Hotplug | `SCHED_OTHER` | 1 | udev watcher for ALSA card add / remove |

Total typical threads: 6. No thread pools. No fork. No anonymous helper threads.

### Inter-thread communication

- **Control → Audio.** Lock-free SPSC ring of small command messages (`load_track`, `play`, `pause`, `stop`, `set_volume`, …). Audio thread polls non-blockingly each cycle.
- **Decode → Audio.** Lock-free SPSC ring of PCM frames. Pre-allocated at engine init, sized for ~250 ms of headroom at 192k/24/2ch. Audio thread reads; decode thread writes.
- **Audio → Telemetry.** Lock-free SPSC ring of trace events / xrun reports. Drained on the control thread.
- **Engine ↔ GUI / DBus / etc.** Snapshot pattern: subscribers register callbacks; engine dispatches them on a worker thread (never on audio thread).

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
│   │   ├── device/     # DeviceFingerprint, hotplug glue
│   │   ├── fsm.cpp     # state machine
│   │   └── engine.cpp  # public API impl
│   ├── library/        # SQLite tag DB + scanner thread
│   │   ├── schema.cpp  # DDL + migrations
│   │   ├── scanner.cpp # walk + upsert
│   │   └── queries.cpp # all SQL strings (centralized)
│   ├── gui/            # Dear ImGui platform + widgets
│   │   ├── platform/   # Wayland + EGL + GL backend for ImGui
│   │   ├── views/      # main, library, settings, error
│   │   └── widgets/    # custom widgets (file picker, etc.)
│   ├── theme/          # Hyprland config parser → theme model
│   ├── dbus/           # MPRIS + custom interface
│   ├── hotplug/        # libudev watcher
│   ├── config/         # TOML config loader
│   └── main.cpp        # argv parsing, mode dispatch, wiring
├── include/
│   └── transporter/
│       └── engine/     # public engine headers
│           ├── engine.hpp
│           ├── device.hpp
│           ├── track.hpp
│           ├── format.hpp
│           ├── error.hpp
│           └── events.hpp
├── tests/
│   ├── unit/
│   ├── integration/
│   └── fixtures/       # known-good audio files at various rates / depths
├── docs/
│   ├── spec/
│   ├── architecture.md (this)
│   └── phases.md
├── third_party/        # vendored deps (Dear ImGui)
├── packaging/
│   └── transporter.desktop
├── meson.build
├── meson_options.txt
├── LICENSE
└── README.md
```

## Public engine API (sketch)

The engine module exposes a small surface. UI / DBus / scripts use only this. Headers under `include/transporter/engine/`.

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
        void set_volume(float linear);   // 0.0–1.0
        float volume() const;
        void set_bit_perfect_mode(bool);
        bool bit_perfect_mode() const;

        // State
        State state() const;
        std::optional<PlaybackInfo> playback_info() const;

        // Subscriptions (callbacks fire on a worker thread, never RT)
        struct Event { /* state change, xrun, hotplug, ... */ };
        using EventCallback = std::function<void(const Event&)>;
        SubscriptionId subscribe(EventCallback);
        void unsubscribe(SubscriptionId);
    };
}
```

The exact shape will evolve, but the *seams* should not: Engine is owned, transport methods return `std::expected`, subscribers go to a worker thread.

## Data flows

### Track load

```
GUI → Engine::load(path)
    → state := Loading
    → pick decoder by extension / magic bytes
    → Decode thread: open file, read header, fill PCM ring with first frames
    → Format: check decoder's native format ∈ DAC capability
    → If mismatch:
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
GUI → Engine::play()
    → state := Playing
    → Audio thread: enter snd_pcm_writei loop; pull frames from ring
    → Decode thread: keep ring fed; on EOF, mark stream-complete
    → On stream-complete + ring drained:
            state := Idle
            emit TrackEnded
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
Audio thread: snd_pcm_writei returns -ENODEV (or similar)
    → state := Disconnected
    → emit DeviceDisconnected
    → audio thread idles

hotplug/ thread: udev sees ADD event matching our DeviceFingerprint
    → emit DeviceReturned
    → engine: re-probe capabilities, re-open hw:, snd_pcm_prepare
    → state := previous (Playing or Paused)
```

### MPRIS Play command

```
DBus thread receives org.mpris.MediaPlayer2.Player.Play
    → Engine::play()        // same path as GUI
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
- All buffers allocated at init.
- Memory locked via `mlockall(MCL_CURRENT | MCL_FUTURE)` at engine create.
- One `std::atomic<int>` per state value the audio thread reads (so the control thread can update without locks).

The trace ring is drained by a "telemetry" capability of the control thread, not by the audio thread.

## Module boundaries (and forbidden cross-cuts)

- `engine/` does NOT depend on `gui/`, `library/`, `theme/`, `dbus/`, `hotplug/`. It is reusable as a library.
- `gui/` depends on `engine/`, `library/`, `theme/`. Does NOT touch `dbus/` or `hotplug/`.
- `dbus/` depends on `engine/`. Does NOT touch `gui/` or `library/`.
- `hotplug/` depends on `engine/` (it produces device events the engine consumes).
- `library/` depends on the decoder trait from `engine/decoder/` for tag reading. Does NOT depend on the playback path.
- `theme/` is pure: input is `hyprland.conf` text, output is a theme struct. No engine deps.
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
preferred = "USBDAC"     # USB serial or ALSA card name; engine picks first match

[audio]
period_ms = 12           # override per-period wall-clock
period_count = 4         # override period count
rt_policy = "auto"       # "auto" | "fifo" | "other"
rt_priority = 80
rt_affinity = -1         # -1 = no affinity; otherwise core index

[library]
directories = ["~/Music"]
ignore = [".*", "*.tmp"]

[theme]
follow_hyprland = true
override_file = ""       # path to a custom theme.toml
```

Final schema is locked in `docs/spec/locked.md` once schema is committed.

---

## See also

- `docs/spec/locked.md` — formal spec with rationale
- `docs/spec/open-questions.md` — unresolved items
- `docs/phases.md` — implementation phase plan
