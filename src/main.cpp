// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/config/config.hpp>
#include <transporter/dbus/service.hpp>
#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/library/library.hpp>
#include <transporter/queue/queue.hpp>
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
        "Linux-native, bit-perfect music player. MPRIS daemon; TUI coming later.\n"
        "\n"
        "Options:\n"
        "  --config PATH       Override config file path\n"
        "  --library-db PATH   Override library SQLite path\n"
        "  --version, -V       Print version and exit\n"
        "  --help, -h          Print this help and exit\n"
        "\n"
        "Positional FILE is loaded and played at launch.\n",
        stdout);
}

struct ParsedArgs {
    bool show_version   = false;
    bool show_help      = false;
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
namespace que = transporter::queue;

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

    install_signal_handlers();

    const std::filesystem::path config_path = args.config_path.empty()
        ? cfg::default_config_path() : args.config_path;
    cfg::Config config = load_config_or_defaults(config_path);

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
            std::fprintf(stderr, "transporter: engine create failed: %s\n",
                         e.error().message.c_str());
        }
    } else {
        std::fprintf(stderr,
                     "transporter: no playback device available (%zu enumerated)\n",
                     devices.size());
    }

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
            std::fprintf(stderr, "transporter: library open failed: %s\n",
                         l.error().message.c_str());
        }
    }

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
            std::fprintf(stderr,
                         "transporter: MPRIS unavailable: %s\n",
                         svc.error().message.c_str());
        }
    }

    // Queue: owns track list and drives engine preload() for gapless.
    std::unique_ptr<que::Queue> queue;
    if (engine != nullptr) {
        queue = std::make_unique<que::Queue>(*engine);
        engine->set_event_callback([&](const eng::Event& ev) {
            queue->on_event(ev);
            if (dbus != nullptr &&
                ev.kind == eng::Event::Kind::TrackLoaded) {
                dbus->notify_track_loaded();
            }
        });
        if (!args.file.empty()) {
            queue->append(args.file);
        }
    }

    wait_for_termination_signal();

    if (dbus != nullptr) {
        dbus->shutdown();
        dbus.reset();
    }
    if (engine != nullptr) {
        engine->set_event_callback({});
        engine->stop();
    }
    queue.reset();
    library.reset();
    engine.reset();
    return 0;
}
