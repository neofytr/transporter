// SPDX-License-Identifier: GPL-3.0-or-later
//
// Owner-side driver: scan a real music root, print the artist / album /
// track tree. Intended to be run by hand against ~/Music.
//
// usage: test_library_browse <music_root> [<scratch_db_path>]
//
// If scratch_db_path is omitted, the DB is written to
// <music_root>/.transporter-library.db.

#include <transporter/library/library.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace transporter::library;

namespace {

template <class F>
void wait_for(F&& pred, std::chrono::seconds timeout) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline && !pred()) {
        std::this_thread::sleep_for(milliseconds{50});
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr,
                     "usage: %s <music_root> [<scratch_db_path>]\n", argv[0]);
        return 2;
    }
    const fs::path root = argv[1];
    if (!fs::is_directory(root)) {
        std::fprintf(stderr, "%s is not a directory\n", argv[1]);
        return 2;
    }
    const fs::path db_path = (argc == 3)
        ? fs::path{argv[2]}
        : root / ".transporter-library.db";

    Config cfg;
    cfg.db_path = db_path;
    cfg.roots = {root};
    cfg.ignore_patterns = {"*/.Trash*", "*/.git/*", "*/lost+found/*"};

    auto lib_or = Library::open(cfg);
    if (!lib_or) {
        std::fprintf(stderr, "open: %s\n", lib_or.error().message.c_str());
        return 1;
    }
    auto& lib = **lib_or;

    std::atomic<int> finished{0};
    lib.set_delta_callback([&](const DeltaEvent& ev) {
        if (ev.kind == DeltaEvent::Kind::ScanFinished) {
            ++finished;
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    lib.rescan_async();

    while (finished.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        const auto p = lib.progress();
        std::fprintf(stderr,
                     "  scanning... seen=%zu indexed=%zu deleted=%zu\n",
                     p.files_seen, p.files_indexed, p.files_deleted);
    }

    const auto dt = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    const auto p = lib.progress();
    std::printf("scan done in %.2fs: seen=%zu indexed=%zu deleted=%zu\n",
                dt, p.files_seen, p.files_indexed, p.files_deleted);

    auto artists = lib.artists();
    if (!artists) {
        std::fprintf(stderr, "artists: %s\n", artists.error().message.c_str());
        return 1;
    }
    std::printf("\n=== %zu artists ===\n", artists->size());
    for (const auto& a : *artists) {
        std::printf("  %s  (albums=%lld tracks=%lld)\n",
                    a.name.c_str(),
                    static_cast<long long>(a.album_count),
                    static_cast<long long>(a.track_count));
    }

    auto albums = lib.albums("");
    if (!albums) {
        std::fprintf(stderr, "albums: %s\n", albums.error().message.c_str());
        return 1;
    }
    std::printf("\n=== %zu albums ===\n", albums->size());
    for (const auto& al : *albums) {
        std::printf("\n[%s] %s (%s) -- %lld tracks\n",
                    al.album_artist.c_str(),
                    al.title.c_str(),
                    al.date.c_str(),
                    static_cast<long long>(al.track_count));
        auto tracks = lib.tracks_in_album(al.id);
        if (!tracks) {
            std::fprintf(stderr, "  tracks_in_album: %s\n",
                         tracks.error().message.c_str());
            continue;
        }
        for (const auto& t : *tracks) {
            std::printf("  %s. %s  [%s %u/%u/%u]  %lld ms\n",
                        t.track_no.empty() ? "?" : t.track_no.c_str(),
                        t.title.empty() ? t.path.filename().string().c_str()
                                        : t.title.c_str(),
                        t.codec.c_str(),
                        t.sample_rate_hz,
                        unsigned{t.bit_depth},
                        unsigned{t.channels},
                        static_cast<long long>(t.duration.count()));
        }
    }

    return 0;
}
