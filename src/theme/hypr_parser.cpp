// SPDX-License-Identifier: GPL-3.0-or-later

#include "hypr_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace transporter::theme::detail {

namespace {

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Expand leading ~ to $HOME.
std::string expand_home(const std::string& p) {
    if (!p.empty() && p[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home != nullptr) {
            return std::string(home) + p.substr(1);
        }
    }
    return p;
}

// Substitute $VAR references in val using vars map.
std::string subst_vars(const std::string& val,
                       const std::unordered_map<std::string, std::string>& vars) {
    std::string result;
    result.reserve(val.size());
    std::size_t i = 0;
    while (i < val.size()) {
        if (val[i] == '$') {
            std::size_t j = i + 1;
            while (j < val.size() && (std::isalnum(static_cast<unsigned char>(val[j])) || val[j] == '_')) {
                ++j;
            }
            const std::string name = val.substr(i + 1, j - i - 1);
            auto it = vars.find(name);
            if (it != vars.end()) {
                result += it->second;
            } else {
                result += val.substr(i, j - i);
            }
            i = j;
        } else {
            result += val[i++];
        }
    }
    return result;
}

void parse_file(const std::filesystem::path& path,
                std::unordered_map<std::string, std::string>& out,
                std::unordered_map<std::string, std::string>& vars,
                std::vector<std::string>& section_stack,
                int depth) {
    if (depth > 8) return;

    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        // Strip # comments (not inside values, but hyprland uses # for comments).
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) continue;

        // Section open: "word {" or "word subword {" etc.
        if (line.back() == '{') {
            // Extract section name (everything before the {).
            std::string sec = trim(line.substr(0, line.size() - 1));
            // Section names can be "general" or "general:col" style; treat as-is.
            section_stack.push_back(sec);
            continue;
        }

        // Section close.
        if (line == "}") {
            if (!section_stack.empty()) {
                section_stack.pop_back();
            }
            continue;
        }

        // key = value
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        value = subst_vars(value, vars);

        // $VAR declaration — store in vars, not in out.
        if (!key.empty() && key[0] == '$') {
            vars[key.substr(1)] = value;
            continue;
        }

        // source = path — recurse.
        if (key == "source") {
            const std::string src = expand_home(value);
            parse_file(std::filesystem::path(src), out, vars, section_stack, depth + 1);
            continue;
        }

        // Build flat key: "section1.section2.key".
        std::string flat_key;
        for (const auto& s : section_stack) {
            flat_key += s;
            flat_key += '.';
        }
        flat_key += key;

        out[flat_key] = value;
    }
}

} // namespace

std::unordered_map<std::string, std::string>
parse_hypr_flat(const std::filesystem::path& config_file) {
    std::unordered_map<std::string, std::string> out;
    std::unordered_map<std::string, std::string> vars;
    std::vector<std::string> section_stack;
    parse_file(config_file, out, vars, section_stack, 0);
    return out;
}

} // namespace transporter::theme::detail
