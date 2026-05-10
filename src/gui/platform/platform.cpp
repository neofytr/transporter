// SPDX-License-Identifier: GPL-3.0-or-later
//
// Window lifecycle: Wayland → xdg-shell → wl_shm → ImGui (software render).

#include "platform.hpp"
#include "platform_internal.hpp"

#include <imgui.h>

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <filesystem>

namespace transporter::gui::platform {

const char* init_error_message(InitError e) noexcept {
    switch (e) {
    case InitError::NoDisplay:        return "failed to connect to Wayland display";
    case InitError::MissingProtocol:  return "compositor missing required protocol (xdg_wm_base / wl_shm)";
    case InitError::EglInitFailed:    return "wl_shm buffer allocation failed";
    case InitError::GlLoadFailed:     return "ImGui font build failed";
    case InitError::OutOfMemory:      return "out of memory";
    }
    return "unknown init error";
}

Window::Window() : impl_(std::make_unique<WindowImpl>()) {}

Window::~Window() {
    if (!impl_) return;
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }
    destroy_shm(*impl_);
    destroy_input(*impl_);
    destroy_surface(*impl_);
    destroy_wayland(*impl_);
}

std::expected<std::unique_ptr<Window>, InitError>
Window::create(const WindowConfig& cfg) {
    auto w = std::unique_ptr<Window>(new Window());
    auto& s = *w->impl_;
    s.title  = cfg.title;
    s.width  = cfg.default_width;
    s.height = cfg.default_height;

    if (!init_wayland(s)) {
        return std::unexpected(s.display ? InitError::MissingProtocol
                                         : InitError::NoDisplay);
    }

    register_seat(s);
    if (wl_display_roundtrip(s.display) < 0) {
        return std::unexpected(InitError::MissingProtocol);
    }
    if (!init_surface(s, cfg.title)) {
        return std::unexpected(InitError::MissingProtocol);
    }
    if (!init_shm(s)) {
        return std::unexpected(InitError::EglInitFailed);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    constexpr const char* kFont =
        "/home/raj/.local/share/fonts/JetBrainsMono/"
        "JetBrainsMonoNerdFont-Regular.ttf";
    if (std::filesystem::exists(kFont)) {
        io.Fonts->AddFontFromFileTTF(kFont, 17.0f);
    } else {
        ImFontConfig fcfg;
        fcfg.SizePixels = 15.0f;
        io.Fonts->AddFontDefault(&fcfg);
    }

    // Build font atlas into CPU memory; set a dummy texture ID (no GPU upload).
    if (!io.Fonts->Build()) {
        return std::unexpected(InitError::GlLoadFailed);
    }
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    io.DisplaySize = ImVec2(static_cast<float>(s.width),
                            static_cast<float>(s.height));
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    return w;
}

bool Window::poll() {
    if (!impl_->display) return false;

    if (wl_display_dispatch_pending(impl_->display) < 0) return false;

    while (wl_display_prepare_read(impl_->display) != 0) {
        if (wl_display_dispatch_pending(impl_->display) < 0) return false;
    }
    if (wl_display_flush(impl_->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(impl_->display);
        return false;
    }
    pollfd pfd{wl_display_get_fd(impl_->display), POLLIN, 0};
    const int rc = ::poll(&pfd, 1, 8);
    if (rc > 0 && (pfd.revents & POLLIN) != 0) {
        if (wl_display_read_events(impl_->display) < 0) return false;
        if (wl_display_dispatch_pending(impl_->display) < 0) return false;
    } else {
        wl_display_cancel_read(impl_->display);
    }
    return !impl_->close_requested;
}

void Window::begin_frame() {
    if (impl_->needs_resize) {
        shm_resize(*impl_);
        impl_->needs_resize = false;
    }
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
    render_frame(*impl_, r, g, b);
    shm_commit(*impl_);
    return true;
}

int Window::framebuffer_width()  const noexcept { return impl_->width; }
int Window::framebuffer_height() const noexcept { return impl_->height; }
bool Window::close_requested()   const noexcept { return impl_->close_requested; }
void Window::request_close()     noexcept       { impl_->close_requested = true; }

bool Window::take_media_play_pause() noexcept { return impl_->media_play_pause.exchange(false); }
bool Window::take_media_stop()       noexcept { return impl_->media_stop.exchange(false); }
bool Window::take_media_next()       noexcept { return impl_->media_next.exchange(false); }
bool Window::take_media_prev()       noexcept { return impl_->media_prev.exchange(false); }
bool Window::take_media_mute()       noexcept { return impl_->media_mute.exchange(false); }
bool Window::take_media_vol_up()     noexcept { return impl_->media_vol_up.exchange(false); }
bool Window::take_media_vol_down()   noexcept { return impl_->media_vol_down.exchange(false); }

void Window::resize(int w, int h) {
    impl_->width  = w;
    impl_->height = h;
    impl_->needs_resize = true;
}

} // namespace transporter::gui::platform
