// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke-tests for the cover decoder + LRU cache. The decoder tests
// synthesise a tiny 4x4 PNG at runtime so we don't carry a binary fixture
// in the source tree; the sidecar-lookup tests use a fresh tempdir each
// run.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../../src/tui/components/cover.hpp"
#include "../../src/tui/components/cover_cache.hpp"

#include <png.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs  = std::filesystem;
namespace tc  = transporter::tui::components;

namespace {

// Write a 4×4 RGB PNG to `path`. Pixel pattern is a deterministic gradient.
// Returns true on success.
bool write_tiny_png(const fs::path& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              nullptr, nullptr, nullptr);
    if (png == nullptr) {
        std::fclose(f);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(f);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(f);
        return false;
    }
    png_init_io(png, f);
    const png_uint_32 w = 4;
    const png_uint_32 h = 4;
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::array<std::array<png_byte, 4 * 3>, 4> rows{};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x * 3 + 0)] =
                static_cast<png_byte>(x * 64);
            rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x * 3 + 1)] =
                static_cast<png_byte>(y * 64);
            rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x * 3 + 2)] =
                static_cast<png_byte>(128);
        }
    }
    std::array<png_bytep, 4> rowptrs{};
    for (int y = 0; y < 4; ++y) {
        rowptrs[static_cast<std::size_t>(y)] = rows[static_cast<std::size_t>(y)].data();
    }
    png_write_image(png, rowptrs.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(f);
    return true;
}

// Create a unique tempdir under the system temp root.
fs::path make_tempdir() {
    std::random_device rd;
    std::mt19937 rng(rd());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "transporter-cover-%08lx",
                  static_cast<unsigned long>(rng()));
    fs::path p = fs::temp_directory_path() / buf;
    fs::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("decode_image: tiny PNG decodes to a 4x4 buffer") {
    const fs::path dir = make_tempdir();
    const fs::path png = dir / "test.png";
    REQUIRE(write_tiny_png(png));

    auto cov = tc::decode_image(png);
    REQUIRE(static_cast<bool>(cov));
    CHECK(cov.width == 4);
    CHECK(cov.height == 4);
    REQUIRE(cov.pixels);
    // Spot-check pixel (1,1) — green channel should be y*64 = 64.
    const std::uint32_t p = cov.pixels[1 * 4 + 1];
    const std::uint8_t g = static_cast<std::uint8_t>((p >> 8) & 0xFF);
    CHECK(g == 64);

    fs::remove_all(dir);
}

TEST_CASE("decode_image: unknown extension yields empty Cover") {
    const fs::path dir = make_tempdir();
    const fs::path bogus = dir / "image.bmp";
    std::ofstream(bogus) << "not a real image";
    auto cov = tc::decode_image(bogus);
    CHECK_FALSE(static_cast<bool>(cov));
    fs::remove_all(dir);
}

TEST_CASE("load_cover_for_track: returns populated Cover when cover.png present") {
    const fs::path dir = make_tempdir();
    const fs::path track = dir / "track1.flac";
    std::ofstream(track) << "ignored — only the dir matters for sidecar lookup";
    const fs::path cover = dir / "cover.png";
    REQUIRE(write_tiny_png(cover));

    auto cov = tc::load_cover_for_track(track);
    REQUIRE(static_cast<bool>(cov));
    CHECK(cov.width  == 4);
    CHECK(cov.height == 4);

    fs::remove_all(dir);
}

TEST_CASE("load_cover_for_track: case-insensitive on filename") {
    const fs::path dir = make_tempdir();
    const fs::path track = dir / "track.flac";
    std::ofstream(track) << "x";
    const fs::path cover = dir / "Cover.PNG";  // mixed case
    REQUIRE(write_tiny_png(cover));

    auto cov = tc::load_cover_for_track(track);
    CHECK(static_cast<bool>(cov));
    fs::remove_all(dir);
}

TEST_CASE("load_cover_for_track: returns empty Cover when no art present") {
    const fs::path dir = make_tempdir();
    const fs::path track = dir / "track.flac";
    std::ofstream(track) << "x";

    auto cov = tc::load_cover_for_track(track);
    CHECK_FALSE(static_cast<bool>(cov));
    fs::remove_all(dir);
}

TEST_CASE("CoverCache: evicts least-recently-used on overflow") {
    tc::CoverCache cache(/*capacity=*/50);
    // Helper to fabricate a Cover (1x1 white pixel) without touching disk.
    auto make_cover = []() {
        tc::Cover c;
        c.width = 1;
        c.height = 1;
        c.pixels = std::make_unique<std::uint32_t[]>(1);
        c.pixels[0] = 0xFFFFFFFFu;
        return c;
    };

    // Insert 51 unique keys; the first inserted must be evicted.
    for (int i = 0; i < 51; ++i) {
        cache.put(fs::path("/fake/track_" + std::to_string(i) + ".flac"),
                  make_cover());
    }
    CHECK(cache.size() == 50);
    CHECK(cache.get(fs::path("/fake/track_0.flac")) == nullptr);
    CHECK(cache.get(fs::path("/fake/track_50.flac")) != nullptr);
}

TEST_CASE("CoverCache: get() bumps the entry to MRU") {
    tc::CoverCache cache(3);
    auto make_cover = []() {
        tc::Cover c;
        c.width = 1;
        c.height = 1;
        c.pixels = std::make_unique<std::uint32_t[]>(1);
        c.pixels[0] = 0u;
        return c;
    };
    cache.put(fs::path("A"), make_cover());
    cache.put(fs::path("B"), make_cover());
    cache.put(fs::path("C"), make_cover());
    // Touch A so it's MRU; B becomes LRU.
    CHECK(cache.get(fs::path("A")) != nullptr);
    cache.put(fs::path("D"), make_cover());
    // B should be evicted, A/C/D survive.
    CHECK(cache.get(fs::path("B")) == nullptr);
    CHECK(cache.get(fs::path("A")) != nullptr);
    CHECK(cache.get(fs::path("C")) != nullptr);
    CHECK(cache.get(fs::path("D")) != nullptr);
}
