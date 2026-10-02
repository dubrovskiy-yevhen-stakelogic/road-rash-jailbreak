#pragma once
// The runtime rider records as the race loader fills them (RASHCDI.BIN SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, resident at 0x8005B5E8 while the race loads; our own
// disassembly). Three functions of BuildGrid RASHCDI 0x80067B08, each accepted by
// its bench row (tools\rrverify\rows_riders.inc) at 0 mismatches:
//
//   RiderRecordInit  RASHCDI 0x80064C0C  SpawnBike's (0x80065DEC) record fill: defaults, then the
//                                        LEVEL<n>.BI record; also the bike's / rider's appearance words
//   RiderNameId      RASHCDI 0x80066E1C  the name string id SpawnBike stores in riderDef +0x26 (0x80065EB8)
//   GridRiderAdjust  RASHCDI 0x800650A0  BuildGrid's second loop (0x80068240): a player's machine
//                                        +0x24/+0x25, the grudge table's class adjustments, own byte 0x80
//
// The 72-byte record lives at 0x800D5758 + 72 * AI index (SpawnBike 0x80065DD4..0x80065DF0) and the
// bike's +0x43C points at it. Written over a GUEST-ADDRESS VIEW (road_query.h GuestRam): every pointer
// is chased as the original chases it, and the SLUS leaves it calls (memset 0x8001E100, memcpy by words
// 0x8001E0B4, the byte copy 0x8001E08C, Rand 0x8001FC58) run natively here with the original's own
// semantics (word stores, alignment faults).
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kRiderRecords = 0x800D5758;     // 20 records of 72 bytes, by AI index
constexpr uint32_t kRiderRecordBytes = 72;
constexpr uint32_t kRrHandleToAi = 0x800D38B0;     // u8[18]
constexpr uint32_t kRrAiToHandle = 0x800D38C8;     // u8[20], 31 = empty
constexpr uint32_t kRrGameStatePtr = 0x8005B2F8;
constexpr uint32_t kRrP1Bike = 0x8005B38C;         // player 1's bike (SpawnBike flags & 1)
constexpr uint32_t kRrPool0Ptr = 0x8005B3A0;       // pool-0 slot 0, stride 1096
constexpr uint32_t kRrPlayerRecords = 0x800D81D8;  // 36 bytes per player (the session's)
constexpr uint32_t kRrSessionFlags = 0x800D80DD;   // the session record's byte +5
constexpr uint32_t kRrClassBlocks = 0x80052EE4;    // three 36-byte per-class AI blocks (GLOBALS.BI)
// RASHCDI's own data (its BSS while the race loads): the grid column's class bit of each handle, written
// by SpawnBike at 0x80065C94 for a racer and read by RiderNameId. On the console RASHCDG is loaded over
// it after the load; a product that keeps RASHCDG resident must restore what it wrote there.
constexpr uint32_t kRrSpawnClassTable = 0x8006B8A0;

// RASHCDI 0x80064C0C (a0 = the LEVEL<n>.BI buffer or 0, a1 = the record index, a2 = the bike).
//   s3 "reset the weapons": records 2..17 unless race type 34 without game_state+5 bit 0; records 18/19;
//      any other record unless race type & 0x20 or 1 or 8, where session byte 0x800D80DD bit 3, else
//      bank (game_state+0x3C) 0 and game_state+5 bit 0, decide.
//   defaults: +0x26 = 0, +0x08 = 1.0, rider+0x13C = 75.0, +0x0C..+0x0F = 128, +0x02 = 255, +0x3D = 112,
//      +0x03 = 4, +0x45 = 123, +0x34..+0x3B = 32, +0x3C = 32, +0x24 = 255, +0x46/+0x47 = 31; with s3 the
//      weapon: +0x2F = 0, +0x2E = 9 (fists), +0x2C = 0x600, +0x30 = 0; +0x10..+0x23 = 0.
//   with a buffer whose record `rec` starts with the byte rec: +0x26 <- f[4], +0x01 <- f[5], +0x02 <- f[6],
//      +0x03 <- f[7], +0x08 <- f[8..11], rider+0x13C <- f[0x0C..], +0x0C..+0x0F <- f[0x10..0x13],
//      +0x10..+0x23 <- f[0x14..0x27], (s3) +0x2C <- f[0x28..0x29], +0x24 <- f[0x2A], +0x45 <- f[0x2B],
//      +0x34..+0x3B <- f[0x2C..0x33], +0x3D <- f[0x35], bike+0x24 bits 12..17 <- f[0x36],
//      rider+0x24 bits 12..17 <- f[0x37], bike+0x4C <- f[0x38..], rider+0x4C <- f[0x3C..].
//   then: +0x04 = 0, +0x28 = 0, +0x27 = 0, +0x00 = 0, +0x25 = +0x24, +0x0E = +0x0F = +0x0D, +0x3C = +0x34;
//      a non-player of class 2 (a police rider): +0x26 = Rand() & 7.
void RiderRecordInit(GuestRam& g, uint32_t bi, int32_t rec, uint32_t e);

// RASHCDI 0x80066E1C: the GAMESTRG id of bike e's rider name, from the record's +0x26 (n, less 8 from 9
// on). A player: race type & 0x20 (career) 110 or 113 + the player record's +10; & 0x10 (split)
// handle + 30 or + 108 (& 8); else 92. Police: 66 + 8 clamp(bank, 0, 2) + (Rand() & 7). Otherwise by
// race type and whether the rider's grid class (0x8006B8A0[handle]) matches a class: 44, 33, 36 / & 4,
// & 1, and the plain race: n + 41 (class 1) or n + 33 (class 0).
uint32_t RiderNameId(GuestRam& g, uint32_t e);

// RASHCDI 0x800650A0, BuildGrid's per-bike pass after the spawns. A player (race type bit 0 clear):
// +0x24 / +0x25 = +0x24 of the record of AI index 8 * class + 2 when that AI index has a bike. Anyone
// else: the grudge bytes toward each player k (+0x10 + k) - same class: 0 (type 44), 15 (a Jailbreak
// race type & 4), else + class block byte 7; other class: + byte 19 toward a police player, 15 (type
// 44), else + byte 6 - clamped to [-15, 15]; then for types without & 4 and & 1 both clear or one set,
// toward AI indices 2..17: -15 (type 44) or + block byte 20 + t / 22 + t (other / same class), clamped.
// Last, the bike's own grudge byte (+0x10 + its AI index) = 0x80.
void GridRiderAdjust(GuestRam& g, uint32_t e);

} // namespace rr::sim
