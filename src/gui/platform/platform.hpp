// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hand-rolled Wayland + xdg-shell + EGL platform shim for the Dear ImGui app.
// No SDL, no GLFW. The Window struct owns every Wayland / EGL / xkb handle
// for the lifetime of the GUI process; teardown is RAII-driven.
//
// Threading: single-threaded. All wayland calls run on the GUI thread. The
// engine + library threads remain decoupled.

#ifndef TRANSPORTER_GUI_PLATFORM_HPP
#define TRANSPORTER_GUI_PLATFORM_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct wl_surface;
struct wl_output;
struct wl_callback;
struct wl_egl_window;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace transporter::gui::platform {

struct WindowConfig {
    std::string title = "transporter";
    int default_width = 960;
    int default_height = 600;
};

// Aggregated reason why init failed. The main process turns this into a
// human-readable stderr line and exits non-zero. We deliberately do NOT
// surface raw libwayland error text here — those calls produce strerror-style
// output that is rarely actionable on its own.
enum class InitError : std::uint8_t {
    NoDisplay,            // wl_display_connect returned null
    MissingProtocol,      // a required global (xdg_wm_base, wl_compositor) is absent
    EglInitFailed,        // eglInitialize / eglChooseConfig / eglCreateContext failed
    GlLoadFailed,         // ImGui's loader rejected the GL context
    OutOfMemory,
};

const char* init_error_message(InitError e) noexcept;

struct WindowImpl;  // defined in platform_internal.hpp

class Window {
public:
    static std::expected<std::unique_ptr<Window>, InitError>
    create(const WindowConfig& cfg);

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;
    ~Window();

    // Pump the wayland event queue without blocking. Returns false if the
    // compositor told us to close.
    bool poll();

    // Begin a frame: ImGui_ImplOpenGL3_NewFrame + our own NewFrame
    // (delta-time, mouse position, keyboard state, display size).
    void begin_frame();

    // End frame, render, and swap. Returns false if EGL swap failed (non-fatal
    // for now — the caller can keep polling).
    bool end_frame(float clear_r, float clear_g, float clear_b);

    int framebuffer_width() const noexcept;
    int framebuffer_height() const noexcept;

    // Set when the user requested close via the compositor (window button or
    // wl_keyboard Esc, depending on what we wire up).
    bool close_requested() const noexcept;

    // Force-close the window (used when the engine reports a fatal error).
    void request_close() noexcept;

    // Resize the window; takes effect at the start of the next begin_frame().
    void resize(int w, int h);

    // Update the compositor window title (best-effort; no-op if surface not ready).
    void set_title(const std::string& title);

    // Read-and-clear XF86 media key flags. Each returns true once per key event.
    bool take_media_play_pause() noexcept;
    bool take_media_stop()       noexcept;
    bool take_media_next()       noexcept;
    bool take_media_prev()       noexcept;
    bool take_media_mute()       noexcept;
    bool take_media_vol_up()     noexcept;
    bool take_media_vol_down()   noexcept;

private:
    Window();
    std::unique_ptr<WindowImpl> impl_;
};

} // namespace transporter::gui::platform

#endif
