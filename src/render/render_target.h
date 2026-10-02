#pragma once
// The picture's offscreen target and its presentation: an internal render
// resolution independent of the window (a percentage of it or a fixed 720p..4K height), multisampling, the resolve
// and the scaled copy to the window, VSync, borderless fullscreen and the GPU's own frame time.
//
// Its GL entry points (framebuffer objects, blits, multisample renderbuffers, timer queries, mipmaps) are loaded
// here, separately from gl_api.h's table, and all of them exist in OpenGL ES 3.2 except the timer query (desktop
// only: GpuTimer then reports nothing) and wglSwapIntervalEXT (the desktop's VSync).
#include "render/gl_api.h"

#include <cstdint>

namespace rr::render {

// Loads the entry points once (a GL context must be current). False when the driver lacks framebuffer objects.
bool LoadTargetGl();

// glBindFramebuffer(GL_FRAMEBUFFER, fb) - for callers that draw into a target (race_render.h GameView).
void BindFramebuffer(unsigned fb);
// glGenerateMipmap(GL_TEXTURE_2D_ARRAY / GL_TEXTURE_2D) of the bound texture.
void GenerateMipmap(unsigned target);
// glTexImage3D, for a texture array (smooth textures).
void TexImage3D(unsigned target, int level, int internalFormat, int w, int h, int depth, unsigned format, unsigned type,
                const void* pixels);

class RenderTarget {
public:
    // A picture of w x h pixels with `samples` samples a pixel (1 = none, clamped to GL_MAX_SAMPLES): an RGBA8
    // colour and a depth / stencil buffer. Re-creates the buffers only when a parameter changed. False on failure.
    bool Ensure(int w, int h, int samples);
    // Draw into it (GL_FRAMEBUFFER).
    void Bind() const;
    // Multisampled: resolve into the single-sample image. Then binds that image for drawing and reading, so the
    // post-process can copy from it and draw back into it.
    void Resolve() const;
    // Copies the (resolved) image to the window's framebuffer (0) over (x, y, w, h) of a winW x winH window,
    // linearly filtered, and leaves the window's framebuffer bound.
    void PresentToWindow(int x, int y, int w, int h) const;
    int Width() const { return width_; }
    int Height() const { return height_; }
    int Samples() const { return samples_; }
    unsigned DrawFramebuffer() const { return samples_ > 1 ? msFbo_ : fbo_; }
    void Release();

private:
    int width_ = 0, height_ = 0, samples_ = 0;
    unsigned fbo_ = 0, colour_ = 0, depth_ = 0;       // single-sample (the resolved image when multisampled)
    unsigned msFbo_ = 0, msColour_ = 0, msDepth_ = 0; // multisampled
};

// wglSwapIntervalEXT(on ? 1 : 0); false when the driver has no such extension.
bool SetSwapInterval(bool on);

// Borderless fullscreen over the monitor the window is on, and back to the window it was. Shown without taking
// the keyboard from another window (SetWindowPos, no activation).
void SetBorderlessFullscreen(HWND hwnd, bool on);

// The GPU time of a stretch of commands, read back two frames later so the CPU never waits for it.
class GpuTimer {
public:
    void Begin();
    void End();
    // The last completed measurement in milliseconds, or -1 when none is available.
    double LastMs() const { return lastMs_; }
    bool Available() const;

private:
    unsigned queries_[4] = {};
    int next_ = 0, pending_ = 0;
    bool open_ = false;
    double lastMs_ = -1.0;
};

// DEVELOPMENT instrumentation for the profiler: counts every glDrawArrays call of this executable while `on`
// (the executable's own import of opengl32!glDrawArrays is pointed at a counting wrapper; nothing else changes).
void CountDrawCalls(bool on);
uint64_t DrawCallCount();       // since the process started counting
uint64_t DrawTriangleCount();   // the triangles those calls drew (GL_TRIANGLES counts / 3)

} // namespace rr::render
