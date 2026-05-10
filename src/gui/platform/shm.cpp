// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform_internal.hpp"

#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

// memfd_create is in sys/mman.h on glibc >= 2.27; fall back to syscall.
#ifndef MFD_CLOEXEC
#  include <sys/syscall.h>
#  define MFD_CLOEXEC 1U
static inline int memfd_create(const char* name, unsigned flags) {
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}
#endif

namespace transporter::gui::platform {

namespace {

static const wl_buffer_listener kBufListener = {
    .release = [](void* data, wl_buffer*) {
        static_cast<ShmBuffer*>(data)->busy = false;
    },
};

bool alloc_buf(WindowImpl& w, ShmBuffer& b) {
    const int size = w.shm_size;
    int fd = memfd_create("transporter-shm", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, size) < 0) { close(fd); return false; }
    void* data = mmap(nullptr, static_cast<std::size_t>(size),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return false; }

    wl_shm_pool* pool = wl_shm_create_pool(w.shm, fd, size);
    close(fd);
    b.buf = wl_shm_pool_create_buffer(pool, 0, w.width, w.height,
                                      w.shm_stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    b.data = static_cast<uint8_t*>(data);
    b.busy = false;
    wl_buffer_add_listener(b.buf, &kBufListener, &b);
    return true;
}

void free_buf(WindowImpl& w, ShmBuffer& b) {
    if (b.buf)  { wl_buffer_destroy(b.buf); b.buf = nullptr; }
    if (b.data) { munmap(b.data, static_cast<std::size_t>(w.shm_size)); b.data = nullptr; }
    b.busy = false;
}

} // namespace

bool init_shm(WindowImpl& w) {
    w.shm_stride = w.width * 4;
    w.shm_size   = w.shm_stride * w.height;
    w.shm_back   = 0;
    for (auto& b : w.shm_bufs) {
        if (!alloc_buf(w, b)) return false;
    }
    return true;
}

void destroy_shm(WindowImpl& w) {
    for (auto& b : w.shm_bufs) { free_buf(w, b); }
    if (w.shm) { wl_shm_destroy(w.shm); w.shm = nullptr; }
}

void shm_resize(WindowImpl& w) {
    for (auto& b : w.shm_bufs) { free_buf(w, b); }
    w.shm_stride = w.width * 4;
    w.shm_size   = w.shm_stride * w.height;
    for (auto& b : w.shm_bufs) { alloc_buf(w, b); }
    w.shm_back = 0;
}

void shm_commit(WindowImpl& w) {
    ShmBuffer& b = w.shm_bufs[w.shm_back];
    b.busy = true;
    wl_surface_attach(w.surface, b.buf, 0, 0);
    wl_surface_damage(w.surface, 0, 0, w.width, w.height);
    wl_surface_commit(w.surface);
    wl_display_flush(w.display);
    // Advance to the other buffer; if it is still busy, drain release events.
    w.shm_back ^= 1;
    if (w.shm_bufs[w.shm_back].busy) {
        wl_display_roundtrip(w.display);
    }
}

} // namespace transporter::gui::platform
