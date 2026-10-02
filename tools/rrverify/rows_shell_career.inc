// Bench rows for the front end's post-race half: src\game\shell\shell_career.{h,cpp}, RASHCDF.BIN
// (sha1 a3fec4b4e9292c358d0f6dc529843f5d8f25924a). Included by verify_physics.cpp right after
// rows_shell.inc, inside the same anonymous namespace, so shellrows:: (PlantShell, ShellRow, Run,
// Pick, Chance, Put8, Put16, Screen, ...) is visible here.
//
// The port's header declares functions in rr::shell, so it has to be included at namespace scope:
// the anonymous namespace is closed around the #include and reopened (an unnamed namespace is the
// same namespace every time it is reopened within a translation unit).
} // namespace
#include "game/shell/shell_career.h"
namespace {
//
// THE MACHINE is rows_shell.inc's: a capture of the running shell planted over the bench
// snapshot. What is compared is, as for every row, the whole 2 MiB outside the stack window plus the
// scratchpad, and v0 where the function returns something.
//
// INPUT FAMILIES.
//   * dump-derived: the capture as it is (a fresh career, mode 32) with only the race's result code
//     player[0]+0x20 and the root counter the LCG is seeded from drawn;
//   * randomised: every field the dispatchers branch on or copy - the result code, the session's
//     venue / race id / mission id / win count / step counter / flags / result counters, the gang,
//     player+0x14, the combat counters player+0x18..+0x1F, the race's rider identities (which the
//     dispatchers copy back into player[0], so they are where the owned mask +0x0C, the current item
//     +0x0E/+0x0F and the level nibbles +0x10 come from), the two progress bitmaps, the Time Trial
//     round fields and times, the attract flag.
// Named bounds, each the range the game's own data keeps:
//   * the venue session+0x04 is 0..5 - every table the career dispatcher indexes by it has six
//     entries (rules.md 7.4), and CareerSetup / the venue steps only ever store 0..5;
//   * the gang player+0x09 is 0 or 1 in a career (CareerSetup writes 0/1; the item tables
//     0x8008A124.. hold two 24-byte rows);
//   * the race id session+0x08 is 0..64 (the 65-bit bitmap session+0xF0), the mission id
//     session+0x07 0..17 (the 18-bit bitmap session+0xFC, and 0x8007E7EC's 18-entry table);
//   * the Time Trial player index session+0x13 is 0..5 (six player records), its round total
//     session+0x12 1..6;
//   * a screen id (session+0x0C, the attract path's) is 0..58, the music track fe+0x0E 0..17
//     (session+0xC0 has 18 bytes).
//
// THE SEAMS: res_show RASHCDF 0x800782B0 (record, mode) - the result panel's resource loader - and
// GetRCnt SLUS 0x80043F00 (spec), whose root-counter read seeds the LCG (the bench's root counter is
// constant per case and drawn by the setup, so the seed is varied too).
//
// THE RESUME REGION. 0x8007FF4C is not callable as it stands: it runs the resource loader
// 0x80080544, the fe reset 0x800665E8 and the screen-table build 0x800806D0 first and the shell
// loop last. Its rows call it at its own entry, with its own prologue and epilogue, and bound the
// region by patching code in BOTH machines identically (the patched words are the same on both
// sides and neither run writes them):
//   * the three init `jal`s (0x8007FF5C, 0x8007FF64, 0x8007FF6C) become nops - their delay slots,
//     one of which is the prologue's `sw s0,16(sp)`, still execute;
//   * the exit becomes `j 0x80080208` (the epilogue, which restores ra/s0..s2 and returns): at
//     0x80080184 (the call of 0x80063908, with `addu v0,s0,zero` in its delay slot so the hub id is
//     returned) for the dispatch alone, and at 0x800801F0 (the call of the shell loop; its delay
//     slot is already a nop) for dispatch + chooser sync + GotoScreen + the parent walk.
// So everything the guest executes is the original's own code from 0x8007FF74 to that exit.
// (A bare `jr ra` at the exit does not work: the dispatchers' `jal`s have overwritten ra.)

namespace shellrows_career {

using namespace shellrows;

constexpr uint32_t kNop = 0u;
constexpr uint32_t kJumpEpilogue = 0x08020082u; // j 0x80080208

// The region's bounds, as the header describes them.
void Bound(Memory& m, uint32_t exit) {
    for (uint32_t a : {0x8007FF5Cu, 0x8007FF64u, 0x8007FF6Cu}) m.PokeWord(a, kNop);
    m.PokeWord(exit, kJumpEpilogue);
}
constexpr uint32_t kMoveV0S0 = 0x02001021u; // addu v0,s0,zero

std::vector<OracleCallee> Seams() {
    return {OracleCallee{sh::kResShow, 2, 0, {}, 0}, OracleCallee{sh::kGetRCnt, 1, 0, {}, 0}};
}

uint32_t Code(Rng& r) {
    static const uint32_t kCodes[] = {1, 2, 3, 4, 5, 8, 12, 15, 16, 0, 17, 248, 249, 250, 251, 252, 253, 254, 255};
    if (Chance(r, 16)) return r.U32();
    return kCodes[Pick(r, 19)];
}

// res_show's records (the 37 x 32-byte table 0x800897E4, frontend.md 7.3) that the dispatchers name
// - 9 (miscres.str) and 20..29 (the result screens) - marked resident, flags bit 0x4, the state the
// capture already has record 22 (win.str) in. A NAMED BOUND OF THE BENCH, not of the game: for a
// record not resident res_show runs the CD loader, which enters a critical section by `syscall`
// (SLUS 0x80043DA8) and the interpreter has no exception path for that. With the record resident
// res_show returns at 0x80078304; the call itself - its address, both arguments and its place in
// the sequence - is still compared, which is all the port owes a presentation seam.
void Resident(Memory& m) {
    for (uint32_t rec : {9u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u}) {
        const uint32_t a = 0x800897E4u + 32u * rec;
        Put16(m, a, Get16(m, a) | 4u);
    }
}

// Everything the career / Five-O / Side Car dispatchers read, drawn inside the named bounds.
void Career(CaseContext& c, bool random) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Resident(m);
    *c.rootCounter = r.U32() & 0xFFFFu;
    m.PokeWord(sh::kPlayers + 0x20u, Code(r));
    m.PokeWord(sh::kPlayers + 0x24u + 0x20u, Code(r));
    if (!random) return;
    Put8(m, sh::kSession + 0x04u, Pick(r, 6));
    Put8(m, sh::kSession + 0x05u, r.U32() & 0xFFu);
    Put8(m, sh::kSession + 0x07u, Pick(r, 18));
    Put8(m, sh::kSession + 0x08u, Pick(r, 65));
    Put8(m, sh::kSession + 0x11u, Chance(r, 3) ? (Chance(r, 2) ? 3u : 6u) : Pick(r, 10));
    Put8(m, sh::kSession + 0x15u, Chance(r, 4) ? (r.U32() & 0xFFu) : Pick(r, 9));
    for (uint32_t off = 0x20; off < 0x2Cu; off += 2) Put16(m, sh::kSession + off, Pick(r, 400));
    Put8(m, sh::kPlayers + 0x09u, Pick(r, 2));
    Put8(m, sh::kPlayers + 0x14u, Chance(r, 4) ? (r.U32() & 0xFFu) : Pick(r, 14));
    // Combat counters: small most of the time, so the total bonus falls across the thresholds.
    for (uint32_t p = 0; p < 2u; ++p) {
        const uint32_t base = sh::kPlayers + 0x24u * p;
        const uint32_t span = Chance(r, 3) ? 256u : (Chance(r, 2) ? 4u : 24u);
        for (uint32_t off = 0x18; off < 0x20u; ++off) Put8(m, base + off, Pick(r, span));
    }
    // The race's rider identities (0x48-byte records): rider 0/1 go back into player[0]/[1].
    for (uint32_t i = 0; i < 18u; ++i) {
        const uint32_t rider = sh::kRiders + 0x48u * i;
        Put16(m, rider + 0x2Cu, Chance(r, 3) ? (r.U32() & 0xFFFFu) : (r.U32() & 0x1FFu));
        Put8(m, rider + 0x2Eu, Pick(r, 10));
        Put8(m, rider + 0x2Fu, Pick(r, 16));
        m.PokeWord(rider + 0x30u, Chance(r, 3) ? 0u : r.U32());
    }
    // The bitmaps: whole bytes set often, so the venue ranges are complete in some cases.
    for (uint32_t b = 0; b < 9u; ++b) Put8(m, sh::kProgress + b, Chance(r, 2) ? 0xFFu : (r.U32() & 0xFFu));
    for (uint32_t b = 0; b < 3u; ++b) Put8(m, sh::kMissions + b, Chance(r, 2) ? 0xFFu : (r.U32() & 0xFFu));
}

// The venue step of the career dispatcher: each venue's range complete or not, a win half the time.
void VenueFocus(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Career(c, true);
    static const uint32_t kWins[] = {1, 2, 3, 248, 249, 251};
    m.PokeWord(sh::kPlayers + 0x20u, Chance(r, 2) ? kWins[Pick(r, 6)] : Code(r));
    const uint32_t venue = Pick(r, 6);
    Put8(m, sh::kSession + 0x04u, venue);
    static const uint32_t kFirst[] = {1, 28, 10, 29, 19, 30}, kLast[] = {9, 28, 18, 29, 27, 30};
    const bool complete = Chance(r, 2);
    for (uint32_t b = kFirst[venue]; b <= kLast[venue]; ++b) {
        const uint32_t a = sh::kProgress + (b >> 3);
        const uint32_t bit = 1u << (b & 7u);
        const uint32_t cur = m.PeekByte(a);
        Put8(m, a, complete ? (cur | bit) : cur);
    }
    if (!complete) { // one bit of the range missing
        const uint32_t b = kFirst[venue] + Pick(r, kLast[venue] - kFirst[venue] + 1u);
        const uint32_t a = sh::kProgress + (b >> 3);
        Put8(m, a, m.PeekByte(a) & ~(1u << (b & 7u)));
    }
    // The race just won is often the range's own (it is set before the test).
    if (Chance(r, 2)) Put8(m, sh::kSession + 0x08u, kFirst[venue] + Pick(r, kLast[venue] - kFirst[venue] + 1u));
}

// The resume region's inputs: the mode, the attract flag, the Time Trial fields.
void Resume(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Career(c, !c.sample);
    if (c.sample) return;
    static const uint32_t kModes[] = {1, 4, 8, 16, 17, 24, 32, 32, 4, 2, 0};
    m.PokeWord(sh::kSession, Chance(r, 16) ? r.U32() : kModes[Pick(r, 11)]);
    m.PokeWord(sh::kDemo, Chance(r, 6) ? 1u : 0u);
    Put8(m, sh::kSession + 0x0Cu, Pick(r, 59));
    Put8(m, sh::kSession + 0x0Du, Pick(r, 8));
    Put8(m, sh::kSession + 0x0Fu, 30u + Pick(r, 10));
    Put8(m, sh::kSession + 0x06u, Pick(r, 4));
    Put8(m, 0x800D81DFu, Pick(r, 21));
    Put8(m, sh::kSession + 0x16u, Pick(r, 2));
    Put8(m, sh::kSession + 0x12u, 1u + Pick(r, 6));
    Put8(m, sh::kSession + 0x13u, Pick(r, 6));
    Put8(m, sh::kSession + 0x1Au, r.U32() & 0xFFu);
    if (m.PeekWord(sh::kSession) == 4u) Put8(m, sh::kSession + 0x08u, 56u + Pick(r, 8)); // the TT records
    for (uint32_t p = 0; p < 6u; ++p) m.PokeWord(sh::kPlayers + 36u * p, Chance(r, 2) ? Pick(r, 200000) : r.U32());
    Put8(m, sh::kFe + 0x0Eu, Pick(r, 18));
}

// The chooser sync's inputs: every field one of its arms reads.
void Sync(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (c.sample) return;
    for (uint32_t off : {0x04u, 0x07u, 0x08u, 0x09u, 0x0Au, 0x0Bu, 0x14u, 0x17u, 0x19u})
        Put8(m, sh::kSession + off, Pick(r, 24));
    Put8(m, sh::kSession + 0x13u, Pick(r, 6));
    for (uint32_t off : {0x07u, 0x08u, 0x09u, 0x0Au, 0x15u, 0x17u, 0x2Bu, 0x39u})
        Put8(m, sh::kPlayers + off, Chance(r, 4) ? (r.U32() & 0xFFu) : Pick(r, 24));
    for (uint32_t p = 0; p < 6u; ++p) Put8(m, sh::kPlayers + 36u * p + 7u, Pick(r, 24));
    Put8(m, sh::kFe + 0x0Eu, Pick(r, 18));
    Put8(m, sh::kFe + 0x19u, Pick(r, 8));
    for (uint32_t t = 0; t < 18u; ++t) Put8(m, sh::kSession + 0xC0u + t, Pick(r, 2));
    for (int t = 0; t < 6; ++t) ShuffleChooser(c, PickChooser(r, m));
}

// res_show reaches a BIOS string function through the SLUS stub 0x800448F4 (A-vector), so the rows
// that reach it run the kernel's own code on both machines (Row::allowBiosCalls).
Row WithBios(Row r) {
    r.allowBiosCalls = true;
    return r;
}

template <class F>
Row DispatcherRow(const char* name, const char* sig, uint32_t entry, uint32_t frame,
                  std::function<void(CaseContext&, Args&)> setup, F port) {
    return WithBios(ShellRow(
        name, sig, entry, std::move(setup),
        [port](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                bool ok = true;
                const int32_t v = port(g, k, &ok);
                if (!ok && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(v);
            });
        },
        true, Seams(), frame));
}

} // namespace shellrows_career

void AddShellCareerRows(std::vector<Row>& rows) {
    using namespace shellrows;
    using namespace shellrows_career;

    // ------------------------------------------------------------------ RASHCDF 0x8007BB34
    rows.push_back(DispatcherRow(
        "shell_career_result", "int CareerResult(void)  [RASHCDF 0x8007BB34, mode 32, with its venue step 0x8007D4A0]",
        0x8007BB34u, 56,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Career(c, !c.sample);
        },
        [](sh::GuestRam& g, sh::ShellCallees& k, bool* ok) { return sh::CareerResult(g, k, ok); }));
    rows.push_back(DispatcherRow(
        "shell_career_venue", "int CareerResult(void)  [RASHCDF 0x8007BB34 - its venue step 0x8007D4A0's six arms]",
        0x8007BB34u, 56,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            VenueFocus(c);
        },
        [](sh::GuestRam& g, sh::ShellCallees& k, bool* ok) { return sh::CareerResult(g, k, ok); }));

    // ------------------------------------------------------------------ RASHCDF 0x8007E2C8 / 0x8007DDA8
    rows.push_back(DispatcherRow(
        "shell_fiveo_result", "int FiveOResult(void)  [RASHCDF 0x8007E2C8, mode 1]", 0x8007E2C8u, 40,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Career(c, !c.sample);
            if (!c.sample && Chance(*c.rng, 2)) { // the three mission venues' ranges complete or not
                static const uint32_t kVenues[] = {0, 2, 4};
                const uint32_t v = kVenues[Pick(*c.rng, 3)];
                Put8(*c.mem, sh::kSession + 0x04u, v);
                if (Chance(*c.rng, 2)) {
                    for (uint32_t b = 3u * v; b < 3u * v + 6u; ++b) {
                        const uint32_t a = sh::kMissions + (b >> 3);
                        Put8(*c.mem, a, c.mem->PeekByte(a) | (1u << (b & 7u)));
                    }
                }
            }
        },
        [](sh::GuestRam& g, sh::ShellCallees& k, bool* ok) { return sh::FiveOResult(g, k, ok); }));
    rows.push_back(DispatcherRow(
        "shell_sidecar_result", "int SideCarResult(void)  [RASHCDF 0x8007DDA8, mode 8]", 0x8007DDA8u, 40,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Career(c, !c.sample);
        },
        [](sh::GuestRam& g, sh::ShellCallees& k, bool* ok) { return sh::SideCarResult(g, k, ok); }));

    // ------------------------------------------------------------------ RASHCDF 0x8007E7EC
    rows.push_back(ShellRow(
        "shell_odd_mission", "int OddMission(int id)  [RASHCDF 0x8007E7EC]", 0x8007E7ECu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            a.a[0] = Chance(*c.rng, 4) ? c.rng->U32() : Pick(*c.rng, 24);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam&, sh::ShellCallees&) {
                return static_cast<uint32_t>(sh::OddMission(static_cast<int32_t>(a.a[0])));
            });
        },
        true));

    // ------------------------------------------------------------------ RASHCDF 0x8007FD20
    rows.push_back(ShellRow(
        "shell_short_commit", "void ShortCommit(void)  [RASHCDF 0x8007FD20 - the attract path's commit]", 0x8007FD20u,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            m.PokeWord(sh::kDemo, 1);
            m.PokeWord(sh::kSession, r.U32() & 0x3Fu);
            for (uint32_t off : {0x04u, 0x06u, 0x08u, 0x0Du}) Put8(m, sh::kSession + off, r.U32() & 0xFFu);
            Put8(m, sh::kSession + 0x0Cu, Pick(r, 59));
            Put8(m, 0x800D81DFu, r.U32() & 0xFFu);
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::ShortCommit(g);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ RASHCDF 0x80063908
    rows.push_back(ShellRow(
        "shell_chooser_sync", "void ChooserSync(void)  [RASHCDF 0x80063908]", 0x80063908u,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Sync(c);
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees&) {
                sh::ChooserSync(g);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ RASHCDF 0x8007FF74..0x80080180
    rows.push_back(WithBios(ShellRow(
        "shell_resume_dispatch", "int ResumeDispatch(void)  [RASHCDF 0x8007FF4C, its region 0x8007FF74..0x80080180]", 0x8007FF4Cu,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Resume(c);
            Bound(*c.mem, 0x80080184u);
            c.mem->PokeWord(0x80080188u, kMoveV0S0);
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                bool ok = true;
                const int32_t hub = sh::ResumeDispatch(g, k, &ok);
                if (!ok && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(hub);
            });
        },
        true, Seams())));
    rows.push_back(WithBios(ShellRow(
        "shell_resume_show",
        "void Resume(void)  [RASHCDF 0x8007FF4C, its region 0x8007FF74..0x800801EC: dispatch, 0x80063908, GotoScreen, the parent walk]",
        0x8007FF4Cu,
        [](CaseContext& c, Args&) {
            PlantShell(c);
            Resume(c);
            Bound(*c.mem, 0x800801F0u);
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                bool ok = true;
                const int32_t hub = sh::ResumeDispatch(g, k, &ok);
                if (!ok && env.failure.empty()) env.failure = "refused";
                sh::ChooserSync(g);
                sh::ResumeShow(g, hub);
                return 0u;
            });
        },
        false, Seams())));
}
