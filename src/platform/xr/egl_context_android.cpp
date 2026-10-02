#include "platform/xr/egl_context_android.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace rr::xr {

EglContext::EglContext() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
        throw std::runtime_error("egl: eglInitialize failed (0x" + std::to_string(eglGetError()) + ")");
    display_ = display;
    // RGBA8, no depth on the EGL surface (the eye framebuffers carry their own), ES 3 renderable, pbuffer-capable.
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig configs[64];
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, configs, 64, &count) || count <= 0)
        throw std::runtime_error("egl: no RGBA8 ES3 pbuffer config");
    EGLConfig config = nullptr;
    for (EGLint i = 0; i < count && !config; ++i) { // the first exact RGBA8 match (ChooseConfig sorts deeper first)
        EGLint r = 0, g = 0, b = 0, a = 0;
        eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &a);
        if (r == 8 && g == 8 && b == 8 && a == 8) config = configs[i];
    }
    if (!config) config = configs[0];
    config_ = config;
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) throw std::runtime_error("egl: eglCreateContext(ES 3.2) failed (0x" + std::to_string(eglGetError()) + ")");
    context_ = context;
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    if (surface == EGL_NO_SURFACE) throw std::runtime_error("egl: eglCreatePbufferSurface failed (0x" + std::to_string(eglGetError()) + ")");
    surface_ = surface;
    if (!eglMakeCurrent(display, surface, surface, context))
        throw std::runtime_error("egl: eglMakeCurrent failed (0x" + std::to_string(eglGetError()) + ")");
    std::printf("egl: EGL %d.%d, ES 3.2 context current\n", major, minor);
}

EglContext::~EglContext() {
    EGLDisplay display = static_cast<EGLDisplay>(display_);
    if (display != EGL_NO_DISPLAY && display) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_) eglDestroySurface(display, static_cast<EGLSurface>(surface_));
        if (context_) eglDestroyContext(display, static_cast<EGLContext>(context_));
        eglTerminate(display);
    }
}

} // namespace rr::xr
