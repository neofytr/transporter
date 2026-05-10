// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_LIBRARY_QUERIES_HPP
#define TRANSPORTER_LIBRARY_QUERIES_HPP

#include <string_view>

namespace transporter::library::queries {

// All SQL strings in one place so they're reviewable. No string concatenation
// for parameters anywhere; bind always.

inline constexpr std::string_view track_select_columns =
    "id, path, title, artist, album, album_artist, track_no, date, codec, "
    "sample_rate_hz, channels, bit_depth, total_frames, duration_ms, "
    "file_size, mtime_unix_ns";

inline constexpr std::string_view select_track_by_path =
    "SELECT id, mtime_unix_ns, file_size FROM tracks WHERE path = ?1";

inline constexpr std::string_view select_track_by_id =
    "SELECT id, path, title, artist, album, album_artist, track_no, date, "
    "codec, sample_rate_hz, channels, bit_depth, total_frames, duration_ms, "
    "file_size, mtime_unix_ns FROM tracks WHERE id = ?1";

inline constexpr std::string_view upsert_track =
    "INSERT INTO tracks "
    "(path, title, artist, album, album_artist, track_no, date, codec, "
    " sample_rate_hz, channels, bit_depth, total_frames, duration_ms, "
    " file_size, mtime_unix_ns, added_at_unix_ns) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, "
    "        ?14, ?15, ?16) "
    "ON CONFLICT(path) DO UPDATE SET "
    "  title=excluded.title, artist=excluded.artist, album=excluded.album, "
    "  album_artist=excluded.album_artist, track_no=excluded.track_no, "
    "  date=excluded.date, codec=excluded.codec, "
    "  sample_rate_hz=excluded.sample_rate_hz, channels=excluded.channels, "
    "  bit_depth=excluded.bit_depth, total_frames=excluded.total_frames, "
    "  duration_ms=excluded.duration_ms, file_size=excluded.file_size, "
    "  mtime_unix_ns=excluded.mtime_unix_ns "
    "RETURNING id";

inline constexpr std::string_view delete_track_by_id =
    "DELETE FROM tracks WHERE id = ?1";

inline constexpr std::string_view delete_track_by_path =
    "DELETE FROM tracks WHERE path = ?1";

inline constexpr std::string_view select_all_paths =
    "SELECT id, path FROM tracks";

inline constexpr std::string_view upsert_artist =
    "INSERT INTO artists (name) VALUES (?1) "
    "ON CONFLICT(name) DO UPDATE SET name=excluded.name "
    "RETURNING id";

inline constexpr std::string_view upsert_album =
    "INSERT INTO albums (title, album_artist, date) VALUES (?1, ?2, ?3) "
    "ON CONFLICT(title, album_artist) DO UPDATE SET date=excluded.date "
    "RETURNING id";

inline constexpr std::string_view upsert_track_album =
    "INSERT INTO track_album (track_id, album_id) VALUES (?1, ?2) "
    "ON CONFLICT(track_id) DO UPDATE SET album_id=excluded.album_id";

inline constexpr std::string_view select_artists =
    "SELECT a.id, a.name, "
    "       (SELECT COUNT(DISTINCT ta.album_id) "
    "        FROM tracks t JOIN track_album ta ON t.id = ta.track_id "
    "        WHERE t.artist = a.name OR t.album_artist = a.name) AS album_count, "
    "       (SELECT COUNT(*) FROM tracks t "
    "        WHERE t.artist = a.name OR t.album_artist = a.name) AS track_count "
    "FROM artists a "
    "ORDER BY a.name COLLATE NOCASE ASC";

inline constexpr std::string_view select_albums_all =
    "SELECT al.id, al.title, al.album_artist, al.date, "
    "       (SELECT COUNT(*) FROM track_album ta WHERE ta.album_id = al.id) "
    "FROM albums al "
    "ORDER BY al.album_artist COLLATE NOCASE ASC, al.title COLLATE NOCASE ASC";

inline constexpr std::string_view select_albums_by_artist =
    "SELECT DISTINCT al.id, al.title, al.album_artist, al.date, "
    "       (SELECT COUNT(*) FROM track_album ta WHERE ta.album_id = al.id) "
    "FROM albums al "
    "JOIN track_album ta ON ta.album_id = al.id "
    "JOIN tracks t ON t.id = ta.track_id "
    "WHERE t.artist = ?1 OR t.album_artist = ?1 OR al.album_artist = ?1 "
    "ORDER BY al.title COLLATE NOCASE ASC";

inline constexpr std::string_view select_tracks_in_album =
    "SELECT t.id, t.path, t.title, t.artist, t.album, t.album_artist, "
    "       t.track_no, t.date, t.codec, t.sample_rate_hz, t.channels, "
    "       t.bit_depth, t.total_frames, t.duration_ms, t.file_size, "
    "       t.mtime_unix_ns "
    "FROM tracks t JOIN track_album ta ON ta.track_id = t.id "
    "WHERE ta.album_id = ?1 "
    "ORDER BY CAST(t.track_no AS INTEGER) ASC, t.title COLLATE NOCASE ASC";

// Search builders. The library composes the WHERE clause and ORDER BY at
// runtime from the SearchFilter; see search.cpp comments for the rule.
// The base query selects the same columns as select_track_by_id.

inline constexpr std::string_view fts_insert =
    "INSERT INTO tracks_fts (rowid, title, artist, album, album_artist) "
    "VALUES (?1, ?2, ?3, ?4, ?5)";

inline constexpr std::string_view fts_delete =
    "DELETE FROM tracks_fts WHERE rowid = ?1";

inline constexpr std::string_view begin_immediate = "BEGIN IMMEDIATE";
inline constexpr std::string_view commit          = "COMMIT";
inline constexpr std::string_view rollback        = "ROLLBACK";

} // namespace transporter::library::queries

#endif
