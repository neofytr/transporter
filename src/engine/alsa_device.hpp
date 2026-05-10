// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-internal adapter: wraps alsa::probe / alsa::Output behind IDevice.
// Real binary uses this; tests inject their own IDevice.

#ifndef TRANSPORTER_ENGINE_ALSA_DEVICE_HPP
#define TRANSPORTER_ENGINE_ALSA_DEVICE_HPP

#include "output_iface.hpp"

#include <transporter/engine/alsa_output.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace transporter::engine::detail {

class AlsaOutputAdapter final : public IOutput {
public:
    explicit AlsaOutputAdapter(alsa::Output out) : out_(std::move(out)) {}

    const PcmFormat& format() const noexcept override { return out_.format(); }

    std::expected<void, Error>
    write_all(std::span<const std::byte> interleaved) override {
        return out_.write_all(interleaved);
    }

    void drop_and_close() noexcept override { out_.drain_and_close(); }

private:
    alsa::Output out_;
};

class AlsaDevice final : public IDevice {
public:
    explicit AlsaDevice(std::string hw_name) : hw_name_(std::move(hw_name)) {}

    std::expected<CapsView, Error> probe_caps() override {
        auto p = alsa::probe(hw_name_);
        if (!p) {
            return std::unexpected(p.error());
        }
        CapsView v;
        v.rates = std::move(p->rates);
        v.formats = std::move(p->formats);
        v.min_channels = p->min_channels;
        v.max_channels = p->max_channels;
        return v;
    }

    std::expected<std::unique_ptr<IOutput>, Error>
    open(const PcmFormat& fmt) override {
        auto o = alsa::Output::open(hw_name_, fmt);
        if (!o) {
            return std::unexpected(o.error());
        }
        return std::make_unique<AlsaOutputAdapter>(std::move(*o));
    }

private:
    std::string hw_name_;
};

} // namespace transporter::engine::detail

#endif
