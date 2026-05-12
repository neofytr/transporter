// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration smoke test: brings up DbusService against a session bus, runs
// org.freedesktop.DBus.Introspectable.Introspect against both object paths,
// and prints the introspection XML. Skips with exit 0 when no session bus is
// available (CI / container without DBus).

#include <transporter/dbus/service.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr const char* kBusName = "org.mpris.MediaPlayer2.transporter";
constexpr const char* kMprisPath = "/org/mpris/MediaPlayer2";
constexpr const char* kCustomPath = "/org/mpris/MediaPlayer2/transporter";

bool session_bus_reachable() {
    try {
        auto c = sdbus::createSessionBusConnection();
        return static_cast<bool>(c);
    } catch (const sdbus::Error&) {
        return false;
    }
}

std::string introspect(sdbus::IConnection& c, const std::string& path) {
    auto proxy = sdbus::createProxy(c, sdbus::ServiceName{kBusName},
                                    sdbus::ObjectPath{path});
    std::string xml;
    proxy->callMethod("Introspect")
        .onInterface("org.freedesktop.DBus.Introspectable")
        .storeResultsTo(xml);
    return xml;
}

} // namespace

int main() {
    if (!session_bus_reachable()) {
        std::puts("SKIP: no session bus reachable");
        return 77;
    }

    auto svc_or = transporter::dbus_svc::DbusService::start(
        nullptr, nullptr, transporter::dbus_svc::Hooks{},
        transporter::dbus_svc::Config{});
    if (!svc_or) {
        const std::string msg = svc_or.error().message;
        // Another transporter (or earlier player) already owns the well-
        // known name on this bus. Env collision, not a code defect.
        if (msg.find("FileExists") != std::string::npos ||
            msg.find("already owned") != std::string::npos ||
            msg.find("Failed to request bus name") != std::string::npos) {
            std::printf(
                "SKIP: bus name already owned by another process\n");
            return 77;
        }
        std::fprintf(stderr, "service start failed: %s\n", msg.c_str());
        return 1;
    }

    // Give the event loop a moment so the first introspect is dispatched
    // cleanly; sdbus-c++ accepts requests immediately, so this is mostly
    // for the observer.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto client = sdbus::createSessionBusConnection();
    if (!client) {
        std::fprintf(stderr, "client connection failed\n");
        return 2;
    }

    try {
        auto xml1 = introspect(*client, kMprisPath);
        std::printf("---- introspect %s ----\n%s\n", kMprisPath, xml1.c_str());
        auto xml2 = introspect(*client, kCustomPath);
        std::printf("---- introspect %s ----\n%s\n", kCustomPath, xml2.c_str());
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "introspect failed: %s\n", e.what());
        return 3;
    }

    return 0;
}
