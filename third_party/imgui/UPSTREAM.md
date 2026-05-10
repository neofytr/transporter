# Dear ImGui (vendored)

- Upstream: https://github.com/ocornut/imgui
- Tag: v1.91.5
- Commit: f401021d5a5d56fe2304056c391e78f81c8d4b8f
- Imported: 2026-05-10
- License: MIT (see `LICENSE`).

Imported files: top-level `imgui.cpp`, `imgui.h`, `imgui_demo.cpp`,
`imgui_draw.cpp`, `imgui_internal.h`, `imgui_tables.cpp`, `imgui_widgets.cpp`,
`imconfig.h`, `imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h`.

Backends: `backends/imgui_impl_opengl3.{cpp,h}` plus
`backends/imgui_impl_opengl3_loader.h`. Other `imgui_impl_*` backends
(SDL, GLFW, Win32, Vulkan, etc.) are deliberately not vendored — the
GUI uses a hand-rolled Wayland + EGL platform layer.

Files are unmodified. Build glue lives in `third_party/imgui/meson.build`
(separate from upstream sources).
