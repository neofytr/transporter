// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_COMPONENTS_COVER_CACHE_HPP
#define TRANSPORTER_TUI_COMPONENTS_COVER_CACHE_HPP

#include "cover.hpp"

#include <cstddef>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace transporter::tui::components {

// LRU cache of decoded covers, keyed by track-path string. The cache holds
// at most `capacity()` entries; on insertion of an entry that would push
// the size beyond the limit, the least-recently-used entry is evicted.
//
// Thread-safety: internal mutex guards the map and list. Returns shared_ptr<>
// so a render thread holding a cover survives concurrent eviction. Decoding
// happens off the cache (caller side) — we'll eventually move it to a worker
// thread; for v1 the call site does the decode synchronously.
class CoverCache {
public:
    explicit CoverCache(std::size_t capacity = 50);

    std::size_t capacity() const noexcept { return cap_; }
    std::size_t size() const;

    // Look up and bump to MRU. Returns nullptr on miss (caller decodes).
    std::shared_ptr<Cover> get(const std::filesystem::path& track_path);

    // Insert (or overwrite) and bump to MRU. Evicts LRU when over capacity.
    void put(const std::filesystem::path& track_path, Cover cov);

    // Equivalent to get() followed by put() when the key is missing. The
    // loader callback is invoked outside the lock; only its result is cached.
    // Useful in render code that wants a single call site.
    template <typename Loader>
    std::shared_ptr<Cover> get_or_load(const std::filesystem::path& track_path,
                                       Loader&& loader);

    void clear();

private:
    using Key  = std::string;
    using List = std::list<Key>;
    struct Entry {
        std::shared_ptr<Cover> cover;
        List::iterator         lru_it;
    };

    void touch_locked(typename std::unordered_map<Key, Entry>::iterator it);
    void evict_if_full_locked();

    mutable std::mutex                 mu_;
    std::size_t                        cap_;
    List                               lru_;
    std::unordered_map<Key, Entry>     map_;
};

template <typename Loader>
std::shared_ptr<Cover>
CoverCache::get_or_load(const std::filesystem::path& track_path, Loader&& loader) {
    if (auto hit = get(track_path)) {
        return hit;
    }
    Cover c = loader();
    if (!c) {
        return {};
    }
    put(track_path, std::move(c));
    return get(track_path);
}

} // namespace transporter::tui::components

#endif
