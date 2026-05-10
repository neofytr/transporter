// SPDX-License-Identifier: GPL-3.0-or-later
//
// Concrete IMonitor over libudev's "udev" netlink source. Subscribes to
// subsystem=sound and filters down to controlC<N> add/remove events. PCM
// device events (pcmC<N>D<M>{p,c}) are ignored because each card add
// emits N+1 of them (one per PCM subdevice) — we'd see duplicate state
// transitions for what is logically one card transition.
//
// "change" events on controlC nodes are coalesced to nothing: udev fires
// them on attribute writes (volume changes, etc.) and they don't represent
// a hotplug transition. Skipping is the safe choice.
//
// Threading: poll() and fd() are called from the engine worker thread only.
// libudev itself is thread-safe per-context but we don't share contexts.

#include <transporter/hotplug/monitor.hpp>

#include "../engine/device/device_internal.hpp"

#include <transporter/engine/device.hpp>
#include <transporter/engine/error.hpp>

#include <libudev.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace transporter::hotplug {

namespace {

struct UdevDeleter {
    void operator()(struct udev* p) const noexcept {
        if (p) {
            udev_unref(p);
        }
    }
};
struct UdevMonitorDeleter {
    void operator()(struct udev_monitor* p) const noexcept {
        if (p) {
            udev_monitor_unref(p);
        }
    }
};
struct UdevDeviceDeleter {
    void operator()(struct udev_device* p) const noexcept {
        if (p) {
            udev_device_unref(p);
        }
    }
};

using UdevHandle = std::unique_ptr<struct udev, UdevDeleter>;
using UdevMonitorHandle = std::unique_ptr<struct udev_monitor, UdevMonitorDeleter>;
using UdevDeviceHandle = std::unique_ptr<struct udev_device, UdevDeviceDeleter>;

bool is_control_node(const char* kernel_name) noexcept {
    if (kernel_name == nullptr) {
        return false;
    }
    // Kernel names: "controlC0", "controlC1", ...
    return std::strncmp(kernel_name, "controlC", 8) == 0;
}

class UdevMonitor final : public IMonitor {
public:
    UdevMonitor(UdevHandle udev, UdevMonitorHandle mon)
        : udev_(std::move(udev)), mon_(std::move(mon)),
          fd_(udev_monitor_get_fd(mon_.get())) {}

    std::size_t poll(std::span<DeviceEvent> out) override {
        if (out.empty()) {
            return 0;
        }
        std::size_t n = 0;
        while (n < out.size()) {
            UdevDeviceHandle dev{udev_monitor_receive_device(mon_.get())};
            if (!dev) {
                break; // socket drained (non-blocking)
            }

            const char* action = udev_device_get_action(dev.get());
            if (action == nullptr) {
                continue;
            }
            const bool is_add = std::strcmp(action, "add") == 0;
            const bool is_remove = std::strcmp(action, "remove") == 0;
            if (!is_add && !is_remove) {
                continue; // skip "change" / "bind" / "unbind"
            }

            const char* kernel = udev_device_get_sysname(dev.get());
            if (!is_control_node(kernel)) {
                continue; // ignore pcm*/midi*/hw* siblings
            }

            DeviceEvent ev{};
            ev.kind = is_add ? EventKind::Added : EventKind::Removed;
            ev.alsa_card_index =
                transporter::engine::detail::alsa_card_index_from_kernel(kernel);

            // Fingerprint the device. On removal the syspath may already be
            // gone; sysattr reads from libudev's cached snapshot still work
            // for the in-flight uevent. Walking parents to find usb_device
            // also still works for "remove".
            transporter::engine::DeviceFingerprint fp{};
            // ALSA card name / longname aren't carried in sound-class
            // sysattrs; we leave them empty here — fingerprint matching
            // hinges on usb_vendor_id:product_id:serial when present, and
            // that's exactly what udev gives us.
            transporter::engine::detail::populate_usb_fingerprint_from_udev_device(
                dev.get(), fp);
            ev.fingerprint = std::move(fp);

            out[n++] = std::move(ev);
        }
        return n;
    }

    int fd() const noexcept override { return fd_; }

private:
    UdevHandle udev_;
    UdevMonitorHandle mon_;
    int fd_;
};

} // namespace

std::expected<std::unique_ptr<IMonitor>, transporter::engine::Error>
open_udev_monitor() {
    UdevHandle udev{udev_new()};
    if (!udev) {
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::DeviceOpenFailed,
            "udev_new failed"});
    }

    UdevMonitorHandle mon{
        udev_monitor_new_from_netlink(udev.get(), "udev")};
    if (!mon) {
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::DeviceOpenFailed,
            "udev_monitor_new_from_netlink(udev) failed"});
    }

    int rc = udev_monitor_filter_add_match_subsystem_devtype(
        mon.get(), "sound", nullptr);
    if (rc < 0) {
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::DeviceOpenFailed,
            std::string{"udev_monitor_filter_add_match_subsystem_devtype: "} +
                std::strerror(-rc)});
    }

    rc = udev_monitor_enable_receiving(mon.get());
    if (rc < 0) {
        return std::unexpected(transporter::engine::Error{
            transporter::engine::ErrorCode::DeviceOpenFailed,
            std::string{"udev_monitor_enable_receiving: "} +
                std::strerror(-rc)});
    }

    return std::make_unique<UdevMonitor>(std::move(udev), std::move(mon));
}

} // namespace transporter::hotplug
