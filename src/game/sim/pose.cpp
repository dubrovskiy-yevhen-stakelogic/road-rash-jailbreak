#include "game/sim/pose.h"

#include "game/sim/fixed.h"
#include "game/sim/integrator.h"

// Every function below is one guest function, transcribed from our listing of RASHCDG.BIN
// (cfe43a77...) / SLUS_010.53 (67ed165a...). Comments carry the original's addresses. Loads and
// stores outside the function's own frame are made in the original's order, with the original's
// re-reads (the owner, its model, the clip's part count are re-read every turn of the part loops).

namespace rr::sim {
namespace {

inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t Mul32(int32_t a, int32_t b) { return S(U(a) * U(b)); } // `mult` then `mflo`
inline int16_t Lo16(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(U(v))); }
inline int16_t Neg16(int16_t v) { return Lo16(-static_cast<int32_t>(v)); }

// The trapping `add` / `sub` of QuatToMatrix: false on a signed 32-bit overflow.
inline bool AddT(int32_t a, int32_t b, int32_t& r) {
    const int64_t w = static_cast<int64_t>(a) + b;
    r = static_cast<int32_t>(w);
    return w == r;
}
inline bool SubT(int32_t a, int32_t b, int32_t& r) {
    const int64_t w = static_cast<int64_t>(a) - b;
    r = static_cast<int32_t>(w);
    return w == r;
}

// MVMVA sf = 1, lm = 0 row: (sum_j R_ij v_j) >> 12, IR saturated to s16.
inline int16_t MvmvaRow(int32_t r0, int32_t r1, int32_t r2, int32_t v0, int32_t v1, int32_t v2) {
    const int64_t sum = static_cast<int64_t>(r0) * v0 + static_cast<int64_t>(r1) * v1 + static_cast<int64_t>(r2) * v2;
    int64_t mac = sum >> 12;
    if (mac < -0x8000) mac = -0x8000;
    if (mac > 0x7FFF) mac = 0x7FFF;
    return static_cast<int16_t>(mac);
}

} // namespace

// ============================================================================ the leaves

// RASHCDG 0x8005E558 Bits(p, pos, w): a w-bit field, MSB first, out of the halfword stream at *p.
// Both halfwords are loaded before the pointer store.
uint32_t AnimPose::Bits(uint32_t p, uint32_t pos, uint32_t w) {
    const uint32_t t1 = g_.U8(pos);
    const uint32_t v1 = g_.U32(p);
    const uint32_t t0 = t1 + w;
    const uint32_t a3 = (t0 >> 3) & 0x1Eu;
    const uint32_t hi = g_.U16(v1);
    const uint32_t lo = g_.U16(v1 + 2u);
    g_.W32(p, v1 + a3);
    g_.W8(pos, static_cast<uint8_t>(t0 & 0xFu));
    const uint32_t win = (hi << 16) | lo;
    return ((win << (t1 & 31u)) >> ((32u - w) & 31u)) & 0xFFu;
}

// RASHCDG 0x80066A60 SetRoot(owner, v): owner +0x1C/+0x1E/+0x20 := v[0..2]. Returns v[2].
uint32_t AnimPose::SetRoot(uint32_t owner, uint32_t v) {
    g_.W16(owner + 28u, g_.U16(v));
    g_.W16(owner + 30u, g_.U16(v + 2u));
    const uint16_t z = g_.U16(v + 4u);
    g_.W16(owner + 32u, z);
    return z;
}

void AnimPose::PoseRoot(uint32_t owner, int16_t x, int16_t y, int16_t z) {
    g_.W16(owner + 28u, static_cast<uint16_t>(x));
    g_.W16(owner + 30u, static_cast<uint16_t>(y));
    g_.W16(owner + 32u, static_cast<uint16_t>(z));
}

// RASHCDG 0x800714FC QuatMul(a, b, out): (x, y, z, w) at +0/+2/+4/+6, 1.14. Every operand is read
// before the first store, so out may alias either input.
void AnimPose::QuatMulHost(const int16_t a[4], const int16_t b[4], int16_t out[4]) const {
    const int32_t ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const int32_t bx = b[0], by = b[1], bz = b[2], bw = b[3];
    const int32_t w = S(U(Mul32(aw, bw)) - U(Mul32(ax, bx)) - U(Mul32(ay, by)) - U(Mul32(az, bz))) >> 14;
    const int32_t x = S(U(Mul32(aw, bx)) + U(Mul32(ax, bw)) + U(Mul32(ay, bz)) - U(Mul32(az, by))) >> 14;
    const int32_t y = S(U(Mul32(aw, by)) + U(Mul32(ay, bw)) + U(Mul32(az, bx)) - U(Mul32(ax, bz))) >> 14;
    const int32_t z = S(U(Mul32(aw, bz)) + U(Mul32(az, bw)) + U(Mul32(ax, by)) - U(Mul32(ay, bx))) >> 14;
    out[0] = Lo16(x);
    out[1] = Lo16(y);
    out[2] = Lo16(z);
    out[3] = Lo16(w);
}

uint32_t AnimPose::QuatMul(uint32_t a, uint32_t b, uint32_t out) {
    int16_t qa[4], qb[4], r[4];
    for (uint32_t k = 0; k < 4; ++k) {
        qa[k] = g_.S16(a + 2u * k);
        qb[k] = g_.S16(b + 2u * k);
    }
    QuatMulHost(qa, qb, r);
    for (uint32_t k = 0; k < 4; ++k) g_.W16(out + 2u * k, static_cast<uint16_t>(r[k]));
    return static_cast<uint16_t>(r[3]); // the last `lhu v0,6(sp)`
}

// SLUS 0x8001005C QuatToMatrix(M, q): q = s32[4] (1.14 << 2); s = 2 / |q|^2 by two `divu` steps;
// the 4.12 matrix stored in the order +14, +10, +0, +8, +16, +2, +6, +4, +12. The `add`/`sub` trap.
void AnimPose::QuatToMatrixHost(uint32_t m, const int32_t q[4]) {
    const int32_t t0 = q[0], t1 = q[1], t2 = q[2], t3 = q[3];
    int32_t n = 0;
    bool ok = AddT(FixMul(t0, t0), FixMul(t1, t1), n);                 // 0x800100A8
    ok = ok && AddT(n, FixMul(t2, t2), n);                              // 0x800100C4
    ok = ok && AddT(n, FixMul(t3, t3), n);                              // 0x800100D8
    if (!ok) { refused_ = true; return; }
    const uint32_t d = U(n);
    const uint32_t q1 = d == 0 ? 0xFFFFFFFFu : 0x20000u / d;           // divu at,v0
    const uint32_t r1 = d == 0 ? 0x20000u : 0x20000u % d;
    const uint32_t n2 = r1 << 16;
    const uint32_t q2 = d == 0 ? 0xFFFFFFFFu : n2 / d;                 // divu t7,v0
    const int32_t s = S((q2 & 0xFFFFu) | (q1 << 16));
    const int32_t xs = FixMul(t0, s), ys = FixMul(t1, s), zs = FixMul(t2, s);
    int32_t v0 = FixMul(t3, xs), v1 = FixMul(t1, zs), at = 0;
    if (!AddT(v0, v1, at)) { refused_ = true; return; }
    g_.W16(m + 14u, static_cast<uint16_t>(at >> 4));
    if (!SubT(v1, v0, at)) { refused_ = true; return; }
    g_.W16(m + 10u, static_cast<uint16_t>(at >> 4));
    const int32_t a1 = FixMul(t0, xs), a2 = FixMul(t1, ys), a3 = FixMul(t2, zs);
    const auto diag = [&](int32_t p, int32_t r, uint32_t off) {
        int32_t sum = 0;
        if (!AddT(p, r, sum) || !SubT(0x10000, sum, sum)) return false;
        g_.W16(m + off, static_cast<uint16_t>(sum >> 4));
        return true;
    };
    if (!diag(a2, a3, 0u) || !diag(a1, a3, 8u) || !diag(a1, a2, 16u)) { refused_ = true; return; }
    v1 = FixMul(t3, zs);
    const int32_t t4 = FixMul(t0, ys);
    v0 = FixMul(t3, ys);
    const int32_t t5 = FixMul(t0, zs);
    if (!SubT(t4, v1, at)) { refused_ = true; return; }
    g_.W16(m + 2u, static_cast<uint16_t>(at >> 4));
    if (!AddT(t4, v1, at)) { refused_ = true; return; }
    g_.W16(m + 6u, static_cast<uint16_t>(at >> 4));
    if (!AddT(t5, v0, at)) { refused_ = true; return; }
    g_.W16(m + 4u, static_cast<uint16_t>(at >> 4));
    if (!SubT(t5, v0, at)) { refused_ = true; return; }
    g_.W16(m + 12u, static_cast<uint16_t>(at >> 4));
}

uint32_t AnimPose::QuatToMatrix(uint32_t m, uint32_t q) {
    int32_t v[4];
    for (uint32_t k = 0; k < 4; ++k) v[k] = g_.S32(q + 4u * k);
    QuatToMatrixHost(m, v);
    return 0; // not compared: v0 is the last `sra` of a trapping sum
}

bool AnimPose::AsinOf(int32_t x, int32_t& out) {
    uint16_t table[64];
    for (uint32_t k = 0; k < 64; ++k) table[k] = g_.U16(kPoseAsinTable + 2u * k);
    if (!Asin(x, table, out)) {
        refused_ = true;
        return false;
    }
    return true;
}

// RASHCDG 0x800710C0 Slerp(t, q0, q1, out): the two weights. cos = dot >> 14 << 2 (the shorter arc:
// a negative cos is negated and so, at the end, is the second weight); below 1 - 16/65536 the
// weights are sin((1-t) th) / sin th and sin(t th) / sin th with th = 1024 - Asin(cos), each a
// sign-armed unsigned FixDiv; else 1 - t and t.
bool AnimPose::SlerpWeights(int32_t t, const int16_t q0[4], const int16_t q1[4], int32_t& w0, int32_t& w1) {
    const int32_t dot = S(U(Mul32(q0[0], q1[0])) + U(Mul32(q0[1], q1[1])) + U(Mul32(q0[2], q1[2])) +
                          U(Mul32(q0[3], q1[3])));
    int32_t c = S(U(dot >> 14) << 2);
    bool flip = false;
    if (c < 0) { c = S(0u - U(c)); flip = true; }                      // 0x80071160
    int32_t s4 = S(0x10000u - U(t));                                   // delay slot of 0x80071184
    int32_t s3 = t;
    if (16 < S(0x10000u - U(c))) {
        const int32_t s0 = S(0x10000u - U(t));
        int32_t as = 0;
        if (!AsinOf(c, as)) return false;
        const int32_t th = S(1024u - U(as));                           // s1
        const int32_t s2 = S(U(static_cast<int32_t>(SinAt(U(th)))) << 4);
        const auto weight = [&](int32_t f) {                            // 0x800711BC.. / 0x800712AC..
            const int32_t sa = S(U(static_cast<int32_t>(SinAt(U(FixMul(f, th))))) << 4);
            if (sa > 0) {
                if (s2 > 0) return S(FixDiv(U(sa), U(s2)));
                return S(0u - FixDiv(U(sa), 0u - U(s2)));
            }
            if (s2 > 0) return S(0u - FixDiv(0u - U(sa), U(s2)));
            return S(FixDiv(0u - U(sa), 0u - U(s2)));
        };
        s4 = weight(s0);
        s3 = weight(t);
    }
    if (flip) s3 = S(0u - U(s3));                                       // 0x800713A8
    w0 = S(U(s4) << 14) >> 16;                                         // a1
    w1 = S(U(s3) << 14) >> 16;                                         // a0
    return true;
}

uint32_t AnimPose::Slerp(int32_t t, uint32_t q0, uint32_t q1, uint32_t out) {
    int16_t a[4], b[4];
    for (uint32_t k = 0; k < 4; ++k) {
        a[k] = g_.S16(q0 + 2u * k);
        b[k] = g_.S16(q1 + 2u * k);
    }
    int32_t w0 = 0, w1 = 0;
    if (!SlerpWeights(t, a, b, w0, w1)) return 0;
    uint32_t v = 0;
    for (uint32_t k = 0; k < 4; ++k) { // each component re-read after the previous store (aliasing)
        const int32_t x = (Mul32(w0, g_.S16(q0 + 2u * k)) >> 14) + (Mul32(w1, g_.S16(q1 + 2u * k)) >> 14);
        v = U(x);
        g_.W16(out + 2u * k, static_cast<uint16_t>(x));
    }
    return v;
}

// ============================================================================ Sample

// RASHCDG 0x8005E5A4 Sample(a, T): every channel of the current clip stepped to `frame` / `frame+1`
// (the second clamped to `frame` past the last key). Raw channels are read out of their arrays;
// animated coded channels are decoded forward from `T.sampled + 1`; constant coded channels are left
// as the header wrote them. T.sampled := frame at the end.
uint32_t AnimPose::Sample(uint32_t a, uint32_t t) {
    const uint32_t cp = g_.U32(a + animf::kClip);
    const int32_t frame = g_.S32(a + animf::kFrame);                  // s6
    int32_t last = S(U(frame) + 1u);                                    // s4
    if (S(U(static_cast<int32_t>(g_.U16(cp + 16u))) - 1u) < last) last = frame;
    int32_t count = g_.S16(t + 2u);
    if (count <= 0) {
        g_.W16(t, static_cast<uint16_t>(frame));
        return U(count);
    }
    uint32_t ch = t + 4u;                                               // s1; s0 = ch + 6
    for (int32_t k = 0;;) {
        const uint8_t fl = g_.U8(ch + 20u);
        if (!(fl & 2u)) {                                               // 0x8005E7C4: raw
            g_.W16(ch + 4u, g_.U16(g_.U32(ch) + 2u * U(frame)));
            g_.W16(ch + 6u, g_.U16(g_.U32(ch) + 2u * U(last)));
        } else if (fl & 1u) {                                           // animated, coded
            int32_t s2 = S(U(static_cast<int32_t>(g_.S16(t))) + 1u);
            while (s2 < last) {
                if (g_.U8(ch + 20u) & 4u) {                             // 0x8005E65C: in a run
                    const uint8_t rv = g_.U8(ch + 18u);
                    uint8_t f2 = g_.U8(ch + 20u);
                    const int32_t rc = S(g_.U32(ch + 12u) - 1u);
                    f2 = static_cast<uint8_t>(f2 & 0xFBu);
                    g_.W8(ch + 21u, rv);
                    g_.W32(ch + 12u, U(rc));
                    if (rc > 0) f2 = static_cast<uint8_t>(f2 | 4u);
                    g_.W8(ch + 20u, f2);
                } else {
                    uint32_t d = Bits(ch, ch + 23u, g_.U8(ch + 22u));   // 0x8005E68C
                    g_.W8(ch + 21u, static_cast<uint8_t>(d));
                    if (static_cast<int8_t>(d) == g_.S8(ch + 19u)) {   // the escape: a run
                        g_.W32(ch + 12u, 0);
                        for (;;) {
                            d = Bits(ch, ch + 23u, g_.U8(ch + 22u));    // 0x8005E6B8
                            g_.W8(ch + 21u, static_cast<uint8_t>(d));
                            const uint32_t a0 = 1u << (g_.U8(ch + 22u) & 31u);
                            if (static_cast<int32_t>(static_cast<int8_t>(d)) != S(a0 - 1u)) break;
                            g_.W32(ch + 12u, g_.U32(ch + 12u) - 1u + a0);
                            if (g_.Faulted()) return 0;
                        }
                        const uint32_t w = g_.U8(ch + 22u);
                        g_.W32(ch + 12u, g_.U32(ch + 12u) + 4u + U(static_cast<int32_t>(g_.S8(ch + 21u))));
                        d = Bits(ch, ch + 23u, w);                      // 0x8005E710
                        g_.W8(ch + 18u, static_cast<uint8_t>(d));
                        g_.W8(ch + 21u, static_cast<uint8_t>(d));
                        const uint8_t f3 = static_cast<uint8_t>(g_.U8(ch + 20u) | 4u);
                        const uint32_t rc = g_.U32(ch + 12u) - 1u;
                        g_.W8(ch + 20u, f3);
                        g_.W32(ch + 12u, rc);
                    }
                }
                // 0x8005E738: the w-bit two's-complement delta, << 4, into the accumulator
                const uint32_t w = g_.U8(ch + 22u);
                const int32_t code = g_.S8(ch + 21u);
                int32_t delta = code;
                if ((code >> ((w - 1u) & 31u)) & 1) delta = S(U(code) - (1u << (w & 31u)));
                g_.W32(ch + 8u, g_.U32(ch + 8u) + (U(delta) << 4));
                const uint32_t scale = g_.U16(ch + 16u);
                const int32_t prod = S(g_.U32(ch + 8u) * scale);         // mult, mflo
                g_.W16(ch + 4u, g_.U16(ch + 6u));
                g_.W16(ch + 6u, static_cast<uint16_t>(U(prod >> 9)));
                s2 = S(U(s2) + 1u);
                if (g_.Faulted()) return 0;
            }
            if (last == frame) g_.W16(ch + 4u, g_.U16(ch + 6u));        // 0x8005E7B0
        }
        ++k;
        count = g_.S16(t + 2u);
        if (!(k < count)) break;
        ch += 24u;
        if (g_.Faulted()) return 0;
    }
    g_.W16(t, static_cast<uint16_t>(frame));
    return 0;
}

// ============================================================================ Pose

// RASHCDG 0x8005D63C Pose(a).
uint32_t AnimPose::Pose(uint32_t a) {
    const uint32_t op = g_.U32(a + animf::kProgram) + 12u * g_.U32(a + animf::kPc);
    const uint8_t pf = g_.U8(op + 2u);
    const uint32_t clip = g_.U32(a + animf::kClip);                    // sp+88
    const bool mirror = (pf & 1u) != 0;                                 // s6
    const uint32_t mask = (pf & 0x40u) ? 0xFFFFFFFFu : g_.U32(a + animf::kMask); // sp+92
    int32_t s = 0;                                                      // s2
    if (g_.U32(a + animf::kFlags) & 4u) s = GuestDiv(S(g_.U32(a + animf::kSub) << 16), g_.S32(a + animf::kRate));
    const uint32_t owner = g_.U32(a);
    int32_t mirrorX = 0, yAdd = 0;                                      // s7, s4
    uint32_t cls = 0;                                                   // s8
    if (((g_.U32(owner + 36u) >> 18) & 1u) == 1u && g_.U32(owner + 52u) != 0u) {
        mirrorX = 1148;
        yAdd = (0xFFFF < g_.S32(owner + 76u)) ? 10 : 140;
    } else if (((g_.U16(g_.U32(owner) + 14u) & 0x78u) >> 3) == 1u) {
        const uint32_t parent = g_.U32(owner + 52u);
        if (parent != 0u) cls = (g_.U16(g_.U32(parent) + 14u) & 0xF80u) >> 7;
    }
    // ---- the root: channels 0..2 (key, or the FixMul lerp of key and next)
    int16_t rx, ry, rz;
    if (s == 0) {
        rx = g_.S16(a + 56u);
        ry = g_.S16(a + 80u);
        rz = g_.S16(a + 104u);
    } else {
        const int16_t k0 = g_.S16(a + 56u), n0 = g_.S16(a + 58u);
        const int16_t k1 = g_.S16(a + 80u), n1 = g_.S16(a + 82u);
        const int16_t k2 = g_.S16(a + 104u), n2 = g_.S16(a + 106u);
        const int32_t s1 = S(0x10000u - U(s));
        rx = Lo16(S(U(FixMul(s1, k0)) + U(FixMul(s, n0))));
        ry = Lo16(S(U(FixMul(s1, k1)) + U(FixMul(s, n1))));
        rz = Lo16(S(U(FixMul(s1, k2)) + U(FixMul(s, n2))));
    }
    if (cls == 1u) {                                                    // 0x8005D864
        rx = Lo16(rx + g_.S16(kPoseSeatTable + 4u));
        ry = Lo16(ry + g_.S16(kPoseSeatTable + 6u));
        rz = Lo16(rz + g_.S16(kPoseSeatTable + 8u));
    }
    PoseRoot(owner, mirror ? Lo16(mirrorX - rx) : rx, Lo16(ry + yAdd), rz); // SetRoot 0x80066A60
    // ---- the parts
    if (g_.U8(clip + 15u) != 0u) {
        const int32_t s14 = S(U(s) << 14) >> 16;                        // s1
        const int32_t s2w = 16384 - s14;                                // s2
        uint32_t k = 0;
        do {
            const uint32_t slot = mirror ? g_.U8(kPoseMirrorSlots + k) : k;
            if ((mask >> (slot & 31u)) & 1u) {
                const uint32_t type = (g_.U16(g_.U32(g_.U32(a)) + 14u) & 0x78u) >> 3;
                const uint32_t base = a + 128u + 96u * k;               // channel 3 + 4k: key at +0
                int16_t q[4];
                for (uint32_t i = 0; i < 4; ++i) {
                    const int16_t key = g_.S16(base + 24u * i);
                    if (s14 == 0) {
                        q[i] = key;
                    } else {
                        const int16_t nx = g_.S16(base + 24u * i + 2u);
                        q[i] = Lo16((Mul32(s2w, key) >> 14) + (Mul32(s14, nx) >> 14));
                    }
                }
                int16_t o[4] = {q[0], q[1], q[2], q[3]};
                if (type == 1u || type == 4u) {
                    if (cls == 1u && ((g_.U32(kPoseSeatTable) >> (k & 31u)) & 1u)) {
                        int16_t off[4];
                        for (uint32_t i = 0; i < 4; ++i) off[i] = g_.S16(kPoseSeatTable + 10u + 8u * k + 2u * i);
                        QuatMulHost(off, q, q);                         // 0x8005DAF0
                    }
                    if (!mirror) {
                        for (int i = 0; i < 4; ++i) o[i] = q[i];
                    } else if (k == 0u) {
                        o[0] = Neg16(q[2]); o[1] = Neg16(q[3]); o[2] = Neg16(q[0]); o[3] = Neg16(q[1]);
                    } else {
                        o[0] = Neg16(q[0]); o[1] = Neg16(q[1]); o[2] = q[2]; o[3] = q[3];
                    }
                } else if (type == 5u) {
                    if (mirror) { o[0] = Neg16(q[0]); o[1] = Neg16(q[1]); }
                } else {
                    if (mirror) { o[1] = Neg16(q[1]); o[2] = Neg16(q[2]); }
                }
                const int32_t v[4] = {S(U(static_cast<int32_t>(o[0])) << 2), S(U(static_cast<int32_t>(o[1])) << 2),
                                      S(U(static_cast<int32_t>(o[2])) << 2), S(U(static_cast<int32_t>(o[3])) << 2)};
                QuatToMatrixHost(g_.U32(g_.U32(a) + 4u) + 24u * slot + 4u, v);
                if (refused_) return 0;
            }
            ++k;
            if (g_.Faulted()) return 0;
        } while (k < g_.U8(clip + 15u));
    }
    // ---- stance 42: the tilt about Z by the bike's pitch
    const uint32_t ow = g_.U32(a);
    if (g_.U16(ow + 544u) != 42u) return 0;
    const int32_t sub = GuestDiv(S(g_.U32(a + animf::kSub) << 16), g_.S32(a + animf::kRate));
    const uint32_t op2 = g_.U32(a + animf::kProgram) + 12u * g_.U32(a + animf::kPc);
    const uint32_t cp = g_.U32(g_.U32(g_.U32(a + animf::kBank) + 4u) + 4u * g_.U8(op2));
    const uint32_t keys1w = (U(static_cast<int32_t>(g_.U16(cp + 16u))) - 1u) << 16;
    const int32_t k1 = S(keys1w) >> 16;
    const int32_t half = S(U((k1 + static_cast<int32_t>(keys1w >> 31)) >> 1) << 16);
    const int32_t tt = S((g_.U32(a + animf::kFrame) << 16) + U(sub));
    int32_t angle;
    if (tt < half) {
        const int32_t r = GuestDiv(S(U(tt) << 1), k1);
        const int32_t v = FixMul(g_.S32(g_.U32(ow + 596u) + 652u), r);
        angle = S(U(v) * 652u) >> 16;
    } else {
        angle = S(g_.U32(g_.U32(ow + 596u) + 652u) * 652u) >> 16;
    }
    const int32_t c = CosAt(U(angle)), sn = SinAt(U(angle));
    const int32_t R[3][3] = {{c, sn, 0}, {-sn, c, 0}, {0, 0, 4096}};
    for (uint32_t col = 0; col < 3; ++col) {                            // 0x8005E058.. / E0A8.. / E0F8..
        const uint32_t p = g_.U32(g_.U32(a) + 4u) + 4u + 2u * col;
        const int32_t v0 = g_.S16(p), v1 = g_.S16(p + 6u), v2 = g_.S16(p + 12u);
        const int16_t i1 = MvmvaRow(R[0][0], R[0][1], R[0][2], v0, v1, v2);
        const int16_t i2 = MvmvaRow(R[1][0], R[1][1], R[1][2], v0, v1, v2);
        const int16_t i3 = MvmvaRow(R[2][0], R[2][1], R[2][2], v0, v1, v2);
        g_.W16(p, static_cast<uint16_t>(i1));
        g_.W16(p + 6u, static_cast<uint16_t>(i2));
        g_.W16(p + 12u, static_cast<uint16_t>(i3));
    }
    {                                                                   // 0x8005E170: the root, RT x V0
        const uint32_t r = g_.U32(a) + 28u;
        const int32_t v0 = g_.S16(r), v1 = g_.S16(r + 2u), v2 = g_.S16(r + 4u);
        const int16_t i1 = MvmvaRow(R[0][0], R[0][1], R[0][2], v0, v1, v2);
        const int16_t i2 = MvmvaRow(R[1][0], R[1][1], R[1][2], v0, v1, v2);
        const int16_t i3 = MvmvaRow(R[2][0], R[2][1], R[2][2], v0, v1, v2);
        g_.W16(r, static_cast<uint16_t>(i1));
        g_.W16(r + 2u, static_cast<uint16_t>(i2));
        g_.W16(r + 4u, static_cast<uint16_t>(i3));
    }
    return 0;
}

// ============================================================================ the blend

// RASHCDG 0x8005CB70 TransitionCapture(a, op, B): the FROM pose (the current clip's keys, mirrored by
// op[1]'s flags) and, after ClipSelect(new clip = op[+28]), the TO pose (its key 0, mirrored by
// op[2]'s flags) into the blend buffer B; B.rate = 10, B.parts = 20, B.mask = from & to & a->mask.
// The class-1 pre-multiplication here indexes the offset table by the (mirrored) SLOT.
uint32_t AnimPose::TransitionCapture(uint32_t a, uint32_t op, uint32_t B) {
    uint32_t fromMask = 0, toMask = 0;                                  // sp+44, sp+48
    g_.W32(B + 4u, 10);
    uint32_t owner = g_.U32(a);
    uint32_t cls = 0;                                                   // s5
    int32_t mirrorX = 0, yAdd = 0;                                      // sp+56, sp+64
    const uint32_t newClip = g_.U8(op + 28u);                           // sp+40
    if (((g_.U32(owner + 36u) >> 18) & 1u) == 1u && g_.U32(owner + 52u) != 0u) {
        mirrorX = 1148;
        yAdd = (0xFFFF < g_.S32(owner + 76u)) ? 10 : 140;
    } else {
        owner = g_.U32(a);
        if (((g_.U16(g_.U32(owner) + 14u) & 0x78u) >> 3) == 1u) {
            const uint32_t parent = g_.U32(owner + 52u);
            if (parent != 0u) cls = (g_.U16(g_.U32(parent) + 14u) & 0xF80u) >> 7;
        }
    }
    const auto root = [&](uint32_t mirrorFlags, uint32_t at) {
        int16_t x = g_.S16(a + 56u), y = g_.S16(a + 80u), z = g_.S16(a + 104u);
        const bool m = (mirrorFlags & 1u) != 0;
        if (cls == 1u) {
            x = Lo16(x + g_.S16(kPoseSeatTable + 4u));
            y = Lo16(y + g_.S16(kPoseSeatTable + 6u));
            z = Lo16(z + g_.S16(kPoseSeatTable + 8u));
        }
        g_.W16(at, static_cast<uint16_t>(m ? Lo16(mirrorX - x) : x));
        g_.W16(at + 2u, static_cast<uint16_t>(Lo16(y + yAdd)));
        g_.W16(at + 4u, static_cast<uint16_t>(z));
        return m;
    };
    const auto parts = [&](uint32_t cp, bool m, uint32_t qOff, uint32_t& maskOut) {
        if (g_.U8(cp + 15u) == 0u) return;
        uint32_t k = 0;
        do {
            const uint32_t slot = m ? g_.U8(kPoseMirrorSlots + k) : k;   // s0
            maskOut |= 1u << (slot & 31u);
            const uint32_t type = (g_.U16(g_.U32(g_.U32(a)) + 14u) & 0x78u) >> 3;
            const uint32_t base = a + 128u + 96u * k;
            int16_t q[4];
            for (uint32_t i = 0; i < 4; ++i) q[i] = g_.S16(base + 24u * i);
            int16_t o[4] = {q[0], q[1], q[2], q[3]};
            if (type == 1u || type == 4u) {
                if (cls == 1u && ((g_.U32(kPoseSeatTable) >> (slot & 31u)) & 1u)) {
                    int16_t off[4];
                    for (uint32_t i = 0; i < 4; ++i) off[i] = g_.S16(kPoseSeatTable + 10u + 8u * slot + 2u * i);
                    QuatMulHost(off, q, q);
                }
                for (int i = 0; i < 4; ++i) o[i] = q[i];
                if (m) {
                    if (slot != 0u) { o[0] = Neg16(q[0]); o[1] = Neg16(q[1]); }
                    else { o[0] = Neg16(q[2]); o[1] = Neg16(q[3]); o[2] = Neg16(q[0]); o[3] = Neg16(q[1]); }
                }
            } else if (type == 5u) {
                if (m) { o[0] = Neg16(q[0]); o[1] = Neg16(q[1]); }
            } else {
                if (m) { o[1] = Neg16(q[1]); o[2] = Neg16(q[2]); }
            }
            const uint32_t dst = B + qOff + 16u * slot;
            for (uint32_t i = 0; i < 4; ++i) g_.W16(dst + 2u * i, static_cast<uint16_t>(o[i]));
            ++k;
            if (g_.Faulted()) return;
        } while (k < g_.U8(cp + 15u));
    };
    const uint32_t oldClip = g_.U32(a + animf::kClip);                  // s7
    const bool m1 = root(g_.U8(op + 14u), B + 12u);
    parts(oldClip, m1, 24u, fromMask);
    AnimMachine machine(g_, *this);
    const uint32_t nc = machine.ClipSelect(a, newClip);                 // 0x8005CF64
    if (machine.Failed()) { refused_ = true; return 0; }
    const bool m2 = root(g_.U8(op + 26u), B + 18u);
    g_.W8(B, 20);
    parts(nc, m2, 32u, toMask);
    const uint32_t v = fromMask & toMask & g_.U32(a + animf::kMask);
    g_.W32(B + 8u, v);
    return v;
}

// RASHCDG 0x8005D36C TransitionBlend(a): t = (frame << 16) / op.a (+ (sub << 16) / (rate op.a) when
// interpolating); the root lerp; the model scale (model +0x0E bit 0, no parent); the play-flag-5
// zeroing; then per part in B.mask Slerp and QuatToMatrix into slot k.
uint32_t AnimPose::Blend(uint32_t a) {
    const uint32_t op = g_.U32(a + animf::kProgram) + 12u * g_.U32(a + animf::kPc);
    const int32_t opa = g_.S16(op + 4u);
    int32_t t = GuestDiv(S(g_.U32(a + animf::kFrame) << 16), opa);
    if (g_.U32(a + animf::kFlags) & 4u) {
        const int32_t d = Mul32(g_.S32(a + animf::kRate), opa);
        t = S(U(t) + U(GuestDiv(S(g_.U32(a + animf::kSub) << 16), d)));
    }
    const int32_t s1 = S(0x10000u - U(t));
    int16_t x = Lo16(S(U(FixMul(s1, g_.S16(a + 1776u))) + U(FixMul(t, g_.S16(a + 1782u)))));
    int16_t y = Lo16(S(U(FixMul(s1, g_.S16(a + 1778u))) + U(FixMul(t, g_.S16(a + 1784u)))));
    int16_t z = Lo16(S(U(FixMul(s1, g_.S16(a + 1780u))) + U(FixMul(t, g_.S16(a + 1786u)))));
    {
        const uint32_t owner = g_.U32(a);
        const uint32_t model = g_.U32(owner);
        if ((g_.U16(model + 14u) & 1u) && g_.U32(owner + 52u) == 0u) {   // 0x8005D49C
            x = Lo16(Mul32(S(U(static_cast<int32_t>(x)) << 12), g_.S32(model + 20u)) >> 24);
            y = Lo16(Mul32(S(U(static_cast<int32_t>(y)) << 12), g_.S32(g_.U32(g_.U32(a)) + 20u)) >> 24);
            z = Lo16(Mul32(S(U(static_cast<int32_t>(z)) << 12), g_.S32(g_.U32(g_.U32(a)) + 20u)) >> 24);
        }
    }
    if (g_.U8(op + 2u) & 0x20u) {                                       // 0x8005D51C
        z = 0;
        x = 0;
        const uint32_t st = g_.U16(g_.U32(a) + 544u);
        const uint32_t cat = g_.U16(kPoseStanceTable + 8u * st + 2u);
        if (cat != 8u && !((st - 69u) < 2u)) y = 0;
    }
    PoseRoot(g_.U32(a), x, y, z);
    if (g_.U8(a + 1764u) == 0u) return 0;
    uint32_t k = 0;
    do {
        if ((g_.U32(a + 1772u) >> (k & 31u)) & 1u) {
            int16_t q0[4], q1[4];
            const uint32_t from = a + 1788u + 16u * k;
            for (uint32_t i = 0; i < 4; ++i) {
                q0[i] = g_.S16(from + 2u * i);
                q1[i] = g_.S16(from + 8u + 2u * i);
            }
            int32_t w0 = 0, w1 = 0;
            if (!SlerpWeights(t, q0, q1, w0, w1)) return 0;
            int32_t v[4];
            for (uint32_t i = 0; i < 4; ++i) {
                const int32_t c = (Mul32(w0, q0[i]) >> 14) + (Mul32(w1, q1[i]) >> 14);
                v[i] = S(U(static_cast<int32_t>(Lo16(c))) << 2);
            }
            QuatToMatrixHost(g_.U32(g_.U32(a) + 4u) + 24u * k + 4u, v);
            if (refused_) return 0;
        }
        ++k;
        if (g_.Faulted()) return 0;
    } while (k < g_.U8(a + 1764u));
    return 0;
}

// RASHCDG 0x8005D2A8 ApplyFrame(a): op 3 -> TransitionBlend, else Sample(a, a + 0x30) then Pose;
// flags := lbu(flags) | 0x10 stored as a WORD; a play-flag bit 4 negates owner byte +0x223 once.
uint32_t AnimPose::ApplyFrame(uint32_t a) {
    const uint32_t op = g_.U32(a + animf::kProgram) + 12u * g_.U32(a + animf::kPc); // s1
    if (g_.U8(op + 1u) == 3u) {
        Blend(a);
    } else {
        Sample(a, a + animf::kTrack);
        if (Failed()) return 0;
        Pose(a);
    }
    if (Failed()) return 0;
    g_.W32(a + animf::kFlags, static_cast<uint32_t>(g_.U8(a + animf::kFlags)) | 0x10u);
    if (!(g_.U8(op + 2u) & 0x10u)) return 0;
    const uint32_t owner = g_.U32(a);
    g_.W8(owner + 547u, static_cast<uint8_t>(0u - g_.U8(owner + 547u)));
    const uint8_t v = static_cast<uint8_t>(g_.U8(op + 2u) & 0xEFu);
    g_.W8(op + 2u, v);
    return v;
}

} // namespace rr::sim
