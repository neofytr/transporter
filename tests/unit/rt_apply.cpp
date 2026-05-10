// SPDX-License-Identifier: GPL-3.0-or-later
//
// rt::apply_to_current_thread on a vanilla unprivileged shell session must
// fall back to SCHED_OTHER and report a non-empty fallback reason. mlockall
// usually fails too (RLIMIT_MEMLOCK low). Affinity may or may not succeed.

#include <transporter/engine/rt.hpp>

#include <cstdio>

namespace rt = transporter::engine::rt;

namespace {

int fail(const char* where) {
    std::fprintf(stderr, "FAIL [%s]\n", where);
    return 1;
}

} // namespace

int main() {
    rt::Policy pol;
    rt::Status s = rt::apply_to_current_thread(pol);

    // Either we got SCHED_FIFO (privileged or audio-group setup), or we
    // fell back. Both are valid outcomes; only the consistency is required.
    if (s.mode == rt::Mode::Fifo) {
        if (!s.fallback_reason.empty()) {
            return fail("FIFO with non-empty fallback_reason");
        }
        if (s.priority != pol.desired_fifo_prio) {
            return fail("FIFO priority mismatch");
        }
        std::printf("ok rt_apply mode=Fifo prio=%d memlocked=%d cpu=%d\n",
                    s.priority, static_cast<int>(s.memlocked), s.cpu_pinned);
    } else {
        if (s.fallback_reason.empty()) {
            return fail("Other with empty fallback_reason");
        }
        std::printf("ok rt_apply mode=Other nice=%d memlocked=%d cpu=%d "
                    "reason=\"%s\"\n",
                    s.priority, static_cast<int>(s.memlocked), s.cpu_pinned,
                    s.fallback_reason.c_str());
    }
    return 0;
}
