#include "game/sim/cops.h"

#include "game/sim/ai.h"       // Length3 SLUS 0x8002E548
#include "game/sim/ai_cmd.h"   // AiBackOffOk 0x800BB8FC, AiStrikeReach 0x800BC1EC, kAiTune
#include "game/sim/ai_plan.h"  // AiTaunt 0x800B92C0
#include "game/sim/coll_util.h"

// Every function below is transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). Comments give the original's addresses. Arithmetic wraps
// in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

using cu::Add;
using cu::Iabs;
using cu::Neg;
using cu::S;
using cu::Sub;
using cu::U;

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPool0Ptr     = 0x8005B3A0;
constexpr uint32_t kPlayerBikes  = 0x8005B268; // [players]
constexpr uint32_t kPlayer1Ptr   = 0x8005B38C;
constexpr uint32_t kLiveBikes    = 0x8005B1F8;
constexpr uint32_t kRaceFlags    = 0x8005AD48;
constexpr uint32_t kPoolTable    = 0x800CE4D0; // +0 base, +4 stride, +12 -> the high index
constexpr uint32_t kJailTab      = 0x800530E4; // s32 by bank: the cop-mode quota time
constexpr uint32_t kJailRecords  = 0x800D6198; // 224 bytes per handle
constexpr uint32_t kFarTab       = 0x80053182; // u16: the race-type-44 distance (x 65536)
constexpr uint32_t kFarGate      = 0x8005ADF0; // s8
constexpr uint32_t kViewArray    = 0x800CD898;
constexpr uint32_t kViewStride   = 1132;
constexpr uint32_t kSlot         = 1096;

uint32_t Gs(GuestRam& g) { return g.U32(kGameStatePtr); }
uint32_t Rd(GuestRam& g, uint32_t e) { return g.U32(e + 1084u); }
// The top command slot's opcode: e + 0x3BC + 8 * (depth - 1).
uint32_t TopOp(GuestRam& g, uint32_t e) { return g.U16(e + 956u + 8u * U(g.S8(e + 946u) - 1)); }
// *(0x800530A8 + 4 * (bank + 3 * (race type & 1))), the game-state pointer re-read.
int32_t NearReach(GuestRam& g) {
    const uint32_t gs = Gs(g);
    const uint32_t k = (0u - (g.U8(gs + 4u) & 1u)) & 3u;
    return g.S32(kCopNearTab + ((g.U32(gs + 60u) + k) << 2));
}
// The octagonal distance the three functions below inline: max - max/32 - max/128 + 3/8 min + 3/128 min.
int32_t Octagon(int32_t a, int32_t b) {
    int32_t hi = Iabs(a), lo = Iabs(b);
    if (hi < lo) {
        const int32_t t = hi;
        hi = lo;
        lo = t;
    }
    const int32_t m = Add(lo, lo >> 1);
    return Add(Add(Sub(Sub(hi, hi >> 5), hi >> 7), m >> 2), m >> 6);
}

} // namespace

// ============================================================================ the leaves

int32_t CopDist(GuestRam& g, uint32_t a, uint32_t b, uint32_t sp) {
    const uint32_t f = sp - 16u;                                     // 0x8009E444
    int32_t v = 0;
    if (a == 0u) return 0;
    if (b == 0u) return 0;
    {
        const uint32_t r = g.U32(b + 852u);
        if ((g.U32(r + 604u) - 3u) < 2u) b = r;                      // 0x8009E46C: off the bike
    }
    const uint32_t x = g.U32(b + 184u);
    g.W32(f + 0, x);
    const uint32_t y = g.U32(b + 188u);
    g.W32(f + 4, y);
    const uint32_t z = g.U32(b + 192u);
    g.W32(f + 8, z);
    const int32_t dx = Sub(g.S16(a + 186u), S(x) >> 16);
    const int32_t dz = Sub(g.S16(a + 194u), S(z) >> 16);
    v = Octagon(dx, dz);
    return Iabs(cu::Shl(v, 16));                                     // 0x8009E510
}

int32_t CopTargetOk(GuestRam& g, uint32_t e, uint32_t t) {
    const uint32_t rd = Rd(g, t);                                    // 0x8009DB58
    if (g.U32(rd + 40u) != 0u) return 0;
    if (!(g.U32(g.U32(e + 852u) + 604u) < 3u)) return 0;
    return g.U8(rd + 39u) < 247u ? 1 : 0;
}

int32_t CopStopTest(GuestRam& g, uint32_t e, uint32_t t, int32_t dist, int32_t stop) {
    const uint32_t r = g.U32(t + 852u);                              // 0x8009DBA0
    const bool seated = g.U32(r + 604u) < 3u;                        // t1
    if (seated && S(kCopSlowSpeed) < g.S32(t + 480u)) return 0;
    const int32_t d = Iabs(Sub(dist, stop));                         // 0x8009DBD4
    if (NearReach(g) < d) return 0;
    if (g.U32(Rd(g, e) + 40u) != 0u) return 0;                       // 0x8009DC24
    const int32_t speed = g.S32(e + 480u);
    if (!(S(kCopSlowSpeed) < speed)) return 1;                       // 0x8009DC48
    if (seated) return 0;
    if (!((g.U16(r + 544u) - 72u) < 2u)) return 0;                   // 0x8009DC58: stance 72 / 73
    if (g.S32(r + 480u) < speed) return 0;
    return 1;
}

int32_t CopAhead(GuestRam& g, uint32_t e, uint32_t t) {
    const uint32_t r = g.U32(t + 852u);                              // 0x8009E3F0
    const int32_t along = (g.U32(r + 604u) < 3u) ? g.S32(t + 368u) : g.S32(r + 368u);
    if (g.S32(t + 364u) > 0) return along < g.S32(e + 368u) ? 1 : 0;
    return g.S32(e + 368u) < along ? 1 : 0;
}

void CopWantedBits(GuestRam& g, uint32_t list) {
    uint32_t a = list & 0xFFFFu;                                     // 0x8009FC80
    if (a == 0u) return;
    const int32_t n = g.S32(kLiveBikes);
    do {
        const uint32_t f = a & 31u;
        if (f != 0u) {
            const int32_t h = S(f) - 1;
            if (h >= 0 && h < n) g.W32(kCopWantedMask, g.U32(kCopWantedMask) | (1u << (U(h) & 31u)));
        }
        a >>= 5;
    } while (a != 0u);
}

uint32_t CopPickTarget(GuestRam& g, uint32_t e, uint32_t sp) {
    const uint32_t f = sp - 16u;                                     // 0x8009E178
    uint32_t h = g.U16(e + 950u + 8u * U(g.S8(e + 946u)));           // the top command's target
    if (h != 224u && (S(h) >> 5) == 0) return h;
    const uint32_t gs = Gs(g);
    const uint32_t type = g.U8(gs + 4u);
    if (type & 1u) return g.U8(gs + 6u);                             // 0x8009E1C8: the race's target
    if (!(type & 0x10u)) {                                           // 0x8009E3B0
        const uint32_t p1 = g.U32(kPlayer1Ptr);
        const uint32_t ph = g.U16(p1 + 172u);
        if (h == ph) return h;
        if (g.U32(Rd(g, p1) + 40u) != 0u) return h;
        return ph;
    }
    if (g.S16(e + 320u) == 0) return h;                              // 0x8009E1DC
    for (int32_t p = 0; p < g.S32(gs + 48u); ++p)                    // 0x8009E1FC: unfinished players
        g.W32(f + 8u + 4u * U(p), g.U32(Rd(g, g.U32(kPlayerBikes + 4u * U(p))) + 40u) == 0u ? 1u : 0u);
    if (g.U32(f + 8u) == 0u) {                                       // 0x8009E38C
        if (g.U32(f + 12u) == 0u) return h;
        return g.U16(g.U32(kPlayerBikes + 4u) + 172u);
    }
    if (g.U32(f + 12u) == 0u) return g.U16(g.U32(kPlayerBikes) + 172u);   // 0x8009E374
    for (int32_t p = 0; p < g.S32(Gs(g) + 48u); ++p) {               // 0x8009E288: the nearer one
        const uint32_t pb = g.U32(kPlayerBikes + 4u * U(p));
        int32_t d = Octagon(Sub(g.S16(e + 186u), g.S16(pb + 186u)), Sub(g.S16(e + 194u), g.S16(pb + 194u)));
        if (!(d < 301)) d = 300;
        const uint32_t fin = g.U32(Rd(g, pb) + 40u);
        g.W32(f + 4u * U(p), U(d) + ((0u - fin) & (300u - U(d))));
    }
    const uint32_t pick = g.S32(f + 4u) < g.S32(f + 0u) ? 1u : 0u;  // 0x8009E344
    return g.U16(g.U32(kPlayerBikes + 4u * pick) + 172u);
}

int32_t CopInRange(GuestRam& g, uint32_t e, uint32_t t, int32_t dist) {
    if (g.S16(t + 320u) == 0) return 0;                              // 0x800BC120
    if (g.U32(g.U32(t + 852u) + 604u) < 3u && S(kCopSlowSpeed) < g.S32(t + 480u)) return 0;
    const int32_t d = Iabs(dist);                                    // 0x800BC160
    if (cu::Shl(g.S32(kAiTune + 0x0Cu), 1) < d) return 0;
    if (S(kCopSlowSpeed) < g.S32(e + 480u)) return 1;                // 0x800BC18C
    return NearReach(g) < d ? 0 : 1;
}

// ============================================================================ the functions with callees

bool CopArrest(GuestRam& g, uint32_t e, uint32_t t, uint32_t sp, CopCallees& c) {
    const uint32_t f = sp - 32u;                                     // 0x8009E528
    const uint32_t r = g.U32(t + 852u);
    g.W32(t + 720u, 0x20000u);
    g.W32(t + 484u, 0);
    g.W32(t + 576u, 0);
    g.W32(t + 480u, 0);
    g.W32(t + 924u, 0);
    g.W32(t + 464u, 0);
    g.W32(t + 460u, 0);
    g.W32(t + 456u, 0);
    const uint32_t fl = g.U32(r + 552u);
    g.W32(r + 464u, 0);
    g.W32(r + 460u, 0);
    g.W32(r + 456u, 0);
    g.W32(r + 488u, 0);
    g.W32(r + 484u, 0);
    g.W32(r + 480u, 0);
    g.W32(r + 552u, fl & 0xFFEFF067u);
    g.W8(Rd(g, t) + 39u, 254);                                       // 0x8009E598: busted
    if (g.Faulted()) return false;
    if (!c.BustedMusic(g.U16(t + 172u), g.U16(e + 172u), f)) return false;   // SLUS 0x8001B3C8
    const uint32_t src = (g.U32(r + 604u) < 3u) ? t + 504u : r + 184u;       // 0x8009E5B4
    for (uint32_t k = 0; k < 3u; ++k) {
        const uint32_t view = kViewArray + kViewStride * g.U16(t + 172u);
        g.W32(view + 184u + 4u * k, g.U32(src + 4u * k));
    }
    {
        const uint32_t view = kViewArray + kViewStride * g.U16(t + 172u);   // 0x8009E6DC
        g.W32(view + 548u, g.U32(view + 548u) | 6u);
    }
    if (g.Faulted()) return false;
    if (!(g.U32(r + 552u) & 0x40u)) return c.EndRace(t, 9, f) && !g.Faulted();   // 0x8009E730
    g.W32(Rd(g, t) + 40u, g.U32(Gs(g) + 16u));                       // 0x8009E740
    return !g.Faulted();
}

bool CopTail(GuestRam& g, uint32_t e, uint32_t sp, CopCallees& c, uint32_t& v0) {
    const uint32_t f = sp - 56u;                                     // 0x8009DC90
    const uint32_t cmd = f + 16u;
    v0 = 0;
    if (g.S8(e + 946u) <= 0) {                                       // 0x8009DCB0: an empty stack
        g.W16(cmd, 4);
        g.W16(cmd + 2u, 224);
        if (g.Faulted() || !c.PushCommand(cmd, 1, e, f)) return false;
    }
    {
        const uint32_t r = g.U32(e + 852u);
        if (!(g.U32(r + 604u) < 3u)) return !g.Faulted();            // 0x8009DCF4
        if (g.U32(r + 552u) & 0x40u) return !g.Faulted();
    }
    {
        const uint32_t gs = Gs(g);                                   // 0x8009DD10
        if (g.U8(gs + 4u) == 44u && !(g.U8(gs + 57u) < 4u)) return !g.Faulted();
    }
    {
        const uint32_t gs = Gs(g);                                   // 0x8009DD40: a busted player stays down
        for (int32_t p = 0; p < g.S32(gs + 48u); ++p) {
            const uint32_t pb = g.U32(kPlayerBikes + 4u * U(p));
            if (g.U8(Rd(g, pb) + 39u) == 254u) g.W32(pb + 720u, 0x20000u);
        }
    }
    CopWantedBits(g, g.U16(Rd(g, e) + 64u));                         // 0x8009DDB0
    const uint32_t s4 = CopPickTarget(g, e, f);                      // 0x8009DDB8
    if (g.Faulted()) return false;
    const uint32_t th = s4 & 0xFFFFu;
    if (th == 224u) return true;
    if ((th >> 5) != 0u) return true;
    const uint32_t t = g.U32(kPool0Ptr) + th * kSlot;                // s0
    if (t == 0u || t == e) return !g.Faulted();
    g.W16(f + 24u, static_cast<uint16_t>(TopOp(g, t)));              // 0x8009DE2C
    int32_t dist = CopDist(g, e, t, f);                              // s2
    if (!CopTargetOk(g, e, t)) return !g.Faulted();                  // 0x8009DE3C
    int32_t stop;
    {
        const int32_t sq = FixMul(g.S32(e + 480u), g.S32(e + 480u));   // 0x8009DE50
        stop = cu::SDiv(sq, cu::Shl(g.S32(e + 600u), 1));            // v^2 / (2 * decel), sign-magnitude
    }
    if (CopStopTest(g, e, t, dist, stop)) {                          // 0x8009DF1C: begin to stop
        if (!c.ClearCommands(e, f)) return false;
        g.W16(cmd, 2);
        g.W16(cmd + 2u, 224);
        if (g.Faulted() || !c.PushCommand(cmd, 1, e, f)) return false;
        g.W32(Rd(g, e) + 40u, g.U32(Gs(g) + 16u));
        v0 = 1;
        return !g.Faulted();
    }
    const int32_t behind = CopAhead(g, e, t);                        // t2, 0x8009DF70
    const uint32_t tr = g.U32(t + 852u), er = g.U32(e + 852u);
    const bool tSeated = g.U32(tr + 604u) < 3u;                      // t0
    const bool eSeated = g.U32(er + 604u) < 3u;                      // t3
    const bool tSlow = !tSeated || !(S(kCopSlowSpeed) < g.S32(t + 480u));   // a2
    const bool eSlow = !(S(kCopSlowSpeed) < g.S32(e + 480u));        // t1
    const bool sameRoad = g.U32(t + 360u) == g.U32(e + 360u);        // a3
    const bool near = !(NearReach(g) < dist);                        // v1
    const bool loose = g.U8(Rd(g, t) + 39u) != 254u;                 // a1
    if (behind) dist = Neg(dist);                                    // 0x8009E02C
    bool push = false;                                               // s3
    uint32_t target = s4;                                            // s4
    const uint32_t op = TopOp(g, e);
    if (g.Faulted()) return false;
    if (op == 2u) {                                                  // 0x8009E064: stopping
        if (eSeated && near && eSlow && loose && tSlow) {            // tSlow: off the bike, or slow
            if (!CopArrest(g, e, t, f, c)) return false;             // 0x8009E094: busted
        } else if (!near) {                                          // 0x8009E0AC: it got away
            if (!c.ClearCommands(e, f)) return false;
            target = 224;
            g.W16(cmd, 4);
            push = true;
            g.W32(Rd(g, e) + 40u, 0);
        }
    } else if (op == 4u) {                                           // 0x8009E0D0: racing
        bool range = tSlow || !sameRoad;
        if (!range) {
            if (AiBackOffOk(g, e, t, dist)) {                        // 0x8009E0E0: back off
                g.W16(cmd, 6);
                push = true;
            } else if (AiStrikeReach(g, e, t, dist)) {               // 0x8009E0F8: fight
                g.W16(cmd, 16);
                push = true;
                if (!AiTaunt(g, Rd(g, e))) return false;
            }
        } else if (CopInRange(g, e, t, dist)) {                      // 0x8009E124: intercept
            g.W16(cmd, 17);
            push = true;
        }
    }
    if (g.Faulted()) return false;
    if (push) {                                                      // 0x8009E13C
        g.W16(cmd + 2u, static_cast<uint16_t>(target));
        if (g.Faulted() || !c.PushCommand(cmd, 1, e, f)) return false;
    }
    return !g.Faulted();
}

bool CopIntercept(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, CopCallees& c,
                  const int16_t* sqrtTable) {
    const uint32_t f = sp - 48u;                                     // 0x800BBEBC
    const uint32_t th = target & 0xFFFFu;
    if (th == 224u) return true;
    if (g.U32(e + 568u) & 0x7FFu) return !g.Faulted();
    const uint32_t b = g.U32(kPool0Ptr) + th * kSlot;                // a1: the target bike
    uint32_t s = b;                                                  // s0: or its rider off the bike
    {
        const uint32_t r = g.U32(b + 852u);
        if (!(g.U32(r + 604u) < 3u)) s = r;
    }
    int32_t dist;
    if (g.U32(s + 360u) == g.U32(e + 360u)) {                        // 0x800BBF38: the same road
        dist = Iabs(Sub(g.S32(s + 368u), g.S32(e + 368u)));
    } else {
        dist = cu::Shl(Octagon(Sub(g.S32(s + 184u), g.S32(e + 184u)) >> 16,
                               Sub(g.S32(s + 192u), g.S32(e + 192u)) >> 16), 16);
    }
    if (g.Faulted()) return false;
    if (!CopInRange(g, e, b, dist)) return c.PopCommand(e, f) && !g.Faulted();   // 0x800BBFEC
    int32_t rel;                                                     // 0x800BBFFC
    {
        const int32_t a1 = g.S32(s + 344u), a0 = g.S32(e + 344u);
        rel = ((g.S32(s + 364u) ^ g.S32(e + 364u)) < 0) ? Add(a1, a0) : Sub(a1, a0);
    }
    const int32_t side = rel >= 0 ? Neg(g.S32(kAiTune + 0x08u)) : g.S32(kAiTune + 0x08u);
    cu::GMulAdd(g, s + 184u, s + 432u, side, e + 880u);              // SLUS 0x8002EAD8: 2.0 beside it
    int32_t v[3];
    for (uint32_t k = 0; k < 3u; ++k) {
        v[k] = Sub(g.S32(e + 880u + 4u * k), g.S32(e + 504u + 4u * k));
        g.W32(f + 16u + 4u * k, U(v[k]));
    }
    int32_t len;
    if (Iabs(v[0]) > 0x5A8000 || Iabs(v[1]) > 0x5A8000 || Iabs(v[2]) > 0x5A8000)
        len = 0x7FFF0000;                                            // 0x800BC0CC
    else
        len = Length3(v, sqrtTable);                                 // SLUS 0x8002E548
    if (!(g.U32(e + 564u) & 0x80000u)) {                             // 0x800BC0E0
        g.W32(e + 916u, U(len));
        g.W32(e + 920u, 0);
        g.W32(e + 564u, g.U32(e + 564u) & 0xFFF7FFFFu);
    }
    return !g.Faulted();
}

bool CopIdle(GuestRam& g, uint32_t e, uint32_t cmd, int32_t dt, uint32_t sp, CopCallees& c) {
    const uint32_t f = sp - 48u;                                     // 0x800BAA2C
    bool go = false;                                                 // s2
    const uint32_t gs = Gs(g);
    const uint32_t type = g.U8(gs + 4u);
    if (type & 1u) {                                                 // the cop-mode player
        if (!(g.U16(e + 172u) < g.U32(gs + 48u))) goto done;
        if ((g.U8(Rd(g, e) + 1u) & 0xFu) != 2u) goto done;
        const uint32_t flags = g.U32(kRaceFlags);
        if (flags & 1u) {                                            // 0x800BAAB8: out of the field
            int32_t place = 0;
            if (!c.ComputePlace(e, 0, f, place)) return false;
            int32_t n = g.S32(g.U32(kPoolTable + 12u));
            uint32_t b = g.U32(kPoolTable);
            int32_t down = 0;
            while (n >= 0) {
                if (g.S16(b + 320u) != 0 && (g.U32(g.U32(b + 852u) + 604u) - 3u) < 2u) ++down;
                b += g.U32(kPoolTable + 4u);
                --n;
                if (g.Faulted()) return false;
            }
            if (!(place < Sub(g.S32(kLiveBikes), down + 1))) go = true;
        } else if (flags & 2u) {                                     // 0x800BAB50: the quota starts
            g.W32(kCopJailAcc, 0);
            const uint32_t rec = kJailRecords + 224u * g.U16(e + 172u);
            g.W32(rec + 116u, g.U32(gs + 16u));
            g.W32(kRaceFlags, (flags & ~2u) | 4u);
            g.W32(kJailRecords + 224u * g.U16(e + 172u) + 108u, 0);
        } else if (flags & 4u) {                                     // 0x800BABB0: the quota runs
            g.W32(f + 16u, 0);
            g.W32(kCopJailAcc, g.U32(kCopJailAcc) + U(dt));
            int32_t v = 0;
            if (!c.JailTest(e, f + 16u, f, v)) return false;
            bool over = v < 0;
            if (!over) over = g.S32(kJailTab + (g.U32(Gs(g) + 60u) << 2)) < g.S32(kCopJailAcc);
            if (!over) over = g.U32(f + 16u) != 0u;
            if (over && !((static_cast<uint8_t>(g.U8(Rd(g, e) + 39u) + 4u)) < 2u)) {
                if (!c.JailRelease(e, f)) return false;
                go = true;
            }
        }
    } else if (type == 44u && !(g.U16(e + 172u) < g.U32(gs + 48u))) {   // 0x800BAC48: the Jailbreak race
        const int32_t a = Iabs(Sub(g.S32(g.U32(kPlayer1Ptr) + 368u), cu::Shl(g.U16(kFarTab), 16)));
        if (g.U32(e + 180u) < 18u) {
            if (g.U8(gs + 57u) == 4u && cu::Shl(g.S8(kFarGate), 16) < a) go = true;
        } else {
            const uint32_t ph = g.U8(gs + 57u);
            if (ph == 1u || ph == 3u) go = true;
        }
    } else {                                                         // 0x800BACF8: the release time
        const int32_t now = S(U(g.S32(gs + 16u)) * 0x6D00u);
        go = !(now < cu::Shl(g.U16(cmd + 4u), 16));
    }
done:
    if (g.Faulted()) return false;
    if (go) {                                                        // 0x800BAD3C
        uint32_t s = cmd;
        if (!(g.S8(e + 946u) < 2)) s = e + 956u;
        g.W16(s, 4);
        g.W16(s + 6u, 0);
        const uint32_t clock = g.U32(Gs(g) + 16u);
        g.W16(s + 4u, 0);
        g.W16(s + 6u, static_cast<uint16_t>(((clock * 0x884u + 0x8000u) >> 16) | 0xC000u));
        g.W32(e + 720u, 0);
    }
    return !g.Faulted();
}

bool CopArrestTest(GuestRam& g, uint32_t e, uint32_t t, uint32_t sp, CopCallees&, uint32_t& v0) {
    const uint32_t f = sp - 40u;                                     // 0x80097470
    v0 = 0;
    if (t == 0u || t == e) return true;
    if (g.S16(t + 320u) == 0) return !g.Faulted();
    g.W16(f + 16u, static_cast<uint16_t>(TopOp(g, t)));
    if (!CopTargetOk(g, e, t)) return !g.Faulted();                  // 0x800974CC
    const int32_t d = CopDist(g, e, t, f);
    v0 = CopStopTest(g, e, t, d, 0) != 0 ? 1u : 0u;                  // 0x800974F0
    return !g.Faulted();
}

bool CopArrestScan(GuestRam& g, uint32_t e, uint32_t sp, CopCallees& c) {
    const uint32_t f = sp - 40u;                                     // 0x80097388
    const uint32_t r = g.U32(e + 852u);
    if (!(g.U32(r + 604u) < 3u)) return !g.Faulted();
    if (g.U32(r + 552u) & 0x40u) return !g.Faulted();
    if (S(kCopSlowSpeed) < g.S32(e + 480u)) return !g.Faulted();
    if (g.U8(Rd(g, e) + 39u) == 255u) return !g.Faulted();
    int32_t n = g.S32(g.U32(kPoolTable + 12u));
    uint32_t b = g.U32(kPoolTable);
    while (n >= 0) {                                                 // 0x80097420
        if (g.Faulted()) return false;
        if (e != b) {
            uint32_t ok = 0;
            if (!CopArrestTest(g, e, b, f, c, ok)) return false;
            if (ok && !c.KnockArrest(e, b, 9, f)) return false;       // 0x8009743C
        }
        b += g.U32(kPoolTable + 4u);
        --n;
    }
    return !g.Faulted();
}

} // namespace rr::sim
