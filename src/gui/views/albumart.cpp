// SPDX-License-Identifier: GPL-3.0-or-later
//
// Album art loading via libpng and libjpeg.
// Probes a directory for cover art filenames, decodes to XRGB8888.

#include "albumart.hpp"

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>  // libjpeg error recovery

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace transporter::gui {

namespace {

// Candidate filenames in priority order (lowercase; matched case-insensitively).
static constexpr const char* kCandidates[] = {
    "cover.jpg", "cover.png",
    "folder.jpg", "folder.png",
    "front.jpg", "front.png",
    "artwork.jpg", "artwork.png",
};

// Returns the path of the first matching art file in dir, or empty.
std::filesystem::path find_art_file(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};

    // Build lowercase-keyed map of what's present.
    std::vector<std::pair<std::string, fs::path>> entries;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) {
            std::string lower = e.path().filename().string();
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            entries.emplace_back(std::move(lower), e.path());
        }
    }

    for (const char* cand : kCandidates) {
        for (const auto& [lower, full] : entries) {
            if (lower == cand) return full;
        }
    }
    return {};
}

// Returns the file extension in lowercase, without the dot.
std::string ext_lower(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(e.begin());
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return e;
}

// ── PNG ──────────────────────────────────────────────────────────────────────

AlbumArt load_png(const std::filesystem::path& path) {
    FILE* f = fopen(path.c_str(), "rb");  // NOLINT(cppcoreguidelines-owning-memory)
    if (!f) return {};

    // Check signature.
    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8 || !png_check_sig(sig, 8)) {
        fclose(f);  // NOLINT
        return {};
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                             nullptr, nullptr);
    if (!png) { fclose(f); return {}; }  // NOLINT

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(f);  // NOLINT
        return {};
    }

    AlbumArt art;

    if (setjmp(png_jmpbuf(png))) {  // NOLINT(cert-err52-cpp)
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(f);  // NOLINT
        return {};
    }

    png_init_io(png, f);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    int w = static_cast<int>(png_get_image_width(png, info));
    int h = static_cast<int>(png_get_image_height(png, info));
    png_byte ct = png_get_color_type(png, info);
    png_byte bd = png_get_bit_depth(png, info);

    // Normalise to 8-bit RGBA.
    if (bd == 16) png_set_strip_16(png);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ct == PNG_COLOR_TYPE_RGB || ct == PNG_COLOR_TYPE_GRAY ||
        ct == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_read_update_info(png, info);

    const int stride = w * 4;
    auto buf = std::make_unique<uint32_t[]>(static_cast<std::size_t>(w * h));
    std::vector<png_bytep> rows(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        rows[static_cast<std::size_t>(y)] =
            reinterpret_cast<png_bytep>(&buf[static_cast<std::size_t>(y * w)]);
    }
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(f);  // NOLINT

    // Convert RGBA → XRGB8888 (renderer byte order: B G R X in LE).
    for (int i = 0; i < w * h; ++i) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&buf[static_cast<std::size_t>(i)]);
        const uint8_t r = p[0], g = p[1], b = p[2]; // p[3] = alpha, discard
        buf[static_cast<std::size_t>(i)] =
            (0xFFu << 24) |
            (static_cast<uint32_t>(r) << 16) |
            (static_cast<uint32_t>(g) <<  8) |
            b;
    }

    // Sanity: suppress the stride variable if it's unused in release builds.
    (void)stride;
    art.pixels = std::move(buf);
    art.width  = w;
    art.height = h;
    return art;
}

// ── JPEG ─────────────────────────────────────────────────────────────────────

struct JpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf               jmpbuf;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    JpegErrorMgr* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    longjmp(err->jmpbuf, 1);  // NOLINT(cert-err52-cpp)
}

AlbumArt load_jpeg(const std::filesystem::path& path) {
    FILE* f = fopen(path.c_str(), "rb");  // NOLINT
    if (!f) return {};

    struct jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    AlbumArt art;

    if (setjmp(jerr.jmpbuf)) {  // NOLINT(cert-err52-cpp)
        jpeg_destroy_decompress(&cinfo);
        fclose(f);  // NOLINT
        return {};
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const int w = static_cast<int>(cinfo.output_width);
    const int h = static_cast<int>(cinfo.output_height);
    const int comps = static_cast<int>(cinfo.output_components);  // should be 3

    auto buf = std::make_unique<uint32_t[]>(static_cast<std::size_t>(w * h));
    std::vector<uint8_t> row(static_cast<std::size_t>(w * comps));
    JSAMPROW rowptr[1] = { row.data() };

    for (int y = 0; y < h; ++y) {
        jpeg_read_scanlines(&cinfo, rowptr, 1);
        for (int x = 0; x < w; ++x) {
            const uint8_t r = row[static_cast<std::size_t>(x * comps + 0)];
            const uint8_t g = row[static_cast<std::size_t>(x * comps + 1)];
            const uint8_t b = row[static_cast<std::size_t>(x * comps + 2)];
            buf[static_cast<std::size_t>(y * w + x)] =
                (0xFFu << 24) |
                (static_cast<uint32_t>(r) << 16) |
                (static_cast<uint32_t>(g) <<  8) |
                b;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);  // NOLINT

    art.pixels = std::move(buf);
    art.width  = w;
    art.height = h;
    return art;
}

} // namespace

AlbumArt load_album_art(const std::filesystem::path& track_dir) {
    const std::filesystem::path art_path = find_art_file(track_dir);
    if (art_path.empty()) return {};

    const std::string ext = ext_lower(art_path);
    if (ext == "png") return load_png(art_path);
    if (ext == "jpg" || ext == "jpeg") return load_jpeg(art_path);
    return {};
}

} // namespace transporter::gui
