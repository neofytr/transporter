// SPDX-License-Identifier: GPL-3.0-or-later

#include "dominant_color.hpp"

#include "../views/albumart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace transporter::gui {

namespace {

// Linear RGB in [0,1] → HSV. Hue is degrees [0,360); s, v in [0,1].
void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d  = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d < 1e-6f) { h = 0.0f; return; }
    if (mx == r)      h = 60.0f * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
    else              h = 60.0f * ((r - g) / d + 4.0f);
    if (h < 0.0f) h += 360.0f;
}

void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    if      (h <  60) { rp = c; gp = x; }
    else if (h < 120) { rp = x; gp = c; }
    else if (h < 180) { gp = c; bp = x; }
    else if (h < 240) { gp = x; bp = c; }
    else if (h < 300) { rp = x; bp = c; }
    else              { rp = c; bp = x; }
    r = rp + m;
    g = gp + m;
    b = bp + m;
}

constexpr std::size_t kCacheMax = 50;

struct Cache {
    std::mutex mtx;
    std::list<std::pair<std::string, DominantColor>> entries;
    std::unordered_map<std::string,
        std::list<std::pair<std::string, DominantColor>>::iterator> index;
};

Cache& cache() {
    static Cache c;
    return c;
}

bool lookup(const std::string& key, DominantColor& out) {
    auto& c = cache();
    std::lock_guard lk(c.mtx);
    auto it = c.index.find(key);
    if (it == c.index.end()) return false;
    c.entries.splice(c.entries.begin(), c.entries, it->second);
    out = it->second->second;
    return true;
}

void store(const std::string& key, const DominantColor& dc) {
    auto& c = cache();
    std::lock_guard lk(c.mtx);
    auto it = c.index.find(key);
    if (it != c.index.end()) {
        it->second->second = dc;
        c.entries.splice(c.entries.begin(), c.entries, it->second);
        return;
    }
    c.entries.emplace_front(key, dc);
    c.index.emplace(key, c.entries.begin());
    while (c.entries.size() > kCacheMax) {
        c.index.erase(c.entries.back().first);
        c.entries.pop_back();
    }
}

} // namespace

DominantColor default_dominant_color() {
    return DominantColor{};
}

DominantColor compute_dominant(const std::string& track_key, const AlbumArt& art) {
    if (!track_key.empty()) {
        DominantColor cached;
        if (lookup(track_key, cached)) return cached;
    }
    if (!art || art.width <= 0 || art.height <= 0) {
        return default_dominant_color();
    }

    // Sub-sample on a fixed grid to keep this cheap (<= 64×64 samples).
    const int sx = std::max(1, art.width  / 64);
    const int sy = std::max(1, art.height / 64);

    std::uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
    std::uint64_t count = 0;
    for (int y = 0; y < art.height; y += sy) {
        const std::uint32_t* row = &art.pixels[static_cast<std::size_t>(y) *
                                               static_cast<std::size_t>(art.width)];
        for (int x = 0; x < art.width; x += sx) {
            const std::uint32_t px = row[x];
            const std::uint32_t r = (px >> 16) & 0xFFu;
            const std::uint32_t g = (px >>  8) & 0xFFu;
            const std::uint32_t b = (px      ) & 0xFFu;
            // Skip near-black so pillarboxed art doesn't drag the average to zero.
            if (r + g + b < 24u) continue;
            sum_r += r; sum_g += g; sum_b += b;
            ++count;
        }
    }
    if (count == 0) return default_dominant_color();

    const float avg_r = static_cast<float>(sum_r) / static_cast<float>(count) / 255.0f;
    const float avg_g = static_cast<float>(sum_g) / static_cast<float>(count) / 255.0f;
    const float avg_b = static_cast<float>(sum_b) / static_cast<float>(count) / 255.0f;

    float h = 0, s = 0, v = 0;
    rgb_to_hsv(avg_r, avg_g, avg_b, h, s, v);

    DominantColor dc;
    {
        float r = 0, g = 0, b = 0;
        hsv_to_rgb(h, std::min(s, 0.25f), 0.20f, r, g, b);
        dc.muted_bg = ImVec4(r, g, b, 1.0f);
    }
    {
        float r = 0, g = 0, b = 0;
        hsv_to_rgb(h, std::max(s, 0.55f), 0.65f, r, g, b);
        dc.accent = ImVec4(r, g, b, 1.0f);
    }
    dc.text     = ImVec4(0.98f, 0.98f, 0.988f, 1.0f);
    dc.text_dim = ImVec4(0.62f, 0.62f, 0.660f, 1.0f);

    if (!track_key.empty()) store(track_key, dc);
    return dc;
}

} // namespace transporter::gui
