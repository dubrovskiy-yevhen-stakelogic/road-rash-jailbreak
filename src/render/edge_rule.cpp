#include "render/edge_rule.h"

#include "render/shaders.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

#ifndef GL_GEOMETRY_SHADER
#define GL_GEOMETRY_SHADER 0x8DD9
#endif

namespace rr::render {
namespace {

std::vector<std::string>& Programs() {
    static std::vector<std::string> p;
    return p;
}

const char* InterpWord(EdgeVarying::Interp i) {
    return i == EdgeVarying::kFlat ? "flat " : (i == EdgeVarying::kNoPerspective ? "noperspective " : "");
}

// The growth: NDC units the triangle's edges move out by (0.004 = ~0.5 console line at 240 lines, more pixels on a
// larger window - only the fragment count grows), and the largest homothety factor (a sliver's inradius is tiny).
std::string GeometrySource(std::initializer_list<EdgeVarying> varyings, int clipDistances) {
    std::string s = "#version 330 core\n"
                    "layout(triangles) in;\n"
                    "layout(triangle_strip, max_vertices = 3) out;\n"
                    "in vec3 eCon[];\n"
                    "uniform vec2 uEdgeViewport;\n" // EdgeViewport: the viewport's size in window pixels
                    "flat out vec2 eC0;\nflat out vec2 eC1;\nflat out vec2 eC2;\nflat out float eOn;\n"
                    "noperspective out vec2 eS;\n";
    for (const EdgeVarying& v : varyings) {
        const std::string q = std::string(InterpWord(v.interp)) + (v.centroid ? "centroid " : "");
        s += q + "in " + v.type + " " + v.name + "[];\n";
        s += q + "out " + v.type + " e_" + v.name + ";\n";
    }
    s += "void main() {\n"
         "    vec4 P0 = gl_in[0].gl_Position, P1 = gl_in[1].gl_Position, P2 = gl_in[2].gl_Position;\n"
         "    bool on = eCon[0].z > 0.5 && eCon[1].z > 0.5 && eCon[2].z > 0.5 && P0.w > 0.0 && P1.w > 0.0 && P2.w > 0.0\n"
         "              && uEdgeViewport.x > 0.0 && uEdgeViewport.y > 0.0;\n"
         "    mat3 S = mat3(1.0), C = mat3(1.0);\n" // column k: the weights of emitted corner k over the three inputs
         "    vec2 q0 = P0.xy / P0.w, q1 = P1.xy / P1.w, q2 = P2.xy / P2.w;\n" // used only when every w > 0
         "    if (on) {\n"
         // Only on the console's own grid: one window pixel per console pixel, rows downwards (the map from the
         // console corners to the window's, J, the identity with y flipped). On a larger window the console's fill
         // rule has no pixels to decide and GL's own (watertight) coverage stays.
         "        vec2 h = 0.5 * uEdgeViewport;\n"
         // (each edge on its own, so a sliver is judged as well as a fat triangle; the tolerance leaves room for the
         // corners the CPU put on their SXY in floats, and any window 1.35 times the console's or more fails it)
         "        vec2 c1 = eCon[1].xy - eCon[0].xy, c2 = eCon[2].xy - eCon[1].xy, c3 = eCon[0].xy - eCon[2].xy;\n"
         "        vec2 w1 = (q1 - q0) * h, w2 = (q2 - q1) * h, w3 = (q0 - q2) * h;\n"
         "        on = length(w1 - vec2(c1.x, -c1.y)) < 0.1 * length(c1) + 0.25 && length(w2 - vec2(c2.x, -c2.y)) < 0.1 * length(c2) + 0.25\n"
         "             && length(w3 - vec2(c3.x, -c3.y)) < 0.1 * length(c3) + 0.25;\n"
         "    }\n"
         "    if (on) {\n"
         "        vec2 e1 = q1 - q0, e2 = q2 - q0;\n"
         "        float area2 = abs(e1.x * e2.y - e1.y * e2.x);\n"
         "        float a = length(q1 - q2), b = length(q2 - q0), c = length(q0 - q1);\n"
         "        float per = a + b + c;\n"
         "        if (area2 > 1.0e-14 && per > 0.0) {\n"
         "            vec3 bI = vec3(a, b, c) / per;\n"      // the incentre's barycentric weights
         "            float r = area2 / per;\n"               // the inradius
         "            float k = 1.0 + min(0.004 / r, 32.0);\n"
         "            vec3 w = vec3(P0.w, P1.w, P2.w);\n"
         "            mat3 S2, C2;\n"
         "            bool ok = true;\n"
         "            for (int j = 0; j < 3; ++j) {\n"
         "                vec3 e = vec3(j == 0 ? 1.0 : 0.0, j == 1 ? 1.0 : 0.0, j == 2 ? 1.0 : 0.0);\n"
         "                vec3 s = (1.0 - k) * bI + k * e;\n"  // I + k (q_j - I) in screen weights
         "                vec3 t = s / w;\n"
         "                float T = t.x + t.y + t.z;\n"
         "                if (!(T > 0.0)) ok = false;\n"
         "                S2[j] = s;\n"
         "                C2[j] = t / T;\n"
         "            }\n"
         "            if (ok) { S = S2; C = C2; }\n"
         "        }\n"
         "    }\n"
         "    for (int j = 0; j < 3; ++j) {\n"
         "        vec3 cw = C[j], sw = S[j];\n"
         "        gl_Position = cw.x * P0 + cw.y * P1 + cw.z * P2;\n";
    for (int k = 0; k < clipDistances; ++k) {
        const std::string i = std::to_string(k);
        s += "        gl_ClipDistance[" + i + "] = cw.x * gl_in[0].gl_ClipDistance[" + i + "] + cw.y * gl_in[1].gl_ClipDistance[" +
             i + "] + cw.z * gl_in[2].gl_ClipDistance[" + i + "];\n";
    }
    for (const EdgeVarying& v : varyings) {
        const std::string n = v.name;
        if (v.interp == EdgeVarying::kFlat)
            s += "        e_" + n + " = " + n + "[2];\n"; // the provoking (last) corner, as without the stage
        else {
            const char* wv = v.interp == EdgeVarying::kSmooth ? "cw" : "sw";
            s += "        e_" + n + " = " + wv + ".x * " + n + "[0] + " + wv + ".y * " + n + "[1] + " + wv + ".z * " + n + "[2];\n";
        }
    }
    s += "        eS = sw.x * eCon[0].xy + sw.y * eCon[1].xy + sw.z * eCon[2].xy;\n"
         "        eC0 = eCon[0].xy; eC1 = eCon[1].xy; eC2 = eCon[2].xy;\n"
         "        eOn = on ? 1.0 : 0.0;\n"
         "        EmitVertex();\n"
         "    }\n"
         "    EndPrimitive();\n"
         "}\n";
    return s;
}

// The console's edge test at the fragment's console point (psxgpu.py `_tl`, hud_view.cpp, shell_view.h RasterQuad).
// The stage turns it on only on the console's own grid (a 384 x 240 picture), where the point is an integer up to the
// interpolation's float error (a corner the CPU placed in floats: up to 1/16 px) and is snapped.
const char* const kEdgeFragment = R"(
flat in vec2 eC0;
flat in vec2 eC1;
flat in vec2 eC2;
flat in float eOn;
noperspective in vec2 eS;
bool EdgeIn(vec2 p, vec2 q, vec2 s) {
    float w = (q.x - p.x) * (s.y - p.y) - (q.y - p.y) * (s.x - p.x);
    if (w != 0.0) return w > 0.0;
    float dx = q.x - p.x, dy = q.y - p.y;
    return (dy == 0.0 && dx > 0.0) || dy < 0.0; // a top edge (the interior below it) or a left edge
}
bool EdgeOut() {
    if (eOn < 0.5) return false;
    vec2 s = eS;
    vec2 r = floor(s + 0.5);
    if (abs(s.x - r.x) < 1.0 / 8.0 && abs(s.y - r.y) < 1.0 / 8.0) s = r;
    vec2 a = eC0, b = eC1, c = eC2;
    float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0.0) return true; // the console draws no pixel of a zero-area triangle
    if (area < 0.0) { vec2 t = b; b = c; c = t; }
    return !(EdgeIn(a, b, s) && EdgeIn(b, c, s) && EdgeIn(c, a, s));
}
)";

std::string FragmentSource(const char* fs, std::initializer_list<EdgeVarying> varyings) {
    std::string src = fs;
    const size_t eol = src.find('\n');
    if (eol == std::string::npos) throw std::runtime_error("edge rule: fragment source has no #version line");
    std::string head;
    for (const EdgeVarying& v : varyings) head += std::string("#define ") + v.name + " e_" + v.name + "\n";
    head += kEdgeFragment;
    src.insert(eol + 1, head);
    const std::string mainSig = "void main() {";
    const size_t m = src.find(mainSig);
    if (m == std::string::npos) throw std::runtime_error("edge rule: fragment source has no 'void main() {'");
    // RRJB_EDGE_SHOW=1 (DEVELOPMENT): the triangles that carry the rule painted magenta
    static const bool show = std::getenv("RRJB_EDGE_SHOW") != nullptr;
    src.insert(m + mainSig.size(), show ? "\n    if (EdgeOut()) discard;\n    if (eOn > 0.5) { oColor = vec4(1.0, 0.0, 1.0, 1.0); return; }"
                                        : "\n    if (EdgeOut()) discard;");
    return src;
}

} // namespace

bool EdgeRuleOn() {
    static const bool on = !(std::getenv("RRJB_EDGE") != nullptr && std::strcmp(std::getenv("RRJB_EDGE"), "gl") == 0);
    return on;
}

GLuint BuildEdgeProgram(const char* vertexSource, const char* fragmentSource, std::initializer_list<EdgeVarying> varyings,
                        int clipDistances, const char* name) {
    if (!EdgeRuleOn()) return BuildProgram(vertexSource, fragmentSource);
    const std::string gs = GeometrySource(varyings, clipDistances);
    const std::string fs = FragmentSource(fragmentSource, varyings);
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, CompileShader(GL_VERTEX_SHADER, vertexSource));
    gl.AttachShader(program, CompileShader(GL_GEOMETRY_SHADER, gs.c_str()));
    gl.AttachShader(program, CompileShader(GL_FRAGMENT_SHADER, fs.c_str()));
    gl.LinkProgram(program);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        gl.GetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        throw std::runtime_error(std::string("edge rule: program '") + name + "' link failed: " + log);
    }
    Programs().push_back(name);
    return program;
}

namespace {
std::map<std::pair<int, int>, size_t>& Viewports() {
    static std::map<std::pair<int, int>, size_t> v;
    return v;
}
} // namespace

void EdgeViewport(GLuint program) {
    if (!EdgeRuleOn() || program == 0) return;
    GLint vp[4] = {};
    glGetIntegerv(GL_VIEWPORT, vp);
    gl.Uniform2f(gl.GetUniformLocation(program, "uEdgeViewport"), static_cast<float>(vp[2]), static_cast<float>(vp[3]));
    ++Viewports()[{vp[2], vp[3]}];
}

std::string EdgeTotals() {
    std::string s = "edges: the console's fill rule (a pixel's integer point inside, or on a top / left edge)";
    if (!EdgeRuleOn()) return s + " - SWITCHED OFF (RRJB_EDGE=gl: GL's own coverage, the control)";
    s += " on the GL programs";
    for (size_t i = 0; i < Programs().size(); ++i) s += (i == 0 ? " " : ", ") + Programs()[i];
    if (Programs().empty()) s += " (none built)";
    s += " where one window pixel is one console pixel; draws by viewport:";
    for (const auto& [wh, n] : Viewports()) s += " " + std::to_string(wh.first) + "x" + std::to_string(wh.second) + " " + std::to_string(n);
    if (Viewports().empty()) s += " none";
    return s + "; the HUD and menu rasterisers on the same rule";
}

} // namespace rr::render
