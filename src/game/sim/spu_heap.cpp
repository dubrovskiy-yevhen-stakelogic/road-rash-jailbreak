// libspu's SPU-RAM allocator and the music stream player's ring buffers (spu_heap.h), SLUS_010.53 SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1. Addresses in the comments are the original's instructions.
#include "game/sim/spu_heap.h"

namespace rr::sim {

namespace {
constexpr uint32_t kFree = 0x80000000u, kTerminal = 0x40000000u, kDropped = 0x2FFFFFFFu, kAddr = 0x0FFFFFFFu;
}

// ---------------------------------------------------------------------------- 0x8004F368
int32_t SpuInitMalloc(GuestRam& g, int32_t n, uint32_t table) {
    if (n <= 0) return 0;                                                   // bgtz v0 / move v0,zero
    const uint32_t shift = g.U32(kSpuAddrShift);
    g.W32(table, 0x40001010u);
    g.W32(kSpuMallocTab, table);
    g.W32(kSpuMallocLast, 0);
    g.W32(kSpuMallocMax, static_cast<uint32_t>(n));
    g.W32(table + 4u, (0x10000u << (shift & 31u)) - 0x1010u);            // sllv / addiu -4112
    return n;
}

// ---------------------------------------------------------------------------- 0x8004F698
void SpuMallocGc(GuestRam& g) {
    // 0x8004F698..0x8004F764: merge each free block with the next live free block that starts where it ends
    {
        const int32_t last = g.S32(kSpuMallocLast);
        if (last >= 0) {
            const uint32_t tab = g.U32(kSpuMallocTab);
            int32_t i = 0;
            uint32_t e = tab;
            do {
                if (g.U32(e) & kFree) {
                    int32_t j = i + 1;
                    while (g.U32(tab + 8u * static_cast<uint32_t>(j)) == kDropped) ++j;
                    const uint32_t f = tab + 8u * static_cast<uint32_t>(j);
                    const uint32_t fw = g.U32(f);
                    if ((fw & kFree) && (fw & kAddr) == (g.U32(e) & kAddr) + g.U32(e + 4u)) {
                        g.W32(f, kDropped);
                        g.W32(e + 4u, g.U32(e + 4u) + g.U32(f + 4u));
                        if (g.Faulted()) return;
                        continue;                                           // 0x8004F74C: the same entry again
                    }
                }
                e += 8u;
                ++i;
                if (g.Faulted()) return;
            } while (!(last < i));
        }
    }
    // 0x8004F764..0x8004F7B4: an empty block is dropped
    {
        const int32_t last = g.S32(kSpuMallocLast);
        if (last >= 0) {
            uint32_t e = g.U32(kSpuMallocTab);
            int32_t i = 0;
            do {
                if (g.U32(e + 4u) == 0) g.W32(e, kDropped);
                ++i;
                e += 8u;
                if (g.Faulted()) return;
            } while (!(last < i));
        }
    }
    // 0x8004F7B8..0x8004F880: sort by address up to the terminal entry
    {
        int32_t last = g.S32(kSpuMallocLast);
        if (last >= 0) {
            const uint32_t tab = g.U32(kSpuMallocTab);
            uint32_t e = tab;
            int32_t i = 0;
            do {
                if (g.U32(e) & kTerminal) break;
                int32_t j = i + 1;
                if (!(last < j)) {
                    const int32_t lastNow = g.S32(kSpuMallocLast);
                    uint32_t f = tab + 8u * static_cast<uint32_t>(j);
                    do {
                        const uint32_t fw = g.U32(f);
                        if (fw & kTerminal) break;
                        const uint32_t ew = g.U32(e);
                        if ((fw & kAddr) < (ew & kAddr)) {
                            g.W32(e, fw);
                            const uint32_t fs = g.U32(f + 4u);
                            const uint32_t es = g.U32(e + 4u);
                            g.W32(e + 4u, fs);
                            g.W32(f, ew);
                            g.W32(f + 4u, es);
                        }
                        ++j;
                        f += 8u;
                        if (g.Faulted()) return;
                    } while (!(lastNow < j));
                }
                last = g.S32(kSpuMallocLast);
                ++i;
                e += 8u;
                if (g.Faulted()) return;
            } while (!(last < i));
        }
    }
    // 0x8004F884..0x8004F904: the first dropped entry takes the terminal one, which moves down to it
    {
        int32_t last = g.S32(kSpuMallocLast);
        if (last >= 0) {
            const uint32_t tab = g.U32(kSpuMallocTab);
            uint32_t e = tab;
            int32_t i = 0;
            do {
                const uint32_t w = g.U32(e);
                if (w & kTerminal) break;
                if (w == kDropped) {
                    const uint32_t t = tab + 8u * static_cast<uint32_t>(last);
                    g.W32(e, g.U32(t));
                    const uint32_t ts = g.U32(t + 4u);
                    g.W32(kSpuMallocLast, static_cast<uint32_t>(i));
                    g.W32(e + 4u, ts);
                    break;
                }
                last = g.S32(kSpuMallocLast);
                ++i;
                e += 8u;
                if (g.Faulted()) return;
            } while (!(last < i));
        }
    }
    // 0x8004F908..0x8004F990: free blocks right below the terminal entry fold into it
    {
        int32_t i = g.S32(kSpuMallocLast) - 1;
        if (i < 0) return;
        const uint32_t tab = g.U32(kSpuMallocTab);
        uint32_t e = tab + 8u * static_cast<uint32_t>(i);
        do {
            const uint32_t w = g.U32(e);
            if (!(w & kFree)) return;
            const int32_t old = g.S32(kSpuMallocLast);
            g.W32(e, (w & kAddr) | kTerminal);
            const uint32_t size = g.U32(e + 4u);
            g.W32(kSpuMallocLast, static_cast<uint32_t>(i));
            const uint32_t more = g.U32(tab + 8u * static_cast<uint32_t>(old) + 4u);
            --i;
            g.W32(e + 4u, size + more);
            e -= 8u;
            if (g.Faulted()) return;
        } while (i >= 0);
    }
}

// ---------------------------------------------------------------------------- 0x8004F3C8
uint32_t SpuMalloc(GuestRam& g, uint32_t size) {
    uint32_t reserve = 0;
    if (g.U32(kSpuReverbOn) != 0)                                           // 0x8004F3F4
        reserve = (0x10000u - g.U32(kSpuReverbStart)) << (g.U32(kSpuAddrShift) & 31u);
    const uint32_t mask = g.U32(kSpuAddrMask);
    if (size & ~mask) size += mask;                                         // 0x8004F42C
    const uint32_t shift = g.U32(kSpuAddrShift) & 31u;
    size = static_cast<uint32_t>(static_cast<int32_t>(size) >> shift) << shift; // srav / sllv
    int32_t found = -1;
    if (g.U32(g.U32(kSpuMallocTab)) & kTerminal) {
        found = 0;
    } else {
        SpuMallocGc(g);                                                     // 0x8004F470
        const int32_t n = g.S32(kSpuMallocMax);
        if (0 < n) {
            uint32_t e = g.U32(kSpuMallocTab);
            for (int32_t k = 0; k < n; ++k, e += 8u) {
                const uint32_t w = g.U32(e);
                if (w & kTerminal) { found = k; break; }
                if ((w & kFree) && !(g.U32(e + 4u) < size)) { found = k; break; }
            }
        }
    }
    if (g.Faulted() || found == -1) return 0xFFFFFFFFu;
    const uint32_t off = 8u * static_cast<uint32_t>(found);
    const uint32_t tab = g.U32(kSpuMallocTab);
    const uint32_t e = off + tab;
    const uint32_t w = g.U32(e);
    if (w & kTerminal) {                                                    // 0x8004F51C: cut from the rest
        if (!(found < g.S32(kSpuMallocMax))) return 0xFFFFFFFFu;
        if (g.U32(e + 4u) - reserve < size) return 0xFFFFFFFFu;
        const uint32_t next = static_cast<uint32_t>(found + 1);
        const uint32_t t = 8u * next + tab;
        g.W32(t, ((g.U32(e) & kAddr) + size) | kTerminal);
        g.W32(t + 4u, g.U32(e + 4u) - size);
        const uint32_t w2 = g.U32(e);
        g.W32(kSpuMallocLast, next);
        g.W32(e + 4u, size);
        g.W32(e, w2 & kAddr);
        SpuMallocGc(g);
        return g.U32(g.U32(kSpuMallocTab) + off);
    }
    const uint32_t have = g.U32(e + 4u);                                    // 0x8004F5C4: a free block
    if (size < have) {
        const int32_t last = g.S32(kSpuMallocLast);
        if (last < g.S32(kSpuMallocMax)) {                                  // the rest becomes a free entry at `last`,
            const uint32_t t = 8u * static_cast<uint32_t>(last) + tab;      // which moves up one
            const uint32_t rest = (w + size) | kFree;
            const uint32_t tw = g.U32(t);
            const uint32_t ts = g.U32(t + 4u);
            g.W32(t, rest);
            g.W32(t + 4u, have - size);
            g.W32(kSpuMallocLast, static_cast<uint32_t>(last + 1));
            g.W32(t + 8u, tw);
            g.W32(t + 12u, ts);
        }
    }
    const uint32_t e2 = off + g.U32(kSpuMallocTab);                         // 0x8004F634
    const uint32_t w2 = g.U32(e2);
    g.W32(e2 + 4u, size);
    g.W32(e2, w2 & kAddr);
    SpuMallocGc(g);
    return g.U32(off + g.U32(kSpuMallocTab));
}

// ---------------------------------------------------------------------------- 0x8004F998
void SpuFree(GuestRam& g, uint32_t addr) {
    const int32_t n = g.S32(kSpuMallocMax);
    if (n > 0) {
        uint32_t e = g.U32(kSpuMallocTab);
        int32_t k = 0;
        do {
            const uint32_t w = g.U32(e);
            if (w & kTerminal) break;
            ++k;
            if (w == addr) {
                g.W32(e, addr | kFree);
                break;
            }
            e += 8u;
            if (g.Faulted()) return;
        } while (k < n);
    }
    SpuMallocGc(g);
}

// ---------------------------------------------------------------------------- 0x800210C4
bool MusicSpuAlloc(GuestRam& g, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    v0 = 0;
    if (g.U32(kMusicStreamCount) == 0) return !g.Faulted();
    uint32_t k = 0, rec = kMusicStreams;
    do {
        const uint32_t bytes = g.U32(kMusicChunkBytes) * g.U32(kMusicChunks); // mult / mflo
        const uint32_t at = SpuMalloc(g, bytes);
        ++k;
        if (g.Faulted()) return false;
        if (static_cast<int32_t>(at) < 0) {
            v0 = at;
            const uint32_t a[1] = {kMusicAllocFailFmt};
            uint32_t r = 0;
            return c.Call(kMusicPrintFn, a, 1, sp - 40u, r);
        }
        g.W32(rec + 32u, at);
        rec += 20u;
    } while (k < g.U32(kMusicStreamCount));
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- 0x80021174
uint32_t MusicSpuFree(GuestRam& g) {
    if (g.S32(kMusicStreamCount) <= 0) return 0;
    int32_t k = 0;
    uint32_t rec = kMusicStreams;
    do {
        SpuFree(g, g.U32(rec + 32u));
        ++k;
        g.W32(rec + 32u, 0);
        rec += 20u;
        if (g.Faulted()) return 0;
    } while (k < g.S32(kMusicStreamCount));
    return 0;
}

} // namespace rr::sim
