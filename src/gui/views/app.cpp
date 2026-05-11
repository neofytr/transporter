// SPDX-License-Identifier: GPL-3.0-or-later
//
// GUI top level. Owns Engine + Library, brings up the Wayland window,
// dispatches between the three views.

#include "app.hpp"
#include "playlist.hpp"
#include "../platform/platform.hpp"

#include <transporter/config/config.hpp>
#include <transporter/dbus/service.hpp>
#include <transporter/engine/decoder_factory.hpp>
#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/library/library.hpp>
#include <transporter/theme/theme.hpp>

#include <imgui.h>

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

void AppState::push_toast(std::string msg, float seconds) {
    std::lock_guard lk(toast_mtx);
    toast = Toast{
        std::move(msg),
        std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(seconds)),
    };
}

void AppState::push_log(std::string text) {
    std::lock_guard lk(log_mtx);
    log.push_back(LogEntry{std::chrono::steady_clock::now(), std::move(text)});
    while (log.size() > 64) {
        log.pop_front();
    }
}

std::vector<LogEntry> AppState::snapshot_log(std::size_t max) const {
    std::lock_guard lk(log_mtx);
    std::vector<LogEntry> out;
    const std::size_t take = std::min(max, log.size());
    out.reserve(take);
    for (std::size_t i = log.size() - take; i < log.size(); ++i) {
        out.push_back(log[i]);
    }
    return out;
}

std::optional<std::filesystem::path> AppState::queue_next() {
    std::lock_guard lk(queue_mtx);
    if (queue.empty()) return std::nullopt;

    if (repeat_mode == RepeatMode::One) {
        // Stay on current track
        if (queue_index < 0 || queue_index >= static_cast<std::int32_t>(queue.size()))
            return std::nullopt;
        return queue[static_cast<std::size_t>(queue_index)];
    }

    if (shuffle) {
        // Pick a random track other than the current one (if more than 1 track).
        if (queue.size() == 1) {
            if (repeat_mode == RepeatMode::All) return queue[0];
            return std::nullopt;
        }
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<std::int32_t> dist{
            0, static_cast<std::int32_t>(queue.size()) - 1};
        std::int32_t next;
        do {
            next = dist(rng);
        } while (next == queue_index);
        queue_index = next;
        return queue[static_cast<std::size_t>(queue_index)];
    }

    const std::int32_t next = queue_index + 1;
    if (next >= static_cast<std::int32_t>(queue.size())) {
        if (repeat_mode == RepeatMode::All) {
            queue_index = 0;
            return queue[0];
        }
        return std::nullopt;
    }
    queue_index = next;
    return queue[static_cast<std::size_t>(queue_index)];
}

std::optional<std::filesystem::path> AppState::queue_previous() {
    std::lock_guard lk(queue_mtx);
    if (queue.empty()) return std::nullopt;
    if (queue_index <= 0) {
        if (repeat_mode == RepeatMode::All) {
            queue_index = static_cast<std::int32_t>(queue.size()) - 1;
            return queue[static_cast<std::size_t>(queue_index)];
        }
        return std::nullopt;
    }
    --queue_index;
    return queue[static_cast<std::size_t>(queue_index)];
}

void AppState::queue_set_single(std::filesystem::path p) {
    {
        std::lock_guard lk(queue_mtx);
        queue.clear();
        queue.emplace_back(std::move(p));
        queue_index = 0;
        pending_preload_path.clear();
    }
    recompute_queue_duration();
}

std::optional<std::filesystem::path> AppState::queue_peek_next() const {
    std::lock_guard lk(queue_mtx);
    if (queue.empty() || queue_index < 0) {
        return std::nullopt;
    }
    if (repeat_mode == RepeatMode::One) {
        return queue[static_cast<std::size_t>(queue_index)];
    }
    const std::int32_t next = queue_index + 1;
    if (next >= static_cast<std::int32_t>(queue.size())) {
        if (repeat_mode == RepeatMode::All) {
            return queue[0];
        }
        return std::nullopt;
    }
    return queue[static_cast<std::size_t>(next)];
}

void AppState::queue_append(std::filesystem::path p) {
    {
        std::lock_guard lk(queue_mtx);
        queue.emplace_back(std::move(p));
    }
    recompute_queue_duration();
    // If appending just created a new "next" slot for the current track,
    // issue a preload so gapless can stage it.
    refresh_preload();
}

void AppState::queue_insert_next(std::filesystem::path p) {
    {
        std::lock_guard lk(queue_mtx);
        const std::int32_t ins = queue_index + 1;
        if (ins <= 0 || ins > static_cast<std::int32_t>(queue.size())) {
            queue.emplace_back(std::move(p));
        } else {
            queue.insert(queue.begin() + ins, std::move(p));
        }
    }
    recompute_queue_duration();
    refresh_preload();
}

bool AppState::queue_jump_to(std::int32_t idx) {
    if (engine_ == nullptr) {
        return false;
    }
    std::filesystem::path target;
    {
        std::lock_guard lk(queue_mtx);
        if (idx < 0 || idx >= static_cast<std::int32_t>(queue.size())) {
            return false;
        }
        queue_index = idx;
        target = queue[static_cast<std::size_t>(idx)];
        // Clear before load so TrackLoaded doesn't spuriously advance queue_index
        // when target happens to equal the previously-preloaded path.
        pending_preload_path.clear();
    }
    if (!engine_->load(target)) {
        return false;
    }
    (void)engine_->play();
    return true;
}

void AppState::queue_remove(std::int32_t idx) {
    {
        std::lock_guard lk(queue_mtx);
        if (idx < 0 || idx >= static_cast<std::int32_t>(queue.size())) {
            return;
        }
        queue.erase(queue.begin() + idx);
        if (queue.empty()) {
            queue_index = -1;
        } else if (idx <= queue_index) {
            --queue_index;
            if (queue_index < 0) {
                queue_index = 0;
            }
        }
    }
    recompute_queue_duration();
    refresh_preload();
}

void AppState::refresh_preload() {
    if (engine_ == nullptr) return;
    engine_->cancel_preload();
    // Peek first (acquires queue_mtx internally), then update pending_preload_path
    // under queue_mtx so the engine event callback sees a consistent view.
    auto peek = queue_peek_next();
    {
        std::lock_guard lk(queue_mtx);
        pending_preload_path = peek ? *peek : std::filesystem::path{};
    }
    if (peek) {
        (void)engine_->preload(*peek);
    }
}

void AppState::queue_move(std::int32_t from_idx, std::int32_t to_idx) {
    {
        std::lock_guard lk(queue_mtx);
        const auto n = static_cast<std::int32_t>(queue.size());
        if (from_idx < 0 || from_idx >= n || to_idx < 0 || to_idx >= n) {
            return;
        }
        std::swap(queue[static_cast<std::size_t>(from_idx)],
                  queue[static_cast<std::size_t>(to_idx)]);
        if (queue_index == from_idx) {
            queue_index = to_idx;
        } else if (queue_index == to_idx) {
            queue_index = from_idx;
        }
    }
    refresh_preload();
}

void AppState::queue_move_to_top(std::int32_t idx) {
    {
        std::lock_guard lk(queue_mtx);
        const auto n = static_cast<std::int32_t>(queue.size());
        if (idx <= 0 || idx >= n) {
            return;
        }
        std::rotate(queue.begin(), queue.begin() + idx, queue.begin() + idx + 1);
        if (queue_index == idx) {
            queue_index = 0;
        } else if (queue_index < idx) {
            ++queue_index;
        }
    }
    refresh_preload();
}

void AppState::queue_move_to_bottom(std::int32_t idx) {
    {
        std::lock_guard lk(queue_mtx);
        const auto n = static_cast<std::int32_t>(queue.size());
        if (idx < 0 || idx >= n - 1) {
            return;
        }
        std::rotate(queue.begin() + idx, queue.begin() + idx + 1, queue.end());
        if (queue_index == idx) {
            queue_index = n - 1;
        } else if (queue_index > idx) {
            --queue_index;
        }
    }
    refresh_preload();
}

void AppState::queue_clear() {
    std::lock_guard lk(queue_mtx);
    queue.clear();
    queue_index = -1;
    queue_total_duration = std::chrono::milliseconds{0};
    queue_row_info.clear();
}

void AppState::queue_shuffle_toggle() {
    shuffle = !shuffle;
}

void AppState::recompute_queue_duration() {
    if (library_ == nullptr) {
        return;
    }
    std::vector<std::filesystem::path> snap;
    {
        std::lock_guard lk(queue_mtx);
        snap = queue;
    }
    if (snap.empty()) {
        std::lock_guard lk(queue_mtx);
        queue_total_duration = std::chrono::milliseconds{0};
        queue_row_info.clear();
        return;
    }
    // Look up only the tracks in the queue by path (uses idx_tracks_path).
    std::unordered_map<std::string, QueueRowInfo> row_info;
    row_info.reserve(snap.size());
    std::chrono::milliseconds total{0};
    for (const auto& p : snap) {
        const std::string key = p.string();
        if (row_info.count(key)) continue; // dedup repeated paths
        if (auto t = library_->track_by_path(p); t) {
            QueueRowInfo ri;
            ri.duration = t->duration;
            if (!t->title.empty()) {
                ri.label = t->title;
                if (!t->artist.empty()) {
                    ri.label += "  \xe2\x80\x94  ";
                    ri.label += t->artist;
                }
            }
            total += t->duration;
            row_info.emplace(key, std::move(ri));
        }
    }
    {
        std::lock_guard lk(queue_mtx);
        queue_total_duration = total;
        queue_row_info = std::move(row_info);
    }
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
            // Cancel any previous scanner and start a fresh one for the new file.
            st.waveform_ready.store(false, std::memory_order_relaxed);
            { std::lock_guard lk(st.waveform_mtx); st.waveform.clear(); }
            st.waveform_scanner = std::jthread([&st, fpath = ev.file_path](std::stop_token stop) {
                auto res = engine::open_decoder(fpath);
                if (!res) return;
                auto& dec = *res;
                const std::uint64_t total = dec->total_frames();
                if (total == 0) return;
                const engine::PcmFormat fmt = dec->format();
                if (fmt.channels == 0 || fmt.frame_bytes() == 0) return;

                constexpr std::size_t N = 256;
                std::vector<double> sums(N, 0.0);
                std::vector<std::uint64_t> counts(N, 0);

                constexpr std::size_t CHUNK = 4096;
                std::vector<std::byte> buf(CHUNK * fmt.frame_bytes());
                std::uint64_t frame = 0;

                while (!stop.stop_requested()) {
                    auto rd = dec->read(std::span{buf}, CHUNK);
                    if (!rd || *rd == 0) break;
                    const std::size_t got = *rd;
                    for (std::size_t f = 0; f < got && !stop.stop_requested(); ++f, ++frame) {
                        const std::size_t bucket = static_cast<std::size_t>(
                            (frame * N) / total);
                        if (bucket >= N) continue;
                        const std::byte* src = buf.data() + f * fmt.frame_bytes();
                        float s = 0.0f;
                        using SF = engine::SampleFormat;
                        switch (fmt.sample_format) {
                        case SF::S16_LE: {
                            std::int16_t v = 0;
                            std::memcpy(&v, src, 2);
                            s = v / 32768.0f;
                            break;
                        }
                        case SF::S24_3LE: {
                            std::int32_t v = 0;
                            std::memcpy(reinterpret_cast<std::byte*>(&v), src, 3);
                            if (v & 0x800000) v |= static_cast<std::int32_t>(0xFF000000u);
                            s = static_cast<float>(v) / 8388608.0f;
                            break;
                        }
                        case SF::S24_LE:
                        case SF::S32_LE: {
                            std::int32_t v = 0;
                            std::memcpy(&v, src, 4);
                            s = static_cast<float>(v) / 2147483648.0f;
                            break;
                        }
                        case SF::FLOAT_LE:
                            std::memcpy(&s, src, 4);
                            break;
                        }
                        sums[bucket] += static_cast<double>(s) * static_cast<double>(s);
                        counts[bucket]++;
                    }
                }

                if (stop.stop_requested()) return;

                std::vector<float> env(N);
                float peak = 0.0f;
                for (std::size_t i = 0; i < N; ++i) {
                    env[i] = counts[i] > 0
                        ? static_cast<float>(std::sqrt(sums[i] / static_cast<double>(counts[i])))
                        : 0.0f;
                    peak = std::max(peak, env[i]);
                }
                if (peak > 0.0f) for (auto& v : env) v /= peak;

                { std::lock_guard lk(st.waveform_mtx); st.waveform = std::move(env); }
                st.waveform_ready.store(true, std::memory_order_release);
            });
            // If the loaded file matches the preloaded path, a gapless swap
            // just occurred: advance queue_index to reflect the new position.
            // pending_preload_path is guarded by queue_mtx (shared with the
            // main thread which also writes it via refresh_preload).
            {
                std::lock_guard lk(st.queue_mtx);
                if (!st.pending_preload_path.empty() &&
                    ev.file_path == st.pending_preload_path) {
                    if (st.queue_index + 1 <
                            static_cast<std::int32_t>(st.queue.size())) {
                        ++st.queue_index;
                    }
                }
                st.pending_preload_path.clear();
            }
            // Speculatively open the next decoder so it is ready at EOF.
            // queue_peek_next acquires queue_mtx internally; take the peek
            // result before re-locking to avoid recursion.
            if (st.engine_ != nullptr) {
                auto peek = st.queue_peek_next();
                {
                    std::lock_guard lk(st.queue_mtx);
                    st.pending_preload_path = peek ? peek->string() : std::string{};
                }
                if (peek) {
                    (void)st.engine_->preload(*peek);
                }
            }
            if (st.dbus_) st.dbus_->notify_track_loaded();
            break;
        case engine::Event::Kind::TrackEnded:
            std::snprintf(line, sizeof(line), "%s", event_kind_name(ev.kind));
            // Gapless swap already happened in the decoder thread if formats
            // matched; TrackEnded fires only on format mismatch or when no
            // preload was staged — advance queue and reload normally.
            {
                std::lock_guard lk(st.queue_mtx);
                st.pending_preload_path.clear();
            }
            if (auto next = st.queue_next(); next) {
                if (st.engine_->load(*next)) {
                    (void)st.engine_->play();
                }
            }
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
            st.push_toast(ev.error.message);
            break;
        case engine::Event::Kind::DeviceLost:
            std::snprintf(line, sizeof(line), "%s", event_kind_name(ev.kind));
            st.push_toast("DAC disconnected — waiting for reconnection", 10.0f);
            break;
        case engine::Event::Kind::DeviceReturn:
            std::snprintf(line, sizeof(line), "%s", event_kind_name(ev.kind));
            st.push_toast("DAC reconnected", 3.0f);
            break;
        }
        st.push_log(std::string{line});
    });
}

static bool is_playlist_ext(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".m3u" || ext == ".m3u8";
}

bool maybe_play_queued_file(AppState& st) {
    if (st.queued_file.empty() || st.engine_ == nullptr) {
        return false;
    }

    if (is_playlist_ext(st.queued_file)) {
        auto tracks = transporter::gui::load_playlist(st.queued_file);
        st.queued_file.clear();
        if (tracks.empty()) {
            return false;
        }
        {
            std::lock_guard lk(st.queue_mtx);
            st.queue = std::move(tracks);
            st.queue_index = 0;
        }
        st.recompute_queue_duration();
        const std::filesystem::path first = [&] {
            std::lock_guard lk(st.queue_mtx);
            return st.queue[0];
        }();
        auto lr = st.engine_->load(first);
        if (!lr) {
            st.push_log(std::string{"queued load failed: "} + lr.error().message);
            return false;
        }
        auto pr = st.engine_->play();
        if (!pr) {
            st.push_log(std::string{"queued play failed: "} + pr.error().message);
        } else {
            st.push_log("loaded: " + first.string());
            if (st.dbus_ != nullptr) {
                st.dbus_->notify_track_loaded();
            }
        }
        return true;
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
        if (st.dbus_ != nullptr) {
            st.dbus_->notify_track_loaded();
        }
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
    if (st.preferred_device.empty()) {
        std::fprintf(stderr, "transporter: no playback device found "
                     "(%zu device(s) enumerated)\n", st.devices.size());
        st.push_log("no playback device selected");
    } else {
        engine::EngineConfig ec{};
        ec.device_id = st.preferred_device;
        std::fprintf(stderr, "transporter: opening %s\n",
                     st.preferred_device.c_str());
        if (auto e = engine::Engine::create(std::move(ec)); e) {
            st.engine_ = std::move(*e);
            wire_engine_events(st);
        } else {
            std::fprintf(stderr, "transporter: engine create failed: %s\n",
                         e.error().message.c_str());
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

void open_dbus_service(AppState& st) {
    if (!st.cfg.dbus.enabled) {
        return;
    }
    dbus_svc::Hooks hooks;
    hooks.next = [&st]() -> std::optional<std::filesystem::path> {
        { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
        return st.queue_next();
    };
    hooks.previous = [&st]() -> std::optional<std::filesystem::path> {
        { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
        return st.queue_previous();
    };
    hooks.reload_config = [&st]() -> bool {
        if (st.config_path.empty()) {
            return false;
        }
        auto r = config::load_file(st.config_path);
        if (!r) {
            return false;
        }
        // Hot-reload: library roots, ignore patterns, and theme override path.
        // Device, audio policy, and dbus.enabled are not hot.
        st.cfg.library = r->library;
        st.cfg.theme = r->theme;
        if (st.library_ != nullptr) {
            st.library_->set_roots(r->library.roots);
            st.library_->set_ignore_patterns(r->library.ignore_patterns);
            st.library_->rescan_async();
        }
        return true;
    };
    hooks.rescan_library = [&st] {
        if (st.library_ != nullptr) {
            st.library_->rescan_async();
        }
    };
    hooks.shuffle_getter = [&st] { return st.shuffle; };
    hooks.shuffle_setter = [&st](bool v) {
        std::lock_guard lk(st.queue_mtx);
        st.shuffle = v;
    };
    hooks.loop_status_getter = [&st]() -> std::string {
        switch (st.repeat_mode) {
        case AppState::RepeatMode::One:  return "Track";
        case AppState::RepeatMode::All:  return "Playlist";
        default:                         return "None";
        }
    };
    hooks.loop_status_setter = [&st](const std::string& v) {
        AppState::RepeatMode m = AppState::RepeatMode::None;
        if (v == "Track")    m = AppState::RepeatMode::One;
        else if (v == "Playlist") m = AppState::RepeatMode::All;
        std::lock_guard lk(st.queue_mtx);
        st.repeat_mode = m;
    };
    dbus_svc::Config dc;
    dc.enabled = st.cfg.dbus.enabled;
    auto svc = dbus_svc::DbusService::start(
        st.engine_.get(), st.library_.get(), std::move(hooks), dc);
    if (svc) {
        st.dbus_ = std::move(*svc);
    } else {
        st.push_log(std::string{"dbus start failed: "} +
                    svc.error().message);
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
    // Show a simple scan-in-progress suffix when the library is active.
    {
        const bool scanning = st.library_ != nullptr &&
            st.library_->progress().state != library::ScanState::Idle;
        tab(scanning ? "Library* (F2)" : "Library (F2)", ViewId::Library);
    }
    tab("Pipeline (F3)", ViewId::Pipeline);
    tab("Queue (F4)", ViewId::Queue);
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
    if (ImGui::IsKeyPressed(ImGuiKey_F4)) {
        st.current_view = ViewId::Queue;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        switch (st.current_view) {
        case ViewId::Main:     st.current_view = ViewId::Library;  break;
        case ViewId::Library:  st.current_view = ViewId::Pipeline; break;
        case ViewId::Pipeline: st.current_view = ViewId::Queue;    break;
        case ViewId::Queue:    st.current_view = ViewId::Main;     break;
        }
    }

    // Transport shortcuts — Ctrl+arrow skips tracks; bare arrow seeks ±5 s.
    if (!st.engine_) return;

    const auto eng_state = st.last_engine_state.load(std::memory_order_relaxed);

    if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        if (eng_state == engine::State::Playing) (void)st.engine_->pause();
        else                                     (void)st.engine_->play();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        if (io.KeyCtrl) {
            { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
            if (auto n = st.queue_next()) {
                (void)st.engine_->load(*n);
                (void)st.engine_->play();
                if (st.dbus_) st.dbus_->notify_track_loaded();
            }
        } else {
            const auto snap = st.engine_->pipeline_snapshot();
            if (snap.source.sample_rate_hz > 0) {
                const std::uint64_t fw = snap.output.frames_written
                                       - snap.output.frames_written_at_track_start;
                const std::uint64_t target =
                    std::min(fw + 5 * static_cast<std::uint64_t>(snap.source.sample_rate_hz),
                             snap.source.total_frames);
                (void)st.engine_->seek(target);
                if (st.dbus_) {
                    const auto pos_us = static_cast<std::int64_t>(
                        (target * 1'000'000ULL) / snap.source.sample_rate_hz);
                    st.dbus_->notify_seeked(pos_us);
                }
            }
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (io.KeyCtrl) {
            { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
            if (auto p = st.queue_previous()) {
                (void)st.engine_->load(*p);
                (void)st.engine_->play();
                if (st.dbus_) st.dbus_->notify_track_loaded();
            }
        } else {
            const auto snap = st.engine_->pipeline_snapshot();
            if (snap.source.sample_rate_hz > 0) {
                const std::uint64_t fw = snap.output.frames_written
                                       - snap.output.frames_written_at_track_start;
                const std::uint64_t back = 5 * static_cast<std::uint64_t>(snap.source.sample_rate_hz);
                const std::uint64_t target = fw > back ? fw - back : 0;
                (void)st.engine_->seek(target);
                if (st.dbus_) {
                    const auto pos_us = static_cast<std::int64_t>(
                        (target * 1'000'000ULL) / snap.source.sample_rate_hz);
                    st.dbus_->notify_seeked(pos_us);
                }
            }
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
        if (st.hw_volume_pct >= 0) {
            const auto& hw = st.engine_->pipeline_snapshot().device.current_hw_string;
            if (!hw.empty()) {
                const int pct = std::clamp(st.hw_volume_pct + 5, 0, 100);
                engine::set_hw_volume_pct(hw, pct);
                st.hw_volume_pct = pct;
                st.hw_volume_last_poll = {};
                if (st.dbus_) { st.dbus_->notify_volume_changed(); }
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
        if (st.hw_volume_pct >= 0) {
            const auto& hw = st.engine_->pipeline_snapshot().device.current_hw_string;
            if (!hw.empty()) {
                const int pct = std::clamp(st.hw_volume_pct - 5, 0, 100);
                engine::set_hw_volume_pct(hw, pct);
                st.hw_volume_pct = pct;
                st.hw_volume_last_poll = {};
                if (st.dbus_) { st.dbus_->notify_volume_changed(); }
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M)) {
        if (st.hw_volume_pct >= 0) {
            const auto& hw = st.engine_->pipeline_snapshot().device.current_hw_string;
            if (!hw.empty()) {
                engine::toggle_hw_mute(hw);
                st.hw_volume_last_poll = {};
                if (st.dbus_) st.dbus_->notify_volume_changed();
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S)) {
        st.queue_shuffle_toggle();
        if (st.dbus_) {
            st.dbus_->notify_shuffle_changed();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        switch (st.repeat_mode) {
        case AppState::RepeatMode::None: st.repeat_mode = AppState::RepeatMode::One;  break;
        case AppState::RepeatMode::One:  st.repeat_mode = AppState::RepeatMode::All;  break;
        case AppState::RepeatMode::All:  st.repeat_mode = AppState::RepeatMode::None; break;
        }
        if (st.dbus_) {
            st.dbus_->notify_loop_status_changed();
        }
    }
}

} // namespace

int run(const AppArgs& args) {
    AppState st;

    static std::atomic<bool> s_signal_exit{false};
    struct SigGuard {
        ~SigGuard() { s_signal_exit.store(false); }
    };
    SigGuard sig_guard;

    {
        struct sigaction sa{};
        sa.sa_handler = [](int) { s_signal_exit.store(true, std::memory_order_relaxed); };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT,  &sa, nullptr);
    }

    st.config_path = args.config_path;
    st.cfg = load_config_or_defaults(args.config_path);
    st.current_view = default_to_viewid(st.cfg.ui.default_view);
    st.queued_file = args.file_to_play;
    if (!st.queued_file.empty() && !is_playlist_ext(st.queued_file)) {
        st.queue_set_single(st.queued_file);
    }

    // Restore session state from previous run
    {
        const char* home = std::getenv("HOME");
        if (home) {
            namespace fs = std::filesystem;
            const fs::path sf =
                fs::path{home} / ".cache" / "transporter" / "session";
            if (std::ifstream f{sf}; f) {
                std::vector<fs::path> saved_queue;
                std::int32_t saved_index = -1;
                std::string line;
                while (std::getline(f, line)) {
                    if (line.starts_with("shuffle="))
                        st.shuffle = (line.back() == '1');
                    else if (line.starts_with("repeat=")) {
                        const char c = line.back();
                        if (c == '1') st.repeat_mode = AppState::RepeatMode::One;
                        else if (c == '2') st.repeat_mode = AppState::RepeatMode::All;
                        else st.repeat_mode = AppState::RepeatMode::None;
                    } else if (line.starts_with("queue_index=")) {
                        try { saved_index = std::stoi(line.substr(12)); }
                        catch (...) {}
                    } else if (line.starts_with("queue=")) {
                        fs::path p{line.substr(6)};
                        std::error_code ec;
                        if (fs::exists(p, ec) && !ec) saved_queue.push_back(std::move(p));
                    }
                }
                // Restore queue only when no CLI file was specified.
                if (args.file_to_play.empty() && !saved_queue.empty()) {
                    std::lock_guard lk(st.queue_mtx);
                    st.queue = std::move(saved_queue);
                    if (saved_index >= 0 &&
                        saved_index < static_cast<std::int32_t>(st.queue.size()))
                        st.queue_index = saved_index;
                    else
                        st.queue_index = 0;
                }
            }
        }
    }

    auto win_or = platform::Window::create(platform::WindowConfig{});
    if (!win_or) {
        std::fprintf(stderr, "transporter: %s\n",
                     platform::init_error_message(win_or.error()));
        return 1;
    }
    auto& win = **win_or;

    {
        const char* home = std::getenv("HOME");
        const auto hypr_conf = std::filesystem::path{home ? home : ""} /
                               ".config/hypr/hyprland.conf";
        const auto t = theme::Theme::load(hypr_conf);
        theme::apply_to_imgui(t);
    }

    // Engine, library, and DBus start on a background thread. The Wayland
    // event loop runs from the very first frame so compositor pings are
    // answered and the window stays responsive during libasound / DBus init.
    std::atomic<bool> backend_ready{false};
    std::thread backend_thread([&]() {
        open_engine_and_library(st, args);
        open_dbus_service(st);
        backend_ready.store(true, std::memory_order_release);
    });

    bool queued_done = false;
    bool queue_metadata_refreshed = false;
    bool was_mini = false;

    while (win.poll()) {
        if (s_signal_exit.load(std::memory_order_relaxed)) {
            win.request_close();
            break;
        }

        const bool ready = backend_ready.load(std::memory_order_acquire);

        // Hot DAC switch requested from the combo box
        if (ready && !st.pending_device_switch.empty()) {
            const std::string new_dev = std::move(st.pending_device_switch);
            st.pending_device_switch.clear();

            // Remember what was playing so we can reload on the new device.
            std::filesystem::path reload_path;
            if (st.engine_) {
                auto snap = st.engine_->pipeline_snapshot();
                if (!snap.source.file_path.empty()) {
                    reload_path = snap.source.file_path;
                }
            }
            if (reload_path.empty()) {
                std::lock_guard lk(st.queue_mtx);
                if (st.queue_index >= 0 &&
                    st.queue_index < static_cast<std::int32_t>(st.queue.size())) {
                    reload_path = st.queue[static_cast<std::size_t>(st.queue_index)];
                }
            }

            // DBus borrows raw Engine*; tear it down first to avoid
            // dangling-pointer dereference on the event-loop thread.
            if (st.dbus_) {
                st.dbus_->shutdown();
                st.dbus_.reset();
            }
            if (st.engine_) {
                st.engine_->stop();
                st.engine_.reset();
            }
            engine::EngineConfig ec{};
            ec.device_id = new_dev;
            if (auto e = engine::Engine::create(std::move(ec)); e) {
                st.engine_ = std::move(*e);
                wire_engine_events(st);
                st.push_log("switched to: " + new_dev);
                if (!reload_path.empty()) {
                    if (auto lr = st.engine_->load(reload_path); lr) {
                        st.engine_->play();
                        st.push_log("loaded: " + reload_path.string());
                    } else {
                        st.push_log("reload failed: " + lr.error().message);
                    }
                }
            } else {
                st.push_log("switch failed: " + e.error().message);
            }
            st.preferred_device = new_dev;
            st.cfg.device.preferred = new_dev;
            if (!st.config_path.empty()) {
                config::save_device_preferred(st.config_path, new_dev);
            }
            open_dbus_service(st);
        }

        // "Release DAC" button: stop engine and drop exclusive ALSA hold.
        if (ready && st.release_dac_requested) {
            st.release_dac_requested = false;
            if (st.dbus_) {
                st.dbus_->shutdown();
                st.dbus_.reset();
            }
            if (st.engine_) {
                st.engine_->stop();
                st.engine_.reset();
            }
            st.push_log("DAC released — exclusive hold dropped");
        }

        if (ready && !queued_done) {
            queued_done = maybe_play_queued_file(st);
        }

        if (ready && !queue_metadata_refreshed) {
            queue_metadata_refreshed = true;
            st.recompute_queue_duration();
        }

        if (ready) {
            if (win.take_media_play_pause() && st.engine_) {
                const auto s = st.last_engine_state.load(std::memory_order_relaxed);
                if (s == engine::State::Playing) (void)st.engine_->pause();
                else                             (void)st.engine_->play();
            }
            if (win.take_media_stop() && st.engine_)
                (void)st.engine_->stop();
            if (win.take_media_next() && st.engine_) {
                { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
                if (auto n = st.queue_next(); n) {
                    (void)st.engine_->load(*n);
                    (void)st.engine_->play();
                    if (st.dbus_) st.dbus_->notify_track_loaded();
                }
            }
            if (win.take_media_prev() && st.engine_) {
                { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
                if (auto p = st.queue_previous(); p) {
                    (void)st.engine_->load(*p);
                    (void)st.engine_->play();
                    if (st.dbus_) st.dbus_->notify_track_loaded();
                }
            }
            if (win.take_media_mute()) {
                const auto& hw = st.engine_
                    ? st.engine_->pipeline_snapshot().device.current_hw_string : std::string{};
                if (!hw.empty()) {
                    engine::toggle_hw_mute(hw);
                    if (st.dbus_) st.dbus_->notify_volume_changed();
                }
            }
            {
                const bool vol_up   = win.take_media_vol_up();
                const bool vol_down = win.take_media_vol_down();
                if (vol_up || vol_down) {
                    const auto& hw = st.engine_
                        ? st.engine_->pipeline_snapshot().device.current_hw_string
                        : std::string{};
                    if (!hw.empty()) {
                        int pct = engine::get_hw_volume_pct(hw);
                        if (pct >= 0) {
                            pct = std::clamp(pct + (vol_up ? 5 : -5), 0, 100);
                            engine::set_hw_volume_pct(hw, pct);
                            st.hw_volume_pct = pct;
                            st.hw_volume_last_poll = {};
                            if (st.dbus_) st.dbus_->notify_volume_changed();
                        }
                    }
                }
            }
        }

        if (ready && st.mini_mode != was_mini) {
            was_mini = st.mini_mode;
            if (st.mini_mode) {
                win.resize(480, 72);
            } else {
                win.resize(960, 600);
            }
        }

        win.begin_frame();

        // Update compositor title at ~1 Hz to reflect the playing track.
        {
            static std::string s_last_title;
            static std::chrono::steady_clock::time_point s_last_check{};
            const auto now_tp = std::chrono::steady_clock::now();
            if (now_tp - s_last_check > std::chrono::seconds(1)) {
                s_last_check = now_tp;
                std::string new_title = "transporter";
                if (st.engine_) {
                    const auto snap = st.engine_->pipeline_snapshot();
                    if (!snap.source.tags.title.empty()) {
                        new_title = snap.source.tags.title;
                        if (!snap.source.tags.artist.empty())
                            new_title = snap.source.tags.artist + " \xe2\x80\x94 " + new_title;
                        new_title += " \xc2\xb7 transporter";
                    }
                }
                if (new_title != s_last_title) {
                    s_last_title = new_title;
                    win.set_title(new_title);
                }
            }
        }

        // Full-window dock
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        constexpr ImGuiWindowFlags kFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::Begin("##transporter", nullptr, kFlags);

        if (!ready) {
            ImGui::TextDisabled("Initializing...");
        } else if (st.mini_mode) {
            draw_mini_view(st);
        } else {
            draw_tab_bar(st);
            handle_view_shortcuts(st);

            switch (st.current_view) {
            case ViewId::Main:     draw_main_view(st);     break;
            case ViewId::Library:  draw_library_view(st);  break;
            case ViewId::Pipeline: draw_pipeline_view(st); break;
            case ViewId::Queue:    draw_queue_view(st);    break;
            }
        }

        ImGui::End();

        win.end_frame(0.07f, 0.08f, 0.10f);
    }

    // Save window geometry for next launch
    {
        const char* home = std::getenv("HOME");
        if (home) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path dir = fs::path{home} / ".cache" / "transporter";
            fs::create_directories(dir, ec);
            if (!ec) {
                if (std::ofstream f{dir / "geometry"}; f) {
                    f << win.framebuffer_width() << ' '
                      << win.framebuffer_height() << '\n';
                }
            }
        }
    }

    // Save session state for next launch
    {
        const char* home = std::getenv("HOME");
        if (home) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path dir = fs::path{home} / ".cache" / "transporter";
            fs::create_directories(dir, ec);
            if (!ec) {
                if (std::ofstream f{dir / "session"}; f) {
                    f << "shuffle=" << (st.shuffle ? "1" : "0") << '\n';
                    int rm = 0;
                    switch (st.repeat_mode) {
                    case AppState::RepeatMode::None: rm = 0; break;
                    case AppState::RepeatMode::One:  rm = 1; break;
                    case AppState::RepeatMode::All:  rm = 2; break;
                    }
                    f << "repeat=" << rm << '\n';
                    // Persist the play queue so it can be resumed on next launch.
                    {
                        std::lock_guard lk(st.queue_mtx);
                        f << "queue_index=" << st.queue_index << '\n';
                        for (const auto& p : st.queue)
                            f << "queue=" << p.string() << '\n';
                    }
                }
            }
        }
    }

    // Detach the event callback before AppState members start tearing down.
    // The decoder thread may fire events during engine destruction; without
    // this the callback would access already-destroyed AppState members.
    if (st.engine_) {
        st.engine_->set_event_callback({});
    }

    backend_thread.join();
    return 0;
}

int run_headless(const AppArgs& args) {
    AppState st;
    st.config_path = args.config_path;
    st.cfg = load_config_or_defaults(args.config_path);

    if (auto r = engine::list_playback_devices(); r) {
        st.devices = std::move(*r);
    }
    const std::string device_id = pick_device(st.cfg, args.device_override, st.devices);
    int engine_create_exit = 0;
    if (device_id.empty()) {
        std::fprintf(stderr,
                     "transporter: no playback devices available (no DAC)\n");
        engine_create_exit = 2;
    } else {
        engine::EngineConfig ec{};
        ec.device_id = device_id;
        auto e = engine::Engine::create(std::move(ec));
        if (!e) {
            std::fprintf(stderr, "transporter: engine create failed [%s] %s\n",
                         std::string(engine::error_code_name(e.error().code)).c_str(),
                         e.error().message.c_str());
            engine_create_exit = 3;
        } else {
            st.engine_ = std::move(*e);
        }
    }

    // Bring DBus up regardless of engine create outcome, so even an
    // engine-less run still publishes a service that returns "Stopped" and
    // exits cleanly. This matches the spec: the DBus service should come up
    // independently of the engine.
    if (!args.file_to_play.empty()) {
        st.queue_set_single(args.file_to_play);
    }
    open_dbus_service(st);
    if (st.cfg.dbus.enabled && st.dbus_ == nullptr) {
        // Bus name conflict (likely a second instance) or session-bus open
        // failed. Headless mode treats this as fatal so scripts notice.
        std::fprintf(stderr,
                     "transporter: DBus service unavailable; another "
                     "instance may be running\n");
        return 8;
    }

    if (engine_create_exit != 0) {
        return engine_create_exit;
    }

    if (args.file_to_play.empty()) {
        std::fprintf(stderr, "transporter: --no-gui requires a file argument\n");
        return 4;
    }

    std::vector<std::filesystem::path> playlist;
    bool use_playlist = is_playlist_ext(args.file_to_play);
    if (use_playlist) {
        playlist = transporter::gui::load_playlist(args.file_to_play);
        if (playlist.empty()) {
            std::fprintf(stderr, "transporter: empty or unreadable playlist: %s\n",
                         args.file_to_play.c_str());
            return 1;
        }
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> done{false};
    std::atomic<int> playlist_pos{0};
    int exit_code = 0;
    st.engine_->set_event_callback([&](const engine::Event& ev) {
        switch (ev.kind) {
        case engine::Event::Kind::TrackLoaded: {
            std::printf("playing: %s\n", ev.file_path.c_str());
            const auto snap = st.engine_->pipeline_snapshot();
            if (!snap.source.tags.title.empty())
                std::printf("title:   %s\n", snap.source.tags.title.c_str());
            if (!snap.source.tags.artist.empty())
                std::printf("artist:  %s\n", snap.source.tags.artist.c_str());
            if (!snap.source.tags.album.empty())
                std::printf("album:   %s\n", snap.source.tags.album.c_str());
            std::printf("format:  %u Hz  %u ch  %s\n",
                        ev.format.sample_rate_hz, ev.format.channels,
                        snap.source.codec_name.c_str());
            if (st.dbus_ != nullptr) {
                st.dbus_->notify_track_loaded();
            }
            break;
        }
        case engine::Event::Kind::TrackEnded:
            std::printf("ended\n");
            if (use_playlist) {
                const int next = playlist_pos.fetch_add(1) + 1;
                if (next < static_cast<int>(playlist.size())) {
                    (void)st.engine_->load(playlist[static_cast<std::size_t>(next)]);
                    (void)st.engine_->play();
                } else {
                    done.store(true);
                    cv.notify_all();
                }
            } else {
                done.store(true);
                cv.notify_all();
            }
            break;
        case engine::Event::Kind::ErrorOccurred:
            std::fprintf(stderr, "error [%s] %s\n",
                         std::string(engine::error_code_name(ev.error.code)).c_str(),
                         ev.error.message.c_str());
            exit_code = 5;
            done.store(true);
            cv.notify_all();
            break;
        default: break;
        }
    });

    const std::filesystem::path first = use_playlist ? playlist[0] : std::filesystem::path(args.file_to_play);
    if (auto r = st.engine_->load(first); !r) {
        std::fprintf(stderr, "load failed: %s\n", r.error().message.c_str());
        return 6;
    }
    if (auto r = st.engine_->play(); !r) {
        std::fprintf(stderr, "play failed: %s\n", r.error().message.c_str());
        return 7;
    }

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&]{ return done.load(); });
    }
    return exit_code;
}

} // namespace transporter::gui
