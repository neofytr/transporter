// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine FSM, command dispatch, decoder thread, audio thread.
//
// Threads:
//   - API thread (caller). load/play/pause/stop post a command and return.
//   - Engine worker. Drains commands; runs the FSM; opens decoders; spins up
//     and tears down the audio path; emits Events.
//   - Decoder thread. Pulls frames from the active IDecoder, pushes into the
//     SPSC ring. Sleeps with a short backoff when the ring is full.
//   - Audio thread. Drains the ring via IOutput::write_all. Hot loop holds no
//     locks once the device is prepared (see audio_thread_fn).
//
// Synchronization:
//   - cmd_mtx_ + cmd_cv_ guard the command vector posted by API → engine.
//   - run_mtx_ + run_cv_ coordinate decoder + audio thread lifecycle (start /
//     stop / EOF) with the engine worker.
//   - state_ is std::atomic<State>; readable by any thread without locking.
//   - The audio thread reads only `audio_run_`, the ring, and the IOutput it
//     was given. No mutex acquisition in the hot loop.

#include <transporter/engine/engine.hpp>

#include "alsa_device.hpp"
#include "engine_test_access.hpp"
#include "output_iface.hpp"

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/decoder_factory.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/format_match.hpp>
#include <transporter/engine/ring.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace transporter::engine {

using namespace std::chrono_literals;

namespace {

constexpr std::size_t round_up_pow2(std::size_t n) noexcept {
    if (n < 2) {
        return 2;
    }
    if (std::has_single_bit(n)) {
        return n;
    }
    return std::bit_ceil(n);
}

} // namespace

struct Engine::Impl {
    // Construction-time
    EngineConfig cfg;
    std::unique_ptr<detail::IDevice> device;
    detail::CapsView caps;

    // Public state
    std::atomic<State> state{State::Idle};
    mutable std::mutex fmt_mtx;
    PcmFormat current_format_{};

    // Telemetry
    std::atomic<std::uint64_t> xrun_count{0};

    // Event delivery (single listener, fired on the engine worker)
    std::mutex cb_mtx;
    EventCallback cb;

    // Command queue (API → engine worker)
    enum class CmdKind : std::uint8_t { Load, Play, Pause, Stop, Shutdown };
    struct Cmd {
        CmdKind kind;
        std::filesystem::path path;
    };
    std::mutex cmd_mtx;
    std::condition_variable cmd_cv;
    std::vector<Cmd> cmds;
    bool stop_worker{false};

    // Engine worker thread
    std::thread worker;

    // Active playback runtime (engine worker owns these)
    std::unique_ptr<IDecoder> decoder;
    std::unique_ptr<detail::IOutput> output;
    std::unique_ptr<SpscByteRing> ring;
    std::thread decoder_thread;
    std::thread audio_thread;

    // Decode/audio coordination
    std::mutex run_mtx;
    std::condition_variable run_cv;
    std::atomic<bool> decoder_run{false};
    std::atomic<bool> audio_run{false};
    std::atomic<bool> decoder_eof{false};
    std::atomic<bool> audio_done{false};
    std::atomic<bool> audio_paused{false};
    std::atomic<bool> audio_error{false};
    Error last_audio_error{ErrorCode::WriteFailed, ""};

    void post_event_(Event ev) {
        std::lock_guard lk(cb_mtx);
        if (cb) {
            cb(ev);
        }
    }
    void emit_state_(State s) {
        state.store(s, std::memory_order_release);
        Event ev;
        ev.kind = Event::Kind::StateChanged;
        ev.state = s;
        post_event_(std::move(ev));
    }
    void emit_track_loaded_(const PcmFormat& f, const Tags& t, std::uint64_t total) {
        Event ev;
        ev.kind = Event::Kind::TrackLoaded;
        ev.format = f;
        ev.tags = t;
        ev.total_frames = total;
        post_event_(std::move(ev));
    }
    void emit_rate_switched_(const PcmFormat& f) {
        Event ev;
        ev.kind = Event::Kind::RateSwitched;
        ev.format = f;
        post_event_(std::move(ev));
    }
    void emit_track_ended_() {
        Event ev;
        ev.kind = Event::Kind::TrackEnded;
        post_event_(std::move(ev));
    }
    void emit_error_(Error e) {
        Event ev;
        ev.kind = Event::Kind::ErrorOccurred;
        ev.error = std::move(e);
        post_event_(std::move(ev));
    }

    void post_cmd_(Cmd c) {
        {
            std::lock_guard lk(cmd_mtx);
            cmds.push_back(std::move(c));
        }
        cmd_cv.notify_one();
    }

    // Decoder thread function. Reads frames from `decoder` into a scratch
    // buffer, then memcpy'd into `ring`. Sleeps briefly when the ring is full
    // or when paused.
    void decoder_thread_fn_() {
        if (!decoder || !ring) {
            decoder_eof.store(true, std::memory_order_release);
            run_cv.notify_all();
            return;
        }
        const PcmFormat fmt = decoder->format();
        const unsigned frame_bytes = fmt.frame_bytes();
        constexpr std::size_t MAX_FRAMES = 4096;
        // Heap-allocate once; no allocations in the loop.
        std::vector<std::byte> buf(MAX_FRAMES * frame_bytes);

        while (decoder_run.load(std::memory_order_acquire)) {
            if (audio_paused.load(std::memory_order_acquire)) {
                // Don't decode further while paused; ring already holds frames
                // for instant resume.
                std::this_thread::sleep_for(2ms);
                continue;
            }
            // Throttle when ring is fuller than ~3/4 to avoid spinning.
            const std::size_t free_bytes = ring->writable();
            if (free_bytes < buf.size()) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            auto r = decoder->read(std::span<std::byte>(buf), MAX_FRAMES);
            if (!r) {
                last_audio_error = r.error();
                audio_error.store(true, std::memory_order_release);
                decoder_eof.store(true, std::memory_order_release);
                run_cv.notify_all();
                return;
            }
            if (*r == 0) {
                decoder_eof.store(true, std::memory_order_release);
                run_cv.notify_all();
                return;
            }
            const std::size_t bytes = *r * frame_bytes;
            std::size_t off = 0;
            while (off < bytes && decoder_run.load(std::memory_order_acquire)) {
                const std::size_t wrote = ring->write(
                    std::span<const std::byte>(buf.data() + off, bytes - off));
                off += wrote;
                if (wrote == 0) {
                    std::this_thread::sleep_for(1ms);
                }
            }
        }
    }

    // Audio thread function. Hot loop: no allocations, no mutex acquisition.
    // Reads from the SPSC ring into a pre-allocated scratch chunk and submits
    // via IOutput::write_all. write_all blocks on writei (kernel throttles to
    // device clock). On error, sets audio_error and exits; engine worker
    // notices and tears down.
    void audio_thread_fn_() {
        if (!output || !ring) {
            audio_done.store(true, std::memory_order_release);
            run_cv.notify_all();
            return;
        }
        const PcmFormat fmt = output->format();
        const unsigned frame_bytes = fmt.frame_bytes();
        constexpr std::size_t CHUNK_FRAMES = 1024;
        // Pre-allocated outside the hot loop. From here on no allocations,
        // no lock acquisitions.
        std::vector<std::byte> buf(CHUNK_FRAMES * frame_bytes);

        while (audio_run.load(std::memory_order_acquire)) {
            if (audio_paused.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            const std::size_t avail = ring->readable();
            if (avail == 0) {
                if (decoder_eof.load(std::memory_order_acquire)) {
                    // Decoder finished and ring drained: natural EOF.
                    break;
                }
                std::this_thread::sleep_for(1ms);
                continue;
            }
            // Read a whole-frame-aligned chunk. We pulled into the ring in
            // frame multiples; the count must remain a multiple of frame_bytes.
            std::size_t want = std::min(avail, buf.size());
            want -= (want % frame_bytes);
            if (want == 0) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            const std::size_t got = ring->read(std::span<std::byte>(buf.data(), want));
            if (got == 0) {
                continue;
            }
            auto wr = output->write_all(std::span<const std::byte>(buf.data(), got));
            if (!wr) {
                last_audio_error = wr.error();
                audio_error.store(true, std::memory_order_release);
                break;
            }
        }
        audio_done.store(true, std::memory_order_release);
        run_cv.notify_all();
    }

    // Tear down the active run cleanly: signal threads, join, release.
    // Caller (engine worker) must not be holding run_mtx.
    void teardown_run_() {
        decoder_run.store(false, std::memory_order_release);
        audio_run.store(false, std::memory_order_release);
        run_cv.notify_all();
        if (decoder_thread.joinable()) {
            decoder_thread.join();
        }
        if (audio_thread.joinable()) {
            audio_thread.join();
        }
        if (output) {
            output->drop_and_close();
            output.reset();
        }
        decoder.reset();
        ring.reset();
        decoder_eof.store(false, std::memory_order_release);
        audio_done.store(false, std::memory_order_release);
        audio_paused.store(false, std::memory_order_release);
        audio_error.store(false, std::memory_order_release);
    }

    // FSM ops, executed on the engine worker.

    std::expected<void, Error> open_track_(const std::filesystem::path& path) {
        // Decode + match phase. If matched, opens (or reopens on rate change)
        // the device, allocates the ring, starts the decoder + audio threads.
        emit_state_(State::Loading);

        auto dec = open_decoder(path);
        if (!dec) {
            emit_error_(dec.error());
            // Stay in whatever pre-load state was; emit state Idle to be
            // explicit if no current run exists.
            if (!output) {
                emit_state_(State::Idle);
            } else {
                emit_state_(State::Playing);
            }
            return std::unexpected(dec.error());
        }
        const PcmFormat new_fmt = (*dec)->format();
        const Tags tags = (*dec)->tags();
        const std::uint64_t total = (*dec)->total_frames();

        auto matched = match(new_fmt, caps.view());
        if (!matched) {
            emit_error_(matched.error());
            if (!output) {
                emit_state_(State::Idle);
            } else {
                emit_state_(State::Playing);
            }
            return std::unexpected(matched.error());
        }

        // Tear down previous run (if any) before re-opening the device.
        const bool had_run = static_cast<bool>(output);
        const PcmFormat old_fmt = had_run ? output->format() : PcmFormat{};
        teardown_run_();

        // Open or re-open the device with the new format.
        auto out = device->open(*matched);
        if (!out) {
            emit_error_(out.error());
            emit_state_(State::Idle);
            return std::unexpected(out.error());
        }
        output = std::move(*out);

        const bool rate_switched =
            had_run && (old_fmt.sample_rate_hz != new_fmt.sample_rate_hz ||
                        old_fmt.sample_format != new_fmt.sample_format ||
                        old_fmt.channels != new_fmt.channels);
        if (rate_switched) {
            emit_rate_switched_(new_fmt);
        }

        ring = std::make_unique<SpscByteRing>(round_up_pow2(cfg.ring_capacity_bytes));
        decoder = std::move(*dec);
        {
            std::lock_guard lk(fmt_mtx);
            current_format_ = new_fmt;
        }

        decoder_run.store(true, std::memory_order_release);
        audio_run.store(true, std::memory_order_release);
        audio_paused.store(false, std::memory_order_release);
        decoder_thread = std::thread([this] { decoder_thread_fn_(); });
        audio_thread = std::thread([this] { audio_thread_fn_(); });

        emit_track_loaded_(new_fmt, tags, total);
        emit_state_(State::Playing);
        return {};
    }

    void check_run_finish_() {
        // Called periodically by the worker. If the audio thread has signaled
        // done or error, finalize the run.
        if (audio_done.load(std::memory_order_acquire) ||
            audio_error.load(std::memory_order_acquire)) {
            const bool errored = audio_error.load(std::memory_order_acquire);
            if (errored) {
                emit_error_(last_audio_error);
            } else {
                emit_track_ended_();
            }
            emit_state_(State::Stopped);
            teardown_run_();
            emit_state_(State::Idle);
        }
    }

    void worker_fn_() {
        for (;;) {
            std::vector<Cmd> drained;
            {
                std::unique_lock lk(cmd_mtx);
                cmd_cv.wait_for(lk, 5ms, [&] {
                    return stop_worker || !cmds.empty() ||
                           audio_done.load(std::memory_order_acquire) ||
                           audio_error.load(std::memory_order_acquire);
                });
                if (stop_worker) {
                    break;
                }
                drained.swap(cmds);
            }
            for (auto& c : drained) {
                switch (c.kind) {
                case CmdKind::Load:
                    (void)open_track_(c.path);
                    break;
                case CmdKind::Play:
                    if (output) {
                        audio_paused.store(false, std::memory_order_release);
                        emit_state_(State::Playing);
                    }
                    break;
                case CmdKind::Pause:
                    if (output) {
                        audio_paused.store(true, std::memory_order_release);
                        emit_state_(State::Paused);
                    }
                    break;
                case CmdKind::Stop:
                    if (output) {
                        emit_state_(State::Stopped);
                        teardown_run_();
                        emit_state_(State::Idle);
                    }
                    break;
                case CmdKind::Shutdown:
                    return;
                }
            }
            check_run_finish_();
        }
    }

    Impl() = default;
    ~Impl() {
        // Signal worker.
        {
            std::lock_guard lk(cmd_mtx);
            stop_worker = true;
            cmds.push_back(Cmd{CmdKind::Shutdown, {}});
        }
        cmd_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        teardown_run_();
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

std::expected<std::unique_ptr<Engine>, Error> Engine::create(EngineConfig cfg) {
    auto dev = std::make_unique<detail::AlsaDevice>(cfg.device_id);
    auto caps = dev->probe_caps();
    if (!caps) {
        return std::unexpected(caps.error());
    }
    auto e = std::unique_ptr<Engine>(new Engine());
    e->impl_->cfg = std::move(cfg);
    e->impl_->device = std::move(dev);
    e->impl_->caps = std::move(*caps);
    e->impl_->state.store(State::Idle, std::memory_order_release);
    e->impl_->worker = std::thread([impl = e->impl_.get()] { impl->worker_fn_(); });
    return e;
}

std::expected<std::unique_ptr<Engine>, Error>
EngineTestHooks::create_with_device(EngineConfig cfg,
                                    std::unique_ptr<detail::IDevice> dev) {
    auto caps = dev->probe_caps();
    if (!caps) {
        return std::unexpected(caps.error());
    }
    auto e = std::unique_ptr<Engine>(new Engine());
    e->impl_->cfg = std::move(cfg);
    e->impl_->device = std::move(dev);
    e->impl_->caps = std::move(*caps);
    e->impl_->state.store(State::Idle, std::memory_order_release);
    e->impl_->worker = std::thread([impl = e->impl_.get()] { impl->worker_fn_(); });
    return e;
}

std::expected<void, Error> Engine::load(std::filesystem::path file) {
    if (!std::filesystem::exists(file)) {
        return std::unexpected(Error{ErrorCode::FileOpenFailed,
                                     "no such file: " + file.string()});
    }
    impl_->post_cmd_(Impl::Cmd{Impl::CmdKind::Load, std::move(file)});
    return {};
}

std::expected<void, Error> Engine::play() {
    impl_->post_cmd_(Impl::Cmd{Impl::CmdKind::Play, {}});
    return {};
}

std::expected<void, Error> Engine::pause() {
    impl_->post_cmd_(Impl::Cmd{Impl::CmdKind::Pause, {}});
    return {};
}

std::expected<void, Error> Engine::stop() {
    impl_->post_cmd_(Impl::Cmd{Impl::CmdKind::Stop, {}});
    return {};
}

State Engine::state() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}

PcmFormat Engine::current_format() const noexcept {
    std::lock_guard lk(impl_->fmt_mtx);
    return impl_->current_format_;
}

void Engine::set_event_callback(EventCallback cb) {
    std::lock_guard lk(impl_->cb_mtx);
    impl_->cb = std::move(cb);
}

} // namespace transporter::engine
