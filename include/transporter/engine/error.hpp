// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_ERROR_HPP
#define TRANSPORTER_ENGINE_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace transporter::engine {

enum class ErrorCode : std::uint8_t {
    FileOpenFailed,
    WavMalformed,
    WavUnsupportedTag,
    FormatNotSupported,
    DeviceOpenFailed,
    DeviceBusy,
    DeviceParamsRejected,
    WriteFailed,
};

// Sub-reason for FormatNotSupported so the test driver can be specific
// without parsing free-form text.
enum class FormatRejection : std::uint8_t {
    None,
    RateNotSupported,
    SampleFormatNotSupported,
    ChannelCountNotSupported,
};

constexpr std::string_view error_code_name(ErrorCode c) noexcept {
    switch (c) {
    case ErrorCode::FileOpenFailed:
        return "FileOpenFailed";
    case ErrorCode::WavMalformed:
        return "WavMalformed";
    case ErrorCode::WavUnsupportedTag:
        return "WavUnsupportedTag";
    case ErrorCode::FormatNotSupported:
        return "FormatNotSupported";
    case ErrorCode::DeviceOpenFailed:
        return "DeviceOpenFailed";
    case ErrorCode::DeviceBusy:
        return "DeviceBusy";
    case ErrorCode::DeviceParamsRejected:
        return "DeviceParamsRejected";
    case ErrorCode::WriteFailed:
        return "WriteFailed";
    }
    return "?";
}

struct Error {
    ErrorCode code;
    std::string message;
    FormatRejection rejection = FormatRejection::None;

    Error(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}
    Error(ErrorCode c, std::string m, FormatRejection r)
        : code(c), message(std::move(m)), rejection(r) {}
};

} // namespace transporter::engine

#endif
