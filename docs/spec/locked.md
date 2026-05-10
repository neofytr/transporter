# Locked specification

Single source of truth for committed design decisions. Updated only via deliberate review.

Each row gives the **decision**, **rationale**, and **implications**. If you need to take an action that contradicts a row here, it must be re-opened.

## Audio engine

### Audio path
- **Decision:** Playback opens `libasound` device as `hw:CARD=X,DEV=Y` (or by index). No `default`, no `plug:`, no `dmix`, no PipeWire / Pulse / JACK.
- **Rationale:** This is the only path on Linux that is unambiguously bit-perfect — the kernel does not insert mixing or resampling.
- **Implications:** Only one app can hold the device. Other apps get `-EBUSY`. The player must surface this cleanly.

### Sound server
- **Decision:** None. Project links `libasound2` and nothing else for audio.
- **Rationale:** Maximum minimalism; eliminates an entire class of "did PipeWire change the bytes" questions.
- **Implications:** When the player is running, system audio loses access to the chosen DAC. User must accept this trade.

### Format scope
- **Decision:** PCM only — `S16_LE`, `S24_LE`, `S24_3LE`, `S32_LE`, `FLOAT_LE`. Exact subset accepted at runtime is the intersection of these and what the DAC advertises.
- **Rationale:** PCM covers the realistic library. DSD adds parser, format-conversion layer, and ALSA-DSD endpoint quirks that are out of scope.
- **Implications:** `.dsf` / `.dff` files are refused. ALAC, FLAC, WAV, AIFF, MP3, OGG, Opus all decode to one of these formats.

### Sample-rate handling
- **Decision:** Auto-switch DAC rate to the track's native rate, every track. On rate transitions: `snd_pcm_drop` → `snd_pcm_close` → `snd_pcm_open` with new params.
- **Rationale:** True bit-perfect — DAC re-clocks to match the file. No resampling anywhere.
- **Implications:** Brief silence on rate changes (PLL re-lock; 100–500 ms typical depending on DAC). No cross-fade to mask it. UI may want a subtle transition indicator.

### Format-mismatch policy
- **Decision:** Refuse playback with explicit error. No bit-depth promotion, no dithered demotion, no resampling.
- **Rationale:** Strongest possible bit-perfect commitment.
- **Implications:** UI must clearly explain refusals ("DAC X supports 16/24-bit PCM up to 192k; this file is 32-bit float at 384k"). Library scanner should pre-flag unplayable files per DAC.

### Exclusivity
- **Decision:** Hard exclusive. Kernel-level `hw:` open is exclusive — second opener gets `-EBUSY`.
- **Rationale:** Strongest bit-perfect guarantee — nothing else can mix into this stream because nothing else has the device.
- **Implications:** If PipeWire was using the DAC at startup, our open will fail. Surface to user with optional helper hints (`pactl suspend-sink`, `wpctl set-default <other>`). Never auto-modify the user's PipeWire / Wireplumber config.

### Device lifecycle
- **Decision:** Claim DAC at process startup; hold for the life of the process; release on graceful exit.
- **Rationale:** Predictable single-purpose semantics — running the player == owning the DAC.
- **Implications:** Launching the player is effectively a global "this DAC is mine now". Pause / stop / skip do NOT release the device. User must close the player to share.

### Volume
- **Decision:** Hardware-volume passthrough where DAC exposes a mixer control (via `snd_ctl_*`); 32-bit-float digital attenuation where it doesn't. A prominent UI toggle disables digital attenuation (forces unity), surrendering volume to downstream gear.
- **Rationale:** Some DACs have analog or hardware-digital volume that doesn't degrade signal; others have nothing. We accommodate both without compromising the bit-perfect commitment.
- **Implications:** UI must show clearly which mode is active. "Bit-perfect" indicator must consider volume state.

### Multi-DAC
- **Decision:** Single active output only.
- **Rationale:** Multi-DAC sync requires drift correction (= per-output rate adjustment = resampling); incompatible with bit-perfect commitments.
- **Implications:** UI offers DAC selection but only one is active at a time. Switching DAC during playback is a stop-on-old / start-on-new transition.

### Realtime scheduling
- **Decision:** Audio thread runs `SCHED_FIFO` priority 80 + `mlockall(MCL_CURRENT | MCL_FUTURE)` + CPU affinity to a single dedicated core. Soft fallback to `SCHED_OTHER` + `nice -10` if RT capability not granted (user not in `audio` group, no PAM `limits.conf` entry, etc.). The UI surfaces the actual mode.
- **Rationale:** Audiophile expectations align with RT for stability; the soft fallback preserves first-run UX without setup; actual-mode visibility lets the user verify.
- **Implications:**
  - Document `/etc/security/limits.d/99-transporter.conf` (`@audio - rtprio 95`, `@audio - memlock unlimited`) for users who want full RT.
  - UI must show "RT: enabled" or "RT: fallback (SCHED_OTHER)".
  - Bit-perfect indicator may downgrade to "qualified" in fallback mode (since xruns become more likely).
  - `mlockall` requires `RLIMIT_MEMLOCK` to be sufficient.

### Buffer / latency
- **Decision:** Default total buffer ~50 ms (4 periods at ~12 ms each at 44.1k). Period frame count scales per active rate to maintain ~constant wall-clock latency.
- **Rationale:** Audiophile playback is not latency-sensitive; safety beats snappiness. Conservative defaults rarely xrun even without RT.
- **Implications:**
  - At 192k, period_size becomes ~2400 frames to keep ~12 ms.
  - User can override `audio.period_ms`, `audio.period_count` in the config file.
  - The buffer-size math is rate-aware, not frame-count-fixed.

### Hotplug
- **Decision:** Pause-and-watch. On `-ENODEV` (or equivalent), engine pauses, identifies the active DAC by USB vendor:product:serial when available (ALSA card name as fallback), auto-resumes when the same DAC reappears.
- **Rationale:** Audiophile DACs occasionally drop for power / USB-cycle reasons; resume UX matches user expectation. We never silently switch to a different DAC (that would risk format mismatch).
- **Implications:**
  - Engine grows a `DeviceFingerprint` abstraction.
  - Hotplug uses udev (libudev) or inotify on `/sys/class/sound`.
  - Pause is indefinite; user can manually stop or change DAC if they prefer.
  - On resume, DAC capabilities are re-probed (firmware may have changed during the disconnect).

## GUI / desktop integration

### GUI toolkit
- **Decision:** Dear ImGui (immediate-mode), drawn on a Wayland EGL surface via OpenGL or Vulkan backend.
- **Rationale:** Custom-rendered (fits Hyprland aesthetic), small dep, audiophile-tool feel, single-language with the engine (C++).
- **Implications:** We render every pixel; no native widgets; HiDPI, file dialogs, etc. are our responsibility. Vendored as `third_party/imgui` (single-header).

### Display server
- **Decision:** Wayland-only.
- **Rationale:** Hyprland is the target; Hyprland is Wayland-native; X11 is legacy.
- **Implications:** No Xwayland, no X11 fallback. ImGui platform layer talks `libwayland-client` + `xdg-shell` + EGL directly.

### Theme integration
- **Decision:** On launch, parse `~/.config/hypr/hyprland.conf` for color / border / rounding / blur / font cues; apply once. User relaunches to refresh.
- **Rationale:** Predictable, simple. File-watch and live-reload are unnecessary complexity.
- **Implications:**
  - Need a small, robust hyprland.conf parser (handles nested sections, variables, sourced files).
  - Sensible Hyprland-aesthetic defaults if no config found.
  - Theme model centralized in `theme/` module.

### System integration
- **Decision:** MPRIS (`org.mpris.MediaPlayer2`) + minimal custom DBus extension (`org.mpris.MediaPlayer2.transporter`) for transport-specific operations (current bit-perfect status, current DAC, etc.).
- **Rationale:** MPRIS is the standard; gives us global media keys, hyprland osd hooks, status-bar (waybar) integration for free. Custom DBus extends with our specifics for power users / scripts.
- **Implications:**
  - DBus dep (likely `sdbus-c++`).
  - MPRIS implementations require precise method signatures; tests for compliance.
  - Custom interface designed to be tiny — only what MPRIS doesn't already cover.

### Control surface
- **Decision:** GUI primary; CLI accepts a single optional file-path argument (`transporter <file>`). All other external control is via MPRIS.
- **Rationale:** Keeps CLI minimal; reuses MPRIS for everything else.
- **Implications:**
  - argv parser stays tiny.
  - `playerctl play-pause`, `playerctl next`, etc. just work.
  - Power users wanting more can write MPRIS clients in any language.

## UX rules

### Bit-perfect indicator
- **Decision:** Three-state — YES / QUALIFIED / NO — prominently displayed on the main view, with a tooltip explaining any caveat.
- **Rationale:** Strict yes/no would be too punishing (a momentary RT fallback would flip the indicator off); a single component breakdown would be too informationally dense for the at-a-glance use case.
- **Implications:**
  - YES requires: digital path bit-perfect (no resampling — always true since we refuse mismatches), no software volume in path (HW vol if present, OR digital toggle disabled), RT enabled, no recent xrun.
  - QUALIFIED on any caveat that doesn't break the digital path: RT fallback, HW volume below unity, etc.
  - NO when digital path is mutated: digital volume engaged, recent xrun, format mismatch in flight.

### DAC capability cache
- **Decision:** Always re-probe on device-open. No on-disk capability cache.
- **Rationale:** Always correct, no staleness window, no invalidation logic to maintain. The 50–200 ms probe is acceptable for a one-shot startup or device-switch event.
- **Implications:** Every `Engine::create` runs the full `snd_pcm_hw_params_test_*` enumeration. Probe results may be cached in-memory for the session.

### First-run UX
- **Decision:** Empty main window with prominent "Select DAC" and "Add library" empty-state prompts. No wizard; no opinionated defaults.
- **Rationale:** Trusts the user; doesn't waste a screen of guided flow on a one-shot setup.
- **Implications:** Main view always renders; DAC selector and library list have well-designed empty states with clear primary actions.

### Config file scope
- **Decision:** Minimal — only the dozen-or-so keys users actually want to override.
- **Rationale:** Anti-knob; fewer ways to break bit-perfect or get into a confusing state.
- **Implications:** Schema in `~/.config/transporter/config.toml` covers `[device]` (preferred), `[audio]` (period_ms, period_count, RT policy override), `[library]` (directories, ignore patterns), `[theme]` (override file path, follow_hyprland flag). No knobs for things users shouldn't change.

## Implementation

### Language
- **Decision:** Modern C++ (C++20/23 features welcome).
- **Rationale:** Direct libasound access, mature audio ecosystem (Dear ImGui, JUCE-like patterns), reasonable performance, balanced safety vs. minimalism.

### Binary topology
- **Decision:** Single binary. Internally, audio engine is a self-contained library / module with no UI deps; GUI is a separate module that consumes the engine via in-process API.
- **Rationale:** No IPC overhead, single deployment artifact; engine remains reusable for headless tools later.
- **Implications:** Build produces multiple static libs that link into one binary. A future `transporter-headless` could relink the engine lib without changes.

### Library / metadata
- **Decision:** Lightweight SQLite tag database with a background scanner thread.
- **Rationale:** Real music-player UX (search, browse by artist / album / etc.); SQLite is small, well-understood, requires no daemon.
- **Implications:** Build-time dep on SQLite. Scanner is lock-free relative to the audio thread (it's UI / control work).

### Decoder set
- **Decision:** libFLAC for FLAC; libalac for ALAC; libmpg123 for MP3; libvorbis for Ogg Vorbis; libopus for Opus; in-house parsers for WAV and AIFF.
- **Rationale:** Each codec via its native library is simpler and lighter than libavcodec for the formats we actually need.
- **Implications:**
  - 5 small codec deps + 2 in-house.
  - All deps are GPLv3-compatible.
  - Decoder trait abstracts the differences (see `docs/architecture.md`).

### Build system
- **Decision:** Meson + Ninja.
- **Rationale:** Clean syntax; very fast; Linux-native ecosystem fit (libwayland, mesa, etc. use Meson).
- **Implications:**
  - `meson.build` files per directory.
  - `subprojects/` for vendored deps where pkg-config isn't appropriate (e.g., Dear ImGui).
  - System deps via pkg-config: `alsa`, `sqlite3`, `wayland-client`, `wayland-egl`, `egl`, `gl` or `vulkan`, `flac`, `mpg123`, `vorbis`, `opus`, `sdbus-c++`. ALAC may need a vendored or AUR-only build.

## Distribution

### License
- **Decision:** GPLv3.
- **Rationale:** Aligns with Linux audio ecosystem; ensures derivatives stay open. All deps are GPLv3-compatible (libFLAC BSD, libalac Apache 2.0, libmpg123 LGPL 2.1, libvorbis BSD, libopus BSD, libasound LGPL, SQLite PD, Dear ImGui MIT, sdbus-c++ LGPL 2.1, Wayland MIT).
- **Implications:** SPDX header (`SPDX-License-Identifier: GPL-3.0-or-later`) in every source file. `LICENSE` file at root. About-page mention.

### Packaging
- **Decision:** Source only (no AUR, no Flatpak, no static binary in v1).
- **Rationale:** Most disciplined. Users build from source; we don't take on package maintenance burden. AUR can be packaged later by community without our involvement.
- **Implications:**
  - README must have a clear, complete build section.
  - `.desktop` file shipped in repo at `packaging/transporter.desktop`; `meson install` puts it in `$prefix/share/applications`.
  - No package-version drift to manage.

## Out of scope (committed)

These are explicitly rejected. Adding any of them requires re-opening the design.

- DSP of any kind on the bit-perfect path (EQ, ReplayGain, cross-fade, room correction, DSD↔PCM, …)
- DSD support (DoP, native, any form)
- System-wide routing of OTHER applications' audio
- Streaming protocols (HTTP, DLNA, AirPlay, Spotify Connect, Tidal Connect, Roon, …)
- Multi-zone / synchronized multi-output
- Plugins, scripting, embedded interpreters
- Cross-platform support (Windows, macOS, BSD)
- X11 (Wayland-only by design)

## Cross-references

- `open-questions.md` — what is still being negotiated.
- `../architecture.md` — runtime architecture, threads, public API.
