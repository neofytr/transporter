// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_LIBRARY_SCANNER_HPP
#define TRANSPORTER_LIBRARY_SCANNER_HPP

#include "db.hpp"

#include <transporter/library/library.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace transporter::library {

// Background scanner. Owns its own write Db connection. Wakes when
// `request_scan()` is called or shutdown_ is set.
class Scanner {
public:
    Scanner(std::filesystem::path db_path,
            std::vector<std::filesystem::path> roots,
            std::vector<std::string> ignore_patterns,
            DeltaCallback cb);

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;
    Scanner(Scanner&&) = delete;
    Scanner& operator=(Scanner&&) = delete;
    ~Scanner();

    // Wake the scanner thread to perform a full scan + sweep.
    void request_scan();

    // Replace watched roots and/or ignore patterns; takes effect on the next
    // scan (not the current one if one is in progress).
    void set_roots(std::vector<std::filesystem::path> roots);
    void set_ignore_patterns(std::vector<std::string> patterns);

    ScanProgress progress() const;

    void set_callback(DeltaCallback cb);

private:
    void thread_main();
    void run_one_scan(Db& db);
    void walk_root(Db& db, const std::filesystem::path& root,
                   std::vector<std::filesystem::path>& live_paths);
    void sweep_deletions(Db& db,
                         const std::vector<std::filesystem::path>& live_paths);
    bool path_ignored(const std::filesystem::path& p) const;
    void emit(DeltaEvent::Kind k, std::int64_t id,
              const std::filesystem::path& p);

    void set_state(ScanState s);
    void set_current_path(const std::filesystem::path& p);

    std::filesystem::path db_path_;
    mutable std::mutex roots_mu_;
    std::vector<std::filesystem::path> roots_;
    std::vector<std::string> ignore_patterns_;

    // Per-scan snapshots; written at the top of run_one_scan (scanner thread
    // only) and read throughout the same scan via walk_root / path_ignored.
    std::vector<std::filesystem::path> scan_roots_;
    std::vector<std::string>           scan_patterns_;

    mutable std::mutex cb_mu_;
    DeltaCallback cb_;

    std::mutex wake_mu_;
    std::condition_variable wake_cv_;
    bool wake_requested_ = false;

    std::atomic<bool> shutdown_{false};
    std::atomic<bool> cancel_requested_{false};

    // Progress fields read concurrently by progress(); written only by the
    // scanner thread. Strings/paths are guarded by progress_mu_.
    mutable std::mutex progress_mu_;
    ScanState state_{ScanState::Idle};
    std::size_t files_seen_{0};
    std::size_t files_indexed_{0};
    std::size_t files_deleted_{0};
    std::filesystem::path current_path_;
    std::chrono::steady_clock::time_point started_at_;

    std::thread thread_;
};

} // namespace transporter::library

#endif
