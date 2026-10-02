#pragma once
// The GL entry points the renderer uses, loaded through wglGetProcAddress.
//
// This was `rrview`'s private block until `rrgame` needed the same renderer. Nothing here is
// game-specific: it is the smallest GL 3.3 surface the two draw paths share, so that there is one
// loader and one `Gl` table rather than a copy per tool.
//
// OpenGL ES 3.2 (the Quest): on Android (or with RR_GLES defined) the same table is
// filled from libGLESv3 and every direct gl* call binds to the ES library. Everything the renderer calls exists in
// ES 3.2 under the same name and enum (buffer textures, geometry shaders, R8 / R32F / RGBA32F); the few desktop-only
// pieces are named where they are used. The shaders are written for GLSL 330 core and CompileShader rewrites their
// header for GLSL 320 es at run time (shaders.cpp EsShaderSource), so there is one copy of each.
#if defined(__ANDROID__) && !defined(RR_GLES)
#define RR_GLES 1
#endif
#ifdef RR_GLES
#include <GLES3/gl32.h>
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <GL/gl.h>
#endif

#include <cstddef>

namespace rr::render {

using GLchar = char;
using GLsizeiptr = ptrdiff_t;

#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
// The ordering-table order (race_scene.h SetOtOrder): a buffer texture over a vertex buffer, and the clip planes.
#ifndef GL_TEXTURE_BUFFER
#define GL_TEXTURE_BUFFER 0x8C2A
#endif
#ifndef GL_R32F
#define GL_R32F 0x822E
#endif
#ifndef GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#endif
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif
#ifndef GL_CLIP_DISTANCE1
#define GL_CLIP_DISTANCE1 0x3001
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif

using PFNGLGENBUFFERS = void(APIENTRY*)(GLsizei, GLuint*);
using PFNGLBINDBUFFER = void(APIENTRY*)(GLenum, GLuint);
using PFNGLBUFFERDATA = void(APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFNGLGENVERTEXARRAYS = void(APIENTRY*)(GLsizei, GLuint*);
using PFNGLBINDVERTEXARRAY = void(APIENTRY*)(GLuint);
using PFNGLVERTEXATTRIBPOINTER = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFNGLENABLEVERTEXATTRIBARRAY = void(APIENTRY*)(GLuint);
using PFNGLCREATESHADER = GLuint(APIENTRY*)(GLenum);
using PFNGLSHADERSOURCE = void(APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFNGLCOMPILESHADER = void(APIENTRY*)(GLuint);
using PFNGLGETSHADERIV = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFNGLGETSHADERINFOLOG = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFNGLCREATEPROGRAM = GLuint(APIENTRY*)(void);
using PFNGLATTACHSHADER = void(APIENTRY*)(GLuint, GLuint);
using PFNGLLINKPROGRAM = void(APIENTRY*)(GLuint);
using PFNGLGETPROGRAMIV = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFNGLGETPROGRAMINFOLOG = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFNGLUSEPROGRAM = void(APIENTRY*)(GLuint);
using PFNGLGETUNIFORMLOCATION = GLint(APIENTRY*)(GLuint, const GLchar*);
using PFNGLUNIFORMMATRIX4FV = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFNGLUNIFORM3F = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
using PFNGLUNIFORM1I = void(APIENTRY*)(GLint, GLint);
using PFNGLUNIFORM1F = void(APIENTRY*)(GLint, GLfloat);
using PFNGLUNIFORM2F = void(APIENTRY*)(GLint, GLfloat, GLfloat);
using PFNGLUNIFORM4F = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFNGLACTIVETEXTURE = void(APIENTRY*)(GLenum);
using PFNGLBLENDEQUATION = void(APIENTRY*)(GLenum);
using PFNGLTEXBUFFER = void(APIENTRY*)(GLenum, GLenum, GLuint);
using PFNGLGETVERTEXATTRIBIV = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFNGLVERTEXATTRIB1F = void(APIENTRY*)(GLuint, GLfloat);
#ifndef RR_GLES
using PFNWGLCREATECONTEXTATTRIBSARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
#endif

struct Gl {
    PFNGLGENBUFFERS GenBuffers = nullptr;
    PFNGLBINDBUFFER BindBuffer = nullptr;
    PFNGLBUFFERDATA BufferData = nullptr;
    PFNGLGENVERTEXARRAYS GenVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAY BindVertexArray = nullptr;
    PFNGLVERTEXATTRIBPOINTER VertexAttribPointer = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAY EnableVertexAttribArray = nullptr;
    PFNGLCREATESHADER CreateShader = nullptr;
    PFNGLSHADERSOURCE ShaderSource = nullptr;
    PFNGLCOMPILESHADER CompileShader = nullptr;
    PFNGLGETSHADERIV GetShaderiv = nullptr;
    PFNGLGETSHADERINFOLOG GetShaderInfoLog = nullptr;
    PFNGLCREATEPROGRAM CreateProgram = nullptr;
    PFNGLATTACHSHADER AttachShader = nullptr;
    PFNGLLINKPROGRAM LinkProgram = nullptr;
    PFNGLGETPROGRAMIV GetProgramiv = nullptr;
    PFNGLGETPROGRAMINFOLOG GetProgramInfoLog = nullptr;
    PFNGLUSEPROGRAM UseProgram = nullptr;
    PFNGLGETUNIFORMLOCATION GetUniformLocation = nullptr;
    PFNGLUNIFORMMATRIX4FV UniformMatrix4fv = nullptr;
    PFNGLUNIFORM3F Uniform3f = nullptr;
    PFNGLUNIFORM1I Uniform1i = nullptr;
    PFNGLUNIFORM1F Uniform1f = nullptr;
    PFNGLUNIFORM2F Uniform2f = nullptr;
    PFNGLUNIFORM4F Uniform4f = nullptr;
    PFNGLACTIVETEXTURE ActiveTexture = nullptr;
    PFNGLBLENDEQUATION BlendEquation = nullptr;
    PFNGLTEXBUFFER TexBuffer = nullptr;
    PFNGLGETVERTEXATTRIBIV GetVertexAttribiv = nullptr;
    PFNGLVERTEXATTRIB1F VertexAttrib1f = nullptr;
};

// The single table both tools bind through. Filled by `LoadGl`, which a GL context must be current
// for; every entry point is required, and a missing one throws rather than leaving a null pointer
// to be called later.
extern Gl gl;

void LoadGl();

// What the context is, filled by LoadGl. On desktop GL `es` is false and nothing below is consulted.
// On ES the two GLSL features the 330 core shaders use that ES 3.2 does not have in core come from extensions when
// the driver offers them (EsShaderSource): gl_ClipDistance (GL_EXT_clip_cull_distance) and the `noperspective`
// qualifier (GL_NV_shader_noperspective_interpolation); without one the shader still compiles, the clip planes are
// then not applied and the attribute is interpolated perspective-correct - LoadGl prints which.
struct GlCaps {
    bool es = false;
    bool clipDistance = true;
    bool noPerspective = true;
    bool srgbWriteControl = false; // GL_EXT_sRGB_write_control: sRGB encoding of an sRGB target can be switched off
};
extern GlCaps glCaps;

#ifdef RR_GLES
// An entry point of libGLESv3 (or eglGetProcAddress); null when absent.
void* GlesProc(const char* name);
#endif

#ifndef GL_CLIP_DISTANCE0_EXT
#define GL_CLIP_DISTANCE0_EXT 0x3000
#endif
#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

} // namespace rr::render

#ifdef RR_GLES
// Several renderer files load a GL entry point the shared table does not carry with their own few lines of
// wglGetProcAddress / GetModuleHandleA("opengl32.dll") / GetProcAddress (lod_pose_draw.cpp, rider_pose_draw.cpp,
// gte_proj.cpp: glBufferSubData, glVertexAttrib4f). On ES those three names resolve to the ES library through these
// shims, so the files compile unchanged on both; new code should call rr::render::GlesProc / the Gl table instead.
using HMODULE = void*;
using HWND = void*; // render_target.h's SetBorderlessFullscreen(HWND, bool): a no-op on ES (render_target_es.cpp)
inline void* wglGetProcAddress(const char* name) { return rr::render::GlesProc(name); }
inline HMODULE GetModuleHandleA(const char*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }
#endif
