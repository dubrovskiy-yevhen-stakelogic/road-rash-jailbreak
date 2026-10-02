#include "render/fx_draw.h"

#include "render/edge_rule.h"
#include "render/ot_order.h"

#include "render/race_scene.h"
#include "render/shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace rr::render {
namespace {

const char* const kFxVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;   // normalised device coordinates
layout(location = 1) in vec2 aTexel; // texel coordinates inside the 256 x 256 texture page
layout(location = 2) in vec2 aCon;   // the console's fill rule (edge_rule.h): the packet's console point
noperspective out vec2 vTexel;
out vec3 eCon;
void main() {
    vTexel = aTexel;
    eCon = vec3(aCon, 1.0);
    gl_Position = vec4(aPos, 1.0);
}
)";

// Pass 0 draws the texels without the STP bit (opaque), pass 1 the ones with it, blended by the
// tpage's mode. Blend function (ONE, SRC_ALPHA): alpha 0 replaces, alpha 0.5 halves the background,
// alpha 1 keeps it (and with the reverse-subtract equation, mode 2 subtracts).
const char* const kFxFragmentShader = R"(#version 330 core
noperspective in vec2 vTexel;
uniform sampler2D uPage;
uniform vec3 uColour;
uniform int uRaw;
uniform int uPass;
uniform int uAbr;
out vec4 oColor;
void main() {
    ivec2 t = clamp(ivec2(floor(vTexel)), ivec2(0), ivec2(255));
    vec4 texel = texelFetch(uPage, t, 0);
    if (texel.a < 0.25) discard;
    bool semi = texel.a > 0.75;
    if ((uPass == 0) == semi) discard;
    vec3 c = texel.rgb;
    if (uRaw == 0) c = min(floor(c * 255.0 * uColour * 255.0 / 128.0) / 255.0, vec3(1.0));
    if (uPass == 0) { oColor = vec4(c, 0.0); return; }
    if (uAbr == 0) oColor = vec4(c * 0.5, 0.5);
    else if (uAbr == 3) oColor = vec4(c * 0.25, 1.0);
    else oColor = vec4(c, 1.0);
}
)";

// DrawWorld: the packet's quad placed in the world (a world position per corner), perspective-correct.
const char* const kFxWorldVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;   // world position
layout(location = 1) in vec2 aTexel; // texel coordinates inside the 256 x 256 texture page
uniform mat4 uViewProj;
out vec2 vTexel;
void main() {
    vTexel = aTexel;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

uint8_t Five(uint16_t c, int shift) {
    const uint32_t v = (c >> shift) & 31u;
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

} // namespace

uint32_t FxDraw::Texel(const rr::game::FxVram& vram, uint16_t tpage, uint16_t clut, int u, int v) {
    const int px = (tpage & 0xF) * 64, py = ((tpage >> 4) & 1) * 256;
    const int depth = (tpage >> 7) & 3;
    const int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
    u &= 255;
    v &= 255;
    uint16_t c = 0;
    if (depth == 0) {
        const uint16_t hw = vram.At(px + u / 4, py + v);
        c = vram.At(cx + ((hw >> ((u & 3) * 4)) & 0xF), cy);
    } else if (depth == 1) {
        const uint16_t hw = vram.At(px + u / 2, py + v);
        c = vram.At(cx + ((hw >> ((u & 1) * 8)) & 0xFF), cy);
    } else {
        c = vram.At(px + u, py + v);
    }
    if (c == 0) return 0;
    const uint32_t a = (c & 0x8000u) ? 0xFFu : 0x80u;
    return (a << 24) | (static_cast<uint32_t>(Five(c, 0)) << 16) | (static_cast<uint32_t>(Five(c, 5)) << 8) | Five(c, 10);
}

void FxDraw::ToNdc(float sx, float sy, float viewAspect, float& x, float& y) {
    // SX = 192 + H X / Z, SY = 120 + H (3412/4096) Y / Z; the scene's projection keeps the vertical
    // field 2 atan(120 / (H 3412/4096)) and widens the horizontal one with the viewport.
    x = (sx - 192.0f) * kGteAspectRow / (120.0f * viewAspect);
    y = -(sy - 120.0f) / 120.0f;
}

void FxDraw::ToNdc(float sx, float sy, float viewAspect, const Place& place, float& x, float& y) {
    // Two players: SX = cx + H X / Z over a view whose projection keeps the field 2 atan(halfH / (H 3412/4096))
    // (tools/rrgame/main.cpp's split pass) - the one-player rule with the view's centre and half height.
    x = (sx - place.cx) * kGteAspectRow / (place.halfH * viewAspect);
    y = -(sy - place.cy) / place.halfH;
}

bool FxDraw::Init() {
    program_ = BuildEdgeProgram(kFxVertexShader, kFxFragmentShader, {{"vec2", "vTexel", EdgeVarying::kNoPerspective}}, 0, "effects");
    if (program_ == 0) return false;
    texLoc_ = gl.GetUniformLocation(program_, "uPage");
    colourLoc_ = gl.GetUniformLocation(program_, "uColour");
    rawLoc_ = gl.GetUniformLocation(program_, "uRaw");
    passLoc_ = gl.GetUniformLocation(program_, "uPass");
    abrLoc_ = gl.GetUniformLocation(program_, "uAbr");
    gl.GenVertexArrays(1, &vao_);
    gl.GenBuffers(1, &vbo_);
    gl.BindVertexArray(vao_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl.BufferData(GL_ARRAY_BUFFER, 6 * 7 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<const void*>(3 * sizeof(float)));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<const void*>(5 * sizeof(float)));
    gl.EnableVertexAttribArray(2);
    gl.BindVertexArray(0);
    // DrawWorld's program: the same fragment rule, the texel interpolated perspective-correct (a quad in the world)
    try {
        std::string fs = kFxFragmentShader;
        const std::string np = "noperspective in vec2 vTexel;";
        if (const size_t at = fs.find(np); at != std::string::npos) fs.replace(at, np.size(), "in vec2 vTexel;");
        worldProgram_ = BuildProgram(kFxWorldVertexShader, fs.c_str());
        worldViewProjLoc_ = gl.GetUniformLocation(worldProgram_, "uViewProj");
        worldTexLoc_ = gl.GetUniformLocation(worldProgram_, "uPage");
        worldColourLoc_ = gl.GetUniformLocation(worldProgram_, "uColour");
        worldRawLoc_ = gl.GetUniformLocation(worldProgram_, "uRaw");
        worldPassLoc_ = gl.GetUniformLocation(worldProgram_, "uPass");
        worldAbrLoc_ = gl.GetUniformLocation(worldProgram_, "uAbr");
        gl.GenVertexArrays(1, &worldVao_);
        gl.GenBuffers(1, &worldVbo_);
        gl.BindVertexArray(worldVao_);
        gl.BindBuffer(GL_ARRAY_BUFFER, worldVbo_);
        gl.BufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<const void*>(3 * sizeof(float)));
        gl.EnableVertexAttribArray(1);
        gl.BindVertexArray(0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "effects: the world billboards' program: %s\n", e.what());
        worldProgram_ = 0;
    }
    return true;
}

size_t FxDraw::DrawWorld(const std::vector<rr::game::FxPacket>& packets, const rr::game::FxVram& vram, uint32_t view,
                         const float camEye[3], const float camRows[3][3], float ofx, float ofy, const Mat4& viewMatrix,
                         const Mat4& viewProj) {
    if (worldProgram_ == 0) return 0;
    // the drawing view's right and up in the world: the first two rows of its LookAt (column-major)
    const float right[3] = {viewMatrix.m[0], viewMatrix.m[4], viewMatrix.m[8]};
    const float up[3] = {viewMatrix.m[1], viewMatrix.m[5], viewMatrix.m[9]};
    const float hx = kGteH, hy = kGteH * kGteAspectRow;
    size_t drawn = 0;
    bool set = false;
    for (const rr::game::FxPacket& p : packets) {
        if (!p.drawable || p.view != view) continue;
        const uint32_t cmd = p.word1 >> 24;
        if (cmd != 0x2Eu && cmd != 0x2Fu && cmd != 0x2Cu && cmd != 0x2Du) continue;
        const float z = static_cast<float>(p.depth) / 64.0f;
        if (!(z > 0.05f)) continue; // at or behind the console's eye: no place to put it
        float x[4], y[4], cx = 0.0f, cy = 0.0f;
        for (int k = 0; k < 4; ++k) {
            x[k] = (static_cast<float>(p.x[k]) - ofx) * z / hx;
            y[k] = (static_cast<float>(p.y[k]) - ofy) * z / hy;
            cx += 0.25f * x[k];
            cy += 0.25f * y[k];
        }
        float centre[3];
        for (int c = 0; c < 3; ++c) centre[c] = camEye[c] + cx * camRows[0][c] + cy * camRows[1][c] + z * camRows[2][c];
        float vtx[4][5];
        for (int k = 0; k < 4; ++k) {
            const float ox = x[k] - cx, oy = y[k] - cy; // oy grows downwards on the console's screen
            for (int c = 0; c < 3; ++c) vtx[k][c] = centre[c] + ox * right[c] - oy * up[c];
            vtx[k][3] = static_cast<float>(p.u[k]);
            vtx[k][4] = static_cast<float>(p.v[k]);
        }
        if (!set) {
            set = true;
            gl.UseProgram(worldProgram_);
            UploadViewProj(worldViewProjLoc_, viewProj);
            gl.BindVertexArray(worldVao_);
            gl.BindBuffer(GL_ARRAY_BUFFER, worldVbo_);
            gl.ActiveTexture(GL_TEXTURE0);
            gl.Uniform1i(worldTexLoc_, 0);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_SRC_ALPHA);
        }
        static constexpr int kOrder[6] = {0, 1, 2, 1, 2, 3};
        float buf[6][5];
        for (int k = 0; k < 6; ++k)
            for (int c = 0; c < 5; ++c) buf[k][c] = vtx[kOrder[k]][c];
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(buf), buf, GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, Page(vram, p.tpage, p.clut));
        gl.Uniform3f(worldColourLoc_, static_cast<float>(p.word1 & 0xFFu) / 255.0f,
                     static_cast<float>((p.word1 >> 8) & 0xFFu) / 255.0f, static_cast<float>((p.word1 >> 16) & 0xFFu) / 255.0f);
        gl.Uniform1i(worldRawLoc_, (cmd & 1u) ? 1 : 0);
        const int abr = (p.tpage >> 5) & 3;
        gl.Uniform1i(worldAbrLoc_, abr);
        gl.Uniform1i(worldPassLoc_, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        gl.Uniform1i(worldPassLoc_, 1);
        if (abr == 2) gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        if (abr == 2) gl.BlendEquation(GL_FUNC_ADD);
        ++drawn;
    }
    if (set) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        gl.BindVertexArray(0);
    }
    return drawn;
}

GLuint FxDraw::Page(const rr::game::FxVram& vram, uint16_t tpage, uint16_t clut) {
    const uint32_t key = (static_cast<uint32_t>(tpage & 0x19Fu)) | (static_cast<uint32_t>(clut) << 16);
    auto it = pages_.find(key);
    if (it != pages_.end()) return it->second;
    std::vector<uint32_t> rgba(256u * 256u);
    for (int v = 0; v < 256; ++v)
        for (int u = 0; u < 256; ++u) {
            const uint32_t t = Texel(vram, tpage, clut, u, v);
            // GL_RGBA bytes: r, g, b, a
            rgba[static_cast<size_t>(v) * 256u + static_cast<size_t>(u)] =
                ((t >> 16) & 0xFFu) | (t & 0xFF00u) | ((t & 0xFFu) << 16) | (t & 0xFF000000u);
        }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    pages_[key] = tex;
    return tex;
}

size_t FxDraw::Draw(const std::vector<rr::game::FxPacket>& packets, const rr::game::FxVram& vram, float viewAspect,
                    float nearZ, float farZ, const Place& place) {
    if (program_ == 0) return 0;
    size_t drawn = 0;
    const char* pixelEnv = std::getenv("RRJB_FX_PIXEL");
    const float pixelShift = (pixelEnv != nullptr && std::strcmp(pixelEnv, "centre") == 0) ? 0.0f : 0.5f;
    gl.UseProgram(program_);
    EdgeViewport(program_); // the console's fill rule (edge_rule.h)
    gl.BindVertexArray(vao_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.Uniform1i(texLoc_, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    for (const rr::game::FxPacket& p : packets) {
        if (!p.drawable || p.view != place.view) continue;
        const uint32_t cmd = p.word1 >> 24;
        if (cmd != 0x2Eu && cmd != 0x2Fu && cmd != 0x2Cu && cmd != 0x2Du) continue;
        const float d = std::max(static_cast<float>(p.depth) / 64.0f - 0.5f, nearZ);
        const int table = otOn_ && otTableOf_ ? otTableOf_(p.entity) : 0;
        ++otTableCounts[table < 0 ? 3 : (table == 0 ? 2 : table - 1)];
        if (table < 0) continue; // its cell is in neither table: 0x800674D4 never reaches it
        const OtTable single{otNear_, otShift_, otMax_, otBase_};
        const OtTable& t = table == 1 ? otTables_[0] : (table == 2 ? otTables_[1] : single);
        const float zNdc = otOn_ ? OtDepthOf(OtSlotOf(p.depth, t.nearOffset, t.shift, t.maxSlot), table != 0 ? otRank_ : 1023,
                                             t.maxSlot, t.base) * 2.0f - 1.0f
                                 : std::clamp((farZ + nearZ) / (farZ - nearZ) - (2.0f * farZ * nearZ) / ((farZ - nearZ) * d), -1.0f, 1.0f);
        float vtx[4][7];
        for (int k = 0; k < 4; ++k) {
            // The GPU covers and samples a pixel at its integer corner (x, y), GL at its centre (x + 0.5, y + 0.5):
            // half a console pixel added to every vertex makes them the same coverage test and the same affine texel
            // (without it the spark sheet's sparse texels land elsewhere). RRJB_FX_PIXEL=centre: control.
            ToNdc(static_cast<float>(p.x[k]) + pixelShift, static_cast<float>(p.y[k]) + pixelShift, viewAspect, place,
                  vtx[k][0], vtx[k][1]);
            vtx[k][2] = zNdc;
            vtx[k][3] = static_cast<float>(p.u[k]);
            vtx[k][4] = static_cast<float>(p.v[k]);
            vtx[k][5] = static_cast<float>(p.x[k]); // edges: the console point the GPU's fill rule is taken at
            vtx[k][6] = static_cast<float>(p.y[k]);
        }
        static constexpr int kOrder[6] = {0, 1, 2, 1, 2, 3};
        float buf[6][7];
        for (int k = 0; k < 6; ++k)
            for (int c = 0; c < 7; ++c) buf[k][c] = vtx[kOrder[k]][c];
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(buf), buf, GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, Page(vram, p.tpage, p.clut));
        gl.Uniform3f(colourLoc_, static_cast<float>(p.word1 & 0xFFu) / 255.0f,
                     static_cast<float>((p.word1 >> 8) & 0xFFu) / 255.0f, static_cast<float>((p.word1 >> 16) & 0xFFu) / 255.0f);
        gl.Uniform1i(rawLoc_, (cmd & 1u) ? 1 : 0);
        const int abr = (p.tpage >> 5) & 3;
        gl.Uniform1i(abrLoc_, abr);
        gl.Uniform1i(passLoc_, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        gl.Uniform1i(passLoc_, 1);
        if (abr == 2) gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        if (abr == 2) gl.BlendEquation(GL_FUNC_ADD);
        ++drawn;
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    gl.BindVertexArray(0);
    return drawn;
}

} // namespace rr::render
