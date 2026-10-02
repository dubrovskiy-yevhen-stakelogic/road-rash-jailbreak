#include "game/sim/subdiv.h"

namespace rr::sim::subdiv {
namespace {

int32_t Add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
int32_t Sub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
bool In(int i) { return i >= 0 && i < kMaxRecords; }

} // namespace

// 0x80069F24..0x80069FB8 (and the same run in the other three): the depth bit by z, the near plane, then the
// screen outcodes of the SXY.
uint8_t Outcode(uint32_t sxy, int32_t z) {
    uint32_t lo = (sxy & 0x8000u) >> 15;
    const int shift = z > 0x18FFF ? static_cast<int>(z < 0xC800) + 5 : static_cast<int>(z < 0xC800) + 6;
    uint32_t f = 1u << shift;
    if (z < 10240) f |= 0x10u;
    const int32_t sy = static_cast<int32_t>(sxy) >> 16;
    if (sy < 0) f |= 8u;
    if (sy >= 241) f |= 4u;
    if ((sxy & 0xFFFFu) >= 385u) lo += 1u;
    return static_cast<uint8_t>(f | lo);
}

// One new record of the first loop of 0x8006929C / 0x80069784 / 0x80069CF0, in their store order.
void Subdivider::Midpoint(int dst, int a, int b, int32_t round, bool edge) {
    Rec& d = rec[dst];
    {
        const uint32_t ua = rec[a].uv, ub = rec[b].uv;
        d.uv = (((ua & 0xFFFFFEFFu) + (ub & 0xFFFFFEFFu)) >> 1) + (ua & ub & 0x100u);
    }
    d.shade = Add(rec[a].shade, rec[b].shade) >> 1;
    d.rgb = (d.rgb & 0xFF000000u) | (env_.Colour(static_cast<uint32_t>(d.shade) & 0xFFu) & 0x00FFFFFFu);
    d.x = Add(rec[a].x, rec[b].x) >> 1;
    d.y = Add(rec[a].y, rec[b].y) >> 1;
    d.z = Add(rec[a].z, rec[b].z) >> 1;
    if (edge && nudge) {
        // 0x80069E50..0x80069EE4: n = (z + r) >> 8; y moves by n (+ when the edge's SX difference is negative)
        // when |dy| < 2 |dx|, x by n (- when the SY difference is negative) when |dx| < 2 |dy|.
        const int32_t n = Add(d.z, round) >> 8;
        const int32_t t0 = static_cast<int32_t>(static_cast<uint32_t>(n) << 1);
        const uint32_t sa = rec[a].sxy, sb = rec[b].sxy;
        int32_t dx = Sub(static_cast<int16_t>(sa & 0xFFFFu), static_cast<int16_t>(sb & 0xFFFFu));
        int32_t dy = Sub(static_cast<int32_t>(sa) >> 16, static_cast<int32_t>(sb) >> 16);
        const int32_t mx = dx >> 31, my = dy >> 31;
        const int32_t ax = static_cast<int32_t>(static_cast<uint32_t>(Add(mx, dx)) ^ static_cast<uint32_t>(mx));
        const int32_t ay = static_cast<int32_t>(static_cast<uint32_t>(Add(my, dy)) ^ static_cast<uint32_t>(my));
        int32_t ny = Sub(t0 & mx, n);
        ny &= Sub(ay, static_cast<int32_t>(static_cast<uint32_t>(ax) << 1)) >> 31;
        int32_t nx = Add(n, Sub(0, t0) & my);
        nx &= Sub(ax, static_cast<int32_t>(static_cast<uint32_t>(ay) << 1)) >> 31;
        d.y = Add(d.y, ny);
        d.x = Add(d.x, nx);
    }
    d.sxy = env_.Project(d.x >> 5, d.y >> 5, d.z >> 5);
    d.rgb = (d.rgb & 0x00FFFFFFu) | (static_cast<uint32_t>(Outcode(d.sxy, d.z)) << 24);
    if (dst > deepest) deepest = dst;
}

// ---------------------------------------------------------------------------- 0x8006929C
bool Subdivider::Tri(int base, uint32_t corners, int32_t level) {
    // 0x80069300..0x80069548: t4 = 0x01009008, two byte selectors per record, 8 bits a record:
    // base+0 = mid(1, 0), base+1 = mid(2, 1), base+2 = mid(0, 2).
    uint32_t t4 = 0x01009008u;
    for (int k = 0; k < 3; ++k) {
        const int a = static_cast<int>((corners >> (t4 & 0x18u)) & 0xFFu);
        t4 >>= 4;
        const int b = static_cast<int>((corners >> (t4 & 0x18u)) & 0xFFu);
        t4 >>= 4;
        if (!In(a) || !In(b) || !In(base + k)) return false;
        Midpoint(base + k, a, b, 0x80, true);
        if (!env_.Stored(base + k, rec[base + k], true)) return false;
    }
    uint8_t bytes[6] = {static_cast<uint8_t>(corners), static_cast<uint8_t>(corners >> 8),
                        static_cast<uint8_t>(corners >> 16), static_cast<uint8_t>(base),
                        static_cast<uint8_t>(base + 1), static_cast<uint8_t>(base + 2)};
    const int32_t next = level + 1;
    for (int k = 0; k < 4; ++k) {
        const int32_t w = static_cast<int32_t>(kids_.tri[k]);
        const int ia = w >> 8, ib = (w >> 4) & 15, ic = w & 15;
        if (ia < 0 || ia > 5 || ib > 5 || ic > 5) return false;
        const int v[3] = {bytes[ia], bytes[ib], bytes[ic]};
        for (int i : v)
            if (!In(i)) return false;
        const uint8_t fa = rec[v[0]].Flags(), fb = rec[v[1]].Flags(), fc = rec[v[2]].Flags();
        if ((fa & fb & fc & 0x1Fu) != 0) continue;
        const uint32_t any = static_cast<uint32_t>(fa | fb | fc);
        if ((any >> (static_cast<uint32_t>(next) & 31u)) != 0 || (base < 12 && (any & 0x10u) != 0)) {
            const uint32_t packed = static_cast<uint32_t>(v[0]) | static_cast<uint32_t>(v[1]) << 8 |
                                    static_cast<uint32_t>(v[2]) << 16;
            if (!Tri(base + 3, packed, next)) return false;
        } else if (!env_.Gt3(rec, v)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------- 0x80069784 / 0x80069CF0
bool Subdivider::QuadCommon(int base, uint32_t corners, int32_t level, int32_t round, int limit) {
    // t3 = 0x2C04A4D8, two 3-bit selectors a record into {corners, base+1, base+3}: base+4 = mid(0, 3),
    // base+3 = mid(3, 2), base+2 = mid(2, 1), base+1 = mid(1, 0), base+0 = mid(base+1, base+3) (the centre,
    // not nudged).
    const uint8_t sel[6] = {static_cast<uint8_t>(corners), static_cast<uint8_t>(corners >> 8),
                            static_cast<uint8_t>(corners >> 16), static_cast<uint8_t>(corners >> 24),
                            static_cast<uint8_t>(base + 1), static_cast<uint8_t>(base + 3)};
    uint32_t t3 = 0x2C04A4D8u;
    for (int s4 = 4; s4 >= 0; --s4) {
        const int a = sel[t3 & 7u];
        const int b = sel[(t3 >> 3) & 7u];
        t3 >>= 6;
        if (!In(a) || !In(b) || !In(base + s4)) return false;
        Midpoint(base + s4, a, b, round, s4 != 0);
        if (!env_.Stored(base + s4, rec[base + s4], true)) return false;
    }
    const uint8_t bytes[9] = {sel[0], sel[1], sel[2], sel[3], static_cast<uint8_t>(base), static_cast<uint8_t>(base + 1),
                              static_cast<uint8_t>(base + 2), static_cast<uint8_t>(base + 3), static_cast<uint8_t>(base + 4)};
    const int32_t next = level + 1;
    for (int k = 0; k < 4; ++k) {
        const int32_t w = static_cast<int32_t>(kids_.quad[k]);
        const int ia = w >> 12, ib = (w >> 8) & 15, id = (w >> 4) & 15, ic = w & 15;
        if (ia < 0 || ia > 8 || ib > 8 || id > 8 || ic > 8) return false;
        const int A = bytes[ia], B = bytes[ib], C = bytes[ic], D = bytes[id];
        if (!In(A) || !In(B) || !In(C) || !In(D)) return false;
        const uint8_t fa = rec[A].Flags(), fb = rec[B].Flags(), fc = rec[C].Flags(), fd = rec[D].Flags();
        if ((fa & fb & fd & fc & 0x1Fu) != 0) continue;
        const uint32_t any = static_cast<uint32_t>(fa | fb | fd | fc);
        if ((any >> (static_cast<uint32_t>(next) & 31u)) != 0 || (base < limit && (any & 0x10u) != 0)) {
            const uint32_t packed = static_cast<uint32_t>(A) | static_cast<uint32_t>(B) << 8 |
                                    static_cast<uint32_t>(D) << 16 | static_cast<uint32_t>(C) << 24;
            if (!QuadCommon(base + 5, packed, next, round, limit)) return false;
        } else {
            const int v[4] = {A, B, C, D};
            if (!env_.Gt4(rec, v)) return false;
        }
    }
    return true;
}

bool Subdivider::Quad(int base, uint32_t corners, int32_t level) { return QuadCommon(base, corners, level, 0x80, 19); }
bool Subdivider::Road(int base, uint32_t corners, int32_t level) { return QuadCommon(base, corners, level, 0x40, 14); }

// ---------------------------------------------------------------------------- 0x8006A25C
bool Subdivider::Line(int base, uint32_t corners, uint32_t colour) {
    // t2 = 0x1908: base+0 = mid(1, 2), base+1 = mid(3, 0); no texel, no nudge, TR z = (za + zb) >> 6.
    uint32_t t2 = 0x1908u;
    for (int k = 0; k < 2; ++k) {
        const int a = static_cast<int>((corners >> (t2 & 0x18u)) & 0xFFu);
        t2 >>= 4;
        const int b = static_cast<int>((corners >> (t2 & 0x18u)) & 0xFFu);
        t2 >>= 4;
        const int dst = base + k;
        if (!In(a) || !In(b) || !In(dst)) return false;
        Rec& d = rec[dst];
        d.shade = Add(rec[a].shade, rec[b].shade) >> 1;
        d.rgb = (d.rgb & 0xFF000000u) | (env_.Colour(static_cast<uint32_t>(d.shade) & 0xFFu) & 0x00FFFFFFu);
        d.x = Add(rec[a].x, rec[b].x) >> 1;
        d.y = Add(rec[a].y, rec[b].y) >> 1;
        const int32_t zSum = Add(rec[a].z, rec[b].z);
        d.z = zSum >> 1;
        d.sxy = env_.Project(d.x >> 5, d.y >> 5, zSum >> 6);
        d.rgb = (d.rgb & 0x00FFFFFFu) | (static_cast<uint32_t>(Outcode(d.sxy, d.z)) << 24);
        if (dst > deepest) deepest = dst;
        if (!env_.Stored(dst, d, false)) return false;
    }
    const uint8_t bytes[6] = {static_cast<uint8_t>(corners), static_cast<uint8_t>(corners >> 8),
                              static_cast<uint8_t>(corners >> 16), static_cast<uint8_t>(corners >> 24),
                              static_cast<uint8_t>(base), static_cast<uint8_t>(base + 1)};
    for (int k = 0; k < 2; ++k) {
        const int32_t w = static_cast<int32_t>(kids_.line[k]);
        const int ia = w >> 12, ib = (w >> 8) & 15, id = (w >> 4) & 15, ic = w & 15;
        if (ia < 0 || ia > 5 || ib > 5 || id > 5 || ic > 5) return false;
        const int A = bytes[ia], B = bytes[ib], C = bytes[ic], D = bytes[id];
        if (!In(A) || !In(B) || !In(C) || !In(D)) return false;
        const uint8_t fa = rec[A].Flags(), fb = rec[B].Flags(), fc = rec[C].Flags(), fd = rec[D].Flags();
        if ((fa & fb & fd & fc & 0x1Fu) != 0) continue;
        if (((fa | fb | fd | fc) & 4u) != 0 && base < 14) {
            const uint32_t packed = static_cast<uint32_t>(A) | static_cast<uint32_t>(B) << 8 |
                                    static_cast<uint32_t>(D) << 16 | static_cast<uint32_t>(C) << 24;
            if (!Line(base + 2, packed, colour)) return false;
        } else {
            const int v[4] = {A, B, C, D};
            if (!env_.F4(rec, v, colour)) return false;
        }
    }
    return true;
}

} // namespace rr::sim::subdiv
