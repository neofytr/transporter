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

    std::expected<std::size_t, Error>
    write_all(std::span<const std::byte> interleaved) override {
        return out_.write_all(interleaved);
    }

    void drop_and_close() noexcept override { out_.drain_and_close(); }

    PeriodInfo period_info() const noexcept override {
        const auto p = out_.period_info();
        PeriodInfo r{};
        r.period_frames = p.period_frames;
        r.periods = p.periods;
        r.buffer_frames = p.buffer_frames;
        return r;
    }

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

    std::expected<std::unique_ptr<IOutput>, Error>
    open(const PcmFormat& fmt, const OpenOpts& opts) override {
        alsa::OpenOptions ao{};
        ao.target_period_ms = opts.target_period_ms;
        ao.periods_target = opts.periods_target;
        ao.xrun_observer = opts.xrun_cb;
        auto o = alsa::Output::open(hw_name_, fmt, ao);
        if (!o) {
            return std::unexpected(o.error());
        }
        return std::make_unique<AlsaOutputAdapter>(std::move(*o));
    }

    const std::string& hw_name() const noexcept { return hw_name_; }

private:
    std::string hw_name_;
};

} // namespace transporter::engine::detail

#endif
