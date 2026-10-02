// The front end's post-race half, ported from RASHCDF.BIN (hash and reading in shell_career.h).
// Every function below is transcribed from our own disassembly of the player's image; the comments
// name the instruction ranges a reader should check it against. Accepted only by the bench rows of tools\rrverify\rows_shell_career.inc.
#include "game/shell/shell_career.h"

namespace rr::shell {

namespace {

constexpr uint32_t kS = kSession;             // 0x800D80D8
constexpr uint32_t kP0 = kPlayers;            // 0x800D81D8, player[0]
constexpr uint32_t kP1 = kPlayers + 0x24u;    // player[1]

// The career tables the dispatcher reads, all RASHCDF data, s32 per venue (index session+0x04)
// unless noted. Named by what the code does with them, not by any string.
constexpr uint32_t kBonusLow = 0x80089DDC;    // total-bonus thresholds (0x8007C414)
constexpr uint32_t kBonusMid = 0x80089DF4;    //                        (0x8007C3D4)
constexpr uint32_t kBonusHigh = 0x80089E0C;   //                        (0x8007C39C)
constexpr uint32_t kRateAddA = 0x80089E24;    // added to player+0x14 (its low byte)
constexpr uint32_t kRateAddB = 0x80089E3C;
constexpr uint32_t kRateFloor = 0x80089E54;   // player+0x14 is raised to it (0x8007C194)
constexpr uint32_t kRateLose = 0x80089E6C;    // added to player+0x14 on one lose panel (0x8007DC0C)
constexpr uint32_t kRateCap = 0x80089E84;
constexpr uint32_t kLevelAddB = 0x80089E9C;   // added to a 4-bit level of player+0x10
constexpr uint32_t kLevelAddA = 0x80089EB4;
constexpr uint32_t kLevelCap = 0x80089ECC;
constexpr uint32_t kCombatW19 = 0x80089EE4;   // combat-bonus weights of player+0x19 / +0x18 / +0x1A
constexpr uint32_t kCombatW18 = 0x80089EFC;
constexpr uint32_t kCombatW1A = 0x80089F14;
constexpr uint32_t kCombatK = 0x80089F2C;     // the weight of the second counter of each pair
// s32[gang][6 venues] (row stride 24): the item index a bonus step grants or levels.
constexpr uint32_t kItemA = 0x8008A124;
constexpr uint32_t kItemB = 0x8008A16C;
constexpr uint32_t kItemC = 0x8008A1B4;
constexpr uint32_t kItemD = 0x8008A1FC;

struct Seams {
    GuestRam& g;
    ShellCallees& k;
    bool ok = true;
    void Show(uint32_t record) {
        const uint32_t a[2] = {record, 1u};
        if (!k.Call(kResShow, a, 2, nullptr)) ok = false;
    }
    // GetRCnt(0xF2000002) and SetSeed(((v & 0xFF) * 0xCE9) >> 6), as every dispatcher starts
    // (0x8007BC70..0x8007BCA4, 0x8007DE54..0x8007DE84, 0x8007E308..0x8007E338).
    void Seed() {
        uint32_t v = 0;
        if (!k.Call1(kGetRCnt, 0xF2000002u, &v)) ok = false;
        SetSeed(g, static_cast<uint32_t>(static_cast<int32_t>((v & 0xFFu) * 0xCE9u) >> 6));
    }
    void Done(bool* out) const {
        if (out != nullptr) *out = ok;
    }
};

int32_t Venue(GuestRam& g) { return g.S8(kS + 0x04u); }
uint32_t V4(GuestRam& g) { return 4u * static_cast<uint32_t>(Venue(g)); }
// venue*4 + gang*24 (0x8007C52C..0x8007C548 and every item-table index).
uint32_t GV(GuestRam& g) { return V4(g) + 24u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(kP0 + 0x09u))); }
int32_t Tv(GuestRam& g, uint32_t table) { return g.S32(table + V4(g)); }
uint8_t Tb(GuestRam& g, uint32_t table) { return g.U8(table + V4(g)); }
void Inc15(GuestRam& g) { g.W8(kS + 0x15u, static_cast<uint8_t>(g.U8(kS + 0x15u) + 1u)); }
void Bump16(GuestRam& g, uint32_t off) { g.W16(kS + off, static_cast<uint16_t>(g.U16(kS + off) + 1u)); }
uint8_t Rate(GuestRam& g) { return g.U8(kP0 + 0x14u); }
void SetRate(GuestRam& g, uint32_t v) { g.W8(kP0 + 0x14u, static_cast<uint8_t>(v)); }

// player+0x0C bit `i` (srav / sllv, so the shift is i & 31).
bool Has(GuestRam& g, int32_t i) { return ((static_cast<uint32_t>(g.U16(kP0 + 0x0Cu)) >> (static_cast<uint32_t>(i) & 31u)) & 1u) != 0; }
void Grant(GuestRam& g, int32_t i) {
    g.W16(kP0 + 0x0Cu, static_cast<uint16_t>(g.U16(kP0 + 0x0Cu) | (1u << (static_cast<uint32_t>(i) & 31u))));
}

// The level step shared by every "already have it" arm (e.g. 0x8007C7C4..0x8007C838): the item's
// 4-bit level out of player+0x10 (0 for an index >= 8), plus `add`, capped by kLevelCap; the
// notification is 1595 when capped, else 1594 if the new level exceeds `sub`, else 1528.
uint16_t LevelUp(GuestRam& g, int32_t item, uint32_t add, uint32_t sub, int32_t* level) {
    uint32_t nib = 0;
    if (item < 8) nib = (g.U32(kP0 + 0x10u) >> ((static_cast<uint32_t>(item) << 2) & 31u)) & 15u;
    int32_t a2 = static_cast<int32_t>(nib + static_cast<uint32_t>(Tv(g, add)));
    const int32_t cap = Tv(g, kLevelCap);
    uint16_t msg;
    if (cap < a2) {
        a2 = cap;
        msg = 1595;
    } else {
        msg = static_cast<int32_t>(static_cast<uint32_t>(a2) - static_cast<uint32_t>(Tv(g, sub))) > 0 ? 1594 : 1528;
    }
    *level = a2;
    return msg;
}

// 0x8007CC20..0x8007CC94 (and 0x8007D3D4..0x8007D448): the level written back into the item's
// nibble, and into player+0x0F when the item is the current one (player+0x0E).
void StoreLevel(GuestRam& g, uint32_t at, int32_t level) {
    uint32_t n = g.U32(kP0 + 0x10u) & ~(15u << ((g.U32(at) << 2) & 31u));
    g.W32(kP0 + 0x10u, n);
    n |= (static_cast<uint32_t>(level) & 15u) << ((g.U32(at) << 2) & 31u);
    g.W32(kP0 + 0x10u, n);
    if (static_cast<uint32_t>(g.U8(kP0 + 0x0Eu)) == g.U32(at)) g.W8(kP0 + 0x0Fu, static_cast<uint8_t>(level));
}

// session+0x15 = (s8)(session+0x15 [+1]) % 8, C remainder (0x8007C4A8..0x8007C4C8).
int32_t Step15(GuestRam& g, bool increment) {
    if (increment) Inc15(g);
    const int32_t v = g.S8(kS + 0x15u) % 8;
    g.W8(kS + 0x15u, static_cast<uint8_t>(v));
    return static_cast<int8_t>(v);
}

// Mark `bit` (a byte read as s8, >> 3 arithmetic) of a bitmap set or clear.
void SetBit(GuestRam& g, uint32_t map, uint8_t id, bool on) {
    const uint32_t a = map + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(id)) >> 3);
    const uint8_t m = static_cast<uint8_t>(1u << (id & 7u));
    g.W8(a, on ? static_cast<uint8_t>(g.U8(a) | m) : static_cast<uint8_t>(g.U8(a) & ~m));
}

// The three bonus chains (0x8007C49C, 0x8007C6C4, 0x8007CCD8), each a jump table over
// session+0x15 into a fall-through ladder. `msg` is what the chain stores at the end.
uint16_t ChainS2(GuestRam& g, int32_t k) {
    switch (k) {
    default: // 0x8007C4FC (0, and a negative remainder)
        Inc15(g);
        [[fallthrough]];
    case 1: { // 0x8007C514
        const int32_t it = g.S32(kItemA + GV(g));
        if (!Has(g, it)) {
            Grant(g, it);
            return 1527;
        }
        Inc15(g);
        [[fallthrough]];
    }
    case 2: // 0x8007C58C
        if (!(Tv(g, kRateCap) < static_cast<int32_t>(Rate(g)))) {
            SetRate(g, Rate(g) + Tb(g, kRateAddA));
            return 1529;
        }
        [[fallthrough]];
    case 3: // 0x8007C5E4
        Inc15(g);
        [[fallthrough]];
    case 4:
    case 5: { // 0x8007C5FC
        const int32_t it = g.S32(kItemB + GV(g));
        if (!Has(g, it)) {
            Grant(g, it);
            return 1527;
        }
        Inc15(g);
        [[fallthrough]];
    }
    case 6:
    case 7: { // 0x8007C674
        const uint8_t old = Rate(g);
        SetRate(g, old + 1u);
        return Tv(g, kRateCap) < static_cast<int32_t>(old) ? 1589 : 1529;
    }
    }
}

// `slot` is where the chain's message goes (fe+0x78 or fe+0x7A): a level-up arm stores its message
// before the write-back, exactly as the original, and the caller's final store repeats it.
uint16_t ChainS5(GuestRam& g, int32_t k, uint32_t slot) {
    int32_t level = 0;
    switch (k) {
    default: // 0x8007C734
        Inc15(g);
        [[fallthrough]];
    case 1: { // 0x8007C74C
        const int32_t it = g.S32(kItemA + GV(g));
        if (it < 6) {
            if (!Has(g, it)) {
                Grant(g, it);
                return 1527;
            }
            const uint16_t msg = LevelUp(g, it, kLevelAddA, kLevelAddA, &level);
            g.W16(slot, msg);
            StoreLevel(g, kItemA + GV(g), level);
            return msg;
        }
        Inc15(g);
        [[fallthrough]];
    }
    case 2: // 0x8007C864
        if (!(Tv(g, kRateCap) < static_cast<int32_t>(Rate(g)))) {
            SetRate(g, Rate(g) + Tb(g, kRateAddA));
            return 1529;
        }
        [[fallthrough]];
    case 3: // 0x8007C8B8
        if (g.S32(kItemA + GV(g)) < 6) {
            const int32_t it = g.S32(kItemB + GV(g));
            if (!Has(g, it)) {
                Grant(g, it);
                return 1527;
            }
            const uint16_t msg = LevelUp(g, it, kLevelAddB, kLevelAddB, &level);
            g.W16(slot, msg);
            StoreLevel(g, kItemB + GV(g), level);
            return msg;
        }
        Inc15(g);
        [[fallthrough]];
    case 4: // 0x8007C9DC
        if (!(Tv(g, kRateCap) < static_cast<int32_t>(Rate(g)))) {
            SetRate(g, Rate(g) + Tb(g, kRateAddB));
            return 1529;
        }
        [[fallthrough]];
    case 5: { // 0x8007CA34
        const int32_t it = g.S32(kItemD + GV(g));
        if (!Has(g, it)) {
            Grant(g, it);
            return 1527;
        }
        Inc15(g);
        [[fallthrough]];
    }
    case 6: // 0x8007CAAC
        if (!(Tv(g, kRateCap) < static_cast<int32_t>(Rate(g)))) {
            SetRate(g, Rate(g) + Tb(g, kRateAddB));
            return 1529;
        }
        Inc15(g);
        [[fallthrough]];
    case 7: // 0x8007CB14
        if (g.S32(kItemB + GV(g)) < 6) {
            const int32_t it = g.S32(kItemC + GV(g));
            if (!Has(g, it)) {
                Grant(g, it);
                return 1527;
            }
            const uint16_t msg = LevelUp(g, it, kLevelAddB, kLevelAddB, &level);
            g.W16(slot, msg);
            StoreLevel(g, kItemC + GV(g), level);
            return msg;
        }
        {
            const uint8_t old = Rate(g); // 0x8007CC98
            SetRate(g, old + 1u);
            return Tv(g, kRateCap) < static_cast<int32_t>(old) ? 1589 : 1529;
        }
    }
}

uint16_t ChainS6(GuestRam& g, int32_t k, uint32_t slot) {
    int32_t level = 0;
    switch (k) {
    default: // 0x8007CD4C
        Inc15(g);
        [[fallthrough]];
    case 1: { // 0x8007CD64
        const int32_t it = g.S32(kItemA + GV(g));
        if (it < 6) {
            if (!Has(g, it)) {
                Grant(g, it);
                return 1527;
            }
            const uint16_t msg = LevelUp(g, it, kLevelAddA, kLevelAddA, &level);
            g.W16(slot, msg);
            StoreLevel(g, kItemA + GV(g), level);
            return msg;
        }
        Inc15(g);
        [[fallthrough]];
    }
    case 2: // 0x8007CE78
        if (!(Tv(g, kRateCap) < static_cast<int32_t>(Rate(g)))) {
            SetRate(g, Rate(g) + Tb(g, kRateAddA));
            return 1529;
        }
        [[fallthrough]];
    case 3: // 0x8007CECC
        if (g.S32(kItemA + GV(g)) < 6) {
            const int32_t it = g.S32(kItemB + GV(g));
            if (!Has(g, it)) {
                Grant(g, it);
                return 1527;
            }
            const uint16_t msg = LevelUp(g, it, kLevelAddB, kLevelAddB, &level);
            g.W16(slot, msg);
            StoreLevel(g, kItemB + GV(g), level);
            return msg;
        }
        Inc15(g);
        [[fallthrough]];
    case 4: // 0x8007CFEC
        if (!(Tv(g, kRateCap) < static_cast<int32_t>(Rate(g)))) {
            SetRate(g, Rate(g) + Tb(g, kRateAddB));
            return 1529;
        }
        [[fallthrough]];
    case 5: { // 0x8007D044: gang 0 reads the gang-1 row, any other gang the gang-0 row
        const uint32_t at = (g.S8(kP0 + 0x09u) == 0) ? kItemA + V4(g) + 24u : kItemA + V4(g);
        const int32_t it = g.S32(at);
        if (!Has(g, it)) {
            Grant(g, it);
            return 1527;
        }
        const uint16_t msg = LevelUp(g, it, kLevelAddB, kLevelAddB, &level);
        g.W16(slot, msg);
        StoreLevel(g, at, level);
        return msg;
    }
    case 6: // 0x8007D23C
        if (!(Tv(g, kRateCap) < static_cast<int32_t>(Rate(g)))) {
            SetRate(g, Rate(g) + Tb(g, kRateAddB));
            return 1529;
        }
        Inc15(g);
        [[fallthrough]];
    case 7: // 0x8007D2AC
        if (g.S32(kItemB + GV(g)) < 6) {
            const int32_t it = g.S32(kItemC + GV(g));
            if (!Has(g, it)) {
                Grant(g, it);
                return 1527;
            }
            // 0x8007D358: added from kLevelAddA, compared against kLevelAddB.
            const uint16_t msg = LevelUp(g, it, kLevelAddA, kLevelAddB, &level);
            g.W16(slot, msg);
            StoreLevel(g, kItemC + GV(g), level);
            return msg;
        }
        {
            const uint8_t old = Rate(g); // 0x8007D44C
            SetRate(g, old + 1u);
            if (Tv(g, kRateCap) < static_cast<int32_t>(old)) return 1589;
            g.W16(slot, 1529);
            Inc15(g);
            return 1529;
        }
    }
}

// fe+0xAC = tag; fe+0x1C = (Rand() & 3) + base - the notification every venue arm raises
// (e.g. 0x8007D4FC..0x8007D524), after res_show(9, 1).
void Notify(Seams& s, uint32_t tag, uint32_t base) {
    s.Show(9);
    s.g.W32(kFeTagA, tag);
    s.g.W16(kFeNotify, static_cast<uint16_t>((Rand(s.g) & 3u) + base));
}

// The venue-progress step, 0x8007D4A0..0x8007D938: `s3` the outcome, returns s7.
int32_t VenueStep(Seams& s, int32_t s3) {
    GuestRam& g = s.g;
    int32_t s7 = 10;
    auto advance = [&](bool resetCount, bool flag) {
        if (resetCount) g.W8(kS + 0x11u, 0);
        g.W8(kS + 0x04u, static_cast<uint8_t>(g.U8(kS + 0x04u) + 1u));
        if (flag) g.W8(kS + 0x05u, static_cast<uint8_t>(g.U8(kS + 0x05u) | 0x10u));
    };
    auto streak = [&](uint32_t pt, uint32_t sp) { // session+0x11 3 / 6 on a win
        if (s3 != 1) return;
        const uint8_t n = g.U8(kS + 0x11u);
        if (n == 3) Notify(s, 0x31305450u, pt);
        else if (n == 6) Notify(s, 0x31305053u, sp);
    };
    const int32_t v = Venue(g);
    switch (static_cast<uint32_t>(v) < 6u ? v : 0) {
    case 0: // 0x8007D4D0
        if (AllDone(g, 1, 9)) {
            s7 = 11;
            advance(false, false);
            Notify(s, 0x3430424Au, 1190);
        } else {
            streak(1166, 1178);
        }
        break;
    case 1: // 0x8007D5B4
        s7 = 14;
        if (AllDone(g, 28, 28)) {
            s7 = 12;
            advance(true, true);
            Notify(s, 0x3330424Au, 1194);
        } else {
            Notify(s, 0x32305450u, 1198);
        }
        break;
    case 2: // 0x8007D64C
        if (AllDone(g, 10, 18)) {
            s7 = 15;
            advance(false, false);
            Notify(s, 0x3130424Au, 1202);
        } else {
            streak(1170, 1182);
        }
        break;
    case 3: // 0x8007D730
        s7 = 18;
        if (AllDone(g, 29, 29)) {
            s7 = 16;
            advance(true, true);
            Notify(s, 0x3530424Au, 1206);
        } else {
            Notify(s, 0x32305053u, 1210);
        }
        break;
    case 4: // 0x8007D7C8
        if (AllDone(g, 19, 27)) {
            s7 = 19;
            advance(false, true);
            Notify(s, 0x3230424Au, 1214);
        } else {
            streak(1174, 1186);
        }
        break;
    default: // 5, 0x8007D8B8
        s7 = 23;
        if (AllDone(g, 30, 30)) {
            s7 = 20;
            SetBit(g, kProgress, g.U8(kS + 0x08u), false);
        } else {
            Notify(s, 0x3830424Au, 1218);
        }
        break;
    }
    return s7;
}

// 0x8007D93C..0x8007DD00: the result panel when no progress notification is pending.
void ResultPanel(Seams& s, int32_t s3) {
    GuestRam& g = s.g;
    if (g.S16(kFeNotify) != 0) return;
    const bool gang = g.S8(kP0 + 0x09u) != 0;
    if (s3 == 1) { // 0x8007D98C: the gang's win screen
        s.Show(gang ? 27u : 26u);
        uint32_t i = Rand(g) & 7u;
        if (!(i < 3u)) i = Rand(g) & 7u;
        g.W32(kFeTagA, g.U32((gang ? 0x80089404u : 0x800893DCu) + 4u * i));
        const uint32_t r = Rand(g) & 3u;
        const uint32_t base = (i < 3u && !gang) ? 1122u : 1134u;
        g.W16(kFeNotify, static_cast<uint16_t>(4u * i + base + r));
    } else if (s3 == 3) { // 0x8007DC3C: busted
        s.Show(20);
        const uint32_t i = Rand(g) % 11u;
        g.W32(kFeTagA, g.U32(0x80089520u + 4u * i));
        g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1314u + (Rand(g) & 3u)));
    } else if (s3 == 4) { // 0x8007DCA0: wrecked
        s.Show(23);
        const uint32_t i = Rand(g) % 5u;
        g.W32(kFeTagA, g.U32(0x80089574u + 4u * i));
        g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1274u + (Rand(g) & 3u)));
    } else { // 0x8007DA7C: the gang's lose screen
        s.Show(gang ? 29u : 28u);
        uint32_t i = Rand(g) % 10u;
        if (!(i < 5u)) i = Rand(g) % 10u;
        g.W32(kFeTagA, g.U32((gang ? 0x80089454u : 0x8008942Cu) + 4u * i));
        const uint32_t r = Rand(g) & 3u;
        if (i < 3u) {
            g.W16(kFeNotify, static_cast<uint16_t>(4u * i + (gang ? 1234u : 1222u) + r));
        } else if (i < 5u) { // 0x8007DBE0
            g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1234u + r));
            const uint8_t rate = Rate(g);
            if (rate < 2u && r == 0u) SetRate(g, rate + Tb(g, kRateLose));
        } else {
            g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1234u + r));
        }
    }
}

uint32_t OutcomeTag(int32_t s3) { // 0x8007DD04..0x8007DD70
    switch (s3) {
    case 1: return 0x524E4E57u; // WNNR
    case 3: return 0x44545342u; // BSTD
    case 4: return 0x444B5257u; // WRKD
    default: return 0x52534F4Cu; // LOSR
    }
}

void CopyIdentity(GuestRam& g, uint32_t dst, uint32_t rider) {
    const uint16_t a = g.U16(rider + 0x2Cu);
    const uint8_t b = g.U8(rider + 0x2Eu), c = g.U8(rider + 0x2Fu);
    const uint32_t d = g.U32(rider + 0x30u);
    g.W16(dst, a);
    g.W8(dst + 2u, b);
    g.W8(dst + 3u, c);
    g.W32(dst + 4u, d);
}

} // namespace

void SetSeed(GuestRam& g, uint32_t seed) { g.W32(g.gp() + 2076u, seed); }

int32_t OddMission(int32_t id) { return static_cast<uint32_t>(id) < 18u ? (id & 1) : 0; }

// ---------------------------------------------------------------------------- RASHCDF 0x8007BB34

int32_t CareerResult(GuestRam& g, ShellCallees& k, bool* ok) {
    Seams s{g, k};
    // 0x8007BB8C..0x8007BBE8: session+0x2C/+0x2E from player+0x19/+0x1D, and the one pass (i = 0)
    // of a nibble clear over player+0x10 gated by bit 0 of player+0x0C.
    const uint16_t owned = g.U16(kP0 + 0x0Cu);
    g.W16(kS + 0x2Cu, g.U8(kP0 + 0x19u));
    g.W16(kS + 0x2Eu, g.U8(kP0 + 0x1Du));
    if (!(owned & 1u)) {
        const uint32_t n = g.U32(kP0 + 0x10u);
        if (n & 0xFu) g.W32(kP0 + 0x10u, n & ~0xFu);
    }
    // 0x8007BC0C..0x8007BC6C: the race's identities back into player[0] and session+0x40.
    CopyIdentity(g, kP0 + 0x0Cu, kRiders);
    for (uint32_t i = 0; i < 16u; ++i) CopyIdentity(g, kS + 0x40u + 8u * i, kRiders + 0x90u + 0x48u * i);
    s.Seed();
    // 0x8007BCA8..0x8007BCD4
    g.W32(kFeTagA, 300);
    g.W32(kFeTagB, 300);
    g.W16(kFe + 0x72u, 0);
    g.W16(kFe + 0x70u, 0);
    g.W16(kFe + 0x74u, 0);
    g.W16(kFe + 0x78u, 0);
    g.W16(kFe + 0x7Au, 0);
    g.W16(kFeNotify, 0);

    // 0x8007BCD8: the result code player[0]+0x20 into an outcome s3 and the race bonus.
    const int32_t code = g.S32(kP0 + 0x20u);
    int32_t s3 = 2;
    uint32_t prize;
    bool win = false;
    if (code == 249 || code == 251 || code == 248 || code == 1) {
        s3 = 1, win = true, prize = g.U16(kPrizeTable + V4(g)), Bump16(g, 0x20);
    } else if (code == 2) {
        s3 = 1, win = true, prize = g.U16(kPrizeTable + V4(g) + 24u), Bump16(g, 0x22);
    } else if (code == 3) {
        s3 = 1, win = true, prize = g.U16(kPrizeTable + V4(g) + 48u), Bump16(g, 0x24);
    } else if (code >= 4 && code < 17) { // 0x8007BFA4: the place's row
        prize = g.U16(kPrizeTable + V4(g) + 24u * static_cast<uint32_t>(code - 1)), Bump16(g, 0x26);
    } else if (code == 250) { // 0x8007BE24
        prize = g.U16(0x8008A10Cu + V4(g)), Bump16(g, 0x26);
    } else if (code == 252 || code == 253 || code == 255) { // 0x8007BE5C
        s3 = 4, prize = g.U16(0x8008A0DCu + V4(g)), Bump16(g, 0x2A);
    } else if (code == 254) { // 0x8007BE90
        s3 = 3, prize = g.U16(0x8008A0F4u + V4(g)), Bump16(g, 0x28);
    } else { // 0x8007BFEC
        prize = g.U16(0x8008A0C4u + V4(g)), Bump16(g, 0x26);
    }
    g.W16(kFe + 0x70u, static_cast<uint16_t>(prize));
    if (win) { // 0x8007BF70: the race-done bit, and the win count session+0x11
        SetBit(g, kProgress, g.U8(kS + 0x08u), true);
        g.W8(kS + 0x11u, static_cast<uint8_t>(g.U8(kS + 0x11u) + 1u));
    }

    // 0x8007C01C..0x8007C160: race bonus += rand % 9; the combat bonus; the total.
    g.W16(kFe + 0x70u, static_cast<uint16_t>(g.U16(kFe + 0x70u) + Rand(g) % 9u));
    const uint32_t r2 = Rand(g);
    {
        const uint32_t kk = static_cast<uint32_t>(Tv(g, kCombatK));
        const uint32_t t0 = (g.U8(kP0 + 0x19u) - kk * g.U8(kP0 + 0x1Du)) * static_cast<uint32_t>(Tv(g, kCombatW19));
        const uint32_t t1 = (g.U8(kP0 + 0x18u) - kk * g.U8(kP0 + 0x1Cu)) * static_cast<uint32_t>(Tv(g, kCombatW18));
        const uint32_t t2 = (g.U16(kP0 + 0x1Au) - kk * g.U16(kP0 + 0x1Eu)) * static_cast<uint32_t>(Tv(g, kCombatW1A));
        const uint32_t combat = t0 + t1 + t2 + r2 % 9u;
        const int32_t sum = static_cast<int32_t>(g.S16(kFe + 0x70u)) + static_cast<int16_t>(combat);
        const uint16_t race = g.U16(kFe + 0x70u);
        g.W16(kFe + 0x72u, static_cast<uint16_t>(combat));
        g.W16(kFe + 0x74u, sum > 0 ? static_cast<uint16_t>(race + combat) : 0);
    }
    // 0x8007C164..0x8007C1AC: player+0x14 is raised to the venue's floor.
    if (static_cast<int32_t>(Rate(g)) < Tv(g, kRateFloor)) SetRate(g, Tb(g, kRateFloor));

    const int32_t total = g.S16(kFe + 0x74u);
    int32_t s5 = 0, s6 = 0, s2 = 0, s8 = 0;
    if (total < 63 || s3 == 4 || s3 == 3) { // 0x8007C1D4: no bonus, and the penalty steps
        s5 = -1;
        g.W16(kFe + 0x72u, 0);
        g.W16(kFe + 0x74u, 0);
        if (g.S8(kS + 0x15u) > 0) {
            int32_t top = -1; // the highest nonzero level nibble among items 5..0
            for (int32_t i = 5; i >= 0; --i)
                if ((g.U32(kP0 + 0x10u) >> (4u * static_cast<uint32_t>(i))) & 15u) {
                    top = i;
                    break;
                }
            if (s3 == 4) { // 0x8007C240
                const int32_t rate = Rate(g);
                if (Tv(g, kRateFloor) < rate) {
                    SetRate(g, Tv(g, kRateAddA) < rate ? static_cast<uint32_t>(rate - Tb(g, kRateAddA)) : Tb(g, kRateFloor));
                    g.W16(kFe + 0x78u, 1525);
                }
            } else if (s3 == 3) {
                if (top < 0) { // 0x8007C334: the highest item owned (bits 8..0) is taken away
                    const uint16_t own = g.U16(kP0 + 0x0Cu);
                    for (int32_t b = 8; b >= 0; --b) {
                        if ((own >> b) & 1u) {
                            g.W16(kP0 + 0x0Cu, static_cast<uint16_t>(own & ~(1u << b)));
                            g.W8(kP0 + 0x0Eu, 9);
                            g.W8(kP0 + 0x0Fu, 0);
                            g.W16(kFe + 0x78u, 1524);
                            break;
                        }
                    }
                } else { // 0x8007C2CC: that item's level is cleared
                    g.W32(kP0 + 0x10u, g.U32(kP0 + 0x10u) & ~(15u << (4u * static_cast<uint32_t>(top))));
                    if (static_cast<int32_t>(g.U8(kP0 + 0x0Eu)) == top) g.W8(kP0 + 0x0Fu, 0);
                    g.W16(kFe + 0x78u, 1526);
                }
            }
            g.W8(kS + 0x15u, static_cast<uint8_t>(g.U8(kS + 0x15u) - 1u)); // 0x8007C360
        }
    } else { // 0x8007C37C: which bonus chains run
        const int32_t c = g.S32(kP0 + 0x20u);
        if (c == 248 || c == 251) {
            s8 = 1;
        } else if (!(total < Tv(g, kBonusHigh))) {
            s5 = 1;
            s6 = s3 == 1 ? 1 : 0;
        } else if (!(total < Tv(g, kBonusMid))) {
            if (c < 10) {
                s5 = 1;
                Inc15(g);
            } else {
                s2 = 1;
            }
        } else if (!(total < Tv(g, kBonusLow))) {
            s2 = c < 11 ? 1 : 0;
        }
    }

    if (s8 > 0) { // 0x8007C460
        const int32_t v = Venue(g);
        if (v == 1) SetRate(g, 3);
        else if (v == 3) SetRate(g, 6);
        g.W16(kFe + 0x78u, 1590);
    }
    if (s2 > 0) g.W16(kFe + 0x78u, ChainS2(g, Step15(g, false)));
    if (s5 > 0) g.W16(kFe + 0x78u, ChainS5(g, Step15(g, true), kFe + 0x78u));
    if (s6 > 0) g.W16(kFe + 0x7Au, ChainS6(g, Step15(g, true), kFe + 0x7Au));

    const int32_t s7 = VenueStep(s, s3);
    ResultPanel(s, s3);
    g.W32(kFeTagB, OutcomeTag(s3));
    s.Done(ok);
    return s7;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8007E2C8

int32_t FiveOResult(GuestRam& g, ShellCallees& k, bool* ok) {
    Seams s{g, k};
    g.W16(kS + 0x2Cu, g.U8(kP0 + 0x19u));
    g.W16(kS + 0x2Eu, g.U8(kP0 + 0x1Du));
    s.Seed();
    // 0x8007E340..0x8007E378: the result code is read before the identity is copied back.
    const int32_t code = g.S32(kP0 + 0x20u);
    CopyIdentity(g, kP0 + 0x0Cu, kRiders);
    int32_t ret = 28;
    auto mission = [&]() { return static_cast<int32_t>(g.S8(kS + 0x07u)); };
    auto missionTag = [&](uint32_t table) { // 0x8007E3D0..0x8007E418: table[(s8)(id % 6)]
        const int32_t q = mission() % 6;
        return g.U32(table + 4u * static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(q))));
    };
    auto caught = [&](uint32_t countOff) { // 0x8007E440..0x8007E49C
        SetBit(g, kMissions, g.U8(kS + 0x07u), true);
        g.W32(kFeTagB, 0x524E4E57u); // WNNR
        g.W8(kS + 0x11u, static_cast<uint8_t>(g.U8(kS + 0x11u) + 1u));
        Bump16(g, countOff);
        s.Show(24);
    };
    switch (code) {
    case 250: { // 0x8007E39C: the target escaped
        g.W32(kFeTagB, 0x52534F4Cu); // LOSR
        Bump16(g, 0x26);
        s.Show(25);
        g.W32(kFeTagA, missionTag(0x80089508u));
        const uint32_t r = Rand(g) & 3u;
        g.W16(kFeNotify, static_cast<uint16_t>(static_cast<uint32_t>(mission()) * 8u + 1370u + r));
        break;
    }
    case 252: { // 0x8007E438
        caught(0x20);
        g.W32(kFeTagA, missionTag(0x800894E8u));
        const uint32_t r = Rand(g) & 3u;
        g.W16(kFeNotify, static_cast<uint16_t>(static_cast<uint32_t>(mission()) * 8u + 1366u + r));
        break;
    }
    case 253: { // 0x8007E510
        caught(0x22);
        const uint32_t r = Rand(g) & 3u;
        if (OddMission(mission()) == 1) {
            g.W32(kFeTagA, 0x414B4C4Au);
            g.W16(kFeNotify, static_cast<uint16_t>(r + 1362u));
        } else {
            g.W32(kFeTagA, 0x53444C4Au);
            g.W16(kFeNotify, static_cast<uint16_t>(r + 1358u));
        }
        break;
    }
    default: { // 0x8007E5C0: wrecked
        g.W32(kFeTagB, 0x444B5257u); // WRKD
        Bump16(g, 0x2A);
        s.Show(23);
        const uint32_t i = Rand(g) % 5u;
        g.W32(kFeTagA, g.U32(0x80089574u + 4u * i));
        g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1294u + (Rand(g) & 3u)));
        break;
    }
    }
    // 0x8007E648: the mission venue steps.
    auto next = [&](uint8_t venue) {
        g.W8(kS + 0x04u, venue);
        g.W8(kS + 0x11u, 0);
        g.W8(kS + 0x05u, static_cast<uint8_t>(g.U8(kS + 0x05u) | 0x10u));
    };
    const int32_t v = Venue(g);
    if (v == 2) {
        if (AllDoneMissions(g, 6, 11)) next(4);
    } else if (v == 0) {
        if (AllDoneMissions(g, 0, 5)) next(2);
    } else if (v == 4) {
        if (AllDoneMissions(g, 12, 17)) {
            ClearMissions(g);
            ret = 21;
            g.W8(kS + 0x04u, 0);
        }
    }
    s.Done(ok);
    return ret;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8007DDA8

int32_t SideCarResult(GuestRam& g, ShellCallees& k, bool* ok) {
    Seams s{g, k};
    g.W32(kFeTagB, 300);
    g.W16(kS + 0x3Cu, g.U8(kP1 + 0x19u));
    g.W16(kS + 0x3Eu, g.U8(kP1 + 0x1Du));
    g.W16(kS + 0x2Cu, g.U8(kP0 + 0x19u));
    const uint8_t t3 = g.U8(kP0 + 0x1Du);
    // 0x8007DE10..0x8007DE50: both riders' identities, read before any store.
    const uint16_t a0 = g.U16(kRiders + 0x2Cu), b0 = g.U16(kRiders + 0x74u);
    const uint8_t a1 = g.U8(kRiders + 0x2Eu), a2 = g.U8(kRiders + 0x2Fu);
    const uint8_t b1 = g.U8(kRiders + 0x76u), b2 = g.U8(kRiders + 0x77u);
    const uint32_t a3 = g.U32(kRiders + 0x30u), b3 = g.U32(kRiders + 0x78u);
    g.W16(kP0 + 0x0Cu, a0);
    g.W8(kP0 + 0x0Eu, a1);
    g.W8(kP0 + 0x0Fu, a2);
    g.W32(kP0 + 0x10u, a3);
    g.W16(kP1 + 0x0Cu, b0);
    g.W8(kP1 + 0x0Eu, b1);
    g.W8(kP1 + 0x0Fu, b2);
    g.W32(kP1 + 0x10u, b3);
    g.W16(kS + 0x2Eu, t3);
    s.Seed();
    const int32_t code = g.S32(kP0 + 0x20u);
    int32_t ret = 37;
    auto lost = [&]() { // 0x8007DEC0 / 0x8007E15C
        g.W32(kFeTagB, 0x52534F4Cu); // LOSR
        Bump16(g, 0x26);
        s.Show(21);
        const uint32_t i = Rand(g) % 5u;
        g.W32(kFeTagA, g.U32(0x8008954Cu + 4u * i));
        g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1254u + (Rand(g) & 3u)));
    };
    switch (code) {
    case 250:
        lost();
        break;
    case 251: case 252: case 253: case 255: { // 0x8007DF44
        g.W32(kFeTagB, 0x444B5257u); // WRKD
        Bump16(g, 0x2A);
        s.Show(23);
        const uint32_t i = Rand(g) % 5u;
        g.W32(kFeTagA, g.U32(0x80089574u + 4u * i));
        g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1274u + (Rand(g) & 3u)));
        break;
    }
    case 254: { // 0x8007DFC8
        g.W32(kFeTagB, 0x44545342u); // BSTD
        Bump16(g, 0x28);
        s.Show(20);
        const uint32_t i = Rand(g) % 11u;
        g.W32(kFeTagA, g.U32(0x80089520u + 4u * i));
        g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1314u + (Rand(g) & 3u)));
        break;
    }
    default:
        if (code < 4) { // 0x8007E054: a place on the podium (anything below 4)
            Bump16(g, code == 1 ? 0x20u : (code == 2 ? 0x22u : 0x24u));
            SetBit(g, kProgress, g.U8(kS + 0x08u), true);
            g.W32(kFeTagB, 0x524E4E57u); // WNNR
            g.W8(kS + 0x11u, static_cast<uint8_t>(g.U8(kS + 0x11u) + 1u));
            s.Show(22);
            const uint32_t i = Rand(g) % 5u;
            g.W32(kFeTagA, g.U32(0x80089560u + 4u * i));
            g.W16(kFeNotify, static_cast<uint16_t>(4u * i + 1146u + (Rand(g) & 3u)));
        } else {
            lost();
        }
        break;
    }
    // 0x8007E1D4: the venue steps (table 0x8005C9B8).
    auto next = [&](uint8_t venue) {
        const uint8_t f = g.U8(kS + 0x05u);
        g.W8(kS + 0x04u, venue);
        g.W8(kS + 0x11u, 0);
        g.W8(kS + 0x05u, static_cast<uint8_t>(f | 0x10u));
    };
    const int32_t v = Venue(g);
    if (v == 2) {
        if (AllDone(g, 10, 15)) next(4);
    } else if (v == 4) {
        if (AllDone(g, 19, 24)) {
            ret = 21;
            SetBit(g, kProgress, g.U8(kS + 0x08u), false);
        }
    } else {
        if (AllDone(g, 1, 6)) next(2);
    }
    s.Done(ok);
    return ret;
}

// ---------------------------------------------------------------------------- the resume path

void ShortCommit(GuestRam& g) {
    const uint32_t gs = g.U32(kGameStatePtr);
    const uint8_t mode = g.U8(kS);
    g.W32(kDemo, 0);
    g.W8(gs + 4u, mode);
    const uint32_t gs2 = g.U32(kGameStatePtr);
    g.W32(gs2 + 0x30u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(kS + 0x06u))));
    g.W32(gs2 + 0x40u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(kS + 0x08u))));
    const int32_t venue = g.S8(kS + 0x04u);
    g.W32(gs2 + 0x3Cu, static_cast<uint32_t>(venue));
    g.W16(gs2 + 0x3Au, static_cast<uint16_t>(venue));
    g.W32(gs2 + 0x48u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(0x800D81DFu))));
    const uint32_t screen = g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(kS + 0x0Cu))));
    g.W16(screen + 4u, static_cast<uint16_t>(static_cast<int16_t>(g.S8(kS + 0x0Du))));
}

// RASHCDF 0x80063908, with its three jump tables 0x8005B7A4 (by chooser +0x01), 0x8005B7D4 and
// 0x8005B824 (by chooser +0x02).
void ChooserSync(GuestRam& g) {
    for (uint32_t i = 0; i < 37u; ++i) {
        const uint32_t c = g.U32(kChoosers + 4u * i);
        if (c == 0 || g.U32(c + 4u) == 0) continue;
        const int32_t sel = static_cast<int8_t>(static_cast<uint8_t>(g.U8(c + 1u) - 1u));
        if (!(static_cast<uint32_t>(sel) < 12u)) continue;
        int32_t v = 0;
        switch (sel) {
        case 0: v = g.S8(kP0 + 9u); break;   // 0x800639A8
        case 1: v = g.S8(kP0 + 10u); break;  // 0x800639B4
        case 2: {                            // 0x800639C0
            const int32_t kind = static_cast<int8_t>(static_cast<uint8_t>(g.U8(c + 2u) - 7u));
            switch (static_cast<uint32_t>(kind) < 19u ? kind : -1) {
            case 0: v = g.S8(kP0 + 36u * g.U8(kS + 0x13u) + 7u); break; // 0x80063A40
            case 10:
            case 18: v = g.S8(kP0 + 43u); break;                        // 0x800639FC
            case 13:                                                    // 0x80063A24
                v = g.S8(kP0 + 43u);
                if (v < 18) continue;
                break;
            case 14:                                                    // 0x80063A08
                v = g.S8(kP0 + 43u);
                if (!(v < 18)) continue;
                break;
            default: v = g.S8(kP0 + 7u); break;                         // 0x80063A64
            }
            break;
        }
        case 3: v = g.S8(kS + 8u); break;    // 0x80063A7C
        case 4:                              // 0x80063A70
            g.W8(kFe + 0x1Fu, 56);
            continue;
        case 5: v = g.S8(kS + 4u); break;    // 0x80063A88
        case 6: {                            // 0x80063ACC
            const int32_t kind = static_cast<int8_t>(static_cast<uint8_t>(g.U8(c + 2u) - 11u));
            if (!(static_cast<uint32_t>(kind) < 26u)) continue;
            switch (kind) {
            case 0: v = g.S8(kS + 9u); break;
            case 1: v = g.S8(kS + 10u); break;
            case 2: v = g.S8(kS + 11u); break;
            case 17: v = g.S8(kS + 20u); break;
            case 18: v = g.S8(kS + 23u); break;
            case 19: v = g.S8(kS + 25u); break;
            case 21: v = g.S8(kS + g.U8(kFe + 0x0Eu) + 192u); break;
            case 24: v = g.S8(kP0 + 23u); break;
            case 25: v = g.S8(kFe + 25u); break;
            default: continue;
            }
            break;
        }
        case 7: v = g.S8(kS + 7u); break;    // 0x80063B8C
        case 8: v = g.U8(kFe + 0x0Eu); break; // 0x80063B80
        case 9: {                            // 0x80063A94
            const int8_t kind = g.S8(c + 2u);
            if (kind == 26) v = g.S8(kP0 + 21u);
            else if (kind == 27) v = g.S8(kP0 + 57u);
            else continue;
            break;
        }
        case 11: {                           // 0x80063B98
            const int8_t kind = g.S8(c + 2u);
            if (kind == 33) {
                g.W8(kFePort, 0);
                v = 0;
            } else if (kind == 34) {
                v = g.S8(kP0 + 8u);
            } else {
                continue;
            }
            break;
        }
        default: continue; // 10: 0x80063BD0
        }
        ChooserSet(g, c, v);
    }
}

int32_t ResumeDispatch(GuestRam& g, ShellCallees& k, bool* ok) {
    bool good = true;
    int32_t s0 = 4;
    if (g.U32(kDemo) != 0) { // 0x8007FF88: the attract path
        const uint8_t n = static_cast<uint8_t>(g.U8(kS + 0x0Fu) + 1u);
        g.W8(kS + 0x0Fu, n);
        if (!(static_cast<int8_t>(n) < 38)) g.W8(kS + 0x0Fu, 32);
        ShortCommit(g);
        s0 = g.S8(kS + 0x0Cu);
    } else { // 0x8007FFC8
        const uint8_t flags = g.U8(kS + 0x05u), keep = g.U8(kS + 0x1Au);
        const uint32_t mode = g.U32(kS);
        g.W8(kS + 0x05u, static_cast<uint8_t>(flags & 0xC7u));
        g.W8(kS + 0x19u, keep);
        switch (mode) { // the 32-entry table 0x8005CB80 by mode - 1
        case 1:
            g.W8(kS + 0x18u, 1);
            s0 = FiveOResult(g, k, &good);
            break;
        case 32:
            g.W8(kS + 0x18u, 1);
            s0 = CareerResult(g, k, &good);
            break;
        case 8:
            g.W8(kS + 0x18u, 1);
            s0 = SideCarResult(g, k, &good);
            break;
        case 4: { // 0x8008005C: Time Trial
            const int8_t single = g.S8(kS + 0x16u);
            g.W8(kFe + 0x22u, 0);
            if (single != 0) {
                const int32_t race = g.S8(kS + 0x08u);
                const uint32_t time = g.U32(kP0);
                g.W8(kFe + 0x22u, 1);
                g.W16(kFeHub, 24);
                if (BeatsRecord(g, race, time)) {
                    g.W8(kS + 0x18u, 1);
                    s0 = 52;
                } else {
                    s0 = 24;
                    NewGame(g, 24);
                }
            } else {
                // fe+0x23 = session+0x13 + 1; session+0x13 itself is NOT advanced here.
                const uint8_t round = static_cast<uint8_t>(g.U8(kS + 0x13u) + 1u);
                g.W8(kFe + 0x23u, round);
                if (round < g.U8(kS + 0x12u)) {
                    s0 = 40;
                    g.W8(kFe + 0x22u, 0);
                } else {
                    s0 = 38;
                    g.W8(kFe + 0x22u, 1);
                }
                g.W16(kFeHub, static_cast<uint16_t>(s0));
                const uint32_t who = g.U8(kS + 0x13u);
                const int32_t race = g.S8(kS + 0x08u);
                if (BeatsRecord(g, race, g.U32(kP0 + 36u * who))) {
                    g.W8(kS + 0x18u, 1);
                    s0 = 52;
                } else if (g.S8(kFe + 0x22u) != 0) {
                    NewGame(g, 38);
                }
            }
            break;
        }
        case 16: s0 = 30; break;
        case 17: s0 = 32; break;
        case 24: s0 = 34; break;
        default: s0 = 4; break;
        }
    }
    if (ok != nullptr) *ok = good;
    return s0;
}

void ResumeShow(GuestRam& g, int32_t hub) {
    GotoScreen(g, static_cast<int16_t>(hub));
    int32_t p = g.S16(g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(hub)) + 10u);
    while (p != -1) {
        ArmScreen(g, p);
        p = g.S16(g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(p)) + 10u);
    }
}

} // namespace rr::shell
