#include "game/sim/partners.h"

#include "game/sim/camera_collide.h" // PropVsShape 0x800B2E64
#include "game/sim/coll_util.h"
#include "game/sim/fight.h"          // FightStat 0x800BFE58
#include "game/sim/contact.h"      // ResponseFar 0x800AA140
#include "game/sim/recover_fall.h" // LaunchLift 0x8007E868
#include "game/sim/resolvers.h"    // FirstPointInsideBox 0x800B7030, FaceNormal 0x800B675C, NoteBigVolume 0x800B2C98

namespace rr::sim {
namespace {

using namespace cu;

constexpr uint32_t kSinCos = 0x8005624C;   // SLUS {s16 sin, s16 cos} x 4096
constexpr uint32_t kPool5Base = 0x800CE5A4; // *(): pool 5's record 0, stride 452 (+0 the model record)
constexpr int32_t kFast = 0x1017E;          // 1.0059: the body-hit sound's speed floor

void Bump(int PartnerTrace::*f, PartnerTrace* tr) {
    if (tr != nullptr) ++(tr->*f);
}

// The box-inside test both rider resolvers share: the first rider corner inside the partner's box
// (corners `box`, rows `rows`), the face normal into fr+40, the rider pushed out by depth + 0.125.
// `corner` 8: nothing inside (the caller returns).
bool PushOut(GuestRam& g, uint32_t e, uint32_t box, uint32_t rows, uint32_t fr, uint32_t& corner, PartnerTrace* tr) {
    g.W32(fr + 16, fr + 48);
    g.W32(fr + 20, fr + 52);
    if (!FirstPointInsideBox(g, e + 196, rows, box, 16, fr + 48, fr + 52, fr, corner)) return false;
    if (corner == 8u) return !g.Faulted();
    g.W32(fr + 16, 0);
    if (!FaceNormal(g, box, rows, g.U32(fr + 48), fr + 40, 0, fr)) return false;
    const int32_t d = Add(g.S32(fr + 52), 8192);
    g.W32(fr + 52, U(d));
    GScale(g, d, fr + 40, fr + 24);
    ApplyImpulse(g, e, fr + 24, 1);
    Bump(&PartnerTrace::pushes, tr);
    return !g.Faulted();
}

} // namespace

PartnerTotals& PartnerCounters() {
    static PartnerTotals t;
    return t;
}

// ============================================================================ RASHCDG 0x800B2844
bool RiderVsTraffic(GuestRam& g, uint32_t e, uint32_t car, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                    PartnerTrace* tr) {
    const uint32_t fr = sp - 88;
    if (InCameraBox(g, e, 0x140000, 0x1C0000) != 0) {
        uint32_t corner = 0;
        if (!PushOut(g, e, car + 196, car + 432, fr, corner, tr)) return false;
        if (corner == 8u) return !g.Faulted();
        g.W32(fr + 56, corner | 0x100u);
        g.W32(fr + 60, g.U32(fr + 48) | 0x200u);
    } else {
        g.W32(fr + 16, fr + 24);
        uint32_t who = 0;
        if (!ResponseFar(g, e, car, fr + 56, fr + 60, fr, t, who)) return false;
        if (who != 0) {
            ApplyImpulse(g, who, fr + 24, 1);
            Bump(&PartnerTrace::pushes, tr);
        }
        for (uint32_t k = 0; k < 3; ++k) g.W16(fr + 40 + 2 * k, static_cast<uint16_t>(0u - g.U16(car + 438 + 2 * k)));
    }
    if (g.U32(fr + 56) != 0) {                                                 // 0x800B297C
        const int32_t dot = Add(FixMul(g.S32(e + 296), g.S32(car + 296)), FixMul(g.S32(e + 300), g.S32(car + 300)));
        g.W32(fr + 52, U(dot));
        const int32_t v = FixMul(dot, g.S32(car + 480));
        const int32_t fx = MulLo(g.S16(e + 450), Half(Sub(g.S32(car + 184), g.S32(e + 184))));
        const int32_t fz = MulLo(g.S16(e + 454), Half(Sub(g.S32(car + 192), g.S32(e + 192))));
        const int32_t closing = Sub(g.S32(e + 480), v);
        if (Add(fx, fz) > 0) {
            if (!(closing > 0)) g.W32(fr + 60, 0);
        } else if (!(closing < 0)) {
            g.W32(fr + 60, 0);
        }
        if (g.U32(fr + 56) != 0) {
            uint32_t r = 0;
            Bump(&PartnerTrace::pedAsks, tr);
            if (!Call(c, coll::kPedHit, {e, fr + 40, g.U32(car + 480), car + 450, car + 172, 0}, fr, &r)) return false;
            if (r != 0) {
                Bump(&PartnerTrace::pedHits, tr);
                if (kFast < g.S32(e + 480) || kFast < g.S32(car + 480)) {
                    if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), 19, 0)) return false;
                    Bump(&PartnerTrace::sounds, tr);
                }
            }
        }
    }
    if (g.U8(fr + 60) == 3u) {                                                 // 0x800B2AA0
        const uint8_t f = g.U8(car + 509);
        if (!(f & 0x10u)) g.W8(car + 509, static_cast<uint8_t>(f | 0x80u));
        g.W8(car + 509, static_cast<uint8_t>(g.U8(car + 509) | 0x10u));
        Bump(&PartnerTrace::carMarks, tr);
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B2B00
bool RiderVsShape(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                  PartnerTrace* tr) {
    (void)t;
    const uint32_t fr = sp - 80;
    if ((g.U16(shape) >> 5) == 6u && 0x20000 < g.S32(shape + 140) && g.U32(shape + 8) != 1u)
        if (!NoteBigVolume(g, e, shape, fr)) return false;
    uint32_t corner = 0;
    if (!PushOut(g, e, shape + 24, shape + 260, fr, corner, tr)) return false;
    if (corner == 8u) return !g.Faulted();
    // the speed class clamp(|speed| >> 17, 0, 4), the `sra/addu/xor` absolute value, INT32_MIN rounded
    int32_t a = Iabs(g.S32(e + 480));
    if (a < 0) a = Add(a, 0x1FFFF);
    const int32_t q = a >> 17;
    const int32_t lo = S(~U(a >> 31) & U(q));
    const int32_t d = Sub(4, q);
    const int32_t cls = Add(lo, S(U(d >> 31) & U(d)));
    uint32_t hit = 1;
    if ((g.U16(e + 172) >> 5) == 1u || cls > 0) {
        Bump(&PartnerTrace::pedAsks, tr);
        if (!Call(c, coll::kPedHit, {e, fr + 40, 0, fr + 40, shape, 0}, fr, &hit)) return false;
        if (hit != 0) Bump(&PartnerTrace::pedHits, tr);
    }
    if (hit != 0) {
        if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), 19, 0)) return false;
        Bump(&PartnerTrace::sounds, tr);
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B2D44
bool TrafficVsTraffic(GuestRam& g, uint32_t car, uint32_t other, uint32_t sp, const BikeTables& t, PartnerTrace* tr) {
    const uint32_t fr = sp - 56;
    g.W32(fr + 16, fr + 24);
    uint32_t who = 0;
    if (!ResponseFar(g, car, other, fr + 40, fr + 44, fr, t, who)) return false;
    if (who != 0) {
        ApplyImpulse(g, who, fr + 24, 1);
        Bump(&PartnerTrace::pushes, tr);
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80084BE8
bool Launch(GuestRam& g, uint32_t e, int32_t k, uint32_t sp, const BikeTables& t) {
    const uint32_t fr = sp - 40;
    GMulAdd(g, e + 504, e + 522, g.S32(e + 768), e + 184);                     // SLUS 0x8002EAD8
    const uint32_t h = g.U16(g.U32(e + 832));
    int32_t ang = 56, lift = 0x6A51E;
    if ((h >> 5) == 3u) {
        ang = 170;
        lift = 0xB1333;
    } else if ((h >> 5) == 5u) {
        const uint32_t rec = g.U32(kPool5Base) + 452u * (h & 31u);
        const uint32_t kind = (g.U16(g.U32(rec) + 14) & 0xF80u) >> 7;
        ang = kind == 8u ? 568 : 455;
        if (kind == 6u) ang -= 114;
        lift = kind == 8u ? 0xB1333 : 0x8DC28;
        if (kind == 6u) lift = Add(lift, S(0xFFFDC8F6u));
    }
    uint32_t dir = e + 450;
    if (k == 0) {
        dir = e + 864;
        if (!(g.U32(e + 568) & 0x2000000u)) {
            g.W16(e + 864, g.U16(e + 450));
            g.W16(e + 866, g.U16(e + 452));
            g.W16(e + 868, g.U16(e + 454));
            g.W32(e + 860, g.U32(e + 480));
            g.W32(e + 568, g.U32(e + 568) | 0x2000000u);
        }
    }
    int32_t as = 0;
    if (!Asin(Shl(g.S16(dir + 2), 4), t.asin, as)) return false;              // SLUS 0x8001FF3C
    const uint32_t idx = U(Sub(Add(ang, 1024), as)) & 0xFFFu;
    const int32_t cs = g.S16(kSinCos + ((idx << 2) | 2u));
    if (!LaunchLift(g, dir, g.S32(e + 576), cs, lift, fr, t)) return false;   // 0x8007E868
    g.W32(e + 568, g.U32(e + 568) | 0xC00u);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x8007ED64
void TurnFacing(GuestRam& g, uint32_t e, int32_t ang) {
    int32_t len = g.S32(e + 308);
    if (ang > 0) len = Neg(len);
    const int32_t r = Div4(Add(Shl(len, 1), len));
    const uint32_t idx = U(ang) & 0xFFFu;
    const int32_t s = FixMul(r, Shl(g.S16(kSinCos + (idx << 2)), 4));
    for (uint32_t k = 0; k < 3; ++k)
        g.W32(e + 184 + 4 * k, U(Add(FixMul(Shl(g.S16(e + 438 + 2 * k), 4), s), g.S32(e + 504 + 4 * k))));
    const int32_t cv = FixMul(r, Sub(0x10000, Shl(g.S16(kSinCos + (idx << 2) + 2), 4)));
    for (uint32_t k = 0; k < 3; ++k)
        g.W32(e + 184 + 4 * k, U(Add(FixMul(Shl(g.S16(e + 528 + 2 * k), 4), cv), g.S32(e + 184 + 4 * k))));
}

// ============================================================================ RASHCDG 0x800B2E64
bool PropVsShapePass(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, CollisionCallees& c, uint32_t& v0) {
    g.W32(sp - 88 - 32, shape + 260);                                         // 0x800B705C `sw s2,24(sp)`
    return PropVsShape(g, e, shape, sp, c, v0);
}

// ============================================================================ RASHCDG 0x800C29F0
namespace {
// The latched heading +0x360 into the owner's +0x1C8, re-reading the owner pointer as the original does.
void CopyLatched(GuestRam& g, uint32_t bike, uint32_t holder) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(g.U32(holder + 852) + 456 + 2 * k, g.U16(bike + 864 + 2 * k));
}
// The two fight counters and the player's HUD flag for one grabbed entity (0x800C2D60 / 0x800C2DDC).
void GrabStat(GuestRam& g, uint32_t rider, uint32_t who) {
    fight::FightStat(g, g.U32(rider + 596), 1, 0);                                 // 0x800BFE58
    fight::FightStat(g, who, 1, 1);
    const uint32_t h = g.U16(g.U32(rider + 596) + 172);
    if (h < g.U32(g.U32(0x8005B2F8u) + 48)) {
        const int32_t d = Sub(1, S(h));
        const uint32_t idx = U(Add(S(h), S(U(d >> 31) & U(d))));
        g.W32(0x800D6198u + idx * 224u + 140u, 1);
    }
}
} // namespace

void RiderGrab(GuestRam& g, uint32_t rider, uint32_t bike, uint32_t sp) {
    const uint32_t fr = sp - 56;
    const int32_t d = GDot(g, bike + 444, rider + 450);                       // SLUS 0x8002E698
    const bool pass = g.U32(bike + 856) != 0;                                 // s2
    g.W32(fr + 20, 0);
    g.W32(fr + 16, 0);
    if (0xDDB2 < Iabs(d)) {
        const uint32_t grip = d > 0 ? 0x204000u : 0x208000u;
        if (!(d > 0)) g.W32(bike + 568, g.U32(bike + 568) | 0x880u);          // 0x800C2BAC
        if (g.U32(g.U32(bike + 852) + 604) < 2u) {
            CopyLatched(g, bike, bike);
            if (d > 0) g.W32(g.U32(bike + 852) + 480, U(FixMul(0x13333, g.S32(rider + 480))));
            const uint32_t o = g.U32(bike + 852);
            g.W32(o + 552, g.U32(o + 552) | grip);
            g.W32(fr + 16, bike);
        }
        if (pass && g.U32(g.U32(g.U32(bike + 856) + 852) + 604) < 2u) {
            CopyLatched(g, bike, g.U32(bike + 856));
            if (d > 0) g.W32(g.U32(g.U32(bike + 856) + 852) + 480, U(FixMul(0x13333, g.S32(rider + 480))));
            const uint32_t o = g.U32(g.U32(bike + 856) + 852);
            g.W32(o + 552, g.U32(o + 552) | grip);
            g.W32(fr + 20, g.U32(bike + 856));
        }
        if (d > 0 && g.U32(bike + 616) == 0) {                                // 0x800C2B84
            g.W32(bike + 624, 0x2DD62Du);
            g.W32(bike + 632, 0);
            g.W32(bike + 620, 0xFFF80000u);
        }
    } else {                                                                  // 0x800C2CAC
        const uint32_t side = GDot(g, bike + 432, rider + 450) > 0 ? 1u : 0u;
        uint32_t who = bike;
        if (side == 0 && pass) who = g.U32(bike + 856);
        const uint32_t o = g.U32(who + 852);
        if (g.U32(o + 604) < 2u) {
            if (pass) {
                g.W32(o + 552, g.U32(o + 552) & 0xFFF9FFFFu);
                const uint32_t o2 = g.U32(who + 852);
                g.W32(o2 + 552, g.U32(o2 + 552) | 0x8000u | ((side << 17) + 0x20000u));
            } else {
                g.W32(o + 552, g.U32(o + 552) | 0x8000u);
                g.W32(who + 568, g.U32(who + 568) | 0x840u | (((0u - side) & 0xFFFF8000u) + 0x10000u));
            }
            g.W32(fr + 16, who);
        }
    }
    if (g.U32(fr + 16) != 0) GrabStat(g, rider, g.U32(fr + 16));            // 0x800C2D60
    if (g.U32(fr + 20) != 0) GrabStat(g, rider, g.U32(fr + 20));            // 0x800C2DDC
    const uint32_t r = g.U32(bike + 1084);                                    // 0x800C2E58
    int32_t v = S(g.U8(r + 37)) - S(g.U8(r + 36) >> 3);
    if (v < 0) v = 0;
    g.W8(r + 37, static_cast<uint8_t>(v));
}

// ============================================================================ SLUS 0x8001B44C
bool PedVoice(GuestRam& g, int32_t x, int32_t z, uint32_t e, uint32_t flag, CollisionCallees& c) {
    if (flag == 0) return !g.Faulted();
    const int32_t id = g.U32(g.U32(e + 96)) - 400u == 30u ? 104 : 103;
    const int32_t bank = g.S32(0x8005AC8Cu + 1912u);                          // lw a3,1912(gp)
    if (g.Faulted()) return false;
    return c.PlaySound3D(x, z, id, bank) && !g.Faulted();                     // SLUS 0x80017BA0
}

// ============================================================================ the serve
bool ServePartners(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                   bool& ok) {
    const uint32_t* a = call.a;
    const bool two = call.n >= 2;
    PartnerTotals& n = PartnerCounters();
    switch (call.fn) {
    case partners::kRiderVsTraffic:
        v0 = 0;
        ++n.riderTraffic;
        ok = two && RiderVsTraffic(g, a[0], a[1], call.sp, t, c, &n.rt);
        return true;
    case partners::kRiderNone: // `jr ra`: nothing at all (v0 is the caller's own; the pass ignores it)
        v0 = 0;
        ++n.riderNone;
        ok = true;
        return true;
    case partners::kRiderVsShape:
        v0 = 0;
        ++n.riderShape;
        ok = two && RiderVsShape(g, a[0], a[1], call.sp, t, c, &n.rs);
        return true;
    case partners::kTrafficVsTraffic:
        v0 = 0;
        ++n.trafficTraffic;
        ok = two && TrafficVsTraffic(g, a[0], a[1], call.sp, t, &n.tt);
        return true;
    case partners::kLaunch:
        v0 = 0;
        ++n.launches;
        ok = two && Launch(g, a[0], S(a[1]), call.sp, t);
        return true;
    case partners::kPropVsShape: {
        // camera_collide.h's port; PropTopple 0x800B3344 comes back through `c` (served by the solid group).
        ++n.propShape;
        if (!two) {
            ok = false;
            return true;
        }
        const uint16_t h = g.U16(a[0] + 172);
        const bool prop = (h >> 5) != 4u || (h & 31u) < 30u;
        const bool tip = prop && (g.U32(a[0] + 592) & 2u) != 0;
        ok = PropVsShapePass(g, a[0], a[1], call.sp, c, v0);
        if (ok && v0 != 0) {
            ++n.propContacts;
            if (tip) ++n.propTopples;
        }
        return true;
    }
    case partners::kRiderGrab: {
        v0 = 0;
        ++n.grabs;
        if (!two) {
            ok = false;
            return true;
        }
        const uint32_t sp = call.sp;
        RiderGrab(g, a[0], a[1], sp);
        n.grabbed += (g.U32(sp - 56 + 16) != 0) + (g.U32(sp - 56 + 20) != 0);
        ok = !g.Faulted();
        return true;
    }
    case partners::kPedVoice:
        v0 = 0;
        ++n.voices;
        ok = call.n >= 4 && PedVoice(g, S(a[0]), S(a[1]), a[2], a[3], c);
        if (ok && call.n >= 4 && a[3] != 0) ++n.voiced;
        return true;
    case partners::kTurnFacing:
        v0 = 0;
        ++n.turns;
        if (two) TurnFacing(g, a[0], S(a[1]));
        ok = two && !g.Faulted();
        return true;
    default: return false;
    }
}

} // namespace rr::sim
