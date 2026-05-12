// SPDX-License-Identifier: GPL-3.0-or-later

#include "cover_cache.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace transporter::tui::components {

CoverCache::CoverCache(std::size_t capacity)
    : cap_(capacity > 0 ? capacity : 1) {}

std::size_t CoverCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

void CoverCache::touch_locked(
    typename std::unordered_map<Key, Entry>::iterator it) {
    lru_.splice(lru_.begin(), lru_, it->second.lru_it);
    it->second.lru_it = lru_.begin();
}

void CoverCache::evict_if_full_locked() {
    while (map_.size() > cap_) {
        const Key& victim = lru_.back();
        map_.erase(victim);
        lru_.pop_back();
    }
}

std::shared_ptr<Cover> CoverCache::get(const std::filesystem::path& track_path) {
    const Key k = track_path.string();
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(k);
    if (it == map_.end()) {
        return {};
    }
    touch_locked(it);
    return it->second.cover;
}

void CoverCache::put(const std::filesystem::path& track_path, Cover cov) {
    const Key k = track_path.string();
    auto sp = std::make_shared<Cover>(std::move(cov));
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(k);
    if (it != map_.end()) {
        it->second.cover = std::move(sp);
        touch_locked(it);
        return;
    }
    lru_.push_front(k);
    map_.emplace(k, Entry{std::move(sp), lru_.begin()});
    evict_if_full_locked();
}

void CoverCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
    lru_.clear();
}

} // namespace transporter::tui::components
