#pragma once
// The PlayStation GPU's fill rule on GL.
//
// The console covers pixel (x, y) of a polygon when the integer point (x, y) is inside the triangle or exactly on its
// TOP or LEFT edge (a polygon is drawn up to, excluding, its right-most column and bottom-most row; a quad is the two
// triangles (v0, v1, v2) and (v1, v2, v3)). The product draws a console point at its pixel centre (the half-pixel
// shift of the GTE projection, uGtePix), so GL samples exactly the console's points and every edge that passes through
// one is a tie - and GL settles ties with ITS rule, which on this machine keeps the bottom edge instead of the top.
//
// BuildEdgeProgram links a program with a geometry stage that (1) grows each triangle whose three corners are exact
// console points (the vertex stage's `out vec3 eCon`: x, y and z = 1) by a homothety about its incentre, so GL makes
// a fragment for every point the console could cover, with every varying extrapolated along the same plane
// (perspective-correct ones through the clip weights, noperspective ones through the screen weights), and (2) hands
// the fragment stage the triangle's console corners and the fragment's console point; the fragment stage then decides
// with the console's own edge test (`EdgeOut`, the same test as tools\scout\psxgpu.py's and hud_view.cpp's) and
// discards what the console would not draw. A triangle with a corner that is not a console point, or drawn where one
// window pixel is not one console pixel (the 1280 x 720 window: there are no console pixels to decide, and GL's own
// coverage is watertight), is drawn as GL draws it - the rule is for the console's own grid (--parity, 384 x 240).
// RRJB_EDGE_SHOW=1 (DEVELOPMENT) paints the triangles that take the rule magenta.
//
// RRJB_EDGE=gl: the programs are built without the stage - GL's own coverage (the
// negative control; it also returns the CPU rasterisers of the HUD and the menus to their old rules).
#include "render/gl_api.h"

#include <initializer_list>
#include <string>

namespace rr::render {

struct EdgeVarying {
    enum Interp { kSmooth, kNoPerspective, kFlat };
    const char* type; // "vec2", "float", ...
    const char* name; // the vertex stage's output = the fragment stage's input
    Interp interp;
    bool centroid = false; // interpolated at the centroid ("centroid in" in the fragment stage)
};

// false with RRJB_EDGE=gl.
bool EdgeRuleOn();

// `vertexSource` must declare `out vec3 eCon;`; `clipDistances` = how many gl_ClipDistance it writes.
GLuint BuildEdgeProgram(const char* vertexSource, const char* fragmentSource, std::initializer_list<EdgeVarying> varyings,
                        int clipDistances, const char* name);

// Hands the bound `program` the viewport's size: the stage decides only where one window pixel is one console pixel
// (on a larger window the rule has no console pixels to decide and GL's own watertight coverage is kept). Call it
// after UseProgram, once the view's viewport is set.
void EdgeViewport(GLuint program);

// For the race log: which programs carry the rule.
std::string EdgeTotals();

} // namespace rr::render
