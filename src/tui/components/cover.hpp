// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_COMPONENTS_COVER_HPP
#define TRANSPORTER_TUI_COMPONENTS_COVER_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace transporter::tui::components {

// Decoded RGBA pixel buffer. R in the low byte (memory order R, G, B, A).
// Row-major, width*height pixels. Decoded once per file; reused for blits.
struct Cover {
    std::unique_ptr<std::uint32_t[]> pixels;
    int width  = 0;
    int height = 0;
    explicit operator bool() const { return pixels && width > 0 && height > 0; }
};

// Decode the file at `path` (JPEG or PNG). Returns an empty Cover on failure
// (unknown extension, decoder error, missing file, etc.).
Cover decode_image(const std::filesystem::path& path);

// Decode an embedded picture from a raw byte buffer (e.g. an APIC frame, a
// FLAC PICTURE block payload). Returns empty Cover on failure.
Cover decode_embedded(const std::vector<unsigned char>& bytes);

} // namespace transporter::tui::components

#endif
