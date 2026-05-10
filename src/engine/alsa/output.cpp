// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/engine/alsa_output.hpp>

#include <alsa/asoundlib.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace transporter::engine::alsa {

namespace {

// RAII for snd_pcm_t.
struct PcmCloser {
    void operator()(snd_pcm_t* p) const noexcept {
        if (p) {
            snd_pcm_close(p);
        }
    }
};
using PcmHandle = std::unique_ptr<snd_pcm_t, PcmCloser>;

// RAII for snd_pcm_hw_params_t (alloca-style allocator pair).
struct HwParamsFreer {
    void operator()(snd_pcm_hw_params_t* p) const noexcept {
        if (p) {
            snd_pcm_hw_params_free(p);
        }
    }
};
using HwParams = std::unique_ptr<snd_pcm_hw_params_t, HwParamsFreer>;

HwParams make_hw_params() {
    snd_pcm_hw_params_t* raw = nullptr;
    if (snd_pcm_hw_params_malloc(&raw) < 0) {
        return HwParams{};
    }
    return HwParams{raw};
}

snd_pcm_format_t to_alsa_format(SampleFormat f) noexcept {
    switch (f) {
    case SampleFormat::S16_LE:
        return SND_PCM_FORMAT_S16_LE;
    case SampleFormat::S24_LE:
        return SND_PCM_FORMAT_S24_LE;
    case SampleFormat::S24_3LE:
        return SND_PCM_FORMAT_S24_3LE;
    case SampleFormat::S32_LE:
        return SND_PCM_FORMAT_S32_LE;
    case SampleFormat::FLOAT_LE:
        return SND_PCM_FORMAT_FLOAT_LE;
    }
    return SND_PCM_FORMAT_UNKNOWN;
}

constexpr std::array<SampleFormat, 5> ALL_FORMATS = {
    SampleFormat::S16_LE, SampleFormat::S24_LE, SampleFormat::S24_3LE,
    SampleFormat::S32_LE, SampleFormat::FLOAT_LE,
};

// Rates we probe. Standard PCM rates the spec admits; the device picks which
// it actually accepts. test_rate is per-rate and authoritative.
constexpr std::array<std::uint32_t, 11> CANDIDATE_RATES = {
    8000,  11025, 16000,  22050,  32000,  44100,
    48000, 88200, 96000, 176400, 192000,
};

std::unexpected<Error> alsa_err(ErrorCode code, const std::string& what, int err) {
    std::string msg = what;
    msg += ": ";
    msg += snd_strerror(err);
    return std::unexpected(Error{code, std::move(msg)});
}

ErrorCode classify_open_error(int err) noexcept {
    // -EBUSY when another process holds the hw: device.
    if (err == -EBUSY) {
        return ErrorCode::DeviceBusy;
    }
    return ErrorCode::DeviceOpenFailed;
}

} // namespace

struct Output::Impl {
    PcmHandle pcm;
    PcmFormat fmt{};
    PeriodInfo periods{};
};

Output::Output(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Output::Output(Output&&) noexcept = default;
Output& Output::operator=(Output&&) noexcept = default;
Output::~Output() {
    if (impl_) {
        drain_and_close();
    }
}

Output::PeriodInfo Output::period_info() const noexcept { return impl_->periods; }
const PcmFormat& Output::format() const noexcept { return impl_->fmt; }

std::expected<DeviceCapsStorage, Error> probe(const std::string& hw_name) {
    snd_pcm_t* raw = nullptr;
    const int open_err = snd_pcm_open(&raw, hw_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (open_err < 0) {
        return alsa_err(classify_open_error(open_err),
                        "snd_pcm_open(" + hw_name + ")", open_err);
    }
    PcmHandle pcm{raw};

    HwParams hwp = make_hw_params();
    if (!hwp) {
        return std::unexpected(Error{ErrorCode::DeviceParamsRejected,
                                     "snd_pcm_hw_params_malloc failed"});
    }
    const int any_err = snd_pcm_hw_params_any(pcm.get(), hwp.get());
    if (any_err < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "snd_pcm_hw_params_any", any_err);
    }

    DeviceCapsStorage caps{};

    for (const SampleFormat f : ALL_FORMATS) {
        const snd_pcm_format_t af = to_alsa_format(f);
        if (snd_pcm_hw_params_test_format(pcm.get(), hwp.get(), af) == 0) {
            caps.formats.push_back(f);
        }
    }

    for (const std::uint32_t r : CANDIDATE_RATES) {
        if (snd_pcm_hw_params_test_rate(pcm.get(), hwp.get(), r, 0) == 0) {
            caps.rates.push_back(r);
        }
    }

    unsigned int min_ch = 0;
    unsigned int max_ch = 0;
    int rc = snd_pcm_hw_params_get_channels_min(hwp.get(), &min_ch);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected,
                        "snd_pcm_hw_params_get_channels_min", rc);
    }
    rc = snd_pcm_hw_params_get_channels_max(hwp.get(), &max_ch);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected,
                        "snd_pcm_hw_params_get_channels_max", rc);
    }
    caps.min_channels = static_cast<std::uint16_t>(min_ch);
    caps.max_channels = static_cast<std::uint16_t>(max_ch);

    return caps;
}

std::expected<Output, Error> Output::open(const std::string& hw_name, const PcmFormat& fmt) {
    const snd_pcm_format_t af = to_alsa_format(fmt.sample_format);
    if (af == SND_PCM_FORMAT_UNKNOWN) {
        return std::unexpected(Error{ErrorCode::DeviceParamsRejected,
                                     "internal: unknown sample format"});
    }

    snd_pcm_t* raw = nullptr;
    // Blocking mode (flags = 0). Phase 5 will revisit non-blocking + poll.
    const int open_err = snd_pcm_open(&raw, hw_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (open_err < 0) {
        return alsa_err(classify_open_error(open_err),
                        "snd_pcm_open(" + hw_name + ")", open_err);
    }
    PcmHandle pcm{raw};

    HwParams hwp = make_hw_params();
    if (!hwp) {
        return std::unexpected(Error{ErrorCode::DeviceParamsRejected,
                                     "snd_pcm_hw_params_malloc failed"});
    }
    int rc = snd_pcm_hw_params_any(pcm.get(), hwp.get());
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "snd_pcm_hw_params_any", rc);
    }

    rc = snd_pcm_hw_params_set_access(pcm.get(), hwp.get(), SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "set_access(RW_INTERLEAVED)", rc);
    }
    rc = snd_pcm_hw_params_set_format(pcm.get(), hwp.get(), af);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "set_format(exact)", rc);
    }
    rc = snd_pcm_hw_params_set_channels(pcm.get(), hwp.get(), fmt.channels);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "set_channels(exact)", rc);
    }
    rc = snd_pcm_hw_params_set_rate(pcm.get(), hwp.get(), fmt.sample_rate_hz, 0);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "set_rate(exact)", rc);
    }

    // Period sizing: target ~12 ms at the active rate; pick the largest
    // accepted value <= target via per-frame-count test, then set exact.
    // Falls back to the device's minimum if even that is rejected at our
    // target. No `_near` calls.
    const std::uint64_t target_period_frames =
        (static_cast<std::uint64_t>(fmt.sample_rate_hz) * 12u) / 1000u;

    snd_pcm_uframes_t period_min = 0;
    snd_pcm_uframes_t period_max = 0;
    int dir = 0;
    rc = snd_pcm_hw_params_get_period_size_min(hwp.get(), &period_min, &dir);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "get_period_size_min", rc);
    }
    dir = 0;
    rc = snd_pcm_hw_params_get_period_size_max(hwp.get(), &period_max, &dir);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "get_period_size_max", rc);
    }

    snd_pcm_uframes_t period_frames =
        static_cast<snd_pcm_uframes_t>(target_period_frames);
    if (period_frames < period_min) {
        period_frames = period_min;
    }
    if (period_frames > period_max) {
        period_frames = period_max;
    }
    dir = 0;
    rc = snd_pcm_hw_params_set_period_size(pcm.get(), hwp.get(), period_frames, dir);
    if (rc < 0) {
        // Some devices accept only specific period sizes; retry at min (exact).
        dir = 0;
        rc = snd_pcm_hw_params_set_period_size(pcm.get(), hwp.get(), period_min, dir);
        if (rc < 0) {
            return alsa_err(ErrorCode::DeviceParamsRejected, "set_period_size(exact)", rc);
        }
        period_frames = period_min;
    }

    // Periods: target 4. Clamp to device range, then set exact.
    unsigned int periods_min = 0;
    unsigned int periods_max = 0;
    dir = 0;
    rc = snd_pcm_hw_params_get_periods_min(hwp.get(), &periods_min, &dir);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "get_periods_min", rc);
    }
    dir = 0;
    rc = snd_pcm_hw_params_get_periods_max(hwp.get(), &periods_max, &dir);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "get_periods_max", rc);
    }
    unsigned int periods = 4;
    if (periods < periods_min) {
        periods = periods_min;
    }
    if (periods > periods_max) {
        periods = periods_max;
    }
    dir = 0;
    rc = snd_pcm_hw_params_set_periods(pcm.get(), hwp.get(), periods, dir);
    if (rc < 0) {
        dir = 0;
        rc = snd_pcm_hw_params_set_periods(pcm.get(), hwp.get(), periods_min, dir);
        if (rc < 0) {
            return alsa_err(ErrorCode::DeviceParamsRejected, "set_periods(exact)", rc);
        }
        periods = periods_min;
    }

    rc = snd_pcm_hw_params(pcm.get(), hwp.get());
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "snd_pcm_hw_params(commit)", rc);
    }

    snd_pcm_uframes_t buffer_frames = 0;
    rc = snd_pcm_hw_params_get_buffer_size(hwp.get(), &buffer_frames);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "get_buffer_size", rc);
    }
    snd_pcm_uframes_t actual_period = 0;
    dir = 0;
    rc = snd_pcm_hw_params_get_period_size(hwp.get(), &actual_period, &dir);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "get_period_size", rc);
    }
    unsigned int actual_periods = 0;
    dir = 0;
    rc = snd_pcm_hw_params_get_periods(hwp.get(), &actual_periods, &dir);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "get_periods", rc);
    }

    rc = snd_pcm_prepare(pcm.get());
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceParamsRejected, "snd_pcm_prepare", rc);
    }

    auto impl = std::make_unique<Impl>();
    impl->pcm = std::move(pcm);
    impl->fmt = fmt;
    impl->periods.period_frames = static_cast<std::uint32_t>(actual_period);
    impl->periods.periods = actual_periods;
    impl->periods.buffer_frames = static_cast<std::uint32_t>(buffer_frames);
    return Output{std::move(impl)};
}

std::expected<void, Error>
Output::write_all(std::span<const std::byte> interleaved_frames) {
    if (!impl_ || !impl_->pcm) {
        return std::unexpected(Error{ErrorCode::WriteFailed, "device not open"});
    }
    const unsigned frame_bytes = impl_->fmt.frame_bytes();
    if (frame_bytes == 0 || (interleaved_frames.size() % frame_bytes) != 0) {
        return std::unexpected(Error{ErrorCode::WriteFailed,
                                     "write payload not aligned to frame size"});
    }
    snd_pcm_t* pcm = impl_->pcm.get();
    const std::byte* p = interleaved_frames.data();
    std::size_t bytes_remaining = interleaved_frames.size();

    while (bytes_remaining > 0) {
        const snd_pcm_uframes_t frames =
            static_cast<snd_pcm_uframes_t>(bytes_remaining / frame_bytes);
        const snd_pcm_sframes_t written = snd_pcm_writei(pcm, p, frames);
        if (written < 0) {
            const int err = static_cast<int>(written);
            if (err == -EPIPE || err == -ESTRPIPE) {
                const int rec = snd_pcm_recover(pcm, err, 1);
                if (rec < 0) {
                    return alsa_err(ErrorCode::WriteFailed, "snd_pcm_recover", rec);
                }
                continue;
            }
            return alsa_err(ErrorCode::WriteFailed, "snd_pcm_writei", err);
        }
        const std::size_t advance =
            static_cast<std::size_t>(written) * static_cast<std::size_t>(frame_bytes);
        p += advance;
        bytes_remaining -= advance;
    }
    return {};
}

void Output::drain_and_close() noexcept {
    if (!impl_) {
        return;
    }
    if (impl_->pcm) {
        // Drain blocks until the buffer flushes. If the device is in an error
        // state we still want close to release the kernel handle: drop is the
        // safe primitive there.
        const snd_pcm_state_t st = snd_pcm_state(impl_->pcm.get());
        if (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_DRAINING) {
            snd_pcm_drain(impl_->pcm.get());
        } else {
            snd_pcm_drop(impl_->pcm.get());
        }
        impl_->pcm.reset();
    }
}

} // namespace transporter::engine::alsa
