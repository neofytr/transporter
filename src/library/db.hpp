// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_LIBRARY_DB_HPP
#define TRANSPORTER_LIBRARY_DB_HPP

#include <transporter/engine/error.hpp>

#include <sqlite3.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace transporter::library {

// RAII wrapper for a sqlite3* with a prepared-statement cache keyed by SQL
// string. Methods are NOT thread-safe by themselves: callers serialize via
// the connection-owner. The library opens one Db per role:
//
//   - one for the scanner thread (write)
//   - one shared for read queries, guarded by a mutex
//
// SQLite is built thread-safe (SQLITE_THREADSAFE=1) so distinct connections
// do not need an external lock between each other.
class Db {
public:
    static std::expected<std::unique_ptr<Db>, transporter::engine::Error>
    open(const std::filesystem::path& path, bool read_only);

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&&) = delete;
    Db& operator=(Db&&) = delete;
    ~Db();

    sqlite3* raw() const noexcept { return handle_.get(); }

    // Returns a cached prepared statement for `sql`. The pointer is stable
    // for the lifetime of the Db. Callers reset+bind+step+reset on each
    // use. Errors during prepare surface as ErrorCode::Sqlite.
    std::expected<sqlite3_stmt*, transporter::engine::Error>
    prepare(std::string_view sql);

    // Convenience: run a statement to completion, expecting no rows.
    std::expected<void, transporter::engine::Error> exec(std::string_view sql);

    // Last error message from sqlite3_errmsg.
    std::string last_error() const;

private:
    Db() = default;

    struct CloseDeleter {
        void operator()(sqlite3* p) const noexcept {
            if (p) {
                sqlite3_close_v2(p);
            }
        }
    };
    struct StmtDeleter {
        void operator()(sqlite3_stmt* s) const noexcept {
            if (s) {
                sqlite3_finalize(s);
            }
        }
    };

    std::unique_ptr<sqlite3, CloseDeleter> handle_;
    std::unordered_map<std::string,
                       std::unique_ptr<sqlite3_stmt, StmtDeleter>> stmts_;
};

// Helper: turn an int sqlite rc + Db into an Error.
transporter::engine::Error sqlite_error(const Db& db, std::string_view where);

// Apply pending schema migrations on `db`. Idempotent; safe to call on
// startup. Returns the schema version after migration.
std::expected<int, transporter::engine::Error> apply_migrations(Db& db);

// Schema version this build expects. Bump when adding a migration.
inline constexpr int schema_target_version = 1;

} // namespace transporter::library

#endif
