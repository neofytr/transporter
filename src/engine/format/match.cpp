// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/engine/format_match.hpp>

#include <algorithm>
#include <string>

namespace transporter::engine {

namespace {

std::string format_list(std::span<const SampleFormat> fs) {
    std::string out;
    out.reserve(fs.size() * 8);
    for (std::size_t i = 0; i < fs.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += sample_format_name(fs[i]);
    }
    return out;
}

std::string rate_list(std::span<const std::uint32_t> rs) {
    std::string out;
    out.reserve(rs.size() * 7);
    for (std::size_t i = 0; i < rs.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += std::to_string(rs[i]);
    }
    return out;
}

} // namespace

std::expected<PcmFormat, Error> match(const PcmFormat& file, const DeviceCaps& dev) {
    if (file.channels < dev.min_channels || file.channels > dev.max_channels) {
        std::string msg = "channel count ";
        msg += std::to_string(file.channels);
        msg += " not in device range [";
        msg += std::to_string(dev.min_channels);
        msg += ',';
        msg += std::to_string(dev.max_channels);
        msg += ']';
        return std::unexpected(Error{ErrorCode::FormatNotSupported, std::move(msg),
                                     FormatRejection::ChannelCountNotSupported});
    }

    const auto& rs = dev.rates;
    if (std::find(rs.begin(), rs.end(), file.sample_rate_hz) == rs.end()) {
        std::string msg = "rate ";
        msg += std::to_string(file.sample_rate_hz);
        msg += " Hz not in device set {";
        msg += rate_list(rs);
        msg += '}';
        return std::unexpected(Error{ErrorCode::FormatNotSupported, std::move(msg),
                                     FormatRejection::RateNotSupported});
    }

    const auto& fs = dev.formats;
    if (std::find(fs.begin(), fs.end(), file.sample_format) == fs.end()) {
        std::string msg = "sample format ";
        msg += sample_format_name(file.sample_format);
        msg += " not in device set {";
        msg += format_list(fs);
        msg += '}';
        return std::unexpected(Error{ErrorCode::FormatNotSupported, std::move(msg),
                                     FormatRejection::SampleFormatNotSupported});
    }

    return file;
}

} // namespace transporter::engine
