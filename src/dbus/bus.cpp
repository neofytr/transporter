// SPDX-License-Identifier: GPL-3.0-or-later

#include "bus.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <string>
#include <utility>

namespace transporter::dbus_svc {

using transporter::engine::Error;
using transporter::engine::ErrorCode;

Bus::Bus(std::unique_ptr<sdbus::IConnection> conn, std::string name)
    : conn_(std::move(conn)), name_(std::move(name)) {}

Bus::~Bus() {
    stop_event_loop();
    if (name_acquired_ && conn_) {
        try {
            conn_->releaseName(sdbus::ServiceName{name_});
        } catch (const sdbus::Error&) {
            // best-effort on shutdown
        }
    }
}

std::expected<std::unique_ptr<Bus>, Error>
Bus::open(std::string well_known_name) {
    std::unique_ptr<sdbus::IConnection> conn;
    try {
        conn = sdbus::createSessionBusConnection();
    } catch (const sdbus::Error& e) {
        return std::unexpected(Error{
            ErrorCode::DeviceOpenFailed,
            std::string{"session bus connect failed: "} + e.what()});
    }
    if (!conn) {
        return std::unexpected(
            Error{ErrorCode::DeviceOpenFailed,
                  "session bus connect returned null"});
    }

    try {
        conn->requestName(sdbus::ServiceName{well_known_name});
    } catch (const sdbus::Error& e) {
        // The well-known name is already owned by another instance, or the
        // policy denied us. Either way the caller cannot proceed; report as
        // DeviceBusy so the GUI can render a "second instance" toast.
        return std::unexpected(Error{
            ErrorCode::DeviceBusy,
            std::string{"DBus name request failed for "} + well_known_name +
                ": " + e.what()});
    }

    auto bus = std::unique_ptr<Bus>(
        new Bus(std::move(conn), std::move(well_known_name)));
    bus->name_acquired_ = true;
    return bus;
}

void Bus::start_event_loop() {
    if (loop_running_ || !conn_) {
        return;
    }
    conn_->enterEventLoopAsync();
    loop_running_ = true;
}

void Bus::stop_event_loop() {
    if (!loop_running_ || !conn_) {
        return;
    }
    try {
        conn_->leaveEventLoop();
    } catch (const sdbus::Error&) {
        // already stopped or detached; nothing useful to do
    }
    loop_running_ = false;
}

} // namespace transporter::dbus_svc
