#include "game/sim/stance.h"

#include "game/sim/ai.h"
#include "game/sim/bike.h"
#include "game/sim/bike_step.h"
#include "game/sim/fixed.h"
#include "game/sim/integrator.h"
#include "game/sim/vec.h"

// Every function below is one guest function of RASHCDG.BIN (cfe43a77...), transcribed from our
// listing; the comments carry its addresses.

namespace rr::sim {
namespace {

inline uint32_t Cat(GuestRam& g, uint32_t s) { return g.U16(kStanceTable + 8u * (s & 0xFFFFu) + 2u); }
inline bool NoRepeat(uint32_t s) { return s == 11 || s == 77 || s == 13 || s == 78 || s == 4; }

} // namespace

// 0x800C4550 StanceEvent(ev, r, p): the gate on `p` parked in the own frame (sp+16, its ADDRESS is
// the gate's a2), then - only when the gate returns exactly 1 - the three children with the
// REQUESTED ev (s0 = a0 & 0xFFFF, not the stance the gate redirected to) and `p` re-loaded from the
// frame word the gate may have rewritten. Returns the gate's v0.
uint32_t StanceLayer::Event(uint32_t ev, uint32_t r, uint32_t p) {
    const uint32_t e = ev & 0xFFFFu;
    uint32_t frameP = p;                                          // sp+16
    const GateOut out = GateCore(e, r, frameP);
    if (out.wrote) frameP = out.second ? out.secondValue : out.first;
    if (out.v0 != 1u) return out.v0;
    Leave(e, r, frameP);
    Enter(e, r, frameP);
    SetRiderState(e, r, frameP);
    return out.v0;
}

uint32_t StanceLayer::Gate(uint32_t ev, uint32_t r, uint32_t pp) {
    const uint32_t p = g_.U32(pp);                                // 0x800C3EE0
    const GateOut out = GateCore(ev, r, p);
    if (out.wrote) {                                              // 0x800C43F8: `lw t1,96(sp)`
        g_.W32(pp, out.first);
        if (out.second) g_.W32(pp, out.secondValue);
    }
    return out.v0;
}

// 0x800C3E9C StanceGate(ev, r, &p).
StanceLayer::GateOut StanceLayer::GateCore(uint32_t ev, uint32_t r, uint32_t p) {
    GateOut out;
    uint32_t s1 = ev;                                             // the stance to play
    uint32_t red = kStanceNone;                                   // sp+32 (u16)
    uint32_t s4 = p;
    uint32_t s8 = s4 & 6u;                                        // play mode
    uint32_t s7 = s4 >> 8;                                        // play flags (a3 = & 0xFF)
    const uint32_t b0 = s4 & 1u;                                  // sp+36: bit 0 BEFORE the clear below
    const uint32_t b16 = (s4 >> 16) & 0xFFu;                      // sp+40
    const uint32_t kind = g_.U16(r + 172u);
    const uint32_t cur = g_.U16(r + 544u);                        // s2
    const uint32_t anim = g_.U32(r + 540u);                       // s5
    const uint32_t rider = ((kind >> 5) == 1u) ? r : 0u;          // s0
    const uint32_t mode = s4 & 0x18u;                             // a2: acceptance
    if (g_.S16(r + 320u) == 0) return out;                        // 0x800C3F2C: not live
    const uint32_t e = s1 & 0xFFFFu;
    if (NoRepeat(e)) {                                            // 0x800C3F60
        s4 &= 0xFFFFFFFEu;
        s7 |= 0x40u;
    }
    if (Cat(g_, cur) == 2u && s8 == 4u) s8 = 2;                   // 0x800C3F98
    if (cur == e && NoRepeat(cur)) return out;                    // 0x800C3FA4: no repeat
    bool ok = false;
    if (rider == 0u || mode == 16u || cur == kStanceNone) {       // 0x800C3FD4
        ok = true;
    } else if (mode == 8u) {
        const uint32_t c = Cat(g_, e);
        ok = (g_.U32(kStanceTable + 8u * cur + 4u) & (1u << (c & 31u))) != 0u;
    } else if (mode == 0u) {
        const uint32_t c = Cat(g_, e);
        if (g_.U32(kStanceTable + 8u * cur + 4u) & (1u << (c & 31u))) {
            ok = true;
        } else if (!(s4 & 0x20u)) {
            red = Path(rider, e) & 0xFFFFu;                       // 0x800C4070, sh v0,32(sp)
            ok = red != kStanceNone;
        }
    }                                                             // mode 0x18: refused
    if (!ok) return out;
    if (red != kStanceNone) {
        s1 = red;                                                 // 0x800C40A8
    } else if (rider != 0u && !(s4 & 0x20u) && g_.U16(rider + 608u) != 0u) {
        g_.W16(rider + 608u, 0);                                  // 0x800C40D4: drop the queue
    }
    if (cur == 0u && (s1 & 0xFFFFu) != 4u) return out;            // 0x800C40EC
    g_.W32(anim + 36u, (g_.U32(anim + 36u) & 0xFFFFFF7Fu) | ((s4 & 0x40u) << 7));
    const uint32_t rc = kStanceTable + 8u * cur;                  // s2
    const uint32_t rn = kStanceTable + 8u * (s1 & 0xFFFFu);       // s3
    const uint32_t newBank = g_.U32(rn) & 0xFu;
    if ((g_.U32(rc) & 0xFu) != newBank) {                         // 0x800C4140: a bank change
        anim_.BankSwitch(anim, g_.U32(kAnimBankTable + (newBank << 2)));
        if (g_.U16(rc + 2u) == 5u && g_.U16(rn + 2u) == 6u) {     // leaving -> tumbling: the seat
            if (g_.U8(rider + 572u) & 0x20u)
                anim_.SeatRelease(g_.U32(g_.U32(rider + 596u) + 856u), rider, 1);
            else
                anim_.SeatRelease(g_.U32(rider + 596u), rider, 0);
            g_.W8(rider + 72u, 3);                                // 0x800C41BC
        }
    }
    const uint32_t f228 = g_.U32(r + 552u) & 0xEFFFFFFFu;         // 0x800C41CC
    g_.W32(r + 552u, f228);
    const uint32_t e2 = s1 & 0xFFFFu;
    if (s8 != 0u) {
        uint32_t queued;
        if (s8 == 2u) {
            g_.W32(r + 552u, f228 | 0x10000000u);                 // 0x800C41EC
            queued = 0;
        } else {
            queued = 1;
            if (anim_.ChannelFree(anim) == 0u) {                  // 0x800C41F8: the channel is busy
                if (red == kStanceNone) return out;
                const uint32_t n = g_.U16(rider + 608u);
                g_.W16(rider + 608u, static_cast<uint16_t>(n + 1u));
                g_.W16(rider + (n << 1) + 610u, static_cast<uint16_t>(s1)); // push, refuse
                return out;
            }
        }
        const uint32_t w0 = g_.U32(kStanceTable + 8u * e2);
        const uint32_t list = g_.U32(kStanceEventList);
        const uint32_t ex = (list != 0u) ? g_.U32(list + (e2 << 2)) : 0u;
        anim_.Transition(anim, (w0 >> 4) & 0xFFFu, queued, s7 & 0xFFu, b0 == 0u ? 1u : 0u,
                         (s4 >> 7) & 1u, b16, ex);               // 0x800C42A8
    } else {
        const uint32_t w0 = g_.U32(kStanceTable + 8u * e2);
        const uint32_t clip = (w0 >> 4) & 0xFFFu;
        const uint32_t list = g_.U32(kStanceEventList);
        const uint32_t ex = (list != 0u) ? g_.U32(list + (e2 << 2)) : 0u;
        if (b0 == 1u)
            anim_.LoopStart(anim, clip, s7 & 0xFFu, b16, ex);     // 0x800C4318
        else if (s4 & 0x8000u)
            anim_.RangedStart(anim, clip, s7 & 0xFFu, 6, 11, b16, ex); // 0x800C4390
        else
            anim_.HardStart(anim, clip, s7 & 0xFFu, b16, ex);     // 0x800C43F0
    }
    out.wrote = true;                                             // 0x800C4400
    out.first = s4;
    if (red != kStanceNone) {
        out.second = true;
        out.secondValue = s4 | 0x20u;
    }
    out.v0 = 1;
    return out;
}

// 0x800C4500 StanceLeave(ev, r, p): the CURRENT stance of category 3 -> 0x800BFD24(r, cur, ev, p);
// else 3 (the `li v0,3` of the compare).
uint32_t StanceLayer::Leave(uint32_t ev, uint32_t r, uint32_t p) {
    const uint32_t cur = g_.U16(r + 544u);
    if (Cat(g_, cur) != 3u) return 3;
    return seams_.CombatLeave(r, cur, ev & 0xFFFFu, p);
}

// 0x800C4454 StanceEnter(ev, r, p): the NEW stance of category 3 -> 0x800BFC5C(r, cur, ev, p);
// else, for ev != 0, `p & 0x8000` (the delay-slot andi); for ev 0, a start of clip 0 on the BIKE's
// animation object `r->f254->f21C`.
uint32_t StanceLayer::Enter(uint32_t ev, uint32_t r, uint32_t p) {
    const uint32_t e = ev & 0xFFFFu;
    const uint32_t c = Cat(g_, e);
    const uint32_t cur = g_.U16(r + 544u);
    const uint32_t t1 = p >> 8;
    if (c == 3u) return seams_.CombatEnter(r, cur, e, p);
    if (e != 0u) return p & 0x8000u;
    const uint32_t anim = g_.U32(g_.U32(r + 596u) + 540u);
    if (p & 0x8000u) return anim_.RangedStart(anim, 0, t1 & 0xFFu, 6, 11, 0, 0); // 0x800C44C8
    return anim_.HardStart(anim, 0, t1 & 0xFFu, 0, 0);                         // 0x800C44E8
}

// 0x800C2FF4 SetRiderState(ev, e, p) - a leaf.
uint32_t StanceLayer::SetRiderState(uint32_t ev, uint32_t e, uint32_t p) {
    if ((g_.U16(e + 172u) >> 5) != 1u) {                          // not a pool-1 rider
        const uint32_t v1 = g_.U32(e + 552u);
        g_.W16(e + 544u, static_cast<uint16_t>(ev));              // +0x220 BEFORE +0x228
        g_.W32(e + 552u, ((v1 | 0x20000000u) & 0xFFFF00FFu) | 0x300u);
        return 0xFFFF00FFu;
    }
    const uint32_t c = Cat(g_, ev);
    uint32_t mount;
    if (c == 0u) mount = 0;
    else if (c < 5u) mount = 1;
    else if (c < 6u) mount = 2;
    else if (c < 7u) mount = 3;
    else mount = 4;
    const uint32_t gs = g_.U32(kAnimGameStatePtr);
    uint32_t v1 = g_.U32(e + 552u);
    g_.W32(e + 604u, mount);                                      // 0x800C3068
    g_.W16(e + 544u, static_cast<uint16_t>(ev));                  // 0x800C306C
    const uint32_t clock = g_.U32(gs + 16u);
    v1 |= 0x02000000u;
    g_.W32(e + 552u, v1);                                         // 0x800C3078
    g_.W32(e + 548u, clock);                                      // the race clock
    g_.W32(e + 552u, (mount - 2u < 2u) ? (v1 | 0x20000000u) : (v1 & 0xDFFFFFFFu)); // 0x800C30A4
    uint32_t v = g_.U32(e + 552u);
    v = (p & 0x100u) ? (v | 0x08000000u) : (v & 0xF7FFFFFFu);     // bit 27: mirrored
    g_.W32(e + 552u, v);                                          // 0x800C30D0
    g_.W8(e + 546u, 0);
    return v;
}

// 0x800C37B0 StancePath(rider, ev) - a leaf with a 448-byte frame buffer. The
// buffer is a host array here; the original's is 224 halfwords, and a path of 223 or more stances
// would run its `buf[n + 1]` store into the gate's frame - the game's tables never come near that
// (their longest path is a handful), and the port refuses rather than imitate a stack overwrite.
uint32_t StanceLayer::Path(uint32_t rider, uint32_t ev) {
    const uint32_t cur0 = g_.U16(rider + 544u);
    uint32_t i = 0;
    if (g_.U8(kStancePathKeys) < cur0) {
        do { ++i; } while (g_.U8(kStancePathKeys + 2u * i) < cur0 && !g_.Faulted());
    }
    if (g_.U8(kStancePathKeys + 2u * i) != g_.U16(rider + 544u)) return kStanceNone;
    const uint32_t t0 = g_.U8(kStancePathKeys + 2u * i + 1u);
    const uint32_t e = ev & 0xFFFFu;
    uint32_t j = t0 + 2u;
    if (g_.U8(kStancePathData + j) < e) {
        ++j;
        while (g_.U8(kStancePathData + j) < e && !g_.Faulted()) ++j;
    }
    if (g_.U8(kStancePathData + j) != e) return kStanceNone;
    uint16_t buf[258] = {};
    buf[0] = static_cast<uint16_t>(ev);                           // `sh a1,0(sp)`: the raw argument
    const uint32_t n0 = g_.U8(kStancePathData + t0);
    if (n0 + 1u >= 224u) {
        refused_ = true;
        return kStanceNone;
    }
    if (n0 != 0u) {
        uint32_t k = 0;
        do {
            ++k;
            buf[k] = g_.U8(kStancePathData + t0 + k);
        } while (static_cast<int32_t>(k) < static_cast<int32_t>(g_.U8(kStancePathData + t0)) && k < 256u);
    }
    buf[g_.U8(kStancePathData + t0) + 1u] = g_.U16(rider + 544u);
    const uint32_t n = g_.U8(kStancePathData + t0);
    for (uint32_t k = 0; k < n; ++k) g_.W16(rider + 610u + 2u * k, buf[k]);
    g_.W16(rider + 608u, static_cast<uint16_t>(n));
    return buf[n];
}

// ============================================================================ the knock-off and the launch

namespace {
constexpr uint32_t kGsPtr = 0x8005B2F8;       // -> game_state (+0x04 mode, +0x10 clock, +0x30 players, +0x3C)
constexpr uint32_t kViewBase = 0x800CD898;    // the per-player view records, 1132 bytes each
constexpr uint32_t kViewBytes = 1132;
constexpr uint32_t kSinCos = 0x8005624C;      // SLUS {s16 sin, s16 cos} x 4096
constexpr uint32_t kAsinTable = 0x800527E0;   // SLUS, Asin's 61 x u16
constexpr uint32_t kFightRecPtr = 0x8005AD4C; // -> the 12-byte FIGHT.BIN records (AiPushCommand)
constexpr uint32_t kKoPlayerBikes = 0x8005B268; // -> player p's bike, stride 4
constexpr uint32_t kRankGate = 0x8005B1F8;    // the police loop's gate on the suspect's rank byte
constexpr uint32_t kArrestRadius = 0x8005309C;
constexpr uint32_t kLaunchSpeedWord = 0x800D3964;
constexpr uint32_t kCamTargets = 0x80053478;  // SLUS 0x800235B0's table, 128 bytes a player

inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t MipsAbs(int32_t x) { // sra/addu/xor
    const uint32_t sgn = static_cast<uint32_t>(x >> 31);
    return S((U(x) + sgn) ^ sgn);
}
inline int32_t MulLo(int32_t a, int32_t b) { return S(U(a) * U(b)); }

// The stance sink AiPushCommand (ai.h) fires on its category-3 arm: the PORTED stance event, with
// the `p = 2` of the original's call at 0x800BCCE8.
struct PushSink final : AiStanceSink {
    StanceLayer& s;
    explicit PushSink(StanceLayer& l) : s(l) {}
    void PlayIdleStance(uint16_t event, uint32_t rider) override { s.Event(event, rider, 2); }
};

// Scale(t, dir) into out, on guest addresses (SLUS 0x8002EE50).
void GuestScale(GuestRam& g, int32_t t, uint32_t dir, uint32_t out) {
    int16_t d[3];
    int32_t o[3];
    for (uint32_t k = 0; k < 3; ++k) d[k] = g.S16(dir + 2u * k);
    Scale(t, d, o);
    for (uint32_t k = 0; k < 3; ++k) g.W32(out + 4u * k, U(o[k]));
}
int32_t GuestDot(GuestRam& g, uint32_t a, uint32_t b) { // SLUS 0x8002E698
    int16_t va[3], vb[3];
    for (uint32_t k = 0; k < 3; ++k) {
        va[k] = g.S16(a + 2u * k);
        vb[k] = g.S16(b + 2u * k);
    }
    return DotLcm(va, vb);
}
} // namespace

uint32_t RiderLayer::Peek32(uint32_t a) {
    const uint32_t seg = a >> 29;
    const uint32_t phys = (seg == 4u || seg == 5u) ? (a & 0x1FFFFFFFu) : (seg <= 3u ? a : 0xFFFFFFFFu);
    if ((a & 3u) != 0u || phys >= 0x00800000u) return 0;
    const uint8_t* p = ram_ + (phys & 0x1FFFFFu);
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// 0x80090D84 RiderKnockOff(R): only for a rider still on the bike (+0x25C < 2, unsigned); the
// knock-off request, bit 15 of +0x228, is consumed either way (0x80091444).
uint32_t RiderLayer::KnockOff(uint32_t r) {
    if (g_.U32(r + 604u) < 2u) KnockOffBody(r);
    const uint32_t v = g_.U32(r + 552u) & 0xFFFF7FFFu;
    g_.W32(r + 552u, v);
    return v;
}

void RiderLayer::KnockOffBody(uint32_t r) {
    uint32_t s2 = 1;
    const uint32_t b = g_.U32(r + 596u);                                    // s1
    const uint32_t s4 = (static_cast<uint32_t>(g_.U8(r + 572u)) >> 5) & 1u; // the passenger bit
    g_.W8(g_.U32(b + 1084u) + 15u, 0);                                     // 0x80090DC8: health := 0
    const uint32_t neg = 0u - s4;
    const uint32_t fc = g_.U32(b + 568u);
    if ((fc & 0x7FFu) == 0u) {
        const uint32_t f = g_.U32(r + 552u);
        if (f & 0x60000u) stance_.Event((neg & 0x31u) + 42u, r, (f & 0x40000u) ? 0x100u : 0u); // 42 / 91
        else stance_.Event((neg & 0x31u) + 41u, r, 0);                                          // 41 / 90
    } else if ((fc & 0x18000u) || ((fc & 0x400u) && (g_.U32(r + 552u) & 0x60000u))) {
        stance_.Event(42, r, (g_.U32(r + 552u) & 0x40000u) ? 0x100u : 0u);                      // 42, no passenger form
    } else {
        const uint32_t f2 = g_.U32(b + 568u);
        if (f2 & 0x20u) {                                                  // 0x80090E90: 39 / 89
            uint32_t p = 0;
            if (g_.S32(b + 676u) < 0) p = 1;
            else if ((f2 & 0x40u) && g_.S32(b + 652u) > 0) p = 1;
            stance_.Event((neg & 0x32u) + 39u, r, p << 8);
            s2 = 0;
        } else if (f2 & 0x140u) {                                          // 0x80090EE8: 38 / 89 (sic)
            s2 = 0;
            stance_.Event((neg & 0x33u) + 38u, r, U(g_.S32(b + 676u) >> 31) & 0x100u);
        } else if ((f2 & 0x680u) && !(f2 & 0x40000u)) {                    // 0x80090F28: 41 / 90
            stance_.Event((neg & 0x31u) + 41u, r, 0);
            s2 = (g_.U32(b + 568u) & 0x480u) != 0u ? 1u : 0u;
        } else {                                                           // 0x80090F50: 40 / 88
            stance_.Event((neg & 0x30u) + 40u, r, 0);
            if (!(g_.U32(b + 568u) & 0x20000u)) g_.W32(r + 480u, U(FixMul(g_.S32(r + 480u), 0xC000)));
            int32_t v = g_.S32(r + 480u);
            if (v < 0x50000) v = 0x50000;
            g_.W32(r + 480u, U(v));                                        // 0x80090FB0
            for (uint32_t k = 0; k < 9; ++k) g_.W16(r + 432u + 2u * k, g_.U16(b + 432u + 2u * k)); // SLUS 0x8003FA18
            g_.W32(b + 568u, g_.U32(b + 568u) & 0xFFF9FFFFu);
            const uint32_t h = g_.U16(r + 172u);
            const uint32_t gs = g_.U32(kGsPtr);
            const bool player = ((h >> 5) == 1u) && (S(h & 0x1Fu) < g_.S32(gs + 48u));
            s2 = 0;
            if (player) {
                const uint32_t x = s4 ? g_.U32(g_.U32(r + 596u) + 856u) : g_.U32(r + 596u);
                const uint32_t rec = kViewBase + kViewBytes * g_.U16(x + 172u);
                const uint32_t v1 = g_.U32(rec + 548u) & 0xFFFD7FFFu;
                const uint32_t busy = g_.U32(rec + 772u);
                g_.W32(rec + 548u, v1);                                    // 0x80091068
                if (busy == 0u) {                                          // the crash camera
                    uint32_t k = 0;
                    const uint32_t other = g_.U32(b + 828u);
                    if (other != 0u && (g_.U16(other) >> 5) == 8u && (g_.U16(other + 2u) & 0x200u)) {
                        k = 1;
                    } else if (g_.U32(r + 552u) & 0x10000u) {
                        k = 0;
                    } else if (0xDDB1 < MipsAbs(GuestDot(g_, b + 820u, g_.U32(b + 340u) + 14u))) {
                        const uint32_t rnd = GuestRand(g_);                // 0x800910E4: 50 % by Rand
                        k = (rnd % 100u) < 50u ? 0u : 2u;
                    }
                    if (k == 1u) {
                        g_.W32(rec + 548u, g_.U32(rec + 548u) | 0x8000u);
                    } else if (k == 2u) {
                        g_.W32(rec + 792u, 0);
                        const uint32_t add = (g_.U32(b + 568u) & 4u) ? 0x80020000u : 0x20000u;
                        g_.W32(rec + 548u, g_.U32(rec + 548u) | add);
                    }
                }
            }
        }
    }
    // 0x80091188
    if (s2 == 0u) seams_.Takedown(b);
    {
        int32_t base[3], out[3];
        int16_t dir[3];
        for (uint32_t i = 0; i < 3; ++i) {
            base[i] = g_.S32(b + 184u + 4u * i);
            dir[i] = g_.S16(b + 438u + 2u * i);
        }
        MulAdd(base, dir, S(0xFFFF0000u), out);                            // SLUS 0x8002EAD8
        for (uint32_t i = 0; i < 3; ++i) g_.W32(r + 184u + 4u * i, U(out[i]));
    }
    g_.W32(r + 468u, g_.U32(r + 184u));                                    // 0x800911BC..C8
    g_.W32(r + 472u, g_.U32(r + 188u));
    g_.W32(r + 476u, g_.U32(r + 192u));
    // AiClearCommands clears 8 x (s8)depth bytes: a negative depth is a 4 GiB memset on the console.
    if (static_cast<int8_t>(g_.U8(b + 0x3B2u)) < 0) { refused_ = true; return; }
    if (g_.Faulted()) return;
    AiClearCommands(Raw(b));                                               // 0x800BCD10
    PushSink sink(stance_);
    AiPushEnv env;
    env.riderAddress = g_.U32(b + 852u);
    env.rider = Raw(env.riderAddress);
    env.altKindTable = Raw(kStanceTable);
    env.fightRecords = Raw(g_.U32(kFightRecPtr));
    env.stance = &sink;
    {
        const uint32_t rd = g_.U32(b + 1084u);
        const uint32_t a3 = (g_.U32(rd + 40u) != 0u || g_.U8(rd + 39u) >= 248u) ? 1u : 0u;
        uint8_t cmd[8] = {};
        const uint16_t op = static_cast<uint16_t>(((0u - a3) & 0xFFFFFFFEu) + 4u); // 4, or 2
        cmd[0] = static_cast<uint8_t>(op);
        cmd[1] = static_cast<uint8_t>(op >> 8);
        cmd[2] = 224;
        env.raceClock = g_.S32(g_.U32(kGsPtr) + 16u);
        AiPushCommand(cmd, 1, Raw(b), env);                                // 0x80091220
    }
    const uint32_t f234 = g_.U32(b + 564u) & 0xFFF7FFFFu;
    g_.W32(b + 924u, 0);
    g_.W32(b + 916u, 0);
    g_.W32(b + 920u, 0);
    g_.W32(b + 564u, f234);
    {
        uint8_t cmd[8] = {};
        cmd[2] = 224;
        env.raceClock = g_.S32(g_.U32(kGsPtr) + 16u);
        AiPushCommand(cmd, 0, Raw(b), env);                                // 0x80091258
    }
    if (s4 == 0u) {
        const uint32_t h = g_.U16(b + 172u);
        if (h < g_.U32(g_.U32(kGsPtr) + 48u)) seams_.RiderOffSound(h, 1);   // sltu
    }
    if (g_.U32(r + 552u) & 0x10000u) Launch(r);                            // 0x800912A0
    Police(b);
}

// 0x800912AC..0x80091430: the arrest test, over the PLAYERS, of a cop player near the bike.
void RiderLayer::Police(uint32_t b) {
    const uint32_t gs = g_.U32(kGsPtr);
    const uint32_t mode = g_.U8(gs + 4u);
    if (!(mode & 1u)) return;
    if ((g_.U8(g_.U32(b + 1084u) + 1u) & 0xFu) == 2u) return;
    if (mode == 33u) return;
    if (!(g_.S32(gs + 48u) > 0)) return;
    uint32_t i = 0;
    do {
        const uint32_t cop = g_.U32(kKoPlayerBikes + 4u * i);
        if ((g_.U8(g_.U32(cop + 1084u) + 1u) & 0xFu) == 2u &&
            !(g_.S32(kRankGate) < static_cast<int32_t>(g_.U8(g_.U32(b + 1084u) + 39u)))) {
            int32_t dx = MipsAbs(g_.S16(cop + 186u) - g_.S16(b + 186u));
            int32_t dz = MipsAbs(g_.S16(cop + 194u) - g_.S16(b + 194u));
            int32_t hi = dx, lo = dz;
            if (dx < dz) {
                hi = dz;
                lo = dx;
            }
            const int32_t a3 = lo + (lo >> 1);
            const int32_t d = hi - (hi >> 5) - (hi >> 7) + (a3 >> 2) + (a3 >> 6);
            if (0x8F0D8 < g_.S32(cop + 480u)) {
                const int32_t lim = g_.S16(kArrestRadius + 4u * g_.U32(g_.U32(kGsPtr) + 60u) + 2u);
                if (d < lim) seams_.Arrest(cop, b, 9);
            }
        }
        ++i;
        if (g_.Faulted()) return;
    } while (S(i) < g_.S32(g_.U32(kGsPtr) + 48u));
}

// ---------------------------------------------------------------------------- RiderLaunch helpers
void RiderLayer::LoadDir(uint32_t r, uint32_t s5) {
    if (g_.U32(r + 552u) & 0x200000u) {                                    // the launch direction
        const uint16_t x = g_.U16(r + 456u), y = g_.U16(r + 458u), z = g_.U16(r + 460u);
        g_.W16(r + 450u, x);
        g_.W16(r + 452u, y);
        g_.W16(r + 454u, z);
    } else {                                                               // the bike's
        g_.W16(r + 450u, g_.U16(s5 + 450u));
        g_.W16(r + 452u, g_.U16(s5 + 452u));
        g_.W16(r + 454u, g_.U16(s5 + 454u));
    }
}

void RiderLayer::Rot357(uint32_t r) { // a passenger's direction turned by 357/4096 (~5 degrees)
    int32_t v0 = (357 * static_cast<int32_t>(g_.S16(r + 454u))) >> 12;
    const uint16_t x = static_cast<uint16_t>(g_.U16(r + 450u) + U(v0));
    g_.W16(r + 450u, x);
    v0 = (357 * static_cast<int32_t>(static_cast<int16_t>(x))) >> 12;
    g_.W16(r + 454u, static_cast<uint16_t>(g_.U16(r + 454u) - U(v0)));
}

// Asin(dir.y << 4), the cosine of (1137 - a) & 4095, then 0x8007E868(dir, speed, cos, 0x633B6).
void RiderLayer::LiftByAsin(uint32_t r) {
    int32_t a = 0;
    const int32_t x = S(U(static_cast<int32_t>(g_.S16(r + 452u))) << 4);
    if (!Asin(x, reinterpret_cast<const uint16_t*>(Raw(kAsinTable)), a)) {
        refused_ = true;
        return;
    }
    const uint32_t i = (1137u - U(a)) & 0xFFFu;
    seams_.LaunchLift(r + 450u, g_.U32(r + 480u), U(static_cast<int32_t>(g_.S16(kSinCos + 4u * i + 2u))), 0x633B6u);
}

// GTE OP (sf = 1, lm = 0): RT's diagonal from `d`, IR from `ir`, the result to `dst`.
void RiderLayer::Op(uint32_t dst, uint32_t d, uint32_t ir) {
    int16_t dv[3], iv[3], out[3];
    for (uint32_t k = 0; k < 3; ++k) {
        dv[k] = g_.S16(d + 2u * k);
        iv[k] = g_.S16(ir + 2u * k);
    }
    OuterProduct(dv, iv, out);
    for (uint32_t k = 0; k < 3; ++k) g_.W16(dst + 2u * k, static_cast<uint16_t>(out[k]));
}

// SLUS 0x8002E468 Normalize(v) as a caller testing its v0 sees it (the sum of squares, two TRAPPING
// adds at 0x8002E4A0/4A4): refused where the console raises the overflow exception.
bool RiderLayer::Norm(uint32_t v, int32_t& n) {
    int16_t c[3];
    for (uint32_t k = 0; k < 3; ++k) c[k] = g_.S16(v + 2u * k);
    const int64_t s1 = static_cast<int64_t>(c[0]) * c[0] + static_cast<int64_t>(c[1]) * c[1];
    const int64_t s2 = s1 + static_cast<int64_t>(c[2]) * c[2];
    if (s1 > INT32_MAX || s2 > INT32_MAX) {
        refused_ = true;
        return false;
    }
    const uint32_t table = g_.U32(g_.gp() + 2260u);
    if (!Normalize(c, reinterpret_cast<const uint16_t*>(Raw(table)))) {
        refused_ = true;
        return false;
    }
    for (uint32_t k = 0; k < 3; ++k) g_.W16(v + 2u * k, static_cast<uint16_t>(c[k]));
    n = static_cast<int32_t>(s2);
    return true;
}

// The tail's quaternion turn (0x800921A8..0x800922BC; the second form 0x800922DC..0x800923F0): the
// root part's quaternion through QuatGet / QuatSet (their frame buffers sp+16 / sp+24 are host arrays
// here), each term `(c q >> 14) +- (s q >> 14)` - shifted BEFORE the add - with c, s the table's
// cosine and sine x 4 (`sll 18; sra 16`).
void RiderLayer::QuatRot(uint32_t r, int32_t ang, bool second) {
    int16_t q[4];
    const uint32_t anim = g_.U32(r + 540u);
    for (uint32_t k = 0; k < 4; ++k) q[k] = g_.S16(anim + 128u + 24u * k); // QuatGet 0x8005C338
    const uint32_t h = U((ang + (ang < 0 ? 1 : 0)) >> 1) & 0xFFFu;
    const int32_t c = S(U(static_cast<int32_t>(g_.S16(kSinCos + 4u * h + 2u))) << 18) >> 16;
    const int32_t s = S(U(static_cast<int32_t>(g_.S16(kSinCos + 4u * h))) << 18) >> 16;
    int32_t o30, o26, o28, o24;
    if (!second) {
        o30 = (MulLo(c, q[3]) >> 14) - (MulLo(s, q[1]) >> 14);
        o26 = (MulLo(c, q[1]) >> 14) + (MulLo(s, q[3]) >> 14);
        o28 = (MulLo(c, q[2]) >> 14) - (MulLo(s, q[0]) >> 14);
        o24 = (MulLo(c, q[0]) >> 14) + (MulLo(s, q[2]) >> 14);
    } else {
        o30 = (MulLo(c, q[3]) >> 14) - (MulLo(s, q[0]) >> 14);
        o26 = (MulLo(c, q[1]) >> 14) - (MulLo(s, q[2]) >> 14);
        o28 = (MulLo(c, q[2]) >> 14) + (MulLo(s, q[1]) >> 14);
        o24 = (MulLo(c, q[0]) >> 14) + (MulLo(s, q[3]) >> 14);
    }
    const uint16_t out[4] = {static_cast<uint16_t>(o24), static_cast<uint16_t>(o26), static_cast<uint16_t>(o28),
                             static_cast<uint16_t>(o30)};
    const uint32_t anim2 = g_.U32(r + 540u);
    for (uint32_t k = 0; k < 4; ++k) g_.W16(anim2 + 128u + 24u * k, out[k]); // QuatSet 0x8005C36C
}

// The player's view bits: view[h] +0x224 |= bits when the bike's handle is a player's (sltu).
void RiderLayer::ViewOr(uint32_t s5, uint32_t bits) {
    const uint32_t h = g_.U16(s5 + 172u);
    if (!(h < g_.U32(g_.U32(kGsPtr) + 48u))) return;
    const uint32_t rec = kViewBase + kViewBytes * h;
    g_.W32(rec + 548u, g_.U32(rec + 548u) | bits);
}

// 0x80091468 RiderLaunch(R). The jump table at 0x8005B7A4 (54 words, index
// stance - 38) is fixed data; its targets are dispatched here: 38 A, 39/89 B, 40/88 C, 41/90 D,
// 42/91 E, the other 45 and everything outside 38..91 the default.
uint32_t RiderLayer::Launch(uint32_t r) {
    const uint32_t s8 = (static_cast<uint32_t>(g_.U8(r + 572u)) >> 5) & 1u; // the passenger bit
    uint32_t s5;
    if (s8) {
        s5 = g_.U32(g_.U32(r + 596u) + 856u);
        g_.W32(s5 + 724u, 0);                                              // 0x800914C0
    } else {
        s5 = g_.U32(r + 596u);
    }
    int32_t phi = 10; // sp+32
    int32_t ang2 = 0; // sp+36
    uint32_t s7 = 1;
    int32_t s3 = 0;
    uint32_t s1 = 0;
    const uint32_t f228 = g_.U32(r + 552u);
    const uint32_t stance = g_.U16(r + 544u);
    g_.W32(r + 488u, 0);                                                   // 0x800914E4
    const uint32_t s6 = (f228 >> 27) & 1u;
    g_.W32(r + 580u, g_.U32(kLaunchSpeedWord));                            // 0x800914F8
    int kase = 0; // 0 default, 1 A, 2 B, 3 C, 4 D, 5 E
    if (stance - 38u < 54u) {
        kase = stance == 38 ? 1 : (stance == 39 || stance == 89) ? 2 : (stance == 40 || stance == 88) ? 3
             : (stance == 41 || stance == 90) ? 4 : (stance == 42 || stance == 91) ? 5 : 0;
    }
    int32_t n = 0;
    switch (kase) {
    case 1: { // 0x80091528: stance 38
        g_.W32(r + 552u, g_.U32(r + 552u) & 0xBFFFFFFFu);
        int32_t a = g_.S32(r + 480u);
        if (a < 0x30000) a = 0x30000;
        g_.W32(r + 480u, U(a));
        s1 = (0x140000 < a) ? 48u : 51u;
        s7 |= (g_.U32(r + 552u) & 0x08000000u) ? 0x182u : 0x82u;
        seams_.PoseInit(kPoseInitAFn, r, 0);
        LoadDir(r, s5);
        const int32_t v = FixMul(s1 == 48u ? 0xD999 : 0xE666, g_.S32(s5 + 480u));
        g_.W32(r + 480u, U(v));
        GuestScale(g_, v, r + 450u, r + 456u);
        const uint16_t v522 = g_.U16(s5 + 522u);
        const uint16_t a0 = g_.U16(r + 450u), v1 = g_.U16(r + 454u);
        g_.W16(r + 438u, v522);
        g_.W16(r + 440u, g_.U16(s5 + 524u));
        const uint16_t a1 = g_.U16(s5 + 526u), v0 = g_.U16(r + 452u);
        g_.W16(r + 444u, a0);
        g_.W16(r + 448u, v1);
        g_.W16(r + 446u, v0);
        g_.W16(r + 442u, a1);
        Op(r + 432u, r + 438u, r + 444u);                                  // side = up x dir
        if (!Norm(r + 432u, n)) return 0;
        if (n == 0) BikeResetOrientation(g_, r);
        g_.W32(r + 600u, 0x30000u);                                        // 0x800916CC
        g_.W32(r + 508u, 0);
        break;
    }
    case 5: { // 0x800916D8: stances 42, 91
        g_.W32(r + 552u, g_.U32(r + 552u) | 0x40000000u);
        seams_.PoseInit(kPoseInitEFn, r, 0);
        GuestScale(g_, g_.S32(r + 480u), r + 450u, r + 456u);
        const uint16_t v522 = g_.U16(s5 + 522u);
        const uint16_t v1 = g_.U16(r + 452u), a0 = g_.U16(r + 454u);
        g_.W16(r + 438u, v522);
        g_.W16(r + 440u, g_.U16(s5 + 524u));
        s7 |= 0x82u;
        const uint16_t a1 = g_.U16(s5 + 526u), v0 = g_.U16(r + 450u);
        g_.W16(r + 446u, v1);
        g_.W16(r + 448u, a0);
        g_.W16(r + 444u, v0);
        g_.W16(r + 442u, a1);
        Op(r + 432u, r + 438u, r + 444u);
        if (!Norm(r + 432u, n)) return 0;
        Op(r + 438u, r + 444u, r + 432u);
        int32_t as = 0;
        if (!Asin(GuestDot(g_, r + 450u, s5 + 444u), reinterpret_cast<const uint16_t*>(Raw(kAsinTable)), as)) {
            refused_ = true;
            return 0;
        }
        s3 = S(1024u - U(as));
        if (s3 < 683) {
            phi = 20;
            s1 = 48;
            if (!s6) s7 |= 0x100u;
        } else {
            s1 = 46;
        }
        seams_.CrashEvent(g_.U16(s5 + 172u), 1);
        if (!s8) ViewOr(s5, ((g_.U32(r + 552u) & 0x10000u) ? 0x0E000000u : 0x06000000u) | 0x80u);
        break;
    }
    case 2: { // 0x800918B8: stances 39, 89
        s7 |= (0u - s6) & 0x100u;
        s1 = 48;
        phi = 40;
        g_.W32(r + 552u, g_.U32(r + 552u) | 0x40000000u);
        if (s8) phi = 25;
        seams_.PoseInit(kPoseInitBFn, r, 0);
        const uint32_t pairs[9][2] = {{432, 444}, {434, 446}, {436, 448}, {444, 432}, {446, 434},
                                      {448, 436}, {438, 438}, {440, 440}, {442, 442}};
        for (const auto& pr : pairs) g_.W16(r + pr[0], g_.U16(s5 + pr[1])); // the bike's frame, rows 0/2 swapped
        const uint32_t row = s6 ? 432u : 444u;                             // one row negated by the mirror
        {
            const uint16_t a = g_.U16(r + row), c = g_.U16(r + row + 4u);
            g_.W16(r + row, static_cast<uint16_t>(0u - a));
            const uint16_t b = g_.U16(r + row + 2u);
            g_.W16(r + row + 4u, static_cast<uint16_t>(0u - c));
            g_.W16(r + row + 2u, static_cast<uint16_t>(0u - b));
        }
        s3 = 1024;
        const int32_t v = (g_.U32(s5 + 568u) & 0x140u) ? 0x50000 : FixMul(0x13333, g_.S32(s5 + 480u));
        g_.W32(r + 480u, U(v));
        LoadDir(r, s5);
        if (s8) Rot357(r);
        LiftByAsin(r);
        if (refused_) return 0;
        GuestScale(g_, g_.S32(r + 480u), r + 450u, r + 456u);
        seams_.CrashEvent(g_.U16(s5 + 172u), 1);
        if (!s8) ViewOr(s5, 0x06000080u);
        g_.W32(r + 580u, 65);                                              // 0x80091F40
        break;
    }
    case 3: { // 0x80091B64: stances 40, 88
        s7 |= 0x82u;
        g_.W32(r + 552u, g_.U32(r + 552u) | 0x40000000u);
        seams_.PoseInit(kPoseInitCFn, r, 0);
        LoadDir(r, s5);
        if (s8) Rot357(r);
        if (g_.U32(r + 552u) & 0x10000u) {
            seams_.LaunchLift(r + 450u, 0, U(-2868), 0);
            const int32_t v = g_.S32(r + 480u);
            int32_t q = S(U(static_cast<int32_t>((static_cast<int64_t>(v) * 0x55555556ll) >> 32)) - U(v >> 31));
            g_.W32(r + 480u, U(q));                                        // speed / 3
            if (q < 0x50000) q = 0x50000;
            g_.W32(r + 480u, U(q));                                        // 0x80091C98
        } else {
            LiftByAsin(r);
            if (refused_) return 0;
            int32_t as = 0;
            if (!Asin(GuestDot(g_, r + 444u, r + 450u), reinterpret_cast<const uint16_t*>(Raw(kAsinTable)), as)) {
                refused_ = true;
                return 0;
            }
            ang2 = S(U(as) - 1024u);
        }
        g_.W16(r + 438u, g_.U16(s5 + 522u));
        g_.W16(r + 440u, g_.U16(s5 + 524u));
        g_.W16(r + 442u, g_.U16(s5 + 526u));
        Op(r + 432u, r + 438u, r + 450u);
        if (!Norm(r + 432u, n)) return 0;
        if (n == 0) BikeResetOrientation(g_, r);
        Op(r + 444u, r + 432u, r + 438u);
        GuestScale(g_, g_.S32(r + 480u), r + 450u, r + 456u);
        uint32_t s0 = 0xFFFFFFFFu;
        const uint32_t o = g_.U32(s5 + 852u);
        if (g_.U8(o + 572u) & 0x10u) {
            const uint32_t sid = s8 ? g_.U16(o + 544u) : g_.U16(g_.U32(g_.U32(s5 + 856u) + 852u) + 544u);
            if (g_.U16(kStanceTable + 8u * sid + 2u) == 6u) s0 = sid;
        }
        int guard = 0;
        for (;;) {                                                         // 0x80091E6C: Rand() % 3
            const uint32_t rem = GuestRand(g_) % 3u;
            s1 = (rem == 0 ? 49u : 46u) + (rem == 1 ? 1u : 0u);            // 49 / 47 / 46
            if (s1 != s0) break;                                           // re-rolled
            if (++guard > 100000 || g_.Faulted()) {
                refused_ = true;
                return 0;
            }
        }
        if (s1 == 47u) phi = 20;
        seams_.CrashEvent(g_.U16(s5 + 172u), 1);
        if (!s8) ViewOr(s5, ((g_.U32(r + 552u) & 0x10000u) ? 0x0E000000u : 0x06000000u) | 0x80u);
        g_.W32(r + 580u, 65);
        break;
    }
    case 4: { // 0x80091F44: stances 41, 90
        s7 |= 0x82u;
        s1 = 43;
        g_.W32(r + 552u, g_.U32(r + 552u) | 0x40000000u);
        seams_.PoseInit(kPoseInitDFn, r, 0);
        LoadDir(r, s5);
        {
            const uint16_t v0 = g_.U16(r + 450u), v1 = g_.U16(r + 452u), a0 = g_.U16(r + 454u);
            g_.W16(r + 444u, v0);
            g_.W16(r + 446u, v1);
            g_.W16(r + 448u, a0);
        }
        const int32_t a1 = g_.S32(s5 + 480u);
        if (0x50000 < a1) {
            g_.W32(r + 480u, U(FixMul(0x8000, a1)));                       // half the bike's
        } else {
            const uint16_t x = g_.U16(r + 450u), z = g_.U16(r + 454u);     // reversed, 5.0 - bike's
            g_.W16(r + 450u, static_cast<uint16_t>(0u - x));
            const uint16_t y = g_.U16(r + 452u);
            g_.W16(r + 454u, static_cast<uint16_t>(0u - z));
            g_.W16(r + 452u, static_cast<uint16_t>(0u - y));
            g_.W32(r + 480u, 0x50000u - g_.U32(s5 + 480u));
        }
        GuestScale(g_, g_.S32(r + 480u), r + 450u, r + 456u);
        {
            const uint16_t v1 = g_.U16(r + 448u), v0 = g_.U16(r + 444u);
            g_.W16(r + 434u, 0);
            g_.W16(r + 432u, v1);
            g_.W16(r + 436u, static_cast<uint16_t>(0u - v0));
        }
        if (!Norm(r + 432u, n)) return 0;
        if (n == 0) BikeResetOrientation(g_, r);
        Op(r + 438u, r + 444u, r + 432u);
        seams_.CrashEvent(g_.U16(s5 + 172u), 1);
        if (!s8 && g_.U16(s5 + 172u) < g_.U32(g_.U32(kGsPtr) + 48u) && (g_.U32(s5 + 568u) & 0x200u))
            ViewOr(s5, (g_.U32(r + 552u) & 0x10000u) ? 0x0E000000u : 0x06000000u); // no 0x80 here
        break;
    }
    default: // 0x8009215C
        s1 = g_.U16(r + 544u);
        break;
    }
    if (refused_ || g_.Faulted()) return 0;
    // ---- the tail, 0x80092160
    seams_.RiderSync(r);
    if (!s8) {
        const uint32_t h = g_.U16(s5 + 172u);
        if (h < g_.U32(g_.U32(kGsPtr) + 48u)) g_.W32(kCamTargets + (h << 7) + 4u, r); // SLUS 0x800235B0
    }
    if (s3 != 0) QuatRot(r, s3, false);
    if (ang2 != 0) QuatRot(r, ang2, true);
    stance_.Event(s1 & 0xFFFFu, r, (s7 | 0x800u) | (U(phi) << 16));       // 0x80092408
    seams_.Settle(r);
    // BuildObbAlt 0x8008BD2C: its owner word is read only for a pool-0 entity (0x8008BA6C), never for
    // a rider, so it is peeked without a fault.
    BuildObbAlt(EntityView(Raw(r)), Raw(kStanceTable), S(Peek32(Peek32(r + 852u) + 604u)));
    g_.W8(r + 535u, 0);
    const uint32_t v = (g_.U32(r + 552u) | 1u) & 0xFFD8FFFFu;
    g_.W32(r + 552u, v);
    return v;
}

} // namespace rr::sim
