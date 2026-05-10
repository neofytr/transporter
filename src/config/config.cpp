// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/config/config.hpp>

#include <toml.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace transporter::config {

namespace {

using transporter::engine::Error;
using transporter::engine::ErrorCode;

std::string getenv_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    if (v != nullptr && *v != '\0') {
        return std::string{v};
    }
    return std::string{fallback != nullptr ? fallback : ""};
}

} // namespace

std::filesystem::path default_config_path() {
    const std::string xdg = getenv_or("XDG_CONFIG_HOME", "");
    if (!xdg.empty()) {
        return std::filesystem::path{xdg} / "transporter" / "config.toml";
    }
    const std::string home = getenv_or("HOME", "");
    if (!home.empty()) {
        return std::filesystem::path{home} / ".config" / "transporter" /
               "config.toml";
    }
    return std::filesystem::path{".config/transporter/config.toml"};
}

std::filesystem::path default_library_db_path() {
    const std::string xdg = getenv_or("XDG_DATA_HOME", "");
    if (!xdg.empty()) {
        return std::filesystem::path{xdg} / "transporter" / "library.db";
    }
    const std::string home = getenv_or("HOME", "");
    if (!home.empty()) {
        return std::filesystem::path{home} / ".local" / "share" /
               "transporter" / "library.db";
    }
    return std::filesystem::path{"transporter.library.db"};
}

std::filesystem::path expand_user(std::filesystem::path p) {
    const std::string s = p.string();
    if (s.empty() || s[0] != '~') {
        return p;
    }
    const std::string home = getenv_or("HOME", "");
    if (home.empty()) {
        return p;
    }
    if (s == "~") {
        return std::filesystem::path{home};
    }
    if (s.size() >= 2 && s[1] == '/') {
        return std::filesystem::path{home + s.substr(1)};
    }
    return p;
}

namespace {

RtPolicy parse_rt_policy(std::string_view s) {
    if (s == "fifo") {
        return RtPolicy::Fifo;
    }
    if (s == "other") {
        return RtPolicy::Other;
    }
    return RtPolicy::Auto;
}

DefaultView parse_default_view(std::string_view s) {
    if (s == "library") {
        return DefaultView::Library;
    }
    if (s == "pipeline") {
        return DefaultView::Pipeline;
    }
    return DefaultView::Main;
}

} // namespace

std::expected<Config, Error> parse(std::string_view toml_text) {
    Config cfg;
    toml::table tbl;
    try {
        tbl = toml::parse(toml_text);
    } catch (const toml::parse_error& err) {
        std::ostringstream os;
        os << "config TOML parse error: " << err.description();
        return std::unexpected(Error{ErrorCode::InvalidArgument, os.str()});
    }

    if (auto* dev = tbl["device"].as_table()) {
        if (auto v = (*dev)["preferred"].value<std::string>()) {
            cfg.device.preferred = *v;
        }
    }

    if (auto* aud = tbl["audio"].as_table()) {
        if (auto v = (*aud)["period_ms"].value<std::int64_t>()) {
            if (*v > 0) {
                cfg.audio.period_ms = static_cast<std::uint32_t>(*v);
            }
        }
        if (auto v = (*aud)["period_count"].value<std::int64_t>()) {
            if (*v > 0) {
                cfg.audio.period_count = static_cast<std::uint32_t>(*v);
            }
        }
        if (auto v = (*aud)["rt_policy"].value<std::string>()) {
            cfg.audio.rt_policy = parse_rt_policy(*v);
        }
    }

    if (auto* lib = tbl["library"].as_table()) {
        if (auto* roots = (*lib)["roots"].as_array()) {
            for (auto& node : *roots) {
                if (auto s = node.value<std::string>()) {
                    cfg.library.roots.emplace_back(expand_user(*s));
                }
            }
        }
        if (auto* ig = (*lib)["ignore_patterns"].as_array()) {
            for (auto& node : *ig) {
                if (auto s = node.value<std::string>()) {
                    cfg.library.ignore_patterns.emplace_back(*s);
                }
            }
        }
    }

    if (auto* th = tbl["theme"].as_table()) {
        if (auto v = (*th)["follow_hyprland"].value<bool>()) {
            cfg.theme.follow_hyprland = *v;
        }
        if (auto v = (*th)["override_file"].value<std::string>()) {
            cfg.theme.override_file = expand_user(*v);
        }
    }

    if (auto* ui = tbl["ui"].as_table()) {
        if (auto v = (*ui)["default_view"].value<std::string>()) {
            cfg.ui.default_view = parse_default_view(*v);
        }
    }

    if (auto* db = tbl["dbus"].as_table()) {
        if (auto v = (*db)["enabled"].value<bool>()) {
            cfg.dbus.enabled = *v;
        }
    }

    return cfg;
}

std::expected<Config, Error> load_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
        return std::unexpected(Error{ErrorCode::NotFound,
                                     "config file not found: " + path.string()});
    }
    std::ostringstream os;
    os << f.rdbuf();
    return parse(os.str());
}

void save_device_preferred(const std::filesystem::path& path,
                           const std::string& hw_string) {
    toml::table tbl;
    {
        std::ifstream f(path);
        if (f) {
            try {
                std::ostringstream os;
                os << f.rdbuf();
                tbl = toml::parse(os.str());
            } catch (...) {
                // corrupt file; start fresh
            }
        }
    }
    if (!tbl.contains("device")) {
        tbl.insert("device", toml::table{});
    }
    tbl["device"].as_table()->insert_or_assign("preferred", hw_string);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << tbl;
    }
}

} // namespace transporter::config
