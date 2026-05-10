// SPDX-License-Identifier: GPL-3.0-or-later
//
// Applies SCHED_FIFO + mlockall + CPU affinity to the calling thread (audio
// thread). Each step is independent: failure of one is recorded and the rest
// still attempt. Caller always receives a fully populated Status.
//
// Failure modes encountered in unprivileged setups:
//   - SCHED_FIFO   -> EPERM if process lacks CAP_SYS_NICE / PAM rtprio limit.
//   - mlockall     -> EPERM if RLIMIT_MEMLOCK is too low.
//   - affinity     -> EINVAL if requested CPU is offline.

#include <transporter/engine/rt.hpp>

#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace transporter::engine::rt {

namespace {

std::string errno_msg(const char* prefix, int e) {
    std::string out{prefix};
    out += ": ";
    out += std::strerror(e);
    return out;
}

bool try_fifo(int prio, std::string& reason_out) noexcept {
    sched_param p{};
    p.sched_priority = prio;
    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
    if (rc == 0) {
        reason_out.clear();
        return true;
    }
    if (rc == EPERM) {
        reason_out = "EPERM: SCHED_FIFO not granted; "
                     "check /etc/security/limits.d (rtprio) and audio group";
    } else {
        reason_out = errno_msg("pthread_setschedparam(SCHED_FIFO)", rc);
    }
    return false;
}

bool try_other_nice(int desired_nice) noexcept {
    sched_param p{};
    p.sched_priority = 0;
    (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &p);
    // setpriority returns 0 on success; -1 errno on failure. nice value -10
    // typically requires CAP_SYS_NICE — best-effort only.
    errno = 0;
    const int rc = setpriority(PRIO_PROCESS, 0, desired_nice);
    (void)rc;
    return true; // best-effort; we keep going either way
}

bool try_mlockall_now() noexcept {
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

int pick_default_core() noexcept {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) {
        return -1;
    }
    return static_cast<int>(n - 1);
}

bool try_affinity(int core) noexcept {
    if (core < 0) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(core), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

} // namespace

Status apply_to_current_thread(const Policy& policy) noexcept {
    Status s{};

    std::string fifo_reason;
    if (try_fifo(policy.desired_fifo_prio, fifo_reason)) {
        s.mode = Mode::Fifo;
        s.priority = policy.desired_fifo_prio;
    } else {
        s.mode = Mode::Other;
        (void)try_other_nice(policy.fallback_nice);
        s.priority = policy.fallback_nice;
        s.fallback_reason = std::move(fifo_reason);
    }

    if (policy.try_mlockall) {
        s.memlocked = try_mlockall_now();
    }

    if (policy.try_affinity) {
        const int core = (policy.affinity_core >= 0)
                             ? policy.affinity_core
                             : pick_default_core();
        if (try_affinity(core)) {
            s.cpu_pinned = core;
        }
    }

    return s;
}

} // namespace transporter::engine::rt
