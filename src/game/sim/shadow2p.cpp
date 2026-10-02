// SLUS 0x80026960, the two-player model shadow (shadow2p.h), from our own disassembly of SLUS_010.53.
#include "game/sim/shadow2p.h"

#include "game/sim/integrator.h" // OuterProduct (the GTE OP, sf 1, lm 0)
#include "game/sim/model_draw.h" // ModelGte: RTPS
#include "game/sim/vec.h"        // DotLcm 0x8002E698, MulAdd 0x8002EAD8, Blend16 0x8002EB78

namespace rr::sim::shadow {
namespace {

constexpr uint32_t kGameStatePtr = 0x8005B2F8;

// `lo >> 16 | hi << 16` of a 64-bit product: bits 16..47.
int32_t Mid(int64_t p) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(p) >> 16)); }

// Normalize SLUS 0x8002E468 over halfwords, the table read through *(0x8005B560) (as shadow.cpp's).
bool Normalize(GuestRam& g, int16_t v[3]) {
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

// 0x80026ACC..0x80026B14: 2^31 / ((|x| >> 1) + ((|x| - 2) >> 31)) (divu; 0 gives 0xFFFFFFFF), negated for x < 0.
int32_t Recip(int32_t x) {
    const int32_t m = x < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(x)) : x;
    const uint32_t d = static_cast<uint32_t>((m >> 1) + (static_cast<int32_t>(static_cast<uint32_t>(m) - 2u) >> 31));
    const uint32_t q = d == 0u ? 0xFFFFFFFFu : 0x80000000u / d;
    return x < 0 ? static_cast<int32_t>(0u - q) : static_cast<int32_t>(q);
}

} // namespace

bool Shadow2p(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t ground, uint32_t normal, Sink2p* sink) {
    const int16_t n[3] = {g.S16(normal), g.S16(normal + 2u), g.S16(normal + 4u)};
    int16_t l[3] = {g.S16(kLight), g.S16(kLight + 2u), g.S16(kLight + 4u)};    // 0x80026988..0x800269BC
    int32_t nl = rr::sim::DotLcm(n, l);                                          // 0x800269C0
    const int32_t sgn = nl >> 31;
    if (!(0xB503 < ((sgn + nl) ^ sgn))) {                                        // 0x800269CC..0x800269E0
        int16_t c[3];
        rr::sim::OuterProduct(n, l, c);                                          // 0x800269E8..0x80026A40
        rr::sim::OuterProduct(c, n, l);                                          // 0x80026A44..0x80026A94
        if (!Normalize(g, l)) return false;                                      // 0x80026A98
        rr::sim::Blend16(n, l, l, 0xB504, 0xB504);                               // 0x80026AB4
        nl = rr::sim::DotLcm(n, l);                                              // 0x80026AC0
    }
    const int32_t r = Recip(nl);
    // the four points, k = 3 - i, moved along L onto the ground plane             0x80026B1C..0x80026C48
    int32_t out[4][3];
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t p = obj + 196u + 12u * (3u - i);
        int32_t d[3], base[3];
        for (uint32_t k = 0; k < 3; ++k) {
            base[k] = g.S32(p + 4u * k);
            d[k] = static_cast<int32_t>(g.U32(ground + 4u * k) - static_cast<uint32_t>(base[k]));
        }
        const uint32_t mx = static_cast<uint32_t>(Mid(static_cast<int64_t>(d[0]) * (static_cast<int32_t>(n[0]) << 4)));
        const uint32_t my = static_cast<uint32_t>(Mid(static_cast<int64_t>(d[1]) * (static_cast<int32_t>(n[1]) << 4)));
        const uint32_t mz = static_cast<uint32_t>(Mid(static_cast<int64_t>(d[2]) * (static_cast<int32_t>(n[2]) << 4)));
        const int32_t sum = static_cast<int32_t>(mz + (my + mx));
        const int32_t t = Mid(static_cast<int64_t>(sum) * r);
        rr::sim::MulAdd(base, l, t, out[i]);
    }
    if (g.Faulted()) return false;
    // >> 10, minus the render camera's eye (halfwords)                            0x80026C50..0x80026D00
    const uint32_t cam = g.U32(kRenderCams + 4u * view);
    int16_t sv[4][3];
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t k = 0; k < 3; ++k)
            sv[i][k] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(out[i][k] >> 10) -
                                                                  g.U16(cam + 0x1Cu + 4u * k)));
    // RotTransPers4 0x8004D1E4(sv0, sv1, sv3, sv2): RT = +0x5C, TR = 0            0x80026D08..0x80026D80
    rr::sim::model::ModelGte m;
    m.ofx = gte.ofx;
    m.ofy = gte.ofy;
    m.h = gte.h;
    m.LoadRt(g, cam + 0x5Cu);
    static constexpr uint32_t kOrder[4] = {0, 1, 3, 2}; // the call's v0..v3
    uint32_t sxy[4];
    int32_t sz[4];
    for (uint32_t k = 0; k < 4; ++k) {
        int32_t mac[3];
        sxy[k] = m.Rtps(sv[kOrder[k]], mac);
        sz[k] = mac[2] < 0 ? 0 : (mac[2] > 0xFFFF ? 0xFFFF : mac[2]);
    }
    const int64_t mac0 = static_cast<int64_t>(gte.zsf4) * (sz[0] + sz[1] + sz[2] + sz[3]);
    const int64_t otz = mac0 >> 12;
    const int32_t s0 = static_cast<int32_t>(otz < 0 ? 0 : (otz > 0xFFFF ? 0xFFFF : otz)) * 4;   // 0x80026D84
    if (s0 < 0) return true;                                                     // 0x80026D88
    // the sprite descriptor 0 and the heap                                        0x80026D90..0x80026E48
    const uint32_t u = g.U32(kSprites), v = g.U32(kSprites + 4u), w = g.U32(kSprites + 8u), h = g.U32(kSprites + 12u);
    const uint16_t tpage = g.U16(kSprites + 16u), clut = g.U16(kSprites + 20u);
    const uint32_t rec = g.U32(kHeapPtr);
    const uint32_t pk = g.U32(rec + 0x10Cu);
    if (!(pk + 40u < g.U32(kHeapLimit))) return false; // SLUS 0x80021C98 not ported
    const uint32_t ot = g.U32(rec + 0x108u);
    // the depth ranges (0x80026E4C..0x80026F2C, the one-player form's mapping)
    int32_t t1 = s0;
    const int32_t nearZ = g.S32(0x1F800004u);
    if (nearZ < 4096) {
        const int32_t b = (s0 >> 11) > 0 ? 1 : 0;
        const uint32_t at = ((s0 >> 12) <= 0 ? 0x1F800018u : 0x1F800019u) + static_cast<uint32_t>(b);
        const int32_t a1 = g.S32(0x1F800000u) + g.S8(at);
        const int32_t i = (~(a1 >> 31) & a1) + (((3 - a1) >> 31) & (3 - a1));
        const uint32_t idx = static_cast<uint32_t>(i) * 2u;
        t1 = static_cast<int32_t>(static_cast<uint32_t>(s0) - (g.U16(0x1F800006u + idx) + static_cast<uint32_t>(g.S32(0x1F800004u))));
        t1 = static_cast<int32_t>(g.U16(0x1F80000Eu + idx) + static_cast<uint32_t>(t1 >> (a1 & 31)));
    } else {
        t1 = static_cast<int32_t>(static_cast<uint32_t>(s0) - static_cast<uint32_t>(nearZ)) >> (g.U32(0x1F800000u) & 31u);
    }
    const int32_t lim = g.S32(0x1F80001Cu);
    const int32_t over = static_cast<int32_t>(static_cast<uint32_t>(lim) - static_cast<uint32_t>(t1));
    const int32_t idx = static_cast<int32_t>(static_cast<uint32_t>(~(t1 >> 31) & t1) + static_cast<uint32_t>((over >> 31) & over));
    const uint32_t slot = static_cast<uint32_t>(idx) * 4u + ot;                  // 0x80026F30..0x80026F34
    const uint32_t link = g.U32(slot);
    g.W32(pk + 4u, 0x2F000000u);                                                 // 0x80026F40
    g.W32(pk, link | 0x09000000u);                                               // 0x80026F4C
    g.W32(slot, pk);                                                             // 0x80026F50
    const auto x = [](uint32_t s) { return static_cast<uint16_t>(s & 0xFFFFu); };
    const auto y = [](uint32_t s) { return static_cast<uint16_t>(s >> 16); };
    g.W16(pk + 8u, x(sxy[0]));
    g.W16(pk + 10u, y(sxy[0]));
    g.W16(pk + 16u, x(sxy[1]));
    g.W16(pk + 18u, y(sxy[1]));
    g.W16(pk + 24u, x(sxy[2]));  // the call's v2 = sv3
    g.W16(pk + 26u, y(sxy[2]));
    g.W16(pk + 32u, x(sxy[3]));  // the call's v3 = sv2
    const uint8_t u0 = static_cast<uint8_t>(u), v0 = static_cast<uint8_t>(v);
    const uint8_t u1 = static_cast<uint8_t>(u + w - 1u), v1 = static_cast<uint8_t>(v + h - 1u);
    g.W8(pk + 12u, u0);
    g.W8(pk + 13u, v0);
    g.W8(pk + 20u, u1);
    g.W8(pk + 21u, v0);
    g.W16(pk + 34u, y(sxy[3]));
    g.W8(pk + 29u, v1);
    g.W8(pk + 36u, u1);
    g.W8(pk + 37u, v1);
    g.W8(pk + 28u, u0);
    g.W16(pk + 22u, tpage);
    g.W16(pk + 14u, clut);
    g.W32(rec + 0x10Cu, pk + 40u);                                               // 0x80026FF4
    if (g.Faulted()) return false;
    if (sink != nullptr) sink->Packet(pk, s0);
    return true;
}

bool EmitterShadow2p(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t w24, Sink2p* sink, bool& ran) {
    ran = false;
    if (((w24 >> 7) & 1u) == 0) return true;                                     // 0x80025E00..0x80025E10
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
    if (g.U32(g.U32(kGameStatePtr) + 0x30u) == 1u) return true;                  // 0x80025E74..0x80025E88
    ran = true;
    return Shadow2p(g, gte, obj, view, ground, normal, sink);                    // 0x80025EA8
}

} // namespace rr::sim::shadow
