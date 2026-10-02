// Single-pass stereo (multiview.h).
#include "render/multiview.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef RR_GLES
#include <GLES3/gl32.h>
#endif

#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE_ARRAY
#define GL_TEXTURE_2D_MULTISAMPLE_ARRAY 0x9102
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
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
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif

namespace rr::render {

namespace {

bool g_multiviewPrograms = false;
const Mat4* g_stereo = nullptr;
bool g_originOn = false; // SetRenderOrigin
double g_origin[3] = {0.0, 0.0, 0.0};
MultiviewGl g_mv;
bool g_mvLoaded = false;

void* Proc(const char* name) {
#ifdef RR_GLES
    return GlesProc(name);
#else
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (p == nullptr || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
        p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
        HMODULE module = GetModuleHandleA("opengl32.dll");
        p = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    }
    return p;
#endif
}

template <typename F>
void Load(F& f, const char* name) {
    f = reinterpret_cast<F>(Proc(name));
}

bool HasExtension(const char* name) {
    using GetStringiFn = const GLubyte*(APIENTRY*)(GLenum, GLuint);
    static GetStringiFn getStringi = reinterpret_cast<GetStringiFn>(Proc("glGetStringi"));
    if (getStringi == nullptr) return false;
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* e = reinterpret_cast<const char*>(getStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (e != nullptr && std::strcmp(e, name) == 0) return true;
    }
    return false;
}

void LoadMv() {
    if (g_mvLoaded) return;
    g_mvLoaded = true;
    Load(g_mv.FramebufferTextureMultiviewOVR, "glFramebufferTextureMultiviewOVR");
    Load(g_mv.FramebufferTextureMultisampleMultiviewOVR, "glFramebufferTextureMultisampleMultiviewOVR");
    Load(g_mv.FramebufferTextureLayer, "glFramebufferTextureLayer");
    Load(g_mv.TexImage3D, "glTexImage3D");
    Load(g_mv.TexImage3DMultisample, "glTexImage3DMultisample");
    Load(g_mv.GenFramebuffers, "glGenFramebuffers");
    Load(g_mv.DeleteFramebuffers, "glDeleteFramebuffers");
    Load(g_mv.BindFramebuffer, "glBindFramebuffer");
    Load(g_mv.CheckFramebufferStatus, "glCheckFramebufferStatus");
    Load(g_mv.BlitFramebuffer, "glBlitFramebuffer");
#ifdef RR_GLES
    Load(g_mv.InvalidateFramebuffer, "glInvalidateFramebuffer");
#endif
}

bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

} // namespace

void SetMultiviewPrograms(bool on) { g_multiviewPrograms = on; }
bool MultiviewPrograms() { return g_multiviewPrograms; }

bool WantsMultiview(const char* source) {
    return std::strstr(source, "uniform mat4 uViewProj;") != nullptr || std::strstr(source, "// rr:multiview") != nullptr;
}

std::string MultiviewVertexSource(const std::string& source) {
    std::string s = source;
    const std::string decl = "uniform mat4 uViewProj;", mark = "@RR_VIEWPROJ_DECL@";
    const size_t d = s.find(decl);
    if (d != std::string::npos) s.replace(d, decl.size(), mark);
    const std::string name = "uViewProj", index = "[gl_ViewID_OVR]";
    for (size_t p = s.find(name); p != std::string::npos; p = s.find(name, p)) {
        const size_t e = p + name.size();
        if ((p > 0 && IsIdentChar(s[p - 1])) || (e < s.size() && IsIdentChar(s[e]))) {
            p = e;
            continue;
        }
        s.insert(e, index);
        p = e + index.size();
    }
    if (d != std::string::npos) s.replace(s.find(mark), mark.size(), "uniform mat4 uViewProj[2];");
    // the extension right after the #version line (every #extension precedes the other tokens), the layout after the
    // leading run of preprocessor lines (the ES header's own #extension lines included)
    const size_t firstLine = s.find('\n');
    s.insert(firstLine == std::string::npos ? s.size() : firstLine + 1, "#extension GL_OVR_multiview2 : require\n");
    size_t pos = 0;
    while (pos < s.size() && s[pos] == '#') {
        const size_t nl = s.find('\n', pos);
        pos = nl == std::string::npos ? s.size() : nl + 1;
    }
    s.insert(pos, "layout(num_views = 2) in;\n");
    return s;
}

void SetStereoViewProj(const Mat4* two) { g_stereo = two; }
const Mat4* StereoViewProj() { return g_stereo; }

void SetRenderOrigin(const float* origin) {
    g_originOn = origin != nullptr;
    for (int k = 0; k < 3; ++k) g_origin[k] = origin != nullptr ? static_cast<double>(origin[k]) : 0.0;
}

void UploadViewProj(GLint location, const Mat4& mono, const Mat4* right) {
    // Camera-relative (SetRenderOrigin): a program that declares uOrigin / uOriginOn takes the matrix times a
    // translation to the origin - its translation column VP (O, 1) summed in double, a few units at most - and computes
    // VP' (world - O) itself; any other program, or no origin, takes the matrix as it is.
    bool rebase = false;
    if (right == nullptr) {
        GLint program = 0;
        glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &program);
        const GLint on = program != 0 ? gl.GetUniformLocation(static_cast<GLuint>(program), "uOriginOn") : -1;
        const GLint at = program != 0 ? gl.GetUniformLocation(static_cast<GLuint>(program), "uOrigin") : -1;
        rebase = g_originOn && on >= 0 && at >= 0;
        if (on >= 0) gl.Uniform1i(on, rebase ? 1 : 0);
        if (rebase) gl.Uniform3f(at, static_cast<float>(g_origin[0]), static_cast<float>(g_origin[1]), static_cast<float>(g_origin[2]));
    }
    const auto rebased = [rebase](const Mat4& m) {
        if (!rebase) return m;
        Mat4 r = m;
        for (int i = 0; i < 4; ++i)
            r.m[12 + i] = static_cast<float>(static_cast<double>(m.m[i]) * g_origin[0] + static_cast<double>(m.m[4 + i]) * g_origin[1] +
                                             static_cast<double>(m.m[8 + i]) * g_origin[2] + static_cast<double>(m.m[12 + i]));
        return r;
    };
    if (g_stereo != nullptr) {
        float both[32];
        for (int e = 0; e < 2; ++e) {
            const Mat4 m = rebased(right != nullptr ? Multiply(g_stereo[e], *right) : g_stereo[e]);
            std::memcpy(both + 16 * e, m.m, sizeof(m.m));
        }
        gl.UniformMatrix4fv(location, 2, GL_FALSE, both);
        return;
    }
    if (right != nullptr) {
        const Mat4 m = Multiply(mono, *right);
        gl.UniformMatrix4fv(location, 1, GL_FALSE, m.m);
        return;
    }
    if (rebase) {
        const Mat4 m = rebased(mono);
        gl.UniformMatrix4fv(location, 1, GL_FALSE, m.m);
        return;
    }
    gl.UniformMatrix4fv(location, 1, GL_FALSE, mono.m);
}

MultiviewCaps DetectMultiview() {
    LoadMv();
    MultiviewCaps c;
    c.multiview = HasExtension("GL_OVR_multiview2") && g_mv.FramebufferTextureMultiviewOVR != nullptr &&
                  g_mv.TexImage3D != nullptr && g_mv.GenFramebuffers != nullptr && g_mv.BindFramebuffer != nullptr &&
                  g_mv.FramebufferTextureLayer != nullptr;
#ifdef RR_GLES
    c.multisampled = c.multiview && HasExtension("GL_OVR_multiview_multisampled_render_to_texture") &&
                     g_mv.FramebufferTextureMultisampleMultiviewOVR != nullptr;
#else
    c.multisampled = c.multiview && g_mv.TexImage3DMultisample != nullptr && g_mv.BlitFramebuffer != nullptr;
#endif
    return c;
}

const MultiviewGl& Mvgl() {
    LoadMv();
    return g_mv;
}

// ---------------------------------------------------------------- MultiviewTarget
bool MultiviewTarget::Ensure(int w, int h, int samples) {
    samples = samples < 1 ? 1 : samples;
    if (fbo_ != 0 && w == width_ && h == height_ && samples == samples_) return true;
    Release();
    const MultiviewCaps caps = DetectMultiview();
    if (!caps.multiview) return false;
    const MultiviewGl& f = g_mv;
    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    samples = caps.multisampled ? (samples > maxSamples ? maxSamples : samples) : 1;
    width_ = w;
    height_ = h;
    const auto makeArray = [&](GLuint& tex, GLint internal, GLenum format, GLenum type) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        f.TexImage3D(GL_TEXTURE_2D_ARRAY, 0, internal, w, h, 2, 0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    };
    makeArray(colour_, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    makeArray(depth_, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8);
    f.GenFramebuffers(1, &fbo_);
    f.GenFramebuffers(1, &readFbo_);
    f.GenFramebuffers(1, &drawFbo_);
    for (int attempt = 0; attempt < 2; ++attempt) {
        samples_ = attempt == 0 ? samples : 1;
        f.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
#ifdef RR_GLES
        if (samples_ > 1) {
            f.FramebufferTextureMultisampleMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth_, 0, samples_, 0, 2);
            f.FramebufferTextureMultisampleMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colour_, 0, samples_, 0, 2);
        } else {
            f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth_, 0, 0, 2);
            f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colour_, 0, 0, 2);
        }
#else
        if (samples_ > 1) { // desktop: 2D multisample arrays drawn into, each layer resolved by a blit
            glGenTextures(1, &msColour_);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, msColour_);
            f.TexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, samples_, GL_RGBA8, w, h, 2, GL_TRUE);
            glGenTextures(1, &msDepth_);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, msDepth_);
            f.TexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, samples_, GL_DEPTH24_STENCIL8, w, h, 2, GL_TRUE);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 0);
            f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, msDepth_, 0, 0, 2);
            f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, msColour_, 0, 0, 2);
        } else {
            f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth_, 0, 0, 2);
            f.FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colour_, 0, 0, 2);
        }
#endif
        const GLenum status = f.CheckFramebufferStatus(GL_FRAMEBUFFER);
        while (glGetError() != GL_NO_ERROR) {
        }
        if (status == GL_FRAMEBUFFER_COMPLETE) {
            f.BindFramebuffer(GL_FRAMEBUFFER, 0);
            return true;
        }
        std::printf("multiview: the %dx %dx%d two-layer target is incomplete (status 0x%X)%s\n", samples_, w, h,
                    unsigned(status), samples_ > 1 ? " - trying single-sampled" : "");
        if (msColour_) glDeleteTextures(1, &msColour_);
        if (msDepth_) glDeleteTextures(1, &msDepth_);
        msColour_ = msDepth_ = 0;
        if (samples_ == 1) break;
    }
    f.BindFramebuffer(GL_FRAMEBUFFER, 0);
    Release();
    return false;
}

void MultiviewTarget::Bind() const {
    g_mv.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void MultiviewTarget::Resolve() const {
    if (msColour_ == 0) return;
    const MultiviewGl& f = g_mv;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glDisable(GL_SCISSOR_TEST);
    for (int layer = 0; layer < 2; ++layer) {
        f.BindFramebuffer(GL_READ_FRAMEBUFFER, readFbo_);
        f.FramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, msColour_, 0, layer);
        f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo_);
        f.FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colour_, 0, layer);
        f.BlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    f.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
}

void MultiviewTarget::ReadLayer(int layer, std::vector<uint8_t>& rgba) const {
    const MultiviewGl& f = g_mv;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    rgba.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u, 0);
    f.BindFramebuffer(GL_FRAMEBUFFER, readFbo_);
    f.FramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colour_, 0, layer & 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    f.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
}

void MultiviewTarget::DiscardDepth() const {
    if (g_mv.InvalidateFramebuffer == nullptr || fbo_ == 0) return;
    const GLenum depth[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    g_mv.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    g_mv.InvalidateFramebuffer(GL_FRAMEBUFFER, 2, depth);
    g_mv.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
}

void MultiviewTarget::Release() {
    if (fbo_) g_mv.DeleteFramebuffers(1, &fbo_);
    if (readFbo_) g_mv.DeleteFramebuffers(1, &readFbo_);
    if (drawFbo_) g_mv.DeleteFramebuffers(1, &drawFbo_);
    for (unsigned* t : {&colour_, &depth_, &msColour_, &msDepth_})
        if (*t) glDeleteTextures(1, t);
    fbo_ = readFbo_ = drawFbo_ = colour_ = depth_ = msColour_ = msDepth_ = 0;
    width_ = height_ = samples_ = 0;
}

} // namespace rr::render
