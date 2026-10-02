#pragma once
// The road query - "which slice am I on" and the AI's road look-ahead - ported from the resident
// executable `SLUS_010.53`, SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text base 0x80010000.
//
// This file carries the addresses line by line. Accepted by `rrverify phys` (tools\rrverify\verify_physics.cpp): 0
// mismatches over the whole guest RAM outside the stack window, on dump-derived and randomised
// inputs, one row per function.
//
// ============================================================================ THE MEMORY MODEL
//
// Everything in this file works on a GUEST-ADDRESS VIEW of the 2 MiB main RAM (`GuestRam`), not on
// resolved C++ structs. That is a deliberate break with the rest of `src\game\sim`, where the caller
// resolves every pointer and hands the port byte views, and the reasons are
// measured, not stylistic:
//
//   * these functions chase a dozen guest pointers per call - the cursor holds six, the network
//     header seven, a resident road object three, and each candidate the neighbour functions build
//     is four more - and WHICH pointer is chased next depends on what the last one held. No caller
//     can resolve that in advance without running the function itself;
//   * three behaviours the original has are properties of guest ADDRESSES, not of values: the
//     partial candidate of 0x80037A30/0x80037FBC leaves 24 bytes of a candidate slot as whatever the
//     caller's frame held, and PickNeighbour then reads that slot's +0x1C; the
//     pool-3 "no admissible road" pick copies the 32 bytes BEFORE the candidate array; and the
//     look-ahead walks the caller's own cursor in place and does not restore it on one exit.
//     A struct port can only imitate these; an address port reproduces them;
//   * the layout these functions see IS the file layout of `ROAD<n>.MAP` and the type-3 road chunks
//     plus the loader's pointer fix-ups, so a product can host them
//     verbatim by loading those files into a flat arena at stable addresses - which is what an
//     address view needs and nothing more.
//
// Stack frames. The two walkers keep their private cursor copies, the candidate array and the
// candidate directions IN THEIR FRAME, at the original's offsets below the `sp` the caller passes,
// because those are exactly the bytes the quirks above read. Every other local lives in C++. The
// bench runs these inside its excluded stack window, so a frame write is never compared - what is
// compared is everything the frame bytes lead to.
//
// Faults. A load or store the console would not survive - outside the 8 MiB RAM mirror, or
// misaligned - is not performed; `GuestRam` records the first such address and the port carries on
// with 0 for the value, so the caller can FAIL the call. It never guesses a value.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "game/sim/ai.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the view
// The guest address space as `src\interp` maps it, restricted to main RAM: KUSEG/KSEG0/KSEG1 are
// reduced to a physical address, the first 8 MiB mirror the 2 MiB of RAM, and anything else
// (scratchpad, I/O, BIOS, KSEG2) is a fault here. `gp` is the one register these functions read
// besides their arguments: `Rand` keeps its seed at gp+2076 and the slice search raises a flag at
// gp+2292.
class GuestRam {
public:
    static constexpr uint32_t kRamSize = 2u * 1024u * 1024u;

    GuestRam(uint8_t* ram, uint32_t gp) : ram_(ram), gp_(gp) {}

    uint32_t U32(uint32_t a);
    int32_t S32(uint32_t a) { return static_cast<int32_t>(U32(a)); }
    uint16_t U16(uint32_t a);
    int16_t S16(uint32_t a) { return static_cast<int16_t>(U16(a)); }
    uint8_t U8(uint32_t a);
    int8_t S8(uint32_t a) { return static_cast<int8_t>(U8(a)); }
    void W32(uint32_t a, uint32_t v);
    void W16(uint32_t a, uint16_t v);
    void W8(uint32_t a, uint8_t v);
    // Host-side block transfer, for a caller that keeps an entity as a host copy.
    void ReadBlock(uint32_t a, uint8_t* dst, uint32_t n);
    void WriteBlock(uint32_t a, const uint8_t* src, uint32_t n);

    uint32_t gp() const { return gp_; }
    // The 1 KiB scratchpad at physical 0x1F800000, mapped as src\interp maps it. Detached (null) by
    // default, and then an access there is a fault as before; the pass-H list walks of
    // road_runtime.h need it (their list lives there).
    void SetScratchpad(uint8_t* spad) { spad_ = spad; }
    bool Faulted() const { return faulted_; }
    uint32_t FaultAddress() const { return faultAddress_; }
    void ClearFault() { faulted_ = false; faultAddress_ = 0; }

private:
    uint8_t* At(uint32_t a, uint32_t n);
    uint8_t* ram_;
    uint32_t gp_;
    uint8_t* spad_ = nullptr;
    bool faulted_ = false;
    uint32_t faultAddress_ = 0;
};

// ---------------------------------------------------------------------------- globals
// Every one read out of the functions' own `lui`/`lw` pairs.
constexpr uint32_t kRoadGraphPtr   = 0x8005B240; // -> ROAD<n>.MAP + 8, with the loader's pointers
constexpr uint32_t kRoadPool0Ptr   = 0x8005B3A0; // -> pool-0 slot 0, stride 1096
constexpr uint32_t kRoadGameState  = 0x8005B2F8; // -> game_state (+0x04 flags, +0x30 players)
constexpr uint32_t kRoadBikeCount  = 0x8005B1F8;
constexpr uint32_t kRoadTraffic    = 0x800CF660; // pool 3, 512-byte vehicle records
constexpr uint32_t kRouteTable     = 0x800D6170; // +0x12 s16 leg count (-1 = no route), +0x24 legs
constexpr uint32_t kRouteLegBytes  = 120;
constexpr uint32_t kRandSeedGp     = 2076;       // gp+2076, the LCG seed (0x8001FC5C)
constexpr uint32_t kOffRoadFlagGp  = 2292;       // gp+2292 = 0x8005B580 (0x80036E80)
constexpr uint32_t kRoadSliceBytes = 52;         // SLCT
constexpr uint32_t kRoadCursorSize = 32;         // the cursor

// The two neighbour functions, by the address the walkers put in their function pointer
// (`lui 0x8003; addiu 31280 / 32700` at 0x80036CB4, 0x80036E94, 0x80038850, ...).
constexpr uint32_t kNeighboursForwardFn  = 0x80037A30;
constexpr uint32_t kNeighboursBackwardFn = 0x80037FBC;

// ---------------------------------------------------------------------------- the leaves
// SLUS 0x8001E0B4: memcpy by words. `n` counts bytes and is decremented by 4, so it must be a
// multiple of 4 (every caller here passes 32). Returns `dst`.
uint32_t GuestCopyWords(GuestRam& m, uint32_t dst, uint32_t src, uint32_t n);
// SLUS 0x8001FC58 on the guest seed at gp+2076.
uint32_t GuestRand(GuestRam& m);

// 0x80039A08, 152 bytes: the BTT_ record of object `id`, or 0.
uint32_t RoadBttRecord(GuestRam& m, int32_t id);
// 0x80039AA0, 92 bytes: the BST_ record of road piece `id`, or 0. The index is checked only
// against the upper bound, so a junction id reads BEFORE the table and only the id test rejects it.
uint32_t RoadBstRecord(GuestRam& m, int32_t id);
// 0x80039AFC, 100 bytes: the BIT_ record (104 bytes) of junction object `id`, or 0.
uint32_t RoadNodeRecord(GuestRam& m, int32_t id);
// 0x80039B60, 80 bytes: the IPT_ record whose +0 is `id`, by linear search, or 0.
uint32_t RoadJunctionIndex(GuestRam& m, int32_t id);
// 0x80039BB0, 136 bytes: stores every PDT_ record of junction `ipt` whose +0x08 is `road` to
// `out` - up to max + 1 of them - and returns how many, or -1 on a (max + 2)-th match.
int32_t RoadTurnsFrom(GuestRam& m, uint32_t ipt, int32_t road, uint32_t out, int32_t max);
// 0x80039C38, 88 bytes: the GPDT record a PDT_ record names, or 0.
uint32_t RoadTurnTarget(GuestRam& m, uint32_t pdt);
// 0x8003A37C, 80 bytes: the first 24-byte arm of a BIT_ record whose +4 is `road`, or 0.
uint32_t RoadNodeArm(GuestRam& m, uint32_t node, int32_t road);
// 0x80039C90, 96 bytes: the GRPT record of a junction object with +2 == 0 and +0x0C == road, or 0.
// The same function as `FindRoadPieceIndex` (road.h, row `road_find_piece`), here over addresses.
uint32_t RoadFindPiece(GuestRam& m, uint32_t obj, int32_t road);
// 0x8003C840 / 0x8003C948, 264 bytes each: the resident object after / before `obj` (through
// BST_ for a road piece, through the arm for `road` or the partner for a junction), or 0 when it
// is not streamed in. The original's first argument is never read and is not taken here.
uint32_t RoadNextObjectFwd(GuestRam& m, uint32_t obj, int32_t road);
uint32_t RoadNextObjectBwd(GuestRam& m, uint32_t obj, int32_t road);
// 0x8003F3B4, 84 bytes: the route leg whose +0 is `road`, or 0.
uint32_t RouteLegOfRoad(GuestRam& m, int32_t road);
// 0x8003F408, 208 bytes: `leg` itself when its +0 is `road`; else the leg whose +0 is `road`,
// provided `road` is in `leg`'s list at +0x64 (count +0x10); else 0. 0 with no route loaded.
uint32_t RouteLegFor(GuestRam& m, uint32_t leg, int32_t road);
// 0x8003F580, 80 bytes: 1 when `road` is in `leg`'s list at +0x54 (count +0x10).
int32_t RouteLegHasRoad(GuestRam& m, uint32_t leg, int32_t road);
// 0x8003F5D0, 176 bytes: the first leg holding `road` in that list - over COUNT + 1 legs.
uint32_t RouteLegContaining(GuestRam& m, int32_t road);

// 0x800394F0, 440 bytes: 1 when the walk from `cursor` in direction `dir` cannot continue because
// the next object is not resident.
int32_t RoadNextObjectMissing(GuestRam& m, uint32_t cursor, int32_t dir);

// ---------------------------------------------------------------------------- the walkers
// 0x8003697C, 408 bytes: the along-distance of `point` from the start of `slice` - or from its END
// when the walk has just been flipped from forward to backward.
int32_t RoadAlongFromAnchor(GuestRam& m, int32_t oldDir, int32_t newDir, uint32_t slice,
                            uint32_t point);

// 0x80037A30 (1420 bytes) and 0x80037FBC (1428 bytes): the candidate cursors past a sub-object
// end, walking forward / backward. `max` is the original's fifth argument (sp+16). `sp` is the
// caller's stack pointer: the turn list TurnsFrom fills lives in this function's own frame. Two
// functions, not one with a flag - they differ in nine ways.
int32_t RoadNeighboursForward(GuestRam& m, uint32_t p, uint32_t in, uint32_t out, uint32_t dirs,
                              int32_t max, uint32_t sp);
int32_t RoadNeighboursBackward(GuestRam& m, uint32_t p, uint32_t in, uint32_t out, uint32_t dirs,
                               int32_t max, uint32_t sp);

// 0x8003CCA0, 548 bytes: the AI's choice between junction candidates (route following).
int32_t AiJunctionChoice(GuestRam& m, uint32_t e, uint32_t cand, int32_t n);

// 0x80039048, 1192 bytes: chooses one of `n` candidates, writes it to `out` and its direction to
// `*dirOut`, and returns `out`'s slice.
uint32_t RoadPickNeighbour(GuestRam& m, uint32_t p, uint32_t cand, uint32_t dirs, int32_t n,
                           uint32_t out, uint32_t dirOut);

// 0x80036B14, 1288 bytes: the road-slice search. THREE arguments (a3 is never read). Walks
// `cursor` slice by slice toward `point`, committing into it, and returns the slice it settled on.
uint32_t RoadSliceSearch(GuestRam& m, uint32_t p, uint32_t cursor, uint32_t point, uint32_t sp);

// 0x800386DC, 2412 bytes: the AI's road look-ahead. Seven o32 arguments, the last three on the
// stack in the original. `outCursor` may be 0.
void RoadLookAhead(GuestRam& m, uint32_t e, int32_t ahead, int32_t along, int32_t dir,
                   uint32_t cursor, uint32_t outPoint, uint32_t outCursor, uint32_t sp);

// ---------------------------------------------------------------------------- SLUS 0x8002E080
// The run-time reciprocal-square-root table `Normalize` / `Normalize32` read through
// `*(gp+2260)` = `*(0x8005B560)`. 41 instructions, called once, from
// 0x80012254 at start-up:
//
//   table = malloc(2048, 0);  *(gp+2260) = table;
//   for (i = 0; i < 1024; i++) {
//       d  = (i >> 1) + ((u32)(i - 2) >> 31);          // 1, 1, 1, 1, 2, 2, 3, 3, ...
//       r  = SqrtGte(0x80000000u / d) << 2;            // divu, then the ported square root
//       sh = 21 - (r ? LZCR(r) : 0);                   // GTE LZCS/LZCR
//       if (sh > 0) r >>= sh;                          // srlv
//       table[i] = (u16)((r << 5) | sh);               // sh may be <= 0: OR'd in, sign bits too
//   }
//
// This is the loop; the allocation is the caller's (the bench runs the game's own allocator on the
// seam, the product owns the buffer). `sqrtTable` is SqrtGte's table INSIDE a window (ai.h): for
// i = 0 and 1 the argument is 0x80000000, negative as an s32, and SqrtGte then reads below it.
void FillRsqrtTable(uint16_t out[1024], const int16_t* sqrtTable);
constexpr uint32_t kRsqrtTableGp = 2260; // gp+2260 = 0x8005B560

// ---------------------------------------------------------------------------- the arena
// A guest image the road query can run on outside the console: `ROAD<n>.MAP` and type-3 road chunks
// placed at stable guest addresses, with exactly the pointer fix-ups the game's loader makes. The
// rules were read off the `rr-race` snapshot (ROAD1.MAP resident at 0x801A8A4C, objects 67, 14
// and 15 at 0x8014541C, 0x8010D41C, 0x8015D41C) by diffing it against the disc, and `rrgame
// --arenacheck` rebuilds those four images from the disc and compares them with the snapshot byte
// for byte:
//   * the map: G = file + 8 and *(0x8005B240) = G; the header words G+0x1C/+0x20/+0x24 and
//     G+0x30/+0x34/+0x38 point 8 bytes into the BTT_, BST_, BIT_, IPT_, PDT_ and GPDT blocks, and
//     the halfwords G+0x28/+0x2A/+0x2C/+0x3C/+0x3E/+0x40 hold their record counts (block size / 32,
//     8, 104, 12, 12, 12). Nothing else in the file changes;
//   * a road object: object = chunk + 0x20; the twelve words chunk+0x4C..+0x68 and +0x7C..+0x88
//     point 8 bytes into the blocks GRPT, SUBT, SLCT, XSIH, XSDH, XSAI, DIST, SEG_ and BGDT, BSDT,
//     BZDT, NMBD (a block the object does not have leaves its word 0); and BTT_ +0x0C of the object's
//     record points at the object - that word is the streaming state the road query tests.
class RoadArena {
public:
    RoadArena() : ram_(GuestRam::kRamSize, 0) {}
    uint8_t* Ram() { return ram_.data(); }
    const uint8_t* Ram() const { return ram_.data(); }
    // Returns G, or 0 with `error` set.
    uint32_t LoadRoadMap(const uint8_t* file, size_t size, uint32_t at, const char** error);
    // Returns the object address (chunk + 0x20), or 0 with `error` set. The map must be loaded first.
    uint32_t LoadRoadObject(const uint8_t* chunk, size_t size, uint32_t at, const char** error);
    uint32_t Graph() const { return graph_; }

private:
    std::vector<uint8_t> ram_;
    uint32_t graph_ = 0;
};

// ---------------------------------------------------------------------------- AiDrive's view
// The look-ahead as `AiDrive RASHCDG 0x800954A0` calls it (0x80095574), for the ported `AiDrive`
// (ai.h). `AiDrive` keeps its 32-byte output cursor at ITS OWN sp+32 (0x80095564) and then reads
// the slice out of that cursor's +0x0C (0x8009557C) - so this adapter needs the stack pointer the
// original `AiDrive` has, and it does the dereference itself: the slice pointer is the PORT's.
//
// `hostEntity`, when not null, is the caller's working copy of the 1096-byte entity: it is written
// to guest RAM before the call and read back after it. Pass null when the entity lives in the view.
class GuestAiRoadQuery final : public AiRoadQuery {
public:
    static constexpr uint32_t kAiDriveFrame = 88;     // `addiu sp,sp,-88` at 0x800954A0
    static constexpr uint32_t kAiDriveCursor = 32;    // `addiu v0,sp,32` at 0x80095564
    static constexpr uint32_t kEntityBytes = 1096;

    GuestAiRoadQuery(GuestRam& m, uint32_t entity, uint8_t* hostEntity, uint32_t aiDriveSp)
        : m_(m), entity_(entity), host_(hostEntity), sp_(aiDriveSp) {}
    bool LookAhead(int32_t ahead, int32_t along, int32_t dir, uint32_t cursorOffset,
                   uint32_t aimOffset, int16_t sliceAxis[3], int32_t sliceOrigin[3]) override;

private:
    GuestRam& m_;
    uint32_t entity_;
    uint8_t* host_;
    uint32_t sp_;
};

} // namespace rr::sim
