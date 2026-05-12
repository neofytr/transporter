// SPDX-License-Identifier: GPL-3.0-or-later

#include "tui/app.hpp"
#include "tui/notcurses_ctx.hpp"

#include <transporter/config/config.hpp>
#include <transporter/dbus/service.hpp>
#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/library/library.hpp>
#include <transporter/version.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace {

void print_version() {
    std::fputs("transporter ", stdout);
    std::fwrite(transporter::version.data(), 1, transporter::version.size(),
                stdout);
    std::fputc('\n', stdout);
}

void print_help() {
    std::fputs(
        "usage: transporter [OPTIONS] [FILE]\n"
        "\n"
        "Linux-native, bit-perfect music player. TUI when stdin/stdout are a\n"
        "real tty; MPRIS-only daemon otherwise.\n"
        "\n"
        "Options:\n"
        "  --daemon            Force daemon mode (no TUI, MPRIS only)\n"
        "  --no-mouse          Disable mouse handling in the TUI\n"
        "  --config PATH       Override config file path\n"
        "  --library-db PATH   Override library SQLite path\n"
        "  --probe-terminal    Print terminal capability matrix and exit\n"
        "  --version, -V       Print version and exit\n"
        "  --help, -h          Print this help and exit\n"
        "\n"
        "Positional FILE is loaded and played at launch.\n"
        "\n"
        "Keys (TUI):\n"
        "  1..5            Switch to page (Library/Queue/NowPlaying/Pipeline/Settings)\n"
        "  Tab / S-Tab     Cycle pages\n"
        "  h j k l         Move (arrows also work)\n"
        "  gg / G          Top / bottom; prefix with count, e.g. 5j\n"
        "  space           Play / pause\n"
        "  / : ?           Search / command palette / help overlay\n"
        "  q : q  Ctrl-C   Quit\n",
        stdout);
}

struct ParsedArgs {
    bool show_version    = false;
    bool show_help       = false;
    bool daemon          = false;
    bool no_mouse        = false;
    bool probe_terminal  = false;
    std::filesystem::path config_path;
    std::filesystem::path library_db_path;
    std::filesystem::path file;
    bool error = false;
    std::string error_msg;
};

bool needs_value(std::string_view a) {
    return a == "--config" || a == "--library-db";
}

ParsedArgs parse_args(int argc, char** argv) {
    ParsedArgs out;
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view a{args[i]};
        if (a == "--version" || a == "-V") {
            out.show_version = true;
        } else if (a == "--help" || a == "-h") {
            out.show_help = true;
        } else if (a == "--daemon") {
            out.daemon = true;
        } else if (a == "--no-mouse") {
            out.no_mouse = true;
        } else if (a == "--probe-terminal") {
            out.probe_terminal = true;
        } else if (needs_value(a)) {
            if (i + 1 >= args.size()) {
                out.error = true;
                out.error_msg = std::string(a) + " requires an argument";
                break;
            }
            const std::string_view val = args[++i];
            if (a == "--config") {
                out.config_path = val;
            } else {
                out.library_db_path = val;
            }
        } else if (a.starts_with("--config=")) {
            out.config_path = a.substr(9);
        } else if (a.starts_with("--library-db=")) {
            out.library_db_path = a.substr(13);
        } else if (!a.empty() && a[0] != '-' && out.file.empty()) {
            out.file = std::string(a);
        } else {
            out.error = true;
            out.error_msg = std::string{"unknown option: "} + std::string(a);
            break;
        }
    }
    return out;
}

namespace eng = transporter::engine;
namespace cfg = transporter::config;
namespace lib = transporter::library;
namespace dbs = transporter::dbus_svc;

std::string pick_device(const cfg::Config& c,
                        const std::vector<eng::DeviceInfo>& devices) {
    if (!c.device.preferred.empty()) {
        return c.device.preferred;
    }
    if (!devices.empty()) {
        return devices.front().alsa_hw_string;
    }
    return {};
}

cfg::Config load_config_or_defaults(const std::filesystem::path& path) {
    if (path.empty()) {
        return cfg::Config{};
    }
    auto r = cfg::load_file(path);
    if (r) {
        return std::move(*r);
    }
    return cfg::Config{};
}

// Signal-based shutdown for daemon mode. Stored as a sig_atomic_t flag and
// polled with sigsuspend so the engine event-loop thread keeps running.
std::atomic<int> g_signal{0};

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_flags = 0;
    sa.sa_handler = [](int sig) {
        g_signal.store(sig, std::memory_order_relaxed);
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);
}

void wait_for_termination_signal() {
    sigset_t mask;
    sigemptyset(&mask);
    while (g_signal.load(std::memory_order_relaxed) == 0) {
        sigsuspend(&mask);
        if (errno != EINTR) {
            break;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const ParsedArgs args = parse_args(argc, argv);

    if (args.error) {
        std::fprintf(stderr, "transporter: %s\n", args.error_msg.c_str());
        print_help();
        return 64;
    }
    if (args.show_help) {
        print_help();
        return 0;
    }
    if (args.show_version) {
        print_version();
        return 0;
    }
    if (args.probe_terminal) {
        transporter::tui::print_capabilities();
        return 0;
    }

    install_signal_handlers();

    // Decide the mode up front so startup messages can be routed correctly.
    // In TUI mode, notcurses takes over the screen; stderr writes leak into
    // the alternate-screen scrollback and flash before the TUI paints. Buffer
    // them and flush after notcurses shuts down so the player surface stays
    // clean. In daemon mode, stderr is the right channel and is fine live.
    const bool tty    = (isatty(STDIN_FILENO) != 0)
                     && (isatty(STDOUT_FILENO) != 0);
    const bool daemon = args.daemon || !tty;

    std::vector<std::string> deferred_errors;
    auto log_err = [&](std::string line) {
        if (!line.empty() && line.back() != '\n') line.push_back('\n');
        if (daemon) {
            std::fputs(line.c_str(), stderr);
        } else {
            deferred_errors.push_back(std::move(line));
        }
    };
    auto flush_deferred = [&] {
        for (const auto& s : deferred_errors) std::fputs(s.c_str(), stderr);
        deferred_errors.clear();
    };

    const std::filesystem::path config_path = args.config_path.empty()
        ? cfg::default_config_path() : args.config_path;
    cfg::Config config = load_config_or_defaults(config_path);

    // Engine. Best-effort device discovery; an empty device list yields an
    // engine-less run which still exposes MPRIS in daemon mode.
    std::unique_ptr<eng::Engine> engine;
    std::vector<eng::DeviceInfo> devices;
    if (auto r = eng::list_playback_devices(); r) {
        devices = std::move(*r);
    }
    const std::string device_id = pick_device(config, devices);
    if (!device_id.empty()) {
        eng::EngineConfig ec{};
        ec.device_id = device_id;
        if (auto e = eng::Engine::create(std::move(ec)); e) {
            engine = std::move(*e);
        } else {
            log_err(std::string{"transporter: engine create failed: "} +
                    e.error().message);
        }
    } else {
        log_err("transporter: no playback device available (" +
                std::to_string(devices.size()) + " enumerated)");
    }

    // Library.
    std::unique_ptr<lib::Library> library;
    {
        lib::Config lc{};
        lc.db_path = args.library_db_path.empty()
            ? cfg::default_library_db_path() : args.library_db_path;
        lc.roots = config.library.roots;
        lc.ignore_patterns = config.library.ignore_patterns;
        std::error_code ec;
        std::filesystem::create_directories(lc.db_path.parent_path(), ec);
        if (auto l = lib::Library::open(std::move(lc)); l) {
            library = std::move(*l);
            if (!config.library.roots.empty()) {
                library->rescan_async();
            }
        } else {
            log_err(std::string{"transporter: library open failed: "} +
                    l.error().message);
        }
    }

    // DBus / MPRIS. Hooks are stubs for now; T1+ will wire the TUI queue.
    std::unique_ptr<dbs::DbusService> dbus;
    if (config.dbus.enabled) {
        dbs::Hooks hooks;
        dbs::Config dc;
        dc.enabled = true;
        auto svc = dbs::DbusService::start(engine.get(), library.get(),
                                           std::move(hooks), dc);
        if (svc) {
            dbus = std::move(*svc);
        } else {
            // Most common cause is "another transporter is already running" —
            // the MPRIS well-known name is single-owner. The TUI still works
            // without MPRIS; surface the reason on exit so the user can decide.
            log_err(std::string{"transporter: MPRIS unavailable: "} +
                    svc.error().message +
                    " (running without external control)");
        }
    }

    // Optional initial file from argv: load + play immediately.
    if (!args.file.empty() && engine != nullptr) {
        if (auto r = engine->load(args.file); !r) {
            log_err(std::string{"transporter: load failed: "} +
                    r.error().message);
        } else if (auto p = engine->play(); !p) {
            log_err(std::string{"transporter: play failed: "} +
                    p.error().message);
        } else if (dbus != nullptr) {
            dbus->notify_track_loaded();
        }
    }

    if (daemon) {
        wait_for_termination_signal();
    } else {
        transporter::tui::InitOptions iopts{};
        iopts.enable_mouse = !args.no_mouse;
        if (transporter::tui::init(iopts) != 0) {
            // Screen was never opened, so stderr is still safe — emit any
            // buffered errors plus the init failure and fall back to daemon.
            flush_deferred();
            std::fputs("transporter: notcurses init failed, "
                       "falling back to daemon mode\n", stderr);
            wait_for_termination_signal();
        } else {
            transporter::tui::run(engine.get(), library.get());
            transporter::tui::shutdown();
            flush_deferred();
        }
    }

    // Teardown order: stop dbus before engine; library + engine drop on
    // unique_ptr destruction below.
    if (dbus != nullptr) {
        dbus->shutdown();
        dbus.reset();
    }
    if (engine != nullptr) {
        engine->set_event_callback({});
        engine->stop();
    }
    library.reset();
    engine.reset();
    return 0;
}
