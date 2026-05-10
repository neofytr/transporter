// SPDX-License-Identifier: GPL-3.0-or-later
//
// EGL: bind a desktop-GL 3.3 core context against the Wayland display.
// imgui_impl_opengl3 accepts both desktop GL and GLES; desktop is used here
// for richer driver coverage.

#include "platform_internal.hpp"

#include <EGL/egl.h>

#include <cstdint>

namespace transporter::gui::platform {

bool init_egl(WindowImpl& w) {
    w.egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(w.display));
    if (w.egl_display == EGL_NO_DISPLAY) {
        return false;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(w.egl_display, &major, &minor) == EGL_FALSE) {
        return false;
    }
    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
        return false;
    }

    constexpr EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE,
    };
    EGLint num = 0;
    if (eglChooseConfig(w.egl_display, cfg_attribs, &w.egl_config, 1, &num) == EGL_FALSE
        || num < 1) {
        return false;
    }

    constexpr EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    w.egl_ctx = eglCreateContext(w.egl_display, w.egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (w.egl_ctx == EGL_NO_CONTEXT) {
        return false;
    }

    w.egl_window = wl_egl_window_create(w.surface, w.width, w.height);
    if (w.egl_window == nullptr) {
        return false;
    }
    w.egl_surf = eglCreateWindowSurface(
        w.egl_display, w.egl_config,
        reinterpret_cast<EGLNativeWindowType>(w.egl_window), nullptr);
    if (w.egl_surf == EGL_NO_SURFACE) {
        return false;
    }
    if (eglMakeCurrent(w.egl_display, w.egl_surf, w.egl_surf, w.egl_ctx) == EGL_FALSE) {
        return false;
    }
    eglSwapInterval(w.egl_display, 1);  // vsync
    return true;
}

bool egl_swap(WindowImpl& w) {
    return eglSwapBuffers(w.egl_display, w.egl_surf) == EGL_TRUE;
}

bool egl_resize(WindowImpl& w) {
    if (w.egl_window == nullptr) {
        return true;
    }
    wl_egl_window_resize(w.egl_window, w.width, w.height, 0, 0);
    return true;
}

void destroy_egl(WindowImpl& w) {
    if (w.egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(w.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (w.egl_surf != EGL_NO_SURFACE) {
            eglDestroySurface(w.egl_display, w.egl_surf);
            w.egl_surf = EGL_NO_SURFACE;
        }
        if (w.egl_ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(w.egl_display, w.egl_ctx);
            w.egl_ctx = EGL_NO_CONTEXT;
        }
        eglTerminate(w.egl_display);
        w.egl_display = EGL_NO_DISPLAY;
    }
    if (w.egl_window != nullptr) {
        wl_egl_window_destroy(w.egl_window);
        w.egl_window = nullptr;
    }
}

} // namespace transporter::gui::platform
