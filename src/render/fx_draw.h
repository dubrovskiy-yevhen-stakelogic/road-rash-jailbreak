#pragma once
// The effects' packets drawn the way the GPU draws them: each
// packet the PORTED pass links (src\game\fx_runtime.h) is a semi-transparent textured quad in SCREEN
// space (GP0 0x2E modulated by its colour, 0x2F raw), vertices v0 v1 v2 v3 as two triangles (v0 v1 v2,
// v1 v2 v3), its texels read out of the texture page and CLUT the packet names from the sheet the
// loader uploaded (`FxVram`), affinely interpolated. The texel rule: 0x0000 is transparent, a texel
// with the STP bit is blended by the tpage's mode (0: B/2 + F/2, 1: B + F, 2: B - F, 3: B + F/4), a
// texel without it is drawn opaque.
//
// Placement: the 384 x 240 screen of the original onto the viewport through the same projection the
// scene uses (race_scene.h: H = 237, the camera's second row scaled by 3412/4096, OriginalVerticalFov),
// so a packet lands on the picture where the original's does, 4:3 or wide.
//
// OURS, named: the depth. The console sorts a packet into its ordering table by OTZ; the renderer has
// a depth buffer instead, so every quad is drawn at its OTZ depth (average camera-space z) pulled 0.5
// world units toward the eye, depth-tested against the scene and not written.
//
// The pixel: the GPU covers and samples pixel (x, y) at its integer corner, GL at its centre - every vertex is
// placed half a console pixel right and down (`RRJB_FX_PIXEL=centre` the control).
#include "render/gl_api.h"
#include "render/mat4.h"
#include "game/fx_runtime.h"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace rr::render {

// Where one view's packets sit on the 384 x 240 draw area: the GTE offset the pass projected with
// (SLUS 0x80011C4C: 192 / 120 for one player, the split rectangle's centre for two -
// split_view.h SplitViewRect) and half the rectangle's height (the view's vertical field).
// (At namespace scope, as FxDraw::Place: clang refuses a nested struct's default member initializers in
// a default argument of the enclosing class, the Quest build's compiler.)
struct FxPlace {
    uint32_t view = 0;
    float cx = 192.0f, cy = 120.0f, halfH = 120.0f;
};

class FxDraw {
public:
    // Builds the program. A GL context must be current.
    bool Init();
    using Place = FxPlace;
    // Draws every drawable packet of `place.view`; the viewport is that view's picture. Returns the number drawn.
    size_t Draw(const std::vector<rr::game::FxPacket>& packets, const rr::game::FxVram& vram, float viewAspect,
                float nearZ, float farZ, const Place& place = Place{});
    // OURS: the same packets for a camera that is not the console's (a VR eye, the
    // head camera) - each quad taken back into the world through the console camera the pass projected it with
    // (`camEye`, `camRows`: the render record's eye and its three rows, normalised - right, down, ahead - and the GTE
    // offset (ofx, ofy)): its depth z = OTZ * 4 / 64 world units, a corner (SX, SY) at X = (SX - ofx) z / H and
    // Y = (SY - ofy) z / (H 3412/4096) on the camera's plane; the quad's centre is placed in the world there and its
    // corners' offsets from that centre are laid along the drawing view's own right and up (`view`: its LookAt), so the
    // sprite keeps the size and shape the console gave it and faces the eye - a billboard at the effect's world
    // position. Depth-tested against the scene, not written; the texel rule and the blend are Draw's. Returns the
    // number drawn. What the console camera did not see was never emitted by the pass (not drawn here either).
    size_t DrawWorld(const std::vector<rr::game::FxPacket>& packets, const rr::game::FxVram& vram, uint32_t view,
                     const float camEye[3], const float camRows[3][3], float ofx, float ofy, const Mat4& viewMatrix,
                     const Mat4& viewProj);
    // The texel the renderer samples: 0 transparent, else 0xAARRGGBB with A = 0xFF for STP, 0x80 without.
    static uint32_t Texel(const rr::game::FxVram& vram, uint16_t tpage, uint16_t clut, int u, int v);
    // The screen point (x, y) of the original's 384 x 240 frame in normalised device coordinates.
    static void ToNdc(float sx, float sy, float viewAspect, float& x, float& y);
    // The same for a view projected about (cx, cy) with a field of 2 halfH lines.
    static void ToNdc(float sx, float sy, float viewAspect, const Place& place, float& x, float& y);
    // The ordering-table order (race_scene.h SetOtOrder): each packet at the slot its depth (OTZ * 4)
    // maps to, linked after the whole scene (the effect pass runs after the draw cycle), instead of a perspective depth.
    void SetOt(bool on, int nearOffset, int shift, int maxSlot, float base) {
        otOn_ = on;
        otNear_ = nearOffset;
        otShift_ = shift;
        otMax_ = maxSlot;
        otBase_ = base;
        otTableOf_ = nullptr;
    }
    // Per-cell tables: SLUS 0x800674D4 runs the effect pass 0x80067770 per CELL of each of the frame's
    // two tables - after the table's models 0x80067690, before its cells - so a packet is linked into the table of the
    // cell its entity is filed in (+0xB0), in link rank `rank` of its slot (between the models' and the cells').
    // `tableOf(entity)` gives 1 or 2, 0 for SetOt's single table, -1 for none (not drawn).
    struct OtTable {
        int nearOffset = 0, shift = 5, maxSlot = 0x512;
        float base = 0.0f;
    };
    void SetOtTables(const OtTable& first, const OtTable& second, std::function<int(uint32_t)> tableOf, int rank) {
        otTables_[0] = first;
        otTables_[1] = second;
        otTableOf_ = std::move(tableOf);
        otRank_ = rank;
    }

private:
    bool otOn_ = false;
    int otNear_ = 0, otShift_ = 5, otMax_ = 0x512;
    float otBase_ = 0.0f;
    OtTable otTables_[2];
    std::function<int(uint32_t)> otTableOf_;
    int otRank_ = 1023;

public:
    // Packets drawn in the first table, the second, SetOt's single one, and left out (their cell in neither).
    size_t otTableCounts[4] = {0, 0, 0, 0};

private:
    GLuint Page(const rr::game::FxVram& vram, uint16_t tpage, uint16_t clut);
    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    GLuint worldProgram_ = 0, worldVao_ = 0, worldVbo_ = 0; // DrawWorld
    GLint worldViewProjLoc_ = -1, worldTexLoc_ = -1, worldColourLoc_ = -1, worldRawLoc_ = -1, worldPassLoc_ = -1,
          worldAbrLoc_ = -1;
    GLint texLoc_ = -1, colourLoc_ = -1, rawLoc_ = -1, passLoc_ = -1, abrLoc_ = -1;
    std::map<uint32_t, GLuint> pages_;
};

} // namespace rr::render
