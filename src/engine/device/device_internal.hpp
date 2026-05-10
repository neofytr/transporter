// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-internal seam between device/list.cpp, device/fingerprint.cpp, and
// device/probe.cpp. Not a public header.

#ifndef TRANSPORTER_ENGINE_DEVICE_INTERNAL_HPP
#define TRANSPORTER_ENGINE_DEVICE_INTERNAL_HPP

#include <transporter/engine/device.hpp>
#include <transporter/engine/error.hpp>

#include <expected>
#include <string>

namespace transporter::engine::detail {

struct ParsedHwString {
    int card_index;
    int device_index;
};

// Parse "hw:CARD=<id>,DEV=<n>" or "hw:<card_index>,<device_index>". Returns
// the resolved card index (snd_card_get_index for the CARD= form) and the
// device index. Used by describe_device.
std::expected<ParsedHwString, Error> parse_hw_string(const std::string& hw_string);

// Walk /sys/class/sound/cardN/device up to a USB device node via libudev.
// On success populates fp.is_usb=true and the usb_* fields. On failure
// (non-USB or no serial) fields are left untouched. Never returns an error;
// non-USB cards are normal.
void populate_usb_fingerprint(int card_index, DeviceFingerprint& fp);

// Variant for an already-resolved udev_device pointing at a sound-class
// node. Hotplug monitor uses this to fingerprint the device delivered by
// the netlink event without re-opening a udev_new context. The pointer is
// declared as void* to keep this header free of <libudev.h>; callers cast
// from struct udev_device*.
void populate_usb_fingerprint_from_udev_device(void* udev_dev, DeviceFingerprint& fp);

// Parse the ALSA card index from a sound-class kernel name. Accepts
// "controlC<N>", "pcmC<N>D<M>{p,c}", "midiC<N>D<M>", "hwC<N>D<M>", etc.
// Returns -1 when the name doesn't carry a card index.
int alsa_card_index_from_kernel(const char* kernel_name);

// Open the PCM, run the format/rate/channel probe, then probe HW volume
// via the simple-mixer API. Cleans up on every path. Sets
// caps.caps_probe_failed=true (with reason) on -EBUSY or any open-time
// error so the GUI can render "in use by another app".
DeviceCapabilities probe_device(const std::string& alsa_hw_string);

} // namespace transporter::engine::detail

#endif
