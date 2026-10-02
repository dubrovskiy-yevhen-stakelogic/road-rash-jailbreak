// The VR frame's own GL pieces (vr_draw.h).
#include "vr_draw.h"

#include "render/multiview.h"
#include "render/shaders.h"

namespace rrgame {

using rr::render::gl;

namespace {

const char* const kPanelVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uViewProj; // (named so: single-pass stereo gives it a matrix per eye, render/multiview.h)
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";
const char* const kPanelFs = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); } // premultiplied: blended ONE, ONE_MINUS_SRC_ALPHA
)";

// A triangle over the whole viewport; the radius from the image centre (in NDC, x scaled to the eye's shape) darkens
// the edge.
const char* const kVignetteVs = R"(#version 330 core
// rr:multiview (drawn into both eyes' layers at once when the eyes are single-pass, render/multiview.h)
out vec2 vNdc;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vNdc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
const char* const kVignetteFs = R"(#version 330 core
in vec2 vNdc;
uniform float uStrength;
out vec4 oColor;
void main() {
    float r = length(vNdc);
    float inner = mix(1.2, 0.45, uStrength);
    float a = smoothstep(inner, inner + 0.35, r) * clamp(uStrength * 1.5, 0.0, 1.0);
    oColor = vec4(0.0, 0.0, 0.0, a);
}
)";

} // namespace

void VrPanel::Draw(const rr::render::Mat4& viewProj, const float corners[4][3], unsigned texture, const float uv[4]) {
    if (program_ == 0) {
        program_ = rr::render::BuildProgram(kPanelVs, kPanelFs);
        mvpLoc_ = gl.GetUniformLocation(program_, "uViewProj");
        texLoc_ = gl.GetUniformLocation(program_, "uTex");
        gl.GenVertexArrays(1, &vao_);
        gl.BindVertexArray(vao_);
        gl.GenBuffers(1, &vbo_);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl.BufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)), reinterpret_cast<void*>(0));
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)),
                               reinterpret_cast<void*>(3 * sizeof(float)));
        gl.EnableVertexAttribArray(1);
    }
    const float u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
    const float* tl = corners[0];
    const float* tr = corners[1];
    const float* bl = corners[2];
    const float* br = corners[3];
    const float verts[6][5] = {{tl[0], tl[1], tl[2], u0, v0}, {tr[0], tr[1], tr[2], u1, v0}, {bl[0], bl[1], bl[2], u0, v1},
                               {tr[0], tr[1], tr[2], u1, v0}, {br[0], br[1], br[2], u1, v1}, {bl[0], bl[1], bl[2], u0, v1}};
    gl.BindVertexArray(vao_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    gl.UseProgram(program_);
    rr::render::UploadViewProj(mvpLoc_, viewProj); // both eyes' while single-pass stereo is set
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    gl.Uniform1i(texLoc_, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void VrVignette::Draw(float strength) {
    if (strength <= 0.01f) return;
    if (program_ == 0) {
        program_ = rr::render::BuildProgram(kVignetteVs, kVignetteFs);
        strengthLoc_ = gl.GetUniformLocation(program_, "uStrength");
        gl.GenVertexArrays(1, &vao_);
    }
    gl.UseProgram(program_);
    gl.Uniform1f(strengthLoc_, strength);
    gl.BindVertexArray(vao_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

} // namespace rrgame
