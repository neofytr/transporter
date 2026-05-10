// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/config/config.hpp>
#include <transporter/version.hpp>

#include "gui/views/app.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace {

void print_version() {
    std::fputs("transporter ", stdout);
    std::fwrite(transporter::version.data(), 1, transporter::version.size(), stdout);
    std::fputc('\n', stdout);
}

void print_usage() {
    std::fputs("usage: transporter [--version|-V] [--no-gui|-N] "
               "[--device hw:CARD=...,DEV=N] [<file>]\n",
               stderr);
}

struct ParsedArgs {
    bool version = false;
    bool no_gui = false;
    bool help = false;
    std::string device;
    std::string file;
    bool error = false;
    std::string error_msg;
};

ParsedArgs parse_args(int argc, char** argv) {
    ParsedArgs out;
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view a{args[i]};
        if (a == "--version" || a == "-V") {
            out.version = true;
        } else if (a == "--no-gui" || a == "-N") {
            out.no_gui = true;
        } else if (a == "--help" || a == "-h") {
            out.help = true;
        } else if (a == "--device") {
            if (i + 1 >= args.size()) {
                out.error = true;
                out.error_msg = "--device requires an argument";
                break;
            }
            out.device = args[++i];
        } else if (a.starts_with("--device=")) {
            out.device = std::string(a.substr(9));
        } else if (!a.empty() && a[0] != '-' && out.file.empty()) {
            out.file = std::string(a);
        } else if (!a.empty() && a[0] != '-' && !out.file.empty()) {
            // Positional: legacy form `--no-gui <file> <hw:string>` from the
            // prior phase test harnesses. Treat the second positional as a
            // device override if --device was not given.
            if (out.device.empty()) {
                out.device = std::string(a);
            } else {
                out.error = true;
                out.error_msg = std::string{"unexpected positional argument: "} +
                                std::string(a);
                break;
            }
        } else {
            out.error = true;
            out.error_msg = std::string{"unknown option: "} + std::string(a);
            break;
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const ParsedArgs args = parse_args(argc, argv);

    if (args.error) {
        std::fprintf(stderr, "transporter: %s\n", args.error_msg.c_str());
        print_usage();
        return 64;
    }
    if (args.help) {
        print_usage();
        return 0;
    }
    if (args.version) {
        print_version();
        return 0;
    }

    transporter::gui::AppArgs ga;
    ga.config_path = transporter::config::default_config_path();
    if (!args.file.empty()) {
        ga.file_to_play = std::filesystem::path{args.file};
    }
    ga.device_override = args.device;

    if (args.no_gui) {
        return transporter::gui::run_headless(ga);
    }
    return transporter::gui::run(ga);
}
