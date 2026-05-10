// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/version.hpp>

#include <cstdio>
#include <span>
#include <string_view>

namespace {

int print_version() {
    std::fputs("transporter ", stdout);
    std::fwrite(transporter::version.data(), 1, transporter::version.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    for (const char* raw : args.subspan(1)) {
        const std::string_view arg{raw};
        if (arg == "--version" || arg == "-V") {
            return print_version();
        }
    }
    return 0;
}
