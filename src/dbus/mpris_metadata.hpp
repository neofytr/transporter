// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_SRC_DBUS_MPRIS_METADATA_HPP
#define TRANSPORTER_SRC_DBUS_MPRIS_METADATA_HPP

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/telemetry.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

namespace transporter::dbus_svc {

// Build the MPRIS 2 Metadata variant map from a pipeline snapshot. Keys
// emitted (when the underlying field is non-empty / non-zero):
//   mpris:trackid          (object path, opaque per-load id)
//   mpris:length           (microseconds, int64)
//   xesam:title            (string)
//   xesam:artist           (array of strings, MPRIS spec quirk)
//   xesam:album            (string)
//   xesam:trackNumber      (int32; parsed from "n" or "n/m")
//   xesam:url              (file://<absolute path>)
//   xesam:contentCreated   (string, ISO 8601 prefix when source.tags.date set)
//
// `trackid` must be a fully-qualified object path; the caller chooses the
// counter / namespace.
std::map<std::string, sdbus::Variant>
mpris_metadata_from_snapshot(const transporter::engine::PipelineSnapshot& snap,
                             const std::string& trackid_path);

// Same conversion exposed for testing without a live Engine. `total_frames`
// and the format determine `mpris:length`; passing 0 for either omits it.
std::map<std::string, sdbus::Variant>
mpris_metadata_from_parts(const std::string& file_path,
                          const transporter::engine::Tags& tags,
                          const transporter::engine::PcmFormat& format,
                          std::uint64_t total_frames,
                          const std::string& trackid_path);

} // namespace transporter::dbus_svc

#endif
