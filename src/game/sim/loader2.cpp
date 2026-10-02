// The race loader's set-up path, second part (loader2.h), ported from our own disassembly of RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06) and SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1).
// The instruction addresses in the comments are the original's.
#include "game/sim/loader2.h"

namespace rr::sim {
namespace {

constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }

bool Call(LoaderCallees& c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp, uint32_t* v0 = nullptr) {
    uint32_t a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int n = 0;
    for (uint32_t x : args)
        if (n < 8) a[n++] = x;
    uint32_t r = 0;
    const bool ok = c.Call(fn, a, n, sp, r);
    if (v0 != nullptr) *v0 = r;
    return ok;
}

uint32_t Gs(GuestRam& g) { return g.U32(kLdGameStatePtr); }
uint32_t CdMode(GuestRam& g) { return g.U32(kLdCdMode); }

// SLUS 0x80044934 memset(dst, byte, n), a leaf (bytes; n <= 0 stores nothing)
void MemSet8(GuestRam& g, uint32_t dst, uint32_t v, int32_t n) {
    if (dst == 0u) return;
    for (int32_t k = 0; k < n; ++k) g.W8(dst + U(k), static_cast<uint8_t>(v));
}

} // namespace

// ============================================================================ SLUS leaves
void SirenSet(GuestRam& g, int32_t set) {                                  // SLUS 0x80018DC8
    int32_t v = -1;
    if (set == 2) v = 22;
    else if (set == 1) v = 21;
    else if (set == 4) v = 23;
    else if (set == 5) v = 24;
    g.W32(g.gp() + 1908u, U(v));
}

void FxGlobalsInit(GuestRam& g) {                                          // SLUS 0x8002B83C
    constexpr uint32_t b = 0x800D7FC8u;
    g.W32(b + 144u, 80);
    g.W32(b + 148u, 0x800538D0u);
    g.W8(b + 153u, 0);
    g.W8(b + 152u, 0);
    g.W16(b + 34u, 2);
    g.W16(b + 32u, 2);
    g.W16(b + 38u, 2);
    g.W16(b + 36u, 2);
}

void SRand(GuestRam& g, uint32_t seed) { g.W32(g.gp() + 2076u, seed); }   // SLUS 0x8001FC84

uint32_t GrfFix(GuestRam& g, uint32_t grf) {                               // SLUS 0x80024610
    const uint32_t a = g.U32(grf + 20u), b = g.U32(grf + 24u);
    g.W32(grf + 20u, grf + a);
    g.W32(grf + 24u, grf + b);
    return 1;
}

// ============================================================================ RASHCDI 0x8005D5E4 / 0x8005D988
void FxRecordsInit(GuestRam& g) {
    g.W32(0x800D8068u, 0x800D39B0u);
    g.W32(0x800D806Cu, 0);
    g.W32(0x800D8070u, 0);
    g.W32(0x800D8074u, 0);
    for (uint32_t k = 0; k < 20u; ++k) {                                   // 0x8005D9B8
        const uint32_t r = 0x800D39B0u + 112u * k;
        const uint32_t w = g.U32(r);
        g.W8(r + 60u, 0);
        g.W32(r, (w & 0xFFFFC03Fu) | 0x3Fu);
    }
    g.W32(0x8005B360u, 0);
}

void FxReset(GuestRam& g) {
    g.W32(0x8005B280u, 0);                                                 // the delay slot of the first call
    FxRecordsInit(g);                                                      // (0x8005D968 -> 0x8005D960: `jr ra`)
}

// ============================================================================ RASHCDI leaves
void RaceFlag(GuestRam& g) {                                               // RASHCDI 0x8006968C
    if (g.U32(0x8005AD34u) == 0u) g.W32(0x8005AD34u, 1);
}

void PedTablesInit(GuestRam& g) {                                          // RASHCDI 0x80068500 (its only call a leaf)
    constexpr uint32_t ids = 0x800D5F40u, st = 0x800D5730u;
    MemSet8(g, ids, 0, 26);                                                // SLUS 0x8001E0DC
    for (uint32_t i = 0; i < 13u; ++i) g.W16(ids + 2u * i, static_cast<uint16_t>(0x5000u + i));
    g.W16(st, 73);
    g.W16(st + 6u, 230);
    g.W16(st + 12u, 230);
    g.W16(st + 18u, 50);
    g.W16(st + 20u, 4);
    g.W16(st + 24u, 70);
    g.W16(st + 2u, 0);
    g.W8(st + 4u, 0);
    g.W16(st + 8u, 0);
    g.W8(st + 10u, 0);
    g.W16(st + 14u, 0);
    g.W8(st + 16u, 0);
    g.W8(st + 22u, 0);
    g.W16(st + 26u, 0);
    g.W8(st + 28u, 0);
    g.W16(st + 30u, 69);
    g.W16(st + 32u, 0);
    g.W8(st + 34u, 0);
}

// ============================================================================ RASHCDI 0x80069000
bool WorldPoolsInit(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2WorldPoolsFrame;
    constexpr uint32_t p6 = 0x800CD6A8u, cap = 0x8005B214u, vols = 0x800D5CF8u, peds = 0x800D4B70u;
    g.W32(p6 + 28u, 0);                                                    // 0x80069034
    g.W32(p6 + 12u, 0);
    g.W32(cap, (g.U8(Gs(g) + 4u) & 0x10u) ? 32u : 24u);                   // 0x8006905C
    uint32_t p = 0;
    if (!Call(c, kLdMalloc, {g.U32(cap) * 280u, 0}, F, &p)) return false;  // 0x8006907C
    g.W32(p6 + 28u, p);
    if (p != 0u) {
        if (!Call(c, kLdMemset, {p, 0, g.U32(cap) * 280u}, F)) return false;
        g.W32(p6 + 12u, g.U32(cap) - 1u);
    }
    g.W32(vols, 0);                                                        // 0x800690D4
    g.W32(vols + 4u, 0);
    g.W32(vols + 32u, 0);
    g.W32(vols + 36u, 0);
    for (int32_t i = 0; i < g.S32(Gs(g) + 48u); ++i) {                     // 0x800690F0
        const uint32_t s0 = vols + 32u * U(i);
        const uint32_t n = g.U32(cap);
        g.W32(s0, n);                                                      // the delay slot of the malloc
        if (!Call(c, kLdMalloc, {n << 3, 0}, F, &p)) return false;
        g.W32(s0 + 4u, p);
        if (p == 0u) continue;
        for (int32_t k = 0; k < g.S32(cap); ++k) {                         // 0x80069148
            g.W16(g.U32(s0 + 4u) + 8u * U(k), static_cast<uint16_t>(k));
            g.W16(g.U32(s0 + 4u) + 8u * U(k) + 2u, 0xFFFF);
            g.W32(g.U32(s0 + 4u) + 8u * U(k) + 4u, 0xFFFFFFFFu);
        }
        for (uint32_t k = 0; k < 3u; ++k) {                                // 0x80069188
            g.W32(s0 + 8u + 4u * k, 0);
            g.W32(s0 + 20u + 4u * k, 0xFFFFFFFFu);
        }
    }
    g.W32(peds + 16u, 0);                                                  // 0x800691D8
    g.W32(peds + 12u, 0);
    if (g.U32(kLdPedSwitch) != 0u) {                                      // 0x800691DC
        if (!Call(c, kLdMalloc, {2288, 0}, F, &p)) return false;
        g.W32(peds + 16u, p);
        if (p != 0u) {
            if (!Call(c, kLdMemset, {p, 0, 2288}, F)) return false;
            g.W32(peds + 12u, 3);
        }
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80068AA4
bool PoolTableInit(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2PoolTableFrame;
    MemSet8(g, F + 16u, 0, 8);                                             // 0x80068AC4: SLUS 0x80044934
    if (g.U8(0x8005AD32u) != 0u) return !g.Faulted();
    uint32_t gs = Gs(g);
    const uint32_t t = g.U8(gs + 4u);
    bool skip = false;                                                     // s2: the 8 bytes are not kept
    if (!((t & 0x20u) != 0u || t == 1u || t == 8u)) skip = true;          // 0x80068AF0..0x80068B0C
    else if (g.U8(0x800D80DDu) & 8u) skip = true;
    else if (g.U32(gs + 60u) == 0u && (g.U8(gs + 5u) & 1u)) skip = true;
    for (uint32_t i = 0; i < 20u; ++i) {                                   // 0x80068B70
        const uint32_t keep = 0x800D5784u + 72u * i, rec = keep - 44u;
        if (!skip && !Call(c, kLdMemcpy, {F + 16u, keep, 8}, F)) return false;
        if (!Call(c, kLdMemset, {rec, 0, 72}, F)) return false;
        if (!skip && !Call(c, kLdMemcpy, {keep, F + 16u, 8}, F)) return false;
        if (i == 1u) {                                                     // 0x80068BBC
            gs = Gs(g);
            skip = g.U8(gs + 4u) != 34u || (g.U8(gs + 5u) & 1u) != 0u;
        } else if (i == 17u) {
            skip = true;
        }
    }
    if (!Call(c, kLdPopResetFn, {}, F)) return false;                      // 0x80068C14
    g.W8(0x8005AD32u, 1);
    constexpr uint32_t v1 = kL2PoolTable;
    const uint32_t strides[7] = {1096, 628, 572, 512, 596, 0xFFFFFE3Cu, 280};
    for (uint32_t k = 0; k < 7u; ++k) g.W32(v1 + 4u + 16u * k, strides[k]);
    g.W32(v1 + 8u, 0x8005B1F8u);
    g.W32(v1 + 24u, 0x8005B218u);
    g.W32(v1 + 56u, 0x800CF650u);
    g.W32(v1 + 12u, 0x8005AD38u);
    g.W32(v1 + 28u, 0x8005AD3Cu);
    g.W32(v1 + 48u, 0x800CF660u);
    g.W32(v1 + 44u, 0x800D4B78u);
    g.W32(v1 + 76u, 0x800CD6D0u);
    g.W32(v1 + 40u, 0x800D4B70u);
    g.W32(v1 + 72u, 0x800CD6C8u);
    g.W32(v1 + 88u, 0x800CE598u);
    g.W32(v1 + 104u, 0x800CD6A8u);
    g.W32(v1 + 60u, 0x800CF658u);
    g.W32(v1 + 92u, 0x800CE5A0u);
    const uint32_t a = g.U32(0x800D4B80u), b = g.U32(0x800CD6D4u), d = g.U32(0x800CE5A4u), e = g.U32(0x800CD6C4u);
    g.W32(v1 + 108u, 0x800CD6B0u);
    g.W32(v1 + 32u, a);
    g.W32(v1 + 64u, b);
    g.W32(v1 + 80u, d);
    g.W32(v1 + 96u, e);
    if (!Call(c, kLdMemset, {0x800D43C0u, 0, 56}, F)) return false;       // 0x80068D30
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x800696B8
bool TrialLimits(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2TrialLimitsFrame;
    int32_t s1 = g.S32(g.U32(kL2PoolTable + 12u));
    uint32_t s0 = g.U32(kL2PoolTable);
    while (s1 >= 0) {                                                      // 0x800696F8
        const uint32_t gs = Gs(g);
        uint32_t v = 0;
        const bool cop = g.U16(s0 + 172u) < g.U32(gs + 48u) && (g.U8(g.U32(s0 + 1084u) + 1u) & 0xFu) == 2u;
        if (cop) {
            if (g.U32(0x8005B2B0u) != 0u) v = g.U32(0x80053048u + (g.U32(gs + 60u) << 2));
        } else {
            const uint32_t tab = g.U32(0x8005B2B0u) == 0u ? 0x8005303Cu : 0x80053054u;
            v = g.U32(tab + (g.U32(Gs(g) + 60u) << 2));
        }
        g.W32(s0 + 924u, v);                                               // 0x800697CC
        v = g.U32(s0 + 924u);
        g.W32(s0 + 480u, v);
        g.W32(s0 + 576u, v);                                               // the delay slot
        if (!Call(c, kL2Scale, {v, s0 + 450u, s0 + 456u}, F)) return false;
        --s1;
        s0 += g.U32(kL2PoolTable + 4u);
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80069394
bool PartSlotsInit(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2PartSlotsFrame;
    constexpr uint32_t peds = 0x800D4B70u, cars = 0x800CF650u;
    uint32_t p = 0;
    if (g.U32(peds + 16u) != 0u) {                                         // 0x800693BC
        uint32_t s2 = g.U32(peds + 16u);
        for (uint32_t i = 0; i < 4u; ++i, s2 += 572u) {
            if (!Call(c, kLdMalloc, {408, 0}, F, &p)) return false;
            g.W32(s2 + 4u, p);
            if (p == 0u) continue;
            g.W32(s2, 0);
            g.W32(s2 + 96u, 0);
            g.W32(s2 + 540u, 0);
            g.W32(peds + 12u, g.U32(peds + 12u) + 1u);
        }
    }
    uint32_t s2 = cars + 16u;
    for (uint32_t i = 0; i < 16u; ++i, s2 += 512u) {                       // 0x80069428
        if (!Call(c, kLdMalloc, {24, 0}, F, &p)) return false;
        g.W32(s2 + 4u, p);
        if (p == 0u) continue;
        g.W32(s2, 0);
        g.W32(s2 + 96u, 0);
        g.W32(cars + 12u, g.U32(cars + 12u) + 1u);
    }
    g.W32(0x800D1660u + 436u, 8344);                                       // 0x8006947C
    g.W32(0x800D1660u, 0);
    for (uint32_t k = 0; k < 18u; ++k) g.W32(0x800D1664u + 24u * k, 0);
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80068614 / 0x80068684
uint32_t CensusCount(GuestRam& g, uint32_t cls, uint32_t out) {
    uint32_t all = 0, live = 0;
    uint32_t e = g.U32(kL2PoolTable);
    for (int32_t n = g.S32(g.U32(kL2PoolTable + 12u)); n >= 0; --n) {   // 0x80068630
        if ((g.U8(g.U32(e + 1084u) + 1u) & 0xFu) == cls) {
            ++all;
            if (g.S16(e + 320u) != 0) ++live;
        }
        e += g.U32(kL2PoolTable + 4u);
    }
    g.W32(out, live);
    return all;
}

bool CensusInit(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    (void)c;
    const uint32_t F = sp - kL2CensusFrame;
    const uint32_t v0 = CensusCount(g, 2, F + 16u);                        // 0x8006868C, a leaf
    const uint32_t a1 = g.U32(F + 16u);
    g.W32(0x800D86F4u, v0);
    g.W32(0x800D86F0u, a1);
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x800689D0
bool PopBlockInit(GuestRam& g, uint32_t block, uint32_t size, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2PopBlockFrame;
    if (g.U8(0x8005AD31u) != 0u) return !g.Faulted();
    if (g.U32(kLdPedSwitch) != 0u && !Call(c, kL2PedTablesFn, {}, F)) return false;
    if (!Call(c, kL2PartSlotsFn, {}, F)) return false;
    if (!Call(c, kL2CensusFn, {}, F)) return false;
    if (!Call(c, kLdBlockCopyFn, {block, size}, F)) return false;
    g.W8(0x8005AD31u, 1);
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x800645E4
bool SpeedClassCopy(GuestRam& g, uint32_t block, uint32_t off, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2SpeedClassFrame;
    if (!Call(c, kLdMemcpy, {kL2SpeedClasses, block + off, 12}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80068740
bool EscapeLoad(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2EscapeLoadFrame;
    g.W32(kL2JailArrays + 4u, 0);                                          // 0x80068770
    g.W32(kL2JailArrays, 0);
    g.W32(kL2JailCounts + 4u, 0);
    g.W32(kL2JailCounts, 0);
    if (g.U8(Gs(g) + 4u) != 44u) return !g.Faulted();
    if (!Call(c, kL2StrCpy, {F + 24u, kL2GridName}, F)) return false;
    uint32_t s5 = 0;
    if (!Call(c, kL2StrRChr, {F + 24u, 46}, F, &s5)) return false;
    if (s5 == 0u) {                                                        // "Ay Caramba", then stores at address 0
        Call(c, kL2Printf, {0x8005B918u}, F);
        return false;
    }
    s5 -= 3u;
    uint32_t s0 = 0;
    for (uint32_t s2 = 0; s2 < 2u; ++s2) {                                 // 0x80068800
        g.W8(s5, 'J');
        g.W8(s5 + 1u, 'B');
        g.W8(s5 + 2u, static_cast<uint8_t>(s2 + 65u));
        if (s2 == 0u) {
            if (!Call(c, kL2Printf, {0x8005B924u, F + 24u}, F)) return false;
            uint32_t r = 0;
            if (!Call(c, kLdLoadFile, {F + 24u, CdMode(g), F + 712u, F + 716u, 0}, F, &r)) return false;
            s0 = F + 48u;
            if (S(r) < 0) {
                g.W32(F + 716u, 0);
            } else {
                if (!Call(c, kLdMemcpy, {F + 48u, g.U32(F + 712u), g.U32(F + 716u)}, F)) return false;
                if (!Call(c, kLdFree, {g.U32(F + 712u)}, F)) return false;
            }
        }
        if (g.U32(F + 716u) == 0u) continue;                               // 0x80068870
        const uint32_t cnt = kL2JailCounts + 4u * s2, arr = kL2JailArrays + 4u * s2;
        for (uint32_t k = 0; k < 5u; ++k) {                                // 0x80068888
            const uint32_t v = g.U32(s0);
            s0 += 4u;
            g.W32(F + 688u + 4u * k, v);
            g.W32(cnt, g.U32(cnt) + v);
        }
        if (g.U32(cnt) != 0u) {                                            // 0x800688C0
            uint32_t p = 0;
            if (!Call(c, kLdMalloc, {g.U32(cnt) * 12u, 0}, F, &p)) return false;
            g.W32(arr, p);
            if (!Call(c, kLdMemset, {p, 0, g.U32(cnt) * 12u}, F)) return false;
        }
        uint32_t idx = 0;
        for (uint32_t grp = 0; grp < 5u; ++grp) {                          // 0x80068908
            for (int32_t n = 0; n < g.S32(F + 688u + 4u * grp); ++n, ++idx) {
                const uint32_t a0 = 12u * idx;
                g.W32(g.U32(arr) + a0 + 8u, g.U32(s0));
                s0 += 4u;
                g.W32(g.U32(arr) + a0, g.U32(s0));
                s0 += 4u;
                g.W32(g.U32(arr) + a0 + 4u, g.U32(s0));
                s0 += 4u;
            }
        }
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80063158
bool AnimNoise(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2AnimNoiseFrame;
    if (g.U32(kL2NoiseFile) != 0u) return !g.Faulted();                    // 0x8006316C
    uint32_t r = 0;
    if (!Call(c, kLdLoadFile, {0x8005B7B8u, CdMode(g), F + 24u, F + 28u, 0}, F, &r)) return false; // "DATA\ANIMNOIZ.DAT"
    if (S(r) <= 0) return !g.Faulted();                                    // 0x80063194
    const uint32_t buf = g.U32(F + 24u);
    uint32_t a1 = buf + 8u;
    const uint32_t end = buf + g.U32(buf + 4u) + 8u;
    g.W32(kL2NoiseFile, buf);
    g.W32(kL2NoiseRecords, a1);
    if (S(a1) < S(end)) {                                                  // 0x800631C8: the records' +8 pointers
        uint32_t v1 = buf + 16u;
        do {
            const uint32_t v = g.U32(v1);
            if (v != 0u) g.W32(v1, v + buf);
            a1 += 12u;
            v1 += 12u;
        } while (S(a1) < S(end));
    }
    uint32_t a0 = a1 + 8u;                                                 // 0x800631FC: the event table
    const uint32_t end2 = a1 + g.U32(a1 + 4u) + 8u;
    g.W32(kL2NoiseEvents, a0);
    for (; S(a0) < S(end2); a0 += 4u) {
        const uint32_t v = g.U32(a0);
        if (v != 0u) g.W32(a0, v + g.U32(kL2NoiseFile));
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x800653E8
bool FightLoad(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kL2FightLoadFrame;
    if (!Call(c, kLdSprintf, {F + 24u, 0x8005B8CCu, name}, F)) return false; // "%s.bin"
    if (!Call(c, kLdLoadFile, {F + 24u, CdMode(g), F + 48u, F + 52u, 0}, F)) return false;
    const uint32_t base = g.U32(F + 48u);
    for (uint32_t i = 0; i < 40u; ++i) {                                   // 0x80065430
        const uint32_t rec = base + 12u * i;
        const uint32_t a = g.U32(rec + 4u), b = g.U32(rec + 8u);
        g.W32(rec + 4u, base + a);
        g.W32(rec + 8u, base + b);
    }
    v0 = base;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006A7F8 / 0x8006A98C / 0x8006AC4C / 0x8006AC6C
bool RoadText(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2RoadTextFrame;
    const uint32_t res = g.U32(kLdResListPtr);
    const uint32_t s1 = g.U32(res + 2648u) << 14, s2 = g.U32(res + 2616u);
    if (!Call(c, kLdSprintf, {F + 16u, 0x8005BA48u, name, g.U32(Gs(g) + 48u)}, F)) return false; // "%sgrf%1d.txt"
    uint32_t s0 = 0, s4 = 0;
    int32_t s3 = -1;
    if (!Call(c, kL2Open, {F + 16u, CdMode(g)}, F, &s0)) return false;
    if (S(s0) >= 0) {
        if (!Call(c, kL2Read, {s0, s2, s1, 0}, F, &s4)) return false;
        if (!Call(c, kL2Close, {s0}, F)) return false;
        s3 = 0;
    }
    if (s3 < 0) return false;                                              // 0x8006A894: `break 7` - the console stops
    if (!Call(c, kL2RouteParse, {s2, s4}, F)) return false;
    FinishTableInit(g);                                                    // 0x8006A8A4, a leaf
    return !g.Faulted();
}

bool RoadClear(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    (void)sp;
    (void)c;
    g.W32(0x8005B338u, 0);
    g.W32(0x8005B33Cu, 0);
    PieceListInit(g);                                                      // 0x8006A9A8, a leaf
    return !g.Faulted();
}

bool RoadRecords(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2RoadRecordsFrame;
    if (!Call(c, kL2RouteRecords, {name}, F)) return false;               // 0x8006AC54: a0 untouched
    return !g.Faulted();
}

bool RoadLoad(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kL2RoadLoadFrame;
    g.W32(0x8005AD54u, 0);                                                 // the delay slot of the first call
    if (!Call(c, kL2RoadTextFn, {name}, F)) return false;
    if (!Call(c, kL2RoadClearFn, {}, F)) return false;
    uint32_t r = 0;
    if (!Call(c, kL2RoadRecordsFn, {name}, F, &r)) return false;
    v0 = (r & 0xFFu) != 0u ? 1u : 0u;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x800244E0 / 0x80022F78 / 0x80023020
bool GrfLoad(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2GrfLoadFrame;
    const uint32_t src = g.U32(Gs(g) + 48u) == 2u ? 0x80010BD8u : 0x80010BE4u; // "STREAM2.GRF" / "STREAM1.GRF"
    if (!Call(c, kLdSprintf, {0x800D7E80u, src}, F)) return false;
    uint32_t s1 = 0, s0 = 0, v = 0;
    if (!Call(c, kL2OpenStream, {0x800D7E80u}, F, &s1)) return false;
    if (!Call(c, kL2Size, {s1}, F, &s0)) return false;
    if (g.U32(g.gp() + 472u) == 0u) {
        if (!Call(c, kLdMalloc, {s0, 0}, F, &v)) return false;
        g.W32(g.gp() + 472u, v);
    }
    if (!Call(c, kL2Read, {s1, g.U32(g.gp() + 472u), s0, 0}, F)) return false;
    if (!Call(c, kL2Close, {s1}, F)) return false;
    GrfFix(g, g.U32(g.gp() + 472u));                                       // 0x80024588, a leaf
    g.W32(g.gp() + 468u, g.U32(g.U32(g.gp() + 472u) + 8u));
    return !g.Faulted();
}

bool StreamSetUp(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2StreamSetUpFrame;
    if (g.U32(Gs(g) + 48u) == 1u && !Call(c, 0x80024630u, {}, F)) return false; // the album (one player)
    if (!Call(c, 0x80023498u, {}, F)) return false;
    if (!Call(c, 0x80022A1Cu, {2}, F)) return false;
    return !g.Faulted();
}

bool StreamStart(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2StreamStartFrame;
    const uint32_t r = g.U32(0x800D6184u);
    if (!Call(c, 0x800235C8u, {g.U32(r), U(static_cast<int32_t>(g.S16(r + 6u))), g.U32(r + 8u)}, F)) return false;
    if (!Call(c, 0x80023714u, {}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006982C
bool BuildRace(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kL2BuildRaceFrame;
    g.W32(F + 320u, 0);
    g.W32(F + 324u, 0);
    {
        const uint32_t gs = Gs(g);
        if (g.U8(gs + 4u) == 44u) {                                        // 0x80069858: Jailbreak
            g.W8(gs + 57u, 0);
            const int32_t dir = g.U16(0x80053174u + 6u) < g.U16(0x80053174u + 2u) ? -1 : 1;
            g.W32(0x8005B2E8u, U(dir));
            g.W8(Gs(g) + 10u, 0);
            g.W32(0x8005ACC0u, 0);
        }
    }
    if (!Call(c, kL2WorldPoolsFn, {}, F)) return false;                    // 0x8006989C
    if (!Call(c, kL2PoolTableFn, {}, F)) return false;
    RaceFlag(g);                                                           // 0x800698AC, a leaf
    uint32_t s0 = 0;
    if (!Call(c, kL2StrRChr, {kL2GridName, 46}, F, &s0)) return false;
    if (s0 == 0u) {                                                        // "Ay Caramba", then a store at -1
        Call(c, kL2Printf, {0x8005B918u}, F);
        return false;
    }
    g.W8(s0, 0);
    if (!Call(c, kL2StrCat, {kL2GridName, 0x8005B950u}, F)) return false;   // ".BIN"
    g.W8(s0 - 1u, g.U32(Gs(g) + 48u) < 2u ? 65u : 66u);                    // 'A' / 'B'
    uint32_t r = 0;
    if (!Call(c, kLdLoadFile, {kL2GridName, CdMode(g), F + 320u, F + 324u, 0}, F, &r)) return false;
    if (S(r) < 0) {
        g.W32(F + 324u, 0);
    } else {                                                               // 0x80069944: block game_state+0x40 - 1
        const uint32_t idx = g.U32(Gs(g) + 64u) - 1u;
        if (!Call(c, kLdMemcpy, {F + 24u, g.U32(F + 320u) + idx * 292u, 292}, F)) return false;
        if (!Call(c, kLdFree, {g.U32(F + 320u)}, F)) return false;
        g.W32(F + 324u, 292);
    }
    uint32_t t = 0;
    if (!Call(c, kL2GetRCnt, {0xF2000002u}, F, &t)) return false;         // 0x80069998
    SRand(g, U(S((t & 0xFFu) * 3305u) >> 6));                              // 0x800699C4, a leaf
    if (!Call(c, kL2BuildGrid, {F + 24u, g.U32(F + 324u)}, F)) return false;
    if (!Call(c, kL2ResetBikes, {}, F)) return false;
    if (!Call(c, kL2CameraSetUp, {}, F)) return false;
    if ((g.U8(Gs(g) + 4u) & 1u) && !Call(c, kL2TrialLimitsFn, {}, F)) return false;
    if (!Call(c, kL2PopBlockFn, {F + 24u, g.U32(F + 324u)}, F)) return false;
    const uint32_t off = g.U32(F + 324u) - 12u;
    g.W32(F + 324u, off);
    if (!Call(c, kL2SpeedClassFn, {F + 24u, off}, F)) return false;
    if (!Call(c, kL2EscapeLoadFn, {}, F)) return false;
    for (uint32_t a : {0x8005B2A8u, 0x8005B2A4u, 0x8005B2E0u, 0x8005B290u, 0x8005B2ECu, 0x8005B2FCu, 0x8005B210u,
                       0x8005B318u, 0x8005B2A0u, 0x8005B30Cu})
        g.W32(a, 0);                                                       // 0x80069A40..
    g.W32(0x8005B36Cu, 0xFFFF0000u);
    g.W32(0x8005B368u, 0xFFFF0000u);
    g.W32(0x8005B284u, 0xFFFFFFFFu);
    g.W8(0x8005B358u, 0);
    return !g.Faulted();
}

} // namespace rr::sim
