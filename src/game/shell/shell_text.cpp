// The front end's text (shell_text.h). Ported from our reading of RASHCDF.BIN; each function names its
// original. Stack addresses: `sp` is the original's stack pointer at the function's entry, and every
// rectangle / sprintf buffer is written at the address the original keeps it (Ghidra's local_XX is
// entry sp - 0xXX), so a seam call carries the very pointer the original passes.
#include "game/shell/shell_text.h"

#include "game/shell/shell_memcard.h"
#include "game/shell/shell_screens.h"

#include <initializer_list>

namespace rr::shell {

namespace {

constexpr uint32_t kS = kSession;
constexpr uint32_t kP0 = kPlayers;
constexpr uint32_t kRecordTimes = 0x80099500;  // s32[6] the per-venue record time, frames / 60
constexpr uint32_t kRecordQuota = 0x80099530;  // s16[6] the per-venue record quota
constexpr uint32_t kFmtOfN = 0x8005C338;       // "%01d%s%01d"
constexpr uint32_t kFmtTime = 0x8005C574;      // "%d:%02d"
constexpr uint32_t kFmt02 = 0x8005C57C;        // "%02d"
constexpr uint32_t kFmtD = 0x8005C584;         // "%d"
constexpr uint32_t kCardScreenRec = 0x8009954C; // Screen* whose +0x08 the card arms take

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
int32_t SignedHalf(uint32_t v) { return static_cast<int16_t>(v & 0xFFFFu); }

// PTR_DAT_8009cfc8 + fe+0x11 * 4 + 4: the ordering-table entry of the layer being drawn.
uint32_t Ot(GuestRam& g, uint32_t extra = 4u) {
    return g.U32(kOtTable) + 4u * U(g.S8(kFeLayer)) + extra;
}
// (&DAT_800d807b)[slot * 0x18] = 0 - the font slot's blend index, cleared before its strings.
void Unblend(GuestRam& g, uint32_t handle) { g.W8(kFontSlots + 0x18u * g.U32(handle) + 3u, 0); }

bool Call(ShellCallees& k, uint32_t address, std::initializer_list<uint32_t> args) {
    uint32_t a[12] = {};
    int n = 0;
    for (uint32_t v : args)
        if (n < 12) a[n++] = v;
    return k.Call(address, a, n, nullptr);
}
bool Id(GuestRam& g, ShellCallees& k, uint32_t id, uint32_t rect, uint32_t rgb, uint32_t just) {
    return Call(k, kTextId, {g.U32(kFontMain), id, rect, Ot(g), rgb, just});
}
bool Str(GuestRam& g, ShellCallees& k, uint32_t buf, uint32_t rect, uint32_t rgb, uint32_t just) {
    return Call(k, kTextStr, {g.U32(kFontMain), buf, rect, Ot(g), rgb, just});
}
void Rect(GuestRam& g, uint32_t at, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    g.W16(at, static_cast<uint16_t>(x));
    g.W16(at + 2u, static_cast<uint16_t>(y));
    g.W16(at + 4u, static_cast<uint16_t>(w));
    g.W16(at + 6u, static_cast<uint16_t>(h));
}
// The widget's rectangle with x moved by `dx` (the "value" column of a label / value pair).
void RectFrom(GuestRam& g, uint32_t at, uint32_t w, int32_t dx) {
    Rect(g, at, g.U16(w + 0x10u) + U(dx), g.U16(w + 0x12u), g.U16(w + 0x14u), g.U16(w + 0x16u));
}
uint32_t Rgb(GuestRam& g, uint32_t w) { return g.U32(w + 0x1Cu); }
uint32_t Just(GuestRam& g, uint32_t w) { return g.U16(w + 0x1Au); }

// The gate the setup-info arms open with: the screen's highlighted item carries flag 0x100.
bool SelectedShowsInfo(GuestRam& g, uint32_t screen) {
    const uint32_t item = U(g.S16(screen + 4u) * 0x78) + g.U32(screen + 0x10u);
    return (g.U16(item + 10u) & 0x100u) != 0;
}

// A label at the widget's rectangle and a value id at the rectangle moved by dx (arms 1, 2, 4, 0x10,
// 0x11, 0x14, 0x15 and the bike / monkey rows share this shape).
bool LabelAndId(GuestRam& g, ShellCallees& k, uint32_t screen, uint32_t w, uint32_t sp, uint32_t label, int32_t dx,
                uint32_t (*value)(GuestRam&)) {
    if (!SelectedShowsInfo(g, screen)) return true;
    Unblend(g, kFontMain);
    if (!Id(g, k, label, w + 0x10u, Rgb(g, w), Just(g, w))) return false;
    const uint32_t rect = sp - 0x20u;
    RectFrom(g, rect, w, dx);
    return Id(g, k, value(g), rect, Rgb(g, w), Just(g, w));
}

uint32_t ModeName(GuestRam& g) { return U(SignedHalf(U(ModeNameId(g.S8(kS + 4u), g.U32(kS))))); }

// 0x800733BC / 0x80074194 / 0x80073BAC share this step: one frame of a counter walking to its target
// in strides that shrink as it closes in, a tick (UI sound 13) per step and a chime (14) on arrival.
// Arm 0x1B's first two counters.
bool StepBonus(GuestRam& g, ShellCallees& k, uint32_t shown, int32_t target) {
    const int32_t cur = g.S16(shown);
    int32_t next = cur;
    if (cur < target - 90) next = cur + 90;
    else if (cur < target - 15) next = cur + 15;
    else if (cur < target) next = cur + 1;
    else if (target + 90 < cur) next = cur - 90;
    else if (target < cur) next = cur - 1;
    else return true;
    g.W16(kFeTick, static_cast<uint16_t>(g.U16(kFeTick) + 1u));
    if (g.S16(kFeTick) <= 0) return true;
    g.W16(shown, static_cast<uint16_t>(next));
    g.W16(kFeTick, 0);
    return k.Call1(kUiSound, SignedHalf(U(next)) == target ? 14u : 13u);
}

// ------------------------------------------------------------------------------------------ the arms
// RASHCDF 0x80072AF4 (kind 1): "Rank:" and the mode's rank name, value 0x5F to the right.
bool Arm01(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    return LabelAndId(g, k, s, w, sp, 0x18, 0x5F, ModeName);
}
// RASHCDF 0x80072C44 (kind 2): the same at 0x78.
bool Arm02(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    return LabelAndId(g, k, s, w, sp, 0x18, 0x78, ModeName);
}
// RASHCDF 0x80072D94 (kind 3): "Qualifications:" and "n of 6 / 9" (9 in a career).
bool Arm03(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    if (!SelectedShowsInfo(g, s)) return true;
    Unblend(g, kFontMain);
    if (!Id(g, k, 0xDE, w + 0x10u, Rgb(g, w), Just(g, w))) return false;
    const uint32_t rect = sp - 0x58u, buf = sp - 0x50u;
    RectFrom(g, rect, w, 0x78);
    const uint32_t of = g.U32(kS) == 0x20u ? 9u : 6u;
    if (!Call(k, kSprintf, {buf, kFmtOfN, g.U8(kS + 0x11u), g.U32(g.U32(kStringTable) + 4u), of})) return false;
    return Str(g, k, buf, rect, Rgb(g, w), Just(g, w));
}
// RASHCDF 0x80072F2C (kind 4): "Course:" (0x19) and the race's course name.
bool Arm04(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    return LabelAndId(g, k, s, w, sp, 0x19, 0x5F,
                      [](GuestRam& gg) { return U(SignedHalf(U(CourseNameId(gg, gg.S8(kS + 8u))))); });
}
// RASHCDF 0x80073074 (kinds 5, 6, 7): "Bike:" / "P 1 Bike:" / "P 2 Bike:" and the bike's name.
bool Arm05(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp, uint32_t kind) {
    if (!SelectedShowsInfo(g, s)) return true;
    Unblend(g, kFontMain);
    uint32_t label = 0x1C, player = 1;
    if (kind == 5) label = 0x1A, player = 0;
    else if (kind == 6) label = 0x1B, player = 0;
    if (!Id(g, k, label, w + 0x10u, Rgb(g, w), Just(g, w))) return false;
    const uint32_t rect = sp - 0x18u;
    RectFrom(g, rect, w, 0x5F);
    const uint32_t p = g.U32(kS) == 4u ? g.U8(kS + 0x13u) : player;
    const int32_t bike = g.S8(kP0 + 4u + p * 0x24u + 3u);
    return Id(g, k, U(SignedHalf(U(BikeNameId(bike)))), rect, Rgb(g, w), Just(g, w));
}
// RASHCDF 0x80073234 (kinds 8, 9): "Monkey:" and who drives player 0 / 1 (Human / AI / None).
bool Arm08(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp, uint32_t kind) {
    if (!SelectedShowsInfo(g, s)) return true;
    Unblend(g, kFontMain);
    if (!Id(g, k, 199, w + 0x10u, Rgb(g, w), Just(g, w))) return false;
    const uint32_t rect = sp - 0x20u;
    RectFrom(g, rect, w, 0x5F);
    const int32_t c = g.S8(kP0 + 0x14u + (kind != 8 ? 0x24u : 0u) + 1u);
    const uint32_t id = c == 1 ? 0xC4u : (c == 2 ? 0xC5u : 0xC6u);
    return Id(g, k, id, rect, Rgb(g, w), Just(g, w));
}
// RASHCDF 0x8007219C (kind 0x0F): the Time Trial player's heading, id session+0x13 + 0x12.
bool Arm0F(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w) {
    if (!SelectedShowsInfo(g, s)) return true;
    Unblend(g, kFontMain);
    return Id(g, k, g.U8(kS + 0x13u) + 0x12u, w + 0x10u, Rgb(g, w), Just(g, w));
}
// RASHCDF 0x80072248 (kind 0x10): "Players:" and the count, id session+0x12 + 7.
bool Arm10(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    return LabelAndId(g, k, s, w, sp, 0x1D, 0x5F, [](GuestRam& gg) { return gg.U8(kS + 0x12u) + 7u; });
}
// RASHCDF 0x80072388 (kind 0x11): "Fugitive:" and the mission's name, id session+0x07 * 6 + 0x129.
bool Arm11(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    return LabelAndId(g, k, s, w, sp, 0x1E, 0x5F, [](GuestRam& gg) { return U(gg.S8(kS + 7u) * 6 + 0x129); });
}
// RASHCDF 0x800724D0 (kind 0x12): "Time:" and the venue's record, m:ss.
bool Arm12(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    if (!SelectedShowsInfo(g, s)) return true;
    Unblend(g, kFontMain);
    if (!Id(g, k, 0x637, w + 0x10u, Rgb(g, w), Just(g, w))) return false;
    const uint32_t rect = sp - 0x60u, buf = sp - 0x58u;
    RectFrom(g, rect, w, 0x5F);
    const int32_t t = g.S32(kRecordTimes + 4u * U(g.S8(kS + 4u)));
    if (!Call(k, kSprintf, {buf, kFmtTime, U(t / 60), U(t % 60)})) return false;
    return Str(g, k, buf, rect, Rgb(g, w), Just(g, w));
}
// RASHCDF 0x80072668 (kind 0x13): "Quota:" and the venue's quota.
bool Arm13(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    if (!SelectedShowsInfo(g, s)) return true;
    Unblend(g, kFontMain);
    if (!Id(g, k, 0x638, w + 0x10u, Rgb(g, w), Just(g, w))) return false;
    const uint32_t rect = sp - 0x60u, buf = sp - 0x58u;
    RectFrom(g, rect, w, 0x5F);
    if (!Call(k, kSprintf, {buf, kFmt02, U(g.S16(kRecordQuota + 2u * U(g.S8(kS + 4u))))})) return false;
    return Str(g, k, buf, rect, Rgb(g, w), Just(g, w));
}
// RASHCDF 0x800727C8 (kind 0x14): "Gang:" and the player's gang.
bool Arm14(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    return LabelAndId(g, k, s, w, sp, 0x1F, 0x3C, [](GuestRam& gg) { return U(SignedHalf(U(GangNameId(gg.S8(kP0 + 9u))))); });
}
// RASHCDF 0x80072910 (kind 0x15): "Alias:" and the player's rank-in-gang name.
bool Arm15(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    return LabelAndId(g, k, s, w, sp, 0x20, 0x3C, [](GuestRam& gg) {
        return U(SignedHalf(U(RankNameId(gg.S8(kP0 + 9u), gg.S8(kP0 + 10u)))));
    });
}

// RASHCDF 0x800733BC (kind 0x1B): THE CAREER RESULT PANEL - Race Bonus, Combat Bonus and Total Bonus
// counted up from fe+0x54.. to fe+0x70.. (what the career dispatcher 0x8007BB34 wrote), then the
// "Rash Cash:" rows naming what the bonus bought (fe+0x78 / fe+0x7A).
bool Arm1B(GuestRam& g, ShellCallees& k, uint32_t w, uint32_t sp) {
    const uint32_t rect = sp - 0xA0u, buf = sp - 0x98u;
    Unblend(g, kFontMain);
    const uint32_t x = g.U16(w + 0x10u);
    int32_t y = g.S16(w + 0x12u);
    Rect(g, rect, x + 5u, U(y), 0x2A, g.U16(w + 0x16u));
    for (uint32_t id : {0x5FBu, 0x5FCu, 0x5FDu}) {
        g.W16(rect + 2u, static_cast<uint16_t>(y));
        if (!Id(g, k, id, rect, Rgb(g, w), Just(g, w))) return false;
        y += 14;
    }
    y = g.S16(w + 0x12u);
    g.W16(rect, static_cast<uint16_t>(x + 0xA0u));
    g.W16(rect + 2u, static_cast<uint16_t>(y));
    // Race bonus.
    if (!StepBonus(g, k, kFeShown, g.S16(kFeBonusTarget))) return false;
    if (!Call(k, kSprintf, {buf, kFmtD, U(g.S16(kFeShown))})) return false;
    if (!Str(g, k, buf, rect, 0x1C058E, 1)) return false;
    // Combat bonus, once the race bonus has arrived.
    if (g.S16(kFeShown) == g.S16(kFeBonusTarget)) {
        g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 0xA0u));
        y += 14;
        g.W16(rect + 2u, static_cast<uint16_t>(y));
        if (!StepBonus(g, k, kFeShown + 2u, g.S16(kFeBonusTarget + 2u))) return false;
        if (!Call(k, kSprintf, {buf, kFmtD, U(g.S16(kFeShown + 2u))})) return false;
        if (!Str(g, k, buf, rect, 0x1C058E, 1)) return false;
    }
    if (g.U32(kFeShown) != g.U32(kFeBonusTarget)) return true;
    // Total bonus, strides 15 / 450 / 90 / 1 in the original's own order.
    g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 0xA0u));
    y += 14;
    g.W16(rect + 2u, static_cast<uint16_t>(y));
    {
        const int32_t target = g.S16(kFeBonusTarget + 4u), cur = g.S16(kFeShown + 4u);
        int32_t next = cur;
        bool step = true;
        if (cur < target - 15) next = cur + 15;
        else if (cur < target - 450) next = cur + 450;
        else if (cur < target - 90) next = cur + 90;
        else if (cur < target) next = cur + 1;
        else if (target + 450 < cur) next = cur - 450;
        else if (target + 90 < cur) next = cur - 90;
        else if (target < cur) next = cur - 1;
        else step = false;
        if (step) {
            const uint16_t t = static_cast<uint16_t>(g.U16(kFeTick) + 1u);
            g.W16(kFeTick, t);
            if (static_cast<int16_t>(t) > 0) {
                g.W16(kFeTick, 0);
                g.W16(kFeShown + 4u, static_cast<uint16_t>(next));
                if (!k.Call1(kUiSound, SignedHalf(U(next)) != target ? 13u : 14u)) return false;
            }
        }
    }
    if (!Call(k, kSprintf, {buf, kFmtD, U(g.S16(kFeShown + 4u))})) return false;
    if (!Str(g, k, buf, rect, 0x1C058E, 1)) return false;
    if (g.U32(kFeShown) == g.U32(kFeBonusTarget) && g.S16(kFeShown + 4u) == g.S16(kFeBonusTarget + 4u) &&
        g.S16(kFeBonusTarget + 8u) != 0) {
        const uint32_t x0 = g.U16(w + 0x10u);
        g.W16(rect, static_cast<uint16_t>(x0 + 5u));
        y += 28;
        g.W16(rect + 2u, static_cast<uint16_t>(y));
        if (!Id(g, k, 0x5FE, rect, Rgb(g, w), Just(g, w))) return false;
        g.W16(rect, static_cast<uint16_t>(x0 + 0x72u));
        if (!Id(g, k, U(g.S16(kFeBonusTarget + 8u)), rect, 0x1C058E, Just(g, w))) return false;
        if (g.S16(kFeBonusTarget + 10u) != 0) {
            g.W16(rect, static_cast<uint16_t>(x0 + 5u));
            y -= 14;
            g.W16(rect + 2u, static_cast<uint16_t>(y));
            if (!Id(g, k, 0x5FE, rect, Rgb(g, w), Just(g, w))) return false;
            g.W16(rect, static_cast<uint16_t>(x0 + 0x72u));
            if (!Id(g, k, U(g.S16(kFeBonusTarget + 10u)), rect, 0x1C058E, Just(g, w))) return false;
        }
    }
    return true;
}

// RASHCDF 0x80073BAC (kind 0x1C): the combat tally, Pro / Con columns of Steals, Hits and Takedowns
// (player 0's counters +0x18/+0x1C, +0x1A/+0x1E, +0x19/+0x1D), each row counted up in turn.
bool Arm1C(GuestRam& g, ShellCallees& k, uint32_t w, uint32_t sp) {
    const uint32_t rect = sp - 0xB0u, buf = sp - 0xA8u;
    const uint32_t x = g.U16(w + 0x10u);
    Unblend(g, kFontMain);
    uint32_t y = g.U16(w + 0x12u);
    Rect(g, rect, x + 0x78u, y, g.U16(w + 0x14u), g.U16(w + 0x16u));
    if (!Id(g, k, 0x5EF, rect, Rgb(g, w), Just(g, w))) return false;
    g.W16(rect, static_cast<uint16_t>(x + 0xAAu));
    if (!Id(g, k, 0x5F0, rect, Rgb(g, w), Just(g, w))) return false;
    g.W16(rect, static_cast<uint16_t>(x + 10u));
    for (uint32_t id : {0x5F1u, 0x5F2u, 0x5F3u}) {
        y += 14u;
        g.W16(rect + 2u, static_cast<uint16_t>(y));
        if (!Id(g, k, id, rect, Rgb(g, w), Just(g, w))) return false;
    }
    g.W16(rect, static_cast<uint16_t>(x + 0x78u));
    y = g.U16(w + 0x12u) + 14u;
    g.W16(rect + 2u, static_cast<uint16_t>(y));
    const uint32_t a = kFeDuelShown, tick = kFeDuelTick;
    auto walk = [&](uint32_t pa, int32_t ta, uint32_t pb, int32_t tb) {
        if (g.S16(pa) < ta || g.S16(pb) < tb) {
            g.W16(tick, static_cast<uint16_t>(g.U16(tick) + 1u));
            if (g.S16(tick) > 5) {
                g.W16(tick, 0);
                if (g.S16(pa) < ta) g.W16(pa, static_cast<uint16_t>(g.U16(pa) + 1u));
                if (g.S16(pb) < tb) g.W16(pb, static_cast<uint16_t>(g.U16(pb) + 1u));
            }
        }
    };
    auto row = [&](uint32_t pa, uint32_t pb) {
        if (!Call(k, kSprintf, {buf, kFmt02, U(g.S16(pa))})) return false;
        if (!Str(g, k, buf, rect, 0x1C058E, Just(g, w))) return false;
        g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 0xAAu));
        if (!Call(k, kSprintf, {buf, kFmt02, U(g.S16(pb))})) return false;
        return Str(g, k, buf, rect, 0x1C058E, Just(g, w));
    };
    walk(a, g.U8(kP0 + 0x18u), a + 2u, g.U8(kP0 + 0x1Cu));
    if (!row(a, a + 2u)) return false;
    if (g.S16(a) != static_cast<int32_t>(g.U8(kP0 + 0x18u))) return true;
    if (g.S16(a + 2u) == static_cast<int32_t>(g.U8(kP0 + 0x1Cu))) {
        g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 0x78u));
        y += 14u;
        g.W16(rect + 2u, static_cast<uint16_t>(y));
        walk(a + 4u, g.U16(kP0 + 0x1Au), a + 6u, g.U16(kP0 + 0x1Eu));
        if (!row(a + 4u, a + 6u)) return false;
    }
    if (g.S16(a) == static_cast<int32_t>(g.U8(kP0 + 0x18u)) && g.S16(a + 2u) == static_cast<int32_t>(g.U8(kP0 + 0x1Cu)) &&
        g.S16(a + 4u) == static_cast<int32_t>(g.U16(kP0 + 0x1Au)) &&
        g.S16(a + 6u) == static_cast<int32_t>(g.U16(kP0 + 0x1Eu))) {
        g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 0x78u));
        y += 14u;
        g.W16(rect + 2u, static_cast<uint16_t>(y));
        walk(a + 8u, g.U8(kP0 + 0x19u), a + 10u, g.U8(kP0 + 0x1Du));
        if (!row(a + 8u, a + 10u)) return false;
    }
    return true;
}

// RASHCDF 0x80074194 (kind 0x1D): the Five-O result - "Current Rank:" and the rank name, then Beats
// Cleared counted up to session+0x11, Wins by Capture to session+0x20, Wins by Quota to session+0x22.
bool Arm1D(GuestRam& g, ShellCallees& k, uint32_t w, uint32_t sp) {
    const uint32_t rect = sp - 0xA0u, buf = sp - 0x98u;
    Unblend(g, kFontMain);
    uint32_t y = g.U16(w + 0x12u);
    Rect(g, rect, g.U16(w + 0x10u) + 10u, y, g.U16(w + 0x14u), g.U16(w + 0x16u));
    if (!Id(g, k, 0x605, rect, Rgb(g, w), Just(g, w))) return false;
    g.W16(rect, static_cast<uint16_t>(g.U16(rect) + 0xA0u));
    if (!Id(g, k, ModeName(g), rect, Rgb(g, w), Just(g, w))) return false;
    const uint32_t beats = kFeFiveShown, capture = kFeFiveShown + 2u, quota = kFeFiveShown + 4u;
    auto tickStep = [&]() {
        g.W8(kFeFiveTick, static_cast<uint8_t>(g.U8(kFeFiveTick) + 1u));
        if (g.S8(kFeFiveTick) > 5) {
            g.W8(kFeFiveTick, 0);
            return true;
        }
        return false;
    };
    if (g.S16(beats) < static_cast<int32_t>(g.U8(kS + 0x11u)) && tickStep()) g.W16(beats, static_cast<uint16_t>(g.U16(beats) + 1u));
    g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 10u));
    y += 14u;
    g.W16(rect + 2u, static_cast<uint16_t>(y));
    if (!Id(g, k, U(SignedHalf(g.U16(beats) + 0x5E6u)), rect, Rgb(g, w), Just(g, w))) return false;
    if (g.S16(beats) != static_cast<int32_t>(g.U8(kS + 0x11u))) return true;
    if (g.S16(capture) < g.S16(kS + 0x20u) && tickStep()) g.W16(capture, static_cast<uint16_t>(g.U16(capture) + 1u));
    y += 14u;
    g.W16(rect + 2u, static_cast<uint16_t>(y));
    if (!Id(g, k, 0x5ED, rect, Rgb(g, w), Just(g, w))) return false;
    g.W16(rect, static_cast<uint16_t>(g.U16(rect) + 0xA0u));
    if (!Call(k, kSprintf, {buf, kFmt02, U(g.S16(capture))})) return false;
    if (!Str(g, k, buf, rect, Rgb(g, w), Just(g, w))) return false;
    if (g.S16(beats) != static_cast<int32_t>(g.U8(kS + 0x11u)) || g.S16(capture) != g.S16(kS + 0x20u)) return true;
    if (g.S16(quota) < g.S16(kS + 0x22u) && tickStep()) g.W16(quota, static_cast<uint16_t>(g.U16(quota) + 1u));
    g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 10u));
    y += 14u;
    g.W16(rect + 2u, static_cast<uint16_t>(y));
    if (!Id(g, k, 0x5EE, rect, Rgb(g, w), Just(g, w))) return false;
    g.W16(rect, static_cast<uint16_t>(g.U16(rect) + 0xA0u));
    if (!Call(k, kSprintf, {buf, kFmt02, U(g.S16(quota))})) return false;
    return Str(g, k, buf, rect, Rgb(g, w), Just(g, w));
}

// RASHCDF 0x80074554 (kind 0x1E): THE RAP SHEET - the career at a glance: alias (rank in the gang),
// gang, rank, bike, qualifications n/9, the nitro count (player+0x14), how many weapons have ammo
// (the non-zero nibbles 0..5 of player+0x10), and the weapons owned (player+0x0C bits 0..8), each a
// sprite with its ammo count on a badge and a frame. Frame 296 bytes; the stack records are the
// original's own (Ghidra's local_XX / uStack_XX = entry sp - 0xXX).
bool Arm1E(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    const uint32_t f = sp - 0x128u; // the arm's own sp
    const uint32_t buf = f + 24u, rect0 = f + 0x98u, rectA = f + 0xA0u, rect1 = f + 0xA8u, rect2 = f + 0xB0u;
    const uint32_t weapon = f + 0xB8u, badge = f + 0xC8u, outer = f + 0xD8u, inner = f + 0xE8u;
    const uint32_t ot = Ot(g);
    Unblend(g, kFontMain);
    RectFrom(g, rect0, w, 0x5F);
    g.W32(w + 0x1Cu, 0x00FF00FFu);
    Rect(g, rectA, 0x92, 0x37, 0xDC, 0x82);
    Rect(g, rect1, 0x1E, 0x37, 0x6B, 0x82);
    Rect(g, rect2, 0xAA, 0x37, 0x6E, 0x82);
    auto id = [&](uint32_t sid, uint32_t rect, uint32_t rgb, uint32_t just) {
        return Call(k, kTextId, {g.U32(kFontMain), sid, rect, ot, rgb, just});
    };
    auto str = [&](uint32_t rect, uint32_t rgb, uint32_t just) {
        return Call(k, kTextStr, {g.U32(kFontMain), buf, rect, ot, rgb, just});
    };
    auto down = [&](int32_t dy) {
        g.W16(rect2 + 2u, static_cast<uint16_t>(g.U16(rect2 + 2u) + static_cast<uint32_t>(dy)));
        g.W16(rect1 + 2u, static_cast<uint16_t>(g.U16(rect1 + 2u) + static_cast<uint32_t>(dy)));
    };
    const uint32_t p0 = kP0;
    down(14);
    if (!id(0xDA, rect1, 0x286EB4, 0) || !id(U(SignedHalf(U(RankNameId(g.S8(p0 + 9u), g.S8(p0 + 10u))))), rect2, 0xB9A096, 0))
        return false;
    down(14);
    if (!id(0xDB, rect1, 0x286EB4, 0) || !id(U(SignedHalf(U(GangNameId(g.S8(p0 + 9u))))), rect2, 0xB9A096, 0)) return false;
    down(14);
    if (!id(0xDC, rect1, 0x286EB4, 0) || !id(ModeName(g), rect2, 0xB9A096, 0)) return false;
    const uint32_t bike = U(BikeNameId(g.S8(p0 + 7u)));
    down(14);
    if (!id(0xDD, rect1, 0x286EB4, 0) || !id(bike, rect2, 0xB9A096, 0)) return false;
    down(14);
    if (!Call(k, kSprintf, {buf, 0x8005C588u, g.U8(kS + 0x11u)})) return false;
    if (!id(0xDE, rect1, 0x286EB4, 0) || !str(rect2, 0xB9A096, 0)) return false;
    down(14);
    if (!id(0x630, rect1, 0x286EB4, 0)) return false;
    g.W16(rect1, 200);
    g.W16(rect2, 0xAA);
    if (!Call(k, kSprintf, {buf, 0x8005C590u, g.U8(p0 + 0x14u)})) return false;
    if (!id(g.U8(p0 + 0x14u) == 1u ? 0x631u : 0x632u, rect1, 0xB9A096, 0) || !str(rect2, 0xB9A096, 0)) return false;
    uint32_t armed = 0;
    for (int32_t i = 5; i >= 0; --i)
        if ((g.U32(p0 + 0x10u) >> (4u * static_cast<uint32_t>(i))) & 15u) ++armed;
    armed &= 0xFFu;
    g.W16(rect1, 0x127);
    g.W16(rect2, 0x109);
    if (!Call(k, kSprintf, {buf, 0x8005C590u, armed})) return false;
    if (!id(armed == 1u ? 0x633u : 0x634u, rect1, 0xB9A096, 0) || !str(rect2, 0xB9A096, 0)) return false;
    // "Current Weapons:" across the whole width, then the nine weapon slots.
    g.W16(rect1 + 4u, 0x200);
    g.W16(rect1, 0);
    g.W16(rect2 + 2u, static_cast<uint16_t>(g.U16(rect2 + 2u) + 0x14u));
    g.W16(rect1 + 2u, static_cast<uint16_t>(g.U16(rect1 + 2u) + 0x0Eu));
    if (!id(0xDF, rect1, 0x286EB4, 2)) return false;
    g.W32(weapon + 4u, 0x01808080u);
    g.W32(badge + 4u, 0x03808080u);
    g.W32(badge, 0x4F4C4148u);
    static const uint32_t kWeapons[9] = {0x4E414843u, 0x42554C43u, 0x45504950u, 0x444F4F57u, 0x4B48434Eu,
                                         0x52414243u, 0x444F5250u, 0x4E555453u, 0x59525053u};
    for (uint32_t i = 0; i < 9u; ++i) {
        const uint32_t x = 57u * i + 11u, y = g.U16(rect1 + 2u) + 20u;
        g.W16(weapon + 10u, static_cast<uint16_t>(y));
        g.W16(weapon + 8u, static_cast<uint16_t>(x));
        if ((static_cast<uint32_t>(g.U16(p0 + 0x0Cu)) >> i) & 1u) {
            g.W32(weapon, kWeapons[i]);
            const uint32_t ammo = i < 6u ? (g.U32(p0 + 0x10u) >> (4u * i)) & 15u : 0u;
            if (ammo != 0) {
                g.W16(badge + 8u, static_cast<uint16_t>(x));
                g.W16(badge + 10u, static_cast<uint16_t>(y));
                g.W16(rect2, static_cast<uint16_t>(x + 36u));
                g.W16(rect2 + 2u, static_cast<uint16_t>(y + 24u));
                if (!Call(k, kSprintf, {buf, 0x8005C590u, ammo})) return false;
                if (!str(rect2, 0xB9A096, 0)) return false;
                if (!Call(k, kSpriteEmit, {s, weapon, ot})) return false;
                if (!Call(k, kSpriteEmit, {s, badge, ot})) return false;
            } else if (!Call(k, kSpriteEmit, {s, weapon, ot})) {
                return false;
            }
        }
        // The slot's frame: an outer and an inner rectangle, 0x1E1414.
        g.W16(inner + 0x10u, 1);
        g.W16(inner, 0);
        g.W16(inner + 2u, 1);
        g.W32(inner + 4u, 0x001E1414u);
        g.W16(inner + 8u, static_cast<uint16_t>(x));
        g.W16(inner + 10u, static_cast<uint16_t>(y));
        g.W16(inner + 12u, static_cast<uint16_t>(x + 46u));
        g.W16(inner + 14u, static_cast<uint16_t>(y + 32u));
        g.W16(outer, 0);
        g.W16(outer + 2u, 0);
        g.W32(outer + 4u, 0x001E1414u);
        g.W16(outer + 8u, static_cast<uint16_t>(x - 4u));
        g.W16(outer + 10u, static_cast<uint16_t>(y - 2u));
        g.W16(outer + 12u, static_cast<uint16_t>(x + 50u));
        g.W16(outer + 14u, static_cast<uint16_t>(y + 34u));
        if (!Call(k, kPanelEmit, {s, outer, ot})) return false;
    }
    return true;
}

// RASHCDF 0x8007593C (kind 0x0A): the controller screen's legend - "Controls" (0xD3) in BTN_FONT
// over the widget, then six rows of a pad-button sprite (JPUD JP_X JPLR TRIG SQRE CRCL, the
// FourCCs at 0x80088DF4) and its action (0xD4..0xD9) in MINIFONT. Frame 88: the rectangle at
// entry-0x40, the sprite record at entry-0x38.
bool Arm0A(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    const uint32_t rect = sp - 0x40u, spr = sp - 0x38u;
    const uint32_t ot4 = Ot(g);
    Unblend(g, kFontMini);
    Rect(g, rect, g.U16(w + 0x10u) + 16u, g.U16(w + 0x12u), g.U16(w + 0x14u), g.U16(w + 0x16u));
    if (!Call(k, kTextId, {g.U32(kFontMain), 0xD3, w + 0x10u, ot4, 0xB9A096u, 2})) return false;
    g.W32(spr, 300);
    g.W32(spr + 4u, 0x00808080u);
    const uint16_t y = static_cast<uint16_t>(g.U16(rect + 2u) + 14u);
    g.W16(rect + 2u, y);
    g.W16(spr + 10u, y);
    g.W16(spr + 8u, g.U16(w + 0x10u));
    for (uint32_t i = 0; i < 6u; ++i) {
        g.W32(spr, g.U32(0x80088DF4u + 4u * i));
        if (!Call(k, kSpriteEmit, {s, spr, Ot(g, 20u)})) return false;
        if (!Call(k, kTextId, {g.U32(kFontMini), 0xD4u + i, rect, ot4, Rgb(g, w), Just(g, w)})) return false;
        g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 12u));
        g.W16(spr + 10u, static_cast<uint16_t>(g.U16(spr + 10u) + 12u));
    }
    return true;
}

// RASHCDF 0x800642DC / 0x80064474: the previous / next selectable option of a chooser (the gate of
// OptionAvailable), wrapping; the current one when no other is.
int32_t ChooserPrevValid(GuestRam& g, uint32_t c) {
    int32_t i = g.S8(c + 3u) - 1;
    if (i < 0) i = g.S8(c) - 1;
    while (i != g.S8(c + 3u) && !OptionAvailable(g, g.U32(c + 4u) + U(i * 12))) {
        --i;
        if (i < 0) i = g.S8(c) - 1;
    }
    return i;
}
int32_t ChooserNextValid(GuestRam& g, uint32_t c) {
    const int32_t cur = g.S8(c + 3u);
    int32_t i = cur + 1;
    if (g.S8(c) <= i) i = 0;
    while (i != cur) {
        if (OptionAvailable(g, g.U32(c + 4u) + U(i * 12))) return i;
        ++i;
        if (g.S8(c) <= i) i = 0;
    }
    return i;
}

// RASHCDF 0x80075D0C (kind 0x28): the jukebox - three headings (0x616..0x618, font slot 1), then five
// rows of the track chooser (object 31) around the current track (fe+0x06 rows above it): its name,
// artist and title through the text source (kinds 0x27, 0x39, 0x3A) with the chooser bound in fe+0x94,
// the current row lit and framed by arrows (SBTH / LARW / RARW, else SBTN), and the track's blurb
// (kind 0x3B). Frame 120: the rectangle at entry-96, the sprite records at entry-72 and entry-88.
bool Arm28(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    const uint32_t rect = sp - 96u, spr0 = sp - 72u, spr1 = sp - 88u;
    const uint32_t r = w + 0x10u;
    g.W32(spr1 + 4u, 0x00808080u);
    g.W32(spr0 + 4u, 0x00808080u);
    g.W16(spr0 + 8u, 27);
    Unblend(g, kFontHdr);
    const uint32_t ot4 = Ot(g), ot12 = Ot(g, 12u), ot16 = Ot(g, 16u);
    Rect(g, rect, g.U16(r), g.U16(r + 2u), 40, g.U16(r + 6u));
    if (!Call(k, kTextId, {1, 0x616, rect, ot4, 0xB9A096u, 2})) return false;
    g.W16(rect + 4u, 160);
    g.W16(rect, static_cast<uint16_t>(g.U16(r) + 95u));
    if (!Call(k, kTextId, {1, 0x617, rect, ot4, 0xB9A096u, 2})) return false;
    g.W16(rect + 4u, 220);
    g.W16(rect, static_cast<uint16_t>(g.U16(r) + 255u));
    if (!Call(k, kTextId, {1, 0x618, rect, ot4, 0xB9A096u, 2})) return false;
    g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 20u));
    const uint32_t chPtr = kChoosers + 124u;
    const uint8_t track = g.U8(kFeTrack);
    const int8_t cur = g.S8(g.U32(chPtr) + 3u);
    for (int32_t i = 0; i < g.S8(kFe + 6u); ++i) {
        const int32_t v = ChooserPrevValid(g, g.U32(chPtr));
        g.W8(g.U32(chPtr) + 3u, static_cast<uint8_t>(v));
    }
    for (int row = 0; row < 5; ++row) {
        Unblend(g, kFontMini);
        g.W32(kFeChooser, g.U32(chPtr));
        const uint32_t ch = g.U32(chPtr);
        g.W8(kFeTrack, g.U8(g.U32(ch + 4u) + U(g.S8(ch + 3u) * 12) + 1u));
        g.W16(rect + 4u, 40);
        g.W16(rect, g.U16(w + 0x10u));
        uint32_t rec = TextSource(g, 39);
        auto lit = [&]() { return cur == g.S8(g.U32(chPtr) + 3u); };
        if (rec != 0) {
            g.W16(spr0 + 10u, static_cast<uint16_t>(g.U16(rect + 2u) - 3u));
            if (lit()) {
                if (!Call(k, kTextId, {g.U32(kFontMini), U(g.S16(rec)), rect, ot4, 0xA08C8Cu, 2})) return false;
                g.W32(spr0, 0x48544253u); // SBTH
                if (!Call(k, kSpriteEmit, {s, spr0, ot16})) return false;
                g.W32(spr1, 0x5752414Cu); // LARW
                g.W16(spr1 + 8u, 13);
                g.W16(spr1 + 10u, static_cast<uint16_t>(g.U16(rect + 2u) - 3u));
                if (!Call(k, kSpriteEmit, {s, spr1, ot12})) return false;
                g.W32(spr1, 0x57524152u); // RARW
                g.W16(spr1 + 8u, 68);
                g.W16(spr1 + 10u, static_cast<uint16_t>(g.U16(rect + 2u) - 3u));
                if (!Call(k, kSpriteEmit, {s, spr1, ot12})) return false;
            } else {
                g.W32(spr0, 0x4E544253u); // SBTN
                if (!Call(k, kSpriteEmit, {s, spr0, ot16})) return false;
                if (!Call(k, kTextId, {g.U32(kFontMini), U(g.S16(rec)), rect, ot4, 0x286EB4u, 2})) return false;
            }
        }
        g.W16(rect + 4u, 160);
        g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 95u));
        rec = TextSource(g, 57);
        if (rec != 0 && !Call(k, kTextId, {g.U32(kFontMini), U(g.S16(rec)), rect, ot4, lit() ? 0xA08C8Cu : 0x286EB4u, 2}))
            return false;
        g.W16(rect + 4u, 220);
        g.W16(rect, static_cast<uint16_t>(g.U16(w + 0x10u) + 255u));
        rec = TextSource(g, 58);
        if (rec != 0 && !Call(k, kTextId, {g.U32(kFontMini), U(g.S16(rec)), rect, ot4, lit() ? 0xA08C8Cu : 0x286EB4u, 2}))
            return false;
        const int32_t v = ChooserNextValid(g, g.U32(chPtr));
        g.W8(g.U32(chPtr) + 3u, static_cast<uint8_t>(v));
        g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 14u));
    }
    g.W8(g.U32(chPtr) + 3u, static_cast<uint8_t>(cur));
    g.W8(kFeTrack, track);
    const uint32_t rec = TextSource(g, 59);
    if (rec != 0) {
        Rect(g, rect, 45, 150, 428, 80);
        if (!Call(k, kTextId, {g.U32(kFontMini), U(g.S16(rec)), rect, ot4, 0xA08C8Cu, 3})) return false;
    }
    g.W32(kFeChooser, 0);
    return true;
}

// RASHCDF 0x80070DD4 (kind 0x2F): the credits - FESTRING 0x63C..0x798 in MINIFONT, one line every
// widget +0x14 pixels, scrolled up by +0x16 every +0x15 frames from 230 (0x8009CCE0, reset on the
// screen's first frame), clipped to the widget by a draw-area primitive (SLUS 0x8001C304) after the
// lines (the OT draws it first) and the full display area before them; fe+0x0A bit 3 when the last
// line has left the box. Frame 96: the rectangle at entry-64.
bool Arm2F(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t r, uint32_t sp) {
    constexpr uint32_t kScroll = 0x8009CCE0;
    const uint32_t rect = sp - 64u;
    const uint32_t env = g.U32(0x8005B470u);
    const uint32_t buf = env + 0x70u * g.U8(env + 6u);
    const uint16_t ox = g.U16(buf + 24u), oy = g.U16(buf + 26u);
    g.W16(sp - 56u, ox);
    g.W16(sp - 48u, oy);
    const uint32_t ot4 = Ot(g);
    if (!Call(k, 0x8001C304u, {U(static_cast<int16_t>(ox)), U(static_cast<int16_t>(oy)), 512, 240, 0, 0, ot4})) return false;
    if (g.S16(s + 2u) == 32767) {
        g.W16(kScroll, 230);
        g.W8(r + 23u, 0);
    } else if (g.S8(r + 23u) < g.S8(r + 21u)) {
        g.W8(r + 23u, static_cast<uint8_t>(g.U8(r + 23u) + 1u));
    } else {
        g.W16(kScroll, static_cast<uint16_t>(g.U16(kScroll) - static_cast<uint32_t>(g.S8(r + 22u))));
        g.W8(r + 23u, 0);
    }
    uint16_t y = g.U16(kScroll);
    g.W16(rect, g.U16(r));
    g.W16(rect + 4u, g.U16(r + 4u));
    g.W8(kFontSlots + 0x18u * g.U32(kFontMini) + 3u, 0);
    const uint16_t bottom = static_cast<uint16_t>(g.U16(r + 2u) + g.U16(r + 6u));
    int32_t drawn = 0;
    for (uint32_t id = 0x63C; id < 1945u; ++id) {
        if (static_cast<int16_t>(y) >= static_cast<int16_t>(bottom)) break;
        if (static_cast<int16_t>(y) + g.S8(r + 20u) >= g.S16(r + 2u)) {
            ++drawn;
            g.W16(rect + 6u, static_cast<uint16_t>(bottom - y));
            g.W16(rect + 2u, y);
            if (!Call(k, kTextId, {g.U32(kFontMini), id, rect, ot4, g.U32(r + 12u), g.U16(r + 10u)})) return false;
        }
        y = static_cast<uint16_t>(y + static_cast<uint32_t>(g.S8(r + 20u)));
    }
    if (!Call(k, 0x8001C304u,
              {U(g.S16(r) + static_cast<int16_t>(g.U16(sp - 56u))), U(g.S16(r + 2u) + static_cast<int16_t>(g.U16(sp - 48u))),
               U(g.S16(r + 4u)), U(g.S16(r + 6u)), 0, 0, ot4}))
        return false;
    if (g.S16(kScroll) < static_cast<int16_t>(bottom) && drawn == 0)
        g.W16(kFeMedia, static_cast<uint16_t>(g.U16(kFeMedia) | 8u));
    return true;
}

// RASHCDF 0x80074C9C (kind 0x36): the trophy room - the best-times table of the race fe+0x1F (the
// records table 0x80053A88, 176 bytes per race from 56): headings 0xE1..0xE5, then eight rows of place,
// name, bike, time (t/18000 : t/300 % 60 : t/3 % 100, or the 9999 "no time" string) and the three
// marks 0xE6..0xE8, all MINIFONT. Frame 240: the five column rectangles at entry-80..-48, the sprintf
// buffer at entry-208.
bool Arm36(GuestRam& g, ShellCallees& k, uint32_t sp) {
    const uint32_t buf = sp - 208u;
    const uint32_t ot4 = Ot(g);
    Unblend(g, kFontMini);
    static const uint16_t kX[5] = {31, 53, 189, 340, 419};
    for (uint32_t c = 0; c < 5u; ++c) Rect(g, sp - 80u + 8u * c, kX[c], 75, 120, 120);
    for (uint32_t c = 0; c < 5u; ++c)
        if (!Call(k, kTextId, {g.U32(kFontMini), 0xE1u + c, sp - 80u + 8u * c, ot4, 0xB9A096u, 0})) return false;
    const uint32_t table = kRecords + U((g.S8(kFe + 0x1Fu) - 56) * 176);
    for (uint32_t i = 0; i < 8u; ++i) {
        const uint32_t e = table + 20u * i;
        const uint16_t y = static_cast<uint16_t>(g.U16(sp - 80u + 2u) + 14u);
        for (uint32_t c = 0; c < 5u; ++c) g.W16(sp - 80u + 8u * c + 2u, y);
        if (!Call(k, kSprintf, {buf, 0x8005C584u, i + 1u})) return false;
        if (!Call(k, kTextStr, {g.U32(kFontMini), buf, sp - 80u, ot4, 0xB9A096u, 0})) return false;
        if (!Call(k, kTextStr, {g.U32(kFontMini), table + 16u + 20u * i, sp - 72u, ot4, 0x286EB4u, 0})) return false;
        if (!Call(k, kTextId, {g.U32(kFontMini), U(BikeNameId(g.S8(e + 32u))), sp - 64u, ot4, 0x286EB4u, 0})) return false;
        const int32_t t = g.S32(e + 28u);
        if (t == 9999) {
            if (!Call(k, kSprintf, {buf, 0x8005C5B8u})) return false;
        } else {
            const int32_t m = t / 18000, sec = t / 300 - m * 60, cs = t / 3 - (t / 300) * 100;
            if (!Call(k, kSprintf, {buf, 0x8005C5C4u, U(m), U(sec), U(cs)})) return false;
        }
        if (!Call(k, kTextStr, {g.U32(kFontMini), buf, sp - 56u, ot4, 0x286EB4u, 0})) return false;
        static const uint32_t kMark[3][2] = {{35, 0xE6}, {33, 0xE7}, {34, 0xE8}};
        for (const auto& mk : kMark)
            if (g.S8(e + mk[0]) != 0 && !Call(k, kTextId, {g.U32(kFontMini), mk[1], sp - 48u, ot4, 0x286EB4u, 0}))
                return false;
    }
    return true;
}

// RASHCDF 0x80075098 (kind 0x37): the name / code keyboard of screens 52 and 56 - two translucent
// POLY_F4 highlights built in the display packet buffer (the key under the cursor, and the edited
// character's cell; SLUS SetSemiTrans 0x8004CDE4 / AddPrim 0x8004CDA4, the buffer's wrap 0x80021C98),
// the eight characters of 0x80088C84, the 4 x 9 key caps of 0x80088C9C and the edit row (two caps,
// 0xE9..0xEB). Frame 120: the one-character string at entry-96, the key cell at entry-88, the edit
// strip at entry-80 / entry-72, the edit-row cell at entry-64 and at entry-56.
bool Arm37(GuestRam& g, ShellCallees& k, uint32_t w, uint32_t sp) {
    const uint32_t chr = sp - 96u, key = sp - 88u, strip = sp - 72u, edit = sp - 64u, wide = sp - 56u;
    for (uint32_t i = 0; i < 3u; ++i) g.W8(chr + i, g.U8(0x8005C5D4u + i));
    Unblend(g, kFontMain);
    Rect(g, sp - 48u, g.U16(w + 0x10u) + 95u, g.U16(w + 0x12u), g.U16(w + 0x14u), g.U16(w + 0x16u));
    const int32_t col = g.S32(kKeyCol), row = g.S32(kKeyRow);
    Rect(g, key, U(col * 24 + 148), U(row * 14 + 85), 24, 14);
    Rect(g, sp - 80u, 150, 57, 212, 14);
    Rect(g, strip, 160, 57, 24, 14);
    const uint32_t ot4 = Ot(g);
    const uint32_t env = g.U32(0x8005B470u);
    if (g.U32(env + 268u) + 48u >= g.U32(0x8005B4D0u)) {
        uint32_t p = 0;
        const uint32_t a[2] = {g.U32(env + 268u), 48};
        if (!k.Call(0x80021C98u, a, 2, &p)) return false;
        g.W32(g.U32(0x8005B470u) + 268u, p);
    }
    const uint32_t prim = g.U32(g.U32(0x8005B470u) + 268u);
    g.W32(g.U32(0x8005B470u) + 268u, prim + 48u);
    g.W8(prim + 3u, 5);
    g.W8(prim + 7u, 40);
    const uint32_t ot20 = Ot(g, 20u);
    auto quad = [&](uint32_t p, int32_t x, int32_t y, int32_t x1, int32_t y1, int32_t x3) {
        g.W16(p + 8u, static_cast<uint16_t>(x));
        g.W16(p + 10u, static_cast<uint16_t>(y));
        g.W16(p + 12u, static_cast<uint16_t>(x1));
        g.W16(p + 14u, static_cast<uint16_t>(y));
        g.W16(p + 16u, static_cast<uint16_t>(x));
        g.W16(p + 18u, static_cast<uint16_t>(y1));
        g.W16(p + 20u, static_cast<uint16_t>(x3));
        g.W16(p + 22u, static_cast<uint16_t>(y1));
        g.W8(p + 4u, 128);
        g.W8(p + 5u, 128);
        g.W8(p + 6u, 255);
        const uint32_t a[2] = {p, 1};
        if (!k.Call(0x8004CDE4u, a, 2, nullptr)) return false;
        const uint32_t b[2] = {ot20, p};
        return k.Call(0x8004CDA4u, b, 2, nullptr);
    };
    if (row == 4) {
        int32_t x = 0, ww = 0;
        switch (col) { // jump table 0x8005C5DC
        case 0: case 1: x = 148; ww = 36; break;
        case 2: x = 184; ww = 36; break;
        case 3: case 4: x = 220; ww = 48; break;
        case 5: case 6: x = 268; ww = 48; break;
        case 7: case 8: x = 316; ww = 48; break;
        default: break; // the bound: the column is 0..8 (every arm of the keyboard keeps it there)
        }
        const int32_t y = static_cast<int16_t>(g.U16(key + 2u) - 2u);
        x = static_cast<int16_t>(x + 1);
        Rect(g, edit, U(x), U(y), U(ww), 14);
        if (!quad(prim, x, y, x + ww, y + 14, x + ww)) return false;
    } else {
        const int32_t x = static_cast<int16_t>(g.U16(key) + 1u), y = static_cast<int16_t>(g.U16(key + 2u) - 2u);
        g.W16(key, static_cast<uint16_t>(x));
        g.W16(key + 2u, static_cast<uint16_t>(y));
        // a POLY_F4 whose upper edge is 25 wide and lower edge 24 (0x80075398 / 0x800753D0)
        if (!quad(prim, x, y, x + 25, y + 14, x + 24)) return false;
    }
    const uint32_t p2 = prim + 24u;
    g.W8(p2 + 3u, 5);
    g.W8(p2 + 7u, 40);
    const int32_t pos = g.S32(kKeyPos);
    const int32_t sx = static_cast<int16_t>(161 + pos * 24);
    if (!quad(p2, sx, 54, sx + 24, 54 + 14, sx + 24)) return false;
    for (uint32_t i = 0; i < 8u; ++i) {
        g.W8(chr, g.U8(kKeyBuf + i));
        g.W16(strip, static_cast<uint16_t>(160u + 24u * i));
        if (!Call(k, kTextStr, {g.U32(kFontMain), chr, strip, ot4, 0xB9A096u, 2})) return false;
    }
    g.W16(strip, static_cast<uint16_t>(160u + 24u * 8u));
    for (int32_t r = 0; r < 4; ++r) {
        g.W16(key + 2u, static_cast<uint16_t>(85 + 14 * r));
        for (int32_t c = 0; c < 9; ++c) {
            g.W8(chr, g.U8(kKeyGrid + U(r * 9 + c)));
            g.W16(key, static_cast<uint16_t>(148 + 24 * c));
            const uint32_t rgb = (c == col && r == row) ? 0x40ACFFu : 0x606060u;
            if (!Call(k, kTextStr, {g.U32(kFontMain), chr, key, ot4, rgb, 2})) return false;
        }
    }
    g.W16(key, static_cast<uint16_t>(148 + 24 * 9));
    Rect(g, key, 148, 141, 24, 14);
    Rect(g, wide, 148, 141, 36, 14);
    auto lit = [&](bool on, uint32_t color) { return on && row == 4 ? color : 0x606060u; };
    g.W8(chr, g.U8(kKeyGrid + 36u));
    if (!Call(k, kTextStr, {g.U32(kFontMain), chr, wide, ot4, lit(static_cast<uint32_t>(col) < 2u, 0x40ACFFu), 2})) return false;
    g.W16(wide, static_cast<uint16_t>(g.U16(wide) + 36u));
    g.W8(chr, g.U8(kKeyGrid + 37u));
    if (!Call(k, kTextStr, {g.U32(kFontMain), chr, wide, ot4, lit(col == 2, 0x40ACFFu), 2})) return false;
    g.W16(wide, static_cast<uint16_t>(g.U16(wide) + 36u));
    g.W16(wide + 4u, 48);
    if (!Call(k, kTextId, {g.U32(kFontMain), 0xE9, wide, ot4, lit(static_cast<uint32_t>(col - 3) < 2u, 0x40ACFFu), 2})) return false;
    g.W16(wide, static_cast<uint16_t>(g.U16(wide) + 48u));
    if (!Call(k, kTextId, {g.U32(kFontMain), 0xEA, wide, ot4, lit(static_cast<uint32_t>(col - 5) < 2u, 0x40ACFFu), 2})) return false;
    g.W16(wide, static_cast<uint16_t>(g.U16(wide) + 48u));
    return Call(k, kTextId, {g.U32(kFontMain), 0xEB, wide, ot4, lit(static_cast<uint32_t>(col - 7) < 2u, 0xC4C4C4u), 2});
}

// RASHCDF 0x80075AFC (kind 0x38): the abort modal's Yes / No, through the text source.
bool Arm38(GuestRam& g, ShellCallees& k, uint32_t w) {
    Unblend(g, kFontMini);
    const uint32_t rec = TextSource(g, g.U16(w + 0x18u));
    return Id(g, k, U(g.S16(rec)), w + 0x10u, Rgb(g, w), Just(g, w));
}

} // namespace

// ---------------------------------------------------------------------------- name ids
// RASHCDF 0x80072100: the group of a button's action code (the highlighted item's help selector).
int32_t ActionGroup(GuestRam& g, uint32_t w) {
    if (static_cast<uint32_t>(g.U16(w + 8u)) - 12u >= 2u) return -1;
    switch (g.U16(w + 0x12u)) {
    case 0: case 8: case 0x14: case 0x20: case 0x26: case 0x2F: case 0x37: case 0x4C: return 0;
    case 1: case 0xB: case 0xF: case 0x19: case 0x24: case 0x2A: case 0x2C: case 0x34: case 0x3D: return 1;
    case 4: return 3;
    case 9: case 10: case 0x1A: return 7;
    case 0xC: case 0x12: case 0x18: case 0x22: case 0x28: case 0x2D: case 0x31: case 0x39: return 6;
    case 0xD: case 0x11: case 0x23: case 0x29: case 0x2E: case 0x32: case 0x3E: return 4;
    case 0x13: case 0x3B: return 2;
    case 0x25: case 0x2B: case 0x35: return 5;
    case 0x4E: case 0x4F: return 8;
    default: return 9;
    }
}

// RASHCDF 0x80071F0C: the rank name of a venue in Jail Break (0x5FF..) or Five-O (mode 1, 0x602..).
int32_t ModeNameId(int32_t venue, uint32_t mode) {
    const bool fiveO = mode == 1u;
    switch (venue) {
    case 2: case 3: return fiveO ? 0x603 : 0x600;
    case 4: case 5: return fiveO ? 0x604 : 0x601;
    default: return fiveO ? 0x602 : 0x5FF;
    }
}

// RASHCDF 0x80071FDC: a bike's name, 0x3ED + 3 * index for the 21 bikes, else -1.
int32_t BikeNameId(int32_t index) { return (index >= 0 && index <= 20) ? 0x3ED + 3 * index : -1; }

// RASHCDF 0x800720B4: a race's course name (six strings per course; the second road set from 0x315).
int32_t CourseNameId(GuestRam& g, int32_t race) {
    int32_t i = race - 1;
    if (g.S8(kSession + 6u) == 2) {
        if (i > 0x12) i = race - 0x14;
        return i * 6 + 0x315;
    }
    return i * 6 + 0x195;
}

// RASHCDF 0x80072A60 / 0x80072A80: the gang's name and the rank-in-gang.
int32_t GangNameId(int32_t gang) { return gang == 1 ? 0xFC : 0xF6; }
int32_t RankNameId(int32_t gang, int32_t level) {
    if (gang == 1) return level == 1 ? 0x11A : (level == 2 ? 0x120 : 0x114);
    return level == 1 ? 0x108 : (level == 2 ? 0x10E : 0x102);
}

// RASHCDF 0x80063C7C.
uint32_t TextSource(GuestRam& g, uint32_t kind) {
    auto flag = [&](uint32_t out, uint32_t at, uint16_t whenZero, uint16_t otherwise) {
        g.W16(out, g.S8(at) == 0 ? whenZero : otherwise);
        return out;
    };
    auto who = [&](uint32_t at) {
        const int32_t c = g.S8(at);
        g.W16(0x80081120u, c == 1 ? 0xC4u : (c == 2 ? 0xC5u : 0xC6u));
        return 0x80081120u;
    };
    switch (kind & 0xFFFFu) {
    case 0x0E: {
        if (g.S16(kFeTicked) != g.S16(kFeCur)) return 0;
        const uint32_t item = g.U32(kFeItem);
        if (item == 0 || g.S16(item + 8u) != 12) return 0;
        uint16_t id = static_cast<uint16_t>(g.U16(item + 0x14u) + 1u);
        if (ActionGroup(g, item) == 1) {
            switch (g.S8(kSession + 4u)) {
            case 1: id = 0x609; break;
            case 3: id = 0x60A; break;
            case 5: id = 0x60B; break;
            default: break;
            }
        }
        g.W16(0x8009C4A8u, id);
        g.W8(0x8009C4AAu, 3);
        g.W32(0x8009C4ACu, 0x00A08C8Cu);
        return 0x8009C4A8u;
    }
    case 0x20: {
        const int32_t v = g.S8(kSession + 0x17u);
        if (v == 0) g.W16(0x80081120u, 200);
        else if (v == 2) g.W16(0x80081120u, 0xC9);
        else return 0;
        return 0x80081120u;
    }
    case 0x21: return flag(0x80081120u, kSession + 0x19u, 7, 6);
    case 0x22:
        g.W16(0x800810F8u, static_cast<uint16_t>(ModeNameId(g.S8(kSession + 4u), g.U32(kSession))));
        return 0x800810F8u;
    case 0x23: return flag(0x80081100u, kSession + 0x09u, 7, 6);
    case 0x24: return flag(0x80081108u, kSession + 0x0Au, 7, 6);
    case 0x25: return flag(0x80081110u, kSession + 0x0Bu, 7, 6);
    case 0x26: {
        uint16_t v = 8;
        switch (g.U8(kSession + 0x12u)) {
        case 2: v = 9; break;
        case 3: v = 10; break;
        case 4: v = 11; break;
        case 5: v = 12; break;
        case 6: v = 13; break;
        default: break;
        }
        g.W16(0x80081118u, v);
        return 0x80081118u;
    }
    case 0x27: return flag(0x80081120u, kSession + 0xC0u + g.U8(kFeTrack), 7, 6);
    case 0x29: return who(kPlayers + 0x15u);
    case 0x2A: return who(kPlayers + 0x24u + 0x15u);
    case 0x2B: return flag(0x80081108u, kSession + 0x14u, 0xB7, 0xB8);
    case 0x2C: return flag(0x80081120u, kPlayers + 0x14u + U(g.S8(kFePort)) * 0x24u + 3u, 7, 6);
    case 0x38: return flag(0x80081108u, kFe + 0x19u, 5, 4);
    default: {
        const uint32_t c = g.U32(kFeChooser);
        if (c == 0 || g.U32(c + 4u) == 0) return 0;
        const uint32_t opt = g.U32(c + 4u) + U(g.S8(c + 3u) * 12);
        const int32_t n = g.S16(opt + 6u);
        uint32_t e = g.U32(opt + 8u);
        for (int32_t i = 0; i < n; ++i, e += 0x1Cu)
            if (g.U16(e + 2u) == (kind & 0xFFFFu)) return e + 4u;
        return 0;
    }
    }
}

// ---------------------------------------------------------------------------- the passes
// RASHCDF 0x8006D3E0.
bool WidgetPass(GuestRam& g, ShellCallees& k, uint32_t s) {
    const uint32_t table = g.S16(s + 2u) == 0 ? 0x8009CF28u : 0x8009CF78u;
    if (g.U32(s + 0x10u) == 0) return true;
    for (int32_t i = 0; i < g.S16(s + 8u); ++i) {
        const uint32_t w = g.U32(s + 0x10u) + 120u * static_cast<uint32_t>(i);
        const uint32_t h = g.U32(table + 4u * U(g.S16(w + 8u)));
        if (h == 0) continue;
        const uint32_t c = g.U32(w + 4u);
        if ((((c & 0x3F00u) >> 8) & (1u << (U(g.S8(kSession + 4u)) & 31u))) == 0) continue;
        if ((c & 0x02000000u) && g.U32(kPorts + 24u * U(g.S8(kFePort)) + 4u) != 1u) continue;
        if ((c & 0x08000000u) && g.U32(kSession) != 4u) continue;
        if ((c & 0x04000000u) && g.U32(kSession) == 4u) continue;
        if ((c & 0x10000000u) && g.S8(kSession + 0x1Bu) == 0) continue;
        if ((c & 0x80000000u) && g.S16(kFeCur) != g.S16(kFeTicked)) continue;
        const uint32_t a[2] = {s, w};
        if (!k.Call(h, a, 2, nullptr)) return false;
    }
    return true;
}

// RASHCDF 0x8006FAC8 (frame 40 bytes: its arms run at sp - 40).
bool TextBlock(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp) {
    if ((g.U16(w + 10u) & 0x20u) && !(g.U16(s) & 2u)) return true;
    if (g.S8(kFontSlots + 0x18u * g.U32(kFontMini)) == 0) return true;
    if (g.S8(kFontSlots + 0x18u * g.U32(kFontHdr)) == 0) return true;
    const uint32_t r = w + 0x10u;
    if (!(g.U16(w + 10u) & 0x2000u)) {
        const uint32_t rec = TextSource(g, g.U16(w + 0x18u));
        if (rec == 0) return true;
        Unblend(g, kFontMain);
        return Call(k, kTextId, {g.U32(kFontMain), U(g.S16(rec)), r, Ot(g), g.U32(rec + 4u), U(g.S8(rec + 2u))});
    }
    const uint32_t kind = g.U16(w + 0x18u);
    const uint32_t asp = sp - 40u;
    switch (kind) {
    case 1: return Arm01(g, k, s, w, asp);
    case 2: return Arm02(g, k, s, w, asp);
    case 3: return Arm03(g, k, s, w, asp);
    case 4: return Arm04(g, k, s, w, asp);
    case 5: case 6: case 7: return Arm05(g, k, s, w, asp, kind);
    case 8: case 9: return Arm08(g, k, s, w, asp, kind);
    case 0x0A: return Arm0A(g, k, s, w, asp);
    case 0x0F: return Arm0F(g, k, s, w);
    case 0x10: return Arm10(g, k, s, w, asp);
    case 0x11: return Arm11(g, k, s, w, asp);
    case 0x12: return Arm12(g, k, s, w, asp);
    case 0x13: return Arm13(g, k, s, w, asp);
    case 0x14: return Arm14(g, k, s, w, asp);
    case 0x15: return Arm15(g, k, s, w, asp);
    case 0x1A: // inline: the career / Five-O / Side Car notification fe+0x1C in the widget's box
        if (g.S16(kFeNotify) == 0) return true;
        Unblend(g, kFontMain);
        return Call(k, kTextId, {g.U32(kFontMain), U(g.S16(kFeNotify)), r, Ot(g), g.U32(r + 0x0Cu), g.U16(r + 0x0Au)});
    case 0x1B: return Arm1B(g, k, w, asp);
    case 0x1C: return Arm1C(g, k, w, asp);
    case 0x1D: return Arm1D(g, k, w, asp);
    case 0x1E: return Arm1E(g, k, s, w, asp);
    case 0x1F: Unblend(g, kFontMain); return true; // 0x800758D4
    case 0x28: return Arm28(g, k, s, w, asp);
    case 0x2D: return CardStatusText(g, k, s, r, g.S16(g.U32(kCardScreenRec) + 8u), asp); // shell_memcard.h
    case 0x2E: return CardSlotsText(g, k, r, g.S16(g.U32(kCardScreenRec) + 8u), asp);
    case 0x2F: return Arm2F(g, k, s, r, asp);
    case 0x36: return Arm36(g, k, asp);
    case 0x37: return Arm37(g, k, w, asp);
    case 0x38: return Arm38(g, k, w);
    default: return true;
    }
}

} // namespace rr::shell
