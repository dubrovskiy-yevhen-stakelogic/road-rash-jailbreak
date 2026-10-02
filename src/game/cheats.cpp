// Cheats (cheats.h). OURS; every guest address below is the one cheats.h names.
#include "game/cheats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "game/race_session.h"
#include "game/weapon_session.h" // ReadWeaponInHand: what the renderer draws in the hand
#include "game/sim/ai.h"
#include "game/sim/ai_cmd.h"
#include "game/sim/bike.h"
#include "game/sim/coll_util.h"
#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

namespace {

using rr::sim::GuestRam;

constexpr uint32_t kGp = 0x8005AC8C;            // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPool0Ptr = 0x8005B3A0;      // pool 0 (the bikes), stride 1096
constexpr uint32_t kLiveCount = 0x8005B1F8;     // the live bike count the AI passes walk
constexpr uint32_t kSlot = 1096;
constexpr uint32_t kPoliceSwitch = 0x8005ACC0;  // PoliceSched 0x8009E89C's gate (0x8009E8A0)
constexpr uint32_t kTrafficSwitch = 0x8005ACC4; // the car spawner's gate (population.h kPopTrafficOn)
constexpr uint32_t kTimeLimit = 0x8005ACC8;     // the race time limit in ticks (modes.h kModeTimeLimit)
constexpr uint32_t kFightLatHalf = 0x80052F74;  // FightUpdate's lateral bound: |lat| <= 2 x this (0x800C0654)
constexpr int32_t kFightAlong = 0xB333;         // FightUpdate's along bound (0.7, 0x800C0664)
constexpr uint32_t kSqrtTable = 0x800560CC;     // SLUS: Length3's table
constexpr uint32_t kCheatSp = 0x801FE800;       // OURS: the stack pointer handed to AimBesideRider (nothing is written)
constexpr int kSwingsFull = 15;                 // the swing nibble's maximum
constexpr int kNitroKeep = 5;
constexpr uint8_t kBagHealth = 255;             // a passive partner's health (the HUD's bar clamps at 127/128, full)
constexpr int32_t kPartnerAhead = 0x3333;       // 0.2: where a partner is held along the player (16.16)
constexpr int32_t kPartnerSide = 0x10000;       // 1.0 to either side of him
constexpr int32_t kPickRange = 60;              // a partner is picked from riders within 60 world units (octagonal x / z),
constexpr int32_t kPickAhead = 0x30000;         // ... those more than 3.0 ahead of the player last (they cannot come back)
constexpr int32_t kDropAhead = 0x50000;         // a partner stopped 5.0 ahead of a standing player gives way to one behind
constexpr int32_t kStood = 0x10000;             // the player "stands": his speed +0x240 below 1.0 unit/s
constexpr uint32_t kBexeWord = 0x80052EEC;      // SLUS: the fighter classes' weapon preference rows (stride 36, 9 bytes)
constexpr uint32_t kShellLoad = 0x8005B5E8;     // RASHCDF.BIN's load address (shell_arena.cpp kOverlayLoad)
constexpr uint32_t kTagTable = 0x80088DF4;      // RASHCDF: the FourCC art tags (frontend.md 5.3)
// rules.md 8.3: entries 91..99 of the table are the nine weapons' tags.
// Which of those nine entries names weapon id k. NOT 91 + k (rules.md 8.3's [probable] link): the rap sheet
// RASHCDF 0x80074554 (PORTED, shell_text.cpp) draws owned bit k (+0x0C) with the sprite tags
// CHAN CLUB PIPE WOOD NCHK CBAR PROD STUN SPRY - entries 95 92 96 93 91 94 97 98 99 of this table - and the race agrees:
// the ported HUD's weapon icon (HudFrame item 24, art 41 + weapon) is a chain for weapon 0 and a nunchaku for weapon 4,
// and model 800 group 0 is four equal chain links (177 / 180 / 184 / 187) while group 4 is a 277 stick, two 100 links
// and a 419 stick.
constexpr uint32_t kWeaponTagEntry[9] = {95, 92, 96, 93, 91, 94, 97, 98, 99};
constexpr uint32_t kGrudgeMap = 0x800D38C8;     // the grudge slots' handle map (u8 x 20; weapon_session.cpp DevOpponentWeapons)
constexpr uint32_t kClassBlocks = 0x80052EE4;   // 36 bytes per bike class (fight.h): +18 CanEngage's speed share /128
constexpr long kRearmFrames = 480;              // a partner robbed of its weapon is armed again 8 s later

CheatSettings g_cheats;
bool g_inRace = false;
int g_weaponNow = -1;
std::vector<CheatWeapon> g_weapons;

// ---- per race
struct Partner {
    int slot = -1; // pool-0 slot
    int side = 1;  // +1 / -1: the lateral offset's sign
    uint8_t hp = 0, ceil = 0; // its health / ceiling when picked (a passive partner is kept there)
    int armed = -1;           // the weapon the cheat gave it (-1 none yet)
    long lost = -1;           // the frame its weapon was taken from it (a steal), -1 not
};
struct Run {
    long frame = 0;
    bool started = false;
    uint32_t policeStart = 0, trafficStart = 0;
    bool policeZeroed = false, trafficZeroed = false;
    int startWeapon = -1;
    uint8_t hp0[2] = {0, 0}, ceil0[2] = {0, 0}, bike0[2] = {0, 0};
    int32_t timeLeft = 0;
    bool frozen = false;
    Partner partners[2];
    std::vector<bool> copLive;
    // tallies
    uint64_t weaponGiven = 0, weaponDeferred = 0, swingsTopped = 0;
    long weaponFirstFrame = -1;
    int weaponFirstId = -1;
    uint64_t policeFrames = 0, copReleases = 0, copTailsGivenUp = 0;
    int copsLiveMax = 0;
    uint64_t trafficFrames = 0;
    int carsMax = 0;
    uint64_t passiveRefused = 0, passiveEnded = 0, passiveStrikes = 0;
    uint64_t partnersAssigned = 0, stoodFrames = 0, reachFrames = 0, lifts = 0, partnerRestores = 0;
    uint64_t partnerArms = 0, partnerTopUps = 0, partnerRobbed = 0, standingLifts = 0, partnerFightFrames = 0;
    long reachRun = 0, reachRunMax = 0;
    uint64_t godRestores = 0;
    int godLowest = -1;
    std::string godAt; // the first restores: frame:health/ceiling
    uint64_t nitroTopUps = 0, frozenFrames = 0;
    uint64_t heldFrames[9] = {};          // frames the player's seat-0 object was weapon k's (LOD k) with +0x2E = k
    uint64_t seatedFrames = 0, godHeldFrames = 0; // god: frames seated / of them at the race-start health after the frame
    int32_t leftStart = -1, leftEnd = -1; // the time limit's ticks left at the start / after the last frame (no limit: -1)
};
bool g_report = false;
Run g_run;

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }

int Players(GuestRam& g) {
    const int32_t n = g.S32(g.U32(kGameStatePtr) + 48u);
    return n < 1 ? 1 : (n > 2 ? 2 : n);
}
uint32_t Pool(GuestRam& g) { return g.U32(kPool0Ptr); }
uint32_t Rd(GuestRam& g, uint32_t e) { return g.U32(e + 0x43Cu); }
bool IsCop(GuestRam& g, uint32_t e) { return (g.U8(Rd(g, e) + 1u) & 0xFu) == 2u; }
uint32_t TopOp(GuestRam& g, uint32_t e) {
    const int32_t depth = g.S8(e + 0x3B2u);
    if (depth <= 0) return 0;
    return g.U16(e + 0x3B4u + 8u * U(depth));
}

// A rival that may be held as a sparring partner: live, AI-driven, not a player or a cop, racing, seated.
bool Eligible(GuestRam& g, uint32_t e, int players) {
    if (e == 0) return false;
    if (g.S16(e + 0x140u) == 0) return false;
    if (!(g.U32(e + 0x230u) & 0x08000000u)) return false;
    if (g.U16(e + 0xACu) < static_cast<uint32_t>(players)) return false;
    if (IsCop(g, e)) return false;
    const uint32_t rd = Rd(g, e);
    if (g.S32(rd + 0x28u) != 0 || g.U8(rd + 0x27u) >= 248u) return false;
    if (g.U32(g.U32(e + 0x354u) + 0x25Cu) >= 2u) return false;
    return TopOp(g, e) != 18u;
}

// FightUpdate 0x800C035C's two geometric gates for `me` fighting `t` (fight.cpp 1099..1152).
void FightGeometry(GuestRam& g, uint32_t me, uint32_t t, int32_t& along, int32_t& lat) {
    along = rr::sim::cu::GProject(g, me + 504u, t + 528u, t + 504u);
    const uint32_t mr = g.U32(me + 0x168u);
    if (mr == g.U32(t + 0x168u) && ((mr >> 16) == 0 || g.U32(me + 0x150u) == g.U32(t + 0x150u))) {
        lat = static_cast<int32_t>(g.U32(me + 344u) - g.U32(t + 344u));
        if (g.S32(t + 364u) < 0) lat = static_cast<int32_t>(0u - U(lat));
    } else {
        lat = rr::sim::cu::GProject(g, me + 184u, t + 432u, t + 184u);
    }
}
bool InReach(GuestRam& g, int32_t along, int32_t lat) {
    const int64_t half = g.S32(kFightLatHalf);
    return std::llabs(static_cast<int64_t>(lat)) <= 2 * half && std::llabs(static_cast<int64_t>(along)) <= kFightAlong;
}

// AimBesideRider's one callee, SetAimDelta 0x80093CAC(e, 0, e + 0x368, &scalar, 1) (PORTED, ai.h).
struct AimCallees final : rr::sim::AiCmdCallees {
    uint8_t* ram;
    explicit AimCallees(uint8_t* r) : ram(r) {}
    bool SetAimDelta(uint32_t e, int32_t scalar, uint32_t) override {
        uint8_t* ep = ram + (e & 0x1FFFFFu);
        rr::sim::SetAimDelta(rr::sim::EntityView(ep), nullptr, reinterpret_cast<const int16_t*>(ep + 0x368),
                             &scalar, 1, reinterpret_cast<const int16_t*>(ram + (kSqrtTable & 0x1FFFFFu)));
        return true;
    }
    bool CmdRace(uint32_t, uint32_t) override { return false; }
    bool PopCommand(uint32_t, uint32_t) override { return false; }
    bool StanceEvent(uint32_t, uint32_t, uint32_t, uint32_t) override { return false; }
    bool CopIdle(uint32_t, uint32_t, int32_t, uint32_t) override { return false; }
    bool CopRelease(uint32_t, uint32_t) override { return false; }
    bool LeaveRace(uint32_t, int32_t, uint32_t) override { return false; }
    bool Fight(uint32_t, uint32_t, int32_t, uint32_t) override { return false; }
    bool Intercept(uint32_t, uint32_t, uint32_t) override { return false; }
    bool Strike(uint32_t, uint32_t, uint32_t) override { return false; }
    bool JailbreakFinish(uint32_t) override { return false; }
    bool CopGap(uint32_t, uint32_t, uint32_t, int32_t&) override { return false; }
};

// The record fields WeaponSteal 0x800BFF04 moves (rules.md 1.4 / 8.3), as weapon_session.cpp's GiveWeapon writes them.
void WriteWeapon(GuestRam& g, uint32_t rd, int weapon, int swings) {
    const uint32_t sw = static_cast<uint32_t>(std::clamp(swings, 0, kSwingsFull));
    g.W8(rd + 0x2Eu, static_cast<uint8_t>(weapon));
    g.W8(rd + 0x2Fu, static_cast<uint8_t>(sw));
    if (weapon >= 9) return;
    g.W16(rd + 0x2Cu, static_cast<uint16_t>(g.U16(rd + 0x2Cu) | (1u << weapon)));
    if (weapon < 8) {
        const uint32_t sh = 4u * static_cast<uint32_t>(weapon);
        g.W32(rd + 0x30u, (g.U32(rd + 0x30u) & ~(15u << sh)) | (sw << sh));
    }
}

bool IsPartner(GuestRam& g, uint32_t e) {
    const uint32_t pool = Pool(g);
    for (const Partner& q : g_run.partners)
        if (q.slot >= 0 && pool != 0 && e == pool + kSlot * U(q.slot)) return true;
    return false;
}

// "Partners attack": each partner's record as a career rider's brings it into a race (cheats.h).
void ArmPartners(GuestRam& g) {
    const CheatSettings& c = g_cheats;
    const uint32_t pool = Pool(g);
    if (pool == 0) return;
    const uint32_t ph = g.U16(pool + 0xACu) & 0xFFu; // the player's handle (slot 0)
    for (Partner& q : g_run.partners) {
        if (q.slot < 0) continue;
        const uint32_t e = pool + kSlot * U(q.slot), rd = Rd(g, e), r = g.U32(e + 0x354u);
        if (TopOp(g, e) == 16u) ++g_run.partnerFightFrames;
        // the grudge against the player at its full value and the mood nibble raised (AiChooseCommand's fight arms)
        for (uint32_t k = 0; k < 20; ++k)
            if (g.U8(kGrudgeMap + k) == ph) g.W8(rd + 0x10u + k, 127);
        g.W8(rd + 0x02u, static_cast<uint8_t>((g.U8(rd + 0x02u) & 0xF0u) | 0x0Fu));
        const int w = c.sparringWeapon;
        if (w < 0) continue;                          // their own weapons: the record as it is
        if (g.U8(r + 0x23Bu) != 0xFFu) continue;      // an object in its hand (mid-swing): the fields as they are
        const uint8_t move = w >= 9 ? 32 : (w >= 6 ? 148 : 142); // CombatDecode's commands: punch / armed swing / 6..8
        for (uint32_t k = 0; k < 8; ++k) g.W8(rd + 0x34u + k, move);
        if (w >= 9) { // fists: nothing owned, nothing in hand
            if ((g.U16(rd + 0x2Cu) & 0x1FFu) != 0 || g.U8(rd + 0x2Eu) != 9) {
                g.W16(rd + 0x2Cu, static_cast<uint16_t>(g.U16(rd + 0x2Cu) & ~0x1FFu));
                g.W8(rd + 0x2Eu, 9);
                g.W8(rd + 0x2Fu, 0);
                ++g_run.partnerArms;
            }
            q.armed = 9;
            continue;
        }
        const bool holds = ((g.U16(rd + 0x2Cu) >> w) & 1u) != 0;
        if (q.armed == w && !holds) { // taken from it (WeaponSteal clears the victim's owned bit): it stays taken a while
            if (q.lost < 0) {
                q.lost = g_run.frame;
                ++g_run.partnerRobbed;
                std::printf("cheats: frame %ld - sparring partner b%d lost its %s (stolen); armed again in %ld frames\n",
                            g_run.frame, q.slot, CheatWeaponName(w).c_str(), kRearmFrames);
            }
            if (g_run.frame - q.lost < kRearmFrames) continue;
        }
        if (q.armed != w || !holds) { // W alone owned, in hand, full swings
            g.W16(rd + 0x2Cu, static_cast<uint16_t>(g.U16(rd + 0x2Cu) & ~0x1FFu));
            WriteWeapon(g, rd, w, kSwingsFull);
            q.armed = w;
            q.lost = -1;
            ++g_run.partnerArms;
        } else if (g.U8(rd + 0x2Eu) == w && g.U8(rd + 0x2Fu) < kSwingsFull) {
            WriteWeapon(g, rd, w, kSwingsFull);
            ++g_run.partnerTopUps;
        }
    }
}

} // namespace

// ============================================================================ settings
bool CheatSettings::AnyOn() const {
    return weapon >= 0 || unlimitedSwings || noPolice || noTraffic || sparring != 0 || rivalsPassive || god ||
           infiniteNitro || freezeTimer;
}

bool CheatSettings::Parse(const std::string& key, const std::string& value) {
    auto flag = [&](bool& b) {
        if (value == "1" || value == "on" || value == "true") return b = true, true;
        if (value == "0" || value == "off" || value == "false") return b = false, true;
        return false;
    };
    if (key == "weapon") {
        if (value == "off" || value == "-1") return weapon = -1, true;
        char* end = nullptr;
        const long v = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || *end != '\0' || v < 0 || v > 8) return false;
        weapon = static_cast<int>(v);
        return true;
    }
    if (key == "sparring") {
        if (value == "off" || value == "0") return sparring = 0, true;
        if (value == "passive" || value == "1") return sparring = 1, true;
        if (value == "fight" || value == "2") return sparring = 2, true;
        return false;
    }
    if (key == "sparring_weapon") {
        if (value == "own" || value == "-1") return sparringWeapon = -1, true;
        if (value == "fists" || value == "9") return sparringWeapon = 9, true;
        char* end = nullptr;
        const long v = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || *end != '\0' || v < 0 || v > 8) return false;
        sparringWeapon = static_cast<int>(v);
        return true;
    }
    if (key == "sparring_attack") return flag(sparringAttack);
    if (key == "sparring_standing") return flag(sparringStanding);
    if (key == "unlimited_swings") return flag(unlimitedSwings);
    if (key == "no_police") return flag(noPolice);
    if (key == "no_traffic") return flag(noTraffic);
    if (key == "rivals_passive") return flag(rivalsPassive);
    if (key == "god") return flag(god);
    if (key == "infinite_nitro") return flag(infiniteNitro);
    if (key == "freeze_timer") return flag(freezeTimer);
    return false;
}

std::string CheatSettings::Serialize() const {
    static const char* const kSpar[3] = {"off", "passive", "fight"};
    std::string s;
    s += "weapon=" + (weapon < 0 ? std::string("off") : std::to_string(weapon)) + "\n";
    s += "unlimited_swings=" + std::to_string(unlimitedSwings ? 1 : 0) + "\n";
    s += "no_police=" + std::to_string(noPolice ? 1 : 0) + "\n";
    s += "no_traffic=" + std::to_string(noTraffic ? 1 : 0) + "\n";
    s += "sparring=" + std::string(kSpar[std::clamp(sparring, 0, 2)]) + "\n";
    s += "sparring_attack=" + std::to_string(sparringAttack ? 1 : 0) + "\n";
    s += "sparring_weapon=" + (sparringWeapon < 0 ? std::string("own") : sparringWeapon >= 9 ? std::string("fists") : std::to_string(sparringWeapon)) + "\n";
    s += "sparring_standing=" + std::to_string(sparringStanding ? 1 : 0) + "\n";
    s += "rivals_passive=" + std::to_string(rivalsPassive ? 1 : 0) + "\n";
    s += "god=" + std::to_string(god ? 1 : 0) + "\n";
    s += "infinite_nitro=" + std::to_string(infiniteNitro ? 1 : 0) + "\n";
    s += "freeze_timer=" + std::to_string(freezeTimer ? 1 : 0) + "\n";
    return s;
}

std::string CheatSettings::Describe() const {
    if (!AnyOn()) return "off";
    static const char* const kSpar[3] = {"off", "passive", "fight back"};
    std::string s;
    auto add = [&](const std::string& x) { s += (s.empty() ? "" : ", ") + x; };
    if (weapon >= 0) add("weapon " + CheatWeaponName(weapon));
    if (unlimitedSwings) add("unlimited swings");
    if (noPolice) add("no police");
    if (noTraffic) add("no traffic");
    if (sparring != 0) {
        std::string x = std::string("sparring ") + kSpar[std::clamp(sparring, 0, 2)];
        if (sparringAttack) {
            x += ", partners attack with ";
            x += sparringWeapon < 0 ? std::string("their own weapons") : sparringWeapon >= 9 ? std::string("fists") : CheatWeaponName(sparringWeapon);
            if (sparringStanding) x += ", even a standing player (cheat rule)";
        }
        add(x);
    }
    if (rivalsPassive) add("rivals passive");
    if (god) add("god mode");
    if (infiniteNitro) add("infinite nitro");
    if (freezeTimer) add("time limit frozen");
    return s;
}

CheatSettings& Cheats() { return g_cheats; }
void SetCheatsInRace(bool inRace) { g_inRace = inRace; }
bool CheatsInRace() { return g_inRace; }
void RequestWeaponNow(int w) { g_weaponNow = w; }

// ============================================================================ the weapons, from the disc
std::vector<CheatWeapon> ReadCheatWeapons(const rr::DiscImage& disc, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return std::vector<CheatWeapon>{};
    };
    const auto exeFile = disc.Find("SLUS_010.53");
    const auto shellFile = disc.Find("RASHCDF.BIN");
    if (!exeFile || !shellFile) return fail("the disc carries no SLUS_010.53 / RASHCDF.BIN");
    const std::vector<uint8_t> exe = disc.ReadFile(*exeFile), shell = disc.ReadFile(*shellFile);
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) return fail("SLUS_010.53 is not a PS-X EXE");
    uint32_t text = 0;
    std::memcpy(&text, exe.data() + 0x18, 4); // the header's t_addr
    // PickWeapon 0x800B9340's preference rows, one per fighter class: the weapon ids the game chooses from.
    std::set<int> ids;
    std::set<int> first;
    for (uint32_t c = 0; c < 3; ++c) {
        const uint64_t at = static_cast<uint64_t>(kBexeWord) + 36u * c - text + 0x800u;
        if (kBexeWord < text || at + 9 > exe.size()) return fail("SLUS_010.53 is shorter than its weapon rows");
        std::set<int> row;
        for (uint32_t k = 0; k < 9; ++k) row.insert(exe[static_cast<size_t>(at + k)]);
        if (c == 0) first = row;
        else if (row != first) return fail("the fighter classes' weapon rows name different weapons");
        ids.insert(row.begin(), row.end());
    }
    std::vector<CheatWeapon> out;
    for (const int id : ids) {
        if (id < 0 || id > 8) return fail("a weapon row names id " + std::to_string(id) + " (the record holds 0..8)");
        const uint64_t at = static_cast<uint64_t>(kTagTable) - kShellLoad + 4u * kWeaponTagEntry[id];
        if (at + 4 > shell.size()) return fail("RASHCDF.BIN is shorter than its art tag table");
        CheatWeapon w;
        w.id = id;
        for (uint32_t k = 0; k < 4; ++k) {
            const char ch = static_cast<char>(shell[static_cast<size_t>(at + k)]);
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return fail("the art tag of weapon " + std::to_string(id) + " is not a tag");
            w.tag.push_back(ch);
        }
        out.push_back(w);
    }
    return out;
}

void SetCheatWeapons(std::vector<CheatWeapon> list) { g_weapons = std::move(list); }
const std::vector<CheatWeapon>& CheatWeapons() { return g_weapons; }
std::string CheatWeaponName(int id) {
    for (const CheatWeapon& w : g_weapons)
        if (w.id == id) return w.tag + " (weapon " + std::to_string(id) + ")";
    return "weapon " + std::to_string(id);
}

// ============================================================================ the race
void CheatRaceStart(RaceSession& s) {
    g_run = Run{};
    g_weaponNow = -1;
    GuestRam g(s.MutableArenaRam(), kGp);
    g_run.policeStart = g.U32(kPoliceSwitch);
    g_run.trafficStart = g.U32(kTrafficSwitch);
    g_run.startWeapon = g_cheats.weapon;
    const size_t players = std::min<size_t>(static_cast<size_t>(std::max(1, s.Players())), 2);
    for (size_t p = 0; p < players && p < s.Bikes().size(); ++p) {
        g_run.hp0[p] = g.U8(s.Bikes()[p].riderDefAddress + 0x0Fu);
        g_run.ceil0[p] = g.U8(s.Bikes()[p].riderDefAddress + 0x0Eu);
        g_run.bike0[p] = g.U8(s.Bikes()[p].riderDefAddress + 0x25u);
    }
    g_run.copLive.assign(s.Bikes().size(), false);
    if (const int32_t limit = g.S32(kTimeLimit); limit != 0) g_run.leftStart = limit - g.S32(g.U32(kGameStatePtr) + 0x10u);
    g_run.started = true;
    if (g_cheats.AnyOn()) std::printf("cheats: %s\n", g_cheats.Describe().c_str());
}

namespace {

void GodRestore(RaceSession& s, GuestRam& g) {
    const size_t players = std::min<size_t>(static_cast<size_t>(std::max(1, s.Players())), 2);
    for (size_t p = 0; p < players && p < s.Bikes().size(); ++p) {
        const uint32_t rd = s.Bikes()[p].riderDefAddress;
        // off the bike (mount state >= 2: thrown, walking) the game keeps its own books on +0x0F (a crash zeroes it,
        // the remount restores it) - only a seated rider's health and ceiling are put back
        if (g.U32(g.U32(s.Bikes()[p].entityAddress + 0x354u) + 0x25Cu) >= 2u) continue;
        const uint8_t hp = g.U8(rd + 0x0Fu), ceil = g.U8(rd + 0x0Eu);
        if (hp < g_run.hp0[p] || ceil < g_run.ceil0[p]) {
            if (g_run.godLowest < 0 || hp < g_run.godLowest) g_run.godLowest = hp;
            if (++g_run.godRestores <= 8)
                g_run.godAt += " f" + std::to_string(g_run.frame) + ":" + std::to_string(hp) + "/" + std::to_string(ceil);
        }
        if (hp < g_run.hp0[p]) g.W8(rd + 0x0Fu, g_run.hp0[p]);
        if (ceil < g_run.ceil0[p]) g.W8(rd + 0x0Eu, g_run.ceil0[p]);
        if (g.U8(rd + 0x25u) < g_run.bike0[p]) g.W8(rd + 0x25u, g_run.bike0[p]); // the HUD's bike condition
    }
}

} // namespace

void CheatBeforeFrame(RaceSession& s) {
    if (!g_run.started) CheatRaceStart(s);
    ++g_run.frame;
    const CheatSettings& c = g_cheats;
    GuestRam g(s.MutableArenaRam(), kGp);
    // the switches: zeroed while the cheat is on, the race's own value back when it is turned off
    if (c.noPolice) {
        g.W32(kPoliceSwitch, 0);
        g_run.policeZeroed = true;
        ++g_run.policeFrames;
    } else if (g_run.policeZeroed) {
        g.W32(kPoliceSwitch, g_run.policeStart);
        g_run.policeZeroed = false;
    }
    if (c.noTraffic) {
        g.W32(kTrafficSwitch, 0);
        g_run.trafficZeroed = true;
        ++g_run.trafficFrames;
    } else if (g_run.trafficZeroed) {
        g.W32(kTrafficSwitch, g_run.trafficStart);
        g_run.trafficZeroed = false;
    }
    // the weapon: the race's start weapon once, the menu's "now" when asked
    if (!s.Bikes().empty()) {
        const RaceBike& p = s.Bikes()[0];
        int want = g_weaponNow >= 0 ? g_weaponNow : g_run.startWeapon;
        if (want >= 0) {
            if (g.U8(p.ownerAddress + 0x23Bu) != 0xFFu) { // a weapon object in the hand (mid-swing): next frame
                ++g_run.weaponDeferred;
            } else {
                WriteWeapon(g, p.riderDefAddress, want, kSwingsFull);
                ++g_run.weaponGiven;
                if (g_run.weaponFirstFrame < 0) {
                    g_run.weaponFirstFrame = g_run.frame;
                    g_run.weaponFirstId = want;
                }
                if (g_weaponNow >= 0) g_weaponNow = -1;
                else g_run.startWeapon = -1;
            }
        }
        if (c.unlimitedSwings) {
            const uint32_t rd = p.riderDefAddress;
            const int w = g.U8(rd + 0x2Eu);
            if (w < 9 && g.U8(rd + 0x2Fu) < kSwingsFull) {
                WriteWeapon(g, rd, w, kSwingsFull);
                ++g_run.swingsTopped;
            }
        }
    }
    if (c.god) GodRestore(s, g);
    if (c.sparring == 1) { // passive partners are punching bags: their health is put back before every frame too
        const uint32_t pool = Pool(g);
        for (const Partner& q : g_run.partners) {
            if (q.slot < 0 || pool == 0) continue;
            const uint32_t e = pool + kSlot * U(q.slot), rd = Rd(g, e);
            if (g.U32(g.U32(e + 0x354u) + 0x25Cu) >= 2u) continue;
            if (g.U8(rd + 0x0Fu) < kBagHealth) {
                if (g.U8(rd + 0x0Fu) < q.hp) ++g_run.partnerRestores; // a blow landed since the last frame
                g.W8(rd + 0x0Fu, kBagHealth);
            }
            if (g.U8(rd + 0x0Eu) < q.ceil) g.W8(rd + 0x0Eu, q.ceil);
        }
    }
    if (c.sparring != 0 && c.sparringAttack) ArmPartners(g);
    if (c.infiniteNitro) {
        const size_t players = std::min<size_t>(static_cast<size_t>(std::max(1, s.Players())), 2);
        for (size_t p = 0; p < players && p < s.Bikes().size(); ++p) {
            const uint32_t e = s.Bikes()[p].entityAddress;
            if (g.S8(e + 0x350u) < kNitroKeep) {
                g.W8(e + 0x350u, static_cast<uint8_t>(kNitroKeep));
                ++g_run.nitroTopUps;
            }
        }
    }
    if (c.freezeTimer) {
        const int32_t limit = g.S32(kTimeLimit), clock = g.S32(g.U32(kGameStatePtr) + 0x10u);
        if (limit != 0) {
            if (!g_run.frozen) {
                g_run.timeLeft = limit - clock;
                g_run.frozen = true;
            }
            g.W32(kTimeLimit, U(clock + g_run.timeLeft));
            ++g_run.frozenFrames;
        }
    } else {
        g_run.frozen = false;
    }
}

void CheatAfterAi(uint8_t* ram, uint32_t gp) {
    Partner* const pa = g_run.partners;
    if (g_cheats.sparring == 0) {
        pa[0] = pa[1] = Partner{};
        return;
    }
    GuestRam g(ram, gp);
    const int players = Players(g);
    const uint32_t pool = Pool(g);
    const int32_t n = std::clamp(g.S32(kLiveCount), 0, 64);
    const uint32_t p = pool; // player 1's bike, slot 0
    if (pool == 0 || g.S16(p + 0x140u) == 0) return;
    for (Partner& q : g_run.partners)
        if (q.slot >= 0 && (q.slot >= n || !Eligible(g, pool + kSlot * U(q.slot), players))) q = Partner{};
    // the candidates: eligible, not a partner, within range; the key is the distance, ahead of him last
    struct Cand {
        int slot;
        int64_t key;
        int32_t along, lat;
    };
    std::vector<Cand> cands;
    for (int32_t i = players; i < n; ++i) {
        if (i == pa[0].slot || i == pa[1].slot) continue;
        const uint32_t e = pool + kSlot * U(i);
        if (!Eligible(g, e, players)) continue;
        const int64_t dx = std::llabs(static_cast<int64_t>(g.S32(e + 0xB8u)) - g.S32(p + 0xB8u)) >> 16;
        const int64_t dz = std::llabs(static_cast<int64_t>(g.S32(e + 0xC0u)) - g.S32(p + 0xC0u)) >> 16;
        const int64_t d = std::max(dx, dz) + std::min(dx, dz) / 2;
        if (d > kPickRange) continue;
        Cand c{i, d, 0, 0};
        FightGeometry(g, e, p, c.along, c.lat);
        if (c.along > kPickAhead) c.key += 1000;
        cands.push_back(c);
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.key < b.key || (a.key == b.key && a.slot < b.slot); });
    const bool standing = g.S32(p + 0x240u) < kStood;
    for (Partner& q : g_run.partners) { // a partner stuck ahead of a standing player gives way to a rider behind him
        if (q.slot < 0 || !standing || cands.empty() || cands.front().along > kPickAhead) continue;
        int32_t along = 0, lat = 0;
        FightGeometry(g, pool + kSlot * U(q.slot), p, along, lat);
        if (along > kDropAhead) q = Partner{};
    }
    // pick: the nearest eligible riders within 60 units (one ahead of a standing player stops where it is)
    size_t next = 0;
    for (int k = 0; k < 2; ++k) {
        if (pa[k].slot >= 0) continue;
        while (next < cands.size() && (cands[next].slot == pa[0].slot || cands[next].slot == pa[1].slot)) ++next;
        if (next >= cands.size()) break;
        const Cand& c = cands[next++];
        pa[k].slot = c.slot;
        pa[k].side = c.lat >= 0 ? 1 : -1;
        const uint32_t rd = Rd(g, pool + kSlot * U(c.slot));
        pa[k].hp = kBagHealth;
        pa[k].ceil = g.U8(rd + 0x0Eu);
        if (pa[1 - k].slot >= 0 && pa[1 - k].side == pa[k].side) pa[k].side = -pa[k].side; // one on each side
        ++g_run.partnersAssigned;
    }
    // hold: the commanded speed closes the along gap at the player's pace; the aim slides beside him
    const bool seated = g.U32(g.U32(p + 0x354u) + 0x25Cu) < 2u;
    const double pv = seated ? std::max(0, g.S32(p + 0x240u)) / 65536.0 : 0.0;
    AimCallees aim(ram);
    for (const Partner& q : g_run.partners) {
        if (q.slot < 0) continue;
        const uint32_t e = pool + kSlot * U(q.slot);
        int32_t along = 0, lat = 0;
        FightGeometry(g, e, p, along, lat);
        // the gap to close (units, + = the partner behind where it should be), then a speed that can still stop in
        // it at 12 units/s/s (the bike step's own brake limit does the braking), linear close in
        const double err = (static_cast<double>(kPartnerAhead) - along) / 65536.0;
        const double close = err > 0.5 ? std::sqrt(2.0 * 12.0 * err) : (err < -0.5 ? -std::sqrt(2.0 * 12.0 * -err) : 2.0 * err);
        const double top = g.S32(g.U32(e + 0x22Cu) + 0xE0u) / 65536.0;
        const double v = std::clamp(pv + close, 0.0, std::max(top, pv));
        g.W32(e + 0x39Cu, U(static_cast<int32_t>(v * 65536.0)));
        g.W32(e + 0x234u, g.U32(e + 0x234u) & ~0x200u); // no start boost for a partner (bike step A1)
        if (g.U32(e + 0x168u) == g.U32(p + 0x168u))
            rr::sim::AiAimBeside(g, e, p, q.side * kPartnerSide, kCheatSp, aim);
    }
    if (g.Faulted()) g.ClearFault();
}

void CheatAfterFrame(RaceSession& s) {
    const CheatSettings& c = g_cheats;
    GuestRam g(s.MutableArenaRam(), kGp);
    if (c.god) GodRestore(s, g);
    // the police: grid cops that came alive (PoliceSched's release)
    int live = 0;
    for (size_t i = 0; i < s.Bikes().size() && i < g_run.copLive.size(); ++i) {
        if (!s.Bikes()[i].isCop) continue;
        const bool now = s.BikeLive(i);
        if (now) ++live;
        if (now && !g_run.copLive[i]) ++g_run.copReleases;
        g_run.copLive[i] = now;
    }
    g_run.copsLiveMax = std::max(g_run.copsLiveMax, live);
    g_run.carsMax = std::max(g_run.carsMax, static_cast<int>(s.Log().carsLive));
    if (!s.Bikes().empty()) {
        const RaceBike& pb = s.Bikes()[0];
        WeaponInHand hand;
        if (ReadWeaponInHand(s.ArenaRam(), pb.ownerAddress, hand) && hand.weapon >= 0 && hand.weapon < 9 &&
            hand.group == hand.weapon)
            ++g_run.heldFrames[hand.weapon];
        if (g.U32(pb.ownerAddress + 0x25Cu) < 2u) {
            ++g_run.seatedFrames;
            if (g.U8(pb.riderDefAddress + 0x0Fu) == g_run.hp0[0]) ++g_run.godHeldFrames;
        }
    }
    if (c.freezeTimer && g_run.frozen && g.S32(kTimeLimit) != 0) // the frame's ticks moved the clock: move the limit too
        g.W32(kTimeLimit, U(g.S32(g.U32(kGameStatePtr) + 0x10u) + g_run.timeLeft));
    if (const int32_t limit = g.S32(kTimeLimit); limit != 0) g_run.leftEnd = limit - g.S32(g.U32(kGameStatePtr) + 0x10u);
    // the sparring partners: FightUpdate's reach while the player stands
    if (c.sparring != 0 && !s.Bikes().empty()) {
        const uint32_t p = s.Bikes()[0].entityAddress;
        if (g.S32(p + 0x240u) < kStood) {
            ++g_run.stoodFrames;
            bool any = false;
            for (const Partner& q : g_run.partners) {
                if (q.slot < 0) continue;
                int32_t along = 0, lat = 0;
                FightGeometry(g, Pool(g) + kSlot * U(q.slot), p, along, lat);
                any = any || InReach(g, along, lat);
            }
            if (any) {
                ++g_run.reachFrames;
                g_run.reachRunMax = std::max(g_run.reachRunMax, ++g_run.reachRun);
            } else {
                g_run.reachRun = 0;
            }
        } else {
            g_run.reachRun = 0;
        }
    }
    if (g.Faulted()) g.ClearFault();
}

bool CheatCopGivesUp() { return g_cheats.noPolice; }

bool CheatRivalPassive(GuestRam& g, uint32_t e, uint32_t t) {
    if (e == 0 || t == 0) return false;
    if (g_cheats.sparring != 0 && g_cheats.sparringAttack && IsPartner(g, e)) return false; // it attacks
    bool on = g_cheats.rivalsPassive;
    if (!on && g_cheats.sparring == 1) {
        const uint32_t pool = Pool(g);
        for (const Partner& q : g_run.partners)
            if (q.slot >= 0 && e == pool + kSlot * U(q.slot)) on = true;
    }
    if (!on) return false;
    const int players = Players(g);
    if (g.U16(e + 0xACu) < static_cast<uint32_t>(players)) return false; // a player
    if (IsCop(g, e)) return false;                                        // the police: the police cheat's
    return g.U16(t + 0xACu) < static_cast<uint32_t>(players);            // against a player only
}

CheatLift CheatSparringLift(GuestRam& g, uint32_t e, uint32_t t) {
    CheatLift lift;
    if (g_cheats.sparring == 0 || e == 0 || t == 0) return lift;
    if (g.U16(e + 0xACu) >= static_cast<uint32_t>(Players(g))) return lift; // the player's blow only
    bool partner = false;
    const uint32_t pool = Pool(g);
    for (const Partner& q : g_run.partners)
        if (q.slot >= 0 && t == pool + kSlot * U(q.slot)) partner = true;
    if (!partner) return lift;
    const uint32_t r = g.U32(t + 0x354u);
    if (g.U32(r + 0x25Cu) != 0u) return lift;
    lift.rider = r;
    lift.stance = g.U16(r + 0x220u);
    g.W32(r + 0x25Cu, 1);
    ++g_run.lifts;
    return lift;
}

void CheatSparringDrop(GuestRam& g, const CheatLift& lift) {
    if (lift.rider == 0) return;
    if (g.U16(lift.rider + 0x220u) == lift.stance && g.U32(lift.rider + 0x25Cu) == 1u) g.W32(lift.rider + 0x25Cu, 0);
}

CheatSpeedLift CheatStandingLift(GuestRam& g, uint32_t e, uint32_t t) {
    CheatSpeedLift lift;
    const CheatSettings& c = g_cheats;
    if (c.sparring == 0 || !c.sparringAttack || !c.sparringStanding || e == 0 || t == 0) return lift;
    if (g.U16(t + 0xACu) >= static_cast<uint32_t>(Players(g))) return lift; // against a player only
    if (!IsPartner(g, e) || !(g.U32(e + 0x230u) & 0x08000000u)) return lift; // a partner, the AI arm of the rule
    if (g.U32(g.U32(t + 0x354u) + 0x25Cu) >= 2u) return lift;               // the player on (or beside) his bike
    // CanEngage 0x800BC3C4..0x800BC430: v = class[+18] x top >> 7 (a negative product rounded toward zero)
    const uint32_t k = g.U8(kClassBlocks + 36u * (g.U8(Rd(g, e) + 1u) & 0xFu) + 18u);
    int32_t v = static_cast<int32_t>(static_cast<uint32_t>(k) * static_cast<uint32_t>(g.S32(g.U32(e + 0x22Cu) + 224u)));
    if (v < 0) v += 127;
    v >>= 7;
    const int32_t was = g.S32(t + 0x1E0u);
    if (was >= v) return lift;
    g.W32(t + 0x1E0u, U(v));
    lift.bike = t;
    lift.was = was;
    lift.lifted = v;
    ++g_run.standingLifts;
    return lift;
}

void CheatStandingDrop(GuestRam& g, const CheatSpeedLift& lift) {
    if (lift.bike == 0) return;
    if (g.S32(lift.bike + 0x1E0u) == lift.lifted) g.W32(lift.bike + 0x1E0u, U(lift.was));
}

void CheatNotePassive(int what) {
    if (what == 0) ++g_run.passiveRefused;
    else if (what == 1) ++g_run.passiveEnded;
    else ++g_run.passiveStrikes;
}
void CheatNoteCopGaveUp() { ++g_run.copTailsGivenUp; }

std::string CheatFrameLog(const RaceSession& s) {
    if (!g_cheats.AnyOn() || s.Bikes().empty()) return "";
    GuestRam g(const_cast<uint8_t*>(s.ArenaRam()), kGp);
    const RaceBike& pb = s.Bikes()[0];
    char b[900];
    int n = std::snprintf(b, sizeof(b), "      cheats: police sw=%u traffic sw=%u | weapon w=%u s=%u obj=%d | hp=%u/%u nitro=%d | limit=%d",
                          g.U32(kPoliceSwitch), g.U32(kTrafficSwitch), g.U8(pb.riderDefAddress + 0x2Eu),
                          g.U8(pb.riderDefAddress + 0x2Fu), static_cast<int>(g.S8(pb.ownerAddress + 0x23Bu)),
                          g.U8(pb.riderDefAddress + 0x0Fu), g.U8(pb.riderDefAddress + 0x0Eu), g.S8(pb.entityAddress + 0x350u),
                          g.S32(kTimeLimit));
    for (const Partner& q : g_run.partners) {
        if (q.slot < 0 || n <= 0 || n >= static_cast<int>(sizeof(b))) continue;
        int32_t along = 0, lat = 0;
        const uint32_t e = g.U32(kPool0Ptr) + kSlot * U(q.slot);
        FightGeometry(g, e, pb.entityAddress, along, lat);
        n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " | partner b%d along=%.2f lat=%.2f v=%.1f op=%u mount=%u stance=%u w=%u own=%03X cmd=%u%s",
                           q.slot, along / 65536.0, lat / 65536.0, g.S32(e + 0x240u) / 65536.0, TopOp(g, e),
                           g.U32(g.U32(e + 0x354u) + 0x25Cu), g.U16(g.U32(e + 0x354u) + 0x220u), g.U8(Rd(g, e) + 0x2Eu),
                           g.U16(Rd(g, e) + 0x2Cu) & 0x7FFu, g.U8(Rd(g, e) + 0x3Cu), InReach(g, along, lat) ? " IN REACH" : "");
    }
    if (g.Faulted()) g.ClearFault();
    return std::string(b) + "\n";
}

void SetCheatReport(bool on) { g_report = on; }
bool CheatReport() { return g_report || g_cheats.AnyOn(); }

std::string CheatTotals() {
    const Run& r = g_run;
    std::string held;
    for (int k = 0; k < 9; ++k)
        if (r.heldFrames[k] != 0) held += " " + CheatWeaponName(k) + " x" + std::to_string(r.heldFrames[k]);
    if (held.empty()) held = " none";
    char b[2600];
    std::snprintf(b, sizeof(b),
                  "the cheats: %s; weapon: put in the hand %llu time(s) (first at frame %ld: %s), deferred "
                  "%llu frame(s) (an object in the hand), swings topped up %llu time(s); police: switch 0x8005ACC0 held 0 "
                  "on %llu frame(s), cop bikes released %llu (at most %d live), cop tails given up %llu; traffic: switch "
                  "0x8005ACC4 held 0 on %llu frame(s), at most %d car(s) live; passive: engagements refused %llu, fights "
                  "ended %llu, strikes skipped %llu; sparring: partners picked %llu, the player stood %llu frame(s), a "
                  "partner inside FightUpdate's reach on %llu of them (longest run %ld), the player's fight calls on a stopped partner %llu, a passive partner's health put back %llu time(s); partners attacking: armed %llu time(s), swings topped up %llu, robbed of their weapon %llu, frames in a fight %llu, the standing-player lifts %llu; god: health / ceiling put back "
                  "%llu time(s) (lowest health seen %d;%s), the player at his race-start health after %llu of %llu seated "
                  "frame(s); nitro topped up %llu time(s); time limit frozen %llu frame(s), ticks left %d at the start and "
                  "%d after the last frame; the player's hand held the weapon objects (frames):%s\n",
                  g_cheats.Describe().c_str(), static_cast<unsigned long long>(r.weaponGiven), r.weaponFirstFrame,
                  r.weaponFirstId >= 0 ? CheatWeaponName(r.weaponFirstId).c_str() : "-",
                  static_cast<unsigned long long>(r.weaponDeferred), static_cast<unsigned long long>(r.swingsTopped),
                  static_cast<unsigned long long>(r.policeFrames), static_cast<unsigned long long>(r.copReleases), r.copsLiveMax,
                  static_cast<unsigned long long>(r.copTailsGivenUp), static_cast<unsigned long long>(r.trafficFrames), r.carsMax,
                  static_cast<unsigned long long>(r.passiveRefused), static_cast<unsigned long long>(r.passiveEnded),
                  static_cast<unsigned long long>(r.passiveStrikes), static_cast<unsigned long long>(r.partnersAssigned),
                  static_cast<unsigned long long>(r.stoodFrames), static_cast<unsigned long long>(r.reachFrames), r.reachRunMax, static_cast<unsigned long long>(r.lifts),
                  static_cast<unsigned long long>(r.partnerRestores), static_cast<unsigned long long>(r.partnerArms),
                  static_cast<unsigned long long>(r.partnerTopUps), static_cast<unsigned long long>(r.partnerRobbed),
                  static_cast<unsigned long long>(r.partnerFightFrames), static_cast<unsigned long long>(r.standingLifts),
                  static_cast<unsigned long long>(r.godRestores), r.godLowest, r.godAt.c_str(),
                  static_cast<unsigned long long>(r.godHeldFrames), static_cast<unsigned long long>(r.seatedFrames),
                  static_cast<unsigned long long>(r.nitroTopUps), static_cast<unsigned long long>(r.frozenFrames), r.leftStart,
                  r.leftEnd, held.c_str());
    return b;
}

} // namespace rr::game
