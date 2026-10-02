#pragma once
// Combat: punching, kicking and swinging,
// ported function by function. Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_fight.inc).
//
// THE MEMORY MODEL is road_query.h's: a guest-address view of main RAM (`GuestRam`). The fight code
// chases bike -> rider (+0x354) -> partner (+0x358) -> its rider, the FIGHT.BIN records through
// *(0x8005AD4C), the stance table SLUS 0x800541D4 and the rider's animation object (+0x21C), with the
// original's missing null tests, so it runs on guest addresses.
//
// STACK. `sp` is the stack pointer AT THE FUNCTION'S ENTRY. Two functions build an 8-byte command
// record in their own frame and hand its address to AiPushCommand (FightPush at entry sp - 32 + 16,
// FightContinue at entry sp - 40 + 16); only the first four bytes are written, and AiPushCommand's
// mode-2 rotation copies all eight into the stack, so the port reads the other four from the guest
// stack where the original leaves them. Everything else lives in C++; stores the original makes into
// its CALLER's home slot by Pick (`sw a1,60(sp)`) is not reproduced - nothing reads it back but Pick;
// 0x800BC4FC's (`sw a2,40(sp)`) is, because SetAimDelta reads the scalar through that address.
//
// A function returns false - and the caller must not trust anything it wrote - when a callee could not
// be served or the view refused an address. It never guesses.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim::fight {

// ---------------------------------------------------------------------------- globals
constexpr uint32_t kFightRecPtr   = 0x8005AD4C; // -> FIGHT.BIN, 40 x 12-byte records (rules.md 8.1)
constexpr uint32_t kGameStatePtr  = 0x8005B2F8;
constexpr uint32_t kPool0Ptr      = 0x8005B3A0; // -> pool 0, stride 1096
constexpr uint32_t kPool1Ptr      = 0x8005B3A4; // -> pool 1, stride 628
constexpr uint32_t kStanceTab     = 0x800541D4; // SLUS, 8 bytes per stance, +2 = category
constexpr uint32_t kComboMap      = 0x800CCB78; // u16[2] per action: control, co-held modifier (15 = none)
constexpr uint32_t kLastSlot      = 0x800CCB6C; // u16 per player: the last opponent
constexpr uint32_t kLastTime      = 0x800CCB70; // s32 per player: when that fight ended
constexpr uint32_t kCannedQueues  = 0x800CCAD0; // 26 rows of 6 node ids (the AI's move queues)
constexpr uint32_t kAttackers     = 0x800CCAC0; // u16 per player: the AI attacker mask
constexpr uint32_t kLastBlow      = 0x800CD548; // 4 x {attacker, victim, time}
constexpr uint32_t kFightStats    = 0x800D81F0; // u8 [player][9][4] counters (0x800BFE58)
constexpr uint32_t kDashFlash     = 0x800D6224; // + 224 * min(h, 1)
constexpr uint32_t kObjPoolBits   = 0x8005AD50; // the rider-object pool bitmap
constexpr uint32_t kObjSlots      = 0x800CF018; // 172-byte weapon-object slots
constexpr uint32_t kAnimPool      = 0x800CE170; // +8 used, +12 capacity
constexpr uint32_t kSinTable      = 0x8005624C; // SLUS, s16, indexed by a byte offset & 0x3FFC
constexpr uint32_t kK             = 0x80052F50; // the combat constants (GLOBALS.BI)
constexpr uint32_t kSpeedTab      = 0x80053024; // SPEED[bank] (GLOBALS.BI)
constexpr uint32_t kDivTab        = 0x80053188; // the slow bar's divisor by bank (GLOBALS.BI)
constexpr uint32_t kClassBlocks   = 0x80052EE4; // 36 bytes per bike class (GLOBALS.BI)
constexpr uint32_t kMoveWeights   = 0x8005ADC4; // FightRestart's cumulative move weights (GLOBALS.BI)
constexpr uint32_t kShoveNode     = 0x80053210; // HitShove: the record's last node
constexpr uint32_t kShoveParams   = 0x800531E8; // HitShove: {p0, p1} per record
constexpr uint32_t kAttractFlag   = 0x8005B220; // no rumble in attract mode

constexpr uint16_t kNoTarget = 224;
constexpr uint8_t kNodeNone = 63;
constexpr uint8_t kNoRecord = 41;

// ---------------------------------------------------------------------------- the callees
// Everything the fight code reaches that is not written in this file. The first group is PORTED
// elsewhere and the caller runs it where it lives (the stance layer, the animation clock, the sound
// runtime, the AI command stack); the second is NOT ported and the caller must refuse or supply it.
struct Callees {
    virtual ~Callees() = default;
    // --- ported
    virtual bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t& v0) = 0;       // 0x800C4550
    virtual bool ClipDone(uint32_t anim, uint32_t& v0) = 0;                                 // 0x8005BE58
    virtual bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) = 0;           // SLUS 0x80017BA0
    virtual bool QueueListenerSound(uint32_t e, int32_t id, int32_t t, int32_t p) = 0;      // SLUS 0x80017B6C
    // RASHCDG 0x800BCA68 AiPushCommand(cmd, mode, e): `cmd` is the 8-byte record the original keeps
    // in its frame at guest `cmdAt`; the callee may write its stamp back into `cmd`.
    virtual bool PushCommand(uint8_t cmd[8], uint32_t cmdAt, int32_t mode, uint32_t e) = 0;
    virtual bool PopCommand(uint32_t e) = 0;                                                // 0x800BC8DC
    virtual bool EndRace(uint32_t e, int32_t reason) = 0;                                   // 0x80092C7C
    // RASHCDG 0x80093CAC SetAimDelta(e, 0, e + 0x368, scalarAt, 1) - 0x800BC4FC's one call. The port has
    // already stored `scalar` at guest `scalarAt` (0x800BC4FC's home slot for a2, entry sp + 8).
    virtual bool SetAimDelta(uint32_t e, uint32_t scalarAt, int32_t scalar) = 0;
    // --- not ported
    // 0x80096F30 Arrest(a, v, 9): KnockOff leaves `a` in a3 too, and Arrest reads a3 (the bench found it).
    virtual bool Arrest(uint32_t a, uint32_t v, int32_t k, uint32_t a3) = 0;
    virtual bool PadMotor(int32_t port, int32_t a, int32_t b, int32_t c) = 0;               // SLUS 0x8001DD74
    virtual bool RiderSpeech(uint32_t h, int32_t a) = 0;                                    // SLUS 0x8001A760
    virtual bool WeaponObject(uint32_t r, uint32_t side) = 0;                               // 0x800958F0
    virtual bool ObjectRelease(uint32_t r, uint32_t slot, uint32_t z) = 0;                  // 0x80068D20
    virtual bool ObjectStop(uint32_t slot) = 0;                                             // SLUS 0x8002847C
    virtual bool ObjectSound(uint32_t slot, int32_t a1, int32_t a2, int32_t a3, int32_t a4) = 0; // SLUS 0x800273EC
    // 0x800C2F84; `v0` in = the v0 the caller leaves (the callee returns it untouched when r+0x22C is 0).
    virtual bool OverlayClip(uint32_t ev, uint32_t r, uint32_t a2, uint32_t a3, uint32_t& v0) = 0; // 0x800C2F84
};

// ---------------------------------------------------------------------------- the input
// RASHCDG 0x800C2178 Edge(pad, n): action n pressed within the press window gs+0x20 (and its co-held
// modifier held). 0x800C2070 EdgeReleased: released less than 75 ticks ago (modifier held).
// 0x800C2100 Held, 0x800C213C Released (the stamp's sign bit).
int32_t Edge(GuestRam& g, uint32_t pad, int32_t n);
int32_t EdgeReleased(GuestRam& g, uint32_t pad, int32_t n);
int32_t Held(GuestRam& g, uint32_t pad, int32_t n);
uint32_t Released(GuestRam& g, uint32_t pad, int32_t n);

// RASHCDG 0x800C2348 CombatDecode(pad, bike) - 580 B, frame 40: the player's move byte riderDef+0x3C
// from the eight actions and the weapon, then FightPush and rider+0x23D = 0; the taunt (sound 78) for
// a weapon that cannot swing. Returns 1 when a move was issued.
bool CombatDecode(GuestRam& g, Callees& c, uint32_t pad, uint32_t bike, uint32_t sp, int32_t& v0);
// RASHCDG 0x800C1DD4 FightPush(bike) - 604 B, frame 32: pushes {16, target} (mode 2) on the bike's
// own command stack; the target is the remembered opponent or Pick's nearest.
bool FightPush(GuestRam& g, Callees& c, uint32_t bike, uint32_t sp);
// RASHCDG 0x8008B428 Pick(hf, mask) - 1060 B, no calls: the nearest pool-`mask` handle by octagonal
// distance in growing squares of the collision grid; 224 = none.
uint16_t Pick(GuestRam& g, uint32_t hf, uint32_t mask);
// RASHCDG 0x800C258C ComboInput(pad, bike) - 1124 B, frame 64: while the fight is on, the FIGHT.BIN
// edges of the rider's record extend its node queue. Returns 0 (v0).
bool ComboInput(GuestRam& g, uint32_t pad, uint32_t bike);

// ---------------------------------------------------------------------------- the fight
// RASHCDG 0x800C035C FightUpdate(me, slot, dt) - 1620 B, frame 72: the whole machine, for the player
// (from the command stack's op 16) and the AI alike. `dt` is not read.
bool FightUpdate(GuestRam& g, Callees& c, uint32_t me, uint16_t slot, uint32_t sp);
// RASHCDG 0x800C09B0 FightStep(me) - 568 B: the node queue against the animation clock; v0 = 1 on the
// one strike frame.
bool FightStep(GuestRam& g, Callees& c, uint32_t me, uint32_t& v0);
bool FightStart(GuestRam& g, Callees& c, uint32_t me, uint32_t t);                // 0x800C110C
bool FightBegin(GuestRam& g, Callees& c, uint32_t me, uint32_t side);             // 0x800C12D8
uint32_t FightPickRecord(GuestRam& g, uint32_t me);                               // 0x800BF978
bool FightEnd(GuestRam& g, Callees& c, uint32_t r, uint32_t& v0);                 // 0x800C1014
bool FightRestart(GuestRam& g, Callees& c, uint32_t me, uint32_t t, uint32_t side); // 0x800C122C
bool FightContinue(GuestRam& g, Callees& c, uint32_t me, uint32_t t, int32_t along, uint32_t s,
                   uint32_t sp, int32_t& v0);                                     // 0x800C0BE8
bool FightSteer(GuestRam& g, Callees& c, uint32_t me, uint32_t t, int32_t along, uint32_t sp); // 0x800C0CE8
void FightPace(GuestRam& g, uint32_t me, uint32_t t, int32_t along);              // 0x800C0E98
int32_t CanEngage(GuestRam& g, uint32_t me, uint32_t t, int32_t along);           // 0x800BC1EC
int32_t BackOff(GuestRam& g, uint32_t me, uint32_t t, int32_t x);                 // 0x800BB8FC
int32_t OtherOnSide(GuestRam& g, uint32_t t, uint32_t h, int32_t bias);           // 0x800BC618
bool SideAim(GuestRam& g, Callees& c, uint32_t me, uint32_t t, int32_t b, uint32_t sp); // 0x800BC4FC
void NextMove(GuestRam& g, uint32_t rd);                                          // 0x800B92C0
void PickWeapon(GuestRam& g, uint32_t rd);                                        // 0x800B9340
// RASHCDG 0x800C159C ReachTest(me, t, lat, along, *flags) - 532 B; `flags` is the caller's word.
int32_t ReachTest(GuestRam& g, uint32_t me, uint32_t t, int32_t lat, int32_t along, uint32_t& flags);
// RASHCDG 0x800C17B0 ApplyHit(me, t, side, flags) - 1572 B, frame 72.
bool ApplyHit(GuestRam& g, Callees& c, uint32_t me, uint32_t t, uint32_t side, uint32_t flags, uint32_t& v0);
// RASHCDG 0x800BF860 HitStance(r, v, hit, *damage): the victim's reaction stance.
uint16_t HitStance(GuestRam& g, uint32_t r, uint32_t v, int32_t hit, uint32_t& damage);
void NoteHit(GuestRam& g, uint32_t a, uint32_t v);                                // 0x800BF424
void HitShove(GuestRam& g, uint32_t v, uint32_t node, int32_t rec, int32_t side); // 0x800BF604
bool HitRumble(GuestRam& g, Callees& c, uint32_t e, int32_t fin);                 // 0x800BFD74
bool KnockOff(GuestRam& g, Callees& c, uint32_t v, uint8_t cmd, int32_t side, uint32_t a); // 0x800BF674
void FightStat(GuestRam& g, uint32_t e, int32_t k, int32_t which);                // 0x800BFE58
int32_t RememberHandle(uint32_t h, uint32_t& set);                                // 0x800A8BE0
bool WeaponSteal(GuestRam& g, Callees& c, uint32_t me, uint32_t t, uint32_t side, uint32_t& flags); // 0x800BFF04
int32_t StealWindow(GuestRam& g, uint32_t r, uint8_t weapon);                     // 0x800C2E9C
void CloseQueue(GuestRam& g, uint32_t r);                                         // 0x800C2030

// ---------------------------------------------------------------------------- the stance layer's children
// RASHCDG 0x800BFD24 CombatLeave(r, cur, ev, p) and 0x800BFC5C CombatEnter(r, cur, ev, p): the two
// seams of stance.h, ported here. 0x80095AEC ReleaseRiderObject(r).
bool CombatLeave(GuestRam& g, Callees& c, uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, uint32_t& v0);
bool CombatEnter(GuestRam& g, Callees& c, uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, uint32_t& v0);
bool ReleaseRiderObject(GuestRam& g, Callees& c, uint32_t r);

} // namespace rr::sim::fight
