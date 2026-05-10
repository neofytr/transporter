// SPDX-License-Identifier: GPL-3.0-or-later

#include "loopback_capture.hpp"

#include <alsa/asoundlib.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace transporter::testing {

namespace tp = transporter::engine;

namespace {

snd_pcm_format_t to_alsa_format(tp::SampleFormat f) noexcept {
    switch (f) {
    case tp::SampleFormat::S16_LE:   return SND_PCM_FORMAT_S16_LE;
    case tp::SampleFormat::S24_LE:   return SND_PCM_FORMAT_S24_LE;
    case tp::SampleFormat::S24_3LE:  return SND_PCM_FORMAT_S24_3LE;
    case tp::SampleFormat::S32_LE:   return SND_PCM_FORMAT_S32_LE;
    case tp::SampleFormat::FLOAT_LE: return SND_PCM_FORMAT_FLOAT_LE;
    }
    return SND_PCM_FORMAT_UNKNOWN;
}

std::unexpected<tp::Error> alsa_err(tp::ErrorCode code, const std::string& what, int err) {
    std::string msg = what + ": " + snd_strerror(err);
    return std::unexpected(tp::Error{code, std::move(msg)});
}

bool buffer_is_silent(const std::byte* p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

} // namespace

struct LoopbackCapture::Impl {
    snd_pcm_t* pcm = nullptr;
    LoopbackCaptureConfig cfg{};
    std::vector<std::byte> bytes;
    std::thread thr;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};

    ~Impl() {
        stop_requested.store(true, std::memory_order_release);
        if (thr.joinable()) {
            thr.join();
        }
        if (pcm) {
            snd_pcm_close(pcm);
            pcm = nullptr;
        }
    }
};

LoopbackCapture::LoopbackCapture() = default;
LoopbackCapture::~LoopbackCapture() = default;
LoopbackCapture::LoopbackCapture(LoopbackCapture&&) noexcept = default;
LoopbackCapture& LoopbackCapture::operator=(LoopbackCapture&&) noexcept = default;

std::expected<void, tp::Error>
LoopbackCapture::start(const LoopbackCaptureConfig& cfg) {
    auto impl = std::make_unique<Impl>();
    impl->cfg = cfg;

    snd_pcm_t* raw = nullptr;
    int rc = snd_pcm_open(&raw, cfg.hw_name.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceOpenFailed,
                        "snd_pcm_open(capture, " + cfg.hw_name + ")", rc);
    }
    impl->pcm = raw;

    snd_pcm_hw_params_t* hwp = nullptr;
    if (snd_pcm_hw_params_malloc(&hwp) < 0) {
        return std::unexpected(tp::Error{tp::ErrorCode::DeviceParamsRejected,
                                         "snd_pcm_hw_params_malloc(capture) failed"});
    }
    auto hwp_guard = std::unique_ptr<snd_pcm_hw_params_t,
                                     void (*)(snd_pcm_hw_params_t*)>{
        hwp, snd_pcm_hw_params_free};

    rc = snd_pcm_hw_params_any(impl->pcm, hwp);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "snd_pcm_hw_params_any(capture)", rc);
    }
    rc = snd_pcm_hw_params_set_access(impl->pcm, hwp, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "set_access(capture, RW_INTERLEAVED)", rc);
    }
    const snd_pcm_format_t af = to_alsa_format(cfg.format.sample_format);
    if (af == SND_PCM_FORMAT_UNKNOWN) {
        return std::unexpected(tp::Error{tp::ErrorCode::DeviceParamsRejected,
                                         "capture: unknown sample format"});
    }
    rc = snd_pcm_hw_params_set_format(impl->pcm, hwp, af);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "set_format(capture, exact)", rc);
    }
    rc = snd_pcm_hw_params_set_channels(impl->pcm, hwp, cfg.format.channels);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "set_channels(capture, exact)", rc);
    }
    rc = snd_pcm_hw_params_set_rate(impl->pcm, hwp, cfg.format.sample_rate_hz, 0);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "set_rate(capture, exact)", rc);
    }
    // snd-aloop is permissive about period sizing. We pick a comfortable
    // period and let ALSA snap if it needs to.
    snd_pcm_uframes_t period = cfg.period_frames;
    int dir = 0;
    rc = snd_pcm_hw_params_set_period_size_near(impl->pcm, hwp, &period, &dir);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "set_period_size_near(capture)", rc);
    }
    unsigned periods = 4;
    dir = 0;
    rc = snd_pcm_hw_params_set_periods_near(impl->pcm, hwp, &periods, &dir);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "set_periods_near(capture)", rc);
    }
    rc = snd_pcm_hw_params(impl->pcm, hwp);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "snd_pcm_hw_params(capture commit)", rc);
    }
    rc = snd_pcm_prepare(impl->pcm);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "snd_pcm_prepare(capture)", rc);
    }
    rc = snd_pcm_start(impl->pcm);
    if (rc < 0) {
        return alsa_err(tp::ErrorCode::DeviceParamsRejected,
                        "snd_pcm_start(capture)", rc);
    }

    impl->running.store(true, std::memory_order_release);

    Impl* raw_impl = impl.get();
    impl->thr = std::thread([raw_impl] {
        const unsigned frame_bytes = raw_impl->cfg.format.frame_bytes();
        const std::size_t period_bytes = raw_impl->cfg.period_frames * frame_bytes;
        std::vector<std::byte> chunk(period_bytes);

        // Trailing-silence check: track how many bytes back from the end of
        // bytes vector are zero. When stop has been requested AND the most
        // recent silence_window_ms of audio is all-zero, exit.
        const std::uint64_t silence_window_bytes =
            static_cast<std::uint64_t>(raw_impl->cfg.silence_window_ms.count()) *
            raw_impl->cfg.format.sample_rate_hz / 1000ull *
            static_cast<std::uint64_t>(frame_bytes);

        while (true) {
            const snd_pcm_sframes_t got = snd_pcm_readi(
                raw_impl->pcm, chunk.data(), raw_impl->cfg.period_frames);
            if (got == -EAGAIN) {
                continue;
            }
            if (got == -EPIPE || got == -ESTRPIPE) {
                snd_pcm_recover(raw_impl->pcm, static_cast<int>(got), 1);
                continue;
            }
            if (got < 0) {
                // Capture failed; record nothing more, leave the loop.
                break;
            }
            const std::size_t got_bytes =
                static_cast<std::size_t>(got) * frame_bytes;
            raw_impl->bytes.insert(raw_impl->bytes.end(),
                                   chunk.begin(),
                                   chunk.begin() +
                                       static_cast<std::ptrdiff_t>(got_bytes));

            if (raw_impl->stop_requested.load(std::memory_order_acquire) &&
                raw_impl->bytes.size() >= silence_window_bytes) {
                const std::byte* tail = raw_impl->bytes.data() +
                                        raw_impl->bytes.size() -
                                        silence_window_bytes;
                if (buffer_is_silent(tail, silence_window_bytes)) {
                    break;
                }
            }
        }
        raw_impl->running.store(false, std::memory_order_release);
    });

    impl_ = std::move(impl);
    return {};
}

void LoopbackCapture::stop() noexcept {
    if (impl_) {
        impl_->stop_requested.store(true, std::memory_order_release);
    }
}

std::vector<std::byte> LoopbackCapture::join() {
    if (!impl_) {
        return {};
    }
    if (impl_->thr.joinable()) {
        impl_->thr.join();
    }
    return std::move(impl_->bytes);
}

} // namespace transporter::testing
