// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal album art loader. Decodes JPEG/PNG from the track directory into
// a heap-allocated XRGB8888 pixel buffer and registers it as a custom
// ImTextureID. The renderer handles any TextureID that is not the font atlas
// as a direct pixel-blit.

#ifndef TRANSPORTER_GUI_VIEWS_ALBUMART_HPP
#define TRANSPORTER_GUI_VIEWS_ALBUMART_HPP

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace transporter::gui {

// AlbumArt holds a decoded XRGB8888 pixel buffer and presents itself as an
// ImTextureID by returning a pointer to its own instance. The renderer casts
// the ID back to AlbumArt* and reads pixels/width/height from it.
struct AlbumArt {
    std::unique_ptr<uint32_t[]> pixels;  // XRGB8888, row-major, width*height
    int width  = 0;
    int height = 0;

    // Returns ImTextureID pointing to this object, or ImTextureID{} on failure.
    // Caller must keep this object alive while the ID is in use.
    ImTextureID texture_id() const {
        if (!pixels) return ImTextureID{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        return reinterpret_cast<ImTextureID>(const_cast<AlbumArt*>(this));
    }

    explicit operator bool() const { return pixels != nullptr; }
};

// Probe the directory of `track_path` for standard cover art filenames.
// Returns a decoded AlbumArt (bool == true) on success, or an empty one.
// Decoded at full resolution; caller should scale at display time via UV.
// Thread-safe: reads only, no shared state.
AlbumArt load_album_art(const std::filesystem::path& track_dir);

} // namespace transporter::gui

#endif
