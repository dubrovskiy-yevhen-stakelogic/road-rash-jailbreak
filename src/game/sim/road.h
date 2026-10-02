#pragma once
// Road-side queries of the per-frame bike step, ported from the resident executable
// `SLUS_010.53`, SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text base 0x80010000.
#include <cstdint>

namespace rr::sim {

// ---------------------------------------------------------------------------- 0x80039C90
// The road object's piece lookup: given a road object and a piece key, find the entry of the
// object's 32-byte piece table that owns that key.
//
//   if (obj == NULL)            return -1;
//   if (obj->s16[0x10] != 1)    return -1;         // only one object kind carries a piece table
//   n       = obj->s16[0x12];
//   entries = obj->ptr[0x2C];
//   for (i = 0; i < n; ++i)
//       if (((entries[i].s32[0x0C] - key) | entries[i].s16[0x02]) == 0) return i;
//   return -1;
//
// The `subtract, OR with a second field, test against zero` idiom is the original's: the entry
// matches only when its key equals `key` AND its halfword at +0x02 is zero, and the two tests are
// fused into one branch. Reproduced literally, including the fact that the subtraction wraps.
//
// The caller resolves `entries` and turns the index back into an address; everything that decides
// WHICH entry is here.
int32_t FindRoadPieceIndex(int16_t objKind, int16_t count, const uint8_t* entries, int32_t key);

constexpr uint32_t kRoadPieceEntrySize = 32;

} // namespace rr::sim
