// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_HOTPLUG_MONITOR_HPP
#define TRANSPORTER_HOTPLUG_MONITOR_HPP

#include <transporter/engine/device.hpp>
#include <transporter/engine/error.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace transporter::hotplug {

enum class EventKind : std::uint8_t { Added, Removed };

// One DeviceEvent per ALSA card add/remove. PCM-device events are filtered
// out at the source so consumers don't see N duplicates per card transition.
// alsa_card_index is -1 when the kernel name didn't carry the controlC<N>
// pattern (rare; we still surface the event so policy can decide).
struct DeviceEvent {
    EventKind kind;
    int alsa_card_index;
    transporter::engine::DeviceFingerprint fingerprint;
};

// Engine-side consumer interface. The implementation owns a netlink socket
// (libudev); fd() exposes it for poll(). poll(out) is non-blocking and
// drains every pending event into the caller's buffer (returns the count).
class IMonitor {
public:
    virtual ~IMonitor() = default;

    // Drain pending events. Non-blocking. Returns 0 when none queued. Stops
    // at out.size(); the next call picks up the rest.
    virtual std::size_t poll(std::span<DeviceEvent> out) = 0;

    // Pollable file descriptor. -1 if the implementation has none (mocks
    // typically return a self-pipe write side; see tests/support/mock_monitor.hpp).
    virtual int fd() const noexcept = 0;
};

// Real udev monitor over the kernel netlink socket. Subscribes to the
// "sound" subsystem and filters down to controlC<N> add/remove events. On
// failure (udev_new() returns null, or netlink enable fails) returns a
// DeviceOpenFailed Error.
std::expected<std::unique_ptr<IMonitor>, transporter::engine::Error>
open_udev_monitor();

} // namespace transporter::hotplug

#endif
