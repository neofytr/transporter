// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_GUI_PLATFORM_INTERNAL_HPP
#define TRANSPORTER_GUI_PLATFORM_INTERNAL_HPP

#include "platform.hpp"

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "xdg-shell-client-protocol.h"

namespace transporter::gui::platform {

enum class RenderMode { CPU, GPU };

struct ShmBuffer {
    wl_buffer* buf  = nullptr;
    uint8_t*   data = nullptr;  // mmap'd pixel data (XRGB8888)
    bool       busy = false;    // true while compositor holds a reference
};

struct WindowImpl {
    // Wayland display + globals
    wl_display*    display    = nullptr;
    wl_registry*   registry   = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base*   wm_base   = nullptr;
    wl_seat*       seat       = nullptr;
    wl_keyboard*   keyboard   = nullptr;
    wl_pointer*    pointer    = nullptr;
    wl_output*     output     = nullptr;
    wl_shm*        shm        = nullptr;

    // Surface stack
    wl_surface*   surface  = nullptr;
    xdg_surface*  xdg_surf = nullptr;
    xdg_toplevel* toplevel = nullptr;

    // Shared-memory pixel buffers (double-buffered; always allocated as fallback)
    ShmBuffer shm_bufs[2];
    int       shm_back   = 0;    // index of the buffer we render into
    int       shm_stride = 0;    // bytes per row = width * 4
    int       shm_size   = 0;    // total bytes = stride * height

    // xkb keyboard model
    ::xkb_context* xkb      = nullptr;
    ::xkb_keymap*  keymap   = nullptr;
    ::xkb_state*   kbd_state = nullptr;

    // Geometry
    int width        = 960;
    int height       = 600;
    int buffer_scale = 1;

    // State flags
    bool configured    = false;
    bool close_requested = false;
    bool needs_resize  = false;

    // Input state
    double mouse_x = 0.0;
    double mouse_y = 0.0;
    bool   mouse_in = false;
    bool   mouse_buttons[5]{};

    // Frame timing
    std::chrono::steady_clock::time_point last_frame_time =
        std::chrono::steady_clock::now();

    std::string title;

    // Keyboard repeat
    int32_t repeat_rate     = 25;
    int32_t repeat_delay_ms = 600;

    // XF86 media key flags — set by input thread, cleared by GUI after reading
    std::atomic<bool> media_play_pause{false};
    std::atomic<bool> media_stop{false};
    std::atomic<bool> media_next{false};
    std::atomic<bool> media_prev{false};
    std::atomic<bool> media_mute{false};
    std::atomic<bool> media_vol_up{false};
    std::atomic<bool> media_vol_down{false};

    // Font atlas (CPU-side; populated on first render call)
    const uint8_t* font_pixels   = nullptr;
    int            font_atlas_w  = 0;
    int            font_atlas_h  = 0;

    RenderMode render_mode = RenderMode::GPU;

    // EGL/GL fields (used only when render_mode == GPU)
    void* egl_display    = nullptr;  // EGLDisplay
    void* egl_surface    = nullptr;  // EGLSurface
    void* egl_context    = nullptr;  // EGLContext
    void* egl_window     = nullptr;  // wl_egl_window*
    bool  gl_initialized = false;
};

// wayland.cpp
bool init_wayland(WindowImpl& w);
void destroy_wayland(WindowImpl& w);
bool dispatch_pending(WindowImpl& w);

// xdg_shell.cpp
bool init_surface(WindowImpl& w, const std::string& title);
void destroy_surface(WindowImpl& w);

// shm.cpp
bool init_shm(WindowImpl& w);
void destroy_shm(WindowImpl& w);
void shm_resize(WindowImpl& w);
void shm_commit(WindowImpl& w);

// render.cpp
void render_frame(WindowImpl& w, float r, float g, float b);

// input.cpp
void register_seat(WindowImpl& w);
void destroy_input(WindowImpl& w);
void input_pump_imgui(WindowImpl& w);

// egl.cpp
bool init_egl(WindowImpl& w);
void destroy_egl(WindowImpl& w);
void egl_swap(WindowImpl& w);
void egl_resize(WindowImpl& w);
// GL viewport/clear + ImGui draw + swap. Called from end_frame on GPU path.
void egl_render_frame(WindowImpl& w, float r, float g, float b);

} // namespace transporter::gui::platform

#endif
