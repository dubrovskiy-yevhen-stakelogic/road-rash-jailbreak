// The rest of the sky's programs (sky_split.h). Transcribed from our own disassembly of SLUS_010.53
// (SHA-1 67ed165a...), RASHCDG.BIN (cfe43a77..., at 0x8005B5E8) and RASHCDI.BIN (9a8b79d8..., at 0x8005B5E8), the
// listing followed where Ghidra's pseudo-C stops (0x8001FC90 / 0x80021C98 taken as not returning). Each accepted by
// its rrverify row (rows_sky3.inc).
#include "game/sim/sky_split.h"

#include "game/sim/sky_draw.h"

namespace rr::sim {

namespace {

using rc::S;
using rc::U;

constexpr uint32_t kFrameRecord = 0x8005B470; // -> the frame record (+0xF0 heap start, +0xF4 buffers, +0x108 OT, +0x10C cursor)
constexpr uint32_t kHeapEndWord = 0x8005B4D0; // gp+0x844
constexpr uint32_t kSinCos = 0x8005624C;
constexpr uint32_t kGameState = 0x8005B2F8;
constexpr uint32_t kSplitStream = 0x8005B2D0; // sky_draw.h kSkySplitFlag
constexpr uint32_t kHeapGuard = 1u << 20;     // loop turns before the port refuses (a busy buffer that never ages)

bool Call(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> a, uint32_t sp, uint32_t* v0 = nullptr) {
    return rc::Call(c, fn, a, sp, v0);
}
int16_t Sin(GuestRam& g, uint32_t a) { return g.S16(kSinCos + (a & 0xFFFu) * 4u); }
int16_t Cos(GuestRam& g, uint32_t a) { return g.S16(kSinCos + (a & 0xFFFu) * 4u + 2u); }
// RotTransPers 0x8004D1B4's SXY of the SVECTOR at `v` into `sxy` (its IR0 / FLAG go to the caller's frame).
void Rtp(GuestRam& g, FxGte& gte, uint32_t v, uint32_t sxy) {
    const int16_t p[3] = {g.S16(v), g.S16(v + 2u), g.S16(v + 4u)};
    g.W32(sxy, gte.Rtps(p));
}
// The heap request of `bytes` at the cursor when it would reach the limit (the callers' shared idiom).
bool Reserve(GuestRam& g, uint32_t bytes, uint32_t sp, RecoverCallees& c) {
    const uint32_t cur = g.U32(g.U32(kFrameRecord) + 0x10Cu);
    if (cur + bytes < g.U32(kHeapEndWord)) return true;
    uint32_t v0 = 0;
    if (!HeapNext(g, cur, bytes, sp, c, v0)) return false;
    g.W32(g.U32(kFrameRecord) + 0x10Cu, v0);
    return !g.Faulted();
}

} // namespace

// ============================================================================ SLUS 0x80021C98
bool HeapNext(GuestRam& g, uint32_t cursor, uint32_t size, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kHeapNextFrame;
    const uint32_t gp = g.gp();
    const auto G = [&](uint32_t o) { return g.U32(gp + o); };
    const auto P = [&](uint32_t o, uint32_t v) { g.W32(gp + o, v); };
    uint32_t turns = 0;
    uint32_t s0 = cursor;
    bool t2 = false;
    // 0x80021D34..0x80021DEC / 0x80021E64..0x80021F1C: the oldest buffer's age test, then gp+0x178 to the first busy
    // buffer after gp+0x174 (or back to it)
    const auto step = [&](uint32_t a2, uint32_t rec) {
        const uint32_t a0 = G(kGpFrameOtNext);
        if (!(g.U32(kHeapVsync) - g.U32(kHeapStamps + a0 * 4u) < 61u)) g.W32(kHeapBusy + a0 * 64u, 0);
        const uint32_t t1 = a2 + 1u;
        P(kGpFrameOtNext, t1);
        if (!(S(t1) < S(g.U8(rec + 0xF4u)))) P(kGpFrameOtNext, 0);
        uint32_t v1 = G(kGpFrameOtNext);
        if (g.U32(kHeapBusy + v1 * 64u) == 0) {
            for (;;) {
                if (v1 == a2) break;
                ++v1;
                P(kGpFrameOtNext, v1);
                if (!(S(v1) < S(g.U8(rec + 0xF4u)))) P(kGpFrameOtNext, 0);
                v1 = G(kGpFrameOtNext);
                if (g.U32(kHeapBusy + v1 * 64u) != 0) break;
                if (++turns > kHeapGuard || g.Faulted()) return false;
            }
        }
        return ++turns <= kHeapGuard && !g.Faulted();
    };
    enum { kA, kB } path = G(kGpHeapLimit) == G(kGpHeapEnd) ? kA : kB;
    const uint32_t endAtEntry = G(kGpHeapEnd);
    for (;;) {
        if (path == kA) { // 0x80021CBC: from the heap's start
            const uint32_t rec = g.U32(kFrameRecord);
            s0 = g.U32(rec + 0xF0u) & 0x00FFFFFFu;
            const uint32_t t0 = s0 + size;
            {
                const uint32_t v1 = g.U32(kHeapMarks + G(kGpFrameOtNext) * 4u);
                if (t0 < v1) {
                    P(kGpHeapLimit, v1);
                    v0 = s0;
                    return !g.Faulted();
                }
            }
            const uint32_t a2 = G(kGpFrameOtBuf);
            if (G(kGpFrameOtNext) != a2) {
                for (;;) {
                    if (!step(a2, rec)) return false;
                    const uint32_t a0 = G(kGpFrameOtNext);
                    const uint32_t v1 = g.U32(kHeapMarks + a0 * 4u);
                    if (t0 < v1) {
                        P(kGpHeapLimit, v1);
                        t2 = true;
                        break;
                    }
                    if (a0 == a2) break;
                }
            }
            break; // 0x80021FF8
        }
        // 0x80021E20: the limit is a buffer's mark
        bool toA = false;
        if (G(kGpFrameOtNext) != G(kGpFrameOtBuf)) {
            const uint32_t a2 = G(kGpFrameOtBuf);
            const uint32_t rec = g.U32(kFrameRecord);
            const uint32_t t3 = endAtEntry, t1 = s0 + size;
            const bool t5 = t1 < t3;
            for (;;) {
                if (!step(a2, rec)) return false;
                const uint32_t a0 = g.U32(kHeapMarks + G(kGpFrameOtNext) * 4u);
                if (G(kGpHeapLimit) != a0) {
                    if (s0 < a0) {
                        P(kGpHeapLimit, a0);
                    } else {
                        P(kGpHeapLimit, t3);
                        if (!t5) {
                            toA = true;
                            break;
                        }
                        t2 = true;
                        break;
                    }
                }
                if (t1 < G(kGpHeapLimit)) { // 0x80021F5C
                    t2 = true;
                    break;
                }
                if (G(kGpFrameOtNext) == a2) break;
            }
        }
        if (toA) {
            path = kA;
            continue;
        }
        // 0x80021F80
        if (t2) {
            v0 = s0;
            return !g.Faulted();
        }
        {
            const uint32_t v1 = g.U32(kHeapMarks + G(kGpFrameOtNext) * 4u);
            if (G(kGpHeapLimit) != v1) {
                if (s0 < v1) {
                    P(kGpHeapLimit, v1);
                } else {
                    const uint32_t end = G(kGpHeapEnd);
                    P(kGpHeapLimit, end);
                    if (!(s0 + size < end)) {
                        path = kA;
                        continue;
                    }
                    t2 = true;
                }
            }
        }
        if (s0 + size < G(kGpHeapLimit)) t2 = true; // 0x80021FE0
        break;
    }
    // 0x80021FF8
    if (!t2)
        if (!Call(c, kHeapFlushFn, {s0}, F)) return false;
    v0 = s0;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80021BE8
bool HeapFlush(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - kHeapFlushFrame;
    const uint32_t gp = g.gp();
    if (!Call(c, kPrintfFn, {0x80010B2Cu}, F)) return false;
    {
        const uint32_t rec = g.U32(kFrameRecord);
        if (!Call(c, kDrawOTagFn, {g.U32(rec + 0x108u) + g.U32(gp + kGpFrameOtLen) * 4u - 4u}, F)) return false;
    }
    if (!Call(c, kSkyDrawSyncFn, {0}, F)) return false;
    if (!Call(c, kPrintfFn, {0x80010B6Cu}, F)) return false;
    {
        const uint32_t rec = g.U32(kFrameRecord);
        if (!Call(c, kClearOTagRFn, {g.U32(rec + 0x108u), g.U32(gp + kGpFrameOtLen)}, F)) return false;
    }
    if (!Call(c, kSkyDrawSyncFn, {0}, F)) return false;
    const uint32_t start = g.U32(g.U32(kFrameRecord) + 0xF0u) & 0x00FFFFFFu;
    g.W32(gp + kGpHeapLimit, g.U32(gp + kGpHeapEnd));
    g.W32(kHeapMarks + g.U32(gp + kGpFrameOtBuf) * 4u, start);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800642F8
bool GradQuad(GuestRam& g, const uint32_t a[9], uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - kGradQuadFrame;
    if (!Reserve(g, 36u, F, c)) return false;
    const uint32_t rec = g.U32(kFrameRecord);
    const uint32_t p = g.U32(rec + 0x10Cu);
    g.W32(rec + 0x10Cu, p + 36u);
    g.W8(p + 3u, 8);
    g.W8(p + 7u, 0x38);
    for (uint32_t k = 0; k < 4; ++k) {
        g.W16(p + 8u + 8u * k, g.U16(a[1 + k]));
        g.W16(p + 10u + 8u * k, g.U16(a[1 + k] + 2u));
    }
    uint32_t head = 0;
    for (uint32_t k = 0; k < 4; ++k)
        for (uint32_t b = 0; b < 3; ++b) {
            if (k == 2 && b == 1) head = g.U32(p); // 0x80064464: word 0 read between the third colour's bytes
            g.W8(p + 4u + 8u * k + b, g.U8(a[5 + k] + b));
        }
    const uint32_t ot = a[0];
    g.W32(p, (head & 0xFF000000u) | (g.U32(ot) & 0x00FFFFFFu));
    g.W32(ot, (g.U32(ot) & 0xFF000000u) | (p & 0x00FFFFFFu));
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80063C5C
bool GradDraw(GuestRam& g, uint32_t view, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyWide* wide) {
    const uint32_t F = sp - kGradFrame;
    const int32_t E = wide != nullptr ? wide->edgeAngle : 426;
    const uint32_t vec = F + 40u, sxy = F + 120u, mid = F + 48u, colL = F + 56u, colR = F + 64u;
    // the view's yaw (+0x7C, lh), kept at 0x800CCD68
    const int32_t yaw = g.S16(g.U32(kSkyRenderCams + 4u * view) + 124u);
    const int32_t s0 = yaw - g.S32(kSunYaw);
    g.W32(kGradYawCopy, U(yaw));
    {
        const uint32_t y = g.U32(kGradYawCopy);
        g.W16(vec, static_cast<uint16_t>(Sin(g, y)));
        g.W16(vec + 2u, 0);
        g.W16(vec + 4u, static_cast<uint16_t>(Cos(g, y)));
    }
    Rtp(g, gte, vec, sxy);
    int32_t s1 = s0 - E, s4 = s0 + E;
    if (s1 < 0) s1 = s0 - E + 4096;
    if (s4 < 0) s4 = s0 + E + 4096;
    else if (!(s4 < 4097)) s4 = s0 + E - 4096;
    uint32_t mode = 0, sun = 0;
    uint32_t ang = 0;
    if (!(s1 < 3073) && s4 < 1024) { // the sun inside the field: its own yaw
        mode = 1;
        sun = kGradSunA;
        ang = g.U32(kSunYaw);
    } else if (s1 < 2048 && !(s4 < 2049)) { // the opposite yaw inside it
        mode = 2;
        sun = kGradSunB;
        ang = g.U32(kSunYaw) + 2048u;
    } else {
        ang = g.U32(kGradYawCopy) - U(E);
    }
    g.W16(vec, static_cast<uint16_t>(Sin(g, ang)));
    g.W16(vec + 4u, static_cast<uint16_t>(Cos(g, ang)));
    g.W16(vec + 2u, static_cast<uint16_t>(-static_cast<int32_t>(g.U16(kSinCos + (g.U32(kSunElevation) & 0xFFFu) * 4u))));
    Rtp(g, gte, vec, mid);
    if (!(s1 < 2049)) s1 = 4096 - s1;
    if (!(s4 < 2049)) s4 = 4096 - s4;
    const auto weight = [](int32_t s) {
        int32_t v = S(U(s) << 16);
        if (v < 0) v += 2047;
        return v >> 11;
    };
    const auto blend = [&](int32_t w, uint32_t out) {
        for (uint32_t k = 0; k < 3; ++k)
            g.W8(out + k, static_cast<uint8_t>(FixMul(w, g.U8(kGradSunB + k)) + FixMul(0x10000 - w, g.U8(kGradSunA + k))));
    };
    blend(weight(s1), colL);
    blend(weight(s4), colR);
    // the corners 0x800523D0
    const uint32_t k7 = kGradCorners;
    const int32_t sy = g.S16(sxy + 2u);
    if (!(g.U32(g.U32(kGameState) + 48u) < 2u)) { // two players: the view's rectangle
        const uint32_t r = g.U32(kSplitViewsPtr) + view * 8u;
        g.W16(k7, g.U16(r));
        const uint16_t x0 = g.U16(k7);
        g.W16(k7 + 2u, g.U16(r + 2u));
        const uint16_t xw = static_cast<uint16_t>(g.U16(r) + g.U16(r + 4u));
        g.W16(k7 + 12u, x0);
        const uint16_t y0 = g.U16(k7 + 2u);
        g.W16(k7 + 4u, xw);
        g.W16(k7 + 6u, y0);
        g.W16(k7 + 14u, static_cast<uint16_t>(g.U32(kSplitSkyOn2) != 0 ? sy - g.U16(kSplitSkyLift) : sy));
        g.W16(k7 + 8u, g.U16(k7 + 4u));
        g.W16(k7 + 10u, g.U16(k7 + 14u));
    } else {
        const uint16_t y = static_cast<uint16_t>(g.U32(kSplitSkyOn2) != 0 ? sy - g.U16(kSplitSkyLift) + 10 : sy + 10);
        g.W16(k7 + 10u, y);
        g.W16(k7 + 14u, y);
    }
    // OURS (the wide picture): the corners the packets are built from moved out to the picture's edges, in a copy
    // under the frame; the original's corners in RAM stay as it wrote them
    uint32_t s7 = k7;
    if (wide != nullptr) {
        s7 = F - 256u;
        for (uint32_t k = 0; k < 16u; k += 4) g.W32(s7 + k, g.U32(k7 + k));
        g.W16(s7, static_cast<uint16_t>(g.S16(s7) - wide->left));
        g.W16(s7 + 12u, static_cast<uint16_t>(g.S16(s7 + 12u) - wide->left));
        g.W16(s7 + 4u, static_cast<uint16_t>(g.S16(s7 + 4u) + wide->right));
        g.W16(s7 + 8u, static_cast<uint16_t>(g.S16(s7 + 8u) + wide->right));
    }
    const auto ot = [&]() { return g.U32(kSkyOtPtr) + 12u; };
    const auto pt = [&](uint32_t at, uint16_t x, uint16_t y) {
        g.W16(at, x);
        g.W16(at + 2u, y);
    };
    if (mode != 0) { // four quads around the sun's point
        const uint16_t mx = g.U16(mid), y0 = g.U16(s7 + 2u), x1 = g.U16(s7 + 4u), my = g.U16(mid + 2u),
                       y3 = g.U16(s7 + 14u), x0 = g.U16(s7);
        pt(F + 72u, mx, y0);
        pt(F + 80u, x1, my);
        pt(F + 88u, mx, y3);
        pt(F + 96u, x0, my);
        const uint32_t q1[9] = {ot(), F + 72u, s7, mid, F + 96u, kGradTop, kGradTop, sun, colL};
        if (!GradQuad(g, q1, F, c)) return false;
        const uint32_t q2[9] = {ot(), s7 + 4u, F + 72u, F + 80u, mid, kGradTop, kGradTop, colR, sun};
        if (!GradQuad(g, q2, F, c)) return false;
        const uint32_t q3[9] = {ot(), F + 96u, mid, s7 + 12u, F + 88u, colL, sun, kGradHorizon, kGradHorizon};
        if (!GradQuad(g, q3, F, c)) return false;
        const uint32_t q4[9] = {ot(), F + 80u, mid, s7 + 8u, F + 88u, colR, sun, kGradHorizon, kGradHorizon};
        if (!GradQuad(g, q4, F, c)) return false;
    } else { // two bands: the top colour to the edges' blends, the blends to the horizon colour
        const uint16_t x1 = g.U16(s7 + 4u), my = g.U16(mid + 2u), x0 = g.U16(s7);
        pt(F + 104u, x1, my);
        pt(F + 112u, x0, my);
        const uint32_t qa[9] = {ot(), s7 + 4u, s7, F + 104u, F + 112u, kGradTop, kGradTop, colR, colL};
        if (!GradQuad(g, qa, F, c)) return false;
        const uint32_t qb[9] = {ot(), F + 112u, F + 104u, s7 + 12u, s7 + 8u, colL, colR, kGradHorizon, kGradHorizon};
        if (!GradQuad(g, qb, F, c)) return false;
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80064CC8
bool SplitSkyDraw(GuestRam& g, uint32_t view, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyWide* wide) {
    const uint32_t F = sp - kSplitSkyFrame;
    const uint32_t camAt = kSkyRenderCams + 4u * view;
    {
        const uint32_t yaw = g.U16(g.U32(camAt) + 124u) & 0xFFFu;
        g.W16(F + 16u, static_cast<uint16_t>(Sin(g, yaw)));
        g.W16(F + 18u, 0);
        g.W16(F + 20u, static_cast<uint16_t>(Cos(g, g.U16(g.U32(camAt) + 124u))));
    }
    Rtp(g, gte, F + 16u, F + 24u);
    const int32_t s5 = g.S16(F + 26u) - g.S16(kSplitSkyLift) - 31;
    constexpr uint32_t a3 = kSkyColumn; // 0x800D5EE8
    int32_t s0 = 0, s3 = 0, s2 = 0;
    const auto columns = [&](int32_t centre) { // the view's first column s0, its end s3, the first column's x s2
        const uint32_t yaw = g.U16(g.U32(camAt) + 124u) & 0xFFFu;
        const int32_t n = g.S16(a3 + 4u);
        const int32_t a2 = S(yaw * 27u) >> 6;
        const int32_t col = S(U(a2) << 16) >> 22;
        const int32_t half = S(U(n) << 16) >> 17;
        s0 = col - half;
        if (s0 < 0) s0 += 27;
        const uint32_t colX = g.U16(kSplitSkyColX + U(col) * 2u);
        s3 = s0 + S(g.U16(a3 + 4u)); // lhu: the count unsigned here, signed in `half`
        s2 = S(U(centre) - (U(a2) - colX)) - half * 64;
    };
    if (g.U32(kSplitStream) != 0 && g.U32(g.U32(kGameState) + 48u) == 1u) { // the split stream, one player
        g.W16(a3 + 4u, 7);
        g.W16(a3 + 6u + view * 2u, 192);
        columns(192);
    } else {
        const uint32_t r = g.U32(kSplitViewsPtr) + view * 8u;
        const int32_t w = g.S16(r + 4u);
        int32_t v1 = w + 63;
        if (v1 < 0) v1 = w + 126;
        g.W16(a3 + 4u, static_cast<uint16_t>(((v1 >> 6) + 1) | 1));
        const int32_t centre = S(U(g.U16(r)) + U(S(U(g.U16(r + 4u)) << 16) >> 17));
        g.W16(a3 + 6u + view * 2u, static_cast<uint16_t>(centre));
        columns(centre);
    }
    int32_t count = g.S16(a3 + 4u);
    if (wide != nullptr) { // OURS (the wide picture): the level's columns on past the view's edges, at most all 27
        int32_t l = (wide->left + 63) / 64, r = (wide->right + 63) / 64;
        if (count + l + r > 27) l = r = (27 - count) / 2;
        s0 -= l;
        if (s0 < 0) s0 += 27;
        s2 -= 64 * l;
        count += l + r;
        s3 = s0 + count;
    }
    if (!Reserve(g, U(count * 48), F, c)) return false;
    int32_t t1 = s0;
    uint32_t a2 = g.U32(g.U32(kFrameRecord) + 0x10Cu);
    g.W32(g.U32(kFrameRecord) + 0x10Cu, a2 + U(count * 48));
    if (!(S(U(t1) << 16) < S(U(s3) << 16))) return !g.Faulted();
    int32_t t6 = t1, t3 = s2;
    do {
        if (static_cast<int16_t>(t1) == 27) t1 = 0;
        int32_t y = s5;
        const uint32_t t5 = U(S(U(t1) << 16) >> 14);
        for (int32_t t0 = 0; t0 < 2; ++t0, y += 31) {
            const int32_t idx = g.S16(kSplitSkyIndex + U(t0) * 2u + t5);
            if (idx == -1) continue;
            g.W32(a2, 0x05000000u);
            const uint32_t rec = kSplitSkyRecords + U(idx * 44);
            g.W32(a2 + 4u, g.U32(rec + 4u));
            const uint32_t cmd = g.U32(kSplitSkyCmd);
            g.W16(a2 + 12u, static_cast<uint16_t>(t3));
            g.W16(a2 + 14u, static_cast<uint16_t>(y));
            g.W32(a2 + 8u, cmd);
            const uint32_t uv = g.U32(rec);
            g.W32(a2 + 20u, 0x001F0040u);
            g.W32(a2 + 16u, uv);
            const uint32_t ot = g.U32(kSkyOtPtr);
            g.W32(a2, (g.U32(a2) & 0xFF000000u) | (g.U32(ot) & 0x00FFFFFFu));
            g.W32(ot, (g.U32(ot) & 0xFF000000u) | (a2 & 0x00FFFFFFu));
            a2 += 24u;
            if (g.Faulted()) return false;
        }
        ++t6;
        ++t1;
        t3 += 64;
    } while (S(U(t6) << 16) < S(U(s3) << 16));
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x800627A8
void SplitSkyLoad(GuestRam& g, uint32_t section, uint32_t payload) {
    if (U(g.U16(section + 2u)) - 4u == kSplitSkyBytes) {
        for (uint32_t k = 0; k < kSplitSkyBytes; k += 4) g.W32(kSplitSkyBase + k, g.U32(payload + k)); // 0x8001E0B4
        g.W32(kSplitSkyOn2, 1);
    } else {
        g.W32(kSplitSkyOn2, 0);
    }
}

// ============================================================================ RASHCDI 0x80062384
void GradLoad(GuestRam& g, uint32_t, uint32_t payload) {
    constexpr uint32_t b = kGradCorners;
    static constexpr uint32_t kTo[12] = {40, 41, 42, 44, 45, 46, 32, 33, 34, 36, 37, 38};
    static constexpr uint32_t kFrom[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
    for (int k = 0; k < 12; ++k) g.W8(b + kTo[k], g.U8(payload + kFrom[k]));
    GradInit(g);
}

// ============================================================================ RASHCDI 0x800611C4
void GradInit(GuestRam& g) {
    constexpr uint32_t b = kGradCorners;
    g.W32(kGradOn, 1);
    g.W16(b, 0);
    const uint8_t t0 = g.U8(b + 40u), a2 = g.U8(b + 44u);
    g.W16(b + 14u, 240);
    g.W16(b + 10u, 240);
    g.W16(b + 4u, 384);
    g.W16(b + 8u, 384);
    g.W16(b + 2u, 0);
    g.W16(b + 6u, 0);
    g.W16(b + 12u, 0);
    g.W8(b + 16u, t0);
    const uint8_t a1 = g.U8(b + 41u), v1 = g.U8(b + 42u), a3 = g.U8(b + 45u), a0 = g.U8(b + 46u);
    constexpr uint32_t e = b + 16u;
    g.W8(e + 4u, t0);
    g.W8(e + 12u, a2);
    g.W8(e + 8u, a2);
    g.W8(e + 1u, a1);
    g.W8(e + 2u, v1);
    g.W8(e + 5u, a1);
    g.W8(e + 6u, v1);
    g.W8(e + 13u, a3);
    g.W8(e + 14u, a0);
    g.W8(e + 9u, a3);
    g.W8(e + 10u, a0);
}

} // namespace rr::sim
