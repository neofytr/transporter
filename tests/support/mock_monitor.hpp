// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hand-pumped IMonitor for unit tests. Uses a self-pipe so a fd() poll on
// the engine worker wakes when push() enqueues an event. Pump from the
// test thread; consume from the engine worker.

#ifndef TRANSPORTER_TESTS_SUPPORT_MOCK_MONITOR_HPP
#define TRANSPORTER_TESTS_SUPPORT_MOCK_MONITOR_HPP

#include <transporter/engine/error.hpp>
#include <transporter/hotplug/monitor.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

namespace transporter::tests {

class MockMonitor final : public transporter::hotplug::IMonitor {
public:
    MockMonitor() {
        int p[2] = {-1, -1};
        if (pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0) {
            read_fd_ = p[0];
            write_fd_ = p[1];
        }
    }
    ~MockMonitor() override {
        if (read_fd_ >= 0) {
            ::close(read_fd_);
        }
        if (write_fd_ >= 0) {
            ::close(write_fd_);
        }
    }

    MockMonitor(const MockMonitor&) = delete;
    MockMonitor& operator=(const MockMonitor&) = delete;
    MockMonitor(MockMonitor&&) = delete;
    MockMonitor& operator=(MockMonitor&&) = delete;

    // Test side: enqueue an event. Wakes any poll() on read_fd_.
    void push(transporter::hotplug::DeviceEvent ev) {
        {
            std::lock_guard lk(mtx_);
            queue_.push_back(std::move(ev));
        }
        if (write_fd_ >= 0) {
            const char b = 'x';
            const ssize_t w = ::write(write_fd_, &b, 1);
            (void)w;
        }
    }

    std::size_t poll(std::span<transporter::hotplug::DeviceEvent> out) override {
        std::lock_guard lk(mtx_);
        std::size_t n = 0;
        // Drain pipe bytes corresponding to events delivered.
        while (n < out.size() && !queue_.empty()) {
            out[n++] = std::move(queue_.front());
            queue_.pop_front();
            if (read_fd_ >= 0) {
                char tmp;
                const ssize_t r = ::read(read_fd_, &tmp, 1);
                (void)r;
            }
        }
        return n;
    }

    int fd() const noexcept override { return read_fd_; }

private:
    std::mutex mtx_;
    std::deque<transporter::hotplug::DeviceEvent> queue_;
    int read_fd_{-1};
    int write_fd_{-1};
};

} // namespace transporter::tests

#endif
