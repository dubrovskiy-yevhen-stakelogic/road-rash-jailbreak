#pragma once
// Single-pass stereo: both eyes of a VR frame drawn by ONE pass over the scene
// through GL_OVR_multiview2 - the draw calls, the uniform uploads, the cull and the vertex uploads happen once, the
// vertex stage runs per view (gl_ViewID_OVR picks the eye's view-projection) and the rasteriser writes the two layers
// of a 2D array target. OpenGL ES 3.2 (the Quest: GL_OVR_multiview2, GL_OVR_multiview_multisampled_render_to_texture)
// and desktop GL 3.3 where the driver offers the extension (the desktop VR mock).
//
// How the renderer's programs take part, without a second copy of any shader:
//   * SetMultiviewPrograms(true) (the VR host, once the display is known to draw single-pass) makes CompileShader
//     rewrite every VERTEX shader compiled from then on that declares `uniform mat4 uViewProj;` (the world's
//     programs: the scene, the panorama, the clouds, the shadow, the effect billboards, the world sky, the HUD panel)
//     or carries the marker `// rr:multiview` (the vignette): `#extension GL_OVR_multiview2 : require`,
//     `layout(num_views = 2) in;`, `uniform mat4 uViewProj[2];` and every use `uViewProj[gl_ViewID_OVR]`. Such a
//     program only draws into a two-view framebuffer; the process that sets the switch draws its world only there.
//     Programs compiled while it is off (every desktop run, the HUD rasteriser, the menus, the theatre quad's
//     pictures) are unchanged, byte for byte.
//   * SetStereoViewProj(two) around a view's draw: UploadViewProj (every `uViewProj` upload of the renderer) then
//     loads both matrices; with it unset the upload is the single matrix exactly as before.
#include "render/gl_api.h"
#include "render/mat4.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rr::render {

// ---- the programs
void SetMultiviewPrograms(bool on);
bool MultiviewPrograms();
// Whether CompileShader rewrites this vertex source (see the top), and the rewrite itself (applied after the ES header
// rewrite; `source` starts with its #version line).
bool WantsMultiview(const char* source);
std::string MultiviewVertexSource(const std::string& source);

// ---- the per-view matrices
// Both eyes' view-projections for the draws that follow (null: one view). The pointer must outlive those draws.
void SetStereoViewProj(const Mat4* two);
const Mat4* StereoViewProj();
// glUniformMatrix4fv of `uViewProj` at `location`: `mono` (times `right` when given) for one view, or both eyes'
// matrices (each times `right`) while SetStereoViewProj is set.
void UploadViewProj(GLint location, const Mat4& mono, const Mat4* right = nullptr);
// Camera-relative transforms: with an origin set (a point at the eye, world units), a program whose
// vertex shader declares `uniform vec3 uOrigin; uniform int uOriginOn;` (the scene's: shaders.cpp) gets from
// UploadViewProj (no `right`) the matrix times a translation to the origin and uOriginOn = 1, and transforms
// (world - uOrigin): every term small, where VP world sums terms of thousands of world units - the float rounding of that
// sum is ~1e-3 in clip z and w, which near 0.05 turns into ~1 cm of depth per metre of distance, different for every
// vertex and every frame (the VR shimmer of coplanar surfaces). Null: off (the ordering-table order, every program as
// before, byte for byte).
void SetRenderOrigin(const float* origin);

// ---- the GL side
// Whether this context can draw single-pass stereo (GL_OVR_multiview2), and with multisampling into a texture array
// (ES: GL_OVR_multiview_multisampled_render_to_texture; desktop: a 2D multisample array, resolved by blits).
struct MultiviewCaps {
    bool multiview = false;
    bool multisampled = false;
};
MultiviewCaps DetectMultiview();

// The entry points (loaded by DetectMultiview; null when absent).
using PfnFramebufferTextureMultiview = void(APIENTRY*)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
using PfnFramebufferTextureMultisampleMultiview = void(APIENTRY*)(GLenum, GLenum, GLuint, GLint, GLsizei, GLint, GLsizei);
struct MultiviewGl {
    PfnFramebufferTextureMultiview FramebufferTextureMultiviewOVR = nullptr;
    PfnFramebufferTextureMultisampleMultiview FramebufferTextureMultisampleMultiviewOVR = nullptr; // ES only
    void(APIENTRY* FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint) = nullptr;
    void(APIENTRY* TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void(APIENTRY* TexImage3DMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLboolean) = nullptr; // desktop
    void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
    GLenum(APIENTRY* CheckFramebufferStatus)(GLenum) = nullptr;
    void(APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
    void(APIENTRY* InvalidateFramebuffer)(GLenum, GLsizei, const GLenum*) = nullptr; // ES 3.0; desktop 4.3 (null there)
};
const MultiviewGl& Mvgl();

// A two-layer target of our own (the desktop / Quest VR mock): RGBA8 colour and depth-stencil 2D array textures, one
// layer per eye, drawn single-pass. Multisampled: ES renders into the tile and resolves on the way out
// (multisampled render-to-texture); the desktop draws into 2D multisample arrays and Resolve() blits each layer.
class MultiviewTarget {
public:
    // False (and the reason printed) when the framebuffer cannot be made complete.
    bool Ensure(int w, int h, int samples);
    void Bind() const;    // GL_FRAMEBUFFER, the viewport the whole layer
    void Resolve() const; // the desktop's multisample layers into the single-sample array (no-op otherwise)
    // Layer `layer` as RGBA8, bottom row first (after Resolve).
    void ReadLayer(int layer, std::vector<uint8_t>& rgba) const;
    // The depth and stencil are not stored (ES: InvalidateFramebuffer).
    void DiscardDepth() const;
    int Width() const { return width_; }
    int Height() const { return height_; }
    int Samples() const { return samples_; }
    unsigned Framebuffer() const { return fbo_; }
    unsigned ColourTexture() const { return colour_; }
    void Release();

private:
    int width_ = 0, height_ = 0, samples_ = 0;
    unsigned fbo_ = 0, colour_ = 0, depth_ = 0;
    unsigned msColour_ = 0, msDepth_ = 0; // desktop multisampling: the arrays drawn into
    unsigned readFbo_ = 0, drawFbo_ = 0;  // Resolve / ReadLayer
};

} // namespace rr::render
