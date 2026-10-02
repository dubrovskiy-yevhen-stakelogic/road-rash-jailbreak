#include "render/gl_api.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef RR_GLES
#include <EGL/egl.h>
#include <dlfcn.h>
#endif

namespace rr::render {

Gl gl;
GlCaps glCaps;

#ifdef RR_GLES
// ES: the core entry points are exported by libGLESv3 itself; eglGetProcAddress is the fallback (and the only source
// of extension functions).
void* GlesProc(const char* name) {
    static void* lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    void* p = lib ? dlsym(lib, name) : nullptr;
    if (!p) p = reinterpret_cast<void*>(eglGetProcAddress(name));
    return p;
}
#endif

namespace {

#ifdef RR_GLES
void* GetGlProc(const char* name) {
    void* p = GlesProc(name);
    if (!p) throw std::runtime_error(std::string("OpenGL ES entry point missing: ") + name);
    return p;
}

void DetectEsCaps() {
    glCaps.es = true;
    glCaps.clipDistance = glCaps.noPerspective = glCaps.srgbWriteControl = false;
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (!e) continue;
        if (std::strcmp(e, "GL_EXT_clip_cull_distance") == 0) glCaps.clipDistance = true;
        if (std::strcmp(e, "GL_NV_shader_noperspective_interpolation") == 0) glCaps.noPerspective = true;
        if (std::strcmp(e, "GL_EXT_sRGB_write_control") == 0) glCaps.srgbWriteControl = true;
    }
    std::printf("gl: OpenGL ES %s (%s, %s); GL_EXT_clip_cull_distance %s, GL_NV_shader_noperspective_interpolation %s, "
                "GL_EXT_sRGB_write_control %s\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)), reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)), glCaps.clipDistance ? "yes" : "NO (clip planes off)",
                glCaps.noPerspective ? "yes" : "NO (affine attributes interpolated perspective-correct)",
                glCaps.srgbWriteControl ? "yes" : "no");
}
#else
void* GetGlProc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (p == nullptr || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
        p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
        HMODULE module = GetModuleHandleA("opengl32.dll");
        p = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    }
    if (!p) throw std::runtime_error(std::string("OpenGL entry point missing: ") + name);
    return p;
}
#endif

} // namespace

void LoadGl() {
    gl.GenBuffers = reinterpret_cast<PFNGLGENBUFFERS>(GetGlProc("glGenBuffers"));
    gl.BindBuffer = reinterpret_cast<PFNGLBINDBUFFER>(GetGlProc("glBindBuffer"));
    gl.BufferData = reinterpret_cast<PFNGLBUFFERDATA>(GetGlProc("glBufferData"));
    gl.GenVertexArrays = reinterpret_cast<PFNGLGENVERTEXARRAYS>(GetGlProc("glGenVertexArrays"));
    gl.BindVertexArray = reinterpret_cast<PFNGLBINDVERTEXARRAY>(GetGlProc("glBindVertexArray"));
    gl.VertexAttribPointer = reinterpret_cast<PFNGLVERTEXATTRIBPOINTER>(GetGlProc("glVertexAttribPointer"));
    gl.EnableVertexAttribArray =
        reinterpret_cast<PFNGLENABLEVERTEXATTRIBARRAY>(GetGlProc("glEnableVertexAttribArray"));
    gl.CreateShader = reinterpret_cast<PFNGLCREATESHADER>(GetGlProc("glCreateShader"));
    gl.ShaderSource = reinterpret_cast<PFNGLSHADERSOURCE>(GetGlProc("glShaderSource"));
    gl.CompileShader = reinterpret_cast<PFNGLCOMPILESHADER>(GetGlProc("glCompileShader"));
    gl.GetShaderiv = reinterpret_cast<PFNGLGETSHADERIV>(GetGlProc("glGetShaderiv"));
    gl.GetShaderInfoLog = reinterpret_cast<PFNGLGETSHADERINFOLOG>(GetGlProc("glGetShaderInfoLog"));
    gl.CreateProgram = reinterpret_cast<PFNGLCREATEPROGRAM>(GetGlProc("glCreateProgram"));
    gl.AttachShader = reinterpret_cast<PFNGLATTACHSHADER>(GetGlProc("glAttachShader"));
    gl.LinkProgram = reinterpret_cast<PFNGLLINKPROGRAM>(GetGlProc("glLinkProgram"));
    gl.GetProgramiv = reinterpret_cast<PFNGLGETPROGRAMIV>(GetGlProc("glGetProgramiv"));
    gl.GetProgramInfoLog = reinterpret_cast<PFNGLGETPROGRAMINFOLOG>(GetGlProc("glGetProgramInfoLog"));
    gl.UseProgram = reinterpret_cast<PFNGLUSEPROGRAM>(GetGlProc("glUseProgram"));
    gl.GetUniformLocation = reinterpret_cast<PFNGLGETUNIFORMLOCATION>(GetGlProc("glGetUniformLocation"));
    gl.UniformMatrix4fv = reinterpret_cast<PFNGLUNIFORMMATRIX4FV>(GetGlProc("glUniformMatrix4fv"));
    gl.Uniform3f = reinterpret_cast<PFNGLUNIFORM3F>(GetGlProc("glUniform3f"));
    gl.Uniform1i = reinterpret_cast<PFNGLUNIFORM1I>(GetGlProc("glUniform1i"));
    gl.Uniform1f = reinterpret_cast<PFNGLUNIFORM1F>(GetGlProc("glUniform1f"));
    gl.Uniform2f = reinterpret_cast<PFNGLUNIFORM2F>(GetGlProc("glUniform2f"));
    gl.Uniform4f = reinterpret_cast<PFNGLUNIFORM4F>(GetGlProc("glUniform4f"));
    gl.ActiveTexture = reinterpret_cast<PFNGLACTIVETEXTURE>(GetGlProc("glActiveTexture"));
    gl.BlendEquation = reinterpret_cast<PFNGLBLENDEQUATION>(GetGlProc("glBlendEquation"));
    gl.TexBuffer = reinterpret_cast<PFNGLTEXBUFFER>(GetGlProc("glTexBuffer"));
    gl.GetVertexAttribiv = reinterpret_cast<PFNGLGETVERTEXATTRIBIV>(GetGlProc("glGetVertexAttribiv"));
    gl.VertexAttrib1f = reinterpret_cast<PFNGLVERTEXATTRIB1F>(GetGlProc("glVertexAttrib1f"));
#ifdef RR_GLES
    DetectEsCaps();
#endif
}

} // namespace rr::render
