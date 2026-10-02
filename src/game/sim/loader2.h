#pragma once
// src\game\sim\loader2 - the race loader's set-up path, second part: BuildRace and
// the rest of SetUpRace's children, ported from our own disassembly of the player's images:
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
//   BuildRace       RASHCDI 0x8006982C, frame 344  the Jailbreak head, the pools, the grid block off DATA\STARTDF<A|B>.BIN,
//                                                  SRand(GetRCnt(2)), BuildGrid, the camera, the time-trial limits,
//                                                  the population block, the speed classes, STARTJBA, the race globals
//   WorldPoolsInit  RASHCDI 0x80069000, frame 48   pool 6 (24 / 32 x 280), the players' volume lists, the pedestrian block
//   PoolTableInit   RASHCDI 0x80068AA4, frame 48   once (0x8005AD32): the 20 rider records cleared (the kept 8 bytes),
//                                                  PopulationReset, the pool table 0x800CE4D0 (7 x 16), 0x800D43C0 cleared
//   RaceFlag        RASHCDI 0x8006968C, a leaf     *(0x8005AD34) = 1 when 0
//   TrialLimits     RASHCDI 0x800696B8, frame 56   race types with bit 0: each bike's +0x39C / +0x1E0 / +0x240 from the
//                                                  bank's limit tables 0x8005303C / 0x80053048 / 0x80053054, Scale 0x8002EE50
//   PopBlockInit    RASHCDI 0x800689D0, frame 32   once (0x8005AD31): PedTablesInit (switch on), PartSlotsInit, CensusInit,
//                                                  RaceBlockCopy
//   PedTablesInit   RASHCDI 0x80068500, frame 32   the pedestrian clip ids 0x800D5F40 and states 0x800D5730
//   PartSlotsInit   RASHCDI 0x80069394, frame 40   the part arrays of pool 2 (4 x 408) and pool 3 (16 x 24), counted in
//                                                  their blocks' +12; the store records 0x800D1660 (18 x 24), 8344 free
//   CensusInit      RASHCDI 0x80068684, frame 32   0x800D86F0 / 0x800D86F4 = CensusCount 0x80068614(2, &out), a leaf
//   SpeedClassCopy  RASHCDI 0x800645E4, frame 24   the grid block's last 12 bytes -> 0x80052FA0 (the AI speed classes)
//   EscapeLoad      RASHCDI 0x80068740, frame 760  race type 44: DATA\STARTJBA.BIN's two blocks into 0x8005B340 / 0x8005B208
//   AnimNoise       RASHCDI 0x80063158, frame 40   DATA\ANIMNOIZ.DAT once: gp+1948 the file, gp+1916 its records, gp+1880
//                                                  the event table, the offsets made absolute
//   FightLoad       RASHCDI 0x800653E8, frame 64   "<name>.bin" off the CD, its 40 records' +4 / +8 made absolute
//   RoadLoad        RASHCDI 0x8006AC6C, frame 24   *(0x8005AD54) = 0, RoadText, RoadClear, RoadRecords
//   RoadText        RASHCDI 0x8006A7F8, frame 168  "<name>grf<players>.txt" read into the stream buffers, parsed (0x8006A0C8),
//                                                  the finish table (0x8006A7C0)
//   RoadClear       RASHCDI 0x8006A98C, frame 24   0x8005B338 / 0x8005B33C = 0, PieceListInit 0x8006A8FC
//   RoadRecords     RASHCDI 0x8006AC4C, frame 24   0x8006ABC8(a0 passed through), v0 = 1
//   GrfLoad         SLUS    0x800244E0, frame 32   STREAM<1|2>.GRF into *(gp+472) (malloc'd once), its offsets fixed
//                                                  (0x80024610), gp+468 = its +8
//   StreamSetUp     SLUS    0x80022F78, frame 24   the album (one player), the stream files, 0x80022A1C(2)
//   StreamStart     SLUS    0x80023020, frame 24   StreamPlace 0x800235C8 at the start record, 0x80023714
//   SirenSet        SLUS    0x80018DC8, a leaf     gp+1908 = the siren's sound index by the hazard set (21 22 - 23 24, -1)
//   FxGlobalsInit   SLUS    0x8002B83C, a leaf     0x800D8058 = 80, 0x800D805C = 0x800538D0, the four flare offsets = 2
//   SRand           SLUS    0x8001FC84, a leaf     gp+2076 = seed
//   GrfFix          SLUS    0x80024610, a leaf     +0x14 / +0x18 made absolute, v0 = 1
//
// MEMORY MODEL and SEAMS as loader.h: `sp` is the stack pointer at the function's entry; every callee with a frame of
// its own goes through `LoaderCallees::Call` (the bench: the oracle or a planted stub; the product: loader_product.cpp);
// the leaves ported here (and FinishTableInit / PieceListInit of loader.h, the byte memsets 0x80044934 / 0x8001E0DC)
// run inline, as the original's `jal` to a leaf leaves no trace but its stores.
#include <cstdint>

#include "game/sim/loader.h"

namespace rr::sim {

constexpr uint32_t kL2BuildRaceFn = 0x8006982C, kL2BuildRaceFrame = 344;
constexpr uint32_t kL2WorldPoolsFn = 0x80069000, kL2WorldPoolsFrame = 48;
constexpr uint32_t kL2PoolTableFn = 0x80068AA4, kL2PoolTableFrame = 48;
constexpr uint32_t kL2RaceFlagFn = 0x8006968C;
constexpr uint32_t kL2TrialLimitsFn = 0x800696B8, kL2TrialLimitsFrame = 56;
constexpr uint32_t kL2PopBlockFn = 0x800689D0, kL2PopBlockFrame = 32;
constexpr uint32_t kL2PedTablesFn = 0x80068500, kL2PedTablesFrame = 32;
constexpr uint32_t kL2PartSlotsFn = 0x80069394, kL2PartSlotsFrame = 40;
constexpr uint32_t kL2CensusFn = 0x80068684, kL2CensusFrame = 32;
constexpr uint32_t kL2SpeedClassFn = 0x800645E4, kL2SpeedClassFrame = 24;
constexpr uint32_t kL2EscapeLoadFn = 0x80068740, kL2EscapeLoadFrame = 760;
constexpr uint32_t kL2AnimNoiseFn = 0x80063158, kL2AnimNoiseFrame = 40;
constexpr uint32_t kL2FightLoadFn = 0x800653E8, kL2FightLoadFrame = 64;
constexpr uint32_t kL2RoadLoadFn = 0x8006AC6C, kL2RoadLoadFrame = 24;
constexpr uint32_t kL2RoadTextFn = 0x8006A7F8, kL2RoadTextFrame = 168;
constexpr uint32_t kL2RoadClearFn = 0x8006A98C, kL2RoadClearFrame = 24;
constexpr uint32_t kL2RoadRecordsFn = 0x8006AC4C, kL2RoadRecordsFrame = 24;
constexpr uint32_t kL2GrfLoadFn = 0x800244E0, kL2GrfLoadFrame = 32;     // SLUS
constexpr uint32_t kL2StreamSetUpFn = 0x80022F78, kL2StreamSetUpFrame = 24; // SLUS
constexpr uint32_t kL2StreamStartFn = 0x80023020, kL2StreamStartFrame = 24; // SLUS
constexpr uint32_t kL2SirenSetFn = 0x80018DC8;  // SLUS
constexpr uint32_t kL2FxGlobalsFn = 0x8002B83C; // SLUS
constexpr uint32_t kL2SRandFn = 0x8001FC84;     // SLUS
constexpr uint32_t kL2GrfFixFn = 0x80024610;    // SLUS

// the callees by address (seams)
constexpr uint32_t kL2StrRChr = 0x80044964, kL2StrCat = 0x80044864, kL2StrCpy = 0x800448E4, kL2Printf = 0x80044894;
constexpr uint32_t kL2GetRCnt = 0x80043F00;   // SLUS GetRCnt(0xF2000002): root counter 2
constexpr uint32_t kL2Open = 0x8001458C, kL2Read = 0x80014780, kL2Close = 0x8001460C, kL2Size = 0x800148BC; // the CD files
constexpr uint32_t kL2OpenStream = 0x80023100; // SLUS: open "<gp+0x1A0 prefix><name>"
constexpr uint32_t kL2BuildGrid = 0x80067B00, kL2ResetBikes = 0x80067784, kL2CameraSetUp = 0x80067564;
constexpr uint32_t kL2Scale = 0x8002EE50;      // SLUS Scale(v, &a, &b)
constexpr uint32_t kL2CensusCount = 0x80068614;
constexpr uint32_t kL2RouteParse = 0x8006A0C8, kL2RouteRecords = 0x8006ABC8;

// the globals
constexpr uint32_t kL2PoolTable = 0x800CE4D0;  // 7 x 16: base, stride, &count, &high
constexpr uint32_t kL2RiderRecords = 0x800D5758; // 20 x 72
constexpr uint32_t kL2SpeedClasses = 0x80052FA0; // 3 words
constexpr uint32_t kL2GridName = 0x80052414;   // "DATA\STARTDFA.BIN" (edited in place)
constexpr uint32_t kL2NoiseFile = 0x8005B428, kL2NoiseRecords = 0x8005B408, kL2NoiseEvents = 0x8005B3E4;
constexpr uint32_t kL2JailCounts = 0x8005B208, kL2JailArrays = 0x8005B340;

bool BuildRace(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool WorldPoolsInit(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool PoolTableInit(GuestRam& g, uint32_t sp, LoaderCallees& c);
void RaceFlag(GuestRam& g);
bool TrialLimits(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool PopBlockInit(GuestRam& g, uint32_t block, uint32_t size, uint32_t sp, LoaderCallees& c);
void PedTablesInit(GuestRam& g);
bool PartSlotsInit(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool CensusInit(GuestRam& g, uint32_t sp, LoaderCallees& c);
// RASHCDI 0x80068614, a leaf: the pool-0 bikes of class `cls` (their rider record +1 & 15); *out = those with a live word.
uint32_t CensusCount(GuestRam& g, uint32_t cls, uint32_t out);
bool SpeedClassCopy(GuestRam& g, uint32_t block, uint32_t off, uint32_t sp, LoaderCallees& c);
bool EscapeLoad(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool AnimNoise(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool FightLoad(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c, uint32_t& v0);
bool RoadLoad(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c, uint32_t& v0);
bool RoadText(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c);
bool RoadClear(GuestRam& g, uint32_t sp, LoaderCallees& c);
// `name`: RoadLoad's a0, which 0x8006AC54's `jal 0x8006ABC8` passes through untouched (MapLoad's name)
bool RoadRecords(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c);
bool GrfLoad(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool StreamSetUp(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool StreamStart(GuestRam& g, uint32_t sp, LoaderCallees& c);
void SirenSet(GuestRam& g, int32_t set);
void FxGlobalsInit(GuestRam& g);
void SRand(GuestRam& g, uint32_t seed);
uint32_t GrfFix(GuestRam& g, uint32_t grf);
// RASHCDI 0x8005D5E4 FxReset (RaceReset's child), frame 24: the draw list *(0x8005B280) = 0, 0x8005D968 (which only calls
// the empty 0x8005D960: no effect, not called here) and FxRecordsInit.
constexpr uint32_t kL2FxResetFn = 0x8005D5E4, kL2FxRecordsFn = 0x8005D988;
void FxReset(GuestRam& g);
// RASHCDI 0x8005D988, a leaf: the effect records' walk pointer 0x800D8068 = 0x800D39B0 (+4 / +8 / +12 = 0), the 20
// records 0x800D39B0 (112 bytes) free - byte +60 = 0, word 0's state and link bits 6..13 = 0x3F in 0..5 - and the last
// spawn stamp 0x8005B360 = 0.
void FxRecordsInit(GuestRam& g);

} // namespace rr::sim
