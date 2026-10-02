#include "game/sim/ground.h"

#include "game/sim/fixed.h"
#include "game/sim/integrator.h"

// Every function below is transcribed from our own disassembly of RASHCDG.BIN
// (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8), checked instruction by
// instruction against the executable model in tools\scout\ground.py (1270 live calls, 0
// differences). Arithmetic wraps in uint32_t wherever the original's `addu`/`subu` does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
// `sra / addu / xor`: INT32_MIN stays negative.
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}
// `srl t,x,31; addu; sra 1`: halving that truncates toward zero.
inline int32_t Half(int32_t v) { return S(U(v) + (U(v) >> 31)) >> 1; }
// `bgez; addiu 3; sra 2`: quartering that truncates toward zero.
inline int32_t Quarter(int32_t v) { return (v < 0) ? (Add(v, 3) >> 2) : (v >> 2); }
// `lh x; sll x,x,10`: a cell vertex component (cell units, world*64) promoted to world 16.16.
inline int32_t Cell16(int16_t v) { return S(U(static_cast<int32_t>(v)) << 10); }

} // namespace

// ------------------------------------------------------------------------------ RASHCDG 0x800B6E08
int32_t PointInPoly(const int32_t p[3], const int32_t (*v)[3], int32_t n, int32_t axis) {
    // 0x800B6E14..0x800B6E68: i = axis + 1 wrapped below 3, j = i + 1 wrapped below 3.
    int32_t i = axis + 1;
    i = (i < 3) ? i : 0;
    int32_t j = i + 1;
    j = (j < 3) ? j : 0;
    int32_t prev = n - 1;                                   // a1 = s6 - 1
    for (int32_t k = 0; k < n; ++k) {                       // blez s6 -> return 1
        const int32_t* c = v[k];
        const int32_t* q = v[prev];
        const int32_t a = FixMul(Sub(c[j], q[j]), Sub(p[i], q[i]));   // 0x800B6ECC
        const int32_t b = FixMul(Sub(c[i], q[i]), Sub(p[j], q[j]));   // 0x800B6EE4
        if (Sub(a, b) >= 0) return 0;                       // `bgez s0` at 0x800B6EF0
        prev = k;
    }
    return 1;
}

// ------------------------------------------------------------------------------ RASHCDG 0x800B6844
void TriNormal(const int32_t (*v)[3], int32_t n, int16_t out[3], const uint16_t* rsqrt) {
    int32_t s[3] = {0, 0, 0};                               // sp+16..sp+24, zeroed at entry
    int32_t k = 0;
    // `blez n-1` at 0x800B687C; the loop compares k against n - 1 again at its bottom.
    for (; k < n - 1; ++k) {
        const int32_t* a = v[k];
        const int32_t* b = v[k + 1];
        s[0] = Add(s[0], FixMul(Sub(a[1], b[1]), Add(a[2], b[2])));
        s[1] = Add(s[1], FixMul(Sub(a[2], b[2]), Add(a[0], b[0])));
        s[2] = Add(s[2], FixMul(Sub(a[0], b[0]), Add(a[1], b[1])));
    }
    {   // 0x800B6918: the closing edge v[k] -> v[0]
        const int32_t* a = v[k];
        const int32_t* b = v[0];
        s[0] = Add(s[0], FixMul(Sub(a[1], b[1]), Add(a[2], b[2])));
        s[1] = Add(s[1], FixMul(Sub(a[2], b[2]), Add(a[0], b[0])));
        s[2] = Add(s[2], FixMul(Sub(a[0], b[0]), Add(a[1], b[1])));
    }
    // 0x800B699C..0x800B6A50: halve all three while any |component| exceeds 0x005A8000.
    constexpr int32_t kClamp = 0x005A8000;
    while (kClamp < Abs(s[0]) || kClamp < Abs(s[1]) || kClamp < Abs(s[2])) {
        s[0] >>= 1;
        s[1] >>= 1;
        s[2] >>= 1;
    }
    Normalize32(s, rsqrt);                                  // SLUS 0x8002E14C at 0x800B6A54
    for (int c = 0; c < 3; ++c) out[c] = static_cast<int16_t>(s[c] >> 4);
}

// ------------------------------------------------------------------------------ RASHCDG 0x800A8498
CellLookupResult CellLookup(GuestRam& m, const int32_t p[3], uint32_t heading, uint32_t stateIn) {
    CellLookupResult r;
    uint32_t state = stateIn;
    // 0x800A84DC..0x800A84F4: player 2's half when the state word is negative.
    uint32_t lo = 0, hi = 12;
    if (S(state) < 0) { lo = 12; hi = 24; }
    uint32_t slot = state & 0x3Fu;                          // s2

    // The 72-byte polygon buffer at sp+16 - six vertices - and which of its slots this call wrote.
    int32_t poly[6][3] = {};
    bool written[6] = {false, false, false, false, false, false};

    for (uint32_t n = lo; n < hi; ++n, ++slot) {            // t6 at 120(sp), 0x800A8B90..0x800A8BA0
        // 0x800A850C..0x800A8528: a slot outside the half is replaced by the half's first.
        if (!(slot >= lo && slot < hi)) slot = lo;
        const uint32_t sl = kCellSlotTable + kCellSlotBytes * slot;
        if (m.U32(sl) == 0xFFFFFFFFu) continue;             // 0x800A854C
        const uint32_t body = m.U32(sl + 4u);
        // 0x800A8564..0x800A85B8: *verts and origin, for every slot visited.
        r.verts = m.U32(body + 0x34u) + 4u;
        r.slotVisited = true;
        const uint32_t r3 = m.U32(body + 0x2Cu);           // s7
        for (uint32_t c = 0; c < 3; ++c) r.origin[c] = S(m.U32(body + 8u + 4u * c) << 10);
        int32_t lp[3];                                      // sp+88
        for (int c = 0; c < 3; ++c) lp[c] = Sub(p[c], r.origin[c]);
        uint32_t sub = static_cast<uint32_t>(S(state) >> 6) & 7u;   // s1, from *state
        if (m.U16(body + 6u) == 0) continue;                // 0x800A8610: B == 0
        for (uint32_t k = 0;;) {
            // 0x800A8618: the sub-area wraps to 0 at B (a signed compare on the u16 B).
            const int32_t B = m.U16(body + 6u);
            if (!(S(sub) < B)) sub = 0;
            const uint32_t a0 = r3 + 2u * sub;
            const int32_t start = m.S16(a0 + 2u);
            const int32_t cnt = Sub(m.S16(a0 + 4u), start); // s0
            if (cnt > 6) { r.declined = true; return r; }   // would overrun the frame buffer
            int32_t mnx = 0x3FFF0000, mxx = S(0xC0010000u), mnz = 0x3FFF0000, mxz = S(0xC0010000u);
            for (int32_t j = 0; j < cnt; ++j) {             // blez s0 skips the fill
                const int32_t vi = m.S16(r3 + 2u * U(Add(start, j)));    // lh: a SIGNED index
                const uint32_t va = r.verts + U(vi) * 8u;
                int32_t* dst = poly[cnt - 1 - j];           // stored reversed
                dst[0] = Cell16(m.S16(va + 0u));
                dst[1] = Cell16(m.S16(va + 2u));
                dst[2] = Cell16(m.S16(va + 4u));
                written[cnt - 1 - j] = true;
                if (!(mnx < dst[0])) mnx = dst[0];          // `slt; beqz; move`: a min / max
                if (!(dst[0] < mxx)) mxx = dst[0];
                if (!(mnz < dst[2])) mnz = dst[2];
                if (!(dst[2] < mxz)) mxz = dst[2];
            }
            bool inside = false;
            // 0x800A8754..0x800A8784: X min, X max, Z min, Z max, then the polygon itself.
            if (!(lp[0] < mnx) && !(mxx < lp[0]) && !(lp[2] < mnz) && !(mxz < lp[2]))
                inside = PointInPoly(lp, poly, cnt, 1) != 0;
            if (m.Faulted()) return r;
            if (inside) {
                // ---- 0x800A87B4: found. Bits 11..31 of the state word are kept.
                state = (state & 0xFFFFF800u) | slot | (sub << 6);
                int32_t best = 0x3FFF0000, edge = 0;
                for (int32_t j = 0; j < cnt; j += 2) {      // the nearest crossing edge
                    if (!written[j] || !written[j + 1]) { r.declined = true; return r; }
                    const int32_t mx = Half(Add(poly[j][0], poly[j + 1][0]));
                    const int32_t mz = Half(Add(poly[j][2], poly[j + 1][2]));
                    int32_t a = Abs(Sub(lp[0], mx)), b = Abs(Sub(lp[2], mz));
                    if (a < b) { const int32_t t = a; a = b; b = t; }
                    const int32_t c = Add(b, b >> 1);
                    const int32_t d = Add(Add(Sub(Sub(a, a >> 5), a >> 7), c >> 2), c >> 6);
                    if (d < best) { edge = Half(Sub(Sub(cnt, j), 1)); best = d; }
                }
                // 0x800A88B0..0x800A88CC
                const int32_t base = Sub(cnt, S(U(edge) << 1));
                const int32_t ia = Sub(base, 2), ib = Sub(base, 1);
                state |= U(edge) << 9;
                if (ia < 0 || ia > 5 || ib < 0 || ib > 5 || !written[ia] || !written[ib]) {
                    r.declined = true;
                    return r;
                }
                int32_t d[3];
                for (int c = 0; c < 3; ++c) d[c] = Quarter(Sub(poly[ia][c], poly[ib][c]));
                const int32_t hx = S(U(static_cast<int32_t>(m.S16(heading + 0u))) << 4);
                const int32_t hy = S(U(static_cast<int32_t>(m.S16(heading + 2u))) << 4);
                const int32_t hz = S(U(static_cast<int32_t>(m.S16(heading + 4u))) << 4);
                // The inlined `mult`/`mfhi,mflo` FixMul of 0x800A896C..0x800A8A30.
                const int32_t dot = Add(FixMul(d[2], hz), Add(FixMul(d[1], hy), FixMul(d[0], hx)));
                const int32_t cross = Sub(FixMul(d[2], hx), FixMul(d[0], hz));
                if (cross < 0) state |= 0x2000u;
                state |= U(dot >> 31) & 0x1000u;
                const uint32_t A = m.U16(body + 4u);
                const uint32_t grp = m.U32(body + 0x28u);
                uint32_t g;
                const uint32_t r7 = ((state >> 11) & 1u) ? m.U32(body + 0x3Cu) : 0u;
                if (((state >> 11) & 1u) != 0u && r7 != 0u) {
                    g = A + m.U16(body + 6u) + sub;         // band 1 of region 7
                    r.prim = r7 + m.U32(grp + 12u * g);
                } else {
                    const uint32_t r6 = m.U32(body + 0x38u);
                    if (r6 == 0u) {                         // 0x800A8AE4 -> 0x800A8BA8
                        r.state = 0;
                        r.counts = 0;
                        return r;
                    }
                    state &= ~0x800u;
                    g = A + sub;                            // band 0 of region 6
                    r.prim = r6 + m.U32(grp + 12u * g);
                }
                r.primWritten = true;
                r.state = state;
                r.counts = m.U32(grp + 12u * g + 4u) | (m.U32(grp + 12u * g + 8u) << 16);
                return r;
            }
            // 0x800A8B74: the next sub-area.
            ++k;
            if (!(S(k) < static_cast<int32_t>(m.U16(body + 6u)))) break;
            ++sub;
        }
    }
    r.state = 0;                                            // 0x800A8BA8
    r.counts = 0;
    return r;
}

// ------------------------------------------------------------------------------ RASHCDG 0x800A7BF8
GroundResult GroundQuery(GuestRam& m, uint32_t e, uint32_t ref, uint32_t outPoint,
                         uint32_t outNormal, uint32_t hint, const uint16_t* rsqrt) {
    GroundResult res;
    const uint32_t s0 = (ref != 0u) ? ref : e + 0xB8u;     // 0x800A7C34
    const uint32_t gs = m.U32(kRoadGameState);
    int32_t dist = m.S32(e + 0x2Cu);
    uint32_t state = hint & 0x7FFu;                         // 120(sp)
    if (!(m.U32(gs + 0x30u) < 2u)) {                        // `sltiu v0,v0,2`: two players
        const int32_t d2 = m.S32(e + 0x30u);
        if (d2 < dist) { state |= 0x80000000u; dist = d2; }
    }
    if (dist < kGroundFineDistance) state |= 0x800u;
    int32_t refNow[3] = {m.S32(s0 + 0u), m.S32(s0 + 4u), m.S32(s0 + 8u)};
    const CellLookupResult look = CellLookup(m, refNow, e + 0x1C2u, state);
    if (look.declined) { res.declined = true; return res; }
    if (m.Faulted()) return res;
    const uint32_t st = look.state;
    const uint32_t t0 = st & 0x7FFFFFFFu;
    if (look.counts == 0u) return res;                      // 0x800A8460: -1
    const uint32_t prim = look.prim;                        // 112(sp); 0 when never stored
    const uint32_t verts = look.slotVisited ? look.verts : 0u;
    int32_t org[3] = {0, 0, 0};
    if (look.slotVisited) for (int c = 0; c < 3; ++c) org[c] = look.origin[c];
    // 0x800A7D00..0x800A7D48: the local point, read AFTER the lookup.
    const int32_t lp[3] = {Sub(m.S32(s0 + 0u), org[0]), Sub(m.S32(s0 + 4u), org[1]),
                           Sub(m.S32(s0 + 8u), org[2])};
    const bool same = ((st ^ hint) & 0xFFFu) == 0u;
    const uint32_t body = m.U32(kCellSlotTable + kCellSlotBytes * (st & 0x3Fu) + 4u);
    const uint32_t r4 = m.U32(body + 0x30u);
    uint32_t off = ((same ? 0xFFFFFFFFu : 0u) & (((hint >> 14) & 0x3FFu) + 1u)) - 1u;  // 128(sp)
    const uint32_t fine = (t0 >> 11) & 1u;
    if (!(off < 512u)) {                                    // 0x800A7D88: search
        const uint32_t n = m.U8(r4 + fine);                 // nCoarse or nAll
        off = 2u;
        if (n == 0u) return res;                            // 0x800A7DAC -> -1
        const uint32_t sub = (t0 >> 6) & 7u, edge = (t0 >> 9) & 3u;
        uint32_t k = 0;
        for (;;) {
            const uint8_t tag = m.U8(r4 + off);
            if (static_cast<uint32_t>(tag >> 7) == fine && ((tag & 0x70u) >> 4) == sub && ((tag & 0x0Cu) >> 2) == edge) break;
            ++k;
            off = off + 2u + m.U8(r4 + off + 1u);
            if (!(S(k) < S(n))) return res;                 // 0x800A7E38 -> -1
        }
    }
    const uint32_t rec = r4 + off;                          // 132(sp)
    const uint32_t len0 = m.U8(rec + 1u);
    if (len0 == 0u) {                                       // 0x800A7E68: list 0 empty
        res.value = (off << 14) | 0xFF000000u | (t0 & 0xFFFu);
        return res;
    }
    // 0x800A7E8C..0x800A7F28: the three lists' inclusive byte bounds, as u8 (`sb`).
    uint8_t b[6];
    b[0] = 2;
    b[2] = static_cast<uint8_t>(len0 + 4u);
    b[1] = static_cast<uint8_t>(len0 + 1u);
    const uint32_t v0 = static_cast<uint32_t>(b[2]) + m.U8(rec + b[2] - 1u);
    b[3] = static_cast<uint8_t>(v0 - 1u);
    b[4] = static_cast<uint8_t>(v0 + 2u);
    b[5] = static_cast<uint8_t>(static_cast<uint32_t>(b[4]) +
                                (m.U8(rec + b[4] - 1u) & (0u - fine)) - 1u);
    const int32_t dir = 1 - S((t0 >> 11) & 2u);            // s8: +-1 inside a list
    const int32_t dirL = 1 - S((t0 >> 12) & 2u);           // 136(sp): +-1 across lists
    int32_t pos = (S(hint) >> 24) & 0x7F;                   // s5
    int32_t L = (S(hint) >> 12) & 3;                        // s3, NOT gated on `same`
    {   // 0x800A7F24..0x800A7F64: an invalid list index becomes 0 (a3 is 0 on entry).
        uint32_t ok = 0;
        if (L < 3) ok = (b[2 * L + 1] < b[2 * L]) ? 0u : 1u;
        L = L & S(0u - ok);
    }
    if (!same || pos == 127 || pos < 2) pos = b[2 * L + (dir < 0 ? 1 : 0)];
    int32_t stop = b[2 * L + (dir >= 0 ? 1 : 0)];           // 140(sp)
    int32_t last = 0;                                       // s4
    if (dirL == 1) {
        last = S(fine) + 1;
        while (last != 0 && b[2 * last + 1] < b[2 * last]) --last;
    }
    const int32_t cap = (static_cast<int32_t>(b[1]) - b[0]) + b[3] - b[2] +
                        (S(0u - fine) & (static_cast<int32_t>(b[5]) - b[4] + 1)) + 2;   // 148(sp)
    const uint32_t tri = look.counts & 0xFFFFu;
    int32_t it = 0, a1 = 0;
    do {
        const uint32_t idx = m.U8(rec + U(pos));
        const bool quad = !(idx < tri);
        const uint32_t ix = quad ? prim + 14u + 20u * tri + 24u * (idx - tri) + 2u
                                 : prim + 14u + 20u * idx;   // s2
        const int32_t nv = quad ? 4 : 3;                    // s1
        int32_t poly[4][3];                                 // sp+56
        int32_t mnx = 0x3FFF0000, mxx = S(0xC0010000u), mnz = 0x3FFF0000, mxz = S(0xC0010000u);
        for (int32_t k = 0; k < nv; ++k) {
            const uint32_t va = verts + 8u * m.U16(ix + 2u * U(k));     // lhu: an UNSIGNED index
            poly[k][0] = Cell16(m.S16(va + 0u));
            poly[k][1] = Cell16(m.S16(va + 2u));
            poly[k][2] = Cell16(m.S16(va + 4u));
            if (!(mnx < poly[k][0])) mnx = poly[k][0];
            if (!(poly[k][0] < mxx)) mxx = poly[k][0];
            if (!(mnz < poly[k][2])) mnz = poly[k][2];
            if (!(poly[k][2] < mxz)) mxz = poly[k][2];
        }
        if (m.Faulted()) return res;
        a1 = 0;
        bool inside = false;
        if (!(lp[0] < mnx) && !(mxx < lp[0]) && !(lp[2] < mnz) && !(mxz < lp[2]))
            inside = PointInPoly(lp, poly, nv, 1) != 0;     // 0x800A8214
        if (inside) {
            int16_t n[3];
            const int32_t* pt;
            if (nv == 4) {
                if (PointInPoly(lp, &poly[1], 3, 1) != 0) { // 0x800A823C: (v1 v2 v3)
                    TriNormal(&poly[1], 3, n, rsqrt);
                } else {                                    // (v0 v1 v3): poly[2] = poly[3]
                    for (int c = 0; c < 3; ++c) poly[2][c] = poly[3][c];
                    TriNormal(poly, 3, n, rsqrt);
                }
                pt = poly[1];
            } else {
                TriNormal(poly, 3, n, rsqrt);
                pt = poly[0];
            }
            // 0x800A8284..0x800A82A4: the normal is stored, then negated in place (`lhu; negu; sh`).
            for (int c = 0; c < 3; ++c)
                m.W16(outNormal + 2u * U(c), static_cast<uint16_t>(0u - static_cast<uint16_t>(n[c])));
            for (int c = 0; c < 3; ++c) m.W32(outPoint + 4u * U(c), U(Add(pt[c], org[c])));
            // 0x800A8354..0x800A8380: primitive +0x02 >> 12 into e+0x216.
            const uint16_t w = m.U16(ix - (nv < 4 ? 12u : 14u));
            m.W8(e + 0x216u, static_cast<uint8_t>(w >> 12));
            a1 = 2;
        } else if (pos != stop) {
            pos += dir;
        } else if (L == last) {
            a1 = 1;                                         // every list exhausted
        } else {                                            // 0x800A83AC: the next list
            L += dirL;
            const int32_t j = 2 * L + (dir < 0 ? 1 : 0);
            if (j < 0 || j > 5 || j + dir < 0 || j + dir > 5) { res.declined = true; return res; }
            pos = b[j];
            stop = b[j + dir];
        }
        ++it;
    } while (!(cap < it) && a1 == 0);
    if (m.Faulted()) return res;
    if (a1 == 2) {
        res.value = (U(pos) << 24) | (off << 14) | (U(L) << 12) | (t0 & 0xFFFu);
        res.hit = true;
        return res;
    }
    const uint32_t v1 = ((off << 14) | (t0 & 0xFFFu) | 0xFF000000u) + 1u;
    res.value = ((0u - U(a1)) & v1) - 1u;
    return res;
}

// ------------------------------------------------------------------------------ RASHCDG 0x8008DBCC
void ViewDistance(GuestRam& m, uint32_t e) {
    m.W32(e + 0x30u, 0x7FFFFFFFu);                          // 0x8008DBF8
    uint32_t view = kViewObjects;
    uint32_t out = e;
    uint32_t p = 0;
    for (;;) {
        const int32_t dx = Sub(m.S32(e + 0xB8u), m.S32(view + 0xB8u));
        const int32_t dy = Sub(m.S32(e + 0xBCu), m.S32(view + 0xBCu));
        const int32_t dz = Sub(m.S32(e + 0xC0u), m.S32(view + 0xC0u));
        ++p;
        view += kViewObjectBytes;
        const int32_t v = ApproxLen3(dx >> 10, dy >> 10, dz >> 10);   // SLUS 0x8001FCB0
        // `sra a0,v0,31; subu v1,0x7FFFFFFF,v0; and; addu`: a negative length saturates.
        const int32_t sat = Add(v, S(U(v >> 31) & U(Sub(0x7FFFFFFF, v))));
        m.W32(out + 0x2Cu, U(sat));
        out += 4u;
        const uint32_t players = m.U32(m.U32(kRoadGameState) + 0x30u);
        if (!(p < players)) break;                          // `sltu` at 0x8008DC78
    }
}

} // namespace rr::sim
