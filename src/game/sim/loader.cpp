// The race loader's set-up path (loader.h), ported from our own disassembly of RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06) and SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1).
// The instruction addresses in the comments are the original's.
#include "game/sim/loader.h"

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

// MIPS `div` by a constant 9 as the compiler emits it (mult 0x38E38E39, sra 1, minus the sign).
int32_t Div9(int32_t v) {
    const int64_t p = static_cast<int64_t>(v) * 0x38E38E39LL;
    const int32_t hi = static_cast<int32_t>(p >> 32);
    return (hi >> 1) - (v >> 31);
}

} // namespace

// ============================================================================ RASHCDI 0x80063B90
bool EnterRace(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdEnterRaceFrame;
    g.W8(Gs(g) + 3u, 1);                                                  // 0x80063BA4
    g.W32(kLdPedSwitch, (g.U32(Gs(g) + 4u) & 0x18u) == 0u ? 1u : 0u);      // 0x80063BC4
    for (uint32_t fn : {0x8001B6A8u, kLdRaceResetFn, 0x800635D0u, kLdSetUpRaceFn, 0x80060088u, 0x8001BDB0u})
        if (!Call(c, fn, {}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80063500
bool RaceReset(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdRaceResetFrame;
    g.W8(Gs(g), 3);                                                       // 0x80063528 (the delay slot)
    if (!Call(c, 0x80018C1Cu, {1}, F)) return false;
    if (!Call(c, 0x80020E30u, {1}, F)) return false;
    if (!Call(c, 0x8005D9ECu, {0x100}, F)) return false;
    for (uint32_t fn : {kLdModelTablesFn, 0x8005D5E4u, 0x8001C1ACu, 0x8005D8D0u, 0x80063E48u, 0x8005ED94u})
        if (!Call(c, fn, {}, F)) return false;
    for (uint32_t k = 0; k < 6u; ++k)                                      // 0x80063574
        if (!Call(c, kLdMemset, {kLdPlayerBlocks + 36u * k, 0, 8}, F)) return false;
    for (uint32_t fn : {0x80063C20u, 0x8002D2E0u, 0x80063C00u, 0x8005D330u, 0x8005D500u})
        if (!Call(c, fn, {}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80063670
bool SetUpRace(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdSetUpRaceFrame;
    g.W32(kLdClock0, 0);                                                  // 0x80063678
    g.W32(kLdClock1, 0);
    uint32_t gs = Gs(g);
    g.W32(kLdTimeLimit, 0);                                               // 0x80063698
    const uint32_t t = g.U8(gs + 4u);
    g.W32(gs + 16u, 0);                                                   // 0x800636AC
    if (t & 1u) {                                                         // 0x800636A4
        const uint32_t tab = (t & 0x10u) ? 0x80053090u : 0x80053084u;
        const uint32_t v = g.U32(tab + (g.U32(gs + 60u) << 2));
        g.W32(kLdTimeLimit, ((((v << 2) + v) << 4) - ((v << 2) + v)) << 2);   // 0x800636E4..0x800636F8: * 300
    }
    gs = Gs(g);
    auto times300 = [](uint32_t v) { return ((((v << 2) + v) << 4) - ((v << 2) + v)) << 2; };
    if (g.U8(gs + 4u) == 36u) g.W32(kLdTimeLimit, times300(g.U16(gs + 8u)));   // 0x80063718
    if (g.U8(gs + 4u) == 33u) g.W32(kLdTimeLimit, times300(g.U32(0x8005ADE0u))); // 0x80063748
    if (g.U8(gs + 4u) == 44u) g.W32(kLdTimeLimit, times300(g.U32(0x8005ADE4u))); // 0x80063778
    uint32_t v0 = 0;
    if (!Call(c, 0x8005D130u, {0x800CE170u}, F)) return false;            // the animation descriptor
    if (!Call(c, 0x800653E8u, {0x800524DCu}, F, &v0)) return false;       // "DATA\FIGHT"
    g.W32(kLdFightPtr, v0);                                               // 0x800637B8
    if (!Call(c, 0x800244E0u, {}, F)) return false;
    if (!Call(c, 0x8006AC6Cu, {0x80052400u}, F)) return false;            // "DATA\ROAD"
    if (!Call(c, 0x80022F78u, {}, F)) return false;
    if (!Call(c, 0x80023020u, {}, F)) return false;
    {
        const uint32_t r = g.U32(0x800D6184u);                            // 0x800637D8
        if (!Call(c, 0x8006ACD0u, {g.U32(r), g.U32(r + 4u), g.U32(r + 8u)}, F)) return false;
    }
    if (!Call(c, 0x8006982Cu, {}, F)) return false;                       // BuildRace
    uint32_t s0 = 1, a0 = 0;
    if (!(g.U8(Gs(g) + 4u) & 0x10u)) {                                    // 0x80063814
        if (!Call(c, 0x8006AD4Cu, {}, F, &v0)) return false;              // HazardPick
        s0 = v0;
        a0 = v0;
    }
    if (!Call(c, 0x80018DC8u, {a0}, F)) return false;
    if (!Call(c, 0x8005C7F0u, {s0}, F)) return false;
    {                                                                     // 0x80063844: the animation-object count
        uint32_t n = g.U32(0x8005B218u) + (g.U32(kLdPedSwitch) << 2);
        n += (g.U8(Gs(g) + 4u) == 8u) ? 2u : 1u;
        if (g.U8(Gs(g) + 4u) == 44u) n += 4u;
        if (!Call(c, 0x8005D1A0u, {0x800CE170u, n}, F)) return false;
    }
    if (g.U32(0x8005B220u) != 0u) {                                       // 0x800638B4
        g.W32(Gs(g) + 52u, 4);
    } else {
        const uint32_t p1 = g.U32(0x8005B38Cu);                           // 0x800638CC
        const uint32_t gsA = Gs(g);
        uint32_t x = 0;
        if (g.U8(g.U32(p1 + 852u) + 572u) & 0x10u) x = (g.U8(g.U32(g.U32(p1 + 856u) + 852u) + 572u) & 0x40u) == 0u ? 1u : 0u;
        const uint32_t p2 = g.U32(0x8005B21Cu);
        uint32_t v1 = x + 1u;
        if (p2 != 0u) {                                                   // 0x8006391C
            uint32_t y = 0;
            if (g.U8(g.U32(p2 + 852u) + 572u) & 0x10u) y = (g.U8(g.U32(g.U32(p2 + 856u) + 852u) + 572u) & 0x40u) == 0u ? 1u : 0u;
            v1 = x + 2u + y;
        }
        g.W32(gsA + 52u, v1);                                             // 0x80063974
        if (!(g.U32(Gs(g) + 52u) < 3u)) g.W32(Gs(g) + 52u, 4);            // 0x80063990
    }
    if (!(g.U8(Gs(g) + 4u) & 0x10u))                                      // 0x800639AC
        if (!Call(c, 0x8006AEF4u, {}, F)) return false;                   // HazardSetup
    if (!Call(c, 0x800235B0u, {0, g.U32(0x8005B38Cu)}, F)) return false;
    if (!Call(c, 0x800235B0u, {1, g.U32(0x8005B21Cu)}, F)) return false;
    if (!Call(c, 0x8002B83Cu, {}, F)) return false;
    if (!Call(c, 0x8005CEE0u, {0}, F)) return false;
    if (!(g.U32(Gs(g) + 48u) < 2u))                                       // 0x800639FC
        if (!Call(c, 0x8005CEE0u, {1}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006A7C0
void FinishTableInit(GuestRam& g) {
    for (uint32_t k = 0; k < 18u; ++k) {
        const uint32_t r = kLdFinishTable + 16u * k;
        g.W32(r, 0xFFFFFFFFu);
        g.W32(r + 4u, 0xFFFFFFFFu);
        g.W32(r + 8u, 0);
        g.W32(r + 12u, 0);
    }
}

// ============================================================================ RASHCDI 0x80068470
bool RaceBlockCopy(GuestRam& g, uint32_t block, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdBlockCopyFrame;
    uint32_t s0 = block + 220u;
    if (!Call(c, kLdMemcpy, {kLdHazardTable, s0, 8}, F)) return false;
    s0 += 8u;
    if (!Call(c, kLdMemcpy, {kLdHazardEvents, s0, 4}, F)) return false;
    s0 += 4u;
    for (uint32_t k = 0; k < 3u; ++k, s0 += 16u)
        if (!Call(c, kLdMemcpy, {kLdHazardTemplates + 16u * k, s0, 16}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006A8FC
void PieceListInit(GuestRam& g) {
    for (uint32_t k = 0; k < 6u; ++k) {
        const uint32_t r = kLdPieceList + 16u * k;
        g.W32(r, 0xFFFFFFFFu);
        g.W32(r + 4u, 0);
        g.W32(r + 8u, 0);
        g.W32(r + 12u, 0);
    }
    g.W32(kLdPiecesUsed, 0);
    g.W32(kLdPiecesLast, 0xFFFFFFFFu);
}

// ============================================================================ RASHCDI 0x800609B0
bool SkyInit(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdSkyInitFrame;
    uint32_t p = 0;
    if (!Call(c, kLdMalloc, {0x3048, 1}, F, &p)) return false;
    g.W32(kLdSkyPtr, p);                                                  // 0x800609D8
    if (!Call(c, kLdBzero, {p, 0x3048}, F)) return false;
    const uint32_t s = g.U32(kLdSkyPtr);
    const uint16_t b394 = g.U16(0x8005B394u);
    g.W32(s, 0x3048);                                                     // 0x800609F4
    g.W16(s + 18u, 0xFFFF);
    g.W16(s + 32u, 1);
    const uint16_t b390 = g.U16(0x8005B390u);
    g.W16(s + 58u, b394);
    g.W16(s + 62u, b394);
    const uint16_t b = static_cast<uint16_t>(b394 + 256u);
    for (uint32_t o : {64u, 66u, 68u, 70u}) g.W16(s + o, b);
    g.W32(s + 24u, 0);
    g.W16(s + 16u, 0);
    g.W16(s + 20u, 0);
    g.W32(s + 4u, 0xFFFFFFFFu);
    g.W16(s + 34u, 0);
    g.W32(s + 8u, 0xFFFFFFFFu);
    g.W32(s + 12u, 0xFFFFFFFFu);
    g.W16(s + 36u, 0);
    g.W16(s + 56u, b390);
    g.W16(s + 60u, b390);                                                 // 0x80060A5C (the delay slot)
    if (!Call(c, kLdMemset, {s + 740u, 0, 240}, F)) return false;
    g.W16(g.U32(kLdSkyPtr) + 22u, 2);                                     // 0x80060A70
    if (!Call(c, 0x8004D9A8u, {0}, F)) return false;                      // libgpu / interrupt set-up
    if (!Call(c, 0x8004D9E4u, {0}, F)) return false;
    if (!Call(c, 0x80043DA4u, {}, F)) return false;                       // EnterCriticalSection
    if (!Call(c, 0x8004DA44u, {0x80013AE4u}, F)) return false;           // the vsync callback
    if (!Call(c, 0x80043DB4u, {}, F)) return false;                       // ExitCriticalSection
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8005D410
bool ResTableInit(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdResTableFrame;
    uint32_t r = g.U32(kLdResListPtr);
    if (g.U32(Gs(g) + 48u) == 1u) {                                       // 0x8005D428
        g.W32(r + 2628u, 29);
        g.W32(r + 2640u, 30);
        g.W32(r + 2644u, 31);
        g.W32(r + 2648u, 32);
        g.W32(r + 2624u, 0);
        g.W32(r + 2636u, 0);
        g.W32(r + 2632u, 0);
        g.W32(r + 2652u, 2);
    } else {
        g.W32(r + 2628u, 15);
        g.W32(r + 2632u, 16);
        g.W32(r + 2636u, 31);
        g.W32(r + 2624u, 0);
        g.W32(r + 2644u, 0);
        g.W32(r + 2640u, 0);
        g.W32(r + 2648u, 32);
        g.W32(r + 2652u, 0);
    }
    r = g.U32(kLdResListPtr);
    const uint32_t n = g.U32(r + 2648u);
    for (uint32_t o : {0u, 4u, 8u, 12u, 20u, 24u, 32u, 36u, 40u, 28u}) g.W32(r + o, 0);
    g.W32(r + 16u, n);                                                    // 0x8005D4DC
    {                                                                     // 0x8005D338, its own frame
        const uint32_t F2 = F - kLdResHeapFrame;
        const uint32_t rr = g.U32(kLdResListPtr);
        if (g.U32(rr + 2620u) == 0u) {
            g.W32(rr + 2620u, 1);
            uint32_t p = 0;
            if (!Call(c, kLdMalloc, {g.U32(rr + 2648u) << 14, 0}, F2, &p)) return false;
            g.W32(g.U32(kLdResListPtr) + 2616u, p);
        }
    }
    {                                                                     // 0x8005D38C, a leaf
        const uint32_t rr = g.U32(kLdResListPtr);
        uint32_t buf = g.U32(rr + 2616u);
        for (uint32_t i = 0; S(i) < g.S32(rr + 2648u); ++i, buf += 0x4000u) {
            const uint32_t rec = rr + 44u + 36u * i;
            g.W32(rec, 0);
            g.W32(rec + 12u, buf);
            g.W32(rec + 4u, i);
            g.W32(rec + 16u, 0);
            g.W32(rec + 20u, 0);
            g.W32(rec + 24u, 0);
            g.W32(rec + 32u, 0);
        }
    }
    {                                                                     // 0x8005D3F4, a leaf
        const uint32_t rr = g.U32(kLdResListPtr);
        g.W32(rr + 2352u, 0);
        g.W32(rr + 2348u, 0);
        g.W32(rr + 2356u, 0);
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8005BE40
bool ModelTablesInit(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdModelTablesFrame;
    {                                                                     // 0x8005D018, a leaf
        for (uint32_t a = kLdSlotById + 216u, k = 0; k < 109u; ++k, a -= 2u) g.W16(a, 0xFFFF);
        const uint32_t f = kLdFamilies;
        g.W32(f + 0u, 0);
        g.W32(f + 24u, 150);
        g.W32(f + 40u, 100);
        g.W32(f + 48u, 20);
        g.W32(f + 56u, 300);
        g.W32(f + 64u, 45);
        g.W32(f + 72u, 400);
        g.W32(f + 28u, kLdSlotById);
        g.W32(f + 88u, 800);
        g.W32(f + 104u, 200);
        g.W32(f + 4u, 0);
        g.W32(f + 8u, 0);
        g.W32(f + 12u, 0);
        g.W32(f + 16u, 21);
        g.W32(f + 20u, 0);
        g.W32(f + 32u, 21);
        g.W32(f + 36u, 0);
        g.W32(f + 44u, kLdSlotById + 42u);
        g.W32(f + 52u, 0);
        g.W32(f + 60u, kLdSlotById + 84u);
        g.W32(f + 68u, 0);
        g.W32(f + 76u, kLdSlotById + 124u);
        g.W32(f + 80u, 1);
        g.W32(f + 84u, 0);
        g.W32(f + 92u, kLdSlotById + 214u);
        g.W32(f + 96u, 1);
        g.W32(f + 100u, 0);
        g.W32(f + 108u, kLdSlotById + 216u);
    }
    uint32_t list = kLdClassListArea;                                     // 0x8005BE70
    for (uint32_t k = 0; k < 7u; ++k) {
        const uint32_t e = kLdClassLists + 8u * k;
        g.W16(e + 2u, 0xFFFF);
        g.W16(e, 0);
        g.W32(e + 4u, list);
        list += U(static_cast<int32_t>(g.S8(kLdClassCaps + k)) << 2);
    }
    if (!Call(c, kLdMemset, {kLdRegistry, 0, 800}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ the registry's leaves
int32_t RegistryFind(GuestRam& g, uint32_t id) {                         // RASHCDI 0x8005C010
    int32_t k = 0;
    for (; k < 50; ++k)
        if (g.U32(kLdRegistry + 16u * U(k)) == id) break;
    return k == 50 ? -1 : k;
}

void TexKey(GuestRam& g, uint32_t slot) {                                 // RASHCDI 0x8005C054
    const uint32_t key = g.U32(g.U32(g.U32(kLdRegistry + 16u * slot + 8u)) + 28u);
    uint32_t p = g.U32(kLdPageTablePtr);
    int32_t i = 0;
    for (; i < 34; ++i, p += 12u)
        if (g.U8(p) == key) break;
    g.W8(kLdRegistry + 16u * slot + 7u, static_cast<uint8_t>(i < 34 ? i : -1));
}

void ClassFirst(GuestRam& g, uint32_t kind, uint32_t slot) {              // RASHCDI 0x8005BD80
    const uint32_t e = kLdClassLists + 8u * kind;
    if (g.S16(e + 2u) == -1) g.W16(e + 2u, static_cast<uint16_t>(slot));
}

void FamilyRegister(GuestRam& g, uint32_t kind, uint32_t id, uint32_t slot) { // SLUS 0x800303BC
    const uint32_t e = kLdFamilies + 16u * kind;
    const uint32_t p = ((id - g.U32(e + 8u)) << 1) + g.U32(e + 12u);
    if (g.S16(p) < 0) {
        g.W16(p, static_cast<uint16_t>(slot));
        g.W32(e + 4u, g.U32(e + 4u) + 1u);
    }
}

// ============================================================================ the RMD3 handlers
bool RmdHandler(GuestRam& g, uint32_t id, uint32_t chunk, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdRmdFrame;
    for (int32_t k = 0; k < 50; ++k) {                                    // 0x8005CBB8 (a signed-byte counter)
        const uint32_t reg = kLdRegistry + 16u * U(k);
        if (g.U32(reg) != 0u) continue;
        g.W32(reg, id);                                                   // 0x8005CBD8
        const uint8_t gc = g.U8(chunk + 12u);
        g.W8(reg + 4u, gc);                                               // 0x8005CBE4
        uint32_t p = 0;
        if (!Call(c, kLdMalloc, {12u * gc, 0}, F, &p)) return false;
        g.W32(reg + 8u, p);                                               // 0x8005CC0C (the delay slot)
        if (!Call(c, kLdMemset, {p, 0, 4}, F)) return false;
        v0 = k;
        return !g.Faulted();
    }
    v0 = -1;
    return !g.Faulted();
}

bool DodHandler(GuestRam& g, uint32_t id, uint32_t chunk, int32_t gi, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdDodFrame;
    const uint32_t off = U(gi) * 12u;
    for (int32_t k = 0; k < 50; ++k) {
        const uint32_t reg = kLdRegistry + 16u * U(k);
        if (g.U32(reg) != id) continue;
        uint32_t parts = 0;
        if (!Call(c, kLdMalloc, {static_cast<uint32_t>(g.U16(chunk + 24u)) << 2, 0}, F, &parts)) return false;
        g.W32(off + g.U32(reg + 8u) + 4u, parts);                         // 0x8005CCA8
        g.W32(off + g.U32(reg + 8u), chunk);                              // 0x8005CCB8
        g.W32(chunk + 32u, chunk + g.U32(chunk + 32u));                   // 0x8005CCC8
        const uint32_t w36 = g.U32(chunk + 36u), w40 = g.U32(chunk + 40u);
        g.W32(chunk + 36u, chunk + w36);                                  // 0x8005CCDC
        g.W32(chunk + 40u, w40 != 0u ? chunk + w40 : 0u);
        const uint32_t w48 = g.U32(chunk + 48u);
        g.W32(chunk + 48u, w48 != 0u ? chunk + w48 : 0u);
        const uint32_t w44 = g.U32(chunk + 44u);
        g.W32(chunk + 44u, w44 != 0u ? chunk + w44 : 0u);
        v0 = k;
        return !g.Faulted();
    }
    v0 = -1;
    return !g.Faulted();
}

int32_t DpdHandler(GuestRam& g, uint32_t id, uint32_t chunk, int32_t gi, int32_t si) {
    int32_t k = 0;
    for (; k < 50; ++k)
        if (g.U32(kLdRegistry + 16u * U(k)) == id) break;
    if (k == 50) return -1;
    {                                                                     // 0x8005CE5C
        const uint32_t v = g.U32(chunk + 20u);
        g.W32(chunk + 20u, v != 0u ? chunk + v : 0u);
    }
    const uint32_t reg = kLdRegistry + 16u * U(k);
    const uint32_t off = U(gi) * 12u;
    g.W32((static_cast<uint32_t>(g.U8(chunk + 13u)) << 2) + g.U32(off + g.U32(reg + 8u) + 4u), chunk); // 0x8005CDF8
    if (U(gi) + 1u != g.U8(reg + 4u)) return 0;
    if (U(si) + 1u != g.U16(g.U32(off + g.U32(reg + 8u)) + 24u)) return 0;
    g.W8(reg + 6u, 1);                                                    // 0x8005CE34
    return 0;
}

int32_t BbdHandler(GuestRam& g, uint32_t id, uint32_t chunk, int32_t gi) {
    const int32_t k = RegistryFind(g, id);                                // 0x8005CE8C
    if (k == -1) return -1;
    g.W32(U(gi) * 12u + g.U32(kLdRegistry + 16u * U(k) + 8u) + 8u, chunk); // 0x8005CEC8
    return 0;
}

// ============================================================================ RASHCDI 0x8005C0C4
bool ChunkWalk(GuestRam& g, uint32_t out, uint32_t buf, int32_t size, uint32_t first, uint32_t sp, LoaderCallees& c,
               int32_t& v0) {
    const uint32_t F = sp - kLdChunkWalkFrame;
    constexpr uint32_t kRmd3 = 0x33444D52, kDod3 = 0x33444F44, kDpd3 = 0x33445044, kBbd3 = 0x33444242;
    uint32_t s0 = buf, s8 = 0;
    bool s7 = false;
    int32_t s2 = -1, s3 = 0, s4 = 0;
    if (size > 0) {
        do {
            const uint32_t tag = g.U32(s0), len = g.U32(s0 + 4u), id = g.U32(s0 + 8u);
            if (tag == kRmd3) {
                int32_t r = 0;
                if (!RmdHandler(g, id, s0, F, c, r)) return false;
                s4 = r;
                s7 = true;
                if (r == -1) {
                    v0 = -1;
                    return !g.Faulted();
                }
                s8 = id;
                s0 += 16u;
            } else if (tag == kDod3) {
                ++s2;
                s3 = 0;
                if (s7) {
                    int32_t r = 0;
                    if (!DodHandler(g, id, s0, s2, F, c, r)) return false;
                    s0 += len;
                    if (r == -1) {
                        v0 = -1;
                        return !g.Faulted();
                    }
                } else {
                    s0 += len;
                }
            } else if (tag == kDpd3) {
                const int32_t r = DpdHandler(g, id, s0, s2, s3);
                ++s3;
                if (r == -1) {
                    v0 = -1;
                    return !g.Faulted();
                }
                s0 += len;
            } else if (tag == kBbd3) {
                if (BbdHandler(g, id, s0, s2) == -1) {
                    v0 = -1;
                    return !g.Faulted();
                }
                s0 += len;
            } else {
                s0 += len;
            }
            if (g.Faulted()) return false;
        } while (S(s0 - buf) < size);
    }
    g.W32(kLdRegistry + 16u * U(s4) + 12u, first != 0u ? buf : 0u);       // 0x8005C248 / 0x8005C258
    g.W32(out, s8);                                                       // 0x8005C264
    v0 = s4;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8005CA10
bool GeoLoad(GuestRam& g, uint32_t geo, uint32_t tex, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdGeoLoadFrame;
    uint32_t r = 0;
    if (tex != 0u) {                                                      // 0x8005CA30
        if (!Call(c, kLdLoadFile, {tex, g.U32(kLdCdMode), F + 24u, F + 28u, 0}, F, &r)) return false;
        if (S(r) < 0) {
            v0 = -1;
            return !g.Faulted();
        }
        if (!TexFile(g, g.U32(F + 24u), g.S32(F + 28u), F, c)) return false;
        if (!Call(c, kLdFree, {g.U32(F + 24u)}, F)) return false;
    }
    if (geo == 0u) {
        v0 = 0;
        return !g.Faulted();
    }
    if (!Call(c, kLdLoadFile, {geo, g.U32(kLdCdMode), F + 24u, F + 28u, 0}, F, &r)) return false;
    if (S(r) < 0) {
        v0 = -1;
        return !g.Faulted();
    }
    uint32_t s2 = g.U32(F + 24u);
    uint32_t a3 = 1;
    if (g.S32(F + 28u) > 0) {
        do {
            const uint32_t tag = g.U32(s2), s3 = g.U32(s2 + 4u);
            if (tag == 0x33444D52u) {                                     // 0x8005CAE0
                int32_t slot = 0;
                if (!ChunkWalk(g, F + 32u, s2, S(s3), a3, F, c, slot)) return false;
                if (slot < 0) {                                           // 0x8005CAA4
                    if (!Call(c, kLdFree, {g.U32(F + 24u)}, F)) return false;
                    v0 = -1;
                    return !g.Faulted();
                }
                TexKey(g, U(slot));
                const uint32_t reg = kLdRegistry + 16u * U(slot);
                ClassFirst(g, (g.U16(g.U32(g.U32(reg + 8u)) + 14u) & 0x78u) >> 3, U(slot));
                FamilyRegister(g, (g.U16(g.U32(g.U32(reg + 8u)) + 14u) & 0x78u) >> 3, g.U32(F + 32u), U(slot));
                a3 = 0;
            }
            s2 += s3;
            if (g.Faulted()) return false;
        } while (S(s2 - g.U32(F + 24u)) < g.S32(F + 28u));
    }
    v0 = 0;
    return !g.Faulted();
}

// ============================================================================ the texture page table (0x800D5F70)
namespace {
constexpr uint32_t kPages = 0x800D5F70u;   // 34 x 12: +0 key, +1..3 the sheet's place, +6 x, +8 tpage, +10 clut
constexpr uint32_t kTexCfg = 0x800533B4u;  // 88 bytes per player count: the VRAM places
constexpr uint32_t kClutCfg = 0x80053254u; // 176 bytes per player count: the CLUT rows
uint16_t TPage(uint32_t x, uint32_t y) {   // the tpage word the loader builds (bit 7 = 8-bit colour)
    return static_cast<uint16_t>(U(static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(y & 0x100u))) >> 4) |
                                 ((x & 0x3FFu) >> 6) | ((y & 0x200u) << 2));
}
// the CLUT row of CLUT number `n`: x at F+16, y at F+18 (w 128, h 1), and the record's clut word
uint16_t ClutRow(GuestRam& g, uint32_t F, uint32_t players, int32_t n) {
    const uint32_t row = kClutCfg + 176u * (players - 1u);
    const int32_t per = g.U8(row + 116u);
    const int32_t mod = per != 0 ? n % per : n;                           // `div` by zero leaves hi = the dividend
    const int32_t third = static_cast<int32_t>((static_cast<int64_t>(n) * 0x55555556LL) >> 32) - (n >> 31);
    const uint32_t x = g.U16(row + 112u) + U(mod) * g.U8(row + 118u) + 640u;
    g.W16(F + 20u, 128);
    g.W16(F + 22u, 1);
    const uint32_t y = 511u - U(third);
    g.W16(F + 18u, static_cast<uint16_t>(y));
    g.W16(F + 16u, static_cast<uint16_t>(x));
    return static_cast<uint16_t>((y << 6) | ((static_cast<uint16_t>(x) >> 4) & 0x3Fu));
}
} // namespace

// RASHCDI 0x8005DDB8
bool LectSheet(GuestRam& g, uint32_t chunk, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdLectFrame;
    const uint32_t players = g.U32(Gs(g) + 48u);                          // s2
    const uint32_t kind = g.U8(chunk + 12u);
    const uint32_t pixelsAt = chunk + 24u;                                // t0
    const uint32_t cfg = kTexCfg + 88u * (players - 1u);
    const uint16_t key = g.U16(chunk + 16u);
    if (kind >= 1u && kind <= 3u) {                                       // 0x8005DE18: the bikes' and riders' sheets
        const int32_t n = g.S32(kPages + 408u);
        for (int32_t i = 0; i < n; ++i)
            if (g.U8(kPages + 24u + 12u * U(i)) == key) return !g.Faulted();
        if (!(g.S32(kPages + 408u) < g.S32(0x8005AE08u + 4u * (players - 1u)))) return !g.Faulted();
        const uint32_t cnt = g.U32(kPages + 408u);
        const uint32_t s1 = kPages + 24u + 12u * cnt;
        if (!Call(c, kLdReadTim, {F + 24u, pixelsAt}, F)) return false;
        const uint32_t a3 = cnt & 3u;
        g.W16(F + 16u, static_cast<uint16_t>(((g.U8(cfg + 58u) & 0xFu) << 6) + ((cnt << 4) & 0x3C0u)));
        g.W16(F + 20u, 64);
        g.W16(F + 22u, 60);
        const uint32_t a0 = ((a3 << 4) - a3) << 2;                        // 60 per quarter
        g.W16(F + 18u, static_cast<uint16_t>(g.U8(cfg + 57u) + ((g.U8(cfg + 58u) & 0x10u) << 4) + a0));
        g.W8(s1, g.U8(chunk + 16u));
        g.W8(s1 + 1u, static_cast<uint8_t>(a3));
        g.W8(s1 + 2u, 0);
        g.W8(s1 + 3u, static_cast<uint8_t>(a0));
        g.W16(s1 + 8u, static_cast<uint16_t>(TPage(g.U16(F + 16u), g.U16(F + 18u)) | 0x80u));
        g.W16(s1 + 6u, g.U16(F + 16u));
        if (!Call(c, kLdLoadImage, {F + 16u, g.U32(F + 40u)}, F)) return false;
        g.W32(kPages + 408u, g.U32(kPages + 408u) + 1u);
        const uint32_t k = g.U8(s1);
        const int32_t clut = k == 116u ? 47 : k == 149u ? 46 : k == 151u ? 41 : g.S32(kPages + 424u);
        g.W16(s1 + 10u, ClutRow(g, F, players, clut));
        if ((U(clut) - 46u) < 2u || clut == 41)
            if (!Call(c, kLdLoadImage, {F + 16u, g.U32(F + 32u)}, F)) return false;
    } else if (kind == 4u) {                                              // 0x8005E090: the pedestrians' sheets
        const int32_t n = g.S32(kPages + 412u);
        for (int32_t i = 0; i < n; ++i)
            if (g.U8(kPages + 312u + 12u * U(i)) == key) return !g.Faulted();
        if (!(g.S32(kPages + 412u) < g.S32(0x8005AE10u + 4u * (players - 1u)))) return !g.Faulted();
        const uint32_t cnt = g.U32(kPages + 412u);
        const uint32_t s1 = kPages + 312u + 12u * cnt;
        if (!Call(c, kLdReadTim, {F + 48u, pixelsAt}, F)) return false;
        const uint32_t a3 = cnt & 7u, odd = cnt & 1u;
        g.W16(F + 16u, static_cast<uint16_t>(((g.U8(cfg + 46u) & 0xFu) << 6) + ((cnt << 3) & 0x3C0u) + (odd << 5)));
        g.W16(F + 20u, 32);
        g.W16(F + 22u, 64);
        const uint32_t a1 = (a3 >> 1) << 6;
        g.W16(F + 18u, static_cast<uint16_t>(g.U8(cfg + 45u) + ((g.U8(cfg + 46u) & 0x10u) << 4) + a1));
        g.W8(s1, g.U8(chunk + 16u));
        g.W8(s1 + 1u, static_cast<uint8_t>(a3));
        g.W8(s1 + 2u, static_cast<uint8_t>(odd << 6));
        g.W8(s1 + 3u, static_cast<uint8_t>(a1));
        g.W16(s1 + 8u, static_cast<uint16_t>(TPage(g.U16(F + 16u), g.U16(F + 18u)) | 0x80u));
        g.W16(s1 + 6u, g.U16(F + 16u));
        if (!Call(c, kLdLoadImage, {F + 16u, g.U32(F + 64u)}, F)) return false;
        g.W32(kPages + 412u, g.U32(kPages + 412u) + 1u);
        g.W16(s1 + 10u, ClutRow(g, F, players, g.S32(kPages + 424u)));
        if (!Call(c, kLdLoadImage, {F + 16u, g.U32(F + 56u)}, F)) return false;
        g.W32(kPages + 424u, g.U32(kPages + 424u) + 1u);
    } else if (kind == 5u || kind == 6u) {                                // 0x8005E2C4 / 0x8005E3C4: the two big sheets
        const bool five = kind == 5u;
        const uint32_t o = five ? 68u : 16u;                              // the config bytes (x/4, y, flags)
        const uint16_t x = static_cast<uint16_t>((g.U8(cfg + o + 2u) & 0xFu) << 6);
        g.W16(F + 16u, x);
        g.W16(F + 20u, 64);
        g.W16(F + 22u, five ? 32 : 192);
        const uint16_t y = static_cast<uint16_t>(g.U8(cfg + o + 1u) + ((g.U8(cfg + o + 2u) & 0x10u) << 4));
        g.W16(F + 18u, y);
        const uint32_t s1 = five ? kPages : kPages + 12u;
        g.W8(s1, g.U8(chunk + 16u));
        g.W16(s1 + 8u, static_cast<uint16_t>(U(static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(y & 0x100u))) >> 4) |
                                             (x >> 6) | ((y & 0x200u) << 2)));
        g.W8(s1 + 1u, 0);
        g.W8(s1 + 2u, static_cast<uint8_t>(g.U8(cfg + o) << 2));
        g.W16(s1 + 6u, x);
        g.W8(s1 + 3u, g.U8(cfg + o + 1u));
        const uint32_t v = ((g.U8(s1 + 3u) + ((g.U8(cfg + o + 2u) & 0x10u) << 4) + (five ? 31u : 191u)) & 0x1FFu) << 4;
        g.W16(s1 + 4u, static_cast<uint16_t>((g.U16(s1 + 4u) & 0xE004u) | v | 0xEu));
        if (!Call(c, kLdLoadImage, {F + 16u, pixelsAt}, F)) return false;
        g.W32(five ? kPages + 416u : kPages + 420u, 1);
    }
    if (!Call(c, kLdDrawSync, {0}, F)) return false;
    return !g.Faulted();
}

// RASHCDI 0x8005DA38
bool KnbpClut(GuestRam& g, uint32_t chunk, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdKnbpFrame;
    g.W16(F + 20u, g.U16(chunk + 12u));
    const uint16_t h = g.U16(chunk + 14u);
    g.W16(F + 22u, h);
    const uint32_t row = kClutCfg + 176u * (g.U32(Gs(g) + 48u) - 1u);
    g.W16(F + 18u, static_cast<uint16_t>(512u - h));
    g.W16(F + 16u, static_cast<uint16_t>(g.U16(row + 112u) + 640u));
    if (!Call(c, kLdLoadImage, {F + 16u, chunk + 20u}, F)) return false;
    g.W32(kPages + 424u, U(static_cast<int32_t>(g.S16(chunk + 8u))));
    return !g.Faulted();
}

// RASHCDI 0x8005DAD0
bool ClutUpload(GuestRam& g, uint32_t src, int32_t n, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdClutUploadFrame;
    if (n == -1) {
        n = g.S32(kPages + 424u);
        g.W32(kPages + 424u, U(n + 1));
    }
    ClutRow(g, F, g.U32(Gs(g) + 48u), n);                                 // the rect at F+16 (its clut word unused)
    if (!Call(c, kLdLoadImage, {F + 16u, src}, F)) return false;
    v0 = n;
    return !g.Faulted();
}

// RASHCDI 0x8005DBB8
bool TslpCluts(GuestRam& g, uint32_t chunk, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdTslpFrame;
    uint32_t s3 = g.U32(kPages + 424u);
    constexpr uint32_t kRec = 0x8006B898u;                                // RASHCDI data: 4 bytes per player
    g.W8(kRec + 7u, 0xFF);
    g.W8(kRec + 3u, 0xFF);
    const uint32_t a3 = chunk + 20u;
    int32_t r = 0;
    for (uint32_t p = 0; p < g.U32(Gs(g) + 48u); ++p) {
        const int32_t bike = g.S32(Gs(g) + 72u + 4u * p);
        uint32_t kind = 0;
        if (!(bike < 9)) kind = (U(bike) - 9u) < 9u ? 1u : 2u;           // 0x8005DC44
        g.W8(kRec + 4u * p, static_cast<uint8_t>(kind));
        if (kind != 2u) {
            const int32_t s2 = g.U32(Gs(g) + 48u) == 2u ? S(p) : g.S8(0x800D81D8u + 36u * p + 10u);
            const uint32_t v = kind == 1u ? U(s2 * 6 + 3) << 8 : U(s2 * 3) << 9;
            uint32_t s1 = a3 + v;
            for (int k = 0; k < 3; ++k, s1 += 256u, ++s3)
                if (!ClutUpload(g, s1, S(s3), F, c, r)) return false;
            g.W8(kRec + 4u * p, static_cast<uint8_t>(kind));
            g.W8(kRec + 4u * p + 1u, static_cast<uint8_t>(s2));
            g.W8(kRec + 4u * p + 2u, 3);
            g.W8(kRec + 4u * p + 3u, static_cast<uint8_t>(s3 - 3u));
        }
        g.W32(kPages + 424u, s3);                                         // 0x8005DD24
    }
    uint32_t s1 = a3 + 4608u;
    for (int32_t n = 43; n < 46; ++n, s1 += 256u)
        if (!ClutUpload(g, s1, n, F, c, r)) return false;
    return !g.Faulted();
}

// RASHCDI 0x8005E6CC
bool RimTim(GuestRam& g, uint32_t name, uint32_t players, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdRimTimFrame;
    const uint32_t n = g.U32(kPages + 408u);
    const uint32_t s0 = kPages + 24u + 12u * n;
    uint32_t r = 0;
    if (!Call(c, kLdTimFile, {name, F + 48u, F + 16u}, F, &r)) return false;
    if (r != 0u) {
        v0 = -1;
        return !g.Faulted();
    }
    const uint32_t cfg = kTexCfg + 88u * (players - 1u);
    const uint32_t cnt = g.U32(kPages + 408u);
    g.W16(F + 40u, static_cast<uint16_t>(((g.U8(cfg + 58u) & 0xFu) << 6) + ((cnt << 4) & 0x3C0u)));
    const uint32_t a2 = cnt & 3u, a1 = ((a2 << 4) - a2) << 2;
    g.W16(F + 42u, static_cast<uint16_t>(g.U8(cfg + 57u) + ((g.U8(cfg + 58u) & 0x10u) << 4) + a1));
    const uint32_t rect = g.U32(F + 28u);
    g.W16(F + 44u, g.U16(rect + 4u));
    g.W16(F + 46u, g.U16(rect + 6u));
    g.W8(s0, 0xFF);
    g.W8(s0 + 1u, static_cast<uint8_t>(a2));
    g.W8(s0 + 2u, 0);
    g.W8(s0 + 3u, static_cast<uint8_t>(a1));
    g.W16(s0 + 8u, static_cast<uint16_t>(TPage(g.U16(F + 40u), g.U16(F + 42u)) | 0x80u));
    g.W16(s0 + 6u, g.U16(F + 40u));
    if (!Call(c, kLdLoadImage, {F + 40u, g.U32(F + 32u)}, F)) return false;
    g.W32(0x8005B348u, s0);                                               // the rim's record
    g.W32(kPages + 408u, g.U32(kPages + 408u) + 1u);
    g.W16(s0 + 10u, 0xFFFF);
    if (!Call(c, kLdFree, {g.U32(F + 48u)}, F)) return false;
    v0 = 0;
    (void)n;
    return !g.Faulted();
}

// RASHCDI 0x8005E848
bool TexTablesInit(GuestRam& g, uint32_t players, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdTexTablesFrame;
    for (uint32_t o : {408u, 412u, 416u, 420u, 424u}) g.W32(kPages + o, 0);
    g.W32(kLdPageTablePtr, kPages);                                       // 0x8005E894 (the delay slot)
    for (int32_t i = 0; i < g.S32(0x8005AE08u + 4u * (players - 1u)); ++i) g.W8(kPages + 24u + 12u * U(i), 0xFF);
    for (int32_t i = 0; i < g.S32(0x8005AE10u + 4u * (players - 1u)); ++i) g.W8(kPages + 312u + 12u * U(i), 0xFF);
    g.W8(kPages, 0xFF);
    g.W8(kPages + 12u, 0xFF);
    int32_t r = 0;
    return RimTim(g, 0x8005B6D0u, players, F, c, r);                      // "DATA\RimA1.tim"
}

// RASHCDI 0x8005E4E8
bool GtpLoad(GuestRam& g, uint32_t players, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdGtpFrame;
    uint32_t r = 0;
    if (!Call(c, kLdLoadFile, {0x8005B6BCu, g.U32(kLdCdMode), F + 32u, F + 36u, 0}, F, &r)) return false;
    if (S(r) < 0) {
        v0 = -1;
        return !g.Faulted();
    }
    const uint32_t cfg = kTexCfg + 88u * (players - 1u);
    const uint16_t x = static_cast<uint16_t>((g.U8(cfg + 34u) & 0xFu) << 6);
    g.W16(F + 24u, x);
    g.W16(F + 28u, 64);
    g.W16(F + 30u, 128);
    const uint32_t y = ((g.U8(cfg + 34u) & 0x10u) << 4) + g.U8(cfg + 33u);
    g.W32(0x800D6168u, y);
    g.W16(F + 26u, static_cast<uint16_t>(y));
    if (!Call(c, kLdLoadImage, {F + 24u, g.U32(F + 32u)}, F)) return false;
    if (!Call(c, kLdFree, {g.U32(F + 32u)}, F)) return false;
    g.W16(0x800D6160u, static_cast<uint16_t>(TPage(g.U16(F + 24u), g.U16(F + 26u))));
    v0 = 0;
    return !g.Faulted();
}

// RASHCDI 0x8005D9EC
bool TexSetUp(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdTexSetUpFrame;
    if (!TexTablesInit(g, g.U32(Gs(g) + 48u), F, c)) return false;
    int32_t r = 0;
    return GtpLoad(g, g.U32(Gs(g) + 48u), F, c, r);
}

// ============================================================================ RASHCDI 0x8005C920 / 0x8005BDAC
void CtkpList(GuestRam& g, uint32_t chunk) {
    const int32_t n = g.S32(chunk + 8u);
    uint32_t e = chunk + 12u;
    for (int32_t i = 0; i < n; ++i, e += 8u) {
        const uint32_t kind = U(static_cast<int32_t>(g.S16(e)));
        g.W16(U(i) * 4u + g.U32(kLdClassLists + (kind << 3) + 4u), g.U16(e + 4u));
        g.W16(U(i) * 4u + g.U32(kLdClassLists + (U(static_cast<int32_t>(g.S16(e))) << 3) + 4u) + 2u, g.U16(e + 6u));
        const uint32_t cnt = kLdClassLists + (U(static_cast<int32_t>(g.S16(e))) << 3);
        g.W16(cnt, static_cast<uint16_t>(g.U16(cnt) + 1u));
        if (g.Faulted()) return;
    }
}

bool TexFile(GuestRam& g, uint32_t buf, int32_t size, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdTexFileFrame;
    if (size <= 0) return !g.Faulted();
    uint32_t s0 = buf;
    do {
        const uint32_t tag = g.U32(s0), len = g.U32(s0 + 4u);
        if (tag == 0x504B5443u) CtkpList(g, s0);                         // "CTKP"
        else if (tag == 0x5443454Cu) { if (!LectSheet(g, s0, F, c)) return false; }      // "LECT"
        else if (tag == 0x50424E4Bu) { if (!KnbpClut(g, s0, F, c)) return false; }       // "KNBP"
        else if (tag == 0x504C5354u) { if (!TslpCluts(g, s0, F, c)) return false; }      // "TSLP"
        s0 += len;
        if (g.Faulted()) return false;
    } while (S(s0 - buf) < size);
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8005C30C
bool RigLoad(GuestRam& g, uint32_t name, uint32_t lenOut, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdRigLoadFrame;
    const uint32_t s0 = g.U32(g.U32(kLdResListPtr) + 2616u);             // the stream buffers' block
    uint32_t r = 0;
    if (!Call(c, kLdReadInto, {name, g.U32(kLdCdMode), s0, lenOut}, F, &r)) return false;
    if (S(r) < 0) {
        v0 = -1;
        return !g.Faulted();
    }
    int32_t slot = 0;
    if (g.U32(s0) == 0x5443454Cu) {                                       // a leading LECT: the texture first
        const uint32_t s2 = g.U32(lenOut) - g.U32(s0 + 4u);
        if (!LectSheet(g, s0, F, c)) return false;
        uint32_t blk = 0;
        if (!Call(c, kLdMalloc, {s2, 0}, F, &blk)) return false;
        if (blk == 0u) {
            v0 = -1;
            return !g.Faulted();
        }
        if (!Call(c, kLdMemcpy, {blk, s0 + g.U32(s0 + 4u), s2}, F)) return false;
        if (!ChunkWalk(g, F + 16u, blk, S(s2), 1, F, c, slot)) return false;
        if (slot == -1) {
            v0 = -1;
            return !g.Faulted();
        }
    } else {
        if (!ChunkWalk(g, F + 16u, s0, g.S32(lenOut), 1, F, c, slot)) return false;
    }
    TexKey(g, U(slot));
    const uint32_t reg = kLdRegistry + 16u * U(slot);
    ClassFirst(g, (g.U16(g.U32(g.U32(reg + 8u)) + 14u) & 0x78u) >> 3, U(slot));
    FamilyRegister(g, (g.U16(g.U32(g.U32(reg + 8u)) + 14u) & 0x78u) >> 3, g.U32(F + 16u), U(slot));
    v0 = slot;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80064034
uint32_t BikeName(uint32_t table, uint32_t idx) {
    if (table == 0u) return idx * 9u + 0x8006B51Cu;
    if (table == 1u) return idx * 9u + 0x8006B5DCu;
    return 0;
}

// ============================================================================ RASHCDI 0x8005C45C
bool LoadBikeBank(GuestRam& g, int32_t bank, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdBikeBankFrame;
    const uint32_t gs = Gs(g);
    const int32_t players = g.S32(gs + 48u);                              // s5
    int32_t s1 = bank < 3 ? bank : 2;
    if (g.U8(gs + 4u) == 44u) s1 = g.S32(gs + 72u) < 9 ? 3 : 4;           // 0x8005C4A0: Jailbreak's two banks
    const uint32_t nameAt = g.U32(0x8006B4A4u + (U(s1) << 2));
    if (!Call(c, kLdSprintf, {F + 24u, 0x8005B628u, 0x80052450u, nameAt, 0x8005B630u}, F)) return false;
    if (!Call(c, kLdSprintf, {F + 128u, 0x8005B628u, 0x80052450u, g.U32(0x8006B4A4u + (U(s1) << 2)), 0x8005B638u}, F))
        return false;
    int32_t s2 = 0;
    if (!GeoLoad(g, F + 24u, F + 128u, F, c, s2)) return false;
    if (s2 != 0) {
        v0 = s2;
        return !g.Faulted();
    }
    for (int32_t p = 0; p < players; ++p) {                               // 0x8005C55C
        const uint32_t idx = g.U32(Gs(g) + 72u + (U(p) << 2));
        if (!((idx - 6u) < 3u || (idx - 15u) < 3u)) continue;
        if (RegistryFind(g, idx + 100u) != -1) continue;
        const uint32_t nm = BikeName(0, g.U32(Gs(g) + 72u + (U(p) << 2)));
        if (!Call(c, kLdSprintf, {F + 24u, 0x8005B640u, 0x8005243Cu, nm, 0x8005B648u}, F)) return false;
        if (!RigLoad(g, F + 24u, F + 232u, F, c, s2)) return false;
    }
    v0 = 0;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8005C7F0
bool HazardModels(GuestRam& g, int32_t set, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdHazardModelsFrame;
    int32_t t0 = set;
    if ((g.U32(Gs(g) + 4u) & 0x18u) == 8u) t0 = 0;                      // 0x8005C82C
    const int32_t tens = static_cast<int32_t>((static_cast<int64_t>(t0) * 0x66666667LL) >> 34) - (t0 >> 31);
    g.W8(F + 25u, 0);
    g.W8(F + 24u, static_cast<uint8_t>(t0 - tens * 10 + 48));
    if (!Call(c, kLdSprintf, {F + 32u, 0x8005B690u, 0x80052450u, 0x8005B69Cu, F + 24u, 0x8005B630u}, F)) return false;
    if (!Call(c, kLdSprintf, {F + 136u, 0x8005B690u, 0x80052450u, 0x8005B69Cu, F + 24u, 0x8005B638u}, F)) return false;
    int32_t r = 0;
    if (!GeoLoad(g, F + 32u, F + 136u, F, c, r)) return false;
    v0 = r == 0 ? 1 : 0;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8005CEE0
bool RenderCamInit(GuestRam& g, uint32_t view, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdRenderCamInitFrame;
    const uint32_t p = 0x8005AEC0u + (view << 2);
    g.W32(p, 0x800D82B0u);                                                // 0x8005CF00
    g.W32(0x8005AEC4u, 0x800D8330u);                                      // 0x8005CF08
    g.W32(g.U32(p), 0);
    g.W32(g.U32(p) + 4u, 237);
    g.W32(g.U32(p) + 8u, 40);
    g.W32(g.U32(p) + 12u, 32767);
    {
        const uint32_t r = g.U32(p);
        g.W32(r + 52u, 64);
        g.W32(r + 48u, 64);
        g.W32(r + 44u, 64);
    }
    for (uint32_t o : {94u, 96u, 98u, 102u, 104u, 106u}) g.W16(g.U32(p) + o, 0);
    for (uint32_t o : {92u, 100u, 108u}) g.W16(g.U32(p) + o, 4096);
    for (uint32_t o : {112u, 116u, 120u}) g.W32(g.U32(p) + o, 0);
    if (!Call(c, kLdSetGeomScreen, {g.U32(g.U32(p) + 4u)}, F)) return false;
    if (!Call(c, kLdSetGeomOffset, {384, 240}, F)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8005C630
bool CarModels(GuestRam& g, int32_t raceId, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdCarModelsFrame;
    uint32_t gs = Gs(g);
    const uint32_t players = g.U32(gs + 48u);                             // s2
    const int32_t tens = static_cast<int32_t>((static_cast<int64_t>(raceId) * 0x66666667LL) >> 34) - (raceId >> 31);
    g.W8(F + 34u, 0);                                                     // 0x8005C668
    g.W8(F + 32u, static_cast<uint8_t>(tens + 48));
    g.W8(F + 33u, static_cast<uint8_t>(raceId - tens * 10 + 48));
    uint32_t s4 = 0, r = 0;
    int32_t gv = 0;
    if ((g.U32(gs + 4u) & 0x18u) == 0u) {                                 // 0x8005C6A4: the pedestrians
        if (!Call(c, kLdSprintf, {F + 40u, 0x8005B654u, 0x80052450u, 0x8005B65Cu}, F)) return false;
        if (!GeoLoad(g, F + 40u, 0, F, c, gv)) return false;
        s4 = gv == 0 ? 1u : 0u;
    }
    gs = Gs(g);
    if ((g.U32(gs + 4u) & 0x18u) == 8u && g.U8(gs + 4u) != 44u) {        // 0x8005C6F4: carSC
        if (!Call(c, kLdSprintf, {F + 40u, 0x8005B654u, 0x80052450u, 0x8005B668u}, F)) return false;
        if (!Call(c, kLdSprintf, {F + 144u, 0x8005B654u, 0x80052450u, 0x8005B674u}, F)) return false;
        if (!GeoLoad(g, F + 40u, F + 144u, F, c, gv)) return false;
        v0 = S(s4);
        return !g.Faulted();
    }
    r = g.U32(0x8006B4B8u + ((players - 1u) << 2));                       // 0x8005C790
    if (!Call(c, kLdSprintf, {F + 40u, 0x8005B680u, 0x80052450u, 0x8005B68Cu, F + 32u, r, 0x8005B630u}, F)) return false;
    if (!GeoLoad(g, F + 40u, 0, F, c, gv)) return false;
    if (gv == 0) ++s4;
    v0 = S(s4);
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80068D54
bool PopulationReset(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdPopResetFrame;
    for (uint32_t k = 0; k < 16u; ++k) {                                  // 0x80068D80: pool 3, +4 kept
        const uint32_t ctl = 0x800CF650u + 512u * k;
        const uint32_t keep = g.U32(ctl + 20u);
        if (!Call(c, kLdMemset, {ctl + 16u, 0, 512}, F)) return false;
        g.W32(ctl + 20u, keep);
    }
    g.W32(0x800CF650u, 0);
    g.W32(0x800CF654u, 0);
    g.W32(0x800CF658u, 0xFFFFFFFFu);
    {
        const uint32_t gs = Gs(g);
        for (uint32_t p = 0; S(p) < g.S32(gs + 48u); ++p) {               // 0x80068DEC
            g.W32(0x800D8710u + 4u + 4u * p, 16);
            g.W32(0x800D8710u + 12u + 4u * p, 4);
        }
    }
    g.W32(0x800D8710u + 24u, 4);
    g.W32(0x800D8710u + 20u, 16);
    g.W32(0x800D8710u + 28u, 180);
    g.W32(0x800D8710u + 32u, 145);
    g.W16(0x800D8710u + 36u, 100);
    g.W16(0x800D8710u + 38u, 75);
    g.W16(0x800D8710u + 40u, 50);
    if (g.U32(0x800D4B80u) != 0u) {                                       // 0x80068E50: pool 4's four records
        for (uint32_t k = 0; k < 4u; ++k) {
            const uint32_t rec = g.U32(0x800D4B80u) + 572u * k;
            const uint32_t keep = g.U32(rec + 4u);
            if (!Call(c, kLdMemset, {rec, 0, 572}, F)) return false;
            g.W32(g.U32(0x800D4B80u) + 572u * k + 4u, keep);
        }
    }
    g.W32(0x800D4B70u, 0);
    g.W32(0x800D4B74u, 0);
    g.W32(0x800D4B78u, 0xFFFFFFFFu);
    g.W32(0x800D8748u, 10);
    g.W32(0x800D8750u, 10);
    g.W32(0x800D8754u, 150);
    g.W32(0x800D8758u, 300);
    g.W32(0x800D8744u, 4);
    g.W32(0x800D874Cu, 4);
    g.W32(0x800D875Cu, 5);
    g.W32(0x800CD6D4u, 0x800D1818u);                                      // 0x80068F00
    g.W32(0x800CD6C8u, 0);
    g.W32(0x800CD6CCu, 0);
    g.W32(0x800CD6D0u, 0xFFFFFFFFu);
    g.W32(0x800CE5A4u, 0x800D36ECu);                                      // 0x80068F24
    g.W32(0x800CE598u, 0);
    g.W32(0x800CE59Cu, 0);
    g.W32(0x800CE5A0u, 0xFFFFFFFFu);
    for (uint32_t k = 0; S(k) < g.S32(0x8005B214u); ++k)                  // 0x80068F48
        if (!Call(c, kLdMemset, {g.U32(0x800CD6C4u) + 280u * k, 0, 280}, F)) return false;
    g.W32(0x800CD6A8u, 0);
    g.W32(0x800CD6ACu, 0);
    g.W32(0x800CD6B0u, 0xFFFFFFFFu);
    g.W32(0x800D1814u, 8344);
    g.W32(0x8005B1FCu, 0);
    g.W32(0x8005B1F8u, 0);
    g.W32(0x8005B218u, 0);
    g.W32(0x8005AD38u, 0xFFFFFFFFu);
    g.W32(0x800D1660u, 0);
    g.W32(0x8005AD3Cu, 0xFFFFFFFFu);
    if (!Call(c, kLdMemset, {0x8005B2B8u, 0, 4}, F)) return false;
    g.W8(0x8005AD32u, 0);                                                 // 0x80068FF4
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80061FAC
bool EffectSheet(GuestRam& g, uint32_t /*a0*/, uint32_t table, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdEffectSheetFrame;
    const uint32_t players = g.U32(Gs(g) + 48u);
    const uint32_t row = (players - 1u) * 11u;                            // sp+120
    const uint32_t s5 = row * 8u + 0x800533B4u;                           // 88 bytes per player count
    uint32_t s8 = table, s4 = 0;
    for (uint32_t i = 0; i < 11u; ++i, s8 += 4u) {                        // 0x80062030
        g.W32(s8, g.U32(s8) + table);                                     // 0x80062044
        const uint32_t s2 = kLdFxSprites + 28u * i;
        if (!Call(c, kLdReadTim, {F + 24u, g.U32(s8)}, F)) return false;
        const uint32_t e = 0x8006B4C0u + 4u * i;
        const uint32_t cfgLo = g.U8(s5 + 54u), cfgY = g.U8(s5 + 53u);
        g.W16(F + 16u, static_cast<uint16_t>(g.U8(e) + ((cfgLo & 0xFu) << 6)));
        g.W16(F + 18u, static_cast<uint16_t>(g.U8(e + 1u) + cfgY + ((g.U8(s5 + 54u) & 0x10u) << 4)));
        const uint32_t rect = g.U32(F + 36u);
        g.W16(F + 20u, g.U16(rect + 4u));
        g.W16(F + 22u, g.U16(rect + 6u));
        g.W32(s2, static_cast<uint32_t>(g.U8(e)) << 2);                   // 0x800620C8
        g.W32(s2 + 4u, ((g.U8(s5 + 54u) & 0x10u) << 4) + g.U8(s5 + 53u) + g.U8(e + 1u));
        g.W32(s2 + 8u, U(static_cast<int32_t>(g.S16(F + 20u)) << 2));
        g.W32(s2 + 12u, U(static_cast<int32_t>(g.S16(g.U32(F + 36u) + 6u))));
        if (!Call(c, kLdLoadImage, {F + 16u, g.U32(F + 40u)}, F)) return false;
        {                                                                 // 0x8006211C
            const uint32_t y = g.U16(F + 18u), x = g.U16(F + 16u);
            uint32_t tp = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((y & 0x100u))) >> 4);
            tp |= (x & 0x3FFu) >> 6;
            if (i == 0u) tp |= 0x40u;
            tp |= (y & 0x200u) << 2;
            g.W16(s2 + 16u, static_cast<uint16_t>(tp));
        }
        const uint16_t cl = g.U16(e + 2u);
        g.W16(s2 + 18u, cl);                                              // 0x80062194
        for (uint32_t k = 0; S(k) < static_cast<int32_t>(g.U16(s2 + 18u)); ++k) {   // 0x800621BC
            const uint32_t s0 = table + (s4 << 2);
            const uint32_t a2 = 0x80053254u + row * 16u;                  // 176 bytes per player count
            g.W32(s0 + 88u, g.U32(s0 + 88u) + table);
            const uint32_t per = g.U8(a2 + 108u);
            const uint32_t mod = per != 0u ? U(S(s4) % S(per)) : s4;       // `div` by zero leaves hi = the dividend
            const uint32_t quo = per != 0u ? U(S(s4) / S(per)) : 0xFFFFFFFFu;
            g.W16(F + 16u, static_cast<uint16_t>(((g.U8(s5 + 54u) & 0xFu) << 6) + g.U16(a2 + 104u) + mod * g.U8(a2 + 110u)));
            g.W16(F + 20u, 16);
            g.W16(F + 22u, 1);
            ++s4;
            g.W16(F + 18u, static_cast<uint16_t>(((g.U8(s5 + 54u) & 0x10u) << 4) + g.U16(a2 + 106u) + quo));
            if (!Call(c, kLdLoadImage, {F + 16u, g.U32(s0 + 88u)}, F)) return false;
            g.W16(s2 + 20u + 2u * k, static_cast<uint16_t>((g.U16(F + 18u) << 6) | ((g.U16(F + 16u) >> 4) & 0x3Fu)));
        }
    }
    {                                                                     // 0x800622BC: the 32 x 1 row of zeroes
        const uint32_t cfg = 0x800533B4u + 88u * (g.U32(Gs(g) + 48u) - 1u);
        g.W16(0x8005B398u, static_cast<uint16_t>(((g.U8(cfg + 54u) & 15u) << 6) + 12u));
        g.W16(0x8005B39Cu, 32);
        g.W16(0x8005B39Eu, 1);
        g.W16(0x8005B39Au, static_cast<uint16_t>(g.U8(cfg + 53u) + ((g.U8(cfg + 54u) & 0x10u) << 4) + 120u));
        for (uint32_t k = 0; k < 16u; ++k) g.W32(F + 108u - 4u * k, 0);
        if (!Call(c, kLdLoadImage, {0x8005B398u, F + 48u}, F)) return false;
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x800627F8
bool SoundLoad(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdSoundLoadFrame;
    const uint32_t gpv = g.gp();
    const int32_t s4 = Div9(g.S32(Gs(g) + 72u));                          // the player's bike / 9
    uint32_t s5 = 6, s6 = 0, v0 = 0;
    if (!Call(c, 0x8001F054u, {0, 0}, F)) return false;
    if (!Call(c, 0x8001F080u, {}, F)) return false;
    if (!Call(c, 0x8001F0D4u, {}, F)) return false;
    if (!Call(c, 0x8001FBD4u, {}, F)) return false;
    const uint32_t kListener = gpv + 1920u, kEngine = gpv + 1952u, kSlots0 = gpv + 1924u, kSlots1 = gpv + 1928u;
    if (g.U32(kListener) == 0u) {                                         // 0x80062874
        const uint32_t n = g.U32(Gs(g) + 48u) * 72u;
        if (!Call(c, kLdMalloc, {n, 0}, F, &v0)) return false;
        g.W32(kListener, v0);
        if (!Call(c, kLdMemset, {v0, 0, g.U32(Gs(g) + 48u) * 72u}, F)) return false;
    }
    if (g.U32(kEngine) == 0u) {                                           // 0x800628CC
        const uint32_t n = g.U32(Gs(g) + 48u) * 132u;
        if (!Call(c, kLdMalloc, {n, 0}, F, &v0)) return false;
        g.W32(kEngine, v0);
        if (!Call(c, kLdMemset, {v0, 0, g.U32(Gs(g) + 48u) * 132u}, F)) return false;
    }
    if (g.U32(kSlots0) == 0u) {                                           // 0x80062924
        if (!Call(c, kLdMalloc, {352, 0}, F, &v0)) return false;
        g.W32(kSlots0, v0);
        if (!Call(c, kLdMemset, {v0, 0, 352}, F)) return false;
    }
    uint32_t which = 0;
    {
        const uint32_t gs = Gs(g);
        if (g.U32(gs + 48u) == 1u) {                                      // 0x8006295C
            const uint32_t l = g.U32(kListener);
            g.W32(l + 20u, 64);
            g.W32(l + 36u, 5);
            g.W32(l + 40u, 3);
            if (g.U8(gs + 4u) == 33u) {
                g.W32(l + 36u, g.U32(l + 36u) - 1u);
                g.W32(l + 40u, g.U32(l + 40u) + 1u);
            }
            which = 0;
        } else {
            if (g.U32(kSlots1) == 0u) {                                   // 0x800629B8
                if (!Call(c, kLdMalloc, {352, 0}, F, &v0)) return false;
                g.W32(kSlots1, v0);
                if (!Call(c, kLdMemset, {v0, 0, 352}, F)) return false;
            }
            const uint32_t l = g.U32(kListener);
            g.W32(l + 92u, 127);
            g.W32(l + 20u, 0);
            g.W32(l + 36u, 3);
            g.W32(l + 40u, 2);
            g.W32(l + 108u, 3);
            g.W32(l + 112u, 2);
            if (!Call(c, 0x80063448u, {0}, F)) return false;
            which = 1;
        }
    }
    if (!Call(c, 0x80063448u, {which}, F)) return false;                  // SoundRecordsInit
    uint32_t h = 0;
    if (!Call(c, 0x8001458Cu, {0x8005B790u, g.U32(kLdCdMode)}, F, &h)) return false;   // "DATA\RASHNZ_E.DAT"
    if (S(h) < 0) return !g.Faulted();
    const uint32_t dir = F + 16u;                                         // 8 x {offset, bank bytes, sample bytes}
    auto bankBytes = [&](int32_t k) { return g.U32(dir + 12u * U(k) + 4u); };
    if (!Call(c, 0x80014780u, {h, dir, 96, 0}, F)) return false;
    if (!Call(c, kLdMalloc, {g.U32(Gs(g) + 48u) == 1u ? g.U32(F + 20u) : g.U32(F + 32u), 0}, F, &v0)) return false;
    g.W32(gpv + 1900u, v0);                                               // 0x80062A84
    if (!Call(c, kLdMalloc, {g.U32(F + 44u), 0}, F, &v0)) return false;
    g.W32(gpv + 1932u, v0);                                               // 0x80062ABC (the delay slot)
    if (!Call(c, kLdMalloc, {bankBytes(s4 + 3), 0}, F, &v0)) return false;
    g.W32(g.U32(kEngine) + 12u, v0);                                      // 0x80062AD0
    if (g.U32(Gs(g) + 48u) == 2u) {                                       // 0x80062ADC
        s6 = U(Div9(g.S32(Gs(g) + 76u)));
        if (!Call(c, kLdMalloc, {bankBytes(S(s6) + 3), 0}, F, &v0)) return false;
        g.W32(g.U32(kEngine) + 144u, v0);
    }
    if (!Call(c, 0x80043F00u, {0xF2000002u}, F, &v0)) return false;      // GetRCnt(root counter 2)
    s5 += (v0 & 0xFFu) >> 7;
    if (g.U8(Gs(g) + 4u) & 1u) {                                          // 0x80062B50
        if (!Call(c, kLdMalloc, {bankBytes(S(s5)), 0}, F, &v0)) return false;
        g.W32(gpv + 1892u, v0);
    }
    uint32_t tmp = 0;
    if (!Call(c, kLdMalloc, {0x40000, 0}, F, &tmp)) return false;
    if (!Call(c, 0x80062D84u, {h, g.U32(Gs(g) + 48u) == 1u ? dir : F + 28u, gpv + 1900u, tmp}, F, &v0)) return false;
    g.W32(gpv + 1912u, v0);                                               // 0x80062BC0
    if (!Call(c, 0x80062D84u, {h, F + 40u, gpv + 1932u, tmp}, F, &v0)) return false;
    g.W32(gpv + 1940u, v0);                                               // 0x80062C08 (the delay slot)
    if (!Call(c, 0x80062D84u, {h, dir + 12u * U(s4) + 36u, g.U32(kEngine) + 12u, tmp}, F, &v0)) return false;
    g.W32(g.U32(kEngine) + 8u, v0);                                       // 0x80062C20
    if (g.U32(Gs(g) + 48u) == 2u) {
        if (!Call(c, 0x80062D84u, {h, dir + 12u * s6 + 36u, g.U32(kEngine) + 144u, tmp}, F, &v0)) return false;
        g.W32(g.U32(kEngine) + 140u, v0);
    }
    if (g.U8(Gs(g) + 4u) & 1u) {                                          // 0x80062C74
        if (!Call(c, 0x80062D84u, {h, dir + 12u * s5, gpv + 1892u, tmp}, F, &v0)) return false;
        g.W32(gpv + 1904u, v0);
    }
    if (!Call(c, 0x8001458Cu, {0x8005B7A4u, g.U32(kLdCdMode)}, F, &v0)) return false;
    const uint32_t rec = 0x800D6858u;                                     // 0x80062CC0: the stream record
    g.W32(rec, v0);
    g.W32(rec + 4u, 0);
    g.W32(rec + 8u, 16);
    g.W32(rec + 12u, 0);                                                  // 0x80062CD8 (the delay slot)
    if (!Call(c, 0x800148BCu, {}, F, &v0)) return false;
    g.W32(rec + 16u, v0);
    g.W32(rec + 20u, 1);
    for (uint32_t k = 0; k < 9u; ++k) {                                   // 0x80062CF8
        g.W32(0x800D6AA0u + 32u * k, 0xFFFFFFFFu);
        g.W32(0x800D6AA4u + 32u * k, 0xFFFFFFFFu);
        g.W32(0x800D6AA8u + 32u * k, 0);
    }
    if (!Call(c, kLdFree, {tmp}, F)) return false;
    if (!Call(c, 0x8001EFE8u, {1}, F)) return false;
    if (!Call(c, 0x8001460Cu, {h}, F)) return false;
    g.W32(gpv + 1964u, 0);                                                // 0x80062D34
    g.W32(gpv + 1884u, 2);
    {
        const uint32_t v = g.U32(0x80052640u);
        g.W32(0x800D6C08u, U(S((v << 3) - v) >> 4));
    }
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8001E614
namespace {
// SLUS 0x8001E7B0, a leaf: the value of `key` in the {key, value} list at `list` (0-terminated).
uint32_t ConfigGet(GuestRam& g, uint32_t list, uint32_t key, uint32_t out, uint32_t index) {
    if (list == 0u) return 0;
    uint32_t i = 0;
    for (uint32_t a = list; g.U32(a) != 0u; a += 8u, ++i) {
        if (g.U32(a) != key) continue;
        if (out != 0u) g.W32(out, g.U32(a + 4u));
        if (index != 0u) g.W32(index, i);
        return 1;
    }
    return 0;
}
// SLUS 0x8001E48C, frame 24: the sound allocator - the hook at gp+272, or Malloc(n, 0) - zero-filled.
bool SoundAlloc(GuestRam& g, uint32_t n, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - 24u;
    const uint32_t hook = g.U32(g.gp() + 272u);
    uint32_t p = 0;
    if (hook != 0u) {
        if (!Call(c, hook, {n}, F, &p)) return false;
    } else if (!Call(c, kLdMalloc, {n, 0}, F, &p)) {
        return false;
    }
    if (p != 0u)
        for (uint32_t k = 0; k < n; ++k) g.W8(p + k, 0);
    v0 = p;
    return !g.Faulted();
}
} // namespace

bool SoundInit(GuestRam& g, uint32_t config, uint32_t sp, LoaderCallees& c, int32_t& v0) {
    const uint32_t F = sp - kLdSoundInitFrame;
    constexpr uint32_t kSys = 0x800D6870u;
    int32_t s3 = 0;
    uint32_t banks = 1, voices = 24;
    if (ConfigGet(g, config, 4, F + 16u, 0) != 0u) {                      // 0x8001E648
        banks = g.U32(F + 16u);
        g.W32(kSys, banks);
    }
    if (ConfigGet(g, config, 5, F + 16u, 0) != 0u) voices = g.U32(F + 16u);   // 0x8001E674
    uint32_t p = 0;
    if (!SoundAlloc(g, banks << 2, F, c, p)) return false;
    g.W32(kSys + 4u, p);                                                  // 0x8001E69C (the delay slot)
    if (p == 0u) {
        s3 = -1;
    } else {
        const uint32_t n = ((((voices << 1) + voices) << 2) - voices) << 2; // 44 bytes a voice
        if (!SoundAlloc(g, n, F, c, p)) return false;
        g.W32(kSys + 12u, p);                                             // 0x8001E6BC (the delay slot)
        if (p == 0u) {
            s3 = -1;
        } else {
            for (uint32_t k = 0; S(k) < S(voices); ++k) g.W32(g.U32(kSys + 12u) + 44u * k + 28u, k);
            g.W32(kSys + 8u, voices);                                     // 0x8001E700
        }
    }
    if (s3 != 0)
        if (!Call(c, 0x8001E738u, {}, F)) return false;                   // the tear-down on a failure
    v0 = s3;
    return !g.Faulted();
}


// ============================================================================ SLUS 0x8001E8C4
bool SoundSpuAttr(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kLdSpuAttrFrame;
    g.W32(F + 16u, 16323);                                                // the mask 0x3FC3
    g.W16(F + 20u, 16383);                                                // main volume L / R
    g.W16(F + 22u, 16383);
    for (uint32_t o : {24u, 26u, 28u, 30u, 32u, 34u}) g.W16(F + o, 0);
    g.W32(F + 36u, 0);
    g.W32(F + 40u, 0);
    g.W16(F + 44u, 0);
    g.W16(F + 46u, 0);
    g.W32(F + 48u, 0);
    g.W32(F + 52u, 0);
    if (!Call(c, kLdSpuCommonAttr, {F + 16u}, F)) return false;
    return !g.Faulted();
}

} // namespace rr::sim
