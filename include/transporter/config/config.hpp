// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_CONFIG_CONFIG_HPP
#define TRANSPORTER_CONFIG_CONFIG_HPP

#include <transporter/engine/error.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace transporter::config {

enum class RtPolicy : std::uint8_t { Auto, Fifo, Other };

enum class DefaultView : std::uint8_t { Main, Library, Pipeline };

struct DeviceSection {
    std::string preferred;            // "hw:CARD=...,DEV=N" or empty
};

struct AudioSection {
    std::uint32_t period_ms = 12;
    std::uint32_t period_count = 4;
    RtPolicy rt_policy = RtPolicy::Auto;
};

struct LibrarySection {
    std::vector<std::filesystem::path> roots;
    std::vector<std::string> ignore_patterns;
};

struct ThemeSection {
    bool follow_hyprland = true;
    std::filesystem::path override_file;
};

struct UiSection {
    DefaultView default_view = DefaultView::Main;
};

struct Config {
    DeviceSection device;
    AudioSection audio;
    LibrarySection library;
    ThemeSection theme;
    UiSection ui;
};

// Default location: $XDG_CONFIG_HOME/transporter/config.toml, falling back to
// ~/.config/transporter/config.toml. Returned path may not exist.
std::filesystem::path default_config_path();

// Default DB location for the library: $XDG_DATA_HOME/transporter/library.db,
// falling back to ~/.local/share/transporter/library.db.
std::filesystem::path default_library_db_path();

// Expand a leading "~" / "~/" in a path to $HOME. No effect if HOME is unset
// or the input does not start with "~".
std::filesystem::path expand_user(std::filesystem::path p);

// Parse TOML text into a Config. Missing keys take their default values.
// Returns InvalidArgument on parse error or wrong-typed key.
std::expected<Config, transporter::engine::Error>
parse(std::string_view toml_text);

// Convenience: read the file at `path` and parse. NotFound when the file
// does not exist (caller decides whether that is fatal).
std::expected<Config, transporter::engine::Error>
load_file(const std::filesystem::path& path);

} // namespace transporter::config

#endif
