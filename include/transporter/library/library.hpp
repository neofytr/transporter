// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_LIBRARY_LIBRARY_HPP
#define TRANSPORTER_LIBRARY_LIBRARY_HPP

#include <transporter/engine/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace transporter::library {

// One row in the library, denormalized for the GUI list view. `bit_depth`
// is 0 for lossy formats (MP3 / Vorbis / Opus / lossy ALAC), set for PCM /
// FLAC / lossless ALAC. `total_frames` is 0 if the decoder couldn't tell
// (rare; some streams lack a seek table). `mtime_unix_ns` is filesystem
// modification time at scan time.
struct Track {
    std::int64_t id;
    std::filesystem::path path;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string track_no;
    std::string date;
    std::string codec;
    std::uint32_t sample_rate_hz;
    std::uint16_t channels;
    std::uint16_t bit_depth;
    std::uint64_t total_frames;
    std::chrono::milliseconds duration;
    std::int64_t file_size_bytes;
    std::int64_t mtime_unix_ns;
};

struct Album {
    std::int64_t id;
    std::string title;
    std::string album_artist;
    std::string date;
    std::int64_t track_count;
};

struct Artist {
    std::int64_t id;
    std::string name;
    std::int64_t album_count;
    std::int64_t track_count;
};

enum class SortBy : std::uint8_t {
    Artist,
    Album,
    Title,
    DateAdded,
};

// Free-text query is matched via FTS5 against (title, artist, album,
// album_artist). `artist` / `album` filter rows to an exact match on the
// corresponding column. limit/offset are 0-based; limit=0 means unbounded.
struct SearchFilter {
    std::string query;
    std::string artist;
    std::string album;
    SortBy      sort_by = SortBy::Artist;
    std::size_t limit  = 0;
    std::size_t offset = 0;
};

enum class ScanState : std::uint8_t {
    Idle,
    Scanning,
    Indexing,
    Cleaning,
};

struct ScanProgress {
    ScanState state;
    std::size_t files_seen;
    std::size_t files_indexed;
    std::size_t files_deleted;
    std::filesystem::path current_path;
    std::chrono::steady_clock::time_point started_at;
};

// Delivered on the scanner thread. Callbacks must return promptly. ScanStarted
// fires before the directory walk; ScanFinished after the deletion sweep.
struct DeltaEvent {
    enum class Kind : std::uint8_t {
        Added,
        Updated,
        Removed,
        ScanStarted,
        ScanFinished,
    };
    Kind kind;
    std::int64_t track_id;
    std::filesystem::path path;
};

using DeltaCallback = std::function<void(const DeltaEvent&)>;

struct Config {
    std::filesystem::path db_path;
    std::vector<std::filesystem::path> roots;
    std::vector<std::string> ignore_patterns;
};

// Owns the SQLite database and the scanner thread. Open() initializes the
// schema (running any pending migrations) and starts the scanner thread idle;
// rescan_async() kicks it off. Queries run on the calling thread against a
// shared, mutex-guarded read connection; the scanner has its own write
// connection. Reads do not block writes thanks to WAL.
//
// All public methods are thread-safe.
class Library {
public:
    static std::expected<std::unique_ptr<Library>, transporter::engine::Error>
    open(Config cfg);

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;
    ~Library();

    void set_delta_callback(DeltaCallback cb);

    void rescan_async();

    ScanProgress progress() const;

    std::expected<std::vector<Track>, transporter::engine::Error>
    search(const SearchFilter& filter) const;

    std::expected<std::vector<Album>, transporter::engine::Error>
    albums(const std::string& artist) const;

    std::expected<std::vector<Artist>, transporter::engine::Error>
    artists() const;

    std::expected<std::vector<Track>, transporter::engine::Error>
    tracks_in_album(std::int64_t album_id) const;

    std::expected<Track, transporter::engine::Error>
    track_by_id(std::int64_t id) const;

private:
    Library();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace transporter::library

#endif
