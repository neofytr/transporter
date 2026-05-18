// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

namespace transporter::engine {
class Engine;
}
namespace transporter::queue {
class Queue;
}
namespace transporter::library {
class Library;
}

namespace transporter::web {

struct WebConfig {
    std::string host       = "0.0.0.0";
    int         port       = 7800;
    std::string token;  // empty = no auth
    std::string static_dir = "web/dist";
    std::string config_path;  // written by /api/devices/select; empty = no-op
};

// Embedded HTTP server. REST control surface plus a 10 Hz WebSocket telemetry
// push. Owns two threads: one runs httplib's listen loop, one broadcasts
// pipeline snapshots to connected WebSocket clients. Both start in start()
// and join in stop() / the destructor. start() returns immediately so it
// never blocks main()'s signal-wait loop.
class WebServer {
public:
    WebServer(engine::Engine&, queue::Queue&, library::Library*, WebConfig cfg);
    ~WebServer();

    WebServer(const WebServer&)            = delete;
    WebServer& operator=(const WebServer&) = delete;

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transporter::web
