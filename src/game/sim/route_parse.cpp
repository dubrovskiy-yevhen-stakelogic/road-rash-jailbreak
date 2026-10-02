// The route block parser and the road-map loader (route_parse.h), ported from our own disassembly of RASHCDI.BIN
// (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06) and SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1);
// the host's kernel string calls from our disassembly of the player's SCPH1001 BIOS. The instruction addresses in the
// comments are the original's.
#include "game/sim/route_parse.h"

#include "game/sim/loader2.h" // RoadLoad's children

namespace rr::sim {
namespace {

constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
constexpr uint32_t kWalk = 1u << 20; // the host's bound on a string walk

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

bool Digit(uint32_t ch) { return ch - 48u < 10u; }

// the kernel's ctype flags of a byte < 0x80 (SCPH1001's table at 0xBFC0DDB1): 1 upper, 2 lower, 4 digit, 8 space
uint32_t CType(uint32_t b) {
    uint32_t f = 0;
    if (b >= 'A' && b <= 'Z') f |= 1u;
    if (b >= 'a' && b <= 'z') f |= 2u;
    if (b >= '0' && b <= '9') f |= 4u;
    if ((b >= 9u && b <= 13u) || b == 32u) f |= 8u;
    return f;
}

// 0xBFC022C0: a character's digit value (base up to 36), 9999999 for a non-digit
bool DigitVal(uint32_t ch, uint32_t& v) {
    const int32_t c = static_cast<int8_t>(static_cast<uint8_t>(ch));
    const uint32_t b = U(c) & 0xFFu;
    if (b >= 0x80u) return false;                                          // NAMED BOUND: the table's upper half
    const uint32_t f = CType(b);
    if (f & 4u) {
        v = U(c - 48);
    } else if (f & 3u) {
        int32_t lo = c;                                                    // 0xBFC02EDC tolower
        if (f & 1u) lo = static_cast<int8_t>(static_cast<uint8_t>(b + 32u));
        v = U(lo - 87);
    } else {
        v = 0x0098967Fu;
    }
    return true;
}

} // namespace

// ============================================================================ the kernel's string calls (the host's)
uint32_t BiosStrNCmp(GuestRam& g, uint32_t a, uint32_t b, uint32_t n) {    // A(18h) 0xBFC03310
    if (a == 0u || b == 0u) {
        if (a == b) return 0;
        return a != 0u ? 1u : 0xFFFFFFFFu;
    }
    int32_t k = S(n) - 1;
    if (k < 0) return 0;
    for (uint32_t w = 0; w < kWalk; ++w) {
        const int32_t cb = g.S8(b), ca = g.S8(a);
        ++b;
        if (cb != ca) break;
        if (ca == 0) return 0;
        ++a;
        if (--k < 0) return 0;
    }
    return U(static_cast<int32_t>(g.S8(a)) - static_cast<int32_t>(g.S8(b - 1u)));
}

uint32_t BiosStrLen(GuestRam& g, uint32_t a) {                             // A(1Bh) 0xBFC03494
    if (a == 0u) return 0;
    uint32_t n = 0;
    while (g.S8(a + n) != 0 && n < kWalk && !g.Faulted()) ++n;
    return n;
}

uint32_t BiosStrNCpy(GuestRam& g, uint32_t dst, uint32_t src, uint32_t n) { // A(1Ah) 0xBFC03418
    if (dst == 0u || src == 0u) return 0;
    const uint32_t v1 = dst;
    if (S(n) <= 0) return v1;
    uint32_t count = 0;
    for (uint32_t w = 0; w < kWalk; ++w) {
        const uint8_t ch = g.U8(src);
        ++dst;
        ++src;
        g.W8(dst - 1u, ch);
        ++count;
        if (ch != 0u) {
            if (count != n) continue;
            return v1;
        }
        if (!(S(count) < S(n))) return v1;
        for (uint32_t z = 0; z < kWalk; ++z) {                             // 0xBFC03460: the zero padding
            ++count;
            const bool more = S(count) < S(n);
            g.W8(dst, 0);
            ++dst;
            if (!more) break;
        }
        return v1;
    }
    return v1;
}

uint32_t BiosStrChr(GuestRam& g, uint32_t a, uint32_t ch) {                // A(1Eh) 0xBFC0357C
    const int32_t c = static_cast<int8_t>(static_cast<uint8_t>(ch));
    if (a == 0u) return 0;
    for (uint32_t w = 0; w < kWalk; ++w, ++a) {
        const int32_t v = g.S8(a);
        if (c == v) return a;
        if (v == 0) return 0;
    }
    return 0;
}

bool BiosAtoi(GuestRam& g, uint32_t a, uint32_t& v0) {                     // A(10h) 0xBFC02950
    v0 = 0;
    if (a == 0u) return true;
    int32_t sign = 1;
    uint32_t base = 10, val = 0;
    int32_t c = g.S8(a);
    for (uint32_t w = 0;; ++w) {                                           // the blanks (ctype bit 3)
        if ((U(c) & 0xFFu) >= 0x80u || w >= kWalk) return false;          // NAMED BOUND: the table's upper half
        if (!(CType(U(c) & 0xFFu) & 8u)) break;
        c = g.S8(++a);
    }
    if (c == '-') {                                                        // 0xBFC029E0: each '-' flips the sign
        do {
            c = g.S8(++a);
            sign = -sign;
        } while (c == '-');
    }
    if (c == '0') {                                                        // 0xBFC029FC: 0b / 0x / octal
        c = g.S8(++a);
        if (c == 'B' || c == 'b') {
            c = g.S8(++a);
            base = 2;
        } else if (c == 'X' || c == 'x') {
            c = g.S8(++a);
            base = 16;
        } else {
            base = 8;
        }
    }
    uint32_t d = 0;
    if (!DigitVal(U(c), d)) return false;
    ++a;
    for (uint32_t w = 0; d < base; ++w) {                                  // 0xBFC02A8C
        if (w >= kWalk) return false;
        val = val * base + d;
        c = g.S8(a);
        ++a;
        if (!DigitVal(U(c), d)) return false;
    }
    v0 = val * U(sign);
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80064084 / 0x80064144 / 0x800643B4
bool ReadLine(GuestRam& g, uint32_t dst, uint32_t max, uint32_t src, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRpReadLineFrame;
    v0 = 0;
    if (src == 0u || g.U8(src) == 0u) return !g.Faulted();                // 0x8006409C
    uint32_t p = src;
    uint32_t ch = g.U8(src);
    if (S(max) > 0 && ch != 13u) {                                         // 0x800640BC
        for (;;) {                                                         // 0x800640D8
            if (ch == 10u) break;
            ++p;
            if (p == 0u) break;
            if (!(S(p - src) < S(max))) break;
            ch = g.U8(p);
            if (ch == 0u || ch == 13u) break;
        }
    }
    const uint32_t len = p - src;                                          // 0x80064114
    if (!Call(c, kRpStrNCpy, {dst, src, len + 1u}, F)) return false;
    g.W8(len + dst + 1u, 0);
    v0 = dst;
    return !g.Faulted();
}

bool ParseInt(GuestRam& g, uint32_t s, uint32_t out, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRpParseIntFrame;
    uint32_t n = 0;
    v0 = 0;
    if (!Call(c, kRpAtoi, {s}, F, &n)) return false;
    g.W32(out, n);                                                         // 0x80064160
    if (n == 0u) {
        uint32_t r = 0;
        if (!Call(c, kRpStrChr, {s, 48}, F, &r)) return false;
        if (r == 0u) return !g.Faulted();                                  // 0x80064180: v0 = 0
    }
    uint32_t a = s;                                                        // 0x80064188
    uint32_t v1 = g.U8(a);
    ++a;
    if (!Digit(v1)) {
        --a;
        for (uint32_t w = 0;; ++w) {                                       // 0x800641A8: to a '-' or the first digit
            if (w >= kWalk || g.Faulted()) return false;                   // the console would not return
            if (v1 == 45u) break;
            ++a;
            v1 = g.U8(a);
            if (Digit(v1)) break;
        }
        ++a;
    }
    for (uint32_t w = 0;; ++w) {                                           // 0x800641D0
        if (w >= kWalk || g.Faulted()) return false;
        const uint32_t x = g.U8(a);
        ++a;
        if (!Digit(x)) break;
    }
    v0 = a - 1u;
    return !g.Faulted();
}

bool NextToken(GuestRam& g, uint32_t src, uint32_t dst, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRpNextTokenFrame;
    v0 = 0;
    if (src == 0u) return !g.Faulted();
    uint32_t p = src;
    uint32_t ch = g.U8(p);
    if (ch == 0u) return !g.Faulted();                                     // 0x800643DC
    while (ch == 13u || ch == 9u || ch == 32u) {                           // 0x800643F0: the blanks
        ++p;
        ch = g.U8(p);
        if (ch == 0u || g.Faulted()) break;
    }
    if (g.U8(p) == 0u) return !g.Faulted();                                // 0x8006441C
    uint32_t s1 = p + 1u;                                                  // 0x80064434
    const uint32_t first = g.U8(p + 1u);
    if (!(first == 0u || first == 13u || first == 9u)) {
        for (uint32_t w = 0;; ++w) {                                       // 0x80064460
            if (w >= kWalk || g.Faulted()) return false;
            if (g.U8(s1) == 32u) break;
            ++s1;
            const uint32_t x = g.U8(s1);
            if (x == 0u || x == 13u || x == 9u) break;
        }
    }
    const uint32_t len = s1 - p;
    if (!Call(c, kRpStrNCpy, {dst, p, len}, F)) return false;             // 0x8006449C
    g.W8(dst + len, 0);
    v0 = s1 + 1u;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80069B10 / 0x80069B8C / 0x80069C60
bool TokenInt(GuestRam& g, uint32_t prev, uint32_t line, uint32_t sep, uint32_t out, uint32_t sp, LoaderCallees& c,
              uint32_t& v0) {
    const uint32_t F = sp - kRpTokenIntFrame;
    g.W32(out, 0);                                                         // 0x80069B30 (the delay slot: always)
    v0 = prev;
    if (prev == 0u) return !g.Faulted();
    if (line == 0u || out == 0u) return !g.Faulted();                      // v0 = prev
    uint32_t s0 = line;
    if ((sep & 0xFFu) != 0u) {                                             // 0x80069B48
        uint32_t r = 0;
        if (!Call(c, kRpStrChr, {line, sep & 0xFFu}, F, &r)) return false;
        if (r == 0u) {
            v0 = 0;
            return !g.Faulted();
        }
        s0 = r + 1u;
    }
    if (!Call(c, kRpParseIntFn, {s0, out}, F)) return false;               // 0x80069B6C (its v0 unused)
    v0 = s0;
    return !g.Faulted();
}

bool NumEntries(GuestRam& g, uint32_t text, uint32_t len, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRpNumEntriesFrame;
    g.W32(F + 272u, 0);
    if (S(len) > 0) {
        uint32_t s0 = text;
        for (;;) {                                                         // 0x80069BBC
            uint32_t r = 0;
            if (!Call(c, kRpReadLineFn, {F + 16u, 127, s0}, F, &r)) return false;
            if (r == 0u) break;
            g.W8(F + 144u, 0);
            if (!Call(c, kRpNextTokenFn, {F + 16u, F + 144u}, F)) return false;
            if (!Call(c, kRpStrNCmp, {F + 144u, 0x8005B958u, 13}, F, &r)) return false; // "[NUM_ENTRIES]"
            if (r == 0u) {
                if (!Call(c, kRpStrChr, {F + 16u, 61}, F, &r)) return false;
                if (r != 0u) {
                    if (!Call(c, kRpParseIntFn, {r + 1u, F + 272u}, F)) return false;
                    break;
                }
            }
            if (!Call(c, kRpStrLen, {F + 16u}, F, &r)) return false;       // 0x80069C20
            s0 += r + 1u;
            if (!(S(s0 - text) < S(len))) break;
        }
    }
    v0 = g.U32(F + 272u);
    return !g.Faulted();
}

bool FindRace(GuestRam& g, uint32_t text, uint32_t len, uint32_t race, uint32_t pStart, uint32_t sp, LoaderCallees& c,
              uint32_t& v0) {
    const uint32_t F = sp - kRpFindRaceFrame;
    uint32_t s1 = text, s3 = 0, s4 = 0, s5 = 0;
    g.W32(F + 332u, pStart);                                               // 0x80069CAC: a3's home slot
    if (S(len) > 0) {
        for (;;) {                                                         // 0x80069CB4
            uint32_t r = 0, tok = 0;
            if (!Call(c, kRpReadLineFn, {F + 16u, 127, s1}, F, &r)) return false;
            if (r == 0u) break;
            g.W8(F + 144u, 0);
            if (!Call(c, kRpNextTokenFn, {F + 16u, F + 144u}, F, &tok)) return false;
            if (!Call(c, kRpStrNCmp, {F + 144u, 0x8005B968u, 7}, F, &r)) return false; // "[BEGIN]"
            if (r == 0u) {
                s3 = s1;
            } else {
                if (!Call(c, kRpStrNCmp, {F + 144u, 0x8005B970u, 8}, F, &r)) return false; // "[RACEID]"
                if (r == 0u) {
                    if (!Call(c, kRpTokenIntFn, {tok, F + 16u, 61, F + 272u}, F)) return false;
                    if (g.U32(F + 272u) == race) s4 = 1;
                } else {
                    if (!Call(c, kRpStrNCmp, {F + 144u, 0x8005B97Cu, 5}, F, &r)) return false; // "[END]"
                    if (r == 0u && s4 != 0u) {
                        s5 = s1;                                           // 0x80069D40
                        break;
                    }
                }
            }
            if (!Call(c, kRpStrLen, {F + 16u}, F, &r)) return false;       // 0x80069D6C
            s1 += r + 1u;
            if (!(S(s1 - text) < S(len))) break;
        }
    }
    v0 = 0;
    if (s3 == 0u || s5 == 0u) return !g.Faulted();                        // 0x80069D8C
    g.W32(g.U32(F + 332u), s3);
    g.W32(g.U32(F + 336u), s5 - s3);                                       // the fifth argument, the caller's sp+16
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80069DEC
bool RouteHeader(GuestRam& g, uint32_t text, uint32_t len, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kRpHeaderFrame;
    constexpr uint32_t H = kRpHeader;
    if (!Call(c, kLdMemset, {H, 0, 40}, F)) return false;
    g.W32(H + 28u, text);
    g.W32(H + 32u, len);
    uint32_t r = 0;
    if (!Call(c, kRpNumEntriesFn, {text, len}, F, &r)) return false;
    g.W16(H + 8u, static_cast<uint16_t>(r));
    if (S(r << 16) > 0) {                                                  // 0x80069E48
        g.W32(F + 16u, H + 32u);
        const uint32_t race = g.U32(g.U32(kLdGameStatePtr) + 64u);
        if (!Call(c, kRpFindRaceFn, {text, len, race, H + 28u, H + 32u}, F)) return false;
    }
    const uint32_t s5 = g.U32(H + 28u), s4 = g.U32(H + 32u);
    if (S(s4) <= 0) return !g.Faulted();
    struct Key {
        uint32_t str, n, off;
    };
    static constexpr Key kKeys[5] = {{0x8005B984u, 10, 18},  // [RACEINTS]
                                     {0x8005B990u, 25, 10},  // [VEHICLE_DEFAULT_DENSITY]
                                     {0x8005B9ACu, 22, 12},  // [VEHICLE_DEFAULT_RATE]
                                     {0x8005B9C4u, 26, 14},  // [REACTIVE_DEFAULT_DENSITY]
                                     {0x8005B9E0u, 23, 16}}; // [REACTIVE_DEFAULT_RATE]
    uint32_t s2 = s5;
    for (;;) {                                                             // 0x80069E8C
        uint32_t tok = 0;
        if (!Call(c, kRpReadLineFn, {F + 24u, 127, s2}, F, &r)) return false;
        if (r == 0u) break;
        g.W8(F + 152u, 0);
        if (!Call(c, kRpNextTokenFn, {F + 24u, F + 152u}, F, &tok)) return false;
        for (const Key& k : kKeys) {
            if (!Call(c, kRpStrNCmp, {F + 152u, k.str, k.n}, F, &r)) return false;
            if (r != 0u) continue;
            if (!Call(c, kRpTokenIntFn, {tok, F + 24u, 61, F + 280u}, F)) return false;
            g.W16(H + k.off, g.U16(F + 280u));
            break;
        }
        if (!Call(c, kRpStrLen, {F + 24u}, F, &r)) return false;          // 0x80069FD0
        s2 += r + 1u;
        if (!(S(s2 - s5) < S(s4))) break;
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006A014
bool LinkFill(GuestRam& g, uint32_t link, uint32_t sp, LoaderCallees& c) {
    (void)sp;
    (void)c;
    if (g.S16(0x800D6182u) == -1) return !g.Faulted();
    const uint32_t id = g.U32(link);
    if (id == 0xFFFFFFFFu) return !g.Faulted();
    const uint32_t road = g.U32(g.U32(g.gp() + kRpGraphGp) + 24u) + (id << 4); // SLUS 0x800245DC, a leaf
    const uint32_t length = g.U32(road + 4u);
    g.W16(link + 12u, 0xFFFF);
    g.W16(link + 14u, 0xFFFF);
    g.W32(link + 8u, (length >> 6) << 16);
    auto node = [&](uint32_t n) { return g.U32(g.U32(g.gp() + kRpGraphGp) + 20u) + n * 40u; }; // SLUS 0x800245F4
    if (node(g.U32(road + 8u)) != 0u) g.W16(link + 12u, g.U16(road + 8u));
    if (node(g.U32(road + 12u)) != 0u) g.W16(link + 14u, g.U16(road + 12u));
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006A0C8
bool RouteParse(GuestRam& g, uint32_t text, uint32_t len, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kRpParseFrame;
    constexpr uint32_t H = kRpHeader;
    constexpr uint32_t kM1 = 0xFFFFFFFFu;
    uint32_t s3 = 0, s4 = 1;
    g.W32(F + 292u, 0);
    g.W32(F + 296u, 0);
    if (text == 0u) return !g.Faulted();
    if (!Call(c, kRpHeaderFn, {text, len}, F)) return false;               // 0x8006A108 (a0 / a1 passed through)
    g.W32(F + 300u, g.U32(H + 28u));
    const int32_t n = g.S16(H + 18u);
    g.W32(F + 304u, g.U32(H + 32u));
    const uint32_t size = n == -1 ? 28u : U(n + 1) * 120u + 56u;           // 0x8006A138
    uint32_t alloc = 0;
    if (!Call(c, kLdMalloc, {size, 0}, F, &alloc)) return false;
    g.W32(H, alloc);
    if (alloc == 0u) return false;                                         // 0x8006A170: `break 7` - the console stops
    uint32_t s7 = alloc;
    g.W32(H + 20u, s7);
    s7 += 28u;
    if (!Call(c, kLdMemset, {alloc, kM1, 28}, F)) return false;
    if (g.S16(H + 18u) != -1) {
        g.W32(H + 24u, s7);
        s7 += 28u;
        if (!Call(c, kLdMemset, {g.U32(H + 24u), kM1, 28}, F)) return false;
        g.W32(H + 36u, s7);
    }
    if (S(g.U32(F + 304u)) <= 0) return !g.Faulted();
    s7 = g.U32(F + 300u);
    const uint32_t line = F + 16u, tokb = F + 144u;
    auto ti = [&](uint32_t prev, uint32_t at, uint32_t sep, uint32_t out, uint32_t& v) {
        return Call(c, kRpTokenIntFn, {prev, at, sep, out}, F, &v);
    };
    for (;;) {                                                             // 0x8006A1CC
        uint32_t r = 0, s1 = 0;
        if (!Call(c, kRpReadLineFn, {line, 127, s7}, F, &r)) return false;
        if (r == 0u) break;
        g.W8(tokb, 0);
        if (!Call(c, kRpNextTokenFn, {line, tokb}, F, &s1)) return false;
        auto key = [&](uint32_t str, uint32_t k, bool& hit) {
            uint32_t x = 0;
            if (!Call(c, kRpStrNCmp, {tokb, str, k}, F, &x)) return false;
            hit = x == 0u;
            return true;
        };
        bool hit = false;
        if (!key(0x8005B9F8u, 7, hit)) return false;                       // "[START]"
        if (hit) {
            if (s4 == 0u) break;                                           // 0x8006A214: a second [START] ends the parse
            if (!ti(s1, line, 61, g.U32(H + 20u), s1)) return false;
            if (!ti(s1, s1, 32, F + 272u, s1)) return false;
            g.W32(g.U32(H + 20u) + 4u, g.U32(F + 272u) << 16);
            if (!ti(s1, s1, 32, g.U32(H + 20u) + 8u, s1)) return false;
            if (!ti(s1, s1, 32, g.U32(H + 20u) + 12u, r)) return false;
            s3 = g.U32(H + 36u) + U(static_cast<int32_t>(g.S16(H + 18u))) * 120u; // 0x8006A28C: the start record
            g.W32(s3 + 4u, U(-4096));
            g.W32(s3 + 8u, U(-4096));
            g.W32(s3, kM1);
            g.W32(s3 + 12u, 1);
            g.W32(s3 + 16u, 1);
            g.W32(s3 + 20u, g.U32(g.U32(H + 20u)));
            g.W32(s3 + 24u, g.U32(g.U32(H + 20u) + 8u));
            if (!Call(c, kRpLinkFillFn, {s3 + 20u}, F)) return false;
            for (uint32_t k = 0; k < 4u; ++k) {                            // 0x8006A2F0
                if (k > 0u) {
                    if (!Call(c, kLdMemset, {s3 + 20u + 16u * k, 0, 16}, F)) return false;
                    g.W32(s3 + 16u * k + 20u, kM1);
                }
                g.W32(s3 + 4u * k + 84u, kM1);
                g.W32(s3 + 4u * k + 100u, kM1);
            }
            const uint32_t road = g.U32(g.U32(H + 20u));
            g.W16(s3 + 116u, 0xFFFF);
            g.W16(s3 + 118u, 0);
            g.W32(s3 + 84u, road);
            g.W32(s3 + 100u, g.U32(g.U32(H + 20u) + 12u));
            s4 = 0;
            if (g.S16(H + 18u) == -1) break;                               // 0x8006A360
        } else {
            if (!key(0x8005BA00u, 8, hit)) return false;                   // "[FINISH]"
            if (hit) {
                if (!ti(s1, line, 61, g.U32(H + 24u), s1)) return false;
                if (!ti(s1, s1, 32, F + 272u, s1)) return false;
                g.W32(g.U32(H + 24u) + 4u, g.U32(F + 272u) << 16);
                if (!ti(s1, s1, 32, g.U32(H + 24u) + 8u, s1)) return false;
                if (!ti(s1, s1, 32, g.U32(H + 24u) + 12u, r)) return false;
            } else {
                bool checker = false;
                uint32_t end = 0;
                if (!key(0x8005BA0Cu, 15, hit)) return false;              // "[START_CHECKER]"
                if (hit) {
                    checker = true;
                    end = H + 20u;
                } else {
                    if (!key(0x8005BA1Cu, 16, hit)) return false;          // "[FINISH_CHECKER]"
                    if (hit) {
                        checker = true;
                        end = H + 24u;
                    }
                }
                if (checker) {
                    if (!ti(s1, line, 61, g.U32(end) + 16u, s1)) return false;
                    if (!ti(s1, s1, 32, g.U32(end) + 20u, s1)) return false;
                    if (!ti(s1, s1, 32, g.U32(end) + 24u, r)) return false;
                } else {
                    if (!key(0x8005BA30u, 8, hit)) return false;           // "[RMAGIC]"
                    if (hit) {
                        if (!ti(s1, line, 61, H + 4u, r)) return false;
                    } else {
                        if (!key(0x8005BA3Cu, 8, hit)) return false;       // "[GMAGIC]" (read, not kept)
                        if (hit) {
                            if (!ti(s1, line, 61, F + 272u, r)) return false;
                        } else {
                            if (!key(0x8005B984u, 10, hit)) return false;  // "[RACEINTS]"
                            if (hit) {
                                g.W32(F + 292u, 1);
                                g.W32(F + 296u, U(static_cast<int32_t>(g.S16(H + 18u))));
                                s3 = g.U32(H + 36u);
                            } else if (g.U32(F + 292u) == 1u) {            // 0x8006A578: a route record
                                if (!Call(c, kLdMemset, {s3, kM1, 120}, F)) return false;
                                if (!ti(s1, tokb, 0, s3, s1)) return false;
                                if (g.U32(s3) == kM1) {
                                    g.W32(F + 292u, 0);                    // 0x8006A760
                                } else {
                                    if (!ti(s1, line, 32, F + 272u, s1)) return false;
                                    g.W32(s3 + 4u, g.U32(F + 272u) << 12);
                                    if (!ti(s1, s1, 32, F + 272u, s1)) return false;
                                    g.W32(s3 + 12u, 0);
                                    g.W32(s3 + 8u, g.U32(F + 272u) << 12);
                                    for (uint32_t k = 0; k < 4u; ++k) {    // 0x8006A610: the links, packed
                                        if (!ti(s1, s1, 32, F + 276u, s1)) return false;
                                        if (!ti(s1, s1, 32, F + 280u, s1)) return false;
                                        const uint32_t road = g.U32(F + 276u);
                                        if (road == kM1) continue;
                                        g.W32(s3 + (g.U32(s3 + 12u) << 4) + 20u, road);
                                        g.W32(s3 + (g.U32(s3 + 12u) << 4) + 24u, g.U32(F + 280u));
                                        if (!Call(c, kRpLinkFillFn, {s3 + (g.U32(s3 + 12u) << 4) + 20u}, F)) return false;
                                        g.W32(s3 + 12u, g.U32(s3 + 12u) + 1u);
                                    }
                                    g.W32(s3 + 16u, 0);
                                    for (uint32_t k = 0; k < 4u; ++k) {    // 0x8006A6B0: the route roads, packed
                                        if (!ti(s1, s1, 32, F + 284u, s1)) return false;
                                        const uint32_t v = g.U32(F + 284u);
                                        if (v == kM1) continue;
                                        g.W32(s3 + (g.U32(s3 + 16u) << 2) + 84u, v);
                                        g.W32(s3 + 16u, g.U32(s3 + 16u) + 1u);
                                    }
                                    uint32_t s0 = s3;
                                    for (uint32_t k = 0; k < 4u; ++k) {    // 0x8006A70C: the next nodes, packed
                                        if (!ti(s1, s1, 32, F + 288u, s1)) return false;
                                        const uint32_t v = g.U32(F + 288u);
                                        if (v == kM1) continue;
                                        g.W32(s0 + 100u, v);
                                        s0 += 4u;
                                    }
                                    g.W16(s3 + 116u, 0);
                                    g.W16(s3 + 118u, 0);
                                    const uint32_t t0 = g.U32(F + 296u) - 1u;
                                    s3 += 120u;
                                    g.W32(F + 296u, t0);
                                    if (t0 == 0u) g.W32(F + 292u, 0);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!Call(c, kRpStrLen, {line}, F, &r)) return false;              // 0x8006A764
        s7 += r + 1u;
        if (!(S(s7 - g.U32(F + 300u)) < S(g.U32(F + 304u)))) break;
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006A9F4 / 0x8006ABC8
bool MapBlocks(GuestRam& g, uint32_t base, uint32_t len, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRpMapBlocksFrame;
    uint32_t s1 = 0, s0 = base;
    const uint32_t s4 = base + len;
    struct Tab {
        uint32_t tag, ptr, count;
        int kind; // 0: >> 5, 1: >> 3, 2: / 104, 3: / 12
    };
    static constexpr Tab kTabs[6] = {{0x8005BA60u, 28, 40, 0}, {0x8005BA68u, 32, 42, 1}, {0x8005BA70u, 36, 44, 2},
                                     {0x8005BA78u, 48, 60, 3}, {0x8005BA80u, 52, 62, 3}, {0x8005BA88u, 56, 64, 3}};
    for (uint32_t w = 0; s0 < s4; ++w) {                                   // 0x8006AA34
        const uint32_t s3 = s0;
        const uint32_t s2 = g.U32(s0 + 4u) - 8u;
        uint32_t r = 0;
        if (!Call(c, kRpStrNCmp, {s0, 0x8005BA58u, 4}, F, &r)) return false; // "MAP_"
        if (r == 0u) {
            s1 = s0 + 8u;
        } else {
            for (const Tab& t : kTabs) {
                if (!Call(c, kRpStrNCmp, {s0, t.tag, 4}, F, &r)) return false;
                if (r != 0u) continue;
                uint32_t count = 0;
                if (t.kind == 0) count = s2 >> 5;
                else if (t.kind == 1) count = s2 >> 3;
                else if (t.kind == 2) count = static_cast<uint32_t>((static_cast<uint64_t>(s2) * 0x4EC4EC4Fu) >> 32) >> 5;
                else count = static_cast<uint32_t>((static_cast<uint64_t>(s2) * 0xAAAAAAABu) >> 32) >> 3;
                g.W32(s1 + t.ptr, s0 + 8u);
                g.W16(s1 + t.count, static_cast<uint16_t>(count));
                break;
            }
        }
        const uint32_t step = g.U32(s3 + 4u);
        if (step == 0u || w >= kWalk || g.Faulted()) return false;         // the console would not return
        s0 += step;
    }
    v0 = s1;
    return !g.Faulted();
}

bool MapLoad(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kRpMapLoadFrame;
    v0 = 0;
    if (!Call(c, kLdSprintf, {F + 24u, 0x8005BA90u, name, g.U32(g.U32(kLdGameStatePtr) + 48u)}, F)) return false; // "%s%d.MAP"
    g.W32(F + 16u, 0);
    uint32_t r = 0;
    if (!Call(c, kLdLoadFile, {F + 24u, g.U32(0x8005AD5Cu), kRpMapFile, F + 152u, 0}, F, &r)) return false;
    if (S(r) < 0) return false;                                            // 0x8006AC1C: `break 7` - the console stops
    if (!Call(c, kRpMapBlocksFn, {g.U32(kRpMapFile), g.U32(F + 152u)}, F, &r)) return false;
    g.W32(kRpMapGraph, r);
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ the product's callees
bool RouteCallees::Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) {
    v0 = 0;
    switch (fn) {
    case kRpReadLineFn: ++ported; return ReadLine(g_, a[0], a[1], a[2], sp, *this, v0);
    case kRpParseIntFn: ++ported; return ParseInt(g_, a[0], a[1], sp, *this, v0);
    case kRpNextTokenFn: ++ported; return NextToken(g_, a[0], a[1], sp, *this, v0);
    case kRpTokenIntFn: ++ported; return TokenInt(g_, a[0], a[1], a[2], a[3], sp, *this, v0);
    case kRpNumEntriesFn: ++ported; return NumEntries(g_, a[0], a[1], sp, *this, v0);
    case kRpFindRaceFn:
        ++ported;
        if (n > 4) g_.W32(sp + 16u, a[4]);
        return FindRace(g_, a[0], a[1], a[2], a[3], sp, *this, v0);
    case kRpHeaderFn: ++ported; return RouteHeader(g_, a[0], a[1], sp, *this);
    case kRpLinkFillFn: ++ported; return LinkFill(g_, a[0], sp, *this);
    case kRpParseFn: ++ported; return RouteParse(g_, a[0], a[1], sp, *this);
    case kRpMapBlocksFn: ++ported; return MapBlocks(g_, a[0], a[1], sp, *this, v0);
    case kRpMapLoadFn: ++ported; return MapLoad(g_, a[0], sp, *this, v0);
    case kL2RoadTextFn: ++ported; v0 = 1; return RoadText(g_, a[0], sp, *this);     // loader2.h
    case kL2RoadClearFn: ++ported; return RoadClear(g_, sp, *this);
    case kL2RoadRecordsFn: ++ported; v0 = 1; return RoadRecords(g_, a[0], sp, *this);
    case kRpStrNCmp: ++bios; v0 = BiosStrNCmp(g_, a[0], a[1], a[2]); return !g_.Faulted();
    case kRpStrLen: ++bios; v0 = BiosStrLen(g_, a[0]); return !g_.Faulted();
    case kRpStrNCpy: ++bios; v0 = BiosStrNCpy(g_, a[0], a[1], a[2]); return !g_.Faulted();
    case kRpStrChr: ++bios; v0 = BiosStrChr(g_, a[0], a[1]); return !g_.Faulted();
    case kRpAtoi:
        ++bios;
        if (!BiosAtoi(g_, a[0], v0)) {
            error = "atoi met a byte >= 0x80 (the kernel's ctype table's upper half is not served)";
            return false;
        }
        return !g_.Faulted();
    default:
        ++forwarded;
        return next_.Call(fn, a, n, sp, v0);
    }
}

} // namespace rr::sim
