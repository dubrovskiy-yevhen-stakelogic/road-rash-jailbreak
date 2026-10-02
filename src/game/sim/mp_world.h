#pragma once
// Two players: the cell-state copy and the class-50 release, ported from our
// own disassembly of the player's images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// Each function is accepted by its own row of `rrverify phys` (tools\rrverify\rows_mp2.inc).
#include <cstdint>

#include "game/sim/recover.h"

namespace rr::sim::mp {

constexpr uint32_t kCellCopyFn     = 0x8001339C; // SLUS
constexpr uint32_t kClass50FreeFn  = 0x800A3ECC; // RASHCDG

// SLUS 0x8001339C(id, p), the tail of the slot fill's region-0 relocation 0x800135E8 (a1 passes through): only in
// two-player mode (game_state+4 bit 4). When player p (CellSlotFor 0x80013204) and the other player
// (OtherSlot 0x80013360) both hold cell `id`, each record's live halfword +4 of the five region-0 arrays
// (+0x28 stride 76 count +2, +0x24 / 68 / +0, +0x30 / 88 / +6, +0x2C / 64 / +4, +0x34 / 64 / +8) is copied from
// p's copy of the cell (the one just loaded) to the other player's copy. A kind-6 record (+0x30) whose word is > 0 (a volume handle) has
// the other player's bit of that volume set by VolumeMask 0x80013294(6, h, p ^ 1); a kind-4 record (+0x2C) whose
// class +2 in the OTHER copy is 50 re-stamps player p's class-50 record by 0x80012BA8(the other copy's region 0
// header (sic: the header, not the record), id, p).
void CellStateCopy(GuestRam& g, uint32_t id, uint32_t p);

// RASHCDG 0x800A3ECC(id, p), 18 instructions: player p's class-50 record 0x800D43C0 + 28 p is released (+0x16 and
// +0x18 zeroed) when it is live (+0x16 != 0) and belongs to cell `id` (+0x18). CellRelease 0x8008C45C calls it in
// two-player mode before it asks whether the other player still holds the cell.
void Class50Release(GuestRam& g, uint32_t id, uint32_t p);

} // namespace rr::sim::mp
