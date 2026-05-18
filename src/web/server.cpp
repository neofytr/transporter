// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/web/server.hpp>

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>
#include <transporter/library/library.hpp>
#include <transporter/queue/queue.hpp>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

namespace transporter::web {

namespace {

namespace eng = transporter::engine;
namespace lib = transporter::library;
using json    = nlohmann::json;

constexpr char kJsonMime[] = "application/json";

const char* state_name(eng::State s) noexcept {
    switch (s) {
    case eng::State::Idle:
        return "Idle";
    case eng::State::Loading:
        return "Loading";
    case eng::State::Playing:
        return "Playing";
    case eng::State::Paused:
        return "Paused";
    case eng::State::Stopped:
        return "Stopped";
    case eng::State::Error:
        return "Error";
    case eng::State::Disconnected:
        return "Disconnected";
    }
    return "Idle";
}

std::string format_string(std::uint32_t rate, eng::SampleFormat fmt,
                          std::uint16_t channels) {
    return std::to_string(rate) + "/" +
           std::string(eng::sample_format_name(fmt)) + "/" +
           std::to_string(channels);
}

void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), kJsonMime);
}

void ok(httplib::Response& res) {
    send_json(res, 200, json{{"ok", true}});
}

void bad_request(httplib::Response& res) {
    send_json(res, 400, json{{"error", "bad request"}});
}

void unauthorized(httplib::Response& res) {
    send_json(res, 401, json{{"error", "unauthorized"}});
}

void not_found(httplib::Response& res) {
    send_json(res, 404, json{{"error", "not found"}});
}

// Returns nullopt and writes a 400 response when the body is not valid JSON.
std::optional<json> parse_body(const httplib::Request& req,
                               httplib::Response& res) {
    json j = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        bad_request(res);
        return std::nullopt;
    }
    return j;
}

const char* mime_for(const std::filesystem::path& p) noexcept {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    if (ext == ".jpg" || ext == ".jpeg") {
        return "image/jpeg";
    }
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".webp") {
        return "image/webp";
    }
    if (ext == ".gif") {
        return "image/gif";
    }
    if (ext == ".html") {
        return "text/html; charset=utf-8";
    }
    if (ext == ".js" || ext == ".mjs") {
        return "text/javascript";
    }
    if (ext == ".css") {
        return "text/css";
    }
    if (ext == ".json") {
        return kJsonMime;
    }
    if (ext == ".svg") {
        return "image/svg+xml";
    }
    if (ext == ".woff2") {
        return "font/woff2";
    }
    if (ext == ".ico") {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

bool read_file(const std::filesystem::path& p, std::string& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) {
        return false;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
}

json track_json(const lib::Track& t) {
    return json{
        {"id", t.id},
        {"path", t.path.string()},
        {"title", t.title},
        {"artist", t.artist},
        {"album", t.album},
        {"track_number", t.track_no},
        {"duration_frames", t.total_frames},
        {"format", std::to_string(t.sample_rate_hz) + "/" +
                       std::to_string(t.bit_depth) + "/" +
                       std::to_string(t.channels)},
    };
}

}  // namespace

struct WebServer::Impl {
    eng::Engine&        engine;
    transporter::queue::Queue& queue;
    lib::Library*       library;
    WebConfig           cfg;

    httplib::Server srv;
    std::thread     listen_thread;
    std::thread     push_thread;
    std::atomic<bool> running{false};

    // Live WebSocket connections for the telemetry broadcast. httplib runs
    // each WS handler on its own thread; the handler registers its socket
    // here and blocks until close, while push_thread fans snapshots out.
    std::mutex                            ws_mtx;
    std::unordered_set<httplib::ws::WebSocket*> ws_clients;

    Impl(eng::Engine& e, transporter::queue::Queue& q, lib::Library* l,
         WebConfig c)
        : engine(e), queue(q), library(l), cfg(std::move(c)) {}

    bool auth_ok(const httplib::Request& req) const {
        if (cfg.token.empty()) {
            return true;
        }
        auto it = req.headers.find("Authorization");
        if (it == req.headers.end()) {
            return false;
        }
        const std::string_view want_prefix = "Bearer ";
        const std::string&      got         = it->second;
        if (got.size() <= want_prefix.size() ||
            std::string_view(got).substr(0, want_prefix.size()) !=
                want_prefix) {
            return false;
        }
        return std::string_view(got).substr(want_prefix.size()) == cfg.token;
    }

    json build_state() {
        const auto snap  = engine.pipeline_snapshot();
        const auto state = engine.state();

        std::uint64_t position = 0;
        if (snap.output.frames_written >=
            snap.output.frames_written_at_track_start) {
            position = snap.output.frames_written -
                       snap.output.frames_written_at_track_start;
        }

        json current = nullptr;
        if (!snap.source.file_path.empty()) {
            current = json{
                {"path", snap.source.file_path},
                {"title", snap.source.tags.title},
                {"artist", snap.source.tags.artist},
                {"album", snap.source.tags.album},
                {"duration_frames", snap.source.total_frames},
                {"format",
                 format_string(snap.source.sample_rate_hz,
                               snap.decoder.output_format.sample_format,
                               snap.source.channels)},
            };
        }

        return json{
            {"state", state_name(state)},
            {"current_track", current},
            {"position_frames", position},
            {"queue_index", queue.current_index()},
            {"queue_size", queue.size()},
        };
    }

    json build_snapshot() {
        const auto s = engine.pipeline_snapshot();

        const char* rt_mode =
            s.realtime.status.mode == eng::rt::Mode::Fifo ? "FIFO" : "OTHER";

        const char* verdict = "No";
        if (s.bit_perfect.level == eng::BitPerfectVerdict::Level::Yes) {
            verdict = "Yes";
        } else if (s.bit_perfect.level ==
                   eng::BitPerfectVerdict::Level::Qualified) {
            verdict = "Qualified";
        }

        return json{
            {"engine_state", state_name(s.engine_state)},
            {"source",
             {{"file_path", s.source.file_path},
              {"codec_name", s.source.codec_name},
              {"sample_rate_hz", s.source.sample_rate_hz},
              {"channels", s.source.channels},
              {"bit_depth_file", s.source.bit_depth_file},
              {"total_frames", s.source.total_frames},
              {"bitrate_kbps", s.source.bitrate_kbps}}},
            {"decoder",
             {{"thread_state", s.decoder.thread_state},
              {"frames_produced", s.decoder.frames_produced}}},
            {"format_match",
             {{"matched_ok", s.format_match.matched_ok},
              {"declared",
               format_string(s.format_match.declared.sample_rate_hz,
                             s.format_match.declared.sample_format,
                             s.format_match.declared.channels)},
              {"matched",
               format_string(s.format_match.matched.sample_rate_hz,
                             s.format_match.matched.sample_format,
                             s.format_match.matched.channels)},
              {"rejection_reason", s.format_match.rejection_reason}}},
            {"ring",
             {{"capacity_bytes", s.ring.capacity_bytes},
              {"fill_bytes", s.ring.fill_bytes},
              {"fill_frames", s.ring.fill_frames},
              {"fill_us", s.ring.fill_us.count()}}},
            {"output",
             {{"period_size_frames", s.output.period_size_frames},
              {"periods", s.output.periods},
              {"xrun_count", s.output.xrun_count},
              {"frames_written", s.output.frames_written},
              {"gapless_pending", s.output.gapless_pending}}},
            {"device",
             {{"current_hw_string", s.device.current_hw_string},
              {"is_connected", s.device.is_connected}}},
            {"realtime",
             {{"mode", rt_mode},
              {"fallback_reason", s.realtime.status.fallback_reason}}},
            {"bit_perfect",
             {{"level", verdict},
              {"qualifications", s.bit_perfect.qualifications}}},
        };
    }

    void serve_static(const httplib::Request& req, httplib::Response& res) {
        const std::filesystem::path root = cfg.static_dir;
        std::string rel = req.path;
        if (rel.empty() || rel == "/") {
            rel = "/index.html";
        }
        // Strip the leading slash and refuse path traversal.
        std::filesystem::path candidate =
            root / std::filesystem::path(rel).relative_path();
        candidate = candidate.lexically_normal();

        std::error_code ec;
        const auto root_abs = std::filesystem::weakly_canonical(root, ec);
        const auto cand_abs = std::filesystem::weakly_canonical(candidate, ec);

        std::string body;
        const bool inside =
            !root_abs.empty() &&
            cand_abs.string().rfind(root_abs.string(), 0) == 0;

        if (inside && read_file(candidate, body)) {
            res.set_content(body, mime_for(candidate));
            return;
        }
        // SPA fallback: serve index.html for unknown non-API paths.
        if (read_file(root / "index.html", body)) {
            res.set_content(body, "text/html; charset=utf-8");
            return;
        }
        not_found(res);
    }

    void serve_art(std::int64_t track_id, httplib::Response& res) {
        if (library == nullptr) {
            not_found(res);
            return;
        }
        auto tr = library->track_by_id(track_id);
        if (!tr) {
            not_found(res);
            return;
        }
        const std::filesystem::path dir = tr->path.parent_path();
        static constexpr const char* kNames[] = {
            "cover.jpg", "cover.png", "folder.jpg", "folder.png"};
        for (const char* n : kNames) {
            const std::filesystem::path art = dir / n;
            std::string body;
            if (read_file(art, body)) {
                res.set_content(body, mime_for(art));
                return;
            }
        }
        not_found(res);
    }

    void register_routes() {
        // Auth gate. Skips '/' static root and '/api/art/*' per spec; the
        // static SPA shell and cover art must load before the token is known.
        srv.set_pre_routing_handler(
            [this](const httplib::Request& req, httplib::Response& res) {
                const std::string& p = req.path;
                const bool exempt =
                    p == "/" || p.rfind("/api/art/", 0) == 0 ||
                    p.rfind("/api/", 0) != 0;  // static assets are exempt
                if (exempt) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                if (!auth_ok(req)) {
                    unauthorized(res);
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            });

        srv.Get("/api/state",
                [this](const httplib::Request&, httplib::Response& res) {
                    send_json(res, 200, build_state());
                });

        srv.Post("/api/play",
                 [this](const httplib::Request&, httplib::Response& res) {
                     (void)engine.play();
                     ok(res);
                 });
        srv.Post("/api/pause",
                 [this](const httplib::Request&, httplib::Response& res) {
                     (void)engine.pause();
                     ok(res);
                 });
        srv.Post("/api/stop",
                 [this](const httplib::Request&, httplib::Response& res) {
                     (void)engine.stop();
                     ok(res);
                 });

        srv.Post("/api/seek", [this](const httplib::Request& req,
                                     httplib::Response& res) {
            auto b = parse_body(req, res);
            if (!b) {
                return;
            }
            if (!b->contains("frame") || !(*b)["frame"].is_number()) {
                bad_request(res);
                return;
            }
            (void)engine.seek((*b)["frame"].get<std::uint64_t>());
            ok(res);
        });

        srv.Post("/api/load", [this](const httplib::Request& req,
                                     httplib::Response& res) {
            auto b = parse_body(req, res);
            if (!b) {
                return;
            }
            if (!b->contains("path") || !(*b)["path"].is_string()) {
                bad_request(res);
                return;
            }
            const std::filesystem::path path =
                (*b)["path"].get<std::string>();
            const auto tracks = queue.tracks();
            auto it           = std::find(tracks.begin(), tracks.end(), path);
            if (it != tracks.end()) {
                queue.jump(static_cast<int>(it - tracks.begin()));
            } else {
                (void)engine.load(path);
            }
            ok(res);
        });

        srv.Get("/api/queue",
                [this](const httplib::Request&, httplib::Response& res) {
                    const auto tracks = queue.tracks();
                    json arr          = json::array();
                    for (std::size_t i = 0; i < tracks.size(); ++i) {
                        arr.push_back({{"index", i},
                                       {"path", tracks[i].string()}});
                    }
                    send_json(res, 200,
                              json{{"tracks", arr},
                                   {"current_index", queue.current_index()}});
                });

        srv.Post("/api/queue/append", [this](const httplib::Request& req,
                                              httplib::Response& res) {
            auto b = parse_body(req, res);
            if (!b) {
                return;
            }
            if (!b->contains("path") || !(*b)["path"].is_string()) {
                bad_request(res);
                return;
            }
            queue.append((*b)["path"].get<std::string>());
            ok(res);
        });

        srv.Post("/api/queue/remove", [this](const httplib::Request& req,
                                              httplib::Response& res) {
            auto b = parse_body(req, res);
            if (!b) {
                return;
            }
            if (!b->contains("index") || !(*b)["index"].is_number_integer()) {
                bad_request(res);
                return;
            }
            queue.remove((*b)["index"].get<int>());
            ok(res);
        });

        srv.Post("/api/queue/reorder", [this](const httplib::Request& req,
                                               httplib::Response& res) {
            auto b = parse_body(req, res);
            if (!b) {
                return;
            }
            if (!b->contains("from") || !b->contains("to") ||
                !(*b)["from"].is_number_integer() ||
                !(*b)["to"].is_number_integer()) {
                bad_request(res);
                return;
            }
            queue.move((*b)["from"].get<int>(), (*b)["to"].get<int>());
            ok(res);
        });

        srv.Post("/api/queue/clear",
                 [this](const httplib::Request&, httplib::Response& res) {
                     queue.clear();
                     ok(res);
                 });

        srv.Post("/api/queue/jump", [this](const httplib::Request& req,
                                            httplib::Response& res) {
            auto b = parse_body(req, res);
            if (!b) {
                return;
            }
            if (!b->contains("index") || !(*b)["index"].is_number_integer()) {
                bad_request(res);
                return;
            }
            queue.jump((*b)["index"].get<int>());
            ok(res);
        });

        srv.Get("/api/library/albums", [this](const httplib::Request&,
                                               httplib::Response& res) {
            if (library == nullptr) {
                send_json(res, 200, json::array());
                return;
            }
            auto albums = library->albums("");
            json arr    = json::array();
            if (albums) {
                for (const auto& a : *albums) {
                    arr.push_back({{"id", a.id},
                                   {"title", a.title},
                                   {"artist", a.album_artist},
                                   {"year", a.date},
                                   {"track_count", a.track_count}});
                }
            }
            send_json(res, 200, arr);
        });

        srv.Get("/api/library/tracks", [this](const httplib::Request& req,
                                               httplib::Response& res) {
            if (library == nullptr) {
                send_json(res, 200, json::array());
                return;
            }
            if (!req.has_param("album_id")) {
                bad_request(res);
                return;
            }
            std::int64_t album_id = 0;
            try {
                album_id = std::stoll(req.get_param_value("album_id"));
            } catch (...) {
                bad_request(res);
                return;
            }
            auto tracks = library->tracks_in_album(album_id);
            json arr    = json::array();
            if (tracks) {
                for (const auto& t : *tracks) {
                    arr.push_back(track_json(t));
                }
            }
            send_json(res, 200, arr);
        });

        srv.Get("/api/library/search", [this](const httplib::Request& req,
                                                httplib::Response& res) {
            if (library == nullptr) {
                send_json(res, 200, json::array());
                return;
            }
            lib::SearchFilter f;
            f.query = req.has_param("q") ? req.get_param_value("q") : "";
            auto tracks = library->search(f);
            json arr    = json::array();
            if (tracks) {
                for (const auto& t : *tracks) {
                    arr.push_back(track_json(t));
                }
            }
            send_json(res, 200, arr);
        });

        srv.Get("/api/devices", [this](const httplib::Request&,
                                         httplib::Response& res) {
            auto devs_r = eng::list_playback_devices();
            json arr = json::array();
            if (!devs_r) {
                send_json(res, 200, arr);
                return;
            }
            std::string active_hw;
            {
                auto snap = engine.pipeline_snapshot();
                active_hw = snap.device.current_hw_string;
            }
            for (const auto& d : *devs_r) {
                json formats = json::array();
                for (auto f : d.caps.formats) {
                    formats.push_back(std::string(eng::sample_format_name(f)));
                }
                json rates = json::array();
                for (auto r : d.caps.sample_rates) {
                    rates.push_back(r);
                }
                std::string display_name = d.fingerprint.alsa_card_longname.empty()
                    ? d.alsa_hw_string : d.fingerprint.alsa_card_longname;
                arr.push_back({
                    {"id",             d.id},
                    {"alsa_hw_string", d.alsa_hw_string},
                    {"display_name",   display_name},
                    {"is_usb",         d.fingerprint.is_usb},
                    {"active",         d.alsa_hw_string == active_hw},
                    {"caps_probe_failed", d.caps.caps_probe_failed},
                    {"probe_failure_reason", d.caps.probe_failure_reason},
                    {"formats",        formats},
                    {"sample_rates",   rates},
                });
            }
            send_json(res, 200, arr);
        });

        srv.Get(R"(/api/art/(\d+))", [this](const httplib::Request& req,
                                             httplib::Response& res) {
            std::int64_t id = 0;
            try {
                id = std::stoll(req.matches[1]);
            } catch (...) {
                not_found(res);
                return;
            }
            serve_art(id, res);
        });

        // 10 Hz pipeline telemetry. The handler registers the socket and
        // blocks reading until the client closes; push_thread does the writes.
        srv.WebSocket(
            "/api/snapshot",
            [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
                {
                    std::lock_guard lk(ws_mtx);
                    ws_clients.insert(&ws);
                }
                std::string msg;
                while (running.load(std::memory_order_relaxed) &&
                       ws.is_open()) {
                    if (ws.read(msg) == httplib::ws::ReadResult::Fail) {
                        break;
                    }
                }
                std::lock_guard lk(ws_mtx);
                ws_clients.erase(&ws);
            });

        // Static assets + SPA fallback for everything not under /api/.
        srv.Get(".*", [this](const httplib::Request& req,
                             httplib::Response& res) {
            if (req.path.rfind("/api/", 0) == 0) {
                not_found(res);
                return;
            }
            serve_static(req, res);
        });
    }

    void push_loop() {
        using namespace std::chrono_literals;
        while (running.load(std::memory_order_relaxed)) {
            std::string payload;
            {
                payload = build_snapshot().dump();
            }
            {
                std::lock_guard lk(ws_mtx);
                for (auto* ws : ws_clients) {
                    if (ws->is_open()) {
                        ws->send(payload);
                    }
                }
            }
            std::this_thread::sleep_for(100ms);
        }
    }
};

WebServer::WebServer(eng::Engine& engine, transporter::queue::Queue& queue,
                     lib::Library* library, WebConfig cfg)
    : impl_(std::make_unique<Impl>(engine, queue, library, std::move(cfg))) {}

WebServer::~WebServer() {
    stop();
}

void WebServer::start() {
    if (impl_->running.exchange(true)) {
        return;
    }
    impl_->register_routes();
    impl_->listen_thread = std::thread([this] {
        impl_->srv.listen(impl_->cfg.host, impl_->cfg.port);
    });
    impl_->push_thread = std::thread([this] { impl_->push_loop(); });
}

void WebServer::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->srv.stop();
    if (impl_->push_thread.joinable()) {
        impl_->push_thread.join();
    }
    if (impl_->listen_thread.joinable()) {
        impl_->listen_thread.join();
    }
}

}  // namespace transporter::web
