// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scan the fixture tree, then exercise the search API: free-text FTS,
// per-column filters, sort, limit, offset.

#include <transporter/engine/error.hpp>
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

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <fixtures_root> <work_root>\n", argv[0]);
        return 2;
    }
    const fs::path src_root = argv[1];
    const fs::path work_root = argv[2];

    std::error_code ec;
    fs::remove_all(work_root, ec);
    copy_tree(src_root, work_root);

    const fs::path db_path = work_root.parent_path() / "library_search.db";
    fs::remove(db_path, ec);

    Config cfg;
    cfg.db_path = db_path;
    cfg.roots = {work_root};
    cfg.ignore_patterns = {"*/.Trash*"};

    std::atomic<int> finished{0};
    auto lib_or = Library::open(cfg);
    if (!lib_or) {
        return fail("open", lib_or.error().message);
    }
    auto& lib = **lib_or;
    lib.set_delta_callback([&](const DeltaEvent& ev) {
        if (ev.kind == DeltaEvent::Kind::ScanFinished) {
            ++finished;
        }
    });
    lib.rescan_async();
    if (!wait_until([&] { return finished.load() >= 1; },
                    std::chrono::seconds{30})) {
        return fail("scan timeout");
    }

    // Free-text query against title.
    {
        SearchFilter f;
        f.query = "blue";
        auto r = lib.search(f);
        if (!r) {
            return fail("fts blue", r.error().message);
        }
        // Matches "Blue Moon" (title) and "Deep Blue" (title).
        if (r->size() != 2) {
            return fail("fts blue size", std::to_string(r->size()));
        }
    }
    {
        SearchFilter f;
        f.query = "Aria";
        auto r = lib.search(f);
        if (!r) {
            return fail("fts aria", r.error().message);
        }
        // 4 tracks where artist == Aria Black.
        if (r->size() != 4) {
            return fail("fts aria size", std::to_string(r->size()));
        }
    }
    {
        SearchFilter f;
        f.query = "twilight";
        auto r = lib.search(f);
        if (!r || r->size() != 2) {
            return fail("fts twilight",
                        r ? std::to_string(r->size()) : r.error().message);
        }
    }
    // Prefix-style match: "shimm" should hit "Shimmer".
    {
        SearchFilter f;
        f.query = "shimm";
        auto r = lib.search(f);
        if (!r || r->size() != 1 || r->front().title != "Shimmer") {
            return fail("fts prefix",
                        r && !r->empty() ? r->front().title : "(empty)");
        }
    }

    // Column-filter exact match.
    {
        SearchFilter f;
        f.album = "Twilight";
        auto r = lib.search(f);
        if (!r || r->size() != 2) {
            return fail("album filter",
                        r ? std::to_string(r->size()) : r.error().message);
        }
    }

    // Limit / offset.
    {
        SearchFilter a;
        a.sort_by = SortBy::Title;
        auto all_r = lib.search(a);
        if (!all_r) {
            return fail("title all", all_r.error().message);
        }
        const auto all = *all_r;

        SearchFilter f;
        f.sort_by = SortBy::Title;
        f.limit = 3;
        f.offset = 1;
        auto r = lib.search(f);
        if (!r) {
            return fail("limit offset", r.error().message);
        }
        if (r->size() != 3) {
            return fail("limit size", std::to_string(r->size()));
        }
        for (std::size_t i = 0; i < r->size(); ++i) {
            if ((*r)[i].id != all[i + 1].id) {
                return fail("offset alignment",
                            std::to_string(i) + ":" +
                                std::to_string((*r)[i].id) + " vs " +
                                std::to_string(all[i + 1].id));
            }
        }
    }

    // Empty-query edge: returns everything.
    {
        SearchFilter f;
        auto r = lib.search(f);
        if (!r || r->size() != 7) {
            return fail("empty query",
                        r ? std::to_string(r->size()) : r.error().message);
        }
    }

    // track_by_path: known path returns the track with correct metadata.
    {
        SearchFilter f;
        f.query = "Shimmer";
        auto r = lib.search(f);
        if (!r || r->empty()) {
            return fail("track_by_path setup", "shimmer not found");
        }
        const fs::path known = r->front().path;
        auto t = lib.track_by_path(known);
        if (!t) {
            return fail("track_by_path hit", t.error().message);
        }
        if (t->path != known) {
            return fail("track_by_path path mismatch", t->path.string());
        }
        if (t->title != "Shimmer") {
            return fail("track_by_path title", t->title);
        }
    }

    // track_by_path: non-existent path returns NotFound.
    {
        const fs::path ghost = work_root / "does" / "not" / "exist.flac";
        auto t = lib.track_by_path(ghost);
        if (t) {
            return fail("track_by_path miss", "expected error, got track");
        }
        if (t.error().code != transporter::engine::ErrorCode::NotFound) {
            return fail("track_by_path miss code",
                        std::to_string(static_cast<int>(t.error().code)));
        }
    }

    std::printf("ok library_search\n");
    return 0;
}
