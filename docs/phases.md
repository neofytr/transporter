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
- `engine/telemetry/`: `PipelineSnapshot` producer + atomic counters wired through decoder + audio threads (frames decoded, frames written, ring fill bytes, session max-watermark, xrun count, RT mode, current ALSA `hw_params`)
- `include/transporter/engine/telemetry.hpp`: `PipelineSnapshot` struct (per-stage sub-structs Source / Decoder / FormatMatch / Ring / Output / Device / Realtime / BitPerfect) + `Engine::pipeline_snapshot()` callable from any non-RT thread
- README section documenting `/etc/security/limits.d/99-transporter.conf`

**Acceptance.**
- Stress test (`stress-ng --cpu N --timeout 30s`) during playback produces ≤ 1 xrun on a tuned RT setup
- Fallback path warns clearly in UI; xrun count visible in telemetry dump
- Trace events from audio thread visible in a separate "telemetry dump" command
- `Engine::pipeline_snapshot()` returns a populated snapshot during playback: live frame counters increase monotonically, ring fill is non-zero, RT mode reports `FIFO` or `OTHER` per environment, all per-stage formats match the loaded track

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
- `gui/views/pipeline.cpp`: dense pipeline transparency view per `docs/spec/locked.md` — vertical stack of stage cards (Source / Decoder / Format match / Ring / Output / Device / Realtime / Bit-perfect verdict); polls `Engine::pipeline_snapshot()` each frame; live counters update at GUI rate
- Toggle (button + keyboard shortcut) to swap main ↔ pipeline view; main remains the default landing surface
- File-path argv triggers immediate load + play
- Stateless rendering; UI subscribes to engine events

**Acceptance.**
- Window opens on Hyprland and a default dark theme renders
- Click play / pause / next; controls work
- File argument loads track on launch
- Pipeline view shows live, populated readouts during playback: per-stage formats, ring fill, frames written, DAC capability matrix, bit-perfect verdict with per-condition breakdown

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

---

## Phase 13a — GPU rendering backend (EGL + OpenGL)

**Goal.** Add an EGL/OpenGL render path as the default; keep the wl_shm CPU path as `--cpu` fallback. GPU enables blur, smooth curves, and proper post-processing for the UI redesign.

**Deliverable.**
- `src/gui/platform/render_gl.cpp`: EGL init (`wl_egl_window`, `eglCreateWindowSurface`, OpenGL 3.3 context); ImGui `imgui_impl_opengl3` backend wired up
- `src/gui/platform/render_cpu.cpp`: existing wl_shm path extracted and renamed (no logic change)
- `IRenderBackend` interface (or simple `RenderMode` enum + conditional compile) allowing Window to use either
- `--cpu` CLI flag → force CPU path; no flag or `--gpu` → GPU, fall back to CPU silently if EGL init fails
- Font atlas: GPU path uploads `GL_R8` texture and uses standard ImGui GL pipeline; CPU path unchanged
- GPU blur post-pass: `src/gui/platform/blur.glsl` — two-pass (H+V) Gaussian; applied to album art region behind track info text (frosted glass effect)
- Meson: detect `wayland-egl` + `egl` + `gl` via pkg-config; CPU-only build still works without them

**Acceptance.**
- `./build/transporter` (no flag): opens with GPU renderer; `glGetString(GL_RENDERER)` not "llvmpipe" on a real GPU
- `./build/transporter --cpu`: opens with software renderer; no EGL calls made
- GPU path: ImGui text renders cleanly at all sizes; album art displays correctly; frosted blur visible behind track info
- CPU path: all existing behaviour unchanged

---

## Phase 13 — UI redesign (Apple Music / Tidal aesthetic)

**Goal.** Rebuild the main view into a polished, visually-driven layout with large cover art, dominant-color background tinting, and a waveform-overlaid seekbar.

**Deliverable.**
- `gui/views/main.cpp`: full-width cover art hero at top; track info (artist / album / title) below in typographic hierarchy; transport controls as Nerd Font icon buttons
- `gui/util/dominant_color.cpp`: extract dominant RGB from AlbumArt pixels (fast median-cut or k=1 k-means); cache per track
- Background clear color and ImGui accent colors shift per-album based on dominant color (muted/desaturated for readability)
- Waveform seekbar replacing the plain progress bar — pre-computed envelope drawn via ImGui draw list; scrub-to-seek on click/drag
- Library browser: album-grid view with larger cover thumbnails; artist/album/track columns retain list fallback
- Queue panel as an overlay child window, toggled by button

**Acceptance.**
- Loading a track with album art: background tints within one frame; cover occupies ≥ 50% of window width
- Waveform seekbar correctly represents amplitude for a reference FLAC; click seeks to within ±1 s
- All transport controls functional; Nerd Font icons render correctly (not boxes)

---

## Phase 14 — Disc-number support

**Goal.** Multi-disc albums sort and display correctly.

**Deliverable.**
- `disc_no TEXT NOT NULL DEFAULT ''` column added to tracks table with migration
- All decoders (FLAC, MP3/ID3, Vorbis, Opus, AIFF, WAV) read DISCNUMBER / TPOS tag into `TrackInfo::disc_no`
- `select_tracks_in_album` query updated: `ORDER BY CAST(disc_no AS INTEGER) ASC, CAST(track_no AS INTEGER) ASC`
- Library browser: disc separator rows shown between disc groups when album has >1 disc

**Acceptance.**
- A multi-disc FLAC album plays disc 1 track 1, disc 1 track 2, …, disc 2 track 1, … in order
- Single-disc albums unaffected (`disc_no` defaults to 0/empty, treated as disc 1)

---

## Phase 15 — Software resampler (opt-in per-device)

**Goal.** When a track's native rate is unsupported by the selected DAC, an opt-in libsoxr resampler bridges the gap instead of hard-refusing.

**Deliverable.**
- libsoxr added as a build dependency (Meson wrap or system pkg)
- `engine/resample/`: `Resampler` class wrapping `soxr_create` / `soxr_process`; inserted between ring buffer output and ALSA write
- Per-device settings stored in SQLite (`device_settings` table): `resample_enabled`, `resample_target_rate`
- Resampler stage added to `PipelineSnapshot` (input rate, output rate, quality mode)
- Bit-perfect indicator: QUALIFIED when resampler active; tooltip explains rate conversion
- Settings UI: per-device toggle and target-rate picker in DAC selection panel

**Acceptance.**
- 192 kHz FLAC played through a 44.1/48 kHz-only DAC with resampler enabled: plays without error; indicator shows QUALIFIED
- Same file with resampler disabled: hard refusal as before
- Bit-exact path unchanged: resampler absent from pipeline snapshot when disabled

---

## Phase 16 — Gapless playback (same-rate)

**Goal.** Zero-gap transitions between consecutive tracks at the same sample rate.

**Deliverable.**
- Engine tracks next-track in queue; begins decoding next file when current track has ≤ 2 s remaining
- If next track matches current ALSA `hw_params` exactly: write continues without ALSA close/reopen
- Rate or format change: `drop → close → reopen`; UI labels this as "rate-change gap"
- `PipelineSnapshot::gapless_pending` bool visible in Pipeline view

**Acceptance.**
- Two 44.1k/16-bit FLACs queued: transition is inaudible (< 1 ms gap)
- 44.1k → 96k transition: brief silence, labeled in UI
- xrun count does not increase across gapless boundary

---

## Phase 17 — Waveform + live spectrum

**Goal.** Visual audio representation: per-file waveform envelope on the seekbar and a real-time FFT spectrum panel.

**Deliverable.**
- `gui/util/waveform.cpp`: background-thread decode → per-file amplitude envelope (peak per 512 frames, stored as `std::vector<float>`); cached by track ID in memory (LRU, max 20 tracks)
- Seekbar waveform overlay: ImGui draw list; current-position cursor; click/drag → seek
- `engine/spectrum/`: lock-free ring buffer tap in audio thread (latest 2048 samples); Hann window + FFT (KissFFT or similar); magnitude per bin posted to GUI thread via atomic snapshot
- Spectrum panel in main view: 64-bar display, peak hold, logarithmic frequency axis

**Acceptance.**
- Waveform renders within 2 s of track load for a 10-minute FLAC
- Spectrum visually responds to frequency content of a sine sweep test file
- CPU overhead of spectrum computation ≤ 3% on a single core

---

## Phase 18 — Queue improvements + smart playlists

**Goal.** Practical playlist management: drag-drop queue, saved playlists, rule-based auto-playlists.

**Deliverable.**
- Queue view: drag-drop reorder via ImGui; remove-from-queue per item; clear-queue button
- Saved playlists: `playlists` and `playlist_tracks` tables in SQLite; save-current-queue and load-playlist UI
- Smart playlists: `smart_playlists` table with JSON rule blob; rule types: artist =, genre =, date range, album =, duration </>; evaluated at query time against `tracks` table

**Acceptance.**
- Drag-drop reorder persists for session
- Saved playlist survives restart
- Smart playlist "all Jazz albums after 2010" returns correct results from a seeded library

---

## Phase 19 — ReplayGain

**Goal.** Optional loudness normalization via REPLAYGAIN tags.

**Deliverable.**
- All decoders expose `replaygain_track_gain_db`, `replaygain_album_gain_db` in `TrackInfo`
- `engine/dsp/replaygain.cpp`: float gain multiplier applied in a digital-domain stage between ring buffer write and ALSA (non-RT path — applies before writing to ring, in decode thread)
- Modes: off / track / album; per-session toggle in UI
- Bit-perfect indicator: QUALIFIED when active; Pipeline view shows gain stage

**Acceptance.**
- Track with known +3.5 dB tag: measured output level matches expectation via loopback
- ReplayGain off: gain stage absent from pipeline; bit-perfect indicator unaffected
- Clipping prevention: applied gain capped at +6 dB; negative gain unlimited

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
