// The sky's own programs (sky_draw.h). Transcribed from our own disassembly of SLUS_010.53 (SHA-1 67ed165a...),
// RASHCDG.BIN (cfe43a77..., at 0x8005B5E8) and RASHCDI.BIN (9a8b79d8..., at 0x8005B5E8); the Ghidra pseudo-C of
// work\ghidra as the reading aid (it drops the heap manager's size argument and 0x800102A4's row loop - the
// listing is followed). Each accepted by its rrverify row (rows_sky2.inc).
#include "game/sim/sky_draw.h"

#include <algorithm>

#include "game/sim/sky_split.h" // the gradient, the two-player sky, the heap manager
#include "game/sim/stream_cd.h" // SkyView (PORTED)

namespace rr::sim {

namespace {

using rc::S;
using rc::U;

uint32_t Sky(GuestRam& g) { return g.U32(kSkyBlockPtr); }
int32_t H16(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }
bool Call(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> a, uint32_t sp, uint32_t* v0 = nullptr) {
    return rc::Call(c, fn, a, sp, v0);
}
// The page column's height limit +0x40[(x - *(0x8005B390)) / 64] (the `bgez / addiu 63 / sra 6` of the listing).
int32_t PageLimit(GuestRam& g, uint32_t sky, int32_t x) {
    int32_t d = x - g.S32(kSkyVramX);
    if (d < 0) d += 63;
    return g.S16(sky + U((d >> 6) * 2) + 0x40u);
}
// The column angle (c * 0x94F) >> 6 (srl) & 0xFFF.
uint32_t ColumnAngle(uint32_t c) { return ((c * 0x94Fu) >> 6) & 0xFFFu; }

// The ported callees answered natively; every other goes to the caller's seam.
struct Native final : RecoverCallees {
    GuestRam& g;
    RecoverCallees& outer;
    Native(GuestRam& gg, RecoverCallees& o) : g(gg), outer(o) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        if (fn == kSkyDecodeFn) {
            v0 = 0;
            return DecodeStart(g, sp, outer);
        }
        if (fn == kSkyTileFn) return TileUpload(g, sp, outer, v0);
        return outer.Call(fn, a, n, sp, v0);
    }
};

// DecodeStart and TileUpload call each other once per column pair; a state the game never builds (+0x1E <= 0 with
// the pair decoded) makes the original recurse until its stack runs out - refused here instead of the host's stack.
struct Depth {
    static int& N() {
        static thread_local int n = 0;
        return n;
    }
    Depth() { ++N(); }
    ~Depth() { --N(); }
    bool Deep() const { return N() > 4000; }
};

// The heap manager 0x80021C98 for a request of `bytes` at `cur`: PORTED (sky_split.h), or the callees' (o.seams).
bool Heap(GuestRam& g, uint32_t cur, uint32_t bytes, uint32_t sp, RecoverCallees& c, const SkyDrawOptions& o, uint32_t& v0) {
    if (o.seams) return Call(c, kHeapFullFn, {cur, bytes}, sp, &v0);
    return HeapNext(g, cur, bytes, sp, c, v0);
}

// One RTPS of (x, y, z) with the result's SXY stored at `sxy` and SZ3 >> 2 at `sz`.
void Rtps(GuestRam& g, FxGte& gte, int16_t x, int16_t y, int16_t z, uint32_t sxy, uint32_t sz) {
    const int16_t v[3] = {x, y, z};
    g.W32(sxy, gte.Rtps(v));
    g.W32(sz, static_cast<uint32_t>(static_cast<int32_t>(gte.sz[3]) >> 2));
}

} // namespace

// ============================================================================ RASHCDG 0x80064B9C
bool SkyDraw(GuestRam& g, uint32_t view, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyDrawOptions& o) {
    const uint32_t F = sp - kSkyDrawFrame;
    gte.tr[0] = gte.tr[1] = gte.tr[2] = 0;
    const uint32_t m = g.U32(kSkyRenderCams + 4u * view) + 0x5Cu; // SetRotMatrix 0x8004D154 (a libgte leaf)
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t w = g.U32(m + 4u * k);
        gte.rt[2 * k] = static_cast<int16_t>(w & 0xFFFFu);
        gte.rt[2 * k + 1] = static_cast<int16_t>(w >> 16);
    }
    gte.rt[8] = static_cast<int16_t>(g.U32(m + 16u) & 0xFFFFu);
    Native n(g, c);
    SkyView(g, view, F, n);
    if (g.Faulted()) return false;
    const uint32_t gs = g.U32(kSkyGameState);
    if (g.U32(gs + 48u) == 1u && g.U32(kSkySplitFlag) == 0 && g.S16(Sky(g) + 20u) != -1)
        if (!PanoDraw(g, gte, F, c, o)) return false;
    if ((g.U32(g.U32(kSkyGameState) + 48u) == 2u || g.U32(kSkySplitFlag) != 0) && g.U32(kSplitSkyOn) != 0)
        if (!(o.seams ? Call(c, kSkySplitFn, {view}, F) : SplitSkyDraw(g, view, gte, F, c, o.wide))) return false;
    if (g.U32(kCloudOn) != 0)
        if (!CloudDraw(g, gte, F, c, o)) return false;
    if (g.U32(kGradOn) != 0)
        if (!(o.seams ? Call(c, kSkyGradFn, {view}, F) : GradDraw(g, view, gte, F, c, o.wide))) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800644F4
bool PanoDraw(GuestRam& g, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyDrawOptions& o) {
    const uint32_t F = sp - kSkyPanoFrame;
    constexpr uint32_t s1 = 0x1F800000u;
    uint32_t sky = Sky(g);
    if (g.S16(sky + 18u) == -1) return !g.Faulted();
    g.W32(s1 + 0x28u, 0x1F800038u);
    g.W32(s1 + 0x2Cu, 0x1F800080u);
    if (g.U32(sky + 24u) != 0) { // the column decodes: wait for each, upload it, start the next
        uint32_t polls = 0;
        for (;;) {
            ++polls;
            if (g.U32(Sky(g) + 24u) == 2u) {
                uint32_t v0 = 0;
                if (!TileUpload(g, F, c, v0)) return false;
                polls = 1;
            }
            if (!(polls < 10000u)) { // 0x800647F0: the MDEC never answered
                sky = Sky(g);
                g.W32(sky + 24u, 0);
                g.W16(sky + 30u, 0);
                Call(c, kDecDctResetFn, {1}, F);
                return !g.Faulted();
            }
            if (g.U32(Sky(g) + 24u) == 0) break;
            if (g.Faulted()) return false;
        }
    }
    // 0x800645AC: the first drawn column's first cache slot
    uint32_t t3 = g.U32(g.U32(kSkyPacketRec) + 0x10Cu);
    sky = Sky(g);
    // the columns drawn: 25 from +0x30. OURS (the wide picture): SkyFrame keeps +0x30 .. +0x32 opened by the wide
    // margins (stream_cd.h SetSkyFrameWide) and the columns of it the decoded window +0x34 .. +0x36 holds are drawn (a
    // column outside it has no tiles in the cache: left out and counted)
    int32_t firstCol = g.S16(sky + 0x30u), ncols = 25;
    if (o.wide != nullptr) {
        const int32_t w0 = g.S16(sky + 0x34u), w1 = g.S16(sky + 0x36u);
        const auto dist = [](int32_t a, int32_t b) { return ((b - a) % 110 + 110) % 110; }; // columns from a on to b
        const int32_t size = dist(w0, w1) + 1;
        const int32_t wanted = 25 + o.wide->columnsLeft + o.wide->columnsRight;
        int32_t lo = -1, hi = -1;
        for (int32_t k = 0; k < wanted && k < 110; ++k)
            if (dist(w0, firstCol + k) < size) {
                if (lo < 0) lo = k;
                hi = k;
            }
        if (lo >= 0) {
            firstCol = (firstCol + lo) % 110;
            ncols = hi - lo + 1;
            o.wide->panoAdded += std::max(0, ncols - 25);
            o.wide->panoMissing += wanted - ncols;
        }
    }
    int32_t s0 = g.S16(sky + 0x34u);
    int32_t s2 = g.S16(sky + 0x24u);
    {
        const int32_t stop = firstCol;
        while (s0 != stop) {
            s2 += g.U8(sky + U(s0) + 0xB6u);
            ++s0;
            if (!(s0 < 110)) s0 = 0;
            if (g.Faulted()) return false;
        }
    }
    sky = Sky(g);
    if (!(s2 < g.S16(sky + 0x26u))) s2 -= g.S16(sky + 0x26u);
    if (g.S16(sky + 0x2Eu) < s2 && s2 < g.S16(sky + 0x2Cu)) s2 = g.S16(sky + 0x2Cu);
    g.W32(s1 + 0x24u, g.U32(sky + 0x3E0u) + 8u);
    {
        const int32_t col = firstCol;
        g.W32(s1 + 4u, U(col));
        const uint32_t row = U(g.U8(g.U32(s1 + 0x24u) + U(col)) + g.U8(sky + U(col) + 0x48u));
        g.W32(s1 + 0xCu, row);
        uint32_t t1 = g.U32(s1 + 0x28u) + row * 4u;
        g.W32(s1 + 0x18u, row + g.U8(sky + U(col) + 0xB6u));
        const uint32_t ang = ColumnAngle(U(col));
        const int16_t vx = g.S16(kSkySinCos + ang * 4u), vz = g.S16(kSkySinCos + ang * 4u + 2u);
        int16_t vy = static_cast<int16_t>(row * 279u - 2232u);
        for (uint32_t n = g.U32(s1 + 0x18u) - (row - 1u); n != 0; --n) {
            Rtps(g, gte, vx, vy, vz, t1, s1 + 0x34u);
            vy = static_cast<int16_t>(static_cast<uint16_t>(vy) + 279u);
            t1 += 4u;
        }
    }
    g.W32(s1 + 4u, g.U32(s1 + 4u) == 109u ? 0u : g.U32(s1 + 4u) + 1u);
    sky = Sky(g);
    g.W32(s1 + 0x20u, g.S16(sky + 0x2Eu) < s2 ? U(g.S16(sky + 0x28u)) : U(g.S16(sky + 0x2Eu) + 1));
    g.W32(s1 + 0x30u, g.U32(g.U32(kSkyOtPtr)));
    g.W32(s1, 0);
    uint32_t s3 = Sky(g) + U(s2) * 12u + 0x2744u;
    do {
        sky = Sky(g);
        const uint32_t col = g.U32(s1 + 4u);
        const uint32_t first = U(g.U8(g.U32(s1 + 0x24u) + col) + g.U8(sky + col + 0x48u));
        g.W32(s1 + 0x10u, first);
        g.W32(s1 + 0x1Cu, first + g.U8(sky + col + 0xB6u));
        int32_t lo = S(first);
        if (g.S32(s1 + 0xCu) < lo) lo = g.S32(s1 + 0xCu);
        g.W32(s1 + 8u, U(lo));
        int32_t hi = g.S32(s1 + 0x18u);
        if (!(g.S32(s1 + 0x1Cu) < hi)) hi = g.S32(s1 + 0x1Cu);
        g.W32(s1 + 0x14u, U(hi));
        {
            uint32_t t1 = g.U32(s1 + 0x2Cu) + U(lo) * 4u;
            const uint32_t ang = ColumnAngle(col);
            const int16_t vx = g.S16(kSkySinCos + ang * 4u), vz = g.S16(kSkySinCos + ang * 4u + 2u);
            int16_t vy = static_cast<int16_t>(U(lo) * 279u - 2232u);
            for (uint32_t n = U(hi) - (U(lo) - 1u); n != 0; --n) {
                Rtps(g, gte, vx, vy, vz, t1, s1 + 0x34u);
                vy = static_cast<int16_t>(static_cast<uint16_t>(vy) + 279u);
                t1 += 4u;
            }
        }
        const int32_t tiles = g.S32(s1 + 0x18u) - g.S32(s1 + 0xCu);
        if (tiles > 0) {
            const uint32_t bytes = U(tiles) * 40u;
            if (!(t3 + bytes < g.U32(kSkyPacketEnd))) {
                uint32_t v0 = 0;
                if (!Heap(g, t3, bytes, F, c, o, v0)) return false;
                t3 = v0;
            }
            const uint32_t t0 = Sky(g);
            uint32_t ta = g.U32(s1 + 0x28u) + g.U32(s1 + 0xCu) * 4u;
            uint32_t tb = g.U32(s1 + 0x2Cu) + g.U32(s1 + 0xCu) * 4u;
            ++s2;
            for (int32_t k = tiles; k != 0; --k) {
                const uint32_t w0 = g.U32(s3), w1 = g.U32(s3 + 4u), w2 = g.U32(s3 + 8u);
                s3 += 12u;
                g.W32(t3, g.U32(s1 + 0x30u) | 0x09000000u);
                g.W32(s1 + 0x30u, t3);
                g.W32(t3 + 4u, g.U32(kSkyColour) | 0x2C000000u);
                g.W32(t3 + 8u, g.U32(ta));
                g.W32(t3 + 12u, w0);
                g.W32(t3 + 16u, g.U32(tb));
                g.W32(t3 + 20u, w1);
                g.W32(t3 + 24u, g.U32(ta + 4u));
                g.W32(t3 + 28u, w2 >> 16);
                g.W32(t3 + 32u, g.U32(tb + 4u));
                g.W32(t3 + 36u, w2);
                ta += 4u;
                tb += 4u;
                t3 += 40u;
                if (!(s2 < g.S32(s1 + 0x20u))) {
                    if (s2 == g.S16(t0 + 0x2Eu) + 1) {
                        s2 = g.S16(t0 + 0x2Cu);
                        s3 = t0 + U(s2) * 12u + 0x2744u;
                        g.W32(s1 + 0x20u, U(g.S16(t0 + 0x28u)));
                    } else if (!(s2 < g.S16(t0 + 0x28u))) {
                        s2 = 0;
                        s3 = t0 + 0x2744u;
                        g.W32(s1 + 0x20u, U(g.S16(t0 + 0x2Eu) + 1));
                    }
                }
                ++s2;
                if (g.Faulted()) return false;
            }
            --s2;
        }
        // the two SXY rows swapped, the next column's rows kept
        const uint32_t a = g.U32(s1 + 0x28u), b = g.U32(s1 + 0x2Cu);
        g.W32(s1 + 0x28u, b);
        g.W32(s1 + 0x2Cu, a);
        g.W32(s1 + 0xCu, g.U32(s1 + 0x10u));
        g.W32(s1 + 0x18u, g.U32(s1 + 0x1Cu));
        g.W32(s1 + 4u, g.U32(s1 + 4u) == 109u ? 0u : g.U32(s1 + 4u) + 1u);
        g.W32(s1, g.U32(s1) + 1u);
        if (g.Faulted()) return false;
    } while (g.S32(s1) < ncols);
    g.W32(g.U32(kSkyOtPtr), g.U32(s1 + 0x30u));
    g.W32(g.U32(kSkyPacketRec) + 0x10Cu, t3);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800657A8
bool TileUpload(GuestRam& g, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kSkyTileFrame;
    v0 = 0;
    const Depth depth;
    if (depth.Deep()) return false;
    g.W16(kSkyRect + 4u, 16);
    g.W16(kSkyRect + 6u, 16);
    uint32_t sky = Sky(g);
    const auto finish = [&]() { // 0x80065CCC: the columns to decode ran out
        const uint32_t s = Sky(g);
        g.W32(s + 24u, 0);
        g.W16(s + 30u, 0);
        v0 = 1;
        return !g.Faulted();
    };
    if (g.S16(sky + 28u) == 1) {
        // ---------------------------------------------------------------- to the right: column +0x36 + 1 on
        int32_t s4 = g.S16(sky + 0x36u) + 1;
        if (!(s4 < 110)) s4 = 0;
        int32_t t3 = 2 - (s4 & 1);
        if (g.S16(sky + 0x1Eu) < t3) t3 = g.S16(sky + 0x1Eu);
        for (--t3; t3 != -1; --t3) {
            sky = Sky(g);
            uint32_t s2 = g.U8(sky + U(s4) + 0xB6u);
            if (s2 != 0) {
                g.W16(kSkyRect, g.U16(sky + 0x3Cu));
                g.W16(kSkyRect + 2u, g.U16(sky + 0x3Eu));
                int32_t s8 = PageLimit(g, sky, g.S16(sky + 0x3Cu));
                uint32_t s1 = g.U32(sky + 0x3DCu) + 8u + U(s4) * 2u;
                uint32_t s5 = sky + U(g.S16(sky + U(s4) * 2u + 0x124u) + 0x4C4);
                int32_t s0 = 6;
                if ((g.U8(s1) >> 6) == 0) { // the column's first coded band
                    s0 = 4;
                    for (;;) {
                        if (s0 < 0) {
                            s0 = 6;
                            ++s1;
                        }
                        const uint32_t code = (U(g.U8(s1)) >> U(s0)) & 3u;
                        s0 -= 2;
                        if (code != 0) break;
                        if (g.Faulted()) return false;
                    }
                    s0 += 2;
                }
                sky = Sky(g);
                uint32_t s3 = g.U32(sky + 0x3E4u) + 0x48u + U(g.S16(sky + U(s4) * 2u + 0x200u)) * 8u;
                for (; s2 != 0; --s2) {
                    sky = Sky(g);
                    {
                        const uint16_t v = static_cast<uint16_t>(g.U16(sky + 0x2Eu) + 1u);
                        g.W16(sky + 0x2Eu, v);
                        if (g.S16(sky + 0x28u) - 1 < H16(v)) g.W16(sky + 0x2Eu, 0);
                    }
                    sky = Sky(g);
                    if (g.S16(sky + 0x20u) == 0 && g.S16(sky + 0x2Eu) == g.S16(sky + 0x2Cu)) {
                        // the ring is full: the oldest columns (from +0x34) give their slots back
                        uint32_t a2 = 0;
                        if (S(s2) > 0) {
                            do {
                                const uint32_t n = g.U8(sky + U(g.S16(sky + 0x34u)) + 0xB6u);
                                const uint16_t v = static_cast<uint16_t>(g.U16(sky + 0x34u) + 1u);
                                g.W16(sky + 0x34u, v);
                                if (H16(v) == 110) g.W16(sky + 0x34u, 0);
                                a2 += n;
                                if (g.Faulted()) return false;
                            } while (S(a2) < S(s2));
                        }
                        sky = Sky(g);
                        const uint32_t t7 = U(S(a2) * g.S16(kSkyRect + 6u));
                        const uint16_t y = static_cast<uint16_t>(g.U16(sky + 0x3Au) + t7);
                        g.W16(sky + 0x3Au, y);
                        const int32_t limit = PageLimit(g, sky, g.S16(sky + 0x38u));
                        if (!(H16(y) < limit)) {
                            const uint16_t a3 = g.U16(kSkyVramX), t1 = g.U16(kSkyVramY);
                            const int32_t t0 = H16(U(a3) + 192u);
                            do {
                                g.W16(sky + 0x3Au, static_cast<uint16_t>(U(t1) + (U(g.U16(sky + 0x3Au)) - U(limit))));
                                const uint16_t x = static_cast<uint16_t>(g.U16(sky + 0x38u) + g.U16(kSkyRect + 4u));
                                g.W16(sky + 0x38u, x);
                                if (!(H16(x) < t0)) g.W16(sky + 0x38u, a3);
                                if (g.Faulted()) return false;
                            } while (!(g.S16(sky + 0x3Au) < limit));
                        }
                        sky = Sky(g);
                        {
                            const uint32_t v = U(g.U16(sky + 0x24u)) + a2;
                            g.W16(sky + 0x24u, static_cast<uint16_t>(v));
                            if (!(H16(v) < g.S16(sky + 0x26u))) g.W16(sky + 0x24u, static_cast<uint16_t>(v - g.U16(sky + 0x26u)));
                        }
                        sky = Sky(g);
                        {
                            const uint32_t v = U(g.U16(sky + 0x2Cu)) + a2;
                            g.W16(sky + 0x2Cu, static_cast<uint16_t>(v));
                            if (!(H16(v) < g.S16(sky + 0x26u))) g.W16(sky + 0x2Cu, static_cast<uint16_t>(v - g.U16(sky + 0x26u)));
                        }
                    }
                    // 0x80065B1C: this tile's code
                    const uint32_t code = (U(g.U8(s1)) >> U(s0)) & 3u;
                    s0 -= 2;
                    if (s0 < 0) {
                        s0 = 6;
                        ++s1;
                    }
                    if (code == 2u) {
                        HorzCut(g, s5, s3);
                        s3 += 8u;
                    }
                    if (!Call(c, kSkyLoadImageFn, {kSkyRect, s5}, F)) return false;
                    sky = Sky(g);
                    {
                        const uint16_t v = static_cast<uint16_t>(g.U16(sky + 0x26u) + 1u);
                        g.W16(sky + 0x26u, v);
                        if (g.S16(sky + 0x28u) < H16(v)) g.W16(sky + 0x26u, g.U16(sky + 0x28u));
                    }
                    s5 += 512u;
                    const uint16_t y = static_cast<uint16_t>(g.U16(kSkyRect + 2u) + g.U16(kSkyRect + 6u));
                    g.W16(kSkyRect + 2u, y);
                    if (!(H16(y) < s8)) {
                        g.W16(kSkyRect + 2u, g.U16(kSkyVramY));
                        const uint16_t a0 = g.U16(kSkyVramX);
                        const uint16_t x = static_cast<uint16_t>(g.U16(kSkyRect) + g.U16(kSkyRect + 4u));
                        g.W16(kSkyRect, x);
                        if (!(H16(x) < H16(U(a0) + 192u))) g.W16(kSkyRect, a0);
                        s8 = PageLimit(g, Sky(g), g.S16(kSkyRect));
                    }
                    if (g.Faulted()) return false;
                }
                sky = Sky(g);
                g.W16(sky + 0x3Cu, g.U16(kSkyRect));
                g.W16(sky + 0x20u, 0);
                g.W16(sky + 0x3Eu, g.U16(kSkyRect + 2u));
            }
            sky = Sky(g);
            const uint16_t left = static_cast<uint16_t>(g.U16(sky + 0x1Eu) - 1u);
            g.W16(sky + 0x36u, static_cast<uint16_t>(s4));
            g.W16(sky + 0x1Eu, left);
            ++s4;
            if (H16(left) <= 0) return finish();
            if (!(s4 < 110)) s4 = 0;
        }
    } else {
        // ---------------------------------------------------------------- to the left: column +0x34 - 1 down
        int32_t s4 = g.S16(sky + 0x34u) - 1;
        if (s4 < 0) s4 = 109;
        int32_t t3 = (s4 & 1) + 1;
        if (g.S16(sky + 0x1Eu) < t3) t3 = g.S16(sky + 0x1Eu);
        for (--t3; t3 != -1; --t3) {
            sky = Sky(g);
            uint32_t s2 = g.U8(sky + U(s4) + 0xB6u);
            if (s2 != 0) {
                g.W16(kSkyRect, g.U16(sky + 0x38u));
                g.W16(kSkyRect + 2u, g.U16(sky + 0x3Au));
                int32_t s0 = 0;
                uint32_t s5 = sky + U(g.S16(sky + U(s4) * 2u + 0x124u) + 0x4C4) + ((s2 - 1u) << 9);
                uint32_t s1 = g.U32(sky + 0x3DCu) + U(s4) * 2u + 9u;
                if ((g.U8(s1) & 3u) == 0) { // the column's last coded band
                    s0 = 2;
                    for (;;) {
                        if (!(s0 < 7)) {
                            s0 = 0;
                            --s1;
                        }
                        const uint32_t code = (U(g.U8(s1)) >> U(s0)) & 3u;
                        s0 += 2;
                        if (code != 0) break;
                        if (g.Faulted()) return false;
                    }
                    s0 -= 2;
                }
                sky = Sky(g);
                uint32_t s3 = g.U32(sky + 0x3E4u) + 0x48u;
                if (s4 < 109)
                    s3 += U(g.S16(sky + U(s4 + 1) * 2u + 0x200u) - 1) * 8u;
                else // column 109: the column's FIRST code-2 record (the original's walk after it stores nothing)
                    s3 += U(g.S16(sky + U(s4) * 2u + 0x200u)) * 8u;
                for (; s2 != 0; --s2) {
                    sky = Sky(g);
                    {
                        const uint16_t v = static_cast<uint16_t>(g.U16(sky + 0x2Cu) - 1u);
                        g.W16(sky + 0x2Cu, v);
                        if (H16(v) < 0) g.W16(sky + 0x2Cu, static_cast<uint16_t>(g.U16(sky + 0x28u) - 1u));
                    }
                    sky = Sky(g);
                    if (g.S16(sky + 0x20u) == 0 && g.S16(sky + 0x2Eu) == g.S16(sky + 0x2Cu)) {
                        uint32_t a2 = 0;
                        if (S(s2) > 0) {
                            do {
                                const uint32_t n = g.U8(sky + U(g.S16(sky + 0x36u)) + 0xB6u);
                                const uint16_t v = static_cast<uint16_t>(g.U16(sky + 0x36u) - 1u);
                                g.W16(sky + 0x36u, v);
                                if (H16(v) < 0) g.W16(sky + 0x36u, 109);
                                a2 += n;
                                if (g.Faulted()) return false;
                            } while (S(a2) < S(s2));
                        }
                        sky = Sky(g);
                        const uint32_t t7 = U(S(a2) * g.S16(kSkyRect + 6u));
                        const int32_t vramY = g.S32(kSkyVramY);
                        const uint16_t y = static_cast<uint16_t>(g.U16(sky + 0x3Eu) - t7);
                        g.W16(sky + 0x3Eu, y);
                        if (H16(y) < vramY) {
                            const uint16_t a3 = g.U16(kSkyVramX);
                            const int32_t t1 = g.S16(kSkyVramX);
                            do {
                                const uint16_t x = static_cast<uint16_t>(g.U16(sky + 0x3Cu) - g.U16(kSkyRect + 4u));
                                g.W16(sky + 0x3Cu, x);
                                if (H16(x) < t1)
                                    g.W16(sky + 0x3Cu, static_cast<uint16_t>(U(a3) - (U(g.U16(kSkyRect + 4u)) - 192u)));
                                const int32_t lim = PageLimit(g, sky, g.S16(sky + 0x3Cu));
                                const uint16_t y2 = static_cast<uint16_t>(U(g.U16(sky + 0x3Eu)) + U(lim) - U(g.U16(kSkyVramY)));
                                g.W16(sky + 0x3Eu, y2);
                                if (g.Faulted()) return false;
                            } while (g.S16(sky + 0x3Eu) < vramY);
                        }
                        sky = Sky(g);
                        {
                            const uint16_t v = static_cast<uint16_t>(U(g.U16(sky + 0x2Eu)) - a2);
                            g.W16(sky + 0x2Eu, v);
                            if (H16(v) < 0) g.W16(sky + 0x2Eu, static_cast<uint16_t>(v + g.U16(sky + 0x26u)));
                        }
                    }
                    // 0x80066050: this tile's code
                    const uint32_t code = (U(g.U8(s1)) >> U(s0)) & 3u;
                    s0 += 2;
                    if (!(s0 < 7)) {
                        s0 = 0;
                        --s1;
                    }
                    if (code == 2u) {
                        HorzCut(g, s5, s3);
                        s3 -= 8u;
                    }
                    if (!Call(c, kSkyLoadImageFn, {kSkyRect, s5}, F)) return false;
                    const uint16_t y = static_cast<uint16_t>(g.U16(kSkyRect + 2u) - g.U16(kSkyRect + 6u));
                    g.W16(kSkyRect + 2u, y);
                    s5 -= 512u;
                    if (H16(y) < g.S32(kSkyVramY)) {
                        const uint16_t w = g.U16(kSkyRect + 4u);
                        const uint16_t x = static_cast<uint16_t>(g.U16(kSkyRect) - w);
                        g.W16(kSkyRect, x);
                        if (H16(x) < g.S16(kSkyVramX)) {
                            g.W16(kSkyRect, static_cast<uint16_t>(U(g.U16(kSkyVramX)) - (U(w) - 192u)));
                            const uint32_t s = Sky(g);
                            g.W16(s + 0x26u, g.U16(s + 0x28u));
                        }
                        const int32_t s8 = PageLimit(g, Sky(g), g.S16(kSkyRect));
                        g.W16(kSkyRect + 2u, static_cast<uint16_t>(U(s8) - g.U16(kSkyRect + 6u)));
                    }
                    sky = Sky(g);
                    {
                        const uint16_t v = static_cast<uint16_t>(g.U16(sky + 0x24u) - 1u);
                        g.W16(sky + 0x24u, v);
                        if (H16(v) < 0) g.W16(sky + 0x24u, static_cast<uint16_t>(g.U16(sky + 0x28u) - 1u));
                    }
                    if (g.Faulted()) return false;
                }
                sky = Sky(g);
                g.W16(sky + 0x20u, 0);
                g.W16(sky + 0x38u, g.U16(kSkyRect));
                g.W16(sky + 0x3Au, g.U16(kSkyRect + 2u));
            }
            sky = Sky(g);
            if (g.S16(sky + 0x28u) < g.S16(sky + 0x26u)) g.W16(sky + 0x26u, g.U16(sky + 0x28u));
            sky = Sky(g);
            const uint16_t left = static_cast<uint16_t>(g.U16(sky + 0x1Eu) - 1u);
            g.W16(sky + 0x34u, static_cast<uint16_t>(s4));
            g.W16(sky + 0x1Eu, left);
            --s4;
            if (H16(left) <= 0) return finish();
            if (s4 < 0) s4 = 109;
        }
    }
    // 0x80066258: the uploads done, the next pair's decode
    for (;;) {
        uint32_t busy = 0;
        if (!Call(c, kSkyDrawSyncFn, {1}, F, &busy)) return false;
        if (busy == 0) break;
        if (!Call(c, kSkyEmptyFn, {}, F)) return false;
    }
    if (!DecodeStart(g, F, c)) return false;
    v0 = 0;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800662BC
bool DecodeStart(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - kSkyDecodeFrame;
    const Depth depth;
    if (depth.Deep()) return false;
    uint32_t sky = Sky(g);
    const int32_t a3 = g.S16(sky + 0x34u), a2 = g.S16(sky + 0x36u);
    const int32_t t2 = a3 / 2, t1 = a2 / 2;
    const auto stop = [&](uint32_t s) { // +0x18 = 0, +0x1E = 0
        g.W32(s + 24u, 0);
        g.W16(s + 30u, 0);
        return !g.Faulted();
    };
    int32_t v1 = 0;
    const int32_t dir = g.S16(sky + 28u);
    if (dir == 0) {
        v1 = a3 - 1;
        if (v1 < 0) v1 = 109;
    } else if (dir == 1) {
        v1 = a2 + 1;
        if (!(v1 < 110)) v1 = 0;
    } else {
        return stop(sky);
    }
    int32_t s0 = v1 / 2;
    sky = Sky(g);
    int32_t a0 = g.S16(sky + 32u);
    if (s0 == g.S16(sky + 20u)) { // the pair is the one decoded: its columns to the cache
        uint32_t v0 = 0;
        return TileUpload(g, F, c, v0);
    }
    uint32_t e = g.U32(sky + U(s0) * 4u + 1000u);
    if (e == 0) return stop(sky);
    uint32_t s1 = e + 8u;
    int32_t steps = 0, a1 = 0;
    if (g.U32(s1 + 4u) == 0) { // this pair not in the panorama's chunk: walk on to one that is
        const uint32_t t0 = sky;
        for (;;) {
            ++steps;
            if (steps == 55) { // 0x8006664C: none - the whole ring
                const uint32_t s = Sky(g);
                g.W16(s + 52u, 0);
                g.W16(s + 54u, 109);
                return stop(s);
            }
            if (g.S16(t0 + 28u) == 1) {
                if (a0 == 0) {
                    if (s0 == t2) {
                        g.W32(t0 + 24u, 0);
                        g.W16(t0 + 30u, 0);
                        const uint16_t v = static_cast<uint16_t>(g.U16(t0 + 52u) - 1u);
                        g.W16(t0 + 54u, v);
                        if (H16(v) < 0) g.W16(t0 + 54u, 109);
                        return !g.Faulted();
                    }
                    ++s0;
                } else {
                    const int32_t b = s0 * 2 - 1;
                    const int32_t old = g.S16(t0 + 54u);
                    a0 = 0;
                    g.W16(t0 + 54u, static_cast<uint16_t>(b));
                    g.W16(t0 + 52u, static_cast<uint16_t>(s0 * 2));
                    a1 = old - b;
                    ++s0;
                }
                if (!(s0 < 55)) s0 = 0;
            } else {
                if (a0 == 0) {
                    if (s0 == t1) {
                        g.W32(t0 + 24u, 0);
                        g.W16(t0 + 30u, 0);
                        const int32_t old = g.S16(t0 + 54u);
                        g.W16(t0 + 52u, static_cast<uint16_t>(g.U16(t0 + 54u) + 1u));
                        if (!(old < 110)) g.W16(t0 + 54u, 0);
                        return !g.Faulted();
                    }
                    --s0;
                } else {
                    const int32_t b = (s0 + 1) * 2;
                    const int32_t old = g.S16(t0 + 52u);
                    a0 = 0;
                    g.W16(t0 + 52u, static_cast<uint16_t>(b));
                    g.W16(t0 + 54u, static_cast<uint16_t>(b - 1));
                    a1 = b - old;
                    --s0;
                }
                if (s0 < 0) s0 = 54;
            }
            e = g.U32(t0 + U(s0) * 4u + 1000u);
            if (e == 0) return stop(t0);
            s1 = e + 8u;
            if (g.U32(s1 + 4u) != 0) break;
            if (g.Faulted()) return false;
        }
    }
    if (steps != 0) { // 0x800664C8: the columns walked past are skipped
        sky = Sky(g);
        const uint32_t twice = U(steps) * 2u;
        if (g.S16(sky + 28u) == 1) {
            const uint16_t v = static_cast<uint16_t>(g.U16(sky + 54u) + twice);
            g.W16(sky + 54u, v);
            if (!(H16(v) < 111)) g.W16(sky + 54u, static_cast<uint16_t>(v - 110u));
        } else {
            const uint16_t v = static_cast<uint16_t>(g.U16(sky + 52u) - twice);
            g.W16(sky + 52u, v);
            if (H16(v) < 0) g.W16(sky + 52u, static_cast<uint16_t>(v + 110u));
        }
        sky = Sky(g);
        const uint16_t left = static_cast<uint16_t>(g.U16(sky + 30u) - twice + U(a1));
        g.W16(sky + 30u, left);
        if (!(H16(left) > 0)) {
            g.W16(sky + 30u, 0);
            g.W32(sky + 24u, 0);
            return !g.Faulted();
        }
    }
    // 0x8006656C: the strip {w, h, BS} at s1
    if (!(U(S(g.U32(s1 + 4u)) * S(g.U32(s1))) - 1u < 4096u)) return stop(Sky(g));
    const uint32_t vlcOut = Sky(g) - (U(g.U16(s1 + 8u)) * 4u - 9920u);
    if (!Call(c, kDecDctInSyncFn, {0}, F)) return false;
    if (!Call(c, kDecDctOutSyncFn, {0}, F)) return false;
    g.W32(Sky(g) + 24u, 2);
    if (DctVlc(g, s1 + 8u, vlcOut) != 0) return stop(Sky(g));
    sky = Sky(g);
    const uint32_t mode = U(g.S16(sky + 22u));
    g.W32(sky + 732u, s1);
    g.W16(sky + 20u, static_cast<uint16_t>(s0));
    g.W32(sky + 24u, 1);
    if (!Call(c, kDecDctInFn, {vlcOut, mode}, F)) return false;
    const int32_t words = S(U(S(g.U32(s1 + 4u)) * S(g.U32(s1)))) / 2;
    if (!Call(c, kDecDctOutFn, {Sky(g) + 1220u, U(words)}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80013AE4
void MdecDone(GuestRam& g) { g.W32(g.U32(kSkyBlockPtr) + 24u, 2); }

// ============================================================================ SLUS 0x800102A4
void HorzCut(GuestRam& g, uint32_t tile, uint32_t record) {
    uint32_t row = tile + 30u;
    for (uint32_t r = 0; r < 16u; ++r, row += 32u) {
        const uint32_t n = (U(g.U8(record + (r >> 1))) >> (((r + 1u) & 1u) << 2)) & 0xFu;
        if (n == 0) continue;
        // halfwords 15 - n .. 15 of the row (the jump table's word stores after one halfword to align)
        for (uint32_t k = 0; k <= n; ++k) g.W16(row - 2u * k, 0);
    }
}

// ============================================================================ SLUS 0x80020400
namespace {
const VlcHostTables*& HostTables() {
    static thread_local const VlcHostTables* t = nullptr;
    return t;
}
} // namespace

void SetVlcHostTables(const VlcHostTables* tables) { HostTables() = tables; }

uint32_t DctVlc(GuestRam& g, uint32_t bs, uint32_t out) {
    const VlcHostTables* host = HostTables();
    const uint32_t t4 = host ? 0u : g.U32(g.gp() + kGpVlcTab0), t3 = host ? 0u : g.U32(g.gp() + kGpVlcTab1);
    const uint32_t t7 = t4 + 8192u, t6 = t3 + 512u;
    uint32_t t1 = bs, a1 = out, t0 = 0;
    g.W16(a1, g.U16(t1));
    a1 += 2u;
    g.W16(a1, g.U16(t1 + 2u));
    a1 += 2u;
    const uint32_t q = U(g.U16(t1 + 4u)) << 10;
    uint32_t a3 = (U(g.U16(t1 + 8u)) << 16) | g.U16(t1 + 10u);
    t1 += 12u;
    uint32_t a0 = a3 >> 22;
    const auto refill = [&]() {
        if (t0 & 0x10u) {
            t0 &= 0xFu;
            a3 |= U(g.U16(t1)) << t0;
            t1 += 2u;
        } else {
            t0 &= 0xFu;
        }
    };
    for (uint32_t guard = 0; guard < 0x100000u; ++guard) {
        if (a0 == 511u) break;
        t0 += 10u;
        a3 <<= 10;
        refill();
        g.W16(a1, static_cast<uint16_t>(a0 | q));
        a1 += 2u;
        for (;;) { // the block's AC codes
            a0 = a3 >> 19;
            uint32_t len = 0, sym = 0;
            if (a0 < 32u) {
                t0 += 8u;
                a3 <<= 8;
                refill();
                a0 = a3 >> 23;
                if (host != nullptr) {
                    len = host->tab1[a0];
                    sym = static_cast<uint32_t>(host->tab1[512u + a0 * 2u] | (host->tab1[513u + a0 * 2u] << 8));
                } else {
                    len = g.U8(t3 + a0);
                    sym = g.U16(t6 + a0 * 2u);
                }
            } else if (host != nullptr) {
                len = host->tab0[a0];
                sym = static_cast<uint32_t>(host->tab0[8192u + a0 * 2u] | (host->tab0[8193u + a0 * 2u] << 8));
            } else {
                len = g.U8(t4 + a0);
                sym = g.U16(t7 + a0 * 2u);
            }
            a3 <<= (len & 31u);
            t0 += len;
            refill();
            if (sym == 0x7C1Fu) { // escape: the next 16 bits as they stand
                g.W16(a1, static_cast<uint16_t>(a3 >> 16));
                a1 += 2u;
                const uint32_t hw = g.U16(t1);
                t1 += 2u;
                a3 = (a3 << 16) | (hw << (t0 & 31u));
                continue;
            }
            g.W16(a1, static_cast<uint16_t>(sym));
            a1 += 2u;
            if (sym == 0xFE00u) break;
            if (g.Faulted()) return 0;
        }
        a0 = a3 >> 22;
        if (g.Faulted()) return 0;
    }
    // 0x8002056C: EOB padding to a 128-byte multiple of the payload
    uint32_t n = (a1 - out) - 4u;
    if ((n & 0x7Fu) != 0) {
        if (a1 & 2u) {
            g.W16(a1, 0xFE00u);
            a1 += 2u;
            n = (a1 - 2u - out) - 2u;
        }
        while ((n & 0x7Fu) != 0) {
            g.W32(a1, 0xFE00FE00u);
            n += 4u;
            a1 += 4u;
        }
    }
    return 0;
}

// ============================================================================ RASHCDG 0x8006396C
bool CloudDraw(GuestRam& g, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyDrawOptions& o) {
    const uint32_t F = sp - kSkyCloudFrame;
    int32_t s6 = (g.S16(kSkyColumn) * 24) / 110;
    // eight segments; OURS (the wide picture): more on each side, at most the ring's 24
    int32_t nseg = 8;
    uint32_t bytes = 320u;
    if (o.wide != nullptr) {
        int32_t l = o.wide->segmentsLeft, r = o.wide->segmentsRight;
        if (8 + l + r > 24) l = r = 8;
        s6 = ((s6 - l) % 24 + 24) % 24;
        nseg = 8 + l + r;
        bytes = U(nseg) * 40u;
    }
    uint32_t s2 = U(s6) * 16u + kCloudRing;
    const uint32_t ot = g.U32(kSkyOtPtr) + 4u;
    if (!(g.U32(g.U32(kSkyPacketRec) + 0x10Cu) + bytes < g.U32(kSkyPacketEnd))) {
        uint32_t v0 = 0;
        if (!Heap(g, g.U32(g.U32(kSkyPacketRec) + 0x10Cu), bytes, F, c, o, v0)) return false;
        g.W32(g.U32(kSkyPacketRec) + 0x10Cu, v0);
    }
    uint32_t s4 = g.U32(g.U32(kSkyPacketRec) + 0x10Cu);
    // the edges' SXY (top, bottom) in two pairs, and whether both points of an edge are off one side
    uint32_t sxy[8] = {};
    const auto edge = [&](uint32_t at, uint32_t slot) {
        const int16_t top[3] = {g.S16(at), g.S16(at + 2u), g.S16(at + 4u)};
        sxy[slot] = gte.Rtps(top);
        const int16_t bot[3] = {g.S16(at + 8u), g.S16(at + 10u), g.S16(at + 12u)};
        sxy[slot + 1] = gte.Rtps(bot);
        const int32_t x0 = H16(sxy[slot]), x1 = H16(sxy[slot + 1]);
        if (o.wide != nullptr) { // OURS: both points off the wide picture's side
            const int32_t lo = -o.wide->left, hi = 385 + o.wide->right;
            sxy[slot + 2] = (U(x0 < lo && x1 < lo) << 15) | U(x0 >= hi && x1 >= hi);
            return;
        }
        sxy[slot + 2] = (sxy[slot] & sxy[slot + 1] & 0x8000u) | U((x0 >= 385) && (x1 >= 385));
    };
    edge(s2, 0);
    s2 += 16u;
    uint32_t s3 = 0;
    for (int s7 = 0; s7 < nseg; ++s7, ++s6) {
        if (s6 == 23) s2 = kCloudRing;
        s3 ^= 4u;
        edge(s2, s3);
        s2 += 16u;
        if ((sxy[2] & sxy[6]) == 0) {
            const uint32_t a0 = s3 ^ 4u;
            const uint32_t slice = kCloudSlices + U(s6 & 3) * 12u;
            g.W32(s4, g.U32(ot) | 0x09000000u);
            g.W32(s4 + 4u, 0x2F000000u);
            g.W32(s4 + 8u, sxy[a0]);
            g.W32(s4 + 12u, g.U32(slice));
            g.W32(s4 + 16u, sxy[s3]);
            g.W32(s4 + 20u, g.U32(slice + 4u));
            g.W32(s4 + 24u, sxy[a0 + 1u]);
            g.W32(s4 + 28u, g.U16(slice + 10u));
            g.W32(s4 + 32u, sxy[s3 + 1u]);
            g.W32(s4 + 36u, g.U32(slice + 8u));
            g.W32(ot, s4);
            s4 += 40u;
        }
        if (g.Faulted()) return false;
    }
    g.W32(g.U32(kSkyPacketRec) + 0x10Cu, s4);
    return !g.Faulted();
}

// ============================================================================ SLUS 0x800202B8
int32_t VlcBuild(GuestRam& g, uint32_t tab0, uint32_t tab1, uint32_t symbols) {
    g.W32(g.gp() + kGpVlcTab0, tab0);
    g.W32(g.gp() + kGpVlcTab1, tab1);
    if (tab0 == 0 || tab1 == 0) return -1;
    if (symbols == 0) symbols = 0x80052C20u;
    uint32_t desc = 0x800528A0u;
    for (int i = 0; i < 0x60; ++i, desc += 4u) {
        const uint16_t sym = g.U16(symbols);
        symbols += 2u;
        const uint32_t len = g.U8(desc);
        const uint32_t code = g.U16(desc + 2u);
        const int32_t fill = 1 << ((13u - len) & 31u);
        uint32_t at = code >> 3;
        for (int32_t k = 0; k < fill;) {
            ++k;
            g.W8(tab0 + at, static_cast<uint8_t>(len));
            g.W16(at * 2u + tab0 + 0x2000u, sym);
            at = (code >> 3) | U(k);
        }
    }
    desc = 0x80052A20u;
    for (int i = 0; i < 0x80; ++i, desc += 4u) {
        const uint16_t sym = g.U16(symbols);
        symbols += 2u;
        const uint32_t len = g.U8(desc);
        const uint32_t code = g.U16(desc + 2u);
        const int32_t fill = 1 << ((9u - len) & 31u);
        uint32_t at = code >> 7;
        for (int32_t k = 0; k < fill;) {
            ++k;
            g.W8(tab1 + at, static_cast<uint8_t>(len));
            g.W16(at * 2u + tab1 + 0x200u, sym);
            at = (code >> 7) | U(k);
        }
    }
    return 0;
}

// ============================================================================ libgpu leaves
uint16_t GpuTPage(uint32_t tp, uint32_t abr, uint32_t x, uint32_t y) {
    return static_cast<uint16_t>(((tp & 3u) << 7) | ((abr & 3u) << 5) | ((y & 0x100u) >> 4) | ((x & 0x3FFu) >> 6) |
                                 ((y & 0x200u) << 2));
}
uint16_t GpuClut(uint32_t x, uint32_t y) {
    return static_cast<uint16_t>((y << 6) | (U(S(x) >> 4) & 0x3Fu));
}

// ============================================================================ RASHCDI 0x80060AA8
void TileRecords(GuestRam& g) {
    // the RECT {x, y, 16, 16} the original keeps in its frame is host locals here; TileRecord 0x80060894 inline
    uint32_t count = 0;
    uint32_t rec = Sky(g) + 0x2744u;
    int32_t x = g.S32(kSkyVramX);
    for (int col = 0; col < 12; ++col) {
        int32_t d = x - g.S32(kSkyVramX);
        if (d < 0) d += 63;
        int32_t rows = g.S16(Sky(g) + U((d >> 6) * 2) + 0x40u) - g.S32(kSkyVramY);
        if (rows < 0) rows += 15;
        rows >>= 4;
        int32_t y = g.S32(kSkyVramY);
        for (int32_t r = 0; r < rows; ++r) {
            // TileRecord(rec, 2, {x, y, 16, 16}, 0, 0)
            const uint32_t sh = 0;
            g.W16(rec + 6u, GpuTPage(2, 0, U(x), U(y)));
            g.W16(rec + 2u, GpuClut(0, 0));
            g.W8(rec + 8u, static_cast<uint8_t>((U(x) & 0x3Fu) << sh));
            g.W8(rec + 10u, static_cast<uint8_t>((U(x) & 0x3Fu) << sh));
            g.W8(rec + 4u, static_cast<uint8_t>(g.U8(rec + 8u) + 16u - 1u));
            g.W8(rec + 0u, static_cast<uint8_t>(g.U8(rec + 10u) + 16u - 1u));
            g.W8(rec + 1u, static_cast<uint8_t>(y));
            g.W8(rec + 11u, static_cast<uint8_t>(y));
            g.W8(rec + 5u, static_cast<uint8_t>(g.U8(rec + 1u) + 16u - 1u));
            g.W8(rec + 9u, static_cast<uint8_t>(g.U8(rec + 11u) + 16u - 1u));
            rec += 12u;
            y += 16;
            ++count;
        }
        x += 16;
    }
    g.W16(Sky(g) + 0x28u, static_cast<uint16_t>(count));
}

// ============================================================================ RASHCDI 0x80060780
void CloudSlice(GuestRam& g, uint32_t rec, uint32_t mode, uint32_t rect, uint32_t clutX, uint32_t clutY) {
    const uint32_t sh = (2u - (mode & 7u)) & 31u;
    g.W16(rec + 6u, GpuTPage(mode & 7u, 0, g.U32(rect), g.U32(rect + 4u)));
    g.W16(rec + 2u, GpuClut(clutX, clutY));
    g.W8(rec + 0u, static_cast<uint8_t>((g.U32(rect) & 0x3Fu) << sh));
    g.W8(rec + 10u, static_cast<uint8_t>((g.U32(rect) & 0x3Fu) << sh));
    g.W8(rec + 4u, static_cast<uint8_t>(g.U8(rec + 0u) + (g.U32(rect + 8u) << sh) - 1u));
    g.W8(rec + 8u, static_cast<uint8_t>(g.U8(rec + 10u) + (g.U32(rect + 8u) << sh) - 1u));
    g.W8(rec + 1u, g.U8(rect + 4u));
    g.W8(rec + 5u, g.U8(rect + 4u));
    g.W8(rec + 11u, static_cast<uint8_t>(g.U8(rec + 1u) + g.U8(rect + 12u)));
    g.W8(rec + 9u, static_cast<uint8_t>(g.U8(rec + 5u) + g.U8(rect + 12u)));
}

// ============================================================================ RASHCDI 0x80061100
void CloudRing(GuestRam& g) {
    const int32_t height = g.S32(kCloudBand);
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 25u; ++i) {
        acc &= 0xFFF00u;
        if (i == 24u) acc = 0;
        const uint32_t ang = U(S(acc) >> 8);
        int32_t y = g.S32(kCloudBand + 4u);
        for (uint32_t k = 0; k < 2u; ++k) {
            const uint32_t at = kCloudRing + i * 16u + k * 8u;
            g.W16(at, g.U16(kSkySinCos + ang * 4u));
            g.W16(at + 2u, static_cast<uint16_t>(y));
            y += height;
            g.W16(at + 4u, g.U16(kSkySinCos + ang * 4u + 2u));
        }
        acc += 0xAA00u;
    }
}

} // namespace rr::sim
