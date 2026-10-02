#include "game/sim/grid_build.h"

#include "game/sim/coll_util.h"   // GMulAdd (SLUS 0x8002EAD8)
#include "game/sim/fixed.h"       // RatAtan2 (SLUS 0x80020018)
#include "game/sim/population.h"  // RoadGate 0x80039DFC, CursorSeat 0x8003A700
#include "game/sim/rider_record.h" // RiderRecordInit 0x80064C0C, RiderNameId 0x80066E1C

namespace rr::sim {
namespace {

constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }

constexpr uint32_t kGs = 0x8005B2F8;           // -> game_state
constexpr uint32_t kP1Bike = 0x8005B38C, kP1Bike2 = 0x8005B268; // player 1's bike (two words)
constexpr uint32_t kP2Bike = 0x8005B21C, kP2Bike2 = 0x8005B26C; // player 2's bike (two words)
constexpr uint32_t kPool0 = 0x8005B3A0, kPool1 = 0x8005B3A4;    // the pools' slot 0
constexpr uint32_t kPool0Count = 0x8005B1F8, kPool0High = 0x8005AD38;
constexpr uint32_t kPool1Count = 0x8005B218, kPool1High = 0x8005AD3C;
constexpr uint32_t kHandleToAi = 0x800D38B0, kAiToHandle = 0x800D38C8, kRiderRecs = 0x800D5758;
constexpr uint32_t kClassTable = 0x8006B8A0;   // RASHCDI data: a racer's grid class bit by handle
constexpr uint32_t kStatArray = 0x8005B248;
constexpr uint32_t kPlayerRecs = 0x800D81D8;   // 36 bytes per player
constexpr uint32_t kSessionByte = 0x800D80F5;  // the session record's handicap byte
constexpr uint32_t kCopFlags = 0x8005AD48;     // bit 0: player 1 is the police (0x800668B4)
constexpr uint32_t kSinCos = 0x8005624C;       // SLUS {s16 sin, s16 cos} x 4096
constexpr uint32_t kAtanTable = 0x8005285C;    // SLUS: RatAtan2's 18 words
constexpr uint32_t kAnimDesc = 0x800CE170, kAnimBanks = 0x800CE190;
constexpr uint32_t kAnimPOn = 0x8005B254;      // the police animation bank is wanted
constexpr uint32_t kDataDir = 0x8005243C;      // SLUS "DATA\"
constexpr uint32_t kWeaponName = 0x80052DE0;   // SLUS "DATA\WEAPONS..."
constexpr uint32_t kPhExt = 0x8005B8E4;        // RASHCDI "PH" (3 bytes)
constexpr uint32_t kWeaponExt = 0x8005B8E8;    // RASHCDI "." (the dot of 0x800665B8)
constexpr uint32_t kWeaponObjs = 0x800CF018;   // 8 x 172-byte weapon objects
constexpr uint32_t kBikeNames = 0x8006B51C;    // RASHCDI: 9-byte bike names (BikeName table 0)
constexpr uint32_t kAppearance = 0x8006B89B;   // RASHCDI: a player's appearance byte, stride 4
constexpr uint32_t kPoolTable = 0x800CE4D0;
constexpr uint32_t kGridStart = kGridStartRecord, kGridCursor = kGridStartCursor;
// The anim bank file names (SLUS data).
constexpr uint32_t kAnimTbl2 = 0x80052E44, kAnimTbj3 = 0x80052E6C, kAnimTbl3 = 0x80052E58;
constexpr uint32_t kAnimTbsb = 0x80052E94, kAnimTbs1 = 0x80052E30, kAnimTbsw = 0x80052E08;
constexpr uint32_t kAnimTblj = 0x80052ED0, kAnimTbls = 0x80052EBC;
constexpr uint32_t kAnimTblb = 0x80052E80, kAnimTbl1 = 0x80052E1C, kAnimTblw = 0x80052DF4;
constexpr uint32_t kAnimTblp = 0x80052EA8;

// The five list heads GridLists sets up (0x80065468), each {first, last}.
constexpr uint32_t kListRiding = 0x8005B298, kListDown = 0x8005B350, kListThrown = 0x8005B2D8;
constexpr uint32_t kListSpin = 0x8005B378, kListDormant = 0x8005B270;

// SLUS 0x8001E100 memset(dst, c, n): the byte replicated into a word, n counted down by 4.
void FillWords(GuestRam& g, uint32_t dst, uint32_t c, uint32_t n) {
    const uint32_t w = (c & 0xFFu) * 0x01010101u;
    while (n != 0u && !g.Faulted()) {
        g.W32(dst, w);
        dst += 4u;
        n -= 4u;
    }
}
// SLUS 0x8001E0DC: byte fill.
void FillBytes(GuestRam& g, uint32_t dst, uint32_t c, uint32_t n) {
    while (n != 0u && !g.Faulted()) {
        g.W8(dst, static_cast<uint8_t>(c));
        ++dst;
        --n;
    }
}
// SLUS 0x8001E0B4 memcpy by words (n counted down by 4).
void CopyWords(GuestRam& g, uint32_t dst, uint32_t src, uint32_t n) {
    while (n != 0u && !g.Faulted()) {
        g.W32(dst, g.U32(src));
        dst += 4u;
        src += 4u;
        n -= 4u;
    }
}
// v - (v >> 7) - (v >> 6): the grid's 125/128 (0x8006672C / 0x800667DC / 0x80066BA8).
uint32_t Scale125(uint32_t v) { return v - U(S(v) >> 7) - U(S(v) >> 6); }

// AllocBike RASHCDI 0x80065974(a0): pool-0 slot a0 (the count when a0 < 0), its handle; the count and the
// high index move when a0 < the count. 0 when the capacity byte is reached (signed bytes).
uint32_t AllocBike(GuestRam& g, int32_t a0) {
    if (!(g.S8(kGridPoolBytes + 1u) < g.S8(kGridPoolBytes))) return 0;
    const int32_t a1 = a0 < 0 ? g.S32(kPool0Count) : a0;
    const int32_t count = g.S32(kPool0Count);
    const uint32_t e = U(a1) * 1096u + g.U32(kPool0);
    g.W16(e + 172u, static_cast<uint16_t>(a1));
    if (a0 < count) {
        g.W32(kPool0Count, U(count) + 1u);
        g.W32(kPool0High, U(a1));
    }
    g.W8(kGridPoolBytes + 1u, static_cast<uint8_t>(g.U8(kGridPoolBytes + 1u) + 1u));
    return e;
}
// AllocRider RASHCDI 0x80065A04(a0): pool-1 slot a0 (the rider count when a0 < 0), handle a0 + 32.
uint32_t AllocRider(GuestRam& g, int32_t a0) {
    if (!(g.S8(kGridPoolBytes + 3u) < g.S8(kGridPoolBytes + 2u))) return 0;
    if (a0 < 0) a0 = g.S32(kPool1Count);
    g.W32(kPool1High, U(a0));
    const uint32_t r = U(a0) * 628u + g.U32(kPool1);
    g.W16(r + 172u, static_cast<uint16_t>(U(a0) + 32u));
    g.W32(kPool1Count, g.U32(kPool1Count) + 1u);
    g.W8(kGridPoolBytes + 3u, static_cast<uint8_t>(g.U8(kGridPoolBytes + 3u) + 1u));
    return r;
}
uint32_t ClassNibble(GuestRam& g, uint32_t e) { return g.U8(g.U32(e + 1084u) + 1u) & 0xFu; }
void Neg32(GuestRam& g, uint32_t a) { g.W32(a, 0u - g.U32(a)); }
void Neg16(GuestRam& g, uint32_t a) { g.W16(a, static_cast<uint16_t>(0u - g.U16(a))); }
void SetBits12(GuestRam& g, uint32_t a, uint32_t v) { g.W32(a, (g.U32(a) & 0xFFFC0FFFu) | ((v & 0x3Fu) << 12)); }

} // namespace

int32_t GridPlayerColumn(GuestRam& g) {                                    // RASHCDI 0x80065648
    const uint32_t gs = g.U32(kGs);
    const uint32_t t = U(static_cast<int32_t>(g.S16(gs + 58u)) << 2) & 31u;
    const int32_t d = static_cast<int32_t>((0x8532u >> t) & 15u);
    const int32_t w = g.S32(gs + 68u);
    const int32_t q = d == 0 ? (w >= 0 ? -1 : 1) : w / d;               // `div`, a zero divisor as the R3000
    const int32_t cap = static_cast<int32_t>((291u >> t) & 15u);
    return static_cast<int32_t>((328u >> t) & 15u) - (q < cap ? q : cap);
}

void GridLists(GuestRam& g) {                                              // RASHCDI 0x80065468
    for (const uint32_t h : {kListRiding, kListDown, kListThrown, kListSpin, kListDormant}) {
        g.W32(h, h);
        g.W32(h + 4u, h);
    }
}

int32_t StartRecord(GuestRam& g, uint32_t road, uint32_t along, uint32_t dir, uint32_t sp) {   // 0x8006ACD0
    const uint32_t f = sp - kStartRecordFrame;
    g.W32(kGridStart + 8u, along);
    g.W32(kGridStart, road & 0xFFFFu);
    g.W32(kGridStart + 4u, dir);
    const uint32_t obj = RoadGate(g, 0, kGridStart);                    // 0x8006ACF8
    return CursorSeat(g, obj, kGridStart, kGridCursor, f);              // 0x8006AD0C
}

// ============================================================================ SpawnBike 0x80065A94
uint32_t SpawnBike(GuestRam& g, uint32_t entry, uint32_t bi, uint32_t slot, uint32_t mask, uint32_t flags,
                   uint32_t sp, SpawnCallees& c, bool& ok) {
    ok = true;
    const uint32_t F = sp - kSpawnBikeFrame;
    auto fail = [&]() {
        ok = false;
        return 0u;
    };
    uint32_t s7 = 0;                                                     // the "other class" flag
    int32_t s1 = -1;                                                     // the explicit pool index
    g.W32(F + 200u, 0);                                                  // 0x80065AE4
    if (flags & 4u) s1 = S(flags) >> 4;
    const uint32_t e = AllocBike(g, s1);                                 // 0x80065AEC
    g.W32(e + 1088u, e + 1088u);                                         // (stored before the test)
    if (e == 0u) return g.Faulted() ? fail() : 0u;
    if (flags & 1u) {
        g.W32(kP1Bike, e);
        g.W32(kP1Bike2, e);
    }
    if (flags & 8u) {
        g.W32(kP2Bike, e);
        g.W32(kP2Bike2, e);
    }
    const uint32_t R = AllocRider(g, s1);                                // 0x80065B3C
    if (R == 0u) return g.Faulted() ? fail() : 0u;
    g.W32(R + 596u, e);
    g.W32(e + 852u, R);
    uint32_t gs = g.U32(kGs);
    uint32_t ai = 0, rec = 0;                                            // s0, a1
    if (flags & 9u) {                                                    // a player: 0x80065B60
        ai = (flags >> 3) & 1u;
        const int32_t w = g.S32(g.U32(kGs) + ai * 4u + 72u);
        const uint32_t cls = U(w / 9);                                   // the 0x38E38E39 idiom, signed
        if (cls == 2u) {
            rec = 26;
        } else {
            const int32_t b = g.S8(kPlayerRecs + ai * 36u + 10u);
            rec = b > 0 ? U(b - 1) * 2u + cls + 22u : cls;
        }
    } else if (!(slot < 17u)) {                                          // police: 0x80065BF4
        rec = slot + 1u;
        ai = rec;
    } else {                                                             // a racer: 0x80065C00
        uint32_t s0 = slot, col, cb;
        for (;;) {
            const uint32_t m = g.U32(mask);
            col = (s0 + 1u) >> 1;
            cb = (s0 + 1u) & 1u;
            if (m & (1u << ((col + cb * 10u) & 31u))) {
                do {
                    ++col;
                } while (m & (1u << ((col + cb * 10u) & 31u)));
            }
            if (S(col) < 9) break;
            ++s0;
        }
        g.W32(mask, g.U32(mask) | (1u << ((col + cb * 10u) & 31u)));   // 0x80065C90
        g.W8(kClassTable + g.U16(e + 172u), static_cast<uint8_t>(cb));
        ai = col + cb * 8u + 1u;                                         // 0x80065CB4
        uint32_t s2 = cb;
        const uint32_t type = g.U8(gs + 4u);
        if (type == 33u) {
            s2 = g.U32(g.U32(kP1Bike) + 180u) < 9u ? 1u : 0u;
        } else if ((type & 1u) && !(g.S32(kPool0Count) < 3)) {           // 0x80065CFC
            uint32_t k = 1;
            if (ClassNibble(g, g.U32(kP1Bike)) != 2u) k = g.U32(gs + 48u) < 2u ? 1u : 0u;
            s2 = ClassNibble(g, g.U32(kPool0) + k * 1096u);
        } else if ((g.U8(g.U32(kGs) + 4u) & 4u) && g.U8(g.U32(kGs) + 4u) != 44u) {   // 0x80065D54
            s2 = ClassNibble(g, g.U32(kP1Bike));
        }
        rec = col + s2 * 8u + 1u;                                        // 0x80065D98
    }
    gs = g.U32(kGs);
    g.W8(kHandleToAi + g.U16(e + 172u), static_cast<uint8_t>(ai));       // 0x80065DC4
    g.W8(kAiToHandle + ai, g.U8(e + 172u));
    g.W32(e + 1084u, kRiderRecs + ai * 72u);
    RiderRecordInit(g, bi, S(rec), e);                                   // 0x80065DEC
    if (g.Faulted()) return fail();
    gs = g.U32(kGs);
    {
        const uint32_t type = g.U8(gs + 4u);
        if ((type == 33u || type == 44u) && ClassNibble(g, e) != ClassNibble(g, g.U32(kP1Bike))) {   // 0x80065E18
            s7 = 1;
            SetBits12(g, e + 36u, g.U8(bi + 1206u));
            const uint32_t r = g.U32(e + 852u);
            SetBits12(g, r + 36u, g.U8(bi + 1207u));
            CopyWords(g, e + 76u, bi + 1208u, 4u);
            CopyWords(g, g.U32(e + 852u) + 76u, bi + 1212u, 4u);
        }
    }
    g.W8(g.U32(e + 1084u) + 38u, static_cast<uint8_t>(RiderNameId(g, e)));   // 0x80065EA8
    if (g.Faulted()) return fail();
    gs = g.U32(kGs);
    if (flags & 9u) {                                                    // a player's appearance, 0x80065EC0
        const uint32_t type = g.U8(gs + 4u);
        if (!(type & 1u) || type == 33u) {
            const uint32_t a = g.U8(kAppearance + ((flags ^ 1u) & 1u) * 4u);   // 0x8005DD9C (lbu)
            if (a != 0xFFFFFFFFu) {                                      // (a byte: never -1)
                SetBits12(g, e + 36u, a);
                SetBits12(g, g.U32(e + 852u) + 36u, a + 1u);
            }
        }
    }
    if (g.U8(g.U32(kGs) + 4u) & 8u) {                                    // 0x80065F5C: the weapon strip
        const uint32_t rd = g.U32(e + 1084u);
        g.W16(rd + 44u, static_cast<uint16_t>(g.U16(rd + 44u) & 0xFECDu));
        const uint32_t wv = g.U8(g.U32(e + 1084u) + 46u);
        if (wv - 4u < 2u || wv == 1u || wv == 8u) {
            g.W8(g.U32(e + 1084u) + 46u, 9);
            g.W8(g.U32(e + 1084u) + 47u, 0);
        }
    }
    uint32_t s2 = 0, s4 = 0;                                             // the model class, the stat block
    if (flags & 9u) {                                                    // 0x80065FD4
        s2 = g.U32(g.U32(kGs) + ((flags & 1u) ^ 1u) * 4u + 72u);
        g.W8(g.U32(e + 1084u) + 1u, static_cast<uint8_t>(static_cast<uint32_t>((static_cast<uint64_t>(s2) * 0x38E38E39ull) >> 32) >> 1));
        int32_t col = GridPlayerColumn(g);                               // 0x80066004
        {
            const uint32_t rd = g.U32(e + 1084u);
            g.W8(rd + 1u, static_cast<uint8_t>(g.U8(rd + 1u) | (U(col) << 4)));
        }
        if (col < 2) col = 8;
        if ((flags & 8u) && ClassNibble(g, e) == ClassNibble(g, g.U32(kP1Bike)))
            col = (col == 5 || col == 2) ? col + 1 : col - 1;
        s4 = 3;
        g.W32(mask, g.U32(mask) | (1u << ((U(col) + g.U8(g.U32(e + 1084u) + 1u) * 10u) & 31u)));   // 0x800660A8
        if (flags & 8u) s4 = 4;
        if (!c.StrCpy(F + 16u, kDataDir, F)) return fail();             // 0x800660C8
        uint32_t idx = s2;
        gs = g.U32(kGs);
        if (!(s2 < 9u) && (g.U8(gs + 4u) & 8u) && g.U8(gs + g.U16(e + 172u) + 10u) != 1u) idx = s2 - 9u;
        if (!c.StrCat(F + 16u, kBikeNames + idx * 9u, F)) return fail();   // BikeName 0x80064034(0, idx)
    } else {                                                             // 0x80066138
        const uint32_t b1 = g.U8(g.U32(e + 1084u) + 1u);
        const uint32_t a1 = b1 & 0xFu;
        const uint32_t cls = a1 + ((0u - s7) & (2u - a1));
        uint32_t col2 = 0;
        if (S(cls) < 2) col2 = (b1 >> 4) < 2u ? 0u : 1u;
        s4 = a1;
        s2 = cls * 9u + col2 * 3u + g.U32(g.U32(kGs) + 60u);
    }
    g.W32(e + 556u, g.U32(kStatArray) + s4 * 448u);                      // 0x800661B0
    if (!(g.U32(kGridStatLoaded) & (1u << (s4 & 31u)))) {               // a player's .PH: 0x800661D0
        uint32_t len = 0;
        if (!c.StrLen(F + 16u, F, len)) return fail();
        g.W8(F + 16u + len, '.');
        g.W8(F + 144u, g.U8(kPhExt));
        g.W8(F + 145u, g.U8(kPhExt + 1u));
        g.W8(F + 146u, g.U8(kPhExt + 2u));
        g.W8(len + F + 16u + 1u, 0);
        if (!c.StrCat(F + 16u, F + 144u, F)) return fail();
        if (!c.LoadBikePh(F + 16u, g.U32(e + 556u), F)) return fail();
    }
    if (flags & 9u) {                                                    // 0x80066230
        if (g.U8(g.U32(kGs) + 4u) & 1u) g.W32(e + 560u, g.U32(e + 560u) | 0x08000000u);
        if (g.U8(g.U32(kGs) + 4u) & 0x20u) g.W8(g.U32(e + 556u) + 445u, 0);
        const uint32_t a0 = g.U8(g.U32(e + 556u) + 445u);
        g.W8(e + 848u, static_cast<uint8_t>(a0));
        const uint32_t v1 = (flags & 8u) ? 36u : 0u;
        g.W8(e + 848u, static_cast<uint8_t>(a0 + g.U8(kPlayerRecs + v1 + 20u) + g.U8(kSessionByte)));
    } else {                                                             // 0x800662CC
        const uint32_t v = g.U32(e + 560u) | 0x08000000u;
        g.W32(e + 560u, v);
        if (flags & 4u) g.W32(e + 560u, v | 0x10000000u);
    }
    g.W32(kGridStatLoaded, g.U32(kGridStatLoaded) | (1u << (s4 & 31u)));   // 0x80066318
    {
        uint32_t v0 = 0;
        if (!c.ModelBind(e, 2, s2, 1, F, v0)) return fail();            // 0x80066314
        g.W32(e + 180u, v0);
    }
    if (!c.BoxSetup(e, 0, F)) return fail();                             // 0x80066324
    {
        const uint32_t st = g.U32(g.U32(e + 556u));
        const uint32_t rm = g.U32(g.U32(e + 852u) + 316u);
        g.W32(e + 540u, 0);
        g.W32(e + 316u, st + rm);                                        // 0x80066344
    }
    gs = g.U32(kGs);
    s7 &= 1u;
    if (g.U16(e + 172u) < g.U32(gs + 48u)) s7 = 0;                       // 0x80066360
    {
        const uint32_t b1 = g.U8(g.U32(e + 1084u) + 1u);
        const uint32_t a1 = b1 & 0xFu;
        const uint32_t rc = a1 + ((0u - s7) & (2u - a1));
        const uint32_t sh = (32u - ((b1 >> 4) << 2)) & 31u;
        const uint32_t a0 = U(S(0x01112222u) >> sh) & 0xFu;
        uint32_t col = 2u - a0;
        if (!(S(rc) < 2)) col = 0;
        const uint32_t cls = rc * 9u + (col << 1) + col + g.U32(g.U32(kGs) + 60u);
        uint32_t v0 = 0;
        if (!c.ModelBind(R, 1, cls, 1, F, v0)) return fail();           // 0x800663E8
        g.W32(R + 180u, v0);
    }
    if (!c.BoxSetup(R, 0, F)) return fail();                             // 0x800663F8
    if (!c.Attach(e, R, 2, 0, F)) return fail();                         // 0x8006640C
    if (flags & 1u) {                                                    // player 1's assets: 0x80066420
        uint32_t v0 = 0;
        if (!c.AnimBank(kAnimDesc, kAnimTbl2, F, v0)) return fail();
        g.W32(kAnimBanks + 4u, v0);
        if (!c.AnimBank(kAnimDesc, g.U8(g.U32(kGs) + 4u) == 44u ? kAnimTbj3 : kAnimTbl3, F, v0)) return fail();
        g.W32(kAnimBanks + 8u, v0);
        if (g.U8(g.U32(kGs) + 4u) & 8u) {
            if (!c.AnimBank(kAnimDesc, kAnimTbsb, F, v0)) return fail();
            g.W32(kAnimBanks + 16u, v0);
            if (!c.AnimBank(kAnimDesc, kAnimTbs1, F, v0)) return fail();
            g.W32(kAnimBanks, v0);
            if (!c.AnimBank(kAnimDesc, kAnimTbsw, F, v0)) return fail();
            g.W32(kAnimBanks + 12u, v0);
            if (!c.AnimBank(kAnimDesc, g.U8(g.U32(kGs) + 4u) == 44u ? kAnimTblj : kAnimTbls, F, v0)) return fail();
            g.W32(kAnimBanks + 24u, v0);
        } else {
            if (!c.AnimBank(kAnimDesc, kAnimTblb, F, v0)) return fail();
            g.W32(kAnimBanks + 16u, v0);
            if (!c.AnimBank(kAnimDesc, kAnimTbl1, F, v0)) return fail();
            g.W32(kAnimBanks, v0);
            if (!c.AnimBank(kAnimDesc, kAnimTblw, F, v0)) return fail();
            g.W32(kAnimBanks + 12u, v0);
            g.W32(kAnimBanks + 24u, 0);
        }
        if (g.U32(kAnimPOn) != 0u) {
            if (!c.AnimBank(kAnimDesc, kAnimTblp, F, v0)) return fail();
            g.W32(kAnimBanks + 20u, v0);
        } else {
            g.W32(kAnimBanks + 20u, 0);
        }
        uint32_t dot = 0;
        if (!c.StrRChr(kWeaponName, '.', F, dot)) return fail();         // 0x800665A4
        if (dot == 0u) {
            if (!c.StrCat(kWeaponName, kWeaponExt, F)) return fail();
            if (!c.StrCat(kWeaponName, F + 144u, F)) return fail();
        } else {
            uint32_t d = 0;
            if (!c.StrNCmp(dot + 1u, F + 144u, 3, F, d)) return fail();
            if (d != 0u && !c.StrCpy(dot + 1u, F + 144u, F)) return fail();
        }
        for (uint32_t k = 0; k < 8u; ++k)                                 // 0x80066608
            if (!c.ModelBind(kWeaponObjs + 172u * k, 5, 0, 1, F, v0)) return fail();
        if (!c.AnimNoise(F)) return fail();                              // 0x8006662C
    }
    gs = g.U32(kGs);
    {                                                                    // the countdown bits, 0x80066634
        const uint32_t rd = g.U32(e + 1084u);
        uint32_t v1 = 0;
        if (flags & 9u) v1 = (g.U8(gs + 4u) & 1u) ^ 1u;
        g.W8(rd, static_cast<uint8_t>(g.U8(rd) | ((0u - v1) & 0x60u)));
        if (g.U8(gs + 4u) == 33u && g.U16(e + 172u) < g.U32(gs + 48u))
            g.W8(g.U32(e + 1084u) + 1u, static_cast<uint8_t>((g.U8(g.U32(e + 1084u) + 1u) & 0xF0u) | 2u));
    }
    g.W32(e + 948u, 0);                                                  // 0x800666C4
    g.W32(e + 952u, 0);
    {
        const int32_t a = g.S32(entry + 4u);
        const uint32_t sg = U(a >> 31);
        if (S((sg + U(a)) ^ sg) < 131) g.W32(entry + 4u, 131);           // |along| at least 131 / 65536
    }
    const uint32_t cur = e + 328u, pos = e + 360u, key = F + 184u;
    CopyWords(g, cur, kGridCursor, 32u);                                 // 0x8006AD24(e + 0x148)
    CopyWords(g, pos, kGridStart, 12u);
    g.W8(e + 534u, 1);
    if (!c.RoadWalk(pos, key, S(Scale125(g.U32(entry + 4u))), F)) return fail();   // 0x80066738
    uint32_t obj = 0;
    if (!c.RoadGate(0, key, F, obj)) return fail();                      // 0x80066744
    bool placed = false;
    uint32_t lat = 0;
    if (obj == 0u) {                                                     // 0x80066B6C: not on a resident object
        CopyWords(g, pos, key, 12u);
        g.W16(e + 320u, 0);
        FillWords(g, cur, 0, 32u);
        g.W32(e + 348u, 0);
        lat = g.U32(entry);
        g.W32(e + 492u, 0);
        g.W32(e + 496u, 0);
        g.W32(e + 372u, 0);
        g.W32(e + 388u, 0);
    } else {
        if (g.U32(kGridStart) == g.U32(key)) {                           // 0x80066764
            Neg32(g, entry);
            Neg32(g, entry + 4u);
        }
        if (g.U32(entry + 8u) == 1u) {                                   // 0x8006678C
            Neg32(g, entry);
            Neg32(g, entry + 4u);
        }
        uint32_t seated = 0;
        if (!c.CursorSeat(obj, key, cur, F, seated)) return fail();      // 0x800667AC
        if (seated == 0u) {
            g.W32(e + 348u, 0);
            lat = g.U32(entry);
            g.W16(e + 320u, 0);
        } else {
            placed = true;
            const uint32_t s = g.U32(e + 340u);                           // the slice
            g.W32(entry, Scale125(g.U32(entry)));
            cu::GMulAdd(g, s + 20u, s + 14u, g.S32(e + 348u), e + 184u); // 0x800667F4
            cu::GMulAdd(g, e + 184u, s + 2u, g.S32(entry), e + 184u);   // 0x80066808
            const uint32_t v = g.U32(entry);
            const uint32_t px = g.U32(e + 184u), py = g.U32(e + 188u), pz = g.U32(e + 192u);
            g.W32(e + 340u, s);
            g.W16(e + 320u, 1);
            g.W32(e + 344u, v);
            g.W32(e + 504u, px);
            g.W32(e + 508u, py);
            g.W32(e + 512u, pz);
            g.W32(e + 468u, px);
            g.W32(e + 472u, py);
            g.W32(e + 476u, pz);
            CopyWords(g, F + 152u, kGridCursor, 32u);                    // 0x8006AD24(sp + 152)
            g.W32(F + 200u, 0);
            gs = g.U32(kGs);
            if ((g.U8(gs + 4u) & 1u) && g.U16(e + 172u) < g.U32(gs + 48u) && ClassNibble(g, e) == 2u) {   // 0x80066878
                g.W32(kCopFlags, g.U32(kCopFlags) | 1u);
                if (!c.RoadPosition(e + 450u, cur, pos, F)) return fail();
                if (!c.RoadClass(e, 1, 0, -1, F)) return fail();
                uint32_t v0 = 0;
                if (!c.PlayerCopPlace(e, F, v0)) return fail();
                g.W32(F + 200u, v0);
            }
            if (g.U32(F + 200u) == 0u) {                                  // the rows from the slice, 0x800668F8
                g.W16(e + 432u, g.U16(s + 2u));
                g.W16(e + 434u, g.U16(s + 4u));
                g.W16(e + 436u, g.U16(s + 6u));
                g.W16(e + 438u, g.U16(s + 8u));
                g.W16(e + 440u, g.U16(s + 10u));
                Neg16(g, e + 438u);
                const uint16_t n5 = g.U16(s + 12u);
                g.W16(e + 442u, n5);
                g.W16(e + 442u, static_cast<uint16_t>(0u - n5));
                Neg16(g, e + 440u);
                g.W16(e + 444u, g.U16(s + 14u));
                g.W16(e + 446u, g.U16(s + 16u));
                g.W16(e + 448u, g.U16(s + 18u));
                auto flip = [&]() {
                    for (const uint32_t o : {444u, 446u, 448u, 432u, 434u, 436u}) Neg16(g, e + o);
                };
                if (g.U32(kGridStart) == g.U32(key) && g.S32(kGridStart + 4u) < 0) flip();   // 0x80066980
                if (g.U32(entry + 8u) == 1u) flip();                                         // 0x800669F0
            }
            for (uint32_t k = 0; k < 9u; ++k) g.W16(e + 516u + 2u * k, g.U16(e + 432u + 2u * k));   // 0x80066A7C
            int32_t atan[18];
            for (uint32_t k = 0; k < 18u; ++k) atan[k] = g.S32(kAtanTable + 4u * k);
            const int32_t a = RatAtan2(S(U(static_cast<int32_t>(g.S16(e + 444u))) << 4),
                                       S(U(static_cast<int32_t>(g.S16(e + 448u))) << 4), atan);   // 0x80066A9C
            g.W32(e + 292u, U(a));
            const uint32_t i = U(a) & 0xFFFu;
            g.W32(e + 296u, U(static_cast<int32_t>(g.S16(kSinCos + ((i << 2) | 2u)))) << 4);
            g.W32(e + 300u, U(static_cast<int32_t>(g.S16(kSinCos + (i << 2)))) << 4);
            cu::GMulAdd(g, e + 184u, e + 528u, S(g.U32(e + 308u) << 1), e + 880u);   // 0x80066AF8: the aim point
            g.W16(e + 450u, g.U16(e + 444u));
            g.W16(e + 452u, g.U16(e + 446u));
            g.W16(e + 454u, g.U16(e + 448u));
            g.W16(e + 814u, g.U16(e + 432u));
            g.W16(e + 816u, g.U16(e + 434u));
            g.W16(e + 818u, g.U16(e + 436u));
            if (!c.RoadPosition(e + 450u, cur, pos, F)) return fail();   // 0x80066B38
            if (!c.RoadClass(e, 1, 0, -1, F)) return fail();             // 0x80066B4C
            if (!c.RoadsideRun(e, 1, -1, F)) return fail();              // 0x80066B5C
        }
    }
    if (!placed) g.W32(e + 344u, Scale125(lat));                         // 0x80066BA8
    g.W32(e + 924u, 0);                                                  // 0x80066BD0
    if (!c.RouteBind(e + 172u, 0, 0, F)) return fail();                  // 0x80066BCC
    {
        uint32_t v0 = 0;
        if (!c.Progress(e + 172u, F, v0)) return fail();                 // 0x80066BD4
        g.W32(e + 324u, v0);
    }
    gs = g.U32(kGs);
    g.W16(e + 956u, static_cast<uint16_t>(((g.U8(gs + 4u) & 1u) && g.U32(F + 200u) == 0u) ? 4u : 1u));   // 0x80066C10
    const int16_t live = g.S16(e + 320u);
    g.W16(e + 958u, 224);
    g.W8(e + 946u, 1);
    if (live != 0) {                                                     // the rider on the bike: 0x80066C2C
        CopyWords(g, R + 328u, e + 328u, 32u);
        g.W32(R + 184u, g.U32(e + 184u));
        g.W32(R + 188u, g.U32(e + 188u));
        g.W32(R + 192u, g.U32(e + 192u));
        g.W32(R + 472u, g.U32(R + 188u));
        g.W32(R + 468u, g.U32(R + 184u));
        g.W32(R + 476u, g.U32(R + 192u));
    }
    g.W16(R + 320u, g.U16(e + 320u));                                    // 0x80066C74
    for (const uint32_t o : {444u, 446u, 448u, 438u, 440u, 442u, 432u, 434u}) g.W16(R + o, g.U16(e + o));
    const uint16_t r436 = g.U16(e + 436u);
    g.W8(R + 571u, 0xFF);
    g.W8(R + 569u, 41);
    g.W8(R + 570u, 0);
    g.W8(R + 572u, 0);
    gs = g.U32(kGs);
    g.W32(R + 556u, 0);
    g.W32(R + 540u, 0);
    g.W16(R + 436u, r436);
    g.W32(R + 604u, (g.U8(gs + 4u) & 1u) ? 1u : 0u);                    // 0x80066D18
    g.W16(R + 544u, 224);
    gs = g.U32(kGs);
    g.W32(e + 176u, 0xFFFFFFFFu);
    g.W32(R + 176u, 0xFFFFFFFFu);
    if (g.U8(gs + 4u) != 33u) {                                          // 0x80066D5C: a police rider's weapon
        const uint32_t rd = g.U32(e + 1084u);
        const uint32_t cn = g.U8(rd + 1u) & 0xFu;
        if (cn == 2u && (g.U16(rd + 44u) & 4u)) g.W8(rd + 46u, static_cast<uint8_t>(cn));
    }
    gs = g.U32(kGs);
    if (!(g.U16(e + 172u) < g.U32(gs + 48u)) && ClassNibble(g, e) == 2u) {   // a police bike: 0x80066DCC
        if (!c.SaveCopRecord(e, F)) return fail();
        const uint8_t b = g.U8(e + 928u);
        g.W16(e + 320u, 0);
        g.W8(e + 928u, static_cast<uint8_t>(b | 0x30u));
    }
    g.W16(e + 870u, 0);                                                  // 0x80066DE4
    if (g.Faulted()) return fail();
    return e;
}

// ============================================================================ BuildGrid 0x80067B00
bool BuildGrid(GuestRam& g, uint32_t block, int32_t size, uint32_t sp, GridCallees& c) {
    const uint32_t F = sp - kBuildGridFrame;
    const uint32_t stage = F + 40u, bi = F + 256u, mask = F + 2304u;
    const uint8_t escaped = g.U8(kGridEscapeFlag);
    g.W32(mask, 0);                                                      // 0x80067B44
    if (escaped != 0u) return !g.Faulted();                              // 0x80067B40 -> 0x80068440
    g.W32(kP2Bike, 0);                                                   // 0x80067B60..
    g.W32(kP2Bike2, 0);
    g.W32(kP1Bike, 0);
    g.W32(0x8005B244u, 0);
    g.W32(kCopFlags, 0);
    g.W32(kP1Bike2, 0);
    g.W32(0x8005B230u, 0);
    FillWords(g, kAiToHandle, 31, 20);                                   // 0x80067B8C
    FillBytes(g, kHandleToAi, 31, 18);                                   // 0x80067BA0
    const uint32_t players = g.U32(g.U32(kGs) + 48u);
    const uint32_t blocks = players == 1u ? 4u : 5u;                     // s5
    uint32_t v0 = 0;
    if (!c.Malloc(blocks * 448u, 0, F, v0)) return false;                // 0x80067BD4
    g.W32(kStatArray, v0);
    GridLists(g);                                                        // 0x80067BE0
    if (!c.LoadLevelPh(g.U32(kStatArray), F)) return false;              // 0x80067BEC
    g.W32(kGridStatLoaded, (1u << ((blocks - g.U32(g.U32(kGs) + 48u)) & 31u)) - 1u);   // 0x80067C18
    if (!c.LoadLevelBi(bi, 2048, F)) return false;                       // 0x80067C14
    g.W32(kGridStatLoaded, g.U32(kGridStatLoaded) | (blocks << 24));
    uint32_t src = block;                                                // s4
    int32_t n = 0;                                                       // s5
    if (size != 0) {
        n = g.S32(src);
        src += 4u;
    }
    if (!(n < 19)) n = 18;
    {
        const uint32_t gs = g.U32(kGs);
        const uint32_t type = g.U8(gs + 4u);
        if (type & 4u) {                                                 // 0x80067C60
            const uint32_t b38 = g.U8(gs + 56u);
            if (b38 == 0u) n = (type & 8u) ? 2 : 1;
            else if (b38 == 2u) n = (type & 8u) ? 4 : 3;
        }
    }
    int32_t total;                                                       // s3
    {
        const uint32_t gs = g.U32(kGs);
        int32_t a0 = g.S32(gs + 48u);
        const uint32_t type = g.U8(gs + 4u);
        const int32_t pl = a0;
        if (type & 8u) a0 = S(U(a0) << 1);
        total = a0 < n ? n : a0;                                         // 0x80067CD0
        if (type & 8u) {
            const uint32_t m = pl == 1 ? 0xFFFFFFFFu : 0u;
            const uint32_t extra = type == 44u ? (m & 3u) : (m & 2u);
            const int32_t lim = S(extra + 12u);
            if (n < lim) total = S(U(total) + g.U32(g.U32(kGs) + 48u));
            const uint32_t cap = U(lim) + g.U32(g.U32(kGs) + 48u);
            if (!(U(total) < cap)) total = S(cap);                       // sltu
        }
    }
    const uint32_t bikeBytes = U(total) * 1096u, riderBytes = U(total) * 628u;
    g.W8(kGridPoolBytes, static_cast<uint8_t>(total));                   // 0x80067D74
    g.W8(kGridPoolBytes + 2u, static_cast<uint8_t>(total));
    if (!c.Malloc(bikeBytes, 0, F, v0)) return false;                    // 0x80067D7C
    g.W32(kPool0, v0);
    if (!c.Malloc(riderBytes, 0, F, v0)) return false;                   // 0x80067DAC
    g.W32(kPool1, v0);
    FillWords(g, g.U32(kPool0), 0, bikeBytes);                           // 0x80067DC8
    FillWords(g, g.U32(kPool1), 0, riderBytes);
    g.W32(kPoolTable, g.U32(kPool0));                                    // 0x80067DF8
    g.W32(kPoolTable + 16u, g.U32(kPool1));
    FillWords(g, stage, 0xFF, 216);                                      // 0x80067E04
    int32_t police = 0;                                                  // s6
    for (int32_t i = 0; i < n; ++i) {                                    // 0x80067E14
        const uint32_t a = stage + 12u * U(i);
        g.W32(a + 8u, g.U32(src));
        g.W32(a + 0u, g.U32(src + 4u));
        police += g.S32(a + 8u) < 17 ? 0 : 1;
        g.W32(a + 4u, g.U32(src + 8u));
        src += 12u;
    }
    {
        const uint32_t gs = g.U32(kGs);
        const uint32_t type = g.U8(gs + 4u);
        if (type == 17u) {                                               // 0x80067E74: swap entries 0 and 1's offsets
            if (!(g.S32(gs + 76u) < 18)) {
                const uint32_t a0 = g.U32(F + 40u), a1 = g.U32(F + 44u);
                const uint32_t b0 = g.U32(F + 52u), b1 = g.U32(F + 56u);
                g.W32(F + 24u, a0);
                g.W32(F + 28u, a1);
                g.W32(F + 40u, b0);
                g.W32(F + 44u, b1);
                g.W32(F + 52u, a0);
                g.W32(F + 56u, a1);
            }
        } else if ((type & 4u) && g.U8(gs + 56u) == 2u) {                // 0x80067EBC: entries 1 and 2 police
            g.W32(F + 60u, 17);
            g.W32(F + 72u, 18);
        } else if (g.U8(g.U32(kGs) + 4u) & 8u) {                         // 0x80067EF4: compact to 10 (+2 / +3) racers
            const uint32_t gs2 = g.U32(kGs);
            const uint32_t type2 = g.U8(gs2 + 4u);
            const uint32_t m = g.U32(gs2 + 48u) == 1u ? 0xFFFFFFFFu : 0u;
            const int32_t t3 = S(U(n - police) - 10u - (type2 == 44u ? (m & 3u) : (m & 2u)));
            if (t3 > 0) {
                n -= t3;
                int32_t s3 = g.S32(g.U32(kGs) + 48u);
                g.W32(kGridRacers, U(s3));
                int32_t a0 = s3;
                if (s3 < n) {
                    uint32_t a2 = stage + 12u * U(s3);
                    int32_t a3 = s3 + t3;
                    bool broke = false;
                    for (;;) {
                        const uint32_t gs3 = g.U32(kGs);
                        const uint32_t mm = g.U32(gs3 + 48u) == 1u ? 0xFFFFFFFFu : 0u;
                        const uint32_t lim = (g.U8(gs3 + 4u) == 44u ? (mm & 3u) : (mm & 2u)) + 10u;
                        if (!(g.S32(kGridRacers) < S(lim))) {
                            broke = true;
                            break;
                        }
                        if (g.S32(a2 + 8u) < 17) {                       // 0x80067FDC
                            int32_t j = 0;
                            while (!(g.S32(stage + 12u * U(a3 + j) + 8u) < 17)) ++j;
                            const uint32_t from = stage + 12u * U(a3 + j);
                            g.W32(a2, g.U32(from));
                            g.W32(kGridRacers, g.U32(kGridRacers) + 1u);
                            g.W32(a2 + 4u, g.U32(from + 4u));
                        }
                        a2 += 12u;
                        ++s3;
                        ++a3;
                        if (!(s3 < n)) break;
                    }
                    (void)broke;
                    a0 = s3;
                }
                s3 = a0 + 1;                                             // 0x80068070
                const int32_t end = n + t3;
                if (s3 < end) {
                    uint32_t dst = stage + 12u * U(a0);
                    for (uint32_t from = stage + 12u * U(s3); s3 < end; ++s3, from += 12u)
                        if (!(g.S32(from + 8u) < 17)) {
                            CopyWords(g, dst, from, 12u);
                            dst += 12u;
                        }
                }
            }
        }
    }
    g.W32(kGridRacers, 0);                                               // 0x800680E0
    if (g.Faulted()) return false;
    for (int32_t i = 0; i < n; ++i) {                                    // 0x800680EC
        g.W32(F + 2308u, bi);
        g.W32(F + 2312u, mask);
        const uint32_t a = stage + 12u * U(i);
        const uint32_t sl = g.U32(a + 8u);
        g.W32(a + 8u, 0);
        const uint32_t two = g.U32(g.U32(kGs) + 48u) == 2u ? (i == 1 ? 1u : 0u) : 0u;
        const uint32_t fl = (two << 3) | (i < 1 ? 1u : 0u);
        g.W32(F + 16u, fl);
        uint32_t e = 0;
        if (!c.SpawnBike(a, bi, sl, mask, fl, F, e)) return false;       // 0x8006815C
        const uint32_t gs = g.U32(kGs);
        uint32_t racer = 0;
        if (g.U16(e + 172u) < g.U32(gs + 48u) || ClassNibble(g, e) != 2u) racer = 1;
        g.W32(kGridRacers, g.U32(kGridRacers) + racer);
        const uint32_t head = g.S16(e + 320u) != 0 ? kListRiding : kListDormant;   // 0x800681B0
        const uint32_t node = e + 1088u;
        g.W32(g.U32(head + 4u), node);
        g.W32(e + 1092u, g.U32(head + 4u));
        g.W32(head + 4u, node);
        g.W32(e + 1088u, head);
        if (g.Faulted()) return false;
    }
    {
        uint32_t e = g.U32(kPoolTable);                                  // 0x80068218
        for (int32_t i = 0; i < g.S32(kPool0Count); ++i, e += 1096u) {   // the initial places
            uint32_t place = 0;
            if (!c.ComputePlace(e, 0, F, place)) return false;
            g.W8(g.U32(e + 1084u) + 39u, static_cast<uint8_t>(place));
            if (!c.RiderAdjust(e, F)) return false;                      // 0x80068240
        }
    }
    if (g.S32(kPool0Count) == 0) {                                       // no grid: 0x80068270
        FillWords(g, F + 24u, 0, 12u);
        for (uint32_t p = 0; p < g.U32(g.U32(kGs) + 48u);) {
            g.W32(F + 24u, p << 17);
            g.W32(F + 16u, p == 0u ? 1u : 8u);
            uint32_t e = 0;
            if (!c.SpawnBike(F + 24u, 0, p, mask, p == 0u ? 1u : 8u, F, e)) return false;
            const uint32_t node = e + 1088u;
            g.W32(g.U32(kListRiding + 4u), node);
            g.W32(e + 1092u, g.U32(kListRiding + 4u));
            g.W32(kListRiding + 4u, node);
            ++p;
            g.W32(e + 1088u, kListRiding);
            g.W8(g.U32(e + 1084u) + 39u, static_cast<uint8_t>(p));
            if (g.Faulted()) return false;
        }
        g.W32(kGridRacers, g.U32(g.U32(kGs) + 48u));
    }
    {                                                                    // the sidecar passengers: 0x80068344
        const uint32_t p1 = g.U32(kP1Bike);
        const uint32_t cls = g.U32(p1 + 180u);
        if (cls - 6u < 3u || cls - 15u < 3u) {
            uint32_t rec;
            if (g.U32(g.U32(kGs) + 48u) == 1u) rec = 1;
            else rec = cls < 9u ? 9u : 17u;
            if (!c.SpawnPassenger(g.U32(kP1Bike), bi, rec, g.U32(kPool0Count), F)) return false;
        }
    }
    {
        const uint32_t p2 = g.U32(kP2Bike);
        if (p2 != 0u) {
            const uint32_t cls = g.U32(p2 + 180u);
            if (cls - 6u < 3u || cls - 15u < 3u) {
                const uint32_t rec = cls < 9u ? 8u : 16u;
                if (!c.SpawnPassenger(p2, bi, rec, g.U32(kPool0Count) + 1u, F)) return false;
            }
        }
    }
    g.W8(kGridEscapeFlag, 1);                                            // 0x8006842C
    g.W32(0x8005B244u, U(g.S32(g.U32(kP1Bike) + 324u) >> 12));
    return !g.Faulted();
}

} // namespace rr::sim
