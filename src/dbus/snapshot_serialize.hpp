// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_SRC_DBUS_SNAPSHOT_SERIALIZE_HPP
#define TRANSPORTER_SRC_DBUS_SNAPSHOT_SERIALIZE_HPP

#include <transporter/engine/device.hpp>
#include <transporter/engine/telemetry.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace transporter::dbus_svc {

constexpr std::string_view bit_perfect_level_name(
    transporter::engine::BitPerfectVerdict::Level lv) noexcept {
    using L = transporter::engine::BitPerfectVerdict::Level;
    switch (lv) {
    case L::Yes:       return "YES";
    case L::Qualified: return "QUALIFIED";
    case L::No:        return "NO";
    }
    return "NO";
}

// "vid:pid:serial" or empty when not USB.
std::string usb_vid_pid_serial(
    const transporter::engine::DeviceFingerprint& fp);

// Union of (format, rate) pairs as ["S16_LE@44100", ...].
std::vector<std::string> device_format_strings(
    const transporter::engine::DeviceCapabilities& caps);

// Render the full snapshot as a dict of stage-name to dict of field-name to
// variant. Schema documented in transporter_iface.cpp.
std::map<std::string, std::map<std::string, sdbus::Variant>>
snapshot_to_dict(const transporter::engine::PipelineSnapshot& snap);

} // namespace transporter::dbus_svc

#endif
