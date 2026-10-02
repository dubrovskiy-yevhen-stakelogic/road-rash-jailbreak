#include "game/sim/ai_cmd.h"

#include "game/sim/ai.h"
#include "game/sim/fixed.h"
#include "game/sim/road_runtime.h"
#include "game/sim/spine.h"

namespace rr::sim {

namespace {

constexpr uint32_t kPool0Ptr = 0x8005B3A0;
constexpr uint32_t kGameState = 0x8005B2F8;
constexpr uint32_t kBikeCount = 0x8005B1F8;
constexpr uint32_t kAttackers = 0x800CCAC0;
constexpr uint32_t kViews = 0x800CD898;
constexpr uint32_t kViewStride = 1132;
constexpr uint32_t kKindTable = 0x800541D4;
constexpr uint32_t kPlayer1 = 0x8005B38C;
constexpr uint32_t kRaceFlags = 0x8005AD48;
constexpr uint32_t kClassBlocks = 0x80052EE4; // 36 bytes per bike class (GLOBALS.BI)
constexpr uint32_t kSpeedShare = 0x80053194;  // 3 words by race bank (GLOBALS.BI)

// The arms of the overlay's table (0x8005B9B8), by address.
constexpr uint32_t kArmLeave = 0x800BA5D4, kArmCop = 0x800BA5E8, kArmRideOn = 0x800BA6CC,
                   kArmRace = 0x800BA6E0, kArmGap = 0x800BA714, kArmBack = 0x800BA728, kArmClose = 0x800BA73C,
                   kArmPass = 0x800BA750, kArmPassStrike = 0x800BA764, kArmFight = 0x800BA778,
                   kArmIntercept = 0x800BA790, kArmNone = 0x800BA79C;

inline int32_t Add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
inline int32_t Neg(int32_t a) { return static_cast<int32_t>(0u - static_cast<uint32_t>(a)); }
inline int32_t Shl(int32_t a, int n) { return static_cast<int32_t>(static_cast<uint32_t>(a) << (n & 31)); }
inline int32_t Mul(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b)));
}
// `sra v1,x,31; addu x,v1,x; xor x,x,v1` - |x| with |INT_MIN| = INT_MIN.
inline int32_t Abs(int32_t x) {
    const int32_t s = x >> 31;
    return Add(s, x) ^ s;
}
// `mult; mflo; bgez +; addiu 127; sra 7` - the product / 128 rounded toward zero.
inline int32_t Div128(int32_t p) { return (p < 0 ? Add(p, 127) : p) >> 7; }

uint32_t Players(GuestRam& g) { return g.U32(g.U32(kGameState) + 48u); }
uint32_t Bike(GuestRam& g, uint32_t h) { return g.U32(kPool0Ptr) + (h & 0xFFFFu) * 1096u; }
int32_t Tune(GuestRam& g, uint32_t off) { return g.S32(kAiTune + off); }
int32_t Gap16(GuestRam& g, uint32_t e, uint32_t t) { return Shl(Sub(g.S32(e + 324u), g.S32(t + 324u)), 4); }

// `t`'s lateral against `e`'s, in `t`'s sense: t.lat - e.lat on the same heading sign, t.lat + e.lat
// against it (the `xor; bltz` pair every handler repeats).
int32_t RelLateral(GuestRam& g, uint32_t t, uint32_t e) {
    const int32_t a1 = g.S32(t + 344u), a0 = g.S32(e + 344u);
    return ((g.S32(t + 364u) ^ g.S32(e + 364u)) < 0) ? Add(a1, a0) : Sub(a1, a0);
}

// The half width of `t`'s lane on its side: 0x39999 (3.6) without lane data, else +0x1A8 x the lane
// count of the side its lateral lies on (0x800BB478..0x800BB4CC and its three copies).
int32_t LaneHalf(GuestRam& g, uint32_t t) {
    if (g.U32(t + 372u) == 0u || g.U16(t + 362u) != 0u) return 0x39999;
    const int32_t a0 = g.S16(t + 420u);
    const int32_t sel = Neg(static_cast<int32_t>(static_cast<uint32_t>(g.S32(t + 344u)) >> 31)) &
                        Sub(g.S16(t + 408u), a0);
    return Mul(g.S32(t + 424u), Add(a0, sel));
}

// The "is `t` riding" gate every test opens with: awake and the rider mounted (+0x25C == 1).
bool Riding(GuestRam& g, uint32_t t) { return g.S16(t + 320u) != 0 && g.S32(g.U32(t + 852u) + 604u) == 1; }

// The tail the manoeuvre arms share: +0x39C (or the stat block's top speed when +0x140 bit 1), the
// brake point words cleared, +0x234 bit 19 cleared.
void CommitSpeed(GuestRam& g, uint32_t e, int32_t speed) {
    g.W32(e + 924u, static_cast<uint32_t>(speed));
    g.W32(e + 916u, 0);
    g.W32(e + 920u, 0);
    g.W32(e + 564u, g.U32(e + 564u) & 0xFFF7FFFFu);
}

} // namespace

// ============================================================================ the tests

int32_t AiGapOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap) {
    if (!Riding(g, t)) return 0;                                               // 0x800BB448..0x800BB46C
    const int32_t a0 = Sub(Abs(g.S32(t + 344u)), LaneHalf(g, t));              // 0x800BB4D0..0x800BB4E8
    if (gap < Mul(g.S32(e + 308u), 6)) return 0;                               // 0x800BB4EC..0x800BB4FC
    if (Tune(g, 0x0C) < gap) return 0;                                         // 0x800BB504
    if (Tune(g, 0x00) < a0) return 0;                                          // 0x800BB51C
    const uint32_t gs = g.U32(kGameState);
    if (!(g.U16(e + 172u) < g.U32(gs + 48u)) && (g.U8(g.U32(e + 1084u) + 1u) & 0xFu) == 2u) return 1;
    const int32_t v = Div128(Mul(g.S32(kSpeedShare + 4u * g.U32(gs + 60u)), g.S32(g.U32(e + 556u) + 224u)));
    return (g.S32(t + 480u) < v) ? 0 : 1;                                      // 0x800BB568..0x800BB5B4
}

int32_t AiBackOffOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap) {
    if (!Riding(g, t)) return 0;                                               // 0x800BB8FC
    const int32_t a0 = Sub(Abs(g.S32(t + 344u)), LaneHalf(g, t));
    if (gap < Tune(g, 0x04)) return 0;                                         // 0x800BB99C
    if (Tune(g, 0x0C) < gap) return 0;
    if (Tune(g, 0x00) < a0) return 0;
    if (g.S32(g.U32(e + 556u) + 224u) < g.S32(t + 480u))                       // 0x800BB9D8
        if (!(g.U16(t + 172u) < Players(g))) return 0;
    return 1;
}

int32_t AiCloseOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap) {
    if (!Riding(g, t)) return 0;                                               // 0x800BBBB8
    if (gap < Neg(Tune(g, 0x10))) return 0;                                    // 0x800BBBE8
    if (Shl(Sub(g.S32(e + 480u), g.S32(t + 480u)), 1) < gap) return 0;         // 0x800BBC08
    return 1;
}

int32_t AiPassOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap) {
    if (!Riding(g, t)) return 0;                                               // 0x800BBD44
    if (!(Abs(gap) < Tune(g, 0x18))) return 0;                                 // 0x800BBD74
    const int32_t me = g.S32(e + 344u);
    int32_t d = Abs(Sub(g.S32(t + 344u), me));                                 // 0x800BBD90
    const uint32_t partner = g.U32(t + 856u);
    if (partner != 0u) {
        const int32_t p = Abs(Sub(g.S32(partner + 344u), me));
        if (p < d) d = p;                                                      // 0x800BBDC8..0x800BBDD8: the nearer
    }
    return (d < Tune(g, 0x1C)) ? 1 : 0;                                        // 0x800BBDDC
}

int32_t AiStrikeReach(GuestRam& g, uint32_t e, uint32_t t, int32_t gap) {
    if (g.S16(t + 320u) == 0) return 0;                                        // 0x800BC20C
    const uint32_t state = g.U32(g.U32(t + 852u) + 604u);
    if (!(state < 2u)) return 0;                                               // 0x800BC22C
    const uint32_t gs = g.U32(kGameState);
    const bool tPlayer = g.U16(t + 172u) < g.U32(gs + 48u);
    if (!tPlayer && state != 1u) return 0;                                     // 0x800BC254
    if (g.U8(gs + 57u) == 3u && g.U16(t + 172u) < g.U32(gs + 48u)) return 1;   // 0x800BC26C
    int32_t lim = Tune(g, 0x34);                                               // 0x800BC298
    if (g.U16(t + 172u) < Players(g)) lim = Shl(lim, 2);
    int32_t side;
    const uint32_t road = g.U32(t + 360u);
    if (road == g.U32(e + 360u) && ((road >> 16) == 0u || g.U32(t + 336u) == g.U32(e + 336u))) {
        side = Sub(g.S32(t + 344u), g.S32(e + 344u));                          // 0x800BC2F4
        if (g.S32(e + 364u) < 0) side = Neg(side);
    } else {
        int32_t p[3], o[3];                                                    // 0x800BC320
        int16_t ax[3];
        for (uint32_t k = 0; k < 3; ++k) {
            p[k] = g.S32(t + 184u + 4u * k);
            o[k] = g.S32(e + 184u + 4u * k);
            ax[k] = g.S16(e + 432u + 2u * k);
        }
        side = AiProject(p, ax, o);
    }
    const uint32_t partner = g.U32(t + 856u);
    if (partner != 0u && side < 0)                                             // 0x800BC334
        side = Add(side, Add(g.S32(t + 304u), g.S32(partner + 304u)));
    if (lim < Abs(side)) return 0;                                             // 0x800BC36C
    int32_t reach = Tune(g, 0x30);                                             // 0x800BC384
    if (g.U16(t + 172u) < Players(g)) reach = Shl(reach, 2);
    if (reach < Abs(gap)) return 0;                                            // 0x800BC3B0
    const bool ai = (g.U32(e + 560u) & 0x08000000u) != 0u;
    if (ai) {                                                                  // 0x800BC3C4
        const uint32_t cls = g.U8(g.U32(e + 1084u) + 1u) & 0xFu;
        const int32_t k = g.U8(kClassBlocks + 36u * cls + 18u);
        const int32_t v = Div128(Mul(k, g.S32(g.U32(e + 556u) + 224u)));
        if (g.S32(t + 480u) < v) return 0;
    }
    const int32_t a0 = Sub(Abs(g.S32(t + 344u)), LaneHalf(g, t));              // 0x800BC434..0x800BC4A0
    if (ai && g.U16(t + 362u) != 1u && Tune(g, 0x00) < a0) return 0;          // 0x800BC4A4..0x800BC4D4
    return 1;
}

int32_t AiOtherSide(GuestRam& g, uint32_t t, uint32_t handle, int32_t side) {
    uint32_t other = 0;                                                        // t0
    const uint32_t th = g.U16(t + 172u);
    const uint32_t mine = handle & 0xFFFFu;
    bool viaMask = false;
    if (th < Players(g)) {
        const uint32_t m = g.U16(kAttackers + 2u * th);
        if (m != 0u) {                                                         // 0x800BC658
            viaMask = true;
            const uint32_t rest = m & (0xFFFFu - (1u << ((mine - 1u) & 31u)));
            if (rest != 0u && (rest & (0u - rest)) == rest) {
                // mtc2 LZCS / mfc2 LZCR: 31 - leading zeros = the one bit's index; +1 = the handle
                uint32_t bit = 0;
                while ((rest >> bit) != 1u) ++bit;
                other = Bike(g, (bit + 1u) & 0xFFFFu);                         // 0x800BC6C0..0x800BC6E8
            }
        }
    }
    if (!viaMask) {                                                            // 0x800BC6F0
        const uint32_t slot = t + 948u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(t + 946u)));
        const uint32_t op = g.U16(slot);
        if (static_cast<uint32_t>(op - 6u) < 2u || static_cast<uint32_t>(op - 14u) < 4u) {
            const uint32_t tg = g.U16(slot + 2u);
            if (tg != mine) other = Bike(g, tg);                               // 0x800BC728..0x800BC754
        }
    }
    if (other == 0u) return 0;                                                 // 0x800BC758
    if (0x7FFFF < Abs(Gap16(g, t, other))) return 0;                          // 0x800BC760..0x800BC784
    const int32_t rel = RelLateral(g, other, t);                               // 0x800BC78C..0x800BC7AC
    return ((rel ^ side) >> 31) + 1;                                           // 0x800BC7B0..0x800BC7BC
}

// ============================================================================ the aim

bool AiAimBeside(GuestRam& g, uint32_t e, uint32_t t, int32_t offset, uint32_t sp, AiCmdCallees& c) {
    int32_t v = offset;                                                        // sp+40, the argument slot
    if ((g.U16(t + 172u) >> 5) == 0u) {                                        // 0x800BC508: a bike
        const uint32_t partner = g.U32(t + 856u);
        if (partner != 0u) {
            const int32_t dir = g.S32(t + 364u);
            if ((static_cast<uint32_t>(~static_cast<uint32_t>(dir)) >> 31) != (static_cast<uint32_t>(offset) >> 31)) {
                const int32_t w = Add(g.S32(t + 304u), g.S32(partner + 304u)); // 0x800BC544..0x800BC578
                v = (dir < 0) ? Sub(offset, w) : Add(offset, w);
            }
        }
    }
    const uint32_t slice = g.U32(t + 340u);                                    // 0x800BC57C
    const int32_t d0 = Mul(g.S16(slice + 2u), g.S16(e + 872u));
    const int32_t d2 = Mul(g.S16(slice + 6u), g.S16(e + 876u));
    int32_t a0 = Sub(Add(v, g.S32(t + 344u)), Shl(g.S16(e + 878u), 5));        // 0x800BC5A4..0x800BC5BC
    if (Add(Shl(d0, 4) >> 16, Shl(d2, 4) >> 16) < 0) a0 = Neg(a0);             // 0x800BC5C4..0x800BC5E4
    return c.SetAimDelta(e, a0, sp - 32u) && !g.Faulted();                     // 0x800BC600
}

// ============================================================================ the arms

bool AiCmdRideOn(GuestRam& g, uint32_t e, uint32_t cmd, uint32_t sp, AiCmdCallees& c) {
    const uint32_t csp = sp - 56u;
    uint32_t gs = g.U32(kGameState);
    const uint32_t handle = g.U16(e + 172u);
    if (handle < g.U32(gs + 48u) && static_cast<uint32_t>(static_cast<uint8_t>(g.U8(gs + 57u) - 1u)) < 2u) {
        if (!c.JailbreakFinish(csp)) return false;                             // 0x800BAE14
    } else if (0x8000 < g.S32(e + 480u)) {                                     // 0x800BAE24
        const uint32_t cls = g.U8(g.U32(e + 1084u) + 1u) & 0xFu;
        gs = g.U32(kGameState);
        if (!(g.U8(gs + 57u) < 4u) && cls != (g.U8(g.U32(g.U32(kPlayer1) + 1084u) + 1u) & 0xFu)) {
            // 0x800BAE80: ride with the player - 10.0 under own speed, beside it
            int32_t v = Add(g.S32(e + 576u), static_cast<int32_t>(0xFFF60000u));
            g.W32(e + 924u, static_cast<uint32_t>(v));
            if (v < 0) g.W32(e + 924u, 0);
            const uint32_t p = g.U32(kPlayer1);
            const int32_t a1 = g.S32(e + 344u), a0 = g.S32(p + 344u);
            const int32_t rel = ((g.S32(e + 364u) ^ g.S32(p + 364u)) < 0) ? Add(a1, a0) : Sub(a1, a0);
            int32_t w;
            if (g.U32(e + 372u) == 0u || g.U16(e + 362u) != 0u) w = 0x39999;  // 0x800BAEC0
            else w = Mul(g.S32(e + 424u), rel >= 0 ? g.S16(e + 420u) : g.S16(e + 408u));
            if (rel < 0) w = Neg(w);
            w = Sub(w, Shl(g.S16(e + 878u), 5));                               // 0x800BAF28..0x800BAF38
            return c.SetAimDelta(e, w, csp) && !g.Faulted();                   // 0x800BAF44; j 0x800BB260
        }
        // 0x800BAF54: stop
        g.W32(e + 924u, 0);
        int32_t scalar = 0;                                                    // sp+24
        const uint32_t tgt = g.U16(cmd + 2u);
        if (!(handle < g.U32(gs + 48u)) && cls == 2u && tgt != 224u) {
            // 0x800BAF94: a cop - stop at its target's distance
            const uint32_t t = Bike(g, tgt);
            int32_t r1 = 0, r2 = 0, r3 = 0;
            if (!c.CopGap(e, t, csp, r1) || !c.CopGap(e, t, csp, r2) || !c.CopGap(e, t, csp, r3)) return false;
            int32_t s0 = Add(r1 >> 31, r2) ^ (r3 >> 31);
            if (0xFFFF < s0) {
                if (!c.CopGap(e, t, csp, r1) || !c.CopGap(e, t, csp, r2) || !c.CopGap(e, t, csp, r3)) return false;
                s0 = Add(r1 >> 31, r2) ^ (r3 >> 31);
            } else {
                s0 = 0x10000;
            }
            scalar = s0;
            if (g.U32(t + 372u) != 0u) {                                       // 0x800BB03C
                if (g.U32(t + 388u) & 1u) {
                    const uint32_t sl = g.U32(t + 340u);                       // 0x800BB060
                    MulAddView(g, sl + 20u, sl + 14u, g.S32(t + 348u), e + 880u);
                } else {
                    const int32_t unit = (g.S32(t + 364u) < 0) ? static_cast<int32_t>(0xFFFF0000u) : 0x10000;
                    const int32_t k = FixMul(unit, g.S32(e + 308u));           // 0x800BB088
                    MulAddView(g, e + 184u, g.U32(t + 340u) + 14u, k, e + 880u);
                }
            }
        } else {
            // 0x800BB0B0: aim 8.0 x +0x134 ahead along the heading, and hold it
            const int32_t k = FixMul(0x80000, g.S32(e + 308u));
            MulAddView(g, e + 504u, e + 450u, k, e + 880u);
            g.W32(e + 560u, g.U32(e + 560u) | 0x40000u);
        }
        if (g.U32(e + 564u) & 0x200u) g.W32(e + 564u, g.U32(e + 564u) & ~0x200u);   // 0x800BB0E0
        if (!(g.U32(e + 564u) & 0x80000u)) {                                   // 0x800BB0FC
            g.W32(e + 920u, 0);
            g.W32(e + 916u, static_cast<uint32_t>(scalar));
            g.W32(e + 564u, g.U32(e + 564u) & 0xFFF7FFFFu);
        }
    }
    // ---- 0x800BB12C: a freshly issued command plays the finish stance
    const uint32_t r = g.U32(e + 852u);
    if ((g.U16(cmd + 6u) & 0x8000u) && g.S32(r + 604u) == 1) {
        const uint32_t st = g.U16(r + 544u);
        if (!(static_cast<uint32_t>((st - 26u) & 0xFFFFu) < 12u) && g.U16(kKindTable + 8u * st + 2u) != 3u) {
            gs = g.U32(kGameState);
            uint32_t ev = 9;
            const uint32_t rd = g.U32(e + 1084u);
            if (g.U16(e + 172u) < g.U32(gs + 48u) && g.U32(rd + 40u) != 0u && g.U8(rd + 39u) < 4u &&
                g.U8(gs + 57u) == 0u)
                ev = 1;                                                        // 0x800BB1A8..0x800BB1E0
            if (!c.StanceEvent(ev, r, 2, csp)) return false;
        }
    }
    // ---- 0x800BB1F0: a player's camera gets the finish event
    gs = g.U32(kGameState);
    const uint32_t h = g.U16(e + 172u);
    if (h < g.U32(gs + 48u)) {
        const uint32_t v = kViews + kViewStride * h;
        if (!(g.U32(v + 552u) & 0x44u) && g.U8(gs + 4u) != 44u) ViewEvent(g, v, 10);
    }
    return !g.Faulted();
}

bool AiCmdTakeGap(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c) {
    const uint32_t csp = sp - 40u;
    const uint32_t tg = target & 0xFFFFu;
    if (tg == 224u) return true;
    if (g.U32(e + 568u) & 0x600u) return true;                                 // 0x800BB2A8
    const uint32_t t = Bike(g, tg);
    const int32_t gap = Gap16(g, e, t);
    if (!AiGapOk(g, e, t, gap)) return c.PopCommand(e, csp) && !g.Faulted();   // 0x800BB2F4 -> 0x800BB3DC
    if (Tune(g, 0x14) < gap) {                                                 // 0x800BB2FC: well behind - hold
        int32_t v = g.S32(e + 924u);
        if (g.U16(e + 320u) & 2u) v = g.S32(g.U32(e + 556u) + 224u);
        g.W32(e + 924u, static_cast<uint32_t>(v));
        if (g.U32(e + 564u) & 0x80000u) return true;
        const int32_t s = g.S32(e + 924u);
        g.W32(e + 916u, static_cast<uint32_t>(gap));
        g.W32(e + 920u, static_cast<uint32_t>(FixMul(s, s)));
        g.W32(e + 564u, g.U32(e + 564u) & 0xFFF7FFFFu);
        return !g.Faulted();
    }
    // 0x800BB374: the command under the top
    const uint32_t below = e + 940u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(e + 946u)));
    if (g.U16(below) == 7u && g.U16(below + 2u) == tg) {
        const uint32_t top = g.U16(t + 956u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(t + 946u)) - 1));
        g.W16(below, static_cast<uint16_t>(top));
        if (((top - 4u) & 0xFFFFu) < 2u) return c.PopCommand(e, csp) && !g.Faulted();
    }
    g.W32(e + 924u, g.U32(t + 924u));                                          // 0x800BB3EC: follow it
    if (g.U32(e + 564u) & 0x80000u) return !g.Faulted();
    g.W32(e + 916u, g.U32(t + 916u));
    g.W32(e + 564u, g.U32(e + 564u) & 0xFFF7FFFFu);
    g.W32(e + 920u, g.U32(t + 920u));
    return !g.Faulted();
}

bool AiCmdBackOff(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c,
                  const int16_t* sqrtTable) {
    const uint32_t csp = sp - 40u;
    const uint32_t tg = target & 0xFFFFu;
    if (tg == 224u) return true;
    if (g.U32(e + 568u) & 0x600u) return true;                                 // 0x800BB5E4
    const uint32_t t = Bike(g, tg);
    int32_t s3;
    if ((g.U8(g.U32(e + 1084u) + 1u) & 0xFu) == 2u) {                          // 0x800BB644: a cop projects
        int32_t p[3], o[3];
        int16_t ax[3];
        for (uint32_t k = 0; k < 3; ++k) {
            p[k] = g.S32(t + 504u + 4u * k);
            o[k] = g.S32(e + 504u + 4u * k);
            ax[k] = g.S16(e + 528u + 2u * k);
        }
        s3 = AiProject(p, ax, o);
    } else {
        s3 = Gap16(g, e, t);
    }
    if (!AiBackOffOk(g, e, t, s3)) return c.PopCommand(e, csp) && !g.Faulted();   // 0x800BB664
    const int32_t rel = Sub(g.S32(e + 480u), g.S32(t + 480u));                 // 0x800BB67C
    if (s3 < Add(Shl(rel, 1), rel)) {
        const int32_t a2 = RelLateral(g, t, e);                                // 0x800BB6A0
        const uint32_t s4 = g.U16(e + 956u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(e + 946u)) - 2));
        int32_t w = 0x20000;
        if (!(((s4 - 14u) & 0xFFFFFFFFu) < 2u)) w = Add(g.S32(e + 304u), g.S32(t + 304u));   // 0x800BB6E0..0x800BB6FC
        int32_t s0;
        const int32_t f58 = Tune(g, 0x08);
        if (w < a2) {
            s0 = Neg(f58);                                                     // 0x800BB750
        } else if (!(Tune(g, 0x14) < s3)) {
            s0 = f58;
        } else if (!(Neg(w) < a2)) {
            s0 = f58;
        } else {
            const uint32_t sl = g.U32(e + 340u);                               // 0x800BB730
            const uint32_t v1 = static_cast<uint32_t>(static_cast<int32_t>(g.S16(sl + 36u))) >> 31;
            const uint32_t v0 = (0 < g.S32(e + 364u)) ? 1u : 0u;
            s0 = (v1 == v0) ? f58 : Neg(f58);
        }
        if (AiOtherSide(g, t, g.U16(e + 172u), s0) != 0) {                     // 0x800BB76C
            if (static_cast<uint32_t>(s4 - 14u) < 3u) s0 = Neg(s0);
            else s0 = Shl(s0, 1);
        }
        if (!AiAimBeside(g, e, t, s0, csp, c)) return false;                   // 0x800BB79C
    } else if (g.S16(e + 944u) > 0) {                                          // 0x800BB7AC: the slide in flight
        int32_t v[3];
        for (uint32_t k = 0; k < 3; ++k) v[k] = g.S32(e + 892u + 4u * k);
        const int32_t len = Length3(v, sqrtTable);                             // 0x800BB7BC
        if (len < 16) {
            g.W32(e + 908u, 0);
            g.W16(e + 944u, 0);
        } else {
            if (len > 0) g.W32(e + 904u, FixDiv(0x28000u, static_cast<uint32_t>(len)));
            else g.W32(e + 904u, static_cast<uint32_t>(Neg(static_cast<int32_t>(FixDiv(0x28000u, static_cast<uint32_t>(Neg(len)))))));
            int32_t q;
            if (len > 0) q = Shl(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(len), 0x28000u)), 8);
            else q = Shl(Neg(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg(len)), 0x28000u))), 8);
            q = Neg(Add(q, 0x8000));                                           // 0x800BB844..0x800BB84C
            g.W16(e + 944u, static_cast<uint16_t>(q >> 16));
        }
    }
    if (g.U16(e + 320u) & 2u) {                                                // 0x800BB858
        uint32_t f = g.U32(e + 564u);
        if (g.U16(t + 172u) < Players(g)) f |= 0x200u;
        g.W32(e + 564u, f);
        g.W32(e + 924u, g.U32(g.U32(e + 556u) + 224u));
    }
    if (!(g.U32(e + 564u) & 0x80000u)) {                                       // 0x800BB8A0
        const int32_t s = g.S32(e + 924u);
        g.W32(e + 916u, static_cast<uint32_t>(s3));
        g.W32(e + 920u, static_cast<uint32_t>(FixMul(s, s)));
        g.W32(e + 564u, g.U32(e + 564u) & 0xFFF7FFFFu);
    }
    return !g.Faulted();
}

bool AiCmdClose(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c) {
    const uint32_t csp = sp - 32u;
    const uint32_t tg = target & 0xFFFFu;
    if (tg == 224u) return true;
    if (g.U32(e + 568u) & 0x600u) return true;                                 // 0x800BBA3C
    const uint32_t t = Bike(g, tg);
    const int32_t gap = Gap16(g, e, t);
    if (!AiCloseOk(g, e, t, gap)) {                                            // 0x800BBA88
        if (gap < Neg(Tune(g, 0x10))) {                                        // 0x800BBA90: it got away
            const uint32_t rd = g.U32(e + 1084u);
            g.W8(rd + 68u, static_cast<uint8_t>(g.U8(rd + 68u) | 0x10u));
            const uint32_t td = g.U32(t + 1084u);
            g.W8(td + 68u, static_cast<uint8_t>(g.U8(td + 68u) | 0x08u));
        }
        return c.PopCommand(e, csp) && !g.Faulted();
    }
    const int32_t rel = RelLateral(g, t, e);                                   // 0x800BBAE8
    const int32_t s0 = (rel < 0) ? Tune(g, 0x08) : Neg(Tune(g, 0x08));
    const int32_t sh = AiOtherSide(g, t, g.U16(e + 172u), s0);                 // 0x800BBB34
    if (!AiAimBeside(g, e, t, Shl(s0, sh), csp, c)) return false;
    int32_t v = g.S32(e + 924u);                                               // 0x800BBB4C
    if (g.U16(e + 320u) & 2u) {
        const int32_t s = Add(g.S32(g.U32(e + 556u) + 224u), g.S32(e + 480u));
        v = Add(s, static_cast<int32_t>(static_cast<uint32_t>(s) >> 31)) >> 1;
    }
    CommitSpeed(g, e, v);
    return !g.Faulted();
}

bool AiCmdPass(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c) {
    const uint32_t csp = sp - 32u;
    const uint32_t tg = target & 0xFFFFu;
    if (tg == 224u) return true;
    if (g.U32(e + 568u) & 0x600u) return true;                                 // 0x800BBC40
    const uint32_t t = Bike(g, tg);
    if (!AiPassOk(g, e, t, Gap16(g, e, t))) return c.PopCommand(e, csp) && !g.Faulted();   // 0x800BBC80
    const int32_t rel = RelLateral(g, t, e);                                   // 0x800BBCA0
    const int32_t off = (rel > 0) ? Sub(static_cast<int32_t>(0xFFFF0000u), Tune(g, 0x1C)) : Add(Tune(g, 0x1C), 0x10000);
    if (!AiAimBeside(g, e, t, off, csp, c)) return false;                      // 0x800BBCEC
    int32_t v = g.S32(e + 924u);
    if (g.U16(e + 320u) & 2u) v = g.S32(g.U32(e + 556u) + 224u);
    CommitSpeed(g, e, v);
    return !g.Faulted();
}

bool AiCmdPassStrike(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c) {
    const uint32_t csp = sp - 40u;
    const uint32_t tg = target & 0xFFFFu;
    if (tg == 224u) return true;
    const uint32_t t = Bike(g, tg);
    const int32_t gap = Gap16(g, e, t);
    if (AiStrikeReach(g, e, t, gap) && Abs(gap) < Tune(g, 0x20))              // 0x800BBE60..0x800BBE84
        if (!c.Strike(e, t, csp)) return false;
    return AiCmdPass(g, e, tg, csp, c);                                        // 0x800BBE98
}

// ============================================================================ the pass

bool AiCommandPass(GuestRam& g, int32_t dt, uint32_t skip, uint32_t maskB, uint32_t sp, AiCmdCallees& c,
                   const int16_t* sqrtTable) {
    const uint32_t csp = sp - 56u;
    uint32_t e = g.U32(kPool0Ptr);                                             // s0, read once
    for (int32_t i = 0; i < g.S32(kBikeCount); ++i, e += 1096u) {             // the count re-read each turn
        if (g.Faulted()) return false;
        const uint32_t bit = 1u << (static_cast<uint32_t>(i) & 31u);
        if (skip & bit) continue;
        const uint32_t gs = g.U32(kGameState);
        const uint32_t slot = e + 948u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(e + 946u)));
        const uint32_t rider = g.U32(e + 852u);                                // t1 = +0x354
        const uint32_t handle = g.U16(e + 172u);
        const uint32_t players = g.U32(gs + 48u);
        const uint32_t op = g.U16(slot);
        const uint32_t tgt = g.U16(slot + 2u);
        if (!(handle < players) && tgt < players && !(op < 14u) && op < 17u) {   // 0x800BA55C..0x800BA5A8
            const uint32_t m = kAttackers + 2u * tgt;
            g.W16(m, static_cast<uint16_t>(g.U16(m) | (1u << ((handle - 1u) & 31u))));
        }
        if (op < 19u) {
            const uint32_t arm = g.U32(kAiArmTable + 4u * op);
            bool ok = true;
            switch (arm) {
            case kArmNone: break;
            case kArmLeave: ok = c.LeaveRace(e, dt, csp); break;
            case kArmCop:                                                      // 0x800BA5E8
                if (g.U32(rider + 552u) & 0x40u) {
                    ok = c.CopRelease(e, csp);
                } else if (g.U16(slot + 4u) != 0u ||
                           (handle < g.U32(g.U32(kGameState) + 48u) &&
                            (g.U8(g.U32(e + 1084u) + 1u) & 0xFu) == 2u && (g.U32(kRaceFlags) & 0x1Fu) != 0u)) {
                    ok = c.CopIdle(e, slot, dt, csp);                          // 0x800BA65C
                    const uint32_t partner = g.U32(e + 856u);
                    if (ok && partner != 0u && g.U16(slot) == 4u)
                        ok = c.CopIdle(partner,
                                       partner + 948u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(partner + 946u))),
                                       dt, csp);
                } else if (g.U32(rider + 552u) & 0x40u) {                      // 0x800BA6A8
                    ok = c.CopRelease(e, csp);
                }
                break;
            case kArmRideOn: ok = AiCmdRideOn(g, e, slot, csp, c); break;
            case kArmRace:
                if (!(maskB & bit) && (g.U32(e + 560u) & 0x08000000u)) ok = c.CmdRace(e, csp);
                break;
            case kArmGap: ok = AiCmdTakeGap(g, e, tgt, csp, c); break;
            case kArmBack: ok = AiCmdBackOff(g, e, tgt, csp, c, sqrtTable); break;
            case kArmClose: ok = AiCmdClose(g, e, tgt, csp, c); break;
            case kArmPass: ok = AiCmdPass(g, e, tgt, csp, c); break;
            case kArmPassStrike: ok = AiCmdPassStrike(g, e, tgt, csp, c); break;
            case kArmFight: ok = c.Fight(e, tgt, dt, csp); break;
            case kArmIntercept: ok = c.Intercept(e, tgt, csp); break;
            default: return false; // an arm address the overlay does not hold: report, never guess
            }
            if (!ok || g.Faulted()) return false;
        }
        g.W16(slot + 6u, static_cast<uint16_t>(g.U16(slot + 6u) & 0x7FFFu));   // 0x800BA79C
    }
    return !g.Faulted();
}

} // namespace rr::sim
