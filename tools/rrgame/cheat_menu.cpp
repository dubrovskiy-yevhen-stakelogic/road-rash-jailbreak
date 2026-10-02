// The cheat menu (cheat_menu.h).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "cheat_menu.h"

#include "graphics_settings.h" // SettingsPath()
#include "settings_file.h"
#include "game/shell/shell_card.h"
#include "game/shell/shell_logic.h" // CardChecksumHost, kSession / kPlayers
#include "rrvfs/disc_image.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rrgame {

namespace {

using rr::game::Cheats;
using rr::game::CheatSettings;

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::pair<std::string, std::string>>& Overrides() { // the command line's --cheat-* values
    static std::vector<std::pair<std::string, std::string>> o;
    return o;
}
long g_nowFrame = -1;
int g_nowWeapon = -1;
struct TimedCheat { // --cheat-at F:key=value: a menu change at frame F of a scripted run
    long frame;
    std::string key, value;
};
std::vector<TimedCheat>& Timed() {
    static std::vector<TimedCheat> t;
    return t;
}
std::string g_cardPath;
const rr::DiscImage* g_disc = nullptr;
int g_menuWeapon = -1; // the weapon row's choice (the list's id)

void SaveNow(std::string& note) {
    if (SaveCheats(SettingsPath(), Cheats())) note = "Saved: " + SettingsPath();
    else note = "Could not write " + SettingsPath();
}

int MenuWeapon() {
    const auto& list = rr::game::CheatWeapons();
    if (Cheats().weapon >= 0) g_menuWeapon = Cheats().weapon;
    if (g_menuWeapon < 0 && !list.empty()) g_menuWeapon = list.front().id;
    if (g_menuWeapon < 0) g_menuWeapon = 0;
    return g_menuWeapon;
}

void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t Get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}
bool ReplaceFile(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec); // POSIX rename replaces atomically
    return !ec;
#endif
}

// The RASHCDF tables the career dispatcher clamps by (shell_career.cpp), s32 per
// venue: player+0x14's cap (kRateCap) and a weapon level's cap (kLevelCap).
constexpr uint32_t kShellLoad = 0x8005B5E8;
constexpr uint32_t kRateCap = 0x80089E84, kLevelCap = 0x80089ECC;
constexpr uint32_t kBlock = 8192;
bool ShellTable(const rr::DiscImage& disc, uint32_t address, int32_t out[6], std::string& error) {
    const auto f = disc.Find("RASHCDF.BIN");
    if (!f) {
        error = "the disc carries no RASHCDF.BIN";
        return false;
    }
    const std::vector<uint8_t> shell = disc.ReadFile(*f);
    const size_t at = address - kShellLoad;
    if (at + 24 > shell.size()) {
        error = "RASHCDF.BIN is shorter than its career tables";
        return false;
    }
    for (int k = 0; k < 6; ++k) out[k] = static_cast<int32_t>(Get32(shell.data() + at + 4u * static_cast<size_t>(k)));
    return true;
}

// The five 5x7 letters of the tag (our own glyphs, rows top down, bit 4 = the left column).
const uint8_t* Glyph(char c) {
    static const uint8_t kC[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static const uint8_t kH[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t kE[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t kA[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t kT[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t kS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    switch (c) {
    case 'C': return kC;
    case 'H': return kH;
    case 'E': return kE;
    case 'A': return kA;
    case 'T': return kT;
    default: return kS;
    }
}

} // namespace

// ============================================================================ settings
bool LoadCheats(const std::string& path, CheatSettings& s) {
    IniPairs kv;
    if (!ReadIniSection(path, "cheats", kv)) return false;
    for (const auto& [key, value] : kv)
        if (!s.Parse(key, value)) std::fprintf(stderr, "settings: %s: ignored '%s=%s'\n", path.c_str(), key.c_str(), value.c_str());
    return true;
}

bool SaveCheats(const std::string& path, const CheatSettings& s) { return WriteIniSection(path, "cheats", s.Serialize()); }

bool ApplyCheatFlag(int argc, char** argv, int& i) {
    const std::string a = argv[i];
    const auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
    };
    const auto set = [&](const char* key, const std::string& v) {
        CheatSettings probe;
        if (!probe.Parse(key, v)) throw std::runtime_error(a + ": bad value '" + v + "'");
        Overrides().emplace_back(key, v);
    };
    if (a == "--cheat-weapon") set("weapon", value());
    else if (a == "--cheat-weapon-now") {
        const std::string v = value();
        if (std::sscanf(v.c_str(), "%ld:%d", &g_nowFrame, &g_nowWeapon) != 2 || g_nowWeapon < 0 || g_nowWeapon > 8)
            throw std::runtime_error(a + ": F:W (frame, weapon 0..8), not '" + v + "'");
    } else if (a == "--cheat-at") {
        const std::string v = value();
        const size_t colon = v.find(':'), eq = v.find('=');
        TimedCheat t{-1, "", ""};
        if (colon != std::string::npos && eq != std::string::npos && eq > colon) {
            t.frame = std::atol(v.substr(0, colon).c_str());
            t.key = v.substr(colon + 1, eq - colon - 1);
            t.value = v.substr(eq + 1);
        }
        CheatSettings probe;
        if (t.frame < 0 || !probe.Parse(t.key, t.value)) throw std::runtime_error(a + ": F:key=value (a [cheats] key), not '" + v + "'");
        Timed().push_back(t);
    } else if (a == "--cheat-report") rr::game::SetCheatReport(true);
    else if (a == "--cheat-swings") set("unlimited_swings", "1");
    else if (a == "--cheat-no-police") set("no_police", "1");
    else if (a == "--cheat-no-traffic") set("no_traffic", "1");
    else if (a == "--cheat-sparring") set("sparring", value());
    else if (a == "--cheat-sparring-attack") set("sparring_attack", "1");
    else if (a == "--cheat-sparring-weapon") set("sparring_weapon", value()); // own | fists | 0..8
    else if (a == "--cheat-sparring-standing") set("sparring_standing", "1"); // the cheat rule
    else if (a == "--cheat-passive") set("rivals_passive", "1");
    else if (a == "--cheat-god") set("god", "1");
    else if (a == "--cheat-nitro") set("infinite_nitro", "1");
    else if (a == "--cheat-freeze-timer") set("freeze_timer", "1");
    else return false;
    return true;
}

CheatSettings& StartCheats(bool scripted) {
    static bool loaded = false;
    CheatSettings& s = Cheats();
    if (scripted) {
        s = CheatSettings{};
    } else if (!loaded) { // once a process: the front end and every race of it share them, the menus edit them
        loaded = true;
        s = CheatSettings{};
        LoadCheats(SettingsPath(), s);
    }
    for (const auto& o : Overrides()) s.Parse(o.first, o.second);
    return s;
}

void CheatScriptFrame(long frame) {
    if (g_nowFrame >= 0 && frame == g_nowFrame) rr::game::RequestWeaponNow(g_nowWeapon);
    for (const TimedCheat& t : Timed())
        if (t.frame == frame && Cheats().Parse(t.key, t.value))
            std::printf("cheats: frame %ld: %s=%s (%s)\n", frame, t.key.c_str(), t.value.c_str(), Cheats().Describe().c_str());
}

std::string LoadCheatWeapons(const rr::DiscImage& disc) {
    if (!rr::game::CheatWeapons().empty()) return "";
    std::string error;
    std::vector<rr::game::CheatWeapon> list = rr::game::ReadCheatWeapons(disc, &error);
    if (list.empty()) return "cheats: no weapon list (" + error + ")\n";
    std::string line = "cheats: the disc's weapons (SLUS 0x80052EEC's rows, RASHCDF 0x80088DF4 tags in the rap sheet's order):";
    for (const auto& w : list) line += " " + std::to_string(w.id) + "=" + w.tag;
    rr::game::SetCheatWeapons(std::move(list));
    return line + "\n";
}

// ============================================================================ the page
std::vector<CheatRow> CheatMenuRows() {
    const CheatSettings& c = Cheats();
    static const char* const kSpar[3] = {"Off", "Passive", "Fight back (game rules)"};
    const bool race = rr::game::CheatsInRace();
    const int w = MenuWeapon();
    std::vector<CheatRow> r;
    r.push_back({"Weapon", rr::game::CheatWeapons().empty() ? "(no list: open a race)" : rr::game::CheatWeaponName(w)});
    r.push_back({"Start every race with it", c.weapon >= 0 ? "On" : "Off"});
    r.push_back({"Put it in the hand now", race ? "Enter" : "(in a race)"});
    r.push_back({"Unlimited weapon swings", c.unlimitedSwings ? "On" : "Off"});
    r.push_back({"No police", c.noPolice ? "On" : "Off"});
    r.push_back({"No traffic", c.noTraffic ? "On" : "Off"});
    r.push_back({"Sparring partners", kSpar[std::clamp(c.sparring, 0, 2)]});
    // The partners' attack (rows kPartnerRows)
    r.push_back({"  Partners attack you", c.sparringAttack ? "On" : "Off"});
    r.push_back({"  Partners' weapon", c.sparringWeapon < 0 ? "Their own" : c.sparringWeapon >= 9 ? "Fists" : rr::game::CheatWeaponName(c.sparringWeapon)});
    r.push_back({"  ... even when you stand (cheat rule)", c.sparringStanding ? "On" : "Off"});
    r.push_back({"Rivals passive (never start a fight)", c.rivalsPassive ? "On" : "Off"});
    r.push_back({"God mode (health, bike damage)", c.god ? "On" : "Off"});
    r.push_back({"Infinite nitro", c.infiniteNitro ? "On" : "Off"});
    r.push_back({"Freeze the time limit", c.freezeTimer ? "On" : "Off"});
    r.push_back({"Career: max nitros, every weapon full", race ? "(not in a race)" : "Enter"});
    r.push_back({"Career: unlock the next venue", race ? "(not in a race)" : "Enter"});
    r.push_back({"All cheats off", c.AnyOn() ? "Enter" : ""});
    return r;
}

bool CheatMenuActivate(int row, int direction, std::string& note) {
    CheatSettings& c = Cheats();
    const int d = direction == 0 ? 1 : direction;
    const bool race = rr::game::CheatsInRace();
    bool changed = true;
    // Rows 7..9 are the partners' attack rows (CheatMenuRows); the rows after them keep their own cases
    constexpr int kPartnerRows = 7, kPartnerRowCount = 3;
    if (row >= kPartnerRows && row < kPartnerRows + kPartnerRowCount) {
        if (row == kPartnerRows) {
            c.sparringAttack = !c.sparringAttack;
            if (c.sparringAttack && c.sparring == 0) note = "Turn Sparring partners on too (Passive keeps them as punching bags)";
        } else if (row == kPartnerRows + 1) { // their own, fists, then the disc's weapons
            std::vector<int> ids{-1, 9};
            for (const auto& w : rr::game::CheatWeapons()) ids.push_back(w.id);
            if (rr::game::CheatWeapons().empty())
                for (int w = 0; w < 9; ++w) ids.push_back(w);
            size_t at = 0;
            for (size_t k = 0; k < ids.size(); ++k)
                if (ids[k] == c.sparringWeapon) at = k;
            at = (at + ids.size() + static_cast<size_t>(d + static_cast<int>(ids.size()))) % ids.size();
            c.sparringWeapon = ids[at];
        } else {
            c.sparringStanding = !c.sparringStanding;
            if (c.sparringStanding) note = "Cheat rule: partners may hit you while you stand (the original never does)";
        }
        std::string saved;
        SaveNow(saved);
        if (note.empty()) note = saved;
        return true;
    }
    if (row >= kPartnerRows + kPartnerRowCount) row -= kPartnerRowCount;
    switch (row) {
    case 0: { // the weapon choice (the disc's list)
        const auto& list = rr::game::CheatWeapons();
        if (list.empty()) {
            note = "The weapon list is read from the disc when a race or the menus open";
            changed = false;
            break;
        }
        const int cur = MenuWeapon();
        size_t at = 0;
        for (size_t k = 0; k < list.size(); ++k)
            if (list[k].id == cur) at = k;
        at = (at + list.size() + static_cast<size_t>(d + static_cast<int>(list.size()))) % list.size();
        g_menuWeapon = list[at].id;
        if (c.weapon >= 0) c.weapon = g_menuWeapon;
        else changed = false;
        note = "Weapon: " + rr::game::CheatWeaponName(g_menuWeapon);
        break;
    }
    case 1: c.weapon = c.weapon >= 0 ? -1 : MenuWeapon(); break;
    case 2:
        changed = false;
        if (direction != 0) break;
        if (!race) {
            note = "Put in the hand during a race (the start-every-race switch works from the menus)";
            break;
        }
        rr::game::RequestWeaponNow(MenuWeapon());
        note = "In the hand: " + rr::game::CheatWeaponName(MenuWeapon());
        break;
    case 3: c.unlimitedSwings = !c.unlimitedSwings; break;
    case 4: c.noPolice = !c.noPolice; break;
    case 5:
        c.noTraffic = !c.noTraffic;
        if (c.noTraffic) note = "No new traffic is spawned (cars already out drive on)";
        break;
    case 6: c.sparring = (c.sparring + d + 3) % 3; break;
    case 7: c.rivalsPassive = !c.rivalsPassive; break;
    case 8: c.god = !c.god; break;
    case 9: c.infiniteNitro = !c.infiniteNitro; break;
    case 10: c.freezeTimer = !c.freezeTimer; break;
    case 11:
    case 12: {
        changed = false;
        if (direction != 0) break;
        if (race) {
            note = "Career cheats change the saved career: use them from the menus, not in a race";
            break;
        }
        if (g_disc == nullptr || g_cardPath.empty()) {
            note = "No memory card in use";
            break;
        }
        note = ApplyCareerCheat(*g_disc, g_cardPath, row == 11 ? CareerCheat::MaxNitroWeapons : CareerCheat::NextVenue);
        break;
    }
    case 13:
        if (direction != 0 || !c.AnyOn()) {
            changed = false;
            break;
        }
        c = CheatSettings{};
        note = "All cheats off";
        break;
    default:
        changed = false;
    }
    if (changed) {
        std::string saved;
        SaveNow(saved);
        if (note.empty()) note = saved;
    }
    return changed;
}

// ============================================================================ the career
void SetCheatCardPath(const std::string& path) { g_cardPath = path; }
void SetCheatDisc(const rr::DiscImage* disc) { g_disc = disc; }

std::string ApplyCareerCheat(const rr::DiscImage& disc, const std::string& cardPath, CareerCheat what, bool* ok) {
    if (ok != nullptr) *ok = false;
    rr::shell::CardImage card;
    std::string error;
    if (!rr::shell::LoadCardFile(cardPath, card, error)) return "Career cheat: " + error + " (nothing written)";
    if (card.block <= 0) return "Career cheat: no Road Rash save on the card (save a career first)";
    uint8_t* const block = card.raw.data() + static_cast<size_t>(card.block) * kBlock;
    const int slot = block[0x200];
    if (slot < 0 || slot > 9) return "Career cheat: the card's current career slot is out of range";
    uint8_t* const rec = block + rr::shell::kCardRecordsAt + rr::shell::kCardRecordBytes * static_cast<uint32_t>(slot);
    {
        uint32_t a = 0, b = 0;
        rr::shell::CardChecksumHost(rec, slot, &a, &b);
        if (Get32(rec) != 0) return "Career cheat: career slot " + std::to_string(slot) + " is empty (save a career first)";
        if (a != Get32(rec + 0x1DC) || b != Get32(rec + 0x1E0))
            return "Career cheat: career slot " + std::to_string(slot) + " fails its checksum (nothing written)";
    }
    // frontend.md 8.4: rec+0x004 = the player records 0x800D81D8, rec+0x0DC = the session 0x800D80D8
    uint8_t* const player = rec + 4;
    uint8_t* const session = rec + 0xDC;
    const int venue = static_cast<int8_t>(session[0x04]);
    const int v = std::clamp(venue, 0, 5);
    std::string done;
    if (what == CareerCheat::MaxNitroWeapons) {
        int32_t rateCap[6], levelCap[6];
        if (!ShellTable(disc, kRateCap, rateCap, error) || !ShellTable(disc, kLevelCap, levelCap, error))
            return "Career cheat: " + error + " (nothing written)";
        // player+0x14 (the nitros the career buys with Rash Cash) at the venue's cap; every weapon of the disc's list
        // owned (player+0x0C) with its level nibble (player+0x10) at the venue's level cap
        const int nitro = std::clamp(rateCap[v], 0, 255);
        const uint32_t level = static_cast<uint32_t>(std::clamp(levelCap[v], 0, 15));
        player[0x14] = static_cast<uint8_t>(nitro);
        uint16_t mask = static_cast<uint16_t>(player[0x0C] | (player[0x0D] << 8));
        uint32_t nibbles = Get32(player + 0x10);
        int weapons = 0;
        for (const rr::game::CheatWeapon& w : rr::game::CheatWeapons()) {
            mask = static_cast<uint16_t>(mask | (1u << w.id));
            if (w.id < 8) nibbles = (nibbles & ~(15u << (4 * w.id))) | (level << (4 * w.id));
            ++weapons;
        }
        if (weapons == 0) return "Career cheat: no weapon list from the disc (nothing written)";
        player[0x0C] = static_cast<uint8_t>(mask);
        player[0x0D] = static_cast<uint8_t>(mask >> 8);
        Put32(player + 0x10, nibbles);
        done = "nitros " + std::to_string(nitro) + ", " + std::to_string(weapons) + " weapons at level " + std::to_string(level);
    } else {
        // the current venue's races (missions) marked won and the venue advanced as the mode's own result dispatcher
        // does once they all are (ResumeDispatch 0x8007FF4C by the mode word session+0x00, shell_career.cpp):
        //   32 the series  VenueStep 0x8007D4A0: race ids 1..9, 28, 10..18, 29, 19..27, 30 in +0xF0, venue + 1;
        //                  venues 1 and 3 reset session+0x11, 1, 3 and 4 set session+0x05 bit 0x10
        //   8  Side Car    SideCarResult 0x8007DDA8's step: 1..6 -> venue 2, 10..15 -> 4 (+0xF0), 4 the last
        //   1  Five-O      FiveOResult 0x8007E2C8's step: missions 0..5 -> venue 2, 6..11 -> 4 (+0xFC), 4 the last
        //   (the two last set +0x11 = 0 and +0x05 |= 0x10 with the venue)
        const uint32_t mode = Get32(session);
        auto mark = [&](uint32_t map, int first, int last) {
            for (int id = first; id <= last; ++id) session[map + static_cast<uint32_t>(id >> 3)] |= static_cast<uint8_t>(1u << (id & 7));
        };
        if (mode == 32u) {
            if (venue >= 5) return "Career cheat: the series is at its last venue (Jail Break) already";
            static const int kRange[6][2] = {{1, 9}, {28, 28}, {10, 18}, {29, 29}, {19, 27}, {30, 30}};
            mark(0xF0, kRange[v][0], kRange[v][1]);
            if (v == 1 || v == 3) session[0x11] = 0;
            session[0x04] = static_cast<uint8_t>(v + 1);
            if (v == 1 || v == 3 || v == 4) session[0x05] |= 0x10u;
            done = "series venue " + std::to_string(v) + " -> " + std::to_string(v + 1);
        } else if (mode == 8u || mode == 1u) {
            const bool fiveO = mode == 1u;
            if (venue == 4) return "Career cheat: the saved game is at its last venue already";
            int first = 0, last = 0;
            if (venue == 2) first = fiveO ? 6 : 10, last = fiveO ? 11 : 15;
            else if (!fiveO || venue == 0) first = fiveO ? 0 : 1, last = fiveO ? 5 : 6;
            else return "Career cheat: Five-O venue " + std::to_string(venue) + " has no step";
            mark(fiveO ? 0xFCu : 0xF0u, first, last);
            const int to = venue == 2 ? 4 : 2;
            session[0x04] = static_cast<uint8_t>(to);
            session[0x11] = 0;
            session[0x05] |= 0x10u;
            done = std::string(fiveO ? "Five-O" : "Side Car") + " venue " + std::to_string(venue) + " -> " + std::to_string(to);
        } else {
            return "Career cheat: this saved game (mode " + std::to_string(mode) + ") has no venues to unlock";
        }
    }
    {
        uint32_t a = 0, b = 0;
        rr::shell::CardChecksumHost(rec, slot, &a, &b);
        Put32(rec + 0x1DC, a);
        Put32(rec + 0x1E0, b);
    }
    // backup once, candidate, read back, verify, replace
    const std::string backup = cardPath + ".before-cheats.bak", temp = cardPath + ".cheat-tmp";
    std::error_code ec;
    if (!std::filesystem::exists(backup, ec)) {
        std::vector<uint8_t> original;
        if (!ReadFileBytes(cardPath, original) || !WriteFileBytes(backup, original))
            return "Career cheat: cannot write the backup " + backup + " (nothing written)";
    }
    if (!rr::shell::SaveCardFile(temp, card, error)) return "Career cheat: " + error + " (the card is unchanged)";
    rr::shell::CardImage back;
    int mismatches = -1;
    const bool readBack = rr::shell::LoadCardFile(temp, back, error) && back.raw == card.raw;
    if (readBack) rr::shell::CheckCard(back, &mismatches);
    if (!readBack || mismatches != 0) {
        std::filesystem::remove(temp, ec);
        return "Career cheat: the candidate did not verify (the card is unchanged)";
    }
    if (!ReplaceFile(temp, cardPath)) {
        std::filesystem::remove(temp, ec);
        return "Career cheat: cannot replace the card (it is unchanged)";
    }
    if (ok != nullptr) *ok = true;
    return "Career slot " + std::to_string(slot) + ": " + done + " - saved and verified; Load Game to use it";
}

// ============================================================================ the HUD's tag
bool CheatsShown() { return Cheats().AnyOn(); }

void StampCheatsTag(uint8_t* rgba, int width, int height) {
    static const char kWord[] = "CHEATS";
    constexpr int kLetters = 6, kW = 6 * kLetters - 1, kH = 7;
    const int x0 = (width - kW) / 2, y0 = 12; // inside the shown part of the draw area (from y 8, main.cpp Hud)
    if (x0 < 1 || y0 + kH + 1 > height) return;
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        uint8_t* p = rgba + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
        p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
    };
    // a dark box behind the letters, then the letters in amber
    for (int y = y0 - 1; y <= y0 + kH; ++y)
        for (int x = x0 - 2; x <= x0 + kW + 1; ++x) put(x, y, 0, 0, 0);
    for (int i = 0; i < kLetters; ++i) {
        const uint8_t* glyph = Glyph(kWord[i]);
        for (int row = 0; row < kH; ++row)
            for (int col = 0; col < 5; ++col)
                if (glyph[row] & (0x10 >> col)) put(x0 + 6 * i + col, y0 + row, 255, 190, 40);
    }
}

} // namespace rrgame
