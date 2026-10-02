#include "game/sim/spine.h"

#include "game/sim/fixed.h"
#include "game/sim/integrator.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

uint32_t GameState(GuestRam& g) { return g.U32(kSpineGameStatePtr); }

uint32_t EffectRecord(int32_t idx) {
    return kEffectPool + kEffectRecordBytes * static_cast<uint32_t>(idx);
}

// The three counter VALUE registers, by physical address.
bool IsCounterValue(uint32_t a) {
    const uint32_t reg = a & 0x1FFFFFFFu;
    return reg == 0x1F801100u || reg == 0x1F801110u || reg == 0x1F801120u;
}

void ReadS16x3(GuestRam& g, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a + 2u * k);
}

} // namespace

// ============================================================================ SLUS 0x80043F00
uint32_t SpineGetRCnt(GuestRam& g, uint32_t spec, const SpineIo& io) {
    const uint32_t idx = spec & 0xFFFFu;                // andi v1,a0,0xffff
    if (!(static_cast<int32_t>(idx) < 3)) return 0;      // slti v0,v1,3
    const uint32_t a = (idx << 4) + g.U32(kRCntBasePtr); // sll 4; addu the block pointer
    if (IsCounterValue(a)) return io.rootCounter & 0xFFFFu; // lhu: the counter register
    return g.U16(a);
}

// ============================================================================ the effect records
// SLUS 0x80027178
int32_t EffectFindFree(GuestRam& g) {
    uint32_t p = g.U32(kEffectPoolWalkPtr);
    int32_t i = 0;
    for (;;) {
        if (((g.U32(p) >> 6) & 0xFu) == 0) break; // 0x80027188..0x80027198
        ++i;
        p += kEffectRecordBytes;
        if (!(i < 20)) break;
    }
    return (i < 20) ? i : -1;
}

// SLUS 0x800271CC
void EffectLink(GuestRam& g, uint32_t e, uint32_t idx) {
    const int32_t head = g.S8(e + 0x49u);
    if (head == -1) {
        g.W8(e + 0x49u, static_cast<uint8_t>(idx)); // the delay slot of the `jr ra`
        return;
    }
    uint32_t rec = EffectRecord(head);
    uint32_t w = g.U32(rec);
    while ((w & 0x3Fu) != 63u) {
        const int32_t next = static_cast<int32_t>(w << 26) >> 26; // sll 26; sra 26: a SIGNED link
        rec = kEffectPool + static_cast<uint32_t>(next * static_cast<int32_t>(kEffectRecordBytes));
        w = g.U32(rec);
        if (g.Faulted()) return;
    }
    g.W32(rec, (g.U32(rec) & ~0x3Fu) | (idx & 0x3Fu)); // re-read at 0x8002723C
}

// SLUS 0x8002705C
void EffectJitterSpray(GuestRam& g, uint32_t rec, int32_t speed, const SpineIo& io) {
    const uint32_t r1 = SpineGetRCnt(g, 0xF2000002u, io);
    g.W16(rec + 0x38u, static_cast<uint16_t>((r1 & 0xFFu) << 4));
    if (speed == 0) {
        g.W16(rec + 0x3Au, 50);
    } else if (speed < 18) {
        g.W16(rec + 0x3Au, 120);
    } else {
        const uint32_t r2 = SpineGetRCnt(g, 0xF2000002u, io) & 0xFFu;
        int32_t v = static_cast<int32_t>((r2 << 8) - r2) >> 8; // sll 8; subu; sra 8
        v -= 127;
        v = static_cast<int32_t>(static_cast<uint32_t>(v) << 24) >> 24; // sll 24; sra 24
        g.W16(rec + 0x3Au, static_cast<uint16_t>(v));
    }
}

// SLUS 0x800270F0
void EffectJitterBurst(GuestRam& g, uint32_t rec, const SpineIo& io) {
    const uint32_t r1 = SpineGetRCnt(g, 0xF2000002u, io);
    g.W16(rec + 0x38u, static_cast<uint16_t>((r1 & 0xFFu) << 4)); // the delay slot of the 2nd `jal`
    const uint32_t r2 = SpineGetRCnt(g, 0xF2000002u, io) & 0xFFu;
    uint32_t v = (r2 << 1) + r2;  // 3
    v = (v << 3) + r2;            // 25
    v = (v << 2) + r2;            // 101
    const int32_t s = static_cast<int32_t>(v << 16) >> 24; // sll 16; sra 24
    g.W16(rec + 0x3Au, static_cast<uint16_t>(s));
    if (!(static_cast<int16_t>(s) < 51)) g.W16(rec + 0x3Au, static_cast<uint16_t>(50 - s));
}

// SLUS 0x800289E8
void EffectLocalToWorld(GuestRam& g, uint32_t e, uint32_t v, uint32_t out) {
    const int32_t x = FixMul(g.S32(e + 0x130u), g.S32(v + 0u));
    const int32_t y = FixMul(g.S32(e + 0x138u), g.S32(v + 4u));
    const int32_t z = FixMul(g.S32(e + 0x134u), g.S32(v + 8u));
    int32_t r[3];
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t m0 = static_cast<int32_t>(g.S16(e + 0x1B0u + 2u * k)) << 4;
        const int32_t m1 = static_cast<int32_t>(g.S16(e + 0x1B6u + 2u * k)) << 4;
        const int32_t m2 = static_cast<int32_t>(g.S16(e + 0x1BCu + 2u * k)) << 4;
        const uint32_t sum = static_cast<uint32_t>(FixMul(m0, x)) + static_cast<uint32_t>(FixMul(m1, y)) +
                             static_cast<uint32_t>(FixMul(m2, z)) + g.U32(e + 0xB8u + 4u * k);
        r[k] = static_cast<int32_t>(sum) >> 10;
    }
    for (uint32_t k = 0; k < 3; ++k) g.W32(out + 4u * k, static_cast<uint32_t>(r[k]));
}

// SLUS 0x80027540
void CrashEmit(GuestRam& g, uint32_t e, uint32_t which, uint32_t kind) {
    const uint32_t gs = GameState(g);
    if (!(g.U16(e + 0xACu) < g.U32(gs + 0x30u))) return; // players only
    if (g.S8(e + 0x08u) > 0) return;
    if (!(static_cast<int32_t>(g.U32(e + 0x24u) >> 30) < 2)) return;
    const int32_t i = EffectFindFree(g);
    if (i == -1) return;
    const uint32_t w24 = g.U32(e + 0x24u);
    g.W32(e + 0x24u, (w24 & 0x3FFFFFFFu) | (((w24 >> 30) + 1u) << 30));
    const uint32_t rec = EffectRecord(i);
    uint32_t w = g.U32(rec);
    g.W8(rec + 0x3Cu, 0);
    w = (w & ~0x3C0u) | 0x100u;
    g.W32(rec, w);
    w = (w & 0xFFC03FFFu) | ((which & 0xFFu) << 14);
    w = (w & 0xFFFFC3FFu) | 0x400u;
    const uint32_t gs1 = GameState(g);
    g.W32(rec, w);
    const uint32_t clock = g.U32(gs1 + 0x10u);
    g.W8(rec + 0x3Du, 0);
    const uint32_t gs2 = GameState(g);
    g.W32(rec + 0x34u, 900);
    g.W16(rec + 0x3Eu, 30);
    g.W32(rec + 0x60u, 9);
    g.W32(rec + 0x24u, 0);
    g.W32(rec + 0x2Cu, 0);
    g.W32(rec + 0x28u, 0);
    g.W32(rec + 0x30u, clock);
    const uint32_t clock2 = g.U32(gs2 + 0x10u);
    g.W8(rec + 0x6Cu, static_cast<uint8_t>(kind));
    g.W8(rec + 0x6Du, 4);
    g.W32(rec + 0x64u, clock2);
    for (uint32_t k = 0; k < 4; ++k) g.W8(rec + 0x68u + k, static_cast<uint8_t>(k + 1u)); // 1..4
    // The part pair, out of the crash table and the bike model's part list (0x800276AC..).
    const uint32_t cls = g.U32(e + 0xB4u);
    uint32_t ta = 0x800537DAu + 6u * cls;
    ta += (g.U32(rec) >> 13) & 0x1FEu;
    const uint32_t model = g.U32(e + 0x00u);
    const int32_t b1 = g.S8(ta + 1u);
    const uint32_t parts = g.U16(model + 24u);
    const int32_t b0 = g.S8(ta + 0u);
    const uint32_t tb = kCrashFxPartTable + 4u * static_cast<uint32_t>(b1);
    const uint32_t node = g.U32(24u * parts + g.U32(e + 0x04u) - 24u);
    const uint32_t base = g.U32(node + 20u) + 20u * static_cast<uint32_t>(b0) + 4u;
    const int32_t p2 = g.S8(tb + 2u);
    g.W16(rec + 0x4Cu, g.U16(base + 2u * static_cast<uint32_t>(p2) + 12u));
    const int32_t p3 = g.S8(tb + 3u);
    const uint16_t second = g.U16(base + 2u * static_cast<uint32_t>(p3) + 12u);
    g.W16(rec + 0x4Eu, second); // the delay slot of the `jal`
    if (g.Faulted()) return;
    EffectLink(g, e, static_cast<uint32_t>(i));
}

// SLUS 0x80027778
void EffectBurst(GuestRam& g, uint32_t e, uint32_t kind, uint32_t life, uint32_t tag, const SpineIo& io) {
    const uint32_t count = (g.U32(e + 0x24u) >> 19) & 0xFu;
    const uint32_t half = (g.U8(GameState(g) + 0x04u) >> 4) & 1u;
    if (!(static_cast<int32_t>(count) < (20 >> half))) return;
    const int32_t i = EffectFindFree(g);
    if (i == -1) return;
    const uint32_t gs = GameState(g);
    const uint32_t last = g.U32(kEffectLastStamp);
    if (g.U32(gs + 0x10u) == last) return; // one per race-clock value
    const uint32_t rec = EffectRecord(i);
    uint32_t w = g.U32(rec);
    g.W32(rec + 0x34u, life);
    w = (w & 0xFFFFC3FFu) | 0x400u;
    w = (w & ~0x3C0u) | 0x1C0u;
    w &= 0xFFC03FFFu;
    w |= (kind & 0xFFu) << 14;
    const uint32_t clock = g.U32(gs + 0x10u);
    g.W32(rec, w);
    g.W32(kEffectLastStamp, clock);
    g.W8(rec + 0x3Cu, static_cast<uint8_t>(tag));
    const uint32_t w24 = g.U32(e + 0x24u);
    g.W32(e + 0x24u, (w24 & 0xFF87FFFFu) | (((((w24 >> 19) & 0xFu) + 1u) & 0xFu) << 19));
    EffectLocalToWorld(g, e, kEffectOffsetTable + 12u * kind, rec + 20u);
    const uint32_t a0 = g.U32(kEffectLastStamp);
    g.W8(rec + 0x6Cu, 0);
    const uint32_t v1 = g.U32(kEffectLastStamp);
    g.W32(rec + 0x60u, static_cast<uint32_t>(300 >> half));
    g.W8(rec + 0x6Du, 1);
    g.W32(rec + 0x30u, a0);
    g.W32(rec + 0x64u, v1);
    for (uint32_t k = 0; k < 4; ++k) g.W8(rec + 0x68u + k, static_cast<uint8_t>(7u + k)); // 7..10
    EffectJitterBurst(g, rec, io);
    g.W8(rec + 0x3Du, static_cast<uint8_t>((kind ^ 1u) != 0u));
    g.W32(rec + 0x40u, 3);
    g.W32(rec + 0x24u, 0);
    g.W32(rec + 0x2Cu, 0);
    g.W32(rec + 0x28u, 0);
    g.W16(rec + 0x3Eu, 30); // the delay slot of the `jal`
    if (g.Faulted()) return;
    EffectLink(g, e, static_cast<uint32_t>(i));
}

// SLUS 0x80027974
void EffectSpray(GuestRam& g, uint32_t e, uint32_t kind, const SpineIo& io) {
    const uint32_t gs0 = GameState(g);
    const bool player = g.U16(e + 0xACu) < g.U32(gs0 + 0x30u);
    const int32_t speed = g.S16(e + 0x1E2u);
    if (!player) return;
    const uint32_t w24 = g.U32(e + 0x24u);
    if (((w24 >> 5) & 3u) != 0) return;
    if (kind == 0) {
        if (!(static_cast<int32_t>((w24 >> 19) & 0xFu) < 4)) return;
        if (static_cast<int32_t>((w24 >> 23) & 3u) > 0) return;
    }
    const int32_t i = EffectFindFree(g);
    if (i == -1) return;
    const uint32_t clock = g.U32(GameState(g) + 0x10u);
    if (clock == g.U32(kEffectLastStamp)) return;
    g.W32(kEffectLastStamp, clock);
    const uint32_t rec = EffectRecord(i);
    uint32_t w = g.U32(rec);
    w = (w & ~0x3C0u) | 0x80u;
    w &= 0xFFC03FFFu;
    w |= (kind & 0xFFu) << 14;
    w = (w & 0xFFFFC3FFu) | 0x400u;
    g.W32(rec, w);
    uint16_t life;
    if (kind == 0) {
        const uint32_t v = g.U32(e + 0x24u);
        g.W32(e + 0x24u, (v & 0xFF87FFFFu) | (((((v >> 19) & 0xFu) + 1u) & 0xFu) << 19));
        life = 30;
    } else {
        const uint32_t v = g.U32(e + 0x24u);
        g.W32(e + 0x24u, (v & 0xFE7FFFFFu) | (((((v >> 23) & 3u) + 1u) & 3u) << 23));
        life = 35;
    }
    g.W16(rec + 0x3Eu, life);
    const int32_t a2 = static_cast<int32_t>(150u - 2u * static_cast<uint32_t>(speed));
    const int32_t lo = static_cast<int32_t>(~static_cast<uint32_t>(a2 >> 31) & static_cast<uint32_t>(a2));
    const int32_t d = static_cast<int32_t>(0x960000u - static_cast<uint32_t>(a2));
    const int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(d >> 31) & static_cast<uint32_t>(d));
    const uint32_t clock2 = g.U32(GameState(g) + 0x10u);
    g.W32(rec + 0x34u, static_cast<uint32_t>(lo) + static_cast<uint32_t>(hi));
    g.W32(rec + 0x30u, clock2); // the delay slot of the `jal`
    EffectJitterSpray(g, rec, speed, io);
    g.W8(rec + 0x3Du, 1);
    g.W32(rec + 0x24u, 0);
    g.W32(rec + 0x2Cu, 0);
    g.W32(rec + 0x28u, 0);
    g.W32(rec + 0x40u, 9); // the delay slot of the `jal`
    if (g.Faulted()) return;
    EffectLink(g, e, static_cast<uint32_t>(i));
}

// ============================================================================ RASHCDG 0x80090270
void RaceGo(GuestRam& g) {
    // Pass 1: the sleeping, non-player, non-police bikes.
    int32_t high = g.S32(g.U32(kSpinePoolTable + 0x0Cu));
    uint32_t e = g.U32(kSpinePoolTable);
    int32_t count = 0;
    if (high >= 0) {
        const uint32_t gs = GameState(g);
        for (; high >= 0; --high) {
            int32_t add = 0;
            if (g.S16(e + 0x140u) == 0 && !(g.U16(e + 0xACu) < g.U32(gs + 0x30u))) {
                const uint32_t cls = g.U8(g.U32(e + 0x43Cu) + 1u) & 0xFu;
                add = ((cls ^ 2u) != 0u) ? 1 : 0;
            }
            count += add;
            e += g.U32(kSpinePoolTable + 4u);
            if (g.Faulted()) return;
        }
    }
    // Pass 2: the first command delay.
    high = g.S32(g.U32(kSpinePoolTable + 0x0Cu));
    e = g.U32(kSpinePoolTable);
    if (high < 0) return;
    const uint32_t gs = GameState(g);
    for (; high >= 0; --high) {
        const uint32_t rd = g.U32(e + 0x43Cu);
        bool run = true;
        if ((g.U8(rd + 1u) & 0xFu) == 2u) run = g.U16(e + 0xACu) < g.U32(gs + 0x30u); // police: players only
        if (run) {
            const uint32_t slot = e + 0x3BCu;
            if (g.S16(e + 0x140u) == 0) {
                g.W16(slot + 4u, 1);
            } else {
                int32_t v = static_cast<int32_t>(g.U8(rd + 0x27u)) - count - 1;
                if (v < 0) v = 0;
                v >>= 1;
                g.W16(slot + 4u, (v != 0) ? static_cast<uint16_t>(static_cast<uint32_t>(v) << 2) : uint16_t{1});
                if (g.U8(g.U32(e + 0x354u) + 0x23Cu) & 0x10u) {
                    const uint32_t p = g.U32(e + 0x358u);
                    const int32_t depth = g.S8(p + 0x3B2u);
                    const uint16_t delay = g.U16(slot + 4u);
                    g.W16(p + static_cast<uint32_t>(depth * 8) + 0x3B8u, delay);
                }
            }
        }
        e += g.U32(kSpinePoolTable + 4u);
        if (g.Faulted()) return;
    }
}

// ============================================================================ SLUS 0x8001447C
uint32_t SpineMalloc(GuestRam& g, uint32_t n, uint32_t heap, bool& refused) {
    refused = false;
    if (n == 0) return 0;
    if (!(heap < 2)) return 0;
    // SLUS 0x800142B4(n, 0)
    uint32_t prev = kHeapHeads; // + 16 * 0: the heap argument was dropped
    const uint32_t size = (n + 11u) & ~7u;
    uint32_t blk = g.U32(prev);
    while (blk != 0) {
        const uint32_t have = g.U32(blk + 4u);
        if (have == size) {
            g.W32(prev, g.U32(blk));
            g.W32(blk, size);
            return blk + 4u;
        }
        if (size < have) {
            const uint32_t rest = blk + size;
            g.W32(prev, rest);
            g.W32(rest, g.U32(blk));
            g.W32(rest + 4u, g.U32(blk + 4u) - size);
            g.W32(blk, size);
            return blk + 4u;
        }
        prev = blk;
        blk = g.U32(blk);
        if (g.Faulted()) return 0;
    }
    refused = true; // SLUS 0x80044894, the BIOS print of the out-of-memory arm
    return 0;
}

// ============================================================================ RASHCDG 0x8008A998
void ViewEvent(GuestRam& g, uint32_t v, uint32_t mode) {
    const bool same = mode == g.U32(v + 0x220u);
    if (mode < 4u && g.U32(v + 0x21Cu) != mode) {
        const uint32_t add = same ? 2u : 6u;
        const uint32_t w224 = g.U32(v + 0x224u);
        g.W32(v + 0x220u, mode);
        const uint32_t target = g.U32(v + 0x238u);
        g.W32(v + 0x224u, (w224 & ~0x18u) | add);
        if ((g.U16(target + 0xACu) >> 5) != 0) {
            const uint32_t idx = (g.U16(v + 0xACu) == 0x9Eu) ? 1u : 0u;
            g.W32(v + 0x238u, g.U32(kSpinePlayerBikes + 4u * idx));
        }
    }
    const uint32_t w228 = g.U32(v + 0x228u);
    g.W32(v + 0x304u, (mode < 7u) ? 0u : 1u);
    if (w228 & 1u) g.W32(v + 0x224u, g.U32(v + 0x224u) | 0x100u);
    const uint32_t cleared = g.U32(v + 0x228u) & 0xFFFFFFFAu;
    const uint32_t arm = g.U32(v + 0x21Cu);
    g.W32(v + 0x228u, cleared);
    g.W32(v + 0x228u, (arm == 12u) ? (cleared | 0x80u) : (cleared & 0xFFFFFC7Fu));
    g.W32(v + 0x21Cu, mode);
    if (same && (g.U32(v + 0x228u) & 0x40u)) {
        const uint32_t w = g.U32(v + 0x228u);
        g.W32(v + 0x304u, 1);
        g.W32(v + 0x228u, w | 0x10u);
    }
}

// ============================================================================ SLUS 0x8002076C
void ReleaseContactAt(GuestRam& g, uint32_t e) {
    const uint32_t c = g.U32(e + 0x340u);
    if (c == 0) return;
    const uint32_t h = g.U16(c);
    const uint32_t pool = h >> 5;
    const uint32_t slot = h & 0x1Fu;
    if (pool == 3u) {
        if ((g.U32(e + 0x234u) & 0x60000u) != 0x40000u) return;
        const uint32_t car = kSpineTrafficPool + (slot << 9);
        int16_t a[3], b[3];
        ReadS16x3(g, car + 0x1C2u, a);
        ReadS16x3(g, e + 0x1C2u, b);
        const int32_t d = DotLcm(a, b);
        const int32_t f = FixMul(g.S32(car + 0x1E0u), d);
        int32_t s = static_cast<int32_t>(g.U32(e + 0x240u) + static_cast<uint32_t>(f));
        g.W32(e + 0x240u, static_cast<uint32_t>(s));
        if (s < 0x23C36) s = 0x23C36;
        g.W32(e + 0x240u, static_cast<uint32_t>(s));
        g.W32(e + 0x1E0u, static_cast<uint32_t>(s));
        int16_t dir[3];
        ReadS16x3(g, e + 0x1C2u, dir);
        int32_t out[3];
        Scale(s, dir, out);
        for (uint32_t k = 0; k < 3; ++k) g.W32(e + 0x1C8u + 4u * k, static_cast<uint32_t>(out[k]));
    } else if (pool < 4u) {
        if (pool == 0u) {
            const uint32_t b = g.U32(kSpinePool0Ptr) + 1096u * slot;
            g.W32(b + 0x230u, g.U32(b + 0x230u) & 0xFDFFFFFFu);
        }
    } else if (pool == 4u) {
        const uint32_t p = g.U32(kSpinePropPoolPtr) + 596u * slot;
        const uint32_t k = (g.U16(g.U32(p) + 14u) & 0xF80u) >> 7;
        if (k - 3u < 3u && (g.U32(p + 0x250u) & 0x200u)) g.W32(p + 0x22Cu, 0x10000u);
    }
}

// ============================================================================ SLUS 0x8002090C
bool ResetBikeState(GuestRam& g, uint32_t e) {
    const uint32_t stats = g.U32(e + 0x22Cu);
    if (g.U32(e + 0x340u) != 0) ReleaseContactAt(g, e);
    const uint32_t f230 = g.U32(e + 0x230u);
    static const uint16_t kZeroA[] = {0x30C, 0x308, 0x304, 0x300, 0x2FC, 0x2F8, 0x2F4, 0x2F0, 0x2EC,
                                      0x2D8, 0x2D4, 0x2D0, 0x2CC, 0x2C8, 0x2C0, 0x2B8, 0x2BC, 0x2B0,
                                      0x2A4, 0x2A8, 0x2AC, 0x2B4, 0x290, 0x294, 0x2A0, 0x284, 0x27C};
    for (uint16_t off : kZeroA) g.W32(e + off, 0);
    g.W32(e + 0x230u, f230 & 0xF8000000u);
    g.W32(e + 0x234u, g.U32(e + 0x234u) & 0xC0000000u);
    g.W32(e + 0x238u, g.U32(e + 0x238u) & 0xF7B00000u);
    static const uint16_t kZeroB[] = {0x280, 0x268, 0x26C, 0x270, 0x248, 0x2E8, 0x1E8,
                                      0x260, 0x25C, 0x254, 0x24C, 0x244, 0x240, 0x1E4};
    for (uint16_t off : kZeroB) g.W32(e + off, 0);

    // +0x258 from the stat block: the four sign arms of 0x80020A1C..0x80020ABC.
    constexpr int32_t kG = 0x9D087; // 9.8135 in 16.16
    const int32_t m = FixMul(g.S32(stats + 0x0Cu), kG);
    const int32_t d = g.S32(stats + 0x10u);
    int32_t r;
    if (m > 0) {
        if (d > 0) r = static_cast<int32_t>(FixDiv(static_cast<uint32_t>(m), static_cast<uint32_t>(d)));
        else r = -static_cast<int32_t>(FixDiv(static_cast<uint32_t>(m), static_cast<uint32_t>(-d)));
    } else {
        if (d > 0) r = -static_cast<int32_t>(FixDiv(static_cast<uint32_t>(-m), static_cast<uint32_t>(d)));
        else r = static_cast<int32_t>(FixDiv(static_cast<uint32_t>(-m), static_cast<uint32_t>(-d)));
    }
    g.W32(e + 0x258u, static_cast<uint32_t>(r));

    const uint32_t bc = g.U32(stats + 0xBCu);
    const int32_t heading = g.S16(e + 0x212u);
    g.W8(e + 0x351u, 0);
    g.W32(e + 0x33Cu, 0);
    g.W32(e + 0x3A4u, 0);
    g.W32(e + 0x2C4u, 0);
    static const uint16_t kZeroH[] = {0x34E, 0x34C, 0x34A, 0x348, 0x346, 0x344, 0x33A};
    for (uint16_t off : kZeroH) g.W16(e + off, 0);
    g.W32(e + 0x250u, bc); // the delay slot of the `jal`
    uint16_t asinTable[64];
    for (uint32_t k = 0; k < 64; ++k) asinTable[k] = g.U16(kSpineAsinTable + 2u * k);
    int32_t asinOut = 0;
    if (!Asin(static_cast<int32_t>(static_cast<uint32_t>(heading) << 4), asinTable, asinOut)) return false;
    g.W16(e + 0x34Au, static_cast<uint16_t>(asinOut));

    auto times157 = [](int32_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v) * 157u); };
    const int32_t a = times157(g.S16(e + 0x206u));
    g.W32(e + 0x2DCu, static_cast<uint32_t>(a));
    const int32_t b = times157(g.S16(e + 0x20Cu));
    g.W32(e + 0x2E0u, static_cast<uint32_t>(b));
    g.W32(e + 0x2E4u, static_cast<uint32_t>(times157(g.S16(e + 0x212u))));
    int32_t atanTable[20];
    for (uint32_t k = 0; k < 20; ++k) atanTable[k] = g.S32(kSpineAtanTable + 4u * k);
    const int32_t ang = RatAtan2(a, b, atanTable);
    const int32_t scaled = static_cast<int32_t>(static_cast<uint32_t>(ang) * 25736u) >> 8;
    const uint32_t partner = g.U32(e + 0x358u);
    g.W32(e + 0x29Cu, static_cast<uint32_t>(-scaled));
    if (partner != 0 && g.U32(e + 0x440u) != 0) g.W32(partner + 0x1E8u, 0);
    g.W32(e + 0x28Cu, g.U32(e + 0x29Cu) + g.U32(e + 0x27Cu));
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800BC7CC
bool StampResult(GuestRam& g, uint32_t e, StampResultCallees& c) {
    const int32_t depth = g.S8(e + 0x3B2u);
    const uint32_t top = g.U16(e + 0x3BCu + static_cast<uint32_t>((depth - 1) * 8));
    if (g.U32(e + 0x230u) & 0x08000000u) {
        const uint32_t rider = g.U32(e + 0x354u);
        const uint32_t cat = g.U16(kSpineStanceTable + 8u * g.U16(rider + 0x220u) + 2u);
        if (cat == 3u) {
            const uint32_t ev = g.U16(g.U32(kSpineFightRecPtr) + 12u * g.U8(rider + 0x239u));
            if (g.Faulted() || !c.StanceEvent(ev, rider, 2)) return false;
        }
    }
    if (g.Faulted()) return false;
    if (!c.ClearCommands(e)) return false;
    if (!c.PushCommand(2, 224, 1, e)) return false;
    if (top == 0u || top == 18u) {
        const uint16_t target = (top == 18u) ? g.U16(e + 0xACu) : uint16_t{224};
        if (!c.PushCommand(static_cast<uint16_t>(top), target, 0, e)) return false;
    }
    g.W16(e + 0x3B0u, 0);
    g.W32(e + 0x38Cu, 0);
    return !g.Faulted();
}

} // namespace rr::sim
