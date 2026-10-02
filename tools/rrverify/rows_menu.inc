// Bench rows for the front end's widget DRAW handlers, ported in src\game\shell\shell_widgets.{h,cpp}.
// Included last by verify_physics.cpp; the machine, the planted shell capture
// and the helpers are shellrows:: (rows_shell.inc's header says what is planted and compared).
//
// Every row calls the handler as the widget pass 0x8006D3E0 does, h(screen, widget), on a widget of its
// type from the capture's screens, and compares v0 and the whole machine - the packets the handler writes
// into the display packet buffer, the ordering-table links, the animation state it advances.
//
// INPUT FAMILIES. dump-derived: the capture as it is (the main menu's machine), one widget of the row's
// type with the screen made live; randomised, inside these named bounds:
//   * the ordering-table layer fe+0x11 is a screen's own +0x0C or +0x0D (every table entry the handlers
//     link into then stays inside the 72-entry table 0x80080400 clears);
//   * the packet cursor is the capture's, or within 0x200 bytes of the buffer's end 0x8005B4D0 (the wrap
//     SLUS 0x80021C98 is then reached, and run by the oracle);
//   * a screen's selection is in [0, item count) or 32767 (rows_shell.inc);
//   * the arrows' animation records keep a frame < their count, a u / v offset on the sheet's frame grid,
//     and flags from the values the handler itself writes (0x00, 0x20, 0x40, 0x60, 0x10, 0x50) - a delay
//     0..2 and a tick 0..3;
//   * the arrow sheets' sprite records: as captured, not loaded, flagged for a shared slot (0x100000) they
//     occupy (0x800A0810), or queued (0x40000000, bit 31 clear / set) with the two queues' counts 0..7.
//     NOT benched: a record that needs its upload (0x20000000 clear, or another occupant of its shared slot)
//     - the upload 0x80065768 is LoadImage, which waits on the GPU's status register the bench machine
//     does not have (the guest traps at 0x80047744 reading 0x1F801814); the port's side of that branch is
//     the seam call and the flag it sets;
//   * the bike page: the bike bytes player[0/1]+0x07 in 0..17 (the bike choosers' option range), the
//     stat bars fe+0x38.. within 8 of the stat table's [min, max];
//   * the sliders' descriptor value 0x8009C548+16k +0x0C within the descriptor's [min, max].
//
// THE SEAMS (per row): the SLUS text calls 0x8002CD78 / 0x8002CB08, the shared sprite emitter 0x800700F0,
// the packet buffer's wrap 0x80021C98 and the sprite upload 0x80065768.

} // namespace
#include "game/shell/shell_widgets.h"
namespace {

namespace menurows {

using namespace shellrows;

// Every (screen, widget) of the capture whose widget has one of `types`.
std::vector<std::pair<uint32_t, uint32_t>> WidgetsOf(const Memory& m, std::initializer_list<int> types) {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (uint32_t id = 0; id < 59u; ++id) {
        const uint32_t s = Screen(m, id);
        if (s == 0 || m.PeekWord(s + 16u) == 0) continue;
        const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
        for (int32_t i = 0; i < n && i < 64; ++i) {
            const uint32_t w = m.PeekWord(s + 16u) + 120u * static_cast<uint32_t>(i);
            const int t = static_cast<int16_t>(Get16(m, w + 8u));
            for (int want : types)
                if (t == want) v.push_back({s, w});
        }
    }
    return v;
}

std::vector<OracleCallee> DrawSeams() {
    return {OracleCallee{sh::kTextAt, 4, 2, {}, 0}, OracleCallee{sh::kTextId, 4, 2, {}, 0},
            OracleCallee{sh::kSpriteEmit, 3, 0, {}, 0}, OracleCallee{sh::kPacketWrap, 2, 0, {}, 0},
            OracleCallee{sh::kSpriteUpload, 1, 0, {}, 0}};
}

// The drawing context: the screen live or not, its layer, the packet cursor, the fonts.
void DrawState(CaseContext& c, uint32_t s) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Put16(m, s, Get16(m, s) | 2u);
    Put8(m, sh::kFeLayer, m.PeekByte(s + 0x0Cu));
    if (c.sample) return;
    if (Chance(r, 5)) Put16(m, s, Get16(m, s) & ~2u);
    if (Chance(r, 2)) Put8(m, sh::kFeLayer, m.PeekByte(s + 0x0Du));
    Select(c, s);
    const uint32_t env = m.PeekWord(0x8005B470u);
    if (Chance(r, 4)) m.PokeWord(env + 0x10Cu, m.PeekWord(sh::kPacketLimit) - 4u * (1u + Pick(r, 0x80)));
    if (Chance(r, 10)) Put8(m, sh::kFontSlots + 0x18u * m.PeekWord(sh::kFontMain), 0);
    if (Chance(r, 10)) Put8(m, sh::kFontSlots + 0x18u * m.PeekWord(sh::kFontMini), 0);
}

// The button / chooser / slider colour gates and the button shape word.
void ButtonState(CaseContext& c, uint32_t w) {
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    ShuffleGates(c);
    static const uint32_t kBits[] = {0x40000000u, 0x02000000u, 0x10000000u, 0x08000000u, 0x04000000u};
    uint32_t cw = m.PeekWord(w) & 0x00FFFFFFu;
    for (uint32_t b : kBits)
        if (Chance(r, 3)) cw |= b;
    m.PokeWord(w, cw);
    if (Chance(r, 5)) m.PokeWord(w + 0x28u, 0);
    if (Chance(r, 6)) Put16(m, w + 8u, Chance(r, 2) ? 17u : 12u);
}

// One animation record (the arrows' +0x34 / +0x4C) inside its named bounds.
void AnimState(CaseContext& c, uint32_t a) {
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t count = m.PeekByte(a + 0x0Cu), fw = m.PeekByte(a + 0x0Du), fh = m.PeekByte(a + 0x0Eu);
    const uint32_t frame = count ? Pick(r, count) : 0u;
    const uint32_t perRow = fw ? 54u / fw : 1u;
    Put8(m, a + 0x0Fu, Pick(r, 3));
    Put8(m, a + 0x10u, frame);
    Put8(m, a + 0x11u, Pick(r, 4));
    Put8(m, a + 0x12u, perRow ? (frame % perRow) * fw : 0u);
    Put8(m, a + 0x13u, perRow ? (frame / perRow) * fh : 0u);
    static const uint8_t kFlags[] = {0x00, 0x20, 0x40, 0x60, 0x10, 0x50, 0x60, 0x40};
    Put8(m, a + 0x14u, kFlags[Pick(r, 8)]);
}

// The sprite record of an animation record's FourCC, in one of the states the handler branches on.
// Returns whether it made the record a shared-slot one (the slot then has one occupant: a second record
// flagged for it would need its upload, which the bench does not reach - the header says why).
bool SpriteRecordState(CaseContext& c, uint32_t a, bool allowShared = true) {
    if (c.sample) return false;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t id = m.PeekWord(a);
    for (uint32_t i = 0; i < 300u; ++i) {
        if (m.PeekWord(sh::kSpriteIds + 4u * i) != id) continue;
        const uint32_t rec = sh::kSpriteRecs + 0x24u * i;
        uint32_t f = m.PeekWord(rec);
        const uint32_t pick = Pick(r, 5);
        switch (pick == 0 && !allowShared ? 4u : pick) {
        case 0: f |= 0x100000u; m.PokeWord(0x800A0810u, id); break;          // a shared slot it occupies
        case 1: f = (f & ~0xA0000000u) | 0x40000000u; break;                // queued, second list
        case 2: f = (f & ~0x20000000u) | 0xC0000000u; break;                // queued, first list, page computed
        case 3: f &= ~0x10000000u; break;                                  // not loaded: nothing drawn
        default: break;
        }
        m.PokeWord(rec, f);
        m.PokeWord(0x8009C2F0u, Pick(r, 8));
        m.PokeWord(0x8009C2F4u, Pick(r, 8));
        return pick == 0 && allowShared;
    }
    return false;
}

// The chooser's arrow context: the valid-option count, the frame's edges, the last frame's item.
void ChooserState(CaseContext& c, uint32_t s, uint32_t w) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (c.sample) {
        const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
        const uint32_t items = m.PeekWord(s + 16u);
        for (int32_t i = 0; i < n; ++i)
            if (items + 120u * static_cast<uint32_t>(i) == w) Put16(m, s + 4u, static_cast<uint32_t>(i));
        Put8(m, sh::kFeValid, 2);
        m.PokeWord(sh::kFeLastItem, w);
        return;
    }
    if (Chance(r, 2)) { // make it the highlighted one (the arrows' branch)
        const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
        const uint32_t items = m.PeekWord(s + 16u);
        for (int32_t i = 0; i < n; ++i)
            if (items + 120u * static_cast<uint32_t>(i) == w) Put16(m, s + 4u, static_cast<uint32_t>(i));
    }
    Put8(m, sh::kFeValid, Pick(r, 4));
    static const uint8_t kEdges[] = {0x00, 0x40, 0x80, 0x00, 0x40, 0x80, 0x01, 0xC0};
    Put8(m, sh::kFeEdges, kEdges[Pick(r, 8)]);
    m.PokeWord(sh::kFeLastItem, Chance(r, 3) ? m.PeekWord(s + 16u) : w);
    AnimState(c, w + 0x34u);
    AnimState(c, w + 0x4Cu);
    const bool shared = SpriteRecordState(c, w + 0x34u);
    SpriteRecordState(c, w + 0x4Cu, !shared);
}

using Draw = bool (*)(sh::GuestRam&, sh::ShellCallees&, uint32_t, uint32_t, uint32_t*);

Row DrawRow(const char* name, const char* sig, uint32_t entry, std::initializer_list<int> types,
            void (*extra)(CaseContext&, uint32_t, uint32_t), Draw h) {
    std::vector<int> ty(types);
    return ShellRow(
        name, sig, entry,
        [ty, extra](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            std::vector<std::pair<uint32_t, uint32_t>> v;
            for (int t : ty) {
                const auto more = WidgetsOf(m, {t});
                v.insert(v.end(), more.begin(), more.end());
            }
            const auto sw = v.empty() ? std::pair<uint32_t, uint32_t>{0u, 0u} : v[Pick(*c.rng, static_cast<uint32_t>(v.size()))];
            DrawState(c, sw.first);
            if (extra != nullptr) extra(c, sw.first, sw.second);
            a.a[0] = sw.first;
            a.a[1] = sw.second;
        },
        [h](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                uint32_t r = 0;
                if (!h(g, k, a.a[0], a.a[1], &r) && env.failure.empty()) env.failure = "refused";
                return r;
            });
        },
        true, DrawSeams());
}

void ButtonExtra(CaseContext& c, uint32_t, uint32_t w) { ButtonState(c, w); }
void ChooserExtra(CaseContext& c, uint32_t s, uint32_t w) {
    ButtonState(c, w);
    Put16(*c.mem, w + 8u, 13u);
    ChooserState(c, s, w);
}
void SliderExtra(CaseContext& c, uint32_t s, uint32_t w) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    ButtonState(c, w);
    Put16(m, w + 8u, 17u);
    ChooserState(c, s, w);
    if (c.sample) return;
    if (Chance(r, 3)) Put8(m, w + 0x67u, m.PeekByte(w + 0x67u) ^ 1u);
    if (Chance(r, 6)) Put16(m, w + 0x64u, 6u + Pick(r, 4)); // no descriptor
    const uint32_t d = 0x8009C548u + 16u * m.PeekByte(w + 0x66u);
    const int32_t lo = static_cast<int32_t>(m.PeekWord(d)), hi = static_cast<int32_t>(m.PeekWord(d + 4u));
    if (hi > lo) Put16(m, d + 12u, static_cast<uint32_t>(lo + static_cast<int32_t>(Pick(r, static_cast<uint32_t>(hi - lo + 1)))));
}
void StatsExtra(CaseContext& c, uint32_t s, uint32_t) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    // The bike choosers' items (action groups 4 / 5) on the screen, one of them highlighted half the time.
    const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
    const uint32_t items = m.PeekWord(s + 16u);
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t w = items + 120u * static_cast<uint32_t>(i);
        const uint32_t code = Get16(m, w + 0x12u);
        if (Get16(m, w + 8u) == 13u && (code == 13u || code == 43u) && (c.sample || Chance(r, 2)))
            Put16(m, s + 4u, static_cast<uint32_t>(i));
    }
    if (c.sample) return;
    Put8(m, sh::kPlayers + 7u, Pick(r, 18));
    Put8(m, sh::kPlayers + 0x24u + 7u, Pick(r, 18));
    for (uint32_t k = 0; k < 3u; ++k) {
        const int32_t lo = static_cast<int32_t>(m.PeekWord(sh::kStatTable + 12u * k));
        const int32_t hi = static_cast<int32_t>(m.PeekWord(sh::kStatTable + 12u * k + 4u));
        if (hi >= lo) m.PokeWord(sh::kFeStatShown + 4u * k, static_cast<uint32_t>(lo - 8 + static_cast<int32_t>(Pick(r, static_cast<uint32_t>(hi - lo + 17)))));
    }
}
void PanelExtra(CaseContext& c, uint32_t, uint32_t w) {
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 2)) Put16(m, w + 0x12u, Get16(m, w + 0x12u) ^ 1u);
    if (Chance(r, 2)) Put16(m, w + 0x30u, Get16(m, w + 0x30u) ^ 1u);
    if (Chance(r, 2)) Put16(m, w + 0x22u, Get16(m, w + 0x22u) ^ 1u);
}

} // namespace menurows

void AddMenuRows(std::vector<Row>& rows) {
    using namespace shellrows;
    using namespace menurows;
    rows.push_back(DrawRow("shell_widget_sprite", "s32 SpriteWidget(Screen*, Widget*)  [RASHCDF 0x8006E894, type 7]",
                           0x8006E894u, {7}, nullptr, sh::SpriteWidget));
    rows.push_back(DrawRow("shell_widget_padhint", "s32 PadHint(Screen*, Widget*)  [RASHCDF 0x8006E7A4, type 9]",
                           0x8006E7A4u, {9}, nullptr, sh::PadHint));
    rows.back().allowBiosCalls = true; // the text calls reach strlen through the A0 vector
    rows.push_back(DrawRow("shell_widget_button", "s32 ButtonDraw(Screen*, Widget*)  [RASHCDF 0x8006F764, type 12]",
                           0x8006F764u, {12, 13, 17}, ButtonExtra, sh::ButtonDraw));
    rows.back().allowBiosCalls = true;
    rows.push_back(DrawRow("shell_widget_chooser", "s32 ChooserDraw(Screen*, Widget*)  [RASHCDF 0x8006EF30 + 0x800705DC, type 13]",
                           0x8006EF30u, {13}, ChooserExtra, sh::ChooserDraw));
    rows.back().allowBiosCalls = true;
    rows.push_back(DrawRow("shell_widget_slider", "s32 SliderDraw(Screen*, Widget*)  [RASHCDF 0x8006F4A0, type 17]",
                           0x8006F4A0u, {17}, SliderExtra, sh::SliderDraw));
    rows.back().allowBiosCalls = true;
    rows.push_back(DrawRow("shell_widget_bikestats", "s32 BikeStatsDraw(Screen*, Widget*)  [RASHCDF 0x8006F0B8, type 18]",
                           0x8006F0B8u, {18}, StatsExtra, sh::BikeStatsDraw));
    rows.back().allowBiosCalls = true;
    rows.push_back(DrawRow("shell_widget_panel", "s32 PanelWidget(Screen*, Widget*)  [RASHCDF 0x8006ECA0 + 0x8006ECDC + 0x8006EE20, type 11]",
                           0x8006ECA0u, {11}, PanelExtra, sh::PanelWidget));
    rows.push_back(DrawRow("shell_widget_header", "s32 HeaderText(Screen*, Widget*)  [RASHCDF 0x8006F9A4, type 14]",
                           0x8006F9A4u, {14}, nullptr, sh::HeaderText));
    rows.back().allowBiosCalls = true;
    rows.push_back(DrawRow("shell_widget_textline", "s32 TextLine(Screen*, Widget*)  [RASHCDF 0x8006FA38, type 15]",
                           0x8006FA38u, {15}, nullptr, sh::TextLine));
    rows.back().allowBiosCalls = true;

    // ------------------------------------------------------------------ RASHCDF 0x800705DC
    rows.push_back(ShellRow(
        "shell_anim_sprite", "s32 AnimSprite(AnimRec*, u32* ot, s32 mode)  [RASHCDF 0x800705DC]", 0x800705DCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const auto v = WidgetsOf(m, {13, 17});
            const auto sw = v.empty() ? std::pair<uint32_t, uint32_t>{0u, 0u} : v[Pick(r, static_cast<uint32_t>(v.size()))];
            DrawState(c, sw.first);
            const uint32_t rec = sw.second + (Chance(r, 2) ? 0x34u : 0x4Cu);
            AnimState(c, rec);
            SpriteRecordState(c, rec);
            a.a[0] = rec;
            a.a[1] = m.PeekWord(sh::kOtTable) + 4u * m.PeekByte(sh::kFeLayer) + 0x0Cu;
            a.a[2] = c.sample ? 3u : Pick(r, 5);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                uint32_t r = 0;
                if (!sh::AnimSprite(g, k, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), &r) && env.failure.empty())
                    env.failure = "refused";
                return r;
            });
        },
        true, {OracleCallee{sh::kPacketWrap, 2, 0, {}, 0}, OracleCallee{sh::kSpriteUpload, 1, 0, {}, 0}}));

    // ------------------------------------------------------------------ RASHCDF 0x8006ECDC
    rows.push_back(ShellRow(
        "shell_panel_emit", "s32 PanelEmit(Screen*, Panel*, u32* ot)  [RASHCDF 0x8006ECDC + 0x8006EE20]", 0x8006ECDCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            const auto v = WidgetsOf(m, {11});
            const auto sw = v.empty() ? std::pair<uint32_t, uint32_t>{0u, 0u} : v[Pick(*c.rng, static_cast<uint32_t>(v.size()))];
            DrawState(c, sw.first);
            PanelExtra(c, sw.first, sw.second);
            a.a[0] = sw.first;
            a.a[1] = sw.second + 0x10u;
            a.a[2] = m.PeekWord(sh::kOtTable) + 4u * m.PeekByte(sh::kFeLayer) + 0x14u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                uint32_t r = 0;
                if (!sh::PanelEmit(g, k, a.a[0], a.a[1], a.a[2], &r) && env.failure.empty()) env.failure = "refused";
                return r;
            });
        },
        true, {OracleCallee{sh::kPacketWrap, 2, 0, {}, 0}}));
}
