# Implementation phases

Each phase produces something runnable and testable. A phase is "done" when its acceptance criteria are met. Phases are ordered for dependency, but several can be parallelized as noted at the end.

---

## Phase 0 — Foundation

**Goal.** Project skeleton compiles. Empty binary launches. Build, lint, format infrastructure ready.

**Deliverable.**
- `meson.build` at root and per-module
- `meson_options.txt` with future-friendly flags (LTO, sanitizers in dev, `-march=native` toggle)
- Project layout under `src/` and `include/` per `docs/architecture.md`
- `.clang-format` and `.clang-tidy` configs
- `LICENSE` (GPLv3); SPDX header in every source file
- Hello-world `main.cpp` printing version and exiting

**Acceptance.** `meson setup build && meson compile -C build && ./build/transporter --version` exits 0 with version text.

---

## Phase 1 — ALSA-direct WAV playback (engine MVP)

**Goal.** Play a known-rate WAV file end-to-end through `hw:` ALSA. No GUI, no library, no library subsystem.

**Deliverable.**
- `engine/alsa/`: `snd_pcm_open`, capability probe (`snd_pcm_hw_params_test_format` / `test_rate`), `hw_params` set-exact, `writei` loop
- `engine/decoder/wav.cpp`: in-house WAV parser (RIFF / WAVE chunks, fmt / data)
- `engine/format/`: format match (refuse on mismatch)
- `engine/ring/`: SPSC ring buffer (provisional impl; final from realtime work later)
- A test driver in `tests/integration/wav_play.cpp` that takes a file path and a `hw:` device, plays it, exits

**Acceptance.**
- `./build/test_wav_play hw:CARD=X,DEV=0 fixtures/sine_44100_16.wav` plays audibly
- Format mismatch (e.g., 24-bit file on a 16-bit-only DAC) returns a clear error and does NOT play

---

## Phase 2 — Decoder layer expansion

**Goal.** All committed decoders work behind a uniform `IDecoder` trait.

**Deliverable.**
- `engine/decoder/decoder.hpp`: `IDecoder` trait
- One `.cpp` per format: FLAC (libFLAC), ALAC (libalac), MP3 (libmpg123), Vorbis (libvorbis), Opus (libopus), AIFF (in-house), WAV (carry-over from Phase 1)
- Format detection by extension + magic bytes (`engine/decoder/detect.cpp`)
- Tag reading per format: at least artist, album, title, track, year, duration

**Acceptance.**
- `tests/unit/decoder_*.cpp` pass for each format with seed fixtures
- `tests/integration/play_each_format.cpp` plays one file of each format end-to-end

---

## Phase 3 — Engine FSM

**Goal.** Full `Idle → Loading → Playing → Paused → Idle` transitions; auto-switch DAC rate on track change; format-mismatch refusal.

**Deliverable.**
- `engine/fsm.cpp`: state machine
- `engine/engine.cpp`: public API impl per `include/transporter/engine/engine.hpp`
- Rate-transition logic (drop / close / open / prepare)
- Hard-exclusive open with `-EBUSY` surfacing as `Error::DeviceBusy`

**Acceptance.**
- Two FLAC files at different rates queued; player switches DAC rate between them with audible re-lock silence (no crackle)
- Format-mismatch file: `Engine::load` returns `Error::FormatRefused`
- DAC already held by PipeWire: `Engine::create` returns `Error::DeviceBusy`

---

## Phase 4 — Device discovery & selection

**Goal.** Enumerate all ALSA cards, probe capabilities, identify by USB serial, present to UI consumer.

**Deliverable.**
- `engine/device/list.cpp`: enumerate cards via `snd_card_next` + `snd_pcm_info`
- `engine/device/fingerprint.cpp`: compute USB vendor:product:serial via libudev (`/sys/class/sound/...` walk-up)
- `engine/device/probe.cpp`: capability probe using `test_format` / `test_rate` (no `_near` calls)

**Acceptance.**
- `./build/test_device_list` prints cards with capabilities and stable IDs
- Re-running across reboots produces consistent IDs even if ALSA card numbers shuffle

---

## Phase 5 — RT thread + buffering policy

**Goal.** Audio thread runs `SCHED_FIFO` + `mlockall` + affinity when permitted; gracefully falls back; reports actual mode.

**Deliverable.**
- `engine/rt/`: thread setup, capability detection (try `pthread_setschedparam`, fall back), `mlockall`, affinity helpers
- Lock-free trace ring buffer (`engine/trace/`)
- Period-count scaling logic per active rate (keep ~constant wall-clock latency)
- README section documenting `/etc/security/limits.d/99-transporter.conf`

**Acceptance.**
- Stress test (`stress-ng --cpu N --timeout 30s`) during playback produces ≤ 1 xrun on a tuned RT setup
- Fallback path warns clearly in UI; xrun count visible in telemetry dump
- Trace events from audio thread visible in a separate "telemetry dump" command

---

## Phase 6 — Hotplug

**Goal.** DAC disconnect pauses; reconnect of the SAME DAC resumes.

**Deliverable.**
- `hotplug/` module using libudev (subscribe to `subsystem=sound`)
- DeviceFingerprint matching on resume
- Engine state transitions: `Disconnected → Paused/Playing` on same-DAC return
- Different-DAC reconnect is ignored (no auto-switch)

**Acceptance.**
- During playback, unplug USB DAC: state flips to Disconnected, no errors logged, no audible glitch beyond the truncation
- Re-plug: playback resumes within ~1 s; bit-perfect indicator restored
- Plug a DIFFERENT DAC during the disconnect window: ignored

---

## Phase 7 — Library subsystem

**Goal.** SQLite tag DB; background scanner; search / sort / filter API.

**Deliverable.**
- `library/schema.cpp`: tracks, albums, artists tables; migration table
- `library/scanner.cpp`: directory walk + tag read + upsert; emits delta events
- `library/queries.cpp`: all SQL strings, prepared statements
- Public API: `Library::search(filter)`, `Library::tracks_in_album(...)`, etc.

**Acceptance.**
- Pointed at a 10k-track directory, scan completes < 60 s on warm cache
- Search-by-artist returns expected results
- Re-scan picks up only changed files (mtime check); deletion of a file removes it from the DB

---

## Phase 8 — GUI MVP

**Goal.** Dear ImGui on Wayland EGL; main view shows current track + transport controls; library browse list works.

**Deliverable.**
- `gui/platform/`: Wayland surface + xdg-shell + EGL + GL initialization for ImGui
- `gui/views/main.cpp`: minimal main view (track info, time, transport buttons, DAC selector)
- `gui/views/library.cpp`: scrollable list pulling from `library/` API
- File-path argv triggers immediate load + play
- Stateless rendering; UI subscribes to engine events

**Acceptance.**
- Window opens on Hyprland and a default dark theme renders
- Click play / pause / next; controls work
- File argument loads track on launch

---

## Phase 9 — Theme integration

**Goal.** Read `~/.config/hypr/hyprland.conf`; apply colors, rounding, blur, fonts to the GUI.

**Deliverable.**
- `theme/parser.cpp`: hyprland.conf parser (handles nested sections, `source =`, basic `$var` substitution)
- `theme/model.cpp`: theme struct (colors, sizes, fonts, blur params)
- `gui/`: ImGui style applied from theme model on init
- Sensible Hyprland-aesthetic defaults if config absent

**Acceptance.**
- Player visually matches the user's Hyprland theme out of the box on a test rig
- Missing or malformed `hyprland.conf` falls back to defaults; no crash
- Override file at `~/.config/transporter/theme.toml` takes precedence

---

## Phase 10 — System integration (MPRIS + DBus)

**Goal.** MPRIS works (`playerctl` controls the player). Custom DBus interface available.

**Deliverable.**
- DBus library choice locked (default leans `sdbus-c++`)
- `dbus/mpris.cpp`: `org.mpris.MediaPlayer2`, `.Player`, plus `.TrackList` (subset)
- `dbus/transporter.cpp`: custom `org.mpris.MediaPlayer2.transporter` for transport-specific ops (current bit-perfect status, current DAC, RT mode)

**Acceptance.**
- `playerctl play-pause` toggles playback
- `playerctl metadata` returns current track tags
- `dbus-send` to custom interface reports bit-perfect status

---

## Phase 11 — Bit-perfect verification harness

**Goal.** Prove bit-perfect end-to-end with a loopback test. Stress xrun-resistance.

**Deliverable.**
- `tests/integration/bit_perfect_loopback.cpp`: load `snd-aloop`, configure, play file, capture, byte-compare
- Reference fixtures: WAV at 44.1k/16, 96k/24, 192k/24
- xrun stress harness using `stress-ng`
- Test framework choice locked (default leans doctest)

**Acceptance.**
- All three reference files round-trip byte-identical via loopback
- 5-minute stress test produces ≤ 1 xrun on a tuned RT system
- `meson test` runs the suite

---

## Phase 12 — Polish & UX

**Goal.** Bit-perfect indicator with crisp semantics, error UX, volume UX, first-run flow.

**Deliverable.**
- Bit-perfect indicator widget — three-state YES / QUALIFIED / NO with tooltip explaining caveat
- Error toast / banner for `DeviceBusy`, `FormatRefused`, `DeviceMissing`
- Volume slider that respects bit-perfect mode and HW-vs-digital path
- First-run UX — empty main window with prominent "Select DAC" / "Add library" empty-state prompts
- README finalized: build instructions, RT setup, usage

**Acceptance.**
- Manual UX walkthrough passes
- All error states have clear, actionable UI
- Bit-perfect indicator behaves per spec under: HW vol changes, digital vol engaged, RT fallback, format mismatch

---

## Out of phase plan (future, if at all)

- AUR packaging — community can do; we don't.
- Flatpak — sandboxing fights raw ALSA `hw:`; likely never.
- X11 fallback — explicitly out of scope.
- DSD support — out of scope.

---

## Parallelization

- Phase 0 must come first.
- Phase 1 depends on Phase 0.
- Phase 2 depends on Phase 1's `IDecoder` seam.
- Phase 3 depends on Phases 1–2.
- Phase 4 can start in parallel with Phase 3 once Phase 1 is done.
- Phase 5 depends on Phase 1 ring buffer; can start while Phases 3–4 are in flight.
- Phase 6 depends on Phase 4 (DeviceFingerprint).
- Phase 7 is independent of the audio path; can run in parallel with Phases 3–6.
- Phase 8 depends on engine API stable (post-Phase 3).
- Phase 9 depends on Phase 8.
- Phase 10 depends on Phase 3.
- Phase 11 (testing) runs continuously, not just at the end.
- Phase 12 is final.
