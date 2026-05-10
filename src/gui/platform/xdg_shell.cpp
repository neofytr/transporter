// SPDX-License-Identifier: GPL-3.0-or-later
//
// xdg_surface + xdg_toplevel handshake.

#include "platform_internal.hpp"

namespace transporter::gui::platform {

namespace {

void wm_base_ping(void* /*data*/, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

constexpr xdg_wm_base_listener kWmBaseListener = {
    .ping = wm_base_ping,
};

void xdg_surface_configure(void* data, xdg_surface* surf, uint32_t serial) {
    auto& w = *static_cast<WindowImpl*>(data);
    xdg_surface_ack_configure(surf, serial);
    w.configured = true;
}

constexpr xdg_surface_listener kXdgSurfaceListener = {
    .configure = xdg_surface_configure,
};

void toplevel_configure(void* data, xdg_toplevel* /*top*/, int32_t width,
                        int32_t height, wl_array* /*states*/) {
    auto& w = *static_cast<WindowImpl*>(data);
    if (width > 0 && height > 0) {
        if (width != w.width || height != w.height) {
            w.width = width;
            w.height = height;
            w.needs_resize = true;
        }
    }
}

void toplevel_close(void* data, xdg_toplevel* /*top*/) {
    auto& w = *static_cast<WindowImpl*>(data);
    w.close_requested = true;
}

void toplevel_configure_bounds(void* /*data*/, xdg_toplevel* /*top*/,
                               int32_t /*width*/, int32_t /*height*/) {}

void toplevel_wm_capabilities(void* /*data*/, xdg_toplevel* /*top*/,
                              wl_array* /*caps*/) {}

constexpr xdg_toplevel_listener kToplevelListener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

} // namespace

bool init_surface(WindowImpl& w, const std::string& title) {
    xdg_wm_base_add_listener(w.wm_base, &kWmBaseListener, &w);

    w.surface = wl_compositor_create_surface(w.compositor);
    if (w.surface == nullptr) {
        return false;
    }

    w.xdg_surf = xdg_wm_base_get_xdg_surface(w.wm_base, w.surface);
    if (w.xdg_surf == nullptr) {
        return false;
    }
    xdg_surface_add_listener(w.xdg_surf, &kXdgSurfaceListener, &w);

    w.toplevel = xdg_surface_get_toplevel(w.xdg_surf);
    if (w.toplevel == nullptr) {
        return false;
    }
    xdg_toplevel_add_listener(w.toplevel, &kToplevelListener, &w);
    xdg_toplevel_set_title(w.toplevel, title.c_str());
    xdg_toplevel_set_app_id(w.toplevel, "transporter");

    // initial commit so the compositor sends us a configure event
    wl_surface_commit(w.surface);

    // wait for the configure to land before bringing up EGL
    while (!w.configured) {
        if (wl_display_dispatch(w.display) < 0) {
            return false;
        }
    }
    return true;
}

void destroy_surface(WindowImpl& w) {
    if (w.toplevel != nullptr) {
        xdg_toplevel_destroy(w.toplevel);
        w.toplevel = nullptr;
    }
    if (w.xdg_surf != nullptr) {
        xdg_surface_destroy(w.xdg_surf);
        w.xdg_surf = nullptr;
    }
    if (w.surface != nullptr) {
        wl_surface_destroy(w.surface);
        w.surface = nullptr;
    }
    if (w.wm_base != nullptr) {
        xdg_wm_base_destroy(w.wm_base);
        w.wm_base = nullptr;
    }
    if (w.compositor != nullptr) {
        wl_compositor_destroy(w.compositor);
        w.compositor = nullptr;
    }
}

} // namespace transporter::gui::platform
