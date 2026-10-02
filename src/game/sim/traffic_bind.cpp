// src\game\sim\traffic_bind - see traffic_bind.h. Each store and call carries the original address.
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c
#include "game/sim/traffic_bind.h"

#include "game/sim/anim.h"
#include "game/sim/population.h"
#include "game/sim/spine.h"

namespace rr::sim {

namespace {

int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kEffectBase   = 0x800D39B0; // 112-byte effect records (spine.h kEffectPool)

// SeatRelease (anim.h) is a leaf of the animation machine that never reaches the pose side; the
// machine still needs a pose seam, and this one refuses (fails the caller) should it ever be reached.
struct NoPose final : AnimPoseSeam {
    bool reached = false;
    uint32_t TransitionCapture(uint32_t, uint32_t, uint32_t) override { reached = true; return 0; }
    uint32_t ApplyFrame(uint32_t) override { reached = true; return 0; }
};

// RASHCDG 0x80068D20 SeatRelease(a0, a1, a2), ported in anim.h.
bool SeatReleaseNative(GuestRam& g, uint32_t a0, uint32_t a1, uint32_t a2) {
    NoPose pose;
    AnimMachine m(g, pose);
    m.SeatRelease(a0, a1, a2);
    return !m.Failed() && !pose.reached;
}

// The part-array reset both LOD paths share: part `base` of the array at obj[+4], every store
// re-reading the pointer as the original does.
void PartIdentity(GuestRam& g, uint32_t obj, uint32_t base, uint32_t unit) {
    g.W16(g.U32(obj + 4) + base + 6, 0);                              // 0x8002FFAC / 0x80012A78
    g.W16(g.U32(obj + 4) + base + 8, 0);
    g.W16(g.U32(obj + 4) + base + 10, 0);
    g.W16(g.U32(obj + 4) + base + 14, 0);
    g.W16(g.U32(obj + 4) + base + 16, 0);
    g.W16(g.U32(obj + 4) + base + 18, 0);
    g.W16(g.U32(obj + 4) + base + 4, static_cast<uint16_t>(unit));
    g.W16(g.U32(obj + 4) + base + 12, static_cast<uint16_t>(unit));
    g.W16(g.U32(obj + 4) + base + 20, static_cast<uint16_t>(unit));
}

} // namespace

// ============================================================================ SLUS 0x8001298C
int32_t LodSelect(GuestRam& g, uint32_t obj, uint32_t lod) {
    const int32_t old = g.S8(obj + 8);                                // t2
    if (lod == U(old)) return old;                                    // 0x80012998
    const uint32_t reg = g.U32(obj + 96);
    if (!(S(lod) < static_cast<int32_t>(g.U8(reg + 4)))) return old;  // 0x800129B0 (signed)
    const uint32_t off = lod * 12u;                                   // t1
    g.W8(obj + 8, static_cast<uint8_t>(lod));                         // 0x800129C4
    const uint32_t dod = g.U32(off + g.U32(reg + 8));
    g.W32(obj + 0, dod);                                              // 0x800129DC
    const uint32_t v40 = g.U32(dod + 16);
    const uint32_t cur = g.U32(obj + 0);
    g.W32(obj + 40, v40);                                             // 0x800129E8
    const uint32_t parts = g.U16(cur + 24);                           // t0
    for (uint32_t i = 0; i < parts; ++i) {
        const uint32_t tab = g.U32(g.U32(obj + 96) + 8);
        const uint32_t dst = g.U32(obj + 4) + 24u * i;
        const uint32_t src = g.U32(off + tab + 4);
        g.W32(dst, g.U32(4u * i + src));                              // 0x80012A30
    }
    const uint32_t kind = (g.U16(g.U32(obj + 0) + 14) & 0x78u) >> 3;
    if (kind == 5 && (old == 4 || old == 0)) PartIdentity(g, obj, 0, 4096); // 0x80012A78..0x80012AD8
    return old;
}

// ============================================================================ SLUS 0x800302C4
int32_t ModelKeySet(GuestRam& g, uint32_t obj, uint32_t key) {
    if (key == 0) {
        g.W16(obj + 74, static_cast<uint16_t>(g.S8(g.U32(obj + 96) + 7))); // 0x8003032C
    } else {
        uint32_t p = g.U32(kModelKeyTablePtr);
        int32_t i = 0;
        for (; i < 34; ++i, p += 12)
            if (g.U8(p) == key) break;                                // 0x800302E4
        g.W16(obj + 74, static_cast<uint16_t>(i < 34 ? i : -1));      // 0x80030310
    }
    const int32_t k = g.S16(obj + 74);
    if (k == -1) return -1;                                           // 0x80030338
    const uint32_t w = g.U32(obj + 36);
    if ((w & 0x3F400u) != 0x3F400u) return 0;                         // 0x8003034C
    const uint32_t f = g.U16(g.U32(kModelKeyTablePtr) + 12u * U(k) + 10);
    const uint32_t v = (511u - (f >> 6)) * 3u + U(S((f & 0x3Fu) - 40u) >> 3);
    g.W32(obj + 36, (w & 0xFFFC0FFFu) | ((v & 0x3Fu) << 12));         // 0x800303AC
    return 0;
}

// ============================================================================ SLUS 0x8002FDEC
bool RegistryBind(GuestRam& g, uint32_t obj, uint32_t slot, uint32_t alloc, uint32_t keyFlag, int32_t& v0) {
    v0 = -1;
    uint32_t reg = 0;
    if (slot < 50u && g.U32(kModelRegistry + 16u * slot) != 0) reg = kModelRegistry + 16u * slot;
    g.W32(obj + 96, reg);                                             // 0x8002FE40
    if (reg == 0) return !g.Faulted();
    const uint8_t lod = static_cast<uint8_t>(g.U8(reg + 4) - 1u);
    g.W8(obj + 8, lod);                                               // 0x8002FE50
    const int32_t cur = g.S8(obj + 8);
    g.W8(obj + 11, lod);                                              // 0x8002FE5C
    const uint32_t reg2 = g.U32(obj + 96);
    g.W8(obj + 10, lod);                                              // 0x8002FE64
    const uint32_t dod = g.U32(U(cur) * 12u + g.U32(reg2 + 8));
    const uint8_t f9 = static_cast<uint8_t>(g.U8(obj + 9) | 3u);
    const uint32_t reg3 = g.U32(obj + 96);
    g.W8(obj + 9, f9);                                                // 0x8002FE8C
    g.W32(obj + 0, dod);                                              // 0x8002FE90
    const uint32_t first = g.U32(g.U32(reg3 + 8));
    const uint32_t parts = g.U16(dod + 24);                           // s1
    const uint32_t cap = g.U16(first + 24);                           // s2
    if (alloc != 0) {
        bool refused = false;
        const uint32_t p = SpineMalloc(g, cap * 24u, 0, refused);     // 0x8002FEB8
        if (refused) return false;
        g.W32(obj + 4, p);                                            // 0x8002FEC0
    }
    if (g.U32(obj + 4) == 0) return !g.Faulted();                     // 0x8002FECC
    for (uint32_t i = 0; i < parts; ++i) {
        const int32_t l = g.S8(obj + 8);
        const uint32_t e = U(l) * 12u + g.U32(g.U32(obj + 96) + 8);
        const uint32_t dst = 24u * i + g.U32(obj + 4);
        g.W32(dst, g.U32(4u * i + g.U32(e + 4)));                     // 0x8002FF24
    }
    g.W8(obj + 9, static_cast<uint8_t>(g.U8(obj + 9) & 0xFBu));       // 0x8002FF44
    for (uint32_t k = 0; k < 2; ++k) {
        g.W32(obj + 56 + 8u * k, 0);                                  // 0x8002FF48
        g.W32(obj + 60 + 8u * k, 0);                                  // 0x8002FF4C
    }
    g.W8(obj + 72, 0);                                                // 0x8002FF60
    g.W8(obj + 73, 0xFF);                                             // 0x8002FF6C
    g.W32(obj + 52, 0);                                               // 0x8002FF74
    g.W32(obj + 20, 0);                                               // 0x8002FF78
    g.W32(obj + 16, 0);                                               // 0x8002FF7C
    g.W32(obj + 12, 0);                                               // 0x8002FF84
    g.W16(obj + 32, 0);                                               // 0x8002FF88
    g.W16(obj + 30, 0);                                               // 0x8002FF8C
    g.W16(obj + 28, 0);                                               // 0x8002FF94
    for (uint32_t j = 0; j < cap; ++j) PartIdentity(g, obj, 24u * j, 4096); // 0x8002FFA0..0x8003002C
    g.W32(obj + 44, 0x7FFFFFFFu);                                     // 0x8003004C
    g.W32(obj + 48, 0x7FFFFFFFu);                                     // 0x80030050
    const uint32_t w36 = g.U32(obj + 36);
    const uint32_t d = g.U32(obj + 0);
    const uint32_t w76 = g.U32(obj + 76);
    const uint32_t t3 = (w36 >> 12) & 0x3Fu;
    const uint32_t d16 = g.U32(d + 16);
    g.W32(obj + 76, 0x10000);                                         // 0x8003006C
    g.W32(obj + 36, 0x3F000);                                         // 0x80030070
    g.W32(obj + 40, d16);                                             // 0x80030074
    const uint32_t kind = (g.U16(d + 14) & 0x78u) >> 3;
    switch (kind) {
        case 1: {                                                     // 0x800300B8
            g.W32(obj + 100, 0x80054198);
            const uint32_t v = g.U32(obj + 36);
            g.W32(obj + 76, w76);                                     // 0x800300E0
            g.W32(obj + 36, ((((v | 0x400u) & 0xFFFC0FFFu) | (t3 << 12)) & 0xFFFBFFFFu)); // 0x800300F8
            break;
        }
        case 2: {                                                     // 0x80030118
            g.W32(obj + 100, 0x80054178);
            uint32_t v = ((g.U32(obj + 36) | 0x400u) & 0xFFFC0FFFu) | (t3 << 12);
            v &= 0xFFFFFFFEu;
            v &= 0xFFFBFFFFu;
            v &= 0xF7FFFFFFu;
            v &= 0xDFFFFFFFu;
            v &= 0xEFFFFFFFu;
            v &= 0xFF87FFFFu;
            v &= 0x3FFFFFFFu;
            g.W32(obj + 36, v);                                       // 0x80030198
            break;
        }
        case 3: {                                                     // 0x8003019C
            g.W32(obj + 100, 0x80054160);
            const uint32_t a0 = g.U32(obj + 36) | 0x400u;
            g.W32(obj + 36, a0);                                      // 0x800301B4
            const int32_t id = g.S32(g.U32(obj + 0) + 8);
            uint32_t bits = 0;
            if (id == 300) bits = 0x2F000;
            else if (id == 315) bits = 0x2E000;
            else if (id == 309) bits = 0x1C000;
            if (bits != 0) {
                g.W32(obj + 36, (a0 & 0xFFFC0FFFu) | bits);           // 0x80030220
            } else {
                const uint32_t r = GuestRand(g);                      // 0x80030224
                const uint32_t a1 = g.U32(obj + 36) & 0xFFFC0FFFu;
                g.W32(obj + 36, a1 | (((r % 15u + 14u) & 0x3Fu) << 12)); // 0x8003026C
            }
            g.W32(obj + 36, g.U32(obj + 36) & 0xFFFBFFFFu);           // 0x80030284
            break;
        }
        case 4:                                                       // 0x800300FC
        {
            const uint32_t v = g.U32(obj + 36);
            g.W32(obj + 100, 0x800541B8);
            g.W32(obj + 36, v | 0x400u);                              // 0x80030114
            break;
        }
        default: g.W32(obj + 100, 0); break;                          // 0x80030288
    }
    g.W16(obj + 74, 0xFFFF);                                          // 0x80030294
    if (keyFlag != 0) ModelKeySet(g, obj, 0);                         // 0x8003029C
    v0 = 0;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002FAD4
bool ModelBind(GuestRam& g, uint32_t obj, int32_t pool, uint32_t cls, uint32_t alloc, uint32_t& v0) {
    v0 = 0xFFFF;
    const uint32_t p = U(pool);
    const uint32_t s3 = p * 16u + kModelPoolTables;                   // 0x8002FB14
    const uint32_t lists = kModelClassLists + p * 8u;
    int32_t slot = -1;                                                // s2
    uint32_t s4 = cls;                                                // the class the function returns
    uint32_t s6 = 0;                                                  // the class-list entry
    const uint32_t idx = p - 1u;
    if (idx < 6u) {
        switch (idx) {                                                // jump table 0x80010CBC
            case 0: {                                                 // 0x8002FB60
                const uint32_t a0 = g.U32(g.gp() + 572u + 4u * ((cls - 9u) < 9u ? 1u : 0u));
                slot = g.S16(g.U32(s3 + 12) + 2u * a0);
                break;
            }
            case 1: {                                                 // 0x8002FB84
                const uint32_t v1 = ((cls - 3u) < 3u || (cls - 12u) < 3u) ? 1u : 0u;
                const uint32_t a0 = cls - 3u * v1;
                slot = g.S16(g.U32(s3 + 12) + 2u * a0);
                break;
            }
            case 2: {                                                 // 0x8002FC40: pool 3
                int32_t n = g.S16(lists);
                if (n == 0) break;
                uint32_t pick = cls;                                  // a2
                if (cls == 0xFFFFu) {
                    const uint32_t rt = g.U8(g.U32(kGameStatePtr) + 4);
                    const int32_t odd = S(rt & 1u);
                    n = (rt == 44u) ? n - 2 - odd : n - odd;          // 0x8002FC8C / 0x8002FC90
                    const uint32_t r = GuestRand(g);                  // 0x8002FC94
                    pick = U(n) == 0 ? r : r % U(n);                  // divu: hi = dividend on zero
                }
                if (!(pick < U(static_cast<int32_t>(g.S16(lists))))) break; // 0x8002FCBC
                s6 = g.U32(lists + 4) + 4u * pick;
                s4 = U(static_cast<int32_t>(g.S16(s6)) - g.S32(s3 + 8));
                slot = g.S16(g.U32(s3 + 12) + 2u * s4);
                break;
            }
            case 3: {                                                 // 0x8002FBC0: pool 4
                const int32_t n = g.S16(lists);
                if (n == 0) break;
                const uint32_t r = GuestRand(g);                      // 0x8002FBE0
                const uint32_t pick = r % U(n);
                if (!(pick < U(static_cast<int32_t>(g.S16(lists))))) break; // 0x8002FBF8
                s6 = g.U32(lists + 4) + 4u * pick;
                s4 = U(static_cast<int32_t>(g.S16(s6)) - g.S32(s3 + 8));
                const uint32_t a0 = g.U32(g.gp() + 580u + 4u * (s4 < 30u ? 1u : 0u));
                slot = g.S16(g.U32(s3 + 12) + 2u * a0);
                break;
            }
            default:                                                  // 0x8002FB54: pools 5, 6
                slot = g.S16(g.U32(s3 + 12));
                break;
        }
    }
    if (g.Faulted()) return false;
    if (slot < 0) return true;                                        // 0x8002FCF8
    int32_t bound = 0;
    if (!RegistryBind(g, obj, U(slot), alloc, g.U32(kModelBindFlags + 4u * p), bound)) return false; // 0x8002FD18
    if (bound < 0) return true;                                       // 0x8002FD24
    int32_t r = bound;                                                // a0
    switch (idx) {                                                    // jump table 0x80010CD4
        case 0: r = ModelKeySet(g, obj, g.U8(kModelKeyPool1 + s4)); break;  // 0x8002FD70
        case 1: r = ModelKeySet(g, obj, g.U8(kModelKeyPool2 + s4)); break;  // 0x8002FD88
        case 3: r = ModelKeySet(g, obj, U(static_cast<int32_t>(g.S16(s6 + 2)))); break; // 0x8002FDA0
        case 4:
        case 5:
            LodSelect(g, obj, s4);                                    // 0x8002FD60
            r = 0;
            break;
        default: break;                                               // pool 3: 0x8002FDB0
    }
    if (g.Faulted()) return false;
    v0 = (r != 0) ? 0xFFFFu : s4;                                     // 0x8002FDB0
    return true;
}

// ============================================================================ SLUS 0x8002A738
void EffectUnlink(GuestRam& g, uint32_t e, uint32_t prevRec, uint32_t rec, uint32_t link) {
    const uint32_t state = (g.U32(rec) >> 6) & 15u;
    // e[+0x24] is read only by the arms that store it back (as the original does)
    const bool counted = state == 1 || state == 2 || state == 3 || state == 4 || state == 7;
    const uint32_t w = counted ? g.U32(e + 36) : 0u;
    bool store = true;
    uint32_t keep = 0, add = 0;
    switch (state) {                                                  // jump table 0x80010C58
        case 1:
        case 3:                                                       // 0x8002A79C
            keep = w & 0xF9FFFFFFu;
            add = ((((w >> 25) & 3u) - 1u) & 3u) << 25;
            break;
        case 2:                                                       // 0x8002A7EC
            if (((g.U32(rec) >> 14) & 0xFFu) == 2u) {
                keep = w & 0xFE7FFFFFu;
                add = ((((w >> 23) & 3u) - 1u) & 3u) << 23;
            } else {
                keep = w & 0xFF87FFFFu;
                add = ((((w >> 19) & 15u) - 1u) & 15u) << 19;
            }
            break;
        case 4:                                                       // 0x8002A860
            keep = w & 0x3FFFFFFFu;
            add = ((w >> 30) - 1u) << 30;
            break;
        case 7:                                                       // 0x8002A7C4
            keep = w & 0xFF87FFFFu;
            add = ((((w >> 19) & 15u) - 1u) & 15u) << 19;
            break;
        case 5:
            CopDrop(g, e);                                            // 0x8002A850
            store = false;
            break;
        default: store = false; break;                                // 0x8002A884
    }
    if (store) g.W32(e + 36, keep | add);                             // 0x8002A880
    const uint32_t w0 = g.U32(rec);
    g.W8(rec + 60, 0);                                                // 0x8002A88C
    g.W32(rec, (w0 & 0xFFFFC03Fu) | 0x3Fu);                           // 0x8002A8A4
    if (prevRec != 0) g.W32(prevRec, (g.U32(prevRec) & 0xFFFFFFC0u) | (link & 0x3Fu)); // 0x8002A8C0
    else g.W8(e + 73, static_cast<uint8_t>(link));                    // 0x8002A8C4
}

// ============================================================================ SLUS 0x8002847C
void CopLeave(GuestRam& g, uint32_t e) {
    const int32_t head = g.S8(e + 73);
    uint32_t rec = U(head) * 112u + kEffectBase;
    if (head == -1 || rec == 0) return;                               // 0x800284C0 / 0x800284C8
    do {
        const int32_t link = S(g.U32(rec) << 26) >> 26;               // 0x800284E4
        EffectUnlink(g, e, 0, rec, U(link));                          // 0x800284E8
        if (g.Faulted()) return;
        rec = (link == -1) ? 0u : U(link) * 112u + kEffectBase;
    } while (rec != 0);
}

// ============================================================================ SLUS 0x80028034
void CopJoin(GuestRam& g, uint32_t e) {
    int32_t base = 0;                                                 // s4
    int32_t count = 4;                                                // s2
    uint32_t tab = 0x800536E8;
    if (((g.U16(g.U32(e + 0) + 14) & 0x78u) >> 3) == 2u) {            // 0x80028070
        const uint32_t a1 = g.U32(e + 180);
        if (a1 < 18u) return;                                         // 0x80028084
        count = 6;
        uint32_t a0 = a1 + (U(S(a1 - 18u) >> 31) & (18u - a1));     // 0x800280A0
        a0 += U(S(20u - a1) >> 31) & (20u - a1);                    // 0x800280B4
        base = S(a0 - 18u);
        tab = 0x800536DC;
    }
    if ((g.U32(e + 36) >> 27) & 1u) return;                           // 0x800280E4
    for (int32_t i = 0; i < count; ++i, tab += 2) {
        const int32_t k = EffectFindFree(g);                          // 0x80028100
        if (g.Faulted()) return;
        if (k == -1) continue;
        const uint32_t rec = U(k) * 112u + kEffectBase;
        g.W32(rec, (g.U32(rec) & 0xFFFFFC3Fu) | 0x140u);              // 0x80028140
        const uint32_t clock = g.U32(g.U32(kGameStatePtr) + 16);
        uint32_t a1 = g.U32(rec);
        g.W32(rec + 36, 0);                                           // 0x80028158
        g.W32(rec + 44, 0);                                           // 0x8002815C
        g.W32(rec + 40, 0);                                           // 0x80028160
        g.W8(rec + 61, 0);                                            // 0x80028164
        g.W32(rec + 48, clock);                                       // 0x80028168
        const int32_t b0 = g.S8(tab);
        a1 &= 0xFFC03FFFu;
        g.W8(rec + 96, 5);                                            // 0x80028174
        a1 |= ((U(b0) + U(count) * U(base)) & 0xFFu) << 14;
        g.W32(rec, a1);                                               // 0x8002818C
        const uint32_t v1 = (a1 & 0xFFFFC3FFu) | 0x800u;
        const uint8_t b1 = g.U8(tab + 1);
        g.W32(rec, v1);                                               // 0x800281A8
        g.W8(rec + 97, b1);                                           // 0x800281AC
        EffectLink(g, e, U(k));                                       // 0x800281B0
    }
    g.W32(e + 36, g.U32(e + 36) | 0x08000000u | 0x10000000u | 0x20000000u); // 0x800281E4
}

// ============================================================================ RASHCDG 0x800CB84C
bool SeatFree(GuestRam& g, uint32_t o) {
    const int32_t i = g.S8(o + 567);
    if (i == -1) return !g.Faulted();                                 // 0x800CB85C
    const uint32_t bits = g.U32(kSeatBitsWord);
    const uint32_t bit = 1u << (U(i) & 31u);
    if ((bits & bit) == 0) return !g.Faulted();                       // 0x800CB874
    g.W32(kSeatBitsWord, bits & ~bit);                                // 0x800CB880
    const int32_t j = g.S8(o + 567);
    if (g.Faulted()) return false;
    return SeatReleaseNative(g, o, U(j) * 172u + kSeatRecords, 0);    // 0x800CB8B0
}

// ============================================================================ RASHCDG 0x800CC0B0
bool ObjectFree(GuestRam& g, uint32_t o) {
    const uint32_t a = g.U32(o + 540);
    if (a != 0) {
        g.W32(a + 36, 0);                                             // 0x800CC0CC
        g.W32(kAnimUsedCount, g.U32(kAnimUsedCount) - 1u);            // 0x800CC0DC
        g.W32(o + 540, 0);                                            // 0x800CC0E0
    }
    if (g.U8(o + 566) != 0) return !g.Faulted();                      // 0x800CC0EC
    if (g.Faulted()) return false;
    return SeatFree(g, o);                                            // 0x800CC0F4
}

// ============================================================================ RASHCDG 0x8008C000
namespace {
// The live-count drop every arm opens with: `if (*ctrl > 0) *ctrl -= 1`.
void CountDown(GuestRam& g, uint32_t ctrl) {
    const int32_t n = g.S32(ctrl);
    if (n > 0) g.W32(ctrl, U(n - 1));
}
// The lowest-free index: `if (slot < ctrl[+4]) ctrl[+4] = slot`.
void LowFree(GuestRam& g, uint32_t ctrl, uint32_t slot) {
    if (S(slot) < g.S32(ctrl + 4)) g.W32(ctrl + 4, slot);
}
} // namespace

bool PoolRelease(GuestRam& g, uint32_t h, int32_t pool) {
    const uint32_t slot = g.U16(h) & 0x1Fu;                           // s1
    const uint32_t idx = U(pool) - 2u;
    if (!(idx < 5u)) return !g.Faulted();                             // 0x8008C02C
    uint32_t ctrl = 0;
    switch (idx) {                                                    // jump table 0x8005B668
        case 0: {                                                     // 0x8008C050: pool 2
            const uint32_t c = 0x800D4B70;
            const uint32_t o = g.U32(c + 16) + 572u * (g.U16(h) & 0x1Fu);
            if (g.Faulted()) return false;
            if (!ObjectFree(g, o)) return false;                      // 0x8008C078
            CountDown(g, c);
            if (g.U32(c + 8) == slot) {
                for (;;) {
                    const uint32_t n = g.U32(c + 8) - 1u;
                    const uint32_t scan = g.U32(kPoolFreeScan);
                    g.W32(c + 8, n);                                  // 0x8008C0BC
                    g.W32(kPoolFreeScan, scan);                       // 0x8008C0C4
                    if (S(n) < 0 || g.Faulted()) break;
                    if (g.U16(g.U32(c + 16) + 572u * n + 172) != 0) break;
                }
            }
            ctrl = c;
            break;
        }
        case 1: {                                                     // 0x8008C100: pool 3
            if (g.U32(h + 8) == 0) {
                const uint32_t car = 0x800CF660 + 512u * (g.U16(h) & 0x1Fu);
                if ((g.U32(car + 36) >> 27) & 1u) {
                    CopLeave(g, car);                                 // 0x8008C140
                    CopDrop(g, car);                                  // 0x8008C148
                }
            }
            const uint32_t c = 0x800CF650;
            CountDown(g, c);
            if (g.U32(c + 8) == slot) {
                for (;;) {
                    const uint32_t n = g.U32(c + 8) - 1u;
                    const uint32_t scan = g.U32(kPoolFreeScan);
                    g.W32(c + 8, n);                                  // 0x8008C194
                    g.W32(kPoolFreeScan, scan);                       // 0x8008C19C
                    if (S(n) < 0 || g.Faulted()) break;
                    if (g.U16(c + 16 + 512u * n + 172) != 0) break;
                }
            }
            ctrl = c;
            break;
        }
        case 2:                                                       // 0x8008C1C4: pool 4
        case 3: {                                                     // 0x8008C298: pool 5
            const bool p4 = idx == 2;
            const uint32_t c = p4 ? 0x800CD6C8u : 0x800CE598u;
            const uint32_t stride = p4 ? 596u : 452u;
            // pool 5's records lie BELOW its pointer: base - 452 n
            auto rec = [&](uint32_t n) { return p4 ? g.U32(c + 12) + stride * n : g.U32(c + 12) - stride * n; };
            CountDown(g, c);
            if (g.U32(c + 8) == slot) {
                for (;;) {
                    const uint32_t n = g.U32(c + 8) - 1u;
                    const uint32_t scan = g.U32(kPoolFreeScan) + stride;
                    g.W32(c + 8, n);                                  // 0x8008C208 / 0x8008C2DC
                    g.W32(kPoolFreeScan, scan);                       // 0x8008C210 / 0x8008C2E4
                    if (S(n) < 0 || g.Faulted()) break;
                    if (g.U16(rec(n) + 172) != 0) break;
                }
            }
            LowFree(g, c, slot);                                      // 0x8008C264 / 0x8008C330
            const uint32_t store = g.U32(rec(slot) + 4);
            g.W32(store, 0);                                          // 0x8008C358
            const uint32_t d = store - kPoolStoreBase;
            const int32_t k = S(0u - d * 0x55555555u) >> 3;           // (store - base) / 24
            if (k < g.S32(kPoolLowFree)) g.W32(kPoolLowFree, U(k));   // 0x8008C39C
            g.W16(h, 0);                                              // 0x8008C43C
            return !g.Faulted();
        }
        default: {                                                    // 0x8008C3A0: pool 6
            const uint32_t c = 0x800CD6A8;
            CountDown(g, c);
            if (g.U32(c + 8) == slot) {
                for (;;) {
                    const uint32_t n = g.U32(c + 8) - 1u;
                    const uint32_t scan = g.U32(kPoolFreeScan);
                    g.W32(c + 8, n);                                  // 0x8008C3E4
                    g.W32(kPoolFreeScan, scan);                       // 0x8008C3EC
                    if (S(n) < 0 || g.Faulted()) break;
                    if (g.U16(g.U32(c + 28) + 280u * n) != 0) break;
                }
            }
            ctrl = c;
            break;
        }
    }
    LowFree(g, ctrl, slot);                                           // 0x8008C438
    g.W16(h, 0);                                                      // 0x8008C43C
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80095AEC
bool ReleaseRiderObject(GuestRam& g, uint32_t r) {
    const uint32_t bits = g.U32(kSeatBitsWord);
    const uint32_t bit = 1u << (U(static_cast<int32_t>(g.S8(r + 571))) & 31u);
    if ((bits & bit) == 0) return !g.Faulted();                       // 0x80095B18
    g.W32(kSeatBitsWord, bits & ~bit);                                // 0x80095B28
    const uint32_t seat = U(static_cast<int32_t>(g.S8(r + 571))) * 172u + kSeatRecords;
    if (g.Faulted()) return false;
    if (!SeatReleaseNative(g, r, seat, 0)) return false;              // 0x80095B58
    const uint32_t a = g.U32(r + 556);
    if (a != 0) {
        g.W32(a + 36, 0);                                             // 0x80095B74
        g.W32(kAnimUsedCount, g.U32(kAnimUsedCount) - 1u);            // 0x80095B84
    }
    if (static_cast<uint32_t>(static_cast<int32_t>(g.S8(r + 572))) & 0x80u) {
        CopLeave(g, U(static_cast<int32_t>(g.S8(r + 571))) * 172u + kSeatRecords); // 0x80095BC0
        g.W8(r + 572, static_cast<uint8_t>(g.U8(r + 572) & 0x7Fu));   // 0x80095BD4
    }
    g.W32(r + 556, 0);                                                // 0x80095BDC
    g.W8(r + 571, 0xFF);                                              // 0x80095BE0
    return !g.Faulted();
}

} // namespace rr::sim
