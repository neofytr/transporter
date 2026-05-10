// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wayland connection + registry binding. We accept the conservative subset:
// wl_compositor v4, xdg_wm_base v1, wl_seat v5, wl_output v2.

#include "platform_internal.hpp"

#include <cstring>

namespace transporter::gui::platform {

namespace {

void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t version) {
    auto& w = *static_cast<WindowImpl*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        const uint32_t use = version < 4 ? version : 4;
        w.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, use));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        const uint32_t use = version < 1 ? version : 1;
        w.wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, use));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        const uint32_t use = version < 5 ? version : 5;
        w.seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, use));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0 && w.output == nullptr) {
        const uint32_t use = version < 2 ? version : 2;
        w.output = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface, use));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        const uint32_t use = version < 1 ? version : 1;
        w.shm = static_cast<wl_shm*>(
            wl_registry_bind(reg, name, &wl_shm_interface, use));
    }
}

void registry_global_remove(void* /*data*/, wl_registry* /*reg*/, uint32_t /*name*/) {
    // best-effort: we don't dynamically rebind compositor / wm_base
}

constexpr wl_registry_listener kRegistryListener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

} // namespace

bool init_wayland(WindowImpl& w) {
    w.display = wl_display_connect(nullptr);
    if (w.display == nullptr) {
        return false;
    }

    w.registry = wl_display_get_registry(w.display);
    if (w.registry == nullptr) {
        return false;
    }
    wl_registry_add_listener(w.registry, &kRegistryListener, &w);

    if (wl_display_roundtrip(w.display) < 0) {
        return false;
    }

    if (w.compositor == nullptr || w.wm_base == nullptr) {
        return false;  // missing required globals
    }
    if (w.shm == nullptr) {
        return false;
    }

    return true;
}

bool dispatch_pending(WindowImpl& w) {
    if (w.display == nullptr) {
        return false;
    }
    wl_display_dispatch_pending(w.display);
    if (wl_display_flush(w.display) < 0 && errno != EAGAIN) {
        return false;
    }
    return true;
}

void destroy_wayland(WindowImpl& w) {
    if (w.output != nullptr) {
        wl_output_destroy(w.output);
        w.output = nullptr;
    }
    if (w.registry != nullptr) {
        wl_registry_destroy(w.registry);
        w.registry = nullptr;
    }
    if (w.display != nullptr) {
        wl_display_disconnect(w.display);
        w.display = nullptr;
    }
}

} // namespace transporter::gui::platform
