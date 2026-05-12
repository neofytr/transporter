# Implementation phases

Each phase produces something runnable and testable. A phase is "done" when its acceptance criteria are met.

The original phases (0–12) brought the engine, library, decoder set, hotplug, MPRIS, and bit-perfect verification online. Phases 13a, 13, 14 built a Wayland/ImGui GUI which has since been retired in favour of a terminal-native (notcurses) frontend. Phases T0–T12 cover that rebuild. Phase 19 (ReplayGain) remains an engine-level item independent of the frontend swap.

---

## Closed phases (engine + library + control surface)

| Phase | Status | Note |
|---|---|---|
| 0 — Foundation | Done | Meson + clang-format + clang-tidy + SPDX |
| 1 — WAV/ALSA MVP | Done | Hard-exclusive `hw:` open + writei loop |
| 2 — Decoder layer | Done | WAV / AIFF / FLAC / ALAC / MP3 / Vorbis / Opus |
| 3 — Engine FSM | Done | Idle/Loading/Playing/Paused/Error/Disconnected |
| 4 — Device discovery | Done | USB-serial-stable IDs via libudev walk-up |
| 5 — RT + telemetry | Done | SCHED_FIFO + mlockall + affinity; full `PipelineSnapshot` |
| 6 — Hotplug | Done | netlink udev + USB fingerprint match; same-DAC auto-resume |
| 7 — Library | Done | SQLite + FTS5 + mtime-delta scan |
| 8 — GUI MVP | Done (superseded by TUI) | Wayland + ImGui + EGL; retired in T0 |
| 9 — Theme | Done (superseded) | Hyprland parser retired with the GUI |
| 10 — DBus / MPRIS | Done | sdbus-c++; full Player + Metadata + TrackList + custom interface |
| 11 — Bit-perfect verification | Done | Loopback test; integration tests now registered with `meson test` (commit `596e91e`) |
| 12 — Polish / first-run | Done (superseded by TUI design) | First-run + error UX rebuilt in T1+ |
| 13a — GPU rendering | Done (superseded) | EGL + OpenGL 3.3 path retired with the GUI |
| 13 — UI redesign | Done (superseded) | Apple-Music-style ImGui hero retired with the GUI |
| 14 — Disc number | Done | Multi-disc sort + `disc_no` column + decoder tag extraction |

## Pre-TUI cleanup (completed)

Done in commits `bf57718`, `596e91e`, `6c2e7e4`, `cad503a`, `c55a45b`, `01b6bb3`:

- `bf57718` resolve font via fontconfig (later reverted in T0 since notcurses uses the terminal's font stack)
- `596e91e` register integration tests with `meson test`; SKIP semantics via exit-77
- `6c2e7e4` drop dead spectrum tap from audio thread (no consumer existed)
- `cad503a` surface digital-volume engagement in bit-perfect verdict (rename `volume_unity` → `digital_path_off`)
- `c55a45b` add `gapless_pending` to PipelineSnapshot
- `01b6bb3` transition to `Disconnected` on ALSA write `-ENODEV` (not just on udev disconnect)

---

## TUI overhaul (T0–T12)

The frontend is a single-process notcurses TUI. The engine, library, decoder set, and MPRIS surface are unchanged below the frontend layer. Headless / daemon mode is supported via smart-tty detection (`--daemon` or no tty → MPRIS-only).

### Locked design decisions (see CLAUDE.md → TUI / desktop)

- notcurses 3.x rendering layer; kitty-graphics by default, sixel inside tmux, halfblock fallback
- Persistent 3-row bottom player bar + paged content (Library / Queue / Now Playing / Pipeline / Settings)
- Album-grid library (responsive auto-fit ~140px tile)
- Now Playing: cover left, info right; live 64-bin log-frequency braille spectrum; L/R horizontal dBFS VU; dual-density peak+RMS waveform overlay
- Accent-only dominant-color tint; terminal background untouched
- Vi-modal input with mouse on by default
- Nerd Font glyphs for transport icons
- In-house JPEG/PNG decode → `ncvisual_from_rgba()` (no transitive ffmpeg dep)
- Min terminal size 100 × 28 comfortable; refuse below 60 × 20
- Smart-tty daemon dispatch

---

### T0 — Tear-out + notcurses bootstrap

**Goal.** Retire the Wayland/ImGui frontend; stand up the notcurses lifecycle.

**Deliverable.**
- Delete `src/gui/`, `src/theme/`, `third_party/imgui/`, `data/transporter.desktop`
- Remove deps from meson: `wayland-client`, `wayland-egl`, `xkbcommon`, `wayland-protocols`, `wayland-scanner`, `EGL`, `GL`/`epoxy`, `fontconfig`
- Add dep: `libnotcurses-dev` (Ubuntu 3.0.7 packaged)
- `src/tui/notcurses_ctx.{cpp,hpp}` — `init()` + `shutdown()`, capability probe (kitty / sixel / truecolor / unicode level), `--probe-terminal` dump
- `src/main.cpp` mode dispatch: `--daemon` OR no tty → MPRIS-only daemon (no notcurses init); else TUI
- `src/tui/meson.build` and root meson wiring
- `tests/unit/tui_notcurses_ctx.cpp` — capability detection unit test using a `--probe-terminal` JSON dump

**Acceptance.**
- `meson compile -C build` clean (no leftover wayland/imgui references)
- `./build/transporter --probe-terminal` prints terminal capability matrix and exits 0
- `./build/transporter --daemon` runs MPRIS-only; `playerctl play-pause` still works
- `./build/transporter` from a real tty opens a notcurses pane (empty body, no panic on quit)

---

### T1 — Player bar + page router

**Goal.** Persistent UI chrome + page navigation.

**Deliverable.**
- `src/tui/app.{cpp,hpp}` — top-level state, event loop, page registry
- `src/tui/components/player_bar.cpp` — 3-row bottom bar with mock data (cover thumb cell, title, artist · album, seekbar/time/transport, telemetry row: bit-perfect · format · DAC · RT · ring · xrun · volume)
- `src/tui/input.{cpp,hpp}` — vi-modal input baseline: `1`–`5` switch pages, `Tab`/`Shift-Tab` cycle, `j`/`k`/arrows navigate, `:` command palette opens, `q`/`Ctrl-C` quit
- Stub pages: `pages/library.cpp`, `pages/queue.cpp`, `pages/now_playing.cpp`, `pages/pipeline.cpp`, `pages/settings.cpp` — each shows page name in centred text
- Rounded-light borders `╭╮╰╯─│`

**Acceptance.**
- Launch shows player bar at bottom; pressing `1`–`5` swaps the body; vi keys move a placeholder cursor; `q` exits cleanly

---

### T2 — Library page (album grid)

**Goal.** Real library grid with kitty/sixel cover thumbnails.

**Deliverable.**
- `src/tui/pages/library.cpp` — responsive auto-fit grid, ~14-cell target tile (~140 px equivalent), pulls from `library::albums()`
- `src/tui/components/cover.{cpp,hpp}` — in-house JPEG/PNG decoder (lifted from the retired `src/gui/views/albumart.cpp`); `ncvisual_from_rgba()` dispatch; per-album cover cache (LRU 50)
- Embedded → sidecar art lookup: `cover.{jpg,png}`, `folder.{jpg,png}`, `front.{jpg,png}`, `AlbumArt*.{jpg,png}` (Picard/Wikipedia conventions)
- `pages/album_detail.cpp` — cover hero + meta header (title, artist, year, disc count, total time, format spread) + rich audiophile track list (`disc# · track# · title · duration · format · ReplayGain`)
- Drill into album_detail: `Enter` on a grid tile
- Disc separators rendered when album has more than one disc

**Acceptance.**
- Pointed at a 1000-track library, grid renders within 1 s with covers visible (cached)
- Drill into a multi-disc album shows disc separators in track list
- Covers render on `foot` (sixel) AND `kitty` (kitty-graphics); fall back to half-block on `xterm`

---

### T3 — Now Playing page (prototype milestone)

**Goal.** Visceral Now Playing view — first compelling artefact to share with the project owner.

**Deliverable.**
- `src/tui/pages/now_playing.cpp` — cover left (~40% width); info right (title 1 line, artist · album 1 line, year · disc 1 line, format pill 1 line)
- Seekbar with **static** waveform overlay (placeholder until T6 wires the live one)
- Transport row of Nerd Font glyphs (⏮ ⏯ ⏭) horizontally centred under the seekbar
- `src/tui/components/seekbar.cpp` — drag-to-seek hit zone; cursor position updates from `Engine::pipeline_snapshot().position`
- `src/tui/components/transport.cpp` — icon row + hit zones; calls `engine::play()` / `pause()` / `next()` / `prev()`
- `src/tui/components/toast.cpp` — ephemeral bottom banner (2 s fade) on track change

**Acceptance.**
- From Library, drill into an album, press `Enter` on a track → playback starts AND Now Playing page shows correct cover + info + transport
- `space` toggles play/pause; transport icons respond
- After this phase, **notify the project owner** — a basic prototype is openable

---

### T4 — Queue page

**Goal.** Manageable queue with keyboard ops.

**Deliverable.**
- `src/tui/pages/queue.cpp` — list view, current track row highlighted with `▶`
- Header: `Queue (N tracks · MM:SS)`
- Keybindings: `j`/`k` navigate, `J`/`K` move selected track down/up, `dd` remove, `D` clear queue (with confirmation), `s` save current queue as playlist (stub the SQLite playlist table for now if not yet present)
- Right-pad: per-row duration

**Acceptance.**
- Queue reorder via `J`/`K` updates engine's track sequence; removed tracks no longer play; save creates a playlist row in SQLite

---

### T5 — Pipeline page (dense + sparklines)

**Goal.** Audit-grade telemetry view.

**Deliverable.**
- `src/tui/pages/pipeline.cpp` — sticky bit-perfect verdict pill at top (colour by state: green YES, amber QUALIFIED, red NO)
- Vertical stage cards: Source / Decoder / FormatMatch / Ring / Output / Device / Realtime / BitPerfect (with each qualification listed)
- Sparklines (5 s history, braille high-density) on: Ring fill %, Output frames-written rate, Xrun timeline
- Refresh ≥ 10 Hz from `Engine::pipeline_snapshot()`
- USB serial + DAC capability matrix in Device card

**Acceptance.**
- During playback, sparklines update live; verdict pill colour reflects state; xruns visible immediately on the Xrun strip; rate-change between tracks shows in the Output card

---

### T6 — Live viz: spectrum + VU + waveform

**Goal.** Audiophile signal feedback on Now Playing.

**Deliverable.**
- `src/engine/spectrum/` — reintroduce the audio-thread tap, now RAII-safe:
  - Public API: `auto handle = engine.attach_spectrum_consumer()` returns a unique handle
  - Lock-free SPSC ring allocated only while a handle is live
  - Audio thread checks `consumer_active.load(acquire)` before writing — no allocations, no syscalls
  - Handle drop releases the ring; further audio-thread writes are no-ops
- `third_party/kissfft/` vendored (MIT, header-only)
- `src/tui/dsp/spectrum_consumer.cpp` — 2048-sample Hann window → KissFFT → 64 log-frequency bins → braille bars + peak hold (decays over 1 s) @ 30 Hz
- `src/tui/dsp/vu_consumer.cpp` — L/R peak + RMS dBFS, smoothed; horizontal bar with green→yellow→red gradient + peak-hold dot
- `src/tui/dsp/waveform_envelope.cpp` — per-file background pre-decode → peak + RMS at ~512-frame windows → LRU 20 cache; dual-density rendering on the seekbar (peaks outer, RMS fill inner; played portion accent-coloured)
- All three viz live in `src/tui/components/{spectrum,vu_meter,seekbar}.cpp`, drawn on Now Playing only

**Acceptance.**
- Sine sweep test file: spectrum bars track the sweep up the log axis; VU shows -3 dBFS-ish for full-scale sine
- 10-minute FLAC: waveform envelope renders within 2 s of track load
- Engine RT-safety preserved: no allocations on the audio thread under any consumer/no-consumer scenario (verify via stress test + RT-safety check)
- Audio thread CPU overhead vs no-consumer baseline: ≤ 3 % single-core

---

### T7 — Search overlay + DAC popup + Help overlay

**Goal.** Discoverable interaction surfaces.

**Deliverable.**
- `src/tui/pages/search_overlay.cpp` — `/` opens a bottom-of-page filter bar; types into FTS5 in Library, substring filter elsewhere; ESC cancels; Enter commits selection (and navigates to it if cross-album)
- `src/tui/pages/dac_popup.cpp` — `D` from any page opens a modal listing every visible DAC with full capability matrix (rate × format), USB vendor:product:serial, current status; Enter selects, ESC cancels
- `src/tui/pages/help_overlay.cpp` — `?` opens a modal listing keybindings grouped by context (Global / Navigation / Transport / Library / Queue)
- Bottom status hint strip on each page shows context-relevant keys (e.g. on Library: `j/k navigate · / search · D dac · ? help`)

**Acceptance.**
- `/` in Library filters live as you type; `D` opens DAC picker; `?` overlay shows all keys; status hint line stays current

---

### T8 — Settings page (tabbed)

**Goal.** Runtime-tunable knobs in-app.

**Deliverable.**
- `src/tui/pages/settings.cpp` — top tab bar `[ Output ] [ Library ] [ Playback ] [ Display ] [ Pipeline ] [ About ]`
- Output: DAC (jump to `D` popup), per-DAC resampler toggle (UI scaffold; engine side is Phase 15), volume mode (HW / Digital / Off), gapless (on / off)
- Library: paths add/remove, ignore pattern list
- Playback: shuffle/repeat defaults, Enter behaviour (replace queue vs append)
- Display: theme accent source (album / fixed), notifications (off / libnotify), spectrum on/off, VU on/off
- Pipeline: refresh rate (5–30 Hz), sparkline window (1–10 s)
- About: version, build info, license, link to source
- All settings round-trip to `~/.config/transporter/config.toml`

**Acceptance.**
- Every visible toggle and field persists across restarts; per-DAC resampler toggle stores into SQLite `device_settings` (rows pre-allocated for Phase 15)

---

### T9 — Theme + capability fallback polish

**Goal.** Accent extraction + graceful degradation on weak terminals.

**Deliverable.**
- `src/tui/theme.{cpp,hpp}` — port the dominant-color extractor (LRU 50, keyed by track path); muted_bg and accent variants; updated on track change
- Apply accent to: border foreground, seekbar fill, focus highlight, bit-perfect pill background, sparkline lines
- `src/tui/notcurses_ctx.cpp` — capability detection result drives blitter pick: kitty → sixel → halfblock; `$TMUX` set → force sixel preference
- Stderr warning at startup when both kitty and sixel are unavailable: `transporter: terminal lacks graphics protocols — album art will render as unicode blocks. Consider kitty / foot / ghostty / wezterm`

**Acceptance.**
- Same library viewed on kitty vs foot vs xterm: covers render with the best available protocol; halfblock fallback is visible but legible; warning prints once on cold start

---

### T10 — MPRIS art forwarding + libnotify (opt-in)

**Goal.** Cover art surfaces in status bars, notification daemons, lockscreens.

**Deliverable.**
- `src/dbus/mpris_metadata.cpp` extension — on track change: extract the embedded picture (or read sidecar), write to `/tmp/transporter-cover-XXXX.{jpg,png}` with `mkstemp`, set `mpris:artUrl = file:///...`; old temp file unlinked
- Optional libnotify path behind `[ui] desktop_notifications = true`: on track change, fire a `notify_notification_new(title, "$artist — $album", art_path)` and `notify_notification_show`
- Cleanup on process exit: temp files unlinked

**Acceptance.**
- `playerctl metadata mpris:artUrl` returns the temp file path during playback; waybar with MPRIS module shows the cover; libnotify popup appears when feature flag is on

---

### T11 — Session persistence + inotify scanner

**Goal.** Resume across launches; library stays current with disk.

**Deliverable.**
- `src/tui/session.{cpp,hpp}` — save on quit / restore on launch: queue + queue_index + shuffle + repeat + position (within current track) + last page open + library sort/filter state + last selected DAC
- On launch with no argv: engine loads last track but stays in `Paused` state (no surprise audio)
- `src/library/scanner.cpp` — inotify watcher on each library root path: `IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO`; events debounced to a per-directory 1 s window; affected paths re-scanned
- Live progress on a status line during initial scan: `Scanning: N tracks · P% · ~/Music/…`

**Acceptance.**
- Quit during playback at 2:34 of track 5 of a queued album → relaunch with no argv → engine state is `Paused`, track 5 loaded, position 2:34
- Drop a new FLAC into `~/Music`: appears in Library within a few seconds without manual rescan

---

### T12 — Polish + min-size handling + final docs

**Goal.** Ship-quality finish.

**Deliverable.**
- Min-size handling: 100 × 28 comfortable; below that, progressively collapse (player bar 3 rows → 1 row, spectrum hidden, cover hidden); below 60 × 20, show "terminal too small — resize or use --no-tui"
- Track-change toast timing tuned (2 s fade with slight overlap to next track)
- Sparkline colour polish (accent-tinted)
- `README.md` updated with: build instructions (Ubuntu/Fedora/Arch deps), RT setup, CLI flags, keybindings cheat sheet
- `docs/architecture.md` updated to reflect the TUI architecture
- `docs/spec/locked.md` updated to match `CLAUDE.md`
- Manual UX walkthrough on foot + kitty + xterm

**Acceptance.**
- Side-by-side comparison with the pre-TUI commit (`0c6442a` redesign) is favourable on every page
- No FIXME / TODO comments left in `src/tui/`

---

## Forward engine-level phases (independent of TUI work)

### Phase 15 — Software resampler (libsoxr, opt-in per-device)

**Goal.** Cover the QUALIFIED path for format-mismatched DAC + track combinations.

Deferred until after T8 (Settings page) lands the UI scaffold. Engine-side work as previously planned: `engine/resample/` wrapping `soxr_create` / `soxr_process` between ring drain and ALSA write; per-DAC `resample_enabled` + `target_rate` in SQLite `device_settings`; resampler stage in `PipelineSnapshot`; bit-perfect indicator downgrades to QUALIFIED when active.

### Phase 16 — Gapless playback (same-rate)

`gapless_pending` field already wired in `PipelineSnapshot` (commit `c55a45b`). Engine has same-rate continuation. Remaining: rate-change-boundary UI label (T5 sparkline edge case); acceptance via loopback for the inaudible-gap criterion.

### Phase 19 — ReplayGain

All decoders already expose `rg_track_gain` / `rg_album_gain` in `Tags`. Remaining: digital-gain stage in decode thread (`engine/dsp/replaygain.cpp`); mode toggle (off / track / album) in Settings → Output (UI scaffold from T8); bit-perfect indicator downgrades to QUALIFIED when active. Phase 17/18 (waveform/spectrum/playlists) are folded into T6 and T4/T8.

---

## Parallelization within the TUI overhaul

```
T0 ─ T1 ─┬ T2 ┐
         ├ T3 ┤
         ├ T4 ├─ T6 ─┬ T7 ┐
         └ T5 ┘       ├ T8 ├─ T10 ─ T11 ─ T12
                      └ T9 ┘
```

After T1 establishes patterns, T2/T3/T4/T5 are independent. T7/T8/T9 are independent after T6.
