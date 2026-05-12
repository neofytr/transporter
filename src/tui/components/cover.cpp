// SPDX-License-Identifier: GPL-3.0-or-later
//
// Album-art loading via libpng and libjpeg. Decoded straight into an RGBA
// pixel buffer for ncvisual_from_rgba(); no intermediate ImageIO layer.

#include "cover.hpp"

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>  // libjpeg / libpng error recovery

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace transporter::tui::components {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ext_lower(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') {
        e.erase(e.begin());
    }
    return to_lower(std::move(e));
}

// Candidate filenames in priority order. Matched case-insensitively;
// `is_prefix` enables the AlbumArt*.png convention (basename starts-with).
struct Candidate {
    const char* prefix;     // lowercase basename (or basename prefix)
    const char* ext;        // lowercase extension without dot
    bool        is_prefix;
};

constexpr Candidate kCandidates[] = {
    {"cover",     "jpg",  false}, {"cover",     "jpeg", false}, {"cover",    "png",  false},
    {"folder",    "jpg",  false}, {"folder",    "jpeg", false}, {"folder",   "png",  false},
    {"front",     "jpg",  false}, {"front",     "jpeg", false}, {"front",    "png",  false},
    {"albumart",  "jpg",  true},  {"albumart",  "jpeg", true},  {"albumart", "png",  true},
};

// Returns the path of the first matching art file in `dir`, or empty.
std::filesystem::path find_art_file(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return {};
    }

    struct Entry {
        std::string basename_lower;
        std::string ext_lower;
        fs::path    full;
    };
    std::vector<Entry> entries;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) {
            continue;
        }
        Entry x;
        x.full = e.path();
        x.basename_lower = to_lower(e.path().stem().string());
        x.ext_lower      = ext_lower(e.path());
        entries.push_back(std::move(x));
    }

    for (const Candidate& cand : kCandidates) {
        for (const Entry& e : entries) {
            if (e.ext_lower != cand.ext) {
                continue;
            }
            if (cand.is_prefix) {
                if (e.basename_lower.rfind(cand.prefix, 0) == 0) {
                    return e.full;
                }
            } else {
                if (e.basename_lower == cand.prefix) {
                    return e.full;
                }
            }
        }
    }
    return {};
}

// ── PNG ──────────────────────────────────────────────────────────────────────

Cover load_png(const std::filesystem::path& path) {
    FILE* f = std::fopen(path.c_str(), "rb");  // NOLINT(cppcoreguidelines-owning-memory)
    if (f == nullptr) {
        return {};
    }

    unsigned char sig[8];
    if (std::fread(sig, 1, 8, f) != 8 || (png_check_sig(sig, 8) == 0)) {
        std::fclose(f);  // NOLINT
        return {};
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                             nullptr, nullptr);
    if (png == nullptr) {
        std::fclose(f);  // NOLINT
        return {};
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(f);  // NOLINT
        return {};
    }

    Cover cov;

    if (setjmp(png_jmpbuf(png))) {  // NOLINT(cert-err52-cpp)
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(f);  // NOLINT
        return {};
    }

    png_init_io(png, f);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    const int w = static_cast<int>(png_get_image_width(png, info));
    const int h = static_cast<int>(png_get_image_height(png, info));
    png_byte ct = png_get_color_type(png, info);
    png_byte bd = png_get_bit_depth(png, info);

    // Normalise to 8-bit RGBA. Each transform may change `ct` / `bd`; we set
    // them up front and call png_read_update_info() once at the end.
    if (bd == 16) {
        png_set_strip_16(png);
    }
    if (ct == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) {
        png_set_tRNS_to_alpha(png);
    }
    if (ct == PNG_COLOR_TYPE_RGB || ct == PNG_COLOR_TYPE_GRAY ||
        ct == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_read_update_info(png, info);

    auto buf = std::make_unique<std::uint32_t[]>(
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::vector<png_bytep> rows(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        rows[static_cast<std::size_t>(y)] =
            reinterpret_cast<png_bytep>(&buf[static_cast<std::size_t>(y) *
                                             static_cast<std::size_t>(w)]);
    }
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(f);  // NOLINT

    // libpng's filler arrangement gives R G B A in memory order on little-
    // endian, which is exactly what ncvisual_from_rgba expects. No swizzle.
    cov.pixels = std::move(buf);
    cov.width  = w;
    cov.height = h;
    return cov;
}

// ── JPEG ─────────────────────────────────────────────────────────────────────

struct JpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf               jmpbuf;
};

void jpeg_error_exit(j_common_ptr cinfo) {  // NOLINT
    JpegErrorMgr* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    longjmp(err->jmpbuf, 1);  // NOLINT(cert-err52-cpp)
}

Cover load_jpeg(const std::filesystem::path& path) {
    FILE* f = std::fopen(path.c_str(), "rb");  // NOLINT
    if (f == nullptr) {
        return {};
    }

    struct jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    Cover cov;

    if (setjmp(jerr.jmpbuf)) {  // NOLINT(cert-err52-cpp)
        jpeg_destroy_decompress(&cinfo);
        std::fclose(f);  // NOLINT
        return {};
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const int w = static_cast<int>(cinfo.output_width);
    const int h = static_cast<int>(cinfo.output_height);
    const int comps = static_cast<int>(cinfo.output_components);  // 3 for JCS_RGB

    auto buf = std::make_unique<std::uint32_t[]>(
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::vector<std::uint8_t> row(static_cast<std::size_t>(w) *
                                  static_cast<std::size_t>(comps));
    JSAMPROW rowptr[1] = { row.data() };

    for (int y = 0; y < h; ++y) {
        jpeg_read_scanlines(&cinfo, rowptr, 1);
        for (int x = 0; x < w; ++x) {
            const std::uint8_t r = row[static_cast<std::size_t>(x * comps + 0)];
            const std::uint8_t g = row[static_cast<std::size_t>(x * comps + 1)];
            const std::uint8_t b = row[static_cast<std::size_t>(x * comps + 2)];
            // Pack as R G B A in memory order (little-endian uint32_t).
            buf[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)
                + static_cast<std::size_t>(x)] =
                static_cast<std::uint32_t>(r)
                | (static_cast<std::uint32_t>(g) << 8)
                | (static_cast<std::uint32_t>(b) << 16)
                | (0xFFu << 24);
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(f);  // NOLINT

    cov.pixels = std::move(buf);
    cov.width  = w;
    cov.height = h;
    return cov;
}

// Sniff the first bytes of `bytes` to classify the format. Returns one of
// "png", "jpg", or "" (unknown).
const char* sniff_format(const std::vector<unsigned char>& bytes) {
    if (bytes.size() < 4) {
        return "";
    }
    static const unsigned char kPng[] = {0x89, 0x50, 0x4E, 0x47};
    if (std::memcmp(bytes.data(), kPng, 4) == 0) {
        return "png";
    }
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return "jpg";
    }
    return "";
}

} // namespace

Cover decode_image(const std::filesystem::path& path) {
    const std::string ext = ext_lower(path);
    if (ext == "png") {
        return load_png(path);
    }
    if (ext == "jpg" || ext == "jpeg") {
        return load_jpeg(path);
    }
    return {};
}

Cover decode_embedded(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    const std::string kind = sniff_format(bytes);
    if (kind.empty()) {
        return {};
    }
    // libjpeg / libpng both take a FILE*; round-trip via a real temp path
    // rather than wiring up the memory-source API for this cold path.
    namespace fs = std::filesystem;
    fs::path tmpdir = fs::temp_directory_path();
    fs::path tmppath = tmpdir / ("transporter-embed." + kind);
    FILE* out = std::fopen(tmppath.c_str(), "wb");  // NOLINT
    if (out == nullptr) {
        return {};
    }
    std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);  // NOLINT

    Cover c = decode_image(tmppath);
    std::error_code ec;
    fs::remove(tmppath, ec);
    return c;
}

Cover load_cover_for_track(const std::filesystem::path& track_path) {
    // Placeholder for embedded picture extraction: when decoders surface
    // PICTURE blocks via Tags, sniff + decode_embedded() here and return on
    // success. Until then, fall through to the sidecar lookup.

    if (track_path.empty()) {
        return {};
    }
    std::filesystem::path dir = track_path;
    if (dir.has_filename()) {
        dir = dir.parent_path();
    }
    const std::filesystem::path art = find_art_file(dir);
    if (art.empty()) {
        return {};
    }
    return decode_image(art);
}

} // namespace transporter::tui::components
