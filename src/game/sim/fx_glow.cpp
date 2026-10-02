// The emitter's glow sprites (fx_glow.h), transcribed from our own disassembly of SLUS_010.53.
#include "game/sim/fx_glow.h"

namespace rr::sim {
namespace {

// `mult; mflo`: the low word of the product.
int32_t MulLo(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
// The `mult; srl lo,12; sll hi,20; or` idiom: bits 12..43 of the 64-bit product.
int32_t MulShift12(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(a) * b) >> 12));
}
// `sra s,v,31; addu v,s,v; xor v,v,s`
int32_t Abs(int32_t v) {
    const int32_t s = v >> 31;
    return static_cast<int32_t>((static_cast<uint32_t>(s) + static_cast<uint32_t>(v)) ^ static_cast<uint32_t>(s));
}
int32_t ClampHigh(int32_t v, int32_t limit) {
    const uint32_t a = ~static_cast<uint32_t>(v >> 31) & static_cast<uint32_t>(v);
    const uint32_t d = static_cast<uint32_t>(limit) - static_cast<uint32_t>(v);
    const uint32_t b = static_cast<uint32_t>(static_cast<int32_t>(d) >> 31) & d;
    return static_cast<int32_t>(a + b);
}
// The emitters' shared depth-to-bucket rule (effects.cpp OtBucket; 0x80027E5C.. / 0x800287F0.. here).
int32_t Bucket(GuestRam& g, int32_t t1) {
    const int32_t nearZ = g.S32(0x1F800004u);
    if (nearZ < 4096) {
        const uint32_t base = ((t1 >> 12) > 0) ? 0x1F800019u : 0x1F800018u;
        const uint32_t step = ((t1 >> 11) > 0) ? 1u : 0u;
        const int32_t shift = static_cast<int32_t>(g.U32(0x1F800000u) + static_cast<uint32_t>(static_cast<int32_t>(g.S8(base + step))));
        const int32_t sel = ClampHigh(shift, 3) * 2;
        const uint32_t from = g.U16(0x1F800006u + static_cast<uint32_t>(sel)) + g.U32(0x1F800004u);
        const int32_t rel = static_cast<int32_t>(static_cast<uint32_t>(t1) - from) >> (static_cast<uint32_t>(shift) & 31u);
        t1 = static_cast<int32_t>(g.U16(0x1F80000Eu + static_cast<uint32_t>(sel)) + static_cast<uint32_t>(rel));
    } else {
        t1 = static_cast<int32_t>(static_cast<uint32_t>(t1) - static_cast<uint32_t>(nearZ)) >> (g.U32(0x1F800000u) & 31u);
    }
    return ClampHigh(t1, g.S32(0x1F80001Cu));
}

struct Uv {
    uint32_t u = 0, v = 0, uR = 0, vB = 0;
    uint16_t tpage = 0, clut = 0;
};
Uv SpriteUv(GuestRam& g, uint32_t d) {
    Uv s;
    s.u = g.U32(d);
    s.v = g.U32(d + 4u);
    s.uR = s.u + g.U32(d + 8u) - 1u;
    s.vB = s.v + g.U32(d + 12u) - 1u;
    s.tpage = g.U16(d + 16u);
    s.clut = g.U16(d + 20u);
    return s;
}

// One POLY_FT4 at the heap's head, linked into bucket `bucket`; `xy` the four screen points in packet order.
void LinkSprite(GuestRam& g, uint32_t p, uint32_t slot, uint32_t word1, const int16_t xy[8], const Uv& s) {
    g.W32(p + 4u, word1);
    g.W32(p, g.U32(slot) | 0x09000000u);
    g.W32(slot, p);
    for (uint32_t k = 0; k < 4; ++k) {
        g.W16(p + 8u + 8u * k, static_cast<uint16_t>(xy[2 * k]));
        g.W16(p + 10u + 8u * k, static_cast<uint16_t>(xy[2 * k + 1]));
    }
    g.W8(p + 12u, static_cast<uint8_t>(s.u));
    g.W8(p + 13u, static_cast<uint8_t>(s.v));
    g.W8(p + 20u, static_cast<uint8_t>(s.uR));
    g.W8(p + 21u, static_cast<uint8_t>(s.v));
    g.W8(p + 28u, static_cast<uint8_t>(s.u));
    g.W8(p + 29u, static_cast<uint8_t>(s.vB));
    g.W8(p + 36u, static_cast<uint8_t>(s.uR));
    g.W8(p + 37u, static_cast<uint8_t>(s.vB));
    g.W16(p + 22u, static_cast<uint16_t>(s.tpage | 0x20u));
    g.W16(p + 14u, s.clut);
    g.W32(g.U32(kFxPacketHeapPtr) + 0x10Cu, p + 40u);
}

// The heap's head and its overflow test (`sltu v0, p + 40, *(0x8005B4D0)`).
bool Head(GuestRam& g, FxEnv& env, uint32_t& p, uint32_t& ot) {
    const uint32_t rec = g.U32(kFxPacketHeapPtr);
    p = g.U32(rec + 0x10Cu);
    ot = g.U32(rec + 0x108u);
    if (!(p + 40u < g.U32(kFxPacketEnd))) {
        env.Refuse("the packet heap is full: its overflow arm SLUS 0x80021C98 is not ported");
        return false;
    }
    return !g.Faulted();
}

} // namespace

// ============================================================================ SLUS 0x80025218..0x800252EC
bool WeaponGlowWanted(GuestRam& g, uint32_t obj) {
    if (((g.U16(g.U32(obj) + 14u) & 0x78u) >> 3) != 5u) return false;
    if (g.S8(obj + 8u) < 6) return false;
    const uint32_t owner = g.U32(obj + 52u);
    const uint32_t rec = g.U32(kFxAnimTables) + 12u * g.U8(owner + 569u);
    const uint32_t frame = g.U8(owner + g.U8(owner + 570u) + 574u);
    if (g.U16(g.U32(rec + 8u) + 12u * frame + 8u) == 0) return false;
    return g.S32(g.U32(owner + 540u) + 16u) >= 9;
}

// ============================================================================ SLUS 0x80025360..0x8002539C
bool BikeLightWanted(GuestRam& g, uint32_t obj, uint32_t w24) {
    if (((g.U16(g.U32(obj) + 14u) & 0x78u) >> 3) != 2u) return false;
    if (((w24 >> 18) & 1u) == 0) return false;
    return g.S8(obj + 8u) == 0;
}

// ============================================================================ SLUS 0x80027B80
void WeaponGlow(GuestRam& g, FxEnv& env, uint32_t obj) {
    const int32_t ref = g.S32(kFxLightScale);
    const int32_t lod = g.S8(obj + 8u);
    const uint32_t shift = g.U16(g.U32(obj) + 14u) >> 12;
    const uint32_t sc = g.U32(kFxModelScreen);
    auto sx = [&](uint32_t k) { return static_cast<int32_t>(g.S16(sc + 4u * k)); };
    auto sy = [&](uint32_t k) { return static_cast<int32_t>(g.S16(sc + 4u * k + 2u)); };
    uint32_t desc = 0, vtx = 0, x0 = 0, y0 = 0;
    int32_t area = 0, yOff = 0;
    if (lod == 7) {
        desc = 0x800D4388u;
        area = MulLo(sx(4) - sx(1), Abs(sy(3) - sy(4))) << 1;
        yOff = -10;
        x0 = g.U16(sc + 16u);
        y0 = g.U16(sc + 18u) + 4u;
        vtx = 4;
    } else if (lod == 6) {
        desc = 0x800D42C4u;
        y0 = g.U16(sc + 58u) + 6u;
        area = MulLo(sx(14) - sx(8), Abs(sy(13) - sy(14)));
        x0 = g.U16(sc + 56u);
        yOff = -10;
        vtx = 14;
    } else if (lod == 8) {
        desc = 0x800D42E0u;
        yOff = 10;
        x0 = g.U16(sc + 4u);
        y0 = g.U16(sc + 6u) - 4u;
        area = MulLo(sx(1) - sx(4), Abs(sy(2) - sy(1)) + 3);
        vtx = 1;
    } else {
        return;
    }
    g.W32(kFxGlowVertex, vtx);
    const int32_t depth = g.S32(g.U32(kFxModelVerts) + 16u * g.U32(kFxGlowVertex) + 8u) >> (shift & 31u);
    if (depth <= 0) return;
    int32_t w = area;
    if (!(w < 201)) w = 200;
    if ((ref >> 2) < depth) {
        const int32_t r = static_cast<int32_t>(static_cast<uint32_t>(w) << 12) / depth;
        w = MulShift12(r, static_cast<int32_t>(static_cast<uint32_t>(ref) << 12)) >> 12;
    } else {
        const int32_t lo = (w < -200) ? -200 : w;
        w = (w < 201) ? lo : lo + 200 - w;
    }
    const int16_t xa = static_cast<int16_t>(x0), ya = static_cast<int16_t>(y0);
    const int16_t xb = static_cast<int16_t>(static_cast<int32_t>(xa) + w);
    const int16_t yb = static_cast<int16_t>(static_cast<int32_t>(ya) + yOff);
    const Uv s = SpriteUv(g, desc);
    uint32_t p = 0, ot = 0;
    if (!Head(g, env, p, ot)) return;
    const int32_t bucket = Bucket(g, depth);
    const int16_t xy[8] = {xa, ya, xb, ya, xa, yb, xb, yb};
    LinkSprite(g, p, ot + 4u * static_cast<uint32_t>(bucket), 0x2F000000u, xy, s);
    if (env.sink != nullptr) env.sink->Packet(p, depth, kFxWeaponGlowFn);
}

// ============================================================================ SLUS 0x80028534
void BikeLight(GuestRam& g, FxEnv& env, uint32_t obj) {
    const uint32_t cls = kFxLightClass + 6u * g.U32(obj + 0xB4u);
    const uint32_t dod = g.U32(obj);
    const uint32_t group = g.U32(g.U32(obj + 4u) + 24u * g.U16(dod + 24u) - 24u);
    const uint32_t prim = g.U32(group + 20u) + 20u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(cls))) + 4u;
    const int32_t ref = g.S32(kFxLightScale);
    const int32_t depth = g.S32(g.U32(kFxModelVerts) + 16u * g.U16(prim + 12u) + 8u) >> ((g.U16(dod + 14u) >> 12) & 31u);
    if (depth <= 0 || g.Faulted()) return;
    const uint32_t set = kFxPartSets + 4u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(cls + 1u)));
    const uint32_t sc = g.U32(kFxModelScreen);
    const uint32_t a = sc + 4u * g.U16(prim + 2u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(set + 2u))) + 12u);
    const uint32_t b = sc + 4u * g.U16(prim + 2u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(set + 3u))) + 12u);
    auto half = [](int32_t v) { return (v + static_cast<int32_t>(static_cast<uint32_t>(v) >> 31)) >> 1; };
    int16_t c[3];
    c[0] = static_cast<int16_t>(half(g.S16(a) + g.S16(b)));
    c[1] = static_cast<int16_t>(half(g.S16(a + 2u) + g.S16(b + 2u)));
    c[2] = 0;
    const int32_t dy = Abs(g.S16(a + 2u) - g.S16(b + 2u));
    const int32_t dx = Abs(g.S16(a) - g.S16(b));
    int32_t size = (dy < dx) ? dx << 1 : dy << 1;
    if ((ref >> 2) < depth) {
        const int32_t r = static_cast<int32_t>(static_cast<uint32_t>(size) << 12) / depth;
        size = MulShift12(r, static_cast<int32_t>(static_cast<uint32_t>(ref) << 12)) >> 12;
    }
    const int32_t angle = static_cast<int32_t>(static_cast<uint32_t>(c[0] + c[1]) << 19) >> 16;
    int16_t q[16] = {};
    EffectQuadSpin(g, q, c, angle, static_cast<uint32_t>(size));
    const Uv s = SpriteUv(g, kFxSpriteTable + 5u * kFxSpriteBytes);
    uint32_t p = 0, ot = 0;
    if (!Head(g, env, p, ot)) return;
    int32_t bucket = Bucket(g, depth);
    bucket -= (bucket >= 3) ? 3 : 0;
    const int16_t xy[8] = {q[0], q[1], q[4], q[5], q[8], q[9], q[12], q[13]};
    LinkSprite(g, p, ot + 4u * static_cast<uint32_t>(bucket), 0x2E000060u, xy, s);
    if (env.sink != nullptr) env.sink->Packet(p, depth, kFxBikeLightFn);
}

} // namespace rr::sim
