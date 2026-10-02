#include "game/sim/camera.h"

#include <vector>

#include "game/sim/ai.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/ground.h"
#include "game/sim/vec.h"

// Every function below is transcribed from our own disassembly of RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c, base 0x8005B5E8) and, for Hermite, of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). Comments give the original's addresses. Arithmetic
// wraps in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
inline int32_t Neg(int32_t a) { return S(0u - U(a)); }
// `(s32)(s16) << 4` with `sll`: a 4096 = 1.0 direction promoted to 16.16.
inline int32_t Promote(int16_t v) { return S(U(static_cast<int32_t>(v)) << 4); }
// The `sra / addu / xor` absolute value: INT32_MIN stays negative.
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}
// `bgez x, +; addiu x, x, 2^n - 1; sra x, x, n`: division by 2^n rounding toward zero.
inline int32_t DivPow2(int32_t x, int n) { return S((x < 0) ? U(x) + ((1u << n) - 1u) : U(x)) >> n; }
// `srl t, x, 31; addu x, x, t; sra x, x, 1`.
inline int32_t Half(int32_t x) { return S(U(x) + (U(x) >> 31)) >> 1; }
// The sign-handling pattern the original wraps round every FixDiv (0x80089040 and eleven others):
// both operands made positive, the unsigned divide, the quotient negated when exactly one was not
// positive. `a == 0` takes the "not positive" arm, as the original's `blez` does.
inline int32_t SDiv(int32_t a, int32_t b) {
    if (a > 0) return (b > 0) ? S(FixDiv(U(a), U(b))) : Neg(S(FixDiv(U(a), U(Neg(b)))));
    return (b > 0) ? Neg(S(FixDiv(U(Neg(a)), U(b)))) : S(FixDiv(U(Neg(a)), U(Neg(b))));
}
// `divu` with the R3000's answer for a zero divisor.
inline uint32_t DivU(uint32_t n, uint32_t d) { return (d == 0u) ? 0xFFFFFFFFu : n / d; }
// radians 16.16 -> the 4096-per-turn table index: `x*163 >> 14 & 0xFFF` (0x80089D08).
inline uint32_t RadToIndex(int32_t r) { return ((U(r) * 163u) >> 14) & 0xFFFu; }
// the 4096-per-turn angle -> radians 16.16: `x*25736 >> 8` (0x80086544, 0x80088E90).
inline int32_t AngleToRad(int32_t a) { return S(U(a) * 25736u) >> 8; }

// The caller's frame of one call: which bytes THIS call has written, so that a read of a slot the
// original may read stale is declined rather than invented (camera.h, STACK).
class Frame {
public:
    Frame(GuestRam& m, uint32_t base, uint32_t bytes, bool& stale, uint32_t& staleAt)
        : m_(m), base_(base), written_(bytes, false), stale_(stale), staleAt_(staleAt) {}
    uint32_t At(uint32_t off) const { return base_ + off; }
    void W32(uint32_t off, uint32_t v) { m_.W32(base_ + off, v); Mark(off, 4); }
    void W16(uint32_t off, uint16_t v) { m_.W16(base_ + off, v); Mark(off, 2); }
    void Mark(uint32_t off, uint32_t n) {
        for (uint32_t i = 0; i < n && off + i < written_.size(); ++i) written_[off + i] = true;
    }
    int32_t S32(uint32_t off) { Check(off, 4); return m_.S32(base_ + off); }
    // The word as the guest stack holds it, without judging it; `Fresh` says whether this call wrote it.
    int32_t Peek32(uint32_t off) const { return m_.S32(base_ + off); }
    bool Fresh(uint32_t off, uint32_t n) const {
        for (uint32_t i = 0; i < n; ++i)
            if (off + i >= written_.size() || !written_[off + i]) return false;
        return true;
    }
    int16_t S16(uint32_t off) { Check(off, 2); return m_.S16(base_ + off); }
    uint16_t U16(uint32_t off) { Check(off, 2); return m_.U16(base_ + off); }

private:
    void Check(uint32_t off, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i)
            if (off + i >= written_.size() || !written_[off + i]) {
                if (!stale_) { stale_ = true; staleAt_ = base_ + off; }
                return;
            }
    }
    GuestRam& m_;
    uint32_t base_;
    std::vector<bool> written_;
    bool& stale_;
    uint32_t& staleAt_;
};

// The three `mult`s of a row of a 4096 = 1.0 matrix with a 16.16 vector, each product's middle word
// (`(lo >> 16) | (hi << 16)` = FixMul), summed with wrap: 0x800882C4, 0x80086600, 0x8008A7C8 ...
inline int32_t RowDot(GuestRam& m, uint32_t row, int32_t x, int32_t y, int32_t z) {
    const int32_t a = FixMul(x, Promote(m.S16(row + 0)));
    const int32_t b = FixMul(y, Promote(m.S16(row + 2)));
    const int32_t c = FixMul(z, Promote(m.S16(row + 4)));
    return Add(c, Add(b, a));
}

void ReadVec16(GuestRam& m, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = m.S16(a + 2u * k);
}
void ReadVec32(GuestRam& m, uint32_t a, int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = m.S32(a + 4u * k);
}
void WriteVec32(GuestRam& m, uint32_t a, const int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) m.W32(a + 4u * k, U(v[k]));
}

// SLUS 0x8002ECB8 Blend16To32 over guest addresses: out = FixMul(wa, a << 4) + FixMul(wb, b << 4).
void Blend16To32View(GuestRam& m, uint32_t a, uint32_t b, uint32_t out, int32_t wa, int32_t wb) {
    int16_t va[3], vb[3];
    ReadVec16(m, a, va);
    ReadVec16(m, b, vb);
    int32_t o[3];
    Blend16To32(va, vb, o, wa, wb);
    WriteVec32(m, out, o);
}

inline uint32_t CamRecord(uint32_t mode) { return kCameraFile + mode * kCameraRecordBytes; }

} // namespace

// ============================================================================ the small leaves

void CameraPort::SpringReset(uint32_t v) {                          // 0x80086AF8
    const uint32_t b = m_.U32(v + 0x238);
    const uint32_t speed = m_.U32(b + 0x1E0);
    m_.W32(v + 0x2E0, 0);
    m_.W32(v + 0x2D0, 0);
    m_.W32(v + 0x2DC, 0);
    m_.W32(v + 0x2C4, 0);
    m_.W32(v + 0x2D4, speed);
}

void CameraPort::ChaseSpring(uint32_t v, int32_t dt, int32_t x) {  // 0x80086B1C
    const uint32_t rec = CamRecord(m_.U32(v + 0x21C));
    const int32_t k = m_.S32(rec + 48);
    const int32_t c = m_.S32(rec + 44);
    const int32_t kx = FixMul(k, x);                                 // 0x80086B5C
    const int32_t cv = FixMul(m_.S32(v + 0x2D0), c);                 // 0x80086B6C
    const int32_t acc = Sub(Neg(kx), cv);
    m_.W32(v + 0x2C4, U(acc));
    const int32_t rate = Add(m_.S32(v + 0x2D0), FixMul(acc, dt));    // 0x80086B84
    m_.W32(v + 0x2D0, U(rate));
    m_.W32(v + 0x2DC, U(Add(m_.S32(v + 0x2DC), FixMul(rate, dt))));  // 0x80086B98
    const uint32_t f = m_.U32(v + 0x224);
    m_.W32(v + 0x224, (Abs(m_.S32(v + 0x2C4)) < 656) ? (f | 0x800000u) : (f & 0xFF7FFFFFu));
}

void CameraPort::ResetFlags(uint32_t v) {                           // 0x8008AAB0
    uint32_t f = m_.U32(v + 0x224);
    if (f & 0x8000u) {
        m_.W32(v + 0x224, f | 0x10006u);
        f = m_.U32(v + 0x224);
    }
    if (f & 0x40000u) m_.W32(v + 0x224, f | 6u);
    m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xFFF17FFFu);
}

void CameraPort::SetMode(uint32_t v, uint32_t mode) {               // 0x8008A998
    const bool same = (mode == m_.U32(v + 0x220));                  // a3
    if (mode < 4u && m_.U32(v + 0x21C) != mode) {
        const uint32_t a2 = same ? 2u : 6u;
        m_.W32(v + 0x220, mode);
        m_.W32(v + 0x224, (m_.U32(v + 0x224) & ~0x18u) | a2);
        const uint32_t t = m_.U32(v + 0x238);
        if ((m_.U16(t + 0xAC) >> 5) != 0) {                         // not a pool-0 bike
            const uint32_t idx = (m_.U16(v + 0xAC) == 0x9Eu) ? 1u : 0u;
            m_.W32(v + 0x238, m_.U32(kCameraPlayerBikes + 4u * idx));
        }
    }
    const uint32_t director = (mode < 7u) ? 0u : 1u;                // 0x8008AA2C
    const uint32_t f228 = m_.U32(v + 0x228);
    m_.W32(v + 0x304, director);
    if (f228 & 1u) m_.W32(v + 0x224, m_.U32(v + 0x224) | 0x100u);
    const uint32_t a2 = m_.U32(v + 0x228) & ~5u;
    m_.W32(v + 0x228, a2);
    const uint32_t old = m_.U32(v + 0x21C);
    m_.W32(v + 0x228, (old == 12u) ? (a2 | 0x80u) : (a2 & ~0x380u));
    m_.W32(v + 0x21C, mode);
    if (same && (m_.U32(v + 0x228) & 0x40u)) {
        m_.W32(v + 0x304, 1u);
        m_.W32(v + 0x228, m_.U32(v + 0x228) | 0x10u);
    }
}

void CameraPort::ToFrame(uint32_t v, uint32_t origin, uint32_t p, uint32_t out) {  // 0x80086584
    const int32_t d0 = Sub(m_.S32(p + 0), m_.S32(origin + 0));
    const int32_t d1 = Sub(m_.S32(p + 4), m_.S32(origin + 4));
    const int32_t d2 = Sub(m_.S32(p + 8), m_.S32(origin + 8));
    m_.W32(out + 0, U(RowDot(m_, v + 0x24C, d0, d1, d2)));
    m_.W32(out + 4, U(RowDot(m_, v + 0x252, d0, d1, d2)));
    m_.W32(out + 8, U(RowDot(m_, v + 0x258, d0, d1, d2)));
}

void CameraPort::ToPolar(uint32_t p, uint32_t) {                    // 0x800863EC
    int32_t a = RatAtan2(m_.S32(p + 0), m_.S32(p + 8), t_.atan);
    if (m_.S32(p + 8) < 0) a = (a >= 0) ? a - 2048 : a + 2048;
    const uint32_t i4 = (U(a) & 0xFFFu) * 4u;
    int32_t r;
    if (U(Abs(a)) - 513u < 1023u) {                                  // 0x8008643C: 513..1535, use the sine
        r = SDiv(m_.S32(p + 0), Promote(m_.S16(kCameraSinCos + i4)));
    } else {
        r = SDiv(m_.S32(p + 8), Promote(m_.S16(kCameraSinCos + i4 + 2u)));
    }
    m_.W32(p + 8, U(r));
    m_.W32(p + 0, U(AngleToRad(a)));
}

void CameraPort::AngleChase(uint32_t angle, int32_t target, int32_t dt, int32_t rate) {  // 0x80086C00
    const int32_t p0 = m_.S32(angle);
    const int32_t d0 = Sub(target, p0);
    const int32_t wrap = (d0 < -2048) ? -4096 : 0;
    m_.W32(angle, U((d0 >= 2049) ? Add(Add(p0, 4096), wrap) : Add(p0, wrap)));
    const int32_t p = m_.S32(angle);
    const int32_t d = Sub(target, p);
    int32_t step = 0;
    if (rate != 0) step = FixMul(FixMul(rate, S(U(d) << 16)), dt) >> 16;   // 0x80086C6C
    int32_t a0;
    if (Abs(step) >= 3) a0 = Add(step, p);
    else if (d > 0) a0 = Add(p, 2);
    else if (d < 0) a0 = Sub(p, 2);
    else a0 = p;
    const bool clamp = (a0 < target && !(p < target)) || (!(a0 < target) && !(target < p) && target < a0);
    m_.W32(angle, U(clamp ? target : a0));
}

void CameraPort::AngleSpring(uint32_t angle, int32_t target, int32_t dt, uint32_t rate, int32_t k,
                             int32_t c) {                           // 0x80086D54
    const int32_t p0 = m_.S32(angle);
    const int32_t d0 = Sub(target, p0);
    const int32_t wrap = (d0 < -2048) ? -4096 : 0;
    m_.W32(angle, U((d0 >= 2049) ? Add(Add(p0, 4096), wrap) : Add(p0, wrap)));
    const int32_t spring = FixMul(k, S(U(Sub(target, m_.S32(angle))) << 16));
    const int32_t damp = FixMul(c, m_.S32(rate));
    const int32_t r = Add(m_.S32(rate), Sub(spring, damp));
    m_.W32(rate, U(r));
    m_.W32(angle, U(Add(m_.S32(angle), FixMul(r, dt) >> 16)));
}

void CameraPort::Hermite(int32_t t, uint32_t h00, uint32_t h01, uint32_t h10, uint32_t h11) {  // SLUS 0x8002FA28
    const int32_t t2 = FixMul(t, t);
    const int32_t t3 = FixMul(t2, t);
    const int32_t a = Sub(t3, t2);
    m_.W32(h11, U(a));
    const int32_t b = Sub(a, t2);
    m_.W32(h10, U(b));
    const int32_t hi = m_.S32(h11);
    const int32_t c = Sub(Neg(b), hi);
    m_.W32(h01, U(c));
    m_.W32(h00, U(Sub(0x10000, c)));
    m_.W32(h10, U(Add(m_.S32(h10), t)));
}

void CameraPort::Zones(uint32_t v, uint32_t sp) {                   // 0x80088140
    const uint32_t F = sp - 32u;
    const uint32_t had = (m_.U32(v + 0x24) >> 8) & 1u;
    m_.W32(v + 0x158, U(Neg(m_.S32(v + 0x158))));
    RoadsideZones(m_, v, F);                                         // SLUS 0x8003A9D8
    const uint32_t w = m_.U32(v + 0x24);
    const uint32_t bit = ((w >> 8) & 1u) | had;
    m_.W32(v + 0x158, U(Neg(m_.S32(v + 0x158))));
    m_.W32(v + 0x24, (w & ~0x100u) | (bit << 8));
}

void CameraPort::Lead(uint32_t a, uint32_t b, int32_t k, uint32_t v, uint32_t out, uint32_t) {  // 0x800871A8
    int32_t d[3];
    for (uint32_t i = 0; i < 3; ++i) d[i] = Sub(m_.S32(a + 4u * i), m_.S32(b + 4u * i));
    int32_t len = SumSquares(d);                                     // 0x8002F0F4
    int32_t want = 0xE0000;                                          // a1, 14.0
    if (!(0x3FFEFFFF < len)) {
        len = S(U(SqrtGte(len, t_.sqrt)) << 2);
        want = FixMul(k, len);
    }
    const int32_t base = S(0x70000u + (((m_.U32(v + 0x24) >> 8) & 1u) ? 0xFFFD0000u : 0u));
    const int32_t o468 = m_.S32(v + 0x468);
    m_.W32(v + 0x468, U((o468 != 0) ? DivPow2(Add(S(U(o468) * 7u), base), 3) : base));
    const int32_t cap = m_.S32(v + 0x468);
    if (want < cap) {                                                // 0x80087298
        const int32_t cur = m_.S32(v + 0x464);
        const bool set = (cur == 0) || (!(0x1FFFF < want) && (m_.U32(v + 0x228) & 0x400u));
        if (set) {
            m_.W32(v + 0x464, U(want));
            m_.W32(v + 0x228, m_.U32(v + 0x228) | 0x400u);
        } else {
            m_.W32(v + 0x464, U(DivPow2(Add(cur, S(U(want) * 3u)), 2)));
            m_.W32(v + 0x228, m_.U32(v + 0x228) & ~0x400u);
        }
    } else {
        const int32_t cur = m_.S32(v + 0x464);
        m_.W32(v + 0x464, U((cur != 0) ? DivPow2(Add(S(U(cur) * 3u), cap), 2) : cap));
    }
    if (len < 6553) len = 6553;
    const int32_t q = SDiv(m_.S32(v + 0x464), len);
    int32_t sd[3];
    Scale32(q, d, sd);                                               // 0x8002E810
    for (uint32_t i = 0; i < 3; ++i) m_.W32(out + 4u * i, U(Add(m_.S32(b + 4u * i), sd[i])));
}

// ============================================================================ 0x8008676C

void CameraPort::Transition(uint32_t v, int32_t dt, uint32_t sp) {
    const uint32_t F = sp - 104u;
    if (m_.U32(v + 0x224) & 0x10u) {                                // start: snapshot the placement
        const int32_t c0 = m_.S32(v + 0x270), c1 = m_.S32(v + 0x274), c2 = m_.S32(v + 0x278);
        const int32_t e0 = m_.S32(v + 0x294), e1 = m_.S32(v + 0x298);
        const uint32_t f = m_.U32(v + 0x224);
        const int32_t e2 = m_.S32(v + 0x29C);
        m_.W32(v + 0x288, U(c0));
        m_.W32(v + 0x28C, U(c1));
        m_.W32(v + 0x290, U(c2));
        m_.W32(v + 0x2AC, U(e0));
        m_.W32(v + 0x2B0, U(e1));
        m_.W32(v + 0x2B4, U(e2));
        if ((f & 0x40u) && ((c2 ^ m_.S32(v + 0x284)) < 0)) {         // polar, radius changes sign
            m_.W32(v + 0x290, U(Neg(c2)));
            m_.W32(v + 0x288, U(Add(c0, (c0 > 0) ? S(0xFFFCDBC1u) : 0x3243F)));
        }
        m_.W32(v + 0x2B8, 0);
        m_.W32(v + 0x224, m_.U32(v + 0x224) & ~0x10u);
    }
    const int32_t t = Add(m_.S32(v + 0x2B8), dt);                   // 0x80086830
    const int32_t dur = m_.S32(v + 0x45C);
    m_.W32(v + 0x2B8, U(t));
    if (!(t < dur)) {                                                // done: land on the target
        const int32_t a = m_.S32(v + 0x27C), b = m_.S32(v + 0x280), c = m_.S32(v + 0x284);
        const int32_t d = m_.S32(v + 0x2A0), e = m_.S32(v + 0x2A4), g = m_.S32(v + 0x2A8);
        m_.W32(v + 0x270, U(a));
        m_.W32(v + 0x274, U(b));
        m_.W32(v + 0x278, U(c));
        m_.W32(v + 0x294, U(d));
        m_.W32(v + 0x298, U(e));
        m_.W32(v + 0x29C, U(g));
        m_.W32(v + 0x224, m_.U32(v + 0x224) & ~8u);
        m_.W32(v + 0x228, m_.U32(v + 0x228) & ~0x380u);
        return;
    }
    const int32_t s = SDiv(t, dur);
    Hermite(s, F + 56, F + 60, F + 64, F + 68);                      // 0x800868F4
    const int32_t h00 = m_.S32(F + 56), h01 = m_.S32(F + 60), h10 = m_.S32(F + 64), h11 = m_.S32(F + 68);
    int32_t d1[3], d2[3];
    for (uint32_t i = 0; i < 3; ++i) {
        d1[i] = Sub(m_.S32(v + 0x27C + 4u * i), m_.S32(v + 0x288 + 4u * i));
        d2[i] = Sub(m_.S32(v + 0x2A0 + 4u * i), m_.S32(v + 0x2AC + 4u * i));
    }
    if (m_.U32(v + 0x224) & 0x40u) {                                // the angle takes the short way
        const uint32_t g = m_.U32(v + 0x228);
        int32_t add = 0;
        if (g & 0x100u) {
            if (d1[0] < 0) add = 0x6487E;
        } else if (g & 0x200u) {
            if (d1[0] > 0) add = S(0xFFF9B782u);
        } else if (0x3243F < d1[0]) {
            add = S(0xFFF9B782u);
        } else if (d1[0] < S(0xFFFCDBC1u)) {
            add = 0x6487E;
        }
        d1[0] = Add(d1[0], add);
    }
    const int32_t dur2 = m_.S32(v + 0x45C);
    for (uint32_t i = 0; i < 3; ++i) {
        // the tangent terms are FixMul(0, h) - the port keeps them to keep the arithmetic literal
        int32_t s1 = Add(Add(FixMul(d1[i], h00), FixMul(0, h01)),
                         FixMul(dur2, Add(FixMul(0, h10), FixMul(0, h11))));
        m_.W32(v + 0x270 + 4u * i, U(Sub(m_.S32(v + 0x27C + 4u * i), s1)));
        s1 = Add(Add(FixMul(d2[i], h00), FixMul(0, h01)), FixMul(dur2, Add(FixMul(0, h10), FixMul(0, h11))));
        m_.W32(v + 0x294 + 4u * i, U(Sub(m_.S32(v + 0x2A0 + 4u * i), s1)));
    }
}

// ============================================================================ 0x80086E1C

void CameraPort::Orient(uint32_t v, uint32_t sp) {
    const uint32_t F = sp - 80u;
    (void)F;
    if (m_.U32(v + 0x304) == 0 && (m_.U32(v + 0x224) & 0x4000u)) { // between bike and rider
        const uint32_t b = m_.U32(v + 0x238);
        const int32_t bx = Sub(m_.S32(v + 0xB8), m_.S32(b + 0xB8));
        const uint32_t r = m_.U32(b + 0x354);
        const int32_t by = Sub(m_.S32(v + 0xBC), m_.S32(b + 0xBC));
        const int32_t bz = Sub(m_.S32(v + 0xC0), m_.S32(b + 0xC0));
        const int32_t bl = SqrtGte(Add(FixMul(bx, bx), FixMul(bz, bz)), t_.sqrt);
        const int32_t pitchBike = RatAtan2(by, S(U(bl) << 2), t_.atan);          // s3
        const int32_t rx = Sub(m_.S32(v + 0xB8), m_.S32(r + 0xB8));
        const int32_t ry = Sub(m_.S32(v + 0xBC), m_.S32(r + 0xBC));
        const int32_t rz = Sub(m_.S32(v + 0xC0), m_.S32(r + 0xC0));
        m_.W32(v + 0x2E8, U(RatAtan2(Neg(rx), Neg(rz), t_.atan)));             // 0x80086F0C
        const int32_t rl = SqrtGte(Add(FixMul(rx, rx), FixMul(rz, rz)), t_.sqrt);
        const int32_t pitchRider = RatAtan2(ry, S(U(rl) << 2), t_.atan);
        if (!(m_.U32(r + 0x228) & 0x80000u)) m_.W32(v + 0x2EC, U(Half(Add(pitchRider, pitchBike))));
        m_.W32(v + 0x224, m_.U32(v + 0x224) | 0x1000u);
    } else {                                                         // 0x80086F80: toward +0x22C
        const int32_t x = Sub(m_.S32(v + 0xB8), m_.S32(v + 0x22C));
        const int32_t y = Sub(m_.S32(v + 0xBC), m_.S32(v + 0x230));
        const int32_t z = Sub(m_.S32(v + 0xC0), m_.S32(v + 0x234));
        const int32_t yaw = RatAtan2(Neg(x), Neg(z), t_.atan);                // F52
        const int32_t l = SqrtGte(Add(FixMul(x, x), FixMul(z, z)), t_.sqrt);
        const int32_t pitch = RatAtan2(y, S(U(l) << 2), t_.atan);             // F48
        if (m_.U32(v + 0x224) & 0x1000u) {
            const int32_t a0 = m_.S32(v + 0x2EC);
            const int32_t dd = Sub(pitch, a0);
            if (dd >= 2049) m_.W32(v + 0x2EC, U(Add(a0, 4096)));
            else if (dd < -2048) m_.W32(v + 0x2EC, U(Add(a0, -4096)));
            const int32_t cur = m_.S32(v + 0x2EC);
            if (Abs(Sub(pitch, cur)) < 3) {
                m_.W32(v + 0x2EC, U(pitch));
                m_.W32(v + 0x224, m_.U32(v + 0x224) & ~0x1000u);
            } else {
                m_.W32(v + 0x2EC, U(DivPow2(Add(S(U(cur) * 3u), pitch), 2)));
            }
        } else {
            m_.W32(v + 0x2EC, U(pitch));
        }
        if (m_.U32(v + 0x224) & 0x2000u) {                          // 0x80087094
            const int32_t a0 = m_.S32(v + 0x2E8);
            const int32_t dd = Sub(yaw, a0);
            if (dd >= 2049) m_.W32(v + 0x2E8, U(Add(a0, 4096)));
            else if (dd < -2048) m_.W32(v + 0x2E8, U(Add(a0, -4096)));
            const int32_t cur = m_.S32(v + 0x2E8);
            if (Abs(Sub(yaw, cur)) < 23) {
                m_.W32(v + 0x2E8, U(yaw));
                m_.W32(v + 0x224, m_.U32(v + 0x224) & ~0x2000u);
            } else if (m_.U32(v + 0x224) & 8u) {
                m_.W32(v + 0x2E8, U(DivPow2(Add(S(U(cur) * 3u), yaw), 2)));
            } else {
                m_.W32(v + 0x2E8, U(Half(Add(cur, yaw))));
            }
        } else {
            m_.W32(v + 0x2E8, U(yaw));
        }
    }
    const int16_t ang[3] = {static_cast<int16_t>(0u - m_.U16(v + 0x2EC)),
                            static_cast<int16_t>(0u - m_.U16(v + 0x2E8)), 0};
    int16_t mat[9];
    RotMatrix(ang, mat, t_.sincos);                                  // 0x80087184 -> v + 0x1B0
    for (uint32_t i = 0; i < 9; ++i) m_.W16(v + 0x1B0 + 2u * i, static_cast<uint16_t>(mat[i]));
}

// ============================================================================ 0x80087420

void CameraPort::Aim(uint32_t v, int32_t dt, uint32_t sp) {
    const uint32_t F = sp - 160u;
    bool stale = false;
    uint32_t staleAt = 0;
    Frame fr(m_, F, 160, stale, staleAt);
    const uint32_t b = m_.U32(v + 0x238);                           // s0: the followed entity
    if ((m_.U16(b + 0xAC) >> 5) != 0) {                             // 0x80087468: not a pool-0 bike
        m_.W32(v + 0x23C, m_.U32(b + 0xB8));                          // 0x800880C8
        m_.W32(v + 0x240, m_.U32(m_.U32(v + 0x238) + 0xBC));
        const uint32_t t = m_.U32(v + 0x238);
        m_.W32(v + 0x244, m_.U32(t + 0xC0));
        for (uint32_t i = 0; i < 9; ++i)                             // SLUS 0x8003FA18(9, t + 0x1B0, v + 0x24C)
            m_.W16(v + 0x24C + 2u * i, m_.U16(t + 0x1B0 + 2u * i));
        m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xEFFFFFFFu);
        return;
    }
    const uint32_t r = m_.U32(b + 0x354);                           // s3: its rider
    uint32_t w96 = 0;
    if ((m_.U32(b + 0x238) & 0x400u) || (m_.U32(b + 0x234) & 0x8800u)) w96 = 1;
    uint32_t w88 = 0;
    uint32_t f = m_.U32(v + 0x224) & ~(w96 << 25);
    m_.W32(v + 0x224, f);
    const uint32_t w100 = (f >> 25) & 1u;
    if (m_.U32(b + 0x238) & 0x100u) {                                // 0x800874C4
        if ((m_.U32(r + 0x228) & 0x18u) || m_.U16(r + 0x220) == 0) w88 = 1;
    }
    if (w88) {                                                       // 0x80087508
        m_.W32(v + 0x26C, m_.U32(v + 0x1C8));
        f = m_.U32(v + 0x224) | 0x20000000u;
    } else {
        if (m_.U32(r + 0x25C) < 2u && (m_.U32(v + 0x224) & 0x20000000u)) {
            const uint32_t rec = CamRecord(m_.U32(v + 0x220));
            const uint32_t w0 = m_.U32(rec);
            m_.W32(v + 0x13C, 0);
            m_.W32(v + 0x1E4, 0);
            m_.W32(v + 0x1C8, w0);
        }
        f = m_.U32(v + 0x224) & 0xDFFFFFFFu;
    }
    m_.W32(v + 0x224, f);                                            // 0x80087588
    f = m_.U32(v + 0x224);
    const uint32_t s8 = (f >> 15) & 1u;
    const uint32_t w112 = (f >> 17) & 1u;
    const uint32_t w92 = (m_.U32(r + 0x25C) < 3u || w88 || s8) ? 1u : 0u;
    int32_t s5 = (m_.U32(v + 0x224) & 2u) ? 0x3E80000 : 0x20000;   // 1000.0 : 2.0 per second
    uint32_t s7 = r + 0x1C2;                                         // delay slot at 0x80087600
    uint32_t w104 = 0, w108 = 0;
    if (w92) {
        const bool other = (m_.U32(b + 0x238) & 0x1Fu) || (m_.U32(b + 0x234) & 0x3400u);
        s7 = b + (other ? 0x210u : 0x1C2u);
        w104 = 0;
        if (m_.U32(b + 0x238) & 0x400u) {
            int16_t hv[3], hm[3];
            ReadVec16(m_, b + 0x1C2, hv);
            ReadVec16(m_, b + 0x1BC, hm);
            w104 = U(DotLcm(hv, hm)) >> 31;                          // 0x80087658
        }
        w108 = 0;
        s5 = Neg(s5);
    } else {
        w104 = 0;
        w108 = w112;
    }
    s5 = Add(s5, S((0u - w100) & U(s5)));                             // 0x80087680: doubled when bit 25
    uint32_t s6 = 0;
    if (m_.U32(v + 0x224) & 0x10000000u) s6 = (m_.U32(b + 0x238) & 0x200u) ? 1u : 0u;
    if (m_.U32(v + 0x460) == 0) {                                    // 0x800876B8: the lead factor
        if (w96 || (m_.U32(r + 0x228) & 0x40000000u) || s8) {
            uint32_t k = 0xCCCC;                                     // 0.8
            if (!s6 && !w96) {
                k = 13107;                                           // 0.2
                if (!((m_.U32(v + 0x24) >> 8) & 1u)) {
                    const uint32_t rv = GuestRand(m_);               // 0x80087724
                    // `multu 0x35558AAB; mfhi; srl 13` is rv / 39321: k = 0.2 + (rv % 39321) / 65536
                    k = rv - static_cast<uint32_t>((static_cast<uint64_t>(rv) * 0x35558AABu) >> 45) * 39321u +
                        13107u;
                }
            }
            m_.W32(v + 0x460, k);
        }
    }
    uint32_t s2 = 0;                                                 // the bike-side look point
    uint32_t s4 = 0;
    // STALE BIKE POINT. With the lead on (+0x224 bit 28, set when +0x238 bit 9 =
    // s6) and the lead factor +0x460 already cleared by the previous frame (0x8008794C), nothing writes
    // sp+40..+51 before the blend reads it: the original reads what earlier calls left at that depth
    // (measured, rr-race: the heading writer 0x8007AC04's saves of the rider pass's s2 / s3 at sp+72 /
    // sp+76 and DormantDrive 0x80095724's save of s0 at sp+16 - registers of the rider pass, not state).
    bool pointStale = false;
    f = m_.U32(v + 0x224);
    if (f & 0x400000u) {                                             // 0x80087764: a fixed look point
        s2 = v + 0x324;
        if (f & 0x200000u) {
            m_.W32(v + 0x248, 0);
            m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xFFDFFFFFu);
        }
        s5 = 0x20000;
        if (m_.U32(v + 0x248) == 0x10000u) m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xFFBFFFFFu);
        m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xEFFFFFFFu);
    } else {                                                         // 0x800877D8
        s2 = F + 40;
        uint32_t nf;
        if (w96 || s6) {
            s4 = m_.U32(v + 0x460);
            nf = f | 0x10000000u;
        } else {
            if (s8) {
                s4 = m_.U32(v + 0x460);
            } else if (s7 == b + 0x1C2 && m_.U32(b + 0x2A4) != 0) { // the wheelie lift
                const uint32_t a = m_.U32(b + 0x2A4);
                const uint32_t i4 = ((a * 163u) >> 12) & 0x3FFCu;
                const int32_t lift = FixMul(Promote(m_.S16(kCameraSinCos + i4)), m_.S32(b + 0x134));
                int32_t base[3], out[3];
                int16_t dir[3];
                ReadVec32(m_, b + 0x1F8, base);
                ReadVec16(m_, b + 0x32E, dir);
                MulAdd(base, dir, lift, out);                        // 0x80087878
                for (uint32_t i = 0; i < 3; ++i) fr.W32(40 + 4 * i, U(out[i]));
            } else {
                for (uint32_t i = 0; i < 3; ++i) fr.W32(40 + 4 * i, m_.U32(b + 0x1F8 + 4 * i));
            }
            nf = m_.U32(v + 0x224) & 0xEFFFFFFFu;                   // 0x800878B0
        }
        m_.W32(v + 0x224, nf);                                       // 0x800878BC
        if (s4 != 0) {
            Lead(b + 0xB8, b + 0x1F8, S(s4), v, F + 40, F);          // 0x800878D4
            fr.Mark(40, 12);
        }
        const uint32_t kind = m_.U32(b + 0xB4);
        if (kind - 6u < 3u || kind - 15u < 3u) {                     // 0x800878DC: raise the point
            int32_t base[3], out[3];
            int16_t dir[3];
            if (!fr.Fresh(40, 12)) pointStale = true;                // raised from the stale words
            for (uint32_t i = 0; i < 3; ++i) base[i] = fr.Peek32(40 + 4 * i);
            ReadVec16(m_, b + 0x204, dir);
            MulAdd(base, dir, 18022, out);                           // 0x8008790C, 0.275
            for (uint32_t i = 0; i < 3; ++i) fr.W32(40 + 4 * i, U(out[i]));
        }
    }
    // 0x80087914: the rider-side look point s4
    if (m_.U32(r + 0x228) & 0x40000000u) {
        Lead(r + 0xB8, r + 0x1F8, m_.S32(v + 0x460), v, F + 24, F); // 0x8008793C
        fr.Mark(24, 12);
        s4 = F + 24;
    } else {
        s4 = r + 0xB8;
        if (!s8 && !w96) {
            m_.W32(v + 0x468, 0);
            m_.W32(v + 0x464, 0);
            m_.W32(v + 0x460, 0);
        }
    }
    // 0x80087970: the blend weight +0x248 toward the rider point, at s5 per second
    {
        const int32_t fw = Add(m_.S32(v + 0x248), FixMul(s5, dt));
        int32_t a0 = (fw < 0) ? 0 : fw;
        const int32_t over = Sub(0x10000, fw);
        a0 = Add(a0, (over < 0) ? over : 0);
        m_.W32(v + 0x248, U(a0));
    }
    f = m_.U32(v + 0x224);
    if ((f & 0x110000u) != 0x100000u) {                              // 0x800879B8
        if (f & 0x10000u) {
            m_.W32(v + 0x224, f & 0xFFFE7FFFu);
            m_.W32(v + 0x248, w92 ? 0u : 0x10000u);
        }
        int32_t pa[3], pb[3], out[3];
        if (s2 == F + 40 && !fr.Fresh(40, 12)) pointStale = true;
        for (uint32_t i = 0; i < 3; ++i) {
            pa[i] = (s2 == F + 40) ? fr.Peek32(40 + 4 * i) : m_.S32(s2 + 4 * i);
            pb[i] = (s4 == F + 24) ? fr.S32(24 + 4 * i) : m_.S32(s4 + 4 * i);
        }
        const int32_t w = m_.S32(v + 0x248);
        // The stale bike point (see pointStale) enters only as FixMul(1.0 - w, p): at w = 1.0 its term is
        // 0 whatever the words hold, and the blend is the console's exactly. Otherwise it is declined.
        if (pointStale && Sub(0x10000, w) != 0 && !stale) { stale = true; staleAt = F + 40; }
        Blend32(pa, pb, out, Sub(0x10000, w), w);                    // 0x80087A08 -> +0x23C
        WriteVec32(m_, v + 0x23C, out);
    }
    // 0x80087A10: which direction the aim follows
    uint32_t s4f = 0;
    if ((m_.U32(b + 0x238) & 0x400u) || ((m_.U32(r + 0x228) & 0x40000000u) && !w100)) s4f = 1;
    bool skipDir = false;
    if (!s8) {
        const uint32_t g = m_.U32(r + 0x228);
        if (w92 || (g & 0x800u) || ((uint32_t(m_.U16(r + 0x220)) - 72u < 2u) && (g & 0x1000000u)) || s4f)
            skipDir = true;
    }
    uint32_t dirAddr = s7;                                           // s7 at 0x80087E1C
    uint32_t sfl = s4f;                                              // s4 at 0x80087E1C
    if (!skipDir) {                                                  // 0x80087AA4
        const uint32_t pBase = (m_.U32(r + 0x228) & 0x40000000u) ? r + 0x1F8 : r + 0xB8;
        int32_t d[3];
        for (uint32_t i = 0; i < 3; ++i) d[i] = Sub(m_.S32(b + 0x1F8 + 4 * i), m_.S32(pBase + 4 * i));
        uint32_t near = 1;
        const int32_t lim = 0x5A8000;
        while (Abs(d[0]) > lim || Abs(d[1]) > lim || Abs(d[2]) > lim) {
            near = 0;
            for (int32_t& x : d) x >>= 1;
        }
        for (uint32_t i = 0; i < 3; ++i) fr.W32(56 + 4 * i, U(d[i]));
        const int32_t len = Length3(d, t_.sqrt);                     // 0x8002E548
        const uint32_t kind = m_.U16(r + 0x220);
        const uint32_t cat = m_.U16(kRiderKindTable + kind * 8u + 2u);
        const uint32_t s6b = (cat == 8u) ? 0u : ((kind - 69u < 2u) ? 0u : 1u);
        uint32_t s5b = 1;
        if (near && len < 655) {                                     // too close: use a heading
            uint32_t src;
            if (m_.U32(v + 0x224) & 0x8000000u) { src = b + 0x334; s5b = 0; }
            else if (s8) src = r + 0x210;
            else src = b + 0x210;
            for (uint32_t i = 0; i < 3; ++i) fr.W16(72 + 2 * i, m_.U16(src + 2 * i));
            for (uint32_t i = 0; i < 3; ++i) fr.W16(72 + 2 * i, static_cast<uint16_t>(0u - fr.U16(72 + 2 * i)));
        } else {                                                     // 0x80087CF0: the unit vector
            uint32_t inv;
            if (len >= 0) {
                inv = DivU(0x80000000u, U((len >> 1) + (Sub(len, 2) >> 31)));
            } else {
                const int32_t nl = Neg(len);
                inv = 0u - DivU(0x80000000u, U((nl >> 1) + (Sub(nl, 2) >> 31)));
            }
            int16_t u[3];
            for (uint32_t i = 0; i < 3; ++i) u[i] = static_cast<int16_t>(FixMul(S(inv), d[i]) >> 4);   // 0x8002EED8
            for (uint32_t i = 0; i < 3; ++i) fr.W16(72 + 2 * i, static_cast<uint16_t>(u[i]));
            if (m_.U32(v + 0x224) & 0x8000000u) {
                bool flip = true;
                if (!s6b) flip = false;
                else if (0xFFFF < len) {
                    int16_t bv[3];
                    ReadVec16(m_, b + 0x334, bv);
                    flip = DotLcm(bv, u) > 0;
                }
                if (flip) {
                    for (uint32_t i = 0; i < 3; ++i)
                        fr.W16(72 + 2 * i, static_cast<uint16_t>(0u - m_.U16(b + 0x334 + 2 * i)));
                }
                s5b = 0;
            }
        }
        dirAddr = F + 72;                                            // 0x80087DBC
        if (s8)
            for (uint32_t i = 0; i < 3; ++i) fr.W16(72 + 2 * i, static_cast<uint16_t>(0u - fr.U16(72 + 2 * i)));
        sfl = 0;
        if (s6b && s5b && !w112) m_.W32(v + 0x224, m_.U32(v + 0x224) | 0x4000u);
    }
    // 0x80087E1C: the aim angles
    auto dirS16 = [&](uint32_t i) -> int16_t {
        return (dirAddr == F + 72) ? fr.S16(72 + 2 * i) : m_.S16(dirAddr + 2 * i);
    };
    int32_t yaw, pitch;
    if (w104 || (Abs(dirS16(0)) < 409 && Abs(dirS16(2)) < 409)) {   // straight up: keep the angles
        yaw = m_.S32(v + 0x260);
        pitch = m_.S32(v + 0x264);
    } else {
        yaw = w108 ? m_.S32(v + 0x260) : RatAtan2(dirS16(0), dirS16(2), t_.atan);
        int32_t as = 0;
        if (!Asin(S(U(static_cast<int32_t>(dirS16(1))) << 4), t_.asin, as)) { Fail(0x80087EC4, "asin"); return; }
        pitch = sfl ? 0 : Neg(as);
    }
    const int32_t oldYaw = m_.S32(v + 0x260);                        // s4 at 0x80087ED8
    if ((m_.U32(v + 0x224) & 6u) == 6u) {                            // snap
        const uint32_t rec = CamRecord(m_.U32(v + 0x220));
        m_.W32(v + 0x260, U(yaw));
        m_.W32(v + 0x264, U(pitch));
        m_.W32(v + 0x268, 0);
        m_.W32(v + 0x26C, m_.U32(rec));
    } else {
        if (w88) {                                                   // 0x80087F2C
            const int32_t rate = m_.S32(v + 0x26C);
            if ((oldYaw >= yaw && rate > 0) || (oldYaw <= yaw && rate < 0)) m_.W32(v + 0x26C, 0);
            AngleSpring(v + 0x260, yaw, dt, v + 0x26C, 0x186A7, 30015);  // 0x80087F80
        } else {
            const uint32_t rec = CamRecord(m_.U32(v + 0x220));
            const int32_t k = m_.S32(rec);
            m_.W32(v + 0x26C, U(k));
            AngleChase(v + 0x260, yaw, dt, k);                       // 0x80087FBC
        }
        AngleSpring(v + 0x264, pitch, dt, v + 0x268, 0x186A7, 30015);    // 0x80087FE4
    }
    // 0x80087FEC: the measured yaw rate +0x1C8
    if (!(m_.U32(r + 0x25C) < 2u) && !w88) {
        m_.W32(v + 0x13C, U(Add(m_.S32(v + 0x13C), dt)));
        const int32_t acc = Add(m_.S32(v + 0x1E4), Sub(m_.S32(v + 0x260), oldYaw));
        m_.W32(v + 0x1E4, U(acc));
        const int32_t tt = m_.S32(v + 0x13C);
        if (0x10000 < tt) {
            m_.W32(v + 0x1C8, U(SDiv(S(U(acc) << 16), tt)));
            m_.W32(v + 0x13C, 0);
            m_.W32(v + 0x1E4, 0);
        }
    }
    const int16_t ang[3] = {static_cast<int16_t>(0u - m_.U16(v + 0x264)),
                            static_cast<int16_t>(0u - m_.U16(v + 0x260)), 0};
    int16_t mat[9];
    RotMatrix(ang, mat, t_.sincos);                                  // 0x800880B8 -> the aim frame +0x24C
    for (uint32_t i = 0; i < 9; ++i) m_.W16(v + 0x24C + 2u * i, static_cast<uint16_t>(mat[i]));
    if (stale) Fail(staleAt, "stale stack read");
}

// ============================================================================ 0x800881B4

namespace {

// The octagonal distance from a director shot point to `tgt` (0x800885E0 and 0x800889B8), returned
// as the original holds it, `d << 16`. `which` and `k` are the two signed bytes after the kind byte.
int32_t ShotPointDistance(GuestRam& m, int32_t which, int32_t k, uint32_t tgt) {
    const int32_t base = m.S16(kShotTable + 524u + 2u * U(which));
    const int32_t idx = (which < 2) ? Add(base, S(U(k) << 1)) : Add(base, k);
    const uint32_t pt = kShotTable + 532u + U(idx) * 6u;
    const int32_t dxw = Sub(S(U(static_cast<int32_t>(m.S16(pt + 0))) << 16), m.S32(tgt + 0xB8));
    const int32_t dzw = Sub(S(U(static_cast<int32_t>(m.S16(pt + 4))) << 16), m.S32(tgt + 0xC0));
    const uint32_t sx = U(dxw >> 31), sz = U(dzw >> 31);
    int32_t big = S((U(dxw >> 16) + sx) ^ sx);
    int32_t small = S((U(dzw >> 16) + sz) ^ sz);
    if (big < small) { const int32_t t = big; big = small; small = t; }
    const int32_t s = Add(small, small >> 1);
    const int32_t d = Add(Add(Sub(Sub(big, big >> 5), big >> 7), s >> 2), s >> 6);
    return S(U(d) << 16);
}

} // namespace

void CameraPort::ViewUpdate(uint32_t v, int32_t dt, uint32_t sp) {
    const uint32_t F = sp - 208u;                                    // `addiu sp,sp,-208`
    bool stale = false;
    uint32_t staleAt = 0;
    Frame fr(m_, F, 208, stale, staleAt);
    auto bail = [&]() { return Failed() || stale; };
    auto finish = [&]() { if (stale) Fail(staleAt, "stale stack read"); };

    // ---------------------------------------------------------------- the director (+0x304 != 0)
    if (m_.U32(v + 0x304) != 0) {                                    // 0x800881F0
        uint32_t s5 = 0;
        const uint32_t idx = m_.U32(v + 0x308);                      // a2
        const uint32_t mode = m_.U32(v + 0x21C);                     // a1
        const uint32_t tgt = m_.U32(v + 0x238);                      // s2, kept for the whole call
        const uint32_t s3a = kShotRecords + idx * 26u;
        const bool s1 = (mode - 7u) < 4u;                            // modes 7..10
        const int32_t a3 = m_.S16(tgt + 0x172);                      // s6
        bool cut = false;
        if (s1) {
            cut = true;                                              // s0 = 1 from 0x800881F4
        } else if (mode - 13u < 2u) {                                // modes 13, 14: 0x8008824C
            const int32_t d0 = Sub(m_.S32(tgt + 0xB8), m_.S32(v + 0xB8));
            const int32_t d1 = Sub(m_.S32(tgt + 0xBC), m_.S32(v + 0xBC));
            const int32_t d2 = Sub(m_.S32(tgt + 0xC0), m_.S32(v + 0xC0));
            fr.W32(24, U(d0)); fr.W32(28, U(d1)); fr.W32(32, U(d2));
            cut = 0x3C0000 < RowDot(m_, v + 0x258, d0, d1, d2);       // ahead by more than 60.0
        } else if (mode == 12u) {                                    // 0x80088328
            const uint32_t gs = m_.U32(kCameraGameState);
            uint32_t ready;
            if (m_.U8(gs + 4) == 44) {
                const int32_t until = m_.S32(v + 0x318);
                if (until <= 0 || until < m_.S32(v + 0x2BC)) {
                    if (idx == 5u) ready = (m_.U32(v + 0x228) >> 4) & 1u;
                    else if (S(idx) < 6) ready = (idx == 4u) ? (m_.U32(m_.U32(kCameraPlayer1Bike) + 0x1E0) == 0 ? 1u : 0u) : 0u;
                    else ready = (idx == 6u) ? (((m_.U32(m_.U32(kCameraPlayer1Bike) + 0x230) >> 27) ^ 1u) & 1u) : 0u;
                } else {
                    ready = 0;
                }
            } else {
                const int32_t until = m_.S32(v + 0x318);
                ready = (until != 0 && until < m_.S32(v + 0x2BC)) ? 1u : 0u;
            }
            const uint32_t p = m_.U32(v + 0x2F4);
            bool go = ready != 0;
            if (p != 0 && m_.S16(p) == 0) go = true;
            if (go) {                                                // 0x80088430
                const uint32_t gs2 = m_.U32(kCameraGameState);
                uint32_t leave = 1;
                if (m_.U8(gs2 + 4) == 44) {
                    leave = 0;
                    const uint32_t w = m_.U32(v + 0x308);
                    if (w == 4u) {
                        const uint32_t pb = m_.U32(kCameraPlayer1Bike);
                        const uint32_t nt = m_.U32(m_.U32(pb + 0x358) + 0x354);
                        m_.W32(v + 0x2F0, 0x800CE5A8u);
                        m_.W32(v + 0x2F4, 0);
                        m_.W32(v + 0x238, nt);
                    } else if (w == 5u) {
                        const uint32_t nt = m_.U32(kCameraPlayer1Bike);
                        const uint32_t e = m_.U32(0x8005B330u);
                        m_.W32(v + 0x238, nt);
                        m_.W32(v + 0x2F0, e + 0xB8u);
                        m_.W32(v + 0x2F4, e + 0x140u);
                    } else {
                        leave = 1;
                        m_.W32(v + 0x224, m_.U32(v + 0x224) | 4u);
                        m_.W32(v + 0x228, m_.U32(v + 0x228) & ~2u);
                    }
                }
                if (leave) {                                         // 0x800884E8
                    SetMode(v, m_.U32(v + 0x220));
                    SpringReset(v);
                    m_.W32(v + 0x228, m_.U32(v + 0x228) & ~0x10u);
                    cut = false;
                } else {
                    m_.W32(v + 0x228, m_.U32(v + 0x228) | 2u);
                    cut = true;
                }
            }
        } else {                                                     // 0x80088524
            const uint32_t g = m_.U32(v + 0x228);
            if (g & 7u) {
                const uint32_t gs = m_.U32(kCameraGameState);
                if (m_.U8(gs + 4) != 44 || m_.U8(gs + 57) < 4) cut = ((g >> 5) & 1u) != 0;
            } else {
                bool c = S(m_.U16(s3a)) < a3;
                if (c) c = S(idx) < Sub(m_.S32(kShotTable), 1);
                else if (S(idx) > 0) c = a3 < S(m_.U16(kShotRecords + (idx - 1u) * 26u));
                if (c) {
                    cut = true;
                } else if (m_.S8(s3a + 4) == 2) {
                    const int32_t d = ShotPointDistance(m_, m_.S8(s3a + 5), m_.S8(s3a + 6), tgt);
                    s5 = 1;
                    cut = !(0x3BFFFF < d);                           // within 60.0
                }
            }
        }
        if (bail()) { finish(); return; }
        // 0x800886B8
        if (cut || (m_.U32(v + 0x228) & 0x10u)) {                   // 0x800886D4: the cut
            const uint32_t g = m_.U32(v + 0x228);
            if (g & 1u) {                                            // back to the chase camera
                const uint32_t m220 = m_.U32(v + 0x220);
                m_.W32(v + 0x304, 0);
                m_.W32(v + 0x21C, m220);
                const uint32_t f = m_.U32(v + 0x224);
                m_.W32(v + 0x228, g & ~1u);
                m_.W32(v + 0x224, f | 0x102u);
            } else if (g & 0xCu) {
                m_.W32(v + 0x228, (g & ~4u) | 8u);
                m_.W32(v + 0x21C, 6);
            } else {
                m_.W32(v + 0x228, g & ~0x10u);
                if (g & 2u) {                                        // 0x8008874C
                    m_.W32(v + 0x308, m_.U32(v + 0x308) + 1u);
                    const uint32_t gs = m_.U32(kCameraGameState);
                    if (m_.U8(gs + 4) & 1u) {
                        m_.W32(v + 0x228, (m_.U32(v + 0x228) & ~2u) | 1u);
                        if (!seams_.RaceOverSignal(0)) { Fail(0x80088780, "SLUS 0x80018C1C refused"); return; }
                        const uint32_t t = m_.U32(v + 0x238);
                        m_.W32(v + 0x2F0, t + 504u);
                        m_.W32(v + 0x2F4, t + 320u);
                        m_.W32(v + 0x238, m_.U32((v == kCameraViews) ? kCameraPlayer1Bike : kCameraPlayer2Bike));
                    }
                } else if (s1) {                                     // 0x800887D8
                    const uint32_t gs = m_.U32(kCameraGameState);
                    if ((m_.U8(gs + 4) & 1u) && m_.U32(v + 0x21C) == 7u) {
                        const uint32_t k = (m_.U32(0x8005B1B0u) == 1u) ? 6u : 4u;
                        const uint32_t g2 = m_.U32(v + 0x228);
                        m_.W32(v + 0x308, k);
                        m_.W32(v + 0x228, g2 | 2u);
                    } else {
                        m_.W32(v + 0x308, m_.U32(v + 0x21C) - 7u);
                        const uint32_t md = m_.U32(v + 0x21C);
                        const uint32_t g2 = m_.U32(v + 0x228);
                        m_.W32(v + 0x228, (md == 7u) ? (g2 | 1u) : (g2 | 4u));
                    }
                } else if (S(m_.U16(s3a)) < a3) {                    // 0x80088870: forward through the shots
                    for (uint32_t guard = 0;; ++guard) {
                        if (guard > (1u << 20)) { Fail(0x80088870, "shot walk"); return; }
                        const int32_t cnt = m_.S32(kShotTable);
                        const int32_t cur = m_.S32(v + 0x308);
                        const int32_t adv = (cur < Sub(cnt, 1)) ? 1 : 0;
                        const int32_t nv = Add(cur, adv);
                        m_.W32(v + 0x308, U(nv));
                        if (!adv) break;
                        if (!(S(m_.U16(kShotRecords + U(nv) * 26u)) < a3)) break;
                        if (bail()) break;
                    }
                } else {                                             // 0x800888C0: backward
                    const int32_t cur = m_.S32(v + 0x308);
                    if (cur > 0 && a3 < S(m_.U16(kShotRecords + U(cur - 1) * 26u))) {
                        for (uint32_t guard = 0;; ++guard) {
                            if (guard > (1u << 20)) { Fail(0x80088904, "shot walk"); return; }
                            const int32_t o = m_.S32(v + 0x308);
                            m_.W32(v + 0x308, U(Sub(o, 1)));
                            if (Sub(o, 1) <= 0) break;
                            if (!(a3 < S(m_.U16(kShotRecords + U(Sub(o, 2)) * 26u)))) break;
                            if (bail()) break;
                        }
                    }
                }
                // 0x80088948: pick the next shot
                const uint32_t cur = m_.U32(v + 0x308);
                const uint32_t s3 = kShotRecords + cur * 26u;
                m_.W32(v + 0x318, (m_.U32(v + 0x228) & 7u) ? (U(m_.U16(s3)) << 16) : 0u);
                uint32_t s1p = s3 + 3u;
                int32_t s0 = 0;
                if (m_.U8(s3 + 2) >= 2) {
                    bool direct = false;
                    if (m_.S8(s1p + 1) == 2) {
                        const int32_t d = ShotPointDistance(m_, m_.S8(s1p + 2), m_.S8(s1p + 3), tgt);
                        s5 = 1;
                        if (m_.S8(s1p + 1) == 2 && !(0x3BFFFF < d)) direct = true;
                    }
                    if (!direct) {                                   // 0x80088AA0: a random alternative
                        const uint32_t n = m_.U8(s3 + 2) - s5;
                        const uint32_t span = n + 1u;
                        if (n == 1u) {
                            s0 = 1;
                        } else {
                            for (uint32_t guard = 0;; ++guard) {
                                if (guard > (1u << 20)) { Fail(0x80088AC4, "shot pick"); return; }
                                uint32_t rc = 0;
                                if (!seams_.GetRCnt(0xF2000002u, rc)) { Fail(0x80088AC4, "GetRCnt refused"); return; }
                                s0 = Add(S(s5), S(span * (rc & 0xFFu)) >> 8);
                                if (s0 != m_.S32(v + 0x310)) break;
                            }
                        }
                        if (s0 > 0) {                                // the s0-th alternative, marked by 7
                            int32_t cnt = 0;
                            s1p += 2u;
                            for (uint32_t guard = 0;; ++guard) {
                                if (guard > (1u << 20) || bail()) { Fail(0x80088AFC, "shot scan"); return; }
                                if (m_.S8(s1p) == 7) ++cnt;
                                const bool more = cnt < s0;
                                s1p += 2u;
                                if (!more) break;
                            }
                            s1p -= 2u;
                        }
                    }
                }
                if (!seams_.ShotSetup(m_, v, s1p)) { Fail(0x80088B24, "RASHCDG 0x800853E4 refused"); return; }
                m_.W32(v + 0x30C, U(S(s1p - 3u - s3) >> 1));
                m_.W32(v + 0x310, U(s0));
                m_.W32(v + 0x2BC, 0);
            }
        }
        m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xFF9FFFFFu);         // 0x80088B48
        if (bail()) { finish(); return; }
    }

    // ---------------------------------------------------------------- 0x80088B58: every camera
    {
        const int32_t clock = m_.S32(v + 0x2BC);
        m_.W32(v + 0x2BC, U((0x270FFFFF < clock) ? clock : Add(clock, dt)));
    }
    const uint32_t mode = m_.U32(v + 0x21C);
    // The polar placement (angle at `angleOff`, radius at `radiusOff`) into the frame's x/z slots
    // (0x80089590, 0x80089CF8, 0x80089E88).
    auto polarToFrame = [&](uint32_t angleOff, uint32_t radiusOff) {
        const int32_t a = m_.S32(v + angleOff);
        if (a != 0) {
            const uint32_t i = RadToIndex(a);
            fr.W32(40, U(FixMul(m_.S32(v + radiusOff), Promote(m_.S16(kCameraSinCos + 4u * i)))));
            fr.W32(48, U(FixMul(m_.S32(v + radiusOff), Promote(m_.S16(kCameraSinCos + 4u * i + 2u)))));
        } else {
            fr.W32(40, 0);
            fr.W32(48, m_.U32(v + radiusOff));
        }
    };
    auto copyTargetToCurrent = [&]() {
        const uint32_t a = m_.U32(v + 0x27C), b = m_.U32(v + 0x280), c = m_.U32(v + 0x284);
        const uint32_t d = m_.U32(v + 0x2A0), e = m_.U32(v + 0x2A4), g = m_.U32(v + 0x2A8);
        m_.W32(v + 0x270, a); m_.W32(v + 0x274, b); m_.W32(v + 0x278, c);
        m_.W32(v + 0x294, d); m_.W32(v + 0x298, e); m_.W32(v + 0x29C, g);
    };
    auto copyKnotsToCurrent = [&]() {
        const uint32_t a = m_.U32(v + 0x33C), b = m_.U32(v + 0x354), c = m_.U32(v + 0x36C);
        const uint32_t d = m_.U32(v + 0x384), e = m_.U32(v + 0x39C), g = m_.U32(v + 0x3B4);
        m_.W32(v + 0x270, a); m_.W32(v + 0x274, b); m_.W32(v + 0x278, c);
        m_.W32(v + 0x294, d); m_.W32(v + 0x298, e); m_.W32(v + 0x29C, g);
    };
    auto copyCurrentToKnots = [&]() {
        const uint32_t a = m_.U32(v + 0x270), b = m_.U32(v + 0x274), c = m_.U32(v + 0x278);
        const uint32_t d = m_.U32(v + 0x294), e = m_.U32(v + 0x298);
        const uint32_t f = m_.U32(v + 0x224) | 0x3018u;
        const uint32_t g = m_.U32(v + 0x29C);
        m_.W32(v + 0x33C, a); m_.W32(v + 0x354, b); m_.W32(v + 0x36C, c);
        m_.W32(v + 0x384, d); m_.W32(v + 0x39C, e); m_.W32(v + 0x3B4, g);
        return f;
    };
    // The six-track spline of the scripted cameras (0x80089360 and 0x8008A2DC): segment advance,
    // then the Hermite blend of every track into +0x270..+0x278 and +0x294..+0x29C.
    auto splineSegment = [&](uint32_t hb) {
        if (!(m_.S32(v + 0x320) >= 2)) return;
        int32_t s2 = m_.S32(v + 0x31C);
        int32_t seg = m_.S32(v + 0x324 + 4u * U(s2));                // s6
        int32_t a1 = Sub(m_.S32(v + 0x2BC), m_.S32(v + 0x314));
        if (!(a1 < seg)) {                                           // next segment
            const int32_t nx = Add(s2, 1);
            const int32_t n = m_.S32(v + 0x320);
            m_.W32(v + 0x31C, U(nx));
            if (nx < Sub(n, 1)) {
                s2 = m_.S32(v + 0x31C);
                const int32_t start = Add(m_.S32(v + 0x314), seg);
                a1 = Sub(m_.S32(v + 0x2BC), start);
                m_.W32(v + 0x314, U(start));
                seg = m_.S32(v + 0x324 + 4u * U(s2));
            } else {
                a1 = seg;
                m_.W32(v + 0x31C, U(Sub(n, 2)));
                if (m_.S32(v + 0x318) < m_.S32(v + 0x2BC)) m_.W32(v + 0x228, m_.U32(v + 0x228) | 0x20u);
            }
        }
        const int32_t t = SDiv(a1, seg);
        Hermite(t, F + hb, F + hb + 4, F + hb + 8, F + hb + 12);     // 0x8002FA28
        fr.Mark(hb, 16);
        const int32_t h00 = fr.S32(hb), h01 = fr.S32(hb + 4), h10 = fr.S32(hb + 8), h11 = fr.S32(hb + 12);
        const uint32_t k0 = 4u * U(s2), k1 = 4u * U(Add(s2, 1));
        for (uint32_t c = 0; c < 3; ++c) {
            for (uint32_t half = 0; half < 2; ++half) {
                const uint32_t track = 24u * c + 72u * half;
                const int32_t p0 = FixMul(m_.S32(v + 0x33C + track + k0), h00);
                const int32_t p1 = FixMul(m_.S32(v + 0x33C + track + k1), h01);
                const int32_t m0 = FixMul(m_.S32(v + 0x3CC + track + k0), h10);
                const int32_t m1 = FixMul(m_.S32(v + 0x3CC + track + k1), h11);
                const int32_t val = Add(Add(p0, p1), FixMul(seg, Add(m0, m1)));
                m_.W32(v + (half ? 0x294u : 0x270u) + 4u * c, U(val));
            }
        }
    };

    if (mode < 4u || mode - 7u < 6u) {                               // 0x80088BA0
        if (m_.U32(v + 0x304) == 0) {
            // ======================================================== 0x80089628: the chase camera
            const uint32_t bike = m_.U32(v + 0x238);                 // s5
            const uint32_t rider = m_.U32(bike + 0x354);             // s0
            if ((m_.U32(v + 0x224) & 0x48000u) && !(m_.U32(rider + 0x25C) < 3u) &&
                !(m_.U32(rider + 0x228) & 0x80000u)) {
                const int32_t dx = Sub(m_.S32(rider + 0xB8), m_.S32(v + 0xB8));
                const int32_t dy = Sub(m_.S32(rider + 0xBC), m_.S32(v + 0xBC));
                const int32_t dz = Sub(m_.S32(rider + 0xC0), m_.S32(v + 0xC0));
                if (!(ApproxLen3(dx >> 16, dy >> 16, dz >> 16) < 61)) {     // 0x8008969C
                    bool reset = true;
                    if (m_.U32(v + 0x224) & 0x40000u) {
                        fr.W32(56, U(dx)); fr.W32(60, U(dy)); fr.W32(64, U(dz));
                        reset = 0 < RowDot(m_, v + 0x1BC, dx, dy, dz);
                    }
                    if (reset) ResetFlags(v);                        // 0x8008979C
                }
            }
            // 0x800897A4: the targets, from CAMERA.CA record +0x21C
            const uint32_t md = m_.U32(v + 0x21C);
            const uint32_t f0 = m_.U32(v + 0x224);
            m_.W32(v + 0x2A0, 0);
            m_.W32(v + 0x27C, 0);
            const uint32_t rec = CamRecord(md);                      // s7
            const uint32_t alt = f0 & 1u;                             // s6: the record's second column
            if ((f0 & 0x800000u) && (m_.U32(bike + 0x184) & 1u) && 0x23C36 < m_.S32(bike + 0x1E0)) {
                uint32_t rc = 0;                                     // 0x80089808: the off-road shake
                if (!seams_.GetRCnt(0xF2000002u, rc)) { Fail(0x80089808, "GetRCnt refused"); return; }
                const uint32_t v1 = (rc & 0xFFu) * 257u;
                const int32_t r8 = S((v1 + (v1 << 16)) >> 24);
                const int32_t speed = m_.S32(bike + 0x1E0);
                const int32_t sp60 = (speed > 0) ? S(FixDiv(U(speed), 0x3C0000u)) : Neg(S(FixDiv(U(Neg(speed)), 0x3C0000u)));
                const int32_t x0 = FixMul(r8, FixMul(sp60, m_.S32(CamRecord(m_.U32(v + 0x21C)) + 52)));
                const int32_t x1 = FixMul(r8, FixMul(sp60, m_.S32(CamRecord(m_.U32(v + 0x21C)) + 52)));
                const int32_t x2 = FixMul(r8, FixMul(sp60, m_.S32(CamRecord(m_.U32(v + 0x21C)) + 52)));
                const int32_t shake = S((U(x0 >> 31) + U(x1)) ^ U(x2 >> 31));
                m_.W32(v + 0x2A4, U(shake));
                m_.W32(v + 0x280, U(Add(m_.S32(rec + 12 + 4 * alt), shake)));
            } else {                                                 // 0x80089914: the distance spring
                const int32_t want = (m_.U32(bike + 0x234) & 0x800u) ? m_.S32(v + 0x2E4) : m_.S32(v + 0x2DC);
                ChaseSpring(v, dt, want);
                m_.W32(v + 0x2A4, 0);
                m_.W32(v + 0x280, U(Sub(m_.S32(rec + 12 + 4 * alt), m_.S32(v + 0x2DC))));
            }
            if (!(m_.U32(v + 0x224) & 1u)) {                         // 0x80089974: the speed follower
                int32_t c, k;
                const uint32_t r2 = CamRecord(m_.U32(v + 0x21C));
                if (m_.S32(bike + 0x1E4) > 0) { c = m_.S32(r2 + 32); k = m_.S32(r2 + 28); }
                else { c = m_.S32(r2 + 40); k = m_.S32(r2 + 36); }
                const int32_t lagv = Sub(m_.S32(v + 0x2D4), m_.S32(bike + 0x1E0));
                const int32_t a = FixMul(c, m_.S32(v + 0x2E0));
                const int32_t b = FixMul(lagv, k);
                m_.W32(v + 0x2C8, U(Sub(Neg(a), b)));
                if (m_.S32(v + 0x2E0) > 0)
                    m_.W32(v + 0x2C8, U(Sub(m_.S32(v + 0x2C8), FixMul(m_.S32(v + 0x2E0), S(U(c) << 1)))));
                const int32_t f2d4 = Add(m_.S32(v + 0x2D4), FixMul(m_.S32(v + 0x2C8), dt));
                m_.W32(v + 0x2D4, U(f2d4));
                const int32_t a1 = Add(m_.S32(v + 0x2E0), FixMul(Sub(f2d4, m_.S32(bike + 0x1E0)), dt));
                int32_t a0 = Add(a1, S(U(Add(a1, 0x640000) >> 31) & U(Sub(S(0xFF9C0000u), a1))));
                const int32_t hi = Sub(0xCCCC, a1);
                a0 = Add(a0, S(U(hi >> 31) & U(hi)));
                m_.W32(v + 0x2E0, U(a0));
                m_.W32(v + 0x284, U(Add(m_.S32(rec + 4 + 4 * alt), a0)));
            } else {
                m_.W32(v + 0x284, m_.U32(rec + 4 + 4 * alt));
            }
            m_.W32(v + 0x2A8, m_.U32(rec + 20 + 4 * alt));
            // 0x80089AB0: (re)initialise the current placement
            const uint32_t f1 = m_.U32(v + 0x224);
            if (f1 & 2u) {
                uint32_t nf;
                if (f1 & 4u) {                                       // snap to the target
                    const uint32_t a = m_.U32(v + 0x27C), b = m_.U32(v + 0x280), c = m_.U32(v + 0x284);
                    const uint32_t d = m_.U32(v + 0x2A0), e = m_.U32(v + 0x2A4), g = m_.U32(v + 0x2A8);
                    m_.W32(v + 0x270, a);
                    const uint32_t ff = m_.U32(v + 0x224);
                    m_.W32(v + 0x274, b); m_.W32(v + 0x278, c);
                    m_.W32(v + 0x294, d); m_.W32(v + 0x298, e); m_.W32(v + 0x29C, g);
                    nf = (ff & 0xFFFFEFA7u) | 0x20u;
                } else {
                    const uint32_t g = m_.U32(v + 0x228);
                    if (g & 0x80u) {                                 // 0x80089B30: from a director shot
                        const uint32_t g2 = g & ~0x300u;
                        const int32_t a1 = m_.S32(v + 0x270);
                        m_.W32(v + 0x228, g2);
                        uint32_t side;
                        if (0x1921E < Abs(a1)) side = (m_.S32(v + 0x2F8) < a1) ? 0x100u : 0x200u;
                        else side = (a1 > 0) ? 0x200u : 0x100u;
                        m_.W32(v + 0x228, g2 | side);
                        const int32_t d0 = Sub(m_.S32(m_.U32(v + 0x2F0) + 0), m_.S32(v + 0x23C));
                        fr.W32(56, U(d0));
                        const int32_t d1 = Sub(m_.S32(m_.U32(v + 0x2F0) + 4), m_.S32(v + 0x240));
                        fr.W32(60, U(d1));
                        const int32_t d2 = Sub(m_.S32(m_.U32(v + 0x2F0) + 8), m_.S32(v + 0x244));
                        fr.W32(64, U(d2));
                        m_.W32(v + 0x294, 0);
                        m_.W32(v + 0x29C, U(ApproxLen3(fr.S16(58), fr.S16(62), fr.S16(66)) << 16));
                    } else if (!(f1 & 0x40u)) {                      // 0x80089BFC: from where it is
                        ToFrame(v, v + 0x23C, v + 0xB8, v + 0x270);
                        ToFrame(v, v + 0x23C, v + 0x22C, v + 0x294);
                        ToPolar(v + 0x270, F);
                        m_.W32(v + 0x224, (m_.U32(v + 0x224) & ~0x20u) | 0x40u);
                    }
                    nf = m_.U32(v + 0x224) | 0x3018u;
                }
                m_.W32(v + 0x224, nf);                               // 0x80089C50
                m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xFEFFFFFFu);
            }
            // 0x80089C68
            const uint32_t f2 = m_.U32(v + 0x224);
            if (f2 & 8u) {
                if (f2 & 0x40u) m_.W32(v + 0x27C, 0);
                Transition(v, dt, F);                                // 0x80089C8C
            } else {
                copyTargetToCurrent();
            }
            if (bail()) { finish(); return; }
            if (m_.U32(v + 0x224) & 0x20u) {                         // 0x80089CCC: already Cartesian
                fr.W32(40, m_.U32(v + 0x270));
                fr.W32(48, m_.U32(v + 0x278));
            } else {
                polarToFrame(0x270, 0x278);
                const uint32_t f3 = m_.U32(v + 0x224);
                if (!(f3 & 8u)) m_.W32(v + 0x224, (f3 & ~0x40u) | 0x20u);
            }
            Aim(v, dt, F);                                           // 0x80089DA4
        } else {
            fr.W32(128, F + 40);                                     // `sw t8,128(sp)`, t8 = sp + 40
            if (mode == 12u) {
                // ==================================================== 0x80088BC0: the scripted pan
                const uint32_t f1 = m_.U32(v + 0x224);
                if (f1 & 2u) {
                    uint32_t nf;
                    if (f1 & 4u) {
                        copyKnotsToCurrent();
                        nf = m_.U32(v + 0x224) & 0xFFFFEFE7u;
                    } else {
                        ToFrame(v, v + 0x23C, v + 0x22C, v + 0x294);
                        if (!(m_.U32(v + 0x224) & 0x40u)) {
                            ToFrame(v, v + 0x23C, v + 0xB8, v + 0x270);
                            ToPolar(v + 0x270, F);
                        }
                        nf = m_.U32(v + 0x224) | 0x3018u;
                    }
                    m_.W32(v + 0x224, nf);
                    m_.W32(v + 0x224, (m_.U32(v + 0x224) & ~0x20u) | 0x40u);
                    if (!(m_.S32(v + 0x320) < 3)) {
                        for (int32_t k = 0; k < Sub(m_.S32(v + 0x320), 1); ++k)
                            fr.W32(56 + 4u * U(k), U(Sub(m_.S32(v + 0x33C + 4u * U(k + 1)), m_.S32(v + 0x33C + 4u * U(k)))));
                        if (!seams_.SplineSlopes(m_, F + 56, v + 0x354, v + 0x3E4, Sub(m_.S32(v + 0x320), 1)) ||
                            !seams_.SplineSlopes(m_, F + 56, v + 0x36C, v + 0x3FC, Sub(m_.S32(v + 0x320), 1))) {
                            Fail(0x80088CEC, "SLUS 0x8002F634 refused");
                            return;
                        }
                    }
                    m_.W32(v + 0x228, m_.U32(v + 0x228) & ~0x20u);
                }
                Aim(v, dt, F);                                       // 0x80088D20
                if (bail()) { finish(); return; }
                int32_t ang = 0;
                if (m_.U32(v + 0x2F0) != 0) {                        // the bearing of the shot point
                    const int32_t d0 = Sub(m_.S32(m_.U32(v + 0x2F0) + 0), m_.S32(v + 0x23C));
                    fr.W32(56, U(d0));
                    const int32_t d1 = Sub(m_.S32(m_.U32(v + 0x2F0) + 4), m_.S32(v + 0x240));
                    fr.W32(60, U(d1));
                    const int32_t d2 = Sub(m_.S32(m_.U32(v + 0x2F0) + 8), m_.S32(v + 0x244));
                    fr.W32(64, U(d2));
                    const int32_t x = RowDot(m_, v + 0x24C, d0, d1, d2);
                    const int32_t z = RowDot(m_, v + 0x258, d0, d1, d2);
                    ang = AngleToRad(RatAtan2(x, z, t_.atan));
                }
                const uint32_t s7 = (m_.U32(v + 0x224) >> 3) & 1u;
                m_.W32(v + 0x270 + (s7 ? 12u : 0u), U(ang));
                if (m_.U32(v + 0x224) & 2u) m_.W32(v + 0x2F8, U(ang));
                const int32_t n = m_.S32(v + 0x320);
                if (!(n < 2)) {                                      // 0x80088F10: the knot search
                    const int32_t knot0 = m_.S32(v + 0x33C);
                    int32_t s2 = m_.S32(v + 0x31C);
                    const int32_t last = Sub(n, 1);
                    int32_t x = ang;
                    auto knot = [&](int32_t i) { return m_.S32(v + 0x33C + 4u * U(i)); };
                    if (x < knot0) {
                        x = Add(x, 0x6487E);
                        s2 = last;
                    } else {
                        bool below;
                        if (s2 != last) below = x < knot(s2);
                        else { s2 = 0; below = x < knot0; }
                        if (!below) {
                            s2 = Add(s2, 1);
                            while (s2 < last && !(x < knot(s2))) {
                                s2 = Add(s2, 1);
                                if (bail()) break;
                            }
                            s2 = Sub(s2, 1);
                        } else {
                            s2 = Sub(s2, 1);
                            // 0x80088FA4: the backward search steps FORWARD
                            for (uint32_t guard = 0; s2 >= 0; ++guard) {
                                if (guard > (1u << 20) || bail()) { Fail(0x80088FB0, "knot search"); return; }
                                if (!(x < knot(s2))) break;
                                s2 = Add(s2, 1);
                            }
                        }
                    }
                    m_.W32(v + 0x31C, U(s2));
                    const int32_t k0 = knot(s2);
                    const int32_t a0 = Sub(x, k0);
                    int32_t y1, m1, z1, n1, s5;
                    if (s2 < last) {
                        const uint32_t i1 = 4u * U(Add(s2, 1));
                        y1 = m_.S32(v + 0x354 + i1); m1 = m_.S32(v + 0x3E4 + i1);
                        z1 = m_.S32(v + 0x36C + i1); n1 = m_.S32(v + 0x3FC + i1);
                        s5 = Sub(knot(Add(s2, 1)), k0);
                    } else {
                        y1 = m_.S32(v + 0x354); m1 = m_.S32(v + 0x3E4);
                        z1 = m_.S32(v + 0x36C); n1 = m_.S32(v + 0x3FC);
                        s5 = Sub(knot0, Add(k0, S(0xFFF9B782u)));
                    }
                    const int32_t t = SDiv(a0, s5);
                    Hermite(t, F + 80, F + 84, F + 88, F + 92);
                    fr.Mark(80, 16);
                    const uint32_t i0 = 4u * U(s2);
                    const uint32_t out = v + 0x270 + (s7 ? 12u : 0u);
                    const int32_t h00 = fr.S32(80), h01 = fr.S32(84), h10 = fr.S32(88), h11 = fr.S32(92);
                    int32_t val = Add(Add(FixMul(m_.S32(v + 0x354 + i0), h00), FixMul(y1, h01)),
                                      FixMul(s5, Add(FixMul(m_.S32(v + 0x3E4 + i0), h10), FixMul(m1, h11))));
                    m_.W32(out + 4, U(val));
                    val = Add(Add(FixMul(m_.S32(v + 0x36C + i0), h00), FixMul(z1, h01)),
                              FixMul(s5, Add(FixMul(m_.S32(v + 0x3FC + i0), h10), FixMul(n1, h11))));
                    m_.W32(out + 8, U(val));
                    if (s7) {
                        m_.W32(v + 0x2A4, 0);
                        m_.W32(v + 0x2A0, 0);
                        m_.W32(v + 0x2A8, U(ApproxLen3(fr.S16(58), fr.S16(62), fr.S16(66)) << 16));
                    } else {
                        m_.W32(v + 0x294, m_.U32(v + 0x384));
                        m_.W32(v + 0x298, m_.U32(v + 0x39C));
                        m_.W32(v + 0x29C, m_.U32(v + 0x3B4));
                    }
                } else if (s7) {                                     // 0x80089194
                    m_.W32(v + 0x2A4, 0);
                    m_.W32(v + 0x2A0, 0);
                    m_.W32(v + 0x280, m_.U32(v + 0x354));
                    m_.W32(v + 0x284, m_.U32(v + 0x36C));
                    m_.W32(v + 0x2A8, U(ApproxLen3(fr.S16(58), fr.S16(62), fr.S16(66)) << 16));
                }
                if (s7) {                                            // 0x800891D8
                    Transition(v, dt, F);
                    if (!(m_.U32(v + 0x224) & 8u)) {
                        m_.W32(v + 0x294, m_.U32(v + 0x384));
                        m_.W32(v + 0x298, m_.U32(v + 0x39C));
                        m_.W32(v + 0x29C, m_.U32(v + 0x3B4));
                    }
                }
            } else {
                // ==================================================== 0x80089214: the scripted track
                const uint32_t f1 = m_.U32(v + 0x224);
                if (f1 & 2u) {
                    uint32_t nf;
                    if (f1 & 4u) {
                        copyKnotsToCurrent();
                        nf = m_.U32(v + 0x224) & 0xFFFFEFE7u;
                    } else {
                        if (!(f1 & 0x40u)) {
                            ToFrame(v, v + 0x23C, v + 0xB8, v + 0x270);
                            ToFrame(v, v + 0x23C, v + 0x22C, v + 0x294);
                            ToPolar(v + 0x270, F);
                        }
                        nf = copyCurrentToKnots();
                    }
                    m_.W32(v + 0x224, nf);
                    m_.W32(v + 0x224, (m_.U32(v + 0x224) & ~0x20u) | 0x40u);
                    if (!(m_.S32(v + 0x320) < 3)) {
                        for (uint32_t j = 0; j < 6; ++j)
                            if (!seams_.SplineSlopes(m_, v + 0x324, v + 0x33C + 24u * j, v + 0x3CC + 24u * j,
                                                     Sub(m_.S32(v + 0x320), 1))) {
                                Fail(0x80089334, "SLUS 0x8002F634 refused");
                                return;
                            }
                    }
                    m_.W32(v + 0x31C, 0);
                    m_.W32(v + 0x314, 0);
                    m_.W32(v + 0x228, m_.U32(v + 0x228) & ~0x20u);
                }
                splineSegment(96);
                if (bail()) { finish(); return; }
                Aim(v, dt, F);                                       // 0x80089588
            }
            if (bail()) { finish(); return; }
            polarToFrame(0x270, 0x278);                              // 0x80089590 (`t9 = sp+128` = sp+40)
        }
        if (bail()) { finish(); return; }
        // ============================================================ 0x80089DAC: place the eye
        const uint32_t f = m_.U32(v + 0x224);
        if ((f & 0x40004u) != 0x40000u) {
            if (f & 0x1000000u) fr.W32(40, U(Neg(fr.S32(40))));
            Blend16To32View(m_, v + 0x258, v + 0x24C, v + 0xB8, fr.S32(48), fr.S32(40));  // 0x80089DF8
            int32_t base[3], out[3];
            int16_t up[3];
            ReadVec32(m_, v + 0xB8, base);
            ReadVec16(m_, v + 0x252, up);
            MulAdd(base, up, m_.S32(v + 0x274), out);                // 0x80089E0C -> sp + 40
            for (uint32_t i = 0; i < 3; ++i) fr.W32(40 + 4 * i, U(out[i]));
            m_.W32(v + 0xB8, U(Add(m_.S32(v + 0x23C), fr.S32(40))));
            m_.W32(v + 0xBC, U(Add(m_.S32(v + 0x240), fr.S32(44))));
            m_.W32(v + 0xC0, U(Add(m_.S32(v + 0x244), fr.S32(48))));
        }
        // 0x80089E50: place the look point +0x22C
        if ((m_.U32(v + 0x224) & 8u) && ((m_.U32(v + 0x228) & 0x80u) || m_.U32(v + 0x21C) == 12u)) {
            polarToFrame(0x270, 0x29C);
        } else {
            fr.W32(40, m_.U32(v + 0x294));
            fr.W32(48, m_.U32(v + 0x29C));
        }
        if (m_.U32(v + 0x224) & 0x1000000u) fr.W32(40, U(Neg(fr.S32(40))));
        Blend16To32View(m_, v + 0x258, v + 0x24C, v + 0x22C, fr.S32(48), fr.S32(40));      // 0x80089F58
        if (m_.S32(v + 0x298) != 0) MulAddView(m_, v + 0x22C, v + 0x252, m_.S32(v + 0x298), v + 0x22C);
        if (m_.U32(v + 0x21C) == 12u && !(m_.U32(v + 0x224) & 8u)) {
            m_.W32(v + 0x22C, U(Add(m_.S32(v + 0x22C), m_.S32(m_.U32(v + 0x2F0) + 0))));
            m_.W32(v + 0x230, U(Add(m_.S32(v + 0x230), m_.S32(m_.U32(v + 0x2F0) + 4))));
            m_.W32(v + 0x234, U(Add(m_.S32(v + 0x234), m_.S32(m_.U32(v + 0x2F0) + 8))));
        } else {
            m_.W32(v + 0x22C, U(Add(m_.S32(v + 0x22C), m_.S32(v + 0x23C))));
            m_.W32(v + 0x230, U(Add(m_.S32(v + 0x230), m_.S32(v + 0x240))));
            m_.W32(v + 0x234, U(Add(m_.S32(v + 0x234), m_.S32(v + 0x244))));
        }
    } else if (mode - 13u < 2u) {
        // ============================================================ 0x80089FF8: modes 13 and 14
        const uint32_t f1 = m_.U32(v + 0x224);
        const uint32_t tgt = m_.U32(v + 0x238);                      // s3
        if (f1 & 2u) {
            if (mode == 13u) {
                m_.W32(v + 0x224, f1 | 4u);
                copyKnotsToCurrent();                                 // `andi v0,v1,0x4` is always set here
                m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xFFFFEFE7u);
                m_.W32(v + 0x224, (m_.U32(v + 0x224) & ~0x20u) | 0x40u);
                if (!(m_.S32(v + 0x320) < 3)) {
                    for (uint32_t j = 0; j < 6; ++j)
                        if (!seams_.SplineSlopes(m_, v + 0x324, v + 0x33C + 24u * j, v + 0x3CC + 24u * j,
                                                 Sub(m_.S32(v + 0x320), 1))) {
                            Fail(0x8008A128, "SLUS 0x8002F634 refused");
                            return;
                        }
                }
                m_.W32(v + 0x31C, 0);
                m_.W32(v + 0x314, 0);
                m_.W32(v + 0x228, m_.U32(v + 0x228) & ~0x20u);
            } else {                                                 // 0x8008A158: mode 14
                const uint32_t i = RadToIndex(m_.S32(v + 0x33C));
                const int32_t c = FixMul(m_.S32(v + 0x36C), Promote(m_.S16(kCameraSinCos + 4u * i + 2u)));
                const int32_t s = FixMul(m_.S32(v + 0x36C), Promote(m_.S16(kCameraSinCos + 4u * i)));
                Blend16To32View(m_, tgt + 0x1C2, tgt + 0x32E, v + 0xB8, c, s);   // 0x8008A1CC
                MulAddView(m_, v + 0xB8, tgt + 0x20A, m_.S32(v + 0x354), v + 0xB8);
                m_.W32(v + 0xB8, U(Add(m_.S32(v + 0xB8), m_.S32(v + 0x23C))));
                m_.W32(v + 0xBC, U(Add(m_.S32(v + 0xBC), m_.S32(v + 0x240))));
                m_.W32(v + 0xC0, U(Add(m_.S32(v + 0xC0), m_.S32(v + 0x244))));
                for (uint32_t k = 0; k < 3; ++k) m_.W32(v + 0x294 + 4u * k, m_.U32(v + 0x384 + 24u * k));
            }
            const uint32_t sl = m_.U32(tgt + 0x154);                 // 0x8008A240: face along the road
            m_.W16(v + 0x258, m_.U16(sl + 14));
            m_.W16(v + 0x25A, m_.U16(m_.U32(tgt + 0x154) + 16));
            m_.W16(v + 0x25C, m_.U16(m_.U32(tgt + 0x154) + 18));
            if (m_.S32(tgt + 0x16C) < 0) {
                const uint16_t a = m_.U16(v + 0x258), c = m_.U16(v + 0x25C);
                m_.W16(v + 0x258, static_cast<uint16_t>(0u - a));
                const uint16_t b = m_.U16(v + 0x25A);
                m_.W16(v + 0x25C, static_cast<uint16_t>(0u - c));
                m_.W16(v + 0x25A, static_cast<uint16_t>(0u - b));
            }
            m_.W32(v + 0x224, m_.U32(v + 0x224) & ~0x60u);
        }
        Aim(v, dt, F);                                               // 0x8008A2C4
        if (bail()) { finish(); return; }
        if (m_.U32(v + 0x21C) == 13u) {
            splineSegment(112);
            if (bail()) { finish(); return; }
            const uint32_t n4 = 4u * m_.U32(v + 0x320);             // 0x8008A500: the knot one past the last
            m_.W32(v + 0xB8, U(Add(m_.S32(v + 0x33C + n4), m_.S32(v + 0x270))));
            m_.W32(v + 0xBC, U(Add(m_.S32(v + 0x354 + 4u * m_.U32(v + 0x320)), m_.S32(v + 0x274))));
            m_.W32(v + 0xC0, U(Add(m_.S32(v + 0x36C + 4u * m_.U32(v + 0x320)), m_.S32(v + 0x278))));
        }
        Blend16To32View(m_, v + 0x258, v + 0x24C, v + 0x22C, m_.S32(v + 0x29C), m_.S32(v + 0x294));  // 0x8008A570
        MulAddView(m_, v + 0x22C, v + 0x252, m_.S32(v + 0x298), v + 0x22C);
        m_.W32(v + 0x22C, U(Add(m_.S32(v + 0x22C), m_.S32(v + 0x23C))));
        m_.W32(v + 0x230, U(Add(m_.S32(v + 0x230), m_.S32(v + 0x240))));
        m_.W32(v + 0x234, U(Add(m_.S32(v + 0x234), m_.S32(v + 0x244))));
    }
    if (bail()) { finish(); return; }

    // ---------------------------------------------------------------- 0x8008A5BC: the tail
    m_.W32(v + 0x224, m_.U32(v + 0x224) & ~6u);
    if (m_.U32(v + 0x21C) != 6u) Orient(v, F);                       // 0x8008A5DC
    RoadRebind(m_, v, 1, F, road_);                                  // SLUS 0x800374D4
    Zones(v, F);                                                     // 0x8008A5F0
    if (bail()) { finish(); return; }
    if (m_.U32(v + 0x224) & 0x80000u) {                              // 0x8008A60C: settle on the ground
        int32_t lift = 0x20000;
        if (!((m_.U32(v + 0x24) >> 8) & 1u)) lift = Add(S(GuestRand(m_) & 0x1FFFFu), 0x20000);
        uint32_t reseat = ((m_.U32(v + 0x24) >> 8) & 1u) ? (m_.U32(v + 0x184) & 1u) : 0u;
        int32_t lat = (m_.S32(v + 0x158) > 0) ? m_.S32(v + 0x19C) : m_.S32(v + 0x190);   // s1
        const int32_t s2 = Neg(lift);
        int32_t tries = 0;
        int32_t s0 = 0;
        for (;;) {                                                   // 0x8008A684
            if (reseat) {
                const uint32_t sl = m_.U32(v + 0x154);
                const int32_t x = Add(m_.S32(sl + 20), FixMul(Promote(m_.S16(sl + 14)), m_.S32(v + 0x15C)));
                m_.W32(v + 0xB8, U(x));
                m_.W32(v + 0x158, U(lat));
                const uint32_t sl2 = m_.U32(v + 0x154);
                m_.W32(v + 0xC0, U(Add(m_.S32(sl2 + 28), FixMul(Promote(m_.S16(sl2 + 18)), m_.S32(v + 0x15C)))));
                m_.W32(v + 0xB8, U(Add(m_.S32(v + 0xB8), FixMul(Promote(m_.S16(m_.U32(v + 0x154) + 2)), lat))));
                m_.W32(v + 0xC0, U(Add(m_.S32(v + 0xC0), FixMul(Promote(m_.S16(m_.U32(v + 0x154) + 6)), m_.S32(v + 0x158)))));
            }
            const uint32_t sl = m_.U32(v + 0x154);                   // 0x8008A724: stand on the slice plane
            const int32_t d0 = Sub(m_.S32(v + 0xB8), m_.S32(sl + 20));
            fr.W32(24, U(d0));
            const int32_t d1 = Neg(m_.S32(m_.U32(v + 0x154) + 24));
            fr.W32(28, U(d1));
            const int32_t d2 = Sub(m_.S32(v + 0xC0), m_.S32(m_.U32(v + 0x154) + 28));
            fr.W32(32, U(d2));
            const uint32_t t0 = m_.U32(v + 0x154);
            const int16_t ny = m_.S16(t0 + 10);
            int32_t t1 = S(U(static_cast<int32_t>(ny)) << 4);
            if (!(t1 < -6552)) t1 = -6553;
            const int32_t dot = RowDot(m_, t0 + 8, d0, d1, d2);
            m_.W32(v + 0xBC, U(Sub(s2, SDiv(dot, t1))));             // 0x8008A868
            RoadRebind(m_, v, 1, F, road_);                          // 0x8008A864
            Zones(v, F);
            if (bail()) { finish(); return; }
            if (m_.U32(v + 0x184) & 1u) {                            // off the road: the ground query
                fr.W32(16, m_.U32(v + 0x218));
                const GroundResult g = GroundQuery(m_, v, 0, F + 24, F + 40, m_.U32(v + 0x218), t_.rsqrt);
                if (g.declined) { Fail(0x8008A898, "ground query declined"); return; }
                s0 = S(g.value);
                m_.W32(v + 0x218, g.value);
            } else {
                if (m_.U32(v + 0x174) != 0) m_.W8(v + 0x216, m_.U8(v + 0x18A));
                else m_.W8(v + 0x216, 1);
                m_.W32(v + 0x218, 0);
                s0 = 0;
            }
            if (s0 < 0 && m_.U32(v + 0x1E0) == 0) {                  // 0x8008A8E4
                m_.W32(v + 0x1E0, 0x1C9C4);
                int16_t dir[3];
                int32_t vel[3];
                ReadVec16(m_, v + 0x1C2, dir);
                Scale(0x1C9C4, dir, vel);                            // SLUS 0x8002EE50
                WriteVec32(m_, v + 0x1C8, vel);
            }
            lat = Add(lat, (lat > 0) ? S(0xFFFF0000u) : 0x10000);
            ++tries;
            if (s0 >= 0 || !(tries < 10)) break;
            reseat = 1;
        }
        Orient(v, F);                                                // 0x8008A93C
        m_.W32(v + 0x224, m_.U32(v + 0x224) & 0xFFF7FFFFu);
    }
    m_.W32(v + 0x224, m_.U32(v + 0x224) & ~0x4000u);                 // 0x8008A958
    finish();
}

} // namespace rr::sim
