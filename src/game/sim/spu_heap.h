#pragma once
// src\game\sim\spu_heap - libspu's SPU-RAM allocator and the music stream player's two ring buffers, ported from
// our own disassembly of the player's executable:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
//   SpuInitMalloc  0x8004F368, a leaf       table[0] = 0x40001010, table[1] = (0x10000 << shift) - 0x1010,
//                                           last = 0, max = n, table pointer = table (n < 1: nothing, 0)
//   SpuMalloc      0x8004F3C8, frame 40     first fit over the table (the reverb work area at the top reserved
//                                           while reverb is on), the block split, the gc; the block's address
//                                           or -1
//   SpuMallocGc    0x8004F698, a leaf       merges adjacent free blocks, drops empty ones (0x2FFFFFFF), sorts
//                                           by address, fills the first hole with the last entry, folds free
//                                           tail blocks into the terminal one
//   SpuFree        0x8004F998, frame 24     marks the block whose word is `addr` free, then the gc
//   MusicSpuAlloc  0x800210C4, frame 40     one SpuMalloc(*(0x800D7580) * *(0x800D7584)) per stream record
//                                           (0x800D7548, stride 20, count *(0x800D754C)) into its +0x20; on a
//                                           failure the debug print 0x80044894("Failed to Alloc SPU Bufs")
//                                           and the negative address
//   MusicSpuFree   0x80021174, frame 40     SpuFree of every record's +0x20, then 0
//
// The table: 8-byte entries {word, size}; word bit 31 = free, bit 30 = the terminal entry (the rest of SPU RAM),
// 0x2FFFFFFF = an entry the gc drops; the address is the low 28 bits. `max` entries plus the terminal one's
// successor (index max) can be written, so the caller's table holds max + 1 entries.
//
// MEMORY MODEL: road_query.h's GuestRam, every global and the table at the original's addresses; every load and
// store in the original's order (the gc's loops cache the counts exactly where the original caches them).
// Accepted by `rrverify phys` rows spu_init_malloc / spu_malloc / spu_malloc_gc / spu_free / music_spu_alloc /
// music_spu_free (tools\rrverify\rows_loader2_spu.inc).
#include <cstdint>

#include "game/sim/loader.h"
#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kSpuInitMallocFn = 0x8004F368, kSpuMallocFn = 0x8004F3C8, kSpuMallocGcFn = 0x8004F698,
                   kSpuFreeFn = 0x8004F998, kMusicSpuAllocFn = 0x800210C4, kMusicSpuFreeFn = 0x80021174;
// libspu's globals (the functions' own lui/lw pairs)
constexpr uint32_t kSpuReverbOn    = 0x8005A3B4; // reverb enabled: the work area is reserved
constexpr uint32_t kSpuReverbStart = 0x8005A3B8; // the reverb work area's start, in 8-byte units
constexpr uint32_t kSpuAddrShift   = 0x8005A444; // the SPU address unit's shift (3)
constexpr uint32_t kSpuAddrMask    = 0x8005A44C; // its mask (7)
constexpr uint32_t kSpuMallocMax   = 0x8005A47C; // SpuInitMalloc's n
constexpr uint32_t kSpuMallocLast  = 0x8005A480; // the terminal entry's index
constexpr uint32_t kSpuMallocTab   = 0x8005A484; // -> the table
// The boot's table (SLUS 0x800163D4 -> 0x8001EBF0: SpuInitMalloc(12, 0x800D6A38)).
constexpr uint32_t kSpuBootTable = 0x800D6A38;
constexpr int32_t kSpuBootBlocks = 12;
// The music stream player's records (0x80020BEC sets the count and the sizes).
constexpr uint32_t kMusicStreams     = 0x800D7548; // +0x04 count; record k at +20k, its SPU buffer at +0x20
constexpr uint32_t kMusicStreamCount = 0x800D754C;
constexpr uint32_t kMusicChunkBytes  = 0x800D7580;
constexpr uint32_t kMusicChunks      = 0x800D7584;
constexpr uint32_t kMusicPrintFn     = 0x80044894; // the debug print
constexpr uint32_t kMusicAllocFailFmt = 0x80010B10; // its format ("Failed to Alloc SPU Bufs")

int32_t SpuInitMalloc(GuestRam& g, int32_t n, uint32_t table);
uint32_t SpuMalloc(GuestRam& g, uint32_t size);
void SpuMallocGc(GuestRam& g);
void SpuFree(GuestRam& g, uint32_t addr);
// `sp` is the stack pointer at 0x800210C4's entry; the print is made through `c` at sp - 40. False when the
// callee refused.
bool MusicSpuAlloc(GuestRam& g, uint32_t sp, LoaderCallees& c, uint32_t& v0);
uint32_t MusicSpuFree(GuestRam& g);

} // namespace rr::sim
