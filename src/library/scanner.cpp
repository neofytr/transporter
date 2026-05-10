// SPDX-License-Identifier: GPL-3.0-or-later

#include "scanner.hpp"
#include "db.hpp"
#include "queries.hpp"

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/decoder_factory.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/library/library.hpp>

#include <fnmatch.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace transporter::library {

namespace {

namespace fs = std::filesystem;

// Extensions we consider for indexing. Magic-byte confirmation happens via
// open_decoder(); this is just a fast prefilter.
const std::array<std::string_view, 13> kAudioExtensions = {
    ".wav", ".wave", ".aif", ".aiff", ".aifc",
    ".flac", ".m4a", ".mp4", ".alac",
    ".mp3", ".ogg", ".oga", ".opus",
};

bool has_audio_ext(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    for (auto e : kAudioExtensions) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

std::int64_t now_unix_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

std::string codec_for_ext(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    if (ext == ".wav" || ext == ".wave") return "WAV";
    if (ext == ".aif" || ext == ".aiff" || ext == ".aifc") return "AIFF";
    if (ext == ".flac") return "FLAC";
    if (ext == ".m4a" || ext == ".mp4" || ext == ".alac") return "ALAC";
    if (ext == ".mp3") return "MP3";
    if (ext == ".ogg" || ext == ".oga") return "Vorbis";
    if (ext == ".opus") return "Opus";
    return "";
}

bool codec_is_lossy(std::string_view codec) {
    return codec == "MP3" || codec == "Vorbis" || codec == "Opus";
}

std::uint16_t bit_depth_for(transporter::engine::SampleFormat fmt,
                            std::string_view codec) {
    if (codec_is_lossy(codec)) {
        return 0;
    }
    using F = transporter::engine::SampleFormat;
    switch (fmt) {
    case F::S16_LE:    return 16;
    case F::S24_LE:    return 24;
    case F::S24_3LE:   return 24;
    case F::S32_LE:    return 32;
    case F::FLOAT_LE:  return 32;
    }
    return 0;
}

struct StatInfo {
    bool ok;
    std::int64_t size;
    std::int64_t mtime_ns;
};

StatInfo stat_file(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
        return {false, 0, 0};
    }
    StatInfo s;
    s.ok = true;
    s.size = static_cast<std::int64_t>(st.st_size);
    s.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
                 static_cast<std::int64_t>(st.st_mtim.tv_nsec);
    return s;
}

void bind_text(sqlite3_stmt* s, int idx, std::string_view sv) {
    sqlite3_bind_text(s, idx, sv.data(),
                      static_cast<int>(sv.size()), SQLITE_TRANSIENT);
}

} // namespace

Scanner::Scanner(fs::path db_path,
                 std::vector<fs::path> roots,
                 std::vector<std::string> ignore_patterns,
                 DeltaCallback cb)
    : db_path_(std::move(db_path)),
      roots_(std::move(roots)),
      ignore_patterns_(std::move(ignore_patterns)),
      cb_(std::move(cb)) {
    thread_ = std::thread([this] { thread_main(); });
}

Scanner::~Scanner() {
    {
        std::lock_guard<std::mutex> lk(wake_mu_);
        shutdown_.store(true, std::memory_order_release);
        cancel_requested_.store(true, std::memory_order_release);
        wake_requested_ = true;
    }
    wake_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Scanner::request_scan() {
    {
        std::lock_guard<std::mutex> lk(wake_mu_);
        wake_requested_ = true;
        cancel_requested_.store(false, std::memory_order_release);
    }
    wake_cv_.notify_all();
}

void Scanner::set_callback(DeltaCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    cb_ = std::move(cb);
}

ScanProgress Scanner::progress() const {
    std::lock_guard<std::mutex> lk(progress_mu_);
    ScanProgress p{};
    p.state = state_;
    p.files_seen = files_seen_;
    p.files_indexed = files_indexed_;
    p.files_deleted = files_deleted_;
    p.current_path = current_path_;
    p.started_at = started_at_;
    return p;
}

void Scanner::set_state(ScanState s) {
    std::lock_guard<std::mutex> lk(progress_mu_);
    state_ = s;
}

void Scanner::set_current_path(const fs::path& p) {
    std::lock_guard<std::mutex> lk(progress_mu_);
    current_path_ = p;
}

void Scanner::emit(DeltaEvent::Kind k, std::int64_t id, const fs::path& p) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    if (cb_) {
        DeltaEvent ev;
        ev.kind = k;
        ev.track_id = id;
        ev.path = p;
        cb_(ev);
    }
}

bool Scanner::path_ignored(const fs::path& p) const {
    const std::string s = p.string();
    for (const auto& glob : ignore_patterns_) {
        if (::fnmatch(glob.c_str(), s.c_str(), 0) == 0) {
            return true;
        }
    }
    return false;
}

void Scanner::thread_main() {
    while (true) {
        std::unique_lock<std::mutex> lk(wake_mu_);
        wake_cv_.wait(lk, [&] {
            return wake_requested_ || shutdown_.load(std::memory_order_acquire);
        });
        if (shutdown_.load(std::memory_order_acquire)) {
            return;
        }
        wake_requested_ = false;
        lk.unlock();

        cancel_requested_.store(false, std::memory_order_release);

        auto db_or = Db::open(db_path_, false);
        if (!db_or) {
            // Without the DB the scanner can't do anything useful. Leave
            // state Idle and wait for the next request.
            set_state(ScanState::Idle);
            continue;
        }
        Db& db = **db_or;
        // Migrations were applied on the read connection at Library::open;
        // re-running here is idempotent and cheap.
        (void)apply_migrations(db);

        run_one_scan(db);
    }
}

void Scanner::run_one_scan(Db& db) {
    {
        std::lock_guard<std::mutex> lk(progress_mu_);
        state_ = ScanState::Scanning;
        files_seen_ = 0;
        files_indexed_ = 0;
        files_deleted_ = 0;
        started_at_ = std::chrono::steady_clock::now();
        current_path_.clear();
    }

    emit(DeltaEvent::Kind::ScanStarted, 0, fs::path{});

    std::vector<fs::path> live_paths;
    live_paths.reserve(1024);

    for (const auto& root : roots_) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            break;
        }
        if (root.empty()) {
            continue;
        }
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
            continue;
        }
        walk_root(db, root, live_paths);
    }

    if (!cancel_requested_.load(std::memory_order_acquire)) {
        sweep_deletions(db, live_paths);
    }

    set_state(ScanState::Idle);
    emit(DeltaEvent::Kind::ScanFinished, 0, fs::path{});
}

void Scanner::walk_root(Db& db, const fs::path& root,
                        std::vector<fs::path>& live_paths) {
    set_state(ScanState::Scanning);

    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return;
    }
    fs::recursive_directory_iterator end;

    std::size_t batch_count = 0;
    bool in_txn = false;

    auto begin_txn = [&] {
        if (!in_txn) {
            (void)db.exec(queries::begin_immediate);
            in_txn = true;
        }
    };
    auto commit_txn = [&] {
        if (in_txn) {
            (void)db.exec(queries::commit);
            in_txn = false;
        }
    };

    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (cancel_requested_.load(std::memory_order_acquire)) {
            break;
        }

        const fs::path& p = it->path();

        if (path_ignored(p)) {
            // Don't descend into ignored directories.
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        if (!has_audio_ext(p)) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(progress_mu_);
            ++files_seen_;
            current_path_ = p;
        }
        live_paths.push_back(p);

        const StatInfo si = stat_file(p);
        if (!si.ok) {
            continue;
        }

        // Existing-row lookup: compare path + mtime + size to decide if a
        // re-decode is needed. Early-out keeps incremental scans cheap.
        std::int64_t existing_id = 0;
        std::int64_t existing_mtime = -1;
        std::int64_t existing_size = -1;
        {
            auto stmt = db.prepare(queries::select_track_by_path);
            if (!stmt) {
                continue;
            }
            const std::string ps = p.string();
            bind_text(*stmt, 1, ps);
            if (sqlite3_step(*stmt) == SQLITE_ROW) {
                existing_id    = sqlite3_column_int64(*stmt, 0);
                existing_mtime = sqlite3_column_int64(*stmt, 1);
                existing_size  = sqlite3_column_int64(*stmt, 2);
            }
        }

        if (existing_id != 0 &&
            existing_mtime == si.mtime_ns &&
            existing_size  == si.size) {
            continue; // Up to date.
        }

        // Need to (re)decode.
        set_state(ScanState::Indexing);
        auto opened = transporter::engine::open_decoder(p);
        if (!opened) {
            // Unparseable file. Skip silently; the scanner shouldn't bail
            // the whole walk over a bad MP3.
            continue;
        }
        auto& dec = **opened;
        const auto fmt = dec.format();
        const auto& tags = dec.tags();
        const std::string codec = codec_for_ext(p);
        const std::uint16_t bd = bit_depth_for(fmt.sample_format, codec);
        const std::uint64_t total_frames = dec.total_frames();
        const std::int64_t duration_ms =
            fmt.sample_rate_hz == 0
                ? 0
                : static_cast<std::int64_t>(
                      (total_frames * 1000ULL) / fmt.sample_rate_hz);

        // Canonical album artist: explicit tag wins; otherwise strip feat./collab
        // suffixes so collaborative tracks group under the primary artist's album.
        auto split_primary = [](const std::string& artist) -> std::string {
            static constexpr const char* kSep[] = {
                " featuring ", " feat. ", " feat ", " ft. ", " ft ",
                " vs. ", " vs ", " & ", " x ",
            };
            std::string lc;
            lc.reserve(artist.size());
            for (char c : artist) {
                lc.push_back(c >= 'A' && c <= 'Z'
                                 ? static_cast<char>(c + ('a' - 'A')) : c);
            }
            for (const char* sep : kSep) {
                if (auto pos = lc.find(sep); pos != std::string::npos) {
                    return artist.substr(0, pos);
                }
            }
            return artist;
        };
        const std::string album_artist =
            !tags.album_artist.empty() ? tags.album_artist
                                       : split_primary(tags.artist);

        // Upsert track and remember album/artist linkage.
        begin_txn();

        std::int64_t track_id = 0;
        {
            auto stmt = db.prepare(queries::upsert_track);
            if (!stmt) {
                continue;
            }
            const std::string ps = p.string();
            bind_text(*stmt, 1,  ps);
            bind_text(*stmt, 2,  tags.title);
            bind_text(*stmt, 3,  tags.artist);
            bind_text(*stmt, 4,  tags.album);
            bind_text(*stmt, 5,  album_artist);
            bind_text(*stmt, 6,  tags.track_no);
            bind_text(*stmt, 7,  tags.date);
            bind_text(*stmt, 8,  codec);
            sqlite3_bind_int (*stmt, 9,  static_cast<int>(fmt.sample_rate_hz));
            sqlite3_bind_int (*stmt, 10, static_cast<int>(fmt.channels));
            sqlite3_bind_int (*stmt, 11, static_cast<int>(bd));
            sqlite3_bind_int64(*stmt, 12, static_cast<std::int64_t>(total_frames));
            sqlite3_bind_int64(*stmt, 13, duration_ms);
            sqlite3_bind_int64(*stmt, 14, si.size);
            sqlite3_bind_int64(*stmt, 15, si.mtime_ns);
            sqlite3_bind_int64(*stmt, 16, now_unix_ns());

            if (sqlite3_step(*stmt) == SQLITE_ROW) {
                track_id = sqlite3_column_int64(*stmt, 0);
            }
            sqlite3_reset(*stmt);
        }
        if (track_id == 0) {
            continue;
        }

        // Upsert artist rows: the full track artist string AND any individual
        // artists extracted by splitting on collaborative separators.
        auto upsert_artist_name = [&](const std::string& name) {
            if (name.empty()) return;
            auto stmt = db.prepare(queries::upsert_artist);
            if (stmt) {
                bind_text(*stmt, 1, name);
                (void)sqlite3_step(*stmt);
                sqlite3_reset(*stmt);
            }
        };
        // Insert the canonical album artist. If the track artist differs
        // (e.g. "Artist A feat. Artist B"), also insert the album_artist (primary)
        // but NOT the full collaborative string — that would create a phantom artist.
        // Optionally extract the featured artist and insert them too.
        upsert_artist_name(album_artist);
        if (album_artist != tags.artist) {
            // Try to extract the featured artist(s) from the full track artist.
            const std::string& full = tags.artist;
            const std::string& prim_lc = [&]() -> const std::string& {
                // album_artist is already primary; build its lowercase version inline
                static thread_local std::string lc;
                lc.clear();
                lc.reserve(album_artist.size());
                for (char c : album_artist) {
                    lc.push_back(c >= 'A' && c <= 'Z'
                                     ? static_cast<char>(c + ('a' - 'A')) : c);
                }
                return lc;
            }();
            (void)prim_lc;
            // Everything after the separator is the featured part.
            static constexpr const char* kSep[] = {
                " featuring ", " feat. ", " feat ", " ft. ", " ft ",
                " vs. ", " vs ", " & ", " x ",
            };
            std::string full_lc;
            full_lc.reserve(full.size());
            for (char c : full) {
                full_lc.push_back(c >= 'A' && c <= 'Z'
                                      ? static_cast<char>(c + ('a' - 'A')) : c);
            }
            for (const char* sep : kSep) {
                auto pos = full_lc.find(sep);
                if (pos != std::string::npos) {
                    const std::size_t after = pos + std::strlen(sep);
                    if (after < full.size()) {
                        upsert_artist_name(full.substr(after));
                    }
                    break;
                }
            }
        }

        // Album row + linkage. Only when we actually have an album name.
        if (!tags.album.empty()) {
            std::int64_t album_id = 0;
            {
                auto stmt = db.prepare(queries::upsert_album);
                if (!stmt) {
                    continue;
                }
                bind_text(*stmt, 1, tags.album);
                bind_text(*stmt, 2, album_artist); // use canonical album artist
                bind_text(*stmt, 3, tags.date);
                if (sqlite3_step(*stmt) == SQLITE_ROW) {
                    album_id = sqlite3_column_int64(*stmt, 0);
                }
                sqlite3_reset(*stmt);
            }
            if (album_id != 0) {
                auto stmt = db.prepare(queries::upsert_track_album);
                if (stmt) {
                    sqlite3_bind_int64(*stmt, 1, track_id);
                    sqlite3_bind_int64(*stmt, 2, album_id);
                    (void)sqlite3_step(*stmt);
                    sqlite3_reset(*stmt);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(progress_mu_);
            ++files_indexed_;
        }
        emit(existing_id == 0 ? DeltaEvent::Kind::Added
                              : DeltaEvent::Kind::Updated,
             track_id, p);

        ++batch_count;
        if (batch_count >= 64) {
            commit_txn();
            batch_count = 0;
        }
    }

    commit_txn();
}

void Scanner::sweep_deletions(Db& db,
                              const std::vector<fs::path>& live_paths) {
    set_state(ScanState::Cleaning);

    std::unordered_set<std::string> live;
    live.reserve(live_paths.size() * 2);
    for (const auto& p : live_paths) {
        live.insert(p.string());
    }

    // First pass: collect (id, path) pairs whose file no longer exists.
    struct Stale {
        std::int64_t id;
        std::string path;
    };
    std::vector<Stale> stale;
    {
        auto stmt = db.prepare(queries::select_all_paths);
        if (!stmt) {
            return;
        }
        while (sqlite3_step(*stmt) == SQLITE_ROW) {
            const std::int64_t id = sqlite3_column_int64(*stmt, 0);
            const auto* p = sqlite3_column_text(*stmt, 1);
            const int n = sqlite3_column_bytes(*stmt, 1);
            if (!p || n <= 0) {
                continue;
            }
            std::string path(reinterpret_cast<const char*>(p),
                             static_cast<std::size_t>(n));
            // A path is stale if either we walked the corresponding root and
            // didn't see it, OR the file is simply absent on disk.
            if (live.find(path) == live.end()) {
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) {
                    stale.push_back({id, std::move(path)});
                }
            }
        }
    }

    if (stale.empty()) {
        return;
    }

    (void)db.exec(queries::begin_immediate);
    for (const auto& s : stale) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            break;
        }
        auto stmt = db.prepare(queries::delete_track_by_id);
        if (!stmt) {
            break;
        }
        sqlite3_bind_int64(*stmt, 1, s.id);
        if (sqlite3_step(*stmt) == SQLITE_DONE) {
            std::lock_guard<std::mutex> lk(progress_mu_);
            ++files_deleted_;
        }
        emit(DeltaEvent::Kind::Removed, s.id, fs::path{s.path});
    }
    (void)db.exec(queries::commit);
}

} // namespace transporter::library
