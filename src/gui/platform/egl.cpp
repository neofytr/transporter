// SPDX-License-Identifier: GPL-3.0-or-later
//
// EGL context creation for the GPU render path (OpenGL 3.3 core on wl_egl_window).
// Compiled only when HAVE_EGL is defined; falls back silently to CPU path.

#ifdef HAVE_EGL

#include "platform_internal.hpp"

// EGL first so KHR/khrplatform.h is already processed and the imgui loader's
// inline khr-type definitions are suppressed by their own include guards.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-egl.h>

// GL function pointers are resolved via dlsym by the imgui loader; including
// imgui_impl_opengl3_loader.h here gives us the extern declaration of
// imgl3wProcs and the gl* macro definitions without duplicating any code.
#include <imgui_impl_opengl3_loader.h>
#include <imgui_impl_opengl3.h>

#include <imgui.h>

namespace transporter::gui::platform {

bool init_egl(WindowImpl& w) {
    EGLDisplay dpy = eglGetDisplay(
        static_cast<EGLNativeDisplayType>(w.display));
    if (dpy == EGL_NO_DISPLAY) {
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        eglTerminate(dpy);
        return false;
    }

    static const EGLint kConfigAttribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      0,
        EGL_DEPTH_SIZE,      0,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint n_configs = 0;
    if (!eglChooseConfig(dpy, kConfigAttribs, &config, 1, &n_configs) ||
        n_configs < 1) {
        eglTerminate(dpy);
        return false;
    }

    wl_egl_window* egl_win = wl_egl_window_create(w.surface, w.width, w.height);
    if (!egl_win) {
        eglTerminate(dpy);
        return false;
    }

    EGLSurface surf = eglCreateWindowSurface(
        dpy, config,
        reinterpret_cast<EGLNativeWindowType>(egl_win),
        nullptr);
    if (surf == EGL_NO_SURFACE) {
        wl_egl_window_destroy(egl_win);
        eglTerminate(dpy);
        return false;
    }

    static const EGLint kCtxAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION,             3,
        EGL_CONTEXT_MINOR_VERSION,             3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,       EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, kCtxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        eglDestroySurface(dpy, surf);
        wl_egl_window_destroy(egl_win);
        eglTerminate(dpy);
        return false;
    }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        wl_egl_window_destroy(egl_win);
        eglTerminate(dpy);
        return false;
    }

    w.egl_display = static_cast<void*>(dpy);
    w.egl_surface = static_cast<void*>(surf);
    w.egl_context = static_cast<void*>(ctx);
    w.egl_window  = static_cast<void*>(egl_win);
    return true;
}

void destroy_egl(WindowImpl& w) {
    if (!w.egl_display) return;
    auto* dpy  = static_cast<EGLDisplay>(w.egl_display);
    auto* ctx  = static_cast<EGLContext>(w.egl_context);
    auto* surf = static_cast<EGLSurface>(w.egl_surface);
    auto* win  = static_cast<wl_egl_window*>(w.egl_window);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx  != EGL_NO_CONTEXT)  eglDestroyContext(dpy, ctx);
    if (surf != EGL_NO_SURFACE)  eglDestroySurface(dpy, surf);
    if (win)                     wl_egl_window_destroy(win);
    eglTerminate(dpy);

    w.egl_display    = nullptr;
    w.egl_surface    = nullptr;
    w.egl_context    = nullptr;
    w.egl_window     = nullptr;
    w.gl_initialized = false;
}

void egl_swap(WindowImpl& w) {
    eglSwapBuffers(
        static_cast<EGLDisplay>(w.egl_display),
        static_cast<EGLSurface>(w.egl_surface));
}

void egl_resize(WindowImpl& w) {
    wl_egl_window_resize(
        static_cast<wl_egl_window*>(w.egl_window),
        w.width, w.height, 0, 0);
    eglMakeCurrent(
        static_cast<EGLDisplay>(w.egl_display),
        static_cast<EGLSurface>(w.egl_surface),
        static_cast<EGLSurface>(w.egl_surface),
        static_cast<EGLContext>(w.egl_context));
}

void egl_render_frame(WindowImpl& w, float r, float g, float b) {
    // GL function pointers are loaded by ImGui_ImplOpenGL3_Init via dlsym.
    glViewport(0, 0,
               static_cast<GLsizei>(w.width),
               static_cast<GLsizei>(w.height));
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(
        static_cast<EGLDisplay>(w.egl_display),
        static_cast<EGLSurface>(w.egl_surface));
}

} // namespace transporter::gui::platform

#else  // !HAVE_EGL

#include "platform_internal.hpp"

namespace transporter::gui::platform {

bool init_egl(WindowImpl&)   { return false; }
void destroy_egl(WindowImpl&) {}
void egl_swap(WindowImpl&)    {}
void egl_resize(WindowImpl&)  {}
void egl_render_frame(WindowImpl&, float, float, float) {}

} // namespace transporter::gui::platform

#endif // HAVE_EGL
