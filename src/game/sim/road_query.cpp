#include "game/sim/road_query.h"

#include "game/sim/fixed.h"

// Every function below is transcribed from our own disassembly of SLUS_010.53
// (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). Comments give the original's addresses. The
// order of loads is kept wherever a load may fault (a pointer chase), because a load the original
// makes and the port does not - or the other way round - would change which calls the console
// survives. Arithmetic is done in uint32_t wherever the original wraps.

namespace rr::sim {

// ============================================================================ the view

namespace {
// The interpreter's segment masks (src\interp\r3000.cpp): KUSEG and KSEG2 unmasked, KSEG0 and
// KSEG1 reduced to physical.
constexpr uint32_t kSegMask[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                  0x7FFFFFFFu, 0x1FFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
} // namespace

uint8_t* GuestRam::At(uint32_t a, uint32_t n) {
    const uint32_t phys = a & kSegMask[a >> 29];
    if (spad_ != nullptr && (a & (n - 1u)) == 0u && phys >= 0x1F800000u && phys < 0x1F800400u)
        return spad_ + (phys - 0x1F800000u);
    if ((a & (n - 1u)) != 0u || phys >= 0x00800000u) {
        if (!faulted_) {
            faulted_ = true;
            faultAddress_ = a;
        }
        return nullptr;
    }
    return ram_ + (phys & (kRamSize - 1u));
}

uint32_t GuestRam::U32(uint32_t a) {
    const uint8_t* p = At(a, 4);
    if (p == nullptr) return 0;
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t GuestRam::U16(uint32_t a) {
    const uint8_t* p = At(a, 2);
    if (p == nullptr) return 0;
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}
uint8_t GuestRam::U8(uint32_t a) {
    const uint8_t* p = At(a, 1);
    return (p == nullptr) ? 0 : p[0];
}
void GuestRam::W32(uint32_t a, uint32_t v) {
    uint8_t* p = At(a, 4);
    if (p == nullptr) return;
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
void GuestRam::W16(uint32_t a, uint16_t v) {
    uint8_t* p = At(a, 2);
    if (p == nullptr) return;
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void GuestRam::W8(uint32_t a, uint8_t v) {
    uint8_t* p = At(a, 1);
    if (p != nullptr) p[0] = v;
}
void GuestRam::ReadBlock(uint32_t a, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst[i] = U8(a + i);
}
void GuestRam::WriteBlock(uint32_t a, const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) W8(a + i, src[i]);
}

// ============================================================================ helpers

namespace {

constexpr uint32_t kSlice = kRoadSliceBytes;

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Neg(int32_t v) { return S(0u - U(v)); }
inline int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
// `(s32)(s16) << 4`, the promotion of a 4096 = 1.0 direction to 16.16, done on the sign-extended
// halfword with a logical shift (it wraps like `sll`).
inline int32_t Promote(int16_t v) { return S(U(static_cast<int32_t>(v)) << 4); }
// `(s32)(s8) << 13`, a racing-line byte in 1/8 world units to 16.16 (0x800388F4 `lb; sll 0xd`).
inline int32_t Line(int8_t v) { return S(U(static_cast<int32_t>(v)) << 13); }
// `divu` remainder. The R3000 gives the dividend for a zero divisor (hi = n), as src\interp does.
inline uint32_t URem(uint32_t n, uint32_t d) { return (d == 0u) ? n : (n % d); }

// The slice test both walkers inline (0x80036C44, 0x80038770, 0x80038918, ...): a slice strictly
// inside its sub-object, i.e. neither index 0 nor index count - 1. `sub->count` is read only when
// the index is not 0, exactly as the original's `beqz` skips the load.
bool Inside(GuestRam& m, uint32_t cursor, uint32_t slice) {
    const int16_t idx = m.S16(slice + 0);
    if (idx == 0) return false;
    const int32_t last = static_cast<int32_t>(m.S16(m.U32(cursor + 8) + 10)) - 1;
    return idx != last;
}

// The dot product inlined in 0x80036800 / 0x8003697C / 0x80036B14: the
// slice's tangent (row 2, SLCT +0x0E) against P - A, three separately truncated products, a
// 32-bit sum.
int32_t AlongRow2(GuestRam& m, uint32_t slice, const int32_t P[3], const int32_t A[3]) {
    uint32_t sum = 0;
    for (uint32_t k = 0; k < 3; ++k)
        sum += U(FixMul(Promote(m.S16(slice + 14u + 2u * k)), Sub(P[k], A[k])));
    return S(sum);
}

// SLUS 0x8002EAD8 MulAdd over guest addresses: out[k] = mid(dir[k] << 4, t) + base[k], one
// component at a time in the original's load/store order, so an aliased `out` behaves as it does.
void MulAddG(GuestRam& m, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t d = Promote(m.S16(dir + 2u * k));
        const int32_t b = m.S32(base + 4u * k);
        m.W32(out + 4u * k, U(Add(FixMul(d, t), b)));
    }
}

// SLUS 0x8002E6F8 Blend32 over guest addresses: out[k] = mid(wb, b[k]) + mid(wa, a[k]), per
// component - the look-ahead's degenerate arm calls it with `out == b` (0x80038AA8).
void Blend32G(GuestRam& m, uint32_t a, uint32_t b, uint32_t out, int32_t wa, int32_t wb) {
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t av = m.S32(a + 4u * k);
        const int32_t bv = m.S32(b + 4u * k);
        m.W32(out + 4u * k, U(Add(FixMul(wb, bv), FixMul(wa, av))));
    }
}

// The signed divide the look-ahead builds out of the UNSIGNED FixDiv (0x80038CA0 / 0x80038EC0).
int32_t SignedFixDiv(int32_t n, int32_t d) {
    if (n > 0) {
        if (d > 0) return S(FixDiv(U(n), U(d)));
        return Neg(S(FixDiv(U(n), U(Neg(d)))));
    }
    if (d > 0) return Neg(S(FixDiv(U(Neg(n)), U(d))));
    return S(FixDiv(U(Neg(n)), U(Neg(d))));
}

// The walkers' function pointer: forward for a positive walk direction, backward otherwise.
int32_t Neighbours(GuestRam& m, int32_t walkDir, uint32_t p, uint32_t in, uint32_t out,
                   uint32_t dirs, int32_t max, uint32_t sp) {
    if (walkDir > 0) return RoadNeighboursForward(m, p, in, out, dirs, max, sp);
    return RoadNeighboursBackward(m, p, in, out, dirs, max, sp);
}

// PickNeighbour's "forget the junction choice" (0x80039370 and 0x80039458).
void Forget(GuestRam& m, uint32_t e) {
    m.W8(e + 947, 0xFF);
    m.W32(e + 948, 0);
    m.W32(e + 952, 0);
}

} // namespace

// ============================================================================ the leaves

uint32_t GuestCopyWords(GuestRam& m, uint32_t dst, uint32_t src, uint32_t n) {
    // 0x8001E0B4: beqz a2; loop { lw; a1 += 4; a2 -= 4; sw; } while (a2 != 0).
    uint32_t d = dst;
    while (n != 0u) {
        const uint32_t v = m.U32(src);
        src += 4u;
        n -= 4u;
        m.W32(d, v);
        d += 4u;
        if (m.Faulted()) break; // a runaway length: the console would not survive it either
    }
    return dst;
}

uint32_t GuestRand(GuestRam& m) {
    uint32_t seed = m.U32(m.gp() + kRandSeedGp);
    const uint32_t v = Rand(seed);
    m.W32(m.gp() + kRandSeedGp, seed);
    return v;
}

uint32_t RoadBttRecord(GuestRam& m, int32_t id) {
    if (id < 0) return 0;                                          // 0x80039A08
    uint32_t g = m.U32(kRoadGraphPtr);
    if (!(id < m.S16(g + 0x28))) return 0;                         // 0x80039A24
    const int32_t junctions = m.S16(g + 0x2C);
    // Road pieces first, then junctions.
    const int32_t idx = (id < junctions) ? Add(id, m.S16(g + 0x2A)) : Sub(id, junctions);
    g = m.U32(kRoadGraphPtr);                                      // 0x80039A5C, reloaded
    if (!(idx < m.S16(g + 0x28))) return 0;
    const uint32_t rec = m.U32(g + 0x1C) + (U(idx) << 5);
    return (m.U32(rec) == U(id)) ? rec : 0u;                       // 0x80039A8C
}

uint32_t RoadBstRecord(GuestRam& m, int32_t id) {
    const uint32_t g = m.U32(kRoadGraphPtr);                       // 0x80039AA0
    const int32_t junctions = m.S16(g + 0x2C);                     // loaded before the sign test
    if (id < 0) return 0;
    const int32_t idx = Sub(id, junctions);
    if (!(idx < m.S16(g + 0x2A))) return 0;                        // upper bound only
    const uint32_t rec = m.U32(g + 0x20) + (U(idx) << 3);
    return (static_cast<int32_t>(m.S16(rec)) == id) ? rec : 0u;
}

uint32_t RoadNodeRecord(GuestRam& m, int32_t id) {
    if (id < 0) return 0;                                          // 0x80039AFC
    const uint32_t g = m.U32(kRoadGraphPtr);
    if (!(id < m.S16(g + 0x2C))) return 0;
    const uint32_t rec = m.U32(g + 0x24) + U(id) * 104u;           // ((2i+i)*4+i)*8
    return (static_cast<int32_t>(m.S16(rec)) == id) ? rec : 0u;
}

uint32_t RoadJunctionIndex(GuestRam& m, int32_t id) {
    if (id < 0) return 0;                                          // 0x80039B60
    const uint32_t g = m.U32(kRoadGraphPtr);
    const int32_t n = m.S16(g + 0x3C);
    uint32_t p = m.U32(g + 0x30);
    if (n <= 0) return 0;
    int32_t i = 0;
    for (;;) {
        if (static_cast<int32_t>(m.S16(p)) == id) return p;        // 0x80039B8C
        ++i;
        if (!(i < n)) return 0;
        p += 12u;
    }
}

int32_t RoadTurnsFrom(GuestRam& m, uint32_t ipt, int32_t road, uint32_t out, int32_t max) {
    if (ipt == 0) return 0;                                        // 0x80039BB0
    int32_t found = 0;
    if (!(0 < m.S16(ipt + 8))) return 0;
    int32_t k = 0;
    const uint32_t g = m.U32(kRoadGraphPtr);                       // once, 0x80039BD0
    do {
        const int32_t index = Add(m.S16(ipt + 6), k);
        const uint32_t rec = m.U32(g + 0x34) + U(index) * 12u;
        if (static_cast<int32_t>(m.S16(rec + 8)) == road) {        // 0x80039BFC
            const bool over = max < found;                         // `slt v0,a3,t1`, delay slot
            ++found;                                               // delay slot of 0x80039C04
            if (over) return -1;
            m.W32(out, rec);
            out += 4u;
        }
        ++k;
    } while (k < m.S16(ipt + 8));                                  // re-read every pass
    return found;
}

uint32_t RoadTurnTarget(GuestRam& m, uint32_t pdt) {
    if (pdt == 0) return 0;                                        // 0x80039C38
    const int32_t i = m.S16(pdt + 2);
    if (i < 0) return 0;
    const uint32_t g = m.U32(kRoadGraphPtr);
    if (!(i < m.S16(g + 0x40))) return 0;
    return m.U32(g + 0x38) + U(i) * 12u;
}

uint32_t RoadNodeArm(GuestRam& m, uint32_t node, int32_t road) {
    if (node == 0) return 0;                                       // 0x8003A37C
    const int32_t n = m.S16(node + 2);
    if (!(n < 5)) return 0;
    uint32_t arm = node + 8u;
    if (n <= 0) return 0;
    int32_t i = 0;
    for (;;) {
        if (static_cast<int32_t>(m.S16(arm + 4)) == road) return arm;  // 0x8003A3A8
        ++i;
        if (!(i < n)) return 0;
        arm += 24u;
    }
}

uint32_t RoadFindPiece(GuestRam& m, uint32_t obj, int32_t road) {
    if (obj == 0) return 0;                                        // 0x80039C90
    if (m.S16(obj + 16) != 1) return 0;
    const int32_t n = m.S16(obj + 18);
    uint32_t a = m.U32(obj + 44);
    const uint32_t end = a + (U(n) << 5);                          // `sltu`: unsigned addresses
    if (!(a < end)) return 0;
    uint32_t key = a + 12u;
    do {
        const uint32_t v0 = m.U32(key) - U(road);                  // 0x80039CC4
        const uint32_t v1 = U(static_cast<int32_t>(m.S16(key - 10u)));
        if ((v1 | v0) == 0u) return a;
        a += 32u;
        key += 32u;
    } while (a < end);
    return 0;
}

uint32_t RoadNextObjectFwd(GuestRam& m, uint32_t obj, int32_t road) {
    int32_t next = -1;                                             // 0x8003C858
    const uint32_t btt = RoadBttRecord(m, m.S32(obj + 0));
    if (btt == 0) return 0;
    if (m.S16(btt + 4) == 0) {                                     // a road piece: BST_ next
        const uint32_t bst = RoadBstRecord(m, m.S32(obj + 0));
        if (bst != 0) next = m.S16(bst + 2);
    } else {
        const uint32_t node = RoadNodeRecord(m, m.S32(obj + 0));
        if (node != 0) {
            if (m.U32(btt + 28) == 0) {
                next = m.S16(node + 6);                            // the partner, outright
            } else {
                const uint32_t arm = RoadNodeArm(m, node, road);
                if (arm != 0) next = (m.S16(arm + 2) <= 0) ? m.S16(node + 6) : m.S16(arm + 0);
            }
        }
    }
    const uint32_t b = RoadBttRecord(m, next);                     // 0x8003C90C
    return (b != 0) ? m.U32(b + 12) : 0u;                          // BTT_ +0x0C: resident
}

uint32_t RoadNextObjectBwd(GuestRam& m, uint32_t obj, int32_t road) {
    int32_t next = -1;                                             // 0x8003C960
    const uint32_t btt = RoadBttRecord(m, m.S32(obj + 0));
    if (btt == 0) return 0;
    if (m.S16(btt + 4) == 0) {                                     // BST_ prev
        const uint32_t bst = RoadBstRecord(m, m.S32(obj + 0));
        if (bst != 0) next = m.S16(bst + 4);
    } else {
        const uint32_t node = RoadNodeRecord(m, m.S32(obj + 0));
        if (node != 0) {
            if (m.U32(btt + 28) == 0) {
                next = m.S16(node + 6);
            } else {
                const uint32_t arm = RoadNodeArm(m, node, road);
                // 0x8003C9FC: the arm's neighbour when its dir is <= 0, the partner otherwise -
                // the mirror of the forward function's test, NOT the same test.
                if (arm != 0) next = (m.S16(arm + 2) <= 0) ? m.S16(arm + 0) : m.S16(node + 6);
            }
        }
    }
    const uint32_t b = RoadBttRecord(m, next);
    return (b != 0) ? m.U32(b + 12) : 0u;
}

uint32_t RouteLegOfRoad(GuestRam& m, int32_t road) {
    const int32_t n = m.S16(kRouteTable + 18);                     // 0x8003F3C0
    if (n <= 0) return 0;
    uint32_t leg = m.U32(kRouteTable + 36);
    int32_t i = 0;
    for (;;) {
        ++i;                                                       // delay slot of the bne
        if (m.U32(leg) == U(road)) return leg;
        if (!(i < n)) return 0;
        leg += kRouteLegBytes;
    }
}

uint32_t RouteLegFor(GuestRam& m, uint32_t leg, int32_t road) {
    if (m.S16(kRouteTable + 18) == -1) return 0;                   // 0x8003F410: no route
    if (leg == 0) return 0;
    if (road == -1) return 0;
    if (m.U32(leg) == U(road)) return leg;                         // 0x8003F440
    uint32_t found = 0;
    const int32_t count = m.S32(leg + 16);                         // read once
    if (!(0 < count)) return 0;
    uint32_t a = leg;
    for (int32_t k = 0; k < count; ++k, a += 4u) {
        if (m.U32(a + 100) != U(road)) continue;                   // the list at +0x64
        // 0x8003F47C: the leg whose +0 is `road`; the outer loop is NOT left when it is found.
        const int32_t n = m.S16(kRouteTable + 18);
        if (n <= 0) continue;
        uint32_t l = m.U32(kRouteTable + 36);
        for (int32_t i = 0; i < n; ++i, l += kRouteLegBytes) {
            if (m.U32(l) == U(road)) {
                found = l;
                break;
            }
        }
    }
    return found;
}

int32_t RouteLegHasRoad(GuestRam& m, uint32_t leg, int32_t road) {
    if (leg == 0) return 0;                                        // 0x8003F580
    if (road < 0) return 0;
    const int32_t n = m.S32(leg + 16);
    if (!(0 < n)) return 0;
    uint32_t a = leg;
    for (int32_t i = 0; i < n; ++i, a += 4u)
        if (m.U32(a + 84) == U(road)) return 1;                    // the list at +0x54
    return 0;
}

uint32_t RouteLegContaining(GuestRam& m, int32_t road) {
    // 0x8003F5D0: `count + 1` legs, re-read every pass.
    if (m.S16(kRouteTable + 18) + 1 <= 0) return 0;
    for (int32_t i = 0;;) {
        const uint32_t leg = m.U32(kRouteTable + 36) + U(i) * kRouteLegBytes;
        ++i;
        if (RouteLegHasRoad(m, leg, road) != 0) return leg;
        if (!(i < m.S16(kRouteTable + 18) + 1)) return 0;
    }
}

int32_t RoadNextObjectMissing(GuestRam& m, uint32_t c, int32_t dir) {
    if (m.U32(c + 0) == 0) return 1;                               // 0x80039514
    if (m.S16(m.U32(c + 4) + 2) == 1) return 0;                    // inside a junction core
    const int16_t idx = m.S16(m.U32(c + 12) + 0);
    if (!(idx == 0 && dir < 0)) {                                  // 0x80039544 / 0x8003954C
        const int32_t last = static_cast<int32_t>(m.S16(m.U32(c + 8) + 10)) - 1;
        // `slt`: an index BELOW the last one continues the walk; an index at or past it with a
        // forward walk asks for the next object (not only an index "== count - 1").
        if (idx < last) return 0;
        if (dir <= 0) return 0;
    }
    const uint32_t obj = m.U32(c + 0);                             // 0x8003957C
    int32_t next;
    if (m.S16(obj + 16) == 0) {
        const uint32_t bst = RoadBstRecord(m, m.S32(obj + 0));
        if (bst == 0) return 1;
        next = (dir <= 0) ? m.S16(bst + 4) : m.S16(bst + 2);
    } else {
        const uint32_t node = RoadNodeRecord(m, m.S32(obj + 0));
        if (node == 0) return 1;
        const uint32_t arm = RoadNodeArm(m, node, m.S32(m.U32(c + 4) + 12));
        if (arm == 0) return 1;
        if (m.S32(obj + 12) == 1) {                                // the approach half
            if (dir > 0)
                next = (m.S16(arm + 2) <= 0) ? m.S16(node + 6) : m.S16(arm + 0);
            else
                next = (m.S16(arm + 2) <= 0) ? m.S16(arm + 0) : m.S16(node + 6);
        } else {
            if ((dir ^ static_cast<int32_t>(m.S16(arm + 2))) < 0) return 0;  // 0x8003964C
            next = m.S16(node + 6);
        }
    }
    if (next == -1) return 1;                                      // 0x80039660
    const uint32_t b = RoadBttRecord(m, next);
    if (b == 0) return 1;
    return (m.U32(b + 12) != 0) ? 0 : 1;
}

// ============================================================================ 0x8003697C

int32_t RoadAlongFromAnchor(GuestRam& m, int32_t oldDir, int32_t newDir, uint32_t s,
                            uint32_t point) {
    int32_t A[3];
    if (newDir > 0 || oldDir == newDir) {                          // 0x80036984 / 0x8003698C
        for (uint32_t k = 0; k < 3; ++k) A[k] = m.S32(s + 20u + 4u * k);   // the slice START
    } else {
        // The END: origin + tangent * chord (0x800369B8), the three components in the original's
        // order (y, x, z) - only the order of the loads differs, the values are per component.
        const int32_t chord = m.S32(s + 32);
        A[1] = Add(FixMul(Promote(m.S16(s + 16)), chord), m.S32(s + 24));
        A[0] = Add(FixMul(Promote(m.S16(s + 14)), chord), m.S32(s + 20));
        A[2] = Add(FixMul(Promote(m.S16(s + 18)), chord), m.S32(s + 28));
    }
    int32_t P[3];
    for (uint32_t k = 0; k < 3; ++k) P[k] = m.S32(point + 4u * k);
    return AlongRow2(m, s, P, A);                                  // 0x80036A68
}

// ============================================================================ 0x80037A30

int32_t RoadNeighboursForward(GuestRam& m, uint32_t p, uint32_t in, uint32_t out, uint32_t dirs,
                              int32_t max, uint32_t sp) {
    (void)p; // passed through to 0x8003C840, which never reads it
    const uint32_t F = sp - 88u;                                   // `addiu sp,sp,-88`
    const uint32_t list = F + 16u;                                 // TurnsFrom's output
    uint32_t slice = 0;                                            // s3
    int32_t k = 0;                                                 // s8, candidates written
    int32_t ok = 0;                                                // s7
    uint32_t obj = m.U32(in + 0);                                  // s0
    uint32_t piece = m.U32(in + 4);                                // s2
    uint32_t sub = m.U32(in + 8);                                  // s4
    uint32_t turn = m.U32(in + 28);                                // s1
    const uint32_t inSlice = m.U32(in + 12);                       // sp+32
    const int32_t last = static_cast<int32_t>(m.S16(sub + 10)) - 1;
    if (m.S16(inSlice + 0) < last) {                               // 0x80037A9C: the step
        piece = 0;
        slice = inSlice + kSlice;
        ok = 1;
    } else {
        const uint32_t oldObj = obj;                               // s5 (delay slot)
        if (m.S16(obj + 16) == 0) {                                // a road piece: next object
            obj = RoadNextObjectFwd(m, obj, -1);
            if (obj != 0) {
                if (m.S16(obj + 16) == 0) piece = m.U32(obj + 44);
                else piece = RoadFindPiece(m, obj, m.S32(oldObj + 8));
                if (piece != 0) ok = 1;
            }
        } else {
            const uint32_t node = RoadNodeRecord(m, m.S32(obj + 0));
            if (node != 0) {
                if (m.S32(obj + 12) == 1) {                        // 0x80037B24: approach half
                    const uint32_t arm = RoadNodeArm(m, node, m.S32(piece + 12));
                    if (arm != 0) {
                        obj = RoadNextObjectFwd(m, obj, m.S32(piece + 12));
                        if (obj != 0) {
                            if (m.S16(obj + 16) == 0) piece = m.U32(obj + 44);
                            else piece = RoadFindPiece(m, obj, m.S16(arm + 4));
                            if (piece != 0) ok = 1;
                        }
                    }
                } else if (m.S16(piece + 2) == 0) {                // a stub of the core
                    const uint32_t arm = RoadNodeArm(m, node, m.S32(piece + 12));
                    if (arm != 0) {
                        if (m.S16(arm + 2) > 0) {                  // it leads out
                            const int32_t road = m.S32(piece + 12);
                            piece = 0;                             // delay slot of the jal
                            obj = RoadNextObjectFwd(m, obj, road);
                            if (obj != 0) piece = RoadFindPiece(m, obj, m.S16(arm + 4));
                            if (piece != 0) ok = 1;
                        } else {                                   // 0x80037BF0: the turn table
                            const uint32_t jn = RoadJunctionIndex(m, m.S32(obj + 0));
                            if (jn != 0) {
                                const int32_t n = RoadTurnsFrom(m, jn, m.S16(arm + 4), list, max);
                                if (n > 0 && n < max && k < n) {
                                    uint32_t dp = dirs;            // s5
                                    for (int32_t i = 0; i < n; ++i) {
                                        const uint32_t link = m.U32(list + 4u * U(i));
                                        const uint32_t t = RoadTurnTarget(m, link);
                                        if (t == 0) continue;      // 0x80037C64
                                        const uint32_t pc =
                                            m.U32(obj + 44) + (U(static_cast<int32_t>(m.S16(t + 2))) << 5);
                                        const int32_t si = Add(m.S16(pc + 20), m.S16(t + 4));
                                        const int16_t ldir = m.S16(link + 6);
                                        const uint32_t sb = m.U32(obj + 48) + U(si) * 28u;
                                        int32_t idx = m.S16(sb + 8);
                                        const int16_t tdir = m.S16(t + 8);
                                        const uint32_t sl = m.U32(obj + 52);
                                        const bool first = (ldir > 0) ? (tdir > 0) : (tdir <= 0);
                                        if (!first) idx = Add(Sub(idx, 1), m.S16(sb + 10));
                                        const uint32_t cs = sl + U(idx) * kSlice;
                                        const int32_t td = m.S16(t + 8);
                                        m.W32(dp, U((ldir > 0) ? td : Neg(td)));
                                        const uint32_t o = out + (U(k) << 5);
                                        m.W32(o + 20, m.U32(in + 20));
                                        dp += 4u;
                                        const uint32_t lat = m.U32(in + 16);
                                        ++k;
                                        m.W32(o + 0, obj);
                                        m.W32(o + 4, pc);
                                        m.W32(o + 8, sb);
                                        m.W32(o + 12, cs);
                                        ok = 1;
                                        m.W32(o + 28, link);
                                        m.W32(o + 24, jn);
                                        m.W32(o + 16, lat);
                                        piece = pc;
                                        sub = sb;
                                        slice = cs;
                                    }
                                }
                            }
                        }
                    }
                } else {                                           // 0x80037D94: the core
                    if (turn != 0) {
                        const uint32_t t = RoadTurnTarget(m, turn);
                        if (t != 0) {
                            int32_t road;
                            if (m.S16(turn + 6) > 0)
                                road = (m.S16(t + 8) <= 0) ? m.S16(turn + 8) : m.S16(turn + 10);
                            else
                                road = (m.S16(t + 8) <= 0) ? m.S16(turn + 10) : m.S16(turn + 8);
                            piece = 0;
                            const uint32_t arm = RoadNodeArm(m, node, road);
                            if (arm != 0) piece = RoadFindPiece(m, obj, m.S16(arm + 4));
                            if (piece != 0) {
                                sub = m.U32(obj + 48) + U(static_cast<int32_t>(m.S16(piece + 20))) * 28u;
                                const uint32_t sl = m.U32(obj + 52);
                                int32_t idx = m.S16(sub + 8);
                                if (!(m.S16(arm + 2) >= 0)) idx = Add(Sub(idx, 1), m.S16(sub + 10));
                                slice = sl + U(idx) * kSlice;
                                m.W32(dirs, U(static_cast<int32_t>(m.S16(arm + 2))));
                                ok = 2;
                            }
                        }
                    }
                }
            }
        }
    }
    // ---- the tail, 0x80037E80
    if (ok == 0) {                                                 // no candidate: the input slice
        const uint32_t o = out + (U(k) << 5);
        m.W32(o + 8, m.U32(in + 8));
        m.W32(o + 12, m.U32(in + 12));
        return k + 1;
    }
    if (k != 0) return k;
    if (piece == 0) {
        const uint32_t inSub = m.U32(in + 8);
        if (sub == inSub || slice == inSlice) {                    // the PARTIAL candidate
            m.W32(out + 8, inSub);
            m.W32(out + 12, slice);
            return 1;
        }
    }
    if (ok != 2) {                                                 // the next piece, entered first
        sub = m.U32(obj + 48) + U(static_cast<int32_t>(m.S16(piece + 20))) * 28u;
        const uint32_t sl = m.U32(obj + 52);
        m.W32(dirs, 1);
        slice = sl + U(static_cast<int32_t>(m.S16(sub + 8))) * kSlice;
    }
    m.W32(out + 20, m.U32(in + 20));
    const uint32_t lat = m.U32(in + 16);
    m.W32(out + 0, obj);
    m.W32(out + 4, piece);
    m.W32(out + 8, sub);
    m.W32(out + 12, slice);
    m.W32(out + 24, 0);
    m.W32(out + 28, 0);
    m.W32(out + 16, lat);
    return 1;
}

// ============================================================================ 0x80037FBC

int32_t RoadNeighboursBackward(GuestRam& m, uint32_t p, uint32_t in, uint32_t out, uint32_t dirs,
                               int32_t max, uint32_t sp) {
    (void)p; // passed through (from its home slot, sp+88) to 0x8003C948, which never reads it
    const uint32_t F = sp - 88u;
    const uint32_t list = F + 16u;
    uint32_t slice = 0;                                            // s4
    int32_t k = 0;                                                 // s8
    int32_t armRoad = -1;                                          // s0, `li s0,-1`
    int32_t ok = 0;                                                // s7
    uint32_t obj = m.U32(in + 0);                                  // s1
    uint32_t piece = m.U32(in + 4);                                // s3
    uint32_t sub = m.U32(in + 8);                                  // s6
    const uint32_t inSlice = m.U32(in + 12);                       // sp+32
    const int16_t idx0 = m.S16(inSlice + 0);
    const uint32_t turn = m.U32(in + 28);                          // s2
    if (idx0 != 0) {                                               // 0x80038024: the step
        slice = inSlice - kSlice;
        piece = 0;
        ok = 1;
    } else {
        const uint32_t oldObj = obj;                               // s5
        if (m.S16(obj + 16) == 0) {                                // road piece: previous object
            obj = RoadNextObjectBwd(m, obj, -1);
            if (obj != 0) {
                if (m.S16(obj + 16) == 0) piece = m.U32(obj + 44);
                else piece = RoadFindPiece(m, obj, m.S32(oldObj + 8));
                if (piece != 0) ok = 1;
            }
        } else {
            const uint32_t node = RoadNodeRecord(m, m.S32(obj + 0));
            if (node != 0) {
                if (m.S32(obj + 12) == 1) {                        // 0x800380A8: approach half
                    const uint32_t arm = RoadNodeArm(m, node, m.S32(piece + 12));
                    if (arm != 0) obj = RoadNextObjectBwd(m, obj, m.S32(piece + 12));
                    // 0x800380E4: the object is tested BEFORE the arm here
                    if (obj != 0 && arm != 0) {
                        if (m.S16(obj + 16) == 0) piece = m.U32(obj + 44);
                        else piece = RoadFindPiece(m, obj, m.S16(arm + 4));
                        if (piece != 0) ok = 1;
                    }
                } else if (m.S16(piece + 2) == 0) {                // a stub of the core
                    const uint32_t arm = RoadNodeArm(m, node, m.S32(piece + 12));
                    if (arm != 0) {
                        if (m.S16(arm + 2) <= 0) {                 // 0x800382E8: it leads out
                            const int32_t road = m.S32(piece + 12);
                            piece = 0;
                            obj = RoadNextObjectBwd(m, obj, road);
                            if (obj != 0) piece = RoadFindPiece(m, obj, m.S16(arm + 4));
                            if (piece != 0) ok = 1;
                        } else {                                   // 0x80038144: the turn table
                            const uint32_t jn = RoadJunctionIndex(m, m.S32(obj + 0));
                            if (jn != 0) {
                                const int32_t n = RoadTurnsFrom(m, jn, m.S16(arm + 4), list, max);
                                if (n > 0 && n < max && k < n) {
                                    uint32_t dp = dirs;            // s0
                                    for (int32_t i = 0; i < n; ++i) {
                                        const uint32_t link = m.U32(list + 4u * U(i));
                                        const uint32_t t = RoadTurnTarget(m, link);
                                        if (t == 0) continue;
                                        const uint32_t pc =
                                            m.U32(obj + 44) + (U(static_cast<int32_t>(m.S16(t + 2))) << 5);
                                        const int32_t si = Add(m.S16(pc + 20), m.S16(t + 4));
                                        const int16_t ldir = m.S16(link + 6);
                                        const uint32_t sb = m.U32(obj + 48) + U(si) * 28u;
                                        int32_t idx = m.S16(sb + 8);
                                        const int16_t tdir = m.S16(t + 8);
                                        const uint32_t sl = m.U32(obj + 52);
                                        const bool first = (ldir > 0) ? (tdir > 0) : (tdir <= 0);
                                        if (!first) idx = Add(Sub(idx, 1), m.S16(sb + 10));
                                        const uint32_t cs = sl + U(idx) * kSlice;
                                        const int32_t td = m.S16(t + 8);
                                        m.W32(dp, U((ldir > 0) ? td : Neg(td)));
                                        const uint32_t o = out + (U(k) << 5);
                                        m.W32(o + 20, m.U32(in + 20));
                                        dp += 4u;
                                        const uint32_t lat = m.U32(in + 16);
                                        ++k;
                                        m.W32(o + 0, obj);
                                        m.W32(o + 4, pc);
                                        m.W32(o + 8, sb);
                                        m.W32(o + 12, cs);
                                        ok = 1;
                                        m.W32(o + 28, link);
                                        m.W32(o + 24, jn);
                                        m.W32(o + 16, lat);
                                        piece = pc;
                                        sub = sb;
                                        slice = cs;
                                    }
                                }
                            }
                        }
                    }
                } else {                                           // 0x80038324: the core
                    piece = 0;                                     // delay slot, always
                    if (turn != 0) {
                        // NO null check on the target: a zero target reads
                        // guest 0x00000008, which the RAM mirror answers.
                        const uint32_t t = RoadTurnTarget(m, turn);
                        if (m.S16(turn + 6) > 0)
                            armRoad = (m.S16(t + 8) <= 0) ? m.S16(turn + 10) : m.S16(turn + 8);
                        else
                            armRoad = (m.S16(t + 8) <= 0) ? m.S16(turn + 8) : m.S16(turn + 10);
                    }
                    // With no turn the initial -1 is looked up (0x80038378).
                    const uint32_t arm = RoadNodeArm(m, node, armRoad);
                    if (arm != 0) piece = RoadFindPiece(m, obj, m.S16(arm + 4));
                    if (piece != 0) {
                        sub = m.U32(obj + 48) + U(static_cast<int32_t>(m.S16(piece + 20))) * 28u;
                        const uint32_t sl = m.U32(obj + 52);
                        int32_t idx = m.S16(sub + 8);
                        if (!(m.S16(arm + 2) > 0)) idx = Add(Sub(idx, 1), m.S16(sub + 10));
                        slice = sl + U(idx) * kSlice;
                        m.W32(dirs, U(static_cast<int32_t>(m.S16(arm + 2))));
                        ok = 2;
                    }
                }
            }
        }
    }
    // ---- the tail, 0x80038408
    if (ok == 0) {
        const uint32_t o = out + (U(k) << 5);
        m.W32(o + 8, m.U32(in + 8));
        m.W32(o + 12, m.U32(in + 12));
        return k + 1;
    }
    if (k != 0) return k;
    if (piece == 0) {
        const uint32_t inSub = m.U32(in + 8);
        if (sub == inSub || slice == inSlice) {                    // the PARTIAL candidate
            m.W32(out + 8, inSub);
            m.W32(out + 12, slice);
            return 1;
        }
    }
    if (ok != 2) {                                                 // the previous piece, entered LAST
        sub = m.U32(obj + 48) + U(static_cast<int32_t>(m.S16(piece + 20))) * 28u;
        const uint32_t sl = m.U32(obj + 52);
        const int32_t first = m.S16(sub + 8);
        const int32_t count = m.S16(sub + 10);
        m.W32(dirs, 0xFFFFFFFFu);
        slice = sl + U(Add(first, count)) * kSlice - kSlice;
    }
    m.W32(out + 20, m.U32(in + 20));
    const uint32_t lat = m.U32(in + 16);
    m.W32(out + 0, obj);
    m.W32(out + 4, piece);
    m.W32(out + 8, sub);
    m.W32(out + 12, slice);
    m.W32(out + 24, 0);
    m.W32(out + 28, 0);
    m.W32(out + 16, lat);
    return 1;
}

// ============================================================================ 0x8003CCA0

int32_t AiJunctionChoice(GuestRam& m, uint32_t e, uint32_t cand, int32_t n) {
    const uint32_t gs = m.U32(kRoadGameState);                     // 0x8003CCA0
    const int32_t fallback = Sub(n, 2);                            // s4
    int32_t follow = -1;                                           // s3
    if (!(m.U16(e + 172) < m.U32(gs + 48))) {                      // an AI rider
        const uint32_t rider = m.U32(e + 1084);
        if ((m.U8(rider + 1) & 0xFu) == 2u && m.S32(e + 480) < 131) {
            // The top AI command's target (0x8003CD20): e + 8*((s8)e[+0x3B2] - 1) + 0x3BE.
            const int32_t depth = Add(m.S8(e + 946), -1);
            const uint16_t h = m.U16(e + (U(depth) << 3) + 958u);
            if (h != 224 && (h >> 5) == 0) {
                const uint32_t b = m.U32(kRoadPool0Ptr) + 1096u * h;
                if (b != 0) follow = m.U16(b + 360);
            }
        }
    }
    if (m.S16(kRouteTable + 18) == -1) {                           // 0x8003CD84: no route
        if (n != 2) return fallback;
        return S(URem(GuestRand(m), U(n)));
    }
    const uint32_t leg = RouteLegFor(m, m.U32(e + 428), m.S32(m.U32(cand + 0) + 8));
    if (leg == 0) return fallback;
    uint32_t r = 0;
    if (!(m.S32(leg + 16) < 2)) r = URem(GuestRand(m), U(n));     // % n, not % the list's count
    if (n <= 0) return fallback;
    const uint32_t gs2 = m.U32(kRoadGameState);
    const bool human = m.U16(e + 172) < m.U32(gs2 + 48);           // once, before the loop
    const uint32_t target = leg + (r << 2);                        // `sllv v0,v1,t1`, t1 = 2
    uint32_t c = cand;
    for (int32_t i = 0; i < n; ++i, c += 32u) {
        if (!human) {
            const uint32_t rider = m.U32(e + 1084);                // re-read every pass
            if ((m.U8(rider + 1) & 0xFu) == 2u && follow != -1) {
                if (static_cast<int32_t>(m.S16(m.U32(c + 28) + 10)) == follow) return i;
                continue;                                          // 0x8003CE3C
            }
        }
        const int32_t to = m.S16(m.U32(c + 28) + 10);              // 0x8003CE4C
        if (to == m.S32(target + 84)) return i;
    }
    return fallback;
}

// ============================================================================ 0x80039048

uint32_t RoadPickNeighbour(GuestRam& m, uint32_t p, uint32_t cand, uint32_t dirs, int32_t n,
                           uint32_t out, uint32_t dirOut) {
    int32_t choice = -1;                                           // s0
    uint32_t pool = 6;                                             // a1, "no entity"
    uint32_t e = 0;                                                // s3
    if (p != 0) {
        const uint16_t h = m.U16(p + 0);
        pool = h >> 5;
        if (pool == 0) e = m.U32(kRoadPool0Ptr) + 1096u * h;
    }
    if (m.U32(cand + 28) == 0) {                                   // 0x800390CC -> 0x80039440
        if (e != 0 && m.U32(e + 356) != 0) Forget(m, e);           // just left a junction
        if (m.U32(out + 8) == m.U32(cand + 8)) m.W32(out + 12, m.U32(cand + 12));
        else GuestCopyWords(m, out, cand, 32);
        const int32_t d = m.S32(dirs);
        if (d == 1 || d == -1) m.W32(dirOut, U(d));
        return m.U32(out + 12);
    }
    if (n < 2) {                                                   // 0x800393FC
        if (m.U32(out + 8) == m.U32(cand + 8)) {
            m.W32(out + 12, m.U32(cand + 12));                     // *dirOut NOT written
        } else {
            GuestCopyWords(m, out, cand, 32);
            m.W32(dirOut, m.U32(dirs));
        }
        return m.U32(out + 12);
    }
    if (pool == 0) {                                               // 0x800392F8
        if (e != 0) {
            const uint32_t gs = m.U32(kRoadGameState);
            const bool human = m.U16(e + 172) < m.U32(gs + 48);
            bool useMemory = true;
            if (!human && (m.U8(m.U32(e + 1084) + 1) & 0xFu) == 2u) useMemory = false;
            if (useMemory) {
                const uint32_t mem = m.U32(e + 948);
                if (mem != 0) {
                    const uint32_t jn = m.U32(cand + 24);
                    if (m.S16(mem + 2) == m.S16(jn + 2)) choice = m.S8(e + 947);  // same junction
                    else Forget(m, e);
                }
            }
            if (choice == -1) choice = AiJunctionChoice(m, e, cand, n);
        }
        // e == 0 cannot be a pool-0 entity; the original then copies with the index still -1.
    } else if (pool == 3) {                                        // 0x800390EC: traffic
        int32_t pick = -1;                                         // s4
        int32_t prefer = -1;                                       // s5
        const uint16_t h = m.U16(p + 0);
        const uint32_t key = m.U32(m.U32(cand + 4) + 12);
        const uint32_t veh = ((static_cast<uint32_t>(h) & 0x1Fu) << 9) + kRoadTraffic;
        if (RouteLegOfRoad(m, S(key)) != 0) {
            const uint32_t gs = m.U32(kRoadGameState);
            if ((m.U8(gs + 4) & 1u) != 0 && m.U32(veh + 180) == 0) {
                const uint32_t bike = m.U8(veh + 511);
                if (S(bike) < m.S32(kRoadBikeCount)) {
                    const uint32_t b = m.U32(kRoadPool0Ptr) + 1096u * bike;
                    if (b != 0) {
                        const uint32_t r = m.U32(b + 360);
                        if ((r >> 16) == 0) prefer = S(r);
                    }
                }
            }
            uint32_t c = cand;
            for (int32_t i = 0; i < n; ++i, c += 32u) {
                const uint32_t g2 = m.U32(kRoadGameState);
                const uint32_t turn = m.U32(c + 28);
                const uint8_t flags = m.U8(g2 + 4);
                const int32_t to = m.S16(turn + 10);
                if ((flags & 0x10u) != 0) {
                    if (RouteLegContaining(m, to) != 0 && !(U(to - 11) < 2u) && to != 20 && to != 26) {
                        pick = i;
                        break;
                    }
                } else {
                    if ((prefer != -1 && to == prefer) || RouteLegContaining(m, to) != 0) {
                        pick = i;
                        break;
                    }
                }
            }
        }
        if (pick == -1) {                                          // 0x80039240
            const uint32_t gs = m.U32(kRoadGameState);
            if ((m.U8(gs + 4) & 0x10u) != 0) {
                uint32_t c = cand;
                for (int32_t i = 0; i < n; ++i, c += 32u) {
                    const uint16_t to = m.U16(m.U32(c + 28) + 10);
                    if (static_cast<uint32_t>(to) - 11u < 2u) continue;
                    const int16_t v = static_cast<int16_t>(to);
                    if (v == 20) continue;
                    if (v != 26) {
                        pick = i;
                        break;
                    }
                }
                // nothing admissible: the index STAYS -1
            } else {
                pick = S(URem(GuestRand(m), U(n)));
            }
        }
        choice = pick;
    } else {                                                       // 0x800392E0: anything else
        choice = S(URem(GuestRand(m), U(n)));
    }
    // 0x800393A4: take candidate `choice` - with -1, the 32 bytes BEFORE the array and dirs[-1].
    GuestCopyWords(m, out, cand + (U(choice) << 5), 32);
    m.W32(dirOut, m.U32(dirs + (U(choice) << 2)));
    if (e != 0 && m.U32(e + 948) == 0) {                           // remember this junction
        m.W8(e + 947, static_cast<uint8_t>(choice));
        m.W32(e + 948, m.U32(out + 24));
        m.W32(e + 952, m.U32(out + 28));
    }
    return m.U32(out + 12);
}

// ============================================================================ 0x80036B14

namespace {

// 3.3: the tag. `e[+0x33C]` is addressed as a POOL-0 slot from the handle, whatever the pool.
void SearchTag(GuestRam& m, uint32_t p, uint32_t slice) {
    if ((m.U32(p + 216) & 0x80u) == 0) return;
    if (m.S16(slice + 50) == 0) return;
    const uint32_t h = m.U16(p + 0);
    m.W32(m.U32(kRoadPool0Ptr) + 1096u * h + 828u, slice);
}

// Commit (0x80036D60 / 0x80036F60): the slice alone while the sub-object is the same, the whole
// cursor otherwise.
void SearchCommit(GuestRam& m, uint32_t cur, uint32_t L, uint32_t lSlice) {
    if (m.U32(cur + 8) == m.U32(L + 8)) m.W32(cur + 12, lSlice);
    else GuestCopyWords(m, cur, L, 32);
}

} // namespace

uint32_t RoadSliceSearch(GuestRam& m, uint32_t p, uint32_t cur, uint32_t point, uint32_t sp) {
    const uint32_t F = sp - 224u;                                  // `addiu sp,sp,-224`
    const uint32_t L = F + 24u;                                    // the private cursor
    const uint32_t cands = F + 56u;                                // 3 x 32
    const uint32_t dirs = F + 152u;                                // 4 words
    const uint32_t dirW = F + 168u;                                // the walk direction
    m.W32(F + 84u, 0);          // cand[0].turn: NOT a dead store - the partial candidate reads it
    m.W32(L + 0, 0);            // "not copied yet"
    m.W32(L + 8, m.U32(cur + 8));
    m.W32(L + 12, m.U32(cur + 12));
    uint32_t s3 = m.U32(cur + 12);
    m.W32(dirW, 0);
    int32_t P[3], A[3];
    for (uint32_t k = 0; k < 3; ++k) {
        P[k] = m.S32(point + 4u * k);
        A[k] = m.S32(s3 + 20u + 4u * k);
    }
    int32_t d = AlongRow2(m, s3, P, A);                            // 0x80036B74
    int32_t prev = 0;                                              // s8: NOT the first distance
    if (d != 0) {                                                  // 0x80036C1C
        m.W32(dirW, (d > 0) ? 1u : 0xFFFFFFFFu);
        int32_t dir = m.S32(dirW);                                 // s2, the direction of a step
        uint32_t cand = s3;                                        // s1
        m.W32(dirs, U(dir));
        // ---- the first step: no streaming check
        if (Inside(m, L, m.U32(L + 12))) {
            cand = s3 + U(dir) * kSlice;
            m.W32(L + 12, cand);
        } else {
            if (m.U32(L + 0) == 0) {
                GuestCopyWords(m, L, cur, 32);
                m.W32(L + 12, cand);
            }
            const int32_t n = Neighbours(m, m.S32(dirW), p, L, cands, dirs, 3, F);
            cand = RoadPickNeighbour(m, p, cands, dirs, n, L, dirW);
        }
        d = RoadAlongFromAnchor(m, dir, m.S32(dirW), cand, point);  // 0x80036D2C
        for (;;) {                                                 // 0x80036D38
            const int32_t w = m.S32(dirW);
            if (!((w > 0 && d >= 0) || (w < 0 && d < 0))) break;   // not passed yet
            SearchCommit(m, cur, L, m.U32(L + 12));
            s3 = m.U32(cur + 12);
            SearchTag(m, p, s3);
            dir = m.S32(dirW);                                     // 0x80036DE8
            if (Inside(m, L, m.U32(L + 12))) {
                cand = cand + U(dir) * kSlice;
                m.W32(L + 12, cand);
            } else {
                if (m.U32(L + 0) == 0) {
                    GuestCopyWords(m, L, cur, 32);
                    m.W32(L + 12, cand);
                }
                if (RoadNextObjectMissing(m, L, m.S32(dirW)) != 0) {    // 0x80036E50
                    const uint32_t gs = m.U32(kRoadGameState);
                    const uint32_t h = m.U16(p + 0);
                    if (h < m.U32(gs + 48)) m.W32(m.gp() + kOffRoadFlagGp, 1);  // a human's search
                    break;
                }
                const int32_t n = Neighbours(m, m.S32(dirW), p, L, cands, dirs, 3, F);
                cand = RoadPickNeighbour(m, p, cands, dirs, n, L, dirW);
            }
            d = RoadAlongFromAnchor(m, dir, m.S32(dirW), cand, point);  // 0x80036EFC
            const int32_t diff = Sub(d, prev);
            const int32_t sg = diff >> 31;
            const int32_t mag = S((U(diff) + U(sg)) ^ U(sg));      // |INT32_MIN| stays negative
            prev = d;
            if (mag < 131) break;                                  // 0x80036F18 `slti`
        }
        // 0x80036F24: a reverse search that ends INSIDE the last candidate commits it.
        if (dir < 0 && m.S32(dirW) < 0 && d > 0) {
            const uint32_t lSlice = m.U32(L + 12);
            if (d < m.S32(lSlice + 32)) {
                SearchCommit(m, cur, L, lSlice);
                s3 = m.U32(cur + 12);
                SearchTag(m, p, s3);
            }
        }
    }
    m.W32(p + 216, m.U32(p + 216) & ~0x80u);                       // every exit
    return s3;
}

// ============================================================================ 0x800386DC

void RoadLookAhead(GuestRam& m, uint32_t e, int32_t ahead, int32_t along, int32_t dir,
                   uint32_t cursor, uint32_t outPoint, uint32_t outCursor, uint32_t sp) {
    const uint32_t F = sp - 312u;                                  // `addiu sp,sp,-312`
    const uint32_t copy72 = F + 72u, copy104 = F + 104u;
    const uint32_t cands = F + 136u, dirs = F + 232u, walk = F + 248u;
    const uint32_t slot24 = F + 24u, slot40 = F + 40u, slot56 = F + 56u;    // the three 3-vectors
    const uint32_t p = e + 172u;                                   // e + 0xAC
    uint32_t c = cursor;                                           // s4
    const uint32_t saved = m.U32(c + 12);                          // sp+256
    int32_t s6;
    if (dir > 0) {
        s6 = Sub(m.S32(saved + 32), along);                        // to the END of the slice
        m.W32(walk, 1);
    } else {
        m.W32(walk, 0xFFFFFFFFu);                                  // dir == 0 walks backward
        s6 = along;
    }
    // ---- phase 1, 0x80038748: walk `ahead` units
    while (s6 < ahead) {
        const uint32_t sl = m.U32(c + 12);
        if (Inside(m, c, sl)) {
            m.W32(c + 12, sl + U(m.S32(walk)) * kSlice);           // IN PLACE, maybe the caller's
        } else {
            m.W32(dirs, m.U32(walk));
            if (c == cursor) {                                     // the first boundary
                c = copy72;
                GuestCopyWords(m, c, cursor, 32);
            }
            if (RoadNextObjectMissing(m, c, m.S32(walk)) != 0) {   // 0x800387F8
                const uint32_t s = m.U32(c + 12);
                const int32_t t = (m.S32(walk) > 0) ? Sub(ahead, s6) : Sub(s6, ahead);
                MulAddG(m, s + 20u, s + 14u, t, outPoint);         // along the TANGENT
                if (outCursor != 0) GuestCopyWords(m, outCursor, c, 32);
                return;                                            // NO restore (0x80038840)
            }
            const int32_t n = Neighbours(m, m.S32(walk), p, c, cands, dirs, 3, F);
            RoadPickNeighbour(m, p, cands, dirs, n, c, walk);
        }
        s6 = Add(s6, m.S32(m.U32(c + 12) + 32));
    }
    // ---- phase 2, 0x800388C4: slice A and the next slice B
    const uint32_t cA = c;                                         // sp+260
    if (outCursor != 0) GuestCopyWords(m, outCursor, c, 32);
    const int32_t dir0 = m.S32(walk);                              // sp+264
    const uint32_t A = m.U32(c + 12);
    int32_t len = m.S32(A + 32);
    int32_t offA;
    if (dir0 > 0) {
        s6 = Sub(len, Sub(s6, ahead));
        offA = Line(m.S8(A + 38));                                 // SLCT +0x26
    } else {
        s6 = Sub(s6, ahead);
        offA = Line(m.S8(A + 39));                                 // SLCT +0x27
    }
    {
        const uint32_t sl = m.U32(c + 12);
        m.W32(walk, 1);
        if (Inside(m, c, sl)) {
            m.W32(c + 12, sl + kSlice);
        } else {
            c = copy104;
            m.W32(dirs, m.U32(walk));
            GuestCopyWords(m, c, cA, 32);
            const int32_t n = Neighbours(m, m.S32(walk), p, c, cands, dirs, 3, F);
            RoadPickNeighbour(m, p, cands, dirs, n, c, walk);
        }
    }
    const uint32_t B = m.U32(c + 12);
    int32_t offB;
    if (m.S32(walk) > 0) {
        offB = Line((dir0 > 0) ? m.S8(B + 38) : m.S8(B + 39));
    } else {                                                       // B entered against the walk
        offB = Line((dir0 > 0) ? m.S8(B + 39) : m.S8(B + 38));
        len = Add(len, m.S32(B + 32));
    }
    // ---- phase 3, 0x80038A40
    bool quad;
    int32_t f;
    uint32_t lerpA = 0, lerpB = 0;
    if (s6 < (len >> 1)) {
        MulAddG(m, B + 20u, B + 2u, offB, slot56);                    // P_B at sp+56
        MulAddG(m, A + 20u, A + 2u, offA, slot40);                    // P_A at sp+40
        if (s6 >= 0) {
            quad = false;
            f = SignedFixDiv(s6, len);
            lerpA = slot40;
            lerpB = slot56;
        } else {                                                   // the target lies before A
            quad = true;
            if (len > 0) {                                         // 0x80038A84
                const int32_t g = Neg(S(FixDiv(0, U(len))));
                Blend32G(m, slot40, slot56, slot56, Sub(0x10000, g), g);    // copies P_A over P_B
                len = 0;
            }
            // FixDiv by (-len), which is 0 whenever the chord was positive: a DIVIDE BY ZERO.
            f = Add(S(FixDiv(U(Neg(s6)), U(Neg(len)))), 0x8000);
            c = cA;
            m.W32(c + 12, A);                                      // 0x80038AF4
            m.W32(walk, 0xFFFFFFFFu);
            if (Inside(m, c, A)) {
                m.W32(c + 12, A - kSlice);
            } else {
                c = copy104;
                m.W32(dirs, m.U32(walk));
                GuestCopyWords(m, c, cA, 32);
                const int32_t n = Neighbours(m, m.S32(walk), p, c, cands, dirs, 3, F);
                RoadPickNeighbour(m, p, cands, dirs, n, c, walk);
            }
            int32_t offC;
            const int32_t w = m.S32(walk);
            if (w < 0) {
                const uint32_t C = m.U32(c + 12);
                offC = Line((dir0 > 0) ? m.S8(C + 38) : m.S8(C + 39));
            } else {                                               // C entered against the walk
                if (!(m.S16(m.U32(c + 8) + 10) < 2)) {
                    m.W32(c + 12, m.U32(c + 12) + kSlice);
                } else {
                    // 0x80038C50: the fifth `jalr` - its answer is IGNORED, only its frame and its
                    // output slots are written.
                    Neighbours(m, w, p, c, cands, dirs, 3, F);
                }
                const int32_t w2 = m.S32(walk);
                const uint32_t C = m.U32(c + 12);
                const uint8_t b38 = m.U8(C + 38);
                if ((w2 ^ dir0) < 0) offC = S(U(b38) << 24) >> 11;
                else offC = Line(m.S8(C + 39));
            }
            const uint32_t C = m.U32(c + 12);
            MulAddG(m, C + 20u, C + 2u, offC, slot24);                // P_C at sp+24
        }
    } else {
        MulAddG(m, A + 20u, A + 2u, offA, slot24);                    // P_A at sp+24
        MulAddG(m, B + 20u, B + 2u, offB, slot40);                    // P_B at sp+40
        s6 = Sub(len, s6);
        if (s6 >= 0) {
            quad = false;
            f = SignedFixDiv(s6, len);
            lerpA = slot40;
            lerpB = slot24;
        } else {                                                   // the target lies beyond B
            quad = true;
            if (len > 0) {                                         // 0x80038D30
                const int32_t g = Neg(S(FixDiv(0, U(len))));
                Blend32G(m, slot40, slot24, slot24, Sub(0x10000, g), g);    // copies P_B over P_A
                len = 0;
            }
            f = Sub(0x8000, S(FixDiv(U(Neg(s6)), U(Neg(len)))));
            const uint32_t sl = m.U32(c + 12);
            const uint32_t cB = c;                                 // sp+260 = s4
            const int32_t wB = m.S32(walk);                        // s3
            if (Inside(m, c, sl)) {
                m.W32(c + 12, sl + U(wB) * kSlice);
            } else {
                c = copy104;
                m.W32(dirs, m.U32(walk));
                GuestCopyWords(m, c, cB, 32);
                const int32_t n = Neighbours(m, m.S32(walk), p, c, cands, dirs, 3, F);
                RoadPickNeighbour(m, p, cands, dirs, n, c, walk);
            }
            uint32_t step = 0;
            if (wB < 0) step = (0 < m.S32(walk)) ? 1u : 0u;        // 0x80038E70
            const uint32_t D = m.U32(c + 12) + step * kSlice;
            MulAddG(m, D + 20u, D + 2u, Line(m.S8(D + 38)), slot56);  // P_D at sp+56, always +0x26
        }
    }
    if (!quad) {
        Blend32G(m, lerpA, lerpB, outPoint, Sub(0x10000, f), f);   // 0x80038F20
    } else {
        // 0x80038F30: the uniform quadratic B-spline basis over sp+24, sp+40, sp+56.
        const int32_t ff = FixMul(f, f);
        const int32_t q2 = ff >> 1;
        const int32_t q0 = Sub(Add(q2, 0x8000), f);
        const int32_t q1 = Sub(f, Sub(ff, 0x8000));
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t a = FixMul(q0, m.S32(slot24 + 4u * k));
            const int32_t b = FixMul(q1, m.S32(slot40 + 4u * k));
            const int32_t cc = FixMul(q2, m.S32(slot56 + 4u * k));
            m.W32(outPoint + 4u * k, U(Add(Add(a, b), cc)));
        }
    }
    m.W32(cursor + 12, saved);                                     // 0x80039014, the restore
}

// ============================================================================ 0x8002E080

void FillRsqrtTable(uint16_t out[1024], const int16_t* sqrtTable) {
    for (uint32_t i = 0; i < 1024; ++i) {
        const uint32_t d = (i >> 1) + ((i - 2u) >> 31);            // 0x8002E0A8..0x8002E0B4
        const uint32_t q = 0x80000000u / d;                        // `divu`; d >= 1
        uint32_t r = U(SqrtGte(S(q), sqrtTable)) << 2;             // 0x8002E0C4, 0x8002E0CC
        uint32_t lz = 0;
        if (r != 0) {                                              // `mtc2 $30` / `swc2 $31`
            uint32_t v = (r & 0x80000000u) ? ~r : r;
            while (lz < 32 && (v & 0x80000000u) == 0) { ++lz; v <<= 1; }
        }
        const int32_t sh = 21 - static_cast<int32_t>(lz);
        if (sh > 0) r >>= static_cast<uint32_t>(sh) & 31u;          // `srlv`
        out[i] = static_cast<uint16_t>((r << 5) | U(sh));
    }
}

// ============================================================================ the arena

namespace {

uint32_t LoadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool TagIs(const uint8_t* p, const char* tag) {
    return p[0] == static_cast<uint8_t>(tag[0]) && p[1] == static_cast<uint8_t>(tag[1]) &&
           p[2] == static_cast<uint8_t>(tag[2]) && p[3] == static_cast<uint8_t>(tag[3]);
}

// The offset of the block tagged `tag` in a `tag[4]; u32 size;` chain starting at `from`, or 0.
size_t FindBlock(const uint8_t* bytes, size_t size, size_t from, const char* tag, uint32_t* blockSize) {
    size_t o = from;
    while (o + 8 <= size) {
        const uint32_t s = LoadU32(bytes + o + 4);
        if (TagIs(bytes + o, tag)) {
            *blockSize = s;
            return o;
        }
        if (s < 8 || o + s > size) return 0;
        o += s;
    }
    return 0;
}

} // namespace

uint32_t RoadArena::LoadRoadMap(const uint8_t* file, size_t size, uint32_t at, const char** error) {
    GuestRam m(ram_.data(), 0);
    if (size < 0x60 || (at & 3u) != 0) {
        *error = "the road map is too short, or the address is unaligned";
        return 0;
    }
    // The block chain starts at the BTT_ tag (file +0x5C on both disc sets).
    size_t chain = 0;
    for (size_t o = 8; o + 8 <= size; o += 4)
        if (TagIs(file + o, "BTT_")) { chain = o; break; }
    if (chain == 0) {
        *error = "the road map has no BTT_ block";
        return 0;
    }
    struct Block { const char* tag; uint32_t headerWord; uint32_t countHalf; uint32_t record; };
    static const Block kBlocks[6] = {{"BTT_", 0x1C, 0x28, 32}, {"BST_", 0x20, 0x2A, 8},
                                     {"BIT_", 0x24, 0x2C, 104}, {"IPT_", 0x30, 0x3C, 12},
                                     {"PDT_", 0x34, 0x3E, 12}, {"GPDT", 0x38, 0x40, 12}};
    for (size_t i = 0; i < size; ++i) m.W8(at + static_cast<uint32_t>(i), file[i]);
    const uint32_t g = at + 8u;
    for (const Block& b : kBlocks) {
        uint32_t bytes = 0;
        const size_t off = FindBlock(file, size, chain, b.tag, &bytes);
        if (off == 0 || bytes < 8) {
            *error = "the road map lacks one of BTT_/BST_/BIT_/IPT_/PDT_/GPDT";
            return 0;
        }
        m.W32(g + b.headerWord, at + static_cast<uint32_t>(off) + 8u);
        m.W16(g + b.countHalf, static_cast<uint16_t>((bytes - 8u) / b.record));
    }
    m.W32(kRoadGraphPtr, g);
    if (m.Faulted()) {
        *error = "the road map does not fit in guest RAM at that address";
        return 0;
    }
    graph_ = g;
    return g;
}

uint32_t RoadArena::LoadRoadObject(const uint8_t* chunk, size_t size, uint32_t at, const char** error) {
    GuestRam m(ram_.data(), 0);
    if (graph_ == 0) {
        *error = "load the road map first";
        return 0;
    }
    if (size < 0x94 || (at & 3u) != 0 || !TagIs(chunk + 0x8C, "GRPT")) {
        *error = "not a type-3 road chunk (no GRPT at +0x8C), or an unaligned address";
        return 0;
    }
    struct Slot { const char* tag; uint32_t word; };
    static const Slot kSlots[12] = {{"GRPT", 0x4C}, {"SUBT", 0x50}, {"SLCT", 0x54}, {"XSIH", 0x58},
                                    {"XSDH", 0x5C}, {"XSAI", 0x60}, {"DIST", 0x64}, {"SEG_", 0x68},
                                    {"BGDT", 0x7C}, {"BSDT", 0x80}, {"BZDT", 0x84}, {"NMBD", 0x88}};
    for (size_t i = 0; i < size; ++i) m.W8(at + static_cast<uint32_t>(i), chunk[i]);
    for (const Slot& s : kSlots) {
        uint32_t bytes = 0;
        const size_t off = FindBlock(chunk, size, 0x8C, s.tag, &bytes);
        if (off != 0) m.W32(at + s.word, at + static_cast<uint32_t>(off) + 8u);
    }
    const uint32_t obj = at + 0x20u;
    // BTT_ +0x0C, through the ported BttRecord so the index rule is the original's own.
    GuestRam view(ram_.data(), 0);
    const uint32_t btt = RoadBttRecord(view, view.S32(obj + 0));
    if (btt == 0) {
        *error = "the object's id has no BTT_ record in the loaded map";
        return 0;
    }
    view.W32(btt + 12u, obj);
    if (m.Faulted() || view.Faulted()) {
        *error = "the road object does not fit in guest RAM at that address";
        return 0;
    }
    return obj;
}

// ============================================================================ AiDrive's view

bool GuestAiRoadQuery::LookAhead(int32_t ahead, int32_t along, int32_t dir, uint32_t cursorOffset,
                                 uint32_t aimOffset, int16_t sliceAxis[3], int32_t sliceOrigin[3]) {
    if (host_ != nullptr) m_.WriteBlock(entity_, host_, kEntityBytes);
    const uint32_t out = sp_ + kAiDriveCursor;
    RoadLookAhead(m_, entity_, ahead, along, dir, entity_ + cursorOffset, entity_ + aimOffset, out,
                  sp_);
    if (host_ != nullptr) m_.ReadBlock(entity_, host_, kEntityBytes);
    // 0x8009557C: `lw a2,44(sp)` - the slice the output cursor names, read by the port.
    const uint32_t slice = m_.U32(out + 12);
    for (uint32_t k = 0; k < 3; ++k) {
        sliceAxis[k] = m_.S16(slice + 2u + 2u * k);
        sliceOrigin[k] = m_.S32(slice + 20u + 4u * k);
    }
    return !m_.Faulted();
}

} // namespace rr::sim
