# Open questions

The design is largely settled. What's listed below is what remains. Items in **(design)** would benefit from a deliberate decision before relevant code is written; items in **(implementation)** are details to be decided when the corresponding code is written.

## Design

All design questions are now resolved. The list below is preserved for posterity.

- Bit-perfect indicator UX → **Resolved.** Three-state YES / QUALIFIED / NO with tooltip explaining caveats.
- DAC capability cache → **Resolved.** Always re-probe on device-open; no on-disk cache.
- First-run UX → **Resolved.** Empty main window with prominent "Select DAC" / "Add library" empty-state prompts.
- Configuration file scope → **Resolved.** Minimal — only the keys users want to override.

## Implementation

These can be decided when the relevant code is written.

- **Test framework.** Catch2, doctest, or GoogleTest. Default lean: doctest.
- **DBus library.** sdbus-c++, libsystemd, or basu. Default lean: sdbus-c++.
- **Trace ring buffer.** Lock-free SPSC implementation choice (roll our own, vendored Vyukov-style, etc.).
- **Hyprland config parser.** Regex-based, hand-rolled, or a small INI/TOML library.
- **Logging / diagnostics format.** Plain text vs JSON; UI-visible vs file.
- **GL vs Vulkan backend for Dear ImGui.** GL is simpler and sufficient. Default: GL.
- **Period-count scaling formula across rates.** "Keep wall-clock latency ~constant" — exact formula (round to power-of-two? snap to DAC's preferred period?) to be determined.
