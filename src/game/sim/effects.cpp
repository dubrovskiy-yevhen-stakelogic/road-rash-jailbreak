#include "game/sim/effects.h"

#include <algorithm>

#include "game/sim/vec.h"

namespace rr::sim {
namespace {

constexpr uint32_t kGameStatePtr = 0x8005B2F8;

uint32_t GameState(GuestRam& g) { return g.U32(kGameStatePtr); }
uint32_t Clock(GuestRam& g) { return g.U32(GameState(g) + 0x10u); }
uint32_t Players(GuestRam& g) { return g.U32(GameState(g) + 0x30u); }

uint32_t Record(int32_t idx) { return kEffectPool + static_cast<uint32_t>(idx * static_cast<int32_t>(kEffectRecordBytes)); }
int32_t Link(uint32_t w) { return static_cast<int32_t>(w << 26) >> 26; }
uint32_t State(uint32_t w) { return (w >> 6) & 0xFu; }
uint32_t Sub(uint32_t w) { return (w >> 10) & 0xFu; }
uint32_t Kind(uint32_t w) { return (w >> 14) & 0xFFu; }

// The render camera of `view`: *(0x8005AEC0 + 4 view).
uint32_t RenderCam(GuestRam& g, uint32_t view) { return g.U32(kFxRenderCamPtrs + 4u * view); }

void ReadS32x3(GuestRam& g, uint32_t a, int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S32(a + 4u * k);
}
void WriteS32x3(GuestRam& g, uint32_t a, const int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, static_cast<uint32_t>(v[k]));
}
void ReadS16x3(GuestRam& g, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a + 2u * k);
}

// (s32)(((s64)a * b) >> 16): the `mult; mfhi; mflo; srl 16; sll 16; or` idiom.
int32_t MulShift16(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(a) * b) >> 16));
}
// The `mult; srl lo,12; sll hi,20; or` idiom: bits 12..43 of the 64-bit product.
int32_t MulShift12(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(a) * b) >> 12));
}

// GCC's signed division by 3413 (0xD55): `mult v,0x999D70BD; mfhi; addu v; sra 11; subu v>>31`.
int32_t Div3413(int32_t v) {
    const int32_t hi = static_cast<int32_t>(static_cast<uint64_t>(static_cast<int64_t>(v) *
                                                                  static_cast<int32_t>(0x999D70BDu)) >> 32);
    return ((static_cast<int32_t>(static_cast<uint32_t>(hi) + static_cast<uint32_t>(v))) >> 11) - (v >> 31);
}

// `divu`: a zero divisor leaves LO = 0xFFFFFFFF on the R3000.
uint32_t DivU(uint32_t a, uint32_t b) { return b == 0 ? 0xFFFFFFFFu : a / b; }

// max(v, 0) + min(limit - v, 0), in the original's bit form (~(v >> 31) & v) + ((limit - v) >> 31 & (limit - v)).
int32_t ClampHigh(int32_t v, int32_t limit) {
    const uint32_t a = ~static_cast<uint32_t>(v >> 31) & static_cast<uint32_t>(v);
    const uint32_t d = static_cast<uint32_t>(limit) - static_cast<uint32_t>(v);
    const uint32_t b = static_cast<uint32_t>(static_cast<int32_t>(d) >> 31) & d;
    return static_cast<int32_t>(a + b);
}

// The depth-to-bucket mapping all three emitters share (0x80029358..0x80029440): the scratchpad holds
// the four-range table 800674D4 writes per view. Returns the bucket before the emitter's own clamp.
int32_t OtBucket(GuestRam& g, int32_t t0) {
    const int32_t nearZ = g.S32(0x1F800004u);
    if (nearZ < 4096) {
        const uint32_t base = ((t0 >> 12) > 0) ? 0x1F800019u : 0x1F800018u;
        const uint32_t step = ((t0 >> 11) > 0) ? 1u : 0u;
        const int32_t idx = g.S8(base + step);
        const int32_t shift = static_cast<int32_t>(g.U32(0x1F800000u) + static_cast<uint32_t>(idx));
        const int32_t sel = ClampHigh(shift, 3) * 2;
        const uint32_t from = g.U16(0x1F800006u + static_cast<uint32_t>(sel)) + g.U32(0x1F800004u);
        const int32_t rel = static_cast<int32_t>(static_cast<uint32_t>(t0) - from) >> (static_cast<uint32_t>(shift) & 31u);
        t0 = static_cast<int32_t>(g.U16(0x1F80000Eu + static_cast<uint32_t>(sel)) + static_cast<uint32_t>(rel));
    } else {
        t0 = static_cast<int32_t>(static_cast<uint32_t>(t0) - static_cast<uint32_t>(nearZ)) >>
             (g.U32(0x1F800000u) & 31u);
    }
    return ClampHigh(t0, g.S32(0x1F80001Cu));
}

// The packet heap's head, and the refusal of its overflow arm (SLUS 0x80021C98 is not ported).
bool HeapHead(GuestRam& g, FxEnv& env, uint32_t& packet, uint32_t& ot) {
    const uint32_t rec = g.U32(kFxPacketHeapPtr);
    packet = g.U32(rec + 0x10Cu);
    ot = g.U32(rec + 0x108u);
    if (!(packet + 40u < g.U32(kFxPacketEnd))) { // sltu
        env.Refuse("the packet heap is full: its overflow arm SLUS 0x80021C98 is not ported");
        return false;
    }
    return !g.Faulted();
}

// RTPS of the first vertex into +8, RTPT of the other three into +16/+24/+32, AVSZ4: OTZ.
uint16_t ProjectQuad(GuestRam& g, FxEnv& env, const int16_t q[16], uint32_t packet) {
    g.W32(packet + 8u, env.gte.Rtps(q + 0));
    const uint32_t s0 = env.gte.Rtps(q + 4);
    const uint32_t s1 = env.gte.Rtps(q + 8);
    const uint32_t s2 = env.gte.Rtps(q + 12);
    g.W32(packet + 16u, s0);
    g.W32(packet + 24u, s1);
    g.W32(packet + 32u, s2);
    return env.gte.Avsz4();
}

// Links `packet` into the bucket for OTZ `otz` (clamped to the table), returns the bucket.
void LinkQuad(GuestRam& g, FxEnv& env, uint32_t packet, uint32_t ot, uint16_t otz, uint32_t function) {
    const int32_t depth = static_cast<int32_t>(otz) * 4;
    int32_t bucket = OtBucket(g, depth);
    if (bucket < 0) bucket = 0;
    else if (g.S32(kFxOtLength) - 1 < bucket) bucket = g.S32(kFxOtLength) - 1;
    const uint32_t slot = ot + 4u * static_cast<uint32_t>(bucket);
    g.W32(packet, g.U32(slot) | 0x09000000u);
    g.W32(slot, packet);
    if (env.sink != nullptr) env.sink->Packet(packet, depth, function);
}

uint32_t TPage(GuestRam& g, uint32_t d, uint32_t flags) {
    const uint16_t t = g.U16(d + 16u);
    return (flags & 2u) ? ((t & 0xFF9Fu) | 0x20u) : (t | 0x60u);
}

} // namespace

// ============================================================================ the GTE
namespace {
struct UnrTable {
    uint8_t v[257];
    constexpr UnrTable() : v{} {
        for (int i = 0; i <= 256; ++i) {
            const int x = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            v[i] = static_cast<uint8_t>(x < 0 ? 0 : x);
        }
    }
};
constexpr UnrTable kUnr{};

int64_t Mac44(int64_t v) { return static_cast<int64_t>(static_cast<uint64_t>(v) << 20) >> 20; }

uint32_t GteDivide(uint32_t h, uint32_t sz3) {
    if (h < sz3 * 2) {
        uint32_t z = 0;
        for (int i = 15; i >= 0 && !(sz3 & (1u << i)); --i) ++z;
        const uint32_t n = h << z;
        uint32_t d = sz3 << z;
        const uint32_t u = kUnr.v[(d - 0x7FC0) >> 7] + 0x101u;
        d = static_cast<uint32_t>((0x2000080u - d * u) >> 8);
        d = static_cast<uint32_t>((0x0000080u + d * u) >> 8);
        const uint64_t r = (static_cast<uint64_t>(n) * d + 0x8000u) >> 16;
        return static_cast<uint32_t>(std::min<uint64_t>(0x1FFFFu, r));
    }
    return 0x1FFFF;
}
int32_t ClampSxy(int32_t v) { return v < -0x400 ? -0x400 : (v > 0x3FF ? 0x3FF : v); }
int32_t ClampIr(int32_t v) { return v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v); }
} // namespace

FxGte FxGte::FromControl(const uint32_t cr[32]) {
    FxGte t;
    t.rt[0] = static_cast<int16_t>(cr[0] & 0xFFFFu);
    t.rt[1] = static_cast<int16_t>(cr[0] >> 16);
    t.rt[2] = static_cast<int16_t>(cr[1] & 0xFFFFu);
    t.rt[3] = static_cast<int16_t>(cr[1] >> 16);
    t.rt[4] = static_cast<int16_t>(cr[2] & 0xFFFFu);
    t.rt[5] = static_cast<int16_t>(cr[2] >> 16);
    t.rt[6] = static_cast<int16_t>(cr[3] & 0xFFFFu);
    t.rt[7] = static_cast<int16_t>(cr[3] >> 16);
    t.rt[8] = static_cast<int16_t>(cr[4] & 0xFFFFu);
    for (int k = 0; k < 3; ++k) t.tr[k] = static_cast<int32_t>(cr[5 + k]);
    t.ofx = static_cast<int32_t>(cr[24]);
    t.ofy = static_cast<int32_t>(cr[25]);
    t.h = static_cast<uint16_t>(cr[26] & 0xFFFFu);
    t.dqa = static_cast<int16_t>(cr[27] & 0xFFFFu);
    t.dqb = static_cast<int32_t>(cr[28]);
    t.zsf4 = static_cast<int16_t>(cr[30] & 0xFFFFu);
    return t;
}

void FxGte::Mvmva(const int16_t v[3], int32_t mac[3]) const {
    for (int n = 0; n < 3; ++n) {
        int64_t acc = static_cast<int64_t>(tr[n]) << 12;
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 0]) * v[0]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 1]) * v[1]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 2]) * v[2]);
        mac[n] = static_cast<int32_t>(acc >> 12);
    }
}

uint32_t FxGte::Rtps(const int16_t v[3]) {
    int64_t a[3];
    for (int n = 0; n < 3; ++n) {
        int64_t acc = static_cast<int64_t>(tr[n]) << 12;
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 0]) * v[0]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 1]) * v[1]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 2]) * v[2]);
        a[n] = acc;
    }
    const int32_t ir1 = ClampIr(static_cast<int32_t>(a[0] >> 12));
    const int32_t ir2 = ClampIr(static_cast<int32_t>(a[1] >> 12));
    const int64_t z = a[2] >> 12;
    sz[0] = sz[1];
    sz[1] = sz[2];
    sz[2] = sz[3];
    sz[3] = static_cast<uint16_t>(z < 0 ? 0 : (z > 0xFFFF ? 0xFFFF : z));
    const uint32_t d = GteDivide(h, sz[3]);
    const int64_t sx = static_cast<int64_t>(d) * ir1 + ofx;
    const int64_t sy = static_cast<int64_t>(d) * ir2 + ofy;
    const int32_t x = ClampSxy(static_cast<int32_t>(sx >> 16));
    const int32_t y = ClampSxy(static_cast<int32_t>(sy >> 16));
    return (static_cast<uint32_t>(y & 0xFFFF) << 16) | static_cast<uint32_t>(x & 0xFFFF);
}

uint16_t FxGte::Avsz4() const {
    const int64_t sum = static_cast<int64_t>(sz[0]) + sz[1] + sz[2] + sz[3];
    const int64_t v = (static_cast<int64_t>(zsf4) * sum) >> 12;
    return static_cast<uint16_t>(v < 0 ? 0 : (v > 0xFFFF ? 0xFFFF : v));
}

// ============================================================================ SLUS 0x8002F17C
void RenderCamera(GuestRam& g, uint32_t view) {
    const uint32_t v = kFxViewRecords + kFxViewStride * view;
    const uint32_t rc = g.U32(g.gp() + 0x234u + 4u * view);
    int16_t m[9];
    for (uint32_t k = 0; k < 9; ++k) m[k] = g.S16(v + 0x1B0u + 2u * k);
    // RASHCDG 0x80070F9C: the transpose into +0x3C; SLUS 0x8004D264: transposed back into +0x5C.
    static constexpr int kT[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
    int16_t t[9];
    for (int k = 0; k < 9; ++k) t[k] = m[kT[k]];
    for (uint32_t k = 0; k < 9; ++k) g.W16(rc + 0x3Cu + 2u * k, static_cast<uint16_t>(t[k]));
    for (uint32_t k = 0; k < 9; ++k) g.W16(rc + 0x5Cu + 2u * k, static_cast<uint16_t>(t[kT[k]]));
    for (uint32_t a = 0x62u; a <= 0x66u; a += 2u) // the second row times 3413 / 4096
        g.W16(rc + a, static_cast<uint16_t>((static_cast<int32_t>(g.S16(rc + a)) * 3413) >> 12));
    g.W32(rc + 0x10u, g.U32(v + 0xB8u));
    g.W32(rc + 0x14u, g.U32(v + 0xBCu));
    const int32_t z = g.S32(v + 0xC0u);
    g.W32(rc + 0x1Cu, static_cast<uint32_t>(g.S32(rc + 0x10u) >> 10));
    g.W32(rc + 0x18u, static_cast<uint32_t>(z));
    g.W32(rc + 0x24u, static_cast<uint32_t>(z >> 10));
    g.W32(rc + 0x20u, static_cast<uint32_t>(g.S32(rc + 0x14u) >> 10));
    g.W16(rc + 0x7Cu, g.U16(v + 0x2E8u));
}

// ============================================================================ SLUS 0x80028E8C
void EffectToView(GuestRam& g, FxEnv& env, const int32_t pos[3], int16_t out[3], uint32_t view) {
    const uint32_t cam = RenderCam(g, view);
    int16_t v[3];
    for (uint32_t k = 0; k < 3; ++k)
        v[k] = static_cast<int16_t>(static_cast<uint32_t>(pos[k]) - g.U16(cam + 0x1Cu + 4u * k));
    int16_t m[9];
    for (uint32_t k = 0; k < 9; ++k) m[k] = g.S16(cam + 0x5Cu + 2u * k);
    env.gte.SetRt(m);
    env.gte.tr[0] = env.gte.tr[1] = env.gte.tr[2] = 0;
    int32_t mac[3];
    env.gte.Mvmva(v, mac);
    for (int k = 0; k < 3; ++k) out[k] = static_cast<int16_t>(mac[k]);
    env.gte.Identity();
}

// ============================================================================ SLUS 0x80028C78
void EffectFromModel(GuestRam& g, FxEnv& env, uint32_t e, int32_t out[3], const int32_t in[3], uint32_t view) {
    const uint32_t ex = g.U16(g.U32(e) + 14u) >> 12;
    int16_t v[3];
    v[0] = static_cast<int16_t>(in[0] >> ex);
    const int16_t y = static_cast<int16_t>(in[1] >> ex);
    v[1] = static_cast<int16_t>(Div3413(static_cast<int32_t>(y) << 12));
    v[2] = static_cast<int16_t>(in[2] >> ex);
    const uint32_t cam = RenderCam(g, view);
    int16_t m[9];
    for (uint32_t k = 0; k < 9; ++k) m[k] = g.S16(cam + 0x5Cu + 2u * k);
    static constexpr int kT[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
    int16_t t[9];
    for (int k = 0; k < 9; ++k) t[k] = m[kT[k]];
    for (int k = 1; k < 9; k += 3) t[k] = static_cast<int16_t>(Div3413(static_cast<int32_t>(t[k]) << 12));
    env.gte.SetRt(t);
    env.gte.tr[0] = env.gte.tr[1] = env.gte.tr[2] = 0;
    int32_t mac[3];
    env.gte.Mvmva(v, mac);
    for (uint32_t k = 0; k < 3; ++k)
        out[k] = static_cast<int32_t>(static_cast<uint32_t>(mac[k]) + g.U32(RenderCam(g, view) + 0x1Cu + 4u * k));
}

// ============================================================================ SLUS 0x80029048
void EffectQuadSpin(GuestRam& g, int16_t q[16], const int16_t c[3], int32_t angle, uint32_t size) {
    const uint32_t a = static_cast<uint32_t>(angle) & 0xFFFu;
    const int32_t s = g.S16(kFxSinCos + 4u * a);
    const int32_t co = g.S16(kFxSinCos + 4u * a + 2u);
    const int32_t sz = static_cast<int32_t>(size << 11);
    const int32_t t0 = MulShift12(s, sz) >> 12;
    const int32_t t2 = MulShift12(co, sz) >> 12;
    auto h = [](int32_t x) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(x))); };
    q[0] = h(c[0] - t0), q[1] = h(c[1] + t2), q[2] = c[2];
    q[4] = h(c[0] + t2), q[5] = h(c[1] + t0), q[6] = c[2];
    q[12] = h(c[0] + t0), q[13] = h(c[1] - t2), q[14] = c[2];
    q[8] = h(c[0] - t2), q[9] = h(c[1] - t0), q[10] = c[2];
}

// ============================================================================ SLUS 0x80029174
uint32_t EffectQuadAround(GuestRam& g, FxEnv& env, int16_t q[16], const int32_t world[3], const int16_t a[3],
                          uint32_t view) {
    int16_t b[3];
    EffectToView(g, env, world, b, view);
    if (b[2] < 64) b[2] = 64;
    auto h = [](int32_t x) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(x))); };
    q[4] = h(a[0] + 13), q[5] = a[1], q[6] = a[2];
    q[0] = h(a[0] - 13), q[1] = a[1], q[2] = a[2];
    q[12] = h(b[0] + 13), q[13] = b[1], q[14] = b[2];
    q[8] = h(b[0] - 13), q[9] = b[1], q[10] = b[2];
    return 0;
}

// ============================================================================ SLUS 0x8002926C
void EffectEmitQuad(GuestRam& g, FxEnv& env, const int16_t q[16], uint32_t flags, uint32_t sprite, uint32_t clut,
                    uint32_t colour) {
    const uint32_t d = kFxSpriteTable + kFxSpriteBytes * sprite;
    uint32_t p = 0, ot = 0;
    if (!HeapHead(g, env, p, ot)) return;
    const uint16_t otz = ProjectQuad(g, env, q, p);
    LinkQuad(g, env, p, ot, otz, 0x8002926Cu);
    g.W32(p + 4u, (flags & 1u) ? (colour | 0x2E000000u) : 0x2F000000u);
    const uint8_t u = g.U8(d), v = g.U8(d + 4u);
    const uint8_t uR = static_cast<uint8_t>(u + g.U8(d + 8u) - 1u), vB = static_cast<uint8_t>(v + g.U8(d + 12u) - 1u);
    g.W8(p + 12u, u), g.W8(p + 13u, vB);
    g.W8(p + 20u, u), g.W8(p + 21u, v);
    g.W8(p + 28u, uR), g.W8(p + 29u, vB);
    g.W8(p + 36u, uR), g.W8(p + 37u, v);
    g.W16(p + 22u, static_cast<uint16_t>(TPage(g, d, flags)));
    g.W16(p + 14u, g.U16(d + 20u + 2u * clut));
    g.W32(g.U32(kFxPacketHeapPtr) + 0x10Cu, p + 40u);
}

// ============================================================================ SLUS 0x800295AC
void EffectEmitDebris(GuestRam& g, FxEnv& env, const int16_t q[16], uint32_t flags, uint32_t sprite, uint32_t frame) {
    const uint32_t d = kFxSpriteTable + kFxSpriteBytes * sprite;
    const uint32_t t = kFxDebrisUv + (frame << 3);
    uint32_t p = 0, ot = 0;
    if (!HeapHead(g, env, p, ot)) return;
    const uint16_t otz = ProjectQuad(g, env, q, p);
    LinkQuad(g, env, p, ot, otz, 0x800295ACu);
    g.W32(p + 4u, (flags & 1u) ? 0x2E000000u : 0x2F000000u);
    const uint8_t u = g.U8(d), v = g.U8(d + 4u);
    static constexpr uint32_t kAt[8] = {12, 13, 20, 21, 28, 29, 36, 37};
    for (uint32_t k = 0; k < 8; ++k)
        g.W8(p + kAt[k], static_cast<uint8_t>(((k & 1u) ? v : u) + g.U8(t + k)));
    g.W16(p + 22u, static_cast<uint16_t>(TPage(g, d, flags)));
    g.W16(p + 14u, g.U16(d + 20u));
    g.W32(g.U32(kFxPacketHeapPtr) + 0x10Cu, p + 40u);
}

// ============================================================================ SLUS 0x8002A2C8
void EffectDraw(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec, uint32_t age, uint32_t view, uint32_t& flags) {
    env.drawing = rec;
    const uint32_t w = g.U32(rec);
    int16_t q[16] = {};
    int16_t vv[3];
    int32_t pos[3], b[3];
    auto sizeClut = [&]() { return static_cast<uint32_t>(g.U16(kFxSizeTable + 2u * static_cast<uint32_t>(g.S8(e + 0x216u)))); };
    auto diff = [&](uint32_t pa, uint32_t pb) { // rec[pa] - rec[pb], three s32
        for (uint32_t k = 0; k < 3; ++k)
            b[k] = static_cast<int32_t>(g.U32(rec + pa + 4u * k) - g.U32(rec + pb + 4u * k));
    };
    int16_t c[3];
    switch (State(w)) {
    case 1:
        ReadS32x3(g, rec + 4u, pos);
        EffectToView(g, env, pos, vv, view);
        diff(0x14u, 0x24u);
        if (EffectQuadAround(g, env, q, b, vv, view) != 0) return;
        EffectEmitQuad(g, env, q, 2, 3, 0, 25700);
        return;
    case 2: {
        uint32_t s2 = sizeClut();
        const uint32_t mask = (Kind(w) == 2u) ? 4u : 8u;
        if ((g.U32(e + 0x234u) & mask) != 0 && (flags & 4u) == 0) {
            const uint32_t t = DivU((Clock(g) - g.U32(rec + 0x30u)) << 17, g.U32(rec + 0x34u));
            int16_t dir[3];
            ReadS16x3(g, e + 0x1C2u, dir);
            int32_t step[3];
            Scale(static_cast<int32_t>(t), dir, step);
            WriteS32x3(g, rec + 0x24u, step);
            for (uint32_t k = 0; k < 3; ++k)
                b[k] = static_cast<int32_t>(g.U32(rec + 4u + 4u * k) -
                                            static_cast<uint32_t>(g.S32(rec + 0x24u + 4u * k) >> 10));
            flags |= 4u;
            ReadS32x3(g, rec + 4u, pos);
            EffectToView(g, env, pos, vv, view);
            if (EffectQuadAround(g, env, q, b, vv, view) == 0)
                EffectEmitQuad(g, env, q, 0, 1, s2 == 2u ? 1u : 0u, 25650);
        }
        if (Kind(g.U32(rec)) != 0) return;
        s2 += (age >= 76u) ? 1u : 0u;
        ReadS16x3(g, rec + 0x44u, c);
        EffectQuadSpin(g, q, c, g.S16(rec + 0x38u), static_cast<uint32_t>(g.U16(rec + 0x3Eu)) + age);
        EffectEmitQuad(g, env, q, 0, 2, s2, 25650);
        return;
    }
    case 3: {
        const uint32_t s2 = sizeClut();
        ReadS32x3(g, rec + 4u, pos);
        EffectToView(g, env, pos, vv, view);
        diff(0x14u, 0x24u);
        if (EffectQuadAround(g, env, q, b, vv, view) != 0) return;
        EffectEmitQuad(g, env, q, 0, 2, s2, 0);
        return;
    }
    case 4:
        ReadS32x3(g, rec + 4u, pos);
        EffectToView(g, env, pos, vv, view);
        diff(0x04u, 0x24u);
        if (EffectQuadAround(g, env, q, b, vv, view) != 0) return;
        EffectEmitDebris(g, env, q, 2, 8, g.U8(rec + 0x68u + g.U8(rec + 0x6Cu)));
        return;
    case 6:
        ReadS16x3(g, rec + 0x44u, c);
        EffectQuadSpin(g, q, c, g.S16(rec + 0x38u), static_cast<uint32_t>(g.U16(rec + 0x3Eu)) + 25u);
        EffectEmitQuad(g, env, q, 0, 9, age & 1u, 25650);
        return;
    case 7: {
        uint32_t s2;
        if (g.U8(rec + 0x3Cu) & 1u)
            s2 = sizeClut() + ((g.S32(e + 0x1E0u) <= 0x7FFFF) ? 1u : 0u);
        else
            s2 = ((g.U32(rec + 0x34u) >> 1) < age) ? 1u : 0u;
        if (g.S16(rec + 0x48u) < 0) g.W32(rec, (g.U32(rec) & ~0x3C00u) | 0x1000u);
        ReadS16x3(g, rec + 0x44u, c);
        EffectQuadSpin(g, q, c, g.S16(rec + 0x38u), static_cast<uint32_t>(g.U16(rec + 0x3Eu)) + (age >> 1));
        EffectEmitQuad(g, env, q, 0, 2, s2, 0);
        return;
    }
    default: // 0, 5 and 8..15 draw nothing here
        return;
    }
}

// ============================================================================ SLUS 0x8002AB14
void EffectDrawLight(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec) {
    env.drawing = rec;
    const int32_t ref = g.S32(kFxLightScale);
    if (Sub(g.U32(rec)) == 0) return;
    if (g.S32(rec + 0x5Cu) <= 0) return;
    if (g.S8(e + 8u) < 2) {
        g.W16(rec + 0x3Eu, static_cast<uint16_t>(g.U16(rec + 0x58u) << 1));
        const int32_t depth = g.S32(rec + 0x5Cu);
        if ((ref >> 2) < depth) {
            const int32_t r = (g.S32(rec + 0x58u) << 12) / depth; // div (signed; depth > 0 here)
            g.W32(rec + 0x58u, static_cast<uint32_t>(MulShift12(r, ref << 12) >> 12));
        }
        const int32_t base = g.U16(rec + 0x3Eu);
        const int32_t size = g.S32(rec + 0x58u);
        int32_t out;
        if (base < size) {
            if (size < 6) out = 12;
            else out = (g.U16(rec + 0x3Eu) < g.S32(rec + 0x58u)) ? g.S32(rec + 0x58u) << 1
                                                                  : static_cast<int32_t>(g.U16(rec + 0x3Eu)) << 1;
        } else {
            if (base < 6) out = 12;
            else out = (g.U16(rec + 0x3Eu) < g.S32(rec + 0x58u)) ? g.S32(rec + 0x58u) << 1
                                                                  : static_cast<int32_t>(g.U16(rec + 0x3Eu)) << 1;
        }
        g.W32(rec + 0x58u, static_cast<uint32_t>(out));
    }
    if (g.S8(rec + 0x61u) != 0) g.W32(rec + 0x58u, g.U32(rec + 0x58u) << 1);
    int16_t q[16] = {};
    int16_t c[3];
    g.W16(rec + 0x54u, 0);
    ReadS16x3(g, rec + 0x50u, c);
    const int32_t angle = static_cast<int32_t>(static_cast<uint32_t>(g.S16(rec + 0x50u) + g.S16(rec + 0x52u)) << 19) >> 16;
    EffectQuadSpin(g, q, c, angle, g.U32(rec + 0x58u));
    const int32_t depth = g.S32(rec + 0x5Cu);
    const uint32_t d = kFxSpriteTable + kFxSpriteBytes * static_cast<uint32_t>(g.S8(rec + 0x60u));
    const uint32_t colour = g.U32(kFxLightColour + 4u * static_cast<uint32_t>(g.S8(rec + 0x61u)));
    uint32_t p = 0, ot = 0;
    if (!HeapHead(g, env, p, ot)) return;
    int32_t bucket = OtBucket(g, depth);
    bucket -= (bucket >= 3) ? 3 : 0;
    const uint32_t slot = ot + 4u * static_cast<uint32_t>(bucket);
    g.W32(p + 4u, colour | 0x2E000000u);
    g.W32(p, g.U32(slot) | 0x09000000u);
    g.W32(slot, p);
    static constexpr int kV[4] = {0, 4, 8, 12};
    for (uint32_t k = 0; k < 4; ++k) {
        g.W16(p + 8u + 8u * k, static_cast<uint16_t>(q[kV[k]]));
        g.W16(p + 10u + 8u * k, static_cast<uint16_t>(q[kV[k] + 1]));
    }
    const uint32_t u = g.U32(d), v = g.U32(d + 4u);
    const uint32_t uR = u + g.U32(d + 8u) - 1u, vB = v + g.U32(d + 12u) - 1u;
    g.W8(p + 12u, static_cast<uint8_t>(u)), g.W8(p + 13u, static_cast<uint8_t>(v));
    g.W8(p + 20u, static_cast<uint8_t>(uR)), g.W8(p + 21u, static_cast<uint8_t>(v));
    g.W8(p + 28u, static_cast<uint8_t>(u)), g.W8(p + 29u, static_cast<uint8_t>(vB));
    g.W8(p + 36u, static_cast<uint8_t>(uR)), g.W8(p + 37u, static_cast<uint8_t>(vB));
    g.W16(p + 22u, static_cast<uint16_t>(g.U16(d + 16u) | 0x20u));
    g.W16(p + 14u, g.U16(d + 20u));
    g.W32(g.U32(kFxPacketHeapPtr) + 0x10Cu, p + 40u);
    if (env.sink != nullptr) env.sink->Packet(p, depth, 0x8002AB14u);
}

// ============================================================================ SLUS 0x8002A738
void EffectFree(GuestRam& g, uint32_t e, uint32_t prev, uint32_t rec, uint32_t next) {
    const uint32_t w24 = g.U32(e + 0x24u);
    switch (State(g.U32(rec))) {
    case 1:
    case 3:
        g.W32(e + 0x24u, (w24 & 0xF9FFFFFFu) | ((((w24 >> 25) & 3u) - 1u) & 3u) << 25);
        break;
    case 2:
        if (Kind(g.U32(rec)) == 2u)
            g.W32(e + 0x24u, (w24 & 0xFE7FFFFFu) | ((((w24 >> 23) & 3u) - 1u) & 3u) << 23);
        else
            g.W32(e + 0x24u, (w24 & 0xFF87FFFFu) | ((((w24 >> 19) & 15u) - 1u) & 15u) << 19);
        break;
    case 4:
        g.W32(e + 0x24u, (w24 & 0x3FFFFFFFu) | (((w24 >> 30) - 1u) << 30));
        break;
    case 5:
        g.W32(e + 0x24u, g.U32(e + 0x24u) & 0xC7FFFFFFu); // SLUS 0x8002820C
        break;
    case 7:
        g.W32(e + 0x24u, (w24 & 0xFF87FFFFu) | ((((w24 >> 19) & 15u) - 1u) & 15u) << 19);
        break;
    default:
        break;
    }
    g.W8(rec + 0x3Cu, 0);
    g.W32(rec, (g.U32(rec) & ~0x3C0u & ~0x3C00u) | 0x3Fu);
    if (prev != 0) g.W32(prev, (g.U32(prev) & ~0x3Fu) | (next & 0x3Fu));
    else g.W8(e + 0x49u, static_cast<uint8_t>(next));
}

// ============================================================================ SLUS 0x8002A8E4
void EffectBlink(GuestRam& g, uint32_t age, uint32_t rec) {
    if (!(g.U32(kFxLightPeriod + 4u * static_cast<uint32_t>(g.S8(rec + 0x61u))) < age)) return;
    const int8_t phase = g.S8(rec + 0x61u);
    g.W32(rec + 0x30u, Clock(g));
    g.W32(rec, (g.U32(rec) & ~0x3C00u) | 0x800u);
    if (phase == 0) return;
    const uint8_t n = static_cast<uint8_t>(g.U8(rec + 0x61u) - 1u);
    g.W8(rec + 0x61u, n);
    if (n == 0) g.W8(rec + 0x61u, 2);
}

// ============================================================================ SLUS 0x8002A974
uint32_t EffectLight(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec, uint32_t age, uint32_t view) {
    const uint32_t type = (g.U16(g.U32(e) + 14u) & 0x78u) >> 3;
    EffectBlink(g, age, rec);
    if (type != 2u) return 1;
    const uint32_t r = g.U32(e + 0x354u);
    bool off = true;
    if (g.U32(r + 0x25Cu) < 3u) {
        const uint32_t st = g.U16(r + 0x220u);
        if (g.U16(kFxStanceTable + 8u * st + 2u) != 0 || st != 0) off = false;
    }
    if (off) {
        g.W32(rec, g.U32(rec) & ~0x3C00u);
        g.W32(e + 0x24u, g.U32(e + 0x24u) & 0xEFFFFFFFu & 0xDFFFFFFFu);
        const uint32_t h = g.U16(e + 0xACu);
        if (h < Players(g)) {
            if (env.callees == nullptr || !env.callees->SirenStop(h)) env.Refuse("SLUS 0x8001836C (the siren stop)");
        }
    }
    if (!(((g.S32(e + 0x2Cu + 4u * view) << 10) >> 16) < 36)) g.W32(rec, g.U32(rec) & ~0x3C00u);
    if (Sub(g.U32(rec)) == 0) return 0;
    g.W32(e + 0x24u, g.U32(e + 0x24u) | 0x10000000u | 0x20000000u);
    const uint32_t h = g.U16(e + 0xACu);
    if (h < Players(g)) {
        if (env.callees == nullptr || !env.callees->SirenStart(h)) env.Refuse("SLUS 0x800182B0 (the siren start)");
    }
    return 1;
}

// ============================================================================ SLUS 0x80029D88
void EffectUpdate(GuestRam& g, FxEnv& env, uint32_t e, uint32_t rec, uint32_t view, uint32_t age) {
    bool kill = false;
    {
        const uint32_t w = g.U32(rec);
        if (State(w) != 5u && g.U32(rec + 0x34u) < age) {
            g.W32(rec, (w & ~0x3C00u) | 0x1000u);
            return;
        }
    }
    if (g.U8(rec + 0x6Du) != 0 && State(g.U32(rec)) != 5u &&
        g.U32(rec + 0x60u) < Clock(g) - g.U32(rec + 0x64u)) {
        const uint32_t n = g.U8(rec + 0x6Du);
        const uint32_t f = g.U8(rec + 0x6Cu);
        g.W8(rec + 0x6Cu, static_cast<uint8_t>(f == n - 1u ? 0u : f + 1u));
        g.W32(rec + 0x64u, Clock(g));
    }
    int32_t v[3], a[3];
    auto startSub = [&](bool copy) {
        if (Sub(g.U32(rec)) != 1u) return;
        if (copy) {
            const uint32_t w = g.U32(rec);
            const uint32_t y = g.U32(rec + 8u), z = g.U32(rec + 12u);
            const uint32_t x = g.U32(rec + 4u);
            g.W32(rec + 0x18u, y);
            g.W32(rec + 0x1Cu, z);
            g.W32(rec, (w & ~0x3C00u) | 0x800u);
            g.W32(rec + 0x14u, x);
        } else {
            g.W32(rec, (g.U32(rec) & ~0x3C00u) | 0x800u);
        }
    };
    auto drift = [&](int32_t scale) { // 0x8002A04C: the step is (now - drawn) * scale
        for (uint32_t k = 0; k < 3; ++k)
            v[k] = static_cast<int32_t>(g.U32(rec + 4u + 4u * k) - g.U32(rec + 0x14u + 4u * k));
        WriteS32x3(g, rec + 0x24u, v);
        ReadS32x3(g, rec + 0x24u, v);
        Scale32(scale, v, a);
        WriteS32x3(g, rec + 0x24u, a);
        kill = (g.U32(e + 0x238u) & 0x600u) != 0;
    };
    switch (State(g.U32(rec))) {
    case 1: {
        uint32_t who = e;
        uint32_t off = kEffectOffsetTable + 12u * Kind(g.U32(rec));
        if (Kind(g.U32(rec)) == 3u && g.U32(e + 0x358u) != 0 && g.U32(e + 0x440u) != 0) {
            who = g.U32(e + 0x358u);
            off = kFxPartnerOffset;
        }
        EffectLocalToWorld(g, who, off, rec + 4u);
        drift(0x3333);
        break;
    }
    case 2:
        EffectLocalToWorld(g, e, kEffectOffsetTable + 12u * Kind(g.U32(rec)), rec + 4u);
        startSub(true);
        drift(0x4CCC);
        g.W16(rec + 0x38u, static_cast<uint16_t>(g.U16(rec + 0x38u) + g.U16(rec + 0x3Au))); // 0x80028E74
        break;
    case 3: {
        int32_t in[3], out[3];
        ReadS32x3(g, rec + 0x50u, in);
        EffectFromModel(g, env, e, out, in, view);
        WriteS32x3(g, rec + 4u, out);
        startSub(true);
        drift(0x8000);
        break;
    }
    case 4: {
        uint32_t t = DivU((Clock(g) - g.U32(rec + 0x30u)) << 17, g.U32(rec + 0x34u));
        int32_t in[3], out[3];
        ReadS32x3(g, rec + 0x50u, in);
        EffectFromModel(g, env, e, out, in, view);
        WriteS32x3(g, rec + 4u, out);
        startSub(false);
        if (0x10000 < static_cast<int32_t>(t)) t = 0x20000u - t;
        int16_t dir[3];
        ReadS16x3(g, e + 0x1C2u, dir);
        Scale(static_cast<int32_t>(t), dir, a);
        WriteS32x3(g, rec + 0x24u, a);
        g.W32(rec + 0x24u, static_cast<uint32_t>(g.S32(rec + 0x24u) >> 10));
        g.W32(rec + 0x2Cu, static_cast<uint32_t>(g.S32(rec + 0x2Cu) >> 10));
        g.W32(rec + 0x28u, static_cast<uint32_t>(g.S32(rec + 0x28u) >> 10));
        break;
    }
    case 5:
        EffectLight(g, env, e, rec, age, view);
        break;
    case 6: {
        int32_t in[3], out[3];
        ReadS32x3(g, rec + 0x50u, in);
        EffectFromModel(g, env, e, out, in, view);
        WriteS32x3(g, rec + 0x14u, out);
        g.W16(rec + 0x38u, static_cast<uint16_t>(g.U16(rec + 0x38u) + g.U16(rec + 0x3Au))); // 0x80028E74
        break;
    }
    case 7:
        g.W32(rec + 0x24u, 0);
        g.W32(rec + 0x2Cu, 0);
        g.W32(rec, (g.U32(rec) & ~0x3C00u) | 0x800u);
        break;
    default:
        break;
    }
    if (State(g.U32(rec)) != 5u) {
        if (g.S8(rec + 0x3Du) != 0) {
            const uint32_t t = g.U32(kFxJitterClock) & 0xFFFu;
            const int32_t cs = static_cast<int32_t>(g.S16(kFxSinCos + 4u * t + 2u)) << 4;
            g.W32(rec + 0x24u, static_cast<uint32_t>(MulShift16(cs, g.S32(rec + 0x40u))) + g.U32(rec + 0x24u));
            const uint32_t t2 = g.U32(kFxJitterClock) & 0xFFFu;
            const int32_t sn = static_cast<int32_t>(g.S16(kFxSinCos + 4u * t2)) << 4;
            g.W32(rec + 0x2Cu, static_cast<uint32_t>(MulShift16(sn, g.S32(rec + 0x40u))) + g.U32(rec + 0x2Cu));
        }
        const uint32_t x = g.U32(rec + 0x14u) + g.U32(rec + 0x24u);
        const uint32_t dy = g.U32(rec + 0x28u), dz = g.U32(rec + 0x2Cu);
        g.W32(rec + 0x14u, x);
        g.W32(rec + 0x18u, g.U32(rec + 0x18u) + dy);
        g.W32(rec + 0x1Cu, g.U32(rec + 0x1Cu) + dz);
    }
    if (kill) g.W32(rec, (g.U32(rec) & ~0x3C00u) | 0x1000u);
}

// ============================================================================ SLUS 0x8002823C
void EffectPass(GuestRam& g, FxEnv& env, uint32_t e, uint32_t view) {
    uint32_t flags = 0; // the walker's sp+24
    const int32_t head = g.S8(e + 0x49u);
    bool walk = head != -1;
    if (walk && g.S8(e + 8u) > 0 && State(g.U32(Record(head))) < 5u) walk = false;
    if (walk) {
        const uint32_t n = g.U32(kFxJitterClock) + 1u;
        g.W32(kFxJitterClock, n);
        if (!(static_cast<int32_t>(n) < 4097)) g.W32(kFxJitterClock, 0);
        uint32_t prev = 0;
        uint32_t rec = Record(head);
        while (rec != 0) {
            const int32_t next = Link(g.U32(rec));
            const uint32_t age = Clock(g) - g.U32(rec + 0x30u);
            const uint32_t st = State(g.U32(rec));
            if ((g.U8(e + 9u) & 8u) == 0 || st == 5u || st == 6u) EffectUpdate(g, env, e, rec, view, age);
            const uint32_t w = g.U32(rec);
            if (Sub(w) == 4u) {
                EffectFree(g, e, prev, rec, static_cast<uint32_t>(next));
            } else if (State(w) == 5u) {
                EffectDrawLight(g, env, e, rec);
                prev = rec;
            } else {
                int32_t pos[3];
                int16_t vv[3];
                ReadS32x3(g, rec + 0x14u, pos);
                EffectToView(g, env, pos, vv, view);
                for (uint32_t k = 0; k < 3; ++k) g.W16(rec + 0x44u + 2u * k, static_cast<uint16_t>(vv[k]));
                EffectDraw(g, env, e, rec, age, view, flags);
                prev = rec;
            }
            if (env.refused || g.Faulted()) return;
            rec = (next == -1) ? 0u : Record(next);
        }
        g.W8(e + 9u, static_cast<uint8_t>(g.U8(e + 9u) | 8u));
    }
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t child = g.U32(e + 0x38u + 8u * k);
        if (child != 0) EffectPass(g, env, child, view);
        if (env.refused || g.Faulted()) return;
    }
}

// ============================================================================ SLUS 0x8002990C
void EffectCapture(GuestRam& g, uint32_t e, uint32_t rec) {
    const uint32_t w = g.U32(rec);
    const uint32_t st = State(w);
    const uint32_t verts = g.U32(kFxModelVerts);
    auto copyVertex = [&](uint32_t v) {
        g.W16(rec + 0x4Cu, static_cast<uint16_t>(v));
        const uint32_t at = verts + 16u * (v & 0xFFFFu);
        const uint32_t a = g.U32(at), b = g.U32(at + 4u), c = g.U32(at + 8u), d = g.U32(at + 12u);
        g.W32(rec + 0x50u, a), g.W32(rec + 0x54u, b), g.W32(rec + 0x58u, c), g.W32(rec + 0x5Cu, d);
    };
    auto half = [](int32_t s) { return (s + static_cast<int32_t>(static_cast<uint32_t>(s) >> 31)) >> 1; };
    if (st == 4u) {
        const uint32_t a = verts + 16u * g.U16(rec + 0x4Cu);
        const uint32_t b = verts + 16u * g.U16(rec + 0x4Eu);
        for (uint32_t k = 0; k < 3; ++k)
            g.W32(rec + 0x50u + 4u * k,
                  static_cast<uint32_t>(half(static_cast<int32_t>(g.U32(a + 4u * k) + g.U32(b + 4u * k)))));
        return;
    }
    if (st == 3u) {
        const uint32_t part = g.U32(g.U32(e + 4u) + 24u * Kind(w));
        copyVertex((g.U16(part + 0x10u) + 1u) & 0xFFFFu);
        return;
    }
    if (st == 6u) {
        const uint32_t model = g.U32(e);
        const uint32_t part = g.U32(24u * g.U16(model + 0x18u) + g.U32(e + 4u) - 24u);
        copyVertex(g.U16(part + 0x10u));
        return;
    }
    if (st != 5u) return;
    const uint32_t model = g.U32(e);
    const uint32_t lod = static_cast<uint32_t>(g.S8(e + 8u));
    const uint32_t ex = g.U16(model + 14u) >> 12;
    const uint32_t ent = ((w >> 11) & 0x7F8u) + kFxLightParts + 2u * lod;
    const int32_t poly = g.S8(ent);
    const uint32_t screen = g.U32(kFxModelScreen);
    if (poly == -1) {
        g.W32(rec + 0x5Cu, static_cast<uint32_t>(poly));
        return;
    }
    const uint32_t set = kFxPartSets + 4u * static_cast<uint32_t>(g.S8(ent + 1u));
    uint32_t partIdx = g.U16(model + 0x18u) - 1u;
    if (g.S8(set) != -1) partIdx = static_cast<uint32_t>(g.S8(set));
    const uint32_t part = g.U32(24u * partIdx + g.U32(e + 4u));
    const uint32_t pr = g.U32(part + 20u) + 20u * static_cast<uint32_t>(poly) + 4u;
    const int32_t z = g.S32(verts + 16u * g.U16(pr + 12u) + 8u) >> ex;
    g.W32(rec + 0x5Cu, static_cast<uint32_t>(z));
    if (z <= 0) return;
    if (g.S8(set + 1u) == 1) {
        const uint32_t s = screen + 4u * g.U16(pr + 12u + 2u * static_cast<uint32_t>(g.S8(set + 2u)));
        g.W16(rec + 0x50u, g.U16(s));
        g.W16(rec + 0x52u, g.U16(s + 2u));
    } else {
        const uint32_t s1 = screen + 4u * g.U16(pr + 12u + 2u * static_cast<uint32_t>(g.S8(set + 2u)));
        const uint32_t s2 = screen + 4u * g.U16(pr + 12u + 2u * static_cast<uint32_t>(g.S8(set + 3u)));
        g.W16(rec + 0x50u, static_cast<uint16_t>(half(g.S16(s1) + g.S16(s2))));
        g.W16(rec + 0x52u, static_cast<uint16_t>(half(g.S16(s1 + 2u) + g.S16(s2 + 2u))));
    }
    const int32_t l = g.S8(e + 8u);
    if (!(l < 2)) {
        g.W32(rec + 0x58u, l == 2 ? 6u : 4u);
        return;
    }
    const uint32_t a = screen + 4u * g.U16(pr + 12u);
    const uint32_t b = screen + 4u * g.U16(pr + 16u);
    uint32_t dx = static_cast<uint32_t>(g.S16(a) - g.S16(b));
    if (static_cast<int32_t>(dx) < 0) dx = 0u - dx;
    const int32_t hx = static_cast<int32_t>(dx) >> 1;
    uint32_t dy = static_cast<uint32_t>(g.S16(a + 2u) - g.S16(b + 2u));
    if (static_cast<int32_t>(dy) < 0) dy = 0u - dy;
    const int32_t hy = static_cast<int32_t>(dy) >> 1;
    g.W32(rec + 0x58u, static_cast<uint32_t>(hy < hx ? hx : hy));
}

// ============================================================================ SLUS 0x80029CA4
void EffectCaptureChain(GuestRam& g, uint32_t e) {
    const int32_t head = g.S8(e + 0x49u);
    if (head == -1) return;
    if (g.S8(e + 8u) > 0 && State(g.U32(Record(head))) < 5u) return;
    uint32_t rec = Record(head);
    while (rec != 0) {
        const int32_t next = Link(g.U32(rec));
        EffectCapture(g, e, rec);
        if (g.Faulted()) return;
        rec = (next == -1) ? 0u : Record(next);
    }
}

// ============================================================================ SLUS 0x8002847C
void EffectFreeChain(GuestRam& g, uint32_t e) {
    const int32_t head = g.S8(e + 0x49u);
    if (head == -1) return;
    uint32_t rec = Record(head);
    while (rec != 0) {
        const int32_t next = Link(g.U32(rec));
        EffectFree(g, e, 0, rec, static_cast<uint32_t>(next));
        if (g.Faulted()) return;
        rec = (next == -1) ? 0u : Record(next);
    }
}

// ============================================================================ SLUS 0x80027258
void SurfaceFx(GuestRam& g, FxEnv& env, uint32_t e, uint32_t kind) {
    const int32_t speed = g.S16(e + 0x1E2u);
    if (!(g.U16(e + 0xACu) < Players(g))) return;
    if (!(((g.U32(e + 0x24u) >> 25) & 3u) < 2u)) return;
    const int32_t i = EffectFindFree(g);
    if (i == -1) return;
    const uint32_t rec = Record(i);
    g.W32(rec, (((g.U32(rec) & 0xFFFFFC3Fu) | 0x40u) & 0xFFC03FFFu) | ((kind & 0xFFu) << 14));
    const uint32_t w24 = g.U32(e + 0x24u);
    g.W32(e + 0x24u, (w24 & 0xF9FFFFFFu) | ((((w24 >> 25) & 3u) + 1u) & 3u) << 25);
    EffectLocalToWorld(g, e, kEffectOffsetTable + 12u * kind, rec + 0x14u);
    const int32_t a = 150 - 2 * speed;
    g.W32(rec + 0x34u, static_cast<uint32_t>(ClampHigh(a, 0x960000)));
    g.W32(rec + 0x30u, Clock(g));
    EffectJitterSpray(g, rec, speed, env.io);
    g.W32(rec + 0x24u, 0);
    g.W32(rec + 0x2Cu, 0);
    g.W32(rec + 0x28u, 0);
    g.W8(rec + 0x3Du, 0);
    g.W16(rec + 0x3Eu, 30);
    EffectLink(g, e, static_cast<uint32_t>(i));
}

} // namespace rr::sim
