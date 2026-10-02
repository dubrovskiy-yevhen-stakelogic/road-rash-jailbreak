// The stream's files and the set-up's CD waits (stream_files.h). Every function transcribed from our own disassembly of
// SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1), the Ghidra pseudo-C of work\ghidra as the
// reading aid; each accepted by its rrverify row (rows_stream3.inc).
#include "game/sim/stream_files.h"

#include "game/sim/cell_draw.h" // RlsRelocate (0x8002428C's fix-up)
#include "game/sim/stream.h"
#include "game/sim/stream_cd.h"

namespace rr::sim {

namespace {

using rc::Call;
using rc::S;
using rc::U;

uint32_t Gs(GuestRam& g) { return g.U32(kStGameState); }
uint32_t Busy(GuestRam& g) { return g.U32(kCdBusy); }

} // namespace

// ============================================================================ SLUS 0x80023498
bool StreamFiles(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - kSfFilesFrame, name = F + 24u, gp = g.gp();
    uint32_t v = 0;
    if (!Call(c, kSfSprintf, {name, kSfFmt, kSfStream, g.U32(Gs(g) + 48u), kSfToc}, F)) return false;
    if (!TocLoad(g, gp + kGpStToc, name, F, c, v)) return false;          // 0x800234E8 (its value not read)
    if (!Call(c, kSfSprintf, {name, kSfFmt, kSfStream, g.U32(Gs(g) + 48u), kSfRls}, F)) return false;
    if (!RlsLoad(g, name, F, c, v)) return false;
    if (v == 0) g.W32(gp + kGpRlsTable, 0);                               // 0x80023524
    g.W32(gp + kGpRlsCursor + 4u, 0);                                     // gp+0x880, then gp+0x87C
    g.W32(gp + kGpRlsCursor, 0);
    if (!Call(c, kSfSprintf, {name, kSfFmt, kSfStream, g.U32(Gs(g) + 48u), kSfStr}, F)) return false;
    uint32_t h = 0;
    if (!Call(c, kSfOpen, {name}, F, &h)) return false;
    g.W32(gp + kGpStStr, h);
    g.W32(gp + kGpStLastPos, 0);
    if (!Call(c, kSfSize, {h}, F, &v)) return false;
    g.W32(gp + kGpStStrSize, v);
    for (uint32_t p = 0; p < 2u; ++p) {
        StreamSelect(g, p);                                               // 0x8002379C
        StreamReset(g);                                                   // 0x80024354
    }
    g.W32(gp + kGpStTurn, 0);
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80023F08
bool TocLoad(GuestRam& g, uint32_t out, uint32_t name, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kSfTocLoadFrame;
    uint32_t h = 0, n = 0, block = 0, got = 0;
    v0 = 0;
    if (!Call(c, kSfOpen, {name}, F, &h)) return false;                   // (no test of the handle)
    if (!Call(c, kSfSize, {h}, F, &n)) return false;
    if (!Call(c, kSfMalloc, {n, 0}, F, &block)) return false;
    if (!Call(c, kSfRead, {h, block, n, 0}, F, &got)) return false;
    if (!Call(c, kSfClose, {h}, F)) return false;
    if (S(got) < 0) return !g.Faulted();
    g.W32(out, block);
    g.W32(block + 24u, block + g.U32(block + 24u));
    const uint32_t t = g.U32(out);
    g.W32(t + 20u, block + g.U32(t + 20u));
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002428C
bool RlsLoad(GuestRam& g, uint32_t name, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kSfRlsLoadFrame;
    uint32_t h = 0, n = 0, block = 0, got = 0;
    v0 = 0;
    if (!Call(c, kSfOpen, {name}, F, &h)) return false;
    if (S(h) < 0) return true;
    if (!Call(c, kSfSize, {h}, F, &n)) return false;
    if (!Call(c, kSfMalloc, {n, 0}, F, &block)) return false;
    if (!Call(c, kSfRead, {h, block, n, 0}, F, &got)) return false;
    if (!Call(c, kSfClose, {h}, F)) return false;
    if (S(got) < 0) return !g.Faulted();
    RlsRelocate(g, block);                                                // 0x800242FC..0x80024334 (PORTED, cell_draw.h)
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80023714
bool StpPlayers(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - kSfStpFrame, name = F + 16u;
    if (!Call(c, kSfSprintf, {name, kSfStpFmt, g.U32(Gs(g) + 48u), g.U32(Gs(g) + 64u)}, F)) return false;
    for (uint32_t p = 0; p < g.U32(Gs(g) + 48u); ++p) {
        StreamSelect(g, p);                                               // 0x8002379C
        uint32_t v = 0;
        if (!StpStart(g, name, F, c, v)) return false;
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80024168
bool StpStart(GuestRam& g, uint32_t name, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kSfStpStartFrame, hdr = F + 16u, gp = g.gp();
    uint32_t h = 0, got = 0;
    v0 = 0;
    if (!Call(c, kSfOpen, {name}, F, &h)) return false;
    if (S(h) < 0) return true;
    if (!Call(c, kSfRead, {h, hdr, 40u, 0}, F, &got)) return false;
    if (S(got) < 0) return true;                                          // (the file stays open, as on the console)
    StreamCursor(g, g.U32(gp + kGpStCur) + 8u, h, g.U32(hdr + 20u), g.U32(hdr + 24u)); // 0x80023C24
    const uint32_t cur = g.U32(gp + kGpStCur);
    const uint32_t arg = g.U32(cur) << 16;
    g.W32(cur + 56u, 0);
    g.W32(cur + 32u, 1);
    for (int k = 0; k < 3; ++k) {
        if (StreamAtEnd(g, g.U32(gp + kGpStCur) + 8u) != 0) break;       // 0x80023300
        StreamRequest(g, g.U32(gp + kGpStCur) + 8u, 64u, 0, arg, F, c);  // 0x80023148
        if (g.Faulted()) return false;
        uint32_t w = 0;
        if (!CdWait(g, 2u, F, c, w)) return false;                        // 0x80022A78(2), twice
        if (!CdWait(g, 2u, F, c, w)) return false;
        if (w != 0) {
            if (!CdAbort(g, 2u, F, c, w)) return false;                   // 0x800229F4(2)
            CdInit(g, F, c);                                              // 0x800229B0
        }
        ResPump(g, F, c);                                                 // 0x80030608
        if (g.Faulted()) return false;
    }
    const uint32_t st = g.U32(gp + kGpStCur);
    g.W32(st + 32u, 0);
    StreamAtEnd(g, st + 8u);
    if (!Call(c, kSfClose, {h}, F)) return false;
    StreamRange(g);                                                       // 0x80023DE4
    StreamLimit(g, g.U32(F + 52u));                                       // 0x800243BC(header +0x24)
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80022A1C / 0x80022A78 / 0x800229F4
bool CdFlush(GuestRam& g, uint32_t mode, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kSfCdFlushFrame;
    if (!Call(c, kStCritEnterFn, {}, F)) return false;
    if (Busy(g) != 0) g.W32(g.U32(g.gp() + kGpCdCur) + 24u, 0);
    CdReset(g);                                                           // 0x80022CC4
    if (!Call(c, kStCritLeaveFn, {}, F)) return false;
    return CdWait(g, mode, F, c, v0);
}

bool CdWait(GuestRam& g, uint32_t mode, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kSfCdWaitFrame;
    if (mode == 1u) {
        if (Busy(g) != 0) return false; // the console spins until the drive's interrupt clears the word
    } else if (mode == 2u) {
        for (int k = 0; k < 200 && Busy(g) != 0; ++k)
            if (!Call(c, kSfVSync, {0}, F)) return false;
    }
    v0 = Busy(g);
    return !g.Faulted();
}

bool CdAbort(GuestRam& g, uint32_t mode, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kSfCdAbortFrame;
    uint32_t w = 0;
    if (!CdFlush(g, mode, F, c, w)) return false;
    v0 = Busy(g);
    g.W32(g.gp() + kGpCdInit, 0);
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80024630 / 0x80024FF8
void AlbumClear(GuestRam& g) {
    g.W32(kSfMusicReqs + 248u, 0);
    g.W32(kSfMusicReqs + 244u, 0);
    g.W32(kSfMusicReqs + 240u, 0);
    for (uint32_t k = 0; k < 30u; ++k) {
        g.W32(kSfMusicReqs + 8u * k, 0xFFFFFFFFu);
        g.W32(kSfMusicReqs + 8u * k + 4u, 0);
    }
}

bool AlbumOpen(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - kSfAlbumFrame, gp = g.gp(), a = kSfAlbum;
    AlbumClear(g);
    g.W32(0x8005B4E8u, 0);
    g.W32(0x80053658u, 0);
    uint32_t h1 = 0, h2 = 0, n = 0;
    if (!Call(c, kSfOpen, {g.U32(gp + 476u)}, F, &h1)) return false;      // "ALBUM.ALB"
    if (S(h1) < 0) return !g.Faulted();
    if (!Call(c, kSfOpen, {g.U32(gp + 480u)}, F, &h2)) return false;      // "ALBUM2.ALB"
    if (S(h2) < 0) return !g.Faulted();
    g.W32(a + 48u, 0xFFFFFFFFu);
    g.W32(a + 52u, 1);
    g.W32(a, h1);
    g.W32(a + 4u, 0);
    g.W32(a + 8u, 8);
    g.W32(a + 12u, 0);
    if (!Call(c, kSfSize, {h1}, F, &n)) return false;
    g.W32(a + 16u, n);
    g.W32(a + 24u, h1);
    g.W32(a + 28u, h2);
    g.W32(a + 44u, 0);
    g.W32(gp + 1380u, 0);
    g.W32(a + 20u, g.U32(a + 20u) | 1u);
    const uint32_t args[15] = {1u, 16000u, 2u, 8192u, 3u, 8u, 4u, 0u, 7u, kSfMusicCb, 5u, 0u, 6u, 0u, 0u};
    uint32_t v = 0;
    if (!c.Call(kSfMusicSetUp, args, 15, F, v)) return false;
    if (S(v) >= 0) g.W32(a + 36u, 0);
    return !g.Faulted();
}

} // namespace rr::sim
