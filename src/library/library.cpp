// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/library/library.hpp>

#include "db.hpp"
#include "queries.hpp"
#include "scanner.hpp"

#include <transporter/engine/error.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace transporter::library {

namespace {

namespace fs = std::filesystem;

std::string column_text(sqlite3_stmt* stmt, int idx) {
    const auto* p = sqlite3_column_text(stmt, idx);
    const int n   = sqlite3_column_bytes(stmt, idx);
    if (!p || n <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(p),
                       static_cast<std::size_t>(n));
}

void bind_text(sqlite3_stmt* s, int idx, std::string_view sv) {
    sqlite3_bind_text(s, idx, sv.data(),
                      static_cast<int>(sv.size()), SQLITE_TRANSIENT);
}

Track row_to_track(sqlite3_stmt* stmt) {
    // Column order matches select_track_by_id / select_tracks_in_album.
    Track t{};
    t.id              = sqlite3_column_int64(stmt, 0);
    t.path            = column_text(stmt, 1);
    t.title           = column_text(stmt, 2);
    t.artist          = column_text(stmt, 3);
    t.album           = column_text(stmt, 4);
    t.album_artist    = column_text(stmt, 5);
    t.track_no        = column_text(stmt, 6);
    t.date            = column_text(stmt, 7);
    t.codec           = column_text(stmt, 8);
    t.sample_rate_hz  = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 9));
    t.channels        = static_cast<std::uint16_t>(sqlite3_column_int(stmt, 10));
    t.bit_depth       = static_cast<std::uint16_t>(sqlite3_column_int(stmt, 11));
    t.total_frames    = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 12));
    t.duration        = std::chrono::milliseconds{sqlite3_column_int64(stmt, 13)};
    t.file_size_bytes = sqlite3_column_int64(stmt, 14);
    t.mtime_unix_ns   = sqlite3_column_int64(stmt, 15);
    return t;
}

// FTS5 query construction. Free-text becomes a prefix-match phrase: each
// space-separated token is a column-untargeted prefix term joined with
// implicit AND. Single-quote characters are doubled so they survive the
// MATCH expression. Empty input is signalled by an empty returned string.
std::string fts_match_string(std::string_view raw) {
    std::string out;
    std::string token;
    auto flush = [&] {
        if (token.empty()) {
            return;
        }
        if (!out.empty()) {
            out += ' ';
        }
        // Quote and append a prefix marker.
        out += '"';
        for (char c : token) {
            if (c == '"') {
                out += '"';
                out += '"';
            } else {
                out += c;
            }
        }
        out += "\"*";
        token.clear();
    };
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            token += c;
        }
    }
    flush();
    return out;
}

const char* sort_order_clause(SortBy s) {
    switch (s) {
    case SortBy::Artist:
        return " ORDER BY artist COLLATE NOCASE ASC, "
               "album COLLATE NOCASE ASC, "
               "CAST(track_no AS INTEGER) ASC, "
               "title COLLATE NOCASE ASC";
    case SortBy::Album:
        return " ORDER BY album COLLATE NOCASE ASC, "
               "CAST(track_no AS INTEGER) ASC, "
               "title COLLATE NOCASE ASC";
    case SortBy::Title:
        return " ORDER BY title COLLATE NOCASE ASC";
    case SortBy::DateAdded:
        return " ORDER BY added_at_unix_ns DESC";
    }
    return " ORDER BY artist COLLATE NOCASE ASC";
}

} // namespace

struct Library::Impl {
    Config cfg;
    std::unique_ptr<Db> read_db;
    std::unique_ptr<Scanner> scanner;
    mutable std::mutex read_mu;

    DeltaCallback delta_cb;
    mutable std::mutex cb_mu;
};

Library::Library() : impl_(std::make_unique<Impl>()) {}
Library::~Library() = default;

std::expected<std::unique_ptr<Library>, transporter::engine::Error>
Library::open(Config cfg) {
    if (cfg.db_path.empty()) {
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::InvalidArgument,
            "Library: db_path is empty"});
    }

    // Make the parent directory exist so a fresh ~/.local/share path works.
    {
        std::error_code ec;
        const auto parent = cfg.db_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
    }

    // First open: create + migrate. The unique_ptr is dropped before the
    // scanner spins up its own write connection.
    {
        auto write_db = Db::open(cfg.db_path, false);
        if (!write_db) {
            return std::unexpected(write_db.error());
        }
        if (auto m = apply_migrations(**write_db); !m) {
            return std::unexpected(m.error());
        }
    }

    auto read_db = Db::open(cfg.db_path, false);
    if (!read_db) {
        return std::unexpected(read_db.error());
    }

    auto lib = std::unique_ptr<Library>(new Library{});
    lib->impl_->cfg = cfg;
    lib->impl_->read_db = std::move(*read_db);

    // Forward scanner deltas to the user-supplied callback under a lock.
    DeltaCallback fwd = [w = lib.get()](const DeltaEvent& ev) {
        std::lock_guard<std::mutex> lk(w->impl_->cb_mu);
        if (w->impl_->delta_cb) {
            w->impl_->delta_cb(ev);
        }
    };

    lib->impl_->scanner = std::make_unique<Scanner>(
        cfg.db_path, cfg.roots, cfg.ignore_patterns, std::move(fwd));

    return lib;
}

void Library::set_delta_callback(DeltaCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->cb_mu);
    impl_->delta_cb = std::move(cb);
}

void Library::set_roots(std::vector<std::filesystem::path> roots) {
    if (impl_->scanner) {
        impl_->scanner->set_roots(std::move(roots));
    }
}

void Library::set_ignore_patterns(std::vector<std::string> patterns) {
    if (impl_->scanner) {
        impl_->scanner->set_ignore_patterns(std::move(patterns));
    }
}

void Library::rescan_async() {
    if (impl_->scanner) {
        impl_->scanner->request_scan();
    }
}

ScanProgress Library::progress() const {
    if (impl_->scanner) {
        return impl_->scanner->progress();
    }
    ScanProgress p{};
    p.state = ScanState::Idle;
    return p;
}

std::expected<std::vector<Track>, transporter::engine::Error>
Library::search(const SearchFilter& filter) const {
    std::lock_guard<std::mutex> lk(impl_->read_mu);

    std::string sql =
        "SELECT id, path, title, artist, album, album_artist, track_no, "
        "       date, codec, sample_rate_hz, channels, bit_depth, "
        "       total_frames, duration_ms, file_size, mtime_unix_ns, "
        "       added_at_unix_ns "
        "FROM tracks";

    const std::string fts = fts_match_string(filter.query);
    const bool use_fts = !fts.empty();

    std::string where;
    auto add_clause = [&](std::string c) {
        where += where.empty() ? " WHERE " : " AND ";
        where += std::move(c);
    };

    if (use_fts) {
        add_clause(
            "id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?)");
    }
    if (!filter.artist.empty()) {
        add_clause("(artist = ? OR album_artist = ?)");
    }
    if (!filter.album.empty()) {
        add_clause("album = ?");
    }
    sql += where;

    // ORDER BY does not reference added_at_unix_ns by name in row_to_track —
    // it's selected so DateAdded sort can use it.
    sql += sort_order_clause(filter.sort_by);
    if (filter.limit != 0) {
        sql += " LIMIT ?";
        if (filter.offset != 0) {
            sql += " OFFSET ?";
        }
    } else if (filter.offset != 0) {
        // OFFSET requires a LIMIT in SQLite; supply -1 (= no limit).
        sql += " LIMIT -1 OFFSET ?";
    }

    auto stmt_or = impl_->read_db->prepare(sql);
    if (!stmt_or) {
        return std::unexpected(stmt_or.error());
    }
    sqlite3_stmt* stmt = *stmt_or;

    int idx = 1;
    if (use_fts) {
        bind_text(stmt, idx++, fts);
    }
    if (!filter.artist.empty()) {
        bind_text(stmt, idx++, filter.artist);
        bind_text(stmt, idx++, filter.artist);
    }
    if (!filter.album.empty()) {
        bind_text(stmt, idx++, filter.album);
    }
    if (filter.limit != 0) {
        sqlite3_bind_int64(stmt, idx++, static_cast<std::int64_t>(filter.limit));
        if (filter.offset != 0) {
            sqlite3_bind_int64(stmt, idx++, static_cast<std::int64_t>(filter.offset));
        }
    } else if (filter.offset != 0) {
        sqlite3_bind_int64(stmt, idx++, static_cast<std::int64_t>(filter.offset));
    }

    std::vector<Track> out;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            out.push_back(row_to_track(stmt));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return std::unexpected(sqlite_error(*impl_->read_db, "search.step"));
        }
    }
    return out;
}

std::expected<std::vector<Album>, transporter::engine::Error>
Library::albums(const std::string& artist) const {
    std::lock_guard<std::mutex> lk(impl_->read_mu);

    auto stmt_or = impl_->read_db->prepare(
        artist.empty() ? queries::select_albums_all
                       : queries::select_albums_by_artist);
    if (!stmt_or) {
        return std::unexpected(stmt_or.error());
    }
    sqlite3_stmt* stmt = *stmt_or;

    if (!artist.empty()) {
        bind_text(stmt, 1, artist);
    }

    std::vector<Album> out;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            Album a{};
            a.id           = sqlite3_column_int64(stmt, 0);
            a.title        = column_text(stmt, 1);
            a.album_artist = column_text(stmt, 2);
            a.date         = column_text(stmt, 3);
            a.track_count  = sqlite3_column_int64(stmt, 4);
            out.push_back(std::move(a));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return std::unexpected(sqlite_error(*impl_->read_db, "albums.step"));
        }
    }
    return out;
}

std::expected<std::vector<Artist>, transporter::engine::Error>
Library::artists() const {
    std::lock_guard<std::mutex> lk(impl_->read_mu);

    auto stmt_or = impl_->read_db->prepare(queries::select_artists);
    if (!stmt_or) {
        return std::unexpected(stmt_or.error());
    }
    sqlite3_stmt* stmt = *stmt_or;

    std::vector<Artist> out;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            Artist a{};
            a.id          = sqlite3_column_int64(stmt, 0);
            a.name        = column_text(stmt, 1);
            a.album_count = sqlite3_column_int64(stmt, 2);
            a.track_count = sqlite3_column_int64(stmt, 3);
            out.push_back(std::move(a));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return std::unexpected(sqlite_error(*impl_->read_db, "artists.step"));
        }
    }
    return out;
}

std::expected<std::vector<Track>, transporter::engine::Error>
Library::tracks_in_album(std::int64_t album_id) const {
    std::lock_guard<std::mutex> lk(impl_->read_mu);

    auto stmt_or = impl_->read_db->prepare(queries::select_tracks_in_album);
    if (!stmt_or) {
        return std::unexpected(stmt_or.error());
    }
    sqlite3_stmt* stmt = *stmt_or;
    sqlite3_bind_int64(stmt, 1, album_id);

    std::vector<Track> out;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            out.push_back(row_to_track(stmt));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return std::unexpected(
                sqlite_error(*impl_->read_db, "tracks_in_album.step"));
        }
    }
    return out;
}

std::expected<Track, transporter::engine::Error>
Library::track_by_id(std::int64_t id) const {
    std::lock_guard<std::mutex> lk(impl_->read_mu);

    auto stmt_or = impl_->read_db->prepare(queries::select_track_by_id);
    if (!stmt_or) {
        return std::unexpected(stmt_or.error());
    }
    sqlite3_stmt* stmt = *stmt_or;
    sqlite3_bind_int64(stmt, 1, id);

    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_track(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::NotFound,
            "track not found"});
    }
    return std::unexpected(sqlite_error(*impl_->read_db, "track_by_id.step"));
}

std::expected<Track, transporter::engine::Error>
Library::track_by_path(const std::filesystem::path& path) const {
    std::lock_guard<std::mutex> lk(impl_->read_mu);

    auto stmt_or = impl_->read_db->prepare(queries::select_full_track_by_path);
    if (!stmt_or) {
        return std::unexpected(stmt_or.error());
    }
    sqlite3_stmt* stmt = *stmt_or;
    bind_text(stmt, 1, path.string());

    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_track(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::NotFound,
            "track not found"});
    }
    return std::unexpected(sqlite_error(*impl_->read_db, "track_by_path.step"));
}

} // namespace transporter::library
