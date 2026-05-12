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
#include "device/device_internal.hpp"
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
#include <transporter/hotplug/monitor.hpp>

#include <poll.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
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
    std::atomic<std::uint64_t> frames_written_at_track_start{0};
    std::atomic<std::uint32_t> xrun_count{0};
    std::atomic<std::size_t> ring_fill_bytes{0};      // last-known consumer view
    std::atomic<std::size_t> ring_max_watermark{0};   // session-wide max producer fill
    std::atomic<bool> mismatch_in_flight{false};
    std::atomic<std::uint64_t> last_xrun_ns{0};
    // Digital-volume engagement. GUI flips this through the engine API
    // when the user enables the digital path; bit-perfect verdict drops
    // out of YES while it is true. Default false: no scale stage active.
    std::atomic<bool> digital_volume_active_{false};

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

    // Command queue (API → engine worker, plus hotplug watcher → worker)
    enum class CmdKind : std::uint8_t {
        Load, Play, Pause, Stop, Seek, Shutdown,
        DeviceRemoved, DeviceAdded,
        Preload, CancelPreload,
    };
    struct Cmd {
        CmdKind kind;
        std::filesystem::path path;
        // Hotplug command payload. Filled for DeviceRemoved / DeviceAdded.
        DeviceFingerprint hp_fp{};
        int hp_card_index{-1};
        std::uint64_t seek_frame{0};  // Seek only
    };
    std::mutex cmd_mtx;
    std::condition_variable cmd_cv;
    std::vector<Cmd> cmds;
    bool stop_worker{false};

    // Engine worker thread
    std::thread worker;

    // Hotplug watcher: polls monitor->fd() with a 100 ms timeout, drains
    // events, and posts DeviceRemoved/DeviceAdded commands. The watcher does
    // not touch FSM state; the worker thread owns transitions.
    std::unique_ptr<hotplug::IMonitor> hp_monitor;
    std::thread hp_watcher;
    std::atomic<bool> hp_run{false};

    // Active DAC fingerprint, captured at create() from describe_device().
    // Empty .alsa_card_longname + non-USB => no fingerprint capture
    // (typical for tests with empty device_id; hotplug then no-ops).
    DeviceFingerprint active_fp;
    bool active_fp_known{false};

    // Connection bookkeeping for the snapshot's DeviceStage.
    std::atomic<bool> device_connected{true};
    mutable std::mutex disc_mtx;
    std::chrono::steady_clock::time_point last_disconnected_at{};

    // Pre-disconnect intent: was the engine paused before the DAC vanished?
    // Restored on same-DAC return so a user-paused track resumes paused.
    bool intent_paused_before_disconnect{false};

    // Active track context (file path + last-known matched format) so we can
    // re-open after reconnect. Set on load, cleared on stop / EOF.
    std::filesystem::path active_path;
    bool have_active_track{false};

    // Active playback runtime (engine worker owns these)
    std::unique_ptr<IDecoder> decoder;
    std::unique_ptr<detail::IOutput> output;
    std::unique_ptr<SpscByteRing> ring;
    std::thread decoder_thread;
    std::thread audio_thread;

    // Preloaded next-track decoder. Set by CmdKind::Preload on the worker;
    // consumed by decoder_thread_fn_ on current-track EOF when the format
    // matches exactly. Protected by next_mtx; the decoder thread acquires it
    // only at EOF (infrequent), so there is no contention on the hot path.
    // Mutex is mutable so pipeline_snapshot() (const) can sample the staged
    // next-decoder format for gapless_pending.
    mutable std::mutex next_mtx;
    std::unique_ptr<IDecoder> next_decoder;
    std::filesystem::path next_path;

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
    void emit_track_loaded_(const PcmFormat& f, const Tags& t, std::uint64_t total,
                            std::filesystem::path fpath) {
        Event ev;
        ev.kind = Event::Kind::TrackLoaded;
        ev.format = f;
        ev.tags = t;
        ev.total_frames = total;
        ev.file_path = std::move(fpath);
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
    void emit_device_lost_() {
        Event ev;
        ev.kind = Event::Kind::DeviceLost;
        post_event_(std::move(ev));
    }
    void emit_device_return_() {
        Event ev;
        ev.kind = Event::Kind::DeviceReturn;
        post_event_(std::move(ev));
    }

    // Match by USB triple when available; fall back to ALSA card longname.
    // Empty fingerprint never matches (avoids false positives on non-USB
    // setups where udev may not give us identifiers in time).
    static bool fingerprints_match_(const DeviceFingerprint& a,
                                    const DeviceFingerprint& b) noexcept {
        if (a.is_usb && b.is_usb) {
            if (a.usb_vendor_id.empty() || a.usb_product_id.empty() ||
                b.usb_vendor_id.empty() || b.usb_product_id.empty()) {
                return false;
            }
            if (a.usb_vendor_id != b.usb_vendor_id ||
                a.usb_product_id != b.usb_product_id) {
                return false;
            }
            // Serial is the discriminator when both present; absent on
            // either side => identical model wins (best we can do).
            if (!a.usb_serial.empty() && !b.usb_serial.empty()) {
                return a.usb_serial == b.usb_serial;
            }
            return true;
        }
        // Non-USB: compare longname/cardname.
        const std::string& la =
            a.alsa_card_longname.empty() ? a.alsa_card_name : a.alsa_card_longname;
        const std::string& lb =
            b.alsa_card_longname.empty() ? b.alsa_card_name : b.alsa_card_longname;
        return !la.empty() && la == lb;
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
                // Current decoder reached EOF. Attempt gapless continuation
                // if a next decoder is staged and the formats match exactly.
                std::unique_lock next_lk(next_mtx);
                if (next_decoder &&
                    next_decoder->format().sample_rate_hz == fmt.sample_rate_hz &&
                    next_decoder->format().channels == fmt.channels &&
                    next_decoder->format().sample_format == fmt.sample_format) {
                    // Swap in the next decoder without stopping.
                    decoder = std::move(next_decoder);
                    next_decoder.reset();
                    const std::filesystem::path np = std::move(next_path);
                    next_path.clear();
                    next_lk.unlock();
                    frames_decoded.store(0, std::memory_order_relaxed);
                    frames_written_at_track_start.store(
                        frames_written.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                    const Tags ntags = decoder->tags();
                    const std::uint64_t ntotal = decoder->total_frames();
                    // Update source stage so the snapshot reflects the new
                    // track while the ring continues uninterrupted.
                    {
                        std::lock_guard lk(source_mtx);
                        source_stage_.file_path = np.string();
                        source_stage_.codec_name = codec_name_for_(np);
                        source_stage_.container = container_for_(np);
                        source_stage_.decoder_lib_version = decoder_lib_version_for_(np);
                        source_stage_.bit_depth_file =
                            static_cast<std::uint16_t>(
                                sample_format_bytes_per_sample(fmt.sample_format) * 8u);
                        source_stage_.channels = fmt.channels;
                        source_stage_.sample_rate_hz = fmt.sample_rate_hz;
                        source_stage_.total_frames = ntotal;
                        source_stage_.tags = ntags;
                        const double secs = decoder->duration_seconds();
                        source_stage_.duration =
                            std::chrono::milliseconds(
                                static_cast<long long>(secs * 1000.0));
                    }
                    emit_track_loaded_(fmt, ntags, ntotal, np);
                    if (trace_ring) {
                        trace::Event tev{};
                        tev.monotonic_ns = trace::monotonic_ns_now();
                        tev.kind = trace::Kind::DecodeOpen;
                        tev.large_a = fmt.sample_rate_hz;
                        tev.large_b = fmt.channels;
                        trace_ring->push(tev);
                    }
                    decode_state.store(DecodeState::Decoding, std::memory_order_relaxed);
                    continue;
                }
                next_lk.unlock();
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

    // Seek within the current track. Pauses the audio briefly (~10 ms), stops
    // the decoder thread, re-creates the ring, seeks the decoder, then
    // restarts the decoder thread. Called from the engine worker thread only.
    void seek_frame_(std::uint64_t frame) {
        if (!decoder || !ring) {
            return;
        }
        const bool was_playing =
            !audio_paused.load(std::memory_order_relaxed);

        // Pause audio thread so it stops touching the ring.
        audio_paused.store(true, std::memory_order_release);

        // Stop the decoder thread.
        decoder_run.store(false, std::memory_order_release);
        run_cv.notify_all();
        if (decoder_thread.joinable()) {
            decoder_thread.join();
        }

        // Wait one audio-thread sleep cycle to ensure it has left ring->read().
        std::this_thread::sleep_for(std::chrono::milliseconds(12));

        // Re-create ring: clean slate for the decoder thread to fill from the
        // new position. Audio thread is in its pause loop and not touching it.
        const std::size_t cap = ring->capacity();
        ring = std::make_unique<SpscByteRing>(cap);
        ring_fill_bytes.store(0, std::memory_order_relaxed);
        ring_max_watermark.store(0, std::memory_order_relaxed);
        decoder_eof.store(false, std::memory_order_release);

        // Seek the decoder. Ignore error — worst case it stays at current pos.
        (void)decoder->seek_frame(frame);

        // Update frames_written to reflect the new position. After a gapless
        // swap frames_written_at_track_start is non-zero, so store the global
        // equivalent rather than the bare intra-track frame offset.
        const std::uint64_t base =
            frames_written_at_track_start.load(std::memory_order_relaxed);
        frames_written.store(base + frame, std::memory_order_relaxed);
        // Reset decoded counter so telemetry reflects decoded frames from the
        // new seek position rather than accumulating from before the seek.
        frames_decoded.store(0, std::memory_order_relaxed);

        // Restart decoder thread.
        decoder_run.store(true, std::memory_order_release);
        decoder_thread = std::thread([this] { decoder_thread_fn_(); });

        // Restore play/pause intent.
        audio_paused.store(!was_playing, std::memory_order_release);
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
        // Drop any staged preload: the new run will preload fresh.
        {
            std::lock_guard nlk(next_mtx);
            next_decoder.reset();
            next_path.clear();
        }
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
        frames_written_at_track_start.store(0, std::memory_order_relaxed);
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

        active_path = src_path;
        have_active_track = true;
        intent_paused_before_disconnect = false;

        emit_track_loaded_(new_fmt, tags, total, src_path);
        emit_state_(State::Playing);
        return {};
    }

    // Disconnect path. Active DAC fingerprint just removed. Tear down the
    // audio side cleanly (drop+close the device, stop audio thread), keep
    // the decoder paused with its position preserved, transition state to
    // Disconnected. Decoder thread itself stays alive but pauses on
    // audio_paused; that ring buffer continues to hold whatever was
    // pre-buffered. We do NOT reset session counters: telemetry across the
    // disconnect/reconnect cycle should be coherent.
    void handle_device_removed_() {
        const State s = state.load(std::memory_order_acquire);
        if (s != State::Playing && s != State::Paused) {
            return;
        }
        intent_paused_before_disconnect = (s == State::Paused);

        // Stop the audio thread; close the device. Keep decoder thread alive
        // but paused so the ring keeps its tail position.
        audio_paused.store(true, std::memory_order_release);
        audio_run.store(false, std::memory_order_release);
        run_cv.notify_all();
        if (audio_thread.joinable()) {
            audio_thread.join();
        }
        if (output) {
            output->drop_and_close();
            output.reset();
        }
        device_connected.store(false, std::memory_order_release);
        {
            std::lock_guard lk(disc_mtx);
            last_disconnected_at = std::chrono::steady_clock::now();
        }
        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = trace::monotonic_ns_now();
            ev.kind = trace::Kind::DeviceLost;
            (void)trace_ring->push(ev);
        }
        emit_device_lost_();
        emit_state_(State::Disconnected);
    }

    // Reconnect path. Same fingerprint just reappeared (possibly under a
    // different ALSA card index). Re-resolve the hw string from the new
    // card index, re-probe caps, re-open the device for the in-flight
    // format, restart the audio thread. If the new caps no longer cover
    // the current format -> emit FormatNotSupported, transition to Idle,
    // tear down.
    void handle_device_added_(int new_card_index) {
        if (state.load(std::memory_order_acquire) != State::Disconnected) {
            return;
        }
        if (!have_active_track || !decoder) {
            // Nothing was playing — ignore. Should not normally happen
            // since handle_device_removed_ requires Playing/Paused.
            return;
        }

        // Real-device path: list playback devices and resolve a fresh
        // hw:CARD=...,DEV=N string for the new card index. The new IDevice
        // is bound to that string. Tests don't take this branch — their
        // injected IDevice isn't an AlsaDevice; we keep the existing one
        // and just call open()/probe_caps() against it again.
        const bool is_real_alsa =
            dynamic_cast<detail::AlsaDevice*>(device.get()) != nullptr;
        if (is_real_alsa) {
            int wanted_dev = 0;
            if (auto p = detail::parse_hw_string(cfg.device_id); p) {
                wanted_dev = p->device_index;
            }
            std::string new_hw;
            DeviceFingerprint new_fp;
            if (auto all = list_playback_devices(); all) {
                for (const auto& d : *all) {
                    if (d.alsa_card_index == new_card_index &&
                        d.alsa_device_index == wanted_dev) {
                        new_hw = d.alsa_hw_string;
                        new_fp = d.fingerprint;
                        break;
                    }
                }
                if (new_hw.empty()) {
                    for (const auto& d : *all) {
                        if (d.alsa_card_index == new_card_index) {
                            new_hw = d.alsa_hw_string;
                            new_fp = d.fingerprint;
                            break;
                        }
                    }
                }
            }
            if (new_hw.empty()) {
                // Reconnect candidate didn't materialise as a PCM playback
                // device. Stay in Disconnected — a follow-up Added may
                // resolve once udev settles.
                return;
            }
            device = std::make_unique<detail::AlsaDevice>(new_hw);
            cfg.device_id = new_hw;
            if (active_fp_known && new_fp.is_usb) {
                active_fp = new_fp;
            }
        } else {
            (void)new_card_index;
        }

        // Always re-probe caps after a reopen (firmware may have changed).
        auto probed = device->probe_caps();
        if (!probed) {
            emit_error_(probed.error());
            // Probe failed: tear down the run; reset to Idle.
            teardown_run_();
            have_active_track = false;
            emit_state_(State::Idle);
            return;
        }
        caps = std::move(*probed);

        // Verify the in-flight format still fits the new caps.
        const PcmFormat want = decoder->format();
        auto matched = match(want, caps.view());
        if (!matched) {
            emit_error_(matched.error());
            teardown_run_();
            have_active_track = false;
            emit_state_(State::Idle);
            return;
        }

        detail::OpenOpts oopts{};
        oopts.target_period_ms = target_period_ms_();
        oopts.periods_target = 4u;
        oopts.xrun_cb = [this](int e) noexcept { on_xrun_(e); };
        auto out = device->open(*matched, oopts);
        if (!out) {
            emit_error_(out.error());
            teardown_run_();
            have_active_track = false;
            emit_state_(State::Idle);
            return;
        }
        output = std::move(*out);

        const auto pi = output->period_info();
        if (trace_ring) {
            trace::Event ev{};
            ev.monotonic_ns = trace::monotonic_ns_now();
            ev.kind = trace::Kind::DeviceReturn;
            ev.large_a = matched->sample_rate_hz;
            ev.large_b = pi.period_frames;
            ev.large_c = pi.periods;
            (void)trace_ring->push(ev);
        }

        // Update the static output stage / format-match for the snapshot.
        // hw string and fingerprint were already refreshed above on the
        // real-ALSA branch.
        {
            std::lock_guard lk(source_mtx);
            output_stage_static_.period_size_frames = pi.period_frames;
            output_stage_static_.periods = pi.periods;
            output_stage_static_.buffer_size_frames = pi.buffer_frames;
            output_stage_static_.hw_params_set = *matched;
            format_match_stage_.matched = *matched;
            format_match_stage_.matched_ok = true;
            format_match_stage_.device_format_set = caps.formats;
            format_match_stage_.device_rate_set = caps.rates;
        }

        device_connected.store(true, std::memory_order_release);

        // Restart audio thread.
        audio_run.store(true, std::memory_order_release);
        audio_paused.store(intent_paused_before_disconnect,
                           std::memory_order_release);
        audio_done.store(false, std::memory_order_release);
        audio_error.store(false, std::memory_order_release);
        audio_thread = std::thread([this] { audio_thread_fn_(); });

        emit_device_return_();
        emit_state_(intent_paused_before_disconnect ? State::Paused
                                                    : State::Playing);
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

    // Classify an Error::message produced by Output::write_all to detect
    // device-gone semantics. The message format is "snd_pcm_writei: <strerr>";
    // <strerr> comes from snd_strerror(errno_neg) which is stable across
    // alsa-lib versions. We match on -ENODEV (typical for USB unplug too fast
    // for udev), -ENXIO (kernel saw device leave mid-write), and -EIO (USB
    // transfer aborted; common with a hot-yank). Anything else is treated as
    // a real audio fault.
    static bool error_indicates_device_gone_(const Error& e) noexcept {
        if (e.code != ErrorCode::WriteFailed) {
            return false;
        }
        const std::string& m = e.message;
        return m.find("No such device") != std::string::npos ||
               m.find("Input/output error") != std::string::npos;
    }

    void check_run_finish_() {
        // Called periodically by the worker. If the audio thread has signaled
        // done or error, finalize the run.
        if (state.load(std::memory_order_acquire) == State::Disconnected) {
            // Audio thread has been stopped intentionally; the audio_done
            // flag is irrelevant in this state.
            return;
        }
        if (audio_done.load(std::memory_order_acquire) ||
            audio_error.load(std::memory_order_acquire)) {
            const bool errored = audio_error.load(std::memory_order_acquire);
            if (errored && active_fp_known &&
                error_indicates_device_gone_(last_audio_error)) {
                // Pure write-side disconnect: the kernel saw the device leave
                // mid-write but no udev event has reached us yet (or the
                // device is non-USB and udev won't fire). Take the hotplug
                // path so reconnect resumes naturally. handle_device_removed_
                // is idempotent against a follow-up udev DeviceRemoved.
                // Clear audio_error / audio_done so the worker loop doesn't
                // wake spinning on stale flags; Disconnected -> reconnect
                // will re-arm them via handle_device_added_.
                audio_error.store(false, std::memory_order_release);
                audio_done.store(false, std::memory_order_release);
                handle_device_removed_();
                return;
            }
            if (errored) {
                emit_error_(last_audio_error);
            } else {
                emit_track_ended_();
            }
            emit_state_(State::Stopped);
            teardown_run_();
            have_active_track = false;
            emit_state_(State::Idle);
        }
    }

    // Hotplug watcher loop. Polls monitor->fd() with a 100 ms timeout.
    // When events arrive, drains them and posts DeviceRemoved / DeviceAdded
    // commands for the engine worker to interpret. Filters here are
    // deliberately minimal: only fingerprint matching happens on the worker
    // (so all FSM logic runs on a single thread).
    void hp_watcher_fn_() {
        if (!hp_monitor) {
            return;
        }
        const int fd = hp_monitor->fd();
        std::array<hotplug::DeviceEvent, 8> batch{};
        while (hp_run.load(std::memory_order_acquire)) {
            if (fd >= 0) {
                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;
                const int rc = ::poll(&pfd, 1, 100);
                if (rc < 0) {
                    // EINTR or similar — retry
                    continue;
                }
                if (rc == 0) {
                    continue; // timeout; loop
                }
            } else {
                // No fd (mock with no pipe) — fall back to short sleep poll.
                std::this_thread::sleep_for(50ms);
            }
            const std::size_t n = hp_monitor->poll(
                std::span<hotplug::DeviceEvent>(batch.data(), batch.size()));
            for (std::size_t i = 0; i < n; ++i) {
                Cmd cmd{};
                cmd.kind = batch[i].kind == hotplug::EventKind::Removed
                               ? CmdKind::DeviceRemoved
                               : CmdKind::DeviceAdded;
                cmd.hp_fp = std::move(batch[i].fingerprint);
                cmd.hp_card_index = batch[i].alsa_card_index;
                post_cmd_(std::move(cmd));
            }
        }
    }

    void start_hotplug_(std::unique_ptr<hotplug::IMonitor> mon) {
        if (!mon) {
            return;
        }
        hp_monitor = std::move(mon);
        hp_run.store(true, std::memory_order_release);
        hp_watcher = std::thread([this] { hp_watcher_fn_(); });
    }
    void stop_hotplug_() noexcept {
        hp_run.store(false, std::memory_order_release);
        if (hp_watcher.joinable()) {
            hp_watcher.join();
        }
        hp_monitor.reset();
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
                    if (state.load(std::memory_order_acquire) ==
                        State::Disconnected) {
                        // Honour user intent: when they hit play during a
                        // disconnect, remember it so reconnection resumes
                        // playing rather than paused.
                        intent_paused_before_disconnect = false;
                    } else if (output) {
                        audio_paused.store(false, std::memory_order_release);
                        emit_state_(State::Playing);
                    }
                    break;
                case CmdKind::Pause:
                    if (state.load(std::memory_order_acquire) ==
                        State::Disconnected) {
                        intent_paused_before_disconnect = true;
                    } else if (output) {
                        audio_paused.store(true, std::memory_order_release);
                        emit_state_(State::Paused);
                    }
                    break;
                case CmdKind::Stop:
                    if (state.load(std::memory_order_acquire) ==
                            State::Disconnected ||
                        output) {
                        emit_state_(State::Stopped);
                        teardown_run_();
                        have_active_track = false;
                        emit_state_(State::Idle);
                    }
                    break;
                case CmdKind::Seek:
                    if (have_active_track) {
                        seek_frame_(c.seek_frame);
                    }
                    break;
                case CmdKind::DeviceRemoved:
                    if (active_fp_known &&
                        fingerprints_match_(active_fp, c.hp_fp)) {
                        handle_device_removed_();
                    }
                    break;
                case CmdKind::DeviceAdded:
                    if (state.load(std::memory_order_acquire) ==
                            State::Disconnected &&
                        active_fp_known &&
                        fingerprints_match_(active_fp, c.hp_fp)) {
                        handle_device_added_(c.hp_card_index);
                    }
                    break;
                case CmdKind::Preload:
                    // Open the next-track decoder speculatively. If the
                    // decoder thread is mid-run, it will pick this up at EOF.
                    if (!c.path.empty() && std::filesystem::exists(c.path)) {
                        if (auto nd = open_decoder(c.path); nd) {
                            std::lock_guard nlk(next_mtx);
                            next_decoder = std::move(*nd);
                            next_path = c.path;
                        }
                    }
                    break;
                case CmdKind::CancelPreload: {
                    std::lock_guard nlk(next_mtx);
                    next_decoder.reset();
                    next_path.clear();
                    break;
                }
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
        // Stop hotplug watcher first so it can't post commands into a
        // worker that's about to drain.
        stop_hotplug_();
        // Signal worker.
        {
            std::lock_guard lk(cmd_mtx);
            stop_worker = true;
            cmds.push_back(Cmd{CmdKind::Shutdown, {}, {}, -1});
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
    // Resolve the active DAC fingerprint up front so hotplug events can be
    // matched without a re-probe round-trip. describe_device may fail if
    // the device disappears between probe and describe; that's fine — we
    // simply skip fingerprint capture and the engine never enters
    // Disconnected for this session.
    DeviceFingerprint active_fp_local{};
    bool active_fp_known_local = false;
    if (auto desc = describe_device(cfg.device_id); desc) {
        active_fp_local = desc->fingerprint;
        active_fp_known_local = true;
    }

    HotplugFactory factory = std::move(cfg.hotplug_factory);
    auto e = std::unique_ptr<Engine>(new Engine());
    e->impl_->cfg = std::move(cfg);
    e->impl_->device = std::move(dev);
    e->impl_->caps = std::move(*caps);
    e->impl_->active_fp = std::move(active_fp_local);
    e->impl_->active_fp_known = active_fp_known_local;
    e->impl_->state.store(State::Idle, std::memory_order_release);
    e->impl_->start_trace_();
    e->impl_->worker = std::thread([impl = e->impl_.get()] { impl->worker_fn_(); });

    // Default factory: real udev monitor. Tests inject their own factory
    // (mock) via EngineConfig::hotplug_factory. If the factory returns
    // nullptr (or open_udev_monitor fails on a system without udev access)
    // we silently disable hotplug.
    std::unique_ptr<hotplug::IMonitor> mon;
    if (factory) {
        mon = factory();
    } else {
        if (auto m = hotplug::open_udev_monitor(); m) {
            mon = std::move(*m);
        }
    }
    if (mon) {
        e->impl_->start_hotplug_(std::move(mon));
    }
    return e;
}

std::expected<std::unique_ptr<Engine>, Error>
EngineTestHooks::create_with_device(EngineConfig cfg,
                                    std::unique_ptr<detail::IDevice> dev) {
    return create_with_device_fp(std::move(cfg), std::move(dev),
                                 DeviceFingerprint{});
}

std::expected<std::unique_ptr<Engine>, Error>
EngineTestHooks::create_with_device_fp(EngineConfig cfg,
                                       std::unique_ptr<detail::IDevice> dev,
                                       DeviceFingerprint active_fp) {
    auto caps = dev->probe_caps();
    if (!caps) {
        return std::unexpected(caps.error());
    }
    HotplugFactory factory = std::move(cfg.hotplug_factory);
    auto e = std::unique_ptr<Engine>(new Engine());
    e->impl_->cfg = std::move(cfg);
    e->impl_->device = std::move(dev);
    e->impl_->caps = std::move(*caps);
    e->impl_->active_fp = std::move(active_fp);
    e->impl_->active_fp_known =
        e->impl_->active_fp.is_usb ||
        !e->impl_->active_fp.alsa_card_longname.empty() ||
        !e->impl_->active_fp.alsa_card_name.empty();
    e->impl_->state.store(State::Idle, std::memory_order_release);
    e->impl_->start_trace_();
    e->impl_->worker = std::thread([impl = e->impl_.get()] { impl->worker_fn_(); });

    // Tests pass an explicit factory or none. No real udev probe in tests.
    if (factory) {
        if (auto mon = factory(); mon) {
            e->impl_->start_hotplug_(std::move(mon));
        }
    }
    return e;
}

std::expected<void, Error> Engine::load(std::filesystem::path file) {
    if (!std::filesystem::exists(file)) {
        return std::unexpected(Error{ErrorCode::FileOpenFailed,
                                     "no such file: " + file.string()});
    }
    // Cancel any staged preload before the new Load so the worker doesn't
    // race to swap in a stale next_decoder after teardown_run_() clears it.
    impl_->post_cmd_(Impl::Cmd{Impl::CmdKind::CancelPreload, {}});
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

std::expected<void, Error> Engine::seek(std::uint64_t frame) {
    Impl::Cmd c{};
    c.kind = Impl::CmdKind::Seek;
    c.seek_frame = frame;
    impl_->post_cmd_(std::move(c));
    return {};
}

std::expected<void, Error> Engine::preload(std::filesystem::path file) {
    if (!std::filesystem::exists(file)) {
        return std::unexpected(Error{ErrorCode::FileOpenFailed,
                                     "no such file: " + file.string()});
    }
    impl_->post_cmd_(Impl::Cmd{Impl::CmdKind::Preload, std::move(file)});
    return {};
}

void Engine::cancel_preload() {
    impl_->post_cmd_(Impl::Cmd{Impl::CmdKind::CancelPreload, {}});
}

State Engine::state() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}

PcmFormat Engine::current_format() const noexcept {
    std::lock_guard lk(impl_->fmt_mtx);
    return impl_->current_format_;
}

void Engine::set_digital_volume_active(bool active) noexcept {
    impl_->digital_volume_active_.store(active, std::memory_order_relaxed);
}

bool Engine::digital_volume_active() const noexcept {
    return impl_->digital_volume_active_.load(std::memory_order_relaxed);
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
    s.output.frames_written_at_track_start =
        impl_->frames_written_at_track_start.load(std::memory_order_relaxed);
    s.output.xrun_count =
        impl_->xrun_count.load(std::memory_order_relaxed);

    // Gapless pending: a same-rate decoder is staged for swap on EOF and the
    // current track is within ~2s of its end. The decoder thread runs the
    // actual swap in decoder_thread_fn_; we just observe the same predicate
    // it will hit. Guarded by next_mtx (rare contention -- only on preload
    // posts and the per-snapshot read).
    {
        std::lock_guard nlk(impl_->next_mtx);
        if (impl_->next_decoder && fmt_now.sample_rate_hz > 0) {
            const PcmFormat nfmt = impl_->next_decoder->format();
            const bool fmt_matches =
                nfmt.sample_rate_hz == fmt_now.sample_rate_hz &&
                nfmt.channels == fmt_now.channels &&
                nfmt.sample_format == fmt_now.sample_format;
            const std::uint64_t played =
                s.output.frames_written - s.output.frames_written_at_track_start;
            const std::uint64_t total = s.source.total_frames;
            const std::uint64_t remaining =
                (total > played) ? (total - played) : 0u;
            const std::uint64_t two_sec_frames =
                2ull * fmt_now.sample_rate_hz;
            s.output.gapless_pending =
                fmt_matches && total > 0 && remaining <= two_sec_frames;
        }
    }

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
            // describe_device failed: usually because the device just
            // vanished. Surface the cached active fingerprint so the GUI
            // doesn't lose all identification while disconnected.
            if (impl_->active_fp_known) {
                s.device.fingerprint = impl_->active_fp;
            }
        }
    } else if (impl_->active_fp_known) {
        s.device.fingerprint = impl_->active_fp;
    }
    s.device.is_connected =
        impl_->device_connected.load(std::memory_order_acquire);
    {
        std::lock_guard lk(impl_->disc_mtx);
        s.device.last_disconnected_at = impl_->last_disconnected_at;
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
    v.digital_path_off =
        !impl_->digital_volume_active_.load(std::memory_order_relaxed);
    // Digital path is bit-perfect only when there's no resampling AND the
    // digital-volume scale stage is disengaged. HW volume rides through
    // analog / DAC-internal stages and does not enter this predicate.
    v.digital_path_bitperfect = v.no_resampling_in_flight && v.digital_path_off;
    v.rt_enabled = s.realtime.status.mode == rt::Mode::Fifo;
    v.no_mismatch_in_flight =
        !impl_->mismatch_in_flight.load(std::memory_order_relaxed);
    constexpr std::uint64_t XRUN_WINDOW_NS = 5ull * 1'000'000'000ull;
    const std::uint64_t now = trace::monotonic_ns_now();
    const std::uint64_t last_xrun = impl_->last_xrun_ns.load(std::memory_order_relaxed);
    v.no_recent_xrun = (last_xrun == 0) || (now - last_xrun > XRUN_WINDOW_NS);

    using L = BitPerfectVerdict::Level;
    const bool disconnected = !s.device.is_connected;
    if (disconnected) {
        v.level = L::No;
        v.qualifications.push_back("DAC currently disconnected");
    } else if (!v.digital_path_bitperfect || !v.no_mismatch_in_flight) {
        v.level = L::No;
    } else if (v.rt_enabled && v.no_recent_xrun) {
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
    if (!v.digital_path_off) {
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
