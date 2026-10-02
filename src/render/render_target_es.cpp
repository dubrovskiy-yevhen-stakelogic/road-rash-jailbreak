// The OpenGL ES 3.2 backend of render_target.h (the Quest build; cmake/vr.cmake
// compiles it INSTEAD of render_target.cpp on Android). The framebuffer part is the desktop file's, calling the ES
// library directly (every entry point is core ES 3.0). What the desktop file does with the window has no meaning under
// OpenXR and is absent here, said so per function: the swap interval (the XR runtime paces the frames), borderless
// fullscreen (no window), the GPU timer (ES has it only through GL_EXT_disjoint_timer_query - not wired yet: reports
// nothing, as the desktop's does on a driver without timer queries) and the draw-call counter (an import-table patch
// of opengl32.dll - counts nothing here). Keep the framebuffer part in step with render_target.cpp.
#include "render/render_target.h"

#include <algorithm>
#include <cstdio>

namespace rr::render {

bool LoadTargetGl() { return true; } // core ES 3.0: linked, nothing to load

void BindFramebuffer(unsigned fb) { glBindFramebuffer(GL_FRAMEBUFFER, fb); }

void GenerateMipmap(unsigned target) { glGenerateMipmap(target); }

void TexImage3D(unsigned target, int level, int internalFormat, int w, int h, int depth, unsigned format, unsigned type,
                const void* pixels) {
    glTexImage3D(target, level, internalFormat, w, h, depth, 0, format, type, pixels);
}

void RenderTarget::Release() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (msFbo_) glDeleteFramebuffers(1, &msFbo_);
    if (colour_) glDeleteTextures(1, &colour_);
    const GLuint rbs[3] = {depth_, msColour_, msDepth_};
    for (GLuint rb : rbs)
        if (rb) glDeleteRenderbuffers(1, &rb);
    fbo_ = colour_ = depth_ = msFbo_ = msColour_ = msDepth_ = 0;
    width_ = height_ = samples_ = 0;
}

bool RenderTarget::Ensure(int w, int h, int samples) {
    if (w <= 0 || h <= 0) return false;
    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    samples = std::clamp(samples, 1, std::max(1, static_cast<int>(maxSamples)));
    if (w == width_ && h == height_ && samples == samples_ && fbo_ != 0) return true;
    Release();
    width_ = w;
    height_ = h;
    samples_ = samples;
    glGenTextures(1, &colour_);
    glBindTexture(GL_TEXTURE_2D, colour_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour_, 0);
    if (samples == 1) {
        glGenRenderbuffers(1, &depth_);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_);
    }
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (samples > 1 && complete) {
        glGenRenderbuffers(1, &msColour_);
        glBindRenderbuffer(GL_RENDERBUFFER, msColour_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        glGenRenderbuffers(1, &msDepth_);
        glBindRenderbuffer(GL_RENDERBUFFER, msDepth_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        glGenFramebuffers(1, &msFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColour_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msDepth_);
        complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
        std::fprintf(stderr, "render target: %dx%d with %d sample(s) is not complete\n", w, h, samples);
        Release();
        return false;
    }
    return true;
}

void RenderTarget::Bind() const { glBindFramebuffer(GL_FRAMEBUFFER, DrawFramebuffer()); }

void RenderTarget::Resolve() const {
    if (samples_ > 1) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
}

void RenderTarget::PresentToWindow(int x, int y, int w, int h) const {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, width_, height_, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool SetSwapInterval(bool) { return false; }       // no window swap chain: the XR runtime paces the frames
void SetBorderlessFullscreen(HWND, bool) {}         // no window
bool GpuTimer::Available() const { return false; }  // GL_EXT_disjoint_timer_query not wired yet
void GpuTimer::Begin() {}
void GpuTimer::End() {}
void CountDrawCalls(bool) {}                        // the desktop's opengl32 import patch has no ES counterpart
uint64_t DrawCallCount() { return 0; }
uint64_t DrawTriangleCount() { return 0; }

} // namespace rr::render
