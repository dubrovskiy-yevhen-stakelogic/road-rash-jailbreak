#pragma once
// The model shadow of a one-player race, transcribed from our own
// disassembly of the player's own image:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by the `look_shadow` row of `rrverify phys` (tools\rrverify\rows_look.inc).
//
//   SLUS 0x80025EE0 Shadow(obj, view, ground, normal) - called from the tail of the primitive emitter
//   0x800251E4 when the object's +0x24 bit 7 is set (ModelVisible 0x80067AC4 sets it) and game_state+0x30
//   == 1 (the two-player form 0x80026960 is NOT ported). Once per draw-list entry (0x80067690 sets
//   *(0x800CCD78) = *(0x800CCDA0) = (+0x09 & 0x30) >> 4; each call decrements 0x800CCD78, so only the first
//   call - the entry itself, not its seated children - builds the matrix) it turns the ground normal
//   `normal` (s16[3]), the level light 0x80052364 and the ground point `ground` (s32[3] 16.16, >> 10, minus
//   the render camera's eye +0x1C.. as halfwords) through the render camera *(0x8005AEC0 + 4 view) + 0x5C,
//   takes n.L >> 8 (LCM row 1, gp+0x89C), bends a light more than 45 degrees off the normal to
//   0.7071 (n + unit((n x L) x n)) (the OP twice, Normalize 0x8002E468, Blend16 0x8002EB78), and stores
//   the planar projection along it: RT = (n.L) I - L n^T at 0x800D7FA8, TR = L (n.g) / (n.L) at
//   0x800D7FBC (Scale 0x8002EE50), the reciprocal 2^31 / ((n.L) / 2) at gp+0x89C. Then, for every quad of
//   the object's `quadsD` list (DOD3 +0x2C: a count, u16[4] vertex numbers), the camera-space vertices
//   the model draw left at *(0x8005ACB0) (16 bytes each), >> the DOD3 exponent and scaled by gp+0x89C,
//   go through RTPT / RTPS and AVSZ4 (ZSF4) into one flat semi-transparent quad: E1 0x740 (mode 2,
//   B - F), E6 3 (mask set and tested: a pixel is darkened once), 0x2A | colour 0x80052348, SXY
//   v0 v1 v3 v2, E6 0 - 36 bytes from the packet heap *(0x8005B470)+0x10C, linked into the ordering
//   table +0x108 at the OTZ the depth ranges of the scratchpad 0x1F800000.. map it to.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim::shadow {

constexpr uint32_t kShadowFn    = 0x80025EE0; // SLUS
constexpr uint32_t kShadow2pFn  = 0x80026960; // SLUS, the two-player form (not ported)
constexpr uint32_t kCounter     = 0x800CCD78; // decremented per call
constexpr uint32_t kCounterTop  = 0x800CCDA0; // the draw-list entry's count: equal = build the matrix
constexpr uint32_t kMatrix      = 0x800D7FA8; // RT (5 words), TR at +0x14 (3 words)
constexpr uint32_t kLight       = 0x80052364; // SVECTOR, the level's light
constexpr uint32_t kColour      = 0x80052348; // the flat colour word
constexpr uint32_t kRsqrtPtr    = 0x8005B560; // -> Normalize's reciprocal-square-root table
constexpr uint32_t kHeapLimit   = 0x8005B4D0;
constexpr uint32_t kHeapPtr     = 0x8005B470; // -> the frame record: +0x108 the OT, +0x10C next free
constexpr uint32_t kRenderCams  = 0x8005AEC0;
constexpr uint32_t kGpScale     = 0x89C;      // gp-relative: n.L >> 8, then the reciprocal

// The GTE state the function takes from its caller: the screen offset OFX / OFY (16.16, as cop2r56 / 57),
// H and ZSF4 (every race capture's: 192 / 120 / 237 / 256).
struct Gte {
    int32_t ofx = 192 << 16, ofy = 120 << 16;
    uint16_t h = 237;
    int16_t zsf4 = 256;
};

// Told of each packet linked: its address, the ordering-table index it went to, and the four corners'
// camera-space points (the RTPT / RTPS MAC1..3, 1/64 world unit, in the render camera's frame - its second
// row 3412/4096 long) in the quad's own order q0..q3 (the packet carries them as q0, q1, q3, q2).
struct Sink {
    virtual ~Sink() = default;
    virtual void Packet(uint32_t address, uint32_t otIndex, const int32_t cam[4][3]) = 0;
};

// SLUS 0x80025EE0. False when it refused: the packet heap would overflow (its arm SLUS 0x80021C98 is not
// ported), Normalize's sum of squares overflows (the console's exception), or a guest access faulted.
bool Shadow(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t ground, uint32_t normal,
            Sink* sink = nullptr);

// The call at the tail of the emitter SLUS 0x800251E4 (0x80025E00..0x80025EAC): nothing unless +0x24
// bit 7; the ground point and normal of the object's parent +0x34 (+0x1F8 / +0x20A) or its own (+0x20A,
// and +0xB8 for kind 3, +0xB8 for kind 1 / 4 unless +0x228 bit 30, else +0x1F8); Shadow when
// game_state+0x30 == 1. `w24` is +0x24 as the emitter read it on entry. `twoPlayer` is set (and nothing
// drawn) when the two-player form would run.
bool EmitterShadow(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t w24, Sink* sink, bool& twoPlayer);

// 0x80067690's two stores before a draw-list entry's model draw.
void BeginEntry(GuestRam& g, uint32_t obj);

} // namespace rr::sim::shadow
