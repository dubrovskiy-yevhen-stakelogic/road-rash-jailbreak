// The OpenXR session on OpenGL / OpenGL ES (xr_session_gl.h).
//
// Start-up in the order the specification requires: the loader -> the instance with the extensions the runtime
// offers -> the HMD system -> the graphics requirements (must be queried before xrCreateSession) -> the session on the
// caller's current GL context -> the reference spaces -> the swapchains -> the action set. Everything optional (the
// display refresh rate, performance levels, the STAGE space) is only logged when absent; the GL binding extension is
// not optional.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <unknwn.h> // XR_USE_PLATFORM_WIN32 declares structures that name IUnknown
#include <filesystem>
#define XR_NO_PROTOTYPES // the loader is a DLL opened at run time; every function comes from xrGetInstanceProcAddr
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#ifdef __ANDROID__
#include <EGL/egl.h>
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif

#include "render/gl_api.h" // before the OpenXR headers: GL types (and on Windows <windows.h> with our macros)
#include "render/multiview.h"
#include "render/shaders.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "platform/xr/xr_actions.h"
#include "platform/xr/xr_session_gl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif

namespace rr::xr {
using render::gl;
using render::glCaps;
namespace {

using Clock = std::chrono::steady_clock;

// The framebuffer entry points (GL 3.0 / ES 3.0), kept private to the XR layer so the shared `Gl` table of
// render/gl_api.h is not touched: the desktop build loads them through wglGetProcAddress, ES links them.
struct Fbo {
    using GenFn = void(APIENTRY*)(GLsizei, GLuint*);
    using BindFn = void(APIENTRY*)(GLenum, GLuint);
    using DelFn = void(APIENTRY*)(GLsizei, const GLuint*);
    using Tex2DFn = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
    using StorageFn = void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
    using RbFn = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
    using StatusFn = GLenum(APIENTRY*)(GLenum);
    using BlitFn = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    using StorageMsFn = void(APIENTRY*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    using Tex2DMsFn = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
    using InvalidateFn = void(APIENTRY*)(GLenum, GLsizei, const GLenum*);
    GenFn GenFramebuffers = nullptr, GenRenderbuffers = nullptr;
    BindFn BindFramebuffer = nullptr, BindRenderbuffer = nullptr;
    DelFn DeleteFramebuffers = nullptr, DeleteRenderbuffers = nullptr;
    Tex2DFn FramebufferTexture2D = nullptr;
    StorageFn RenderbufferStorage = nullptr;
    RbFn FramebufferRenderbuffer = nullptr;
    StatusFn CheckFramebufferStatus = nullptr;
    BlitFn BlitFramebuffer = nullptr;
    StorageMsFn RenderbufferStorageMultisample = nullptr;
    // ES only, optional: GL_EXT_multisampled_render_to_texture (the tile's samples resolve on the way out)
    Tex2DMsFn FramebufferTexture2DMultisampleEXT = nullptr;
    StorageMsFn RenderbufferStorageMultisampleEXT = nullptr;
    InvalidateFn InvalidateFramebuffer = nullptr; // ES 3.0 core: the depth is not stored back to memory

    void Load() {
#ifdef RR_GLES
        GenFramebuffers = &glGenFramebuffers;
        GenRenderbuffers = &glGenRenderbuffers;
        BindFramebuffer = &glBindFramebuffer;
        BindRenderbuffer = &glBindRenderbuffer;
        DeleteFramebuffers = &glDeleteFramebuffers;
        DeleteRenderbuffers = &glDeleteRenderbuffers;
        FramebufferTexture2D = &glFramebufferTexture2D;
        RenderbufferStorage = &glRenderbufferStorage;
        FramebufferRenderbuffer = &glFramebufferRenderbuffer;
        CheckFramebufferStatus = &glCheckFramebufferStatus;
        BlitFramebuffer = &glBlitFramebuffer;
        RenderbufferStorageMultisample = &glRenderbufferStorageMultisample;
        InvalidateFramebuffer = &glInvalidateFramebuffer;
        FramebufferTexture2DMultisampleEXT =
            reinterpret_cast<Tex2DMsFn>(render::GlesProc("glFramebufferTexture2DMultisampleEXT"));
        RenderbufferStorageMultisampleEXT =
            reinterpret_cast<StorageMsFn>(render::GlesProc("glRenderbufferStorageMultisampleEXT"));
#else
        auto get = [](const char* name) {
            void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
            if (p == nullptr || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
                p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1))
                throw std::runtime_error(std::string("OpenGL entry point missing: ") + name);
            return p;
        };
        GenFramebuffers = reinterpret_cast<GenFn>(get("glGenFramebuffers"));
        GenRenderbuffers = reinterpret_cast<GenFn>(get("glGenRenderbuffers"));
        BindFramebuffer = reinterpret_cast<BindFn>(get("glBindFramebuffer"));
        BindRenderbuffer = reinterpret_cast<BindFn>(get("glBindRenderbuffer"));
        DeleteFramebuffers = reinterpret_cast<DelFn>(get("glDeleteFramebuffers"));
        DeleteRenderbuffers = reinterpret_cast<DelFn>(get("glDeleteRenderbuffers"));
        FramebufferTexture2D = reinterpret_cast<Tex2DFn>(get("glFramebufferTexture2D"));
        RenderbufferStorage = reinterpret_cast<StorageFn>(get("glRenderbufferStorage"));
        FramebufferRenderbuffer = reinterpret_cast<RbFn>(get("glFramebufferRenderbuffer"));
        CheckFramebufferStatus = reinterpret_cast<StatusFn>(get("glCheckFramebufferStatus"));
        BlitFramebuffer = reinterpret_cast<BlitFn>(get("glBlitFramebuffer"));
        RenderbufferStorageMultisample = reinterpret_cast<StorageMsFn>(get("glRenderbufferStorageMultisample"));
#endif
    }
};

// Whether the ES context offers an extension (glGetStringi, ES 3.0).
bool HasGlExtension(const char* name) {
#ifdef RR_GLES
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (e != nullptr && std::strcmp(e, name) == 0) return true;
    }
#else
    (void)name;
#endif
    return false;
}

// The copy of our framebuffer into a swapchain image: a full-screen triangle, texel for texel. `uDecode` undoes the
// sRGB encoding an ES driver applies to an sRGB target when it cannot be switched off, so the stored bytes are the
// source bytes either way.
const char* const kCopyVs = R"(#version 330 core
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
const char* const kCopyFs = R"(#version 330 core
uniform sampler2D uSrc;
uniform int uDecode;
out vec4 oColor;
void main() {
    vec3 c = texelFetch(uSrc, ivec2(gl_FragCoord.xy), 0).rgb;
    if (uDecode != 0) {
        vec3 lo = c / 12.92;
        vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
        c = mix(hi, lo, vec3(lessThanEqual(c, vec3(0.04045))));
    }
    oColor = vec4(c, 1.0);
}
)";

// An offscreen target: RGBA8 colour texture + depth-stencil renderbuffer; multisampled, a second framebuffer of
// multisample renderbuffers that is drawn into and resolved into the texture (FinishEye).
struct Target {
    GLuint fbo = 0, color = 0, depth = 0;
    GLuint msFbo = 0, msColor = 0, msDepth = 0;
    int width = 0, height = 0, samples = 1;
    GLuint DrawFbo() const { return samples > 1 ? msFbo : fbo; }
};

struct Chain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<GLuint> images;
    int width = 0, height = 0;
    bool finishedThisFrame = false;
    bool everReleased = false; // an image was released: a quad layer can show it again (KeepQuad)
};

// A direct eye: the acquired swapchain image itself is the colour attachment (the Quest), with a depth-stencil
// renderbuffer of ours - multisampled through GL_EXT_multisampled_render_to_texture when it is on.
struct DirectEye {
    GLuint fbo = 0, depth = 0;
    int samples = 1;
    uint32_t index = 0;
    bool acquired = false;
};

#define RRXR_FUNCTIONS(X)                                                                                             \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrPollEvent) X(xrResultToString) X(xrGetSystem)                  \
    X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession)               \
    X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)             \
    X(xrLocateViews) X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace) X(xrLocateSpace) X(xrDestroySpace)        \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages)            \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)

#define RRXR_OPTIONAL_FUNCTIONS(X)                                                                                    \
    X(xrEnumerateDisplayRefreshRatesFB) X(xrGetDisplayRefreshRateFB) X(xrRequestDisplayRefreshRateFB)                  \
    X(xrPerfSettingsSetPerformanceLevelEXT) X(xrCreateFoveationProfileFB) X(xrDestroyFoveationProfileFB)                \
    X(xrUpdateSwapchainFB)

struct Api {
    PFN_xrGetInstanceProcAddr gipa = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance xrCreateInstance = nullptr;
#define RRXR_DECL(n) PFN_##n n = nullptr;
    RRXR_FUNCTIONS(RRXR_DECL)
    RRXR_OPTIONAL_FUNCTIONS(RRXR_DECL)
#undef RRXR_DECL
#ifdef _WIN32
    PFN_xrGetOpenGLGraphicsRequirementsKHR xrGetOpenGLGraphicsRequirementsKHR = nullptr;
#else
    PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR = nullptr;
#endif
};

XrPosef IdentityPose() {
    XrPosef p{};
    p.orientation.w = 1.0f;
    return p;
}

#ifdef _WIN32
// RRJB_XR_RUNTIME=meta | vdxr | steamvr selects that runtime FOR THIS PROCESS ONLY (XR_RUNTIME_JSON), leaving the
// system's active runtime (HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime) unchanged; unset / system: the active one.
// The same policy as GT2's SelectRunningRuntime, reduced to its explicit choices.
void SelectRuntime() {
    if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr, 0)) return;
    const char* policy = std::getenv("RRJB_XR_RUNTIME");
    if (!policy || std::strcmp(policy, "system") == 0) return;
    const bool meta = std::strcmp(policy, "meta") == 0, vdxr = std::strcmp(policy, "vdxr") == 0,
               steam = std::strcmp(policy, "steamvr") == 0;
    if (!meta && !vdxr && !steam) throw std::runtime_error("RRJB_XR_RUNTIME must be system, meta, vdxr or steamvr");
    std::filesystem::path manifest;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const wchar_t* exe = meta ? L"OVRServer_x64.exe" : vdxr ? L"VirtualDesktop.Streamer.exe" : L"vrserver.exe";
    for (BOOL found = snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &entry); found;
         found = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, exe) != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t path[32768];
        DWORD size = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            const auto dir = std::filesystem::path(path).parent_path();
            const auto candidate = meta ? dir / "oculus_openxr_64.json"
                                   : vdxr ? dir / "OpenXR" / "virtualdesktop-openxr.json"
                                          : dir.parent_path().parent_path() / "steamxr_win64.json";
            if (std::filesystem::is_regular_file(candidate)) manifest = candidate;
        }
        CloseHandle(process);
        if (!manifest.empty()) break;
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    if (manifest.empty() && (meta || vdxr)) {
        wchar_t pf[32768];
        const DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, DWORD(std::size(pf)));
        if (n && n < std::size(pf)) {
            if (vdxr) {
                const auto c = std::filesystem::path(pf) / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json";
                if (std::filesystem::is_regular_file(c)) manifest = c;
            } else {
                for (const wchar_t* name : {L"Meta Horizon", L"Oculus"}) {
                    const auto c = std::filesystem::path(pf) / name / "Support" / "oculus-runtime" / "oculus_openxr_64.json";
                    if (std::filesystem::is_regular_file(c)) {
                        manifest = c;
                        break;
                    }
                }
            }
        }
    }
    if (manifest.empty()) throw std::runtime_error(std::string("RRJB_XR_RUNTIME=") + policy + ": that runtime was not found on this PC");
    if (!SetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifest.c_str()))
        throw std::runtime_error("cannot select the OpenXR runtime for this process");
    std::printf("xr: runtime manifest %s (this process only; the system default is unchanged)\n", manifest.string().c_str());
}

HMODULE LoadLoader(std::string& tried) {
    std::vector<std::string> candidates;
    char exePath[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n > 0) {
        std::string dir(exePath, n);
        const size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) candidates.push_back(dir.substr(0, slash + 1) + "openxr_loader.dll");
    }
    candidates.push_back("openxr_loader.dll");
    for (const std::string& c : candidates) {
        if (HMODULE m = LoadLibraryA(c.c_str())) return m;
        tried += (tried.empty() ? "" : ", ") + c;
    }
    return nullptr;
}
#endif

} // namespace

struct GlSession::Impl {
#ifdef _WIN32
    HMODULE loader = nullptr;
#endif
    Api api;
    Fbo fbo;
    std::unique_ptr<ControllerActions> controls;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE, stageSpace = XR_NULL_HANDLE, baseSpace = XR_NULL_HANDLE;
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool viewsValid = false;
    Chain eyes[2], quad;
    Target eyeTargets[2], quadTarget;
    DirectEye direct[2];
    bool directEyes = false, msaaToTexture = false;
    // single-pass stereo: eyes[0] is the two-layer swapchain, drawn through mvFbo with a two-layer depth of ours
    bool multiview = false, mvAcquired = false;
    GLuint mvFbo = 0, mvDepth = 0, mvReadFbo = 0;
    int mvSamples = 1;
    uint32_t mvIndex = 0;
    GLuint copyFbo = 0, copyProgram = 0, copyVao = 0;
    GLint copySrcLoc = -1, copyDecodeLoc = -1;
    bool decode = false; // the copy undoes the driver's sRGB encoding
    XrPosef quadPose = IdentityPose();
    // statistics over the current 5 s window
    Clock::time_point windowStart = Clock::now(), lastBegin{};
    long long windowFrames = 0, windowRendered = 0;
    double windowWaitMs = 0, windowWorkMs = 0, windowIntervalMs = 0, windowMaxIntervalMs = 0;
    Clock::time_point beginDone{};

    std::string Text(XrResult r) const {
        char buf[XR_MAX_RESULT_STRING_SIZE] = {};
        if (api.xrResultToString && instance != XR_NULL_HANDLE && api.xrResultToString(instance, r, buf) == XR_SUCCESS) return buf;
        return std::to_string(int(r));
    }
    void Check(XrResult r, const char* what) const {
        if (XR_FAILED(r)) throw std::runtime_error(std::string("OpenXR: ") + what + " failed (" + Text(r) + ")");
    }

    Target MakeTarget(int w, int h, int samples = 1) {
        Target t = MakeSingleTarget(w, h);
        if (samples <= 1) return t;
        GLint maxSamples = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        t.samples = std::clamp(samples, 1, std::max(1, int(maxSamples)));
        if (t.samples <= 1) return t;
        fbo.GenRenderbuffers(1, &t.msColor);
        fbo.BindRenderbuffer(GL_RENDERBUFFER, t.msColor);
        fbo.RenderbufferStorageMultisample(GL_RENDERBUFFER, t.samples, GL_RGBA8, w, h);
        fbo.GenRenderbuffers(1, &t.msDepth);
        fbo.BindRenderbuffer(GL_RENDERBUFFER, t.msDepth);
        fbo.RenderbufferStorageMultisample(GL_RENDERBUFFER, t.samples, GL_DEPTH24_STENCIL8, w, h);
        fbo.GenFramebuffers(1, &t.msFbo);
        fbo.BindFramebuffer(GL_FRAMEBUFFER, t.msFbo);
        fbo.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.msColor);
        fbo.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.msDepth);
        const GLenum status = fbo.CheckFramebufferStatus(GL_FRAMEBUFFER);
        fbo.BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::printf("xr: %dx eye multisampling is not available (status 0x%X): single-sampled\n", t.samples, unsigned(status));
            fbo.DeleteFramebuffers(1, &t.msFbo);
            fbo.DeleteRenderbuffers(1, &t.msColor);
            fbo.DeleteRenderbuffers(1, &t.msDepth);
            t.msFbo = t.msColor = t.msDepth = 0;
            t.samples = 1;
        }
        return t;
    }

    Target MakeSingleTarget(int w, int h) {
        Target t;
        t.width = w;
        t.height = h;
        glGenTextures(1, &t.color);
        glBindTexture(GL_TEXTURE_2D, t.color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        fbo.GenRenderbuffers(1, &t.depth);
        fbo.BindRenderbuffer(GL_RENDERBUFFER, t.depth);
        fbo.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        fbo.GenFramebuffers(1, &t.fbo);
        fbo.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        fbo.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
        fbo.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
        const GLenum status = fbo.CheckFramebufferStatus(GL_FRAMEBUFFER);
        fbo.BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("xr: the eye framebuffer is incomplete (status 0x" + std::to_string(status) + ")");
        return t;
    }

    void FreeTarget(Target& t) {
        if (t.fbo) fbo.DeleteFramebuffers(1, &t.fbo);
        if (t.depth) fbo.DeleteRenderbuffers(1, &t.depth);
        if (t.color) glDeleteTextures(1, &t.color);
        if (t.msFbo) fbo.DeleteFramebuffers(1, &t.msFbo);
        if (t.msColor) fbo.DeleteRenderbuffers(1, &t.msColor);
        if (t.msDepth) fbo.DeleteRenderbuffers(1, &t.msDepth);
        t = Target{};
    }

    // A direct eye's framebuffer and depth buffer (the colour is attached per frame: the acquired image).
    void MakeDirect(DirectEye& d, int w, int h, int samples) {
        FreeDirect(d);
        d.samples = msaaToTexture ? std::max(1, samples) : 1;
        fbo.GenRenderbuffers(1, &d.depth);
        fbo.BindRenderbuffer(GL_RENDERBUFFER, d.depth);
        if (d.samples > 1 && fbo.RenderbufferStorageMultisampleEXT)
            fbo.RenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, d.samples, GL_DEPTH24_STENCIL8, w, h);
        else
            fbo.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        fbo.BindRenderbuffer(GL_RENDERBUFFER, 0);
        fbo.GenFramebuffers(1, &d.fbo);
    }
    void FreeDirect(DirectEye& d) {
        if (d.fbo) fbo.DeleteFramebuffers(1, &d.fbo);
        if (d.depth) fbo.DeleteRenderbuffers(1, &d.depth);
        d = DirectEye{};
    }

    void Acquire(Chain& chain, uint32_t& index) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        Check(api.xrAcquireSwapchainImage(chain.handle, &ai, &index), "xrAcquireSwapchainImage");
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        Check(api.xrWaitSwapchainImage(chain.handle, &wi), "xrWaitSwapchainImage");
    }
    void Release(Chain& chain) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Check(api.xrReleaseSwapchainImage(chain.handle, &ri), "xrReleaseSwapchainImage");
        chain.finishedThisFrame = true;
        chain.everReleased = true;
    }

    void ApplyFoveation(int level) {
        if (!foveationOn || !api.xrCreateFoveationProfileFB || !api.xrUpdateSwapchainFB || !api.xrDestroyFoveationProfileFB) return;
        XrFoveationLevelProfileCreateInfoFB levelInfo{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
        levelInfo.level = level <= 0 ? XR_FOVEATION_LEVEL_NONE_FB
                        : level == 1 ? XR_FOVEATION_LEVEL_LOW_FB
                        : level == 2 ? XR_FOVEATION_LEVEL_MEDIUM_FB : XR_FOVEATION_LEVEL_HIGH_FB;
        levelInfo.verticalOffset = 0.0f;
        levelInfo.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
        XrFoveationProfileCreateInfoFB profileInfo{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
        profileInfo.next = &levelInfo;
        XrFoveationProfileFB profile = XR_NULL_HANDLE;
        const XrResult made = api.xrCreateFoveationProfileFB(session, &profileInfo, &profile);
        if (XR_FAILED(made)) {
            std::printf("xr: foveation profile refused (%s)\n", Text(made).c_str());
            return;
        }
        XrResult applied = XR_SUCCESS;
        for (Chain& c : eyes) {
            if (c.handle == XR_NULL_HANDLE) continue; // single-pass stereo: one two-layer swapchain
            XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
            state.profile = profile;
            const XrResult r = api.xrUpdateSwapchainFB(c.handle, reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&state));
            if (XR_FAILED(r)) applied = r;
        }
        api.xrDestroyFoveationProfileFB(profile);
        std::printf("xr: fixed foveation level %d on the eye swapchains: %s%s\n", level, Text(applied).c_str(),
                    directEyes ? "" : " (the eyes are copied into them, so it has no effect on the drawing)");
    }
    bool foveationOn = false;

    Chain MakeChain(int64_t format, int w, int h, uint32_t layers = 1) {
        Chain c;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = static_cast<uint32_t>(w);
        ci.height = static_cast<uint32_t>(h);
        ci.faceCount = 1;
        ci.arraySize = layers;
        ci.mipCount = 1;
        Check(api.xrCreateSwapchain(session, &ci, &c.handle), "xrCreateSwapchain");
        uint32_t n = 0;
        Check(api.xrEnumerateSwapchainImages(c.handle, 0, &n, nullptr), "xrEnumerateSwapchainImages");
#ifdef _WIN32
        std::vector<XrSwapchainImageOpenGLKHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
#else
        std::vector<XrSwapchainImageOpenGLESKHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#endif
        Check(api.xrEnumerateSwapchainImages(c.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
              "xrEnumerateSwapchainImages");
        for (const auto& image : images) c.images.push_back(image.image);
        c.width = w;
        c.height = h;
        return c;
    }

    // Our target -> the acquired image of `chain`, bytes unchanged. GL state the renderer relies on is restored.
    void CopyToChain(const Target& src, Chain& chain) {
        if (src.samples > 1) { // the multisampled picture resolved into the target's own texture first
            fbo.BindFramebuffer(GL_READ_FRAMEBUFFER, src.msFbo);
            fbo.BindFramebuffer(GL_DRAW_FRAMEBUFFER, src.fbo);
            glDisable(GL_SCISSOR_TEST);
            fbo.BlitFramebuffer(0, 0, src.width, src.height, 0, 0, src.width, src.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            fbo.BindFramebuffer(GL_FRAMEBUFFER, src.msFbo);
        }
        uint32_t index = 0;
        Acquire(chain, index);

        GLint prevFbo = 0, prevProgram = 0, prevVao = 0, prevActive = 0, prevTex = 0, vp[4] = {};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
        glGetIntegerv(GL_VIEWPORT, vp);
        const GLenum caps[] = {GL_DEPTH_TEST, GL_BLEND, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE};
        GLboolean was[5];
        for (int i = 0; i < 5; ++i) {
            was[i] = glIsEnabled(caps[i]);
            glDisable(caps[i]);
        }
        GLboolean mask[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.ActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

        fbo.BindFramebuffer(GL_FRAMEBUFFER, copyFbo);
        fbo.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, chain.images.at(index), 0);
        // an sRGB image stores what the shader writes only with the encoding off (desktop: FRAMEBUFFER_SRGB is off
        // unless enabled; ES: GL_EXT_sRGB_write_control, else the shader decodes first)
        if (!glCaps.es || glCaps.srgbWriteControl) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
        glViewport(0, 0, chain.width, chain.height);
        gl.UseProgram(copyProgram);
        gl.Uniform1i(copySrcLoc, 0);
        gl.Uniform1i(copyDecodeLoc, decode ? 1 : 0);
        glBindTexture(GL_TEXTURE_2D, src.color);
        gl.BindVertexArray(copyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        fbo.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        gl.ActiveTexture(static_cast<GLenum>(prevActive));
        gl.BindVertexArray(static_cast<GLuint>(prevVao));
        gl.UseProgram(static_cast<GLuint>(prevProgram));
        fbo.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        for (int i = 0; i < 5; ++i)
            if (was[i]) glEnable(caps[i]);

        Release(chain);
    }
};

const char* GlSession::StateName() const {
    switch (XrSessionState(state_)) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

bool GlSession::Focused() const { return state_ == XR_SESSION_STATE_FOCUSED; }

GlSession::GlSession(const SessionOptions& options) : impl_(std::make_unique<Impl>()), options_(options) {
    Impl& s = *impl_;
#ifdef _WIN32
    SelectRuntime();
    std::string tried;
    s.loader = LoadLoader(tried);
    if (!s.loader) throw std::runtime_error("no OpenXR loader (tried " + tried + "): put openxr_loader.dll next to the executable");
    s.api.gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(s.loader, "xrGetInstanceProcAddr")));
    if (!s.api.gipa) throw std::runtime_error("the OpenXR loader has no xrGetInstanceProcAddr");
    if (!options_.binding.hdc || !options_.binding.hglrc) throw std::runtime_error("xr: the GL binding needs the HDC and HGLRC");
    info_.graphicsApi = "OpenGL (XR_KHR_opengl_enable)";
#elif defined(__ANDROID__)
    if (!options_.androidVm || !options_.androidActivity)
        throw std::runtime_error("OpenXR (Android): the session needs the activity's JavaVM and its instance");
    {
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        const XrResult got = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                                   reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
        if (XR_FAILED(got) || !initializeLoader)
            throw std::runtime_error("OpenXR (Android): the loader has no xrInitializeLoaderKHR (" + std::to_string(int(got)) + ")");
        XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        init.applicationVM = options_.androidVm;
        init.applicationContext = options_.androidActivity;
        const XrResult r0 = initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init));
        if (XR_FAILED(r0)) throw std::runtime_error("OpenXR (Android): xrInitializeLoaderKHR failed (" + std::to_string(int(r0)) + ")");
    }
    s.api.gipa = &xrGetInstanceProcAddr;
    if (!options_.binding.eglDisplay || !options_.binding.eglConfig || !options_.binding.eglContext)
        throw std::runtime_error("xr: the GLES binding needs the EGL display, config and context");
    info_.graphicsApi = "OpenGL ES (XR_KHR_opengl_es_enable)";
#else
    throw std::runtime_error("the OpenXR GL session is implemented for Windows and Android only");
#endif
    s.fbo.Load();

    s.api.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
               reinterpret_cast<PFN_xrVoidFunction*>(&s.api.xrEnumerateInstanceExtensionProperties));
    s.api.gipa(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&s.api.xrCreateInstance));
    if (!s.api.xrEnumerateInstanceExtensionProperties || !s.api.xrCreateInstance)
        throw std::runtime_error("the OpenXR loader found no runtime");
    uint32_t extCount = 0;
    XrResult r = s.api.xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) throw std::runtime_error("OpenXR: no runtime available (xrEnumerateInstanceExtensionProperties -> " + std::to_string(int(r)) + ")");
    std::vector<XrExtensionProperties> props(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    s.api.xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props.data());
    auto has = [&](const char* name) {
        for (const XrExtensionProperties& p : props)
            if (std::strcmp(p.extensionName, name) == 0) return true;
        return false;
    };
#ifdef _WIN32
    const char* graphicsExt = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
#else
    const char* graphicsExt = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
#endif
    if (!has(graphicsExt)) throw std::runtime_error(std::string("the OpenXR runtime does not offer ") + graphicsExt);
    std::vector<const char*> extensions{graphicsExt};
#ifdef __ANDROID__
    const bool androidCreate = has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    if (androidCreate) extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
    info_.refreshRateExtension = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (info_.refreshRateExtension) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    info_.performanceExtension = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (info_.performanceExtension) extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    // fixed foveation (the Quest): the profile, its level configuration and the swapchain update that applies it
    info_.foveationExtension = has(XR_FB_FOVEATION_EXTENSION_NAME) && has(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) &&
                               has(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    if (info_.foveationExtension) {
        extensions.push_back(XR_FB_FOVEATION_EXTENSION_NAME);
        extensions.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
        extensions.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    }

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = options_.androidVm;
    androidInfo.applicationActivity = options_.androidActivity;
    if (androidCreate) ici.next = &androidInfo;
#endif
    std::snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "%s", options_.appName.c_str());
    std::snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "%s", "rrjb");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.enabledExtensionNames = extensions.data();
    r = s.api.xrCreateInstance(&ici, &s.instance);
    if (XR_FAILED(r)) throw std::runtime_error("OpenXR: xrCreateInstance failed (" + std::to_string(int(r)) + ")");

#define RRXR_LOAD(n) s.api.gipa(s.instance, #n, reinterpret_cast<PFN_xrVoidFunction*>(&s.api.n));
    RRXR_FUNCTIONS(RRXR_LOAD)
    RRXR_OPTIONAL_FUNCTIONS(RRXR_LOAD)
#ifdef _WIN32
    RRXR_LOAD(xrGetOpenGLGraphicsRequirementsKHR)
#else
    RRXR_LOAD(xrGetOpenGLESGraphicsRequirementsKHR)
#endif
#undef RRXR_LOAD
#define RRXR_REQUIRE(n) if (!s.api.n) throw std::runtime_error("OpenXR: the runtime does not provide " #n);
    RRXR_FUNCTIONS(RRXR_REQUIRE)
#undef RRXR_REQUIRE

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (s.api.xrGetInstanceProperties(s.instance, &ip) == XR_SUCCESS) {
        info_.runtimeName = ip.runtimeName;
        info_.runtimeName += " " + std::to_string(XR_VERSION_MAJOR(ip.runtimeVersion)) + "." +
                             std::to_string(XR_VERSION_MINOR(ip.runtimeVersion)) + "." +
                             std::to_string(XR_VERSION_PATCH(ip.runtimeVersion));
    }
    std::printf("xr: runtime %s, %s\n", info_.runtimeName.c_str(), info_.graphicsApi.c_str());

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = s.api.xrGetSystem(s.instance, &sgi, &s.system);
    if (XR_FAILED(r))
        throw std::runtime_error("OpenXR [" + info_.runtimeName + "]: no headset (xrGetSystem -> " + s.Text(r) +
                                 "). Connect and wake the headset in this runtime first.");
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (s.api.xrGetSystemProperties(s.instance, s.system, &sp) == XR_SUCCESS) info_.systemName = sp.systemName;

    uint32_t viewCount = 0;
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    s.Check(s.api.xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                    &viewCount, vcv),
            "xrEnumerateViewConfigurationViews");
    if (viewCount != 2) throw std::runtime_error("OpenXR: the stereo view configuration has " + std::to_string(viewCount) + " views");
    info_.recommendedWidth = vcv[0].recommendedImageRectWidth;
    info_.recommendedHeight = vcv[0].recommendedImageRectHeight;
    info_.maxWidth = vcv[0].maxImageRectWidth;
    info_.maxHeight = vcv[0].maxImageRectHeight;

    // the graphics requirements must be read before the session is created (the specification says so)
#ifdef _WIN32
    if (!s.api.xrGetOpenGLGraphicsRequirementsKHR) throw std::runtime_error("OpenXR: no xrGetOpenGLGraphicsRequirementsKHR");
    XrGraphicsRequirementsOpenGLKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    s.Check(s.api.xrGetOpenGLGraphicsRequirementsKHR(s.instance, s.system, &req), "xrGetOpenGLGraphicsRequirementsKHR");
#else
    if (!s.api.xrGetOpenGLESGraphicsRequirementsKHR) throw std::runtime_error("OpenXR: no xrGetOpenGLESGraphicsRequirementsKHR");
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    s.Check(s.api.xrGetOpenGLESGraphicsRequirementsKHR(s.instance, s.system, &req), "xrGetOpenGLESGraphicsRequirementsKHR");
#endif
    std::printf("xr: system '%s', recommended eye %ux%u (max %ux%u), GL API %u.%u .. %u.%u\n", info_.systemName.c_str(),
                info_.recommendedWidth, info_.recommendedHeight, info_.maxWidth, info_.maxHeight,
                unsigned(XR_VERSION_MAJOR(req.minApiVersionSupported)), unsigned(XR_VERSION_MINOR(req.minApiVersionSupported)),
                unsigned(XR_VERSION_MAJOR(req.maxApiVersionSupported)), unsigned(XR_VERSION_MINOR(req.maxApiVersionSupported)));

#ifdef _WIN32
    XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    binding.hDC = static_cast<HDC>(options_.binding.hdc);
    binding.hGLRC = static_cast<HGLRC>(options_.binding.hglrc);
#else
    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = static_cast<EGLDisplay>(options_.binding.eglDisplay);
    binding.config = static_cast<EGLConfig>(options_.binding.eglConfig);
    binding.context = static_cast<EGLContext>(options_.binding.eglContext);
#endif
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = s.system;
    s.Check(s.api.xrCreateSession(s.instance, &sci, &s.session), "xrCreateSession");

    // --- spaces: LOCAL is the seated world, VIEW the head, STAGE the floor when the runtime has one
    uint32_t spaceCount = 0;
    s.api.xrEnumerateReferenceSpaces(s.session, 0, &spaceCount, nullptr);
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    s.api.xrEnumerateReferenceSpaces(s.session, spaceCount, &spaceCount, spaces.data());
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace = IdentityPose();
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    s.Check(s.api.xrCreateReferenceSpace(s.session, &rsci, &s.localSpace), "xrCreateReferenceSpace(LOCAL)");
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    s.Check(s.api.xrCreateReferenceSpace(s.session, &rsci, &s.viewSpace), "xrCreateReferenceSpace(VIEW)");
    if (std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end()) {
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        info_.stageSpace = s.api.xrCreateReferenceSpace(s.session, &rsci, &s.stageSpace) == XR_SUCCESS;
    }
    s.baseSpace = options_.preferStage && info_.stageSpace ? s.stageSpace : s.localSpace;

    // --- swapchains: sRGB preferred (the compositor then reads our display-referred bytes as what they are)
    uint32_t formatCount = 0;
    s.api.xrEnumerateSwapchainFormats(s.session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    s.api.xrEnumerateSwapchainFormats(s.session, formatCount, &formatCount, formats.data());
    auto hasFormat = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
    if (hasFormat(GL_SRGB8_ALPHA8)) {
        info_.swapchainFormat = GL_SRGB8_ALPHA8;
        info_.srgbSwapchain = true;
    } else if (hasFormat(GL_RGBA8)) {
        info_.swapchainFormat = GL_RGBA8;
    } else {
        throw std::runtime_error("OpenXR: the runtime offers neither GL_SRGB8_ALPHA8 nor GL_RGBA8 swapchains");
    }
    s.decode = info_.srgbSwapchain && glCaps.es && !glCaps.srgbWriteControl;
    const float scale = std::clamp(options_.eyeScale, 0.25f, 4.0f);
    info_.eyeWidth = std::min<uint32_t>(info_.maxWidth ? info_.maxWidth : 16384, uint32_t(std::lround(info_.recommendedWidth * scale)));
    info_.eyeHeight = std::min<uint32_t>(info_.maxHeight ? info_.maxHeight : 16384, uint32_t(std::lround(info_.recommendedHeight * scale)));
    // Direct eyes: only where the stored bytes stay the game's own (ES with the sRGB write control switched off, or an
    // RGBA8 swapchain) - the desktop keeps the copy path rrvrtest uses.
    s.directEyes = options_.directEyes && glCaps.es && (!info_.srgbSwapchain || glCaps.srgbWriteControl);
    s.msaaToTexture = glCaps.es && HasGlExtension("GL_EXT_multisampled_render_to_texture") &&
                      s.fbo.FramebufferTexture2DMultisampleEXT && s.fbo.RenderbufferStorageMultisampleEXT;
    info_.msaaToTexture = s.msaaToTexture;
    info_.directEyes = s.directEyes;
    // single-pass stereo: direct eyes only (the swapchain's array image is the target), the extensions the samples need
    if (s.directEyes && options_.multiview) {
        const render::MultiviewCaps mv = render::DetectMultiview();
        s.multiview = mv.multiview && (options_.eyeSamples <= 1 || mv.multisampled || !s.msaaToTexture);
        s.mvSamples = mv.multisampled && s.msaaToTexture ? std::max(1, options_.eyeSamples) : 1;
        if (!mv.multiview) std::printf("xr: GL_OVR_multiview2 not offered: a pass per eye\n");
    }
    info_.multiview = s.multiview;
    if (s.multiview) {
        s.eyes[0] = s.MakeChain(info_.swapchainFormat, int(info_.eyeWidth), int(info_.eyeHeight), 2);
        s.eyes[1] = Chain{};
        const render::MultiviewGl& f = render::Mvgl();
        glGenTextures(1, &s.mvDepth);
        glBindTexture(GL_TEXTURE_2D_ARRAY, s.mvDepth);
        f.TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH24_STENCIL8, GLsizei(info_.eyeWidth), GLsizei(info_.eyeHeight), 2, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        s.fbo.GenFramebuffers(1, &s.mvFbo);
        s.fbo.GenFramebuffers(1, &s.mvReadFbo);
        info_.eyeSamples = s.mvSamples;
    } else {
        for (int e = 0; e < 2; ++e) {
            s.eyes[e] = s.MakeChain(info_.swapchainFormat, int(info_.eyeWidth), int(info_.eyeHeight));
            if (s.directEyes) s.MakeDirect(s.direct[e], int(info_.eyeWidth), int(info_.eyeHeight), options_.eyeSamples);
            else s.eyeTargets[e] = s.MakeTarget(int(info_.eyeWidth), int(info_.eyeHeight), options_.eyeSamples);
        }
        info_.eyeSamples = s.directEyes ? s.direct[0].samples : s.eyeTargets[0].samples;
    }
    s.quad = s.MakeChain(info_.swapchainFormat, int(options_.quadWidth), int(options_.quadHeight));
    s.quadTarget = s.MakeTarget(int(options_.quadWidth), int(options_.quadHeight));
    s.fbo.GenFramebuffers(1, &s.copyFbo);
    s.copyProgram = render::BuildProgram(kCopyVs, kCopyFs);
    s.copySrcLoc = gl.GetUniformLocation(s.copyProgram, "uSrc");
    s.copyDecodeLoc = gl.GetUniformLocation(s.copyProgram, "uDecode");
    gl.GenVertexArrays(1, &s.copyVao);
    std::printf("xr: eye swapchains %ux%u x2 (%.2f x recommended), quad %ux%u, format %s%s, %zu images each; space %s\n",
                info_.eyeWidth, info_.eyeHeight, double(scale), options_.quadWidth, options_.quadHeight,
                info_.srgbSwapchain ? "GL_SRGB8_ALPHA8" : "GL_RGBA8",
                s.decode ? " (the copy decodes: no GL_EXT_sRGB_write_control)" : " (bytes stored unencoded)",
                s.eyes[0].images.size(), s.baseSpace == s.stageSpace ? "STAGE" : "LOCAL");
    std::printf("xr: eyes drawn %s, %dx multisampling%s; foveation extension %s\n",
                s.multiview ? "single-pass stereo (GL_OVR_multiview2) straight into one two-layer swapchain"
                : s.directEyes ? "straight into the swapchain images" : "into our own targets and copied", info_.eyeSamples,
                s.multiview && s.mvSamples > 1 ? " (GL_OVR_multiview_multisampled_render_to_texture)"
                : s.msaaToTexture ? " (GL_EXT_multisampled_render_to_texture)" : "", info_.foveationExtension ? "yes" : "no");
    s.foveationOn = info_.foveationExtension;
    if (options_.foveation > 0) s.ApplyFoveation(options_.foveation);

    s.controls = std::make_unique<ControllerActions>(s.instance, s.session, s.api.gipa);

    if (info_.refreshRateExtension && s.api.xrEnumerateDisplayRefreshRatesFB) {
        uint32_t n = 0;
        s.api.xrEnumerateDisplayRefreshRatesFB(s.session, 0, &n, nullptr);
        info_.refreshRates.resize(n);
        s.api.xrEnumerateDisplayRefreshRatesFB(s.session, n, &n, info_.refreshRates.data());
        if (options_.refreshHz > 0) SetRefreshRate(options_.refreshHz);
        float current = 0;
        if (s.api.xrGetDisplayRefreshRateFB && s.api.xrGetDisplayRefreshRateFB(s.session, &current) == XR_SUCCESS)
            info_.refreshHz = double(current);
        std::string list;
        for (float f : info_.refreshRates) list += (list.empty() ? "" : " ") + std::to_string(int(std::lround(f)));
        std::printf("xr: display refresh %.1f Hz (offered: %s)\n", info_.refreshHz, list.c_str());
    }
}

GlSession::~GlSession() {
    Impl& s = *impl_;
    s.controls.reset();
    for (Chain* c : {&s.eyes[0], &s.eyes[1], &s.quad})
        if (c->handle != XR_NULL_HANDLE && s.api.xrDestroySwapchain) s.api.xrDestroySwapchain(c->handle);
    for (Target* t : {&s.eyeTargets[0], &s.eyeTargets[1], &s.quadTarget}) s.FreeTarget(*t);
    for (DirectEye& d : s.direct) s.FreeDirect(d);
    if (s.copyFbo) s.fbo.DeleteFramebuffers(1, &s.copyFbo);
    if (s.mvFbo) s.fbo.DeleteFramebuffers(1, &s.mvFbo);
    if (s.mvReadFbo) s.fbo.DeleteFramebuffers(1, &s.mvReadFbo);
    if (s.mvDepth) glDeleteTextures(1, &s.mvDepth);
    for (XrSpace space : {s.localSpace, s.viewSpace, s.stageSpace})
        if (space != XR_NULL_HANDLE && s.api.xrDestroySpace) s.api.xrDestroySpace(space);
    if (s.session != XR_NULL_HANDLE && s.api.xrDestroySession) s.api.xrDestroySession(s.session);
    if (s.instance != XR_NULL_HANDLE && s.api.xrDestroyInstance) s.api.xrDestroyInstance(s.instance);
#ifdef _WIN32
    if (s.loader) FreeLibrary(s.loader);
#endif
}

bool GlSession::SetRefreshRate(float hz) {
    Impl& s = *impl_;
    if (!info_.refreshRateExtension || !s.api.xrRequestDisplayRefreshRateFB) return false;
    const XrResult r = s.api.xrRequestDisplayRefreshRateFB(s.session, hz);
    if (XR_FAILED(r)) {
        std::printf("xr: %.1f Hz refused by the runtime (%s)\n", double(hz), s.Text(r).c_str());
        return false;
    }
    return true;
}

void GlSession::SetQuadPlacement(float metres, float distance, float heightOffset, float yawOffset) {
    options_.quadMetres = metres;
    options_.quadDistance = distance;
    options_.quadHeightOffset = heightOffset;
    options_.quadYawOffset = yawOffset;
    quadLatched_ = false;
}

void GlSession::SetEyeSamples(int samples) {
    Impl& s = *impl_;
    samples = std::max(1, samples);
    if (s.multiview) { // the samples are the attachment's (multisampled render-to-texture): nothing to re-create
        const render::MultiviewCaps mv = render::DetectMultiview();
        const int now = mv.multisampled && s.msaaToTexture ? samples : 1;
        if (now == s.mvSamples) return;
        s.mvSamples = now;
        info_.eyeSamples = now;
    } else if (s.directEyes) {
        if ((s.msaaToTexture ? samples : 1) == s.direct[0].samples) return;
        for (DirectEye& d : s.direct) s.MakeDirect(d, int(info_.eyeWidth), int(info_.eyeHeight), samples);
        info_.eyeSamples = s.direct[0].samples;
    } else {
        if (samples == s.eyeTargets[0].samples) return;
        for (Target& t : s.eyeTargets) {
            s.FreeTarget(t);
            t = s.MakeTarget(int(info_.eyeWidth), int(info_.eyeHeight), samples);
        }
        info_.eyeSamples = s.eyeTargets[0].samples;
    }
    std::printf("xr: eye multisampling now %dx\n", info_.eyeSamples);
}

void GlSession::SetFoveation(int level) { impl_->ApplyFoveation(level); }

namespace {
void ReadBound(const Fbo& f, GLuint fbo, int w, int h, std::vector<uint8_t>& rgba) {
    rgba.assign(size_t(w) * size_t(h) * 4, 0);
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    f.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    f.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
}
} // namespace

bool GlSession::ReadEye(int eye, std::vector<uint8_t>& rgba, int& width, int& height) {
    Impl& s = *impl_;
    if (s.multiview) { // the layer of the acquired two-layer image
        if (!s.mvAcquired) return false;
        width = s.eyes[0].width;
        height = s.eyes[0].height;
        GLint prev = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
        s.fbo.BindFramebuffer(GL_FRAMEBUFFER, s.mvReadFbo);
        render::Mvgl().FramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s.eyes[0].images.at(s.mvIndex), 0, eye & 1);
        rgba.assign(size_t(width) * size_t(height) * 4, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        s.fbo.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
        return true;
    }
    if (s.directEyes) {
        const DirectEye& d = s.direct[eye & 1];
        if (!d.acquired) return false;
        width = s.eyes[eye & 1].width;
        height = s.eyes[eye & 1].height;
        s.fbo.BindFramebuffer(GL_FRAMEBUFFER, d.fbo);
        ReadBound(s.fbo, d.fbo, width, height, rgba);
        return true;
    }
    const Target& t = s.eyeTargets[eye & 1];
    if (t.samples > 1) {
        s.fbo.BindFramebuffer(GL_READ_FRAMEBUFFER, t.msFbo);
        s.fbo.BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
        s.fbo.BlitFramebuffer(0, 0, t.width, t.height, 0, 0, t.width, t.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    width = t.width;
    height = t.height;
    ReadBound(s.fbo, t.fbo, width, height, rgba);
    s.fbo.BindFramebuffer(GL_FRAMEBUFFER, t.DrawFbo());
    return true;
}

bool GlSession::ReadQuad(std::vector<uint8_t>& rgba, int& width, int& height) {
    Impl& s = *impl_;
    width = s.quadTarget.width;
    height = s.quadTarget.height;
    ReadBound(s.fbo, s.quadTarget.fbo, width, height, rgba);
    return true;
}

bool GlSession::PollEvents() {
    Impl& s = *impl_;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (s.api.xrPollEvent(s.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            state_ = int(changed.state);
            std::printf("xr: session %s\n", StateName());
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                s.Check(s.api.xrBeginSession(s.session, &bi), "xrBeginSession");
                running_ = true;
                if (info_.performanceExtension && s.api.xrPerfSettingsSetPerformanceLevelEXT) {
                    const XrResult cpu = s.api.xrPerfSettingsSetPerformanceLevelEXT(s.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                                                                                    XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                    const XrResult gpu = s.api.xrPerfSettingsSetPerformanceLevelEXT(s.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                                                                                    XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                    std::printf("xr: performance CPU sustained-high %s, GPU sustained-high %s\n", s.Text(cpu).c_str(), s.Text(gpu).c_str());
                }
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                running_ = false;
                frameOpen_ = false;
                s.Check(s.api.xrEndSession(s.session), "xrEndSession");
            } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                running_ = false;
                quit_ = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            std::printf("xr: instance loss pending\n");
            running_ = false;
            quit_ = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            quadLatched_ = false; // the runtime recentred: the theatre screen is placed again ahead of the head
            recenterEvent_ = true; // ... and the game's seat (rrgame: vr_rig.h Recenter)
            std::printf("xr: the runtime recentred its space\n");
            break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& rate = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(event);
            info_.refreshHz = double(rate.toDisplayRefreshRate);
            std::printf("xr: display refresh rate %.1f Hz\n", info_.refreshHz);
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            std::printf("xr: interaction profile changed\n");
            break;
        default: break;
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !quit_;
}

void GlSession::RequestExit() {
    Impl& s = *impl_;
    if (running_ && s.api.xrRequestExitSession) s.api.xrRequestExitSession(s.session);
    else quit_ = true;
}

bool GlSession::BeginFrame() {
    Impl& s = *impl_;
    if (!running_ || quit_) return false;
    const Clock::time_point t0 = Clock::now();
    s.frameState = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    const XrResult waited = s.api.xrWaitFrame(s.session, &fwi, &s.frameState);
    if (waited == XR_ERROR_SESSION_NOT_RUNNING) return false;
    s.Check(waited, "xrWaitFrame");
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult began = s.api.xrBeginFrame(s.session, &fbi);
    if (began == XR_ERROR_SESSION_NOT_RUNNING) return false;
    s.Check(began, "xrBeginFrame");
    const Clock::time_point t1 = Clock::now();
    frameOpen_ = true;
    shouldRender_ = s.frameState.shouldRender == XR_TRUE;
    displayTime_ = s.frameState.predictedDisplayTime;
    displayPeriod_ = s.frameState.predictedDisplayPeriod;
    s.viewsValid = false;
    for (Chain* c : {&s.eyes[0], &s.eyes[1], &s.quad}) c->finishedThisFrame = false;
    s.windowWaitMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (s.lastBegin != Clock::time_point{}) {
        const double interval = std::chrono::duration<double, std::milli>(t0 - s.lastBegin).count();
        s.windowIntervalMs += interval;
        s.windowMaxIntervalMs = std::max(s.windowMaxIntervalMs, interval);
    }
    s.lastBegin = t0;
    s.beginDone = t1;
    return true;
}

bool GlSession::LocateViews(EyeView eyes[2], Pose& head) {
    Impl& s = *impl_;
    if (!frameOpen_) return false;
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = s.frameState.predictedDisplayTime;
    vli.space = s.baseSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    s.views[0] = XrView{XR_TYPE_VIEW};
    s.views[1] = XrView{XR_TYPE_VIEW};
    if (XR_FAILED(s.api.xrLocateViews(s.session, &vli, &vs, 2, &count, s.views)) || count != 2) return false;
    constexpr XrViewStateFlags kValid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((vs.viewStateFlags & kValid) != kValid) return false;
    for (int v = 0; v < 2; ++v) {
        const XrView& x = s.views[v];
        eyes[v].pose.position[0] = x.pose.position.x;
        eyes[v].pose.position[1] = x.pose.position.y;
        eyes[v].pose.position[2] = x.pose.position.z;
        eyes[v].pose.orientation[0] = x.pose.orientation.x;
        eyes[v].pose.orientation[1] = x.pose.orientation.y;
        eyes[v].pose.orientation[2] = x.pose.orientation.z;
        eyes[v].pose.orientation[3] = x.pose.orientation.w;
        eyes[v].fov = {x.fov.angleLeft, x.fov.angleRight, x.fov.angleUp, x.fov.angleDown};
    }
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kPose = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (s.api.xrLocateSpace(s.viewSpace, s.baseSpace, s.frameState.predictedDisplayTime, &loc) == XR_SUCCESS &&
        (loc.locationFlags & kPose) == kPose) {
        head.position[0] = loc.pose.position.x;
        head.position[1] = loc.pose.position.y;
        head.position[2] = loc.pose.position.z;
        head.orientation[0] = loc.pose.orientation.x;
        head.orientation[1] = loc.pose.orientation.y;
        head.orientation[2] = loc.pose.orientation.z;
        head.orientation[3] = loc.pose.orientation.w;
    } else {
        for (int k = 0; k < 3; ++k) head.position[k] = 0.5f * (eyes[0].pose.position[k] + eyes[1].pose.position[k]);
        for (int k = 0; k < 4; ++k) head.orientation[k] = eyes[0].pose.orientation[k];
    }
    s.viewsValid = true;
    return true;
}

FrameTarget GlSession::BindEye(int eye) {
    Impl& s = *impl_;
    if (s.directEyes) {
        DirectEye& d = s.direct[eye & 1];
        Chain& chain = s.eyes[eye & 1];
        if (!d.acquired && frameOpen_ && shouldRender_) {
            s.Acquire(chain, d.index);
            d.acquired = true;
        }
        s.fbo.BindFramebuffer(GL_FRAMEBUFFER, d.fbo);
        const GLuint image = d.acquired ? chain.images.at(d.index) : 0;
        if (d.samples > 1)
            s.fbo.FramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0, d.samples);
        else
            s.fbo.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
        s.fbo.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, d.depth);
        if (glCaps.srgbWriteControl) glDisable(GL_FRAMEBUFFER_SRGB_EXT); // the game's bytes stored as they are
        glViewport(0, 0, chain.width, chain.height);
        return {d.fbo, chain.width, chain.height};
    }
    const Target& t = s.eyeTargets[eye & 1];
    s.fbo.BindFramebuffer(GL_FRAMEBUFFER, t.DrawFbo());
    glViewport(0, 0, t.width, t.height);
    return {t.DrawFbo(), t.width, t.height};
}

void GlSession::FinishEye(int eye) {
    Impl& s = *impl_;
    if (s.directEyes) {
        DirectEye& d = s.direct[eye & 1];
        if (!d.acquired) return;
        if (s.fbo.InvalidateFramebuffer) { // the depth never leaves the tile
            const GLenum depth[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
            s.fbo.BindFramebuffer(GL_FRAMEBUFFER, d.fbo);
            s.fbo.InvalidateFramebuffer(GL_FRAMEBUFFER, 2, depth);
        }
        s.fbo.BindFramebuffer(GL_FRAMEBUFFER, 0);
        s.Release(s.eyes[eye & 1]);
        d.acquired = false;
        return;
    }
    if (!frameOpen_ || !shouldRender_) return;
    s.CopyToChain(s.eyeTargets[eye & 1], s.eyes[eye & 1]);
}

FrameTarget GlSession::BindEyes() {
    Impl& s = *impl_;
    if (!s.multiview) return {};
    Chain& chain = s.eyes[0];
    if (!s.mvAcquired && frameOpen_ && shouldRender_) {
        s.Acquire(chain, s.mvIndex);
        s.mvAcquired = true;
    }
    const render::MultiviewGl& f = render::Mvgl();
    s.fbo.BindFramebuffer(GL_FRAMEBUFFER, s.mvFbo);
    const GLuint image = s.mvAcquired ? chain.images.at(s.mvIndex) : 0;
    if (s.mvSamples > 1) {
        f.FramebufferTextureMultisampleMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, s.mvDepth, 0, s.mvSamples, 0, 2);
        f.FramebufferTextureMultisampleMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, image, 0, s.mvSamples, 0, 2);
    } else {
        f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, s.mvDepth, 0, 0, 2);
        f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, image, 0, 0, 2);
    }
    if (glCaps.srgbWriteControl) glDisable(GL_FRAMEBUFFER_SRGB_EXT); // the game's bytes stored as they are
    glViewport(0, 0, chain.width, chain.height);
    return {s.mvFbo, chain.width, chain.height};
}

void GlSession::FinishEyes() {
    Impl& s = *impl_;
    if (!s.multiview || !s.mvAcquired) return;
    if (s.fbo.InvalidateFramebuffer) { // the depth never leaves the tile
        const GLenum depth[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
        s.fbo.BindFramebuffer(GL_FRAMEBUFFER, s.mvFbo);
        s.fbo.InvalidateFramebuffer(GL_FRAMEBUFFER, 2, depth);
    }
    s.fbo.BindFramebuffer(GL_FRAMEBUFFER, 0);
    s.Release(s.eyes[0]);
    s.eyes[1].finishedThisFrame = true; // both views are in the one image
    s.mvAcquired = false;
}

FrameTarget GlSession::BindQuad() {
    Impl& s = *impl_;
    s.fbo.BindFramebuffer(GL_FRAMEBUFFER, s.quadTarget.fbo);
    glViewport(0, 0, s.quadTarget.width, s.quadTarget.height);
    return {s.quadTarget.fbo, s.quadTarget.width, s.quadTarget.height};
}

void GlSession::FinishQuad() {
    Impl& s = *impl_;
    if (!frameOpen_ || !shouldRender_) return;
    s.CopyToChain(s.quadTarget, s.quad);
}

void GlSession::EndFrame() {
    Impl& s = *impl_;
    if (!frameOpen_) return;
    s.fbo.BindFramebuffer(GL_FRAMEBUFFER, 0);
    const Clock::time_point t0 = Clock::now();
    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const bool stereo = shouldRender_ && s.viewsValid && s.eyes[0].finishedThisFrame && s.eyes[1].finishedThisFrame;
    if (stereo) {
        for (int v = 0; v < 2; ++v) {
            const Chain& c = s.multiview ? s.eyes[0] : s.eyes[v]; // single-pass stereo: layer v of the one swapchain
            pv[v].pose = s.views[v].pose;
            pv[v].fov = s.views[v].fov;
            pv[v].subImage.swapchain = c.handle;
            pv[v].subImage.imageRect = {{0, 0}, {c.width, c.height}};
            pv[v].subImage.imageArrayIndex = s.multiview ? uint32_t(v) : 0u;
        }
        projection.space = s.baseSpace;
        projection.viewCount = 2;
        projection.views = pv;
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));
    }
    if (shouldRender_ && (s.quad.finishedThisFrame || (keepQuad_ && s.quad.everReleased))) {
        if (!quadLatched_) {
            // the theatre screen: ahead of the head with the yaw it had when the screen appeared (plus the offset),
            // at its height - latched, so it stays put while the player looks around
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            constexpr XrSpaceLocationFlags kPose = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if (s.api.xrLocateSpace(s.viewSpace, s.baseSpace, s.frameState.predictedDisplayTime, &head) == XR_SUCCESS &&
                (head.locationFlags & kPose) == kPose) {
                Pose p;
                p.orientation[0] = head.pose.orientation.x;
                p.orientation[1] = head.pose.orientation.y;
                p.orientation[2] = head.pose.orientation.z;
                p.orientation[3] = head.pose.orientation.w;
                const float yaw = YawOf(p) + options_.quadYawOffset;
                s.quadPose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
                s.quadPose.position = {head.pose.position.x - std::sin(yaw) * options_.quadDistance,
                                       head.pose.position.y + options_.quadHeightOffset,
                                       head.pose.position.z - std::cos(yaw) * options_.quadDistance};
                quadLatched_ = true;
                std::printf("xr: theatre quad at (%.2f, %.2f, %.2f), yaw %.1f deg, %.2f m wide\n", double(s.quadPose.position.x),
                            double(s.quadPose.position.y), double(s.quadPose.position.z), double(yaw) * 57.29577951308232,
                            double(options_.quadMetres));
            }
        }
        if (quadLatched_) {
            quad.space = s.baseSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = s.quad.handle;
            quad.subImage.imageRect = {{0, 0}, {s.quad.width, s.quad.height}};
            quad.pose = s.quadPose;
            quad.size = {options_.quadMetres, options_.quadMetres * float(s.quad.height) / float(s.quad.width)};
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
        }
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = s.frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = static_cast<uint32_t>(layers.size());
    fei.layers = layers.data();
    s.Check(s.api.xrEndFrame(s.session, &fei), "xrEndFrame");
    frameOpen_ = false;
    ++frameIndex_;

    const Clock::time_point t1 = Clock::now();
    s.windowWorkMs += std::chrono::duration<double, std::milli>(t0 - s.beginDone).count();
    ++s.windowFrames;
    if (!layers.empty()) ++s.windowRendered;
    const double seconds = std::chrono::duration<double>(t1 - s.windowStart).count();
    if (seconds >= 5.0) {
        const double n = double(std::max<long long>(s.windowFrames, 1));
        std::printf("xr: frames %lld, %.1f fps over %.1f s (display period %.3f ms = %.1f Hz), %lld with layers; "
                    "per frame: xrWaitFrame+xrBeginFrame %.2f ms, app work %.2f ms, interval avg %.2f / max %.2f ms; state %s\n",
                    frameIndex_, double(s.windowFrames) / seconds, seconds, double(displayPeriod_) / 1e6,
                    displayPeriod_ > 0 ? 1e9 / double(displayPeriod_) : 0.0, s.windowRendered, s.windowWaitMs / n,
                    s.windowWorkMs / n, s.windowIntervalMs / std::max(n - 1.0, 1.0), s.windowMaxIntervalMs, StateName());
        s.windowStart = t1;
        s.windowFrames = s.windowRendered = 0;
        s.windowWaitMs = s.windowWorkMs = s.windowIntervalMs = s.windowMaxIntervalMs = 0;
    }
}

bool GlSession::PollPad(XrPad& pad) {
    Impl& s = *impl_;
    return s.controls->Poll(pad, running_ && Focused());
}

void GlSession::Vibrate(float left, float right) {
    Impl& s = *impl_;
    s.controls->Vibrate(left, right, running_ && Focused());
}

void GlSession::LocateHands(HandPose hands[2]) {
    Impl& s = *impl_;
    if (!running_) {
        hands[0] = hands[1] = HandPose{};
        return;
    }
    s.controls->Locate(s.baseSpace, s.frameState.predictedDisplayTime, hands);
}

} // namespace rr::xr
