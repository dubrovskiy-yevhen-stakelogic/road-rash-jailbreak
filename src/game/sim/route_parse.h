#pragma once
// src\game\sim\route_parse - the race's route block parser and the road-map loader of RoadLoad RASHCDI 0x8006AC6C,
// ported from our own disassembly of the player's images:
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
//   ReadLine    RASHCDI 0x80064084, frame 32   one text line (up to the \n / \r / NUL / `max`) and its terminator
//                                              byte copied (strncpy), a NUL after it; v0 = dst, or 0 at a NUL / null
//   ParseInt    RASHCDI 0x80064144, frame 32   *out = atoi(s); v0 = the first non-digit after the number (0 when
//                                              atoi gave 0 and the text holds no '0')
//   NextToken   RASHCDI 0x800643B4, frame 32   blanks (\r \t ' ') skipped, the token up to ' ' / NUL / \r / \t copied
//                                              and terminated; v0 = one past its end, 0 for an empty text
//   TokenInt    RASHCDI 0x80069B10, frame 32   *out = 0; the int after the separator `sep` (none: at the text);
//                                              v0 = one past the separator (the cursor the next call starts at)
//   NumEntries  RASHCDI 0x80069B8C, frame 304  the [NUM_ENTRIES] value of the text
//   FindRace    RASHCDI 0x80069C60, frame 320  the [BEGIN] .. [END] block whose [RACEID] is `race`: *start, *len
//   RouteHeader RASHCDI 0x80069DEC, frame 320  the 40-byte header 0x800D6170 zeroed, the text / length, the entry
//                                              count, the race block, [RACEINTS] and the four density keys
//   LinkFill    RASHCDI 0x8006A014, frame 32   a link {road, dir} completed from the race graph *(gp+472): the
//                                              road's length >> 6 << 16, its two nodes (when they are nodes)
//   RouteParse  RASHCDI 0x8006A0C8, frame 352  the route block: the allocation (START, FINISH, the records, the
//                                              start record), [START] / [FINISH] / the checkers / [RMAGIC], the
//                                              route records after [RACEINTS]
//   MapBlocks   RASHCDI 0x8006A9F4, frame 48   ROAD<n>.MAP's block chain: MAP_ is the graph G, BTT_ / BST_ / BIT_ /
//                                              IPT_ / PDT_ / GPDT its tables (+0x1C.. pointers, +0x28.. counts)
//   MapLoad     RASHCDI 0x8006ABC8, frame 168  "%s%d.MAP" (name, players) LoadFile'd to 0x8005B338, *(0x8005B240) = G
//
// The kernel's string calls go through the SLUS stubs (`li t2,0xA0; jr t2; li t1,n`): strncmp A(18h) 0x80044874,
// strlen A(1Bh) 0x800448F4, strncpy A(1Ah) 0x80044914, strchr A(1Eh) 0x80044924, atoi A(10h) 0x80044974. The bench
// runs the kernel's own code for them (allowBiosCalls); the product the host's Bios* below, each benched against the
// kernel on its own row (rows_route.inc).
//
// MEMORY MODEL and SEAMS as loader.h: `sp` is the stack pointer at the function's entry; every callee with a frame
// goes through `LoaderCallees::Call`; SLUS 0x800245DC / 0x800245F4 (the race graph's road / node records) are
// leaves and run inline.
#include <cstdint>

#include "game/sim/loader.h"

namespace rr::sim {

constexpr uint32_t kRpReadLineFn = 0x80064084, kRpReadLineFrame = 32;
constexpr uint32_t kRpParseIntFn = 0x80064144, kRpParseIntFrame = 32;
constexpr uint32_t kRpNextTokenFn = 0x800643B4, kRpNextTokenFrame = 32;
constexpr uint32_t kRpTokenIntFn = 0x80069B10, kRpTokenIntFrame = 32;
constexpr uint32_t kRpNumEntriesFn = 0x80069B8C, kRpNumEntriesFrame = 304;
constexpr uint32_t kRpFindRaceFn = 0x80069C60, kRpFindRaceFrame = 320;
constexpr uint32_t kRpHeaderFn = 0x80069DEC, kRpHeaderFrame = 320;
constexpr uint32_t kRpLinkFillFn = 0x8006A014, kRpLinkFillFrame = 32;
constexpr uint32_t kRpParseFn = 0x8006A0C8, kRpParseFrame = 352;
constexpr uint32_t kRpMapBlocksFn = 0x8006A9F4, kRpMapBlocksFrame = 48;
constexpr uint32_t kRpMapLoadFn = 0x8006ABC8, kRpMapLoadFrame = 168;

// the kernel's string calls (SLUS stubs into the A0 table)
constexpr uint32_t kRpStrNCmp = 0x80044874, kRpStrLen = 0x800448F4, kRpStrNCpy = 0x80044914, kRpStrChr = 0x80044924,
                   kRpAtoi = 0x80044974;

// the globals
constexpr uint32_t kRpHeader = 0x800D6170;      // 40 bytes: +0 alloc, +4 [RMAGIC], +8 entries, +0xA..+0x10 densities,
                                                // +0x12 [RACEINTS], +0x14 START, +0x18 FINISH, +0x1C text, +0x20 its
                                                // length, +0x24 the records
constexpr uint32_t kRpMapFile = 0x8005B338;     // ROAD<n>.MAP's buffer (LoadFile)
constexpr uint32_t kRpMapGraph = 0x8005B240;    // G = the MAP_ block's payload
constexpr uint32_t kRpGraphGp = 472;            // gp+472: the race graph STREAM<n>.GRF

bool ReadLine(GuestRam& g, uint32_t dst, uint32_t max, uint32_t src, uint32_t sp, LoaderCallees& c, uint32_t& v0);
bool ParseInt(GuestRam& g, uint32_t s, uint32_t out, uint32_t sp, LoaderCallees& c, uint32_t& v0);
bool NextToken(GuestRam& g, uint32_t src, uint32_t dst, uint32_t sp, LoaderCallees& c, uint32_t& v0);
bool TokenInt(GuestRam& g, uint32_t prev, uint32_t line, uint32_t sep, uint32_t out, uint32_t sp, LoaderCallees& c,
              uint32_t& v0);
bool NumEntries(GuestRam& g, uint32_t text, uint32_t len, uint32_t sp, LoaderCallees& c, uint32_t& v0);
// the fifth argument (&len) is the caller's sp+16, as o32 passes it
bool FindRace(GuestRam& g, uint32_t text, uint32_t len, uint32_t race, uint32_t pStart, uint32_t sp, LoaderCallees& c,
              uint32_t& v0);
bool RouteHeader(GuestRam& g, uint32_t text, uint32_t len, uint32_t sp, LoaderCallees& c);
bool LinkFill(GuestRam& g, uint32_t link, uint32_t sp, LoaderCallees& c);
bool RouteParse(GuestRam& g, uint32_t text, uint32_t len, uint32_t sp, LoaderCallees& c);
bool MapBlocks(GuestRam& g, uint32_t base, uint32_t len, uint32_t sp, LoaderCallees& c, uint32_t& v0);
bool MapLoad(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c, uint32_t& v0);

// The kernel's A-table string functions, the host's (SCPH1001's code, read out of our disassembly of the player's
// BIOS: strncmp 0xBFC03310, strlen 0xBFC03494, strncpy 0xBFC03418, strchr 0xBFC0357C, atoi 0xBFC02950). Each
// walks at most 1 MiB. BiosAtoi returns false for a byte >= 0x80 where it looks one up in the kernel's ctype
// table (the table's upper half is not ctype data - the host does not serve it: a NAMED BOUND).
uint32_t BiosStrNCmp(GuestRam& g, uint32_t a, uint32_t b, uint32_t n);
uint32_t BiosStrLen(GuestRam& g, uint32_t a);
uint32_t BiosStrNCpy(GuestRam& g, uint32_t dst, uint32_t src, uint32_t n);
uint32_t BiosStrChr(GuestRam& g, uint32_t a, uint32_t ch);
bool BiosAtoi(GuestRam& g, uint32_t a, uint32_t& v0);

// The product's callees for the functions above and RoadLoad's children (loader2.h RoadText / RoadClear /
// RoadRecords): each ported one runs natively (its own children through this object again), the kernel's string
// calls are the host's Bios*, everything else goes to `next`.
class RouteCallees final : public LoaderCallees {
public:
    RouteCallees(GuestRam& g, LoaderCallees& next) : g_(g), next_(next) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override;
    size_t ported = 0, bios = 0, forwarded = 0;
    const char* error = nullptr; // the host's refusal (a kernel table it does not serve)

private:
    GuestRam& g_;
    LoaderCallees& next_;
};

} // namespace rr::sim
