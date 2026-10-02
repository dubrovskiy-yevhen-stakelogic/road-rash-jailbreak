#pragma once
// The model draw's geometry half - the visibility test, the per-part transforms and the vertex
// buffers the effect capture reads, transcribed from our own disassembly
// of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_model.inc).
//
//   SLUS    0x800220A4 ModelVerts     RTPS over a part's vertices: camera-space MAC words (16 bytes a
//                                     vertex) to *(0x8005ACB0), SXY to *(0x8005ACB4), clip flags to
//                                     *(0x8005ACB8), all indexed by the model's own vertex numbers
//   SLUS    0x8001FD24 EulerMatrix    RotMatrix-style 3x3 of (x, y, z) angles (the wheels' spin)
//   SLUS    0x8004D014 ScaleMatrix    the columns of a 3x3 scaled by three 4.12 factors
//   RASHCDG 0x80066A84 SeatVertex     the model vertex a seated child is placed on (3 / 5 / 6 parts)
//   RASHCDG 0x80066B98 ChildPlace     a seated child's position and its seat matrix +0x68
//   RASHCDG 0x80066EC4 BikeParts      the wheels' matrices, the bike's attachment program, its seats
//   RASHCDG 0x80067064 AttachWalk     an attachment program: the matrix stack at *(0x1F8002BC), one
//                                     ModelVerts per part, the seat copies (code-3 words)
//   RASHCDG 0x8006745C RiderParts     program 0 / 1 / 2 by the part count (17 / 12 / 4)
//   RASHCDG 0x80066B28 ObjectParts    program 6 / 7 (4 / 3 parts), or only the stack pop
//   RASHCDG 0x80067AC4 ModelVisible   the per-view test: LODs, the shadow bit, the object's camera
//                                     matrix +0x88 and position +0x50, the draw list *(0x8005B280)
//   RASHCDG 0x80068468 ModelDraw      part 0 through the object's camera matrix, the per-normal light
//                                     bytes 0x1F800140.., the parts, the primitives (0x800251E4, a
//                                     callee here), then the children (visibility + draw)
//
// THE GTE is not a register file here: `ModelGte` holds what these functions load (RT, TR, row 1 of
// the colour matrix) and the projection constants, and the one piece of state that crosses calls
// (SXY2, stored by ModelVerts' pipelined first iteration and by its tail for a one-vertex part).
// The arithmetic is src\interp\gte.cpp's (44-bit MAC wrap, IR / SXY / SZ saturation, the UNR divide);
// the interpreter is not linked.
//
// Memory model: road_query.h's `GuestRam` over RAM and the scratchpad (the matrix stack, the light
// bytes). A load or store the console would not survive is not performed; the view records it and the
// caller fails the call.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {
struct FxEnv; // effects.h
}

namespace rr::sim::model {

// ---------------------------------------------------------------------------- the addresses
constexpr uint32_t kVertsPtr      = 0x8005ACB0; // -> camera-space vertices, 16 bytes each (MAC1..3)
constexpr uint32_t kScreenPtr     = 0x8005ACB4; // -> SXY, 4 bytes each
constexpr uint32_t kFlagsPtr      = 0x8005ACB8; // -> clip flags, 1 byte each
constexpr uint32_t kStackPtr      = 0x1F8002BC; // -> the current 32-byte matrix-stack entry (RT, TR)
constexpr uint32_t kLightBytes    = 0x1F800140; // u8 per model normal, 0..31
constexpr uint32_t kRenderCamPtrs = 0x8005AEC0; // -> the render camera of view 0 / 1 (+0x5C RT, +0x10 eye)
constexpr uint32_t kDrawList      = 0x8005B280; // the draw list head, linked through +0xA8
constexpr uint32_t kDrawRanges    = 0x800CC6A4; // s32 by DOD3 kind: the draw distance (RASHCDG data)
constexpr uint32_t kPlayerStack   = 0x800CCD80; // 32 bytes: 0x80066EC4's copy for a player's bike
constexpr uint32_t kProgWords     = 0x800CC790; // the attachment programs' command words (rmd3.md 9.2)
constexpr uint32_t kProgRecords   = 0x800CC854; // {u8 first word, u8 link count, u8 passes}[8]
constexpr uint32_t kLightVector   = 0x80052364; // SVECTOR, the level's light
constexpr uint32_t kSinCos        = 0x8005624C; // {s16 sin, s16 cos}[4096]
constexpr uint32_t kEmitFn        = 0x800251E4; // SLUS, the primitive emitter (a callee here)

// ---------------------------------------------------------------------------- the GTE
struct ModelGte {
    int16_t rt[9] = {};
    int32_t tr[3] = {};
    int16_t l1[3] = {};  // LCM row 1 (cr16 low / high, cr17 low)
    int32_t ofx = 0, ofy = 0;
    uint16_t h = 0;
    uint32_t sxy2 = 0;   // cop2r14
    // cop2r32..63 and cop2r0..31 as a savestate stores them.
    static ModelGte From(const uint32_t cr[32], const uint32_t dr[32]);
    void LoadRt(GuestRam& g, uint32_t a); // ctc2 $0..$4 from five words at `a`
    void LoadTr(GuestRam& g, uint32_t a); // ctc2 $5..$7 from three words at `a`
    // MVMVA 0x49E012: RT x IR, sf 1, lm 0, no translation; returns IR1..3.
    void MulIr(const int16_t in[3], int16_t out[3]) const;
    // MVMVA 0x486012: RT x V0, sf 1, no translation; returns MAC1..3.
    void MulV0(const int16_t v[3], int32_t mac[3]) const;
    // MVMVA 0x45E012: LCM x IR, sf 0; returns MAC1 (row 1).
    int32_t LightRow(const int16_t ir[3]) const;
    // RTPS 0x180001 (sf 1, lm 0): MAC1..3 into `mac`, pushes SXY; returns the new SXY2.
    uint32_t Rtps(const int16_t v[3], int32_t mac[3]);
};

// ---------------------------------------------------------------------------- the callees
// The primitive emitter SLUS 0x800251E4(obj, view) the model draw calls between the parts and the
// children. The bench has the oracle run it; the product serves its capture head (model_runtime.h).
struct DrawCallees {
    virtual ~DrawCallees() = default;
    virtual bool Emit(uint32_t obj, uint32_t view) = 0;
};

// ---------------------------------------------------------------------------- the functions
void ModelVerts(GuestRam& g, ModelGte& gte, uint32_t entry, uint32_t count, uint32_t verts, uint32_t cam,
                uint32_t screen, uint32_t flags);
void EulerMatrix(GuestRam& g, uint32_t x, uint32_t y, uint32_t z, uint32_t dst);
void ScaleMatrix(GuestRam& g, uint32_t m, const int32_t s[3]);
uint32_t SeatVertex(GuestRam& g, uint32_t obj);
void ChildPlace(GuestRam& g, ModelGte& gte, uint32_t obj, uint32_t k);
void AttachWalk(GuestRam& g, ModelGte& gte, uint32_t prog, uint32_t obj);
void BikeParts(GuestRam& g, ModelGte& gte, uint32_t obj);
void RiderParts(GuestRam& g, ModelGte& gte, uint32_t obj);
void ObjectParts(GuestRam& g, ModelGte& gte, uint32_t obj);
uint32_t ModelVisible(GuestRam& g, ModelGte& gte, uint32_t obj, uint32_t view);
// RASHCDG 0x800667C4 (GameFrame step 4), a leaf: the LOD of each of `views` views out of the LOD
// table +0x64 ({far, near} s32 pairs) and the view distance +0x2C + 4 view, into +0x0A + view (clamped
// to the registry's LOD count - 1); a rider / pedestrian (kind 1 / 4) sets its animation object's
// part mask +0x6E0 from 0x80052390 and its interpolate bit (+0x24 bit 2); +0x09 bit 3 cleared.
void ModelLod(GuestRam& g, uint32_t obj, uint32_t views);
// False when the emitter callee refused or the view faulted.
bool ModelDraw(GuestRam& g, ModelGte& gte, uint32_t obj, uint32_t view, DrawCallees& callees);

// RASHCDG 0x800674D4 RenderModels(cells, count, view): the scratchpad's depth ranges and texture-page
// words, then for each cell index of `cells` (s32[count]) the model draw of every draw-list entry in
// that cell (0x80067690 -> ModelDraw), then for each cell the effect pass of those entries (0x80067770
// -> EffectPass SLUS 0x8002823C, the list's head unlinked as it goes). `fx` is the effect pass's
// environment (effects.h). False when a callee refused or the view faulted.
constexpr uint32_t kCellRecords = 0x800D87E8; // 112-byte records, +8 the cell pointer
bool RenderModels(GuestRam& g, ModelGte& gte, rr::sim::FxEnv& fx, uint32_t cells, int32_t count, uint32_t view,
                  DrawCallees& callees);

} // namespace rr::sim::model
