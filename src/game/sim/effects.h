#pragma once
// The effect records' per-frame pass - update, ageing, freeing and the draw - and the pieces around
// it, transcribed from our own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_fx.inc). The spawners and the record
// helpers are spine.h's; the record layout is theirs:
//
//   w0      bits 0..5 the next record (signed, 63 = end), 6..9 the state (1 spark, 2 spray, 3 part
//           streak, 4 crash debris, 5 police light, 6 weapon trail, 7 burst; 0 = free), 10..13 the
//           sub-state (4 = to be freed), 14..21 the kind
//   +0x04   the current world point (s32, 1/64 world unit: the entity's +0xB8 >> 10)
//   +0x14   the drawn point, +0x24 its step per frame
//   +0x30   the race clock at spawn, +0x34 the lifetime (race-clock units)
//   +0x38   s16 spin angle, +0x3A s16 spin rate, +0x3C tag, +0x3D wobble on, +0x3E u16 base size
//   +0x40   wobble amplitude, +0x44 s16[3] the point in camera space (this frame's draw)
//   +0x4C   u16[2] two model vertices (crash debris), +0x50 s32[4] the model-draw capture
//   +0x60   frame period / sprite, +0x61 blink phase, +0x64 last frame step, +0x68 u8[4] frames,
//   +0x6C   the current frame, +0x6D frame count
//
//   SLUS 0x8002F17C RenderCamera      the render camera *(0x8005AEC0)[view] from view record `view`
//   SLUS 0x80028E8C EffectToView      a world point to camera space (GTE MVMVA); leaves RT = I, TR = 0
//   SLUS 0x80028C78 EffectFromModel   a model-draw camera-space vertex back to world (GTE MVMVA)
//   SLUS 0x80029048 EffectQuadSpin    a square of half-size `size` turned by `angle`, in camera space
//   SLUS 0x80029174 EffectQuadAround  a 26-unit-wide streak from a camera-space point to a world point
//   SLUS 0x8002926C EffectEmitQuad    RTPS + RTPT + AVSZ4, one POLY_FT4 into the ordering table
//   SLUS 0x800295AC EffectEmitDebris  the same with a sub-rectangle of the sprite (crash debris)
//   SLUS 0x8002A2C8 EffectDraw        the per-state draw
//   SLUS 0x8002AB14 EffectDrawLight   the police light (state 5): a screen-space quad
//   SLUS 0x8002A738 EffectFree        the budget decrement, the free and the unlink
//   SLUS 0x8002A8E4 EffectBlink       the police light's blink clock
//   SLUS 0x8002A974 EffectLight       the police light's update (two siren callees)
//   SLUS 0x80029D88 EffectUpdate      the per-state update: follow, drift, wobble, age
//   SLUS 0x8002823C EffectPass        one entity's chain: update, free or draw each record; recurses
//                                     into the two child entities at +0x38 / +0x40
//   SLUS 0x8002990C EffectCapture     a record's point out of the model draw's vertex buffers
//   SLUS 0x80029CA4 EffectCaptureChain  ... over an entity's chain (called by the model draw 0x800251E4)
//   SLUS 0x8002847C EffectFreeChain   every record of an entity's chain freed
//   SLUS 0x80027258 SurfaceFx         the spark spawner (state 1) the wall and bike contacts call
//
// THE GTE is not a register file here: `FxGte` holds the registers these functions read (RT, TR, the
// projection constants) and the SZ FIFO AVSZ4 averages, and every store the originals make to them is
// made to it in the original's order. The arithmetic is the hardware's as src\interp\gte.cpp models it
// (44-bit MAC wrap, IR and SXY saturation, the UNR divide) - the interpreter is not linked.
//
// Memory model: road_query.h's `GuestRam` (an address port); a fault or an unported arm sets
// `FxEnv::refused` and the caller fails the call. Buffers the original keeps in its own stack frame
// are host locals.
#include <cstdint>
#include <string>

#include "game/sim/road_query.h"
#include "game/sim/spine.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the addresses
constexpr uint32_t kFxRenderCamPtrs  = 0x8005AEC0; // gp+0x234: -> the render camera of view 0 / 1
constexpr uint32_t kFxViewRecords    = 0x800CD898; // View[2], stride 1132
constexpr uint32_t kFxViewStride     = 1132;
constexpr uint32_t kFxJitterClock    = 0x800D8074; // the pass's wobble counter, 0..4096
constexpr uint32_t kFxSpriteTable    = 0x800D4270; // 11 x 28-byte sprite descriptors (RASHCDI 0x80061FAC)
constexpr uint32_t kFxSpriteBytes    = 28;
constexpr uint32_t kFxPacketHeapPtr  = 0x8005B470; // -> the frame record: +0x108 the OT, +0x10C next free
constexpr uint32_t kFxPacketEnd      = 0x8005B4D0; // the packet heap's end
constexpr uint32_t kFxOtLength       = 0x8005ADFC; // entries of the ordering table
constexpr uint32_t kFxModelVerts     = 0x8005ACB0; // -> the model draw's camera-space vertices, 16 bytes each
constexpr uint32_t kFxModelScreen    = 0x8005ACB4; // -> the model draw's screen points, 4 bytes each
constexpr uint32_t kFxSizeTable      = 0x800538B0; // u16 by (s8)entity[+0x216]: the spray's CLUT index
constexpr uint32_t kFxDebrisUv       = 0x80053858; // 8-byte sub-rectangles of the debris sprite
constexpr uint32_t kFxLightScale     = 0x80053250; // s32: the police light's size reference
constexpr uint32_t kFxLightColour    = 0x800536C4; // u32 by the blink phase
constexpr uint32_t kFxLightPeriod    = 0x800536D0; // u32 by the blink phase
constexpr uint32_t kFxLightParts     = 0x80053728; // (s8 poly, s8 part set) by kind and level of detail
constexpr uint32_t kFxPartSets       = 0x800536F0; // 4 bytes per set (spine.h kCrashFxPartTable)
constexpr uint32_t kFxPartnerOffset  = 0x800536B8; // the spark offset on the partner (two-rider bikes)
constexpr uint32_t kFxSinCos         = 0x8005624C; // {s16 sin, s16 cos}[4096]
constexpr uint32_t kFxStanceTable    = 0x800541D4; // 8-byte stance records (+2 the category)

constexpr uint32_t kFxSurfaceFn    = 0x80027258; // SLUS, SurfaceFx below (the contact pass asks for it)
constexpr uint32_t kFxSirenStopFn  = 0x8001836C; // SLUS, the siren voice stop (sound, not ported)
constexpr uint32_t kFxSirenStartFn = 0x800182B0; // SLUS, the siren voice start (sound, not ported)
constexpr uint32_t kFxHeapFlushFn  = 0x80021C98; // SLUS, the packet heap's overflow arm (not ported)

// ---------------------------------------------------------------------------- the GTE
struct FxGte {
    int16_t rt[9] = {};  // RT11..RT33
    int32_t tr[3] = {};  // TRX..TRZ
    int32_t ofx = 0, ofy = 0;
    uint16_t h = 0;
    int16_t dqa = 0;
    int32_t dqb = 0;
    int16_t zsf4 = 0;
    uint16_t sz[4] = {}; // SZ0..SZ3
    // The control registers cop2r32..63 as a savestate stores them.
    static FxGte FromControl(const uint32_t cr[32]);
    // MVMVA sf = 1, lm = 0, mx = RT, v = V0, cv = TR: MAC1..3.
    void Mvmva(const int16_t v[3], int32_t mac[3]) const;
    // RTPS (sf = 1, lm = 0): pushes SZ3 and returns the SXY2 word.
    uint32_t Rtps(const int16_t v[3]);
    // AVSZ4: OTZ.
    uint16_t Avsz4() const;
    void SetRt(const int16_t m[9]) { for (int k = 0; k < 9; ++k) rt[k] = m[k]; }
    void Identity() {
        for (int k = 0; k < 9; ++k) rt[k] = (k % 4 == 0) ? int16_t{4096} : int16_t{0};
        tr[0] = tr[1] = tr[2] = 0;
    }
};

// ---------------------------------------------------------------------------- the environment
// The two siren callees of the police light (sound; the bench has the oracle run them, the product
// names them as not run).
struct FxCallees {
    virtual ~FxCallees() = default;
    virtual bool SirenStop(uint32_t handle) = 0;  // SLUS 0x8001836C
    virtual bool SirenStart(uint32_t handle) = 0; // SLUS 0x800182B0
};
// Told of every packet an emitter links, with the depth it was sorted by (OTZ * 4 for the GTE quads,
// the captured vertex depth for the police light) - an observer only, nothing flows back.
struct FxPacketSink {
    virtual ~FxPacketSink() = default;
    virtual void Packet(uint32_t address, int32_t depth, uint32_t function) = 0;
};
struct FxEnv {
    FxGte gte;
    SpineIo io;                   // the root counter SurfaceFx's jitter reads
    FxCallees* callees = nullptr;
    FxPacketSink* sink = nullptr;
    uint32_t drawing = 0;         // the record EffectDraw / EffectDrawLight is drawing (for the sink only)
    bool refused = false;
    std::string why;
    void Refuse(const char* what) {
        if (!refused) why = what;
        refused = true;
    }
};

// ---------------------------------------------------------------------------- the functions
void RenderCamera(GuestRam& g, uint32_t view);
// `pos` is read as the low halves of three s32 (the original's `lhu` at +0/+4/+8).
void EffectToView(GuestRam& g, FxEnv& env, const int32_t pos[3], int16_t out[3], uint32_t view);
void EffectFromModel(GuestRam& g, FxEnv& env, uint32_t e, int32_t out[3], const int32_t in[3], uint32_t view);
// A quad is four 8-byte SVECTORs as the GTE loads them: q[4k .. 4k+2], q[4k+3] unused.
void EffectQuadSpin(GuestRam& g, int16_t q[16], const int16_t centre[3], int32_t angle, uint32_t size);
uint32_t EffectQuadAround(GuestRam& g, FxEnv& env, int16_t q[16], const int32_t world[3], const int16_t viewPt[3],
                          uint32_t view);
void EffectEmitQuad(GuestRam& g, FxEnv& env, const int16_t q[16], uint32_t flags, uint32_t sprite, uint32_t clut,
                    uint32_t colour);
void EffectEmitDebris(GuestRam& g, FxEnv& env, const int16_t q[16], uint32_t flags, uint32_t sprite, uint32_t frame);
// `flags` is the pass's per-entity word (the walker's sp+24): bit 2 = the wheel streak drawn.
void EffectDraw(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec, uint32_t age, uint32_t view, uint32_t& flags);
void EffectDrawLight(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec);
void EffectFree(GuestRam& g, uint32_t e, uint32_t prev, uint32_t rec, uint32_t next);
void EffectBlink(GuestRam& g, uint32_t age, uint32_t rec);
uint32_t EffectLight(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec, uint32_t age, uint32_t view);
void EffectUpdate(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec, uint32_t view, uint32_t age);
void EffectPass(GuestRam& g, FxEnv& env, uint32_t e, uint32_t view);
void EffectCapture(GuestRam& g, uint32_t e, uint32_t rec);
void EffectCaptureChain(GuestRam& g, uint32_t e);
void EffectFreeChain(GuestRam& g, uint32_t e);
void SurfaceFx(GuestRam& g, FxEnv& env, uint32_t e, uint32_t kind);

} // namespace rr::sim
