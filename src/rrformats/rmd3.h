#pragma once
// RMD3 - the model container of Road Rash: Jailbreak (*.GEO).
// Layout established byte-exactly in docs\formats\rmd3.md; this is the authoritative parser
// (the Python probe tools\scout\rmd3.py stays as an independent cross-check).
#include "rrformats/skeleton.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rr {

struct SVector {
    int16_t x = 0, y = 0, z = 0, pad = 0;
};

// One drawn quad. `u[k], v[k]` is the texel of corner `index[k]` - NOT the record's byte order: the
// record holds i1's pair at +0x00 and i0's at +0x04 (rmd3.cpp ParseGeo, from the model emitter).
struct Primitive {
    uint8_t u[4] = {};
    uint8_t v[4] = {};
    // Low bits = the PS1 CLUT id. Bit 15: the primitive is two-sided (the emitter skips its NCLIP,
    // rmd3.h TriangleSoup::Vertex::window[3]); bit 13: a quad (clear: the emitter's triangle arm).
    uint16_t clut = 0;
    uint16_t tpage = 0; // dense small index into the paired .TEX, NOT a raw GPU tpage word
    uint16_t index[4] = {};
};

// The six soup corners of a primitive (BuildTriangleSoup's order). A quad (clut bit 13) goes to the GPU as the GT4
// (i0, i1, i3, i2) (SLUS 0x800251E4), which the GPU draws as (i0, i1, i3) + (i1, i3, i2): the i1-i3 diagonal - it
// decides the fold of a non-planar quad and, affinely mapped, where the texels land.
// A 3-corner primitive (clut bit 13 clear) is ONE triangle: the emitter's arm SLUS 0x80025BDC builds a single FT3 / GT3
// (GP0 0x24 / 0x34) of (i0 uv+4, i1 uv+0, i2 uv+8) and never reads the record's fourth index or the texel at +10 (in
// all 3166 such records of the game both are 0: padding). The captures agree (rmd3.md 3.3): their model
// GT3s carry the (i0, i1, i2) texels, none the (i0, i2, i3) ones. The soup keeps six vertices a primitive (the stride
// every consumer, the OT key rule of shaders.cpp and the captured / GTE paths index by), so the second triangle is
// (i2, i2, i2): zero area, no fragment - and soup vertex 4 is still i2, the key's z2.
inline const int* SoupCorners(const Primitive& prim) {
    static const int quad[6] = {0, 1, 3, 1, 2, 3};
    static const int tri[6] = {0, 1, 2, 2, 2, 2};
    return (prim.clut & 0x2000u) ? quad : tri;
}

// Whether soup triangle `t` (0 or 1) of `prim` is one the console draws (the second of a 3-corner primitive is not).
inline bool SoupTriangleDrawn(const Primitive& prim, int t) { return t == 0 || (prim.clut & 0x2000u) != 0; }

// A sub-mesh owns a contiguous slice of the group's vertex array (a part/bone), but its
// primitives may reference vertices outside that slice - the parts stitch together.
struct SubMesh {
    uint16_t vertCount = 0;
    uint32_t vertBase = 0;
    std::vector<Primitive> prims;
};

struct BoundingBox {
    SVector centre;
    int16_t half[3] = {};
    int16_t radius = 0;
};

// One LOD of a model.
struct ModelGroup {
    // Bit 30 of `flags` marks the groups authored in the large coordinate unit. Within one model the
    // large-unit groups are about 16x bigger than the small-unit ones (measured by `rrtool lodcheck`:
    // 495 objects, median ratio 17.0, range 14.3..19.2). The factor itself comes from the coordinate
    // shift exponent in the same word - see LodFactor below for where the original applies it.
    static constexpr uint32_t kLargeUnitFlag = 0x40000000u;
    bool IsLargeUnit() const { return (flags & kLargeUnitFlag) != 0; }

    uint32_t flags = 0;
    uint32_t radius = 0;
    uint32_t scale = 0; // 4096 in all but three groups game-wide; meaning unconfirmed
    uint32_t slot = 0;
    std::vector<uint8_t> blobA; // meaning unknown
    std::vector<SVector> verts;
    std::vector<SVector> normals;
    std::vector<std::array<uint16_t, 4>> quadsD; // untextured quad list, purpose unknown
    std::vector<uint16_t> vertNormalIndex;
    std::vector<SubMesh> subMeshes;
    BoundingBox bbox;
};

// One RMD3 chunk: a model with its LOD chain.
struct Model {
    uint32_t id = 0;
    std::vector<ModelGroup> groups;
};

// Parses a whole *.GEO file (a chain of RMD3 chunks ending exactly at EOF).
// Throws std::runtime_error on any inconsistency - the format is exact, so a mismatch means
// our understanding is wrong and must not be papered over.
std::vector<Model> ParseGeo(std::span<const uint8_t> data);

// The models of a *.MRO file (the sidecar machines CRUISES1..3 / SPORTS1..3): an EA chunk chain
// (`char tag[4]; u32 size;`, textures.md 1) of a LECT texture and one RMD3 model; every RMD3 chunk is
// parsed as ParseGeo parses one. RASHCDI 0x8005C30C loads it for a player on a sidecar bike
// (RASHCDI 0x8005C45C, rules.md 16). Throws on a chain that does not end exactly at EOF.
std::vector<Model> ParseMro(std::span<const uint8_t> data);

// Triangulated, de-indexed view of one group, ready for a vertex buffer.
struct TriangleSoup {
    struct Vertex {
        float x = 0, y = 0, z = 0;
        float nx = 0, ny = 0, nz = 0;
        float u = 0, v = 0;
        uint16_t clut = 0;
        uint16_t tpage = 0;
        // How the PlayStation GPU colours this vertex. `shade[3]` is the mode: 0 = the texel as it
        // is (models, props - the default, so every existing soup is unchanged), 1 = the texel
        // MODULATED by `shade[0..2]` the way GP0 does it for a non-raw textured polygon
        // (`texel * colour / 128`, 0x80 = 1.0), 2 = an untextured polygon of flat colour `shade[0..2]`.
        uint8_t shade[4] = {128, 128, 128, 0};
        // The GP0(E2) texture window this primitive is drawn under, in texels of the image the
        // renderer binds: `window[2]` is the tile size (0 = no window), `window[0..1]` its origin.
        // Inside a window the hardware keeps only the low bits of the texel coordinate and ORs the
        // tile origin in, so a primitive's UVs are tile-relative and repeat across the tile.
        // `window[3]` is not part of the window: it is 1 when the model primitive is ONE-SIDED. The
        // model emitter SLUS 0x800251E4 skips the GTE NCLIP only for a primitive whose record word 0
        // is negative (`bltz` at 0x80025940: `clut` bit 15); every other primitive is dropped when
        // NCLIP over its corners (i0, i1, i2) is negative (0x80025970). A renderer that honours the
        // flag (race_scene.cpp, the props) shows one face of a front/back pair, as the console does.
        uint8_t window[4] = {0, 0, 0, 0};
        // The ordering-table key rule of the primitive this vertex belongs to (render/race_scene.h SetOtOrder):
        // -1 a model primitive (six soup vertices, SLUS 0x800251E4's
        // (z0 + z1 + 2 z2) / 4), <= -2 a scene-cell primitive of the static soup (-(2 + back + 8 * size): the
        // largest view depth of its `size` soup vertices starting `back` before this one - RASHCDG 0x8006D350 and
        // its siblings), >= 0 the key itself in cell units (1/64 world unit), set by a per-frame builder.
        float ot = -1.0f;
    };
    std::vector<Vertex> vertices; // 3 per triangle
};

TriangleSoup BuildTriangleSoup(const ModelGroup& group);

// ------------------------------------------------------------------ part assembly

// Multiplier that puts a group's vertices into the finest unit. `DOD3+0x0E` bits 12..15 is a
// coordinate shift exponent and `RASHCDG 0x80066CDC` computes `shift = 4 - exponent`.
//
// NOTE, established by the frame trace: the original does NOT apply
// that shift to the vertices it draws - `0x80022110` feeds them to the GTE straight out of the
// .GEO with a unit-scale matrix, for exponent-0 and exponent-4 groups alike. The shift is a
// PLACEMENT-time operation. We fold it into the mesh anyway so that groups of different exponents
// come out in one comparable unit, which is a rendering convenience, not the original's behaviour.
int LodFactor(const ModelGroup& group);

// The result of standing a model up: one origin per sub-mesh, plus how well it worked.
struct Assembly {
    bool assembled = false;
    std::vector<SVector> origins;  // one per sub-mesh; part 0 is always at (0,0,0)
    size_t programIndex = 0;       // which attachment program was used
    int32_t boxError = 0;          // max |assembled AABB - authored BBD3 box| over the six planes
};

// Chooses the attachment program and computes the part origins.
//
// HEURISTIC, and deliberately visible as one: the original passes the program index into
// 0x80067064 as an argument we have not located, and several programs describe the same part
// count, so we try every candidate and keep the one whose assembled bounding box best reproduces
// the authored BBD3 box. BBD3 turns out to be the box of the ASSEMBLED model, which makes it a
// free oracle - across the game's 43 distinct multi-part groups the best candidate lands within a
// few units while a wrong assembly is off by hundreds.
Assembly AssembleGroup(const ModelGroup& group, const SkeletonTable& skeleton);

// Triangles with each part moved to its assembled origin and the LOD shift applied.
TriangleSoup BuildAssembledTriangleSoup(const ModelGroup& group, const Assembly& assembly);

} // namespace rr
