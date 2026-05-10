// SPDX-License-Identifier: GPL-3.0-or-later
//
// Round-trip a synthetic config through the TOML parser; assert defaults
// applied; assert override values stick.

#include <transporter/config/config.hpp>

#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace transporter::config;

namespace {

int fail(const char* where) {
    std::fprintf(stderr, "FAIL %s\n", where);
    return 1;
}

int test_defaults() {
    constexpr std::string_view empty = "";
    auto r = parse(empty);
    if (!r) return fail("parse empty");
    const Config& c = *r;
    if (!c.device.preferred.empty()) return fail("default preferred");
    if (c.audio.period_ms != 12) return fail("default period_ms");
    if (c.audio.period_count != 4) return fail("default period_count");
    if (c.audio.rt_policy != RtPolicy::Auto) return fail("default rt_policy");
    if (!c.library.roots.empty()) return fail("default roots");
    if (!c.library.ignore_patterns.empty()) return fail("default ignore_patterns");
    if (!c.theme.follow_hyprland) return fail("default follow_hyprland");
    if (c.ui.default_view != DefaultView::Main) return fail("default view");
    return 0;
}

int test_full_override() {
    constexpr std::string_view text =
        "[device]\n"
        "preferred = \"hw:CARD=USBDAC,DEV=0\"\n"
        "\n"
        "[audio]\n"
        "period_ms = 24\n"
        "period_count = 8\n"
        "rt_policy = \"fifo\"\n"
        "\n"
        "[library]\n"
        "roots = [\"/srv/music\", \"/mnt/flac\"]\n"
        "ignore_patterns = [\"*/.Trash/*\", \"*/lost+found/*\"]\n"
        "\n"
        "[theme]\n"
        "follow_hyprland = false\n"
        "override_file = \"/etc/transporter/theme.toml\"\n"
        "\n"
        "[ui]\n"
        "default_view = \"pipeline\"\n";

    auto r = parse(text);
    if (!r) return fail("parse override");
    const Config& c = *r;

    if (c.device.preferred != "hw:CARD=USBDAC,DEV=0") return fail("preferred");
    if (c.audio.period_ms != 24) return fail("period_ms");
    if (c.audio.period_count != 8) return fail("period_count");
    if (c.audio.rt_policy != RtPolicy::Fifo) return fail("rt_policy");
    if (c.library.roots.size() != 2) return fail("roots size");
    if (c.library.roots[0] != "/srv/music") return fail("roots[0]");
    if (c.library.roots[1] != "/mnt/flac") return fail("roots[1]");
    if (c.library.ignore_patterns.size() != 2) return fail("ignore size");
    if (c.theme.follow_hyprland) return fail("follow_hyprland");
    if (c.theme.override_file != "/etc/transporter/theme.toml")
        return fail("override_file");
    if (c.ui.default_view != DefaultView::Pipeline) return fail("default_view");
    return 0;
}

int test_partial_override() {
    constexpr std::string_view text =
        "[audio]\n"
        "period_ms = 16\n"
        "[ui]\n"
        "default_view = \"library\"\n";
    auto r = parse(text);
    if (!r) return fail("parse partial");
    const Config& c = *r;
    if (c.audio.period_ms != 16) return fail("period_ms partial");
    if (c.audio.period_count != 4) return fail("period_count default kept");
    if (c.audio.rt_policy != RtPolicy::Auto) return fail("rt_policy default kept");
    if (c.ui.default_view != DefaultView::Library) return fail("default_view partial");
    return 0;
}

int test_parse_error() {
    constexpr std::string_view text = "[unterminated\n";
    auto r = parse(text);
    if (r) return fail("expected parse error");
    return 0;
}

int test_user_expansion() {
    setenv("HOME", "/home/test", 1);
    auto p = expand_user("~/Music");
    if (p != "/home/test/Music") return fail("expand_user ~/");
    p = expand_user("/abs/Music");
    if (p != "/abs/Music") return fail("expand_user abs");
    return 0;
}

} // namespace

int main() {
    if (int rc = test_defaults();        rc != 0) return rc;
    if (int rc = test_full_override();   rc != 0) return rc;
    if (int rc = test_partial_override();rc != 0) return rc;
    if (int rc = test_parse_error();     rc != 0) return rc;
    if (int rc = test_user_expansion();  rc != 0) return rc;
    std::puts("ok config_toml");
    return 0;
}
