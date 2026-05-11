// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpris_metadata.hpp"

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/telemetry.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace transporter::dbus_svc {

namespace {

// Probe common cover-art filenames in the directory of an audio file.
// Returns a file:// URI on the first hit, or empty string.
std::string find_art_uri(const std::string& file_path) {
    if (file_path.empty()) return {};
    const std::filesystem::path dir{file_path};
    static constexpr std::array kNames = {
        "cover.jpg", "cover.png", "folder.jpg", "folder.png",
        "front.jpg", "front.png", "album.jpg",  "album.png",
    };
    for (const char* name : kNames) {
        std::error_code ec;
        const auto p = dir.parent_path() / name;
        if (std::filesystem::exists(p, ec) && !ec)
            return "file://" + p.string();
    }
    return {};
}

std::string to_file_uri(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::filesystem::path p{path};
    if (!p.is_absolute()) {
        std::error_code ec;
        auto abs = std::filesystem::absolute(p, ec);
        if (!ec) {
            p = abs;
        }
    }
    // Minimal RFC 3986 percent-encoding: encode characters outside the
    // "unreserved" + "/" set. This avoids pulling in a URI library.
    static constexpr std::string_view kSafe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/";
    std::string s = p.string();
    std::string out;
    out.reserve(s.size() + 8);
    out.append("file://");
    for (char raw : s) {
        const auto c = static_cast<unsigned char>(raw);
        if (kSafe.find(raw) != std::string_view::npos) {
            out.push_back(raw);
        } else {
            constexpr char hex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::int32_t parse_track_no(const std::string& s) {
    // "n" or "n/m". Take the leading integer; ignore the rest.
    if (s.empty()) {
        return 0;
    }
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    std::size_t j = i;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
        ++j;
    }
    if (j == i) {
        return 0;
    }
    std::int32_t n = 0;
    auto [ptr, ec] = std::from_chars(s.data() + i, s.data() + j, n);
    (void)ptr;
    if (ec != std::errc{}) {
        return 0;
    }
    return n;
}

} // namespace

std::map<std::string, sdbus::Variant>
mpris_metadata_from_parts(const std::string& file_path,
                          const transporter::engine::Tags& tags,
                          const transporter::engine::PcmFormat& format,
                          std::uint64_t total_frames,
                          const std::string& trackid_path) {
    std::map<std::string, sdbus::Variant> out;

    out.emplace("mpris:trackid",
                sdbus::Variant{sdbus::ObjectPath{trackid_path}});

    if (total_frames > 0 && format.sample_rate_hz > 0) {
        const std::int64_t length_us = static_cast<std::int64_t>(
            (total_frames * 1'000'000ULL) / format.sample_rate_hz);
        out.emplace("mpris:length", sdbus::Variant{length_us});
    }

    if (!tags.title.empty()) {
        out.emplace("xesam:title", sdbus::Variant{tags.title});
    }
    if (!tags.artist.empty()) {
        std::vector<std::string> arr{tags.artist};
        out.emplace("xesam:artist", sdbus::Variant{arr});
    }
    if (!tags.album.empty()) {
        out.emplace("xesam:album", sdbus::Variant{tags.album});
    }
    if (auto n = parse_track_no(tags.track_no); n > 0) {
        out.emplace("xesam:trackNumber", sdbus::Variant{n});
    }
    if (!file_path.empty()) {
        out.emplace("xesam:url", sdbus::Variant{to_file_uri(file_path)});
    }
    if (auto art = find_art_uri(file_path); !art.empty()) {
        out.emplace("mpris:artUrl", sdbus::Variant{art});
    }
    if (!tags.date.empty()) {
        out.emplace("xesam:contentCreated", sdbus::Variant{tags.date});
    }

    return out;
}

std::map<std::string, sdbus::Variant>
mpris_metadata_from_snapshot(const transporter::engine::PipelineSnapshot& snap,
                             const std::string& trackid_path) {
    return mpris_metadata_from_parts(
        snap.source.file_path,
        snap.source.tags,
        transporter::engine::PcmFormat{
            snap.source.sample_rate_hz,
            snap.source.channels,
            transporter::engine::SampleFormat::S16_LE  // unused for length
        },
        snap.source.total_frames,
        trackid_path);
}

} // namespace transporter::dbus_svc
