#pragma once
// The game modes' own rules (docs\formats\rules.md 2, 9): the player-as-cop
// (Five-O, race type bit 0 - mode 1 and the career's venue 3, race type 33) and the Jailbreak venue's
// milestones (race type 44), ported from our own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8
//
// Accepted only by `rrverify phys` rows (tools\rrverify\rows_modes.inc, one row per function).
// The memory model is road_query.h's guest-address view (`GuestRam`), as cops.h: every function takes
// guest addresses, `sp` is the stack pointer at entry, locals whose address the original hands out sit
// at the original's frame offsets; false = a callee could not run or an address faulted.
//
// THE PLAYER-COP CHAIN (rules.md 9.3 / 9.4; the RASHCDI loader's part in race_modes.h):
//   the loader          player 1 on a class-2 bike in a race type with bit 0: +0x230 bit 27 (under AI
//                       control), the arrest word 0x8005AD48 |= 1, the quota 0x8005AD44 = gs+0x07, the
//                       designated suspect's bit of 0x800D8708, the command {1 or 4, 224}
//   AiDrive's arm       0x80096818 ArrestFsm on the arrest word: 1 = the player cop rides behind the
//                       field (AI speed) until it may chase, then hands the bike to the pad and starts
//                       the mission clock 0x8005ACD0; 2 = an arrest's stop; 4 / 8 = the two 2.5 s beats
//                       after it; 16 = the mission is closed
//   the three triggers  RiderKnockOff 0x8009130C (within 3.0 and faster than 8.94), the stopped scan
//                       0x80097388 (cops.h), the knock-off 0x800BF848 (fight.h) - all -> Arrest
//   Arrest 0x80096F30   the designated suspect: the player's place byte 252 (Five-O, "captured") or
//                       248 (race type 33, a success) and SpeechCue(0); any other bike: quota - 1 and
//                       253 when it runs out ("quota"); the suspect stunned 0x80097308 (place 254); the
//                       player's bike under AI control, its command {4 / 1, 224}, the arrest word |= 2;
//                       252 / 253 end the player's race (stamp, ViewEvent)
//   CopIdle's quota arm JailTest 0x8009DA4C / JailRelease 0x800A0708 (cops.h CopIdle calls them)
//
// THE JAILBREAK VENUE (race type 44, docs\formats\rules.md 9.6): FinishTest's
// milestone arm crosses SLUS 0x80053174[gs+0x39] and bumps gs+0x39, then MilestoneAdvance 0x800C8D4C
// arms the new phase; gs+0x39 == 1 also runs MilestoneFirst 0x800C92F8 every frame.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- guest addresses
constexpr uint32_t kModeArrestFn        = 0x80096F30;
constexpr uint32_t kModeStunFn          = 0x80097308;
constexpr uint32_t kModeJailTestFn      = 0x8009DA4C;
constexpr uint32_t kModeJailReleaseFn   = 0x800A0708;
constexpr uint32_t kModeArrestFsmFn     = 0x80096818;
constexpr uint32_t kModeMilestoneFn     = 0x800C8D4C;
constexpr uint32_t kModeMilestoneFirstFn = 0x800C92F8;
// the callees that are not in this file
constexpr uint32_t kModeSpeechCueFn     = 0x8001B244; // SLUS SpeechCue(kind), PORTED (sound_frame.h)
constexpr uint32_t kModePlaceFn         = 0x800138E8; // SLUS ComputePlace(e, mode), PORTED (race.h)
constexpr uint32_t kModeCopJoinFn       = 0x80028034; // SLUS CopJoin(e), PORTED (traffic_bind.h)
constexpr uint32_t kModeSoundHoldFn     = 0x80020E30; // SLUS the ambient voices' pitch hold(v), not ported
constexpr uint32_t kModeArrestSceneFn   = 0x8009D664; // the arrest's police car (pool 3), not ported
constexpr uint32_t kModeViewEventFn     = 0x8008A998; // CameraSetMode / ViewEvent(view, mode), PORTED
constexpr uint32_t kModeStanceEventFn   = 0x800C4550; // the stance event, PORTED (stance.h)
constexpr uint32_t kModeClearFn         = 0x800BCD10; // AiClearCommands, PORTED (ai.h)
constexpr uint32_t kModePushFn          = 0x800BCA68; // AiPushCommand, PORTED (ai.h)
constexpr uint32_t kModeRemountFn       = 0x800903F4; // Remount(B, fromRoad), PORTED (crash.h)
constexpr uint32_t kModeEscapeSceneFn   = 0x800C9420; // the Jailbreak escape scene(block), not ported
constexpr uint32_t kModeRiderModelFn    = 0x800302C4; // SLUS the rider's model(R, id), not ported
// data
constexpr uint32_t kModeArrestWord      = 0x8005AD48; // the arrest FSM / player-cop flags
constexpr uint32_t kModeQuota           = 0x8005AD44; // s32: arrests left
constexpr uint32_t kModeBeatClock       = 0x8005B2EC; // s32: the FSM's 16.16 beat accumulator
constexpr uint32_t kModeMissionBase     = 0x8005ACD0; // s32: the race clock at which the mission began
constexpr uint32_t kModeTimeLimit       = 0x8005ACC8; // s32: the race's time limit in ticks
constexpr uint32_t kModeRoadside        = 0x8005B2B0; // s32: the player cop started at the roadside
constexpr uint32_t kModePostTail        = 0x8005B230; // s32: the post-race tail counter
constexpr uint32_t kModeSkipResults     = 0x8005B220; // s32
constexpr uint32_t kModeJailRecords     = 0x800D6198; // 224 bytes per handle (+108, +116)
constexpr uint32_t kModePoliceOn        = 0x8005ACC0; // the police scheduler's switch
constexpr uint32_t kModeTrafficOn       = 0x8005ACC4; // the second switch (traffic)
constexpr uint32_t kModeJbDir           = 0x8005B2E8; // s32: the Jailbreak route's direction sign
constexpr uint32_t kModeMilestones      = 0x80053174; // {u16 along; u16 road}[5]
constexpr uint32_t kModeEscapeCar       = 0x8005B330; // the escape scene's car (pool 3), phase 1

// The callees not written in this file. Each returns false when the caller could not run it.
struct ModeCallees {
    virtual ~ModeCallees() = default;
    // ---- PORTED elsewhere; the caller runs them on the same memory
    virtual bool SpeechCue(int32_t kind, uint32_t sp) = 0;                                  // SLUS 0x8001B244
    virtual bool ComputePlace(uint32_t e, int32_t mode, uint32_t sp, int32_t& v0) = 0;       // SLUS 0x800138E8
    virtual bool CopJoin(uint32_t e, uint32_t sp) = 0;                                       // SLUS 0x80028034
    virtual bool ViewEvent(uint32_t view, uint32_t mode, uint32_t sp) = 0;                   // 0x8008A998
    virtual bool StanceEvent(uint32_t ev, uint32_t rider, uint32_t p, uint32_t sp) = 0;     // 0x800C4550
    virtual bool ClearCommands(uint32_t e, uint32_t sp) = 0;                                 // 0x800BCD10
    virtual bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) = 0;       // 0x800BCA68
    virtual bool Remount(uint32_t e, int32_t fromRoad, uint32_t sp) = 0;                     // 0x800903F4
    // ---- NOT ported
    virtual bool SoundHold(int32_t v, uint32_t sp) = 0;                                      // SLUS 0x80020E30
    virtual bool ArrestScene(uint32_t cop, uint32_t suspect, uint32_t sp, uint32_t& v0) = 0; // 0x8009D664
    virtual bool EscapeScene(int32_t block, uint32_t sp) = 0;                                // 0x800C9420
    virtual bool RiderModel(uint32_t rider, uint32_t id, uint32_t sp) = 0;                   // SLUS 0x800302C4
};

// ---- the leaves ---------------------------------------------------------------------------------
// 0x80097308: bike `t` stunned by an arrest - place byte 254, the finish stamp, its rider's +0x228 bits
// cleared, +0x2D0 = 2.0, its speeds zeroed (unless the rider is in stance 72 / 73), +0x39C = 0.
void ModeStun(GuestRam& g, uint32_t t);
// 0x8009DA4C: the pool-3 object (+0xAC != 0, +0xB4 == 0) nearest to `e` along the road (|+0x144 diff|
// << 4 below 999.0); `*out` = its speed below 131 (the last nearer one's); answer that distance, or
// 0xFFFF0000 when none.
uint32_t ModeJailTest(GuestRam& g, uint32_t e, uint32_t out);

// ---- the functions with callees -----------------------------------------------------------------
// 0x800A0708, frame 32: the player cop put back on its road: Remount(e, 1), the three speeds = the
// initial cop speed 0x80053138[bank], the lateral from the road piece's edge, the box centre moved there.
bool ModeJailRelease(GuestRam& g, uint32_t e, uint32_t sp, ModeCallees& c);
// 0x80096F30, frame 40: player cop `e` arrests bike `t` (`how` is passed on to ViewEvent).
bool ModeArrest(GuestRam& g, uint32_t e, uint32_t t, uint32_t how, uint32_t sp, ModeCallees& c);
// 0x80096818, frame 40: AiDrive's player-cop arm (a player's bike under AI control in a race type with
// bit 0), dispatched on the arrest word. v0: 0 = AiDrive returns at once, 1 = it goes on.
bool ModeArrestFsm(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, ModeCallees& c, uint32_t& v0);
// 0x800C8D4C, frame 48, no arguments: arm the Jailbreak phase gs+0x39 just reached (FinishTest's
// milestone arm). v0: FinishTest's new answer (phase 1: the escape deadline passed).
bool ModeMilestoneAdvance(GuestRam& g, uint32_t sp, ModeCallees& c, uint32_t& v0);
// 0x800C92F8, frame 24, no arguments: phase 1's per-frame test - player 1 turned against the route
// hands the bike to the AI; turned back past the milestone hands it back.
bool ModeMilestoneFirst(GuestRam& g, uint32_t sp);

// ---- the modes' HUD elements (HudFrame's arms for the timed and police races) -------------------------
// Item indices of DASH1P.CSV: 53..59 the race / mission clock (five digits from 53), 60..66 the split
// label, 67..73 the split time (digits from 67), 74..80 the record's split (digits from 74).
struct HudCallees;
// SLUS 0x80013B90: seconds * 256 `t` into the five digit items from `item` (tens of minutes, minutes,
// tens of seconds, seconds, tenths), each a sprite item taking art `art0 + 16 * digit`; nothing above
// 0x176F0000. Answers the seconds digit's low bit.
uint32_t HudClockDigits(GuestRam& g, int32_t t, uint32_t item, uint32_t art0);
// RASHCDG 0x80063530: the race clock (Time Trial, `down` = 0: the elapsed race clock; race types 44 / 36,
// `down` = 1: the time left before the limit 0x8005ACC8, flashing under 31 s) in items 53..59, linked
// unless `mask` or the dash's +0x8C hold it back (or the flash is off). Only while racing and not frozen
// on the grid; nothing once the bike's finish word is set.
void HudRaceClock(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t dash, uint32_t bike,
                  uint32_t down, uint32_t mask);
// RASHCDG 0x800636F0: Time Trial's splits - the label 60..66; at each of the three split distances
// 0x800D5F60[k] (progress units >> 12) the race clock is stamped (0x800D9C40[k], dash+0xB4) and shown
// in 67..73 against the race's record split (SLUS 0x80053A88 + 176 (race - 56) + 4k) in 74..80, flashing.
void HudSplits(GuestRam& g, uint32_t ot, uint32_t items, uint32_t dash, uint32_t bike, uint32_t mask);
// RASHCDG 0x80062C40: the player cop's mission clock - limit 0x8005ACC8 minus (clock - mission base
// 0x8005ACD0), in items 53..59, flashing under 31 s.
void HudCopClock(GuestRam& g, uint32_t ot, uint32_t items, uint32_t dash, uint32_t state, uint32_t bike);
// RASHCDG 0x8005FAC4, frame 32: the designated suspect's name (its riderDef +0x26, GAMESTRG) at `item`'s
// position into player p's HUD list, grey.
bool HudSuspectName(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t item, int32_t p);
// RASHCDG 0x80062610, frame 88: the arrest quota box - one player: a grey cell (polyline) per arrest of the
// quota gs+0x07 from item 3, the arrest badge (item 2's sprite) in each cell already filled (quota minus
// 0x8005AD44), and the two brackets out to items 51 and 50, all packets from the frame heap (SLUS
// 0x80021C98 when full); two players side by side (0x8005AD14 == 2): the count in digit items 6 / 7 and
// item 2. Nothing in one player while `mask` holds.
bool HudArrests(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, int32_t p, uint32_t mask);
// RASHCDG 0x80062F34, frame 72: the arrest message - while the arrest word 0x8005AD48 has bits 1..4 and its
// flash timer (dash+0x60) is on, two GAMESTRG lines centred on item 103 (0x12/0x13 captured, 0x18/0x19 the
// race-type-33 success, 0x14/0x15 the quota met, 0x16/0x17 an arrest, 0x0F..0x11 three lines on bit 3)
// over a dark box (the POLY_F4 at 0x800CC674 and the draw mode 0x800CCD50), into player p's list.
bool HudArrestMessage(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t items, uint32_t bike, uint32_t dash, int32_t p);

} // namespace rr::sim
