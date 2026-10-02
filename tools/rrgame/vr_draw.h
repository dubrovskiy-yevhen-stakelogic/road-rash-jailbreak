#pragma once
// The GL pieces the VR frame adds to the race's eyes, portable (desktop GL 3.3 and
// OpenGL ES 3.2):
//   * VrPanel: a textured rectangle hung in the world - the original's HUD (the PORTED HudFrame's packets, rasterised
//     into the 384 x 240 overlay as on the desktop) on a panel fixed to the rider's levelled frame, ahead and below
//     the eye, drawn into each eye over the scene (premultiplied alpha, no depth test: it is always readable);
//   * VrVignette: the comfort vignette - the edge of each eye image darkened towards black by `strength` (0..1).
#include "render/gl_api.h"
#include "render/mat4.h"

namespace rrgame {

class VrPanel {
public:
    // `corners`: world positions of the top-left, top-right, bottom-left, bottom-right corners; `uv` = u0, v0, u1, v1
    // of `texture` (premultiplied RGBA). Draws into the bound framebuffer / viewport with `viewProj`.
    void Draw(const rr::render::Mat4& viewProj, const float corners[4][3], unsigned texture, const float uv[4]);

private:
    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    GLint mvpLoc_ = -1, texLoc_ = -1;
};

class VrVignette {
public:
    void Draw(float strength);

private:
    GLuint program_ = 0, vao_ = 0;
    GLint strengthLoc_ = -1;
};

} // namespace rrgame
