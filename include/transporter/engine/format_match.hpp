// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_FORMAT_MATCH_HPP
#define TRANSPORTER_ENGINE_FORMAT_MATCH_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <expected>

namespace transporter::engine {

// Returns the file's format unchanged iff every component is in the device's
// accepted set. Refuses with FormatNotSupported otherwise. No coercion, no
// rounding, no nearest-anything.
std::expected<PcmFormat, Error> match(const PcmFormat& file, const DeviceCaps& dev);

} // namespace transporter::engine

#endif
