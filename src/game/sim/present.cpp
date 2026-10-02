#include "game/sim/present.h"

#include "game/sim/ai.h"
#include "game/sim/fight.h"
#include "game/sim/fixed.h"
#include "game/sim/pose.h"

// Every function is one guest function of RASHCDG.BIN (cfe43a77...); the comments carry its
// addresses. Loads and stores are in the original's order, with its re-reads of `R+540` (the
// animation object) after every store.

namespace rr::sim {
namespace {

inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t Mul32(int32_t a, int32_t b) { return S(U(a) * U(b)); }
inline int32_t Abs32(int32_t v) { const int32_t s = v >> 31; return S((U(s) + U(v)) ^ U(s)); }
// max(v, 0) + min(1.0 - v, 0), the engine's clamp to [0, 1] (0x800C3514 / 0x800C36C8)
inline int32_t Clamp01(int32_t v) {
    const int32_t a = S(~U(v >> 31) & U(v));
    const int32_t d = S(0x10000u - U(v));
    return S(U(a) + (U(d >> 31) & U(d)));
}

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kStanceTab = 0x800541D4;  // SLUS, 8-byte stance records
constexpr uint32_t kEventLists = 0x8005B3E4; // *(...) -> the ANIMNOIZ.DAT event list table (0 = none)
constexpr uint32_t kPool0Ptr = 0x8005B3A0;
constexpr uint32_t kPoolTable = 0x800CE4D0;
constexpr uint32_t kFollowTable = 0x8005BBA0; // 80 words, 0x800C45D8's jump table by stance
constexpr uint32_t kLeanTable = 0x8005BA98;   // 65 words, 0x800C3950's by stance - 18
constexpr uint32_t kMountStance = 0x800CCBA0; // u16 by mount, 0x800C45D8's default
constexpr uint32_t kIdleTimer = 0x800CCB9C;   // s32, the look-around interval
constexpr uint32_t kFlashAcc = 0x8005B2FC, kFlashCount = 0x800CCA7C;
constexpr uint32_t kSeatStance9 = 0x8005421C; // the stance-9 record (0x800C4968)

} // namespace

int32_t PresentLayer::LastKey(uint32_t a) {
    const uint32_t op = g_.U32(a + 4u) + 12u * g_.U32(a + 12u);
    const uint32_t cp = g_.U32(g_.U32(g_.U32(a + 40u) + 4u) + 4u * g_.U8(op));
    return S((U(static_cast<int32_t>(g_.U16(cp + 16u))) - 1u) << 16) >> 16;
}

// ============================================================================ the fall scrubbers

// 0x800C31CC FallScrubLean(R): stances 39 / 89.
uint32_t PresentLayer::FallScrubLean(uint32_t R) {
    uint32_t s4 = 0;
    const uint32_t mir = (g_.U32(R + 552u) >> 27) & 1u;
    const uint32_t B = g_.U32(R + 596u);
    const uint32_t fc = g_.U32(B + 568u);
    const int32_t s0 = g_.S32(B + 636u);                                // the lean +0x27C
    const uint32_t pass = g_.U32(B + 856u);
    const uint32_t s2 = (fc >> 5) & 1u;
    if (pass != 0u && mir == 0u) s4 = U(s0) >> 31;
    const int32_t last = LastKey(g_.U32(R + 540u));                      // s1 (0x800C3274)
    bool setLast = false, scrub = false;
    if (s2 != 0u) {
        if (s4 != 0u) {
            if (S(0xFFFF79F6u) < s0) scrub = true; else setLast = true;
        } else if (s0 == 0) {
            setLast = true;
        } else if (last != 0) {
            if (s0 <= 0) scrub = true; else setLast = true;
        } else {
            if (s0 < 0) setLast = true; else scrub = true;
        }
    } else {
        if (anim_.ClipDone(g_.U32(R + 540u)) != 0u || 0xC90F < Abs32(s0)) setLast = true;
        else return 0;                                                  // 0x800C335C, s2 = 0
    }
    if (setLast) {                                                      // 0x800C32F8
        uint32_t a = g_.U32(R + 540u);
        g_.W32(a + 36u, g_.U32(a + 36u) | 8u);
        g_.W32(g_.U32(R + 540u) + 16u, U(last));
        a = g_.U32(R + 540u);
        const int32_t v = S((U(last) - g_.U32(a + 16u)) << 16);
        g_.W32(a + 32u, U(Mul32(g_.S32(a + 24u), v) >> 16));
        g_.W32(g_.U32(R + 540u) + 28u, 0);
        return seams_.RiderLaunch(R);
    }
    (void)scrub;                                                        // 0x800C3364
    const int32_t a0 = S(U(s0) + (U(-last) & (0u - U(s0) - U(s0))));  // the mask is -(last key): the guest's register
    int32_t t = S(0u - U(FixMul(a0, 0x1E8EC)));
    if (s4 == 0u) t = S(U(t) + 0x10000u);
    const int32_t prod = Mul32(t, last);
    uint32_t a = g_.U32(R + 540u);
    g_.W32(a + 36u, g_.U32(a + 36u) | 8u);
    g_.W32(g_.U32(R + 540u) + 16u, U(prod >> 16));
    a = g_.U32(R + 540u);
    g_.W32(a + 32u, U(Mul32(g_.S32(a + 24u), S(U(prod) - (g_.U32(a + 16u) << 16))) >> 16));
    g_.W32(g_.U32(R + 540u) + 28u, 0);
    g_.W32(g_.U32(R + 540u) + 32u, 0);
    return 0;
}

// 0x800C341C FallScrubLift(R): stances 40 / 88.
uint32_t PresentLayer::FallScrubLift(uint32_t R) {
    uint32_t b = g_.U32(R + 596u);
    int32_t s1 = g_.S32(b + 616u);                                     // the bike's +0x268
    if (g_.U8(R + 572u) & 0x20u) b = g_.U32(b + 856u);
    const int32_t s3 = LastKey(g_.U32(R + 540u));
    g_.W32(R + 580u, g_.U32(b + 184u) - g_.U32(b + 504u));
    g_.W32(R + 584u, g_.U32(b + 188u) - g_.U32(b + 508u));
    g_.W32(R + 588u, g_.U32(b + 192u) - g_.U32(b + 512u));
    for (uint32_t k = 0; k < 9; ++k) g_.W16(R + 432u + 2u * k, g_.U16(b + 432u + 2u * k)); // SLUS 0x8003FA18
    if (g_.S32(b + 620u) < 0 && !(s1 < S(0xFFFF4D48u))) {
        s1 = Clamp01(FixMul(Abs32(s1), 0x16EB1));
        const int32_t prod = Mul32(s1, s3);
        uint32_t a = g_.U32(R + 540u);
        g_.W32(a + 36u, g_.U32(a + 36u) | 8u);
        g_.W32(g_.U32(R + 540u) + 16u, U(prod >> 16));
        a = g_.U32(R + 540u);
        g_.W32(a + 32u, U(Mul32(g_.S32(a + 24u), S(U(prod) - (g_.U32(a + 16u) << 16))) >> 16));
        g_.W32(g_.U32(R + 540u) + 28u, 0);
        return 0;
    }
    const uint32_t a1 = g_.U32(R + 540u);                               // 0x800C3594: pushed past the end
    const int32_t t = GuestDiv(S(g_.U32(a1 + 32u) << 16), g_.S32(a1 + 24u));
    uint32_t v1 = g_.U32(a1 + 16u) << 16;
    g_.W32(a1 + 36u, g_.U32(a1 + 36u) | 8u);
    v1 = v1 + U(t) + (U(s3) << 16);
    g_.W32(g_.U32(R + 540u) + 16u, U(S(v1) >> 16));
    const uint32_t a = g_.U32(R + 540u);
    g_.W32(a + 32u, U(Mul32(g_.S32(a + 24u), S(v1 - (g_.U32(a + 16u) << 16))) >> 16));
    g_.W32(g_.U32(R + 540u) + 28u, 0);
    return seams_.RiderLaunch(R);
}

// 0x800C3630 FallScrubPitch(R): stance 38 (and a passenger's 39 / 89).
uint32_t PresentLayer::FallScrubPitch(uint32_t R) {
    const uint32_t B = g_.U32(R + 596u);
    const uint32_t t0 = g_.U32(R + 540u);
    const int32_t x = Abs32(g_.S32(B + 652u));
    const int32_t s1 = LastKey(t0);
    if (!(0x138C2 < x)) {
        const int32_t v = Clamp01(FixMul(S(U(x) + 0xFFFF4D48u), 0x1E8EC));
        const int32_t prod = Mul32(v, s1);
        uint32_t a = g_.U32(R + 540u);
        g_.W32(a + 36u, g_.U32(a + 36u) | 8u);
        g_.W32(g_.U32(R + 540u) + 16u, U(prod >> 16));
        a = g_.U32(R + 540u);
        g_.W32(a + 32u, U(Mul32(g_.S32(a + 24u), S(U(prod) - (g_.U32(a + 16u) << 16))) >> 16));
        g_.W32(g_.U32(R + 540u) + 28u, 0);
        return 0;
    }
    g_.W32(t0 + 36u, g_.U32(t0 + 36u) | 8u);                            // 0x800C3748: the entry's object
    g_.W32(g_.U32(R + 540u) + 16u, U(s1));
    const uint32_t a = g_.U32(R + 540u);
    g_.W32(a + 32u, U(Mul32(g_.S32(a + 24u), S((U(s1) - g_.U32(a + 16u)) << 16)) >> 16));
    g_.W32(g_.U32(R + 540u) + 28u, 0);
    return seams_.RiderLaunch(R);
}

// ============================================================================ the riding lean

// 0x800C3950 RidingLean(R): 1 when it scrubbed the lean clip, else the stance event's byte or 0.
uint32_t PresentLayer::RidingLean(uint32_t R) {
    uint32_t s6 = 0;
    const uint32_t s2 = g_.U32(R + 596u);                               // the bike
    int32_t s1 = g_.S32(s2 + 672u);                                     // +0x2A0
    const int32_t s7 = LastKey(g_.U32(R + 540u));
    int32_t s3 = Abs32(s1);
    const uint32_t s5 = (g_.U8(R + 572u) >> 5) & 1u;                    // a passenger
    int32_t s4 = S(U(s7) << 16);
    if (s5 != 0u) {
        if (!(0xFFFF < g_.S32(R + 76u)) && s1 < 0) s4 = 0x38000;
        if (s3 == 0) {
            const uint32_t pb = g_.U32(g_.U32(R + 596u) + 856u);
            if (!(g_.U32(pb + 564u) & 2u)) {
                s3 = g_.S32(pb + 672u);
                s1 = (g_.S32(s2 + 636u) >= 0) ? s3 : S(0u - U(s3));
                s4 = (s1 <= 0) ? 0x18000 : 0x40000;
            }
        }
    } else if (g_.S32(s2 + 636u) > 0) {
        s6 = 1;
        if (g_.U32(s2 + 856u) != 0u && g_.U32(s2 + 1088u) != 0u) s4 = 0x50000;
    }
    {                                                                   // 0x800C3AA4: the grunt
        const int32_t clock = g_.S32(g_.U32(kGameStatePtr) + 16u);
        if (!(S(U(clock) - g_.U32(R + 548u)) < 301)) {
            const int32_t sp = g_.S32(s2 + 480u);
            const int32_t a1 = sp >> 16;
            const int32_t d = 150 - a1;
            const int32_t v1 = S(U(d >> 31) & (0u - U(d))) + 150 - a1;
            const int32_t a2 = S(U(v1) + (U(sp >> 31) & U(a1)));
            if (0xE666 < s3) {
                g_.W32(R + 548u, U(clock));
                seams_.ObjectSound(R, g_.S32(s2 + 636u) > 0 ? 15u : 12u, U(a2), 3u, 2u);
                if (Failed()) return 0;
            }
        }
    }
    if (s3 == 0) {                                                      // 0x800C3DDC
        const uint32_t st = g_.U16(R + 544u);
        if (g_.U16(kStanceTab + 8u * st + 2u) != 2u) return 0;
        const uint32_t idx = st - 18u;
        if (!(idx < 65u)) return 0;
        uint32_t ev = 0, p = 0;
        switch (g_.U32(kLeanTable + 4u * idx)) {
        case 0x800C3E28: ev = 11; p = 0; break;
        case 0x800C3E38: ev = 13; p = 1; break;
        case 0x800C3E40: ev = 77; p = 0; break;
        case 0x800C3E50: ev = 79; p = 1; break;
        case 0x800C3E6C: return 0;
        default: refused_ = true; return 0;
        }
        return seams_.StanceEvent(ev, R, p) & 0xFFu;
    }
    uint32_t a0;
    if (s5 != 0u) {
        a0 = 1;
        const uint32_t st = g_.U16(R + 544u);
        if (s1 > 0) { if (st == 75u || st == 82u) a0 = 0; }
        else if (s1 < 0) { if (st == 74u || st == 81u) a0 = 0; }
    } else {
        a0 = (g_.U16(kStanceTab + 8u * g_.U16(R + 544u) + 2u) != 2u) ? 1u : 0u;
    }
    if (a0 != 0u) {                                                     // 0x800C3BBC: into the lean clip
        const uint32_t cat = g_.U16(kStanceTab + 8u * g_.U16(R + 544u) + 2u);
        if (cat == 2u) g_.W32(R + 552u, g_.U32(R + 552u) & 0xFFBFFFFFu);
        else g_.W32(R + 552u, g_.U32(R + 552u) | 0x400000u);
        uint32_t ns;
        if (s5 != 0u) {
            if (g_.U16(R + 544u) == 79u) ns = (s1 > 0) ? 82u : 81u;
            else ns = (s1 > 0) ? 75u : 74u;
        } else if (g_.U32(s2 + 180u) < 9u) {
            ns = 19;
        } else {
            ns = (g_.U16(R + 544u) == 13u) ? 20u : 18u;
        }
        g_.W16(R + 544u, static_cast<uint16_t>(ns));
        const uint32_t clip = (g_.U32(kStanceTab + 8u * g_.U16(R + 544u)) >> 4) & 0xFFFu;
        const uint32_t lists = g_.U32(kEventLists);
        const uint32_t ex = lists != 0u ? g_.U32(lists + 72u) : 0u;
        anim_.LoopStart(g_.U32(R + 540u), clip, s6 & 0xFFu, 0, ex);
        if (anim_.Failed()) return 0;
        if (g_.U16(R + 608u) != 0u) g_.W16(R + 608u, 0);
    }
    const int32_t prod = Mul32(s3, s7);                                 // 0x800C3CC4
    int32_t a1 = s4;
    if (prod < a1) a1 = prod;
    {
        const uint32_t a = g_.U32(R + 540u);
        g_.W8(g_.U32(a + 4u) + 12u * g_.U32(a + 12u) + 2u, static_cast<uint8_t>(s6));
    }
    const uint32_t a2 = g_.U32(R + 552u);
    s3 = a1;
    if (a2 & 0x400000u) {                                               // slewed at most 2 keys a pass
        const uint32_t a = g_.U32(R + 540u);
        const int32_t t = GuestDiv(S(g_.U32(a + 32u) << 16), g_.S32(a + 24u));
        const int32_t cur = S((g_.U32(a + 16u) << 16) + U(t));
        const int32_t d = S(U(s3) - U(cur));
        if (0x20000 < d) s3 = S(U(cur) + 0x20000u);
        else if (d < S(0xFFFE0000u)) s3 = S(U(cur) - 0x20000u);
        else g_.W32(R + 552u, a2 & 0xFFBFFFFFu);
    }
    uint32_t a = g_.U32(R + 540u);
    g_.W32(a + 36u, g_.U32(a + 36u) | 8u);
    g_.W32(g_.U32(R + 540u) + 16u, U(s3 >> 16));
    a = g_.U32(R + 540u);
    g_.W32(a + 32u, U(Mul32(g_.S32(a + 24u), S(U(s3) - (g_.U32(a + 16u) << 16))) >> 16));
    g_.W32(g_.U32(R + 540u) + 28u, 0);
    return 1;
}

// ============================================================================ the follow-up dispatcher

// 0x800C45D8 FollowStance(R, out): the stance that follows a finished clip, and its request word.
uint32_t PresentLayer::FollowStanceHost(uint32_t R, uint32_t& out) {
    out = 16;
    const uint32_t st = g_.U16(R + 544u);
    const auto orTwo = [&](uint32_t v) { out |= 2u; return v; };
    if (st < 80u) {
        switch (g_.U32(kFollowTable + 4u * st)) {
        case 0x800C4610: return 79;
        case 0x800C4618: out = 3; return 50;
        case 0x800C4620: out = 3; return 51;
        case 0x800C4628: return orTwo(49);
        case 0x800C4634:
        case 0x800C4640: {
            const uint32_t v = (g_.U32(kFollowTable + 4u * st) == 0x800C4634u) ? 70u : 69u;
            if ((g_.U16(R + 544u) - 60u) < 4u) out = 4096;
            return orTwo(v);
        }
        case 0x800C4660: return orTwo(71);
        case 0x800C4698: return g_.U16(R + 544u);                       // `lhu a2,544(a0); jr ra`
        case 0x800C466C: return 4;
        case 0x800C4674: return 7;
        case 0x800C467C:
            if (0x7FFF < g_.S32(g_.U32(R + 596u) + 480u)) return 7;
            return g_.U16(R + 544u);
        case 0x800C46A4: out = 3; return 11;
        case 0x800C46B4: out = 1; return 13;
        case 0x800C46C4: {
            uint32_t v = 8;
            if (!(g_.U8(R + 572u) & 0x20u)) {
                const uint32_t B = g_.U32(R + 596u);
                if (!(0x7FFF < g_.S32(B + 480u))) {
                    const uint32_t gs = g_.U32(kGameStatePtr);
                    v = 5;
                    if (g_.U16(B + 172u) < g_.U32(gs + 48u)) {
                        const uint32_t rd = g_.U32(B + 1084u);
                        if (g_.U32(rd + 40u) != 0u && g_.U8(rd + 39u) < 4u && g_.U8(gs + 57u) == 0u) v = 1;
                    }
                }
            }
            return orTwo(v);
        }
        case 0x800C4764: break;
        default: refused_ = true; return 0;
        }
    }
    if ((static_cast<uint32_t>(g_.U16(R + 172u)) >> 5) != 1u) return 48;   // 0x800C4764
    if (g_.U32(R + 604u) < 2u && (g_.U8(R + 572u) & 0x20u)) return 77;
    return g_.U16(kMountStance + 2u * g_.U32(R + 604u));
}

uint32_t PresentLayer::FollowStance(uint32_t R, uint32_t outAddr) {
    uint32_t out = 0;
    const uint32_t v = FollowStanceHost(R, out);
    // Every path stores the word: 16 first, then its own value (0x800C45DC).
    g_.W32(outAddr, out);
    return v;
}

// 0x800C47CC FollowQueue(R): the queued stance +0x262[n - 1].
uint32_t PresentLayer::FollowQueue(uint32_t R) {
    const uint32_t n = g_.U16(R + 608u);
    if (n == 0u) return 0;
    const uint32_t f = g_.U32(R + 552u);
    const uint32_t ev = g_.U16(R + 2u * (n - 1u) + 610u);
    uint32_t p = (f & 0x10000000u) ? 34u : 36u;
    if (f & 0x08000000u) p |= 0x100u;
    if (seams_.StanceEvent(ev, R, p) != 0u) g_.W16(R + 608u, static_cast<uint16_t>(g_.U16(R + 608u) - 1u));
    return 1;
}

// 0x800C4860 FollowSeat(R): the kick-start, the push-off (stance 9 by RangedStart), the stop stances.
uint32_t PresentLayer::FollowSeat(uint32_t R) {
    const uint32_t B = g_.U32(R + 596u);
    g_.W32(R + 552u, g_.U32(R + 552u) & 0xEFFFFFFFu);
    const uint32_t cmd = B + 956u + 8u * U(static_cast<int32_t>(g_.S8(B + 946u)));
    bool lowMount;
    if (!(g_.U8(R + 572u) & 0x20u)) {
        const uint32_t m = g_.U32(R + 604u);
        if (m != 0u) {
            lowMount = m < 2u;
            goto seat;
        }
        if (!(g_.U16(cmd - 8u) < 3u) && 0x8000 < g_.S32(B + 480u)) {
            const uint32_t st = g_.U16(R + 544u);
            if (st == 4u) return seams_.StanceEvent(3, R, 16);
            if (st == 6u) return seams_.StanceEvent(7, R, 16);
            return 0;
        }
    }
    lowMount = g_.U32(R + 604u) < 2u;                                   // 0x800C4914
seat:
    if (lowMount) {                                                     // 0x800C4920
        const uint32_t B2 = g_.U32(R + 596u);
        if ((g_.U32(B2 + 564u) & 0x08000000u) && !(g_.U8(R + 572u) & 0x20u)) {
            if (g_.U16(R + 544u) == 9u) return 1;
            const uint32_t clip = (g_.U32(kSeatStance9) >> 4) & 0xFFFu;
            const uint32_t last = g_.U32(B2 + 856u) != 0u ? 4u : 13u;
            const uint32_t lists = g_.U32(kEventLists);
            const uint32_t ex = lists != 0u ? g_.U32(lists + 36u) : 0u;
            anim_.RangedStart(g_.U32(R + 540u), clip, 0, 0, last, 0, ex);
            if (anim_.Failed()) return 0;
            StanceLayer layer(g_, seams_);
            layer.SetRiderState(9, R, 0);                               // 0x800C49C8
            if (layer.Failed()) { refused_ = true; return 0; }
            return 1;
        }
    }
    if (g_.U32(R + 604u) != 1u) return 0;                               // 0x800C49D8
    {
        const uint32_t st = g_.U16(R + 544u);
        if (!((st - 9u) < 2u || st == 76u) && 0x8000 < g_.S32(B + 480u) &&
            g_.S32(B + 600u) < S(3u * g_.U32(B + 596u))) {
            const uint32_t ev = (g_.U8(R + 572u) & 0x20u) ? 76u : 10u;
            return seams_.StanceEvent(ev, R, 10);
        }
    }
    if (g_.U8(R + 572u) & 0x20u) return 0;                              // 0x800C4A5C
    if (0x7FFF < g_.S32(B + 480u)) return 0;
    if (g_.U32(B + 568u) & 0xFu) return 0;
    if (g_.U32(B + 564u) & 0x0021D000u) return 0;
    const uint32_t gs = g_.U32(kGameStatePtr);
    uint32_t ev = 5;
    if (g_.U16(B + 172u) < g_.U32(gs + 48u)) {
        const uint32_t rd = g_.U32(B + 1084u);
        if (g_.U32(rd + 40u) != 0u && g_.U8(rd + 39u) < 4u && g_.U8(gs + 57u) == 0u) ev = 1;
    }
    return seams_.StanceEvent(ev, R, 12);
}

// 0x800C4B30 FollowGear(R): +0x23C bits 2..3 -> stance 16.
uint32_t PresentLayer::FollowGear(uint32_t R) {
    const uint32_t b = g_.U8(R + 572u);
    if (b & 0x20u) return 0;
    if (!(b & 0xCu)) return 0;
    const uint32_t v = seams_.StanceEvent(16, R, (b << 5) & 0x100u);
    if (v != 0u) g_.W8(R + 572u, static_cast<uint8_t>(g_.U8(R + 572u) & 0xF3u));
    return v;
}

// 0x800C4BA0 FollowRide(R): the riding lean, the brake stance, the tuck by speed, the look-around.
uint32_t PresentLayer::FollowRide(uint32_t R) {
    uint32_t s1 = 0;
    const uint32_t B = g_.U32(R + 596u);
    if (g_.U32(R + 604u) != 1u) return 0;
    {
        const uint32_t st = g_.U16(R + 544u);
        if (!(g_.U16(kStanceTab + 8u * st + 2u) == 2u || (st - 77u) < 3u || (st - 11u) < 3u)) return 0;
    }
    s1 = RidingLean(R) & 0xFFu;
    if (Failed()) return 0;
    if (s1 == 0u) {
        if (g_.U32(B + 560u) & 1u) s1 = seams_.StanceEvent((g_.U8(R + 572u) & 0x20u) ? 77u : 8u, R, 4);
        if (s1 == 0u) {
            uint32_t st = g_.U16(R + 544u);
            uint32_t ev = 0;
            if ((st == 11u || st == 14u || st == 77u || st == 80u) && 0x1F4AE6 < g_.S32(B + 480u)) {
                ev = (g_.U8(R + 572u) & 0x20u) ? 78u : 12u;
            } else {
                st = g_.U16(R + 544u);
                if (((st - 12u) < 2u || st == 78u || st == 79u) && !(0x1AD27B < g_.S32(B + 480u)))
                    ev = (g_.U8(R + 572u) & 0x20u) ? 80u : 14u;
            }
            if (ev != 0u) s1 = seams_.StanceEvent(ev, R, 20);
        }
    }
    if (g_.U8(R + 572u) & 0x20u) return s1;                             // 0x800C4D30
    if (s1 != 0u) return s1;
    const int32_t since = S(g_.U32(g_.U32(kGameStatePtr) + 16u) - g_.U32(R + 548u));
    if (!(g_.S32(kIdleTimer) < since)) return s1;
    const uint32_t st = g_.U16(R + 544u);
    uint32_t ev;
    if (st == 11u) ev = g_.S32(B + 616u) > 0 ? 25u : 24u;
    else if (st == 13u) ev = 23u;
    else return s1;
    s1 = seams_.StanceEvent(ev, R, 4);
    uint32_t seed = g_.U32(g_.gp() + 2076u);                            // SLUS 0x8001FC58 Rand
    const uint32_t rnd = Rand(seed);
    g_.W32(g_.gp() + 2076u, seed);
    g_.W32(kIdleTimer, ((rnd % 6u) + 3u) * 300u);                      // 3..8 s to the next look
    return s1;
}

// 0x800C4E18 FollowNear(R): a rival riding alongside (Pick 0x8008B428 over pool 1) -> stances
// 15 / 16 / 17 by the along-road gap and the lateral side.
uint32_t PresentLayer::FollowNear(uint32_t R) {
    const uint32_t B = g_.U32(R + 596u);
    const uint32_t s2 = B + 956u + 8u * U(static_cast<int32_t>(g_.S8(B + 946u)));
    if (g_.U8(R + 572u) & 0x20u) return 0;
    if (g_.U32(R + 604u) != 1u) return 0;
    const uint32_t cmd = g_.U16(s2 - 8u);
    if (cmd < 4u || !(cmd < 13u)) return 0;
    const uint32_t h = fight::Pick(g_, B + 172u, 1) & 0xFFFFu;
    if (g_.Faulted()) return 0;
    if (h == 224u) return 0;
    uint32_t s1 = 224;
    const uint32_t o = g_.U32(kPool0Ptr) + h * 1096u;
    const uint32_t gap = (g_.U32(o + 324u) - g_.U32(B + 324u)) << 4;    // a1
    const uint32_t oc = o + 956u + 8u * U(static_cast<int32_t>(g_.S8(o + 946u)));
    bool fight = false;                                                 // a2
    {
        const uint32_t c0 = g_.U16(s2 - 8u);
        if (c0 == 16u || c0 == 6u) fight = true;
        else {
            const uint32_t c1 = g_.U16(oc - 8u);
            if (c1 == 16u || c1 == 6u) fight = true;
        }
    }
    if (!(0x13FFFFu < gap - 0xA0000u)) {
        if (fight) s1 = 15;
    } else if (!(0x9FFFFu < gap) && fight) {
        const uint32_t st = g_.U16(R + 544u);
        s1 = (st == 13u || st == 20u) ? 17u : 16u;
    }
    if (s1 == 224u) return 0;
    if (s1 == g_.U16(R + 544u)) return 0;
    int32_t d;
    if (g_.U32(o + 360u) == g_.U32(B + 360u) &&
        ((g_.U32(o + 360u) >> 16) == 0u || g_.U32(o + 336u) == g_.U32(B + 336u))) {
        d = S(g_.U32(o + 344u) - g_.U32(B + 344u));
        if (g_.S32(B + 364u) < 0) d = S(0u - U(d));
    } else {
        int32_t p[3], q[3];
        int16_t ax[3];
        for (uint32_t k = 0; k < 3; ++k) {
            p[k] = g_.S32(o + 184u + 4u * k);
            q[k] = g_.S32(B + 184u + 4u * k);
            ax[k] = g_.S16(B + 432u + 2u * k);
        }
        d = AiProject(p, ax, q);                                        // 0x800B6AAC
    }
    if (0xEFFFF < Abs32(d)) return 0;
    return seams_.StanceEvent(s1, R, d < 0 ? 260u : 4u);
}

// 0x800C5078 FollowUp(R): the first child that acts wins; else, when the clip is done, the next
// stance of FollowStance.
uint32_t PresentLayer::FollowUp(uint32_t R) {
    if (g_.U16(kStanceTab + 8u * g_.U16(R + 544u) + 2u) == 3u) return 0;
    if (FollowQueue(R) != 0u || Failed()) return 0;
    if (FollowSeat(R) != 0u || Failed()) return 0;
    if (FollowGear(R) != 0u || Failed()) return 0;
    if (FollowRide(R) != 0u || Failed()) return 0;
    if (FollowNear(R) != 0u || Failed()) return 0;
    const uint32_t st = g_.U16(R + 544u);
    if (g_.U16(kStanceTab + 8u * st + 2u) == 2u || st == 39u) return 0;
    if (anim_.ClipDone(g_.U32(R + 540u)) == 0u) return 0;
    uint32_t out = 0;
    const uint32_t ev = FollowStanceHost(R, out) & 0xFFFFu;
    if (Failed()) return 0;
    return seams_.StanceEvent(ev, R, out);
}

// 0x80090814 PresentationPass(dt): over pool 0.
uint32_t PresentLayer::Pass(int32_t dt) {
    int32_t n = g_.S32(g_.U32(kPoolTable + 12u));
    uint32_t e = g_.U32(kPoolTable);
    while (n >= 0) {
        const uint32_t gs = g_.U32(kGameStatePtr);
        bool run = true;
        if (!(g_.U16(e + 172u) < g_.U32(gs + 48u)) && (g_.U8(g_.U32(e + 1084u) + 1u) & 0xFu) == 2u &&
            !(g_.U8(e + 928u) & 0x10u))
            run = false;
        if (run) {
            if (g_.S16(e + 320u) != 0) {
                const uint32_t a0 = g_.U32(kGameStatePtr);
                const uint32_t hnd = g_.U16(e + 172u);
                bool flash = hnd < g_.U32(a0 + 48u) && hnd == 0u && (g_.U32(a0 + 4u) & 0x18u) == 16u;
                if (!flash) {
                    const uint32_t b4 = g_.U8(a0 + 4u);
                    flash = !(b4 & 0x10u) && (b4 & 1u) && g_.U16(e + 172u) == g_.U8(a0 + 6u);
                }
                if (flash) {                                            // 0x80090930
                    const uint32_t acc = g_.U32(kFlashAcc) + U(dt);
                    const int32_t a3 = g_.S32(kFlashCount);
                    g_.W32(kFlashAcc, acc);
                    if (a3 < 0) {
                        const uint32_t gs2 = g_.U32(kGameStatePtr);
                        if (!(S(acc) < g_.S32(0x8005306Cu + 4u * g_.U32(gs2 + 60u)))) {
                            const uint32_t f = g_.U32(e + 36u);
                            const uint32_t r = g_.U32(e + 852u);
                            g_.W32(kFlashAcc, 0);
                            g_.W32(e + 36u, f | 0x800u);
                            const uint32_t rf = g_.U32(r + 36u);
                            g_.W32(kFlashCount, 1);
                            g_.W32(r + 36u, rf | 0x800u);
                        }
                    } else {
                        const uint32_t a2 = g_.U32(kGameStatePtr);
                        if (!(S(acc) < g_.S32(0x80053060u + 4u * g_.U32(a2 + 60u)))) {
                            const uint32_t f = g_.U32(e + 36u);
                            const uint32_t r = g_.U32(e + 852u);
                            g_.W32(e + 36u, (f & 0xFFFFF7FFu) | ((((f >> 11) & 1u) ^ 1u) << 11));
                            const uint32_t rf = g_.U32(r + 36u);
                            g_.W32(r + 36u, (rf & 0xFFFFF7FFu) | ((((rf >> 11) & 1u) ^ 1u) << 11));
                            if ((g_.U32(e + 36u) >> 11) & 1u) g_.W32(kFlashCount, U(a3 + 1));
                            if (!(g_.S32(kFlashCount) < g_.S32(0x80053078u + 4u * g_.U32(a2 + 60u))) &&
                                !((g_.U32(e + 36u) >> 11) & 1u))
                                g_.W32(kFlashCount, 0xFFFFFFFFu);
                            g_.W32(kFlashAcc, 0);
                        }
                    }
                }
                uint32_t s2 = (g_.U8(g_.U32(e + 852u) + 572u) >> 4) & 1u;  // a passenger rides too
                const uint32_t P = g_.U32(e + 856u);
                if (P != 0u) {                                          // the passenger's bike copy
                    for (uint32_t k = 0; k < 9; ++k) g_.W16(P + 432u + 2u * k, g_.U16(e + 432u + 2u * k));
                    for (uint32_t k = 0; k < 9; ++k) g_.W16(P + 516u + 2u * k, g_.U16(e + 516u + 2u * k));
                    g_.W16(P + 450u, g_.U16(e + 450u));
                    g_.W16(P + 452u, g_.U16(e + 452u));
                    g_.W16(P + 454u, g_.U16(e + 454u));
                    for (uint32_t off : {568u, 564u, 480u, 636u, 652u, 616u, 620u, 676u}) g_.W32(P + off, g_.U32(e + off));
                    g_.W16(P + 320u, g_.U16(e + 320u));
                }
                uint32_t r = g_.U32(e + 852u);
                for (;;) {                                              // 0x80090B48
                    const uint32_t f0 = g_.U32(r + 552u);
                    if (f0 & 0x4000u) {
                        const uint32_t b = g_.U32(r + 596u);
                        g_.W32(r + 552u, f0 | 0x8000u);
                        g_.W32(b + 568u, g_.U32(b + 568u) | 8u);
                    }
                    if (g_.U32(r + 552u) & 0x8000u) {
                        const uint32_t m = g_.U32(r + 604u);
                        if ((m - 1u) < 2u || (m == 0u && g_.U16(r + 544u) != 0u)) seams_.RiderKnockOff(r);
                    }
                    const uint32_t f1 = g_.U32(r + 552u);
                    if (f1 & 0x4000u) {
                        const uint32_t b = g_.U32(r + 596u);
                        g_.W32(r + 552u, f1 & 0xFFFFBFFFu);
                        g_.W32(b + 568u, g_.U32(b + 568u) & 0xFFFFFFF7u);
                    }
                    if (g_.U32(r + 604u) == 2u) {                       // leaving the bike
                        const uint32_t st = g_.U16(r + 544u);
                        if (st == 88u || st == 40u) {
                            FallScrubLift(r);
                        } else if (st == 39u || st == 89u) {
                            if ((g_.U8(r + 572u) & 0x20u) && (g_.U32(e + 568u) & 0x40u)) FallScrubPitch(r);
                            else if (g_.U32(e + 568u) & 0x200u) seams_.RiderLaunch(r);
                            else FallScrubLean(r);
                        } else if (st == 38u) {
                            FallScrubPitch(r);
                        } else if (anim_.ClipDone(g_.U32(r + 540u)) != 0u) {
                            seams_.RiderLaunch(r);
                        }
                    } else if (!(g_.U32(r + 552u) & 0x02000000u)) {
                        FollowUp(r);
                    }
                    if (Failed()) return 0;
                    if (s2 == 0u) break;                                // 0x80090D08
                    r = g_.U32(g_.U32(e + 856u) + 852u);
                    const uint32_t v0 = s2;
                    s2 -= 1u;
                    if (!(S(v0) > 0)) break;
                }
            }
            g_.W32(e + 560u, g_.U32(e + 560u) & 0xFFFFFFDEu);           // 0x80090D2C
            g_.W8(e + 928u, static_cast<uint8_t>(g_.U8(e + 928u) & 0xFDu));
        }
        n -= 1;
        e += g_.U32(kPoolTable + 4u);
        if (Failed()) return 0;
    }
    return 0;
}

} // namespace rr::sim
