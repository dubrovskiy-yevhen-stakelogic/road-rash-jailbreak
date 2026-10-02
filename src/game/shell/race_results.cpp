// The race's results scene (race_results.h), ported as layout from our reading of RASHCDG.BIN.
#include "game/shell/race_results.h"

#include "game/shell/shell_arena.h"

#include <cstdio>

namespace rr::shell {

namespace {

constexpr uint32_t kBikePool = 0x8005B3A0;   // RASHCDG: the 1096-byte bike records
constexpr uint32_t kBikeCount = 0x8005B1F8;
constexpr uint32_t kBikeStride = 0x448;
constexpr uint32_t kColourDone = 0x8005AF5C; // the "Busted! / Wrecked!" row's cycling colour
constexpr uint32_t kColourRow = 0x8005AF58;  // the player's row's cycling colour
constexpr uint32_t kQuitFlag = 0x8005AF4C;   // set when the race was left from the pause menu
constexpr uint32_t kTop3Init = 0x8005BCE0;   // u32[3], 0xE0 = no rider
constexpr uint32_t kFmtPlace = 0x8005BCEC;   // "%d"
constexpr uint32_t kFmtTime = 0x8005BD48;    // "%02d:%02d:%02d"
constexpr uint32_t kFmtTwo = 0x8005BD58;     // "%s%s"
constexpr uint32_t kFmtCatch = 0x8005BD04;   // Five-O: the capture line
constexpr uint32_t kFmtQuota = 0x8005BD1C;   // Five-O: the quota line
constexpr uint32_t kFmtOne = 0x8005BD3C;     // Five-O: one quip
constexpr uint32_t kQuipDone = 0x800CCC10, kQuipPick = 0x800CCC14;
constexpr uint32_t kPlayerRecords = 0x800D81D8;

struct Ctx {
    GuestRam& g;
    const std::vector<std::string>& strings;
    std::vector<TextCall>& out;
    std::string S(uint32_t id) const { return id < strings.size() ? strings[id] : std::string(); }
    std::string CStr(uint32_t p) {
        std::string s;
        for (uint32_t i = 0; i < 256u; ++i) {
            const char c = static_cast<char>(g.U8(p + i));
            if (c == 0) break;
            s.push_back(c);
        }
        return s;
    }
    void Text(const std::string& text, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb, int32_t just) {
        TextCall t;
        t.font = 4;
        t.text = text;
        t.x = static_cast<int16_t>(x);
        t.y = static_cast<int16_t>(y);
        t.w = static_cast<int16_t>(w);
        t.h = static_cast<int16_t>(h);
        t.rgb = rgb;
        t.just = just;
        out.push_back(std::move(t));
    }
    uint32_t Gs() { return g.U32(0x8005B2F8u); }
    uint32_t Bike(uint32_t i) { return g.U32(kBikePool) + kBikeStride * i; }
    uint32_t Def(uint32_t bike) { return g.U32(bike + 0x43Cu); }
    bool Human(uint32_t bike) { return g.U16(bike + 0xACu) < g.U32(Gs() + 0x30u); }
    // RASHCDG 0x800C84C0: player[k]+0x00 = the rider's stamp, +0x20 = its place / result code.
    void Commit(uint32_t bike, uint32_t k) {
        g.W32(kPlayerRecords + 36u * k, g.U32(Def(bike) + 0x28u));
        g.W32(kPlayerRecords + 36u * k + 0x20u, g.U8(Def(bike) + 0x27u));
    }
    std::string Fmt(uint32_t fmt, int32_t a, int32_t b = 0, int32_t c = 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), CStr(fmt).c_str(), a, b, c);
        return buf;
    }
    // RASHCDG 0x800C83D8: ticks (300 a second) as minutes, seconds, hundredths.
    void Time(int32_t t, int32_t just, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
        Text(Fmt(kFmtTime, t / 18000, t / 300 - (t / 18000) * 60, t / 3 - (t / 300) * 100), x, y, w, h, rgb, just);
    }
    // RASHCDG 0x800C5B5C: the column headings Place / Name / Finish Time, each a third of the box.
    void Headings(int32_t x, int32_t y, int32_t w, int32_t h) {
        if (g.U8(Gs() + 4u) & 0x10u) return;
        const int32_t col = static_cast<int16_t>(w) / 3;
        Text(S(0x61), x, y, col, h, 0x14506E, 2);
        Text(S(0x5F), x + col, y, col, h, 0x14506E, 2);
        Text(S(0x60), x + 2 * col, y, col, h, 0x14506E, 2);
    }
    // RASHCDG 0x800C5DF0: the player's row (cycling colour) and, when the player finished, the
    // For / Against tally of Hits, TKOs and Steals from the player record's counters +0x18..+0x1F.
    void PlayerRow(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bike) {
        const int32_t col = static_cast<int16_t>(w) / 3;
        const uint32_t def = Def(bike);
        const uint32_t r = g.U8(def + 0x27u);
        const bool gone = static_cast<uint8_t>(r + 2u) < 2u; // 0xFE busted, 0xFF wrecked / quit
        uint32_t rgb;
        if (gone) {
            rgb = g.U32(kColourDone);
            Text(S(r - 0xE2u), x, y, col, h, rgb, 2);
            Text(S(r - 0xE2u), x + 2 * col, y, col, h, rgb, 2);
        } else {
            rgb = g.U32(kColourRow);
            Text(Fmt(kFmtPlace, static_cast<int32_t>(r)), x, y, col, h, rgb, 2);
            Time(g.S32(def + 0x28u), 2, x + 2 * col, y, col, h, rgb);
        }
        const uint32_t name = (g.U8(Gs() + 4u) & 8u) ? 0xA9u : g.U8(def + 0x26u);
        Text(S(name), x + col, y, col, h, rgb, 2);
        if (gone) return;
        const uint32_t p = kPlayerRecords;
        int32_t hitsFor = g.U16(p + 0x1Au), hitsAgainst = g.U16(p + 0x1Eu);
        int32_t tkoFor = g.U8(p + 0x19u), tkoAgainst = g.U8(p + 0x1Du);
        int32_t stealFor = g.U8(p + 0x18u), stealAgainst = g.U8(p + 0x1Cu);
        if (g.U8(Gs() + 4u) & 8u) { // co-op: the two players' counters summed (0x800D8238..)
            hitsFor += g.U16(0x800D823Au);
            hitsAgainst += g.U16(0x800D823Eu);
            tkoFor += g.U8(0x800D8239u);
            tkoAgainst += g.U8(0x800D823Du);
            stealFor += g.U8(0x800D8238u);
            stealAgainst += g.U8(0x800D823Cu);
        }
        Text(S(0xB3), x, 0x83, col, h, 0x14506E, 2);
        Text(S(0xB4), x + 2 * col, 0x83, col, h, 0x14506E, 2);
        const int32_t rows[3][3] = {{0x92, hitsFor, hitsAgainst}, {0xA1, tkoFor, tkoAgainst}, {0xB0, stealFor, stealAgainst}};
        const uint32_t labels[3] = {0xAF, 0xB0, 0xB1};
        for (int i = 0; i < 3; ++i) {
            Text(Fmt(kFmtPlace, rows[i][1]), x, rows[i][0], col, h, 0x505A50, 2);
            Text(S(labels[i]), x + col, rows[i][0], col, h, 0x14506E, 2);
            Text(Fmt(kFmtPlace, rows[i][2]), x + 2 * col, rows[i][0], col, h, 0x505A50, 2);
        }
    }
    // RASHCDG 0x800C5C68: a rider's row - place, name, finish time - or the player's.
    void Row(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bike, uint32_t rgb) {
        if (Human(bike)) {
            PlayerRow(x, y, w, h, bike);
            return;
        }
        const int32_t col = static_cast<int16_t>(w) / 3;
        const uint32_t def = Def(bike);
        Text(Fmt(kFmtPlace, g.U8(def + 0x27u)), x, y, col, h, rgb, 2);
        Text(S(g.U8(def + 0x26u)), x + col, y, col, h, rgb, 2);
        Time(g.S32(def + 0x28u), 2, x + 2 * col, y, col, h, rgb);
    }
    // RASHCDG 0x800C6634: the top three, then the player's own row when the player is not among them.
    void Standings(const uint32_t top3[3], int32_t x, int32_t y, int32_t w, int32_t h) {
        Headings(x, y + 15, w, h);
        bool human = false;
        for (int i = 0; i < 3; ++i) {
            if (top3[i] == 0xE0u) continue;
            const uint32_t bike = Bike(top3[i]);
            if (Human(bike)) {
                human = true;
                Commit(bike, 0);
            }
            Row(x, y + 15 + static_cast<int32_t>(g.U8(Def(bike) + 0x27u)) * 15, w, h, bike, 0x505A50);
        }
        if (human) return;
        const uint32_t bike = Bike(0);
        Commit(bike, 0);
        int32_t down = 0x3C;
        if (g.U8(kQuitFlag) != 0) {
            g.W8(kQuitFlag, 0);
            g.W8(Def(bike) + 0x27u, 0xFF);
            Commit(bike, 0);
            down = 0x4B;
        }
        PlayerRow(x, y + 15 + down, w, h, bike);
    }
    // RASHCDG 0x800C84F8: the three story missions (race type '$' gauntlet, '!' donut patrol,
    // ',' the Spaz rescue) - the player's row or the mission's two-line verdict.
    void Mission(int32_t x, int32_t y0, int32_t w, int32_t h, uint32_t mode) {
        uint32_t bike = 0;
        bool found = false;
        for (uint32_t i = 0; i < static_cast<uint32_t>(g.S32(kBikeCount)); ++i)
            if (Human(Bike(i))) bike = Bike(i), found = true;
        if (!found) return; // the original would read through a null record here
        const int32_t y = y0 + 15;
        const int8_t c = static_cast<int8_t>(g.U8(Def(bike) + 0x27u));
        auto two = [&](uint32_t first, uint32_t second, int32_t just) {
            Text(S(first), x, y0 + 0x3C, w, h, 0x14506E, 2);
            Text(S(second), x, y0 + 0x4B, w, h, 0x14506E, just);
        };
        if (static_cast<uint8_t>(c + 2) < 2u) {
            PlayerRow(x, y, w, h, bike);
            if (mode == 2) two(0x7F, 0x80, 2);
        } else if (g.U8(kQuitFlag) != 0) {
            g.W8(kQuitFlag, 0);
            g.W8(Def(bike) + 0x27u, 0xFF);
            Commit(bike, 0);
            PlayerRow(x, y, w, h, bike);
        } else if (mode == 1) {
            if (c == -6) two(0x7B, 0x7C, 2);
            else if (c == -8) two(0x7D, 0x7E, 2);
        } else if (mode == 0) {
            if (c == -5) two(0x77, 0x78, 2);
            else two(0x79, 0x7A, 2);
        } else {
            if (c == -6) {
                if (g.U8(Gs() + 0x39u) == 0) Text(S(0x83), x, y0 + 0x4B, w, h, 0x14506E, 2);
                else if (g.U8(Gs() + 0x39u) == 1) two(0x81, 0x82, 2);
            } else if (c == -7) {
                Text(S(g.U8(Def(bike) + 0x26u)) + S(0x84), x, y0 + 0x3C, w, h, 0x14506E, 2);
                Text(S(0x85), x, y0 + 0x4B, w, h, 0x14506E, 2);
            }
        }
        Commit(bike, 0);
    }
    // RASHCDG 0x800C7510: Five-O - the capture / quota line and a quip, or the player's row.
    void FiveO() {
        const uint32_t bike = Bike(0);
        Commit(bike, 0);
        const int8_t c = static_cast<int8_t>(g.U8(Def(bike) + 0x27u));
        if (static_cast<uint8_t>(c + 2) < 2u) {
            PlayerRow(0x43, 0x65, 0xF8, 0x94, bike);
            return;
        }
        if (g.U8(kQuitFlag) != 0) {
            g.W8(kQuitFlag, 0);
            g.W8(Def(bike) + 0x27u, 0xFF);
            Commit(bike, 0);
            PlayerRow(0x43, 0x65, 0xF8, 0x94, bike);
            return;
        }
        const int32_t t = g.S32(Def(bike) + 0x28u);
        const int32_t mm = t / 18000, ss = t / 300 - mm * 60, cc = t / 3 - (t / 300) * 100;
        auto pick = [&](uint32_t n) {
            if (g.U8(kQuipDone) == 0) {
                const uint32_t r = Rand(g);
                g.W8(kQuipDone, 1);
                g.W32(kQuipPick, r % n);
            }
            return g.U32(kQuipPick);
        };
        char buf[256];
        std::string quip;
        if (c == -4) {
            quip = S(0x96 + pick(6));
            std::snprintf(buf, sizeof(buf), CStr(kFmtCatch).c_str(), S(0x95).c_str(), S(0x8E).c_str(), mm, ss, cc);
            Text(buf, 0x43, 0x65, 0xF8, 0x94, 0x14506E, 2);
        } else if (c == -3) {
            const int32_t n = g.U8(Gs() + 7u);
            std::snprintf(buf, sizeof(buf), CStr(kFmtQuota).c_str(), S(0x8C).c_str(), n, S(0x8D).c_str(), n, S(0x8E).c_str(),
                          mm, ss, cc);
            const std::string line = buf;
            char q[256];
            std::snprintf(q, sizeof(q), CStr(kFmtOne).c_str(), S(0x8F + pick(6)).c_str());
            quip = q;
            Text(line, 0x43, 0x65, 0xF8, 0x94, 0x14506E, 2);
        } else {
            quip = S(0x87 + pick(5));
            Text(S(0x86), 0x43, 0x65, 0xF8, 0x94, 0x14506E, 2);
        }
        Text(quip, 0x43, 0x74, 0xF8, 0x94, 0x6E5050, 2);
    }
    // RASHCDG 0x800C836C: n as "%d" in the given box, colour 0x505A50.
    void Number(int32_t n, int32_t just, int32_t x, int32_t y, int32_t w, int32_t h) {
        Text(Fmt(kFmtPlace, n), x, y, w, h, 0x505A50, just);
    }
    // RASHCDG 0x800C79D4 (race type bit 0x10, and 0x18 through 0x800C65A0): the two players side by side.
    // The box (x, y, w, h); player 1's column x, player 2's x + 2 w/3, each w/3 wide and h/2 high.
    // Rows 15 apart from y + 15: the two names (strings 0xAB / 0xAC; co-op 0xA9 / 0xAA), Place (0xAD),
    // Time (0xAE), then Hits (0xAF), TKO's (0xB0), Steals (0xB1) as "for - against" (0x8005BD40) from
    // the player records' counters +0x18..+0x1F (co-op: players 0 + 2 and 1 + 3 summed), and 30 lower
    // the running score (0xB2) - player[k]+0x16, bumped once per race while 0x8005AF55 (RASHCDI
    // 0x80063C00 sets it at the race's start; the scene clears it every frame) for the better place.
    void TwoPlayer(const uint32_t top3[3], int32_t x, int32_t y, int32_t w, int32_t h) {
        constexpr uint32_t kFmtVs = 0x8005BD40;
        constexpr uint32_t kCountFlag = 0x8005AF55;
        // 0x800C5B5C runs here and draws nothing for race type & 0x10
        for (int i = 0; i < 3; ++i) // the top three's players: 0x800C84C0(bike, pool index)
            if (top3[i] != 0xE0u && Human(Bike(top3[i]))) Commit(Bike(top3[i]), top3[i]);
        const uint32_t b0 = Bike(0), b1 = Bike(1);
        Commit(b0, 0);
        Commit(b1, 1);
        const int32_t col = static_cast<int16_t>(w) / 3;
        const int32_t half = static_cast<int16_t>(h) / 2;
        int32_t row = y + 15;
        const int32_t x2 = x + 2 * col;
        const uint32_t p0 = kPlayerRecords, p1 = kPlayerRecords + 36u;
        const bool coop = (g.U8(Gs() + 4u) & 8u) != 0;
        auto sum16 = [&](uint32_t a, uint32_t o) { return static_cast<int32_t>(g.U16(a + o) + (coop ? g.U16(a + 72u + o) : 0u)); };
        auto sum8 = [&](uint32_t a, uint32_t o) { return static_cast<int32_t>(g.U8(a + o) + (coop ? g.U8(a + 72u + o) : 0u)); };
        Text(S(coop ? 0xA9 : 0xAB), x, row, col, half, 0x14506E, 0);
        Text(S(coop ? 0xAA : 0xAC), x2, row, col, half, 0x14506E, 1);
        const int32_t hitsFor[2] = {sum16(p0, 0x1A), sum16(p1, 0x1A)}, hitsAg[2] = {sum16(p0, 0x1E), sum16(p1, 0x1E)};
        const int32_t tkoFor[2] = {sum8(p0, 0x19), sum8(p1, 0x19)}, tkoAg[2] = {sum8(p0, 0x1D), sum8(p1, 0x1D)};
        const int32_t stealFor[2] = {sum8(p0, 0x18), sum8(p1, 0x18)}, stealAg[2] = {sum8(p0, 0x1C), sum8(p1, 0x1C)};
        const uint32_t bike[2] = {b0, b1};
        const int32_t colX[2] = {x, x2};
        row += 15;
        Text(S(0xAD), x, row, w, h, 0x14506E, 2);
        for (int k = 0; k < 2; ++k) {
            const uint32_t r = g.U8(Def(bike[k]) + 0x27u);
            if (static_cast<uint8_t>(r + 2u) < 2u) Text(S(r - 0xE2u), colX[k], row, col, half, 0x505A50, 2);
            else Number(static_cast<int32_t>(r), 2, colX[k], row, col, half);
        }
        row += 15;
        Text(S(0xAE), x, row, w, h, 0x14506E, 2);
        for (int k = 0; k < 2; ++k) {
            const uint32_t r = g.U8(Def(bike[k]) + 0x27u);
            if (static_cast<uint8_t>(r + 2u) < 2u) Text(S(r - 0xE2u), colX[k], row, col, half, 0x505A50, 2);
            else Time(g.S32(Def(bike[k]) + 0x28u), 2, colX[k], row, col, half, 0x505A50);
        }
        const uint32_t labels[3] = {0xAF, 0xB0, 0xB1};
        const int32_t* fors[3] = {hitsFor, tkoFor, stealFor};
        const int32_t* ags[3] = {hitsAg, tkoAg, stealAg};
        for (int i = 0; i < 3; ++i) {
            row += 15;
            Text(S(labels[i]), x, row, w, h, 0x14506E, 2);
            for (int k = 0; k < 2; ++k) Text(Fmt(kFmtVs, fors[i][k], ags[i][k]), colX[k], row, col, half, 0x505A50, 2);
        }
        row += 30;
        Text(S(0xB2), x, row, w, h, 0x14506E, 2);
        if (g.U8(kCountFlag) != 0) { // 0x800C8208..0x800C82D4
            const uint8_t r0 = g.U8(Def(b0) + 0x27u), r1 = g.U8(Def(b1) + 0x27u);
            bool second = false;
            bool done = false;
            if (static_cast<uint8_t>(r0 + 2u) < 2u) {
                second = r1 < 0xF7u;
            } else if (r0 < r1) {
                g.W8(p0 + 0x16u, static_cast<uint8_t>(g.U8(p0 + 0x16u) + 1u));
                done = true;
            } else {
                second = r1 < r0;
                if (static_cast<uint8_t>(r1 + 2u) < 2u) done = true;
            }
            if (!done && second) g.W8(p1 + 0x16u, static_cast<uint8_t>(g.U8(p1 + 0x16u) + 1u));
        }
        Number(static_cast<int8_t>(g.U8(p0 + 0x16u)), 2, x, row, col, half);
        Number(static_cast<int8_t>(g.U8(p1 + 0x16u)), 2, x2, row, col, half);
            g.W8(kQuitFlag, 0);
    }
    // RASHCDG 0x800C67FC (race type bit 4): Time Trial. The record of the race's best-times table
    // (0x80053A88 + 176 * (race - 56): entry 1's time +0x1C, entry 8's +0xA8) against the player's time:
    // better than entry 1 -> 0x8005AF54 = 1, "New Record!" (0x76) in the cycling colour and the record
    // becomes the player's; not worse than entry 8 -> 2 (0x75). A time for a rider who did not finish:
    // 99999998 busted (0xFE), 99999999 wrecked / out (0xFA / 0xFF). Then "%s %02d:%02d:%02d" (0x8005BCF0)
    // of 0xA8 and the record, the player's result into player[session+0x13] (0x800C84C0), and the Time
    // Trial's players 0..session+0x13 copied to 0x800CD588 (36 bytes each, their indices at 0x800CD578),
    // sorted by time (the original's swap walk, which follows the current player only when it is the
    // later of a swapped pair), placed 1.. unless out, and painted: place, "Player n" (0xA2 + index),
    // time, in three columns of w/3 from y + 45, 15 apart; the current player's place in the cycling
    // colour.
    void TimeTrial(int32_t x, int32_t y, int32_t w, int32_t h) {
        constexpr uint32_t kFmtRecord = 0x8005BCF0, kFlag = 0x8005AF54, kSort = 0x800CD588, kOrder = 0x800CD578;
        const uint32_t sess = 0x800D80D8u;
        const uint32_t table = 0x80053A88u + static_cast<uint32_t>((g.S8(sess + 8u) - 0x38) * 0xB0);
        int32_t best = g.S32(table + 0x1Cu);
        const uint32_t def = Def(Bike(0));
        const int8_t code = static_cast<int8_t>(g.U8(def + 0x27u));
        if (code == -1) {
            g.W32(def + 0x28u, 99999999u);
        } else if (code == -2 || code == -6) {
            g.W32(def + 0x28u, code == -2 ? 99999998u : 99999999u);
        } else {
            const int32_t t = g.S32(def + 0x28u);
            if (best < t) {
                if (t <= g.S32(table + 0xA8u)) g.W8(kFlag, 2);
            } else {
                g.W8(kFlag, 1);
                best = t;
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), CStr(kFmtRecord).c_str(), S(0xA8).c_str(), best / 18000,
                      best / 300 - (best / 18000) * 60, best / 3 - (best / 300) * 100);
        uint32_t rgb = 0x505A50;
        if (g.U8(kFlag) == 1) {
            Text(S(0x76), x, y + 15, w, h, g.U32(kColourRow), 2);
            rgb = g.U32(kColourRow);
        } else if (g.U8(kFlag) == 2) {
            Text(S(0x75), x, y + 15, w, h, g.U32(kColourRow), 2);
        }
        Text(buf, x, y + 30, w, h, rgb, 2);
        const int32_t cur = g.S8(sess + 0x13u);
        Commit(Bike(0), static_cast<uint32_t>(cur));
        const int32_t n = cur + 1;
        for (int32_t i = 0; i < n; ++i) {
            for (uint32_t k = 0; k < 36u; k += 4u) g.W32(kSort + 36u * static_cast<uint32_t>(i) + k, g.U32(kPlayerRecords + 36u * static_cast<uint32_t>(i) + k));
            g.W8(kOrder + static_cast<uint32_t>(i), static_cast<uint8_t>(i));
        }
        int32_t mine = cur;
        uint32_t place = 1;
        for (int32_t a = 0; a < n; ++a) {
            const uint32_t ea = kSort + 36u * static_cast<uint32_t>(a);
            for (int32_t b = a + 1; b < n; ++b) {
                const uint32_t eb = kSort + 36u * static_cast<uint32_t>(b);
                if (g.S32(eb) < g.S32(ea)) {
                    for (uint32_t k = 0; k < 36u; k += 4u) {
                        const uint32_t va = g.U32(ea + k), vb = g.U32(eb + k);
                        g.W32(ea + k, vb);
                        g.W32(eb + k, va);
                    }
                    const uint8_t oa = g.U8(kOrder + static_cast<uint32_t>(a)), ob = g.U8(kOrder + static_cast<uint32_t>(b));
                    g.W8(kOrder + static_cast<uint32_t>(b), oa);
                    g.W8(kOrder + static_cast<uint32_t>(a), ob);
                    if (b == mine) mine = a;
                }
            }
            const uint32_t r = g.U32(ea + 0x20u);
            if (r != 0xFAu && r != 0xFFu && r != 0xFEu) g.W32(ea + 0x20u, place++ & 0xFFu);
        }
        const int32_t col = static_cast<int16_t>(w) / 3;
        for (int32_t i = 0; i < n; ++i) {
            const uint32_t e = kSort + 36u * static_cast<uint32_t>(i);
            const int32_t ry = y + 15 + 30 + 15 * i;
            const uint32_t r = g.U32(e + 0x20u);
            const uint32_t dim = g.U32(kColourDone);
            auto status = [&](int32_t cx) {
                if (r == 0xFFu || r == 0xFAu) Text(S(0x1D), cx, ry, col, h, dim, 2);
                else if (r == 0xFEu) Text(S(0x1C), cx, ry, col, h, dim, 2);
                else return false;
                return true;
            };
            if (!status(0x43)) Text(Fmt(kFmtPlace, static_cast<int32_t>(r)), 0x43, ry, col, h, i == mine ? g.U32(kColourRow) : 0x14506Eu, 2);
            Text(S(0xA2u + g.U8(kOrder + static_cast<uint32_t>(i))), 0x43 + col, ry, col, h, dim, 2);
            if (!status(0x43 + 2 * col)) Time(g.S32(e), 2, 0x43 + 2 * col, ry, col, h, dim);
        }
    }
    // RASHCDG 0x800C6FF8 (race type 0x11 through 0x800C6FAC): the two-player cops and robbers verdict, in
    // the box (0x43, 0x38 / 0x65, 0xF8, 0x94). The cop is the player on a police machine (bike +0xB4 >= 18):
    // its code 0xFC (the capture: string 0x9C over "%s %s %02d:%02d:%02d" of 0x9D, 0x8E and the time),
    // 0xFD (0x9E over "%s %02d:%02d:%02d" of 0x8E and the time), else, with the robber wrecked or out
    // (0xFA / 0xFF), 0xA1 alone, or 0x9F over 0xA0.
    void TwoPlayerFiveO() {
        constexpr uint32_t kFmtCaught = 0x8005BD04, kFmtTimeOnly = 0x8005BCF0;
        const uint32_t b0 = Bike(0), b1 = Bike(1);
        const bool firstIsRobber = g.U32(b0 + 0xB4u) < 0x12u;
        const uint32_t cop = firstIsRobber ? b1 : b0, robber = firstIsRobber ? b0 : b1;
        Commit(b0, 0);
        Commit(b1, 1);
        const int32_t x = 0x43, w = 0xF8, h = 0x94;
        const uint8_t c = g.U8(Def(cop) + 0x27u);
        const int32_t t = g.S32(Def(cop) + 0x28u);
        const int32_t mm = t / 18000, ss = t / 300 - mm * 60, cc = t / 3 - (t / 300) * 100;
        char buf[256];
        if (c == 0xFCu) {
            std::snprintf(buf, sizeof(buf), CStr(kFmtCaught).c_str(), S(0x9D).c_str(), S(0x8E).c_str(), mm, ss, cc);
            Text(S(0x9C), x, 0x65, w, h, 0x14506E, 2);
            Text(buf, x, 0x74, w, h, 0x14506E, 2);
        } else if (c == 0xFDu) {
            std::snprintf(buf, sizeof(buf), CStr(kFmtTimeOnly).c_str(), S(0x8E).c_str(), mm, ss, cc);
            Text(S(0x9E), x, 0x65, w, h, 0x14506E, 2);
            Text(buf, x, 0x74, w, h, 0x14506E, 2);
        } else {
            const uint8_t rc = g.U8(Def(robber) + 0x27u);
            if (rc == 0xFAu || rc == 0xFFu) {
                Text(S(0xA1), x, 0x65, w, h, 0x14506E, 2);
            } else {
                Text(S(0x9F), x, 0x65, w, h, 0x14506E, 2);
                Text(S(0xA0), x, 0x74, w, h, 0x14506E, 2);
            }
        }
    }
};

} // namespace

void RaceResults::Begin(std::vector<uint8_t> raceRam, const std::vector<std::string>* strings) {
    ram_ = std::move(raceRam);
    ram_.resize(GuestRam::kRamSize, 0);
    strings_ = strings;
    active_ = strings_ != nullptr;
    ran_ = active_;
    gap_.clear();
}

uint32_t RaceResults::PlayerWord(int k, uint32_t offset) const {
    const uint32_t a = (kPlayerRecords + 36u * static_cast<uint32_t>(k) + offset) & 0x1FFFFFu;
    if (a + 4u > ram_.size()) return 0;
    return static_cast<uint32_t>(ram_[a]) | (static_cast<uint32_t>(ram_[a + 1]) << 8) |
           (static_cast<uint32_t>(ram_[a + 2]) << 16) | (static_cast<uint32_t>(ram_[a + 3]) << 24);
}

// RASHCDG 0x800C5918.
bool RaceResults::Frame(std::vector<TextCall>& out) {
    if (!active_) return false;
    GuestRam g(ram_.data(), kArenaGp);
    Ctx c{g, *strings_, out};
    uint32_t top3[3] = {g.U32(kTop3Init), g.U32(kTop3Init + 4u), g.U32(kTop3Init + 8u)};
    // 0x800C6358: the bike of each of places 1..3.
    for (uint32_t i = 0; i < static_cast<uint32_t>(g.S32(kBikeCount)); ++i) {
        const uint32_t place = g.U8(c.Def(c.Bike(i)) + 0x27u) - 1u;
        if (place < 3u) top3[place] = i;
    }
    g.W32(kColourDone, g.S32(kColourDone) < 0x101089 ? g.U32(kColourDone) + 7u : 0x101010u);
    g.W32(kColourRow, g.S32(kColourRow) < 0x888081 ? g.U32(kColourRow) + 0x70606u : 0x101010u);
    // 0x800C63CC: the heading and the "press START" line in the box (0x43, 0x29, 0xF8, 0x94).
    const int32_t x = 0x43, y = 0x29, w = 0xF8, h = 0x94;
    c.Text(c.S(0x5E), x, y, w, h, 0x14506E, 2);
    c.Text(c.S(0x6B), x, y + 0x96, w, h, 0x6E5050, 2);
    const uint8_t type = g.U8(c.Gs() + 4u);
    bool ported = true;
    if (type & 0x22u) {
        if (type == '$') c.Mission(x, y, w, h, 0);
        else if (type == '!') c.Mission(x, y, w, h, 1);
        else if (type == ',') c.Mission(x, y, w, h, 2);
        else c.Standings(top3, x, y, w, h);
    } else if (type & 4u) {
        c.TimeTrial(x, y, w, h);              // 0x800C67FC
    } else if (type & 1u) {
        if (type & 0x10u) c.TwoPlayerFiveO(); // 0x800C6FAC -> 0x800C6FF8
        else c.FiveO();
    } else if (type & 0x10u) {
        c.TwoPlayer(top3, x, y, w, h);        // 0x800C79D4
    } else if (type & 8u) {
        c.Standings(top3, x, y, w, h);
    }
    g.W8(0x8005AF55u, 0); // 0x800C5B50: the two-player score counts the race once
    return ported;
}

} // namespace rr::shell
