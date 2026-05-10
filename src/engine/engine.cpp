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
#include <transporter/engine/device.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/format_match.hpp>
#include <transporter/engine/ring.hpp>
#include <transporter/engine/rt.hpp>
#include <transporter/engine/telemetry.hpp>
#include <transporter/engine/trace.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <ostream>
#include <span>
#include <string>
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

    // Telemetry — atomic counters live here. Audio thread bumps with
    // memory_order_relaxed (monotonic, single producer); other threads read
    // with memory_order_relaxed too (no ordering guarantees needed for
    // statistics).
    std::atomic<std::uint64_t> frames_decoded{0};
    std::atomic<std::uint64_t> frames_written{0};
    std::atomic<std::uint32_t> xrun_count{0};
    std::atomic<std::size_t> ring_fill_bytes{0};      // last-known consumer view
    std::atomic<std::size_t> ring_max_watermark{0};   // session-wide max producer fill
    std::atomic<bool> mismatch_in_flight{false};
    std::atomic<std::uint64_t> last_xrun_ns{0};

    // Decoder thread state (atomically tagged for the snapshot's "thread_state").
    enum class DecodeState : std::uint8_t {
        Idle, Decoding, BlockedRingFull, Eof, Error
    };
    std::atomic<DecodeState> decode_state{DecodeState::Idle};

    // Source-stage details captured at load time. Read-only during the
    // run; guarded by source_mtx for swapping in new track info.
    mutable std::mutex source_mtx;
    SourceStage source_stage_;       // populated after decoder open + match
    FormatMatchStage format_match_stage_;
    OutputStage output_stage_static_; // period/periods/buffer/hw_params from open

    // Realtime status as observed by the audio thread (set once on entry).
    std::mutex rt_mtx;
    rt::Status rt_status_;

    // Trace ring + drainer thread + rolling log. Capacity is a power of two
    // (1024 events @ 32 B = 32 KiB). The drainer wakes every 100 ms and
    // copies into the rolling log; the log is bounded so old events drop.
    static constexpr std::size_t TRACE_RING_CAP = 1024;
    static constexpr std::size_t TRACE_LOG_CAP = 4096;
    std::unique_ptr<trace::Ring> trace_ring;
    mutable std::mutex trace_log_mtx;
    std::vector<trace::Event> trace_log;     // rolling, oldest at front
    std::thread trace_drain_thread;
    std::atomic<bool> trace_drain_run{false};

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
            decode_state.store(DecodeState::Error, std::memory_order_relaxed);
            run_cv.notify_all();
            return;
        }
        const PcmFormat fmt = decoder->format();
        const unsigned frame_bytes = fmt.frame_bytes();
        constexpr std::size_t MAX_FRAMES = 4096;
        // Heap-allocate once; no allocations in the loop.
        std::vector<std::byte> buf(MAX_FRAMES * frame_bytes);
        decode_state.store(DecodeState::Decoding, std::memory_order_relaxed);

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
                decode_state.store(DecodeState::BlockedRingFull,
                                   std::memory_order_relaxed);
                std::this_thread::sleep_for(1ms);
                continue;
            }
            decode_state.store(DecodeState::Decoding, std::memory_order_relaxed);
            auto r = decoder->read(std::span<std::byte>(buf), MAX_FRAMES);
            if (!r) {
                last_audio_error = r.error();
                audio_error.store(true, std::memory_order_release);
                decoder_eof.store(true, std::memory_order_release);
                decode_state.store(DecodeState::Error, std::memory_order_relaxed);
                if (trace_ring) {
                    trace_ring->push(trace::Event{
                        trace::monotonic_ns_now(),
                        trace::Kind::DecodeError, 0, 0, 0, 0, 0});
                }
                run_cv.notify_all();
                return;
            }
            if (*r == 0) {
                decoder_eof.store(true, std::memory_order_release);
                decode_state.store(DecodeState::Eof, std::memory_order_relaxed);
                if (trace_ring) {
                    trace_ring->push(trace::Event{
                        trace::monotonic_ns_now(),
                        trace::Kind::DecodeEof, 0, 0, 0, 0, 0});
                }
                run_cv.notify_all();
                return;
            }
            frames_decoded.fetch_add(static_cast<std::uint64_t>(*r),
                                     std::memory_order_relaxed);
            const std::size_t bytes = *r * frame_bytes;
            std::size_t off = 0;
            while (off < bytes && decoder_run.load(std::memory_order_acquire)) {
                const std::size_t wrote = ring->write(
                    std::span<const std::byte>(buf.data() + off, bytes - off));
                off += wrote;
                if (wrote == 0) {
                    decode_state.store(DecodeState::BlockedRingFull,
                                       std::memory_order_relaxed);
                    std::this_thread::sleep_for(1ms);
                }
            }
            // Track session max watermark. The producer side knows the latest
            // `head - tail` after its writes. We approximate via ring.readable()
            // (capacity - writable() would also do).
            const std::size_t fill = ring->readable();
            std::size_t cur_max =
                ring_max_watermark.load(std::memory_order_relaxed);
            while (fill > cur_max &&
                   !ring_max_watermark.compare_exchange_weak(
                       cur_max, fill, std::memory_order_relaxed)) {
                // CAS retry; cur_max updated by failed exchange.
            }
        }
    }

    // Audio thread function. Hot loop: no allocations, no mutex acquisition.
    // Reads from the SPSC ring into a pre-allocated scratch chunk and submits
    // via IOutput::write_all. write_all blocks on writei (kernel throttles to
    // device clock). On error, sets audio_error and exits; engine worker
    // notices and tears down.
    //
    // RT discipline: SCHED_FIFO + mlockall + affinity applied on entry. The
    // hot loop performs only:
    //   - atomic loads/stores (relaxed for counters; acquire/release for
    //     run/pause flags)
    //   - ring read (lock-free)
    //   - output->write_all (libasound writei, RT-safe)
    //   - trace_ring->push (lock-free; falls through if full)
    void audio_thread_fn_() {
        // Apply RT policy first thing on the audio thread itself.
        rt::Policy pol;
        const rt::Status st = rt::apply_to_current_thread(pol);
        {
            std::lock_guard lk(rt_mtx);
            rt_status_ = st;
        }

        if (!output || !ring) {
            audio_done.store(true, std::memory_order_release);
            run_cv.notify_all();
            return;
        }
        const PcmFormat fmt = output->format();
        const unsigned frame_bytes = fmt.frame_bytes();
        const auto pi = output->period_info();
        constexpr std::size_t CHUNK_FRAMES = 1024;
        // Pre-allocated outside the hot loop. From here on no allocations,
        // no lock acquisitions.
        std::vector<std::byte> buf(CHUNK_FRAMES * frame_bytes);

        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = trace::monotonic_ns_now();
            ev.kind = trace::Kind::AudioStart;
            ev.large_a = fmt.sample_rate_hz;
            ev.large_b = pi.period_frames;
            ev.large_c = pi.periods;
            (void)trace_ring->push(ev);
        }

        // Throttle FrameWritten trace to roughly one event per 8 periods to
        // avoid spam at high rates.
        std::uint32_t fw_div = 0;

        while (audio_run.load(std::memory_order_acquire)) {
            if (audio_paused.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            const std::size_t avail = ring->readable();
            ring_fill_bytes.store(avail, std::memory_order_relaxed);
            if (avail == 0) {
                if (decoder_eof.load(std::memory_order_acquire)) {
                    // Decoder finished and ring drained: natural EOF.
                    break;
                }
                std::this_thread::sleep_for(1ms);
                continue;
            }
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
            const std::uint64_t frames = static_cast<std::uint64_t>(*wr);
            frames_written.fetch_add(frames, std::memory_order_relaxed);
            if (trace_ring && (++fw_div & 7u) == 0) {
                trace::Event ev{};
                ev.monotonic_ns = trace::monotonic_ns_now();
                ev.kind = trace::Kind::FrameWritten;
                ev.large_a = static_cast<std::uint32_t>(frames);
                ev.large_b = pi.period_frames;
                (void)trace_ring->push(ev);
            }
        }

        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = trace::monotonic_ns_now();
            ev.kind = trace::Kind::AudioStop;
            (void)trace_ring->push(ev);
        }
        audio_done.store(true, std::memory_order_release);
        run_cv.notify_all();
    }

    // Xrun observer: invoked from the audio thread inside Output::write_all
    // before snd_pcm_recover. Bumps counter and pushes a trace event.
    void on_xrun_(int errno_neg) noexcept {
        const std::uint32_t seq =
            xrun_count.fetch_add(1, std::memory_order_relaxed) + 1u;
        const std::uint64_t now = trace::monotonic_ns_now();
        last_xrun_ns.store(now, std::memory_order_relaxed);
        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = now;
            ev.kind = trace::Kind::Xrun;
            ev.small_a = static_cast<std::uint16_t>(seq & 0xFFFFu);
            ev.large_a = static_cast<std::uint32_t>(-errno_neg);
            (void)trace_ring->push(ev);
            ev.kind = trace::Kind::Recover;
            ev.monotonic_ns = trace::monotonic_ns_now();
            (void)trace_ring->push(ev);
        }
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
        decode_state.store(DecodeState::Idle, std::memory_order_relaxed);
        ring_fill_bytes.store(0, std::memory_order_relaxed);
    }

    // FSM ops, executed on the engine worker.

    // Compute period_ms from EngineConfig::target_latency / periods_target.
    unsigned target_period_ms_() const noexcept {
        const auto total_ms =
            static_cast<unsigned>(cfg.target_latency.count() <= 0
                                      ? 50
                                      : cfg.target_latency.count());
        constexpr unsigned periods = 4u;
        unsigned per_period = total_ms / periods;
        if (per_period == 0) {
            per_period = 1;
        }
        return per_period;
    }

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
        const std::filesystem::path src_path = path;

        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = trace::monotonic_ns_now();
            ev.kind = trace::Kind::DecodeOpen;
            ev.large_a = new_fmt.sample_rate_hz;
            ev.large_b = new_fmt.channels;
            (void)trace_ring->push(ev);
        }

        auto matched = match(new_fmt, caps.view());
        if (!matched) {
            // Mark mismatch in flight for the bit-perfect verdict, and stage
            // it for the snapshot's format-match block.
            mismatch_in_flight.store(true, std::memory_order_relaxed);
            {
                std::lock_guard lk(source_mtx);
                format_match_stage_ = FormatMatchStage{};
                format_match_stage_.declared = new_fmt;
                format_match_stage_.matched_ok = false;
                format_match_stage_.rejection_reason = matched.error().message;
                format_match_stage_.device_format_set = caps.formats;
                format_match_stage_.device_rate_set = caps.rates;
            }
            emit_error_(matched.error());
            if (!output) {
                emit_state_(State::Idle);
            } else {
                emit_state_(State::Playing);
            }
            return std::unexpected(matched.error());
        }
        mismatch_in_flight.store(false, std::memory_order_relaxed);

        // Tear down previous run (if any) before re-opening the device.
        const bool had_run = static_cast<bool>(output);
        const PcmFormat old_fmt = had_run ? output->format() : PcmFormat{};
        teardown_run_();

        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = trace::monotonic_ns_now();
            ev.kind = trace::Kind::RatePrepare;
            ev.large_a = matched->sample_rate_hz;
            (void)trace_ring->push(ev);
        }

        // Open or re-open the device with the new format. Pass period_ms +
        // xrun observer through the IDevice seam.
        detail::OpenOpts oopts{};
        oopts.target_period_ms = target_period_ms_();
        oopts.periods_target = 4u;
        oopts.xrun_cb = [this](int e) noexcept { on_xrun_(e); };
        auto out = device->open(*matched, oopts);
        if (!out) {
            emit_error_(out.error());
            emit_state_(State::Idle);
            return std::unexpected(out.error());
        }
        output = std::move(*out);

        const auto pi = output->period_info();
        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = trace::monotonic_ns_now();
            ev.kind = trace::Kind::RateLocked;
            ev.large_a = matched->sample_rate_hz;
            ev.large_b = pi.period_frames;
            ev.large_c = pi.periods;
            (void)trace_ring->push(ev);
        }

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

        // Reset session-wide counters for the new track.
        frames_decoded.store(0, std::memory_order_relaxed);
        frames_written.store(0, std::memory_order_relaxed);
        xrun_count.store(0, std::memory_order_relaxed);
        ring_max_watermark.store(0, std::memory_order_relaxed);
        ring_fill_bytes.store(0, std::memory_order_relaxed);
        last_xrun_ns.store(0, std::memory_order_relaxed);

        // Stage the source / format-match / output static fields for snapshots.
        {
            std::lock_guard lk(source_mtx);
            source_stage_ = SourceStage{};
            source_stage_.file_path = src_path.string();
            source_stage_.codec_name = codec_name_for_(src_path);
            source_stage_.container = container_for_(src_path);
            source_stage_.decoder_lib_version = decoder_lib_version_for_(src_path);
            source_stage_.bitrate_kbps = 0;  // populated only for lossy formats
            source_stage_.bit_depth_file =
                static_cast<std::uint16_t>(
                    sample_format_bytes_per_sample(new_fmt.sample_format) * 8u);
            source_stage_.channels = new_fmt.channels;
            source_stage_.sample_rate_hz = new_fmt.sample_rate_hz;
            source_stage_.total_frames = total;
            const double secs = decoder ? decoder->duration_seconds() : 0.0;
            source_stage_.duration =
                std::chrono::milliseconds(static_cast<long long>(secs * 1000.0));
            source_stage_.tags = tags;

            format_match_stage_ = FormatMatchStage{};
            format_match_stage_.declared = new_fmt;
            format_match_stage_.matched = *matched;
            format_match_stage_.matched_ok = true;
            format_match_stage_.device_format_set = caps.formats;
            format_match_stage_.device_rate_set = caps.rates;

            output_stage_static_ = OutputStage{};
            output_stage_static_.period_size_frames = pi.period_frames;
            output_stage_static_.periods = pi.periods;
            output_stage_static_.buffer_size_frames = pi.buffer_frames;
            output_stage_static_.hw_params_set = *matched;
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

    static std::string codec_name_for_(const std::filesystem::path& p) {
        const std::string ext = p.extension().string();
        std::string lower = ext;
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + 32);
            }
        }
        if (lower == ".wav")  { return "WAV"; }
        if (lower == ".aiff" || lower == ".aif") { return "AIFF"; }
        if (lower == ".flac") { return "FLAC"; }
        if (lower == ".m4a")  { return "ALAC"; }
        if (lower == ".mp3")  { return "MP3"; }
        if (lower == ".ogg")  { return "Vorbis"; }
        if (lower == ".opus") { return "Opus"; }
        return lower.empty() ? std::string{"unknown"} : lower.substr(1);
    }
    static std::string container_for_(const std::filesystem::path& p) {
        const std::string ext = p.extension().string();
        std::string lower = ext;
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + 32);
            }
        }
        if (lower == ".m4a")  { return "MP4"; }
        if (lower == ".ogg")  { return "Ogg"; }
        if (lower == ".opus") { return "Ogg"; }
        return std::string{"raw"};
    }
    static std::string decoder_lib_version_for_(const std::filesystem::path& p) {
        const std::string ext = p.extension().string();
        std::string lower = ext;
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + 32);
            }
        }
        // Best-effort. Real libs report version strings; the compile-time
        // constants are sufficient for the audit-grade view.
        if (lower == ".flac") { return "libFLAC"; }
        if (lower == ".m4a")  { return "Apple ALAC reference"; }
        if (lower == ".mp3")  { return "libmpg123"; }
        if (lower == ".ogg")  { return "libvorbis"; }
        if (lower == ".opus") { return "libopusfile"; }
        return std::string{"in-house"};
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

    // Drain the trace ring into the rolling log every 100 ms. Lock-free on
    // the producer side; bounded on the consumer side by TRACE_LOG_CAP.
    void trace_drain_fn_() {
        constexpr std::size_t BATCH = 256;
        std::array<trace::Event, BATCH> batch{};
        while (trace_drain_run.load(std::memory_order_acquire)) {
            std::size_t got = 0;
            while ((got = trace_ring->drain(std::span<trace::Event>(
                       batch.data(), batch.size()))) > 0) {
                std::lock_guard lk(trace_log_mtx);
                for (std::size_t i = 0; i < got; ++i) {
                    if (trace_log.size() >= TRACE_LOG_CAP) {
                        // Drop oldest. We accept O(N) here since the drain
                        // thread is non-RT.
                        trace_log.erase(trace_log.begin());
                    }
                    trace_log.push_back(batch[i]);
                }
            }
            std::this_thread::sleep_for(100ms);
        }
        // Final flush after stop signal.
        std::size_t got = 0;
        while ((got = trace_ring->drain(std::span<trace::Event>(
                   batch.data(), batch.size()))) > 0) {
            std::lock_guard lk(trace_log_mtx);
            for (std::size_t i = 0; i < got; ++i) {
                if (trace_log.size() >= TRACE_LOG_CAP) {
                    trace_log.erase(trace_log.begin());
                }
                trace_log.push_back(batch[i]);
            }
        }
    }

    void start_trace_() {
        trace_ring = std::make_unique<trace::Ring>(TRACE_RING_CAP);
        trace_log.reserve(TRACE_LOG_CAP);
        trace_drain_run.store(true, std::memory_order_release);
        trace_drain_thread = std::thread([this] { trace_drain_fn_(); });
    }

    static const char* decode_state_str_(DecodeState s) noexcept {
        switch (s) {
        case DecodeState::Idle:             return "idle";
        case DecodeState::Decoding:         return "decoding";
        case DecodeState::BlockedRingFull:  return "blocked-ring-full";
        case DecodeState::Eof:              return "eof";
        case DecodeState::Error:            return "error";
        }
        return "?";
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
        trace_drain_run.store(false, std::memory_order_release);
        if (trace_drain_thread.joinable()) {
            trace_drain_thread.join();
        }
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
    e->impl_->start_trace_();
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
    e->impl_->start_trace_();
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

namespace {

const char* kind_name(trace::Kind k) noexcept {
    switch (k) {
    case trace::Kind::AudioStart:   return "AudioStart";
    case trace::Kind::AudioStop:    return "AudioStop";
    case trace::Kind::RatePrepare:  return "RatePrepare";
    case trace::Kind::RateLocked:   return "RateLocked";
    case trace::Kind::Xrun:         return "Xrun";
    case trace::Kind::Recover:      return "Recover";
    case trace::Kind::FrameWritten: return "FrameWritten";
    case trace::Kind::DecodeOpen:   return "DecodeOpen";
    case trace::Kind::DecodeEof:    return "DecodeEof";
    case trace::Kind::DecodeError:  return "DecodeError";
    case trace::Kind::DeviceOpen:   return "DeviceOpen";
    case trace::Kind::DeviceClose:  return "DeviceClose";
    case trace::Kind::DeviceLost:   return "DeviceLost";
    case trace::Kind::DeviceReturn: return "DeviceReturn";
    }
    return "?";
}

} // namespace

PipelineSnapshot Engine::pipeline_snapshot() const {
    PipelineSnapshot s{};
    s.engine_state = impl_->state.load(std::memory_order_acquire);
    s.captured_at = std::chrono::steady_clock::now();

    // Pull the staged source / format-match / static output fields under a
    // brief mutex. The audio thread does not touch source_mtx; only the
    // engine worker (on load) and the snapshot reader do.
    PcmFormat fmt_now{};
    {
        std::lock_guard lk(impl_->source_mtx);
        s.source = impl_->source_stage_;
        s.format_match = impl_->format_match_stage_;
        s.output = impl_->output_stage_static_;
    }
    {
        std::lock_guard lk(impl_->fmt_mtx);
        fmt_now = impl_->current_format_;
    }
    s.decoder.output_format = fmt_now;
    s.decoder.frames_produced =
        impl_->frames_decoded.load(std::memory_order_relaxed);
    s.decoder.thread_state = Engine::Impl::decode_state_str_(
        impl_->decode_state.load(std::memory_order_relaxed));

    // Live counters.
    s.output.frames_written =
        impl_->frames_written.load(std::memory_order_relaxed);
    s.output.xrun_count =
        impl_->xrun_count.load(std::memory_order_relaxed);

    // Ring fill: capacity is what the ring was constructed with; live fill
    // is the audio-thread-published readable() snapshot.
    if (impl_->ring) {
        s.ring.capacity_bytes = impl_->ring->capacity();
        const std::size_t fill_b =
            impl_->ring_fill_bytes.load(std::memory_order_relaxed);
        s.ring.fill_bytes = fill_b;
        const unsigned fb = fmt_now.frame_bytes();
        s.ring.fill_frames = fb == 0 ? 0u : fill_b / fb;
        if (fmt_now.sample_rate_hz > 0) {
            s.ring.fill_us = std::chrono::microseconds(
                static_cast<long long>(s.ring.fill_frames * 1'000'000ull /
                                       fmt_now.sample_rate_hz));
        }
        s.ring.max_watermark_bytes =
            impl_->ring_max_watermark.load(std::memory_order_relaxed);
    }

    // Device stage. describe_device() is a real ALSA call when a hw: string
    // is set; for tests/mocks the device id is empty -> we leave stage empty.
    if (!impl_->cfg.device_id.empty()) {
        auto desc = describe_device(impl_->cfg.device_id);
        if (desc) {
            s.device.fingerprint = desc->fingerprint;
            s.device.capabilities = desc->caps;
            s.device.current_hw_string = desc->alsa_hw_string;
        } else {
            s.device.current_hw_string = impl_->cfg.device_id;
        }
    }

    // Realtime stage.
    {
        std::lock_guard lk(impl_->rt_mtx);
        s.realtime.status = impl_->rt_status_;
    }
    s.realtime.trace_dropped =
        impl_->trace_ring ? impl_->trace_ring->dropped() : 0u;

    // Bit-perfect verdict per the locked spec.
    auto& v = s.bit_perfect;
    v.no_resampling_in_flight =
        s.format_match.matched_ok &&
        s.format_match.declared.sample_rate_hz ==
            s.format_match.matched.sample_rate_hz &&
        s.format_match.declared.sample_format ==
            s.format_match.matched.sample_format &&
        s.format_match.declared.channels == s.format_match.matched.channels;
    v.digital_path_bitperfect = v.no_resampling_in_flight; // no DSP/resample/dither in this build
    v.rt_enabled = s.realtime.status.mode == rt::Mode::Fifo;
    v.no_mismatch_in_flight =
        !impl_->mismatch_in_flight.load(std::memory_order_relaxed);
    constexpr std::uint64_t XRUN_WINDOW_NS = 5ull * 1'000'000'000ull;
    const std::uint64_t now = trace::monotonic_ns_now();
    const std::uint64_t last_xrun = impl_->last_xrun_ns.load(std::memory_order_relaxed);
    v.no_recent_xrun = (last_xrun == 0) || (now - last_xrun > XRUN_WINDOW_NS);
    // Volume: this build does not (yet) drive HW volume. Treat as unity until
    // the volume module lands; the spec's "digital path off" branch holds.
    v.volume_unity = true;

    using L = BitPerfectVerdict::Level;
    if (!v.digital_path_bitperfect || !v.no_mismatch_in_flight) {
        v.level = L::No;
    } else if (v.rt_enabled && v.no_recent_xrun && v.volume_unity) {
        v.level = L::Yes;
    } else {
        v.level = L::Qualified;
    }
    if (!v.rt_enabled) {
        v.qualifications.push_back("RT scheduling not granted: " +
                                   s.realtime.status.fallback_reason);
    }
    if (!v.no_recent_xrun) {
        v.qualifications.push_back("xrun within last 5 s");
    }
    if (!v.volume_unity) {
        v.qualifications.push_back("digital volume engaged");
    }
    if (!v.no_mismatch_in_flight) {
        v.qualifications.push_back("format mismatch on most recent load");
    }
    if (!s.realtime.status.memlocked) {
        v.qualifications.push_back("mlockall failed (RLIMIT_MEMLOCK)");
    }

    return s;
}

void Engine::dump_trace(std::ostream& os) const {
    std::vector<trace::Event> snap;
    {
        std::lock_guard lk(impl_->trace_log_mtx);
        snap = impl_->trace_log;
    }
    for (const auto& e : snap) {
        os << e.monotonic_ns << ' ' << kind_name(e.kind) << ' '
           << "small_a=" << e.small_a << ' '
           << "large_a=" << e.large_a << ' '
           << "large_b=" << e.large_b << ' '
           << "large_c=" << e.large_c << '\n';
    }
}

} // namespace transporter::engine
