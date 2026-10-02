#pragma once
// The knock-off's tail and the two sound-side calls that go with it, ported from our own disassembly of
// RASHCDG.BIN and the player's
// SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). One `rrverify phys` row per function in
// tools\rrverify\rows_takedown.inc.
//
//   Takedown      RASHCDG 0x800BF51C (232 B, frame 48) -> FightStat 0x800BFE58 x2 (fight.h)
//   RiderOffSound SLUS    0x80018440 (108 B, frame 24) -> StopVoice 0x8001F7EC x2 (sound_engine.h)
//   BustedMusic   SLUS    0x8001B3C8 (124 B, frame 24) -> StreamRequest 0x80023148 (SpeechCallees)
#include <cstdint>

#include "game/sim/speech.h"

namespace rr::sim {

constexpr uint32_t kBustedMusicFn = 0x8001B3C8; // SLUS

// RASHCDG 0x800BF51C Takedown(B): every entry of the last-blow table 0x800CD548 (4 x {attacker, victim,
// time}, written by NoteHit 0x800BF424) whose victim is `B` and whose blow is less than 240 ticks old
// (signed, gs+0x10 - time) credits its attacker: FightStat(attacker, 1, 0), FightStat(B, 1, 1) and the
// attacker's dash flash 0x800D6224 + 224 * min(h, 1) := 1. Returns how many entries credited (ours, for
// the product's counter); the original's v0 is always 0.
int Takedown(GuestRam& g, uint32_t B);

// SLUS 0x80018440 RiderOffSound(h, mode) on engine record *(gp+1952) + 132 h: mode != 0 sets the mode
// bits +0x7C := 7 ("rider off the bike"); mode 0 stops voices +0x24 and +0x28, sets +0x54 = +0x58 = -1
// and clears +0x7C. No record array: nothing. Returns the original's v0 (h * 132, 7 or -1).
uint32_t RiderOffSound(SoundMachine& s, uint32_t h, uint32_t mode);

// SLUS 0x8001B3C8 BustedMusic(): with fewer than 2 players outside cop mode (gs+4 bit 0), speech slot 0's
// category := 7, the AUDTAUNT.STR stream record 0x800D6858 moved to 0x230000 and one record requested
// (StreamRequest(0x800D6858, 1, 1, 0)), the pending cue gp+1944 := 1 and slot 0's state := 4. Returns
// the original's v0 (0 / 1 on the gates, 4 when it ran); `ok` false when the callee failed.
uint32_t BustedMusic(SoundMachine& s, SpeechCallees& c, bool& ok);

} // namespace rr::sim
