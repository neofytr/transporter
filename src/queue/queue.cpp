// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/queue/queue.hpp>

#include <algorithm>
#include <filesystem>

namespace transporter::queue {

Queue::Queue(engine::Engine& engine) : engine_(engine) {}

void Queue::on_event(const engine::Event& ev) {
    using K = engine::Event::Kind;
    std::lock_guard lk(mtx_);
    switch (ev.kind) {
    case K::TrackLoaded:
        // A track just became active (either via load() or a gapless swap).
        // Speculatively open the next decoder so the engine can do a gapless
        // swap at EOF when the formats match.
        preload_next_locked_();
        break;
    case K::TrackEnded:
        // The engine could not do a gapless swap (format mismatch or no
        // preload staged). Advance manually and issue an explicit load().
        if (current_index_ + 1 < static_cast<int>(tracks_.size())) {
            ++current_index_;
            (void)engine_.load(tracks_[static_cast<std::size_t>(current_index_)]);
        }
        break;
    default:
        break;
    }
}

void Queue::append(std::filesystem::path path) {
    std::lock_guard lk(mtx_);
    tracks_.push_back(std::move(path));
    // If the engine is idle and nothing is playing, start immediately.
    if (current_index_ < 0 && tracks_.size() == 1) {
        current_index_ = 0;
        (void)engine_.load(tracks_[0]);
        (void)engine_.play();
    }
}

void Queue::insert(int index, std::filesystem::path path) {
    std::lock_guard lk(mtx_);
    const int sz = static_cast<int>(tracks_.size());
    const int clamped = std::clamp(index, 0, sz);
    tracks_.insert(tracks_.begin() + clamped, std::move(path));
    if (current_index_ >= clamped) {
        ++current_index_;
    }
}

void Queue::remove(int index) {
    std::lock_guard lk(mtx_);
    const int sz = static_cast<int>(tracks_.size());
    if (index < 0 || index >= sz) {
        return;
    }
    tracks_.erase(tracks_.begin() + index);
    if (index < current_index_) {
        --current_index_;
    } else if (index == current_index_) {
        // Removed the playing track. If there's a next one, load it.
        if (current_index_ >= static_cast<int>(tracks_.size())) {
            current_index_ = static_cast<int>(tracks_.size()) - 1;
        }
        if (current_index_ >= 0) {
            (void)engine_.load(
                tracks_[static_cast<std::size_t>(current_index_)]);
        } else {
            (void)engine_.stop();
        }
    }
}

void Queue::move(int from, int to) {
    std::lock_guard lk(mtx_);
    const int sz = static_cast<int>(tracks_.size());
    if (from < 0 || from >= sz || to < 0 || to >= sz || from == to) {
        return;
    }
    auto path = std::move(tracks_[static_cast<std::size_t>(from)]);
    tracks_.erase(tracks_.begin() + from);
    if (to > from) {
        --to;
    }
    tracks_.insert(tracks_.begin() + to, std::move(path));
    // Adjust current_index_ to follow the moved track if it was the current
    // one, or shift it appropriately otherwise.
    if (current_index_ == from) {
        current_index_ = to;
    } else {
        if (from < current_index_) {
            --current_index_;
        }
        if (to <= current_index_) {
            ++current_index_;
        }
    }
}

void Queue::clear() {
    std::lock_guard lk(mtx_);
    tracks_.clear();
    current_index_ = -1;
    (void)engine_.stop();
}

void Queue::jump(int index) {
    std::lock_guard lk(mtx_);
    const int sz = static_cast<int>(tracks_.size());
    if (sz == 0) {
        return;
    }
    current_index_ = std::clamp(index, 0, sz - 1);
    engine_.cancel_preload();
    (void)engine_.load(tracks_[static_cast<std::size_t>(current_index_)]);
    (void)engine_.play();
}

std::vector<std::filesystem::path> Queue::tracks() const {
    std::lock_guard lk(mtx_);
    return tracks_;
}

int Queue::current_index() const {
    std::lock_guard lk(mtx_);
    return current_index_;
}

int Queue::size() const {
    std::lock_guard lk(mtx_);
    return static_cast<int>(tracks_.size());
}

void Queue::preload_next_locked_() {
    const int next = current_index_ + 1;
    if (next < static_cast<int>(tracks_.size())) {
        (void)engine_.preload(tracks_[static_cast<std::size_t>(next)]);
    }
}

} // namespace transporter::queue
