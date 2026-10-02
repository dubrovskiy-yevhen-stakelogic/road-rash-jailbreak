// The GT2 VR hands (vr_hands_draw.h). The mesh record, the left hand's mirror, the morph blend, the 32-step posing and
// the grip / aim basis are the gt2-play project's (MIT; src\gt2view\vr_driving_visuals.cpp, a port of MiamiVR's
// VRHandModel), drawn here with GL instead of GT2's Vulkan scene renderer.
#include "vr_hands_draw.h"

#include "platform/png_read.h"
#include "render/multiview.h"
#include "render/render_target.h"
#include "render/shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace rrgame {

using rr::render::gl;

namespace {

// The hands: model space in metres (the mesh), the posed normal, the albedo; a diffuse light from above plus the
// ambient GT2 gives its hands (0.72 + 0.28 n.l), so they read in the dark of the road.
const char* const kHandVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform vec3 uLight;
out vec2 vUv;
out float vShade;
void main() {
    vec3 n = normalize(mat3(uModel) * aNormal);
    vShade = 0.72 + 0.28 * max(dot(n, uLight), 0.0);
    vUv = aUv;
    gl_Position = uViewProj * (uModel * vec4(aPos, 1.0));
}
)";
const char* const kHandFs = R"(#version 330 core
in vec2 vUv;
in float vShade;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = vec4(texture(uTex, vUv).rgb * vShade, 1.0); }
)";

// The grip markers: flat-coloured boxes.
const char* const kMarkerVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
uniform mat4 uViewProj;
uniform vec3 uLight;
out vec3 vColor;
void main() {
    float lit = 0.45 + 0.55 * max(dot(normalize(aNormal), uLight), 0.0);
    vColor = aColor * lit;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";
const char* const kMarkerFs = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

constexpr uint32_t kMagic = 0x48525855; // "UXRH"
constexpr int kSteps = 32;              // the morph's quantisation (GT2: re-posed only when a step changes)
constexpr float kPalmBack = 0.055f;     // metres: the mesh origin behind the grip pose along the fingers (GT2)
// The world's up for the light: the PlayStation's axes are Y down.
constexpr float kLight[3] = {0.20f, -0.90f, -0.35f};

struct V {
    float x = 0, y = 0, z = 0;
};
V Make(const float p[3]) { return {p[0], p[1], p[2]}; }
V Add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V Sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V Mul(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V Cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V Normal(V a) {
    const float n = std::sqrt(Dot(a, a));
    return n > 1e-6f ? Mul(a, 1.0f / n) : V{};
}
// The four baked poses blended by grip and trigger (vr_driving_visuals.cpp Blend).
V Blend(const float poses[4][3], float grip, float trigger) {
    const float w[4] = {(1 - grip) * (1 - trigger), grip * (1 - trigger), (1 - grip) * trigger, grip * trigger};
    V p;
    for (int i = 0; i < 4; ++i) p = Add(p, Mul(V{poses[i][0], poses[i][1], poses[i][2]}, w[i]));
    return p;
}

void Scale(const float v[3], float s, float out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = v[k] * s;
}

} // namespace

bool VrHandsDraw::Load() {
    if (loaded_ || failed_) return loaded_;
    failed_ = true;
    static_assert(sizeof(Vertex) == 104, "the UXRH vertex record");
    try {
        for (int h = 0; h < 2; ++h) {
            const std::span<const uint8_t> bytes = VrHandAsset(h);
            uint32_t header[4] = {};
            if (bytes.size() < sizeof(header)) throw std::runtime_error("truncated hand mesh");
            std::memcpy(header, bytes.data(), sizeof(header));
            const uint64_t expected = 16 + uint64_t(header[2]) * sizeof(Vertex) + uint64_t(header[3]) * 2;
            if (header[0] != kMagic || header[1] != 1 || header[2] == 0 || header[2] > 65535 || header[3] == 0 ||
                header[3] > 1000000 || header[3] % 3 != 0 || expected != bytes.size())
                throw std::runtime_error("invalid hand mesh");
            Mesh& m = meshes_[h];
            m.vertices.resize(header[2]);
            m.indices.resize(header[3]);
            std::memcpy(m.vertices.data(), bytes.data() + 16, m.vertices.size() * sizeof(Vertex));
            std::memcpy(m.indices.data(), bytes.data() + 16 + m.vertices.size() * sizeof(Vertex), m.indices.size() * 2);
            for (uint16_t index : m.indices)
                if (index >= m.vertices.size()) throw std::runtime_error("invalid hand index");
            // Both baked meshes have the same anatomical orientation: the left one is reflected across its palm
            // normal (y), normals and winding included (GT2).
            if (h == 0) {
                for (Vertex& v : m.vertices)
                    for (int pose = 0; pose < 4; ++pose) {
                        v.position[pose][1] = -v.position[pose][1];
                        v.normal[pose][1] = -v.normal[pose][1];
                    }
                for (size_t i = 0; i + 2 < m.indices.size(); i += 3) std::swap(m.indices[i + 1], m.indices[i + 2]);
            }
        }
        const rr::PngImage image = rr::DecodePng(VrHandAsset(2));
        glGenTextures(1, &albedo_);
        glBindTexture(GL_TEXTURE_2D, albedo_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        rr::render::GenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703); // GL_LINEAR_MIPMAP_LINEAR
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        handProgram_ = rr::render::BuildProgram(kHandVs, kHandFs);
        handViewProjLoc_ = gl.GetUniformLocation(handProgram_, "uViewProj");
        handModelLoc_ = gl.GetUniformLocation(handProgram_, "uModel");
        handLightLoc_ = gl.GetUniformLocation(handProgram_, "uLight");
        handTexLoc_ = gl.GetUniformLocation(handProgram_, "uTex");
        for (int h = 0; h < 2; ++h) {
            gl.GenVertexArrays(1, &handVao_[h]);
            gl.BindVertexArray(handVao_[h]);
            gl.GenBuffers(1, &handVbo_[h]);
            gl.BindBuffer(GL_ARRAY_BUFFER, handVbo_[h]);
            const GLsizei stride = 8 * static_cast<GLsizei>(sizeof(float));
            gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
            gl.EnableVertexAttribArray(0);
            gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
            gl.EnableVertexAttribArray(1);
            gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
            gl.EnableVertexAttribArray(2);
        }
        gl.BindVertexArray(0);
        std::printf("vr hands: the GT2 / UltimateXR hands (third_party\\vrhands): %zu + %zu vertices, %zu + %zu indices, "
                    "albedo %dx%d\n",
                    meshes_[0].vertices.size(), meshes_[1].vertices.size(), meshes_[0].indices.size(),
                    meshes_[1].indices.size(), image.width, image.height);
    } catch (const std::exception& e) {
        std::printf("vr hands: not drawn - %s\n", e.what());
        return false;
    }
    failed_ = false;
    loaded_ = true;
    return true;
}

// Hand h's mesh at a grip / trigger step, expanded to triangles (position, normal, texel) in its VBO.
void VrHandsDraw::Pose(int h, int gripStep, int triggerStep) {
    if (gripStep_[h] == gripStep && triggerStep_[h] == triggerStep) return;
    gripStep_[h] = gripStep;
    triggerStep_[h] = triggerStep;
    const float grip = static_cast<float>(gripStep) / kSteps, trigger = static_cast<float>(triggerStep) / kSteps;
    const Mesh& m = meshes_[h];
    std::vector<float> posed(m.vertices.size() * 8);
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        const Vertex& s = m.vertices[i];
        const V p = Blend(s.position, grip, trigger), n = Normal(Blend(s.normal, grip, trigger));
        float* o = &posed[i * 8];
        o[0] = p.x, o[1] = p.y, o[2] = p.z, o[3] = n.x, o[4] = n.y, o[5] = n.z, o[6] = s.u, o[7] = s.v;
    }
    std::vector<float> tris(m.indices.size() * 8);
    for (size_t i = 0; i < m.indices.size(); ++i) std::memcpy(&tris[i * 8], &posed[size_t(m.indices[i]) * 8], 8 * sizeof(float));
    gl.BindBuffer(GL_ARRAY_BUFFER, handVbo_[h]);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<rr::render::GLsizeiptr>(tris.size() * sizeof(float)), tris.data(), GL_DYNAMIC_DRAW);
    handCount_[h] = m.indices.size();
}

// One box: centre `c`, half-axis vectors `a`, `b`, `n` (world), flat-shaded.
void VrHandsDraw::Box(const float c[3], const float a[3], const float b[3], const float n[3], const float rgb[3]) {
    const float* axes[3] = {a, b, n};
    for (int f = 0; f < 3; ++f) {
        const float* ax = axes[f];
        const float* u = axes[(f + 1) % 3];
        const float* v = axes[(f + 2) % 3];
        float len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        if (len < 1e-9f) len = 1.0f;
        for (int side = -1; side <= 1; side += 2) {
            float corner[4][3];
            const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
            for (int q = 0; q < 4; ++q)
                for (int k = 0; k < 3; ++k) corner[q][k] = c[k] + ax[k] * static_cast<float>(side) + u[k] * su[q] + v[k] * sv[q];
            const float nrm[3] = {ax[0] / len * static_cast<float>(side), ax[1] / len * static_cast<float>(side),
                                  ax[2] / len * static_cast<float>(side)};
            static const int kTri[6] = {0, 1, 2, 0, 2, 3};
            for (int t : kTri)
                verts_.insert(verts_.end(), {corner[t][0], corner[t][1], corner[t][2], nrm[0], nrm[1], nrm[2], rgb[0], rgb[1], rgb[2]});
        }
    }
}

void VrHandsDraw::Marker(const GripMarker& m) {
    // two square brackets round the grip's ends: cyan where to reach, green within reach
    const float rgb[3] = {m.inReach ? 0.25f : 0.10f, m.inReach ? 0.95f : 0.78f, m.inReach ? 0.30f : 0.92f};
    const float side[3] = {m.axis[1] * m.up[2] - m.axis[2] * m.up[1], m.axis[2] * m.up[0] - m.axis[0] * m.up[2],
                           m.axis[0] * m.up[1] - m.axis[1] * m.up[0]};
    const float s = m.scale, half = 0.030f, bar = 0.0035f;
    for (int end = -1; end <= 1; end += 2) {
        float e[3];
        for (int k = 0; k < 3; ++k) e[k] = m.centre[k] + m.axis[k] * 0.050f * s * static_cast<float>(end);
        for (int edge = 0; edge < 4; ++edge) {
            const float* along = (edge & 1) ? m.up : side;
            const float* off = (edge & 1) ? side : m.up;
            const float sign = (edge & 2) ? -1.0f : 1.0f;
            float c[3], ha[3], hb[3], hn[3];
            for (int k = 0; k < 3; ++k) c[k] = e[k] + off[k] * half * s * sign;
            Scale(along, half * s, ha);
            Scale(off, bar * s, hb);
            Scale(m.axis, bar * s, hn);
            Box(c, ha, hb, hn, rgb);
        }
    }
}

// The basis (vr_driving_visuals.cpp): the fingers along the aim's forward, "up" the grip's +X made square to them,
// right = up x forward turned to the grip's forward; the mesh's x = forward, y = -up, z = right, its origin 5.5 cm
// behind the grip pose along the fingers.
rr::render::Mat4 VrHandsDraw::HandModel(const GloveDraw& g) {
    const V forward = Normal(Make(g.aimForward));
    V up = Make(g.gripRight);
    up = Sub(up, Mul(forward, Dot(up, forward)));
    if (Dot(up, up) < 1e-4f) up = Make(g.gripUp);
    up = Normal(up);
    V right = Normal(Cross(up, forward));
    if (Dot(right, Make(g.gripForward)) < 0.0f) right = Mul(right, -1.0f);
    const V y = Mul(up, -1.0f);
    const V origin = Sub(Make(g.origin), Mul(forward, kPalmBack * g.scale));
    rr::render::Mat4 model;
    const V cols[3] = {Mul(forward, g.scale), Mul(y, g.scale), Mul(right, g.scale)};
    for (int c = 0; c < 3; ++c) {
        model.m[c * 4 + 0] = cols[c].x;
        model.m[c * 4 + 1] = cols[c].y;
        model.m[c * 4 + 2] = cols[c].z;
        model.m[c * 4 + 3] = 0.0f;
    }
    model.m[12] = origin.x, model.m[13] = origin.y, model.m[14] = origin.z, model.m[15] = 1.0f;
    return model;
}

void VrHandsDraw::FistCentre(const GloveDraw& g, float out[3]) {
    const rr::render::Mat4 m = HandModel(g);
    for (int k = 0; k < 3; ++k)
        out[k] = m.m[k] * kFistCentre[0] + m.m[4 + k] * kFistCentre[1] + m.m[8 + k] * kFistCentre[2] + m.m[12 + k];
}

void VrHandsDraw::SeatFist(GloveDraw& g, const float point[3]) {
    float now[3];
    FistCentre(g, now);
    for (int k = 0; k < 3; ++k) g.origin[k] += point[k] - now[k]; // the basis does not depend on the origin
}

void VrHandsDraw::Draw(const rr::render::Mat4& viewProj, const GloveDraw gloves[2], const GripMarker markers[2]) {
    lastVertices_ = 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    // ---- the hands
    if (Load()) {
        bool bound = false;
        for (int h = 0; h < 2; ++h) {
            const GloveDraw& g = gloves[h];
            if (!g.visible) continue;
            const int hand = g.right ? 1 : 0;
            Pose(hand, static_cast<int>(std::lround(std::clamp(g.grip, 0.0f, 1.0f) * kSteps)),
                 static_cast<int>(std::lround(std::clamp(g.trigger, 0.0f, 1.0f) * kSteps)));
            const rr::render::Mat4 model = HandModel(g);
            if (!bound) {
                gl.UseProgram(handProgram_);
                rr::render::UploadViewProj(handViewProjLoc_, viewProj); // both eyes in single-pass (multiview.h)
                const float l = std::sqrt(kLight[0] * kLight[0] + kLight[1] * kLight[1] + kLight[2] * kLight[2]);
                gl.Uniform3f(handLightLoc_, kLight[0] / l, kLight[1] / l, kLight[2] / l);
                gl.ActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, albedo_);
                gl.Uniform1i(handTexLoc_, 0);
                bound = true;
            }
            gl.UniformMatrix4fv(handModelLoc_, 1, GL_FALSE, model.m);
            gl.BindVertexArray(handVao_[hand]);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(handCount_[hand]));
            lastVertices_ += handCount_[hand];
        }
        if (bound) glBindTexture(GL_TEXTURE_2D, 0);
    }
    // ---- the markers
    verts_.clear();
    for (int h = 0; h < 2; ++h)
        if (markers[h].visible) Marker(markers[h]);
    if (!verts_.empty()) {
        if (program_ == 0) {
            program_ = rr::render::BuildProgram(kMarkerVs, kMarkerFs);
            viewProjLoc_ = gl.GetUniformLocation(program_, "uViewProj");
            lightLoc_ = gl.GetUniformLocation(program_, "uLight");
            gl.GenVertexArrays(1, &vao_);
            gl.BindVertexArray(vao_);
            gl.GenBuffers(1, &vbo_);
            gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
            const GLsizei stride = 9 * static_cast<GLsizei>(sizeof(float));
            gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
            gl.EnableVertexAttribArray(0);
            gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
            gl.EnableVertexAttribArray(1);
            gl.VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
            gl.EnableVertexAttribArray(2);
        }
        gl.BindVertexArray(vao_);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl.BufferData(GL_ARRAY_BUFFER, static_cast<rr::render::GLsizeiptr>(verts_.size() * sizeof(float)), verts_.data(),
                      GL_DYNAMIC_DRAW);
        gl.UseProgram(program_);
        rr::render::UploadViewProj(viewProjLoc_, viewProj);
        const float l = std::sqrt(kLight[0] * kLight[0] + kLight[1] * kLight[1] + kLight[2] * kLight[2]);
        gl.Uniform3f(lightLoc_, kLight[0] / l, kLight[1] / l, kLight[2] / l);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size() / 9));
    }
    gl.BindVertexArray(0);
}

} // namespace rrgame
