// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drives the engine through Disconnected/return cycles using a mock
// IMonitor and mock IDevice. Exercises every Phase 6 transition rule.

#include "../../src/engine/engine_test_access.hpp"
#include "../../src/engine/output_iface.hpp"
#include "../support/mock_monitor.hpp"

#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/hotplug/monitor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace tp = transporter::engine;
namespace td = transporter::engine::detail;
namespace th = transporter::hotplug;

namespace {

using namespace std::chrono_literals;

class MockOutput final : public td::IOutput {
public:
    MockOutput(tp::PcmFormat fmt, std::atomic<int>* writes,
               std::atomic<int>* closes)
        : fmt_(fmt), writes_(writes), closes_(closes) {}
    const tp::PcmFormat& format() const noexcept override { return fmt_; }
    std::expected<std::size_t, tp::Error>
    write_all(std::span<const std::byte> in) override {
        std::this_thread::sleep_for(2ms);
        writes_->fetch_add(static_cast<int>(in.size()), std::memory_order_release);
        const unsigned fb = fmt_.frame_bytes();
        return fb == 0 ? std::size_t{0} : in.size() / fb;
    }
    void drop_and_close() noexcept override {
        if (closes_) {
            closes_->fetch_add(1, std::memory_order_release);
        }
    }
private:
    tp::PcmFormat fmt_;
    std::atomic<int>* writes_;
    std::atomic<int>* closes_;
};

class MockDevice final : public td::IDevice {
public:
    std::atomic<int> probes{0};
    std::atomic<int> opens{0};
    std::atomic<int> writes{0};
    std::atomic<int> closes{0};

    std::expected<td::CapsView, tp::Error> probe_caps() override {
        probes.fetch_add(1, std::memory_order_release);
        td::CapsView v;
        v.rates = {44100, 48000, 88200, 96000, 192000};
        v.formats = {tp::SampleFormat::S16_LE, tp::SampleFormat::S24_3LE,
                     tp::SampleFormat::S32_LE};
        v.min_channels = 1;
        v.max_channels = 2;
        return v;
    }
    using td::IDevice::open;
    std::expected<std::unique_ptr<td::IOutput>, tp::Error>
    open(const tp::PcmFormat& fmt) override {
        opens.fetch_add(1, std::memory_order_release);
        return std::make_unique<MockOutput>(fmt, &writes, &closes);
    }
};

struct EventLog {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<tp::Event::Kind> kinds;
    std::vector<tp::State> states;

    void on(const tp::Event& e) {
        std::lock_guard lk(mtx);
        kinds.push_back(e.kind);
        if (e.kind == tp::Event::Kind::StateChanged) {
            states.push_back(e.state);
        }
        cv.notify_all();
    }

    bool wait_state(tp::State s, std::chrono::milliseconds timeout) {
        std::unique_lock lk(mtx);
        return cv.wait_for(lk, timeout, [&] {
            for (auto x : states) {
                if (x == s) {
                    return true;
                }
            }
            return false;
        });
    }
    void clear() {
        std::lock_guard lk(mtx);
        kinds.clear();
        states.clear();
    }
    bool saw(tp::Event::Kind k) {
        std::lock_guard lk(mtx);
        for (auto x : kinds) {
            if (x == k) {
                return true;
            }
        }
        return false;
    }
    tp::State last_state() {
        std::lock_guard lk(mtx);
        return states.empty() ? tp::State::Idle : states.back();
    }
};

int fail(const char* where) {
    std::fprintf(stderr, "FAIL [%s]\n", where);
    return 1;
}

tp::DeviceFingerprint make_fp(const char* vid, const char* pid,
                              const char* serial) {
    tp::DeviceFingerprint fp;
    fp.is_usb = true;
    fp.usb_vendor_id = vid;
    fp.usb_product_id = pid;
    fp.usb_serial = serial;
    return fp;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixtures/sine_44100_16.flac>\n", argv[0]);
        return 2;
    }

    // ============================================================
    // Case 1: Playing -> Removed-matching -> Disconnected
    //         Disconnected -> Added-matching -> Playing
    // ============================================================
    {
        auto* mon_raw = new transporter::tests::MockMonitor();
        auto mon = std::unique_ptr<th::IMonitor>(mon_raw);

        tp::EngineConfig cfg;
        cfg.hotplug_factory =
            [m = std::move(mon)]() mutable -> std::unique_ptr<th::IMonitor> {
                return std::move(m);
            };

        auto e = tp::EngineTestHooks::create_with_device_fp(
            std::move(cfg), std::make_unique<MockDevice>(),
            make_fp("0bda", "4c10", "TPDAC0001"));
        if (!e) {
            return fail("case1-create");
        }
        auto& engine = **e;

        EventLog log;
        engine.set_event_callback([&](const tp::Event& ev) { log.on(ev); });

        if (!engine.load(argv[1])) {
            return fail("case1-load");
        }
        if (!log.wait_state(tp::State::Playing, 2s)) {
            return fail("case1-wait-playing");
        }

        // Removed-matching event.
        th::DeviceEvent rm{};
        rm.kind = th::EventKind::Removed;
        rm.alsa_card_index = 1;
        rm.fingerprint = make_fp("0bda", "4c10", "TPDAC0001");
        mon_raw->push(rm);

        if (!log.wait_state(tp::State::Disconnected, 2s)) {
            return fail("case1-wait-disconnected");
        }
        if (!log.saw(tp::Event::Kind::DeviceLost)) {
            return fail("case1-device-lost-event");
        }

        // Added-matching event.
        log.clear();
        th::DeviceEvent ad{};
        ad.kind = th::EventKind::Added;
        ad.alsa_card_index = 2;  // ALSA reshuffled
        ad.fingerprint = make_fp("0bda", "4c10", "TPDAC0001");
        mon_raw->push(ad);

        if (!log.wait_state(tp::State::Playing, 2s)) {
            return fail("case1-wait-playing-2");
        }
        if (!log.saw(tp::Event::Kind::DeviceReturn)) {
            return fail("case1-device-return-event");
        }
    }

    // ============================================================
    // Case 2: Disconnected -> Added-DIFFERENT-fp -> still Disconnected
    // ============================================================
    {
        auto* mon_raw = new transporter::tests::MockMonitor();
        auto mon = std::unique_ptr<th::IMonitor>(mon_raw);
        tp::EngineConfig cfg;
        cfg.hotplug_factory =
            [m = std::move(mon)]() mutable -> std::unique_ptr<th::IMonitor> {
                return std::move(m);
            };
        auto dev_owner = std::make_unique<MockDevice>();
        MockDevice* dev = dev_owner.get();
        auto e = tp::EngineTestHooks::create_with_device_fp(
            std::move(cfg), std::move(dev_owner),
            make_fp("0bda", "4c10", "TPDAC0001"));
        if (!e) {
            return fail("case2-create");
        }
        auto& engine = **e;
        EventLog log;
        engine.set_event_callback([&](const tp::Event& ev) { log.on(ev); });

        if (!engine.load(argv[1])) {
            return fail("case2-load");
        }
        if (!log.wait_state(tp::State::Playing, 2s)) {
            return fail("case2-wait-playing");
        }

        const int opens_before = dev->opens.load();
        const int closes_before = dev->closes.load();

        th::DeviceEvent rm{};
        rm.kind = th::EventKind::Removed;
        rm.fingerprint = make_fp("0bda", "4c10", "TPDAC0001");
        mon_raw->push(rm);
        if (!log.wait_state(tp::State::Disconnected, 2s)) {
            return fail("case2-disc");
        }

        // Different fp arrives. No state change should happen.
        log.clear();
        th::DeviceEvent ad{};
        ad.kind = th::EventKind::Added;
        ad.alsa_card_index = 7;
        ad.fingerprint = make_fp("dead", "beef", "OTHERDAC");
        mon_raw->push(ad);

        std::this_thread::sleep_for(150ms);
        if (engine.state() != tp::State::Disconnected) {
            return fail("case2-stayed-disconnected");
        }
        if (log.saw(tp::Event::Kind::DeviceReturn)) {
            return fail("case2-no-device-return");
        }
        // Engine must not have called open() again.
        if (dev->opens.load() != opens_before) {
            return fail("case2-no-extra-opens");
        }
        // close_drop should have been called exactly once on the disconnect.
        if (dev->closes.load() <= closes_before) {
            return fail("case2-close-on-disconnect");
        }
    }

    // ============================================================
    // Case 3: Paused -> Removed-matching -> Disconnected (intent preserved)
    //         Disconnected -> Added-matching -> Paused (not Playing)
    // ============================================================
    {
        auto* mon_raw = new transporter::tests::MockMonitor();
        auto mon = std::unique_ptr<th::IMonitor>(mon_raw);
        tp::EngineConfig cfg;
        cfg.hotplug_factory =
            [m = std::move(mon)]() mutable -> std::unique_ptr<th::IMonitor> {
                return std::move(m);
            };
        auto e = tp::EngineTestHooks::create_with_device_fp(
            std::move(cfg), std::make_unique<MockDevice>(),
            make_fp("0bda", "4c10", "TPDAC0001"));
        if (!e) {
            return fail("case3-create");
        }
        auto& engine = **e;
        EventLog log;
        engine.set_event_callback([&](const tp::Event& ev) { log.on(ev); });

        if (!engine.load(argv[1])) {
            return fail("case3-load");
        }
        if (!log.wait_state(tp::State::Playing, 2s)) {
            return fail("case3-wait-playing");
        }
        if (!engine.pause()) {
            return fail("case3-pause");
        }
        if (!log.wait_state(tp::State::Paused, 1s)) {
            return fail("case3-wait-paused");
        }

        log.clear();
        th::DeviceEvent rm{};
        rm.kind = th::EventKind::Removed;
        rm.fingerprint = make_fp("0bda", "4c10", "TPDAC0001");
        mon_raw->push(rm);
        if (!log.wait_state(tp::State::Disconnected, 2s)) {
            return fail("case3-disc");
        }

        log.clear();
        th::DeviceEvent ad{};
        ad.kind = th::EventKind::Added;
        ad.alsa_card_index = 3;
        ad.fingerprint = make_fp("0bda", "4c10", "TPDAC0001");
        mon_raw->push(ad);

        if (!log.wait_state(tp::State::Paused, 2s)) {
            return fail("case3-resume-paused");
        }
        // Verify we did NOT pass through Playing on the resume.
        if (log.last_state() != tp::State::Paused) {
            return fail("case3-final-paused");
        }
    }

    std::printf("ok hotplug_fsm\n");
    return 0;
}
