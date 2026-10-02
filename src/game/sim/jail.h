#pragma once
// src\game\sim\jail - the Jailbreak mode's escape scene (docs\formats\rules.md 9.6 / 9.7 / 16), ported
// from our own disassembly of the player's images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by its `rrverify phys` row (tools\rrverify\rows_jail.inc, `jail_escape_scene`).
//
// MEMORY MODEL (recover.h's): guest addresses through `GuestRam`; `sp` is the stack pointer at the
// function's entry. The locals whose address a callee receives - the road key sp+80, the cursor sp+96,
// the car placement record sp+128, the roadblock record sp+16, the turn matrix sp+200 - live in guest
// RAM at the original's frame offsets (frame 288), so the fields the original never writes (most of
// the 68-byte car record) are whatever that memory holds, on both sides of the bench. Every callee
// with a frame of its own is called through `EscapeCallees` at the original's depth (sp - 288); the
// leaves - memcpy / memset, MulAdd SLUS 0x8002EAD8, MulMatrix0 0x8003FA40, CopyHalfwords 0x8003FA18,
// RatAtan2 0x80020018, ModelKeySet 0x800302C4, VecMat 0x8002EFF4 - are run inline.
#include <cstdint>

#include "game/sim/integrator.h" // BikeTables
#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kJailEscapeSceneFn = 0x800C9420;
constexpr uint32_t kJailBlockCount   = 0x8005B208; // s32[2]: records in STARTJBA block i (RASHCDI 0x800688D0)
constexpr uint32_t kJailBlockArray   = 0x8005B340; // ptr[2]: 12-byte records {lateral, along, packed}
constexpr uint32_t kJailFirstBike    = 0x8005B330; // the first bike the scene placed (group 0)
constexpr uint32_t kJailEscapeCar    = 0x800CCC18; // the first moving police car of block 0 (group 3)
constexpr uint32_t kJailFrame        = 288;        // `addiu sp,sp,-288` at 0x800C9420

// The callees of 0x800C9420 that have a frame (or a hardware read) of their own. `sp` is the caller's
// stack pointer at the call (the escape scene's own sp - 288). False: the callee refused / faulted.
class EscapeCallees {
public:
    virtual ~EscapeCallees() = default;
    virtual bool SeatRelease(uint32_t b, uint32_t r, uint32_t idx, uint32_t sp) = 0;          // 0x80068D20
    virtual bool Placement(uint32_t e, uint32_t sp) = 0;                                      // 0x8009432C
    virtual bool RoadGate(uint32_t h, uint32_t key, uint32_t sp, uint32_t& v0) = 0;           // SLUS 0x80039DFC
    virtual bool CursorSeat(uint32_t obj, uint32_t key, uint32_t cursor, uint32_t sp, uint32_t& v0) = 0; // SLUS 0x8003A700
    virtual bool AxisRotation(uint32_t axis, int32_t ang, uint32_t out, uint32_t sp) = 0;     // SLUS 0x8003FB34
    virtual bool BuildObb(uint32_t e, uint32_t sp) = 0;                                       // 0x8008BA18
    virtual bool RoadUpdate(uint32_t e, uint32_t sp) = 0;                                     // SLUS 0x80037450
    virtual bool WeaponObject(uint32_t r, uint32_t side, uint32_t sp) = 0;                    // 0x800958F0
    virtual bool GetRCnt(uint32_t id, uint32_t sp, uint32_t& v0) = 0;                         // SLUS 0x80043F00
    virtual bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) = 0;           // 0x800C4550
    virtual bool CarSpawn(uint32_t rec, uint32_t a1, uint32_t sp, uint32_t& v0) = 0;          // 0x8009AD48
    virtual bool Roadblock(uint32_t rec, uint32_t player, uint32_t sp) = 0;                   // 0x800A2630
};

// RASHCDG 0x800C9420 EscapeScene(block), 661 instructions, frame 288: stages STARTJBA.BIN block
// `block` (1 = the player reached the jail in time, 0 = the deadline expired) at the milestone anchor
// SLUS 0x80053178 {road, along}: group 0 - the escape bikes (the next pool-0 bike of the other class,
// placed and re-seated by Placement; block 1 parks them on command 0 and their riders on mount 4);
// group 1 - the riders on foot at the jail walking pool 0 down from the pool-1 count (the player's
// passenger's rider joins the player's sidecar: bits 0x60 / 0x10 of +0x23C), their stance 64 / 65 /
// 66 with the prisoner skin 151; group 3 - the police cars (CarSpawn; parked in block 1, moving at
// 2.4 in block 0); group 4 - the roadblock props (0x800A2630). The records are negated in place
// when player 1 drives the road backwards. False when a callee refused or the view faulted.
bool EscapeScene(GuestRam& g, int32_t block, uint32_t sp, EscapeCallees& c);

constexpr uint32_t kJailFinishFn = 0x800C9E74;
constexpr uint32_t kJailBoardFn  = 0x800CA05C;

// The command-stack callees of the two functions below (ai.h's AiPopCommand 0x800BC8DC and
// AiPushCommand 0x800BCA68, the latter reaching the stance event). `cmd` is the 8-byte command record in
// the caller's frame. False: the callee refused / faulted.
class JailCallees {
public:
    virtual ~JailCallees() = default;
    virtual bool PopCommand(uint32_t e, uint32_t sp) = 0;                            // 0x800BC8DC AiPopCommand
    virtual bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) = 0; // 0x800BCA68
};

// RASHCDG 0x800C9E74 JailbreakFinish(), 106 instructions, frame 32: the AI drive's op-2 arm for player 1
// in Jailbreak phases 1 and 2 (0x800BAE14). More than 10.0 short of the stop (the along of milestone
// gs+0x39 - 1 less *(s8 0x8005ADF2) << 16): the speed target +0x39C = rd+0x45 x stats+0xE0 / 128 and the
// aim point +0x370 on the road ahead (up to 40.0; within 40.0 also the lateral *(s8 0x8005ADF3) << 16);
// at the stop: speed 0, the aim point 8 x +0x134 along the facing +0x1C2 from +0x1F8, +0x230 bit 18, the
// steering +0x394 / +0x398 cleared once (+0x234 bit 19), and in phase 2 with the bike down to 0.5 and
// the passenger bike's top command 0 the partner is sent to board: JailBoard(0), run natively.
bool JailbreakFinish(GuestRam& g, uint32_t sp, const BikeTables& t, JailCallees& c);

// RASHCDG 0x800CA05C JailBoard(late), 244 instructions, frame 96. late == 0 (JailbreakFinish): the
// passenger bike's top command popped and op 18 (walk to player 1's bike and board it) pushed, player 1's
// rider +0x23C bit 4 (a passenger rides); late != 0 (RiderRecover 0x80092EB0: player 1 off the bike in
// phase 2): player 1's result 254 stamped at the race clock. Then every live bike of neither the police
// class nor player 1's whose rider stands in stance 64 (the guards) is given op 18 too - when late, only
// a guard with player 1's rider beyond 4.0 and facing it (the cosine of the rider-to-player and
// rider-to-bike vectors above 0xCCCB), and a guard nearer than that sets the post-race word *(0x8005B228)
// to 5.0 instead; when not late each guard's +0x3C8 is drawn from Rand between *(0x8005ADE8) and
// *(0x8005ADEC). A late call that sent any guard sets *(0x8005B228) = *(0x8005ADE8) + 10.0.
bool JailBoard(GuestRam& g, uint32_t late, uint32_t sp, const BikeTables& t, JailCallees& c);

} // namespace rr::sim
