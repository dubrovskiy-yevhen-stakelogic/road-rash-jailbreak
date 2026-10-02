#include "game/sim/recover_air.h"

#include "game/sim/ai.h"
#include "game/sim/coll_util.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {

namespace {

using cu::GDot;
using rc::S;
using rc::U;

// The side test of the wall records (0x80097814 / 0x8009799C): the wall's normal +0x10 (x) / +0x14 (z)
// against the offset from the rider's box centre to the wall's point +4 / +0x0C, in whole units, with
// the 32-bit `mult` low words and a wrapping add; true when it is positive (the rider is behind it).
bool BehindWall(GuestRam& g, uint32_t R, uint32_t rec) {
    const int32_t dx = S(g.U32(rec + 4u) - g.U32(R + 184u)) >> 16;
    const int32_t px = cu::MulLo(g.S16(rec + 16u), dx);
    const int32_t dz = S(g.U32(rec + 12u) - g.U32(R + 192u)) >> 16;
    const int32_t pz = cu::MulLo(g.S16(rec + 20u), dz);
    return S(U(px) + U(pz)) > 0;
}

} // namespace

// ============================================================================ RASHCDG 0x8008F404
bool RiderGroundStep(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - kRiderGroundStepFrame;
    const uint32_t kind = g.U16(kRcStanceTable + 8u * g.U16(R + 544u) + 2u);
    if (kind != 5u) {
        if ((g.U32(R + 552u) & 2u) != 0) {
            if (!rc::Call(c, kAirRoadRebindFn, {R, 1u}, F)) return false;
            if (!rc::Call(c, kAirSettleFn, {R}, F)) return false;
            g.W32(R + 552u, (g.U32(R + 552u) | 1u) & ~2u); // 0x8008F47C
        }
        const uint32_t f = g.U32(R + 552u);
        if ((f & 0x40000000u) != 0) {
            if (!rc::Call(c, kRiderAirStepFn, {R, U(dt)}, F)) return false;
        } else if ((f & 0x04000000u) == 0) {
            if (!rc::Call(c, kAirGroundFn, {R, U(dt)}, F)) return false;
        }
    }
    if (!rc::IsPlayerRider(g, g.U16(R + 172u))) return !g.Faulted();
    const uint32_t view = kRcViewArray + kRcViewStride * g.U16(g.U32(R + 596u) + 172u);
    if ((g.U32(view + 548u) & 0x20000u) == 0) return !g.Faulted();
    const uint32_t acc = g.U32(view + 792u) + U(dt);
    g.W32(view + 792u, acc); // 0x8008F548
    if (!(0x8000 < S(acc))) return !g.Faulted();
    g.W32(view + 792u, 0); // 0x8008F558
    if (g.U16(R + 544u) == 43u) {
        g.W32(view + 548u, g.U32(view + 548u) & 0xFFF1FFFFu); // 0x8008F580
        return !g.Faulted();
    }
    // The shake's side axis: the up vector +0x20A crossed with the heading +0x1C2, normalised.
    rc::GteOp(g, R + 522u, R + 450u, F + 16u);
    int32_t sum = 0;
    if (!rc::GNormalize(g, F + 16u, t, sum)) return false;
    const uint32_t notTop = (g.U32(view + 548u) >> 31) ^ 1u;
    int32_t side = 0x8000; // s2
    int32_t len = 0;       // s0
    if ((GuestRand(g) & 1u) != 0) {
        const uint32_t r = GuestRand(g);
        side = S((r & 0xFFFFu) - U(side));
        const int32_t sp480 = g.S32(R + 480u);
        const int32_t hi = static_cast<int32_t>((static_cast<int64_t>(sp480) * 0x55555556LL) >> 32);
        len = S(U(hi) - U(sp480 >> 31)); // +0x1E0 / 3
    } else {
        const uint32_t r = GuestRand(g);
        const uint32_t hi = static_cast<uint32_t>((static_cast<uint64_t>(r) * 0xAAAAAAABull) >> 32);
        const uint32_t q = hi >> 17;
        const uint32_t rem = r - (((q << 1) + q) << 16); // r % 0x30000
        side = S(((0u - notTop) & rem) + 0x30000u);
        if ((GuestRand(g) & 1u) != 0) side = S(0u - U(side));
        len = g.S32(R + 480u);
    }
    if (len < 0xC0000) len = 0xC0000;
    g.W32(view + 184u, g.U32(R + 184u) + U(FixMul(cu::Shl(g.S16(R + 450u), 4), len))); // 0x8008F6BC
    g.W32(view + 192u, g.U32(R + 192u) + U(FixMul(cu::Shl(g.S16(R + 454u), 4), len))); // 0x8008F6DC
    g.W32(view + 184u, g.U32(view + 184u) + U(FixMul(cu::Shl(g.S16(F + 16u), 4), side))); // 0x8008F6FC
    const int32_t dz = FixMul(cu::Shl(g.S16(F + 20u), 4), side);
    g.W32(view + 192u, g.U32(view + 192u) + U(dz));                                  // 0x8008F72C
    g.W32(view + 548u, (g.U32(view + 548u) & 0x7FFDFFFFu) | 0xC0000u);             // 0x8008F730
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800976C4
bool RiderAirStep(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - kRiderAirStepFrame;
    if ((g.U32(R + 552u) & 0x80000u) == 0) {
        bool wallHit = false; // s5: the player's edge record
        bool sideHit = false; // s3: the road side or the junction arm
        uint32_t normal = 0;  // s4
        if (rc::IsPlayerRider(g, g.U16(R + 172u))) {
            const uint32_t rec = kAirEdgeRecords + kAirEdgeStride * g.U16(g.U32(R + 596u) + 172u);
            if (g.S16(rec + 22u) != 0 && g.U32(rec) == g.U32(R + 360u)) {
                int32_t big = cu::Iabs(S(g.U32(rec + 4u) - g.U32(R + 184u)) >> 16);
                int32_t small = cu::Iabs(S(g.U32(rec + 12u) - g.U32(R + 192u)) >> 16);
                if (big < small) {
                    const int32_t x = big;
                    big = small;
                    small = x;
                }
                const int32_t s = small + (small >> 1);
                const int32_t d = big - (big >> 5) - (big >> 7) + (s >> 2) + (s >> 6);
                if (d < 50) {
                    const int32_t dot = GDot(g, R + 450u, rec + 16u);
                    wallHit = false;
                    if (dot <= 0) wallHit = BehindWall(g, R, rec);
                    normal = rec + 16u;
                }
            }
        }
        if ((g.U32(R + 388u) & 1u) != 0 && g.U32(R + 372u) != 0 && ((g.U32(R + 36u) >> 9) & 1u) == 0) {
            const uint32_t slice = g.U32(R + 340u);
            const int32_t lat = g.S32(R + 344u);
            const uint32_t m49 = g.U8(slice + 49u);
            const bool margin = (lat > 0) ? (g.U8(slice + 48u) != 0) : (m49 != 0);
            if (margin) {
                const int32_t across = GDot(g, R + 450u, g.U32(R + 340u) + 2u);
                const uint32_t sl = g.U32(R + 340u);
                sideHit = false;
                if (S(U(g.U8(sl + 48u)) << 15) < lat && across > 0) sideHit = true;
                else if (lat < S(0u - (U(g.U8(sl + 49u)) << 15)) && across < 0) sideHit = true;
                if (sideHit) {
                    const uint32_t row = g.U32(R + 340u);
                    normal = row + 2u;
                    if (across > 0) {
                        g.W16(F + 24u, static_cast<uint16_t>(0u - g.U16(row + 2u))); // 0x80097950
                        g.W16(F + 26u, static_cast<uint16_t>(0u - g.U16(normal + 2u)));
                        g.W16(F + 28u, static_cast<uint16_t>(0u - g.U16(normal + 4u)));
                        normal = F + 24u;
                    }
                }
            } else {
                uint32_t rec = 0;
                if (!rc::Call(c, kJunctionMarginFn, {R}, F, &rec)) return false;
                if (rec != 0) {
                    const int32_t dot = GDot(g, R + 450u, rec + 16u);
                    sideHit = false;
                    if (dot <= 0) sideHit = BehindWall(g, R, rec);
                    if (sideHit) normal = rec + 16u;
                }
            }
        }
        if (sideHit || wallHit) {
            uint32_t v0 = 0;
            if (!rc::Call(c, kRiderAirHitFn, {R, normal, 0x80000u}, F, &v0)) return false;
            if (v0 == 2u && !rc::Call(c, kAirStanceEventFn, {43u, R, 3u}, F)) return false;
        }
    }
    // 0x80097A28: the drag on the velocity +0x1C8 (and gravity on its y), its length, the heading.
    const int32_t drag = FixMul(g.S32(R + 580u), dt);
    g.W32(R + 456u, g.U32(R + 456u) - U(FixMul(drag, g.S32(R + 456u)))); // 0x80097A58
    const int32_t dy = S(U(FixMul(drag, g.S32(R + 460u))) - U(FixMul(0x9D087, dt)));
    g.W32(R + 460u, g.U32(R + 460u) - U(dy));                             // 0x80097A88
    g.W32(R + 464u, g.U32(R + 464u) - U(FixMul(drag, g.S32(R + 464u)))); // 0x80097A9C
    int32_t vel[3];
    cu::Read32x3(g, R + 456u, vel);
    const int32_t len = Length3(vel, t.sqrt);
    g.W32(R + 480u, U(len)); // 0x80097AA4
    if (len != 0) ScaleTo16(g, rc::Recip(len), R + 456u, R + 450u);
    if (!rc::Call(c, kAirRowsFromUpFn, {R}, F)) return false;
    if (rc::IsPlayerRider(g, g.U16(R + 172u)) && (g.U32(R + 388u) & 1u) == 0) {
        uint32_t v0 = 0;
        if (!rc::Call(c, kRoadEdgeProbeFn, {R, g.U32(R + 348u), g.U32(R + 344u), R + 328u, R + 522u}, F, &v0))
            return false;
        if (v0 == 1u) {
            const uint16_t x = g.U16(R + 522u), z = g.U16(R + 526u);
            g.W16(R + 522u, static_cast<uint16_t>(0u - x)); // 0x80097B78
            g.W16(R + 526u, static_cast<uint16_t>(0u - z));
            g.W16(R + 524u, static_cast<uint16_t>(0u - g.U16(R + 524u)));
        }
    }
    g.W32(R + 552u, g.U32(R + 552u) & 0x7FF9FFFFu); // 0x80097BA0
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A966C
bool RiderAirHit(GuestRam& g, uint32_t R, uint32_t dir, int32_t k, uint32_t sp, const BikeTables& t,
                 RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRiderAirHitFrame;
    v0 = 0;
    const uint32_t axis = R + 432u;
    const uint32_t glancing = (0xB332 < cu::Iabs(GDot(g, axis, dir))) ? 0u : 1u; // s3
    const uint32_t behind = U(GDot(g, R + 450u, R + 444u)) >> 31;                  // s5
    uint32_t mine = 0;                                                             // s1
    if (glancing != 0) mine = ((g.U16(R + 172u) >> 5) == 1u) ? 1u : 0u;
    uint32_t r = 0;
    if (!rc::Call(c, kAirBounceFn, {dir, R + 450u, R + 480u, R + 456u, 5u, U(k), mine}, F, &r)) return false;
    if (S(r) < 0) return !g.Faulted();
    if (r != 0) {
        const uint16_t x = g.U16(R + 450u), y = g.U16(R + 452u), z = g.U16(R + 454u);
        g.W16(R + 444u, x); // 0x800A9740
        g.W16(R + 446u, y);
        g.W16(R + 448u, z);
        if (behind != 0) {
            g.W16(R + 444u, static_cast<uint16_t>(0u - x));
            g.W16(R + 446u, static_cast<uint16_t>(0u - y));
            g.W16(R + 448u, static_cast<uint16_t>(0u - z));
        }
        if (mine != 0) {
            g.W16(R + 444u, static_cast<uint16_t>(0u - g.U16(R + 444u)));
            g.W16(R + 448u, static_cast<uint16_t>(0u - g.U16(R + 448u)));
            g.W16(R + 446u, static_cast<uint16_t>(0u - g.U16(R + 446u)));
        }
        const uint16_t az = g.U16(R + 448u), ax = g.U16(R + 444u);
        g.W16(R + 434u, 0); // 0x800A97A8
        g.W16(R + 432u, az);
        g.W16(R + 436u, static_cast<uint16_t>(0u - ax));
        int32_t sum = 0;
        if (!rc::GNormalize(g, axis, t, sum)) return false;
        if (sum == 0) {
            g.W16(R + 432u, 4096); // 0x800A97C8
            g.W16(R + 436u, 0);
        }
        rc::GteOp(g, R + 444u, axis, R + 438u);
    }
    v0 = ((r & glancing) != 0) ? 2u : 1u;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8003E338
bool JunctionMargin(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0) {
    (void)t;
    const uint32_t F = sp - kJunctionMarginFrame;
    v0 = 0;
    if (e == 0) return true;
    if (g.S16(e + 320u) == 0) return !g.Faulted();
    const uint32_t obj = g.U32(e + 328u);
    if (obj == 0 || g.U32(obj + 104u) == 0) return !g.Faulted();
    int32_t found = -1;
    const uint32_t node = RoadNodeRecord(g, g.S32(obj));
    if (g.Faulted()) return false;
    if (node != 0) {
        uint32_t r = 0;
        if (!rc::Call(c, kAirNodeWedgeFn, {e + 184u, node, F + 24u, F + 28u, 0xFFFFFFFFu}, F, &r)) return false;
        found = S(r);
    }
    if (found < 0) return !g.Faulted();
    const uint32_t list = g.U32(g.U32(e + 328u) + 104u);
    const int32_t n = g.S16(list + 2u);
    if (n <= 0) return !g.Faulted();
    const int32_t roadA = g.S16(g.U32(F + 24u) + 4u);
    const uint32_t armB = g.U32(F + 28u);
    uint32_t p = list + 4u;
    for (int32_t i = 0; i < n; ++i, p += 24u) {
        const int32_t first = g.S16(p);
        if (first == roadA && g.S16(p + 2u) == g.S16(armB + 4u)) {
            v0 = p;
            break;
        }
        if (g.S16(p + 2u) == roadA && first == g.S16(armB + 4u)) {
            v0 = p;
            break;
        }
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80038550
bool RoadEdgeProbe(GuestRam& g, uint32_t e, int32_t along, int32_t lateral, uint32_t cur, uint32_t out,
                   uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRoadEdgeProbeFrame;
    v0 = 0;
    g.W32(F + 84u, 0); // 0x8003857C: the second candidate's +0x1C
    const uint32_t slice = g.U32(cur + 12u);
    const int32_t index = g.S16(slice);
    bool end = index == 0;
    if (!end) end = index == g.S16(g.U32(cur + 8u) + 10u) - 1;
    if (g.Faulted()) return false;
    if (end) {
        uint32_t missing = 0;
        if (!rc::Call(c, kAirRoadMissingFn, {cur, 1u}, F, &missing)) return false;
        if (missing != 0) return !g.Faulted();
    }
    GuestCopyWords(g, F + 24u, cur, 32u);
    uint32_t n = 0;
    if (!rc::Call(c, kAirNeighboursFn, {e + 172u, F + 24u, F + 56u, F + 152u, 3u}, F, &n)) return false;
    if (n == 0) return !g.Faulted();
    const int32_t tn = RatTan(g.S16(slice + 36u), t.sincos);
    const int32_t chord = S(g.U32(slice + 32u) - U(FixMul(lateral, tn)));
    const int32_t q = cu::SDiv(along, chord);
    const int32_t rest = S(0x10000u - U(q));
    const int32_t w = S((U(q) & ~U(q >> 31)) + (U(rest) & U(rest >> 31))); // clamp to 0..0x10000
    g.W32(F + 16u, U(w)); // 0x80038698: 0x8002EB78's fifth argument
    int16_t a[3], b[3], o[3];
    cu::Read16x3(g, slice + 8u, a);
    cu::Read16x3(g, g.U32(F + 68u) + 8u, b);
    if (g.Faulted()) return false;
    Blend16(a, b, o, S(0x10000u - U(w)), w);
    cu::Write16x3(g, out, o);
    int32_t sum = 0;
    if (!rc::GNormalize(g, out, t, sum)) return false;
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B208C
bool RiderLand(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    (void)t;
    const uint32_t F = sp - kRiderLandFrame;
    const uint32_t N = F + 64u, DEPTH = F + 72u, IMP = F + 32u;
    for (uint32_t k = 0; k < 3; ++k) g.W16(N + 2u * k, static_cast<uint16_t>(0u - g.U16(R + 522u + 2u * k))); // 0x800B20B4
    uint32_t corner = 0;
    if (!rc::Call(c, kAirCornerMinFn, {R + 196u, N, R + 504u, DEPTH, 0u}, F, &corner)) return false;
    if (corner >= 8u) return !g.Faulted();
    const uint16_t h = g.U16(R + 172u);
    const uint32_t mine = ((h >> 5) == 1u) ? R : 0u; // s2: a rider (pool 1)
    if (rc::IsPlayerRider(g, h) || ((h >> 5) == 1u && (g.U8(R + 572u) & 0x20u) != 0)) {
        if (!rc::Call(c, kAirEffectBurstFn, {R, 3u, 300u, 1u}, F)) return false;
    }
    // The tail at 0x800B271C: the view's +0x314 floored at 0 on +0x228 bit 19.
    auto tail = [&]() -> bool {
        if (mine != 0 && (g.U32(R + 552u) & 0x80000u) != 0) {
            const uint32_t view = kRcViewArray + kRcViewStride * g.U16(g.U32(mine + 596u) + 172u);
            if (g.S32(view + 788u) < 0) g.W32(view + 788u, 0); // 0x800B2778
        }
        return !g.Faulted();
    };
    if (g.S8(R + 534u) == 4 || (g.U32(R + 552u) & 0x04000000u) != 0) {
        if (0x50000 < cu::Iabs(g.S32(DEPTH))) {
            g.W32(R + 464u, 0); // 0x800B21B0
            g.W32(R + 460u, 0);
            g.W32(R + 456u, 0);
            g.W32(R + 552u, (g.U32(R + 552u) | 0x04000000u) & 0xBFFFFFFFu); // 0x800B21C4
        }
        if (rc::IsPlayerRider(g, g.U16(R + 172u))) g.W32(R + 552u, g.U32(R + 552u) | 0x80000u); // 0x800B2208
        return tail();
    }
    cu::GScale(g, g.S32(DEPTH), N, IMP);
    if (!rc::Call(c, kAirImpulseFn, {R, IMP, 1u}, F)) return false;
    bool bounced = true;
    if (!(g.S16(N + 2u) < -2632)) {
        uint32_t v = 0;
        if (!rc::Call(c, kRiderAirHitFn, {R, N, 0x80000u}, F, &v)) return false;
        if (v == 2u && g.U16(R + 544u) != 43u && !rc::Call(c, kAirStanceEventFn, {43u, R, 3u}, F)) return false;
    } else if (g.S8(R + 535u) <= 0 && 0x6B4A1 < g.S32(R + 480u)) {
        if (!rc::Call(c, kRiderAirHitFn, {R, N, 0u}, F)) return false;
    } else {
        bounced = false;
    }
    if (bounced) {
        if (!rc::Call(c, kAirSoundFn, {g.U32(R + 184u), g.U32(R + 192u), 55u, 0u}, F)) return false;
    } else {
        // 0x800B22A0: the landing. The contact point's height over the box centre along the up vector.
        const int32_t dx = S(g.U32(R + 504u) - g.U32(R + 184u));
        g.W32(F + 48u, U(dx));
        const int32_t dy = S(g.U32(R + 508u) - g.U32(R + 188u));
        g.W32(F + 52u, U(dy));
        const int32_t dz = S(g.U32(R + 512u) - g.U32(R + 192u));
        g.W32(F + 56u, U(dz));
        const int32_t px = FixMul(dx, cu::Shl(g.S16(R + 522u), 4));
        const int32_t py = FixMul(dy, cu::Shl(g.S16(R + 524u), 4));
        const int32_t uz = cu::Shl(g.S16(R + 526u), 4);
        const uint64_t pz64 = static_cast<uint64_t>(static_cast<int64_t>(dz) * uz);
        g.W32(F + 80u, static_cast<uint32_t>(pz64)); // 0x800B2348: the product's lo / hi spilled
        g.W32(F + 84u, static_cast<uint32_t>(pz64 >> 32));
        const int32_t pz = FixMul(dz, uz);
        const int32_t d = S(U(pz) + U(S(U(py) + U(px))));
        g.W32(R + 508u, U(d));             // 0x800B236C
        g.W32(R + 508u, U(d < 0 ? 0 : d)); // 0x800B2380
        if (rc::IsPlayerRider(g, g.U16(R + 172u)) && (g.U32(R + 388u) & 1u) == 0) {
            uint32_t v = 0;
            if (!rc::Call(c, kRoadEdgeProbeFn, {R, g.U32(R + 348u), g.U32(R + 344u), R + 328u, R + 438u}, F, &v))
                return false;
            if (v == 1u) {
                const uint16_t x = g.U16(R + 438u), z = g.U16(R + 442u);
                g.W16(R + 438u, static_cast<uint16_t>(0u - x)); // 0x800B23EC
                g.W16(R + 442u, static_cast<uint16_t>(0u - z));
                g.W16(R + 440u, static_cast<uint16_t>(0u - g.U16(R + 440u)));
            }
        }
        if (!rc::Call(c, kAirRowsFromUpFn, {R}, F)) return false;
        const int32_t dot = GDot(g, R + 528u, R + 450u);
        const uint16_t fx = g.U16(R + 528u), fy = g.U16(R + 530u), fz = g.U16(R + 532u);
        g.W32(DEPTH, U(dot)); // 0x800B2424
        g.W16(R + 450u, fx);
        g.W16(R + 452u, fy);
        g.W16(R + 454u, fz);
        if (dot < 0) {
            g.W16(R + 450u, static_cast<uint16_t>(0u - fx));
            g.W16(R + 452u, static_cast<uint16_t>(0u - fy));
            g.W16(R + 454u, static_cast<uint16_t>(0u - fz));
        }
        // The lying stance: 43 -> 45, 47 -> 58, 46 -> 52 / 53 / 54 by a Rand, any other kept.
        const uint32_t st = g.U16(R + 544u);
        uint32_t ev = st;
        if (st == 46u) {
            const uint32_t r = GuestRand(g);
            const uint32_t q = static_cast<uint32_t>((static_cast<uint64_t>(r) * 0xAAAAAAABull) >> 32) >> 1;
            const uint32_t m = r - ((q << 1) + q); // r % 3
            ev = (m == 0u) ? 53u : 54u;
            if (m == 1u) ev -= 2u;
        } else if (st == 43u) {
            ev = 45u;
        } else if (st == 47u) {
            ev = 58u;
        }
        if (ev == 48u) {
            g.W32(g.U32(R + 540u) + 24u, 4u); // 0x800B2500
        } else if (ev == 49u) {
            g.W32(g.U32(R + 540u) + 24u, 8u); // 0x800B2514
            const uint32_t anim = g.U32(R + 540u);
            const uint32_t cur = g.U32(anim + 12u);
            const uint32_t set = g.U32(anim + 40u);
            const int32_t a3 = g.S16(anim + 16u);
            const uint32_t b = g.U8(g.U32(anim + 4u) + 12u * cur);
            const uint32_t rec = g.U32(g.U32(set + 4u) + 4u * b);
            const uint32_t clip = (g.U32(kRcStanceTable + 8u * 49u) >> 4) & 0xFFFu;
            const int32_t last = static_cast<int16_t>(static_cast<uint16_t>(g.U16(rec + 16u) - 1u));
            const uint32_t owner = g.U32(0x8005B3E4u);
            const uint32_t extra = owner != 0 ? g.U32(owner + 196u) : 0u;
            if (g.Faulted()) return false;
            if (!rc::Call(c, kAirRangedStartFn, {g.U32(R + 540u), clip, 0u, U(a3), U(last), 0u, extra}, F))
                return false;
            if (!rc::Call(c, kAirSetRiderStateFn, {49u, R, 0u}, F)) return false;
        } else {
            if (!rc::Call(c, kAirStanceEventFn, {ev & 0xFFFFu, R, 2u}, F)) return false;
        }
        g.W32(mine != 0 ? mine + 600u : R + 488u, 0x30000u); // 0x800B25D4 / 0x800B25D8
        cu::GScale(g, g.S32(R + 480u), R + 450u, R + 456u);
        const uint32_t f = g.U32(R + 552u);
        g.W8(R + 535u, 0);                // 0x800B25FC
        g.W32(R + 552u, f & 0xBFFFFFFFu); // 0x800B2604
        if (rc::IsPlayerRider(g, g.U16(R + 172u))) {
            const uint32_t view = kRcViewArray + kRcViewStride * g.U16(g.U32(mine + 596u) + 172u);
            g.W32(view + 548u, g.U32(view + 548u) & 0xFFFFFF7Fu); // 0x800B2674
        }
    }
    // 0x800B2698: the pad rumble (a player's rider, or +0x23C & 0x60 == 0x20), and the landing count.
    if (mine != 0 && (rc::IsPlayerRider(g, g.U16(mine + 172u)) || (g.U32(mine + 572u) & 0x60u) == 32u) &&
        g.U32(0x8005B220u) == 0) {
        if (!rc::Call(c, kAirPadRumbleFn, {g.U32(mine + 596u), mine, g.U32(mine + 480u), 0x165A1Cu, 2u}, F))
            return false;
    }
    g.W8(R + 535u, static_cast<uint8_t>(g.U8(R + 535u) + 1u)); // 0x800B2718
    return tail();
}

} // namespace rr::sim
