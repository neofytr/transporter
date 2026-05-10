// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scan the committed fixture tree, then verify:
//   - track count and per-artist counts
//   - sorting by artist / title
//   - mtime mutation triggers a re-index of one row only
//   - file deletion fires a Removed event and prunes the row
//
// The fixture directory is read-only; we copy it into the Meson build dir
// so we can mutate freely without polluting the source tree.

#include <transporter/library/library.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace transporter::library;

namespace {

int fail(const char* what, const std::string& detail = {}) {
    if (detail.empty()) {
        std::fprintf(stderr, "FAIL [%s]\n", what);
    } else {
        std::fprintf(stderr, "FAIL [%s]: %s\n", what, detail.c_str());
    }
    return 1;
}

void copy_tree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy(from, to,
             fs::copy_options::recursive |
             fs::copy_options::overwrite_existing);
}

template <class F>
bool wait_until(F&& pred, std::chrono::milliseconds timeout) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds{20});
    }
    return pred();
}

struct DeltaCounters {
    std::mutex mu;
    std::atomic<int> added{0};
    std::atomic<int> updated{0};
    std::atomic<int> removed{0};
    std::atomic<int> scan_finished{0};
    std::vector<fs::path> removed_paths;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <fixtures_root> <work_root>\n"
                     "  fixtures_root: source tests/data/library\n"
                     "  work_root: writable scratch dir under build/\n",
                     argv[0]);
        return 2;
    }
    const fs::path src_root  = argv[1];
    const fs::path work_root = argv[2];

    std::error_code ec;
    fs::remove_all(work_root, ec);
    copy_tree(src_root, work_root);

    const fs::path db_path = work_root.parent_path() / "library_scan.db";
    fs::remove(db_path, ec);

    Config cfg;
    cfg.db_path = db_path;
    cfg.roots = {work_root};
    cfg.ignore_patterns = {"*/.Trash*", "*/lost+found/*"};

    DeltaCounters counters;
    auto lib_or = Library::open(cfg);
    if (!lib_or) {
        return fail("Library::open", lib_or.error().message);
    }
    auto& lib = **lib_or;

    lib.set_delta_callback([&](const DeltaEvent& ev) {
        switch (ev.kind) {
        case DeltaEvent::Kind::Added:        ++counters.added;        break;
        case DeltaEvent::Kind::Updated:      ++counters.updated;      break;
        case DeltaEvent::Kind::Removed: {
            std::lock_guard<std::mutex> lk(counters.mu);
            ++counters.removed;
            counters.removed_paths.push_back(ev.path);
            break;
        }
        case DeltaEvent::Kind::ScanStarted:                          break;
        case DeltaEvent::Kind::ScanFinished:  ++counters.scan_finished; break;
        }
    });

    lib.rescan_async();

    if (!wait_until([&] { return counters.scan_finished.load() >= 1; },
                    std::chrono::seconds{30})) {
        return fail("first scan did not finish");
    }

    // Expect 7 audio files indexed. The .Trash entry is ignored;
    // README.txt is filtered by extension.
    if (counters.added.load() != 7) {
        return fail("added != 7", std::to_string(counters.added.load()));
    }

    auto search_all = SearchFilter{};
    auto all = lib.search(search_all);
    if (!all || all->size() != 7) {
        return fail("search size", std::to_string(all ? all->size() : 0u));
    }

    // SortBy::Artist: row 0 must be Aria Black.
    SearchFilter sf_artist;
    sf_artist.sort_by = SortBy::Artist;
    auto by_artist = lib.search(sf_artist);
    if (!by_artist) {
        return fail("search artist", by_artist.error().message);
    }
    if (by_artist->front().artist != "Aria Black") {
        return fail("first artist != Aria Black",
                    by_artist->front().artist);
    }

    // SortBy::Title: row 0 must be alphabetical.
    SearchFilter sf_title;
    sf_title.sort_by = SortBy::Title;
    auto by_title = lib.search(sf_title);
    if (!by_title || by_title->front().title != "Blue Moon") {
        return fail("title sort first",
                    by_title ? by_title->front().title : std::string{});
    }

    // Filter by artist.
    SearchFilter sf_only_aria;
    sf_only_aria.artist = "Aria Black";
    auto only_aria = lib.search(sf_only_aria);
    if (!only_aria) {
        return fail("filter aria", only_aria.error().message);
    }
    // Aria Black appears on Twilight (2), Origin (1), Compilation (1) = 4
    if (only_aria->size() != 4) {
        return fail("aria count", std::to_string(only_aria->size()));
    }

    auto artists = lib.artists();
    if (!artists || artists->size() != 2) {
        // 2 unique artists appear in any track row: "Aria Black", "Cmaj7".
        return fail("artist count",
                    std::to_string(artists ? artists->size() : 0u));
    }

    // 4 distinct album titles, but since album_artist falls back to the
    // track's `artist` (Tags has no album_artist field), the compilation
    // becomes two album rows -- one per contributing artist. That's the
    // honest answer for files that don't carry album_artist tags.
    auto albums = lib.albums("");
    if (!albums || albums->size() != 5) {
        return fail("album count",
                    std::to_string(albums ? albums->size() : 0u));
    }

    // Mutate one file's mtime, rescan, expect exactly one re-index.
    const fs::path mut_path = work_root /
        "Aria Black" / "Twilight" / "01-evenfall.flac";
    if (!fs::exists(mut_path)) {
        return fail("mutate target missing", mut_path.string());
    }
    counters.added.store(0);
    counters.updated.store(0);
    counters.removed.store(0);
    counters.scan_finished.store(0);
    fs::last_write_time(mut_path,
                        fs::file_time_type::clock::now() +
                            std::chrono::seconds{2});
    lib.rescan_async();
    if (!wait_until([&] { return counters.scan_finished.load() >= 1; },
                    std::chrono::seconds{30})) {
        return fail("second scan timeout");
    }

    if (counters.added.load() != 0) {
        return fail("added on touch", std::to_string(counters.added.load()));
    }
    if (counters.updated.load() != 1) {
        return fail("updated on touch",
                    std::to_string(counters.updated.load()));
    }

    // Delete a file, rescan, expect Removed.
    const fs::path del_path = work_root / "Cmaj7" / "First Light" /
                              "02-blue moon.flac";
    fs::remove(del_path, ec);
    counters.added.store(0);
    counters.updated.store(0);
    counters.removed.store(0);
    counters.scan_finished.store(0);
    lib.rescan_async();
    if (!wait_until([&] { return counters.scan_finished.load() >= 1; },
                    std::chrono::seconds{30})) {
        return fail("third scan timeout");
    }
    if (counters.removed.load() != 1) {
        return fail("removed count",
                    std::to_string(counters.removed.load()));
    }
    auto post = lib.search(SearchFilter{});
    if (!post || post->size() != 6) {
        return fail("post-delete size",
                    std::to_string(post ? post->size() : 0u));
    }

    std::printf("ok library_scan: added=7 updated=1 removed=1 final=%zu\n",
                post->size());
    return 0;
}
