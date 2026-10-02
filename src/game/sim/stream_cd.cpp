// The rest of the original's streamer (stream_cd.h). Transcribed from our own disassembly of SLUS_010.53
// (SHA-1 67ed165a...) and RASHCDG.BIN (cfe43a77..., at 0x8005B5E8); the Ghidra pseudo-C of work\ghidra
// as the reading aid. Each accepted by its rrverify row (rows_stream2.inc).
#include "game/sim/stream_cd.h"

namespace rr::sim {

namespace {

using rc::S;
using rc::U;

uint32_t Gs(GuestRam& g) { return g.U32(kStGameState); }
uint32_t Sky(GuestRam& g) { return g.U32(kStSkyPtr); }
bool Call(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> a, uint32_t sp, uint32_t* v0 = nullptr) {
    return rc::Call(c, fn, a, sp, v0);
}
int32_t S16(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

} // namespace

// ============================================================================ the CD access layer
int32_t CdQueue(GuestRam& g, uint32_t req, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 32u;
    if (g.U32(kCdCount) == 49u) return -100;
    GuestCopyWords(g, kCdRing + (g.U32(kCdTail) << 5), req, 32u); // 0x8001E0B4
    const uint32_t nx = g.U32(kCdTail) + 1u;
    g.W32(kCdTail, nx);
    g.W32(kCdTail, nx < kCdRingSize ? nx : 0u);
    Call(c, kStCritEnterFn, {}, csp);
    g.W32(kCdCount, g.U32(kCdCount) + 1u);
    if (g.U32(req) == 8u) g.W32(kCdMusicCount, g.U32(kCdMusicCount) + 1u);
    Call(c, kStCritLeaveFn, {}, csp);
    if (g.U32(kCdCount) == 1u && g.U32(kCdBusy) == 0) CdNext(g, 0, csp, c);
    return 0;
}

int32_t CdTake(GuestRam& g, uint32_t out) {
    if (g.U32(kCdCount) == 0) return -99;
    g.W32(out, kCdRing + (g.U32(kCdHead) << 5));
    const uint32_t nx = g.U32(kCdHead) + 1u;
    g.W32(kCdHead, nx);
    g.W32(kCdHead, nx < kCdRingSize ? nx : 0u);
    g.W32(kCdCount, g.U32(kCdCount) - 1u);
    if (g.U32(g.U32(out)) == 8u) g.W32(kCdMusicCount, g.U32(kCdMusicCount) - 1u);
    return 0;
}

void CdNext(GuestRam& g, uint32_t a0, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 40u;
    if (a0 != 0) return;
    const uint32_t cur = g.gp() + kGpCdCur;
    uint32_t ok = 1;
    for (;;) {
        const int32_t r = CdTake(g, cur);
        if (r == 0) {
            const uint32_t q = g.U32(cur);
            if (g.U32(q) != 0x200u) {
                // 0x80030868(index, arg): the record not read yet
                ok = (g.U32(g.U32(kStResList) + 36u * g.U32(q + 12u) + 0x2Cu) ^ 1u) & 1u;
                if (ok == 0) Call(c, kStPrintFn, {0x80010B94u}, csp); // "CdAccess: Dbl Buf Overflow"
            }
        }
        if (r == -99) {
            g.W32(kCdBusy, 0);
            return;
        }
        if (r == 0 && ok != 0) break;
        if (g.Faulted()) return;
    }
    g.W32(kCdBusy, 1);
    CdStart(g, g.U32(cur), csp, c);
}

void CdStart(GuestRam& g, uint32_t req, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 24u;
    Call(c, kCdSeekFn, {g.U32(req + 4u), g.U32(req + 8u), 0}, csp);
    const uint32_t file = g.U32(req + 4u), buf = g.U32(req + 16u), bytes = g.U32(req + 28u);
    const uint32_t gp = g.gp();
    const uint32_t clock = g.U32(Gs(g) + 12u);
    g.W32(gp + kGpCdStarts, g.U32(gp + kGpCdStarts) + 1u);
    g.W32(gp + kGpStCdStamp, clock);
    g.W32(gp + kGpStCdBusy, 1);
    uint32_t v = 0;
    Call(c, kCdReadFn, {file, buf, bytes, kCdDoneLabel}, csp, &v);
    if (S(v) < 0) g.W32(gp + kGpStCdBusy, 0);
}

void CdDone(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 24u;
    const uint32_t gp = g.gp();
    const uint32_t q = g.U32(gp + kGpCdCur);
    g.W32(gp + kGpStCdBusy, 0);
    const uint32_t cb = g.U32(q + 24u);
    g.W32(gp + kGpCdDone, g.U32(gp + kGpCdDone) + 1u);
    if (cb != 0) Call(c, cb, {g.U32(q + 12u), g.U32(q), g.U32(q + 20u)}, csp);
    CdNext(g, 0, csp, c);
}

void CdReset(GuestRam& g) {
    g.W32(kCdMusicCount, 0);
    g.W32(kCdCount, 0);
    g.W32(kCdHead, 0);
    g.W32(kCdTail, 0);
}

void CdInit(GuestRam& g, uint32_t, RecoverCallees&) {
    const uint32_t gp = g.gp();
    if (g.U32(gp + kGpCdInit) == 0) {
        g.W32(kCdBusy, 0);
        CdReset(g);
        g.W32(gp + kGpCdInit, 1);
    }
    g.W32(gp + kGpStCdStamp, 0);
    g.W32(gp + kGpStCdBusy, 0);
    g.W32(gp + kGpCdDone, 0);
    g.W32(gp + kGpCdStarts, 0);
}

void ResFreeIrq(GuestRam& g, uint32_t index, uint32_t sp, RecoverCallees& c) {
    g.W32(kStMusicBusy, 1);
    ResFree(g, g.U32(kStResList) + 36u * index + 44u, sp - 48u, c); // 0x80030FD0 (frame 24) from 0x80030FA0 (24)
    g.W32(kStMusicBusy, 0);
}

// ============================================================================ the sky
void SkyView(GuestRam& g, uint32_t view, uint32_t sp, RecoverCallees& c) {
    const uint32_t cam = g.U32(kStRenderCams + 4u * view);
    const uint32_t col = (((U(S16(g.U16(cam + 124u))) - 443u) & 0xFFFu) * 110u) >> 12;
    g.W16(kSkyStartCol, static_cast<uint16_t>(col));
    g.W16(kSkyStartCol + 2u, static_cast<uint16_t>(col + 24u));
    if (g.U32(Gs(g) + 48u) == 1u && g.U32(kSkySplit) == 0) SkyFrame(g, sp - 24u, c);
}

namespace {
int32_t g_wideLeft = 0, g_wideRight = 0; // SetSkyFrameWide (OURS)
} // namespace

void SetSkyFrameWide(int32_t left, int32_t right) {
    g_wideLeft = left;
    g_wideRight = right;
}

void SkyFrame(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 32u;
    uint32_t sky = Sky(g);
    if (g.U32(sky + 24u) != 0) return;
    const uint32_t col = (((U(S16(g.U16(g.U32(kStRenderCams) + 124u))) - 443u) & 0xFFFu) * 110u) >> 12;
    const uint32_t last = col + 24u + U(g_wideRight), from = S(col) - g_wideLeft < 0 ? col + 110u - U(g_wideLeft) : col - U(g_wideLeft);
    g.W16(sky + 50u, static_cast<uint16_t>(last));
    g.W16(sky + 48u, static_cast<uint16_t>(from));
    if (!(S(last) < 110)) g.W16(sky + 50u, static_cast<uint16_t>(last - 110u));
    const uint32_t st = g.U32(kStreamStateSel);
    const uint32_t road = g.U32(st + 72u);
    const uint32_t along = g.U32(st + 68u) << 6;
    const int32_t pick = SkyPick(g, road, along);
    uint32_t a3 = Sky(g);
    g.W16(a3 + 16u, static_cast<uint16_t>(pick));
    if (S16(U(pick)) == -1) {
        g.W16(a3 + 18u, 0xFFFFu);
        return;
    }
    g.W32(a3 + 8u, road);
    g.W32(a3 + 12u, along);
    if (S16(g.U16(a3 + 16u)) == S16(g.U16(a3 + 18u))) {
        // 0x80065420: the same panorama - the view's turn
        if (S16(g.U16(a3 + 48u)) == S16(g.U16(a3 + 42u))) return;
        const int32_t t = SkyTurn(U(S16(g.U16(a3 + 52u))), U(S16(g.U16(a3 + 54u))), U(S16(g.U16(a3 + 48u))),
                                  U(S16(g.U16(a3 + 50u))));
        sky = Sky(g);
        g.W16(sky + 30u, static_cast<uint16_t>(t));
        if (S16(U(t)) != 0) {
            if (S16(U(t)) < 0) {
                g.W16(sky + 30u, static_cast<uint16_t>(-t));
                g.W16(sky + 28u, 0);
            } else {
                g.W16(sky + 28u, 1);
            }
            Call(c, kSkyUploadFn, {}, csp);
        }
        sky = Sky(g);
        g.W16(sky + 42u, g.U16(sky + 48u));
        return;
    }
    Call(c, kSkyNopFn, {0xFF00u}, csp);
    // the new panorama's column tables: per column its first tile row (+0x124), its count of code-2 tiles so far
    // (+0x200), its first non-empty band (+0x48) and its tile count (+0xB6)
    uint32_t twos = 0, offset = 0, colIdx = 0;
    sky = Sky(g);
    uint32_t codes = g.U32(sky + 988u) + 8u;
    g.W16(sky + 18u, g.U16(sky + 16u));
    for (; S(colIdx) < 110; ++colIdx) {
        if ((colIdx & 1u) == 0) offset = 0;
        const uint32_t s = Sky(g);
        g.W16(s + 2u * colIdx + 292u, static_cast<uint16_t>(offset));
        g.W16(s + 2u * colIdx + 512u, static_cast<uint16_t>(twos));
        const uint32_t e = g.U32(s + 4u * U(S(colIdx) / 2) + 1000u);
        if (e == 0) continue;
        uint32_t n = 0;
        int32_t shift = 6;
        bool first = true;
        g.W8(s + colIdx + 72u, 0);
        for (uint32_t k = 0; S(k) < 8; ++k) {
            const uint32_t code = (U(g.U8(codes)) >> shift) & 3u;
            if (code != 0) {
                ++n;
                if (code == 2u) ++twos;
                if (first) {
                    first = false;
                    g.W8(Sky(g) + colIdx + 72u, static_cast<uint8_t>(k));
                }
            }
            shift -= 2;
            if (shift < 0) {
                shift = 6;
                ++codes;
            }
        }
        if (g.U32(e + 12u) != 0 || n == 0) {
            offset += n << 9;
            g.W8(Sky(g) + colIdx + 182u, static_cast<uint8_t>(n));
        }
        if (g.Faulted()) return;
    }
    sky = Sky(g);
    g.W16(sky + 30u, static_cast<uint16_t>(26 + g_wideLeft + g_wideRight)); // 26 (OURS: + the wide margins)
    const uint16_t b390 = g.U16(0x8005B390u), b394 = g.U16(0x8005B394u);
    const uint16_t c30 = g.U16(sky + 48u);
    g.W16(sky + 36u, 1);
    g.W16(sky + 38u, 0);
    g.W16(sky + 32u, 1);
    g.W16(sky + 28u, 1);
    g.W16(sky + 58u, b394);
    g.W16(sky + 62u, static_cast<uint16_t>(b394 + 16u));
    g.W16(sky + 20u, 0xFFFFu);
    const int32_t even = (S16(c30) / 2) * 2;
    g.W16(sky + 52u, static_cast<uint16_t>(even));
    g.W16(sky + 54u, static_cast<uint16_t>(even - 1));
    g.W16(sky + 44u, 1);
    g.W16(sky + 56u, b390);
    g.W16(sky + 60u, b390);
    g.W16(sky + 46u, 0);
    g.W16(sky + 42u, g.U16(sky + 48u));
    Call(c, kSkyUploadFn, {}, csp);
}

int32_t SkyPick(GuestRam& g, uint32_t road, uint32_t along) {
    const uint32_t sky = Sky(g);
    if (S16(g.U16(sky + 34u)) == 0) return -1;
    const int32_t shown = S16(g.U16(sky + 18u));
    const uint32_t want = g.U32(g.U32(kRlsCursorWord));
    if (shown != -1 && g.U32(sky + U(shown) * 12u + 748u) == want) return shown;
    int32_t idx = 0;
    uint32_t e = sky + 740u;
    bool found = false;
    for (; idx < 20; ++idx, e += 12u) {
        if (g.U32(e + 8u) == want) {
            found = true;
            break;
        }
    }
    if (!found) {
        int32_t best = 0xFFFF, bestIdx = -1;
        e = sky + 740u;
        for (int32_t i = 0; i < 20; ++i, e += 12u) {
            if (g.U32(e) == 0) continue;
            uint32_t w = g.U32(e + 4u);
            int32_t k = 0;
            for (; k < 4; ++k, w += 6u)
                if (S16(g.U16(w)) == S(road)) break;
            if (k >= 4) continue;
            const int32_t sum = S16(g.U16(w + 4u)) + S16(g.U16(w + 2u));
            const int32_t d = (sum + S(U(sum) >> 31)) / 2 - S(along);
            const int32_t sg = d >> 31;
            const int32_t ad = (sg + d) ^ sg;
            if (ad < best) {
                best = ad;
                bestIdx = i;
                found = true;
            }
        }
        idx = bestIdx;
        e = Sky(g) + U(bestIdx) * 12u + 740u;
        if (!found) return S16(g.U16(Sky(g) + 18u));
    }
    // 0x80065640: the panorama's 59 column records
    uint32_t v = g.U32(e) + 12u;
    uint32_t out = Sky(g) + 984u;
    for (int32_t k = 1; k < 60; ++k, out += 4u) {
        g.W32(out, v);
        v += g.U32(v + 4u);
        if (g.Faulted()) return idx;
    }
    const uint32_t s = Sky(g);
    g.W32(s + 4u, g.U32(g.U32(s + 984u) + 20u));
    return idx;
}

int32_t SkyTurn(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t t2 = a1, v1 = a2, t0 = a3;
    const int32_t t1 = S16(a1);
    if (S16(a1) < S16(a0)) {
        t2 = a1 + 110u; // the delay slot runs either way
        if (S16(a2) < t1) {
            v1 = a2 + 110u;
            t0 = a3 + 110u;
        }
    }
    const int32_t p3 = S16(v1);
    if (S16(t0) < p3) t0 += 110u;
    const int32_t a = S16(a0);
    int32_t lo = 0, hi = 0; // v1, a2 at 0x8006578C
    if (p3 < a) {
        lo = a - p3;
        const int32_t e = S16(t0);
        hi = (e < a ? e - S16(t2) : a - S16(t2)) + 110;
    } else {
        const int32_t e = S16(t0), b = S16(t2);
        if (!(b < e)) return 0;
        hi = e - b;
        lo = (b < p3 ? a - p3 : a - b) + 110;
    }
    return lo < hi ? -lo : hi;
}

// ============================================================================ the cells' textures
uint32_t CellTexBind(GuestRam& g, uint32_t slot, uint32_t p) {
    for (uint32_t k = 0; k < 2u; ++k) {
        const uint32_t key = g.U32(slot + 80u + 4u * k);
        if (key == 0xFFFFFFFFu) break;
        const int32_t i = TexFind(g, key, p);
        if (i != -1) g.W32(slot + 104u + 4u * k, kStTexSlots + 48u * U(i));
    }
    for (uint32_t k = 0; k < 2u; ++k) {
        const uint32_t key = g.U32(slot + 88u + 4u * k);
        if (key == 0xFFFFFFFFu) break;
        const int32_t i = TexFind(g, key, p);
        if (i != -1) g.W32(slot + 96u + 4u * k, kStTexSlots + 48u * U(i));
    }
    return 1;
}

} // namespace rr::sim
