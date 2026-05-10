// SPDX-License-Identifier: GPL-3.0-or-later

#include "db.hpp"

#include <transporter/engine/error.hpp>

#include <sqlite3.h>

#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace transporter::library {

transporter::engine::Error sqlite_error(const Db& db, std::string_view where) {
    std::string msg{where};
    msg += ": ";
    msg += db.last_error();
    return transporter::engine::Error{
        transporter::engine::ErrorCode::Sqlite, std::move(msg)};
}

std::expected<std::unique_ptr<Db>, transporter::engine::Error>
Db::open(const std::filesystem::path& path, bool read_only) {
    auto db = std::unique_ptr<Db>(new Db{});

    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE;
    if (read_only) {
        flags |= SQLITE_OPEN_READONLY;
    } else {
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }

    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    db->handle_.reset(raw);
    if (rc != SQLITE_OK) {
        std::string msg = "sqlite3_open_v2 ";
        msg += path.string();
        msg += ": ";
        if (raw) {
            msg += sqlite3_errmsg(raw);
        } else {
            msg += sqlite3_errstr(rc);
        }
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::Sqlite, std::move(msg)});
    }

    sqlite3_busy_timeout(raw, 5000);

    if (!read_only) {
        // Pragmas applied per-connection. journal_mode=WAL is sticky on the
        // file but each connection still has to set synchronous and
        // foreign_keys.
        const char* const pragmas[] = {
            "PRAGMA journal_mode=WAL",
            "PRAGMA synchronous=NORMAL",
            "PRAGMA foreign_keys=ON",
            "PRAGMA temp_store=MEMORY",
            "PRAGMA cache_size=-16384", // 16 MB
        };
        for (const char* p : pragmas) {
            char* errmsg = nullptr;
            if (sqlite3_exec(raw, p, nullptr, nullptr, &errmsg) != SQLITE_OK) {
                std::string msg = p;
                msg += ": ";
                if (errmsg) {
                    msg += errmsg;
                    sqlite3_free(errmsg);
                }
                return std::unexpected(transporter::engine::Error{
                    transporter::engine::ErrorCode::Sqlite, std::move(msg)});
            }
        }
    } else {
        // Read connections still want foreign_keys for join semantics; WAL
        // lets them read while writers commit.
        sqlite3_exec(raw, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
        sqlite3_exec(raw, "PRAGMA query_only=ON",  nullptr, nullptr, nullptr);
    }

    return db;
}

Db::~Db() {
    // Finalize cached statements before closing the connection.
    stmts_.clear();
}

std::expected<sqlite3_stmt*, transporter::engine::Error>
Db::prepare(std::string_view sql) {
    const std::string key{sql};
    if (auto it = stmts_.find(key); it != stmts_.end()) {
        sqlite3_reset(it->second.get());
        sqlite3_clear_bindings(it->second.get());
        return it->second.get();
    }

    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(
        handle_.get(), sql.data(),
        static_cast<int>(sql.size()), &raw, nullptr);
    if (rc != SQLITE_OK) {
        if (raw) {
            sqlite3_finalize(raw);
        }
        std::string msg = "prepare: ";
        msg += sqlite3_errmsg(handle_.get());
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::Sqlite, std::move(msg)});
    }

    auto [it, _] = stmts_.emplace(
        std::move(key),
        std::unique_ptr<sqlite3_stmt, StmtDeleter>(raw));
    return it->second.get();
}

std::expected<void, transporter::engine::Error>
Db::exec(std::string_view sql) {
    char* errmsg = nullptr;
    const std::string s{sql};
    const int rc = sqlite3_exec(
        handle_.get(), s.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = "exec: ";
        if (errmsg) {
            msg += errmsg;
            sqlite3_free(errmsg);
        }
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::Sqlite, std::move(msg)});
    }
    return {};
}

std::string Db::last_error() const {
    const char* msg = sqlite3_errmsg(handle_.get());
    return msg ? std::string{msg} : std::string{};
}

} // namespace transporter::library
