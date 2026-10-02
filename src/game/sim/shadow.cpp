// SLUS 0x80025EE0, the model shadow (shadow.h), from our own disassembly of SLUS_010.53.
#include "game/sim/shadow.h"

#include "game/sim/integrator.h" // OuterProduct (the GTE OP, sf 1, lm 0)
#include "game/sim/model_draw.h" // ModelGte: MVMVA RT x V0, LCM row 1 x IR, RTPS
#include "game/sim/vec.h"        // Scale 0x8002EE50, Blend16 0x8002EB78

namespace rr::sim::shadow {
namespace {

constexpr uint32_t kVertsPtr = 0x8005ACB0;
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kSpadIn = 0x1F800210;  // four SVECTORs, 8 bytes apart
constexpr uint32_t kSpadSxy = 0x1F800230; // SXY0, SXY1, SXY2 of the RTPT, then the RTPS's SXY2 at +0x0C

// `lo >> 16 | hi << 16` of a 64-bit product: bits 16..47.
int32_t Mid(int64_t p) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(p) >> 16)); }

// Normalize SLUS 0x8002E468 over guest halfwords (vec.h's arithmetic, the table read from the guest).
bool GuestNormalize(GuestRam& g, int16_t v[3]) {
    const int64_t s12 = static_cast<int64_t>(v[0]) * v[0] + static_cast<int64_t>(v[1]) * v[1];
    if (s12 > 0x7FFFFFFF) return false; // the trapping `add`
    const int64_t s = s12 + static_cast<int64_t>(v[2]) * v[2];
    if (s > 0x7FFFFFFF) return false;
    const uint32_t n = static_cast<uint32_t>(s);
    uint32_t probe = (n & 0x80000000u) ? ~n : n, lz = 0;
    while (lz < 32 && (probe & 0x80000000u) == 0) {
        ++lz;
        probe <<= 1;
    }
    int32_t sh = 22 - static_cast<int32_t>(lz & ~1u);
    if (sh <= 0) sh = 0;
    const int32_t index = static_cast<int32_t>(n) >> sh;
    const uint32_t w = g.U16(g.U32(kRsqrtPtr) + 2u * static_cast<uint32_t>(index));
    int32_t f = static_cast<int32_t>((w >> 5) << (w & 0x1Fu));
    f >>= ((sh >> 1) & 31);
    for (int i = 0; i < 3; ++i)
        v[i] = static_cast<int16_t>(static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(v[i]) * f)) >> 12);
    return true;
}

// The reciprocal idiom 0x80026350..0x8002639C: 2^31 / ((|x| >> 1) + ((|x| - 2) >> 31)) (divu, no
// divide-by-zero check: the R3000 gives 0xFFFFFFFF), negated for x < 0.
int32_t Recip(int32_t x) {
    const int32_t m = x < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(x)) : x;
    const uint32_t d = static_cast<uint32_t>((m >> 1) + (static_cast<int32_t>(static_cast<uint32_t>(m) - 2u) >> 31));
    const uint32_t q = d == 0u ? 0xFFFFFFFFu : 0x80000000u / d;
    return x < 0 ? static_cast<int32_t>(0u - q) : static_cast<int32_t>(q);
}

int16_t Lo(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(v))); }

} // namespace

void BeginEntry(GuestRam& g, uint32_t obj) {
    const uint32_t c = (g.U8(obj + 9u) & 0x30u) >> 4;
    g.W32(kCounter, c);
    g.W32(kCounterTop, c);
}

bool Shadow(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t ground, uint32_t normal, Sink* sink) {
    const uint32_t gpScale = g.gp() + kGpScale;
    const uint32_t dod = g.U32(obj);
    const uint32_t quads = g.U32(dod + 0x2Cu);
    const uint32_t exp = g.U16(dod + 14u) >> 12;
    const uint32_t verts = g.U32(kVertsPtr);
    const uint32_t count = g.U32(quads);
    {   // 0x80025F3C: heap + count * 36 against the limit (sltu)
        const uint32_t heap = g.U32(g.U32(kHeapPtr) + 0x10Cu);
        if (!(heap + count * 36u < g.U32(kHeapLimit))) return false; // SLUS 0x80021C98 not ported
    }
    if (g.Faulted()) return false;
    rr::sim::model::ModelGte m;
    m.ofx = gte.ofx;
    m.ofy = gte.ofy;
    m.h = gte.h;
    if (g.U32(kCounterTop) == g.U32(kCounter)) {
        const uint32_t cam = g.U32(kRenderCams + 4u * view);
        int32_t mac[3];
        // the normal, then the light, through the render camera (+0x5C): the MACs' low halves
        const int16_t n0[3] = {g.S16(normal), g.S16(normal + 2u), g.S16(normal + 4u)};
        m.LoadRt(g, cam + 0x5Cu);
        m.MulV0(n0, mac);
        const int16_t n[3] = {Lo(mac[0]), Lo(mac[1]), Lo(mac[2])};
        const int16_t l0[3] = {g.S16(kLight), g.S16(kLight + 2u), g.S16(kLight + 4u)};
        m.MulV0(l0, mac);
        int16_t l[3] = {Lo(mac[0]), Lo(mac[1]), Lo(mac[2])};
        // the ground point: (p >> 10) - eye, as halfwords
        int16_t p0[3];
        for (uint32_t k = 0; k < 3; ++k)
            p0[k] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(g.S32(ground + 4u * k) >> 10) -
                                                               g.U16(cam + 0x1Cu + 4u * k)));
        int32_t gc[3];
        m.MulV0(p0, gc);
        // n . L >> 8 (LCM row 1 = L, IR = n, sf 0)
        for (int k = 0; k < 3; ++k) m.l1[k] = l[k];
        int32_t nl = m.LightRow(n) >> 8;
        g.W32(gpScale, static_cast<uint32_t>(nl));
        const int32_t sgn = nl >> 31;
        if (!(0xB503 < ((sgn + nl) ^ sgn))) {
            // more than 45 degrees off the normal: L' = 0.7071 (n + unit((n x L) x n))
            int16_t c[3], p[3];
            rr::sim::OuterProduct(n, l, c);
            rr::sim::OuterProduct(c, n, p);
            if (!GuestNormalize(g, p)) return false; // the console's overflow exception
            rr::sim::Blend16(n, p, p, 0xB504, 0xB504);
            for (int k = 0; k < 3; ++k) m.l1[k] = p[k];
            nl = m.LightRow(n) >> 8;
            g.W32(gpScale, static_cast<uint32_t>(nl));
            for (int k = 0; k < 3; ++k) l[k] = p[k];
        }
        const int32_t r = Recip(static_cast<int32_t>(g.U32(gpScale)));
        const int32_t sum3 = static_cast<int32_t>(
            static_cast<uint32_t>(Mid(static_cast<int64_t>(gc[2]) * (static_cast<int32_t>(n[2]) << 4))) +
            (static_cast<uint32_t>(Mid(static_cast<int64_t>(gc[1]) * (static_cast<int32_t>(n[1]) << 4))) +
             static_cast<uint32_t>(Mid(static_cast<int64_t>(gc[0]) * (static_cast<int32_t>(n[0]) << 4)))));
        const int32_t t = Mid(static_cast<int64_t>(sum3) * r);
        g.W32(gpScale, static_cast<uint32_t>(r));
        const auto diag = [](int32_t a, int32_t b) {
            return static_cast<int32_t>(static_cast<uint32_t>(a * b) << 4) >> 16;
        };
        const int32_t s2 = diag(n[0], l[0]), s3 = diag(n[1], l[1]), s1 = diag(n[2], l[2]);
        int32_t tr[3];
        rr::sim::Scale(t, l, tr);
        for (uint32_t k = 0; k < 3; ++k) g.W32(kMatrix + 0x14u + 4u * k, static_cast<uint32_t>(tr[k]));
        const auto neg = [](int32_t a, int32_t b) { return static_cast<uint16_t>(-((a * b) >> 12)); };
        g.W16(kMatrix + 0u, static_cast<uint16_t>(s3 + s1));
        g.W16(kMatrix + 2u, neg(l[0], n[1]));
        g.W16(kMatrix + 4u, neg(l[0], n[2]));
        g.W16(kMatrix + 6u, neg(l[1], n[0]));
        g.W16(kMatrix + 8u, static_cast<uint16_t>(s2 + s1));
        g.W16(kMatrix + 10u, neg(l[1], n[2]));
        g.W16(kMatrix + 12u, neg(l[2], n[0]));
        g.W16(kMatrix + 14u, neg(l[2], n[1]));
        g.W16(kMatrix + 16u, static_cast<uint16_t>(s2 + s3));
    }
    g.W32(kCounter, g.U32(kCounter) - 1u);
    m.LoadRt(g, kMatrix);
    m.LoadTr(g, kMatrix + 0x14u);
    uint32_t q = g.U32(g.U32(obj) + 0x2Cu) + 4u;
    const uint32_t rec = g.U32(kHeapPtr);
    for (uint32_t left = count; left != 0; --left, q += 8u) {
        const int32_t scale = g.S32(gpScale);
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t base = verts + (static_cast<uint32_t>(g.U16(q + 2u * k)) << 4);
            for (uint32_t c = 0; c < 3; ++c) {
                const int32_t v = g.S32(base + 4u * c) >> exp;
                g.W16(kSpadIn + 8u * k + 2u * c, static_cast<uint16_t>(static_cast<uint32_t>(Mid(static_cast<int64_t>(v) * scale))));
            }
        }
        // RTPT on v0..v2, RTPS on v3, AVSZ4 (0x168002E) over SZ0..SZ3 = the four depths
        uint32_t sxy[4];
        int32_t sz[4], camPt[4][3];
        for (uint32_t k = 0; k < 4; ++k) {
            const int16_t v[3] = {g.S16(kSpadIn + 8u * k), g.S16(kSpadIn + 8u * k + 2u), g.S16(kSpadIn + 8u * k + 4u)};
            sxy[k] = m.Rtps(v, camPt[k]);
            sz[k] = camPt[k][2] < 0 ? 0 : (camPt[k][2] > 0xFFFF ? 0xFFFF : camPt[k][2]);
        }
        g.W32(kSpadSxy + 0u, sxy[0]);
        g.W32(kSpadSxy + 4u, sxy[1]);
        g.W32(kSpadSxy + 8u, sxy[2]);
        g.W32(kSpadSxy + 12u, sxy[3]);
        const int64_t mac0 = static_cast<int64_t>(gte.zsf4) * (sz[0] + sz[1] + sz[2] + sz[3]);
        const int64_t otz = mac0 >> 12;
        int32_t t0 = static_cast<int32_t>(otz < 0 ? 0 : (otz > 0xFFFF ? 0xFFFF : otz)) * 4;
        if (t0 < 0) continue;
        // the depth ranges (the emitter's own mapping, 0x80026784..0x80026880)
        const int32_t nearZ = g.S32(0x1F800004u);
        if (nearZ < 4096) {
            const int32_t b = (t0 >> 11) > 0 ? 1 : 0;
            const uint32_t at = ((t0 >> 12) <= 0 ? 0x1F800018u : 0x1F800019u) + static_cast<uint32_t>(b);
            const int32_t a1 = g.S32(0x1F800000u) + g.S8(at);
            const int32_t i = (~(a1 >> 31) & a1) + (((3 - a1) >> 31) & (3 - a1));
            const uint32_t idx = static_cast<uint32_t>(i) * 2u;
            t0 = static_cast<int32_t>(static_cast<uint32_t>(t0) - (g.U16(0x1F800006u + idx) + static_cast<uint32_t>(g.S32(0x1F800004u))));
            t0 = static_cast<int32_t>(g.U16(0x1F80000Eu + idx) + static_cast<uint32_t>(t0 >> (a1 & 31)));
        } else {
            t0 = static_cast<int32_t>(static_cast<uint32_t>(t0) - static_cast<uint32_t>(nearZ)) >> (g.U32(0x1F800000u) & 31u);
        }
        const int32_t lim = g.S32(0x1F80001Cu);
        const int32_t over = static_cast<int32_t>(static_cast<uint32_t>(lim) - static_cast<uint32_t>(t0));
        t0 = static_cast<int32_t>(static_cast<uint32_t>(~(t0 >> 31) & t0) + static_cast<uint32_t>((over >> 31) & over));
        const uint32_t p = g.U32(rec + 0x10Cu), ot = g.U32(rec + 0x108u);
        g.W32(rec + 0x10Cu, p + 36u);
        g.W32(p, 0x08000000u);
        g.W32(p + 4u, 0xE1000740u);
        g.W32(p + 8u, 0xE6000003u);
        g.W32(p + 12u, g.U32(kColour) | 0x2A000000u);
        g.W32(p + 16u, g.U32(kSpadSxy + 0u));
        g.W32(p + 20u, g.U32(kSpadSxy + 4u));
        g.W32(p + 24u, g.U32(kSpadSxy + 12u));
        g.W32(p + 32u, 0xE6000000u);
        g.W32(p + 28u, g.U32(kSpadSxy + 8u));
        const uint32_t slot = static_cast<uint32_t>(t0) * 4u + ot;
        g.W32(p, (g.U32(p) & 0xFF000000u) | (g.U32(slot) & 0x00FFFFFFu));
        g.W32(slot, (g.U32(slot) & 0xFF000000u) | (p & 0x00FFFFFFu));
        if (sink != nullptr) sink->Packet(p, static_cast<uint32_t>(t0), camPt);
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

bool EmitterShadow(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t w24, Sink* sink, bool& twoPlayer) {
    twoPlayer = false;
    if (((w24 >> 7) & 1u) == 0) return true;
    const uint32_t parent = g.U32(obj + 0x34u);
    uint32_t ground = 0, normal = 0;
    if (parent != 0) {
        ground = parent + 0x1F8u;
        normal = parent + 0x20Au;
    } else {
        const uint32_t kind = (g.U16(g.U32(obj) + 14u) & 0x78u) >> 3;
        normal = obj + 0x20Au;
        if (kind == 3u) ground = obj + 0xB8u;
        else if (kind == 1u || kind == 4u) ground = (g.U32(obj + 0x228u) & 0x40000000u) ? obj + 0x1F8u : obj + 0xB8u;
        else ground = obj + 0x1F8u;
    }
    if (g.U32(g.U32(kGameStatePtr) + 0x30u) != 1u) {
        twoPlayer = true;
        return true;
    }
    return Shadow(g, gte, obj, view, ground, normal, sink);
}

} // namespace rr::sim::shadow
