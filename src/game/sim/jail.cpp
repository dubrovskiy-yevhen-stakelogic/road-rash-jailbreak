// src\game\sim\jail - the Jailbreak escape scene RASHCDG 0x800C9420 (jail.h), transcribed from our own
// disassembly of RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c).
#include "game/sim/jail.h"

#include "game/sim/coll_util.h"
#include "game/sim/fixed.h"         // RatAtan2 0x80020018
#include "game/sim/integrator.h"    // MulMatrix0 0x8003FA40
#include "game/sim/recover_walk.h"  // VecMat SLUS 0x8002EFF4
#include "game/sim/traffic_bind.h"  // ModelKeySet SLUS 0x800302C4
#include "game/sim/ai.h"            // Length3 SLUS 0x8002E548
#include "game/sim/resolvers.h"     // Dot32 SLUS 0x8002E604

namespace rr::sim {

namespace {

using cu::S;
using cu::U;

constexpr uint32_t kPlayer1 = 0x8005B38C;   // -> player 1's bike
constexpr uint32_t kPool0Base = 0x8005B3A0; // -> pool 0 (1096-byte bikes)
constexpr uint32_t kPool0Count = 0x8005B1F8;
constexpr uint32_t kPool1Count = 0x8005B218; // read as u16 (lhu at 0x800C943C)
constexpr uint32_t kMilestone1 = 0x80053178; // {u16 road, u16 along}: the anchor (lo16 / hi16)
constexpr uint32_t kSinCos = 0x8005624C;     // SLUS: {s16 sin, s16 cos} x 4096
constexpr uint32_t kAtanTable = 0x8005285C;  // SLUS: RatAtan2's 18 words
constexpr uint32_t kBikeBytes = 1096;
constexpr uint32_t kGsPtr = 0x8005B2F8;      // -> game_state (+0x10 clock, +0x39 the Jailbreak phase)
constexpr uint32_t kMilestones = 0x80053174; // {u16 road, u16 along} x 5
constexpr uint32_t kPoolTable = 0x800CE4D0;  // pool 0: {base, stride, -> live, -> high}
constexpr uint32_t kPostLimit = 0x8005B228;  // the post-race word (16.16 s)
constexpr uint32_t kRandSeed = 0x8005B4A8;   // gp + 2076, the shared LCG

uint32_t ClassOf(GuestRam& g, uint32_t e) { return g.U8(g.U32(e + 1084u) + 1u) & 0xFu; }

void Copy(GuestRam& g, uint32_t dst, uint32_t src, uint32_t n) { // SLUS 0x8001E0B4 memcpy
    for (uint32_t k = 0; k < n; ++k) g.W8(dst + k, g.U8(src + k));
}

// MulMatrix0 0x8003FA40(a, b, out) over guest halfwords (out may alias a).
void GuestMulMatrix(GuestRam& g, uint32_t a, uint32_t b, uint32_t out) {
    int16_t x[9], y[9], o[9];
    for (uint32_t k = 0; k < 9; ++k) {
        x[k] = g.S16(a + 2u * k);
        y[k] = g.S16(b + 2u * k);
    }
    MulMatrix0(x, y, o);
    for (uint32_t k = 0; k < 9; ++k) g.W16(out + 2u * k, static_cast<uint16_t>(o[k]));
}

// 0x800C99DC..0x800C9A3C (and 0x800C9C90..0x800C9CE8): the heading from the forward row +0x1BC /
// +0x1C0 through RatAtan2, and its cosine / sine (x 16) from the table.
void HeadingFromRows(GuestRam& g, uint32_t e) {
    int32_t atan[18];
    for (uint32_t k = 0; k < 18; ++k) atan[k] = g.S32(kAtanTable + 4u * k);
    const int32_t a = RatAtan2(g.S16(e + 444u), g.S16(e + 448u), atan);
    g.W32(e + 292u, U(a));
    const uint32_t i = U(a) & 0xFFFu;
    g.W32(e + 296u, U(static_cast<int32_t>(g.S16(kSinCos + ((i << 2) | 2u))) << 4));
    g.W32(e + 300u, U(static_cast<int32_t>(g.S16(kSinCos + (i << 2))) << 4));
}

} // namespace

bool EscapeScene(GuestRam& g, int32_t block, uint32_t sp0, EscapeCallees& c) {
    const uint32_t sp = sp0 - kJailFrame;                               // 0x800C9420
    const uint32_t blk = U(block) << 2;
    g.W32(sp + 288u, U(block));                                         // the argument home
    uint32_t local30 = 1;                                               // sp+240: the stance word
    uint32_t idx0 = 0, off0 = 0;                                        // sp+236 / sp+244: group 0's walk
    uint32_t cnt224 = g.U16(kPool1Count), cnt228 = g.U16(kPool1Count);  // sp+224 / sp+228: group 1's walks
    g.W32(kJailEscapeCar, 0);                                           // 0x800C9480
    if (!(g.S32(kJailBlockCount + blk) > 0)) return !g.Faulted();      // 0x800C9488
    const uint32_t s8 = kJailBlockArray + blk;
    uint32_t s7 = 0;
    for (uint32_t i = 0;; ++i) {
        const uint32_t rec = g.U32(s8) + s7;                            // a3
        const uint32_t packed = g.U32(rec + 8u);
        int32_t s6 = S(packed << 20) >> 8;                              // 0x800C94C4..0x800C94E4: -(deg << 12) / 360
        {
            const int32_t v1 = cu::Neg(s6);
            const int64_t prod = static_cast<int64_t>(v1) * static_cast<int64_t>(S(0xB60B60B7u));
            const int32_t hi = static_cast<int32_t>(prod >> 32);
            s6 = S(U(S(U(hi) + U(v1)) >> 8) - U(v1 >> 31));
        }
        const uint32_t grp = (packed >> 12) & 0xFu;                     // a2 (sra, andi)
        const int32_t s1 = S(packed) >> 16;                             // the road half
        if (g.S32(g.U32(kPlayer1) + 364u) < 0) {                        // 0x800C94F8: player 1 drives backwards
            g.W32(rec + 4u, U(cu::Neg(g.S32(rec + 4u))));
            g.W32(rec + 0u, U(cu::Neg(g.S32(rec + 0u))));
            const int32_t a0 = S(U(s6) + 2048u);
            const int32_t a1 = a0 >= 0 ? a0 : S(U(s6) + 6143u);
            s6 = S(U(a0) - U((a1 >> 12) << 12));
        }
        if (g.Faulted()) return false;
        const uint32_t f = sp;                                          // the callees' sp
        if (grp == 0u) {                                                // 0x800C9584: an escape bike
            const uint32_t s5 = ClassOf(g, g.U32(kPlayer1)) == 0u ? 1u : 0u;
            uint32_t a1 = 2;
            const uint32_t count = g.U32(kPool0Count);
            if (S(idx0) < S(count)) {
                for (;;) {
                    if (a1 == s5) break;                                // 0x800C95D8
                    off0 += kBikeBytes;
                    const uint32_t nb = g.U32(kPool0Base) + kBikeBytes * idx0 + kBikeBytes;
                    ++idx0;
                    a1 = ClassOf(g, nb);
                    if (!(S(idx0) < S(count))) break;
                    if (g.Faulted()) return false;
                }
            }
            const uint32_t s4 = g.U32(kPool0Base) + off0;
            if (g.U32(kJailFirstBike) == 0u) g.W32(kJailFirstBike, s4);
            if (block == 0) g.W32(g.U32(s4 + 556u) + 224u, 0x141DDDu);  // 0x800C964C
            g.W32(s4 + 360u, g.U16(kMilestone1));
            g.W32(s4 + 368u, (static_cast<uint32_t>(g.U16(kMilestone1 + 2u)) << 16) + g.U32(rec + 4u));
            g.W32(s4 + 364u, g.U32(g.U32(kPlayer1) + 364u));
            g.W32(s4 + 292u, U(s6));
            g.W16(s4 + 320u, 1);
            g.W32(s4 + 344u, g.U32(rec + 0u));
            if (g.Faulted()) return false;
            if (!c.Placement(s4, f)) return false;                      // 0x800C96AC
            if (block != 0) {
                const uint32_t r = g.U32(s4 + 852u);
                g.W16(s4 + 966u, 224);
                g.W8(s4 + 946u, 2);
                g.W16(s4 + 964u, 0);
                g.W32(r + 604u, 4);
            }
        } else if (grp == 1u) {                                         // 0x800C96E4: a rider on foot
            const uint32_t pc = ClassOf(g, g.U32(kPlayer1));
            const uint32_t v1 = s1 < 17 ? 0u : 0xFFFFFFFFu;
            const uint32_t s5 = pc != 0u ? (~v1 & pc) : (v1 & 1u);
            uint32_t& cnt = s1 < 17 ? cnt228 : cnt224;
            uint32_t a0 = cnt;
            if (S(a0) > 0) {
                uint32_t a1 = 2;
                for (;;) {
                    if (a1 == s5) break;                                // 0x800C974C
                    const uint32_t nv = a0 - 1u;
                    cnt = nv;
                    a1 = ClassOf(g, g.U32(kPool0Base) + kBikeBytes * nv);
                    a0 = nv;
                    if (!(S(nv) > 0)) break;
                    if (g.Faulted()) return false;
                }
            }
            const uint32_t s4 = g.U32(kPool0Base) + kBikeBytes * cnt;
            const uint32_t s3 = g.U32(s4 + 852u);
            if (g.U32(s4 + 856u) != 0u) {                               // 0x800C97BC: the player's passenger boards
                const uint32_t pr = g.U32(g.U32(g.U32(kPlayer1) + 856u) + 852u);
                g.W8(pr + 572u, static_cast<uint8_t>(g.U8(pr + 572u) | 0x60u));
                const uint32_t hr = g.U32(g.U32(kPlayer1) + 852u);
                g.W8(hr + 572u, static_cast<uint8_t>(g.U8(hr + 572u) | 0x10u));
            }
            if (g.Faulted()) return false;
            if (g.S8(s3 + 72u) == 1) {                                  // 0x800C9814: still seated
                if (!c.SeatRelease(s4, s3, g.U32(s4 + 856u) != 0u ? 1u : 0u, f)) return false;
            }
            g.W8(s3 + 72u, 3);
            const uint32_t road = g.U16(kMilestone1);
            g.W32(s3 + 360u, road);
            g.W32(sp + 80u, road);
            const uint32_t along = (static_cast<uint32_t>(g.U16(kMilestone1 + 2u)) << 16) + g.U32(g.U32(s8) + s7 + 4u);
            g.W32(s3 + 368u, along);
            g.W32(sp + 88u, along);
            const uint32_t dir = g.U32(g.U32(kPlayer1) + 364u);
            g.W32(s3 + 364u, dir);
            g.W32(sp + 84u, dir);
            g.W32(s3 + 344u, g.U32(g.U32(s8) + s7 + 0u));
            if (g.Faulted()) return false;
            uint32_t obj = 0;
            if (!c.RoadGate(0, sp + 80u, f, obj)) return false;        // 0x800C989C
            if (obj != 0u) {
                uint32_t ok = 0;
                if (!c.CursorSeat(obj, sp + 80u, sp + 96u, f, ok)) return false;   // 0x800C98B4
                if (ok != 0u) {
                    Copy(g, s3 + 328u, sp + 96u, 32);                   // 0x800C98C8
                    const uint32_t s2 = g.U32(sp + 108u);
                    cu::GMulAdd(g, s2 + 20u, s2 + 14u, g.S32(sp + 116u), s3 + 184u);
                    cu::GMulAdd(g, s3 + 184u, s2 + 2u, g.S32(g.U32(s8) + s7 + 0u), s3 + 184u);
                    g.W32(s3 + 508u, 0);
                    g.W16(s3 + 438u, g.U16(s2 + 8u));
                    g.W16(s3 + 440u, g.U16(s2 + 10u));
                    g.W16(s3 + 438u, static_cast<uint16_t>(0u - g.U16(s3 + 438u)));
                    g.W16(s3 + 442u, static_cast<uint16_t>(0u - g.U16(s2 + 12u)));
                    g.W16(s3 + 440u, static_cast<uint16_t>(0u - g.U16(s3 + 440u)));
                    g.W16(s3 + 444u, g.U16(s2 + 14u));
                    g.W16(s3 + 446u, g.U16(s2 + 16u));
                    g.W16(s3 + 448u, g.U16(s2 + 18u));
                    g.W16(s3 + 432u, g.U16(s2 + 2u));
                    g.W16(s3 + 434u, g.U16(s2 + 4u));
                    g.W16(s3 + 436u, g.U16(s2 + 6u));
                    if (g.Faulted()) return false;
                    if (!c.AxisRotation(s2 + 8u, s6, sp + 200u, f)) return false;   // 0x800C9990
                    GuestMulMatrix(g, s3 + 432u, sp + 200u, s3 + 432u); // 0x800C99A4
                    g.W16(s3 + 450u, g.U16(s3 + 444u));
                    g.W16(s3 + 452u, g.U16(s3 + 446u));
                    g.W16(s3 + 454u, g.U16(s3 + 448u));
                    for (uint32_t k = 0; k < 9; ++k) g.W16(s3 + 516u + 2u * k, g.U16(s3 + 432u + 2u * k)); // 0x8003FA18
                    if (g.Faulted()) return false;
                    if (!c.BuildObb(s3, f)) return false;              // 0x800C99D4
                    HeadingFromRows(g, s3);                             // 0x800C99E4
                    g.W16(s3 + 320u, 1);
                    if (g.Faulted()) return false;
                    if (!c.RoadUpdate(s3, f)) return false;             // 0x800C9A38
                    uint32_t ev;
                    if (s5 != ClassOf(g, g.U32(kPlayer1))) {            // 0x800C9A60: a guard
                        const uint32_t rd = g.U32(g.U32(s3 + 596u) + 1084u);
                        g.W8(rd + 46u, 2);
                        g.W8(g.U32(g.U32(s3 + 596u) + 1084u) + 47u, 0);
                        ev = 64;
                        if (g.Faulted()) return false;
                        if (!c.WeaponObject(s3, 0, f)) return false;    // 0x800C9A8C
                    } else {                                            // a prisoner of the player's gang
                        ev = 66;
                        if ((g.U16(s3 + 172u) >> 1) & 1u) {
                            ev = 65;
                            if (!(g.U8(s3 + 572u) & 0x40u)) {
                                const uint32_t rd = g.U32(g.U32(s3 + 596u) + 1084u);
                                g.W8(rd + 46u, 5);
                                g.W8(g.U32(g.U32(s3 + 596u) + 1084u) + 47u, 0);
                                if (g.Faulted()) return false;
                                if (!c.WeaponObject(s3, 0, f)) return false;   // 0x800C9AEC
                            }
                        }
                        ModelKeySet(g, s3, 151);                        // 0x800C9B00
                        g.W32(s3 + 36u, (g.U32(s3 + 36u) & 0xFFFC0FFFu) | 0x29000u);
                    }
                    g.W16(s4 + 966u, 224);                              // 0x800C9B2C
                    g.W16(s4 + 964u, 0);
                    g.W8(s4 + 946u, 2);
                    uint32_t rc = 0;
                    if (g.Faulted()) return false;
                    if (!c.GetRCnt(0xF2000002u, f, rc)) return false;   // 0x800C9B40
                    const uint32_t v = rc & 0xFFu;
                    local30 = local30 | 0x800u | ((((v * 3u) >> 7) + 5u) << 16);
                    if (!c.StanceEvent(ev, s3, local30, f)) return false;   // 0x800C9B78
                }
            }
        } else if (grp == 3u) {                                         // 0x800C9B88: a police car
            const uint32_t r = sp + 128u;
            g.W32(r + 8u, g.U16(kMilestone1));
            g.W32(r + 36u, (static_cast<uint32_t>(g.U16(kMilestone1 + 2u)) << 16) + g.U32(g.U32(s8) + s7 + 4u));
            const uint32_t p1 = g.U32(kPlayer1);
            g.W16(r + 2u, static_cast<uint16_t>(U(s1)));
            g.W16(r + 64u, static_cast<uint16_t>(((0u - (i & 1u)) & 0xFFFFFFFEu) + 3u));
            g.W16(r + 60u, g.U16(p1 + 364u));
            if (g.Faulted()) return false;
            uint32_t car = 0;
            if (!c.CarSpawn(r, p1, f, car)) return false;               // 0x800C9BE4
            if (car != 0u) {
                if (block != 0) {                                       // parked at the jail
                    g.W32(car + 484u, 0);
                    g.W32(car + 480u, 0);
                    const uint32_t sl = g.U32(car + 340u);
                    cu::GMulAdd(g, sl + 20u, sl + 14u, g.S32(car + 348u), car + 184u);
                    cu::GMulAdd(g, car + 184u, g.U32(car + 340u) + 2u, g.S32(g.U32(s8) + s7 + 0u), car + 184u);
                    if (g.Faulted()) return false;
                    if (!c.AxisRotation(g.U32(car + 340u) + 8u, s6, sp + 200u, f)) return false;   // 0x800C9C58
                    GuestMulMatrix(g, car + 432u, sp + 200u, car + 432u);
                    g.W16(car + 450u, g.U16(car + 444u));
                    g.W16(car + 452u, g.U16(car + 446u));
                    g.W16(car + 454u, g.U16(car + 448u));
                    if (g.Faulted()) return false;
                    if (!c.BuildObb(car, f)) return false;              // 0x800C9C88
                    HeadingFromRows(g, car);
                } else {                                                // 0x800C9CEC: moving
                    g.W32(car + 484u, 0x26666u);
                    if (g.U32(kJailEscapeCar) == 0u) g.W32(kJailEscapeCar, car);
                    g.W32(car + 480u, 0);
                }
            }
        } else if (grp == 4u) {                                         // 0x800C9D10: a roadblock prop
            const uint32_t r = sp + 16u;
            for (uint32_t k = 0; k < 64; ++k) g.W8(r + k, 0);           // SLUS 0x8001E100
            const uint32_t road = g.U16(kMilestone1);
            g.W32(sp + 24u, road);
            g.W32(sp + 80u, road);
            const uint32_t along = (static_cast<uint32_t>(g.U16(kMilestone1 + 2u)) << 16) + g.U32(g.U32(s8) + s7 + 4u);
            g.W32(sp + 52u, along);
            g.W32(sp + 88u, along);
            const uint16_t dir = g.U16(g.U32(kPlayer1) + 364u);
            g.W16(sp + 78u, static_cast<uint16_t>(U(s1)));
            g.W16(sp + 18u, static_cast<uint16_t>(U(s1)));
            g.W16(sp + 76u, dir);
            g.W32(sp + 84u, U(static_cast<int32_t>(static_cast<int16_t>(dir))));
            if (g.Faulted()) return false;
            uint32_t obj = 0;
            if (!c.RoadGate(0, sp + 80u, f, obj)) return false;        // 0x800C9D7C
            if (obj != 0u) {
                uint32_t ok = 0;
                if (!c.CursorSeat(obj, sp + 80u, sp + 96u, f, ok)) return false;   // 0x800C9D90
                if (ok != 0u) {
                    const uint32_t s2 = g.U32(sp + 108u);
                    cu::GMulAdd(g, s2 + 20u, s2 + 14u, g.S32(sp + 116u), sp + 36u);
                    cu::GMulAdd(g, sp + 36u, s2 + 2u, g.S32(g.U32(s8) + s7 + 0u), sp + 36u);
                    g.W16(sp + 28u, 3);
                    if (g.Faulted()) return false;
                    if (!c.AxisRotation(s2 + 8u, s6, sp + 200u, f)) return false;   // 0x800C9DEC
                    VecMat(g, s2 + 14u, sp + 200u, sp + 30u);           // 0x800C9DFC
                    if (g.Faulted()) return false;
                    if (!c.Roadblock(r, g.U32(kPlayer1), f)) return false;   // 0x800C9E0C
                }
            }
        }
        if (g.Faulted()) return false;
        s7 += 12u;                                                      // 0x800C9E14
        if (!(S(i + 1u) < g.S32(kJailBlockCount + blk))) break;         // 0x800C9E38
    }
    return !g.Faulted();
}

bool JailbreakFinish(GuestRam& g, uint32_t sp, const BikeTables& t, JailCallees& c) {
    const uint32_t f = sp - 32u;                                        // 0x800C9E74
    const uint32_t t0 = g.U32(kPlayer1);
    const uint32_t ms = kMilestones + 4u * (g.U8(g.U32(kGsPtr) + 57u) - 1u);
    const int32_t a2 = S(U(static_cast<int32_t>(g.S8(0x8005ADF2u))) * 0x10000u - g.U32(t0 + 368u) +
                         (static_cast<uint32_t>(g.U16(ms + 2u)) << 16));
    if (0xA0000 < a2) {                                                 // 0x800C9ED8: short of the stop
        const uint32_t rd = g.U32(t0 + 1084u);
        int32_t v = S(U(g.U8(rd + 69u)) * g.U32(g.U32(t0 + 556u) + 224u));
        if (v < 0) v += 127;
        g.W32(t0 + 924u, U(v >> 7));
        const uint32_t sl = g.U32(t0 + 340u);
        if (0x280000 < a2) {                                            // 0x800C9F18
            cu::GMulAdd(g, sl + 20u, sl + 14u, 0x280000, t0 + 880u);
        } else {
            cu::GMulAdd(g, sl + 20u, sl + 14u, a2, t0 + 880u);
            const uint32_t p = g.U32(kPlayer1);
            cu::GMulAdd(g, p + 880u, g.U32(p + 340u) + 2u, S(U(static_cast<int32_t>(g.S8(0x8005ADF3u))) << 16), p + 880u);
        }
        return !g.Faulted();
    }
    g.W32(t0 + 924u, 0);                                                // 0x800C9F80: at the stop
    cu::GMulAdd(g, t0 + 504u, t0 + 450u, S(g.U32(t0 + 308u) << 3), t0 + 880u);
    const uint32_t p = g.U32(kPlayer1);
    g.W32(p + 560u, g.U32(p + 560u) | 0x40000u);
    if (!(g.U32(p + 564u) & 0x80000u)) {                                // 0x800C9FBC
        g.W32(p + 916u, 0);
        g.W32(p + 920u, 0);
        g.W32(p + 564u, g.U32(p + 564u) & 0xFFF7FFFFu);
    }
    if (g.U8(g.U32(kGsPtr) + 57u) != 2u) return !g.Faulted();          // 0x800C9FEC
    const uint32_t q = g.U32(kPlayer1);
    if (0x7FFF < g.S32(q + 480u)) return !g.Faulted();                  // 0x800CA004: still rolling
    const uint32_t P = g.U32(q + 856u);
    if (g.U16(P + 956u + 8u * U(static_cast<int32_t>(g.S8(P + 946u)) - 1)) != 0u) return !g.Faulted();
    if (g.Faulted()) return false;
    return JailBoard(g, 0, f, t, c);                                    // 0x800CA03C
}

bool JailBoard(GuestRam& g, uint32_t late, uint32_t sp, const BikeTables& t, JailCallees& c) {
    const uint32_t f = sp - 96u;                                        // 0x800CA05C
    g.W32(sp + 0u, late);                                               // the argument home
    uint32_t sent = 0;                                                  // s8
    if (late != 0u) {                                                   // 0x800CA094: busted at the jail
        g.W8(g.U32(g.U32(kPlayer1) + 1084u) + 39u, 254);
        g.W32(g.U32(g.U32(kPlayer1) + 1084u) + 40u, g.U32(g.U32(kGsPtr) + 16u));
        g.W16(f + 16u, 18);
    } else {                                                            // 0x800CA0D4: the partner boards
        if (g.Faulted()) return false;
        if (!c.PopCommand(g.U32(g.U32(kPlayer1) + 856u), f)) return false;
        const uint32_t p = g.U32(kPlayer1);
        g.W16(f + 16u, 18);
        g.W16(f + 18u, g.U16(p + 172u));
        if (!c.PushCommand(f + 16u, 0, g.U32(p + 856u), f)) return false;
        const uint32_t hr = g.U32(g.U32(kPlayer1) + 852u);
        g.W8(hr + 572u, static_cast<uint8_t>(g.U8(hr + 572u) | 0x10u));
    }
    int32_t n = g.S32(g.U32(kPoolTable + 12u));                         // s5
    uint32_t e = g.U32(kPoolTable);                                     // s4
    while (n >= 0) {                                                    // 0x800CA158
        if (g.Faulted()) return false;
        if (g.S16(e + 320u) != 0) {
            const uint32_t cls = g.U8(g.U32(e + 1084u) + 1u) & 0xFu;
            const uint32_t a1 = g.U32(kPlayer1);
            if (cls != 2u && cls != (g.U8(g.U32(a1 + 1084u) + 1u) & 0xFu) && g.U16(g.U32(e + 852u) + 544u) == 64u) {
                bool send = true;                                       // a guard (stance 64)
                if (late != 0u) {                                       // 0x800CA1D4
                    const uint32_t pr = g.U32(a1 + 852u), er = g.U32(e + 852u);
                    for (uint32_t k = 0; k < 3u; ++k)
                        g.W32(f + 24u + 4u * k, g.U32(pr + 184u + 4u * k) - g.U32(er + 184u + 4u * k));
                    int32_t v[3];
                    for (uint32_t k = 0; k < 3u; ++k) v[k] = g.S32(f + 24u + 4u * k);
                    const int32_t d1 = Length3(v, t.sqrt);              // 0x800CA220
                    if (!(0x3FFFF < d1)) {
                        g.W32(kPostLimit, 0x50000);                     // 0x800CA24C
                        send = false;
                    } else {
                        for (uint32_t k = 0; k < 3u; ++k)
                            g.W32(f + 40u + 4u * k, g.U32(e + 184u + 4u * k) - g.U32(g.U32(e + 852u) + 184u + 4u * k));
                        for (uint32_t k = 0; k < 3u; ++k) v[k] = g.S32(f + 40u + 4u * k);
                        const int32_t d2 = Length3(v, t.sqrt);          // 0x800CA294
                        const int64_t prod = static_cast<int64_t>(d1) * static_cast<int64_t>(d2);
                        const uint32_t lo = static_cast<uint32_t>(prod);
                        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(prod) >> 32);
                        const uint32_t s0 = (lo >> 16) | (hi << 16);
                        const int32_t dot = Dot32(g, f + 24u, f + 40u); // 0x800CA2BC
                        const int32_t s2 = S(s0);
                        int32_t cosv;
                        if (dot > 0) cosv = s2 > 0 ? S(FixDiv(U(dot), s0)) : cu::Neg(S(FixDiv(U(dot), U(cu::Neg(s2)))));
                        else cosv = s2 > 0 ? cu::Neg(S(FixDiv(U(cu::Neg(dot)), s0))) : S(FixDiv(U(cu::Neg(dot)), U(cu::Neg(s2))));
                        if (!(0xCCCB < cosv)) send = false;             // 0x800CA31C
                        else ++sent;
                    }
                }
                if (send) {                                             // 0x800CA328
                    if (g.Faulted()) return false;
                    if (!c.PopCommand(e, f)) return false;
                    g.W16(f + 18u, g.U16(e + 172u));
                    if (!c.PushCommand(f + 16u, 0, e, f)) return false;
                    if (late == 0u) {                                   // 0x800CA358: the guard's pace
                        uint32_t seed = g.U32(kRandSeed);
                        uint32_t r = Rand(seed) & 0x1Fu;
                        g.W32(kRandSeed, seed);
                        if (!(S(r) < 17)) r -= 16u;
                        int32_t v = S((16u - r) * g.U32(0x8005ADE8u) + r * g.U32(0x8005ADECu));
                        if (v < 0) v += 15;
                        const int32_t s3 = v >> 4;
                        g.W16(e + 968u, static_cast<uint16_t>((U(s3) * 75u) >> 14));
                    }
                }
            }
        }
        --n;
        e += g.U32(kPoolTable + 4u);
    }
    if (sent != 0u) g.W32(kPostLimit, g.U32(0x8005ADE8u) + 0xA0000u);  // 0x800CA3E0
    return !g.Faulted();
}

} // namespace rr::sim
