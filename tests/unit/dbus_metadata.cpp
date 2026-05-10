// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit test for MPRIS metadata variant-map construction. No live bus.

#include "../../src/dbus/mpris_metadata.hpp"

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/format.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

#define REQUIRE(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #cond);                                               \
            return 1;                                                          \
        }                                                                     \
    } while (0)

int test_full_tags() {
    transporter::engine::Tags tags{
        .artist = "Miles Davis",
        .album = "Kind of Blue",
        .title = "So What",
        .track_no = "1/5",
        .date = "1959-08-17",
    };
    transporter::engine::PcmFormat fmt{
        .sample_rate_hz = 44100,
        .channels = 2,
        .sample_format = transporter::engine::SampleFormat::S16_LE,
    };
    const std::uint64_t total_frames = 44100ULL * 540;  // 9 minutes
    const std::string trackid =
        "/org/mpris/MediaPlayer2/transporter/track/3";

    auto m = transporter::dbus_svc::mpris_metadata_from_parts(
        "/home/u/Music/sowhat.flac", tags, fmt, total_frames, trackid);

    REQUIRE(m.contains("mpris:trackid"));
    REQUIRE(m.contains("mpris:length"));
    REQUIRE(m.contains("xesam:title"));
    REQUIRE(m.contains("xesam:artist"));
    REQUIRE(m.contains("xesam:album"));
    REQUIRE(m.contains("xesam:trackNumber"));
    REQUIRE(m.contains("xesam:url"));
    REQUIRE(m.contains("xesam:contentCreated"));

    REQUIRE(m.at("mpris:trackid").get<sdbus::ObjectPath>() ==
            sdbus::ObjectPath{trackid});

    const auto length = m.at("mpris:length").get<std::int64_t>();
    REQUIRE(length == 540LL * 1'000'000LL);

    REQUIRE(m.at("xesam:title").get<std::string>() == "So What");
    const auto artists =
        m.at("xesam:artist").get<std::vector<std::string>>();
    REQUIRE(artists.size() == 1 && artists[0] == "Miles Davis");
    REQUIRE(m.at("xesam:album").get<std::string>() == "Kind of Blue");
    REQUIRE(m.at("xesam:trackNumber").get<std::int32_t>() == 1);

    const auto url = m.at("xesam:url").get<std::string>();
    REQUIRE(url == "file:///home/u/Music/sowhat.flac");

    REQUIRE(m.at("xesam:contentCreated").get<std::string>() == "1959-08-17");

    return 0;
}

int test_missing_tags() {
    transporter::engine::Tags tags{};
    transporter::engine::PcmFormat fmt{
        .sample_rate_hz = 0,
        .channels = 0,
        .sample_format = transporter::engine::SampleFormat::S16_LE,
    };
    auto m = transporter::dbus_svc::mpris_metadata_from_parts(
        "", tags, fmt, 0, "/org/mpris/MediaPlayer2/transporter/track/0");

    // Always present: trackid. Everything else absent.
    REQUIRE(m.contains("mpris:trackid"));
    REQUIRE(!m.contains("mpris:length"));
    REQUIRE(!m.contains("xesam:title"));
    REQUIRE(!m.contains("xesam:artist"));
    REQUIRE(!m.contains("xesam:url"));
    return 0;
}

int test_track_no_parsing() {
    transporter::engine::Tags tags{};
    tags.title = "x";
    tags.track_no = "12";
    transporter::engine::PcmFormat fmt{
        .sample_rate_hz = 48000,
        .channels = 2,
        .sample_format = transporter::engine::SampleFormat::S24_LE,
    };
    auto m = transporter::dbus_svc::mpris_metadata_from_parts(
        "/x.flac", tags, fmt, 48000, "/org/mpris/MediaPlayer2/transporter/track/1");

    REQUIRE(m.at("xesam:trackNumber").get<std::int32_t>() == 12);

    tags.track_no = "9/15";
    m = transporter::dbus_svc::mpris_metadata_from_parts(
        "/x.flac", tags, fmt, 48000, "/org/mpris/MediaPlayer2/transporter/track/2");
    REQUIRE(m.at("xesam:trackNumber").get<std::int32_t>() == 9);

    tags.track_no = "  ";
    m = transporter::dbus_svc::mpris_metadata_from_parts(
        "/x.flac", tags, fmt, 48000, "/org/mpris/MediaPlayer2/transporter/track/3");
    REQUIRE(!m.contains("xesam:trackNumber"));
    return 0;
}

int test_url_encoding() {
    transporter::engine::Tags tags{};
    tags.title = "x";
    transporter::engine::PcmFormat fmt{
        .sample_rate_hz = 44100,
        .channels = 2,
        .sample_format = transporter::engine::SampleFormat::S16_LE,
    };
    auto m = transporter::dbus_svc::mpris_metadata_from_parts(
        "/home/u/Music/some track.flac", tags, fmt, 44100,
        "/org/mpris/MediaPlayer2/transporter/track/0");
    const auto url = m.at("xesam:url").get<std::string>();
    REQUIRE(url == "file:///home/u/Music/some%20track.flac");
    return 0;
}

} // namespace

int main() {
    if (int rc = test_full_tags(); rc != 0) {
        return rc;
    }
    if (int rc = test_missing_tags(); rc != 0) {
        return rc;
    }
    if (int rc = test_track_no_parsing(); rc != 0) {
        return rc;
    }
    if (int rc = test_url_encoding(); rc != 0) {
        return rc;
    }
    std::puts("ok");
    return 0;
}
