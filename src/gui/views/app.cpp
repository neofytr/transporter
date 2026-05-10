// SPDX-License-Identifier: GPL-3.0-or-later
//
// GUI top level. Owns Engine + Library, brings up the Wayland window,
// dispatches between the three views.

#include "app.hpp"
#include "../platform/platform.hpp"

#include <transporter/config/config.hpp>
#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/library/library.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace transporter::gui {

namespace {

const char* state_name(engine::State s) {
    switch (s) {
    case engine::State::Idle:         return "Idle";
    case engine::State::Loading:      return "Loading";
    case engine::State::Playing:      return "Playing";
    case engine::State::Paused:       return "Paused";
    case engine::State::Stopped:      return "Stopped";
    case engine::State::Error:        return "Error";
    case engine::State::Disconnected: return "Disconnected";
    }
    return "?";
}

const char* event_kind_name(engine::Event::Kind k) {
    switch (k) {
    case engine::Event::Kind::StateChanged:  return "state";
    case engine::Event::Kind::TrackLoaded:   return "loaded";
    case engine::Event::Kind::TrackEnded:    return "ended";
    case engine::Event::Kind::RateSwitched:  return "rate-switch";
    case engine::Event::Kind::ErrorOccurred: return "error";
    case engine::Event::Kind::DeviceLost:    return "device-lost";
    case engine::Event::Kind::DeviceReturn:  return "device-return";
    }
    return "?";
}

ViewId default_to_viewid(config::DefaultView v) {
    switch (v) {
    case config::DefaultView::Library:  return ViewId::Library;
    case config::DefaultView::Pipeline: return ViewId::Pipeline;
    case config::DefaultView::Main:     break;
    }
    return ViewId::Main;
}

config::Config load_config_or_defaults(const std::filesystem::path& path) {
    if (auto r = config::load_file(path); r) {
        return std::move(*r);
    }
    return config::Config{};
}

std::string pick_device(const config::Config& cfg,
                        const std::string& override_id,
                        const std::vector<engine::DeviceInfo>& devices) {
    if (!override_id.empty()) {
        return override_id;
    }
    if (!cfg.device.preferred.empty()) {
        return cfg.device.preferred;
    }
    if (!devices.empty()) {
        return devices.front().alsa_hw_string;
    }
    return {};
}

} // namespace

void AppState::push_log(std::string text) {
    std::lock_guard lk(log_mtx);
    log.push_back(LogEntry{std::chrono::steady_clock::now(), std::move(text)});
    while (log.size() > 64) {
        log.pop_front();
    }
}

std::vector<LogEntry> AppState::snapshot_log(std::size_t max) const {
    std::lock_guard lk(const_cast<std::mutex&>(log_mtx));
    std::vector<LogEntry> out;
    const std::size_t take = std::min(max, log.size());
    out.reserve(take);
    for (std::size_t i = log.size() - take; i < log.size(); ++i) {
        out.push_back(log[i]);
    }
    return out;
}

namespace {

void wire_engine_events(AppState& st) {
    if (st.engine_ == nullptr) {
        return;
    }
    st.engine_->set_event_callback([&st](const engine::Event& ev) {
        char line[192];
        switch (ev.kind) {
        case engine::Event::Kind::StateChanged:
            st.last_engine_state.store(ev.state, std::memory_order_relaxed);
            std::snprintf(line, sizeof(line), "%s -> %s",
                          event_kind_name(ev.kind), state_name(ev.state));
            break;
        case engine::Event::Kind::TrackLoaded:
            std::snprintf(line, sizeof(line),
                          "%s rate=%u ch=%u total=%llu",
                          event_kind_name(ev.kind),
                          ev.format.sample_rate_hz, ev.format.channels,
                          static_cast<unsigned long long>(ev.total_frames));
            break;
        case engine::Event::Kind::TrackEnded:
            std::snprintf(line, sizeof(line), "%s", event_kind_name(ev.kind));
            break;
        case engine::Event::Kind::RateSwitched:
            std::snprintf(line, sizeof(line), "%s -> %u Hz",
                          event_kind_name(ev.kind), ev.format.sample_rate_hz);
            break;
        case engine::Event::Kind::ErrorOccurred:
            std::snprintf(line, sizeof(line), "%s [%s] %s",
                          event_kind_name(ev.kind),
                          std::string(engine::error_code_name(ev.error.code)).c_str(),
                          ev.error.message.c_str());
            break;
        case engine::Event::Kind::DeviceLost:
        case engine::Event::Kind::DeviceReturn:
            std::snprintf(line, sizeof(line), "%s", event_kind_name(ev.kind));
            break;
        }
        st.push_log(std::string{line});
    });
}

bool maybe_play_queued_file(AppState& st) {
    if (st.queued_file.empty() || st.engine_ == nullptr) {
        return false;
    }
    auto lr = st.engine_->load(st.queued_file);
    if (!lr) {
        st.push_log(std::string{"queued load failed: "} + lr.error().message);
        st.queued_file.clear();
        return false;
    }
    auto pr = st.engine_->play();
    if (!pr) {
        st.push_log(std::string{"queued play failed: "} + pr.error().message);
    } else {
        st.push_log("loaded: " + st.queued_file.string());
    }
    st.queued_file.clear();
    return true;
}

void open_engine_and_library(AppState& st, const AppArgs& args) {
    // device discovery (best-effort)
    if (auto r = engine::list_playback_devices(); r) {
        st.devices = std::move(*r);
    }
    st.preferred_device = pick_device(st.cfg, args.device_override, st.devices);

    // engine
    if (!st.preferred_device.empty()) {
        engine::EngineConfig ec{};
        ec.device_id = st.preferred_device;
        if (auto e = engine::Engine::create(std::move(ec)); e) {
            st.engine_ = std::move(*e);
            wire_engine_events(st);
        } else {
            st.push_log(std::string{"engine create failed: "} + e.error().message);
        }
    }

    // library
    library::Config lc{};
    lc.db_path = config::default_library_db_path();
    lc.roots = st.cfg.library.roots;
    lc.ignore_patterns = st.cfg.library.ignore_patterns;
    std::error_code ec;
    std::filesystem::create_directories(lc.db_path.parent_path(), ec);
    if (auto l = library::Library::open(std::move(lc)); l) {
        st.library_ = std::move(*l);
        if (!st.cfg.library.roots.empty()) {
            st.library_->rescan_async();
        }
    } else {
        st.push_log(std::string{"library open failed: "} + l.error().message);
    }
}

void draw_tab_bar(AppState& st) {
    constexpr ImVec4 kAccent{0.30f, 0.55f, 0.90f, 1.0f};
    auto tab = [&](const char* label, ViewId id) {
        const bool active = (st.current_view == id);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
        }
        if (ImGui::Button(label)) {
            st.current_view = id;
        }
        if (active) {
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine();
    };
    tab("Main (F1)", ViewId::Main);
    tab("Library (F2)", ViewId::Library);
    tab("Pipeline (F3)", ViewId::Pipeline);
    ImGui::NewLine();
    ImGui::Separator();
}

void handle_view_shortcuts(AppState& st) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        return;  // an InputText / focused widget owns the keystroke
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
        st.current_view = ViewId::Main;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
        st.current_view = ViewId::Library;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) {
        st.current_view = ViewId::Pipeline;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        switch (st.current_view) {
        case ViewId::Main:     st.current_view = ViewId::Library;  break;
        case ViewId::Library:  st.current_view = ViewId::Pipeline; break;
        case ViewId::Pipeline: st.current_view = ViewId::Main;     break;
        }
    }
}

} // namespace

int run(const AppArgs& args) {
    AppState st;
    st.cfg = load_config_or_defaults(args.config_path);
    st.current_view = default_to_viewid(st.cfg.ui.default_view);
    st.queued_file = args.file_to_play;

    auto win_or = platform::Window::create(platform::WindowConfig{});
    if (!win_or) {
        std::fprintf(stderr, "transporter: %s\n",
                     platform::init_error_message(win_or.error()));
        return 1;
    }
    auto& win = **win_or;

    open_engine_and_library(st, args);
    bool queued_done = false;

    while (win.poll()) {
        if (!queued_done) {
            queued_done = maybe_play_queued_file(st);
        }

        win.begin_frame();

        // Full-window dock
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        constexpr ImGuiWindowFlags kFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::Begin("##transporter", nullptr, kFlags);

        draw_tab_bar(st);
        handle_view_shortcuts(st);

        switch (st.current_view) {
        case ViewId::Main:     draw_main_view(st);     break;
        case ViewId::Library:  draw_library_view(st);  break;
        case ViewId::Pipeline: draw_pipeline_view(st); break;
        }

        ImGui::End();

        win.end_frame(0.07f, 0.08f, 0.10f);
    }
    return 0;
}

int run_headless(const AppArgs& args) {
    AppState st;
    st.cfg = load_config_or_defaults(args.config_path);

    if (auto r = engine::list_playback_devices(); r) {
        st.devices = std::move(*r);
    }
    const std::string device_id = pick_device(st.cfg, args.device_override, st.devices);
    if (device_id.empty()) {
        std::fprintf(stderr,
                     "transporter: no playback devices available (no DAC)\n");
        return 2;
    }

    engine::EngineConfig ec{};
    ec.device_id = device_id;
    auto e = engine::Engine::create(std::move(ec));
    if (!e) {
        std::fprintf(stderr, "transporter: engine create failed [%s] %s\n",
                     std::string(engine::error_code_name(e.error().code)).c_str(),
                     e.error().message.c_str());
        return 3;
    }
    st.engine_ = std::move(*e);

    if (args.file_to_play.empty()) {
        std::fprintf(stderr, "transporter: --no-gui requires a file argument\n");
        return 4;
    }

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    int exit_code = 0;
    st.engine_->set_event_callback([&](const engine::Event& ev) {
        switch (ev.kind) {
        case engine::Event::Kind::TrackLoaded:
            std::printf("loaded rate=%u ch=%u total=%llu\n",
                        ev.format.sample_rate_hz, ev.format.channels,
                        static_cast<unsigned long long>(ev.total_frames));
            break;
        case engine::Event::Kind::TrackEnded:
            std::printf("ended\n");
            { std::lock_guard lk(mtx); done = true; }
            cv.notify_all();
            break;
        case engine::Event::Kind::ErrorOccurred:
            std::fprintf(stderr, "error [%s] %s\n",
                         std::string(engine::error_code_name(ev.error.code)).c_str(),
                         ev.error.message.c_str());
            exit_code = 5;
            { std::lock_guard lk(mtx); done = true; }
            cv.notify_all();
            break;
        default: break;
        }
    });

    if (auto r = st.engine_->load(args.file_to_play); !r) {
        std::fprintf(stderr, "load failed: %s\n", r.error().message.c_str());
        return 6;
    }
    if (auto r = st.engine_->play(); !r) {
        std::fprintf(stderr, "play failed: %s\n", r.error().message.c_str());
        return 7;
    }

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&]{ return done; });
    }
    return exit_code;
}

} // namespace transporter::gui
