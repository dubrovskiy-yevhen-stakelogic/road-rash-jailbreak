#pragma once
// The race spine ported from the race overlay `RASHCDG.BIN`,
// SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8 (file offset 0 = that
// address). The bench that proves each function bit-exact against the original is
// `rrverify phys`.
//
// House rules this file follows, the same ones `bike.h` and `ai.h` follow:
//   * every pointer chase is the CALLER's, so a port never follows a guest pointer out of a byte
//     view. Everything the original reaches through a global or through
//     `entity[+0x43C]` arrives here already resolved, in an `*Env` struct;
//   * a callee that is not ported is asked for through an interface and SUPPLIED BY THE ORACLE,
//     never stubbed. A stub was measured and rejected on this project;
//   * a case the port cannot handle returns false rather than guessing.
#include <cstdint>

#include "game/sim/bike.h"
#include "game/sim/vec.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the seam interfaces
//
// `RASHCDG 0x80090270`, 388 bytes - the "GO" event. It takes NO arguments (its first instruction
// builds the pool table address at 0x800CE4D0 and it never reads a0..a3), walks entity pool 0 and
// the rider records, and contains no `jal` of its own. It is not ported; `TickCountdown` asks for
// it through this interface and the bench runs the ORIGINAL machine code for it, on the
// candidate's own memory, at the moment our code asks.
struct RaceGoEvent {
    virtual ~RaceGoEvent() = default;
    virtual void Go() = 0;
};

// The seven functions `RaceTick` dispatches to. None of them is ported - between them they are the
// whole race frame, some 40 KiB of the overlay - so all seven are supplied by the oracle. What the
// `race_tick` row proves is therefore the TICK ITSELF: its accumulator arithmetic, the planner
// arms, the countdown gate and the fact that the six children run, once each, in this order, with
// this argument; the bodies of the seven are trusted to the oracle.
struct RaceTickChildren {
    virtual ~RaceTickChildren() = default;
    virtual void AiPlan(int32_t arg) = 0;          // RASHCDG 0x800B8018, one argument
    virtual void RaceDirector(int32_t dt) = 0;     // RASHCDG 0x800B9414
    virtual void SpawnerPass(int32_t dt) = 0;      // RASHCDG 0x8008CD88
    virtual void WorldBikePass(int32_t dt) = 0;    // RASHCDG 0x8008AC80
    virtual void CollisionPass(int32_t dt) = 0;    // RASHCDG 0x800A4774
    virtual void RiderEnginePass(int32_t dt) = 0;  // RASHCDG 0x8008ACE8
    virtual void PresentationPass(int32_t dt) = 0; // RASHCDG 0x80090814
};

// ---------------------------------------------------------------------------- RASHCDG 0x8008AD38
// s32 TickCountdown(s32 dt), 348 bytes
//
// The pre-race countdown, and the only early exit from the race tick. It returns 1 when the
// simulation may run this frame and 0 when it may not, which is what freezes the WHOLE race for
// 3.00 seconds - not just the player's bike.
//
// Everything it touches, with the address it reads it from:
//
//   *(0x8005B38C)   player 1's bike, dereferenced UNCONDITIONALLY at 0x8008AD48 - the original has
//                   no null guard on it, so `player1` and `rider1` are never null here;
//   *(0x8005B21C)   player 2's bike, guarded on every use (0x8008AD6C, 0x8008ADCC, 0x8008AE2C);
//   bike[+0x43C]    -> the 0x800D5758 rider record; its byte +0x00 carries the two gate bits;
//   bike[+0x2D0]    forced to 0xFFFF0000 while the countdown holds, 0 at "GO";
//   *(0x8005B230)   the countdown itself, 16.16 SECONDS, counting DOWN (the same word the
//                   post-race delay later counts UP);
//   *(0x8005B30C)   the periodic accumulator that paces the AI planner, zeroed at "GO";
//   gameState+0x10  the race clock, zeroed twice: when the countdown is armed and again at "GO",
//                   which is why the clock never includes the countdown.
//
// The three arms, in the original's own order:
//
//   if (!(rider1[0] & 0x40)) return 1;                  // 0x8008AD5C - no countdown, run the tick
//   bike1[+0x2D0] = bike2[+0x2D0] = 0xFFFF0000;         // 0x8008AD70/74
//   if (rider1[0] & 0x20) {                             // 0x8008AD94 - ARM it
//       raceClock = 0;  countdown = 3.00;               // 0x8008ADA4, 0x8008ADB0 (lui v0,0x3)
//       rider1[0] &= 0xDF;  rider2[0] &= 0xDF;
//       return 0;
//   }
//   countdown -= dt;                                    // 0x8008ADFC
//   if (countdown >= 0) return 0;                       // 0x8008AE00
//   rider1[0] &= 0xBF;  rider2[0] &= 0xBF;              // "GO": release both riders
//   bike1[+0x2D0] = bike2[+0x2D0] = 0;
//   eventAcc = 0;  raceClock = 0;  Go();                // 0x8008AE64, 0x8008AE6C, 0x8008AE68
//   return 1;
//
// Note that the countdown is stored BEFORE the sign test, so the word ends the "GO" frame holding
// a small negative number rather than zero, and that both riders are released even though only
// player 1's flag was tested.
struct CountdownEnv {
    uint8_t* player1 = nullptr; // *(0x8005B38C), never null
    uint8_t* rider1 = nullptr;  // player1[+0x43C], never null
    uint8_t* player2 = nullptr; // *(0x8005B21C), or null in a one-player race
    uint8_t* rider2 = nullptr;  // player2[+0x43C] when player2 is not null
    uint8_t* raceClock = nullptr; // the four bytes at gameState+0x10, as bytes so that the caller
                                  // can hand over a slice of its own game-state image
    int32_t* countdown = nullptr; // *(0x8005B230)
    int32_t* eventAcc = nullptr;  // *(0x8005B30C)
    RaceGoEvent* go = nullptr;    // RASHCDG 0x80090270
};

int32_t TickCountdown(int32_t dt, const CountdownEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x8008AB00
// void RaceTick(s32 dt), 384 bytes
//
// The per-frame simulation step, called once from `RaceStep SLUS 0x80012524` with
// `dt = 218 * ticks` in 16.16 seconds. Everything in a race is a child of this call.
//
//   acc = (eventAcc += dt);                                       // 0x8008AB14..0x8008AB30
//   if (planCount != 0) {                                         // 0x8008AB2C - the planner has
//       if (0x8000 < acc) {                                       //   run, so the countdown is
//           AiPlan(acc); eventAcc = 0; ++planCount;               //   over and TickCountdown is
//       }                                                         //   NEVER CALLED AGAIN
//   } else if (TickCountdown(dt) == 0) {                          // 0x8008AB34
//       if ((s8)gameState[3] == 0) return;                        // 0x8008ABF8 - THE ONLY EXIT
//   } else {
//       a = eventAcc;                                             // re-read: "GO" zeroed it
//       if (0x8000 < a && !(0x8000 < a - dt)) AiPlan(0);           // 0x8008AB4C..0x8008AB68
//       else {
//           k = gameState[+0x3C] + ((gameState[4] & 4) ? 6 : (gameState[4] & 1) ? 3 : 0);
//           if (planTable[k] < eventAcc) { eventAcc = dt; ++planCount; }   // 0x8008ABC8
//       }
//   }
//   RaceDirector(dt); SpawnerPass(dt); WorldBikePass(dt);
//   CollisionPass(dt); RiderEnginePass(dt); PresentationPass(dt);  // 0x8008AC38..0x8008AC64
//
// Two details a port gets wrong if it paraphrases:
//   * the accumulator is written back BEFORE the planner test, so the `planCount != 0` arm's
//     `0x8000 < acc` tests the value including this frame's `dt`, while the countdown arm re-reads
//     the global afterwards and may see the 0 that "GO" just stored;
//   * the two `AiPlan` sites pass DIFFERENT arguments - `acc` at 0x8008AC20 (the argument register
//     still holds it from 0x8008AB28) and 0 at 0x8008AB64.
//
// `planTable` is `SLUS 0x80052FAC` - game data, and the original bounds its index by nothing at
// all, so the caller supplies the pointer and is responsible for the window.
struct RaceTickEnv {
    int32_t* eventAcc = nullptr;   // *(0x8005B30C)
    int32_t* planCount = nullptr;  // *(0x8005B2A8), the AI planner pass counter
    const uint8_t* gameState = nullptr; // *(0x8005B2F8)
    const int32_t* planTable = nullptr; // SLUS 0x80052FAC
    CountdownEnv countdown;
    RaceTickChildren* children = nullptr;
};

void RaceTick(int32_t dt, const RaceTickEnv& env);

// ---------------------------------------------------------------------------- SLUS 0x80012524
// void RaceStep(void), 296 bytes
//
// The resident half of the frame: `GameFrame 0x80011C4C` calls it once, it turns the frame delta
// into the simulation's time step, runs the tick, and then runs the camera once per player.
//
//   ticks = gameState[+0x18];                       // the frame delta, 1/300 s units
//   if (ticks == 0) { gameState[+0x1C] = 0; return; }
//   if (ticks >= 31) ticks = 30;                    // SIGNED, so a negative delta is not clamped
//   gameState[+0x1C] = ticks;
//   dt = 218 * ticks;                               // built as ((7t << 5) - 7t) + t, 16.16 SECONDS
//   *(0x8005B580) = 0;
//   gameState[+0x10] += ticks;                      // the race clock, in TICKS, not in seconds
//   RaceTick(dt);
//   for (p = 0; p < (unsigned)gameState[+0x30]; p++) {
//       view = 0x800CD898 + 1132*p;
//       view[+0x1D4..+0x1DF] = view[+0x0B8..+0x0C3];         // last frame's camera position
//       ViewUpdate(view, dt);                                 // RASHCDG 0x800881B4
//       if ((view[+0x224] & 0x100) && 0x800A421C(view)) 0x80086E1C(view);
//   }
//
// **The unit split is the whole point of this function.** The race clock advances in ticks and the
// simulation in 16.16 seconds, and `218 = floor(65536/300)`, so the two drift by 0.2 % by
// construction. A port that derives `dt` from the race clock is not bit-exact.
//
// Two details that are easy to paraphrase away: the player count is RE-READ out of `game_state` on
// every iteration (0x80012608..0x80012614), so a camera arm that changes it changes the loop; and
// the comparison is UNSIGNED while the clamp above is signed.
struct RaceStepCamera {
    virtual ~RaceStepCamera() = default;
    // The 1132-byte view record of player `index`. The pointer stays valid across the calls below,
    // and its contents are refreshed by them - the caller owns the record, as everywhere here.
    virtual uint8_t* View(int32_t index) = 0;
    virtual void Update(int32_t index, int32_t dt) = 0; // RASHCDG 0x800881B4(view, dt)
    virtual int32_t Test(int32_t index) = 0;            // RASHCDG 0x800A421C(view)
    virtual void Apply(int32_t index) = 0;              // RASHCDG 0x80086E1C(view)
};

struct RaceStepEnv {
    uint8_t* gameState = nullptr; // *(0x8005B2F8); +0x10, +0x18, +0x1C and +0x30 are all read here
    int32_t* frameFlag = nullptr; // *(0x8005B580), zeroed once per frame
    RaceStepCamera* camera = nullptr;
    RaceTickEnv tick;
};

void RaceStep(const RaceStepEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x800B9794
// void ProgressPass(u32 *skip), 452 bytes
//
// The first thing `RaceDirector` runs after the end-of-race test. Once per frame it walks every
// live pool-0 bike, clears three per-frame flags, runs the finish test on anyone who has not
// finished yet, and builds the mask of riders the three AI passes behind it must SKIP.
//
//   for (i = 0; i < *(0x8005B1F8); i++) {                 // re-read every iteration
//       e = *(0x8005B3A0) + 1096*i;
//       if (e[+0x0AC] >= gameState[+0x30]                  // not a player, and
//           && (e->riderDef[0x01] & 0xF) == 2              // police, and
//           && !(e[+0x3A0] & 0x10))                        // not kept in
//           { *skip |= 1 << i; continue; }
//       e->owner[+0x228] &= ~0x02000000;
//       e[+0x230] &= ~0x00040000;
//       e[+0x3A0] &= ~0x40;
//       done = e->riderDef[0x28] != 0 || FinishTest(e);    // 0x800B9958, THE FINISH TEST
//       if (done) {
//           p = e->riderDef[0x27];
//           if (p <= 246 || p == 250 || p == 254 || p == 255) e[+0x230] |= 0x08000000;
//           if ((u8)(p + 2) < 2) { *skip |= 1 << i; continue; }   // 254 and 255: wrecked / quit
//       }
//       if ((s16)e[+0x140] != 0) continue;                 // still live: do not skip
//       if ((u32)e->owner[+0x25C] < 3) { *skip |= 1 << i; continue; }
//       if ((s16)e->owner[+0x140] != 0) continue;
//       *skip |= 1 << i;
//   }
//
// Two details that only come out of the instruction words: the shift
// is `sllv`, so the mask wraps at 32 riders rather than saturating, and the result-code test
// `(u8)(place + 2) < 2` is the compiler's way of writing "254 or 255" - the two codes `rules.md`
// 7.1 calls wrecked and quit.
struct RaceFinishTest {
    virtual ~RaceFinishTest() = default;
    virtual int32_t Test(int32_t index) = 0; // RASHCDG 0x800B9958(entity)
};

struct ProgressNode {
    uint8_t* entity = nullptr;   // the 1096-byte pool-0 slot
    uint8_t* riderDef = nullptr; // entity[+0x43C]
    uint8_t* owner = nullptr;    // entity[+0x354]
};

struct ProgressPassEnv {
    const uint8_t* gameState = nullptr; // *(0x8005B2F8); +0x30 is re-read on every iteration
    const int32_t* liveBikes = nullptr; // *(0x8005B1F8); likewise
    ProgressNode* bikes = nullptr;      // resolved by the caller, in pool order
    int32_t count = 0;                  // how many of them the caller resolved
    RaceFinishTest* finish = nullptr;
};

// Returns false - having done everything up to that point - when the loop reaches a bike the caller
// did not resolve, rather than guessing at one. The same rule `AiRunCommands` follows.
bool ProgressPass(uint32_t* skip, const ProgressPassEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x800B9414
// void RaceDirector(s32 dt), 896 bytes
//
// Child 1 of the race tick, and **the function that ends a race**. Three parts:
//
//  1. a per-player loop (0x800B9480..0x800B9630) that remounts a fallen rider and builds the mask
//     of players whose result code is in. When every player is in it accumulates `dt` into
//     `*(0x8005B230)` - the same word the countdown counted DOWN - until it passes the 5.00 s in
//     `*(0x8005B228)`, and then writes `game_state+0x00`: **2 when `*(0x8005B220)` is set, 6
//     otherwise**. A race is over when the PLAYERS are done; nothing waits for the AI field;
//  2. the progress pass, then the three AI passes, the first of which fills two masks in this
//     function's own stack frame;
//  3. a second per-player loop for the two-rider bike: opcode 16 on the partner's command stack
//     runs `0x800C035C`, and opcode 18 with `game_state+0x39 == 2` runs `0x80092E04`.
//
// Details of the transcription:
//   * the remount arm dereferences `entity[+0x358]` with **no null guard** (0x800B94A4), so it is
//     reachable only on a two-rider bike; on a single-rider bike with `owner[+0x23C] & 0x10` set
//     the original faults. That was measured, not deduced - see the `race_director` row;
//   * the player count is re-read from `game_state` inside BOTH loops and the comparison is
//     unsigned, while `(1 << n) - 1` is built with `sllv`, so it wraps at 32 players;
//   * both parts of part 3 read the SAME command-stack slot, `partner[+0x3B4 + 8*depth]`: the
//     first arm reaches it as `+0x3BC + 8*depth - 8` and the second as `+0x3BC + 8*(depth-1)`.
struct RaceDirectorCallbacks {
    virtual ~RaceDirectorCallbacks() = default;
    virtual void RiderRemount(int32_t player) = 0; // RASHCDG 0x800903F4(entity, 1)
    virtual void ResultsPrepare() = 0;             // RASHCDG 0x8003F708()
    virtual void RaceOverSignal() = 0;             // SLUS   0x80018C1C(1)
    // RASHCDG 0x800BA304(dt, skip, &maskB, &maskC) - the two masks are OUTPUTS in the original's
    // own stack frame, which is why they are passed by pointer here too.
    virtual void AiDrivePass(int32_t dt, uint32_t skip, uint32_t* maskB, uint32_t* maskC) = 0;
    virtual void AiRunCommands(int32_t dt, uint32_t skip, uint32_t maskB) = 0; // 0x800BA4CC
    virtual void AiBrainPass(int32_t dt, uint32_t skip, uint32_t maskC) = 0;   // 0x800BCD48
    virtual void PartnerCommand(int32_t player, uint32_t arg, int32_t dt) = 0; // 0x800C035C
    virtual void PartnerFinish(int32_t player, int32_t dt) = 0;                // 0x80092E04
};

struct RaceDirectorPlayer {
    uint8_t* entity = nullptr;       // *(0x8005B268 + 4p)
    const uint8_t* riderDef = nullptr; // entity[+0x43C]
    uint8_t* owner = nullptr;        // entity[+0x354]
    uint8_t* partner = nullptr;      // entity[+0x358], null on a single-rider bike
    uint8_t* partnerOwner = nullptr; // partner[+0x354], when there is a partner
    const uint8_t* view = nullptr;   // 0x800CD898 + 1132p; only bit 0x8 of +0x228 is read
};

struct RaceDirectorEnv {
    uint8_t* gameState = nullptr;         // +0x00 is WRITTEN here: 6 or 2, the end of the race
    RaceDirectorPlayer* players = nullptr;
    int32_t playerCount = 0;              // how many the caller resolved
    const uint8_t* kindTable = nullptr;   // SLUS 0x800541D4, 8-byte records; index unbounded
    int32_t* postDelay = nullptr;         // *(0x8005B230)
    const int32_t* postLimit = nullptr;   // *(0x8005B228), 5.00 s in 16.16
    const int32_t* skipResults = nullptr; // *(0x8005B220)
    ProgressPassEnv progress;
    RaceDirectorCallbacks* calls = nullptr;
};

// Returns false - having run everything before that point - when a loop reaches a player the caller
// did not resolve, or when the original would dereference a null partner.
bool RaceDirector(int32_t dt, const RaceDirectorEnv& env);

// ============================================================ progress and place
//
// Four functions of the resident executable, and they form a closed tree: nothing below reaches a
// callee that is not here.

// ---------------------------------------------------------------------------- SLUS 0x8003B4B0
// const Leg *FindRouteLeg(const RouteObject *o, u32 key), 112 bytes
//
//   if (*(s16*)0x800D6182 == -1) return 0;          // no route is loaded
//   if (o == 0) return 0;
//   for (i = 0; i < o[+0x0C]; i++)
//       if (*(u32*)(o + 20 + 16*i) == key) return o + 20 + 16*i;
//   return 0;
//
// The original returns the ENTRY'S GUEST ADDRESS. A native port has no guest addresses, so this
// returns the entry's index and the caller turns it back into an address - the same
// caller-resolves-pointers seam as everywhere else. `legCount` is
// `o[+0x0C]`, read by the caller because `o` may be null.
int32_t RouteFindLeg(const uint8_t* legs, int32_t legCount, uint32_t key, int16_t routeArmed);

// ---------------------------------------------------------------------------- SLUS 0x8003B8F4
// s32 RouteBindingValid(void *p), 120 bytes, called as `RouteBindingValid(entity + 0xAC)`
//
//   if (p == 0 || p[+0x100] == 0) return 0;         // entity[+0x1AC], the route object
//   w = p[+0x0BC];                                  // entity[+0x168]: roadId | (kind << 16)
//   if ((w >> 16) == 1) return (w & 0xFFFF) == *(u32*)p[+0x100];
//   return FindRouteLeg(p[+0x100], w & 0xFFFF) != 0;
//
// So `entity[+0x168]`'s two halves - the road id and the `kind` - are what
// decides whether the entity's cached route binding still describes where it is. `kind == 1` (in a
// node) compares the road id against the object's own first word; anything else searches the leg
// table.
struct RouteBinding {
    const uint8_t* routeObject = nullptr; // entity[+0x1AC], or null
    uint32_t firstWord = 0;               // *(u32*)routeObject, when it is not null
    const uint8_t* legs = nullptr;        // routeObject + 20
    int32_t legCount = 0;                 // routeObject[+0x0C]
    int16_t routeArmed = 0;               // *(s16*)0x800D6182; -1 means "no route"
};

int32_t RouteBindingValid(const uint8_t* entity, const RouteBinding& b);

// ---------------------------------------------------------------------------- SLUS 0x8003B96C
// s32 ProgressOf(Entity *e), 60 bytes
//
//   return RouteBindingValid(e + 0xAC) ? e[+0x144] : 0x7FFFF000;
//
// `entity+0x144` is the 20.12 distance remaining (`rules.md` 6.2): SMALLER IS AHEAD, and a rider
// whose route binding has gone stale is parked at the back with a sentinel rather than compared.
int32_t ProgressOf(const uint8_t* entity, const RouteBinding& b);

// ---------------------------------------------------------------------------- SLUS 0x800138E8
// s32 ComputePlace(Entity *e, s32 mode), 508 bytes
//
// The place of one rider, recomputed from scratch - there is no stored ranking.
//
//   if ((e->riderDef[1] & 0xF) == 2 && e->riderDef[0x27] < 247) {      // an unfinished police bike
//       if (e[+0x0AC] >= gameState[+0x30]                              // ... that is not a player
//           || ((gameState[4] & 1) && !(*(0x8005AD48) & 1)))
//           return *(0x8005B1F8) + 1;                                  // parked behind the field
//   }
//   if (e->riderDef[0x28] != 0 || e->riderDef[0x27] >= 248) return e->riderDef[0x27];
//   mine = ProgressOf(e);
//   place = 1;
//   for (i = pool0.high; i >= 0; i--) {
//       o = pool0.base + stride*i;
//       if (e[+0x0AC] >= gameState[+0x30] && (o->riderDef[1] & 0xF) == 2) continue;  // no police
//       if (o[+0x0AC] == e[+0x0AC]) continue;                                        // not me
//       if (*(0x8005B1F8) < o->riderDef[0x27]) continue;                             // not a code
//       if (mode == 1) { place += (o->riderDef[0x28] > 0); continue; }
//       place += (o->riderDef[0x28] > 0 || ProgressOf(o) < mine);
//   }
//   return place;
//
// Three things worth naming. The police test on the OTHER bike is gated on whether *we* are a
// player, not on what the other bike is. `mode == 1` counts only riders who have already finished,
// which is what the finishing-order path wants. And the loop runs from the top of the pool down,
// so it is O(n) per rider and O(n^2) per field - which is why the game does NOT recompute it for
// everyone every frame.
struct PlaceNode {
    const uint8_t* entity = nullptr;
    const uint8_t* riderDef = nullptr;
    RouteBinding binding;
};

struct ComputePlaceEnv {
    const uint8_t* gameState = nullptr;
    int32_t liveBikes = 0;   // *(0x8005B1F8)
    uint32_t raceFlags = 0;  // *(0x8005AD48)
    PlaceNode* pool = nullptr;
    int32_t poolHigh = -1;   // **(0x800CE4D0 + 0x0C), the top slot index
    int32_t poolCount = 0;   // how many of them the caller resolved
};

// `place` is the answer; the return value is false when the loop reached a slot the caller did not
// resolve, exactly as the two passes above.
bool ComputePlace(const uint8_t* entity, const uint8_t* riderDef, const RouteBinding& binding,
                  int32_t mode, const ComputePlaceEnv& env, int32_t* place);

// ---------------------------------------------------------------------------- RASHCDG 0x800B9958
// s32 FinishTest(Entity *e), 2476 bytes - **the function that lets a rider finish**
//
// `ProgressPass` runs it once per non-police bike per frame (16 times for an 18-bike pool).
// It answers "is this rider done", and on a yes it also writes the result:
// the place byte `riderDef+0x27`, the finish stamp `riderDef+0x28` and the finished flag
// `entity+0x230` bit `0x08000000`. Until it exists a race cannot end.
//
// Five independent ways in, read out of the instruction words and given here in the original's own
// order. Everything below is `[established]` from our own disassembly and `[proven]` by the
// `finish_test` bench row.
//
//  1. **Already finished** (0x800B998C). `riderDef+0x28 != 0` returns 1 at once, setting the
//     finished flag for a player that somehow lost it. This is the arm `ProgressPass`'s
//     `riderDef[0x28] != 0 || FinishTest(e)` short-circuits past, so the pass never reaches it -
//     but the two other callers of the finish test do.
//  2. **The clocks** (0x800B99E0..0x800B9B18), players only and never in race type 33:
//       * `0 < *(0x8005B220) < 3` and a race clock past **108000 ticks (6:00)**;
//       * race type 36 past `*(0x8005ACC8)`                              -> place 250;
//       * race type odd and `*(0x8005ACC8) - (clock - *(0x8005ACD0)) <= 0` -> place 250;
//       * race type with bit 2: past **216000 ticks (12:00)** when the type is exactly 44
//         (Jailbreak), past 108000 otherwise.
//     A hit here jumps STRAIGHT to the result block, skipping the route test entirely.
//  3. **The milestone** (0x800B9B34), race type 44 and a player. `SLUS 0x80053174 + 4*gs[+0x39]` is
//     a 4-byte record `{u16 road; u16 along}`; the road half is compared against the WHOLE of
//     `entity+0x168`, so it matches only while `kind == 0`. On milestone 0, past the time limit and
//     with the finished flag still clear, being within **400 world units** of the milestone's
//     `along` sets `entity+0x230 |= 0x28000000` and anything else is place 250.
//  4. **The finish line** (0x800B9CF0). The route record at `*(0x800D6188)` carries
//     `{u32 road; s32 line; s32 side; s32 leg}`: the rider is done when its distance along has
//     reached the far side of `line`, i.e. when `side ^ (along - line) >= 0`, and its binding
//     matches `leg` (or `leg == -1`). The same test is then run for the PARTNER of a two-rider
//     bike, whose answer is OR-ed in.
//  5. **The milestone advance** (0x800B9E34), when the road matched in 3. Crossing the milestone's
//     `along` in the direction `*(0x8005B2E8)` bumps `gs[+0x39]` and the answer becomes whatever
//     `RASHCDG 0x800C8D4C` returns; `gs[+0x39] == 1` afterwards also runs `RASHCDG 0x800C92F8`.
//
// And when the answer is yes and the rider is a PLAYER (0x800B9F28), the result code is chosen from
// the race type and `gs[+0x39]` - 250, 251 or 254 - the Jailbreak clock `*(0x800D9C4C)` is stamped,
// race type 17 hands the OTHER player the same finish time, and then, for every rider:
//
//   if (riderDef[0x27] < 247) riderDef[0x27] = ComputePlace(e, 1);   // 0x800BA114
//   riderDef[0x28] = gameState[+0x10];                                // the finish stamp
//   0x800BC7CC(e);  SLUS 0x8003F680(handle, 1);                       // the result records
//
// Three things worth naming because a paraphrase loses them:
//   * the finish-line arm **clears** the answer built by arms 2 and 3 (`move s3,zero` at
//     0x800B9CFC) before recomputing it, so reaching it resets an earlier yes;
//   * `entity+0x172` - the integer half of the 16.16 distance `entity+0x170` - is read as a SIGNED
//     halfword for the 400-unit milestone window, so the window wraps at 32768 units;
//   * the whole two-player tail at 0x800BA148 reads one word of the caller's frame that not every
//     path writes. `FinishTest` here returns false rather than inventing a value for it; see
//     `frameWordSet` below.
struct FinishTestCalls {
    virtual ~FinishTestCalls() = default;
    // RASHCDG 0x800C8D4C, 1452 bytes. NO arguments - it writes `a0` before it reads it - and its
    // body is a five-arm jump table on `gameState[+0x39] - 1`. Not ported: supplied by the oracle.
    virtual int32_t MilestoneAdvance() = 0;
    // RASHCDG 0x800C92F8, likewise no arguments. Not ported: supplied by the oracle.
    virtual void MilestoneFirst() = 0;
    // RASHCDG 0x800BC7CC(entity), one argument - the same callee `EndRace` uses. Not ported.
    virtual void StampResult() = 0;
    // SLUS 0x8003F680(handle, 1), two arguments, 136 bytes and no `jal` of its own. Not ported.
    virtual void RecordFinish(uint32_t handle, int32_t flag) = 0;
    // SLUS 0x800394F0(&entity[+0x148], entity[+0x16C]), two arguments - the road-graph step, part
    // of the chunk-streaming family. Not ported.
    virtual int32_t RoadAdvance() = 0;
    // `ComputePlace(entity, 1)` - PORTED (`SLUS 0x800138E8`, above). It is reached through an
    // interface only because its pool walk is a pointer chase, which is the caller's job here
    // exactly as it is for `BikeSteerDriver`'s list. Returning false
    // declines the case, as everywhere in this file.
    virtual bool ComputePlaceMode1(int32_t* place) = 0;
};

struct FinishTestEnv {
    uint8_t* entity = nullptr;       // the 1096-byte pool-0 slot; null returns 0 as the original does
    uint8_t* riderDef = nullptr;     // entity[+0x43C]
    uint8_t* owner = nullptr;        // entity[+0x354]
    uint8_t* partnerOwner = nullptr; // entity[+0x358][+0x354]; the original has NO null guard
    uint8_t* gameState = nullptr;    // *(0x8005B2F8); +0x39 is WRITTEN here
    // The three route objects the function may test: whichever of the entity's or the owner's
    // `+0x1AC` the `owner[+0x25C] < 3` gate selects, and the partner's.
    const uint8_t* entityRoute = nullptr;
    const uint8_t* ownerRoute = nullptr;
    const uint8_t* partnerRoute = nullptr;
    const uint8_t* routeRecord = nullptr; // *(0x800D6188); +0x00 road, +0x04 line, +0x08 side,
                                          // +0x0C leg. Dereferenced with no null guard.
    RoadSliceView slice{nullptr, nullptr}; // entity[+0x154], for the projection arm
    int32_t sliceAlongBase = 0;            // *(s32*)(entity[+0x154] + 40)
    int16_t sliceIndex = 0;                // *(s16*)(entity[+0x154] + 0)
    int16_t chunkLast = 0;                 // *(s16*)(entity[+0x150] + 10)
    bool sliceValid = false;
    bool chunkValid = false;
    const uint8_t* milestones = nullptr;   // SLUS 0x80053174, 4-byte records, index gameState[+0x39]
    int32_t milestoneCount = 0;            // the window the caller is willing to answer for
    int16_t routeArmed = 0;                // *(s16*)0x800D6182; -1 short-circuits the route arms
    const int32_t* raceOverFlag = nullptr; // *(0x8005B220)
    const int32_t* timeLimit = nullptr;    // *(0x8005ACC8), ticks
    const int32_t* timeBase = nullptr;     // *(0x8005ACD0), ticks
    const int32_t* startDir = nullptr;     // *(0x8005B2E8), the milestone's travel direction
    int32_t* postDelay = nullptr;          // *(0x8005B230), zeroed when a player finishes
    uint8_t* jailbreakClock = nullptr;     // *(0x800D9C4C), 4 bytes, stamped with the race clock
    uint8_t* finishOrder = nullptr;        // 0x800D5DA8, 16-byte records indexed by place - 1
    int32_t finishOrderCount = 0;
    const uint8_t* player2 = nullptr;      // *(0x8005B21C), compared against `entity` by identity
    uint8_t* p1Entity = nullptr;           // *(0x8005B38C)
    uint8_t* p1RiderDef = nullptr;         // p1Entity[+0x43C]
    // The two entries of `*(0x8005B268)`, already resolved to rider records: the race-type-17 arm
    // reads the OTHER player's, chosen as `(handle == 0) ? 1 : 0`.
    uint8_t* playerRiderDef[2] = {nullptr, nullptr};
    FinishTestCalls* calls = nullptr;
};

// `*result` is the original's `v0`. The return value is false when the port declines - a null the
// original would have faulted on, an index outside the window the caller declared, or the
// uninitialised caller-frame word of the two-player tail - exactly as `AiRunCommands` declines its
// seventeen unwritten arms rather than guessing.
bool FinishTest(const FinishTestEnv& env, int32_t* result);

// ---------------------------------------------------------------------------- SLUS 0x8003F680
// void RecordFinish(u32 handle, s32 flag), 136 bytes, and it has no `jal` of its own
//
// One of the two result records `FinishTest` writes at 0x800BA140. It fills a 16-byte entry of the
// finishing-order table at **`0x800D5D98`**, indexed by the rider's PLACE:
//
//   e  = *(0x8005B3A0) + 1096 * handle;     // ((((h<<4)+h)<<3)+h)<<3 at 0x8003F680..0x8003F698
//   rd = e[+0x43C];
//   if ((u32)(rd[0x27] - 1) < 18) {         // a place, not a result code, and inside the table
//       rec = 0x800D5D98 + 16 * rd[0x27];
//       rec[0] = e[+0x0AC];                 // the HANDLE READ BACK OUT OF THE ENTITY,
//       rec[1] = rd[0x27];                  //   not the argument it was just indexed by
//       rec[2] = flag;
//       rec[3] = rd[+0x28];                 // the finish stamp
//   }
//
// Two details worth keeping. The stored handle is re-read from `entity+0x0AC` rather than taken
// from `a0`, so a caller that passes a handle the pool does not match writes the pool's. And the
// table is the same one `FinishTest`'s two-player tie-break reaches as `0x800D5DA8 + 16*(place-1)`
// - the two addresses are one table, sixteen bytes apart in indexing.
struct FinishOrderEnv {
    const uint8_t* entity = nullptr;   // *(0x8005B3A0) + 1096*handle, resolved by the caller
    const uint8_t* riderDef = nullptr; // entity[+0x43C]
    uint8_t* table = nullptr;          // 0x800D5D98
    int32_t tableBytes = 0;            // the window the caller is willing to answer for
};

// False when the caller did not resolve the entity, or when the record would fall outside the
// window it declared. The original bounds the index only by `place - 1 < 18`.
bool RecordFinish(int32_t flag, const FinishOrderEnv& env);

// ---------------------------------------------------------------------------- RASHCDG 0x80092C7C
// void EndRace(Entity *e, s32 reason), 392 bytes
//
// What a finished race writes back, and the other four of the race's five ways out: a
// wipeout (`reason == 9`) stamps result code **255**, anything else **254**.
//
//   e[+0x230] |= 0x08000000;                       // the finished flag
//   *(0x8005B230) = 0;                             // the countdown / post-race word, unconditional
//   if (riderDef[+0x28] == 0) {                    // only the FIRST call does anything
//       riderDef[0x27] = (reason == 9) ? 255 : 254;
//       riderDef[0x28] = gameState[+0x10];
//       0x800BC7CC(e);
//       if (riderDef[0x27] == 255 && (u32)owner[+0x25C] < 2) {
//           SLUS 0x8002090C(e);  e[+0x238] |= 0x08000000;
//       }
//   }
//   view = 0x800CD898 + 1132 * e[+0x0AC];          // INDEXED BY THE HANDLE, not by the player
//   if (reason == 10) { view[+0x304] = 1;  view[+0x228] |= 0x14; }
//   else if (!(view[+0x228] & 4)) RASHCDG 0x8008A998(view, reason);
//
// The view index is the fact worth pulling out: the stride is built as
// `((((h<<3)+h)<<3)-h)<<2)-h)<<2` = 1132 at 0x80092D44..0x80092D5C and the base is 0x800CD898, i.e.
// the same two-entry camera array `RaceStep` drives - but indexed by the bike's own handle, so a
// call on any bike past handle 1 reaches past the array. The original bounds it by nothing; the
// caller supplies the pointer.
struct EndRaceCalls {
    virtual ~EndRaceCalls() = default;
    virtual void StampResult() = 0;              // RASHCDG 0x800BC7CC(entity)
    virtual void WipeoutEffect() = 0;            // SLUS   0x8002090C(entity)
    virtual void ViewEvent(int32_t reason) = 0;  // RASHCDG 0x8008A998(view, reason)
};

struct EndRaceEnv {
    uint8_t* entity = nullptr;
    uint8_t* riderDef = nullptr;     // entity[+0x43C]
    const uint8_t* owner = nullptr;  // entity[+0x354]
    uint8_t* view = nullptr;         // 0x800CD898 + 1132 * entity[+0x0AC]
    const uint8_t* gameState = nullptr;
    int32_t* postDelay = nullptr;    // *(0x8005B230)
    EndRaceCalls* calls = nullptr;
};

// False when the caller did not resolve something the original dereferences without a guard.
bool EndRace(int32_t reason, const EndRaceEnv& env);

} // namespace rr::sim
