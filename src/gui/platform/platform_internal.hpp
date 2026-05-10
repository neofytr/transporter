// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared internal state for the platform layer. Each translation unit
// (wayland.cpp, xdg_shell.cpp, egl.cpp, input.cpp) gets visibility into
// Window::Impl through this header.

#ifndef TRANSPORTER_GUI_PLATFORM_INTERNAL_HPP
#define TRANSPORTER_GUI_PLATFORM_INTERNAL_HPP

#include "platform.hpp"

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>

#include <xkbcommon/xkbcommon.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "xdg-shell-client-protocol.h"

namespace transporter::gui::platform {

struct WindowImpl {
    // Wayland display + globals (registry-bound)
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
    wl_output* output = nullptr;

    // Surface stack
    wl_surface* surface = nullptr;
    xdg_surface* xdg_surf = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_egl_window* egl_window = nullptr;

    // EGL
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_ctx = EGL_NO_CONTEXT;
    EGLSurface egl_surf = EGL_NO_SURFACE;
    EGLConfig egl_config = nullptr;

    // xkb keyboard model
    ::xkb_context* xkb = nullptr;
    ::xkb_keymap* keymap = nullptr;
    ::xkb_state* kbd_state = nullptr;

    // Geometry
    int width = 960;
    int height = 600;
    int buffer_scale = 1;

    // State flags
    bool configured = false;
    bool close_requested = false;
    bool needs_resize = false;

    // Input running state
    double mouse_x = 0.0;
    double mouse_y = 0.0;
    bool mouse_in = false;
    bool mouse_buttons[5]{}; // L M R X1 X2

    // Frame timing
    std::chrono::steady_clock::time_point last_frame_time =
        std::chrono::steady_clock::now();

    // Title (kept for re-application after configure)
    std::string title;

    // Repeat state from wl_keyboard.repeat_info
    int32_t repeat_rate = 25;
    int32_t repeat_delay_ms = 600;
};

// wayland.cpp
bool init_wayland(WindowImpl& w);
void destroy_wayland(WindowImpl& w);
bool dispatch_pending(WindowImpl& w);

// xdg_shell.cpp
bool init_surface(WindowImpl& w, const std::string& title);
void destroy_surface(WindowImpl& w);

// egl.cpp
bool init_egl(WindowImpl& w);
void destroy_egl(WindowImpl& w);
bool egl_swap(WindowImpl& w);
bool egl_resize(WindowImpl& w);

// input.cpp
void register_seat(WindowImpl& w);
void destroy_input(WindowImpl& w);
void input_pump_imgui(WindowImpl& w);

} // namespace transporter::gui::platform

#endif
