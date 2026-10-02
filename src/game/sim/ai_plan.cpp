#include "game/sim/ai_plan.h"

#include "game/sim/ai.h"
#include "game/sim/fixed.h"

// Every function below is transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). Comments give the original's addresses. Arithmetic wraps
// in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
// The `sra / addu / xor` absolute value.
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPoolTable    = 0x800CE4D0; // +0 base, +8 -> the slot count
constexpr uint32_t kPool0Ptr     = 0x8005B3A0;
constexpr uint32_t kPlayerBikes  = 0x8005B268;
constexpr uint32_t kPlayer1Ptr   = 0x8005B38C;
constexpr uint32_t kPlayer2Ptr   = 0x8005B21C;
constexpr uint32_t kLiveBikes    = 0x8005B1F8;
constexpr uint32_t kPlanParity   = 0x8005B2A8;
constexpr uint32_t kRaceFlags    = 0x8005AD48;
constexpr uint32_t kDrama        = 0x800CCAC4;
constexpr uint32_t kClaims       = 0x800CCAC0;
constexpr uint32_t kDramaAmt     = 0x800CD52E; // + slot (slots >= 2): 0x800CD530 + slot - 2
constexpr uint32_t kRankGate     = 0x800CD540;
constexpr uint32_t kRankCap      = 0x8005ADC0;
constexpr uint32_t kTauntTab     = 0x8005ADC4;
constexpr uint32_t kGrudgeW      = 0x8005ADCC;
constexpr uint32_t kMoodW        = 0x8005ADD4;
constexpr uint32_t kEventW       = 0x8005ADDC;
constexpr uint32_t kAiIndexOf    = 0x800D38B0;
constexpr uint32_t kHandleOf     = 0x800D38C8;
constexpr uint32_t kClassTab     = 0x80052EE4; // SLUS, 36 bytes per bike class
constexpr uint32_t kCloseGap     = 0x80052F70;
constexpr uint32_t kStanceCat    = 0x800541D4;
constexpr uint32_t kFightRecPtr  = 0x8005AD4C;
constexpr uint32_t kSlot         = 1096;
constexpr uint32_t kRunaway      = 1u << 22; // a scan longer than guest RAM is a runaway

uint32_t Players(GuestRam& g) { return g.U32(g.U32(kGameStatePtr) + 0x30); }
// The top command slot: e + 0x3B4 + 8 * (s8)e[0x3B2].
uint32_t Top(GuestRam& g, uint32_t e) { return e + 0x3B4u + 8u * U(g.S8(e + 0x3B2)); }
uint32_t Rd(GuestRam& g, uint32_t e) { return g.U32(e + 0x43C); }

// The branch-free clamp of a byte into [-15, 15] (0x800B8458.., 0x800B87B4..): `x` is the 32-bit sum,
// the test reads its sign-extended low byte, the store keeps the low byte.
uint8_t Clamp15(uint32_t x) {
    const int32_t b = static_cast<int8_t>(static_cast<uint8_t>(x));
    const uint32_t lo = U((b + 15) >> 31) & (U(-15) - x);
    const uint32_t hi = U((15 - b) >> 31) & (15u - x);
    return static_cast<uint8_t>(x + lo + hi);
}

// R3000 `div` (no break follows it in the original): a zero divisor gives -1 / +1.
int32_t Div(int32_t n, int32_t d) {
    if (d == 0) return n >= 0 ? -1 : 1;
    if (n == INT32_MIN && d == -1) return INT32_MIN;
    return n / d;
}

// AiProject (ai.h) on three guest arrays.
int32_t Project(GuestRam& g, uint32_t p, uint32_t axis, uint32_t org) {
    int32_t pv[3], ov[3];
    int16_t av[3];
    for (uint32_t k = 0; k < 3; ++k) pv[k] = g.S32(p + 4 * k);
    for (uint32_t k = 0; k < 3; ++k) av[k] = g.S16(axis + 2 * k);
    for (uint32_t k = 0; k < 3; ++k) ov[k] = g.S32(org + 4 * k);
    return AiProject(pv, av, ov);
}

void NibbleAdd(GuestRam& g, uint32_t a, int32_t d) {
    uint8_t b = g.U8(a);
    AiNibbleAdd(&b, d);
    g.W8(a, b);
}
void NibbleDecay(GuestRam& g, uint32_t a, int32_t d) {
    uint8_t b = g.U8(a);
    AiNibbleDecay(&b, d);
    g.W8(a, b);
}

// The reach limits' common tail (0x8002064C / 0x800206DC).
int32_t ReachTail(GuestRam& g, int32_t speed, uint32_t cls, int32_t k, uint32_t off, int32_t floor) {
    const int32_t t = FixMul(k, speed);
    const int32_t lim = g.S32(kClassTab + (cls * 9u << 2) + off);
    int32_t d = S(U(lim) - U(FixMul(t, lim)));
    if (!(floor < d)) d = floor;
    return (d < lim) ? d : lim;
}

} // namespace

// ============================================================================ SLUS leaves

int32_t AiReachAhead(GuestRam& g, int32_t speed, uint32_t cls) {
    const int32_t t = FixMul(speed, 0xCCC);                         // 0x800205C0
    const int32_t lim = g.S32(kClassTab + (cls * 9u << 2) + 24);
    int32_t u = FixMul(t, lim);
    if (!(0x300000 < u)) u = 0x300000;                              // 0x80020620
    return (u < lim) ? u : lim;
}

int32_t AiReachBehind(GuestRam& g, int32_t speed, uint32_t cls) {
    return ReachTail(g, speed, cls, 0x666, 28, 0x400000);           // 0x8002064C
}

int32_t AiReachSide(GuestRam& g, int32_t speed, uint32_t cls) {
    return ReachTail(g, speed, cls, 0x8F5, 32, 0x190000);           // 0x800206DC
}

// ============================================================================ RASHCDG leaves

uint32_t AiOwnEvent(GuestRam& g, uint32_t e, uint32_t oldPlace) {
    if (!(g.U16(e + 0xAC) < Players(g)) && (g.U8(Rd(g, e) + 1) & 0xFu) == 2u) return 0;  // 0x800BD2B8
    if (g.U32(kPlanParity) & 1u) return 0;                                            // 0x800BD2EC
    const uint32_t place = g.U8(Rd(g, e) + 0x27);
    if (g.U32(kLiveBikes) < place) return 0;                                          // 0x800BD30C
    if (!(place < 4u) && oldPlace < place) return 2;                                  // 0x800BD314
    if (place < 6u && place < oldPlace) return 4;                                     // 0x800BD330
    return 0;
}

uint32_t AiRelation(GuestRam& g, uint32_t me, uint32_t o) {
    const int32_t dm = g.S8(me + 0x3B2);                            // 0x800BCF94
    if (dm <= 0) return 0;
    const uint32_t mine = me + 8u * U(dm) + 0x3B4u;                 // s2
    uint32_t his = 0;                                               // s1
    const int32_t dh = g.S8(o + 0x3B2);
    if (dh > 0) his = o + 8u * U(dh) + 0x3B4u;
    uint32_t m = 0;
    if (AiHandleInList(g.U16(o + 0xAC), g.U16(Rd(g, me) + 0x40))) m = 0x40;   // 0x800BCFDC
    if (his != 0 && g.U16(his) == 16u && g.U16(his + 2) == g.U16(me + 0xAC) &&
        (g.U16(his + 6) & 0x4000u)) {                               // 0x800BCFF0..0x800BD028
        m |= 1u;
        bool v = true;
        if (g.U16(o + 0xAC) < Players(g)) {                         // 0x800BD044
            if (!(g.U32(o + 0x230) & 0x08000000u) &&
                !AiHandleInList(g.U16(me + 0xAC), g.U16(Rd(g, o) + 0x40))) {
                m &= ~1u;                                           // 0x800BD080
                v = false;
            }
        }
        if (v && (g.U16(mine) != 16u || g.U16(mine + 2) != g.U16(o + 0xAC))) m |= 2u;  // 0x800BD0B4
    }
    if (g.U8(Rd(g, me) + 0x46) == g.U16(o + 0xAC)) m |= 0x1000u;    // 0x800BD0D4
    if (AiHandleInList(g.U16(o + 0xAC), g.U32(me + 0x390))) m |= 0x80u;       // 0x800BD0F0
    if (his != 0) {
        const uint32_t op = g.U16(his);
        if ((op == 10u || op == 14u || op == 15u || op == 13u) && g.U16(his + 2) == g.U16(me + 0xAC) &&
            (g.U16(his + 6) & 0x4000u)) {
            m |= 4u;                                                // 0x800BD160
            if (!((g.U16(mine) - 10u) < 7u) || g.U16(mine + 2) != g.U16(o + 0xAC)) m |= 8u;
        }
    }
    if (AiHandleInList(g.U16(o + 0xAC), g.U16(Rd(g, me) + 0x42))) m |= 0x100u;  // 0x800BD198
    if (g.U16(mine) == 16u && g.U16(mine + 2) == g.U16(o + 0xAC) && (g.U16(mine + 6) & 0x4000u))
        m |= 0x10u;                                                 // 0x800BD1D4
    {
        const uint32_t op = g.U16(mine);
        if ((op == 10u || op == 13u || op == 14u || op == 15u) && g.U16(mine + 2) == g.U16(o + 0xAC) &&
            (g.U16(mine + 6) & 0x4000u))
            m |= 0x20u;                                             // 0x800BD228
    }
    if (g.U8(Rd(g, me) + 0x47) == g.U16(o + 0xAC)) m |= 0x800u;     // 0x800BD248
    if (his != 0 && g.U16(his) == 0u && (g.U16(his + 6) & 0x4000u)) m |= 0x400u;  // 0x800BD278
    return m;
}

int32_t AiReachable(GuestRam& g, uint32_t h, uint32_t e) {
    const int32_t ahead = AiReachAhead(g, g.S32(e + 0x1E0), g.U8(Rd(g, e) + 1) & 0xFu);   // 0x800BEA5C
    const int32_t behind = AiReachBehind(g, g.S32(e + 0x1E0), g.U8(Rd(g, e) + 1) & 0xFu); // 0x800BEA74
    int32_t along;
    if (g.U8(e + 0x3A0) & 1u) {                                     // 0x800BEA88: on the road graph
        along = S((U(g.S32(e + 0x144)) - U(g.S32(h + 0x98))) << 4);
        if (g.U8(Rd(g, e)) & 0x80u) along = S(0u - U(along));
    } else {
        along = Project(g, h + 0x0C, e + 0x210, e + 0x1F8);
    }
    if (ahead < along) return 0;                                    // 0x800BEAD0
    if (along < S(0u - U(behind))) return 0;
    int32_t lat;
    if (g.U8(e + 0x3A0) & 1u) {                                     // 0x800BEAE8
        const uint32_t key = g.U32(h + 0xBC);
        if (key == g.U32(e + 0x168) && ((key >> 16) == 0u || g.U32(h + 0xA4) == g.U32(e + 0x150)))
            lat = S(U(g.S32(h + 0xAC)) - U(g.S32(e + 0x158)));    // 0x800BEB30
        else
            lat = Project(g, h + 0x0C, e + 0x1B0, e + 0xB8);
    } else {
        lat = Project(g, h + 0x0C, e + 0x204, e + 0x1F8);           // 0x800BEB48
    }
    lat = Abs(lat);
    const int32_t side = AiReachSide(g, g.S32(e + 0x1E0), g.U8(Rd(g, e) + 1) & 0xFu);     // 0x800BEB78
    return (side < lat) ? 0 : 1;
}

bool AiWeaponPick(GuestRam& g, uint32_t rd) {
    const uint32_t cmd = g.U8(rd + 0x3C);                           // 0x800B9340
    if (!(cmd & 0x80u)) {
        if (cmd & 0x20u) {                                          // 0x800B93FC
            g.W8(rd + 0x2E, 9);
            g.W8(rd + 0x2F, 0);
        }
        return !g.Faulted();
    }
    if ((g.U16(rd + 0x2C) & 0x1FFu) == 0u) {                        // 0x800B93E4: no weapon
        g.W8(rd + 0x2E, 9);
        g.W8(rd + 0x2F, 0);
        g.W8(rd + 0x3C, 32);
        return !g.Faulted();
    }
    const uint32_t row = kClassTab + 8u + (((g.U8(rd + 1) & 0xFu) * 9u) << 2);  // 0x80052EEC + 36 cls
    const uint32_t owned = g.U16(rd + 0x2C);
    uint32_t w = 0;
    for (uint32_t k = 0;; ++k) {                                    // 0x800B9390
        if (k > kRunaway || g.Faulted()) return false;
        w = g.U8(row + k);
        g.W8(rd + 0x2E, static_cast<uint8_t>(w));
        if ((S(owned) >> (w & 31u)) & 1) break;
    }
    const uint32_t lvl = (w < 8u) ? ((g.U32(rd + 0x30) >> ((w << 2) & 31u)) & 0xFu) : 0u;   // 0x800B93B8
    g.W8(rd + 0x2F, static_cast<uint8_t>(lvl));
    return !g.Faulted();
}

bool AiTaunt(GuestRam& g, uint32_t rd) {
    const uint32_t r = GuestRand(g) & 0x7Fu;                        // 0x800B92DC
    uint32_t sum = 0, k = 0;
    for (;; ++k) {                                                  // 0x800B92F4
        if (k > kRunaway || g.Faulted()) return false;
        sum += g.U8(kTauntTab + k);
        if (!(sum < r)) break;
    }
    g.W8(rd + 0x3C, g.U8(rd + 0x34 + k));                           // 0x800B9314
    return AiWeaponPick(g, rd);
}

// ============================================================================ the planner

bool AiChooseCommand(GuestRam& g, uint32_t e, int32_t aggr, uint32_t t, uint32_t sp, AiPlanCallees& c,
                     uint32_t& v0) {
    const uint32_t fsp = sp - 48u;                                  // 0x800B8FB0
    const uint32_t cmd = fsp + 16u;
    v0 = 0;
    const int32_t a = static_cast<int8_t>(static_cast<uint8_t>(aggr));
    const uint32_t rd = Rd(g, e);
    const uint32_t row = kClassTab + (((g.U8(rd + 1) & 0xFu) * 9u) << 2);
    const int32_t lateral = S((U(g.S32(e + 0x144)) - U(g.S32(t + 0x144))) << 4);   // 0x800B902C
    uint32_t idx = (a >= g.S8(row + 4) ? 1u : 0u) + (a >= g.S8(row + 5) ? 1u : 0u);
    if (!((g.U8(rd + 2) & 0xFu) < g.U8(row + 17))) idx |= 4u;       // 0x800B904C: the mood gate
    const uint32_t top = Top(g, e);                                 // s3, before any callee
    if (g.Faulted()) return false;
    uint32_t r = 0;
    switch (idx) {                                                  // the table at 0x8005B998
        case 0: case 4:                                             // 0x800B91A4
            if (!c.CanEngage(e, t, lateral, fsp, r)) return false;
            if (r != 0 && Abs(lateral) < g.S32(kCloseGap)) {
                g.W16(cmd, 9);
            } else {
                if (!c.ChaseTest(e, t, lateral, fsp, r)) return false;
                if (r == 0) return !g.Faulted();
                g.W16(cmd, 8);
            }
            break;
        case 1: case 2: case 5:                                     // 0x800B9174
            if (!c.CanEngage(e, t, lateral, fsp, r)) return false;
            if (r == 0) return !g.Faulted();
            g.W16(cmd, 16);
            if (!AiTaunt(g, Rd(g, e))) return false;
            break;
        case 6: {                                                   // 0x800B908C
            if (!c.CanEngage(e, t, lateral, fsp, r)) return false;
            if (r == 0) return !g.Faulted();
            if (g.U16(t + 0xAC) < Players(g))
                if (!c.RiderVoice(g.U16(e + 0xAC), 0, fsp)) return false;
            g.W16(cmd, 16);
            const uint32_t d = Rd(g, e);
            if (g.U16(d + 0x2C) & 0x1FFu) {                         // 0x800B90E8
                g.W8(d + 0x3C, 143);
                for (int32_t k = 0; k < 8; ++k) {
                    const uint32_t dk = Rd(g, e);
                    const uint8_t w = g.U8(dk + 0x34u + U(k));
                    if (g.U8(dk + 0x3C) < w) g.W8(dk + 0x3C, w);
                }
                if (!AiWeaponPick(g, Rd(g, e))) return false;
            } else {                                                // 0x800B9150: fists
                g.W8(d + 0x2E, 9);
                g.W8(Rd(g, e) + 0x2F, 0);
                g.W8(Rd(g, e) + 0x3C, 38);
            }
            break;
        }
        default:                                                    // 3, and the >= 7 test
            return !g.Faulted();
    }
    const uint32_t th = g.U16(t + 0xAC);                            // 0x800B91F4
    if (g.U16(top + 2) != th) {
        g.W16(cmd + 2, static_cast<uint16_t>(th));                  // 0x800B9294
        if (g.Faulted()) return false;
        int32_t pv = 0;
        if (!c.PushCommand(cmd, 2, e, fsp, pv)) return false;
        v0 = U(pv);
        return !g.Faulted();
    }
    const uint32_t cur = g.U16(top);
    if (cur == 6u) return !g.Faulted();                             // 0x800B9210
    if (cur == 16u && g.U16(cmd) != 16u) {                          // 0x800B9224: leaving a fight
        const uint32_t rider = g.U32(e + 0x354);
        if (g.U16(kStanceCat + 8u * g.U16(rider + 0x220) + 2u) == 3u) {
            const uint32_t ev = g.U16(g.U32(kFightRecPtr) + 12u * g.U8(rider + 0x239));
            if (g.Faulted()) return false;
            if (!c.StanceEvent(ev, rider, 2, fsp)) return false;
        }
    }
    g.W16(top, g.U16(cmd));                                         // 0x800B9290
    v0 = 1;
    return !g.Faulted();
}

bool AiPlan(GuestRam& g, int32_t acc, uint32_t sp, AiPlanCallees& c) {
    const uint32_t fsp = sp - 96u;                                  // 0x800B8020
    uint32_t near = 0;                                              // sp+32
    if (g.U32(g.U32(kGameStatePtr) + 0x30) == 2u) {                 // 0x800B805C: head to head
        const uint32_t p1 = g.U32(kPlayer1Ptr), p2 = g.U32(kPlayer2Ptr);
        const int32_t gap = S(U(g.S32(p1 + 0x144) >> 12) - U(g.S32(p2 + 0x144) >> 12));
        const bool far = !(Abs(gap) < 825);
        const uint32_t d = g.U32(kDrama);
        g.W16(fsp + 40, static_cast<uint16_t>(U(gap) >> 31));      // 0x800B80B0
        if (!(d & 1u) && far) g.W32(kDrama, d | 0x11u);
        const uint32_t d2 = g.U32(kDrama);
        if (!far && (d2 & 1u)) g.W32(kDrama, (d2 & ~1u) | 0x40u);  // 0x800B80E0
    }
    const uint32_t base = g.U32(kPoolTable);                        // 0x800B80F8
    const int32_t count = g.S32(g.U32(kPoolTable + 8));
    if (g.Faulted()) return false;

    // ---- loop 1: the rank by proximity score (0x800B8120..0x800B81E0)
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t ri = base + kSlot * U(i) + 0x353u;
        g.W8(ri, 0);
        const uint32_t cap = g.U16(kRankCap + ((Players(g) - 1u) << 1));
        if (!(cap < g.U16(kRankGate))) continue;
        if (!(g.U16(ri - 531u) & 0x30u)) continue;                  // e+0x140
        for (int32_t j = 0; j < count; ++j) {
            const uint32_t sj = base + kSlot * U(j) + 0x352u;
            uint32_t inc = 0;
            if (g.U16(sj - 530u) & 0x30u) inc = (g.U8(sj) < g.U8(ri - 1u)) ? 1u : 0u;
            g.W8(ri, static_cast<uint8_t>(g.U8(ri) + inc));
        }
        if (g.Faulted()) return false;
    }

    // ---- loop 2: per bike (0x800B81F4..0x800B8A38). `e` is the original's s4: the partner chain
    // moves it, and the next slot is taken from wherever it ended.
    uint32_t e = base;
    for (int32_t s7 = 0; s7 < count; ++s7, e += kSlot) {
        if (g.Faulted()) return false;
        if (e == 0) continue;                                       // 0x800B81F4
        if (!(g.U16(e + 0xAC) < Players(g) || (g.U8(Rd(g, e) + 1) & 0xFu) != 2u)) {
            // a cop bike that is not a player: planned whatever its place
        } else {
            const uint32_t d = Rd(g, e);                            // 0x800B8234
            if (g.U32(d + 0x28) != 0u) continue;                    // finished
            if (!(g.U8(d + 0x27) < 248u)) continue;                 // jailed / escaped
        }
        {
            const uint32_t partner = g.U32(e + 0x358);              // 0x800B8260
            if (partner != 0u && g.U32(e + 0x440) != 0u) e = partner;
        }
        uint32_t rd = 0;                                            // s5
        for (uint32_t hop = 0;; ++hop) {                            // 0x800B8284: health regeneration
            if (hop > kRunaway || g.Faulted()) return false;
            rd = Rd(g, e);
            if (g.U8(rd + 15) < g.U8(rd + 14)) {
                const uint32_t st = g.U32(g.U32(e + 0x354) + 604);
                uint32_t k = 0;
                if (st < 3u) k = 2;
                else if (st == 4u) k = 4;
                if (k != 0) {
                    const uint32_t cur = g.U8(rd + 15), mx = g.U8(rd + 14);
                    g.W8(rd + 15, static_cast<uint8_t>(S(cur + k) < S(mx) ? cur + k : mx));
                }
            }
            if (g.U32(e + 0x440) != 0u) break;                      // 0x800B8308
            e = g.U32(e + 0x358);
        }
        const uint32_t oldPlace = g.U8(Rd(g, e) + 0x27);            // s2
        if (!(g.U32(kPlanParity) & 1u)) {                           // 0x800B8338: the place recompute
            int32_t place = 0;
            if (!c.ComputePlace(e, 0, fsp, place)) return false;
            g.W8(Rd(g, e) + 0x27, static_cast<uint8_t>(place));
            g.W8(e + 0x3A0, g.U8(e + 0x3A0) | 8u);
        }
        g.W8(rd, g.U8(rd) & 0xF7u);                                 // 0x800B8364
        if (s7 >= 2) {                                              // 0x800B8374: the drama
            const uint32_t d = g.U32(kDrama);
            if (d & 0xF0u) {
                const uint32_t amt = kDramaAmt + U(s7);
                if (d & 0x10u) {
                    const uint32_t k = g.U16(fsp + 40) ^ 1u;        // the frame word, stale on 1P
                    const int32_t v = 15 - g.S8(Rd(g, e) + k + 16);
                    g.W8(amt, static_cast<uint8_t>(v < 8 ? v : 8));
                    const uint32_t a = Rd(g, e) + (g.U16(fsp + 40) ^ 1u) + 16u;
                    g.W8(a, static_cast<uint8_t>(g.U8(a) + g.U8(amt)));
                }
                if (g.U32(kDrama) & 0x40u) {                        // 0x800B8414
                    const uint32_t a = Rd(g, e) + (g.U16(fsp + 40) ^ 1u) + 16u;
                    g.W8(a, static_cast<uint8_t>(g.U8(a) - g.U8(amt)));
                }
                const uint32_t r16 = Rd(g, e) + 16u;
                g.W8(r16, Clamp15(g.U8(r16)));
                const uint32_t r17 = Rd(g, e) + 17u;
                g.W8(r17, Clamp15(g.U8(r17)));
            }
        }
        if (g.S16(e + 0x140) == 0) continue;                        // 0x800B84D4: dormant
        if (!(g.U16(e + 0xAC) < Players(g)) && (g.U8(Rd(g, e) + 1) & 0xFu) == 2u &&
            !(g.U8(e + 0x3A0) & 0x10u))
            continue;                                               // 0x800B8528
        {
            const uint32_t gs = g.U32(kGameStatePtr);               // 0x800B8530
            if ((g.U8(gs + 4) & 1u) && g.U16(e + 0xAC) < g.U32(gs + 0x30)) {
                if (!c.CopArrestScan(e, fsp)) return false;
                if (g.U32(Rd(g, e) + 0x28) != 0u) continue;
            }
        }
        if ((g.U32(e + 0x230) & 0x18000000u) != 0x08000000u) continue;   // 0x800B8594: AI-driven
        if (g.U32(kPlanParity) == 0u) continue;
        // mood decay (0x800B85AC)
        const uint32_t accw = g.U32(rd + 4) + U(acc);
        const int32_t n = Div(S(accw) >> 16, static_cast<int32_t>(g.U8(rd + 3)));
        const uint32_t top = Top(g, e);                             // s8
        g.W32(rd + 4, accw);
        if (n != 0) NibbleDecay(g, rd + 2, n);                      // 0x800BCEEC
        {
            const uint32_t per = U(static_cast<int32_t>(g.U8(rd + 3))) << 16;
            const int32_t w = g.S32(rd + 4);
            if (S(per) < w) g.W32(rd + 4, U(w) - per);
        }
        // the own event (0x800B8610)
        int32_t mood = 0;                                           // s3
        const uint32_t ev = AiOwnEvent(g, e, oldPlace) | g.U8(rd + 0x44);
        g.W8(rd + 0x44, 0);
        {
            const uint32_t w = g.U32(kEventW);
            for (uint32_t b = 0; b < 7u; ++b)
                if (ev & (1u << b)) mood += S(w << ((b << 2) & 31u)) >> 28;
        }
        if (ev & 0x20u) {                                           // 0x800B8660
            const uint32_t v = g.U8(rd + 0x3D);
            const uint32_t hi = v >> 4;
            if (!(hi < 2u)) g.W8(rd + 0x3D, static_cast<uint8_t>((v & 0xFu) | ((hi - 1u) << 4)));
        }
        if (ev & 0x40u) {                                           // 0x800B8698
            const uint32_t v = g.U8(rd + 0x3D);
            const uint32_t hi = v >> 4;
            if (!(hi < 3u)) g.W8(rd + 0x3D, static_cast<uint8_t>((v & 0xFu) | ((hi - 2u) << 4)));
            else if (!(hi < 2u)) g.W8(rd + 0x3D, static_cast<uint8_t>((v & 0xFu) | ((hi - 1u) << 4)));
        }
        // the relation scan (0x800B86E0..0x800B8810)
        for (int32_t s2 = 0; s2 < count; ++s2) {
            const uint32_t o = base + kSlot * U(s2);
            if (s7 == s2 || o == 0u) continue;
            const uint32_t rel = AiRelation(g, e, o);
            if (rel == 0u) continue;
            int32_t grudge = 0;                                     // a3
            for (uint32_t b = 0; b < 13u; ++b) {
                if (!(rel & (1u << b))) continue;
                const uint32_t sh = (b & 7u) << 2, wi = (b >> 3) << 2;
                mood += S(g.U32(kMoodW + wi) << sh) >> 28;
                grudge += S(g.U32(kGrudgeW + wi) << sh) >> 28;
            }
            if (grudge != 0) {
                const uint32_t a = rd + g.U8(kAiIndexOf + g.U16(o + 0xAC)) + 16u;
                g.W8(a, Clamp15(g.U8(a) + U(grudge)));
            }
            if (g.Faulted()) return false;
        }
        if (mood != 0) NibbleAdd(g, rd + 2, mood);                  // 0x800BD34C
        if ((g.U16(top) - 10u) < 6u && AiHandleInList(g.U16(top + 2), g.U32(e + 0x390)))
            NibbleAdd(g, rd + 0x3D, 1);                             // 0x800B8850: the combo counter
        if (!(g.U16(e + 0xAC) < Players(g)) && (g.U8(Rd(g, e) + 1) & 0xFu) == 2u) {
            if (!c.CopTail(e, fsp)) return false;                   // 0x800B8894
            continue;
        }
        if (!((g.U16(top) - 3u) < 14u)) continue;                   // 0x800B88AC
        if (g.U32(Rd(g, e) + 0x28) != 0u) continue;
        const int32_t d1 = S(U(g.S32(e + 0x144)) - U(g.S32(g.U32(kPlayer1Ptr) + 0x144)));
        int32_t gap = d1 >> 12;                                     // s0
        if (g.U32(g.U32(kGameStatePtr) + 0x30) == 2u) {             // 0x800B88F8
            const uint32_t s1 = U(d1 >> 31);
            gap = S((s1 + U(gap)) ^ s1);
            const int32_t d2 = S(U(g.S32(e + 0x144)) - U(g.S32(g.U32(kPlayer2Ptr) + 0x144)));
            const uint32_t s2 = U(d2 >> 31);
            const int32_t v = S((s2 + U(d2 >> 12)) ^ s2);
            if (v < gap) gap = v;
        }
        const uint32_t np = Players(g);
        const uint32_t cap = g.U16(kRankCap + ((np - 1u) << 1));
        if ((U(gap) + 63u) < 75u) {                                 // 0x800B8940: near a player
            if (g.U16(top) != 4u && g.U16(top + 2) < Players(g)) continue;
            if (g.U8(e + 0x353) < cap || g.U16(e + 0xAC) < np) {
                near |= 1u << (U(s7) & 31u);                        // 0x800B89C4
                continue;
            }
        } else if (g.U8(e + 0x353) < cap) {                         // 0x800B89DC
            continue;
        }
        g.W8(rd, g.U8(rd) | 8u);                                    // 0x800B8A14
    }
    if (g.Faulted()) return false;

    // ---- cop mode: a busting player bike blanks the mask (0x800B8A3C..0x800B8ADC)
    {
        const uint32_t gs = g.U32(kGameStatePtr);
        if ((g.U8(gs + 4) & 1u) && (g.U32(kRaceFlags) & 0x1Fu)) {
            const int32_t np = g.S32(gs + 0x30);
            for (int32_t p = 0; p < np; ++p) {
                const uint32_t b = g.U32(kPlayerBikes + 4u * U(p));
                if ((g.U8(Rd(g, b) + 1) & 0xFu) == 2u && (g.U32(b + 0x230) & 0x08000000u)) near = 0;
            }
        }
    }

    // ---- loop 3 (0x800B8AF8)
    e = base;
    for (int32_t i = 0; i < count; ++i, e += kSlot) {
        const uint32_t rd = Rd(g, e);
        g.W32(e + 0x390, 0);
        g.W16(rd + 0x40, 0);
        g.W16(rd + 0x42, 0);
        g.W8(rd + 0x47, 31);
        g.W8(rd + 0x46, 31);
        const uint32_t top = Top(g, e);
        g.W16(top + 6, static_cast<uint16_t>(g.U16(top + 6) & 0xBFFFu));
        if (g.Faulted()) return false;
    }

    // ---- loop 4: issue commands (0x800B8B5C..0x800B8D10)
    e = base;
    for (int32_t s7 = 0; s7 < count; ++s7, e += kSlot) {
        if (!((S(near) >> (U(s7) & 31u)) & 1)) continue;
        const uint32_t rd = Rd(g, e);
        uint32_t best = U(-15);                                     // t0
        uint32_t bestIdx = 19;                                      // s3
        for (uint32_t k = 0; k < 20u; ++k) {
            if (!(static_cast<int8_t>(static_cast<uint8_t>(best)) < g.S8(rd + 16u + k))) continue;
            const uint32_t h = g.U8(kHandleOf + k);
            if (h == g.U16(e + 0xAC) || h == 31u) continue;
            const uint32_t t = g.U32(kPool0Ptr) + h * kSlot;
            if (!AiReachable(g, t + 0xAC, e)) continue;             // 0x800BEA30
            best = g.U8(rd + 16u + k);
            bestIdx = h;
        }
        if (g.Faulted()) return false;
        if (!(S(bestIdx) < g.S32(kLiveBikes))) continue;            // 0x800B8C1C
        const uint32_t t = g.U32(kPool0Ptr) + bestIdx * kSlot;
        const uint32_t gs = g.U32(kGameStatePtr);
        const uint32_t th = g.U16(t + 0xAC);
        bool allow = true;
        if (th < g.U32(gs + 0x30)) {                                // 0x800B8C58: the target is a player
            const uint32_t mode = g.U8(gs + 4);
            if (mode != 36u && mode != 44u) {
                const uint32_t bit = 1u << ((g.U16(e + 0xAC) - 1u) & 31u);
                const uint32_t others = g.U16(kClaims + (bestIdx << 1)) & (0xFFFFu - bit);
                allow = ((others & (0u - others)) == others) || S(others) < 1;   // at most one other
                const uint32_t ca = kClaims + (th << 1);
                g.W16(ca, static_cast<uint16_t>(g.U16(ca) & ~bit));
            }
        }
        if (allow) {
            uint32_t v0 = 0;
            if (!AiChooseCommand(g, e, static_cast<int8_t>(static_cast<uint8_t>(best)), t, fsp, c, v0))
                return false;
        }
    }

    // ---- loop 5: a player under attack - his partner is sent at the attacker (0x800B8D44..)
    if (Players(g) != 0u) {
        uint32_t s7 = 0;
        do {
            const uint32_t gs = g.U32(kGameStatePtr);
            const uint32_t pb = g.U32(kPlayerBikes + 4u * s7);
            const uint32_t claimAddr = kClaims + 2u * s7;
            if (g.U8(gs + s7 + 10u) != 2u) continue;
            const uint32_t partner = g.U32(pb + 0x358);
            if (!(g.U32(g.U32(partner + 0x354) + 604) < 2u)) continue;
            if (g.U16(claimAddr) == 0u) {                           // 0x800B8D94
                if (g.U32(gs + 0x30) < 2u) continue;
                const uint32_t ob = g.U32(kPlayerBikes + (s7 == 0u ? 4u : 0u));
                const uint32_t otop = Top(g, ob);
                if (g.U16(otop) != 16u) continue;
                if (g.U16(otop + 2) != g.U16(pb + 0xAC)) continue;
            }
            if (g.U16(Top(g, partner)) == 16u) continue;            // 0x800B8E18
            uint32_t mask = g.U16(claimAddr);                       // s2
            uint32_t s3 = 1;
            bool first = mask == 0u;
            if (first) s3 = (s7 == 0u) ? 1u : 0u;
            while (first || mask != 0u) {
                if (!first && !((S(mask) >> ((s3 - 1u) & 31u)) & 1)) {
                    ++s3;
                    continue;
                }
                first = false;
                const uint32_t o = g.U32(kPool0Ptr) + s3 * kSlot;   // 0x800B8E8C
                const uint32_t key = g.U32(o + 0x168);
                int32_t gap;
                if (key == g.U32(pb + 0x168) && ((key >> 16) == 0u || g.U32(o + 0x150) == g.U32(pb + 0x150))) {
                    gap = S(U(g.S32(o + 0x158)) - U(g.S32(pb + 0x158)));
                    if (g.S32(pb + 0x16C) < 0) gap = S(0u - U(gap));
                } else {
                    gap = Project(g, o + 0xB8, pb + 0x1B0, pb + 0xB8);
                }
                if (g.Faulted()) return false;
                if (gap > 0) {                                      // 0x800B8E38
                    if (!AiTaunt(g, Rd(g, g.U32(pb + 0x358)))) return false;
                    g.W16(fsp + 16, 16);
                    g.W16(fsp + 18, static_cast<uint16_t>(s3));
                    if (g.Faulted()) return false;
                    int32_t pv = 0;
                    if (!c.PushCommand(fsp + 16, 2, g.U32(pb + 0x358), fsp, pv)) return false;
                    break;
                }
                mask &= 0xFFFFu - (1u << ((s3 - 1u) & 31u));
                ++s3;
            }
            if (g.Faulted()) return false;
        } while (++s7 < Players(g));
    }

    // ---- epilogue (0x800B8F5C)
    g.W16(kClaims + 2, 0);
    g.W16(kClaims, 0);
    g.W32(kDrama, g.U32(kDrama) & ~0xF0u);
    return !g.Faulted();
}

} // namespace rr::sim
