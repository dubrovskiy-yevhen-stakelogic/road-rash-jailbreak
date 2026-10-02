#pragma once
// src\game\sim\loader - the race loader's set-up path, ported from our own disassembly of the player's images:
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
//   EnterRace        RASHCDI 0x80063B90, frame 24   game_state+0x03 = 1, the pedestrian switch 0x8005B254, six calls
//   RaceReset        RASHCDI 0x80063500, frame 32   game_state+0x00 = 3, the players' 8-byte blocks, sixteen calls
//   SetUpRace        RASHCDI 0x80063670, frame 24   the race clock triple, the time limit per race type, the
//                                                   fight table pointer, the animation-object count (0x80063844),
//                                                   game_state+0x34 from the players' riders, eighteen calls
//   FinishTableInit  RASHCDI 0x8006A7C0, a leaf     the 18 finish records 0x800D5DA8 = {-1, -1, 0, 0}
//   RaceBlockCopy    RASHCDI 0x80068470, frame 32   the grid block's hazard part: +220 -> 0x8005B328 (8),
//                                                   +228 -> 0x8006B8B4 (4), +232 -> 0x8006EB10 (3 x 16)
//   PieceListInit    RASHCDI 0x8006A8FC, a leaf     the resident piece list 0x800D4B10 empty
//   SkyInit          RASHCDI 0x800609B0, frame 24   the sky block *(0x8005B278) (malloc 0x3048, heap 1)
//   ResTableInit     RASHCDI 0x8005D410, frame 24   the stream resource table (+ 0x8005D338 / 0x8005D38C / 0x8005D3F4)
//   ModelTablesInit  RASHCDI 0x8005BE40, frame 24   the model families (0x8005D018), the class lists, the registry
//   RegistryFind     RASHCDI 0x8005C010, a leaf     the registry slot of a model id, or -1
//   TexKey           RASHCDI 0x8005C054, a leaf     a registry record's page-table key (+7)
//   ClassFirst       RASHCDI 0x8005BD80, a leaf     a class list's first registry slot
//   FamilyRegister   SLUS    0x800303BC, a leaf     a model id's slot in its family table
//   RmdHandler       RASHCDI 0x8005CB9C, frame 32   a free registry slot, its LOD array (malloc)
//   DodHandler       RASHCDI 0x8005CC4C, frame 40   a LOD's DOD3 and part array (malloc), its pointers relocated
//   DpdHandler       RASHCDI 0x8005CD60, frame 40   a part's DPD3 (+ the relocation 0x8005CE5C)
//   BbdHandler       RASHCDI 0x8005CE78, frame 32   a LOD's BBD3
//   ChunkWalk        RASHCDI 0x8005C0C4, frame 56   one RMD3 chunk through the four handlers
//   GeoLoad          RASHCDI 0x8005CA10, frame 64   a .GEO file (and a .TEX file) off the CD, every RMD3 registered
//   CarModels        RASHCDI 0x8005C630, frame 272  the pedestrians' and the race's car .GEO
//   PopulationReset  RASHCDI 0x80068D54, frame 40   pools 3 / 4 / 5 / 6 and the pedestrians' reset, the pool counts
//   EffectSheet      RASHCDI 0x80061FAC, frame 176  the level bundle's type-5 section: 11 effect sprites
//   SoundLoad        RASHCDI 0x800627F8, frame 144  the race's sound set-up (records, RASHNZ_E.DAT's banks)
//   SoundInit        SLUS    0x8001E614, frame 48   the sound system's bank table and voices
//
// MEMORY MODEL (road_query.h's GuestRam): every function takes `sp`, the stack pointer at ITS entry, and
// makes each call at the depth the original makes it (sp - its frame, nested for the ported children it
// runs itself). The locals a callee receives by address sit at the original's frame offsets.
//
// THE SEAMS: every callee with a frame of its own that is not ported here, every CD read, every malloc /
// free and every GPU / SPU library call goes through `LoaderCallees::Call(fn, args, sp, v0)`. The bench
// (tools\rrverify\rows_loader.inc) answers with the oracle (or a stub planted on both machines, named per
// row); the product (src\game\loader_product.cpp) with the ported functions and the host's reads and
// blocks. RASHCDI's own data (the class capacities 0x8006B49C, the file-name formats, the sprite table
// 0x8006B4C0) is read where the overlay puts it: a caller whose arena holds RASHCDG there lays RASHCDI over
// it for the call, as the console has it resident while the race loads.
#include <cstdint>
#include <initializer_list>

#include "game/sim/road_query.h"

namespace rr::sim {

struct LoaderCallees {
    virtual ~LoaderCallees() = default;
    // The original's callee `fn` with its o32 arguments a[0..n) (n <= 8; a[4..] go to sp+16..), made at
    // stack pointer `sp`. False when the callee refused (a trap, a host failure): the port stops there.
    virtual bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) = 0;
};

// ---------------------------------------------------------------------------- entry points and frames
constexpr uint32_t kLdEnterRaceFn = 0x80063B90, kLdEnterRaceFrame = 24;
constexpr uint32_t kLdRaceResetFn = 0x80063500, kLdRaceResetFrame = 32;
constexpr uint32_t kLdSetUpRaceFn = 0x80063670, kLdSetUpRaceFrame = 24;
constexpr uint32_t kLdFinishInitFn = 0x8006A7C0;
constexpr uint32_t kLdBlockCopyFn = 0x80068470, kLdBlockCopyFrame = 32;
constexpr uint32_t kLdPieceListFn = 0x8006A8FC;
constexpr uint32_t kLdSkyInitFn = 0x800609B0, kLdSkyInitFrame = 24;
constexpr uint32_t kLdResTableFn = 0x8005D410, kLdResTableFrame = 24, kLdResHeapFrame = 24; // 0x8005D338's
constexpr uint32_t kLdModelTablesFn = 0x8005BE40, kLdModelTablesFrame = 24;
constexpr uint32_t kLdRegistryFindFn = 0x8005C010, kLdTexKeyFn = 0x8005C054, kLdClassFirstFn = 0x8005BD80;
constexpr uint32_t kLdFamilyRegisterFn = 0x800303BC; // SLUS
constexpr uint32_t kLdRmdFn = 0x8005CB9C, kLdRmdFrame = 32;
constexpr uint32_t kLdDodFn = 0x8005CC4C, kLdDodFrame = 40;
constexpr uint32_t kLdDpdFn = 0x8005CD60, kLdDpdFrame = 40;
constexpr uint32_t kLdBbdFn = 0x8005CE78, kLdBbdFrame = 32;
constexpr uint32_t kLdChunkWalkFn = 0x8005C0C4, kLdChunkWalkFrame = 56;
constexpr uint32_t kLdGeoLoadFn = 0x8005CA10, kLdGeoLoadFrame = 64;
constexpr uint32_t kLdCarModelsFn = 0x8005C630, kLdCarModelsFrame = 272;
constexpr uint32_t kLdPopResetFn = 0x80068D54, kLdPopResetFrame = 40;
constexpr uint32_t kLdTexFileFrame = 40;       // RASHCDI 0x8005C920 TexFile
constexpr uint32_t kLdCtkpFn = 0x8005BDAC;     // RASHCDI, a leaf
constexpr uint32_t kLdRigLoadFn = 0x8005C30C, kLdRigLoadFrame = 40;
constexpr uint32_t kLdBikeBankFn = 0x8005C45C, kLdBikeBankFrame = 272;
constexpr uint32_t kLdHazardModelsFn = 0x8005C7F0, kLdHazardModelsFrame = 272;
constexpr uint32_t kLdRenderCamInitFn = 0x8005CEE0, kLdRenderCamInitFrame = 24;
constexpr uint32_t kLdLectFrame = 96, kLdKnbpFrame = 48;                 // RASHCDI 0x8005DDB8 / 0x8005DA38
constexpr uint32_t kLdTslpFrame = 72, kLdClutUploadFn = 0x8005DAD0, kLdClutUploadFrame = 32;
constexpr uint32_t kLdRimTimFn = 0x8005E6CC, kLdRimTimFrame = 72;
constexpr uint32_t kLdTexTablesFn = 0x8005E848, kLdTexTablesFrame = 24;
constexpr uint32_t kLdGtpFn = 0x8005E4E8, kLdGtpFrame = 48;
constexpr uint32_t kLdTexSetUpFn = 0x8005D9EC, kLdTexSetUpFrame = 24;
constexpr uint32_t kLdDrawSync = 0x800487C0;   // SLUS libgpu DrawSync
constexpr uint32_t kLdTimFile = 0x8001408C;    // SLUS: a .TIM file off the CD, read (buf, TIM record)
constexpr uint32_t kLdSetGeomScreen = 0x8002201C, kLdSetGeomOffset = 0x80022064; // SLUS libgte (the GTE's H / offsets)
constexpr uint32_t kLdKnbp = 0x8005DA38, kLdTslp = 0x8005DBB8, kLdLect = 0x8005DDB8; // RASHCDI: TexFile's other arms
constexpr uint32_t kLdReadInto = 0x80014680;   // SLUS: a file read into a given buffer (the CD)
constexpr uint32_t kLdBikeName = 0x80064034;   // RASHCDI BikeName (grid_build.h runs it inline)
constexpr uint32_t kLdEffectSheetFn = 0x80061FAC, kLdEffectSheetFrame = 176;
constexpr uint32_t kLdSoundLoadFn = 0x800627F8, kLdSoundLoadFrame = 144;
constexpr uint32_t kLdSoundInitFn = 0x8001E614, kLdSoundInitFrame = 48; // SLUS
constexpr uint32_t kLdLoadBankFn = 0x8001ED28, kLdLoadBankFrame = 48;   // SLUS
constexpr uint32_t kLdSpuAttrFn = 0x8001E8C4, kLdSpuAttrFrame = 64;     // SLUS
constexpr uint32_t kLdSpuUpload = 0x8001E938;  // SLUS: SpuMalloc + the DMA of a bank's samples
constexpr uint32_t kLdPatchBank = 0x8001EAA4;  // SLUS PatchBank (PORTED, sound.h)
constexpr uint32_t kLdSpuCommonAttr = 0x800510B8; // SLUS libspu SpuSetCommonAttr

// ---------------------------------------------------------------------------- the seams, by address
constexpr uint32_t kLdMalloc = 0x8001447C;   // SLUS Malloc(bytes, heap)
constexpr uint32_t kLdFree = 0x800144B8;     // SLUS Free(block)
constexpr uint32_t kLdLoadFile = 0x80014654; // SLUS LoadFile(name, mode, &buf, &len, 0): the CD read into a malloc
constexpr uint32_t kLdMemset = 0x8001E100;   // SLUS memset by words
constexpr uint32_t kLdMemcpy = 0x8001E0B4;   // SLUS memcpy by words
constexpr uint32_t kLdBzero = 0x800448B4;    // SLUS bzero (BIOS A(28h) stub)
constexpr uint32_t kLdSprintf = 0x80043FD4;  // SLUS sprintf
constexpr uint32_t kLdTexFile = 0x8005C920;  // RASHCDI: a .TEX file's chunks (VRAM uploads, CTKP lists)
constexpr uint32_t kLdReadTim = 0x80014044;  // SLUS: libgs OpenTIM + ReadTIM into a 20-byte record
constexpr uint32_t kLdLoadImage = 0x80048A6C; // SLUS libgpu LoadImage(rect, pixels)

// ---------------------------------------------------------------------------- the globals
constexpr uint32_t kLdGameStatePtr = 0x8005B2F8;
constexpr uint32_t kLdPedSwitch = 0x8005B254;
constexpr uint32_t kLdPlayerBlocks = 0x800D81F0; // 6 x 8 bytes, stride 36
constexpr uint32_t kLdFinishTable = 0x800D5DA8;  // 18 x 16
constexpr uint32_t kLdClock0 = 0x8005ACD0, kLdClock1 = 0x8005ACCC, kLdTimeLimit = 0x8005ACC8;
constexpr uint32_t kLdFightPtr = 0x8005AD4C;
constexpr uint32_t kLdHazardTable = 0x8005B328, kLdHazardEvents = 0x8006B8B4, kLdHazardTemplates = 0x8006EB10;
constexpr uint32_t kLdPieceList = 0x800D4B10, kLdPiecesUsed = 0x8005B320, kLdPiecesLast = 0x8005B31C;
constexpr uint32_t kLdSkyPtr = 0x8005B278;
constexpr uint32_t kLdResListPtr = 0x8005ACBC;
constexpr uint32_t kLdRegistry = 0x800CE1B0;     // 50 x 16
constexpr uint32_t kLdClassLists = 0x800CE560;   // 7 x 8
constexpr uint32_t kLdClassListArea = 0x800D4B88;
constexpr uint32_t kLdClassCaps = 0x8006B49C;    // RASHCDI: 7 signed bytes
constexpr uint32_t kLdFamilies = 0x800D4C38;     // 7 x 16
constexpr uint32_t kLdSlotById = 0x800CD6D8;     // 109 x s16
constexpr uint32_t kLdPageTablePtr = 0x8005B2E4;
constexpr uint32_t kLdCdMode = 0x8005AD5C;
constexpr uint32_t kLdFxSprites = 0x800D4270;    // 11 x 28

// ---------------------------------------------------------------------------- the functions
bool EnterRace(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool RaceReset(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool SetUpRace(GuestRam& g, uint32_t sp, LoaderCallees& c);
void FinishTableInit(GuestRam& g);
bool RaceBlockCopy(GuestRam& g, uint32_t block, uint32_t sp, LoaderCallees& c);
void PieceListInit(GuestRam& g);
bool SkyInit(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool ResTableInit(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool ModelTablesInit(GuestRam& g, uint32_t sp, LoaderCallees& c);
int32_t RegistryFind(GuestRam& g, uint32_t id);
void TexKey(GuestRam& g, uint32_t slot);
void ClassFirst(GuestRam& g, uint32_t kind, uint32_t slot);
void FamilyRegister(GuestRam& g, uint32_t kind, uint32_t id, uint32_t slot);
// The handlers and the walk return the original's v0 in `v0`; false only when a seam refused.
bool RmdHandler(GuestRam& g, uint32_t id, uint32_t chunk, uint32_t sp, LoaderCallees& c, int32_t& v0);
bool DodHandler(GuestRam& g, uint32_t id, uint32_t chunk, int32_t gi, uint32_t sp, LoaderCallees& c, int32_t& v0);
int32_t DpdHandler(GuestRam& g, uint32_t id, uint32_t chunk, int32_t gi, int32_t si);
int32_t BbdHandler(GuestRam& g, uint32_t id, uint32_t chunk, int32_t gi);
bool ChunkWalk(GuestRam& g, uint32_t out, uint32_t buf, int32_t size, uint32_t first, uint32_t sp, LoaderCallees& c,
               int32_t& v0);
bool GeoLoad(GuestRam& g, uint32_t geo, uint32_t tex, uint32_t sp, LoaderCallees& c, int32_t& v0);
// RASHCDI 0x8005C920 TexFile(buf, size): a .TEX file's chunks - CTKP (0x8005BDAC, the class lists) here, KNBP /
// TSLP / LECT (0x8005DA38 / 0x8005DBB8 / 0x8005DDB8: VRAM and the texture tables) through the seams.
bool TexFile(GuestRam& g, uint32_t buf, int32_t size, uint32_t sp, LoaderCallees& c);
void CtkpList(GuestRam& g, uint32_t chunk);                            // RASHCDI 0x8005BDAC
// RASHCDI 0x8005C30C RigLoad(name, &len): a .MRO off the CD into the stream buffers' block, its LECT uploaded and the
// rest copied into a malloc'd block, walked and registered. v0 = the registry slot or -1.
bool RigLoad(GuestRam& g, uint32_t name, uint32_t lenOut, uint32_t sp, LoaderCallees& c, int32_t& v0);
// RASHCDI 0x8005C45C LoadBikeBank(bank): the bank's .GEO / .TEX (GeoLoad), then each player's rig bike (6..8 / 15..17)
// not yet registered (RigLoad). v0 = 0, or GeoLoad's -1.
bool LoadBikeBank(GuestRam& g, int32_t bank, uint32_t sp, LoaderCallees& c, int32_t& v0);
// RASHCDI 0x8005C7F0 HazardModels(set): DATA\hazard<set % 10>.GEO / .TEX through GeoLoad (set 0 in the race types
// with flags & 0x18 == 8). v0 = 1 when GeoLoad returned 0.
bool HazardModels(GuestRam& g, int32_t set, uint32_t sp, LoaderCallees& c, int32_t& v0);
// RASHCDI 0x8005CEE0 RenderCamInit(view) (SetUpRace's last calls, view 1 only with two players): the render camera
// pointer *(0x8005AEC0 + 4 view) = 0x800D82B0 and *(0x8005AEC4) = 0x800D8330, the record's H 237, depth 40 / 0x7FFF,
// the three 64s, the identity rotation and a zero eye; then SetGeomScreen(H) and SetGeomOffset(384, 240).
bool RenderCamInit(GuestRam& g, uint32_t view, uint32_t sp, LoaderCallees& c);
// The texture page table 0x800D5F70 (*(0x8005B2E4); 34 x 12 bytes: key, the sheet's place, x, tpage, clut) - the
// records ModelKeySet / TexKey look a key up in. VRAM (LoadImage, DrawSync) and the TIM reads go through the seams.
bool LectSheet(GuestRam& g, uint32_t chunk, uint32_t sp, LoaderCallees& c);      // RASHCDI 0x8005DDB8 (LECT)
bool KnbpClut(GuestRam& g, uint32_t chunk, uint32_t sp, LoaderCallees& c);       // RASHCDI 0x8005DA38 (KNBP)
// RASHCDI 0x8005DBB8 (TSLP): each player's three rider CLUT rows (by the bike's class and the player's appearance
// byte +10 of 0x800D81D8 + 36 p), their record at 0x8006B898 + 4 p (the overlay's data), and the three rows 43..45.
bool TslpCluts(GuestRam& g, uint32_t chunk, uint32_t sp, LoaderCallees& c);
// RASHCDI 0x8005DAD0 ClutUpload(src, n): CLUT row n (-1: the counter 0x800D6118, advanced) through LoadImage; v0 = n.
bool ClutUpload(GuestRam& g, uint32_t src, int32_t n, uint32_t sp, LoaderCallees& c, int32_t& v0);
bool RimTim(GuestRam& g, uint32_t name, uint32_t players, uint32_t sp, LoaderCallees& c, int32_t& v0); // 0x8005E6CC
bool TexTablesInit(GuestRam& g, uint32_t players, uint32_t sp, LoaderCallees& c); // RASHCDI 0x8005E848
bool GtpLoad(GuestRam& g, uint32_t players, uint32_t sp, LoaderCallees& c, int32_t& v0); // RASHCDI 0x8005E4E8
bool TexSetUp(GuestRam& g, uint32_t sp, LoaderCallees& c);                       // RASHCDI 0x8005D9EC (RaceReset's)
// RASHCDI 0x80064034 BikeName(table, idx), a leaf: the 9-byte name of bike `idx` in table 0 (0x8006B51C) or 1 (0x8006B5DC).
uint32_t BikeName(uint32_t table, uint32_t idx);
bool CarModels(GuestRam& g, int32_t raceId, uint32_t sp, LoaderCallees& c, int32_t& v0);
bool PopulationReset(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool EffectSheet(GuestRam& g, uint32_t a0, uint32_t table, uint32_t sp, LoaderCallees& c);
bool SoundLoad(GuestRam& g, uint32_t sp, LoaderCallees& c);
bool SoundInit(GuestRam& g, uint32_t config, uint32_t sp, LoaderCallees& c, int32_t& v0);
// SLUS 0x8001E8C4: SpuSetCommonAttr({mask 0x3FC3, main volume 0x3FFF / 0x3FFF, the rest 0}) - libspu's main volume.
bool SoundSpuAttr(GuestRam& g, uint32_t sp, LoaderCallees& c);

} // namespace rr::sim
