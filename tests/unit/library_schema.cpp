// SPDX-License-Identifier: GPL-3.0-or-later
//
// Open a fresh DB; assert the schema bring-up creates every expected
// table, every expected index, and writes the version row.

#include <transporter/library/library.hpp>

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace {

namespace fs = std::filesystem;

int fail(const char* what, const std::string& detail = {}) {
    if (detail.empty()) {
        std::fprintf(stderr, "FAIL [%s]\n", what);
    } else {
        std::fprintf(stderr, "FAIL [%s]: %s\n", what, detail.c_str());
    }
    return 1;
}

bool sqlite_has_object(sqlite3* db, const char* type, const char* name) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT 1 FROM sqlite_master WHERE type = ?1 AND name = ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

int journal_mode(sqlite3* db, std::string& out) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, nullptr)
        != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* p = sqlite3_column_text(stmt, 0);
        if (p) {
            out = reinterpret_cast<const char*>(p);
        }
    }
    sqlite3_finalize(stmt);
    return 1;
}

int max_version(sqlite3* db, int& out) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT MAX(version) FROM migrations",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    int rc_ok = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = sqlite3_column_int(stmt, 0);
        rc_ok = 1;
    }
    sqlite3_finalize(stmt);
    return rc_ok;
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    const fs::path tmp = fs::temp_directory_path() /
                         "transporter_library_schema_test.db";
    std::error_code ec;
    fs::remove(tmp, ec);

    {
        transporter::library::Config cfg;
        cfg.db_path = tmp;
        auto lib = transporter::library::Library::open(cfg);
        if (!lib) {
            return fail("Library::open", lib.error().message);
        }
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(tmp.c_str(), &db, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        return fail("sqlite3_open_v2", sqlite3_errmsg(db));
    }

    const char* tables[] = {
        "tracks", "artists", "albums", "track_album",
        "tracks_fts", "migrations",
    };
    for (const char* t : tables) {
        if (!sqlite_has_object(db, "table", t)) {
            sqlite3_close(db);
            return fail("missing table", t);
        }
    }

    const char* indexes[] = {
        "idx_tracks_artist", "idx_tracks_album",
        "idx_tracks_album_artist", "idx_tracks_path",
        "idx_tracks_added_at", "idx_track_album_album",
    };
    for (const char* i : indexes) {
        if (!sqlite_has_object(db, "index", i)) {
            sqlite3_close(db);
            return fail("missing index", i);
        }
    }

    const char* triggers[] = {
        "trg_tracks_ai", "trg_tracks_ad", "trg_tracks_au",
    };
    for (const char* tr : triggers) {
        if (!sqlite_has_object(db, "trigger", tr)) {
            sqlite3_close(db);
            return fail("missing trigger", tr);
        }
    }

    int v = -1;
    if (!max_version(db, v) || v != 1) {
        sqlite3_close(db);
        return fail("schema version", std::to_string(v));
    }

    std::string mode;
    journal_mode(db, mode);
    sqlite3_close(db);
    if (mode != "wal") {
        return fail("journal_mode", mode);
    }

    fs::remove(tmp, ec);
    std::printf("ok library_schema: tables=%zu indexes=%zu triggers=3 version=%d\n",
                sizeof(tables) / sizeof(tables[0]),
                sizeof(indexes) / sizeof(indexes[0]),
                v);
    return 0;
}
