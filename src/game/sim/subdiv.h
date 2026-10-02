#pragma once
// The cell draw's polygon subdividers (docs\formats\scene_cell.md 13.8), transcribed from our
// own disassembly of
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_subdiv.inc):
//
//   0x8006929C SubTri(base, corners, level)    band 1 fine near triangles (0x8006A630): 3 edge midpoints,
//                                              4 children through the table 0x800CCA30, GT3 leaves
//   0x80069784 SubQuad(base, corners, level)   band 1 fine near quads (0x8006A630): 4 edge midpoints and the
//                                              centre, 4 children through 0x800CCA40, GT4 leaves
//   0x80069CF0 SubRoad(base, corners, level)   band 2 near lane strips (0x8006E474 / 0x8006F5D0, level 5):
//                                              0x80069784 with the nudge rounded by 0x40 (not 0x80) and the
//                                              near-plane refinement stopping at base 14 (not 19)
//   0x8006A25C SubLine(base, corners, colour)  the lane lines (F4): 2 midpoints (edges 1-2 and 3-0), 2 children
//                                              through 0x800CCA50, refined only while a piece reaches below
//                                              line 240 (outcode 4) and base < 14
//
// The vertex records are 32 bytes at 0x1F800100 + 32 i: +0 x, +4 y, +8 z (the cell view space of MVMVA
// LLM x V + BK, sf 1, << 8), +0x10 the texel (u | v << 8), +0x14 the colour-table index, +0x18 the screen
// SXY, +0x1C the colour 0x800D4CA8[index] with the outcode byte on top (+0x1F). A new vertex is the
// midpoint of two (x, y, z and the index by `(a + b) >> 1`, the texel by a carry-safe floor average), an
// EDGE midpoint of the triangle / quad / road subdividers is then nudged by n = (z + r) >> 8 along x or y
// away from the edge's screen direction (about a pixel: it closes the cracks between pieces), and the
// point goes through RTPS with TR = (x, y, z) >> 5 (RT is 0 in the cell draw: 0x8001064C moved the view
// into LLM / BK). The outcode byte: 0x80 / 0x40 / 0x20 for z < 0xC800 / < 0x19000 / farther, 0x10 for
// z < 0x2800 (the near plane), 8 above the screen, 4 below line 240, 2 left of column 0, 1 right of 384.
// A child whose four (three) outcodes share one of the low five bits is dropped; it is refined again
// while its OR has a bit at or above `level + 1` (the depth bits: level 7 always stops) or, below the
// function's base limit, while it touches the near plane; otherwise it is written to the packet heap
// *(0x1F800020) chained after *(0x1F800028), the heap's overflow SLUS 0x80021C98 a callee.
//
// `Subdivider` works on its own copy of the records and hands every new record and every leaf to `Env`:
// the bench writes them through to the guest (records and packets), the product (race_scene.cpp) turns
// the leaves into its own affine triangles. It fails (false) where the console would fault: a record
// past the scratchpad or a callee that refused.
#include <cstdint>

namespace rr::sim::subdiv {

constexpr uint32_t kSubTriFn = 0x8006929C, kSubQuadFn = 0x80069784, kSubRoadFn = 0x80069CF0,
                   kSubLineFn = 0x8006A25C;
constexpr uint32_t kRecords = 0x1F800100;   // 32-byte vertex records
constexpr uint32_t kHeapNext = 0x1F800020;  // the packet heap's next free word
constexpr uint32_t kHeapPrev = 0x1F800028;  // the packet the next one chains after
constexpr uint32_t kUvHigh0 = 0x1F800034;   // ORed into the first vertex's texel word (the CLUT)
constexpr uint32_t kUvHigh1 = 0x1F800038;   // ORed into the second's (the texture page)
constexpr uint32_t kHeapEnd = 0x8005B4D0;   // the heap's end
constexpr uint32_t kColourTable = 0x800D4CA8;
constexpr uint32_t kTriKids = 0x800CCA30, kQuadKids = 0x800CCA40, kLineKids = 0x800CCA50;
constexpr uint32_t kHeapFullFn = 0x80021C98; // SLUS, the heap's overflow arm (a callee)
constexpr int kMaxRecords = 24;             // 0x1F800100 .. 0x1F8003FF: the scratchpad ends there

struct Rec {
    int32_t x = 0, y = 0, z = 0;
    uint32_t uv = 0;
    int32_t shade = 0;
    uint32_t sxy = 0;
    uint32_t rgb = 0; // the outcode byte in bits 24..31
    uint8_t Flags() const { return static_cast<uint8_t>(rgb >> 24); }
};

// The child tables (RASHCDG data): a word per child, nibbles naming the vertices (corners first, then the
// new records) in the order A = w >> 12, B = (w >> 8) & 15, D = (w >> 4) & 15, C = w & 15 (a triangle's:
// A = w >> 8, B = (w >> 4) & 15, C = w & 15).
struct Kids {
    uint32_t tri[4] = {};
    uint32_t quad[4] = {};
    uint32_t line[2] = {};
};

struct Env {
    virtual ~Env() = default;
    // RTPS 0x198001 with TR = (tx, ty, tz): the new SXY2.
    virtual uint32_t Project(int32_t tx, int32_t ty, int32_t tz) = 0;
    // The colour table 0x800D4CA8[index] (index 0..255).
    virtual uint32_t Colour(uint32_t index) = 0;
    // Record `i` was written; `uv` false for SubLine, which leaves +0x10 alone.
    virtual bool Stored(int i, const Rec& r, bool uv) = 0;
    // Leaves, in packet vertex order (A, B, C, D).
    virtual bool Gt4(const Rec* recs, const int v[4]) = 0;
    virtual bool Gt3(const Rec* recs, const int v[3]) = 0;
    virtual bool F4(const Rec* recs, const int v[4], uint32_t colour) = 0;
};

// The outcode byte of a projected record (the same arithmetic in all four and in their callers).
uint8_t Outcode(uint32_t sxy, int32_t z);

class Subdivider {
public:
    Subdivider(Env& env, const Kids& kids) : env_(env), kids_(kids) {}
    Rec rec[kMaxRecords] = {};
    bool Tri(int base, uint32_t corners, int32_t level);                 // 0x8006929C
    bool Quad(int base, uint32_t corners, int32_t level);                // 0x80069784
    bool Road(int base, uint32_t corners, int32_t level);                // 0x80069CF0
    bool Line(int base, uint32_t corners, uint32_t colour);              // 0x8006A25C
    int deepest = 0; // the largest record index written (a diagnostic)
    // false leaves out the edge midpoints' nudge (ours, for the product: a depth-buffered rasteriser has no
    // cracks to close, and the nudged point, moved at a constant depth, would stand off the polygon's plane).
    bool nudge = true;

private:
    bool QuadCommon(int base, uint32_t corners, int32_t level, int32_t round, int limit);
    void Midpoint(int dst, int a, int b, int32_t round, bool edge);
    Env& env_;
    const Kids& kids_;
};

} // namespace rr::sim::subdiv
