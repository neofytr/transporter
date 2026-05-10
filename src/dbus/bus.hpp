// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_SRC_DBUS_BUS_HPP
#define TRANSPORTER_SRC_DBUS_BUS_HPP

#include <transporter/engine/error.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <expected>
#include <memory>
#include <string>

namespace transporter::dbus_svc {

// Thin wrapper around sdbus::IConnection plus the well-known bus-name
// acquisition. Owns the event-loop thread spawned via enterEventLoopAsync().
//
// open() opens a session-bus connection and requests the given well-known
// name. If the name is already taken (collision with a second instance), the
// underlying sd-bus call surfaces -EEXIST which sdbus-c++ reports as an
// sdbus::Error; we translate to engine::Error{DeviceBusy, ...} so callers can
// distinguish "bus unavailable" (DeviceOpenFailed) from "name taken"
// (DeviceBusy).
class Bus {
public:
    static std::expected<std::unique_ptr<Bus>, transporter::engine::Error>
    open(std::string well_known_name);

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    Bus(Bus&&) = delete;
    Bus& operator=(Bus&&) = delete;
    ~Bus();

    sdbus::IConnection& connection() noexcept { return *conn_; }

    // Spawn the event-loop thread. Must be called after all object vtables are
    // registered; sdbus-c++ recommends installing vtables on the calling
    // thread before the loop starts.
    void start_event_loop();

    // Stop the event-loop thread; safe to call multiple times. Called by the
    // destructor as well.
    void stop_event_loop();

private:
    Bus(std::unique_ptr<sdbus::IConnection> conn, std::string name);

    std::unique_ptr<sdbus::IConnection> conn_;
    std::string name_;
    bool name_acquired_ = false;
    bool loop_running_ = false;
};

} // namespace transporter::dbus_svc

#endif
