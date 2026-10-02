#include "render/sky_gpu.h"

#include "render/edge_rule.h"
#include "render/race_scene.h"
#include "render/shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

namespace rr::render {
namespace {

const char* const kSkyGpuVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;    // the console pixel (half a pixel on already)
layout(location = 1) in vec2 aTexel;  // texel coordinates inside the 256 x 256 texture page
layout(location = 2) in vec3 aColour; // the G4's vertex colour (0..255)
uniform vec4 uMap;                    // DrawRequest::gteMap
noperspective out vec2 vTexel;
noperspective out vec3 vColour;
out vec3 eCon; // the console's fill rule (edge_rule.h): the console point the fill rule is taken at
void main() {
    vTexel = aTexel;
    eCon = vec3(aPos - vec2(0.5), 1.0);
    vColour = aColour;
    gl_Position = vec4(aPos.x * uMap.x + uMap.y, aPos.y * uMap.z + uMap.w, 0.0, 1.0);
}
)";

// Pass 0 draws the texels drawn opaque (every texel of an opaque command, the ones without STP of a semi-transparent
// one), pass 1 the STP texels of a semi-transparent command, blended by the tpage's mode. Blend (ONE, SRC_ALPHA):
// alpha 0 replaces, 0.5 halves the background, 1 keeps it (mode 2 with the reverse-subtract equation).
const char* const kSkyGpuFragmentShader = R"(#version 330 core
noperspective in vec2 vTexel;
noperspective in vec3 vColour;
uniform sampler2D uPage;
uniform vec3 uColour;
uniform int uRaw;
uniform int uSemi;
uniform int uPass;
uniform int uAbr;
uniform int uShaded;
out vec4 oColor;
void main() {
    if (uShaded != 0) { // the G4: the colour interpolated in 8 bits and truncated, then to 5 bits (no dither)
        vec3 o5 = floor(floor(vColour + 0.001) / 8.0);
        oColor = vec4((o5 * 8.0 + floor(o5 / 4.0)) / 255.0, 0.0);
        return;
    }
    ivec2 t = clamp(ivec2(floor(vTexel + vec2(1.0 / 256.0))), ivec2(0), ivec2(255));
    vec4 texel = texelFetch(uPage, t, 0);
    if (texel.a < 0.25) discard;
    bool blended = uSemi != 0 && texel.a > 0.75;
    if ((uPass == 0) == blended) discard;
    vec3 c = texel.rgb;
    if (uRaw == 0) { // (texel5 * colour8) >> 7, saturated at 31
        vec3 t5 = floor(c * 255.0 / 8.0 + 0.001);
        vec3 o5 = min(floor(t5 * floor(uColour * 255.0 + 0.5) / 128.0), vec3(31.0));
        c = (o5 * 8.0 + floor(o5 / 4.0)) / 255.0;
    }
    if (uPass == 0) { oColor = vec4(c, 0.0); return; }
    if (uAbr == 0) oColor = vec4(c * 0.5, 0.5);
    else if (uAbr == 3) oColor = vec4(c * 0.25, 1.0);
    else oColor = vec4(c, 1.0);
}
)";

struct State {
    const std::vector<rr::game::SkyPacket>* packets = nullptr;
    const rr::game::SkyVram* vram = nullptr;
    GLuint program = 0, vao = 0, vbo = 0;
    GLint mapLoc = -1, pageLoc = -1, colourLoc = -1, rawLoc = -1, semiLoc = -1, passLoc = -1, abrLoc = -1, shadedLoc = -1;
    struct Page {
        GLuint tex = 0;
        uint64_t generation = 0;
    };
    std::map<uint32_t, Page> pages;
    SkyGpuStats stats;
    bool failed = false;
    bool anyPicture = true; // the packets on the float camera and the wide picture too
};
State& St() {
    static State s;
    return s;
}

uint8_t Five(uint16_t c, int shift) {
    const uint32_t v = (c >> shift) & 31u;
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

// The texel the GPU reads at (u, v) of `tpage` / `clut`: 0 transparent, else RGBA with A 0xFF for STP, 0x80 without.
uint32_t Texel(const rr::game::SkyVram& vram, uint16_t tpage, uint16_t clut, int u, int v) {
    const int px = (tpage & 0xF) * 64, py = ((tpage >> 4) & 1) * 256;
    const int depth = (tpage >> 7) & 3;
    const int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
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
    return Five(c, 0) | (static_cast<uint32_t>(Five(c, 5)) << 8) | (static_cast<uint32_t>(Five(c, 10)) << 16) | (a << 24);
}

GLuint PageTexture(State& s, uint16_t tpage, uint16_t clut) {
    const uint32_t key = static_cast<uint32_t>(tpage & 0x19Fu) | (static_cast<uint32_t>(clut) << 16);
    State::Page& p = s.pages[key];
    if (p.tex != 0 && p.generation == s.vram->generation) return p.tex;
    std::vector<uint32_t> rgba(256u * 256u);
    for (int v = 0; v < 256; ++v)
        for (int u = 0; u < 256; ++u) rgba[static_cast<size_t>(v) * 256u + static_cast<size_t>(u)] = Texel(*s.vram, tpage, clut, u, v);
    if (p.tex == 0) {
        glGenTextures(1, &p.tex);
        glBindTexture(GL_TEXTURE_2D, p.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, p.tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    p.generation = s.vram->generation;
    ++s.stats.pages;
    return p.tex;
}

bool Init(State& s) {
    if (s.program != 0) return true;
    if (s.failed) return false;
    s.program = BuildEdgeProgram(kSkyGpuVertexShader, kSkyGpuFragmentShader,
                                 {{"vec2", "vTexel", EdgeVarying::kNoPerspective}, {"vec3", "vColour", EdgeVarying::kNoPerspective}},
                                 0, "sky"); // the console's fill rule (RRJB_EDGE=gl: GL's own)
    if (s.program == 0) {
        s.failed = true;
        return false;
    }
    s.mapLoc = gl.GetUniformLocation(s.program, "uMap");
    s.pageLoc = gl.GetUniformLocation(s.program, "uPage");
    s.colourLoc = gl.GetUniformLocation(s.program, "uColour");
    s.rawLoc = gl.GetUniformLocation(s.program, "uRaw");
    s.semiLoc = gl.GetUniformLocation(s.program, "uSemi");
    s.passLoc = gl.GetUniformLocation(s.program, "uPass");
    s.abrLoc = gl.GetUniformLocation(s.program, "uAbr");
    s.shadedLoc = gl.GetUniformLocation(s.program, "uShaded");
    gl.GenVertexArrays(1, &s.vao);
    gl.GenBuffers(1, &s.vbo);
    gl.BindVertexArray(s.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, s.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, 6 * 7 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<const void*>(2 * sizeof(float)));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<const void*>(4 * sizeof(float)));
    gl.EnableVertexAttribArray(2);
    gl.BindVertexArray(0);
    return true;
}

} // namespace

void SkyGpuSubmit(const std::vector<rr::game::SkyPacket>* packets, const rr::game::SkyVram* vram, bool anyPicture) {
    St().packets = packets;
    St().vram = vram;
    St().anyPicture = anyPicture;
}

const SkyGpuStats& SkyGpuCounters() { return St().stats; }

bool SkyGpuHasGradient() {
    const State& s = St();
    if (s.packets == nullptr || s.failed) return false;
    for (const rr::game::SkyPacket& p : *s.packets)
        if (p.cmd == 0x38u) return true;
    return false;
}

bool SkyGpuDraw(const DrawRequest& request) {
    State& s = St();
    if (s.packets == nullptr || s.vram == nullptr || s.packets->empty()) return false;
    // the float camera and the wide picture too - the packets sit at the GTE's own points, which the
    // frame's projection maps onto the picture whichever way the world is placed (the wide picture's extra sky is
    // the sky's programs drawing on past the console's field, sky_product.h)
    if (request.debugMode != 0 || (!s.anyPicture && (!request.gteProj || request.sideSqueeze > 1.001f))) {
        ++s.stats.fallbacks; // RRJB_SKY3=off: the 4:3 GTE picture only
        return false;
    }
    if (!Init(s)) return false;
    gl.UseProgram(s.program);
    EdgeViewport(s.program); // the console's fill rule (edge_rule.h)
    gl.BindVertexArray(s.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, s.vbo);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.Uniform1i(s.pageLoc, 0);
    gl.Uniform4f(s.mapLoc, request.gteMap[0], request.gteMap[1], request.gteMap[2], request.gteMap[3]);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    for (const rr::game::SkyPacket& p : *s.packets) {
        const bool shaded = p.cmd >= 0x38u && p.cmd <= 0x3Bu;
        const bool sprite = p.w != 0 && p.cmd >= 0x64u && p.cmd <= 0x67u;
        if (!shaded && !sprite && p.cmd != 0x2Cu && p.cmd != 0x2Du && p.cmd != 0x2Eu && p.cmd != 0x2Fu) continue;
        float vtx[4][7] = {};
        if (sprite) { // SPRT: the rectangle's pixels, texel (u + dx, v + dy) at pixel (x + dx, y + dy)
            const float x0 = p.x[0], y0 = p.y[0], x1 = x0 + p.w, y1 = y0 + p.h;
            const float u0 = p.u[0], v0 = p.v[0], u1 = u0 + p.w, v1 = v0 + p.h;
            const float q[4][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1}, {x1, y1, u1, v1}};
            for (int k = 0; k < 4; ++k)
                for (int c = 0; c < 4; ++c) vtx[k][c] = q[k][c];
        } else {
            // the G4: the GPU's top-left rule for a pixel centre ON an edge (the gradient's top edge is row 0's
            // centres): the vertices a hair up-left, so a top / left edge takes its pixels and a bottom / right one not
            // (edge_rule.h: the programs' own edge test decides it exactly - RRJB_EDGE=gl brings the nudge back)
            const float nudge = shaded && !EdgeRuleOn() ? 1.0f / 16.0f : 0.0f; // well over the projection map's float error
            for (int k = 0; k < 4; ++k) {
                vtx[k][0] = static_cast<float>(p.x[k]) + 0.5f - nudge;
                vtx[k][1] = static_cast<float>(p.y[k]) + 0.5f - nudge;
                vtx[k][2] = static_cast<float>(p.u[k]);
                vtx[k][3] = static_cast<float>(p.v[k]);
                vtx[k][4] = static_cast<float>(p.rgb[k] & 0xFFu);
                vtx[k][5] = static_cast<float>((p.rgb[k] >> 8) & 0xFFu);
                vtx[k][6] = static_cast<float>((p.rgb[k] >> 16) & 0xFFu);
            }
        }
        static constexpr int kOrder[6] = {0, 1, 2, 1, 2, 3};
        float buf[6][7];
        int n = 0;
        for (int tri = 0; tri < 2; ++tri) {
            const int* ix = kOrder + 3 * tri;
            if (!sprite) { // the hardware drops a polygon wider than 1023 or taller than 511 (per triangle)
                int minX = 1 << 20, maxX = -(1 << 20), minY = 1 << 20, maxY = -(1 << 20);
                for (int k = 0; k < 3; ++k) {
                    minX = std::min<int>(minX, p.x[ix[k]]);
                    maxX = std::max<int>(maxX, p.x[ix[k]]);
                    minY = std::min<int>(minY, p.y[ix[k]]);
                    maxY = std::max<int>(maxY, p.y[ix[k]]);
                }
                if (maxX - minX >= 1024 || maxY - minY >= 512) continue;
            }
            for (int k = 0; k < 3; ++k, ++n)
                for (int c = 0; c < 7; ++c) buf[n][c] = vtx[ix[k]][c];
        }
        if (n == 0) continue;
        gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * 7 * sizeof(float)), buf, GL_DYNAMIC_DRAW);
        gl.Uniform1i(s.shadedLoc, shaded ? 1 : 0);
        if (shaded) { // opaque Gouraud (0x3A / 0x3B would be semi-transparent: not emitted by the sky)
            gl.Uniform1i(s.passLoc, 0);
            glDrawArrays(GL_TRIANGLES, 0, n);
            ++s.stats.packets;
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, PageTexture(s, p.tpage, p.clut));
        gl.Uniform3f(s.colourLoc, static_cast<float>(p.colour & 0xFFu) / 255.0f, static_cast<float>((p.colour >> 8) & 0xFFu) / 255.0f,
                     static_cast<float>((p.colour >> 16) & 0xFFu) / 255.0f);
        gl.Uniform1i(s.rawLoc, (p.cmd & 1u) ? 1 : 0);
        const bool semi = (p.cmd & 2u) != 0;
        gl.Uniform1i(s.semiLoc, semi ? 1 : 0);
        const int abr = (p.tpage >> 5) & 3;
        gl.Uniform1i(s.abrLoc, abr);
        gl.Uniform1i(s.passLoc, 0);
        glDrawArrays(GL_TRIANGLES, 0, n);
        if (semi) {
            gl.Uniform1i(s.passLoc, 1);
            if (abr == 2) gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glDrawArrays(GL_TRIANGLES, 0, n);
            if (abr == 2) gl.BlendEquation(GL_FUNC_ADD);
        }
        ++s.stats.packets;
    }
    gl.Uniform1i(s.shadedLoc, 0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    gl.BindVertexArray(0);
    ++s.stats.frames;
    return true;
}

} // namespace rr::render
