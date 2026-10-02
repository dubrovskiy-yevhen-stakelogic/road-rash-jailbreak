// Bench rows for the front end. Included by verify_physics.cpp
// inside its anonymous namespace, so everything defined above that point is visible here.
//
// The port is src\game\shell\shell_logic.{h,cpp} (and the boot's initialisers in shell_arena.cpp):
// RASHCDF.BIN functions over road_query.h's guest-address view, one row per function.
//
// THE MACHINE. The bench's snapshot (rr-race) holds RASHCDG in the overlay window, so every shell row
// first PLANTS a capture of the running shell - work\oracle\state\retro-shell\ram.bin, taken
// while the main menu (screen 4) was up, RASHCDF resident - over the machine's RAM from 0x80010000 up
// (the kernel's RAM below it is left as the snapshot has it; no shell function reaches it). The guest
// and the port then start from the same shell machine; what is compared afterwards is, as for every
// row, the whole 2 MiB outside the stack window plus the scratchpad.
//
// INPUT FAMILIES.
//   * dump-derived: the capture as it is, with one pad slot (or none) pressed and the arguments the
//     shell itself passes (the current screen, its pad window).
//   * randomised: the fields each function branches on drawn - pad press codes, the current screen,
//     screen flags, widget types/codes/condition words, the session's mode, venue and flags, chooser
//     options and their gates, the bitmaps, the slider descriptor.
// Named bounds, each the range the game's own data keeps:
//   * a screen's selection +0x04 is in [0, item count) or 32767 ("nothing selected") - outside it the
//     original's cursor walk never returns (it waits for the index to come round again);
//   * screen ids are 0..58 and a screen's parent chain ends at -1 (the tables are the capture's);
//   * the edited port fe+0x16 is 0..3 (four per-port records), a player index 0..5 (six records);
//   * a chooser's current option is in [0, count); a slider's slot +0x66 is 0..5 (six sliders);
//   * the venue session+0x04 is 0..5 (the commit indexes six-entry tables with it);
//   * the attract flag 0x8005B220 is 0 for the commit row half the time and set the other half.
//
// THE SEAMS - callees the port does not contain, run by the oracle on the candidate's clone and
// witnessed: PlayUiSound RASHCDF 0x8007EAC0, MusicPlay 0x8007EDE0, MusicStop 0x8007F158, SkipMovie
// 0x8006FED4, the chooser pass 0x8006738C, the sound-mode pair SLUS 0x8001EFDC / RASHCDF 0x8007ED34,
// the jukebox SLUS 0x8002490C, the splash fade SLUS 0x80022A78, the pad install SLUS 0x8001B67C, and
// the per-screen / per-object handlers that are not ported yet.

namespace shellrows {

namespace sh = rr::shell;

const std::vector<uint8_t>& ShellRam() {
    static const std::vector<uint8_t> ram = [] {
        std::vector<uint8_t> v;
        std::string error;
        if (!rr::interp::ReadWholeFile("work\\oracle\\state\\retro-shell\\ram.bin", v, error) || v.size() < 0x200000u)
            v.clear();
        return v;
    }();
    return ram;
}

uint32_t Pick(Rng& r, uint32_t n) { return n == 0 ? 0u : r.U32() % n; }
bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }
void Put8(Memory& m, uint32_t a, uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    m.WriteBlock(a, &b, 1);
}
void Put16(Memory& m, uint32_t a, uint32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }
uint16_t Get16(const Memory& m, uint32_t a) { return static_cast<uint16_t>(m.PeekByte(a) | (m.PeekByte(a + 1) << 8)); }

// The captured shell over the machine's RAM from 0x80010000 up (the file header says why).
void PlantShell(CaseContext& c) {
    const std::vector<uint8_t>& ram = ShellRam();
    if (ram.empty()) return; // the rows then FAIL on the overlay's code not being there
    c.mem->WriteBlock(0x80010000u, ram.data() + 0x10000u, 0x200000u - 0x10000u);
}

uint32_t Screen(const Memory& m, uint32_t id) { return m.PeekWord(m.PeekWord(sh::kScreenTablePtr) + 4u * id); }
uint32_t ChooserObj(const Memory& m, uint32_t i) { return m.PeekWord(sh::kChoosers + 4u * i); }

// A screen that has a widget array, by id.
uint32_t PickScreenId(Rng& r, const Memory& m) {
    for (int t = 0; t < 64; ++t) {
        const uint32_t id = Pick(r, 59);
        const uint32_t s = Screen(m, id);
        if (s != 0 && m.PeekWord(s + 16u) != 0 && static_cast<int16_t>(Get16(m, s + 8u)) > 0) return id;
    }
    return 4;
}

// Clears every pad press code of the four snapshot records, then presses at most one slot per pad.
void Pads(CaseContext& c, bool random) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t p = 0; p < 4u; ++p)
        for (int s = 0; s < 19; ++s) Put8(m, sh::PadSlot(p, s), 0);
    const uint32_t pads = random ? 1u + Pick(r, 3) : 1u;
    for (uint32_t p = 0; p < pads; ++p) {
        if (Chance(r, 5)) continue;
        const int slot = static_cast<int>(Pick(r, random ? 14 : 8));
        uint32_t code = 2;
        if (random) code = Chance(r, 4) ? (r.U32() & 0xFFu) : (1u + Pick(r, 3));
        Put8(m, sh::PadSlot(random ? Pick(r, 4) : p, slot), code);
    }
}

// Widget fields every menu function branches on, drawn over one screen's items.
void ShuffleWidgets(CaseContext& c, uint32_t screen) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const int32_t n = static_cast<int16_t>(Get16(m, screen + 8u));
    const uint32_t items = m.PeekWord(screen + 16u);
    if (items == 0) return;
    static const uint16_t kTypes[] = {12, 13, 17, 7, 16, 9, 12, 13, 11, 12};
    static const uint32_t kBits[] = {0x40000000u, 0x02000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x00063F7Fu};
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t w = items + 120u * static_cast<uint32_t>(i);
        if (Chance(r, 3)) Put16(m, w + 8u, kTypes[Pick(r, 10)]);
        if (Chance(r, 3)) {
            uint32_t cw = 0;
            for (uint32_t b : kBits)
                if (Chance(r, 3)) cw |= b;
            m.PokeWord(w, cw);
        }
        if (Chance(r, 3)) Put16(m, w + 18u, Pick(r, 81));
        if (Chance(r, 4)) Put16(m, w + 10u, Get16(m, w + 10u) ^ 0x1000u);
        if (Chance(r, 4)) Put16(m, w + 100u, Pick(r, 8));
        if (Chance(r, 4)) Put8(m, w + 0x66u, Pick(r, 6));
    }
}

// The session and port state the navigation gates read.
void ShuffleGates(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 2)) m.PokeWord(sh::kSession, Chance(r, 2) ? 4u : (Chance(r, 2) ? 0x20u : 1u));
    if (Chance(r, 2)) Put8(m, sh::kSession + 0x1Bu, Pick(r, 2));
    if (Chance(r, 2)) Put8(m, sh::kMultitapByte, Chance(r, 2) ? 0x80u : (r.U32() & 0xFFu));
    Put8(m, sh::kFePort, Pick(r, 4));
    for (uint32_t p = 0; p < 4u; ++p)
        if (Chance(r, 2)) m.PokeWord(sh::kPorts + 24u * p + 4u, Pick(r, 3));
    for (uint32_t p = 0; p < 4u; ++p)
        if (Chance(r, 2)) m.PokeWord(sh::kPads + 192u * p + 0x10u, Pick(r, 2));
}

// A screen's selection inside its named bound.
void Select(CaseContext& c, uint32_t screen) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const int32_t n = static_cast<int16_t>(Get16(m, screen + 8u));
    Put16(m, screen + 4u, (n <= 0 || Chance(r, 8)) ? 32767u : Pick(r, static_cast<uint32_t>(n)));
}

// Chooser objects and options: the current index in bounds, option gates drawn.
void ShuffleChooser(CaseContext& c, uint32_t obj) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (obj == 0) return;
    const int32_t n = static_cast<int8_t>(m.PeekByte(obj));
    const uint32_t opts = m.PeekWord(obj + 4u);
    if (n <= 0 || opts == 0) return;
    Put8(m, obj + 3u, Pick(r, static_cast<uint32_t>(n)));
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t o = opts + 12u * static_cast<uint32_t>(i);
        if (Chance(r, 3)) Put8(m, o, Chance(r, 2) ? (Chance(r, 2) ? 4u : 8u) : Pick(r, 3));
        if (Chance(r, 3)) Put8(m, o + 1u, Pick(r, 72));
        if (Chance(r, 3)) Put8(m, o + 2u, r.U32() & 0xF0u);
        if (Chance(r, 3)) Put8(m, o + 3u, r.U32() & 0x3Fu);
    }
    Put8(m, sh::kFeOptMask, 1u << Pick(r, 6));
    for (uint32_t b = 0; b < 12u; ++b)
        if (Chance(r, 2)) Put8(m, sh::kProgress + b, r.U32() & 0xFFu);
}

// A chooser that has options.
uint32_t PickChooser(Rng& r, const Memory& m) {
    for (int t = 0; t < 64; ++t) {
        const uint32_t obj = ChooserObj(m, Pick(r, 37));
        if (obj != 0 && m.PeekWord(obj + 4u) != 0 && static_cast<int8_t>(m.PeekByte(obj)) > 0) return obj;
    }
    return ChooserObj(m, 0);
}

// ---------------------------------------------------------------------------- the seams
struct ShellOracle final : sh::ShellCallees {
    NativeEnv& env;
    explicit ShellOracle(NativeEnv& e) : env(e) {}
    bool Call(uint32_t address, const uint32_t* args, int count, uint32_t* v0) override {
        uint32_t a[12] = {};
        for (int i = 0; i < count && i < 12; ++i) a[i] = args[i];
        const uint32_t r = env.CallOracleArgs(address, a, count > 4 ? count : 4);
        if (v0 != nullptr) *v0 = r;
        return env.failure.empty();
    }
};

template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    sh::GuestRam g(env.clone->mem.ram().data(), env.gp);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    ShellOracle k(env);
    const uint32_t v = f(g, k);
    if (g.Faulted() && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the port met an address the console faults on: 0x%08X", g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

std::vector<OracleCallee> Sounds() {
    return {OracleCallee{sh::kUiSound, 1, 0, {}, 0}, OracleCallee{sh::kMusicPlay, 1, 0, {}, 0}};
}

// Every handler of the two tables that the port does not contain yet, and the pass the input pass
// calls; arity 1 (h(screen)) except the skip handshake (no argument).
std::vector<OracleCallee> PassSeams() {
    std::vector<OracleCallee> v = Sounds();
    v.push_back(OracleCallee{sh::kMusicStop, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSkipMovie, 0, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSoundMode, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSoundModeUi, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kJukeboxSet, 2, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSplashFade, 1, 0, {}, 0});
    // The four memory-card screens are not ported yet (the rest are, shell_screens.h); the sound
    // options' mixer, preview and preview-stop calls are reached natively through them.
    // The card screens run ported (shell_memcard.h); their card layer is the seam.
    for (uint32_t a : {0x8005F158u, 0x8005F180u, 0x8005F21Cu}) v.push_back(OracleCallee{a, 0, 0, {}, 0});
    for (uint32_t a : {0x8005ED6Cu, 0x8005EC28u}) v.push_back(OracleCallee{a, 1, 0, {}, 0});
    v.push_back(OracleCallee{0x8007F20Cu, 2, 0, {}, 0});
    v.push_back(OracleCallee{0x8007EB1Cu, 2, 0, {}, 0});
    v.push_back(OracleCallee{0x8007ECFCu, 0, 0, {}, 0});
    return v;
}

Row ShellRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
             std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, std::vector<OracleCallee> callees = {},
             uint32_t frame = 0) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native), std::move(callees)};
    r.oracleFrame = frame;
    r.maxSteps = 20'000'000;
    return r;
}

// The current screen made `id`, the way the shell's own globals hold it.
void MakeCurrent(Memory& m, uint32_t id) {
    Put16(m, sh::kFeCur, id);
    m.PokeWord(sh::kCurScreen, Screen(m, id));
}

} // namespace shellrows

// ---------------------------------------------------------------------------- the text rows
// src\game\shell\shell_text.{h,cpp}: the widget pass 0x8006D3E0, the text-block handler 0x8006FAC8
// with its arms, the text source 0x80063C7C and the name-id helpers.
// The header is included at namespace scope (the anonymous namespace is closed around it, as
// rows_shell_career.inc does).
} // namespace
#include "game/shell/shell_text.h"
#include "game/shell/shell_memcard.h"
#include "game/shell/shell_screens.h"
#include "game/shell/shell_sound.h"
namespace {

namespace shelltext {

using namespace shellrows;

// Every screen of the capture that carries a type-16 widget, and those widgets.
std::vector<std::pair<uint32_t, uint32_t>> TextWidgets(const Memory& m) {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (uint32_t id = 0; id < 59u; ++id) {
        const uint32_t s = Screen(m, id);
        if (s == 0 || m.PeekWord(s + 16u) == 0) continue;
        const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
        for (int32_t i = 0; i < n; ++i) {
            const uint32_t w = m.PeekWord(s + 16u) + 120u * static_cast<uint32_t>(i);
            if (Get16(m, w + 8u) == 16u) v.push_back({s, w});
        }
    }
    return v;
}

// The session, player and fe fields the arms and the text source read, drawn inside the named bounds
// (each keeps every string id the original would pass inside FESTRING.LOC's 1945, as the game's own
// data does): mode 32 / 1 / 4 / 8 / 16; venue 0..5; road set 1..2; mission 0..17; race 1..40; Five-O
// beats 0..6 (the panel's id is 0x5E6 + the count); the players' bikes 0..20, gangs 0..1, levels
// 0..3; the notifications fe+0x1C / +0x78 / +0x7A 0 or an id of the career messages (0x48E..0x636).
void TextState(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    static const uint32_t kModes[] = {0x20u, 1u, 4u, 8u, 0x10u, 0x20u};
    m.PokeWord(sh::kSession, kModes[Pick(r, 6)]);
    Put8(m, sh::kSession + 4u, Pick(r, 6));
    Put8(m, sh::kSession + 6u, 1u + Pick(r, 2));
    Put8(m, sh::kSession + 7u, Pick(r, 18));
    Put8(m, sh::kSession + 8u, 1u + Pick(r, 40));
    for (uint32_t o : {0x09u, 0x0Au, 0x0Bu, 0x14u, 0x17u, 0x19u, 0x1Bu}) Put8(m, sh::kSession + o, Pick(r, 3));
    Put8(m, sh::kSession + 0x11u, Pick(r, 7));
    Put8(m, sh::kSession + 0x12u, 1u + Pick(r, 6));
    Put8(m, sh::kSession + 0x13u, Pick(r, 2));
    Put16(m, sh::kSession + 0x20u, Pick(r, 12));
    Put16(m, sh::kSession + 0x22u, Pick(r, 12));
    for (uint32_t p = 0; p < 2u; ++p) {
        const uint32_t b = sh::kPlayers + 0x24u * p;
        Put8(m, b + 7u, Pick(r, 21));
        Put8(m, b + 9u, Pick(r, 2));
        Put8(m, b + 10u, Pick(r, 4));
        Put8(m, b + 0x15u, Pick(r, 4));
        Put8(m, b + 0x17u, Pick(r, 2));
        for (uint32_t o = 0x18; o < 0x20u; ++o) Put8(m, b + o, Chance(r, 3) ? 0u : Pick(r, 12));
        // The rap sheet's weapons: owned bits 0..8 at +0x0C, ammo nibbles at +0x10, nitros at +0x14.
        Put16(m, b + 0x0Cu, r.U32() & 0x1FFu);
        m.PokeWord(b + 0x10u, Chance(r, 4) ? 0u : (r.U32() & 0x00FFFFFFu));
        Put8(m, b + 0x14u, Pick(r, 10));
    }
    Put8(m, sh::kFe + 0x0Eu, Pick(r, 18));
    Put8(m, sh::kFe + 0x19u, Pick(r, 2));
    Put8(m, sh::kFePort, Pick(r, 2));
    auto noteId = [&]() { return Chance(r, 3) ? 0u : 0x48Eu + Pick(r, 0x636u - 0x48Eu + 1u); };
    Put16(m, sh::kFe + 0x1Cu, noteId());
    // The post-race panels: the targets the dispatchers wrote and the counters walking towards them.
    for (uint32_t i = 0; i < 3u; ++i) {
        const uint32_t target = Chance(r, 4) ? 0u : Pick(r, 3000);
        Put16(m, sh::kFe + 0x70u + 2u * i, target);
        uint32_t shown = target;
        if (Chance(r, 2)) shown = Pick(r, 3000);
        else if (Chance(r, 2)) shown = target > 20u ? target - Pick(r, 20) : 0u;
        Put16(m, sh::kFe + 0x54u + 2u * i, shown);
    }
    Put16(m, sh::kFe + 0x78u, noteId());
    Put16(m, sh::kFe + 0x7Au, noteId());
    Put16(m, sh::kFe + 0x62u, Chance(r, 8) ? r.U32() : Pick(r, 3));
    for (uint32_t i = 0; i < 6u; ++i) Put16(m, sh::kFe + 0x44u + 2u * i, Pick(r, 13));
    Put16(m, sh::kFe + 0x52u, Chance(r, 8) ? r.U32() : Pick(r, 7));
    Put16(m, sh::kFe + 0x64u, Pick(r, 7));
    Put16(m, sh::kFe + 0x66u, Pick(r, 12));
    Put16(m, sh::kFe + 0x68u, Pick(r, 12));
    Put8(m, sh::kFe + 0x76u, Chance(r, 8) ? r.U32() : Pick(r, 7));
    Put8(m, sh::kFe + 0x11u, Pick(r, 4));
}

// The state the ported arms read, inside named bounds (each the range the game keeps):
//   * the card record: the pointer 0x8009954C at 0x80099550 (0x8005EB80 sets it), a state and a
//     previous state 0..26, a slot 0..9, each slot record's in-use word 0 / 1 and its saved mode one of
//     the game's, venue 0..5, gang 0..1, level 0..3;
//   * the keyboard: column 0..8, row 0..4, cursor 0..7 (every arm of 52 / 56 keeps them there);
//   * the trophy room's race 56..64 (the nine tables of 0x80053A88) with bikes 0..20 in its rows, and
//     no row's time 9999 ("no time"): that arm's sprintf 0x8005C5B8 takes no value, and the a2 the
//     guest then passes is what the preceding text call left in the register - which the seam compares
//     and the port cannot know (measured: 2 of 1160 cases on the second seed before this bound);
//   * the jukebox: rows above the cursor 0..4, the track chooser's current option in range;
//   * the credits: a line step 8..15, 0..3 frames per scroll step, a scroll step 1..2, the scroll
//     position 100..260 (from 230 down, as the arm moves it).
void ArmState(CaseContext& c, uint32_t w) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t card = 0x80099550u;
    m.PokeWord(0x8009954Cu, card);
    Put16(m, card + 8u, Pick(r, 27));
    Put16(m, card + 10u, Pick(r, 27));
    Put16(m, card + 14u, Pick(r, 10));
    Put8(m, card + 545u, Pick(r, 2));
    static const uint32_t kModes[] = {1, 4, 8, 0x10, 0x11, 0x18, 0x20, 0x20, 2};
    for (uint32_t s = 0; s < 10u; ++s) {
        const uint32_t rec = card + 2132u + 484u * s;
        m.PokeWord(rec, Chance(r, 3) ? 1u : 0u);
        m.PokeWord(rec + 220u, kModes[Pick(r, 9)]);
        Put8(m, rec + 224u, Pick(r, 6));
        Put8(m, rec + 237u, Pick(r, 10));
        Put8(m, rec + 13u, Pick(r, 2));
        Put8(m, rec + 14u, Pick(r, 4));
    }
    m.PokeWord(sh::kKeyCol, Pick(r, 9));
    m.PokeWord(sh::kKeyRow, Pick(r, 5));
    m.PokeWord(sh::kKeyPos, Pick(r, 8));
    const uint32_t race = 56u + Pick(r, 9);
    Put8(m, sh::kFe + 0x1Fu, race);
    for (uint32_t i = 0; i < 8u; ++i) {
        const uint32_t e = sh::kRecords + (race - 56u) * 176u + 20u * i;
        Put8(m, e + 32u, Pick(r, 21));
        if (Chance(r, 3)) m.PokeWord(e + 28u, Pick(r, 400000)); // never 9999 (the bound below)
        for (uint32_t f = 33; f < 36u; ++f) Put8(m, e + f, Chance(r, 3) ? 1u : 0u);
    }
    Put8(m, sh::kFe + 6u, Pick(r, 5));
    const uint32_t juke = m.PeekWord(sh::kChoosers + 124u);
    if (juke != 0 && static_cast<int8_t>(m.PeekByte(juke)) > 0)
        Put8(m, juke + 3u, Pick(r, static_cast<uint32_t>(static_cast<int8_t>(m.PeekByte(juke)))));
    Put8(m, w + 0x24u, 8u + Pick(r, 8));
    Put8(m, w + 0x25u, Pick(r, 4));
    Put8(m, w + 0x26u, 1u + Pick(r, 2));
    Put8(m, w + 0x27u, Pick(r, 4));
    Put16(m, 0x8009CCE0u, 100u + Pick(r, 161));
    if (Chance(r, 2)) Put16(m, sh::kFeCur, 43u + Pick(r, 4));
}

std::vector<OracleCallee> TextSeams() {
    std::vector<OracleCallee> v = {
        OracleCallee{sh::kTextId, 4, 2, {}, 0},
        // The string's pointer is a local of the arm's frame, at the original's own address on both
        // sides (the port writes its buffers there), so it is compared as it is.
        OracleCallee{sh::kTextStr, 4, 2, {}, 0},
        OracleCallee{sh::kTextAt, 4, 2, {}, 0},
        // sprintf is variadic: its first value is compared here, the rest through the text call's buffer.
        OracleCallee{sh::kSprintf, 3, 0, {}, 0},
        OracleCallee{sh::kUiSound, 1, 0, {}, 0},
        // The sprite and panel emitters the rap sheet draws its weapon slots with (screen, record, ot).
        OracleCallee{sh::kSpriteEmit, 3, 0, {}, 0},
        OracleCallee{sh::kPanelEmit, 3, 0, {}, 0},
    };
    // Every arm is ported now (shell_text.cpp, shell_memcard.cpp); what they reach below them: the
    // credits' draw-area primitive SLUS 0x8001C304 (x, y, w, h + three on the stack), and the keyboard's
    // SetSemiTrans 0x8004CDE4, AddPrim 0x8004CDA4 and the packet buffer's wrap 0x80021C98.
    v.push_back(OracleCallee{0x8001C304u, 4, 3, {}, 0});
    v.push_back(OracleCallee{0x8004CDE4u, 2, 0, {}, 0});
    v.push_back(OracleCallee{0x8004CDA4u, 2, 0, {}, 0});
    v.push_back(OracleCallee{0x80021C98u, 2, 0, {}, 0});
    return v;
}

void AddRows(std::vector<Row>& rows) {
    // ------------------------------------------------------------------ RASHCDF 0x80063C7C
    rows.push_back(ShellRow(
        "shell_text_source", "void* TextSource(s16 kind)  [RASHCDF 0x80063C7C]", 0x80063C7Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            uint32_t kind = 14;
            if (!c.sample) {
                TextState(c);
                static const uint32_t kKinds[] = {14, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                                                  0x29, 0x2A, 0x2B, 0x2C, 0x38, 11, 12, 13, 1, 0};
                kind = Chance(r, 6) ? (r.U32() & 0xFFFFu) : kKinds[Pick(r, 20)];
                // The selected item (a real widget of a random screen, or none) and the ticked screen.
                const uint32_t s = Screen(m, PickScreenId(r, m));
                const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
                m.PokeWord(sh::kFeItem, (Chance(r, 5) || n <= 0) ? 0u : m.PeekWord(s + 16u) + 120u * Pick(r, static_cast<uint32_t>(n)));
                Put16(m, sh::kFeTicked, Chance(r, 4) ? Pick(r, 59) : Get16(m, sh::kFeCur));
                // The bound chooser (one of the table's, or none) and its current option.
                const uint32_t ch = Chance(r, 5) ? 0u : ChooserObj(m, Pick(r, 37));
                m.PokeWord(sh::kFeChooser, ch);
                if (ch != 0 && m.PeekWord(ch + 4u) != 0) {
                    const int32_t cnt = static_cast<int8_t>(m.PeekByte(ch));
                    Put8(m, ch + 3u, cnt > 0 ? Pick(r, static_cast<uint32_t>(cnt)) : 0u);
                }
            }
            a.a[0] = kind;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) { return sh::TextSource(g, a.a[0]); });
        },
        true));

    // ------------------------------------------------------------------ the name-id helpers
    struct Helper {
        const char* name;
        const char* sig;
        uint32_t entry;
        int args;
    };
    static const Helper kHelpers[] = {
        {"shell_text_mode_name", "s16 ModeNameId(s8 venue, u32 mode)  [RASHCDF 0x80071F0C]", 0x80071F0Cu, 2},
        {"shell_text_bike_name", "s16 BikeNameId(s8 bike)  [RASHCDF 0x80071FDC]", 0x80071FDCu, 1},
        {"shell_text_gang_name", "s16 GangNameId(s8 gang)  [RASHCDF 0x80072A60]", 0x80072A60u, 1},
        {"shell_text_rank_name", "s16 RankNameId(s8 gang, s8 level)  [RASHCDF 0x80072A80]", 0x80072A80u, 2},
        {"shell_text_course_name", "s16 CourseNameId(s8 race)  [RASHCDF 0x800720B4]", 0x800720B4u, 1},
        {"shell_text_action_group", "s32 ActionGroup(Widget*)  [RASHCDF 0x80072100]", 0x80072100u, 1},
    };
    for (const Helper& h : kHelpers) {
        const uint32_t entry = h.entry;
        rows.push_back(ShellRow(
            h.name, h.sig, entry,
            [entry](CaseContext& c, Args& a) {
                PlantShell(c);
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                auto small = [&]() { return Chance(r, 5) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 24)) - 2); };
                a.a[0] = small();
                a.a[1] = Chance(r, 2) ? 1u : small();
                if (entry == 0x800720B4u) Put8(m, sh::kSession + 6u, Chance(r, 2) ? 2u : Pick(r, 4));
                if (entry == 0x80072100u) {
                    const uint32_t s = Screen(m, c.sample ? 4u : PickScreenId(r, m));
                    const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
                    const uint32_t w = m.PeekWord(s + 16u) + 120u * Pick(r, static_cast<uint32_t>(n > 0 ? n : 1));
                    if (!c.sample) {
                        if (Chance(r, 2)) Put16(m, w + 8u, Chance(r, 2) ? 12u : 13u);
                        if (Chance(r, 2)) Put16(m, w + 0x12u, Chance(r, 6) ? r.U32() : Pick(r, 81));
                    }
                    a.a[0] = w;
                }
            },
            [entry](NativeEnv& env, const Args& a) {
                return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) -> uint32_t {
                    // Each takes its arguments as whole registers (the callers load them sign-extended).
                    switch (entry) {
                    case 0x80071F0Cu: return static_cast<uint32_t>(sh::ModeNameId(static_cast<int32_t>(a.a[0]), a.a[1]));
                    case 0x80071FDCu: return static_cast<uint32_t>(sh::BikeNameId(static_cast<int32_t>(a.a[0])));
                    case 0x80072A60u: return static_cast<uint32_t>(sh::GangNameId(static_cast<int32_t>(a.a[0])));
                    case 0x80072A80u: return static_cast<uint32_t>(sh::RankNameId(static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1])));
                    case 0x800720B4u: return static_cast<uint32_t>(sh::CourseNameId(g, static_cast<int32_t>(a.a[0])));
                    case 0x80072100u: return static_cast<uint32_t>(sh::ActionGroup(g, a.a[0]));
                    default: return 0u;
                    }
                });
            },
            true));
    }

    // ------------------------------------------------------------------ RASHCDF 0x8006D3E0
    rows.push_back(ShellRow(
        "shell_widget_pass", "void WidgetPass(Screen*)  [RASHCDF 0x8006D3E0]", 0x8006D3E0u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            // Named bound of the bench: a randomised case takes a screen with none of the animated
            // decorations 2..6 (0x8006E4D8 drives the MDEC, which the interpreter does not model).
            uint32_t id = 4;
            for (int t = 0; !c.sample && t < 64; ++t) {
                id = PickScreenId(r, m);
                const uint32_t cand = Screen(m, id);
                bool mdec = false;
                for (int32_t i = 0; i < static_cast<int16_t>(Get16(m, cand + 8u)); ++i) {
                    const uint32_t ty = Get16(m, m.PeekWord(cand + 16u) + 120u * static_cast<uint32_t>(i) + 8u);
                    mdec = mdec || (ty >= 2u && ty <= 6u);
                }
                if (!mdec) break;
                id = 4;
            }
            const uint32_t s = Screen(m, id);
            if (!c.sample) {
                Put16(m, s + 2u, Chance(r, 2) ? 0u : (Chance(r, 2) ? 32766u : 1u));
                const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
                static const uint32_t kBits[] = {0x80000000u, 0x02000000u, 0x10000000u, 0x08000000u, 0x04000000u};
                for (int32_t i = 0; i < n; ++i) {
                    const uint32_t w = m.PeekWord(s + 16u) + 120u * static_cast<uint32_t>(i);
                    // Named bound of the bench: no type is turned INTO one whose handler loads or decodes
                    // a picture - the movie player 1 (0x8006E400, unused by the shipped data), the
                    // animated decorations 2..6 (0x8006E4D8), the logo 8, the chooser 13 and 10 / 18 -
                    // they reach the MDEC, a device the interpreter does not model (measured: with 8,
                    // 10, 13, 18 allowed 23 of 512 random cases trap at SLUS 0x8004DA98).
                    static const uint32_t kTypes[] = {0, 7, 9, 11, 12, 14, 15, 16, 17, 19};
                    if (Chance(r, 3) && false) Put16(m, w + 8u, kTypes[Pick(r, 14)]);
                    uint32_t cw = m.PeekWord(w + 4u);
                    if (Chance(r, 3)) cw = (cw & ~0x3F00u) | ((r.U32() & 0x3Fu) << 8);
                    for (uint32_t b : kBits)
                        if (Chance(r, 5)) cw ^= b;
                    m.PokeWord(w + 4u, cw);
                }
                Put8(m, sh::kSession + 4u, Pick(r, 6));
                m.PokeWord(sh::kSession, Chance(r, 2) ? 4u : 0x20u);
                Put8(m, sh::kSession + 0x1Bu, Pick(r, 2));
                Put8(m, sh::kFePort, Pick(r, 4));
                for (uint32_t p = 0; p < 4u; ++p) m.PokeWord(sh::kPorts + 24u * p + 4u, Pick(r, 3));
                Put16(m, sh::kFeTicked, Chance(r, 2) ? Get16(m, sh::kFeCur) : Pick(r, 59));
            }
            a.a[0] = s;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                if (!sh::WidgetPass(g, k, a.a[0]) && env.failure.empty()) env.failure = "refused";
                return 0u;
            });
        },
        false, [] {
            std::vector<OracleCallee> v;
            for (uint32_t h : {0x8006E400u, 0x8006E3BCu, 0x8006E4D8u, 0x8006E894u, 0x8006E8FCu, 0x8006E7A4u,
                               0x8006EDE4u, 0x8006ECA0u, 0x8006F764u, 0x8006EF30u, 0x8006F9A4u, 0x8006FA38u,
                               0x8006FAC8u, 0x8006F4A0u, 0x8006F0B8u, 0x80070580u})
                v.push_back(OracleCallee{h, 2, 0, {}, 0});
            return v;
        }()));
    rows.back().allowBiosCalls = true; // the handlers' text calls reach strlen through the A0 vector

    // ------------------------------------------------------------------ RASHCDF 0x8006FAC8
    rows.push_back(ShellRow(
        "shell_text_block", "s32 TextBlock(Screen*, Widget*)  [RASHCDF 0x8006FAC8 + its 56 arms]", 0x8006FAC8u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const auto widgets = TextWidgets(m);
            // dump-derived: the main menu's blurb (kind 14) as the capture holds it, or a random text
            // block of the capture with its own kind and flags.
            std::pair<uint32_t, uint32_t> sw = widgets.empty() ? std::pair<uint32_t, uint32_t>{0u, 0u}
                                                               : widgets[Pick(r, static_cast<uint32_t>(widgets.size()))];
            if (c.sample && Chance(r, 2))
                for (const auto& p : widgets)
                    if (p.first == Screen(m, 4u) && Get16(m, p.second + 0x18u) == 14u) sw = p;
            const uint32_t s = sw.first, w = sw.second;
            const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
            Put16(m, s + 4u, n > 0 ? Pick(r, static_cast<uint32_t>(n)) : 0u);
            Put16(m, s, Get16(m, s) | 2u);
            if (!c.sample) {
                TextState(c);
                if (Chance(r, 4)) Put16(m, w + 10u, Get16(m, w + 10u) ^ 0x2000u);
                if (Chance(r, 6)) Put16(m, w + 10u, Get16(m, w + 10u) ^ 0x20u);
                if (Chance(r, 6)) Put16(m, s, Get16(m, s) & ~2u);
                if (Chance(r, 2)) {
                    static const uint32_t kKinds[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
                                                      0x15, 0x1A, 0x1B, 0x1B, 0x1C, 0x1C, 0x1D, 0x1D, 0x1F, 0x38,
                                                      0x1E, 0x1E, 0x1E, 0x1B, 0x1C, 0x1D, 0x1A, 0x03, 0x12, 0x13,
                                                      0x0A, 0x1E, 0x28, 0x2F, 0x36, 0x37, 0, 11, 12, 14, 0x16, 0x39};
                    Put16(m, w + 0x18u, kKinds[Pick(r, sizeof(kKinds) / sizeof(kKinds[0]))]);
                }
                if (Chance(r, 3)) Put16(m, w + 0x18u, 0x2Du + Pick(r, 2)); // the card screens' two
                if (Chance(r, 3)) Put16(m, w + 0x18u, Chance(r, 2) ? 0x28u : 0x37u);
                if (Get16(m, w + 0x18u) == 0x2Du && Chance(r, 2)) Put16(m, s + 6u, 43u + Pick(r, 4));
                ArmState(c, w);
                if (Chance(r, 3)) Put16(m, w + 0x1Au, Pick(r, 4));
                if (n > 0 && Chance(r, 2)) {
                    const uint32_t item = m.PeekWord(s + 16u) + 120u * Get16(m, s + 4u);
                    Put16(m, item + 10u, Get16(m, item + 10u) ^ 0x100u);
                }
                if (Chance(r, 12)) Put8(m, 0x800D8078u + 0x18u * m.PeekWord(sh::kFontMini), 0);
                if (Chance(r, 12)) Put8(m, 0x800D8078u + 0x18u * m.PeekWord(sh::kFontHdr), 0);
                Put16(m, sh::kFeTicked, Chance(r, 4) ? Pick(r, 59) : Get16(m, sh::kFeCur));
            }
            a.a[0] = s;
            a.a[1] = w;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                if (!sh::TextBlock(g, k, a.a[0], a.a[1], env.sp) && env.failure.empty()) env.failure = "refused";
                return 1u;
            });
        },
        true, TextSeams()));
    rows.back().allowBiosCalls = true; // the text calls reach strlen through the A0 vector (0x800448F4)
}

} // namespace shelltext

// ---------------------------------------------------------------------------- the card screens
// src\game\shell\shell_memcard.{h,cpp}: the card screens' state machine 0x8006C770, the four handlers,
// the slot copies and the mode label.
//
// THE SEAMS: the card layer (enter 0x8005F158, leave 0x8005F180, poll 0x8005F21C, format 0x8005ED6C,
// write 0x8005EC28), PlayUiSound, and what LoadSlot reaches (the sound mode 0x8001EFDC, the jukebox
// 0x8002490C, MusicPlay 0x8007EDE0, the mixer 0x8007F20C).
// NAMED BOUNDS, besides ArmState's card record:
//   * the card driver is idle: *(0x800548CC) is the record's +0x1BC4 (0x8005EB80 puts it there) and its
//     status word +0x900 equals the status the record last saw (+0x10), so a poll starts no card I/O;
//     its busy words +0x8F4 / +0x8F8 / +0x9A4 are 0 (no operation in flight);
//   * the music may not start (fe+0x0F = 0): LoadSlot's MusicPlay of a track other than the loaded one
//     reads the CD (see rows_shell_screens.inc);
//   * the saved screen (session +0x10 of a slot) is a screen id 0..58.
namespace cardrows {

using namespace shellrows;

void CardMachineState(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    shelltext::ArmState(c, 0x80099550u + 0x20u); // the widget bytes it also draws land in the block copy
    const uint32_t card = 0x80099550u;
    for (uint32_t i = 0; i < 8u; ++i) Put8(m, card + i, i == 0 ? Pick(r, 3) : Pick(r, 2));
    const uint32_t status = Chance(r, 2) ? 0u : (Chance(r, 2) ? 0xFFFFFFFFu : 0xFFFFFFFEu);
    m.PokeWord(card + 0x10u, status);
    const uint32_t drv = card + 0x1BC4u;
    m.PokeWord(0x800548CCu, drv);
    m.PokeWord(drv + 0x900u, status);
    m.PokeWord(drv + 0x8F4u, 0);
    m.PokeWord(drv + 0x8F8u, 0);
    m.PokeWord(drv + 0x9A4u, 0);
    for (uint32_t s = 0; s < 10u; ++s) Put8(m, card + 2132u + 484u * s + 220u + 16u, Pick(r, 59));
    Put8(m, sh::kFeMusicOk, 0);
    Put8(m, sh::kCardRecordsSaved, Pick(r, 2));
    Put8(m, sh::kCardRecordsAsked, Pick(r, 2));
    m.PokeWord(sh::kCardOp, Chance(r, 2) ? 0x11u : 0x13u);
    m.PokeWord(0x80081098u, 0);
    m.PokeWord(0x8008108Cu, 0);
}

std::vector<OracleCallee> CardSeams() {
    std::vector<OracleCallee> v = Sounds();
    for (uint32_t a : {sh::kCardEnter, sh::kCardLeave, sh::kCardPoll}) v.push_back(OracleCallee{a, 0, 0, {}, 0});
    v.push_back(OracleCallee{sh::kCardFormat, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kCardWrite, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSoundMode, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kJukeboxSet, 2, 0, {}, 0});
    v.push_back(OracleCallee{0x8007F20Cu, 2, 0, {}, 0});
    return v;
}

uint32_t CardScreen(CaseContext& c, uint32_t id) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t s = Screen(m, id);
    MakeCurrent(m, id);
    Pads(c, !c.sample);
    if (!c.sample && Chance(r, 2)) {
        Put8(m, s + 14u, Pick(r, 2));
        Put8(m, s + 15u, 1u + Pick(r, 3));
    }
    return s;
}

void AddRows(std::vector<Row>& rows) {
    rows.push_back(ShellRow(
        "shell_card_machine", "s32 CardMachine(Screen*)  [RASHCDF 0x8006C770 - the card screens' 27 states]", 0x8006C770u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            CardMachineState(c);
            Rng& r = *c.rng;
            const uint32_t s = CardScreen(c, 43u + Pick(r, 4));
            if (Chance(r, 3)) Put16(*c.mem, 0x80099550u + 8u, 15); // the slot list, where most arms are
            a.a[0] = s;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                int32_t r = 0;
                if (!sh::CardMachine(g, k, a.a[0], &r) && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, CardSeams()));
    rows.back().allowBiosCalls = true;

    static const uint32_t kHandlers[4] = {0x8006C06Cu, 0x8006C124u, 0x8006C1DCu, 0x8006C290u};
    static const char* kNames[4] = {"shell_card_screen_43", "shell_card_screen_44", "shell_card_screen_45",
                                    "shell_card_screen_46"};
    for (uint32_t i = 0; i < 4u; ++i) {
        const uint32_t h = kHandlers[i];
        rows.push_back(ShellRow(
            kNames[i], "s32 CardScreenInput(Screen*)  [RASHCDF 0x8006C06C / C124 / C1DC / C290]", h,
            [i](CaseContext& c, Args& a) {
                PlantShell(c);
                CardMachineState(c);
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const uint32_t s = CardScreen(c, 43u + i);
                Put16(m, s + 2u, Chance(r, 3) ? 32767u : 0u);
                m.PokeWord(sh::kCardHold, Chance(r, 3) ? Pick(r, 121) : 0u);
                a.a[0] = s;
            },
            [h](NativeEnv& env, const Args& a) {
                return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                    int32_t r = 0;
                    if (!sh::CardScreenInput(g, k, h, a.a[0], &r) && env.failure.empty()) env.failure = "refused";
                    return static_cast<uint32_t>(r);
                });
            },
            true, CardSeams()));
        rows.back().allowBiosCalls = true;
    }

    rows.push_back(ShellRow(
        "shell_card_load_slot", "s32 LoadSlot(s16 slot)  [RASHCDF 0x8006CE60]", 0x8006CE60u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            CardMachineState(c);
            a.a[0] = Pick(*c.rng, 10);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                int32_t r = 0;
                if (!sh::CardLoadSlot(g, k, static_cast<int32_t>(a.a[0]), &r) && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, CardSeams()));
    rows.push_back(ShellRow(
        "shell_card_save_slot", "void SaveSlot(s16 slot)  [RASHCDF 0x8006CDA4]", 0x8006CDA4u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            CardMachineState(c);
            a.a[0] = Pick(*c.rng, 10);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::CardSaveSlot(g, static_cast<int32_t>(a.a[0]));
                return 0u;
            });
        },
        false));
    struct Leaf {
        const char* name;
        const char* sig;
        uint32_t entry;
        bool v0;
    };
    static const Leaf kLeaves[] = {
        {"shell_card_seed_records", "void SeedRecords(void)  [RASHCDF 0x8006CE24]", 0x8006CE24u, false},
        {"shell_card_restore_records", "s32 RestoreRecords(void)  [RASHCDF 0x8006D10C]", 0x8006D10Cu, true},
        {"shell_card_prev_state", "void CardPrevState(void)  [RASHCDF 0x8006CD48]", 0x8006CD48u, false},
        {"shell_card_mode_label", "s32 ModeLabel(u32 mode)  [RASHCDF 0x80071F70]", 0x80071F70u, true},
    };
    for (const Leaf& l : kLeaves) {
        const uint32_t entry = l.entry;
        rows.push_back(ShellRow(
            l.name, l.sig, entry,
            [](CaseContext& c, Args& a) {
                PlantShell(c);
                CardMachineState(c);
                Rng& r = *c.rng;
                static const uint32_t kModes[] = {1, 4, 8, 0x10, 0x11, 0x18, 0x20, 2, 0};
                a.a[0] = Chance(r, 5) ? r.U32() : kModes[Pick(r, 9)];
            },
            [entry](NativeEnv& env, const Args& a) {
                return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) -> uint32_t {
                    switch (entry) {
                    case 0x8006CE24u: sh::CardSeedRecords(g); return 0u;
                    case 0x8006D10Cu: return static_cast<uint32_t>(sh::CardRestoreRecords(g));
                    case 0x8006CD48u: sh::CardPrevState(g); return 0u;
                    default: return static_cast<uint32_t>(sh::CardModeLabel(a.a[0]));
                    }
                });
            },
            l.v0));
    }
}

// The sound options' two SLUS leaves the product runs natively (shell_sound.h).
void AddSoundLeafRows(std::vector<Row>& rows) {
    rows.push_back(ShellRow(
        "shell_jukebox_set", "void JukeboxSet(u32 track, s32 on)  [SLUS 0x8002490C]", 0x8002490Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 6) ? r.U32() : Pick(r, 22);
            a.a[1] = Chance(r, 4) ? r.U32() : Pick(r, 2);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::JukeboxSet(g, a.a[0], static_cast<int32_t>(a.a[1]));
                return 0u;
            });
        },
        false));
    rows.push_back(ShellRow(
        "shell_sound_mode", "void SoundModeSet(u32 mono)  [SLUS 0x8001EFDC]", 0x8001EFDCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            a.a[0] = Chance(*c.rng, 3) ? c.rng->U32() : Pick(*c.rng, 2);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::SoundModeSet(g, a.a[0]);
                return 0u;
            });
        },
        false));
}

} // namespace cardrows

void AddShellScreenRows(std::vector<Row>& rows); // rows_shell_screens.inc
void AddShellCareerRows(std::vector<Row>& rows); // rows_shell_career.inc

void AddShellRows(std::vector<Row>& rows) {
    using namespace shellrows;

    // ------------------------------------------------------------------ RASHCDF 0x8006C700
    rows.push_back(ShellRow(
        "shell_find_by_code", "s16 FindByCode(Screen*, s16 code)  [RASHCDF 0x8006C700]", 0x8006C700u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, c.sample ? 4u : PickScreenId(r, m));
            if (!c.sample) ShuffleWidgets(c, s);
            const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
            int32_t code = static_cast<int32_t>(Pick(r, 81));
            if (n > 0 && Chance(r, 2)) code = Get16(m, m.PeekWord(s + 16u) + 120u * Pick(r, static_cast<uint32_t>(n)) + 18u);
            if (!c.sample && Chance(r, 8)) code = static_cast<int16_t>(r.U32());
            a.a[0] = s;
            a.a[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(code)));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                return static_cast<uint32_t>(sh::FindByCode(g, a.a[0], static_cast<int16_t>(a.a[1])));
            });
        },
        true));

    // ------------------------------------------------------------------ RASHCDF 0x8006C3A4 / 0x8006C558
    for (int dir = 0; dir < 2; ++dir) {
        rows.push_back(ShellRow(
            dir == 0 ? "shell_move_next" : "shell_move_prev",
            dir == 0 ? "void MoveNext(Screen*)  [RASHCDF 0x8006C3A4]" : "void MovePrev(Screen*)  [RASHCDF 0x8006C558]",
            dir == 0 ? 0x8006C3A4u : 0x8006C558u,
            [](CaseContext& c, Args& a) {
                PlantShell(c);
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const uint32_t s = Screen(m, c.sample ? 4u : PickScreenId(r, m));
                if (!c.sample) {
                    ShuffleWidgets(c, s);
                    ShuffleGates(c);
                }
                Select(c, s);
                a.a[0] = s;
            },
            [dir](NativeEnv& env, const Args& a) {
                return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                    const bool ok = dir == 0 ? sh::MoveNext(g, k, a.a[0]) : sh::MovePrev(g, k, a.a[0]);
                    if (!ok && env.failure.empty()) env.failure = "refused";
                    return 0u;
                });
            },
            false, Sounds()));
    }

    // ------------------------------------------------------------------ RASHCDF 0x80064B30
    rows.push_back(ShellRow(
        "shell_bind_slider", "void BindSlider(Screen*, Widget*)  [RASHCDF 0x80064B30]", 0x80064B30u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, c.sample ? 48u : PickScreenId(r, m));
            if (!c.sample) ShuffleWidgets(c, s);
            const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
            const uint32_t w = (n > 0 && !Chance(r, 8)) ? m.PeekWord(s + 16u) + 120u * Pick(r, static_cast<uint32_t>(n)) : 0u;
            for (uint32_t i = 0; i < 7u; ++i)
                if (!c.sample && Chance(r, 2)) m.PokeWord(sh::kSession + 0xD4u + 4u * i, r.U32() & 0xFFu);
            a.a[0] = s;
            a.a[1] = w;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::BindSlider(g, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ RASHCDF 0x8006B03C
    rows.push_back(ShellRow(
        "shell_menu_input", "int MenuInput(Screen*, int padFirst, int padEnd)  [RASHCDF 0x8006B03C]", 0x8006B03Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t id = c.sample ? 4u : PickScreenId(r, m);
            const uint32_t s = Screen(m, id);
            Pads(c, !c.sample);
            if (!c.sample) {
                ShuffleWidgets(c, s);
                ShuffleGates(c);
                MakeCurrent(m, Chance(r, 2) ? id : Pick(r, 59));
                Put16(m, s, Get16(m, s) ^ (static_cast<uint32_t>(Pick(r, 8)) << 8)); // flags 0x100/0x200/0x400
                Put8(m, sh::kSession + 0x18u, Pick(r, 2));
                Put8(m, m.PeekWord(sh::kGameStatePtr) + 4u, Chance(r, 2) ? 4u : 0x22u);
                Put8(m, sh::kFeMusicRestart, Pick(r, 2));
                Put8(m, sh::kFeMusicOk, Pick(r, 2));
                if (Chance(r, 2)) {
                    const uint32_t slot = Pick(r, 6);
                    const uint32_t d = sh::kSliderDescs + 16u * slot;
                    m.PokeWord(sh::kFeSlider, d);
                    m.PokeWord(sh::kFeSliderDst, Chance(r, 3) ? 0u : 0x800D81ACu + 4u * slot);
                    m.PokeWord(d, Pick(r, 4));
                    m.PokeWord(d + 4u, 4u + Pick(r, 20));
                    Put16(m, d + 8u, 1u + Pick(r, 3));
                    m.PokeWord(d + 12u, Pick(r, 30));
                } else {
                    m.PokeWord(sh::kFeSlider, 0);
                }
            } else {
                MakeCurrent(m, id);
            }
            Select(c, s);
            const int32_t first = static_cast<int8_t>(m.PeekByte(s + 14u));
            const int32_t end = first + static_cast<int8_t>(m.PeekByte(s + 15u));
            a.a[0] = s;
            a.a[1] = static_cast<uint32_t>(c.sample ? first : static_cast<int32_t>(Pick(r, 3)));
            a.a[2] = static_cast<uint32_t>(c.sample ? end : static_cast<int32_t>(Pick(r, 4)));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                int32_t r = 0;
                if (!sh::MenuInput(g, k, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]), &r) &&
                    env.failure.empty())
                    env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, Sounds()));

    // ------------------------------------------------------------------ RASHCDF 0x80069418
    rows.push_back(ShellRow(
        "shell_default_input", "int DefaultScreenInput(Screen*)  [RASHCDF 0x80069418]", 0x80069418u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t id = c.sample ? 4u : PickScreenId(r, m);
            const uint32_t s = Screen(m, id);
            Pads(c, !c.sample);
            if (!c.sample) ShuffleWidgets(c, s);
            MakeCurrent(m, id);
            Select(c, s);
            a.a[0] = s;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                int32_t r = 0;
                if (!sh::DefaultScreenInput(g, k, a.a[0], &r) && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, Sounds()));

    // ------------------------------------------------------------------ RASHCDF 0x8006C354
    rows.push_back(ShellRow(
        "shell_start_race", "int StartRaceInput(Screen*)  [RASHCDF 0x8006C354 - the handover]", 0x8006C354u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            if (!c.sample) {
                Put16(m, sh::kFeMedia, r.U32() & 0xFFFFu);
                Put8(m, sh::kFeHold, r.U32() & 0xFFu);
                m.PokeWord(sh::kIdle, r.U32());
            }
            a.a[0] = Screen(m, 57);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                return static_cast<uint32_t>(sh::StartRaceInput(g, a.a[0]));
            });
        },
        true));

    // ------------------------------------------------------------------ RASHCDF 0x800685BC
    rows.push_back(ShellRow(
        "shell_set_mode", "void SetMode(Widget*)  [RASHCDF 0x800685BC]", 0x800685BCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, c.sample ? 5u : PickScreenId(r, m));
            const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
            const uint32_t w = (n > 0 && !Chance(r, 10)) ? m.PeekWord(s + 16u) + 120u * Pick(r, static_cast<uint32_t>(n)) : 0u;
            if (w != 0 && !c.sample) {
                if (Chance(r, 2)) Put16(m, w + 8u, 11u + Pick(r, 4));
                Put16(m, w + 18u, Pick(r, 40));
            }
            m.PokeWord(sh::kSession, 0x5A5A5A5Au);
            a.a[0] = w;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::SetMode(g, a.a[0]);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ RASHCDF 0x80080A70
    rows.push_back(ShellRow(
        "shell_first_item", "s16 FirstItem(Screen*)  [RASHCDF 0x80080A70]", 0x80080A70u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, PickScreenId(r, m));
            if (!c.sample) ShuffleWidgets(c, s);
            a.a[0] = s;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) { return static_cast<uint32_t>(sh::FirstItem(g, a.a[0])); });
        },
        true));

    // ------------------------------------------------------------------ the chooser leaves
    rows.push_back(ShellRow(
        "shell_chooser_set", "void ChooserSet(Chooser*, int value)  [RASHCDF 0x800630C0]", 0x800630C0u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Rng& r = *c.rng;
            const uint32_t obj = PickChooser(r, *c.mem);
            if (!c.sample) ShuffleChooser(c, obj);
            a.a[0] = obj;
            a.a[1] = Chance(r, 2) ? Pick(r, 40) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(r.U32())));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::ChooserSet(g, a.a[0], static_cast<int32_t>(a.a[1]));
                return 0u;
            });
        },
        false));
    rows.push_back(ShellRow(
        "shell_chooser_first_valid", "int ChooserFirstValid(Chooser*)  [RASHCDF 0x80062D6C]", 0x80062D6Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            const uint32_t obj = PickChooser(*c.rng, *c.mem);
            if (!c.sample) {
                ShuffleChooser(c, obj);
                ShuffleGates(c);
            }
            a.a[0] = obj;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) { return static_cast<uint32_t>(sh::ChooserFirstValid(g, a.a[0])); });
        },
        true));
    rows.push_back(ShellRow(
        "shell_chooser_count", "int ChooserCount(Chooser*)  [RASHCDF 0x800649BC]", 0x800649BCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            const uint32_t obj = PickChooser(*c.rng, *c.mem);
            if (!c.sample) {
                ShuffleChooser(c, obj);
                ShuffleGates(c);
            }
            a.a[0] = obj;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) { return static_cast<uint32_t>(sh::ChooserCount(g, a.a[0])); });
        },
        true));
    for (int dir = 0; dir < 2; ++dir) {
        rows.push_back(ShellRow(
            dir == 0 ? "shell_chooser_prev" : "shell_chooser_next",
            dir == 0 ? "void ChooserPrev(Chooser*)  [RASHCDF 0x8006460C]" : "void ChooserNext(Chooser*)  [RASHCDF 0x800647E4]",
            dir == 0 ? 0x8006460Cu : 0x800647E4u,
            [](CaseContext& c, Args& a) {
                PlantShell(c);
                const uint32_t obj = PickChooser(*c.rng, *c.mem);
                if (!c.sample) {
                    ShuffleChooser(c, obj);
                    ShuffleGates(c);
                }
                a.a[0] = obj;
            },
            [dir](NativeEnv& env, const Args& a) {
                return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                    const bool ok = dir == 0 ? sh::ChooserPrev(g, k, a.a[0]) : sh::ChooserNext(g, k, a.a[0]);
                    if (!ok && env.failure.empty()) env.failure = "refused";
                    return 0u;
                });
            },
            false, Sounds()));
    }
    rows.push_back(ShellRow(
        "shell_chooser_apply", "void ChooserApply(Chooser*)  [RASHCDF 0x8006310C]", 0x8006310Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t obj = Chance(r, 16) ? 0u : PickChooser(r, m);
            if (!c.sample && obj != 0) {
                ShuffleChooser(c, obj);
                Put8(m, obj + 2u, Pick(r, 0x26));
                Put8(m, sh::kFeEdges, r.U32() & 0xFFu);
                Put8(m, 0x800D81E1u, Pick(r, 3));
                Put8(m, sh::kSession + 0x13u, Pick(r, 6));
                Put8(m, sh::kFePort, Pick(r, 4));
                Put8(m, sh::kFeTrack, Pick(r, 18));
                Put8(m, sh::kSession + 0x14u, Pick(r, 2));
            }
            a.a[0] = obj;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                if (!sh::ChooserApply(g, k, a.a[0]) && env.failure.empty()) env.failure = "refused";
                return 0u;
            });
        },
        false, PassSeams()));
    rows.push_back(ShellRow(
        "shell_bind_chooser", "void BindChooser(Screen*, Widget*)  [RASHCDF 0x800680E8]", 0x800680E8u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, c.sample ? 8u : PickScreenId(r, m));
            const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
            const uint32_t w = (n > 0 && !Chance(r, 10)) ? m.PeekWord(s + 16u) + 120u * Pick(r, static_cast<uint32_t>(n)) : 0u;
            if (w != 0 && !c.sample) {
                if (Chance(r, 2)) Put16(m, w + 8u, 13);
                Put16(m, w + 18u, Pick(r, 90));
                Put8(m, 0x800D81E1u, Pick(r, 4));
                Put8(m, 0x800D81DFu, Pick(r, 24));
            }
            a.a[0] = s;
            a.a[1] = w;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::BindChooser(g, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));
    rows.push_back(ShellRow(
        "shell_object_input", "int ObjectInput(Screen*)  [RASHCDF 0x8006B7BC + 0x8006B800's fe+0x15 arms]", 0x8006B7BCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, c.sample ? 8u : PickScreenId(r, m));
            const uint32_t obj = Chance(r, 8) ? 0u : PickChooser(r, m);
            if (!c.sample) {
                ShuffleChooser(c, obj);
                Put8(m, s + 14u, Pick(r, 2));
                Put8(m, s + 15u, Pick(r, 3));
            }
            m.PokeWord(sh::kFeChooser, obj);
            Pads(c, !c.sample);
            a.a[0] = s;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                int32_t r = 0;
                if (!sh::ObjectInput(g, k, a.a[0], &r) && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, Sounds()));

    // ------------------------------------------------------------------ the career readers
    for (int which = 0; which < 2; ++which) {
        rows.push_back(ShellRow(
            which == 0 ? "shell_all_done" : "shell_all_done_missions",
            which == 0 ? "int AllDone(int first, int last)  [RASHCDF 0x8007E724, session+0xF0]"
                       : "int AllDoneMissions(int first, int last)  [RASHCDF 0x8007E770, session+0xFC]",
            which == 0 ? 0x8007E724u : 0x8007E770u,
            [which](CaseContext& c, Args& a) {
                PlantShell(c);
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const uint32_t base = which == 0 ? sh::kProgress : sh::kMissions;
                for (uint32_t b = 0; b < 12u; ++b)
                    Put8(m, base + b, Chance(r, 3) ? 0xFFu : (Chance(r, 2) ? 0u : (r.U32() & 0xFFu)));
                const int32_t span = which == 0 ? 72 : 24;
                const int32_t first = static_cast<int32_t>(Pick(r, static_cast<uint32_t>(span))) - 3;
                const int32_t last = first + static_cast<int32_t>(Pick(r, 12)) - 2;
                a.a[0] = static_cast<uint32_t>(first);
                a.a[1] = static_cast<uint32_t>(last);
            },
            [which](NativeEnv& env, const Args& a) {
                return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                    return static_cast<uint32_t>(which == 0 ? sh::AllDone(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]))
                                                            : sh::AllDoneMissions(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1])));
                });
            },
            true));
    }
    rows.push_back(ShellRow(
        "shell_beats_record", "bool BeatsRecord(int raceId, int value)  [RASHCDF 0x8007E7BC]", 0x8007E7BCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Rng& r = *c.rng;
            a.a[0] = 38u + Pick(r, 30);
            a.a[1] = Chance(r, 2) ? r.U32() : Pick(r, 100000);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                return static_cast<uint32_t>(sh::BeatsRecord(g, static_cast<int32_t>(a.a[0]), a.a[1]));
            });
        },
        true));

    // ------------------------------------------------------------------ the card checksum
    rows.push_back(ShellRow(
        "shell_card_checksum", "void CardChecksum(int slot, int* a, u32* b)  [RASHCDF 0x8005E9D0]", 0x8005E9D0u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t slot = Pick(r, 10);
            const uint32_t rec = m.PeekWord(0x8009954Cu) + 0x854u + 0x1E4u * slot;
            if (!c.sample)
                for (uint32_t i = 0; i < 476u; ++i)
                    if (Chance(r, 3)) Put8(m, rec + i, r.U32() & 0xFFu);
            a.a[0] = slot;
            a.a[1] = c.scratch;
            a.a[2] = c.scratch + 4u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::CardChecksum(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ screens
    rows.push_back(ShellRow(
        "shell_goto_screen", "void GotoScreen(s16 id)  [RASHCDF 0x800809E0]", 0x800809E0u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            a.a[0] = Pick(*c.rng, 59);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::GotoScreen(g, static_cast<int16_t>(a.a[0]));
                return 0u;
            });
        },
        false));
    rows.push_back(ShellRow(
        "shell_change_screen", "void ChangeScreen(void)  [RASHCDF 0x80066EF8, with CareerSetup 0x8006883C]", 0x80066EF8u,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            static const uint32_t kFrom[] = {4, 5, 6, 7, 29, 8, 24, 42};
            const uint32_t cur = c.sample ? 4u : (Chance(r, 2) ? kFrom[Pick(r, 8)] : Pick(r, 59));
            static const uint32_t kTo[] = {5, 7, 8, 24, 38, 27, 30, 32, 34, 36, 29, 42, 4, 53, 54};
            const uint32_t next = Chance(r, 4) ? Pick(r, 59) : kTo[Pick(r, 15)];
            Put16(m, sh::kFeCur, cur);
            Put16(m, sh::kFeNext, Chance(r, 16) ? cur : next);
            for (uint32_t i = 0; i < 59u; ++i) {
                const uint32_t s = Screen(m, i);
                if (s == 0 || c.sample) continue;
                if (Chance(r, 3)) Put16(m, s, Get16(m, s) ^ Pick(r, 8));
                if (Chance(r, 3)) Put16(m, s + 2u, Chance(r, 2) ? 0u : 32767u);
            }
            if (!c.sample) {
                Put8(m, 0x800D81E1u, Pick(r, 3));
                Put8(m, 0x800D81E2u, Pick(r, 3));
            }
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                sh::ChangeScreen(g, k);
                return 0u;
            });
        },
        false));
    rows.push_back(ShellRow(
        "shell_new_game", "void NewGame(int screenId)  [RASHCDF 0x80068688]", 0x80068688u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Rng& r = *c.rng;
            static const uint32_t kIds[] = {0x18, 0x26, 8, 4, 24, 38, 40};
            a.a[0] = kIds[Pick(r, 7)];
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::NewGame(g, static_cast<int32_t>(a.a[0]));
                return 0u;
            });
        },
        false));
    rows.push_back(ShellRow(
        "shell_career_setup", "void CareerSetup(int screenId)  [RASHCDF 0x8006883C]", 0x8006883Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            static const uint32_t kIds[] = {7, 8, 0x18, 0x26, 0x1B, 0x1E, 0x20, 0x22, 0x24, 4, 42};
            a.a[0] = kIds[Pick(r, 11)];
            if (!c.sample) {
                Put8(m, 0x800D81E1u, Pick(r, 3));
                Put8(m, 0x800D81E2u, Pick(r, 3));
                Put8(m, 0x800D81DFu, Pick(r, 21));
            }
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::CareerSetup(g, static_cast<int32_t>(a.a[0]));
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ per-screen handlers
    rows.push_back(ShellRow(
        "shell_movie_input", "int MovieInput(Screen*)  [RASHCDF 0x8006A8FC]", 0x8006A8FCu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            static const uint32_t kMovies[] = {1, 2, 6, 11, 12, 15, 16, 19, 20, 22, 58};
            const uint32_t id = kMovies[Pick(r, 11)];
            const uint32_t s = Screen(m, id);
            MakeCurrent(m, id);
            Pads(c, !c.sample);
            Put16(m, s + 2u, Chance(r, 3) ? 32767u : 0u);
            Put16(m, sh::kFeMedia, Pick(r, 8));
            a.a[0] = s;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                bool ok = true;
                const int32_t r = sh::MovieInput(g, k, a.a[0], &ok);
                if (!ok && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, Sounds()));
    rows.push_back(ShellRow(
        "shell_splash_input", "int SplashInput(Screen*)  [RASHCDF 0x8006AC80]", 0x8006AC80u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, 3);
            MakeCurrent(m, 3);
            Put16(m, s + 2u, Chance(r, 3) ? 32767u : 0u);
            const uint32_t now = r.U32() & 0x7FFFu;
            m.PokeWord(sh::kVblankCount, now);
            Put16(m, sh::kFeStamp, now - Pick(r, 600));
            a.a[0] = s;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                bool ok = true;
                const int32_t r = sh::SplashInput(g, k, a.a[0], &ok);
                if (!ok && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, PassSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8006738C
    rows.push_back(ShellRow(
        "shell_chooser_pass", "void ChooserPass(Player*)  [RASHCDF 0x8006738C, with 0x80068448 and 0x80080ACC]", 0x8006738Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            static const uint32_t kModes[] = {1, 4, 8, 0x10, 0x11, 0x18, 0x20, 0x20};
            static const uint32_t kIds[] = {4, 5, 7, 8, 24, 27, 30, 32, 34, 36, 38, 42, 48};
            const uint32_t id = c.sample ? 4u : kIds[Pick(r, 13)];
            MakeCurrent(m, id);
            Select(c, Screen(m, id));
            if (!c.sample) {
                m.PokeWord(sh::kSession, kModes[Pick(r, 8)]);
                Put8(m, sh::kSession + 4u, Pick(r, 7));
                Put8(m, 0x800D81E1u, Pick(r, 3));
                Put8(m, 0x800D81DFu, Pick(r, 21));
                Put8(m, 0x800D81EDu, Pick(r, 3));
                Put8(m, 0x800D8211u, Pick(r, 3));
                for (uint32_t p = 0; p < 4u; ++p) Put8(m, sh::kPlayers + 36u * p + 8u, Pick(r, 6));
                ShuffleGates(c);
                for (int t = 0; t < 4; ++t) ShuffleChooser(c, PickChooser(r, m));
                ShuffleWidgets(c, Screen(m, id));
                Select(c, Screen(m, id));
                m.PokeWord(sh::kFeChooser, Chance(r, 2) ? PickChooser(r, m) : 0u);
            }
            a.a[0] = sh::kPlayers;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                if (!sh::ChooserPass(g, k, a.a[0]) && env.failure.empty()) env.failure = "refused";
                return 0u;
            });
        },
        false, PassSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x800667E4
    rows.push_back(ShellRow(
        "shell_input_pass", "void InputPass(void)  [RASHCDF 0x800667E4 - the shell's per-frame input]", 0x800667E4u,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            static const uint32_t kIds[] = {4, 5, 7, 8, 24, 27, 29, 42, 48, 3, 1, 57, 47, 30, 26};
            const uint32_t id = c.sample ? 4u : kIds[Pick(r, 15)];
            MakeCurrent(m, id);
            Pads(c, !c.sample);
            const uint32_t s = Screen(m, id);
            Select(c, s);
            if (!c.sample) {
                Put8(m, sh::kFeMusicOk, Pick(r, 2));
                Put8(m, sh::kFeMusicOn, Pick(r, 2));
                // Bit 0 only: bit 2 ("skip asked") makes SkipMovie 0x8006FED4 stop the MDEC, which
                // only exists while a movie plays - and the bench has no MDEC (named bound).
                Put16(m, sh::kFeMedia, Pick(r, 2));
                m.PokeWord(sh::kIdle, Chance(r, 4) ? 1801u + Pick(r, 10) : Pick(r, 100));
                m.PokeWord(sh::kPads + 4u, Pick(r, 2));
            }
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                if (!sh::InputPass(g, k) && env.failure.empty()) env.failure = "refused";
                return 0u;
            });
        },
        false, PassSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8007F37C
    rows.push_back(ShellRow(
        "shell_commit", "void CommitSelection(void)  [RASHCDF 0x8007F37C - the menu into game_state]", 0x8007F37Cu,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            static const uint32_t kModes[] = {1, 4, 8, 0x10, 0x11, 0x18, 0x20, 0x20, 2};
            if (!c.sample) {
                m.PokeWord(sh::kSession, kModes[Pick(r, 9)]);
                Put8(m, sh::kSession + 0x04u, Pick(r, 6));
                Put8(m, sh::kSession + 0x05u, r.U32() & 0xFFu);
                Put8(m, sh::kSession + 0x08u, Pick(r, 64));
                Put8(m, sh::kSession + 0x09u, Pick(r, 2));
                Put8(m, sh::kSession + 0x0Au, Pick(r, 2));
                Put8(m, sh::kSession + 0x0Bu, Pick(r, 2));
                Put8(m, sh::kSession + 0x13u, Pick(r, 6));
                Put8(m, 0x800D81DFu, Pick(r, 21));
                Put8(m, 0x800D81DEu, Pick(r, 21));
                Put8(m, 0x800D81E1u, Pick(r, 3));
                m.PokeWord(m.PeekWord(sh::kGameStatePtr) + 0x34u, 1u + Pick(r, 4));
                m.PokeWord(sh::kDemo, Chance(r, 8) ? 1u : 0u);
                m.PokeWord(sh::kDebugFlags, r.U32() & 0x20u);
                for (uint32_t i = 0; i < 0x80u; i += 4) m.PokeWord(sh::kSession + 0x40u + i, r.U32());
                for (uint32_t i = 0; i < 216u; i += 4)
                    if (Chance(r, 2)) m.PokeWord(sh::kPlayers + i, r.U32() & 0x03030303u);
            }
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                if (!sh::CommitSelection(g, k) && env.failure.empty()) env.failure = "refused";
                return 0u;
            });
        },
        false, {OracleCallee{sh::kPadInstall, 1, 0, {}, 0}}));
    AddShellScreenRows(rows);
    AddShellCareerRows(rows);
    cardrows::AddRows(rows); // the card screens (shell_memcard.h)
    cardrows::AddSoundLeafRows(rows);
    shelltext::AddRows(rows);
}
