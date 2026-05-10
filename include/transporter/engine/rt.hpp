// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_RT_HPP
#define TRANSPORTER_ENGINE_RT_HPP

#include <cstdint>
#include <string>

namespace transporter::engine::rt {

// Scheduling mode the audio thread actually runs under. Fifo == SCHED_FIFO at
// the requested priority. Other == SCHED_OTHER + nice value (soft fallback).
enum class Mode : std::uint8_t { Fifo, Other };

// Outcome of apply_to_current_thread. Stashed by the engine and surfaced via
// PipelineSnapshot for the GUI / telemetry consumers.
struct Status {
    Mode mode = Mode::Other;
    int priority = 0;             // FIFO prio if Fifo; nice value if Other
    bool memlocked = false;       // mlockall(MCL_CURRENT|MCL_FUTURE) succeeded
    int cpu_pinned = -1;          // -1 if affinity not applied; else core idx
    std::string fallback_reason;  // empty iff mode == Fifo
};

// Knobs the caller can dial. Defaults match the locked spec
// (SCHED_FIFO 80, mlockall, last-core affinity).
struct Policy {
    int desired_fifo_prio = 80;
    int fallback_nice = -10;
    bool try_mlockall = true;
    bool try_affinity = true;
    int affinity_core = -1;  // -1 lets apply_to_current_thread pick last core
};

// Apply the policy to the calling thread. Never throws. On any best-effort
// step that fails (no CAP_SYS_NICE, low RLIMIT_MEMLOCK, no online cores),
// records the reason in Status and proceeds to the next step. The caller
// always gets back a fully-populated Status.
Status apply_to_current_thread(const Policy& policy) noexcept;

} // namespace transporter::engine::rt

#endif
