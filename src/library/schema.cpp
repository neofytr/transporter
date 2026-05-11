// SPDX-License-Identifier: GPL-3.0-or-later

#include "db.hpp"

#include <transporter/engine/error.hpp>

#include <sqlite3.h>

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace transporter::library {

namespace {

// Migration 1: full schema bring-up.
//
// Tables:
//   tracks       — one row per file on disk
//   artists      — distinct artist + album_artist names (UNIQUE on name)
//   albums       — (title, album_artist) tuples
//   track_album  — many tracks → at most one album (unique on track_id)
//   tracks_fts   — FTS5 mirror of (title, artist, album, album_artist)
//   migrations   — version history
//
// Indexes target the common access patterns: by artist, album, album_artist,
// and path lookups during incremental scans.
constexpr std::string_view migration_001 = R"(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS migrations (
    version          INTEGER PRIMARY KEY,
    applied_at_unix_ns INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS tracks (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    path             TEXT    NOT NULL UNIQUE,
    title            TEXT    NOT NULL DEFAULT '',
    artist           TEXT    NOT NULL DEFAULT '',
    album            TEXT    NOT NULL DEFAULT '',
    album_artist     TEXT    NOT NULL DEFAULT '',
    track_no         TEXT    NOT NULL DEFAULT '',
    date             TEXT    NOT NULL DEFAULT '',
    codec            TEXT    NOT NULL DEFAULT '',
    sample_rate_hz   INTEGER NOT NULL DEFAULT 0,
    channels         INTEGER NOT NULL DEFAULT 0,
    bit_depth        INTEGER NOT NULL DEFAULT 0,
    total_frames     INTEGER NOT NULL DEFAULT 0,
    duration_ms      INTEGER NOT NULL DEFAULT 0,
    file_size        INTEGER NOT NULL DEFAULT 0,
    mtime_unix_ns    INTEGER NOT NULL DEFAULT 0,
    added_at_unix_ns INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_tracks_artist        ON tracks(artist COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_tracks_album         ON tracks(album COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_tracks_album_artist  ON tracks(album_artist COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_tracks_path          ON tracks(path);
CREATE INDEX IF NOT EXISTS idx_tracks_added_at      ON tracks(added_at_unix_ns);

CREATE TABLE IF NOT EXISTS artists (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT    NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS albums (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    title        TEXT NOT NULL DEFAULT '',
    album_artist TEXT NOT NULL DEFAULT '',
    date         TEXT NOT NULL DEFAULT '',
    UNIQUE(title, album_artist)
);

CREATE TABLE IF NOT EXISTS track_album (
    track_id INTEGER PRIMARY KEY,
    album_id INTEGER NOT NULL,
    FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE CASCADE,
    FOREIGN KEY(album_id) REFERENCES albums(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_track_album_album ON track_album(album_id);

CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(
    title, artist, album, album_artist,
    content='tracks',
    content_rowid='id',
    tokenize='unicode61 remove_diacritics 2'
);

CREATE TRIGGER IF NOT EXISTS trg_tracks_ai AFTER INSERT ON tracks BEGIN
    INSERT INTO tracks_fts (rowid, title, artist, album, album_artist)
        VALUES (new.id, new.title, new.artist, new.album, new.album_artist);
END;

CREATE TRIGGER IF NOT EXISTS trg_tracks_ad AFTER DELETE ON tracks BEGIN
    INSERT INTO tracks_fts (tracks_fts, rowid, title, artist, album, album_artist)
        VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist);
END;

CREATE TRIGGER IF NOT EXISTS trg_tracks_au AFTER UPDATE ON tracks BEGIN
    INSERT INTO tracks_fts (tracks_fts, rowid, title, artist, album, album_artist)
        VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist);
    INSERT INTO tracks_fts (rowid, title, artist, album, album_artist)
        VALUES (new.id, new.title, new.artist, new.album, new.album_artist);
END;
)";

std::int64_t now_unix_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

std::expected<int, transporter::engine::Error> current_version(Db& db) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(
        db.raw(),
        "SELECT MAX(version) FROM migrations",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        // Table may not exist yet.
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        return 0;
    }
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            version = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return version;
}

std::expected<void, transporter::engine::Error>
record_migration(Db& db, int version) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db.raw(),
            "INSERT INTO migrations (version, applied_at_unix_ns) "
            "VALUES (?1, ?2)",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return std::unexpected(sqlite_error(db, "migrations.insert prepare"));
    }
    sqlite3_bind_int(stmt, 1, version);
    sqlite3_bind_int64(stmt, 2, now_unix_ns());
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(sqlite_error(db, "migrations.insert step"));
    }
    return {};
}

// Migration 2: add disc_no column to tracks.
constexpr std::string_view migration_002 =
    "ALTER TABLE tracks ADD COLUMN disc_no TEXT NOT NULL DEFAULT ''";

} // namespace

std::expected<int, transporter::engine::Error> apply_migrations(Db& db) {
    const auto v = current_version(db);
    if (!v) {
        return std::unexpected(v.error());
    }
    int version = *v;

    if (version < 1) {
        if (auto e = db.exec(migration_001); !e) {
            return std::unexpected(e.error());
        }
        if (auto e = record_migration(db, 1); !e) {
            return std::unexpected(e.error());
        }
        version = 1;
    }

    if (version < 2) {
        if (auto e = db.exec(migration_002); !e) {
            return std::unexpected(e.error());
        }
        if (auto e = record_migration(db, 2); !e) {
            return std::unexpected(e.error());
        }
        version = 2;
    }

    return version;
}

} // namespace transporter::library
