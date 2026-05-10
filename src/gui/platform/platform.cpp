// SPDX-License-Identifier: GPL-3.0-or-later
//
// Window lifecycle: Wayland -> xdg-shell -> EGL -> ImGui, in that order.
// Bringing up the window without a compositor is the most likely failure
// case; the InitError taxonomy lets the caller log a clean message.

#include "platform.hpp"
#include "platform_internal.hpp"

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>

// Forward declarations of the three GL symbols we touch directly. Defined by
// libGL (or libEGL via OpenGL ES); imgui_impl_opengl3 already pulls in its
// own loader for the rest. Using extern "C" here keeps us out of any
// platform-specific gl.h ABI variation.
extern "C" {
    void glClearColor(float r, float g, float b, float a);
    void glClear(unsigned int mask);
    void glViewport(int x, int y, int w, int h);
}
constexpr unsigned kGlColorBufferBit = 0x00004000u;

namespace transporter::gui::platform {

const char* init_error_message(InitError e) noexcept {
    switch (e) {
    case InitError::NoDisplay:
        return "failed to connect to Wayland: no display environment";
    case InitError::MissingProtocol:
        return "Wayland compositor missing required protocols (xdg_wm_base, wl_compositor)";
    case InitError::EglInitFailed:
        return "EGL initialization failed";
    case InitError::GlLoadFailed:
        return "OpenGL 3.3 context creation failed";
    case InitError::OutOfMemory:
        return "out of memory during GUI initialization";
    }
    return "unknown init error";
}

Window::Window() : impl_(std::make_unique<WindowImpl>()) {}

Window::~Window() {
    if (impl_ == nullptr) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
    }
    destroy_egl(*impl_);
    destroy_input(*impl_);
    destroy_surface(*impl_);
    destroy_wayland(*impl_);
}

std::expected<std::unique_ptr<Window>, InitError>
Window::create(const WindowConfig& cfg) {
    auto w = std::unique_ptr<Window>(new Window());
    auto& s = *w->impl_;
    s.title = cfg.title;
    s.width = cfg.default_width;
    s.height = cfg.default_height;

    if (!init_wayland(s)) {
        const bool no_display = (s.display == nullptr);
        return std::unexpected(no_display ? InitError::NoDisplay
                                          : InitError::MissingProtocol);
    }
    if (s.compositor == nullptr || s.wm_base == nullptr) {
        return std::unexpected(InitError::MissingProtocol);
    }

    register_seat(s);

    // Seat capabilities arrive after the listener is attached; this
    // roundtrip ensures the pointer and keyboard objects are created
    // before we bring up the surface.
    if (wl_display_roundtrip(s.display) < 0) {
        return std::unexpected(InitError::MissingProtocol);
    }

    if (!init_surface(s, cfg.title)) {
        return std::unexpected(InitError::MissingProtocol);
    }

    if (!init_egl(s)) {
        return std::unexpected(InitError::EglInitFailed);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // do not write imgui.ini to the cwd

    // Load JetBrainsMono Nerd Font at 17px for a clean, readable size.
    // Fall back to the built-in bitmap font at 15px if unavailable.
    constexpr const char* kFont =
        "/home/raj/.local/share/fonts/JetBrainsMono/"
        "JetBrainsMonoNerdFont-Regular.ttf";
    if (std::filesystem::exists(kFont)) {
        io.Fonts->AddFontFromFileTTF(kFont, 17.0f);
    } else {
        ImFontConfig cfg;
        cfg.SizePixels = 15.0f;
        io.Fonts->AddFontDefault(&cfg);
    }
    io.DisplaySize = ImVec2(static_cast<float>(s.width),
                            static_cast<float>(s.height));
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        return std::unexpected(InitError::GlLoadFailed);
    }
    return w;
}

bool Window::poll() {
    if (impl_->display == nullptr) {
        return false;
    }

    // Drain anything already queued (mesa's EGL backend may have read frame
    // callbacks into the queue during the prior eglSwapBuffers wait).
    if (wl_display_dispatch_pending(impl_->display) < 0) {
        return false;
    }

    // Read fresh events off the socket via the prepare-read / read_events /
    // dispatch_pending dance. Without this the queue stays empty: input,
    // configure, and frame_done events never arrive, so the next vsync
    // swap blocks forever and the window appears frozen.
    while (wl_display_prepare_read(impl_->display) != 0) {
        if (wl_display_dispatch_pending(impl_->display) < 0) {
            return false;
        }
    }
    if (wl_display_flush(impl_->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(impl_->display);
        return false;
    }
    pollfd pfd{wl_display_get_fd(impl_->display), POLLIN, 0};
    int rc = ::poll(&pfd, 1, 0);  // non-blocking; eglSwapBuffers paces us
    if (rc > 0 && (pfd.revents & POLLIN) != 0) {
        if (wl_display_read_events(impl_->display) < 0) {
            return false;
        }
        if (wl_display_dispatch_pending(impl_->display) < 0) {
            return false;
        }
    } else {
        wl_display_cancel_read(impl_->display);
    }

    return !impl_->close_requested;
}

void Window::begin_frame() {
    if (impl_->needs_resize) {
        egl_resize(*impl_);
        impl_->needs_resize = false;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(impl_->width),
                            static_cast<float>(impl_->height));
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<float> dt = now - impl_->last_frame_time;
    impl_->last_frame_time = now;
    io.DeltaTime = dt.count() > 0.0f ? dt.count() : (1.0f / 60.0f);
    input_pump_imgui(*impl_);
    ImGui::NewFrame();
}

bool Window::end_frame(float r, float g, float b) {
    ImGui::Render();
    glViewport(0, 0, impl_->width, impl_->height);
    glClearColor(r, g, b, 1.0f);
    glClear(kGlColorBufferBit);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return egl_swap(*impl_);
}

int Window::framebuffer_width() const noexcept { return impl_->width; }
int Window::framebuffer_height() const noexcept { return impl_->height; }

bool Window::close_requested() const noexcept { return impl_->close_requested; }
void Window::request_close() noexcept { impl_->close_requested = true; }

} // namespace transporter::gui::platform
