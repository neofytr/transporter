// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_GUI_VIEWS_APP_HPP
#define TRANSPORTER_GUI_VIEWS_APP_HPP

#include <transporter/config/config.hpp>
#include <transporter/dbus/service.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/library/library.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace transporter::gui {

enum class ViewId { Main, Library, Pipeline };

struct LogEntry {
    std::chrono::steady_clock::time_point at;
    std::string text;
};

struct AppState {
    std::filesystem::path config_path;
    config::Config cfg;
    std::unique_ptr<engine::Engine> engine_;
    std::unique_ptr<library::Library> library_;
    std::unique_ptr<dbus_svc::DbusService> dbus_;

    std::string preferred_device;     // last applied --device or [device].preferred
    std::vector<engine::DeviceInfo> devices;

    std::filesystem::path queued_file;  // launch arg, played once

    // In-process queue. Phase 10 keeps it minimal: command-line file lands
    // here on launch, OpenUri appends, MPRIS Next/Previous walks it. Index
    // is the currently-loaded slot (or -1 when empty).
    std::mutex queue_mtx;
    std::vector<std::filesystem::path> queue;
    std::int32_t queue_index = -1;

    ViewId current_view = ViewId::Main;

    // Library UI state
    std::string library_query;
    std::string selected_artist;
    std::int64_t selected_album_id = 0;
    std::string library_status;       // e.g. "scanning... 234 / 1024"

    // Engine event log (kept short, ~64 entries)
    std::mutex log_mtx;
    std::deque<LogEntry> log;
    std::atomic<engine::State> last_engine_state{engine::State::Idle};

    void push_log(std::string text);
    std::vector<LogEntry> snapshot_log(std::size_t max = 12) const;

    // Queue helpers; called from any thread (DBus event-loop in particular).
    std::optional<std::filesystem::path> queue_next();
    std::optional<std::filesystem::path> queue_previous();
    void queue_set_single(std::filesystem::path p);
};

// Each draw_* fn renders a single ImGui window's contents within the current
// frame. They do not push a window themselves; the parent renders the tab bar
// + selector and only forwards to the active view.
void draw_main_view(AppState& st);
void draw_library_view(AppState& st);
void draw_pipeline_view(AppState& st);

// Test seam: render the pipeline view directly from a snapshot, decoupled
// from Engine. Same content, no engine pointer dereferenced.
void draw_pipeline_view_with_snapshot(const engine::PipelineSnapshot& snap);

// Runs the GUI loop; consumes argv quirks already parsed by main().
// Returns the process exit code.
struct AppArgs {
    std::filesystem::path config_path;
    std::filesystem::path file_to_play;
    std::string device_override;
};

int run(const AppArgs& args);

// Headless (no Wayland) telemetry-style run for `--no-gui`. Loads the file,
// optionally pinning the device, plays until EOF or fatal, prints brief
// status lines to stdout, returns process exit code.
int run_headless(const AppArgs& args);

} // namespace transporter::gui

#endif
