#pragma once
// The opponent AI, ported function by function from the original MIPS code.
// Acceptance: `rrverify phys` at 0 mismatches over the whole
// guest RAM outside the guest stack plus the scratchpad, across dump-derived and randomised inputs.
//
// Overlay `RASHCDG.BIN`, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
// (file offset 0 = that address). Resident executable `SLUS_010.53`, SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text base 0x80010000 (file offset 0x800). Every
// function below carries the overlay it lives in.
//
// As everywhere in `src\game\sim`, a guest pointer is never followed from inside the port: the
// caller resolves the chase and hands over the bytes.
#include <cstdint>

#include "game/sim/bike.h"

namespace rr::sim {

// ---------------------------------------------------------------- the AI's entity fields
// Added to `ent` in bike.h. All offsets are into the 1096-byte pool-0 entity and every one of them
// is read out of the original's own code.
namespace ent {
constexpr uint32_t kAimSliceAxis = 0x368; // s16[3] lateral axis of the aimed-at road slice
constexpr uint32_t kAimLateral   = 0x36E; // s16 wanted lateral offset on that slice
constexpr uint32_t kAimPoint     = 0x370; // 3 x 16.16, the AI's 3D aim point (== kAimFrom)
constexpr uint32_t kAimDelta     = 0x37C; // 3 x 16.16, aim point -> commanded target
constexpr uint32_t kAimRate      = 0x388; // 16.16, the rate kAimBlend advances at
constexpr uint32_t kAimBlend     = 0x38C; // 16.16, the 0..1.0 blend along kAimDelta
constexpr uint32_t kAimSuppress  = 0x3B0; // s16, > 0 suppresses a new delta
constexpr uint32_t kCmdSpeed     = 0x39C; // 16.16 commanded speed (== kDriftLimit)
constexpr uint32_t kAiState      = 0x3A0; // AI/police state byte
constexpr uint32_t kRubberBand   = 0x3A1; // s8 rubber-band percentage
constexpr uint32_t kAiCmdDepth   = 0x3B2; // s8 command stack depth, 1-based
constexpr uint32_t kAiCmdStack   = 0x3B4; // 16 slots of 8 bytes; slot k at kAiCmdStack + 8*k
} // namespace ent

// One entry of the AI command stack. Eight bytes, little-endian, and the
// port reads it out of the entity's own bytes rather than declaring it as a struct over guest
// memory.
constexpr uint32_t kAiCmdOpcode = 0; // u16 0..18
constexpr uint32_t kAiCmdTarget = 2; // u16 handle, or 224 = none
constexpr uint32_t kAiCmdSpare  = 4; // u16 zeroed on push
constexpr uint32_t kAiCmdStamp  = 6; // u16 bit 15 = fresh, bit 14 = seen by one planner pass

// ---------------------------------------------------------------------------- SLUS 0x8001E100
// void *MemSet32(void *dst, u32 byteValue, u32 length)
//
// The game's own `memset`. It is WORD-WISE: it stores the byte replicated into all four lanes with
// `sw` and decrements the length by 4 until it reaches exactly zero, so a length that is not a
// multiple of four never terminates on the console. That is a property of the original, not an
// approximation - the port reproduces the word store and leaves the loop condition alone.
void MemSet32(uint8_t* dst, uint32_t byteValue, uint32_t length);

// ---------------------------------------------------------------------------- RASHCDG 0x800B6AAC
// s32 AiProject(const s32 p[3], const s16 axis[3], const s32 org[3])
//
// The lateral/longitudinal projection helper: the length of `p - org` along `axis`, with `axis` in
// the usual 4096 = 1.0 direction units promoted by `<< 4` and the result in 16.16.
//
//   acc = 0;  for k in 0..2:  acc += (u32)((s64)((s32)axis[k] << 4) * (s32)(p[k] - org[k]) >> 16)
//
// Each term is a separate 64-bit product of which only bits 47..16 survive, and the three terms are
// then summed as 32-bit words, so the wrap of a point far from the slice is reproduced rather than
// approximated. The original also computes the three high words and drops them; they are dead.
int32_t AiProject(const int32_t p[3], const int16_t axis[3], const int32_t org[3]);

// ---------------------------------------------------------------------------- RASHCDG 0x800A8C48
// s32 AiHandleInList(u32 handle, u32 list)
//
// The membership test over a packed handle list: `list` holds 5-bit fields of `handle + 1`, lowest
// field first, terminated by a zero field or by the word running out.
//
//   if (list == 0) return 0;
//   want = (handle & 0xFFFF) + 1;
//   do { if ((list & 0x1F) == want) return 1; list >>= 5; } while (list != 0);
//   return 0;
int32_t AiHandleInList(uint32_t handle, uint32_t list);

// ---------------------------------------------------------------------------- RASHCDG 0x800BD34C
// void AiNibbleAdd(u8 *p, s32 delta)
//
// Add to the LOW nibble of a byte and clamp the result into [0, 15], leaving the high nibble alone.
// The clamp is the original's branch-free idiom `max(x,0) + min(15-x,0)`; the port keeps it in that
// form because the intermediate wraps on a large `delta` exactly as the console does.
void AiNibbleAdd(uint8_t* p, int32_t delta);

// ---------------------------------------------------------------------------- RASHCDG 0x800BCEEC
// void AiNibbleDecay(u8 *p, s32 step)
//
// Move the low nibble of a byte `step` toward the high nibble (the resting value), snapping when
// the remaining distance is smaller than `step`; the high nibble is rewritten unchanged.
//
//   cur = *p & 0xF;  rest = *p >> 4;
//   if (|cur - rest| < step) cur = rest;
//   else { cur += (cur - rest >= 0) ? -step : +step;  cur = clamp(cur, 0, 15); }
//   *p = (cur & 0xF) | (rest << 4);
void AiNibbleDecay(uint8_t* p, int32_t step);

// ---------------------------------------------------------------------------- RASHCDG 0x800BCD10
// void AiClearCommands(Entity *e)
//
//   MemSet32(e + 0x3BC, 0, 8 * (s8)e[0x3B2]);  e[0x3B2] = 0;
//
// i.e. it clears slots 1..depth of the command stack (slot k sits at 0x3B4 + 8k, so slot 1 is at
// 0x3BC) and then empties it. `e` is a byte view rather than an EntityView because the length is
// the entity's own depth byte and the original does not bound it.
void AiClearCommands(uint8_t* e);

// ---------------------------------------------------------------------------- SLUS 0x8002F0F4
// s32 SumSquares(const s32 v[3])
//
//   acc = 0;  for k in 0..2:  acc += (u32)((s64)v[k] * v[k] >> 16)
//
// The 16.16 squared length, in the same "sum the low words of three 64-bit products" shape as
// AiProject above.
int32_t SumSquares(const int32_t v[3]);

// ---------------------------------------------------------------------------- SLUS 0x8004CF74
// s32 SqrtGte(s32 x, const s16 *table)
//
// The square root. The original feeds `x` to the GTE's leading-zero counter (`mtc2 $30` / `mfc2
// $31`), normalises `x` into the 64..191 range that indexes the 128-entry table at 0x800560CC, and
// then shifts the table value back by half the exponent:
//
//   lz = leading bits of x equal to its sign bit, 1..32;  if (lz == 32) return 0;
//   e  = lz & ~1;  sh = (19 - e) >> 1;
//   n  = (e >= 24) ? (x << (e - 24)) : (x >> (24 - e));      // arithmetic shift right
//   w  = table[n - 64];                                       // s16
//   return (sh >= 0) ? (w << sh) : ((u32)w >> -sh);
//
// `table` is game data, so the caller passes a pointer into the player's own image. The index is
// NOT bounded by the original: a negative `x` - which `SumSquares` produces as soon as the squared
// length wraps - makes the console read up to 640 bytes BELOW the table. The caller therefore hands
// a window around the table rather than the table alone, exactly as `bike_engine` does for its stat
// block, and `table` points at entry 0 inside it. Both `addi`
// instructions in the original trap on signed overflow; nothing in the reachable input range
// reaches them, and the port does not invent a result for inputs that would.
int32_t SqrtGte(int32_t x, const int16_t* table);

// ---------------------------------------------------------------------------- SLUS 0x8002E548
// s32 Length3(const s32 v[3], const s16 *sqrtTable)
//
//   return SqrtGte(SumSquares(v), sqrtTable) << 2;
int32_t Length3(const int32_t v[3], const int16_t* sqrtTable);

// ---------------------------------------------------------------------------- RASHCDG 0x80093CAC
// void SetAimDelta(Entity *e, const s32 target[3], const s16 dir[3], const s32 *scalar, s32 mode)
//
// The aim point at `+0x370` slides toward whatever a command asks for instead of jumping; this is
// the function that arms that slide.
//
//   if (target)            for k: e[0x37C+4k] = target[k] - e[0x370+4k];
//   else if (dir && scalar) Scale(*scalar, dir, &e[0x37C]);
//   else                    return;
//   if ((s16)e[0x3B0] > 0)  return;
//   len = scalar ? |*scalar| : Length3(&e[0x37C]);
//   if (len < 131)          return;                            // 131/65536 = 0.002 world units
//   K = mode ? 0x10000 : 0xA0000;                              // 1.0 or 10.0
//   e[0x388] = (len > 0) ? FixDiv(K, len) : -FixDiv(K, -len);
//   t = (len > 0) ? (0x8000 + FixDiv(len, K)) : (0x8000 - FixDiv(-len, K));
//   if (t > 0x7FFF00) t = 0x7FFF00;
//   e[0x3B0] = (s16)(((u32)t << 8) >> 16 arithmetic);
//   if (that value == 0) e[0x38C] = 0;
//
// The fifth argument `mode` arrives on the stack at sp+16 in the original.
//
// `sqrtTable` is the game data `Length3` needs; it is only read on the `scalar == nullptr` arm.
void SetAimDelta(EntityView e, const int32_t* target, const int16_t* dir, const int32_t* scalar,
                 int32_t mode, const int16_t* sqrtTable);

// ---------------------------------------------------------------------------- RASHCDG 0x800BCA68
// s32 AiPushCommand(u8 *cmd, s32 mode, Entity *e, ...)
//
// Everything the AI issues goes through this. `cmd` is an 8-byte command
// record IN THE CALLER'S memory, and the function may write its stamp field, so it is passed as a
// mutable byte pointer rather than by value.
//
//   if ((s8)e[0x3B2] >= 16) { e[0x3B2] = 1; MemSet32(e + 0x3C4, 0, 120); }   // keep 15 slots
//   if (e[0x3B2] == 0) goto push;
//   mode 1: if slot[depth] matches (opcode, target) return 1;
//   mode 2: search slot[depth]..slot[1]; on a match at k:
//             if (k == depth) return 1;
//             cmd->stamp = slot[k].stamp;
//             move slot[k+1..depth] down one and store *cmd at slot[depth];  return 1;
//   push: e[0x3B2]++;  slot[depth] = *cmd;  slot[depth].spare = 0;
//         slot[depth].stamp = 0xC000 | (((2180 * raceClock) + 0x8000) >> 16);
//         if (e[0x230] & 0x08000000) {                       // under AI control
//             rider = e[0x354];
//             if (altKind[rider[0x220]].category == 3) PlayIdleStance(fight[rider[0x239]].event);
//         }
//   return 1;
//
// The one call on that last arm is `RASHCDG 0x800C4550`, which this port does not contain: it is
// requested through `AiStanceSink` and the bench supplies it from the oracle.
struct AiStanceSink {
    virtual ~AiStanceSink() = default;
    // `event` is the halfword the fight record supplies; `rider` is the guest address of the rider
    // object, which the original passes through untouched as the callee's second argument.
    virtual void PlayIdleStance(uint16_t event, uint32_t rider) = 0;
};

// Everything the original reaches through a guest pointer on the push arm, resolved by the caller.
// `altKindTable` is the 8-byte-per-record table at SLUS 0x800541D4 and `fightRecords` the 12-byte
// records behind the pointer at RASHCDG 0x8005AD4C; `rider` is the bytes of the object at
// `e[0x354]` and `riderAddress` its guest address.
struct AiPushEnv {
    int32_t raceClock = 0;             // game_state[+0x10], through the pointer at 0x8005B2F8
    const uint8_t* rider = nullptr;    // the object at e[0x354], or null when there is none
    uint32_t riderAddress = 0;
    const uint8_t* altKindTable = nullptr;
    const uint8_t* fightRecords = nullptr;
    AiStanceSink* stance = nullptr;
};

int32_t AiPushCommand(uint8_t* cmd, int32_t mode, uint8_t* e, const AiPushEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x800BA7F4
// void AiCmdRace(Entity *e)
//
// The handler of **command 4, "race"** - the arm of the dispatcher `0x800BA4CC` that every one of
// the sixteen racers is in, in every savestate. It is the racing line
// itself: it decides whether this bike is allowed to hold a line of its own this pass and, if it
// is, aims it sideways off the road slice it is on.
//
//   gate:  a = 0;
//          if (raceClock < 900 || (riderDef[0] & 1)) { a = handle - 1; if (a < 0) a = 0; }
//          if      (flags & 1)                                   a = 1;
//          else if ((riderDef[1] & 0xF) == 2 && (flags & 4))     a = 1;
//          else if (gameState[0x39] >= 4 && ((((handle >> 1) - 1) & 1) != 0)) a = 1;
//          if (!(a & 1)) { e[0x3B0] = 0; riderDef[0] &= ~4; return; }
//
//   bias:  if ((gameState[4] & 1) && ((flags & 1) || ((riderDef[1] & 0xF) == 2 && (flags & 4)))) {
//              if ((s16)e[0x188] == 4) bias = e[0x158] - ((s16)e[0x36E] << 5);
//              else                    bias = (e[0x16C] >= 0) ? -0x20000 : +0x20000, with the
//                                             sign flipped when the slice's lateral axis and the
//                                             bike's stored one point opposite ways;
//          } else the same +-0x20000 rule;
//          SetAimDelta(e, NULL, &e[0x368], &bias, 1);
//          e[0x3B0] = 1;  e[0x38C] = 0x10000;  riderDef[0] |= 4;
//
// Its only callee is `SetAimDelta`, which is ported, so this row has no seam.
struct AiCmdRaceEnv {
    const uint8_t* gameState = nullptr;  // through the pointer at RASHCDG 0x8005B2F8
    uint8_t* riderDef = nullptr;         // e[+0x43C]; bit 2 of byte 0 is written
    const int16_t* slice = nullptr;      // the road slice at e[+0x154]; halfwords 1 and 3 are read
    uint32_t flags = 0;                  // *(u32 *)RASHCDG 0x8005AD48
    const int16_t* sqrtTable = nullptr;  // for the SetAimDelta call (unused on this path)
};
void AiCmdRace(EntityView e, const AiCmdRaceEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x80095BF8
// s32 AiTargetSpeed(Entity *e, s32 dt)
//
// The whole speed story of an opponent, 603 instructions: it picks the
// reference rider, runs the police ramp or the racer rubber band, and returns the speed `AiDrive`
// then takes the minimum of with `e[+0x39C]`.
//
// Five arms, in the order the original tests them:
//   1. **the reference pick.** With two players it chooses the one whose place is closer to this
//      rider's, skipping a player who has finished (`riderDef[+0x28] != 0`) or whose place code is
//      >= 248; otherwise it is always player 1.
//   2. **the cop arm** (`(riderDef[+0x01] & 0xF) == 2`), gated on `e[+0x3A0] & 0x10`. Jailbreak
//      phase >= 4 returns a flat speed from `SLUS 0x80053030`; below that the speed ramps in steps
//      with the time since release and is written back into `stats[+0xE0]` - the cop is the only
//      thing in the game that writes that field.
//   3. **the Jailbreak arm** (`gameState[+0x39] == 1`): the player's own current speed.
//   4. **the class-match arm** (`gameState[+0x39] == 3`): zero when this rider's bike class equals
//      player 1's.
//   5. **the racer rubber band.** Before the race clock passes a per-race-type threshold the rider
//      is held on a "think period" schedule; after it, the signed percentage `e[+0x3A1]` is
//      interpolated out of the 3 x 5 x 4 difficulty profile at `SLUS 0x800531AC` by the field
//      spread, this rider's place band and the signed place gap, and the answer is
//      `stats[+0xE0] * (128 + e[+0x3A1]) / 128`, scaled once more by `SLUS 0x80052FA0[class]`
//      through `FixMul` when the bike is not yet awake.
//
// It writes `stats[+0xE0]`, `e[+0x140]`, `e[+0x234]`, `e[+0x2C0]`, `e[+0x3A0]`, `e[+0x3A1]`,
// `riderDef[+0x00]` and the two cached globals `0x800CCA84` / `0x800CCA88`, so all of those are
// passed as mutable blocks. Its only callee is `FixMul`, which is ported - no seam.
//
// The pointer to player 1's bike is `RASHCDG 0x8005B38C` (`lui 0x8006` + `lw -19572`), not
// `0x800CB38C`.
struct AiBikeRef {
    const uint8_t* entity = nullptr;   // the 1096-byte pool-0 slot
    const uint8_t* riderDef = nullptr; // its 72-byte runtime rider record
};

struct AiSpeedEnv {
    const uint8_t* gameState = nullptr; // through the pointer at 0x8005B2F8; +0x04/+0x10/+0x30/
                                        // +0x39/+0x3C are read
    AiBikeRef player1;                  // *(0x8005B38C)
    AiBikeRef player2;                  // *(0x8005B21C)
    AiBikeRef playerSlot[2];            // *(0x8005B268 + 4*p)
    const uint8_t* jailbreakBike = nullptr; // *(0x800CCC18), or null when that pointer is 0
    int32_t liveBikes = 0;              // *(0x8005B1F8)
    int32_t bikeCap = 0;                // *(0x8005B1FC)
    int32_t spreadDiv = 0;              // *(0x8005B244)
    uint32_t raceFlags = 0;             // *(0x8005AD48)
    uint32_t altFlag = 0;               // *(0x8005B2B0)
    int32_t copScale = 0;               // *(0x800D86F8)
    int32_t* clockStamp = nullptr;      // -> *(0x800CCA84), read AND written
    int32_t* spreadBand = nullptr;      // -> *(0x800CCA88), read AND written
    // Game data, read out of the player's own image. The indices are `gameState[+0x3C]` plus 0, 3 or
    // 6, and the difficulty profile's index is `20*a1 + 4*a3 + t1` with `a1` unbounded - so the
    // caller hands windows, exactly as `bike_engine` does for its stat block.
    const int32_t* tabSpeedClass = nullptr; // 0x80052FA0, by bike class
    const int32_t* tabThinkA = nullptr;     // 0x80052FD0
    const int32_t* tabThinkB = nullptr;     // 0x80052FF4
    const int32_t* tabCopFlat = nullptr;    // 0x80053030
    const int32_t* tabAltA = nullptr;       // 0x8005303C
    const int32_t* tabAltB = nullptr;       // 0x80053054
    const int32_t* tabSpeedCap = nullptr;   // 0x80053138
    const int16_t* tabCopBase = nullptr;    // 0x80053144, the s16 at +2 of each 4-byte entry
    const int16_t* tabCopStep = nullptr;    // 0x80053150, likewise
    const int32_t* tabCopMul = nullptr;     // 0x8005315C
    const int8_t* profile = nullptr;        // 0x800531AC, the chosen 60-byte difficulty profile
};

int32_t AiTargetSpeed(uint8_t* e, uint8_t* riderDef, uint8_t* stats, int32_t dt,
                      const AiSpeedEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x800BA4CC
// void AiRunCommands(s32 dt, u32 skip, u32 maskB)
//
// The command pass: once per frame it walks every live pool-0 bike, reads
// the top of its 16-slot command stack, marks the per-player attacker mask, dispatches the opcode
// through the 19-entry jump table and clears the command's "freshly issued" bit.
//
//   for (i = 0; i < *(0x8005B1F8); i++) {
//       if (skip & (1 << i)) continue;
//       Cmd *c = &e->stack[(s8)e[0x3B2]];
//       if (e[0xAC] >= gameState[0x30] && c->target < gameState[0x30] && 14 <= c->opcode < 17)
//           attackerMask[c->target] |= 1 << (e[0xAC] - 1);
//       if (c->opcode < 19) (*arm[c->opcode])();
//       c->stamp &= 0x7FFF;
//   }
//
// **The jump table is DATA.** `arm` is read out of the player's own overlay at `RASHCDG 0x8005B9B8` -
// the address the dispatcher itself builds at 0x800BA5B4/0x800BA5B8 - and the port dispatches on
// the address it finds there, never on a baked-in opcode-to-handler mapping. Two of the nineteen
// arms are implemented here:
//   * `0x800BA79C`, which eight opcodes share and which does nothing but the stamp clear;
//   * `0x800BA6E0`, the command-4 arm: `if (!(maskB & (1 << i)) && (e[0x230] & 0x08000000))
//     AiCmdRace(e)`.
// Any other arm makes `AiRunCommands` return false without running it, rather than guessing: the
// remaining seventeen handlers are stages B, C and D of the porting order and a port that pretended
// to have them would be exactly the kind of stub this project rejects.
struct AiCommandNode {
    uint8_t* entity = nullptr;    // the 1096-byte pool-0 slot
    uint8_t* riderDef = nullptr;  // e[+0x43C]
    const int16_t* slice = nullptr; // e[+0x154], for the command-4 arm
};

struct AiRunEnv {
    const uint8_t* gameState = nullptr;
    uint32_t raceFlags = 0;            // *(u32 *)0x8005AD48
    const int16_t* sqrtTable = nullptr;
    uint16_t* attackerMask = nullptr;  // -> 0x800CCAC0, one u16 per player
    const uint32_t* armTable = nullptr; // the 19 words at 0x8005B9B8, from the player's image
    uint32_t armNone = 0;              // the address of the do-nothing arm (0x800BA79C)
    uint32_t armRace = 0;              // the address of the command-4 arm (0x800BA6E0)
};

// Returns false as soon as it meets an arm it does not implement, having run everything before it.
bool AiRunCommands(AiCommandNode* bikes, size_t count, int32_t dt, uint32_t skip, uint32_t maskB,
                   const AiRunEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x800BD388
// s32 AiRecoverLine(Entity *e)
//
// Called by the drive pass (`0x800BA304` at `0x800BA3C0`) right after `AiDrive`, and its answer is
// the bit the pass collects into the third of its two masks. It is the "this bike is off its line"
// correction: `e[+0x184]` bit 0 says the bike is somewhere it should not be, and then
//
//   * below 2.25 of speed copy (`e[+0x240] <= 0x00023FFF`) the aim point is snapped straight back
//     onto the road centre line - `MulAdd(slice->pos, slice->m[6..8], e[+0x15C], &e[+0x370])`,
//     i.e. the slice origin plus the slice TANGENT times the distance along - the wanted lateral
//     offset `e[+0x36E]` is zeroed and `e[+0x3A0]` bit 6 is set;
//   * above it, and only with `e[+0x184]` bit 4 also set, the twelve bits at `e[+0x184] >> 8` are
//     a half width: the correction only runs while `|e[+0x158]|` is between `width/8` and
//     `width/8 + 5.0`, and it then slides the aim point sideways by
//     `+-(width/8 + 1.0) - (e[+0x36E] << 5)` along the slice's lateral axis `e[+0x368]`, adds
//     `that >> 5` to `e[+0x36E]`, clears `e[+0x3B0]` and loads the commanded speed `e[+0x39C]`
//     with `((gameState[+0x3C] * 8 + 24) << 16)`.
//
// Returns 1 when it corrected and 0 otherwise. `slicePos` and `sliceTangent` are the caller's
// resolution of `e[+0x154]` (only the slow arm reads them), `raceBank` is `gameState[+0x3C]`.
int32_t AiRecoverLine(EntityView e, const int32_t* slicePos, const int16_t* sliceTangent,
                      int32_t raceBank);

// ---------------------------------------------------------------------------- RASHCDG 0x800BC8DC
// void AiPopCommand(Entity *e)
//
// The other half of `AiPushCommand`: the drive pass calls it (`0x800BA418`) when a bike is running
// a command of 5 or above and its rider has neither finished nor been jailed. It pops the top of
// the 16-slot command stack and, when the command underneath is another "fight" (opcode 16) whose
// target is a player that no longer has exactly one attacker, pops again - the original is
// genuinely recursive here (`jal 0x800BC8DC` at `0x800BCA48`).
//
//   if ((s8)e[0x3B2] == 0) return;
//   slot = &e[0x3B4 + 8*(s8)e[0x3B2]];
//   if ((u16)(slot->opcode - 10) < 7) riderDef[0x3D] &= 0xF0;      // drop the combo nibble
//   if (e[0x230] & 0x08000000) {                                   // under AI control
//       e[0x2C0] = 0;
//       rider = e[0x354];
//       if (altKind[rider[0x220]].category == 3) PlayIdleStance(fight[rider[0x239]].event, rider);
//   }
//   MemSet32(slot, 0, 8);
//   e[0x3B2] = (u8)e[0x3B2] - 1;
//   slot = &e[0x3B4 + 8*(s8)e[0x3B2]];
//   if (slot->opcode != 16) return;
//   if (!(slot->target < gameState[0x30])) return;
//   if (gameState[4] == 36 || gameState[4] == 44) return;
//   m = attackerMask[slot->target];
//   if (m == 0 || (m & -m) == m) return;                           // nobody, or exactly one
//   AiPopCommand(e);                                               // ... otherwise pop again
//
// The stance trigger is the same `RASHCDG 0x800C4550` `AiPushCommand` reaches, requested through
// the same `AiStanceSink` and supplied by the bench from the oracle.
struct AiPopEnv {
    const uint8_t* gameState = nullptr;      // through the pointer at RASHCDG 0x8005B2F8
    uint8_t* riderDef = nullptr;             // e[+0x43C]; byte +0x3D is written
    const uint8_t* rider = nullptr;          // the object at e[+0x354]
    uint32_t riderAddress = 0;               // its guest address, passed through to the callee
    const uint8_t* altKindTable = nullptr;   // SLUS 0x800541D4, 8-byte records
    const uint8_t* fightRecords = nullptr;   // the 12-byte records behind RASHCDG 0x8005AD4C
    const uint16_t* attackerMask = nullptr;  // -> 0x800CCAC0, one u16 per player (read only)
    AiStanceSink* stance = nullptr;
};
void AiPopCommand(uint8_t* e, const AiPopEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x800954A0
// void AiDrive(Entity *e, s32 dt)
//
// The function that makes an opponent drive. Every frame, for every bike
// whose top command is 3..17, it asks the road where the bike should be aiming and turns the
// answer into the two numbers the rest of the simulation consumes: the 3D aim point `e[+0x370]`
// that the already-ported `BikeAimTarget` steers at, and the commanded speed `e[+0x39C]`.
//
//   stats = e[+0x22C];
//   if (e[+0x230] & 0x08000000) {                           // under AI control
//       e[+0x39C] = stats[+0xE0];
//       ahead = (|e[+0x158]| << 1) + FixMul(stats[+0x1A4], e[+0x1E0]);
//       ahead = clamp(ahead, 0x000F0000, 0x00500000);       // 15.0 .. 80.0 world units
//       dir   = (riderDef[0] & 0x80) ? -e[+0x16C] : e[+0x16C];
//       RoadLookAhead(e, ahead, e[+0x15C], dir, &e[+0x148], &e[+0x370], &cursor);
//       slice = cursor[+0x0C];
//       e[+0x36E] = (s16)(AiProject(&e[+0x370], slice + 2, slice + 20) >> 5);
//       e[+0x368] = slice[+2];  e[+0x36A] = slice[+4];  e[+0x36C] = slice[+6];
//   }
//   if ((u16)e[+0xAC] < gameState[+0x30]) {                 // a human player's bike
//       e[+0x140] |= 2;                                     // halfword
//       if (!(e[+0x230] & 0x08000000)) return;
//       if (gameState[+0x04] & 1) { ... 0x80096818 ... }     // NOT PORTED - see below
//       v = (riderDef[+0x45] * stats[+0xE0]) >> 7;          // with the `+127` negative rounding
//   } else {
//       v = AiTargetSpeed(e, dt);
//   }
//   if (v < e[+0x39C]) e[+0x39C] = v;
//
// **`SLUS 0x800386DC`, the road look-ahead, is NOT ported.** Its own tree is the road-chunk
// streaming layer - `0x80037A30` / `0x80037FBC` through a function pointer chosen by the sign of
// the travel direction, `0x80039048`, `0x800394F0`, `0x8001E0B4` and a dozen more below those -
// which is not ported. It is requested through
// `AiRoadQuery` and the bench supplies it from the oracle, with the SEVEN-argument extension of
// the seam.
//
// **One arm is not ported and says so.** With a human player's bike under AI control AND
// `gameState[+0x04] & 1`, the original calls `RASHCDG 0x80096818` and, on its answer, `0x8008A998`
// and `0x80086AF8`. None of the three is read yet, so `AiDrive` returns false there rather than
// guessing - the same rule `AiRunCommands` follows for the seventeen dispatch arms it does not
// contain.
struct AiRoadQuery {
    virtual ~AiRoadQuery() = default;
    // Runs `SLUS 0x800386DC(e, ahead, along, dir, e + cursorOffset, e + aimOffset, &out)` and hands
    // back the road slice the 32-byte output cursor's word at +0x0C points at, already resolved:
    // `sliceAxis` are the three halfwords at `slice + 2` and `sliceOrigin` the three words at
    // `slice + 20`. The entity's own bytes are updated in place, because the callee writes the aim
    // point at `e + aimOffset` and may advance the cursor at `e + cursorOffset`.
    virtual bool LookAhead(int32_t ahead, int32_t along, int32_t dir, uint32_t cursorOffset,
                           uint32_t aimOffset, int16_t sliceAxis[3], int32_t sliceOrigin[3]) = 0;
};

// The player-cop arm of AiDrive (0x80095618..0x800956A8, docs\formats\rules.md 9.4): a player's bike under
// AI control in a race type with bit 0 runs the arrest FSM RASHCDG 0x80096818 (modes.h ModeArrestFsm); on a
// yes, when the view's +0x21C differs from +0x220, CameraSetMode(view, view[+0x220]) 0x8008A998 and
// CameraSpringReset(view) 0x80086AF8. `ViewReset` makes that test and those two calls. Null (the bench's
// rows, and every caller that does not set it): the arm refuses, as before.
struct AiPlayerCopArm {
    virtual ~AiPlayerCopArm() = default;
    virtual bool Fsm(uint8_t* e, int32_t dt, uint32_t& v0) = 0;
    virtual bool ViewReset(uint8_t* e) = 0;
};

struct AiDriveEnv {
    const uint8_t* gameState = nullptr; // through the pointer at RASHCDG 0x8005B2F8
    uint8_t* riderDef = nullptr;        // e[+0x43C]; bytes +0x00 and +0x45 are read
    uint8_t* stats = nullptr;           // e[+0x22C]; +0xE0 and +0x1A4 are read, +0xE0 written by
                                        // the police arm of AiTargetSpeed
    AiRoadQuery* road = nullptr;        // SLUS 0x800386DC, supplied by the oracle
    AiSpeedEnv speed;                   // everything AiTargetSpeed reaches (ai.h above)
    AiPlayerCopArm* copArm = nullptr;   // the player-cop arm (above); null = refuse
};

// Returns false only on the unported human-player arm described above; true otherwise.
bool AiDrive(uint8_t* e, int32_t dt, const AiDriveEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x800BA304
// void AiDrivePass(s32 dt, u32 skip, u32 *maskB, u32 *maskC)
//
// The first of the race director's three passes. Once per frame it walks
// every live pool-0 bike, reads the top of its command stack and decides which of them drive.
//
//   for (i = 0; i < *(0x8005B1F8); i++, e += 1096) {
//       if (skip & (1 << i)) continue;
//       op = e->stack[(s8)e[0x3B2]].opcode;
//       if (e[0x234] & 0x200) *maskC |= 1 << i;          // written back either way
//       if (3 <= op && op < 18) {
//           AiDrive(e, dt);
//           if (op < 4) continue;                        // command 3 drives and nothing else
//           if (!AiRecoverLine(e)) continue;
//           *maskB |= 1 << i;
//           if (op < 5) continue;
//           if (riderDef[0x28] != 0) continue;           // a WORD: the finish time
//           if (riderDef[0x27] >= 248) continue;         // jailed / escaped
//           AiPopCommand(e);
//       } else {
//           if (riderDef[0x28] == 0) continue;           // ONLY riders who have finished
//           if (e[0x1E0] < 132) continue;                // ... and are still rolling
//           if (!(e[0x230] & 0x08000000)) continue;
//           if ((s16)e[0x140] == 0) continue;
//           AiDrive(e, dt);
//       }
//   }
//
// Note the ELSE branch at `0x800BA428` - the arm on which a rider whose race is already over keeps
// being driven while it still has speed - and that `0x800BC8DC` is behind `op >= 5` AND the rider
// having neither finished nor been jailed, not behind `op >= 5` alone.
//
// Every pointer chase is the caller's, as everywhere in this bench: `AiDriveNode` is one bike with
// its rider record, its stat block, its road slice and its rider object already resolved.
struct AiDriveNode {
    uint8_t* entity = nullptr;
    uint8_t* riderDef = nullptr;           // e[+0x43C]
    uint8_t* stats = nullptr;              // e[+0x22C]
    const int32_t* slicePos = nullptr;     // e[+0x154] + 20, read by AiRecoverLine's slow arm
    const int16_t* sliceTangent = nullptr; // e[+0x154] + 14, likewise
    const uint8_t* rider = nullptr;        // e[+0x354], read by AiPopCommand
    uint32_t riderAddress = 0;
    AiRoadQuery* road = nullptr;           // this bike's view of SLUS 0x800386DC
};

struct AiDrivePassEnv {
    const uint8_t* gameState = nullptr;
    int32_t raceBank = 0;                   // gameState[+0x3C], for AiRecoverLine
    const uint8_t* altKindTable = nullptr;  // for AiPopCommand's stance trigger
    const uint8_t* fightRecords = nullptr;
    const uint16_t* attackerMask = nullptr;
    AiStanceSink* stance = nullptr;
    AiSpeedEnv speed;                       // the globals AiTargetSpeed reaches
    AiPlayerCopArm* copArm = nullptr;       // AiDrive's player-cop arm; null = refuse
};

// Returns false as soon as an `AiDrive` declines, having run everything before it.
bool AiDrivePass(AiDriveNode* bikes, size_t count, int32_t dt, uint32_t skip, uint32_t* maskB,
                 uint32_t* maskC, const AiDrivePassEnv& env);

} // namespace rr::sim
