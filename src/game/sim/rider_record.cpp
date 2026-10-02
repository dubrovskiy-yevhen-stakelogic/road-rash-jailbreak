#include "game/sim/rider_record.h"

namespace rr::sim {
namespace {

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }

// SLUS 0x8001E100 memset(dst, c, n): the byte replicated into a word, stored word by word while n != 0
// (n counts down by 4).
void GuestFillWords(GuestRam& g, uint32_t dst, uint32_t c, uint32_t n) {
    const uint32_t w = (c & 0xFFu) | ((c & 0xFFu) << 8) | ((c & 0xFFu) << 16) | ((c & 0xFFu) << 24);
    uint32_t d = dst;
    while (n != 0u) {
        g.W32(d, w);
        n -= 4u;
        d += 4u;
        if (g.Faulted()) break;
    }
}
// SLUS 0x8001E08C: byte copy.
void GuestCopyBytes(GuestRam& g, uint32_t dst, uint32_t src, uint32_t n) {
    while (n != 0u) {
        g.W8(dst, g.U8(src));
        ++src;
        --n;
        ++dst;
    }
}
// The 6-bit field at bits 12..17 of a +0x24 word (0x80064F40..0x80064F94).
void SetBits12(GuestRam& g, uint32_t a, uint32_t v) {
    g.W32(a, (g.U32(a) & 0xFFFC0FFFu) | ((v & 0x3Fu) << 12));
}
// The grudge clamp of 0x8006525C / 0x80065368: the signed sum of two bytes, stored as the unsigned sum
// unless it leaves [-15, 15].
uint8_t Clamp15(uint32_t own, uint32_t adj) {
    const uint32_t a1 = own + adj;
    const int32_t sum = static_cast<int32_t>(static_cast<int8_t>(own)) + static_cast<int8_t>(adj);
    uint32_t v = a1;
    if (sum < -15) v = U(-15);
    if (sum > 15) v = 15u;
    return static_cast<uint8_t>(v);
}

} // namespace

void RiderRecordInit(GuestRam& g, uint32_t bi, int32_t rec, uint32_t e) {
    auto rd = [&]() { return g.U32(e + 1084u); };
    // s3: whether the record's weapon fields are reset (0x80064C28..0x80064CF4)
    uint32_t s3 = 0;
    const uint32_t v1 = U(rec) - 2u;
    if (v1 < 16u) {
        const uint32_t gs = g.U32(kRrGameStatePtr);
        if (g.U8(gs + 4u) != 34u) s3 = 1;
        else s3 = (g.U8(gs + 5u) & 1u) ? 1u : 0u;
    } else if (v1 < 18u) {
        s3 = 1;
    } else {
        uint32_t gs = g.U32(kRrGameStatePtr);
        const uint32_t type = g.U8(gs + 4u);
        if (!(type & 0x20u) && type != 1u && type != 8u) {
            s3 = 1;
        } else if (g.U8(kRrSessionFlags) & 8u) {
            s3 = 1;
        } else {
            gs = g.U32(kRrGameStatePtr);
            if (g.U32(gs + 60u) != 0u) s3 = 0;
            else s3 = (g.U8(gs + 5u) & 1u) ? 1u : 0u;
        }
    }
    // the defaults (0x80064CF8..0x80064DEC)
    g.W8(rd() + 38u, 0);
    g.W32(rd() + 8u, 0x10000u);
    g.W32(g.U32(e + 852u) + 316u, 0x4B0000u);
    {
        const uint32_t r = rd();
        g.W8(r + 13u, 128);
        g.W8(r + 12u, 128);
    }
    {
        const uint32_t r = rd();
        g.W8(r + 14u, 128);
        g.W8(r + 15u, 128);
    }
    g.W8(rd() + 2u, 255);
    g.W8(rd() + 61u, 112);
    g.W8(rd() + 3u, 4);
    g.W8(rd() + 69u, 123);
    GuestFillWords(g, rd() + 52u, 32u, 8u);
    g.W8(rd() + 60u, 32);
    g.W8(rd() + 36u, 255);
    g.W8(rd() + 70u, 31);
    g.W8(rd() + 71u, 31);
    if (s3 != 0u) {
        g.W8(rd() + 47u, 0);
        g.W8(rd() + 46u, 9);
        g.W16(rd() + 44u, 1536);
        g.W32(rd() + 48u, 0);
    }
    GuestFillWords(g, rd() + 16u, 0u, 20u);
    // the file's record (0x80064DF0..0x80064FA8)
    if (bi != 0u) {
        const uint32_t p = bi + (U(rec) << 6);
        if (g.S8(p) == rec) {
            uint32_t s1 = p + 4u;
            g.W8(rd() + 38u, g.U8(s1));
            ++s1;
            g.W8(rd() + 1u, g.U8(s1));
            ++s1;
            const uint8_t s0 = g.U8(s1);
            ++s1;
            const uint8_t b3 = g.U8(s1);
            ++s1;
            g.W8(rd() + 3u, b3);
            GuestCopyWords(g, rd() + 8u, s1, 4u);
            s1 += 4u;
            GuestCopyWords(g, g.U32(e + 852u) + 316u, s1, 4u);
            s1 += 4u;
            GuestCopyWords(g, rd() + 12u, s1, 4u);
            s1 += 4u;
            g.W8(rd() + 2u, s0);
            GuestCopyWords(g, rd() + 16u, s1, 20u);
            s1 += 20u;
            if (s3 != 0u) GuestCopyBytes(g, rd() + 44u, s1, 2u);
            s1 += 2u;
            const uint8_t b36 = g.U8(s1);
            ++s1;
            g.W8(rd() + 36u, b36);
            const uint8_t b69 = g.U8(s1);
            ++s1;
            g.W8(rd() + 69u, b69);
            GuestCopyWords(g, rd() + 52u, s1, 8u);
            s1 += 9u;
            const uint8_t b61 = g.U8(s1);
            ++s1;
            g.W8(rd() + 61u, b61);
            // 0x80064F1C: s1 == p + 54 here on every path (the walk above is fixed), so the loader's
            // "record not read correctly" printf 0x80044894 is never reached.
            const uint8_t eb = g.U8(s1);
            ++s1;
            SetBits12(g, e + 36u, eb);
            const uint32_t a3 = g.U32(e + 852u);
            const uint8_t rb = g.U8(s1);
            ++s1;
            SetBits12(g, a3 + 36u, rb);
            GuestCopyWords(g, e + 76u, s1, 4u);
            GuestCopyWords(g, g.U32(e + 852u) + 76u, s1 + 4u, 4u);
        }
    }
    // the tail (0x80064FAC..0x80065078)
    g.W32(rd() + 4u, 0);
    g.W32(rd() + 40u, 0);
    g.W8(rd() + 39u, 0);
    g.W8(rd() + 0u, 0);
    {
        const uint32_t r = rd();
        g.W8(r + 37u, g.U8(r + 36u));
    }
    {
        const uint32_t r = rd();
        g.W8(r + 15u, g.U8(r + 13u));
    }
    {
        const uint32_t r = rd();
        g.W8(r + 14u, g.U8(r + 13u));
    }
    {
        const uint32_t r = rd();
        g.W8(r + 60u, g.U8(r + 52u));
    }
    const uint32_t gs = g.U32(kRrGameStatePtr);
    if (!(g.U16(e + 172u) < g.U32(gs + 48u)) && (g.U8(rd() + 1u) & 0xFu) == 2u) {
        const uint32_t r = GuestRand(g);                                          // 0x80065068
        g.W8(rd() + 38u, static_cast<uint8_t>(r & 7u));
    }
}

uint32_t RiderNameId(GuestRam& g, uint32_t e) {
    const uint32_t rdp = g.U32(e + 1084u);
    int32_t a3 = g.U8(rdp + 38u);
    if (!(a3 < 9)) a3 -= 8;
    const uint32_t gs = g.U32(kRrGameStatePtr);
    const uint32_t h = g.U16(e + 172u);
    const uint32_t t0 = g.U32(gs + 48u);
    if (h < t0) {                                                                 // a player
        const uint32_t type = g.U8(gs + 4u);
        if (type & 0x20u) {
            const uint32_t pr = kRrPlayerRecords + 36u * h;
            const int32_t b9 = g.S8(pr + 9u), b10 = g.S8(pr + 10u);
            return U(b10 + (b9 == 0 ? 110 : 113));
        }
        if (type & 0x10u) return (type & 8u) ? h + 108u : h + 30u;
        return 92u;
    }
    if ((g.U8(rdp + 1u) & 0xFu) == 2u) {                                          // police (0x80066EE8)
        const uint32_t r = GuestRand(g);
        const int32_t bank = g.S32(g.U32(kRrGameStatePtr) + 60u);
        const int32_t lo = bank >= 0 ? bank : 0;
        const int32_t d = 2 - bank;
        const int32_t hi = d < 0 ? d : 0;
        return (U((lo + hi) << 3) + 66u + (r & 7u)) & 0xFFu;
    }
    const uint32_t a1 = g.U8(gs + 4u);
    const uint32_t tab = kRrSpawnClassTable + h;
    auto p1Class = [&]() { return g.U8(g.U32(g.U32(kRrP1Bike) + 1084u) + 1u) & 0xFu; };
    if (a1 == 44u) {                                                              // 0x80066F44
        const uint32_t cls = p1Class();
        const int32_t m = (cls == g.U8(tab)) ? -1 : 0;
        const int32_t v0 = cls != 0u ? -40 : -48;
        return U((m & v0) + 82 - 1 + a3);
    }
    if (a1 == 33u) {                                                              // 0x80066F94
        const uint32_t two = g.U32(g.U32(kRrP1Bike) + 180u) < 9u ? 1u : 0u;
        if (g.U8(tab) != two) return U(a3 + 81);
        return U(a3 + 73);
    }
    if (a1 == 36u || (a1 & 4u)) {                                                 // 0x80066FD4
        const uint32_t cls = p1Class();
        const int32_t base = cls != 0u ? 42 : 34;
        if (g.U8(tab) != cls) return U(base + 15 + a3);
        return U(base - 1 + a3);
    }
    if (a1 & 1u) {                                                                // 0x8006702C
        const uint32_t which = (p1Class() == 2u) ? 1u : (t0 < 2u ? 1u : 0u);
        const uint32_t b = g.U32(kRrPool0Ptr) + 1096u * which;
        const uint32_t cls = g.U8(g.U32(b + 1084u) + 1u) & 0xFu;
        const int32_t base = cls != 0u ? 42 : 34;
        if (g.U8(tab) == cls) return U(base - 1 + a3);
        return U(base + 15 + a3);
    }
    if (g.U8(tab) != 0u) return U(a3 + 41);                                       // 0x800670C4
    return U(a3 + 33);
}

void GridRiderAdjust(GuestRam& g, uint32_t e) {
    const uint32_t gs = g.U32(kRrGameStatePtr);
    uint32_t t0 = 0;
    if (g.U16(e + 172u) < g.U32(gs + 48u)) {                                      // a player
        if (g.U8(gs + 4u) & 1u) return;
        const uint32_t a1 = g.U32(e + 1084u);
        t0 = g.U8(a1 + 1u) & 0xFu;
        const uint32_t a0 = 8u * t0 + 2u;
        if (g.U8(kRrAiToHandle + a0) == 31u) return;
        const uint8_t v = g.U8(kRiderRecords + kRiderRecordBytes * a0 + 36u);
        g.W8(a1 + 36u, v);
        g.W8(a1 + 37u, v);
    } else {
        const uint32_t type = g.U8(gs + 4u);
        const uint32_t t4 = (type & 4u) ? 1u : ((type & 1u) << 1);
        uint32_t a3 = 0;
        do {                                                                      // 0x80065188: toward each player
            const uint32_t a2 = g.U32(e + 1084u);
            const uint32_t pc = g.U8(kRiderRecords + kRiderRecordBytes * a3 + 1u) & 0xFu;
            t0 = g.U8(a2 + 1u) & 0xFu;
            const uint32_t blk = kRrClassBlocks + 36u * t0;
            const uint32_t at = a2 + a3 + 16u;
            if (t0 == pc) {
                if (g.U8(g.U32(kRrGameStatePtr) + 4u) == 44u) g.W8(at, 0);
                else if (t4 == 1u) g.W8(at, 15);
                else g.W8(at, Clamp15(g.U8(at), g.U8(blk + 7u)));
            } else if (pc == 2u) {
                g.W8(at, Clamp15(g.U8(at), g.U8(blk + 19u)));
            } else if (g.U8(g.U32(kRrGameStatePtr) + 4u) == 44u) {
                g.W8(at, 15);
            } else {
                g.W8(at, Clamp15(g.U8(at), g.U8(blk + 6u)));
            }
            ++a3;
        } while (a3 < g.U32(g.U32(kRrGameStatePtr) + 48u));
        if (t4 < 2u) {                                                            // 0x800652C8: toward AI 2..17
            const uint32_t t3 = kRrClassBlocks + t4 + 36u * t0;
            for (uint32_t k = 2; k < 18u; ++k) {
                if (g.U8(g.U32(kRrGameStatePtr) + 4u) == 44u) {
                    g.W8(g.U32(e + 1084u) + k + 16u, static_cast<uint8_t>(-15));
                    continue;
                }
                const uint32_t kc = g.U8(kRiderRecords + kRiderRecordBytes * k + 1u) & 0xFu;
                const uint8_t adj = (t0 == kc) ? g.U8(t3 + 22u) : g.U8(t3 + 20u);
                const uint32_t at = g.U32(e + 1084u) + k + 16u;
                g.W8(at, Clamp15(g.U8(at), adj));
            }
        }
    }
    const uint32_t ai = g.U8(kRrHandleToAi + g.U16(e + 172u));                   // 0x800653B8
    g.W8(g.U32(e + 1084u) + ai + 16u, 0x80);
}

} // namespace rr::sim
