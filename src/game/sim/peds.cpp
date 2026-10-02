// The pedestrians (peds.h), line by line from our own listing of RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c) and SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1).
// The addresses beside the statements are the original's instructions.
#include "game/sim/peds.h"

#include "game/sim/ai.h"             // AiProject, Length3
#include "game/sim/coll_util.h"      // the guest wrappers of the SLUS leaves
#include "game/sim/crash.h"          // ScaleTo16 SLUS 0x8002EED8
#include "game/sim/fixed.h"          // FixMul, RatAtan2
#include "game/sim/population.h"     // RoadGate, CursorSeat, RoadWindow, ViewSlot, Attach
#include "game/sim/road_runtime.h"   // the road layer
#include "game/sim/traffic_bind.h"   // ModelBind, PoolRelease, LodSelect
#include "game/sim/traffic_leaves.h" // CarSetup
#include "game/sim/world_pop.h"      // JunctionSlice

namespace rr::sim {
namespace {

using cu::Iabs;
using rc::S;
using rc::U;

constexpr uint32_t kPoolRec2 = 0x800CE4F0; // the pool table's pool-2 record {base, stride, -> live, -> high}
constexpr uint32_t kSinCos = 0x8005624C;
constexpr uint32_t kAtanTable = 0x8005285C;
constexpr uint32_t kBankTable = 0x800CE190;
constexpr uint32_t kAnimDesc = 0x800CE170;
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPlayerBikePtr = 0x8005B38C;

inline uint16_t Neg16(uint16_t v) { return static_cast<uint16_t>(0u - v); }
// FixMul SLUS 0x8001FC90, the ported leaf.
inline int32_t FM(int32_t a, int32_t b) { return FixMul(a, b); }

// The clip id's bank / clip (0x800CAB64..0x800CABB0, 0x800CAC90..0x800CACD8 and three more copies).
uint32_t ClipBank(GuestRam& g, uint32_t id) {
    if (id < 224u) return g.U32(kPedClipMap + 8u * id) & 0xFu;
    return static_cast<uint32_t>(g.U16(kPedClipIds + 2u * (id - 224u)) >> 12);
}
uint32_t ClipNum(GuestRam& g, uint32_t id) {
    if (id < 224u) return (g.U32(kPedClipMap + 8u * id) >> 4) & 0xFFFu;
    return g.U16(kPedClipIds + 2u * (id - 224u)) & 0xFFFu;
}

// The walk turned round (0x800CA6CC.., 0x800CA8B8.., 0x800CAF08..): the direction +0x450 negated, the
// "walking back" flag +0x235 bit 1 toggled, the rows +0x1B0 / +0x1BC rebuilt from it, the distance
// walked +0x230 restarted.
void TurnRound(GuestRam& g, uint32_t e) {
    const uint8_t f = g.U8(e + 565u);
    g.W32(e + 560u, 0);
    g.W16(e + 434u, 0);
    g.W16(e + 450u, Neg16(g.U16(e + 450u)));
    g.W16(e + 452u, Neg16(g.U16(e + 452u)));
    g.W16(e + 454u, Neg16(g.U16(e + 454u)));
    const uint16_t x = g.U16(e + 450u), y = g.U16(e + 452u), z = g.U16(e + 454u);
    g.W8(e + 565u, static_cast<uint8_t>((((f & 2u) == 0u) ? 2u : 0u) | (f & 0xFDu)));
    g.W16(e + 444u, x);
    g.W16(e + 446u, y);
    g.W16(e + 436u, Neg16(x));
    g.W16(e + 448u, z);
    g.W16(e + 432u, z);
}

// The walk's common tail (0x800CA768..0x800CA7AC / 0x800CA944..0x800CA988): the road re-bind, the
// progress scalar, and the pedestrian moved back onto the slice's plane.
void WalkTail(GuestRam& g, uint32_t e, uint32_t F) {
    RoadRuntimeNative road;
    RoadRebindBody(g, e, F, road);                                            // 0x800CA768
    g.W32(e + 324u, U(ProgressScalar(g, e + 172u, F)));                       // 0x800CA770
    const uint32_t sl = g.U32(e + 340u);
    const int32_t d = cu::GProject(g, e + 184u, sl + 8u, sl + 20u);           // 0x800CA78C
    g.W32(F + 16u, U(d));
    MulAddView(g, e + 184u, g.U32(e + 340u) + 8u, S(0u - U(d)), e + 184u);    // 0x800CA7A8
}

bool Seam(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> a, uint32_t sp, uint32_t* v0 = nullptr) {
    return rc::Call(c, fn, a, sp, v0);
}

} // namespace

// ============================================================================ the walk

int32_t PedEdge(GuestRam& g, uint32_t e, uint32_t step) {
    const int32_t a0 = g.S32(e + 388u) >> 20;                                 // 0x800CA448..0x800CA458
    const int32_t s1 = Iabs(S(g.U32(e + 556u) - g.U32(e + 560u)));
    const int32_t a1 = g.S32(e + 344u);
    if (a1 < 0) {                                                             // 0x800CA468
        int32_t s0 = 0;
        if ((a0 & 0xF) == 0) s0 = Iabs(S(g.U32(e + 400u) - U(a1)));           // 0x800CA470..0x800CA490
        if (s0 < s1 && !(g.S32(step) < s0) && cu::GDot(g, g.U32(e + 340u) + 2u, e + 444u) < 0) { // 0x800CA4B8
            g.W32(step, U(s0));
            return 1;
        }
    } else {
        int32_t s0 = 0;
        if ((a0 & 0xF) == 0) s0 = Iabs(S(g.U32(e + 412u) - U(a1)));           // 0x800CA4D0..0x800CA4F0
        if (s0 < s1 && !(g.S32(step) < s0) && cu::GDot(g, g.U32(e + 340u) + 2u, e + 444u) > 0) { // 0x800CA518
            g.W32(step, U(s0));
            return 1;
        }
    }
    if (g.S32(step) < s1) return 0;                                           // 0x800CA530
    g.W32(step, U(s1));
    return 1;
}

bool PedWalkA(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables&) {
    const uint32_t F = sp - 40u;
    g.W32(F + 16u, U(FM(dt, g.S32(e + 480u))));                               // 0x800CA580
    if (!(g.U8(e + 565u) & 4u)) {                                             // 0x800CA598: the walk's set-up
        const uint32_t sl = g.U32(e + 340u);
        g.W16(e + 438u, 0);
        g.W16(e + 440u, 4096);
        g.W16(e + 442u, 0);
        g.W16(e + 444u, g.U16(sl + 2u));                                      // the slice's side row
        g.W16(e + 446u, g.U16(sl + 4u));
        const uint16_t z = g.U16(sl + 6u);
        const uint8_t kind = g.U8(e + 564u);
        g.W16(e + 448u, z);
        if (kind == 0 && g.S32(e + 344u) > 0) {                               // 0x800CA5D8..0x800CA610
            g.W16(e + 444u, Neg16(g.U16(e + 444u)));
            g.W16(e + 448u, Neg16(g.U16(e + 448u)));
            g.W16(e + 446u, Neg16(g.U16(e + 446u)));
        }
        rc::GteOp(g, e + 438u, e + 444u, e + 432u);                           // 0x800CA654: OP (sf 1)
        const uint16_t x = g.U16(e + 444u), y = g.U16(e + 446u), w = g.U16(e + 448u);
        g.W16(e + 450u, x);
        g.W16(e + 452u, y);
        g.W16(e + 454u, w);
        g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) | 4u));
        g.W32(e + 556u, g.U32(e + 412u) - g.U32(e + 344u));                   // the length: to the far edge
    }
    if (PedEdge(g, e, F + 16u) != 0) {                                        // 0x800CA6AC
        MulAddView(g, e + 184u, e + 450u, g.S32(F + 16u), e + 184u);          // 0x800CA6C4
        TurnRound(g, e);                                                      // 0x800CA6CC..0x800CA744
        g.W32(e + 556u, g.U32(e + 412u) - g.U32(e + 400u));
    } else {
        g.W32(e + 560u, U(g.S32(e + 560u) + g.S32(F + 16u)));                 // 0x800CA754..0x800CA764
        MulAddView(g, e + 184u, e + 450u, g.S32(F + 16u), e + 184u);
    }
    WalkTail(g, e, F);
    return !g.Faulted();
}

bool PedWalkB(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables&) {
    const uint32_t F = sp - 40u;
    g.W32(F + 16u, U(FM(dt, g.S32(e + 480u))));                               // 0x800CA7E4
    const uint32_t s2 = g.U32(e + 344u);                                      // 0x800CA7F4: the lateral kept
    if (!(g.U8(e + 565u) & 4u)) {                                             // 0x800CA7FC: the walk's set-up
        const uint32_t sl = g.U32(e + 340u);
        g.W16(e + 438u, 0);
        g.W16(e + 440u, 4096);
        g.W16(e + 442u, 0);
        g.W16(e + 444u, g.U16(sl + 14u));                                     // the slice's forward row, negated
        g.W16(e + 446u, g.U16(sl + 16u));
        const uint16_t z = g.U16(sl + 18u);
        g.W16(e + 434u, 0);
        g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) | 4u));
        g.W16(e + 448u, z);
        g.W16(e + 444u, Neg16(g.U16(e + 444u)));
        g.W16(e + 448u, Neg16(z));
        g.W16(e + 432u, Neg16(z));
        g.W16(e + 454u, Neg16(z));
        g.W16(e + 446u, Neg16(g.U16(e + 446u)));
        const uint16_t x = g.U16(e + 444u), y = g.U16(e + 446u);
        g.W16(e + 436u, Neg16(x));
        g.W16(e + 450u, x);
        g.W16(e + 452u, y);
    }
    if (PedEdge(g, e, F + 16u) != 0) {                                        // 0x800CA898
        MulAddView(g, e + 184u, e + 450u, g.S32(F + 16u), e + 184u);          // 0x800CA8B0
        TurnRound(g, e);                                                      // 0x800CA8B8..0x800CA924
    } else {
        g.W32(e + 560u, U(g.S32(e + 560u) + g.S32(F + 16u)));                 // 0x800CA930..0x800CA940
        MulAddView(g, e + 184u, e + 450u, g.S32(F + 16u), e + 184u);
    }
    WalkTail(g, e, F);                                                        // 0x800CA944..0x800CA988
    MulAddView(g, e + 184u, g.U32(e + 340u) + 2u, S(s2 - g.U32(e + 344u)), e + 184u); // 0x800CA9A0: back to the lane
    const uint32_t sl = g.U32(e + 340u);
    g.W32(e + 344u, s2);
    g.W16(e + 450u, g.U16(sl + 14u));                                         // 0x800CA9B4..0x800CA9DC
    g.W16(e + 452u, g.U16(sl + 16u));
    const uint16_t z = g.U16(sl + 18u);
    g.W16(e + 454u, z);
    if (!(g.U8(e + 565u) & 2u)) {                                             // 0x800CA9D8
        g.W16(e + 450u, Neg16(g.U16(e + 450u)));
        g.W16(e + 454u, Neg16(z));
        g.W16(e + 452u, Neg16(g.U16(e + 452u)));
    }
    const uint16_t x = g.U16(e + 450u), y = g.U16(e + 452u), w = g.U16(e + 454u);
    g.W16(e + 434u, 0);                                                       // 0x800CAA04..0x800CAA28
    g.W16(e + 444u, x);
    g.W16(e + 446u, y);
    g.W16(e + 448u, w);
    g.W16(e + 432u, w);
    g.W16(e + 436u, Neg16(x));
    return !g.Faulted();
}

bool PedStep(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables& t) {
    const uint32_t F = sp - 24u;
    if (g.U8(e + 565u) & 1u) return !g.Faulted();                             // 0x800CAA58
    const uint8_t kind = g.U8(e + 564u);
    if (kind == 1u) return PedWalkA(g, e, dt, F, t);                          // 0x800CAA80
    if (kind == 2u) return PedWalkB(g, e, dt, F, t);                          // 0x800CAA90
    return !g.Faulted();
}

bool PedStart(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t) {
    const uint32_t F = sp - 24u;
    const uint8_t kind = g.U8(e + 564u);
    if (kind != 1u && kind == 2u) return PedWalkB(g, e, 0, F, t);             // 0x800CAAC8
    return PedWalkA(g, e, 0, F, t);                                           // 0x800CAAD8
}

// ============================================================================ the animation machine

bool PedAnim(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 56u;
    const uint32_t s4 = kPedStates;
    const uint32_t s2 = g.U8(e + 553u);                                       // the state
    const uint32_t a0 = g.U32(e + 552u);
    const uint32_t s5 = g.U8(e + 554u);                                       // the side
    uint32_t s1 = g.U16(s4 + 6u * s2);                                        // the state's clip
    const uint32_t s6 = g.U32(kPedClassTable + 16u * g.U8(e + 566u) + 12u);   // the class's rate
    if (a0 & 1u) {                                                            // 0x800CAB5C: start the state's clip
        if (!Seam(c, kPedBankSwitchFn, {g.U32(e + 540u), g.U32(kBankTable + 4u * ClipBank(g, s1))}, F)) return false;
        if (s2 == 0) {                                                        // 0x800CABC8: standing
            s1 = 231;
            if (g.U32(e + 388u) & 1u) {                                       // off the road
                g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) | 1u));
            } else {
                g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) & 0xFEu));
                const uint32_t row = kPedClassTable + 16u * g.U8(e + 566u);
                s1 = g.U16(row + 2u);                                         // the class's idle clip
                g.W32(e + 480u, g.U32(row + 8u));
            }
            if (!Seam(c, kPedBankSwitchFn, {g.U32(e + 540u), g.U32(kBankTable + 4u * ClipBank(g, s1))}, F)) return false;
            if (!Seam(c, kPedLoopStartFn, {g.U32(e + 540u), ClipNum(g, s1), s5 & 0xFFu, s6, 0u}, F)) return false; // 0x800CACE8
        } else {
            if (s2 == 2u) {                                                   // 0x800CAD00: running, with a cry
                s1 = g.U16(kPedClassTable + 16u * g.U8(e + 566u) + 4u);
                if (!Seam(c, kPedVoiceFn, {g.U32(e + 184u), g.U32(e + 192u), e, 0u}, F)) return false;
            } else if (s2 == 1u) {                                            // 0x800CAD3C: stepping aside
                s1 = g.U16(kPedClassTable + 16u * g.U8(e + 566u) + 6u);
            }
            if (!Seam(c, kPedHardStartFn, {g.U32(e + 540u), ClipNum(g, s1), s5 & 0xFFu, s6, 0u}, F)) return false; // 0x800CADB0
            g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) | 1u));
        }
        g.W16(e + 544u, static_cast<uint16_t>(s1));                           // 0x800CADD0
        g.W32(e + 552u, g.U32(e + 552u) & 0xFFFFFFFEu);
        return !g.Faulted();
    }
    const uint32_t thrown = (a0 >> 29) & 1u;                                  // 0x800CADE0
    uint32_t done = 0;
    if (thrown == 0) {
        if (!Seam(c, kPedClipDoneFn, {g.U32(e + 540u)}, F, &done)) return false;  // 0x800CAEE4
    } else if (!(a0 & 0x40000000u)) {                                         // 0x800CADF4: knocked down, not flying
        if (!Seam(c, kPedGetUpFn, {e}, F, &done)) return false;               // 0x800CADFC
        if (done != 0) {                                                      // up again
            const uint32_t row = kPedClassTable + 16u * g.U8(e + 566u);
            const uint32_t speed = g.U32(row + 8u);
            g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) & 0xFBu));
            g.W32(e + 480u, speed);
            const uint32_t clip = g.U16(e + 544u);
            const bool back = clip == 51u || clip == 45u || clip == 61u || clip == 62u || clip == 68u || clip == 57u ||
                              clip == 53u;                                    // 0x800CAE30..0x800CAE6C
            g.W16(s4 + 20u, static_cast<uint16_t>((back ? -1 : 0) + 5));      // state 3's next state
            const uint32_t c2 = g.U16(e + 544u);
            const uint32_t v1 = ((c2 - 60u) < 4u || c2 == 44u) ? 1u : 0u;     // 0x800CAE90..0x800CAEAC
            g.W8(s4 + 22u, static_cast<uint8_t>((v1 << 4) | (g.U8(s4 + 22u) & 0xEFu)));
            g.W32(e + 552u, g.U32(e + 552u) & 0xDFFFFFFFu);
        }
    }
    if (done != 0) {                                                          // 0x800CAEF0: the clip is over
        if (g.U16(e + 544u) == 63u) TurnRound(g, e);                          // 0x800CAF00..0x800CAF70
        const uint32_t st = s4 + 6u * s2;                                     // 0x800CAF78..0x800CAFC8: the next state
        uint32_t v = (U(g.U16(st + 2u)) << 8) | (g.U32(e + 552u) & 0xFFFF00FFu) | 1u;
        g.W32(e + 552u, v);
        v = (U(g.U8(st + 4u)) << 16) | (v & 0xFF00FFFFu);
        g.W32(e + 552u, v);
    }
    if (thrown == 0 && !((s2 - 4u) < 2u) && !PedFace(g, e, F, t)) return false;   // 0x800CAFCC..0x800CAFE0
    if (g.U32(e + 552u) & 1u) return PedAnim(g, e, F, t, c);                  // 0x800CAFFC
    return !g.Faulted();
}

bool PedFace(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables&) {
    const uint32_t F = sp - 72u;
    uint32_t a0 = g.U32(kPlayerBikePtr);                                      // 0x800CB03C: the player's bike,
    const uint32_t rider = g.U32(a0 + 852u);
    const uint32_t s5 = g.U8(e + 553u);
    uint32_t s3 = 0;
    if (!(g.U32(rider + 604u) < 3u)) a0 = rider;                              // or its rider when he is off it
    const int32_t dx = S(g.U32(e + 184u) - g.U32(a0 + 184u));
    g.W32(F + 16u, U(dx));
    const int32_t dy = S(g.U32(e + 188u) - g.U32(a0 + 188u));
    g.W32(F + 20u, U(dy));
    const int32_t dz = S(g.U32(e + 192u) - g.U32(a0 + 192u));
    g.W32(F + 24u, U(dz));
    const int32_t s4 = Iabs(S(g.U32(e + 480u) - g.U32(a0 + 480u)));          // the closing speed
    const int32_t their = g.S32(a0 + 344u);
    if ((g.U32(e + 364u) >> 31) != (g.S32(e + 344u) < their ? 1u : 0u)) s3 |= 1u; // 0x800CB0D0..0x800CB0E8: the side
    auto mid = [](int64_t p) { return S(U(static_cast<uint32_t>(static_cast<uint64_t>(p) >> 16))); };
    const int64_t p1 = static_cast<int64_t>(dx) * static_cast<int64_t>(S(U(static_cast<int32_t>(g.S16(e + 444u))) << 4));
    const int64_t p2 = static_cast<int64_t>(dy) * static_cast<int64_t>(S(U(static_cast<int32_t>(g.S16(e + 446u))) << 4));
    const int64_t p3 = static_cast<int64_t>(dz) * static_cast<int64_t>(S(U(static_cast<int32_t>(g.S16(e + 448u))) << 4));
    g.W32(F + 32u, static_cast<uint32_t>(static_cast<uint64_t>(p3)));       // 0x800CB168: the product kept
    g.W32(F + 36u, static_cast<uint32_t>(static_cast<uint64_t>(p3) >> 32));
    int32_t s0 = S(U(mid(p3)) + (U(mid(p2)) + U(mid(p1))));                 // the bike along the walk
    const int32_t s2 = cu::GDot(g, g.U32(kPlayerBikePtr) + 444u, e + 444u);   // 0x800CB180: heading at each other
    bool coming = false;
    if (s0 < 0) {                                                             // 0x800CB188
        s0 = Iabs(s0);
        coming = s0 < FM(s4, 0x6FC48) && s2 < -58982;                        // 0x800CB1A4..0x800CB1BC
    }
    if (coming) {
        if ((s5 - 1u) < 2u) return !g.Faulted();                              // 0x800CB1C8: already reacting
        const uint32_t gs = g.U32(kGameStatePtr);
        if (!(g.S32(kPedReactTimer) < S(g.U32(gs + 16u) - g.U32(e + 548u)))) return !g.Faulted(); // 0x800CB1EC
        g.W32(e + 552u, (s3 << 16) | (g.U32(e + 552u) & 0xFF00FFFFu));       // 0x800CB21C
        if (FM(s4, 0x3FDE0) < s0) g.W32(e + 552u, (g.U32(e + 552u) & 0xFFFF00FFu) | 0x101u); // far: step aside
        else g.W32(e + 552u, (g.U32(e + 552u) & 0xFFFF00FFu) | 0x201u);   // near: run
        g.W32(e + 548u, g.U32(g.U32(kGameStatePtr) + 16u));                   // 0x800CB268
        const uint32_t r = GuestRand(g);                                      // 0x800CB264
        g.W32(kPedReactTimer, r % 900u + 300u);                               // 0x800CB26C..0x800CB2A4
        return !g.Faulted();
    }
    if ((s5 - 1u) < 2u)                                                       // 0x800CB2A8: the bike has passed
        g.W32(e + 552u, (((s3 << 16) | (g.U32(e + 552u) & 0xFF00FFFFu)) & 0xFFFF00FFu) | 1u);
    return !g.Faulted();
}

// ============================================================================ the passes

bool PedPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 40u;
    RoadRuntimeNative road;
    int32_t n = g.S32(g.U32(kPoolRec2 + 12u));                                // 0x800CB328
    uint32_t e = g.U32(kPoolRec2);
    for (; n >= 0; --n, e += g.U32(kPoolRec2 + 4u)) {
        if (g.U16(e + 172u) == 0) continue;                                   // 0x800CB350
        if (g.S16(e + 320u) != 0) {                                           // 0x800CB360: live
            g.W32(e + 472u, g.U32(e + 188u));                                 // 0x800CB368..0x800CB384: the last position
            g.W32(e + 468u, g.U32(e + 184u));
            g.W32(e + 476u, g.U32(e + 192u));
            const uint32_t v1 = g.U32(e + 552u);
            const uint32_t state = g.U8(e + 553u);
            if (v1 & 0x20000000u) {                                           // 0x800CB390: knocked
                if (v1 & 0x40000000u) {                                       // flying: the velocity
                    int32_t b[3], d[3], o[3];
                    cu::Read32x3(g, e + 184u, b);
                    cu::Read32x3(g, e + 456u, d);
                    MulAdd32(b, d, dt, o);                                    // 0x800CB3AC
                    cu::Write32x3(g, e + 184u, o);
                    RoadRebindBody(g, e, F, road);                            // 0x800CB3B4
                    if (!Seam(c, kPedSettleFn, {e}, F)) return false;         // 0x800CB3BC
                } else {                                                      // sliding
                    MulAddView(g, e + 184u, e + 450u, FM(dt, g.S32(e + 480u)), e + 184u); // 0x800CB3D0..0x800CB3E4
                    int32_t a2 = g.S32(e + 508u);
                    if (a2 > 0) {                                             // 0x800CB3F4: the lift, 0.1 a frame
                        const int32_t v0 = a2 - 6553;
                        g.W32(e + 508u, U(v0));
                        if (v0 >= 0) a2 = 6553;
                        else g.W32(e + 508u, 0);
                        MulAddView(g, e + 184u, e + 522u, a2, e + 184u);      // 0x800CB418
                    }
                    if (RoadRebindBody(g, e, F, road) != 0) {                 // 0x800CB420
                        if (!Seam(c, kPedSettleFn, {e}, F)) return false;     // 0x800CB430
                        g.W32(e + 568u, g.U32(e + 568u) | 1u);
                    }
                }
            } else if (state == 0u) {                                         // 0x800CB44C: walking
                if (!PedStep(g, e, dt, F, t)) return false;                   // 0x800CB458
            }
            if (!Seam(c, kPedBoxFn, {e}, F)) return false;                    // 0x800CB460
            int32_t table[18];
            for (uint32_t k = 0; k < 18; ++k) table[k] = g.S32(kAtanTable + 4u * k);
            const int32_t h = RatAtan2(S(U(static_cast<int32_t>(g.S16(e + 444u))) << 4),
                                       S(U(static_cast<int32_t>(g.S16(e + 448u))) << 4), table); // 0x800CB474
            g.W32(e + 292u, U(h));
            g.W32(e + 296u, U(static_cast<int32_t>(g.S16(kSinCos + (((U(h) & 0xFFFu) << 2) | 2u)))) << 4);
            g.W32(e + 300u, U(static_cast<int32_t>(g.S16(kSinCos + ((g.U32(e + 292u) & 0xFFFu) << 2)))) << 4);
        }
        g.W16(e + 320u, static_cast<uint16_t>(RoadWindow(g, e + 172u, F)));  // 0x800CB4BC
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

bool PedRelease(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 48u;
    RoadRuntimeNative road;
    int32_t n = g.S32(g.U32(kPoolRec2 + 12u));                                // 0x800CB520
    uint32_t e = g.U32(kPoolRec2);
    for (; n >= 0; --n, e += g.U32(kPoolRec2 + 4u)) {
        if (g.U16(e + 172u) == 0) continue;                                   // 0x800CB540
        if (g.S16(e + 320u) == 0) {                                           // 0x800CB550: out of the window
            if (!PoolRelease(g, e + 172u, 2)) return false;                   // 0x800CB750
            continue;
        }
        if ((g.U8(e + 9u) & 3u) == 0 && g.U8(e + 553u) == 0) {                // 0x800CB558..0x800CB56C: not drawn, standing
            if (!(g.U8(e + 565u) & 8u)) {
                if (!Seam(c, kPedHoldFn, {g.U32(e + 540u)}, F)) return false; // 0x800CB5CC
                g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) | 8u));
            }
        } else {
            if (g.U8(e + 565u) & 8u) {
                if (!Seam(c, kPedResumeFn, {g.U32(e + 540u)}, F)) return false; // 0x800CB58C
                g.W8(e + 565u, static_cast<uint8_t>(g.U8(e + 565u) & 0xF7u));
            }
            if (!PedAnim(g, e, F, t, c)) return false;                        // 0x800CB5A4
        }
        if (g.U32(e + 568u) & 2u) {                                           // 0x800CB5E4
            RoadRebindBody(g, e, F, road);                                    // 0x800CB5F8
            if (!Seam(c, kPedSettleFn, {e}, F)) return false;                 // 0x800CB600
            g.W32(e + 568u, (g.U32(e + 568u) | 1u) & 0xFFFFFFFDu);
        }
        const uint32_t v1 = g.U32(e + 552u);
        if (!(v1 & 0x20000000u)) continue;                                    // 0x800CB628
        if (v1 & 0x40000000u) {                                               // 0x800CB634: the flight
            const int32_t s1 = FM(g.S32(kPedDrag), dt);                       // 0x800CB640
            const int32_t vx = FM(s1, g.S32(e + 456u));
            g.W32(e + 456u, g.U32(e + 456u) - U(vx));
            int32_t s0 = FM(s1, g.S32(e + 460u));
            s0 = S(U(s0) - U(FM(0x9D087, dt)));                               // gravity
            g.W32(e + 460u, g.U32(e + 460u) - U(s0));
            const int32_t vz = FM(s1, g.S32(e + 464u));
            g.W32(e + 464u, g.U32(e + 464u) - U(vz));
            int32_t v[3];
            cu::Read32x3(g, e + 456u, v);
            const int32_t len = Length3(v, t.sqrt);                           // 0x800CB6AC
            g.W32(e + 480u, U(len));
            if (len != 0) ScaleTo16(g, rc::Recip(len), e + 456u, e + 450u);   // 0x800CB6BC..0x800CB708
            g.W32(e + 552u, g.U32(e + 552u) & 0x7FFFFFFFu);
        } else if (!(v1 & 0x04000000u)) {                                     // 0x800CB728: on the ground
            if (!Seam(c, kPedSlideFn, {e, U(dt), e + 488u}, F)) return false; // 0x800CB73C
        }
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

// ============================================================================ the spawner

uint32_t PedProp(GuestRam& g, uint32_t e, int32_t kind, int32_t lod) {
    if (g.U32(kPedObjectMask) == 255u) return 0;                              // 0x800CB7B4
    uint32_t s0 = kPedObjects;
    for (uint32_t a3 = 0; a3 < 8u; ++a3, s0 += 172u) {                        // 0x800CB7CC..0x800CB82C
        const uint32_t mask = g.U32(kPedObjectMask), bit = 1u << a3;
        if (mask & bit) continue;
        g.W32(kPedObjectMask, mask | bit);
        g.W8(e + 567u, static_cast<uint8_t>(a3));
        if (lod != static_cast<int32_t>(g.S8(s0 + 8u))) LodSelect(g, s0, U(lod)); // 0x800CB7FC
        Attach(g, e, s0, kind, 0);                                            // 0x800CB810
        return 1;
    }
    return 0;
}

bool PedSpawn(GuestRam& g, uint32_t rec, uint32_t onRecord, uint32_t bike, uint32_t sp, const BikeTables& t,
              RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - 96u;
    v0 = 0;
    RoadRuntimeNative road;
    if (g.U32(kAnimDesc + 8u) == g.U32(kAnimDesc + 12u)) return !g.Faulted(); // 0x800CB908: no animation object
    if (g.S16(0x800CE580u) == 0) return !g.Faulted();                         // 0x800CB91C: no pedestrian class
    g.W32(F + 16u, g.U32(rec + 8u));                                          // 0x800CB92C..0x800CB950: the road key
    g.W32(F + 24u, g.U32(rec + 36u));
    g.W32(F + 20u, U(static_cast<int32_t>(g.S16(rec + 60u))));
    const uint32_t gate = RoadGate(g, 0, F + 16u);                            // 0x800CB94C
    if (gate == 0) return !g.Faulted();
    uint32_t e = 0;                                                           // 0x800CB960..0x800CBA44: allocate
    if (g.U32(kPedBlock + 16u) != 0) {
        const int32_t a2 = g.S32(kPedBlock + 4u);
        if (a2 < 4 && g.S32(kPedBlock) < g.S32(0x800D8744u)) {
            const uint32_t base = g.U32(kPedBlock + 16u);
            int32_t t0 = a2 + 1;
            uint32_t v1 = base + 572u * U(t0) + 4u;
            while (t0 < 4) {                                                  // the next free slot with part slots
                if (g.U16(v1 + 168u) == 0 && g.U32(v1) != 0) break;
                ++t0;
                v1 += 572u;
            }
            g.W32(kPedBlock + 4u, U(t0));
            e = g.U32(kPedBlock + 16u) + 572u * U(a2);
            g.W16(e + 172u, static_cast<uint16_t>(a2 + 64));                  // the handle: pool 2, slot a2
            g.W32(kPedBlock, g.U32(kPedBlock) + 1u);
            if (g.S32(kPedBlock + 8u) < a2) g.W32(kPedBlock + 8u, U(a2));
        }
    }
    if (e == 0) return !g.Faulted();
    g.W8(e + 567u, 0xFF);                                                     // 0x800CBA50
    g.W32(e + 56u, 0);
    if (g.U32(rec + 44u) != 0) {                                              // 0x800CBA58: the placement jitter
        const uint32_t r = GuestRand(g);
        g.W32(rec + 20u, g.U32(rec + 20u) + ((r % g.U32(rec + 44u)) << 16));
    }
    if (g.U32(rec + 40u) != 0) {
        const uint32_t r = GuestRand(g);
        g.W32(rec + 28u, g.U32(rec + 28u) + ((r % g.U32(rec + 40u)) << 16));
    }
    auto fail = [&]() {                                                       // 0x800CC024
        if (!PoolRelease(g, e + 172u, 2)) return false;
        v0 = 0;
        return !g.Faulted();
    };
    if (CursorSeat(g, gate, F + 16u, F + 32u, F) == 0) return fail();         // 0x800CBAD4
    if (g.U16(F + 18u) == 1u) JunctionSlice(g, F + 32u, rec + 20u);           // 0x800CBAF8
    GuestCopyWords(g, e + 328u, F + 32u, 32u);                                // 0x800CBB08
    const uint32_t s2 = g.U32(e + 340u);                                      // the slice
    uint32_t cls = 0;
    if (!ModelBind(g, e, 4, 0xFFFFu, 0, cls)) return false;                   // 0x800CBB20
    g.W32(e + 180u, cls);
    if (cls == 0xFFFFu) return fail();
    const uint32_t v1 = g.U16(rec + 2u);                                      // 0x800CBB34: the record's class
    auto walks = [&](uint32_t row) { return g.U16(kPedClassTable + 16u * (row & 0xFFu)) != 0; };
    auto randomWalk = [&]() {                                                 // 0x800CBCF8
        const uint32_t r = GuestRand(g);
        g.W8(e + 564u, static_cast<uint8_t>((r & 1u) ? 1u : 2u));
    };
    if (v1 < 19u) {
        g.W8(e + 566u, static_cast<uint8_t>(v1));
        if (walks(v1)) randomWalk();
        else g.W8(e + 564u, 0);
    } else if (v1 == 19u || v1 == 20u) {                                      // 0x800CBB84 / 0x800CBBA8
        const uint32_t r = GuestRand(g);
        g.W8(e + 564u, static_cast<uint8_t>(v1 == 19u ? 1u : 2u));
        g.W8(e + 566u, static_cast<uint8_t>(r % 13u));
    } else if (v1 == 21u) {                                                   // 0x800CBBF0
        const uint32_t r = GuestRand(g);
        g.W8(e + 566u, static_cast<uint8_t>(r % 13u));
        if (walks(r % 13u)) randomWalk();
        else g.W8(e + 564u, 0);
    } else if (v1 == 22u) {                                                   // 0x800CBC58
        const uint32_t r = GuestRand(g);
        g.W8(e + 564u, 0);
        g.W8(e + 566u, static_cast<uint8_t>(r % 6u + 13u));
    } else {                                                                  // 0x800CBC94
        const uint32_t r = GuestRand(g);
        g.W8(e + 566u, static_cast<uint8_t>(r % 19u));
        if (walks(r % 19u)) randomWalk();
        else g.W8(e + 564u, 0);
    }
    CarSetup(g, e, 0, F);                                                     // 0x800CBD18: the half extents
    if (onRecord != 0) {                                                      // 0x800CBD20: at the record's point
        g.W32(e + 184u, g.U32(rec + 20u));
        g.W32(e + 188u, g.U32(rec + 24u));
        g.W32(e + 192u, g.U32(rec + 28u));
        RoadProjectView(g, e + 184u, s2, e + 344u, e + 348u, F);              // 0x800CBD4C
    } else {                                                                  // on the slice
        const int32_t along = g.S32(e + 348u);
        g.W32(e + 184u, g.U32(s2 + 20u));
        g.W32(e + 188u, g.U32(s2 + 24u));
        g.W32(e + 192u, g.U32(s2 + 28u));
        MulAddView(g, s2 + 20u, s2 + 14u, along, e + 184u);                   // 0x800CBD80
    }
    const uint32_t row = kPedClassTable + 16u * g.U8(e + 566u);               // 0x800CBD8C..0x800CBE38: the frame
    g.W16(e + 438u, 0);
    g.W16(e + 440u, 4096);
    g.W16(e + 442u, 0);
    g.W32(e + 480u, g.U32(row + 8u));
    g.W16(e + 444u, g.U16(s2 + 14u));
    g.W16(e + 446u, g.U16(s2 + 16u));
    g.W16(e + 444u, Neg16(g.U16(e + 444u)));
    const uint16_t a3 = g.U16(s2 + 18u);
    g.W16(e + 434u, 0);
    g.W16(e + 448u, a3);
    g.W32(e + 188u, g.U32(e + 188u) + 0xFFFF0000u);                           // one unit up
    g.W16(e + 448u, Neg16(a3));
    g.W16(e + 432u, Neg16(a3));
    g.W16(e + 454u, Neg16(a3));
    g.W16(e + 446u, Neg16(g.U16(e + 446u)));
    const uint16_t x = g.U16(e + 444u), y = g.U16(e + 446u);
    g.W16(e + 436u, Neg16(x));
    g.W16(e + 450u, x);
    g.W16(e + 452u, y);
    RoadPosition(g, e + 450u, e + 328u, e + 360u, F);                         // 0x800CBE34
    g.W32(e + 364u, g.U32(F + 20u));
    RouteBind(g, e + 172u, 0, bike, F, road);                                 // 0x800CBE50
    g.W32(e + 324u, U(ProgressScalar(g, e + 172u, F)));                       // 0x800CBE58
    g.W16(e + 322u, g.U16(rec + 62u));
    g.W32(e + 316u, 0x4B0000u);
    g.W32(e + 556u, g.U32(rec + 64u) << 4);                                   // the walk length
    const uint32_t live = RoadWindow(g, e + 172u, F);                         // 0x800CBE80
    g.W16(e + 320u, static_cast<uint16_t>(live));
    if ((live & 0xFFFFu) == 0) return fail();                                 // 0x800CBE90
    RoadClass(g, e, 1, 0, -1, F, road);                                       // 0x800CBEA8
    RoadsideRun(g, e, 1, -1, F, road);                                        // 0x800CBEB8
    {                                                                         // 0x800CBEC0..0x800CBF20: inside the edges
        const int32_t lat = g.S32(e + 344u);
        if (lat < 0 && lat < g.S32(e + 400u)) {
            MulAddView(g, e + 184u, s2 + 2u, S(U(g.S32(e + 400u) - lat) + 0x10000u), e + 184u);
        } else if (g.S32(e + 412u) < g.S32(e + 344u)) {
            MulAddView(g, e + 184u, s2 + 2u, S(g.U32(e + 412u) - g.U32(e + 344u) + 0xFFFF0000u), e + 184u);
        }
    }
    if (onRecord == 0) {                                                      // 0x800CBF24: the record's lateral
        g.W32(e + 344u, g.U32(rec + 32u));
        const int32_t a0 = g.S32(rec + 32u);
        const int32_t a1 = Iabs(a0);
        const uint32_t node = g.U32(e + 372u);
        if (a1 == 0x10000 && node != 0) {                                     // one unit off an edge
            uint32_t lat;
            if (a0 > 0) lat = g.S16(node + 138u) != 0 ? g.U32(node + 160u) + U(a1) : g.U32(node + 144u) - U(a1);
            else lat = g.S16(node + 10u) != 0 ? g.U32(node + 32u) - U(a1) : g.U32(node + 16u) + U(a1);
            g.W32(e + 344u, lat);
        }
        MulAddView(g, e + 184u, s2 + 2u, g.S32(e + 344u), e + 184u);          // 0x800CBFD0
    }
    g.W32(e + 552u, 0);                                                       // 0x800CBFE8..0x800CC00C
    g.W32(e + 540u, 0);
    g.W8(e + 565u, 0);
    g.W32(e + 560u, 0);
    g.W32(e + 552u, 1u);                                                      // "start the state's clip", state 0
    const uint32_t a = ViewSlot(g, kAnimDesc, e);                             // 0x800CC008
    g.W32(e + 540u, a);
    g.W8(e + 72u, 3);
    if (a == 0) return fail();                                                // 0x800CC01C
    if (!PedAnim(g, e, F, t, c)) return false;                                // 0x800CC038
    if (!PedStart(g, e, F, t)) return false;                                  // 0x800CC040
    if (!PedFace(g, e, F, t)) return false;                                   // 0x800CC048
    g.W32(e + 176u, 0xFFFFFFFFu);                                             // 0x800CC05C
    g.W32(e + 548u, g.U32(g.U32(kGameStatePtr) + 16u));
    if (g.U8(e + 566u) == 0) PedProp(g, e, 10, 9);                            // 0x800CC07C: class 0 holds an object
    v0 = e;
    return !g.Faulted();
}

} // namespace rr::sim
