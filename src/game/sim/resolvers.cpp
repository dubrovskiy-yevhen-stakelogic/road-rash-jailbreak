#include "game/sim/resolvers.h"

#include "game/sim/coll_util.h"

namespace rr::sim {
namespace {

using namespace cu;

constexpr uint32_t kSinCos = 0x8005624C;    // SLUS {s16 sin, s16 cos} x 4096
constexpr uint32_t kFaceRowTable = 0x8005B970; // RASHCDG, 6 bytes: FaceNormal's face -> row
constexpr uint32_t kBigCount = 0x800CCF90;  // s32, NoteBigVolume's count
constexpr uint32_t kBigIds = 0x800CCF88;    // u8[8]

// The face nibbles of 0x800B7030 / 0x800B71AC / 0x800B74F0: NIB = 0x002EDF31 gives per face a signed
// nibble +-(row + 1), ORG = 0x00606600 the box corner of the face plane.
constexpr uint32_t kNib = 0x002EDF31u;
constexpr uint32_t kOrg = 0x00606600u;
int32_t FaceNib(uint32_t f) { return S(kNib << (28u - 4u * f)) >> 28; }
uint32_t FaceRow(int32_t nib) { return U(Iabs(nib)) - 1u; }
uint32_t FaceOrg(uint32_t f) { return (kOrg >> (4u * f)) & 0xFu; }

// "sticky": e->f340 == shape ? (u8 e[0x235]) >> 7 : 0.
uint32_t Sticky(GuestRam& g, uint32_t e, uint32_t s) { return g.U32(e + 832) == s ? (g.U8(e + 565) >> 7) : 0u; }

// The heading rebuild inlined by the resolvers: +0x124 = RatAtan2(+0x1C2 << 4, +0x1C6 << 4),
// +0x128 = cos << 4, +0x12C = sin << 4 (the angle re-read between the two).
void RebuildHeading(GuestRam& g, uint32_t e, const BikeTables& t) {
    const int32_t a = RatAtan2(Shl(g.S16(e + 450), 4), Shl(g.S16(e + 454), 4), t.atan);
    g.W32(e + 292, U(a));
    const int32_t cs = Shl(g.S16(kSinCos + (((U(a) & 0xFFFu) << 2) | 2u)), 4);
    const uint32_t a2 = g.U32(e + 292);
    g.W32(e + 296, U(cs));
    g.W32(e + 300, U(Shl(g.S16(kSinCos + ((a2 & 0xFFFu) << 2)), 4)));
}

// e->f228 = (v <u e->f228) ? e->f228 : v.
void MaxU228(GuestRam& g, uint32_t e, uint32_t v) {
    const uint32_t old = g.U32(e + 552);
    g.W32(e + 552, v < old ? old : v);
}

} // namespace

// ============================================================================ RASHCDG 0x800B675C
bool FaceNormal(GuestRam& g, uint32_t corners, uint32_t rows, uint32_t face, uint32_t out, uint32_t outPt,
                uint32_t sp) {
    const uint32_t fr = sp - 8;
    for (uint32_t k = 0; k < 6; ++k) g.W8(fr + k, g.U8(kFaceRowTable + k));   // lwl/lwr + 2 x lb/sb
    const int32_t row = g.S8(fr + face);                                      // no bound on face
    const uint32_t r = rows + U(MulLo(row, 6));
    uint32_t pt;
    if (face < 2u || face == 5u) {
        g.W16(out + 0, static_cast<uint16_t>(0u - g.U16(r + 0)));
        g.W16(out + 2, static_cast<uint16_t>(0u - g.U16(r + 2)));
        g.W16(out + 4, static_cast<uint16_t>(0u - g.U16(r + 4)));
        pt = corners + 48;
    } else {
        g.W16(out + 0, g.U16(r + 0));
        g.W16(out + 2, g.U16(r + 2));
        g.W16(out + 4, g.U16(r + 4));
        pt = corners + 24;
    }
    if (outPt != 0)
        for (uint32_t k = 0; k < 3; ++k) g.W32(outPt + 4 * k, g.U32(pt + 4 * k));
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B6B58
int32_t XzProject(GuestRam& g, uint32_t p, uint32_t n, uint32_t q) {
    const int32_t x = Mid(Shl(g.S16(n + 0), 4), Sub(g.S32(p + 0), g.S32(q + 0)));
    const int32_t z = Mid(Shl(g.S16(n + 4), 4), Sub(g.S32(p + 8), g.S32(q + 8)));
    return Add(x, z);
}

// ============================================================================ RASHCDG 0x800B6D70
bool PointInPoly(GuestRam& g, uint32_t pt, uint32_t verts, uint32_t normals, int32_t n, int32_t tol, uint32_t sp,
                 uint32_t& v0) {
    (void)sp;
    v0 = 1;
    if (n <= 0) return !g.Faulted();
    const int32_t lim = Neg(tol);
    for (int32_t i = 0; i < n; ++i) {
        if (GProject(g, pt, normals + 6u * U(i), verts + 12u * U(i)) < lim) {
            v0 = 0;
            break;
        }
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B7030
bool FirstPointInsideBox(GuestRam& g, uint32_t pts, uint32_t rows, uint32_t box, uint32_t mask, uint32_t outFace,
                         uint32_t outDepth, uint32_t sp, uint32_t& v0) {
    g.W32(sp + 0, pts);                                                       // the home stores
    g.W32(sp + 4, rows);
    g.W32(sp + 8, box);
    g.W32(sp + 12, mask);
    int32_t mn = 0x7FFF0000;
    g.W32(outFace, 6);
    for (uint32_t p = 0; p < 8; ++p) {
        uint32_t f = 0;
        for (; f < 6; ++f) {
            const int32_t nib = FaceNib(f);
            int32_t d = GProject(g, pts + 12u * p, rows + 6u * FaceRow(nib), box + 12u * FaceOrg(f));
            if (nib < 0) d = Neg(d);
            if (d < 0) break;
            if (!((mask >> f) & 1u) && d < mn) {
                g.W32(outFace, f);
                mn = d;
            }
        }
        if (f == 6) {
            if (outDepth != 0) g.W32(outDepth, U(mn));
            v0 = p;
            return !g.Faulted();
        }
    }
    v0 = 8;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B71AC
bool DeepestInsideAlongDir(GuestRam& g, uint32_t pts, uint32_t dir, int32_t sign, uint32_t box, uint32_t rows,
                           uint32_t mask, uint32_t outFace, uint32_t outDist, uint32_t sp, uint32_t& v0) {
    const uint32_t fr = sp - 120;
    g.W32(sp + 0, pts);                                                       // sw a0,120(sp)
    g.W32(sp + 12, box);                                                      // sw a3,132(sp)
    g.W32(fr + 72, kOrg);
    int8_t inside[8] = {};
    int32_t n = 0;
    for (int32_t p = 0; p < 8; ++p) {
        uint32_t f = 0;
        for (; f < 6; ++f) {
            if ((mask >> f) & 1u) continue;
            const int32_t nib = FaceNib(f);
            int32_t d = GProject(g, pts + 12u * U(p), rows + 6u * FaceRow(nib), box + 12u * FaceOrg(f));
            if (nib < 0) d = Neg(d);
            if (d < 0) break;
        }
        if (f == 6) {
            g.W8(fr + 24 + U(n), static_cast<uint8_t>(p));
            inside[n++] = static_cast<int8_t>(p);
        }
    }
    if (n == 0) {
        v0 = 8;
        return !g.Faulted();
    }
    const uint32_t d16 = fr + 16;                                             // RayPlane reads it
    for (uint32_t k = 0; k < 3; ++k) {
        const uint16_t c = g.U16(dir + 2 * k);
        g.W16(d16 + 2 * k, sign >= 0 ? static_cast<uint16_t>(0u - c) : c);
    }
    int8_t bestFace[8];
    for (int8_t& b : bestFace) b = 6;                                         // SLUS 0x8001E100(sp+64, 6, 8)
    g.W32(fr + 64, 0x06060606u);
    g.W32(fr + 68, 0x06060606u);
    int32_t best[8] = {};
    for (int32_t j = 0; j < n; ++j) {
        best[j] = 0x100000;
        for (uint32_t f = 0; f < 6; ++f) {
            if ((mask >> f) & 1u) continue;
            const uint32_t row = FaceRow(FaceNib(f));
            int32_t d = RayPlane(g, pts + U(MulLo(inside[j], 12)), d16, rows + 6u * row, box + 12u * FaceOrg(f));
            if (!(-2048 < d)) continue;
            if (!(2048 < d)) d = 2048;
            if (d < best[j]) {
                best[j] = d;
                bestFace[j] = static_cast<int8_t>(f);
            }
        }
    }
    int32_t t = 0;
    for (int32_t j = 1; j < n; ++j)
        if (best[t] < best[j]) t = j;
    g.W32(outFace, U(bestFace[t]));
    g.W32(outDist, U(best[t]));
    v0 = U(inside[t]);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B7810
// Straight from the disassembly: every load and store in the original's order (the edge normals are
// read back from `edgeN` after they are stored), the three shared tails reached as the original's
// jumps reach them.
bool FaceQuad(GuestRam& g, uint32_t box, uint32_t rows, uint32_t face, uint32_t quad, uint32_t edgeN) {
    uint32_t a0 = box, v0 = 0, v1 = 0;
    const uint32_t a1 = rows, a3 = quad, t0 = edgeN;
    switch (face) {
    case 3: // 0x800B783C
        v0 = g.U32(a0 + 24u);  // 800b783c
        g.W32(a3 + 0u, v0);  // 800b7844
        v0 = g.U32(a0 + 28u);  // 800b7848
        g.W32(a3 + 4u, v0);  // 800b7850
        v0 = g.U32(a0 + 32u);  // 800b7854
        g.W32(a3 + 8u, v0);  // 800b785c
        v0 = g.U32(a0 + 36u);  // 800b7860
        g.W32(a3 + 12u, v0);  // 800b7868
        v0 = g.U32(a0 + 40u);  // 800b786c
        g.W32(a3 + 16u, v0);  // 800b7874
        v0 = g.U32(a0 + 44u);  // 800b7878
        g.W32(a3 + 20u, v0);  // 800b7880
        v0 = g.U32(a0 + 84u);  // 800b7884
        g.W32(a3 + 24u, v0);  // 800b788c
        v0 = g.U32(a0 + 88u);  // 800b7890
        g.W32(a3 + 28u, v0);  // 800b7898
        v0 = g.U32(a0 + 92u);  // 800b789c
        g.W32(a3 + 32u, v0);  // 800b78a4
        v0 = g.U32(a0 + 72u);  // 800b78a8
        g.W32(a3 + 36u, v0);  // 800b78b0
        v0 = g.U32(a0 + 76u);  // 800b78b4
        g.W32(a3 + 40u, v0);  // 800b78bc
        v0 = g.U32(a0 + 80u);  // 800b78c0
        g.W32(a3 + 44u, v0);  // 800b78c8
        v0 = g.U16(a1 + 6u);  // 800b78cc
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b78d4
        v0 = g.U16(a1 + 8u);  // 800b78d8
        g.W16(t0 + 2u, static_cast<uint16_t>(v0));  // 800b78e0
        v0 = g.U16(a1 + 10u);  // 800b78e4
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b78ec
        v0 = g.U16(a1 + 6u);  // 800b78f0
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b78f8
        v0 = g.U16(a1 + 8u);  // 800b78fc
        g.W16(t0 + 14u, static_cast<uint16_t>(v0));  // 800b7904
        v0 = g.U16(a1 + 10u);  // 800b7908
        v1 = g.U16(t0 + 2u);  // 800b790c
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7910
        v0 = g.U16(a1 + 0u);  // 800b7914
        g.W16(t0 + 6u, static_cast<uint16_t>(v0));  // 800b791c
        v0 = g.U16(a1 + 2u);  // 800b7920
        g.W16(t0 + 8u, static_cast<uint16_t>(v0));  // 800b7928
        v0 = g.U16(a1 + 4u);  // 800b792c
        g.W16(t0 + 10u, static_cast<uint16_t>(v0));  // 800b7934
        v0 = g.U16(a1 + 0u);  // 800b7938
        g.W16(t0 + 18u, static_cast<uint16_t>(v0));  // 800b7940
        v0 = g.U16(a1 + 2u);  // 800b7944
        g.W16(t0 + 20u, static_cast<uint16_t>(v0));  // 800b794c
        a0 = g.U16(a1 + 4u);  // 800b7950
    L7954:
        v0 = g.U16(t0 + 0u);  // 800b7954
        v1 = 0u - v1;  // 800b7958
        g.W16(t0 + 2u, static_cast<uint16_t>(v1));  // 800b795c
        v1 = g.U16(t0 + 18u);  // 800b7960
        v0 = 0u - v0;  // 800b7964
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7968
        v0 = g.U16(t0 + 4u);  // 800b796c
        v1 = 0u - v1;  // 800b7970
        g.W16(t0 + 18u, static_cast<uint16_t>(v1));  // 800b7974
        g.W16(t0 + 22u, static_cast<uint16_t>(a0));  // 800b7978
        v1 = a0;  // 800b797c
        v0 = 0u - v0;  // 800b7980
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7984
    L7988:
        v0 = g.U16(t0 + 20u);  // 800b7988
        v1 = 0u - v1;  // 800b798c
        g.W16(t0 + 22u, static_cast<uint16_t>(v1));  // 800b7990
        v0 = 0u - v0;  // 800b7994
        g.W16(t0 + 20u, static_cast<uint16_t>(v0));  // 800b799c
        return !g.Faulted();
    case 1: // 0x800B79A0
        v0 = g.U32(a0 + 0u);  // 800b79a0
        g.W32(a3 + 0u, v0);  // 800b79a8
        v0 = g.U32(a0 + 4u);  // 800b79ac
        g.W32(a3 + 4u, v0);  // 800b79b4
        v0 = g.U32(a0 + 8u);  // 800b79b8
        g.W32(a3 + 8u, v0);  // 800b79c0
        v0 = g.U32(a0 + 12u);  // 800b79c4
        g.W32(a3 + 12u, v0);  // 800b79cc
        v0 = g.U32(a0 + 16u);  // 800b79d0
        g.W32(a3 + 16u, v0);  // 800b79d8
        v0 = g.U32(a0 + 20u);  // 800b79dc
        g.W32(a3 + 20u, v0);  // 800b79e4
        v0 = g.U32(a0 + 60u);  // 800b79e8
        g.W32(a3 + 24u, v0);  // 800b79f0
        v0 = g.U32(a0 + 64u);  // 800b79f4
        g.W32(a3 + 28u, v0);  // 800b79fc
        v0 = g.U32(a0 + 68u);  // 800b7a00
        g.W32(a3 + 32u, v0);  // 800b7a08
        v0 = g.U32(a0 + 48u);  // 800b7a0c
        g.W32(a3 + 36u, v0);  // 800b7a14
        v0 = g.U32(a0 + 52u);  // 800b7a18
        g.W32(a3 + 40u, v0);  // 800b7a20
        v0 = g.U32(a0 + 56u);  // 800b7a24
        g.W32(a3 + 44u, v0);  // 800b7a2c
        v0 = g.U16(a1 + 6u);  // 800b7a30
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7a38
        v0 = g.U16(a1 + 8u);  // 800b7a3c
        g.W16(t0 + 2u, static_cast<uint16_t>(v0));  // 800b7a44
        v0 = g.U16(a1 + 10u);  // 800b7a48
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7a50
        v0 = g.U16(a1 + 6u);  // 800b7a54
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b7a5c
        v0 = g.U16(a1 + 8u);  // 800b7a60
        g.W16(t0 + 14u, static_cast<uint16_t>(v0));  // 800b7a68
        v0 = g.U16(a1 + 10u);  // 800b7a6c
        v1 = g.U16(t0 + 2u);  // 800b7a70
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7a74
        v0 = g.U16(a1 + 0u);  // 800b7a78
        g.W16(t0 + 6u, static_cast<uint16_t>(v0));  // 800b7a80
        v0 = g.U16(a1 + 2u);  // 800b7a84
        g.W16(t0 + 8u, static_cast<uint16_t>(v0));  // 800b7a8c
        v0 = g.U16(a1 + 4u);  // 800b7a90
        g.W16(t0 + 10u, static_cast<uint16_t>(v0));  // 800b7a98
        v0 = g.U16(a1 + 0u);  // 800b7a9c
        g.W16(t0 + 18u, static_cast<uint16_t>(v0));  // 800b7aa4
        v0 = g.U16(a1 + 2u);  // 800b7aa8
        g.W16(t0 + 20u, static_cast<uint16_t>(v0));  // 800b7ab0
        a0 = g.U16(a1 + 4u);  // 800b7ab4
        v0 = g.U16(t0 + 0u);  // 800b7ab8
        v1 = 0u - v1;  // 800b7abc
        g.W16(t0 + 2u, static_cast<uint16_t>(v1));  // 800b7ac0
        v1 = g.U16(t0 + 6u);  // 800b7ac4
        v0 = 0u - v0;  // 800b7ac8
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7acc
        v0 = g.U16(t0 + 4u);  // 800b7ad0
        v1 = 0u - v1;  // 800b7ad4
        g.W16(t0 + 6u, static_cast<uint16_t>(v1));  // 800b7ad8
        v1 = g.U16(t0 + 10u);  // 800b7adc
        g.W16(t0 + 22u, static_cast<uint16_t>(a0));  // 800b7ae0
        v0 = 0u - v0;  // 800b7ae4
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7aec
        goto L7FFC;
    case 2: // 0x800B7AF0
        v0 = g.U32(a0 + 12u);  // 800b7af0
        g.W32(a3 + 0u, v0);  // 800b7af8
        v0 = g.U32(a0 + 16u);  // 800b7afc
        g.W32(a3 + 4u, v0);  // 800b7b04
        v0 = g.U32(a0 + 20u);  // 800b7b08
        g.W32(a3 + 8u, v0);  // 800b7b10
        v0 = g.U32(a0 + 24u);  // 800b7b14
        g.W32(a3 + 12u, v0);  // 800b7b1c
        v0 = g.U32(a0 + 28u);  // 800b7b20
        g.W32(a3 + 16u, v0);  // 800b7b28
        v0 = g.U32(a0 + 32u);  // 800b7b2c
        g.W32(a3 + 20u, v0);  // 800b7b34
        v0 = g.U32(a0 + 72u);  // 800b7b38
        g.W32(a3 + 24u, v0);  // 800b7b40
        v0 = g.U32(a0 + 76u);  // 800b7b44
        g.W32(a3 + 28u, v0);  // 800b7b4c
        v0 = g.U32(a0 + 80u);  // 800b7b50
        g.W32(a3 + 32u, v0);  // 800b7b58
        v0 = g.U32(a0 + 60u);  // 800b7b5c
        g.W32(a3 + 36u, v0);  // 800b7b64
        v0 = g.U32(a0 + 64u);  // 800b7b68
        g.W32(a3 + 40u, v0);  // 800b7b70
        v0 = g.U32(a0 + 68u);  // 800b7b74
        g.W32(a3 + 44u, v0);  // 800b7b7c
        v0 = g.U16(a1 + 6u);  // 800b7b80
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7b88
        v0 = g.U16(a1 + 8u);  // 800b7b8c
        g.W16(t0 + 2u, static_cast<uint16_t>(v0));  // 800b7b94
        v0 = g.U16(a1 + 10u);  // 800b7b98
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7ba0
        v0 = g.U16(a1 + 6u);  // 800b7ba4
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b7bac
        v0 = g.U16(a1 + 8u);  // 800b7bb0
        g.W16(t0 + 14u, static_cast<uint16_t>(v0));  // 800b7bb8
        v0 = g.U16(a1 + 10u);  // 800b7bbc
        v1 = g.U16(t0 + 2u);  // 800b7bc0
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7bc4
        v0 = g.U16(a1 + 12u);  // 800b7bc8
        g.W16(t0 + 6u, static_cast<uint16_t>(v0));  // 800b7bd0
        v0 = g.U16(a1 + 14u);  // 800b7bd4
        g.W16(t0 + 8u, static_cast<uint16_t>(v0));  // 800b7bdc
        v0 = g.U16(a1 + 16u);  // 800b7be0
        g.W16(t0 + 10u, static_cast<uint16_t>(v0));  // 800b7be8
        v0 = g.U16(a1 + 12u);  // 800b7bec
        g.W16(t0 + 18u, static_cast<uint16_t>(v0));  // 800b7bf4
        v0 = g.U16(a1 + 14u);  // 800b7bf8
        g.W16(t0 + 20u, static_cast<uint16_t>(v0));  // 800b7c00
        a0 = g.U16(a1 + 16u);  // 800b7c04
        v0 = g.U16(t0 + 0u);  // 800b7c08
        v1 = 0u - v1;  // 800b7c0c
        g.W16(t0 + 2u, static_cast<uint16_t>(v1));  // 800b7c10
        v1 = g.U16(t0 + 6u);  // 800b7c14
        v0 = 0u - v0;  // 800b7c18
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7c1c
        v0 = g.U16(t0 + 4u);  // 800b7c20
        v1 = 0u - v1;  // 800b7c24
        g.W16(t0 + 6u, static_cast<uint16_t>(v1));  // 800b7c28
        v1 = g.U16(t0 + 10u);  // 800b7c2c
        g.W16(t0 + 22u, static_cast<uint16_t>(a0));  // 800b7c30
        v0 = 0u - v0;  // 800b7c34
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7c3c
        goto L7FFC;
    case 0: // 0x800B7C40
        v0 = g.U32(a0 + 36u);  // 800b7c40
        g.W32(a3 + 0u, v0);  // 800b7c48
        v0 = g.U32(a0 + 40u);  // 800b7c4c
        g.W32(a3 + 4u, v0);  // 800b7c54
        v0 = g.U32(a0 + 44u);  // 800b7c58
        g.W32(a3 + 8u, v0);  // 800b7c60
        v0 = g.U32(a0 + 0u);  // 800b7c64
        g.W32(a3 + 12u, v0);  // 800b7c6c
        v0 = g.U32(a0 + 4u);  // 800b7c70
        g.W32(a3 + 16u, v0);  // 800b7c78
        v0 = g.U32(a0 + 8u);  // 800b7c7c
        g.W32(a3 + 20u, v0);  // 800b7c84
        v0 = g.U32(a0 + 48u);  // 800b7c88
        g.W32(a3 + 24u, v0);  // 800b7c90
        v0 = g.U32(a0 + 52u);  // 800b7c94
        g.W32(a3 + 28u, v0);  // 800b7c9c
        v0 = g.U32(a0 + 56u);  // 800b7ca0
        g.W32(a3 + 32u, v0);  // 800b7ca8
        v0 = g.U32(a0 + 84u);  // 800b7cac
        g.W32(a3 + 36u, v0);  // 800b7cb4
        v0 = g.U32(a0 + 88u);  // 800b7cb8
        g.W32(a3 + 40u, v0);  // 800b7cc0
        v0 = g.U32(a0 + 92u);  // 800b7cc4
        g.W32(a3 + 44u, v0);  // 800b7ccc
        v0 = g.U16(a1 + 6u);  // 800b7cd0
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7cd8
        v0 = g.U16(a1 + 8u);  // 800b7cdc
        g.W16(t0 + 2u, static_cast<uint16_t>(v0));  // 800b7ce4
        v0 = g.U16(a1 + 10u);  // 800b7ce8
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7cf0
        v0 = g.U16(a1 + 6u);  // 800b7cf4
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b7cfc
        v0 = g.U16(a1 + 8u);  // 800b7d00
        g.W16(t0 + 14u, static_cast<uint16_t>(v0));  // 800b7d08
        v0 = g.U16(a1 + 10u);  // 800b7d0c
        v1 = g.U16(t0 + 2u);  // 800b7d10
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7d14
        v0 = g.U16(a1 + 12u);  // 800b7d18
        g.W16(t0 + 6u, static_cast<uint16_t>(v0));  // 800b7d20
        v0 = g.U16(a1 + 14u);  // 800b7d24
        g.W16(t0 + 8u, static_cast<uint16_t>(v0));  // 800b7d2c
        v0 = g.U16(a1 + 16u);  // 800b7d30
        g.W16(t0 + 10u, static_cast<uint16_t>(v0));  // 800b7d38
        v0 = g.U16(a1 + 12u);  // 800b7d3c
        g.W16(t0 + 18u, static_cast<uint16_t>(v0));  // 800b7d44
        v0 = g.U16(a1 + 14u);  // 800b7d48
        g.W16(t0 + 20u, static_cast<uint16_t>(v0));  // 800b7d50
        a0 = g.U16(a1 + 16u);  // 800b7d54
        goto L7954;
    case 4: // 0x800B7D60
        v0 = g.U32(a0 + 12u);  // 800b7d60
        g.W32(a3 + 0u, v0);  // 800b7d68
        v0 = g.U32(a0 + 16u);  // 800b7d6c
        g.W32(a3 + 4u, v0);  // 800b7d74
        v0 = g.U32(a0 + 20u);  // 800b7d78
        g.W32(a3 + 8u, v0);  // 800b7d80
        v0 = g.U32(a0 + 0u);  // 800b7d84
        g.W32(a3 + 12u, v0);  // 800b7d8c
        v0 = g.U32(a0 + 4u);  // 800b7d90
        g.W32(a3 + 16u, v0);  // 800b7d98
        v0 = g.U32(a0 + 8u);  // 800b7d9c
        g.W32(a3 + 20u, v0);  // 800b7da4
        v0 = g.U32(a0 + 36u);  // 800b7da8
        g.W32(a3 + 24u, v0);  // 800b7db0
        v0 = g.U32(a0 + 40u);  // 800b7db4
        g.W32(a3 + 28u, v0);  // 800b7dbc
        v0 = g.U32(a0 + 44u);  // 800b7dc0
        g.W32(a3 + 32u, v0);  // 800b7dc8
        v0 = g.U32(a0 + 24u);  // 800b7dcc
        g.W32(a3 + 36u, v0);  // 800b7dd4
        v0 = g.U32(a0 + 28u);  // 800b7dd8
        g.W32(a3 + 40u, v0);  // 800b7de0
        v0 = g.U32(a0 + 32u);  // 800b7de4
        g.W32(a3 + 44u, v0);  // 800b7dec
        v0 = g.U16(a1 + 12u);  // 800b7df0
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7df8
        v0 = g.U16(a1 + 14u);  // 800b7dfc
        g.W16(t0 + 2u, static_cast<uint16_t>(v0));  // 800b7e04
        v0 = g.U16(a1 + 16u);  // 800b7e08
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7e10
        v0 = g.U16(a1 + 12u);  // 800b7e14
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b7e1c
        v0 = g.U16(a1 + 14u);  // 800b7e20
        g.W16(t0 + 14u, static_cast<uint16_t>(v0));  // 800b7e28
        v0 = g.U16(a1 + 16u);  // 800b7e2c
        v1 = g.U16(t0 + 14u);  // 800b7e30
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7e34
        v0 = g.U16(a1 + 0u);  // 800b7e38
        g.W16(t0 + 6u, static_cast<uint16_t>(v0));  // 800b7e40
        v0 = g.U16(a1 + 2u);  // 800b7e44
        g.W16(t0 + 8u, static_cast<uint16_t>(v0));  // 800b7e4c
        v0 = g.U16(a1 + 4u);  // 800b7e50
        g.W16(t0 + 10u, static_cast<uint16_t>(v0));  // 800b7e58
        v0 = g.U16(a1 + 0u);  // 800b7e5c
        g.W16(t0 + 18u, static_cast<uint16_t>(v0));  // 800b7e64
        v0 = g.U16(a1 + 2u);  // 800b7e68
        g.W16(t0 + 20u, static_cast<uint16_t>(v0));  // 800b7e70
        a0 = g.U16(a1 + 4u);  // 800b7e74
        v0 = g.U16(t0 + 12u);  // 800b7e78
        v1 = 0u - v1;  // 800b7e7c
        g.W16(t0 + 14u, static_cast<uint16_t>(v1));  // 800b7e80
        v1 = g.U16(t0 + 18u);  // 800b7e84
        v0 = 0u - v0;  // 800b7e88
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b7e8c
        v0 = g.U16(t0 + 16u);  // 800b7e90
        v1 = 0u - v1;  // 800b7e94
        g.W16(t0 + 18u, static_cast<uint16_t>(v1));  // 800b7e98
        g.W16(t0 + 22u, static_cast<uint16_t>(a0));  // 800b7e9c
        v1 = a0;  // 800b7ea0
        v0 = 0u - v0;  // 800b7ea4
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7eac
        goto L7988;
    case 5: // 0x800B7EB0
        v0 = g.U32(a0 + 48u);  // 800b7eb0
        g.W32(a3 + 0u, v0);  // 800b7eb8
        v0 = g.U32(a0 + 52u);  // 800b7ebc
        g.W32(a3 + 4u, v0);  // 800b7ec4
        v0 = g.U32(a0 + 56u);  // 800b7ec8
        g.W32(a3 + 8u, v0);  // 800b7ed0
        v0 = g.U32(a0 + 60u);  // 800b7ed4
        g.W32(a3 + 12u, v0);  // 800b7edc
        v0 = g.U32(a0 + 64u);  // 800b7ee0
        g.W32(a3 + 16u, v0);  // 800b7ee8
        v0 = g.U32(a0 + 68u);  // 800b7eec
        g.W32(a3 + 20u, v0);  // 800b7ef4
        v0 = g.U32(a0 + 72u);  // 800b7ef8
        g.W32(a3 + 24u, v0);  // 800b7f00
        v0 = g.U32(a0 + 76u);  // 800b7f04
        g.W32(a3 + 28u, v0);  // 800b7f0c
        v0 = g.U32(a0 + 80u);  // 800b7f10
        g.W32(a3 + 32u, v0);  // 800b7f18
        v0 = g.U32(a0 + 84u);  // 800b7f1c
        g.W32(a3 + 36u, v0);  // 800b7f24
        v0 = g.U32(a0 + 88u);  // 800b7f28
        g.W32(a3 + 40u, v0);  // 800b7f30
        v0 = g.U32(a0 + 92u);  // 800b7f34
        g.W32(a3 + 44u, v0);  // 800b7f3c
        v0 = g.U16(a1 + 12u);  // 800b7f40
        g.W16(t0 + 0u, static_cast<uint16_t>(v0));  // 800b7f48
        v0 = g.U16(a1 + 14u);  // 800b7f4c
        g.W16(t0 + 2u, static_cast<uint16_t>(v0));  // 800b7f54
        v0 = g.U16(a1 + 16u);  // 800b7f58
        g.W16(t0 + 4u, static_cast<uint16_t>(v0));  // 800b7f60
        v0 = g.U16(a1 + 12u);  // 800b7f64
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b7f6c
        v0 = g.U16(a1 + 14u);  // 800b7f70
        g.W16(t0 + 14u, static_cast<uint16_t>(v0));  // 800b7f78
        v0 = g.U16(a1 + 16u);  // 800b7f7c
        v1 = g.U16(t0 + 14u);  // 800b7f80
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7f84
        v0 = g.U16(a1 + 0u);  // 800b7f88
        g.W16(t0 + 6u, static_cast<uint16_t>(v0));  // 800b7f90
        v0 = g.U16(a1 + 2u);  // 800b7f94
        g.W16(t0 + 8u, static_cast<uint16_t>(v0));  // 800b7f9c
        v0 = g.U16(a1 + 4u);  // 800b7fa0
        g.W16(t0 + 10u, static_cast<uint16_t>(v0));  // 800b7fa8
        v0 = g.U16(a1 + 0u);  // 800b7fac
        g.W16(t0 + 18u, static_cast<uint16_t>(v0));  // 800b7fb4
        v0 = g.U16(a1 + 2u);  // 800b7fb8
        g.W16(t0 + 20u, static_cast<uint16_t>(v0));  // 800b7fc0
        a0 = g.U16(a1 + 4u);  // 800b7fc4
        v0 = g.U16(t0 + 12u);  // 800b7fc8
        v1 = 0u - v1;  // 800b7fcc
        g.W16(t0 + 14u, static_cast<uint16_t>(v1));  // 800b7fd0
        v1 = g.U16(t0 + 6u);  // 800b7fd4
        v0 = 0u - v0;  // 800b7fd8
        g.W16(t0 + 12u, static_cast<uint16_t>(v0));  // 800b7fdc
        v0 = g.U16(t0 + 16u);  // 800b7fe0
        v1 = 0u - v1;  // 800b7fe4
        g.W16(t0 + 6u, static_cast<uint16_t>(v1));  // 800b7fe8
        v1 = g.U16(t0 + 10u);  // 800b7fec
        g.W16(t0 + 22u, static_cast<uint16_t>(a0));  // 800b7ff0
        v0 = 0u - v0;  // 800b7ff4
        g.W16(t0 + 16u, static_cast<uint16_t>(v0));  // 800b7ff8
    L7FFC:
        v0 = g.U16(t0 + 8u);  // 800b7ffc
        v1 = 0u - v1;  // 800b8000
        g.W16(t0 + 10u, static_cast<uint16_t>(v1));  // 800b8004
        v0 = 0u - v0;  // 800b8008
        g.W16(t0 + 8u, static_cast<uint16_t>(v0));  // 800b800c
        return !g.Faulted();
    default:
        return !g.Faulted();
    }
}

// ============================================================================ RASHCDG 0x800B74F0
bool DeepestThroughFace(GuestRam& g, uint32_t pts, uint32_t dir, int32_t len, uint32_t box, uint32_t rows,
                        uint32_t mask, uint32_t outFace, uint32_t outDist, uint32_t sp, uint32_t& v0) {
    const uint32_t fr = sp - 576;
    const uint32_t d16 = fr + 456, hit = fr + 464, n16 = fr + 480;
    g.W32(fr + 488, kOrg);
    g.W32(sp + 0, pts);                                                       // the home stores
    g.W32(sp + 8, U(len));
    g.W32(sp + 12, box);
    uint32_t fbPt = 8, fbFace = 6;
    int32_t fbDist = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        const uint16_t c = g.U16(dir + 2 * k);
        g.W16(d16 + 2 * k, len >= 0 ? static_cast<uint16_t>(0u - c) : c);
    }
    const int32_t alen = Iabs(len);
    g.W32(sp + 8, U(alen));
    int32_t bestD = 0;
    uint32_t bestP = 8;
    for (uint32_t f = 0; f < 6; ++f) {
        if ((mask >> f) & 1u) continue;
        const uint32_t q = fr + 24 + 48 * f, en = fr + 312 + 24 * f;
        g.W32(fr + 16, en);                                                   // the stack argument
        if (!FaceQuad(g, box, rows, f, q, en)) return false;
        g.W32(fr + 16, 0);
        if (!FaceNormal(g, box, rows, f, n16, 0, fr)) return false;
        const uint32_t org = box + 12u * FaceOrg(f);
        const uint16_t n0 = g.U16(n16 + 0), n2 = g.U16(n16 + 4);
        g.W16(n16 + 0, static_cast<uint16_t>(0u - n0));
        const uint16_t n1 = g.U16(n16 + 2);
        g.W16(n16 + 4, static_cast<uint16_t>(0u - n2));
        g.W16(n16 + 2, static_cast<uint16_t>(0u - n1));
        for (int32_t k = 7; k >= 0; --k) {
            const uint32_t p = pts + 12u * U(k);
            if (GProject(g, p, n16, org) < 0) continue;
            int32_t t = RayPlane(g, p, d16, n16, org);
            if (t < -2048) continue;
            if (Add(alen, 2048) < t) continue;
            GMulAdd(g, p, d16, Neg(t), hit);
            g.W32(fr + 16, 2048);
            uint32_t in = 0;
            if (!PointInPoly(g, hit, q, en, 4, 2048, fr, in)) return false;
            if (in == 0) continue;
            if (!(2048 < t)) t = 2048;
            if (bestD < t) {
                bestD = t;
                bestP = U(k);
            }
        }
        if (bestP < 8u) {
            if (GDot(g, n16, d16) < 4097) {
                g.W32(outFace, f);
                g.W32(outDist, U(bestD));
                v0 = bestP;
                return !g.Faulted();
            }
            fbFace = f;
            fbPt = bestP;
            fbDist = bestD;
            g.W32(fr + 496, fbFace);
            g.W32(fr + 492, fbPt);
            g.W32(fr + 500, U(fbDist));
        }
    }
    g.W32(outFace, fbFace);
    g.W32(outDist, U(fbDist));
    v0 = fbPt;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B2C98
bool NoteBigVolume(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp) {
    (void)sp;
    if (!(g.S32(kBigCount) < 8)) return !g.Faulted();
    if (InCameraBox(g, e, 0x140000, 0x1C0000) == 0) return !g.Faulted();
    const int32_t n = g.S32(kBigCount);
    if (n > 0) {
        const uint8_t id = g.U8(shape);
        int32_t i = 0;
        do {
            if (g.U8(kBigIds + U(i)) == id) return !g.Faulted();
            ++i;
        } while (i < n);
    }
    const int32_t cnt = g.S32(kBigCount);
    const uint8_t id = g.U8(shape);
    g.W8(kBigIds + U(cnt), id);
    g.W32(kBigCount, U(Add(cnt, 1)));
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B09C4
bool PointResolve(GuestRam& g, uint32_t e, uint32_t s, uint32_t sp, const BikeTables& t, CollisionCallees& c) {
    const uint32_t fr = sp - 608;
    uint32_t st = 0;
    const bool on = g.U32(e + 832) == s;
    g.W32(fr + 568, 0);
    if (on) st = g.U8(e + 565) >> 7;
    if (g.U32(e + 828) == s && st == 0) return !g.Faulted();
    const uint32_t h = g.U16(s);
    uint32_t o;
    if ((h >> 5) >= 5u) {                                                    // a fake entity at sp+24
        for (uint32_t i = 0; i < 172; i += 4) g.W32(fr + 24 + i, 0);          // SLUS 0x8001E100
        GuestCopyWords(g, fr + 196, s, 280);                                  // SLUS 0x8001E0B4
        g.W16(fr + 474, g.U16(s + 272));
        g.W16(fr + 476, g.U16(s + 274));
        const uint16_t h6 = g.U16(s + 276);
        o = fr + 24;
        for (uint32_t k : {504u, 480u, 484u, 488u, 512u, 516u, 520u}) g.W32(fr + k, 0);
        g.W16(fr + 478, h6);
    } else {
        const uint32_t pt = kPoolTableAddr + 16u * (h >> 5);
        o = U(Add(g.S32(pt), MulLo(g.S32(pt + 4), S(h & 0x1Fu))));
    }
    int32_t stale = 0;
    if (!StaleHeading(g, e, t, stale)) return false;
    uint32_t who = 0;
    if (InCameraBox(g, e, 0x140000, 0x1C0000) != 0) {
        g.W32(fr + 16, fr + 528);
        g.W32(fr + 20, fr + 568);
        if (!Call(c, rsv::kResponse, {e, o, fr + 560, fr + 564, fr + 528, fr + 568}, fr, &who)) return false;
        if (who != 0 && who == g.U32(e + 856)) who = e;
        const int32_t mag = g.S32(fr + 568);
        if (mag > 0) {
            const int32_t v = FixMul(g.S32(e + 480), mag);
            GScale(g, v, e + 450, fr + 544);
            ApplyImpulse(g, e, fr + 544, 1);
            if ((g.U16(s) >> 5) < 5u) ApplyImpulse(g, o, fr + 544, 1);
            MaxU228(g, e, g.U32(fr + 568));
        }
    } else {
        if (stale != 0) RebuildHeading(g, e, t);
        g.W32(fr + 16, fr + 528);
        if (!Call(c, rsv::kResponseFar, {e, o, fr + 560, fr + 564, fr + 528}, fr, &who)) return false;
    }
    // 0x800B0C30
    const uint32_t code = g.U32(fr + 560);
    if (code != 0 || st != 0) {
        if (who != e) {
            const uint16_t w0 = g.U16(fr + 528), w1 = g.U16(fr + 532);
            g.W32(fr + 528, U(static_cast<int16_t>(static_cast<uint16_t>(0u - w0))));
            const uint16_t w2 = g.U16(fr + 536);
            g.W32(fr + 532, U(static_cast<int16_t>(static_cast<uint16_t>(0u - w1))));
            g.W32(fr + 536, U(static_cast<int16_t>(static_cast<uint16_t>(0u - w2))));
        }
        g.W32(fr + 16, fr + 528);
        uint32_t r = 0;
        if (!Call(c, rsv::kImpactGate, {e, s, code, g.U32(fr + 564), fr + 528}, fr, &r)) return false;
        g.W32(fr + 560, r);
    }
    if (g.U32(fr + 560) == 0) return !g.Faulted();
    int32_t d;
    const uint32_t road = g.U32(s + 188);
    if (road == g.U32(e + 360) && ((road >> 16) == 0 || g.U32(s + 164) == g.U32(e + 336))) {
        d = Sub(g.S32(s + 172), g.S32(e + 344));
        if (g.S32(e + 364) < 0) d = Neg(d);
    } else {
        d = GProject(g, s + 12, e + 432, e + 184);
    }
    const int32_t hw = g.S32(e + 304);
    int32_t side = d < Neg(hw) ? 0 : 3;
    if (hw < d) side -= 1;
    g.W32(fr + 16, U(side));
    if (!Call(c, rsv::kImpactSeverity, {e, 0, 0, 0x10000, U(side)}, fr)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B0D8C
bool BoxResolve(GuestRam& g, uint32_t e, uint32_t s, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                uint32_t& v0) {
    const uint32_t fr = sp - 96;
    int32_t toi = 0;                                                          // s1, NOT reset per pass
    if ((g.U16(s) >> 5) == 6u && 0x20000 < g.S32(s + 140) && g.U32(s + 8) != 1u)
        if (!NoteBigVolume(g, e, s, fr)) return false;
    const uint32_t st = Sticky(g, e, s);
    if (g.U32(e + 828) == s && st == 0) {
        v0 = s;
        return !g.Faulted();
    }
    int32_t stale = 0;
    if (!StaleHeading(g, e, t, stale)) return false;
    const uint32_t mask = (g.U32(e + 568) & 0x600u) ? 16u : 48u;
    v0 = 1;
    for (;;) {
        // 0x800B0E60
        g.W32(fr + 16, fr + 48);
        g.W32(fr + 20, fr + 52);
        uint32_t p = 0;
        if (!FirstPointInsideBox(g, e + 196, s + 260, s + 24, mask, fr + 48, fr + 52, fr, p)) return false;
        const uint32_t hit = p == 8 ? 0u : (p | 0x100u);
        uint32_t again = 0;
        if (g.U32(e + 856) != 0 && g.U32(e + 1088) != 0 && st == 0)
            if (p == 8 || (p & 1u) != ((p & 2u) >> 1)) again = 1;
        if (hit != 0) {
            g.W32(fr + 16, 0);
            if (!FaceNormal(g, s + 24, s + 260, g.U32(fr + 48), fr + 24, 0, fr)) return false;
            GScale(g, Add(g.S32(fr + 52), 8192), fr + 24, fr + 32);
            if (g.U16(e + 172) < g.U32(GameState(g) + 48) && !(g.S32(e + 480) < 132)) {
                const int32_t v = Neg(GDot(g, fr + 24, e + 450));
                if (v > 0) {
                    const int32_t w = FixMul(v, g.S32(e + 480));
                    if (!(w < 132)) {
                        const int32_t dep = g.S32(fr + 52);
                        if (dep < w) toi = SDiv(dep, w);
                    }
                    const int32_t dt = g.S32(kCollDt);
                    toi = toi < dt ? toi : dt;
                    MaxU228(g, e, U(toi));
                }
            }
        }
        if (st != 0) {
            g.W32(fr + 40, 0);
            g.W32(fr + 36, 0);
            g.W32(fr + 32, 0);
            g.W16(fr + 28, 0);
            g.W16(fr + 26, 0);
            g.W16(fr + 24, 0);
        }
        if (hit != 0 || st != 0) {
            bool log = false;
            int32_t n = 0;
            if (st == 0 && ((g.U16(e + 320) & 4u) || g.U32(e + 856) != 0)) {
                n = g.S32(kContactCount);
                log = n < 8;
            }
            if (!log) {
                again = 0;
                g.W32(fr + 16, fr + 32);
                g.W32(fr + 20, fr + 24);
                if (!Call(c, coll::kBoxReact, {e, s, hit, g.U32(fr + 48) | 0x200u, fr + 32, fr + 24}, fr)) return false;
            } else {
                // 0x800B10A4: log a contact record
                if (g.U32(e + 1088) == 0) {
                    again = 2;
                    e = g.U32(e + 856);                                       // 0 reads *(u16*)0xAC below
                }
                const uint32_t q = kContactList + 36u * U(n);
                g.W16(q + 0, g.U16(e + 172));
                g.W16(q + 2, g.U16(s));
                g.W16(q + 4, g.U16(fr + 24));
                g.W16(q + 6, g.U16(fr + 26));
                g.W16(q + 8, g.U16(fr + 28));
                g.W32(q + 16, g.U32(fr + 32));
                const uint32_t face = g.U32(fr + 48);
                g.W32(q + 20, g.U32(fr + 36));
                const uint32_t f40 = g.U32(fr + 40);
                g.W32(q + 28, hit | ((face | 0x200u) << 16));
                g.W32(q + 32, g.U32(fr + 52));
                g.W32(q + 24, f40);
                if (again == 2) {
                    const int32_t r = ContactMerge(g, q);
                    g.W32(kContactCount, U(Add(g.S32(kContactCount), r)));
                } else {
                    g.W32(kContactCount, U(Add(n, 1)));
                }
            }
        }
        if (again != 1) break;
        e = g.U32(e + 856);
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800AE794
bool PoleResolve(GuestRam& g, uint32_t e, uint32_t s, uint32_t sp, const BikeTables& t, CollisionCallees& c) {
    const uint32_t fr = sp - 152;
    g.W32(sp + 4, s);                                                         // sw a1,156(sp)
    const bool on = g.U32(e + 832) == s;
    g.W32(fr + 96, 0);
    if (on) g.W32(fr + 96, g.U8(e + 565) >> 7);
    if (g.U32(e + 828) == g.U32(sp + 4) && g.U32(fr + 96) == 0) return !g.Faulted();
    {
        int32_t stale = 0;
        if (!StaleHeading(g, e, t, stale)) return false;
        g.W32(fr + 100, U(stale));
    }
    int32_t top, bot;
    if (g.U32(e + 568) & 0x600u) {
        top = bot = g.S32(e + 200);
        for (uint32_t k = 1; k < 8; ++k) {
            const int32_t y = g.S32(e + 200 + 12 * k);
            if (top < y) top = y;
            else if (y < bot) bot = y;
        }
    } else {
        top = g.S32(e + 200);
        bot = g.S32(e + 248);
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t y = g.S32(e + 212 + 12 * k);
            if (top < y) top = y;
            const int32_t z = g.S32(e + 260 + 12 * k);
            if (z < bot) bot = z;
        }
    }
    const uint32_t sh = g.U32(sp + 4);
    const int32_t cy = g.S32(e + 508);
    if (g.S32(sh + 140) < Sub(cy, top)) return !g.Faulted();
    const int32_t hb = Sub(cy, bot);
    if ((g.U16(sh) >> 5) == 6u) {
        const int32_t f58 = g.S32(sh + 88);
        g.W32(fr + 104, 0);
        const int32_t f54 = g.S32(sh + 84), f84 = g.S32(sh + 132);
        const uint32_t hi = f58 < hb ? 1u : 0u;
        const int32_t r = Add(Add(f84, S((0u - hi) & U(Sub(f54, f84)))), -16);
        g.W32(fr + 92, U(r));
        if (0x10000 < r) g.W32(fr + 104, hi & 1u);
    } else {
        int32_t a = g.S32(sh + 136);
        const int32_t b = g.S32(sh + 132);
        g.W32(fr + 104, 0);
        if (b < a) a = b;
        g.W32(fr + 92, U(Add(a, -16)));
    }
    uint32_t s6 = 0, s7 = 0, s8 = 0;
    g.W16(fr + 50, 0);                                                        // once, before the loop head
    do {
        // 0x800AE970
        s6 = 6;
        uint32_t two = 0;
        if (g.U32(e + 856) != 0) two = g.U32(e + 1088) != 0 ? 1u : 0u;
        g.W32(fr + 88, 0);
        s8 = two & (g.U32(fr + 96) == 0 ? 1u : 0u);
        bool took = false;
        if (InCameraBox(g, e, 0x140000, 0x1C0000) != 0) {
            const int32_t lean = g.S32(e + 676);
            if (S(0xFFFE6DE1u) < lean && !(0x1921E < lean) && lean != 0) {
                g.W32(fr + 16, fr + 88);
                if (!Call(c, rsv::kLeanPoleTest, {e, g.U32(sp + 4), g.U32(fr + 92), fr + 24, fr + 88}, fr, &s6))
                    return false;
                s7 = s6 < 6u ? 1u : 0u;
                took = true;
            }
        }
        if (!took) {
            s7 = 0;
            if (g.U32(fr + 100) != 0) RebuildHeading(g, e, t);
            const uint32_t sa = g.U32(sp + 4);
            const int32_t r92 = g.S32(fr + 92);
            const int32_t s4 = Add(g.S32(e + 304), r92), s5 = Add(g.S32(e + 308), r92);
            const int32_t lg = Add(FixMul(g.S32(e + 300), Sub(g.S32(sa + 12), g.S32(e + 184))),
                                   FixMul(g.S32(e + 296), Sub(g.S32(sa + 20), g.S32(e + 192))));
            const int32_t lt = Add(FixMul(g.S32(e + 296), Sub(g.S32(sa + 12), g.S32(e + 184))),
                                   FixMul(Neg(g.S32(e + 300)), Sub(g.S32(sa + 20), g.S32(e + 192))));
            const int32_t spd = g.S32(e + 480);
            const uint32_t fast = 0x100000 < spd ? 1u : 0u;
            const int32_t step = S((0u - fast) & U(FixMul(g.S32(kCollDt), spd)));
            if (lg < s5 && Sub(Neg(s5), step) < lg) {
                const int32_t ang = g.S32(e + 636);
                const uint32_t off = (U(MulLo(ang, 163)) >> 12) & 0x3FFCu;
                const int32_t sn = Shl(g.S16(kSinCos + off), 4);
                g.W32(fr + 88, U(sn));
                const int32_t v = FixMul(sn, g.S32(e + 312));
                g.W32(fr + 88, U(v));
                if (g.S32(e + 636) < 0) {
                    if (lt < s4) s7 = Sub(v, s4) < lt ? 1u : 0u;
                } else {
                    if (lt < Add(s4, v)) s7 = Neg(s4) < lt ? 1u : 0u;
                }
            }
            if (s7 != 0) {
                if (Sub(Iabs(lt), g.S32(fr + 92)) < 8519) {                   // an END contact
                    const int32_t v1 = Sub(Neg(s5), s5);
                    const int32_t v0 = lg > 0 ? Add(s5, v1) : Add(s5, S((0u - fast) & U(v1)));
                    g.W32(fr + 88, U(Add(lg, v0)));
                    s6 = lg > 0 ? 3u : 2u * fast + 1u;
                    g.W32(fr + 24, U(FixMul(g.S32(e + 300), g.S32(fr + 88))));
                    g.W32(fr + 32, U(FixMul(g.S32(e + 296), g.S32(fr + 88))));
                } else {                                                      // a SIDE contact
                    g.W32(fr + 88, U(lt >= 0 ? Sub(lt, s4) : Add(lt, s4)));
                    s6 = U(S(~U(lt)) >> 31) & 2u;
                    g.W32(fr + 24, U(FixMul(g.S32(e + 296), g.S32(fr + 88))));
                    g.W32(fr + 32, U(Neg(FixMul(g.S32(e + 300), g.S32(fr + 88)))));
                }
                g.W32(fr + 28, 0);
            }
        }
        // 0x800AEC90
        s8 &= (s7 == 0 ? 1u : 0u) | (s6 == 2 ? 1u : 0u) | (s6 == 3 ? 1u : 0u);
        if (s7 != 0) {
            const uint32_t n = e + (((s6 & 5u) == 1u) ? 450u : 814u);
            const uint16_t n0 = g.U16(n + 0);
            g.W16(fr + 40, n0);
            const uint16_t n1 = g.U16(n + 2);
            g.W16(fr + 42, n1);
            const uint16_t n2 = g.U16(n + 4);
            g.W16(fr + 44, n2);
            if (s6 & 2u) {
                g.W16(fr + 40, static_cast<uint16_t>(0u - n0));
                g.W16(fr + 42, static_cast<uint16_t>(0u - n1));
                g.W16(fr + 44, static_cast<uint16_t>(0u - n2));
            }
            if (s8 != 0) {                                                    // save the first box's result
                const uint16_t hA = g.U16(e + 172);
                const uint16_t m0 = g.U16(fr + 40), m2 = g.U16(fr + 44);
                const uint32_t p0 = g.U32(fr + 24), p1 = g.U32(fr + 28), p2 = g.U32(fr + 32);
                const int32_t l = g.S32(fr + 88);
                g.W16(fr + 48, hA);
                const uint16_t hs = g.U16(g.U32(sp + 4));
                const uint16_t m1 = g.U16(fr + 42);
                const uint32_t f104 = g.U32(fr + 104);
                g.W16(fr + 52, m0);
                g.W16(fr + 56, m2);
                g.W32(fr + 64, p0);
                g.W32(fr + 68, p1);
                g.W32(fr + 72, p2);
                g.W16(fr + 54, m1);
                g.W32(fr + 80, U(Iabs(l)));
                g.W16(fr + 58, static_cast<uint16_t>(s6 | (f104 << 4)));
                g.W16(fr + 50, hs);
            }
        }
        // 0x800AED84
        if (g.U32(e + 1088) == 0) s8 = 2;
        if (s8 != 0) {
            e = g.U32(e + 856);
            g.W32(fr + 100, 1);
        }
    } while (s8 == 1);
    const uint32_t sb = g.U32(sp + 4);
    if (g.U16(fr + 50) == g.U16(sb)) {
        g.W32(fr + 88, U(Iabs(g.S32(fr + 88))));
        bool take = false;
        if (s7 == 0) {
            take = true;
        } else {
            const uint32_t code = g.U16(fr + 58) & 0xFu;
            if (s6 == 0 || code == 2) {                                       // 0x800AEE08
                s6 = 3;
                g.W16(fr + 40, static_cast<uint16_t>(0u - g.U16(e + 450)));
                g.W16(fr + 42, static_cast<uint16_t>(0u - g.U16(e + 452)));
                g.W16(fr + 44, static_cast<uint16_t>(0u - g.U16(e + 454)));
                const uint32_t other = g.U32(e + 856);
                GMulAdd(g, fr + 24, fr + 40, Sub(g.S32(e + 308), g.S32(other + 308)), fr + 24);
                g.W32(fr + 24, U(Add(g.S32(fr + 24), g.S32(fr + 64))));
                g.W32(fr + 28, U(Add(g.S32(fr + 28), g.S32(fr + 68))));
                g.W32(fr + 32, U(Add(g.S32(fr + 32), g.S32(fr + 72))));
                g.W32(fr + 88, U(Add(g.S32(fr + 88), g.S32(fr + 80))));
            } else if (!(s6 == 3 && code == 3)) {                             // 0x800AEE94
                g.W32(fr + 24, U(Add(g.S32(fr + 24), g.S32(fr + 64))));
                g.W32(fr + 28, U(Add(g.S32(fr + 28), g.S32(fr + 68))));
                g.W32(fr + 32, U(Add(g.S32(fr + 32), g.S32(fr + 72))));
            }
            // 0x800AEED4
            if (g.S32(fr + 88) < g.S32(fr + 80) && s6 == 3 && (g.U16(fr + 58) & 0xFu) == s6) take = true;
        }
        if (take) {                                                           // 0x800AEF10
            s7 = 1;
            const uint16_t m0 = g.U16(fr + 52), m1 = g.U16(fr + 54), m2 = g.U16(fr + 56);
            const uint32_t p0 = g.U32(fr + 64), p1 = g.U32(fr + 68), p2 = g.U32(fr + 72);
            s6 = g.U16(fr + 58) & 0xFu;
            g.W16(fr + 40, m0);
            g.W16(fr + 42, m1);
            g.W16(fr + 44, m2);
            g.W32(fr + 24, p0);
            g.W32(fr + 28, p1);
            g.W32(fr + 32, p2);
        }
        g.W32(fr + 104, g.U32(fr + 104) | (g.U16(fr + 58) >> 4));
    }
    // 0x800AEF60
    if (s7 == 0) return !g.Faulted();
    bool log = false;
    int32_t n = 0;
    if (g.U32(fr + 96) == 0 && (g.U16(g.U32(sp + 4)) >> 5) == 6u && (g.U16(e + 320) & 4u)) {
        n = g.S32(kContactCount);
        log = n < 8;
    }
    if (!log) {
        g.W32(fr + 20, fr + 24);
        g.W32(fr + 16, g.U32(fr + 104));
        if (!Call(c, coll::kPoleReact, {e, g.U32(sp + 4), fr + 40, s6, g.U32(fr + 104), fr + 24}, fr)) return false;
        return !g.Faulted();
    }
    const uint32_t q = kContactList + 36u * U(n);
    g.W16(q + 0, g.U16(e + 172));
    g.W16(q + 2, g.U16(g.U32(sp + 4)));
    g.W16(q + 4, g.U16(fr + 40));
    g.W16(q + 6, g.U16(fr + 42));
    g.W16(q + 8, g.U16(fr + 44));
    g.W32(q + 16, g.U32(fr + 24));
    const uint32_t p1 = g.U32(fr + 28);
    g.W32(kContactCount, U(Add(n, 1)));
    g.W32(q + 20, p1);
    const uint32_t f104 = g.U32(fr + 104);
    const uint32_t p2 = g.U32(fr + 32);
    g.W16(q + 10, static_cast<uint16_t>(s6 | (f104 << 4)));
    g.W32(q + 24, p2);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B2794
bool RiderWallHit(GuestRam& g, uint32_t rider, uint32_t n, uint32_t sp, CollisionCallees& c) {
    const uint32_t fr = sp - 40;
    int32_t a = Iabs(g.S32(rider + 480));
    if (a < 0) a = Add(a, 0x1FFFF);
    const int32_t q = a >> 17;                                                // |speed| / 2^17
    const int32_t lo = S(~U(a >> 31) & U(q));
    const int32_t hi = Sub(4, q);
    const int32_t cls = Add(lo, S(U(hi >> 31) & U(hi)));
    g.W32(fr + 16, rider + 172);
    g.W32(fr + 20, 0);
    uint32_t r = 0;
    if (!Call(c, coll::kPedHit, {rider, n, 0, n, rider + 172, 0}, fr, &r)) return false;
    if (r != 0 && cls > 0)
        if (!c.PlaySound3D(g.S32(rider + 184), g.S32(rider + 192), 19, 0)) return false;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002E604
int32_t Dot32(GuestRam& g, uint32_t a, uint32_t b) {
    const int32_t x = Mid(g.S32(a + 0), g.S32(b + 0));
    const int32_t y = Mid(g.S32(a + 4), g.S32(b + 4));
    const int32_t z = Mid(g.S32(a + 8), g.S32(b + 8));
    return Add(z, Add(y, x));
}

// ============================================================================ RASHCDG 0x800AD04C
namespace {
// The inline 32-bit dot of the frame's s32[3] at `v` with the bike's up row +0x20A << 4.
int32_t UpDot(GuestRam& g, uint32_t v, uint32_t e) {
    const int32_t x = Mid(g.S32(v + 0), Shl(g.S16(e + 522), 4));
    const int32_t y = Mid(g.S32(v + 4), Shl(g.S16(e + 524), 4));
    const int32_t z = Mid(g.S32(v + 8), Shl(g.S16(e + 526), 4));
    return Add(z, Add(y, x));
}
// clamp(|v| / 2^17, 0, 4) with the sra/nor/and idiom (INT32_MIN gives 0).
int32_t SpeedClass(int32_t v) {
    int32_t a = Iabs(v);
    if (a < 0) a = Add(a, 0x1FFFF);
    const int32_t q = a >> 17;
    const int32_t lo = S(~U(a >> 31) & U(q));
    const int32_t hi = Sub(4, q);
    return Add(lo, S(U(hi >> 31) & U(hi)));
}
} // namespace

bool BikeVsRider(GuestRam& g, uint32_t e, uint32_t p, uint32_t sp, const BikeTables& t, CollisionCallees& c) {
    const uint32_t fr = sp - 136;
    const uint32_t ps = p + 172;
    g.W32(fr + 88, 0);
    uint32_t st = Sticky(g, e, ps);                                          // s6
    const uint32_t h = g.U16(p + 172);
    uint32_t rd = 0;                                                          // s4
    if ((h >> 5) == 1u) rd = U(Add(g.S32(0x8005B3A4u), MulLo(628, S(h & 0x1Fu))));
    uint32_t own = 0;                                                         // s3
    if (rd != 0) {
        const uint32_t my = g.U32(e + 852);
        const uint16_t hr = g.U16(rd + 172);
        if (g.U16(my + 172) == hr) own = 1;
        else if ((g.U8(my + 572) & 0x10u) && g.U16(g.U32(g.U32(e + 856) + 852) + 172) == hr) own = 1;
        if ((g.U32(rd + 604) == 2u || (g.U32(e + 568) & 0xECu)) && own != 0) return !g.Faulted();
    }
    if (g.U32(e + 828) == ps && st == 0) return !g.Faulted();
    int32_t stale = 0;
    if (!StaleHeading(g, e, t, stale)) return false;
    uint32_t who = 0;                                                         // s5
    if (InCameraBox(g, e, 0x140000, 0x1C0000) != 0) {
        g.W32(fr + 16, fr + 24);
        g.W32(fr + 20, fr + 88);
        if (!Call(c, rsv::kResponse, {e, p, fr + 80, fr + 84, fr + 24, fr + 88}, fr, &who)) return false;
        if (who != 0 && who == g.U32(e + 856)) who = e;
        const int32_t mag = g.S32(fr + 88);
        if (mag > 0) {
            GScale(g, FixMul(g.S32(e + 480), mag), e + 450, fr + 56);
            ApplyImpulse(g, e, fr + 56, 1);
            ApplyImpulse(g, p, fr + 56, 1);
            MaxU228(g, e, g.U32(fr + 88));
        }
    } else {
        if (stale != 0) RebuildHeading(g, e, t);
        g.W32(fr + 16, fr + 24);
        if (!Call(c, rsv::kResponseFar, {e, p, fr + 80, fr + 84, fr + 24}, fr, &who)) return false;
    }
    if (who != 0) {                                                           // 0x800AD2E4
        const bool mine = g.U16(who + 172) == g.U16(e + 172);
        const uint32_t b = mine ? p : e;
        const uint32_t f = mine ? g.U32(fr + 84) : g.U32(fr + 80);
        g.W32(fr + 16, 0);
        if (!FaceNormal(g, b + 196, b + 432, f & 0xFFu, fr + 72, 0, fr)) return false;
        if (b == p) {
            const uint16_t n0 = g.U16(fr + 72), n2 = g.U16(fr + 76);
            g.W16(fr + 72, static_cast<uint16_t>(0u - n0));
            const uint16_t n1 = g.U16(fr + 74);
            g.W16(fr + 76, static_cast<uint16_t>(0u - n2));
            g.W16(fr + 74, static_cast<uint16_t>(0u - n1));
        }
    }
    // 0x800AD358
    uint32_t s3;
    bool kill = false;
    if (g.U32(fr + 84) != 0 && rd != 0 && own != 0) {
        if (g.U32(e + 480) == 0 && U(Sub(g.U16(rd + 544), 72)) < 2u) {
            kill = true;
        } else {
            const uint32_t f238 = g.U32(e + 568);
            if (f238 & 0xE0u) kill = true;
            else if ((g.U32(rd + 552) & 0x40000000u) && (f238 & 0x600u)) kill = true;
            else if (g.U8(GameState(g) + 57) == 2u && rd == g.U32(g.U32(g.U32(0x8005B38Cu) + 856) + 852)) kill = true;
        }
    }
    if (kill) {
        g.W32(fr + 84, 0);
        s3 = 0;
    } else {
        s3 = g.U32(fr + 84);
    }
    if (s3 != 0 || st != 0) {                                                 // 0x800AD430
        const uint32_t fc = g.U32(e + 568);
        if (!(fc & 0x02000000u)) {
            const uint16_t h0 = g.U16(e + 450), h1 = g.U16(e + 452), h2 = g.U16(e + 454);
            const uint32_t f = g.U32(e + 568) | 0x02000000u;
            const uint32_t spd = g.U32(e + 480);
            g.W16(e + 864, h0);
            g.W16(e + 866, h1);
            g.W16(e + 868, h2);
            g.W32(e + 860, spd);
            g.W32(e + 568, f);
        }
        for (uint32_t k = 0; k < 3; ++k) g.W32(fr + 40 + 4 * k, U(Sub(g.S32(p + 184 + 4 * k), g.S32(e + 184 + 4 * k))));
        GScale(g, g.S32(p + 480), p + 450, fr + 56);
        for (uint32_t k = 0; k < 3; ++k) g.W32(fr + 56 + 4 * k, U(Sub(g.S32(fr + 56 + 4 * k), g.S32(e + 456 + 4 * k))));
        s3 = 0;
        if (st != 0 || !(g.U32(p + 552) & 0x20000000u) || Dot32(g, fr + 40, fr + 56) < 0) s3 = 1;
        if (s3 != 0) {
            if (st != 0 || !(g.U32(p + 552) & 0x40000000u)) {                 // 0x800AD544: p's lowest corner
                int32_t lo = g.S32(p + 284);
                uint32_t k = 7;
                for (int32_t i = 6; i >= 0; --i) {
                    const int32_t y = g.S32(p + 200 + 12u * U(i));
                    if (y < lo) {
                        k = U(i);
                        lo = y;
                    }
                }
                const uint32_t f138 = g.U32(p + 312);
                const uint32_t keep134 = g.U32(p + 308);
                g.W32(p + 308, f138);
                const uint32_t cp = p + 196 + 12 * k;
                g.W32(fr + 40, U(Sub(g.S32(cp + 0), g.S32(p + 184))));
                g.W32(fr + 44, U(Sub(g.S32(cp + 4), g.S32(p + 188))));
                g.W32(fr + 48, U(Sub(g.S32(cp + 8), g.S32(p + 192))));
                const int32_t d = UpDot(g, fr + 40, e);
                g.W32(p + 312, U(d));
                int32_t ad = Iabs(d);
                if (ad < 19660) ad = 19660;
                g.W32(p + 312, U(ad));
                g.W32(fr + 16, 0);
                uint32_t r = 0;
                if (!Call(c, rsv::kImpactGate, {e, ps, g.U32(fr + 80), g.U32(fr + 84), 0}, fr, &r)) return false;
                const uint32_t v = g.U32(p + 308);
                st = 0;
                g.W32(p + 308, keep134);
                g.W32(p + 312, v);
                if (g.U32(e + 832) == ps) st = g.U8(e + 565) >> 7;
                s3 = r;
            } else {                                                          // 0x800AD6C0
                if (rd != 0 && !(g.U32(rd + 552) & 0x20u) && !(g.U32(e + 560) & 0x20000000u)) {
                    if (UpDot(g, fr + 40, e) < S(0xFFFF4000u))
                        if (!Call(c, rsv::kRiderGrab, {rd, e}, fr)) return false;
                }
            }
            if (s3 != 0) {                                                    // 0x800AD798
                const int32_t c0 = FixMul(g.S32(e + 296), g.S32(p + 296));
                const int32_t c1 = FixMul(g.S32(e + 300), g.S32(p + 300));
                const int32_t v = FixMul(Add(c0, c1), g.S32(p + 480));
                const int32_t q = SpeedClass(Sub(g.S32(e + 480), v));
                g.W32(fr + 16, e + 172);
                g.W32(fr + 20, st);
                uint32_t r = 0;
                if (!Call(c, coll::kPedHit, {p, fr + 72, g.U32(e + 860), e + 864, e + 172, st}, fr, &r)) return false;
                if (r != 0 && q > 0) {
                    if (!c.PlaySound3D(g.S32(p + 184), g.S32(p + 192), 19, 0)) return false;
                    if (g.U16(e + 172) < g.U32(GameState(g) + 48) && g.U32(g.U32(e + 852) + 604) < 2u &&
                        g.U32(0x8005B220u) == 0) {
                        g.W32(fr + 16, 2);
                        if (!c.Rumble(e, 0, g.S32(e + 480), 0x0023C361, 2, fr)) return false;
                    }
                    if (g.U16(p + 172) < 64u) {
                        if (!Call(c, rsv::kRiderSpeech, {g.U16(g.U32(rd + 596) + 172), 1}, fr)) return false;
                    } else {
                        if (!Call(c, rsv::kPedVoice, {g.U32(p + 184), g.U32(p + 192), p, 1}, fr)) return false;
                    }
                }
            }
        }
    }
    // 0x800AD8FC
    if (g.U32(fr + 84) == 0) return !g.Faulted();
    if (s3 != 0 && st != 0) return !g.Faulted();
    if (g.U16(0x800541D4u + 8u * g.U16(p + 544) + 2u) == 5u) return !g.Faulted();
    if (g.U16(who + 172) == g.U16(e + 172)) {                                 // who == 0 reads *(u16*)0xAC
        const uint32_t x = g.U32(fr + 24), z = g.U32(fr + 32);
        g.W32(fr + 24, 0u - x);
        const uint32_t y = g.U32(fr + 28);
        g.W32(fr + 32, 0u - z);
        g.W32(fr + 28, 0u - y);
        ApplyImpulse(g, p, fr + 24, 1);
    } else {
        ApplyImpulse(g, who, fr + 24, 1);
    }
    return !g.Faulted();
}

// ============================================================================ the product's dispatch
bool ServeResolvers(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                    bool& ok) {
    auto need = [&](int k) { return call.n >= k; };
    const uint32_t* a = call.a;
    switch (call.fn) {
    case rsv::kFaceNormal:
        ok = need(5) && FaceNormal(g, a[0], a[1], a[2], a[3], a[4], call.sp);
        v0 = 0;
        return true;
    case rsv::kXzProject:
        v0 = need(3) ? U(XzProject(g, a[0], a[1], a[2])) : 0u;
        ok = need(3) && !g.Faulted();
        return true;
    case rsv::kPointInPoly:
        ok = need(5) && PointInPoly(g, a[0], a[1], a[2], S(a[3]), S(a[4]), call.sp, v0);
        return true;
    case rsv::kFirstPointInside:
        ok = need(6) && FirstPointInsideBox(g, a[0], a[1], a[2], a[3], a[4], a[5], call.sp, v0);
        return true;
    case rsv::kDeepestInside:
        ok = need(8) && DeepestInsideAlongDir(g, a[0], a[1], S(a[2]), a[3], a[4], a[5], a[6], a[7], call.sp, v0);
        return true;
    case rsv::kDeepestThrough:
        ok = need(8) && DeepestThroughFace(g, a[0], a[1], S(a[2]), a[3], a[4], a[5], a[6], a[7], call.sp, v0);
        return true;
    case rsv::kFaceQuad:
        ok = need(5) && FaceQuad(g, a[0], a[1], a[2], a[3], a[4]);
        v0 = 0;
        return true;
    case rsv::kNoteBigVolume:
        ok = need(2) && NoteBigVolume(g, a[0], a[1], call.sp);
        v0 = 0;
        return true;
    case coll::kPointResolve:
        ok = need(2) && PointResolve(g, a[0], a[1], call.sp, t, c);
        v0 = 0;
        return true;
    case coll::kBoxResolve:
        v0 = 0;
        ok = need(2) && BoxResolve(g, a[0], a[1], call.sp, t, c, v0);
        return true;
    case coll::kPoleResolve:
        ok = need(2) && PoleResolve(g, a[0], a[1], call.sp, t, c);
        v0 = 0;
        return true;
    case coll::kBikeVsRider:
        ok = need(2) && BikeVsRider(g, a[0], a[1], call.sp, t, c);
        v0 = 0;
        return true;
    case rsv::kDot32:
        v0 = need(2) ? U(Dot32(g, a[0], a[1])) : 0u;
        ok = need(2) && !g.Faulted();
        return true;
    case coll::kRiderWallHit:
        ok = need(2) && RiderWallHit(g, a[0], a[1], call.sp, c);
        v0 = 0;
        return true;
    default:
        return false;
    }
}

} // namespace rr::sim
