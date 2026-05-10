// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB fingerprint extraction via libudev. Walks the udev tree starting at
// /sys/class/sound/cardN up to a parent of subsystem=usb, devtype=usb_device,
// then reads idVendor / idProduct / serial. Non-USB cards quietly leave the
// fingerprint USB fields empty.

#include <transporter/engine/device.hpp>

#include "device_internal.hpp"

#include <libudev.h>

#include <cctype>
#include <cstring>
#include <memory>
#include <string>

namespace transporter::engine {

namespace {

struct UdevDeleter {
    void operator()(struct udev* p) const noexcept {
        if (p) {
            udev_unref(p);
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
using UdevDeviceHandle = std::unique_ptr<struct udev_device, UdevDeviceDeleter>;

std::string lower_hex(const char* s) {
    std::string out;
    if (s == nullptr) {
        return out;
    }
    for (const char* p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string sanitize_for_id(const std::string& in) {
    // Replace anything outside [a-zA-Z0-9._-] with '_'. Filesystem-safe and
    // visually unambiguous.
    std::string out;
    out.reserve(in.size());
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(keep ? ch : '_');
    }
    return out;
}

} // namespace

namespace detail {

void populate_usb_fingerprint(int card_index, DeviceFingerprint& fp) {
    UdevHandle udev{udev_new()};
    if (!udev) {
        return;
    }

    std::string syspath = "/sys/class/sound/card";
    syspath += std::to_string(card_index);

    UdevDeviceHandle dev{
        udev_device_new_from_syspath(udev.get(), syspath.c_str())};
    if (!dev) {
        return;
    }

    // Walk up parent chain looking for subsystem=usb / devtype=usb_device.
    // udev_device_get_parent_with_subsystem_devtype does this in one call.
    struct udev_device* usb_parent = udev_device_get_parent_with_subsystem_devtype(
        dev.get(), "usb", "usb_device");
    if (usb_parent == nullptr) {
        return;
    }

    const char* vid = udev_device_get_sysattr_value(usb_parent, "idVendor");
    const char* pid = udev_device_get_sysattr_value(usb_parent, "idProduct");
    const char* serial = udev_device_get_sysattr_value(usb_parent, "serial");

    fp.is_usb = true;
    fp.usb_vendor_id = lower_hex(vid);
    fp.usb_product_id = lower_hex(pid);
    fp.usb_serial = serial != nullptr ? std::string{serial} : std::string{};
}

} // namespace detail

std::string make_stable_id(const DeviceFingerprint& fp) {
    if (fp.is_usb && !fp.usb_vendor_id.empty() && !fp.usb_product_id.empty()) {
        std::string id = "usb:";
        id += fp.usb_vendor_id;
        id += ":";
        id += fp.usb_product_id;
        if (!fp.usb_serial.empty()) {
            id += ":";
            id += sanitize_for_id(fp.usb_serial);
        } else {
            // No serial: differentiate two of the same model by tagging the
            // longname so two identical DACs aren't conflated. Rare but
            // possible (multiple identical USB DACs on one host).
            id += ":noserial:";
            id += sanitize_for_id(fp.alsa_card_longname.empty()
                                      ? fp.alsa_card_name
                                      : fp.alsa_card_longname);
        }
        return id;
    }
    std::string id = "card:";
    id += sanitize_for_id(fp.alsa_card_longname.empty() ? fp.alsa_card_name
                                                        : fp.alsa_card_longname);
    return id;
}

} // namespace transporter::engine
