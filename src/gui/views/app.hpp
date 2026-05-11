// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_GUI_VIEWS_APP_HPP
#define TRANSPORTER_GUI_VIEWS_APP_HPP

#include <transporter/config/config.hpp>
#include <transporter/dbus/service.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/library/library.hpp>

#include "lyrics.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace transporter::gui {

enum class ViewId { Main, Library, Pipeline, Queue };

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
    std::string pending_device_switch; // set by DAC combo; cleared after switch
    bool release_dac_requested = false; // set by "Release DAC" button; cleared after teardown

    // Cached HW volume (0-100, -1 = unavailable). Polled at ~4 Hz from main thread.
    int hw_volume_pct = -1;
    bool hw_volume_muted = false;
    std::chrono::steady_clock::time_point hw_volume_last_poll{};

    std::filesystem::path queued_file;  // launch arg, played once
    std::filesystem::path pending_preload_path; // path passed to last preload()

    // Waveform amplitude envelope computed by background scanner.
    // 256 RMS buckets, values in [0, 1], normalised to the loudest bucket.
    std::mutex waveform_mtx;
    std::vector<float> waveform;
    std::atomic<bool> waveform_ready{false};
    std::jthread waveform_scanner;  // destruction requests stop

    // In-process queue. Phase 10 keeps it minimal: command-line file lands
    // here on launch, OpenUri appends, MPRIS Next/Previous walks it. Index
    // is the currently-loaded slot (or -1 when empty).
    std::mutex queue_mtx;
    std::vector<std::filesystem::path> queue;
    std::int32_t queue_index = -1;
    std::chrono::milliseconds queue_total_duration{0};
    // Per-entry metadata resolved from the library. Populated by
    // recompute_queue_duration(); missing entries fall back to path stem + 0ms.
    struct QueueRowInfo {
        std::string label;
        std::chrono::milliseconds duration{};
    };
    std::unordered_map<std::string, QueueRowInfo> queue_row_info;
    bool shuffle = false;
    enum class RepeatMode : std::uint8_t { None, One, All };
    RepeatMode repeat_mode = RepeatMode::None;

    ViewId current_view = ViewId::Main;
    bool mini_mode = false;

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

    // Transient error toast shown at the top of the main view.
    struct Toast {
        std::string msg;
        std::chrono::steady_clock::time_point expires;
    };
    std::optional<Toast> toast;
    std::mutex toast_mtx;

    // Thread-safe. Replaces any existing toast.
    void push_toast(std::string msg, float seconds = 4.0f);

    // Queue helpers; called from any thread (DBus event-loop in particular).
    std::optional<std::filesystem::path> queue_next();
    std::optional<std::filesystem::path> queue_previous();
    void queue_set_single(std::filesystem::path p);
    // Returns the path after the current index without advancing it.
    std::optional<std::filesystem::path> queue_peek_next() const;

    Lyrics lyrics;
    std::filesystem::path lyrics_loaded_for;

    void queue_append(std::filesystem::path p);
    void queue_insert_next(std::filesystem::path p);
    bool queue_jump_to(std::int32_t idx);
    void queue_remove(std::int32_t idx);
    void queue_move(std::int32_t from_idx, std::int32_t to_idx);
    void queue_move_to_top(std::int32_t idx);
    void queue_move_to_bottom(std::int32_t idx);
    void queue_clear();
    void queue_shuffle_toggle();

    // Recomputes queue_total_duration from library metadata.
    // Safe to call from any thread. No-op if library_ is null.
    void recompute_queue_duration();
};

// Each draw_* fn renders a single ImGui window's contents within the current
// frame. They do not push a window themselves; the parent renders the tab bar
// + selector and only forwards to the active view.
void draw_main_view(AppState& st);
void draw_library_view(AppState& st);
void draw_pipeline_view(AppState& st);
void draw_queue_view(AppState& st);
void draw_mini_view(AppState& st);

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
