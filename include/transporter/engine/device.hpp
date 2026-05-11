// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_DEVICE_HPP
#define TRANSPORTER_ENGINE_DEVICE_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace transporter::engine {

// Stable identifier inputs. Empty USB fields => non-USB or no serial available.
// alsa_card_name / longname mirror snd_ctl_card_info_get_{name,longname}.
struct DeviceFingerprint {
    std::string usb_vendor_id;       // four-hex, lowercase, e.g. "0bda"
    std::string usb_product_id;      // four-hex, lowercase
    std::string usb_serial;          // empty if device has no serial
    std::string alsa_card_name;      // e.g. "PCH" or "Topping_E70"
    std::string alsa_card_longname;  // e.g. "HDA Intel PCH at 0xN ..."
    bool is_usb = false;
};

// Hardware-volume control info. has_db_scale==true means min/max are in
// hundredths of a dB. When the element exists but reports no dB scale, the
// raw range is left at 0; callers should treat that as "present but
// uncalibrated".
struct VolumeControl {
    bool present = false;
    long min_db_x100 = 0;            // hundredths of a dB
    long max_db_x100 = 0;
    bool has_db_scale = false;
    std::string element_name;        // "Master" / "Speaker" / "PCM" / etc.
};

// Per-device probe result. caps_probe_failed=true when the PCM was busy or
// an open/probe call rejected us; the format / rate / channel fields are
// then empty and the GUI can render "in use by another app" instead of
// hiding the device.
struct DeviceCapabilities {
    std::vector<SampleFormat> formats;
    std::vector<std::uint32_t> sample_rates;
    std::uint16_t channels_min = 0;
    std::uint16_t channels_max = 0;
    VolumeControl hw_volume;
    bool caps_probe_failed = false;
    std::string probe_failure_reason;  // populated when caps_probe_failed
};

struct DeviceInfo {
    // Stable id derived from fingerprint. Survives ALSA card-index reshuffling
    // across reboots when the device exposes a USB serial; otherwise tied to
    // the ALSA card longname.
    std::string id;

    // Current ALSA hw string of the form "hw:CARD=<name>,DEV=<n>". Stable
    // within a boot; may shuffle across reboots.
    std::string alsa_hw_string;

    int alsa_card_index = -1;
    int alsa_device_index = -1;

    DeviceFingerprint fingerprint;
    DeviceCapabilities caps;
};

// Enumerate every PCM playback device on the system. Each card with no
// playback subdevices is skipped silently (it cannot serve us). Devices that
// fail to open with -EBUSY are still reported with caps_probe_failed=true so
// the GUI can show "DAC busy" instead of hiding them.
std::expected<std::vector<DeviceInfo>, Error> list_playback_devices();

// Build a DeviceInfo for one explicit hw:CARD=...,DEV=N string. Used by the
// engine when the user has pinned a preferred device and we want full caps
// before opening for playback.
std::expected<DeviceInfo, Error> describe_device(const std::string& alsa_hw_string);

// Stable-id helper.
//   USB with serial:  "usb:<vendor>:<product>:<serial>"
//   USB without serial: "usb:<vendor>:<product>:noserial:<sanitized-longname>"
//   non-USB:          "card:<sanitized-longname>"
// Sanitization: characters outside [a-zA-Z0-9._-] are replaced with '_'.
std::string make_stable_id(const DeviceFingerprint& fp);

// HW volume helpers. Operate on the ALSA control device (separate from the
// PCM device; safe to call while the PCM is held open exclusively).
// alsa_hw_string must be in "hw:CARD=<name>,DEV=<n>" form.
// Returns -1 when the control is unavailable.
int get_hw_volume_pct(const std::string& alsa_hw_string);
// No-op when hw_volume is absent. pct is clamped to [0, 100].
void set_hw_volume_pct(const std::string& alsa_hw_string, int pct);
void toggle_hw_mute(const std::string& alsa_hw_string);
// Returns true when muted, false when unmuted or when there is no mute switch.
bool get_hw_mute_state(const std::string& alsa_hw_string);

} // namespace transporter::engine

#endif
