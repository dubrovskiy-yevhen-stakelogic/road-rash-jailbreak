#include "render/render_target.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace rr::render {

namespace {

constexpr GLenum kFramebuffer = 0x8D40, kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9, kRenderbuffer = 0x8D41;
constexpr GLenum kColourAttachment0 = 0x8CE0, kDepthStencilAttachment = 0x821A, kDepth24Stencil8 = 0x88F0;
constexpr GLenum kFramebufferComplete = 0x8CD5, kMaxSamples = 0x8D57, kTimeElapsed = 0x88BF;
constexpr GLenum kQueryResult = 0x8866, kQueryResultAvailable = 0x8867, kRgba8 = 0x8058;

using PFNGENFRAMEBUFFERS = void(APIENTRY*)(GLsizei, GLuint*);
using PFNDELETEFRAMEBUFFERS = void(APIENTRY*)(GLsizei, const GLuint*);
using PFNBINDFRAMEBUFFER = void(APIENTRY*)(GLenum, GLuint);
using PFNFRAMEBUFFERTEXTURE2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFNFRAMEBUFFERRENDERBUFFER = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
using PFNCHECKFRAMEBUFFERSTATUS = GLenum(APIENTRY*)(GLenum);
using PFNGENRENDERBUFFERS = void(APIENTRY*)(GLsizei, GLuint*);
using PFNDELETERENDERBUFFERS = void(APIENTRY*)(GLsizei, const GLuint*);
using PFNBINDRENDERBUFFER = void(APIENTRY*)(GLenum, GLuint);
using PFNRENDERBUFFERSTORAGEMULTISAMPLE = void(APIENTRY*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PFNBLITFRAMEBUFFER = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using PFNGENERATEMIPMAP = void(APIENTRY*)(GLenum);
using PFNTEXIMAGE3D = void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFNGENQUERIES = void(APIENTRY*)(GLsizei, GLuint*);
using PFNBEGINQUERY = void(APIENTRY*)(GLenum, GLuint);
using PFNENDQUERY = void(APIENTRY*)(GLenum);
using PFNGETQUERYOBJECTIV = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFNGETQUERYOBJECTUI64V = void(APIENTRY*)(GLuint, GLenum, uint64_t*);
using PFNSWAPINTERVAL = BOOL(WINAPI*)(int);

struct TargetGl {
    bool loaded = false, ok = false;
    PFNGENFRAMEBUFFERS GenFramebuffers = nullptr;
    PFNDELETEFRAMEBUFFERS DeleteFramebuffers = nullptr;
    PFNBINDFRAMEBUFFER BindFramebuffer = nullptr;
    PFNFRAMEBUFFERTEXTURE2D FramebufferTexture2D = nullptr;
    PFNFRAMEBUFFERRENDERBUFFER FramebufferRenderbuffer = nullptr;
    PFNCHECKFRAMEBUFFERSTATUS CheckFramebufferStatus = nullptr;
    PFNGENRENDERBUFFERS GenRenderbuffers = nullptr;
    PFNDELETERENDERBUFFERS DeleteRenderbuffers = nullptr;
    PFNBINDRENDERBUFFER BindRenderbuffer = nullptr;
    PFNRENDERBUFFERSTORAGEMULTISAMPLE RenderbufferStorageMultisample = nullptr;
    PFNBLITFRAMEBUFFER BlitFramebuffer = nullptr;
    PFNGENERATEMIPMAP GenerateMipmap = nullptr;
    PFNTEXIMAGE3D TexImage3D = nullptr;
    PFNGENQUERIES GenQueries = nullptr;
    PFNBEGINQUERY BeginQuery = nullptr;
    PFNENDQUERY EndQuery = nullptr;
    PFNGETQUERYOBJECTIV GetQueryObjectiv = nullptr;
    PFNGETQUERYOBJECTUI64V GetQueryObjectui64v = nullptr;
    PFNSWAPINTERVAL SwapInterval = nullptr;
} g;

void* Proc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (p == nullptr || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
        p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
        HMODULE module = GetModuleHandleA("opengl32.dll");
        p = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    }
    return p;
}

template <typename T>
void Load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(Proc(name));
}

} // namespace

bool LoadTargetGl() {
    if (g.loaded) return g.ok;
    g.loaded = true;
    Load(g.GenFramebuffers, "glGenFramebuffers");
    Load(g.DeleteFramebuffers, "glDeleteFramebuffers");
    Load(g.BindFramebuffer, "glBindFramebuffer");
    Load(g.FramebufferTexture2D, "glFramebufferTexture2D");
    Load(g.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    Load(g.CheckFramebufferStatus, "glCheckFramebufferStatus");
    Load(g.GenRenderbuffers, "glGenRenderbuffers");
    Load(g.DeleteRenderbuffers, "glDeleteRenderbuffers");
    Load(g.BindRenderbuffer, "glBindRenderbuffer");
    Load(g.RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
    Load(g.BlitFramebuffer, "glBlitFramebuffer");
    Load(g.GenerateMipmap, "glGenerateMipmap");
    Load(g.TexImage3D, "glTexImage3D");
    Load(g.GenQueries, "glGenQueries");
    Load(g.BeginQuery, "glBeginQuery");
    Load(g.EndQuery, "glEndQuery");
    Load(g.GetQueryObjectiv, "glGetQueryObjectiv");
    Load(g.GetQueryObjectui64v, "glGetQueryObjectui64v");
    Load(g.SwapInterval, "wglSwapIntervalEXT");
    g.ok = g.GenFramebuffers && g.DeleteFramebuffers && g.BindFramebuffer && g.FramebufferTexture2D &&
           g.FramebufferRenderbuffer && g.CheckFramebufferStatus && g.GenRenderbuffers && g.DeleteRenderbuffers &&
           g.BindRenderbuffer && g.RenderbufferStorageMultisample && g.BlitFramebuffer && g.GenerateMipmap && g.TexImage3D;
    if (!g.ok) std::fprintf(stderr, "render target: the driver lacks framebuffer objects; the window's own buffer is used\n");
    return g.ok;
}

void BindFramebuffer(unsigned fb) {
    if (LoadTargetGl()) g.BindFramebuffer(kFramebuffer, fb);
}

void GenerateMipmap(unsigned target) {
    if (LoadTargetGl()) g.GenerateMipmap(target);
}

void TexImage3D(unsigned target, int level, int internalFormat, int w, int h, int depth, unsigned format, unsigned type,
                const void* pixels) {
    if (LoadTargetGl()) g.TexImage3D(target, level, internalFormat, w, h, depth, 0, format, type, pixels);
}

void RenderTarget::Release() {
    if (!g.ok) return;
    if (fbo_) g.DeleteFramebuffers(1, &fbo_);
    if (msFbo_) g.DeleteFramebuffers(1, &msFbo_);
    if (colour_) glDeleteTextures(1, &colour_);
    const GLuint rbs[3] = {depth_, msColour_, msDepth_};
    for (GLuint rb : rbs)
        if (rb) g.DeleteRenderbuffers(1, &rb);
    fbo_ = colour_ = depth_ = msFbo_ = msColour_ = msDepth_ = 0;
    width_ = height_ = samples_ = 0;
}

bool RenderTarget::Ensure(int w, int h, int samples) {
    if (!LoadTargetGl() || w <= 0 || h <= 0) return false;
    GLint maxSamples = 1;
    glGetIntegerv(kMaxSamples, &maxSamples);
    samples = std::clamp(samples, 1, std::max(1, static_cast<int>(maxSamples)));
    if (w == width_ && h == height_ && samples == samples_ && fbo_ != 0) return true;
    Release();
    width_ = w;
    height_ = h;
    samples_ = samples;
    // the single-sample image: a texture (the post-process copies from it) and, when it is the one drawn into, a
    // depth / stencil buffer (the shadow's mask bit is the stencil, race_scene.cpp DrawShadow)
    glGenTextures(1, &colour_);
    glBindTexture(GL_TEXTURE_2D, colour_);
    glTexImage2D(GL_TEXTURE_2D, 0, kRgba8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g.GenFramebuffers(1, &fbo_);
    g.BindFramebuffer(kFramebuffer, fbo_);
    g.FramebufferTexture2D(kFramebuffer, kColourAttachment0, GL_TEXTURE_2D, colour_, 0);
    if (samples == 1) {
        g.GenRenderbuffers(1, &depth_);
        g.BindRenderbuffer(kRenderbuffer, depth_);
        g.RenderbufferStorageMultisample(kRenderbuffer, 0, kDepth24Stencil8, w, h);
        g.FramebufferRenderbuffer(kFramebuffer, kDepthStencilAttachment, kRenderbuffer, depth_);
    }
    bool complete = g.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    if (samples > 1 && complete) {
        g.GenRenderbuffers(1, &msColour_);
        g.BindRenderbuffer(kRenderbuffer, msColour_);
        g.RenderbufferStorageMultisample(kRenderbuffer, samples, kRgba8, w, h);
        g.GenRenderbuffers(1, &msDepth_);
        g.BindRenderbuffer(kRenderbuffer, msDepth_);
        g.RenderbufferStorageMultisample(kRenderbuffer, samples, kDepth24Stencil8, w, h);
        g.GenFramebuffers(1, &msFbo_);
        g.BindFramebuffer(kFramebuffer, msFbo_);
        g.FramebufferRenderbuffer(kFramebuffer, kColourAttachment0, kRenderbuffer, msColour_);
        g.FramebufferRenderbuffer(kFramebuffer, kDepthStencilAttachment, kRenderbuffer, msDepth_);
        complete = g.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    }
    g.BindRenderbuffer(kRenderbuffer, 0);
    g.BindFramebuffer(kFramebuffer, 0);
    if (!complete) {
        std::fprintf(stderr, "render target: %dx%d with %d sample(s) is not complete\n", w, h, samples);
        Release();
        return false;
    }
    return true;
}

void RenderTarget::Bind() const {
    if (g.ok) g.BindFramebuffer(kFramebuffer, DrawFramebuffer());
}

void RenderTarget::Resolve() const {
    if (!g.ok) return;
    if (samples_ > 1) {
        g.BindFramebuffer(kReadFramebuffer, msFbo_);
        g.BindFramebuffer(kDrawFramebuffer, fbo_);
        g.BlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    g.BindFramebuffer(kFramebuffer, fbo_);
}

void RenderTarget::PresentToWindow(int x, int y, int w, int h) const {
    if (!g.ok) return;
    g.BindFramebuffer(kReadFramebuffer, fbo_);
    g.BindFramebuffer(kDrawFramebuffer, 0);
    glDisable(GL_SCISSOR_TEST);
    g.BlitFramebuffer(0, 0, width_, height_, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    g.BindFramebuffer(kFramebuffer, 0);
}

bool SetSwapInterval(bool on) {
    LoadTargetGl();
    return g.SwapInterval != nullptr && g.SwapInterval(on ? 1 : 0) != FALSE;
}

void SetBorderlessFullscreen(HWND hwnd, bool on) {
    static bool isFull = false;
    static LONG_PTR savedStyle = 0;
    static RECT savedRect = {};
    if (hwnd == nullptr || on == isFull) return;
    if (on) {
        savedStyle = GetWindowLongPtrA(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &savedRect);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrA(hwnd, GWL_STYLE, (savedStyle & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, savedStyle);
        SetWindowPos(hwnd, nullptr, savedRect.left, savedRect.top, savedRect.right - savedRect.left,
                     savedRect.bottom - savedRect.top, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    isFull = on;
}

bool GpuTimer::Available() const { return g.GenQueries != nullptr && g.BeginQuery != nullptr && g.GetQueryObjectui64v != nullptr; }

void GpuTimer::Begin() {
    LoadTargetGl();
    if (!Available() || open_) return;
    if (queries_[0] == 0) g.GenQueries(4, queries_);
    // collect the oldest result first when every query is in flight
    if (pending_ == 4) {
        const GLuint q = queries_[next_];
        uint64_t ns = 0;
        g.GetQueryObjectui64v(q, kQueryResult, &ns);
        lastMs_ = static_cast<double>(ns) / 1.0e6;
        --pending_;
    }
    g.BeginQuery(kTimeElapsed, queries_[next_]);
    open_ = true;
}

void GpuTimer::End() {
    if (!open_) return;
    g.EndQuery(kTimeElapsed);
    open_ = false;
    next_ = (next_ + 1) % 4;
    ++pending_;
    // read what has completed, oldest first
    while (pending_ > 0) {
        const int oldest = (next_ - pending_ + 4) % 4;
        GLint ready = 0;
        g.GetQueryObjectiv(queries_[oldest], kQueryResultAvailable, &ready);
        if (!ready) break;
        uint64_t ns = 0;
        g.GetQueryObjectui64v(queries_[oldest], kQueryResult, &ns);
        lastMs_ = static_cast<double>(ns) / 1.0e6;
        --pending_;
    }
}

// ---------------------------------------------------------------- the draw-call counter (the profiler)
namespace {

using PFNDRAWARRAYS = void(APIENTRY*)(GLenum, GLint, GLsizei);
std::atomic<uint64_t> g_drawCalls{0}, g_drawTriangles{0};
PFNDRAWARRAYS g_realDrawArrays = nullptr;
void** g_drawArraysSlot = nullptr;

void APIENTRY CountingDrawArrays(GLenum mode, GLint first, GLsizei count) {
    g_drawCalls.fetch_add(1, std::memory_order_relaxed);
    if (mode == GL_TRIANGLES && count > 0) g_drawTriangles.fetch_add(static_cast<uint64_t>(count) / 3u, std::memory_order_relaxed);
    g_realDrawArrays(mode, first, count);
}

// The executable's import slot of opengl32.dll!glDrawArrays.
void** FindDrawArraysSlot() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; ++imp) {
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        if (_stricmp(dll, "opengl32.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), "glDrawArrays") == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

void SetSlot(void* value) {
    DWORD old = 0;
    if (!VirtualProtect(g_drawArraysSlot, sizeof(void*), PAGE_READWRITE, &old)) return;
    *g_drawArraysSlot = value;
    VirtualProtect(g_drawArraysSlot, sizeof(void*), old, &old);
}

} // namespace

void CountDrawCalls(bool on) {
    if (g_drawArraysSlot == nullptr) {
        g_drawArraysSlot = FindDrawArraysSlot();
        if (g_drawArraysSlot == nullptr) return;
        g_realDrawArrays = reinterpret_cast<PFNDRAWARRAYS>(*g_drawArraysSlot);
    }
    SetSlot(on ? reinterpret_cast<void*>(&CountingDrawArrays) : reinterpret_cast<void*>(g_realDrawArrays));
}

uint64_t DrawCallCount() { return g_drawCalls.load(std::memory_order_relaxed); }
uint64_t DrawTriangleCount() { return g_drawTriangles.load(std::memory_order_relaxed); }

} // namespace rr::render
