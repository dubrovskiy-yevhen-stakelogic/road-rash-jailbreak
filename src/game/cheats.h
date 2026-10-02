#pragma once
// Cheats - OURS, a player's convenience like the gt2-play project's cheat menu:
// every cheat is OFF by default and reversible, a scripted run (a frame count, a gate) never reads the settings file
// (only its own --cheat-* flags), and each one acts at the place in the game where the thing it changes is decided:
//
//   Give weapon      the player's rider record riderDef +0x2C (owned mask), +0x2E (weapon in hand), +0x2F (swings)
//                    and +0x30 (the weapon's swing nibble) - the four fields WeaponSteal RASHCDG 0x800BFF04 moves
//                    from a victim to the thief. The object in the hand is then the game's own: CombatEnter
//                    0x800BFC5C / WeaponObject 0x800958F0 take it at the first swing, the HUD's icon reads +0x2E
//                    (HudFrame item 24), the renderer (weapon_draw.h, the VR hand) reads the seated object. Not
//                    written while the rider holds an object (+0x23B != 0xFF, mid-swing): deferred a frame.
//                    The list of weapons is the disc's (CheatWeapons): the ids of the fighter classes' preference
//                    rows SLUS 0x80052EEC (PickWeapon 0x800B9340's order) and the art tags of the front end's
//                    FourCC table RASHCDF 0x80088DF4 entries 91..99 in the rap sheet's order (weapon 0 is
//                    the chain, 4 the nunchaku - cheats.cpp kWeaponTagEntry).
//   No police        the police switch *(0x8005ACC0) = 0 - PoliceSched RASHCDG 0x8009E89C's own gate (0x8009E8A0),
//                    the switch Time Trial's "police" option and Jailbreak phase 4 clear - so no dormant cop is
//                    released; a cop already out gives up in the planner's cop tail (cop_race.cpp ProductCopTail:
//                    instead of CopTail 0x8009DC90, the tail's own "back to the release state" - clear, {4, 224},
//                    riderDef +0x28 = 0), so it never intercepts, fights or arrests.
//   No traffic       the traffic switch *(0x8005ACC4) = 0 - the car spawner's gate (population.h kPopTrafficOn),
//                    Time Trial's "traffic" option: no new car is spawned (the cars already out drive on).
//   Rivals passive   the planner's CanEngage 0x800BC1EC answers 0 for a rival (not a cop) against a player
//                    (ai_race.cpp): AiChooseCommand 0x800B8FB0 never issues a fight {16} or a strike {9}; a fight a
//                    rival is already in ends the way FightContinue 0x800C0BE8 ends one (AiPopCommand), a strike
//                    arm {9} does not strike.
//   Sparring         the two rivals nearest the player (not cops, within 60 units) are held
//                    beside him inside FightUpdate's reach (|along| <= 0.7, |across| <= 2 x *(0x80052F74)): after
//                    the AI's brain pass (inside RaceDirector, before the bike step) their commanded speed +0x39C
//                    closes the along gap to the player's pace and the PORTED AimBesideRider 0x800BC4FC slides
//                    their aim point 1.0 unit to either side of him; the start boost (+0x234 bit 9, which the
//                    bike step's A1 turns into +0x39C = 2 x top) is cleared for them. "Passive" partners are
//                    passive as above; "fight back" partners keep the original's fight rules, under which a rival
//                    engages only a player riding at >= class[+18]/128 of its own top speed (CanEngage) - so they
//                    hit back once the player rolls, never at a standstill (only cops attack a stopped player).
//                    A partner stopped with a foot down is presented as riding to the player's own FightUpdate only
//                    (CheatSparringLift below) - otherwise the original refuses every blow at a standstill. A passive
//                    partner is a punching bag: its health +0x0F is held at 255 before every frame (a blow lands and
//                    hurts - the fight log shows it - but even a weapon's strongest node does not knock it off, so it
//                    stays; the HUD's bar clamps at full); a fight-back partner takes damage by the original's rules.
//   Partners attack  the sparring partners fight the player: each partner's record is given what a
//                    career rider's record brings into a race (the --opponent-weapons edit, weapon_session.cpp) - a
//                    grudge of 127 against the player (the grudge slot whose handle map 0x800D38C8 entry is his) and
//                    the mood nibble +0x02 at 15, so the planner's AiChooseCommand 0x800B8FB0 takes a fight arm - and
//                    a passive partner is not passive against him (it stays a punching bag for health). "Partners'
//                    weapon": their own (the record as it is), fists (owned mask +0x2C cleared, moves +0x34..+0x3B
//                    the bare punch 32) or weapon W (owned mask = W alone, W in hand, 15 swings, the moves the armed
//                    swing - 142, or 148 for W 6..8 as CombatDecode issues them), topped up while held; a weapon the
//                    player steals stays stolen for 8 s, then the partner is armed again. PickWeapon 0x800B9340,
//                    FightRestart and the rest of the fight are the original's.
//   Standing player  CHEAT RULE, OFF by default, only with "partners attack": the original never lets a
//                    rival engage a player slower than class[+18]/128 of the rival's top speed (CanEngage 0x800BC1EC,
//                    the AI-bit arm - the planner's and FightUpdate's) - a standing player is never attacked. Around
//                    exactly those two calls for a partner against the standing player, the player's speed +0x1E0
//                    reads as that threshold (put back after the call unless the call wrote it).
//   God mode         the players' health riderDef +0x0F, its ceiling +0x0E and the bike's condition +0x25 (HudFrame's
//                    Condition 0x80060D94 reads +0x25 over +0x24) put back to their race-start values before and after
//                    every frame while the rider is seated: a blow lands (the fight log shows it) and never takes the
//                    rider to 0 (KnockOff 0x800BF674 is not reached). Off the bike (a crash zeroes +0x0F, the remount
//                    restores it) the game keeps its own books.
//   Infinite nitro   the bike's nitro count +0x350 (HudNitro 0x80062368, the pad reader's nitro arm) kept at >= 5.
//   Freeze time      a race with a time limit *(0x8005ACC8) (Five-O, rules.md 4.3): the limit moves with the race
//                    clock game_state +0x10, so the time left stays what it was when the cheat was turned on.
#include <cstdint>
#include <string>
#include <vector>

namespace rr {
class DiscImage;
}
namespace rr::sim {
class GuestRam;
}

namespace rr::game {

class RaceSession;

struct CheatSettings {
    int weapon = -1;              // -1 off; else the weapon id every race starts with in the player's hand
    bool unlimitedSwings = false; // the weapon in hand never runs out of swings (+0x2F kept at 15)
    bool noPolice = false;
    bool noTraffic = false;
    int sparring = 0;             // 0 off, 1 passive partners, 2 partners with the original's fight rules
    bool sparringAttack = false;  // the partners attack the player (grudge / mood; not passive at him)
    int sparringWeapon = -1;      // the partners' weapon: -1 their own, 9 fists, 0..8 that weapon
    bool sparringStanding = false; // CHEAT RULE: the partners may attack a player who stands
    bool rivalsPassive = false;
    bool god = false;
    bool infiniteNitro = false;
    bool freezeTimer = false;

    bool AnyOn() const;
    bool operator==(const CheatSettings&) const = default;
    // key=value of the [cheats] section / the command line (false: unknown key or bad value)
    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const; // the [cheats] section's lines
    std::string Describe() const;  // one log line ("off" when none is on)
};

// The cheats of this process (the front end's menus and every race of it share them).
CheatSettings& Cheats();
// A race is running (the menus offer "put it in the hand now" only then; the career cheats only outside one).
void SetCheatsInRace(bool inRace);
bool CheatsInRace();
// The menu's one-shot "put weapon `w` in the player's hand now" (consumed by the next CheatBeforeFrame).
void RequestWeaponNow(int w);

// ---- the weapons, from the player's disc
struct CheatWeapon {
    int id = 0;
    std::string tag; // the four-character art tag (the HUD / rap sheet icon's id), the rap sheet's for the id
};
// Reads SLUS_010.53 / RASHCDF.BIN; empty (and `error`) when the disc does not carry them as expected.
std::vector<CheatWeapon> ReadCheatWeapons(const rr::DiscImage& disc, std::string* error = nullptr);
// The list the menus show (set once a disc is open).
void SetCheatWeapons(std::vector<CheatWeapon> list);
const std::vector<CheatWeapon>& CheatWeapons();
std::string CheatWeaponName(int id); // "TAG (weapon N)", or "weapon N" without a list

// ---- the race (rrgame's frame loop and the session's passes call these)
void CheatRaceStart(RaceSession& s);  // after the session is built: race-start values, the counters reset
void CheatBeforeFrame(RaceSession& s); // before RaceSession::Frame
void CheatAfterFrame(RaceSession& s);  // after it
// Inside the frame, after RaceDirector's brain pass (race_session.cpp): the sparring partners' pace and aim.
void CheatAfterAi(uint8_t* ram, uint32_t gp);
// The decision points' questions (the product callees of the ported passes ask them):
bool CheatCopGivesUp();                                        // cop_race.cpp ProductCopTail
// Is `e` a rival that must not fight the bike at `t` (a player)? (ai_race.cpp: the planner's CanEngage, op 16, op 9)
bool CheatRivalPassive(rr::sim::GuestRam& g, uint32_t e, uint32_t t);
void CheatNotePassive(int what); // 0 an engagement refused, 1 a fight ended, 2 a strike skipped
// Sparring, the one rule it bends: a partner stopped beside a standing player has put a foot down (FollowSeat
// 0x800C4860's stop stance, mount state rider +0x25C = 0), and CanEngage 0x800BC1EC refuses a rival that is not riding
// (mount != 1) - no blow could land on it. Around the PLAYER's op-16 FightUpdate against a partner (ai_race.cpp) the
// partner's mount state reads 1; after the call it is put back unless the call changed the stance (a hit reaction).
struct CheatLift {
    uint32_t rider = 0;
    uint16_t stance = 0;
};
CheatLift CheatSparringLift(rr::sim::GuestRam& g, uint32_t e, uint32_t t);
void CheatSparringDrop(rr::sim::GuestRam& g, const CheatLift& lift);
// The CHEAT RULE "partners hit a standing player": around the planner's CanEngage and a partner's op-16
// FightUpdate against the standing player (ai_race.cpp), the player's speed +0x1E0 reads as the partner's threshold
// class[+18]/128 x top speed (CanEngage 0x800BC3C4..0x800BC430); put back after the call unless the call wrote it.
struct CheatSpeedLift {
    uint32_t bike = 0;
    int32_t was = 0, lifted = 0;
};
CheatSpeedLift CheatStandingLift(rr::sim::GuestRam& g, uint32_t e, uint32_t t);
void CheatStandingDrop(rr::sim::GuestRam& g, const CheatSpeedLift& lift);
void CheatNoteCopGaveUp();
// The frame log's line (empty when no cheat is on) and the run tail's.
std::string CheatFrameLog(const RaceSession& s);
std::string CheatTotals();
// Whether the run tail gets CheatTotals: a cheat is on, or the run asked (--cheat-report, a control run's tally).
void SetCheatReport(bool on);
bool CheatReport();

} // namespace rr::game
