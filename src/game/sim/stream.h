#pragma once
// The original's streamer: which road pieces, scene cells, textures and panoramas are resident, and when.
// Ported function by function from our own disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDI.BIN  SHA-1 9a8b79d8... (the loader overlay; only the resource-table initialiser 0x8005D410)
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_stream.inc).
//
// THE MACHINE. Each player has a stream record (0x80053478 + 0x80 p, the current one at gp+0x1A8): the
// road / along / direction it follows (+0x48 / +0x44 / +0x40, from the CamTarget entity +0x04), and a read
// cursor (+0x08: file, position, kind, start, end, flags) over the STREAM<set>.STR range of that road in
// that direction (STREAM<set>.TOC, gp+0x86C). Every game frame (SLUS 0x8002305C, first thing of GameFrame)
// per player: StreamTrack re-reads the position, StreamSeek moves the cursor on a road change, the release
// pass frees every loaded resource whose residency windows no longer hold the position, and then one read
// request asks for up to five 16 KiB chunks into free records of the resource table *(0x8005ACBC) (32
// records, each owning a 16 KiB buffer). A completed read (ReadDone 0x80030894) queues the record; the pump
// (0x80030608) dispatches each by its key type to its loader: cells (0/8/9: the slot table 0x800D87E8),
// textures (1/2: the texture slots 0x800D9268 and the VRAM pages), road pieces (3: the resident piece list
// 0x800D4B10 and the BTT_ binding), panoramas (4) and SPU samples (10).
//
// THE HOST SIDE (named, in the product): the CD. SLUS 0x80022D20 queues a read for the CD interrupt; the
// product reads the 16 KiB from the disc image at once and runs ReadDone in its place, so a read completes
// at the moment it is queued (the one timing difference to the console, where a chunk takes ~50 ms).
//
// MEMORY MODEL, STACK and THE CALL SEAM are recover.h's: every callee that is not a function of this file
// goes through `RecoverCallees::Call(fn, ...)` at the original's call-site stack pointer - the bench
// answers it with the original code, the product with its natives (src\game\stream_product.cpp).
#include <cstdint>

#include "game/sim/recover.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- addresses
constexpr uint32_t kStGameState  = 0x8005B2F8; // -> game_state (+0 state, +4 mode bits, +0xC clock, +0x28, +0x30 players)
constexpr uint32_t kStResList    = 0x8005ACBC; // -> the resource table (0x800CE5B8 in every capture)
constexpr uint32_t kStRoadTables = 0x8005AE64; // -> +0x14 node records (40 B), +0x18 road records (16 B)
constexpr uint32_t kStStates     = 0x80053478; // two 128-byte stream records
constexpr uint32_t kStStateBytes = 0x80;
constexpr uint32_t kStReqList    = 0x800D7D80; // the record pointers a read request takes (s32[64])
constexpr uint32_t kStFileTable  = 0x800D6510; // the open files, 24 bytes each (+0 flags bit 8)
constexpr uint32_t kStPieces     = 0x800D4B10; // 6 x {id, half, obj[2]}
constexpr uint32_t kStPiecesLast = 0x8005B31C; // s32: the highest used index
constexpr uint32_t kStPiecesUsed = 0x8005B320; // s32: how many
constexpr uint32_t kStFirstPiece = 0x8005AD54; // the first road object ever bound
constexpr uint32_t kStCellSlots  = 0x800D87E8; // 24 x 112 (scene_cell.md 1.1)
constexpr uint32_t kStTexSlots   = 0x800D9268; // 2 x 24 x 48: the texture slots
constexpr uint32_t kStVramA      = 0x800D76D0; // kind 1 pages: 8-byte entries, 32 bytes a player
constexpr uint32_t kStVramB      = 0x800D7710; // kind 0 pages: 8-byte entries, 24 bytes a player
constexpr uint32_t kStVramCountB = 0x8005AEE4; // pages of kind 0 (3, or 2 with two players)
constexpr uint32_t kStVramCountA = 0x8005AEE8; // pages of kind 1 (4)
constexpr uint32_t kStTpageTable = 0x800D8768; // u32 per page: the two texture keys of a tpage
constexpr uint32_t kStVramLayout = 0x800533B5; // SLUS data: the players' VRAM layout bytes
constexpr uint32_t kStSkyPtr     = 0x8005B278; // -> the sky block: +0x10/+0x12 panoramas shown, +0x22 count, +0x2E4 slots
constexpr uint32_t kStMusicBusy  = 0x800541D0; // set while the CD layer's own callback frees a record
constexpr uint32_t kStPumpFlag   = 0x8005AED8;
constexpr uint32_t kStStartRec   = 0x800D6184; // -> [START] record (road, along s16 +6, direction +8)
constexpr uint32_t kStFinishRec  = 0x800D6188;
constexpr uint32_t kStFinishCell = 0x8005B310;
constexpr uint32_t kStPlayer1    = 0x8005B38C; // -> player 1's bike
constexpr uint32_t kStPlayer2    = 0x8005B21C; // -> player 2's bike
constexpr uint32_t kStRiderPtrs  = 0x8005B268; // Bike*[2]
constexpr uint32_t kStViews      = 0x800CD898; // view records, 1132 bytes
// gp-relative words
constexpr uint32_t kGpStCdBusy = 0x194, kGpStCdStamp = 0x190, kGpStCur = 0x1A8, kGpStCellCount = 0x250,
                   kGpStToc = 0x86C, kGpStStr = 0x870, kGpStTurn = 0x874, kGpStRls = 0x87C, kGpStStrSize = 0x884,
                   kGpStLastPos = 0x888, kGpStSkyBusy = 0x644;

// ---------------------------------------------------------------------------- the seams
constexpr uint32_t kStCdQueueFn    = 0x80022D20; // (req) -> 0 queued, -100 full: THE CD (host side)
constexpr uint32_t kStMusicFrameFn = 0x800247E8; // the music streamer's frame (1 player)
constexpr uint32_t kStMusicDoneFn  = 0x800248E4; // (rec+4, buf): a music chunk read
constexpr uint32_t kStCritEnterFn  = 0x80043DA4; // EnterCriticalSection wrapper
constexpr uint32_t kStCritLeaveFn  = 0x80043DB4;
constexpr uint32_t kStPrintFn      = 0x80044894; // (fmt): the debug print (the product: not run)
constexpr uint32_t kStEntityCellFn = 0x8008B99C; // (e) -> v0 (ported, cell_draw.h)
constexpr uint32_t kStRegionZeroFn = 0x800135E8; // (slot + 4, p): the cell's region-0 arrays (ported)
constexpr uint32_t kStCellTexFn    = 0x800325BC; // (slot, p): the cell's texture pages (renderer side)
constexpr uint32_t kStCellTexPassFn = 0x80033F14; // (slot, pass, p): the cell's texture fix-ups (renderer side)
constexpr uint32_t kStTexPassAllFn = 0x800333F4; // (pass, p): 0x80033F14 over the player's slots (renderer side)
constexpr uint32_t kStTexUnlinkFn  = 0x800348CC; // (slot, kind): a cell's texture link dropped (renderer side)
constexpr uint32_t kStLoadImageFn  = 0x80048A6C; // LoadImage(rect, src): THE GPU (host side)
constexpr uint32_t kStSpeechLoadFn = 0x8001A0C0; // (buf, id, rec+4) -> 1 / 2 (ported, speech.h)
constexpr uint32_t kStCellFreeFn   = 0x8008C45C; // CellRelease: ported here, called natively
constexpr uint32_t kStActivateFn   = 0x80093E6C; // RASHCDG population passes (ported, population.h)
constexpr uint32_t kStDownedFn     = 0x800950E8;
constexpr uint32_t kStNearPieceFn  = 0x80039F68; // (h) -> s16 (ported)
constexpr uint32_t kStPoolDropFn   = 0x8008C000; // (h, pool) (ported)
constexpr uint32_t kStSleepFn      = 0x80093ED4; // (e, 1) (ported)
constexpr uint32_t kStRiderSleepFn = 0x800951B8; // (rider, 1) (ported)
constexpr uint32_t kStJailCellFn   = 0x800A3ECC; // (id, p): two-player mode only, the class-50 release (ported, mp_world.h)
constexpr uint32_t kStOtherSlotFn  = 0x80013360; // (id, p) (ported, world_pop.h)
constexpr uint32_t kStFxStopFn     = 0x8002847C; // (car) (ported, effects.h)
constexpr uint32_t kStFxFreeFn     = 0x8002820C; // (car) (ported)
constexpr uint32_t kStPedFreeFn    = 0x800CC0B0; // (ped) (ported, traffic_bind.h)
constexpr uint32_t kStStampFn      = 0x8009C41C; // (slot, 6, rec, 0) (ported, world_pop.h)
constexpr uint32_t kStRebaseAFn    = 0x8003D7C4; // (h, delta): a cursor moved with its object (two players)
constexpr uint32_t kStRebaseBFn    = 0x8003D7A8; // (e, delta)

// ---------------------------------------------------------------------------- the ported functions
constexpr uint32_t kStFrameFn      = 0x8002305C;
constexpr uint32_t kStPlayersFn    = 0x800237B8;
constexpr uint32_t kStTrackFn      = 0x80023A14;
constexpr uint32_t kStSeekFn       = 0x80023FBC;
constexpr uint32_t kStRangeFn      = 0x80023DE4;
constexpr uint32_t kStCursorFn     = 0x80023C24;
constexpr uint32_t kStLimitFn      = 0x800243BC;
constexpr uint32_t kStAtEndFn      = 0x80023300;
constexpr uint32_t kStRequestFn    = 0x80023148;
constexpr uint32_t kStUnwindFn     = 0x80023358;
constexpr uint32_t kStFreeCountFn  = 0x80023C4C;
constexpr uint32_t kStReadFn       = 0x80023CAC;
constexpr uint32_t kStReleaseFn    = 0x80030E58;
constexpr uint32_t kStWindowFn     = 0x80023900;
constexpr uint32_t kStStallFn      = 0x80022C64;
constexpr uint32_t kStStallCellsFn = 0x80023870;
constexpr uint32_t kStPlaceFn      = 0x800235C8;
constexpr uint32_t kStResetFn      = 0x80024354;
constexpr uint32_t kStTakeFn       = 0x80030CD8;
constexpr uint32_t kStTakeMusicFn  = 0x80031064;
constexpr uint32_t kStTakeStreamFn = 0x80031170;
constexpr uint32_t kStArmFn        = 0x80031368;
constexpr uint32_t kStCountFn      = 0x800322D0;
constexpr uint32_t kStFreeFn       = 0x800313EC;
constexpr uint32_t kStFreeByBufFn  = 0x80031008;
constexpr uint32_t kStDoneFn       = 0x80030894;
constexpr uint32_t kStPumpFn       = 0x80030608;
constexpr uint32_t kStPopFn        = 0x80031D70;
constexpr uint32_t kStDupFn        = 0x80031BF8;
constexpr uint32_t kStPairFn       = 0x800319F8;
constexpr uint32_t kStHeadFn       = 0x80031B4C;
constexpr uint32_t kStLoadFn       = 0x80031604;
constexpr uint32_t kStUnloadFn     = 0x800318E8;
constexpr uint32_t kStEntryTagFn   = 0x80031E1C;
constexpr uint32_t kStEntryFn      = 0x80023960;
constexpr uint32_t kStCellLoadFn   = 0x80032A20;
constexpr uint32_t kStCellFixFn    = 0x8003234C;
constexpr uint32_t kStCellUnloadFn = 0x80032810;
constexpr uint32_t kStPieceLoadFn  = 0x8003CEC4;
constexpr uint32_t kStPieceAddFn   = 0x8003CA50;
constexpr uint32_t kStPieceBindFn  = 0x8003CFCC;
constexpr uint32_t kStPieceUnloadFn = 0x8003D844;
constexpr uint32_t kStPieceDropFn  = 0x8003CB6C;
constexpr uint32_t kStPieceRebaseFn = 0x8003D39C;
constexpr uint32_t kStTexLoadFn    = 0x80034DEC;
constexpr uint32_t kStTexFindFn    = 0x80022218;
constexpr uint32_t kStTexLinkFn    = 0x800324CC;
constexpr uint32_t kStVramFindFn   = 0x800335B4;
constexpr uint32_t kStVramBindFn   = 0x80033B50;
constexpr uint32_t kStVramUploadFn = 0x8002227C;
constexpr uint32_t kStTexFreeFn    = 0x80034D38;
constexpr uint32_t kStTexUnlinkAllFn = 0x800326BC;
constexpr uint32_t kStVramFreeFn   = 0x80033634;
constexpr uint32_t kStTexSweepFn   = 0x80033198;
constexpr uint32_t kStPanoLoadFn   = 0x800136BC;
constexpr uint32_t kStPanoFreeFn   = 0x80013828;
constexpr uint32_t kStResInitFn    = 0x8005D410; // RASHCDI

// ============================================================================ the stream record
// 0x8002379C: gp+0x1A8 = the stream record of player p.
void StreamSelect(GuestRam& g, uint32_t p);
// 0x80023A14: the record follows its entity's road position (+0x168 high half 0 only): road, along,
// direction; the road-changed / direction-changed flags +0x2C / +0x30, the road record +0x5C and its
// length +0x60, the next road +0x34, the distance run +0x64, the near-an-end state +0x3C / +0x24 / +0x28;
// then the snapshot +0x50 = {road, along, dir} (0x80023DB8).
void StreamTrack(GuestRam& g);
// 0x80023C24(cursor, file, start, len): a read cursor over [start, start + len), kind 0x10.
void StreamCursor(GuestRam& g, uint32_t cursor, uint32_t file, uint32_t start, uint32_t len);
// 0x80023DE4: the cursor over the TOC range of the record's road in its direction (mode +0x38 == 0), or
// over nothing (mode 1); then +0x64 = 0, +0x68 = (dir < 1) << 16.
void StreamRange(GuestRam& g);
// 0x800243BC(off): the cursor's position = its start + off, at most its end.
void StreamLimit(GuestRam& g, uint32_t off);
// 0x80023FBC: the seek state machine on the change flags (+0x24 / +0x28 / +0x2C / +0x30, mode +0x38),
// through the release list's cursor for the resume point; the flags cleared.
void StreamSeek(GuestRam& g);
// 0x80023300(cursor): at the end of its range or not (+0x14 bit 1, the file table's bit 8 cleared);
// returns the bit.
uint32_t StreamAtEnd(GuestRam& g, uint32_t cursor);
// 0x80024354: the current record emptied (the loader, twice).
void StreamReset(GuestRam& g);
// 0x800235C8(road, along, dir): every player's record placed at the [START] position.
void StreamPlace(GuestRam& g, uint32_t road, int32_t along, int32_t dir);

// ============================================================================ the resource table
// 0x800322D0(from, to): one record moved between the state counters (+0x10 free, +0x00 loaded, +0x14
// pending, +0x0C reading) - inside a critical section unless *(0x800541D0).
void ResCount(GuestRam& g, int32_t from, int32_t to, uint32_t sp, RecoverCallees& c);
// 0x80032190(from, to): the bump itself, which the table functions call bare inside their own critical section.
void ResBump(GuestRam& g, int32_t from, int32_t to);
// 0x800313EC(rec): a record done with - its extra chunk count +0x0F000 decremented and re-armed, or freed.
void ResFree(GuestRam& g, uint32_t rec, uint32_t sp, RecoverCallees& c);
// 0x80031008(buf): the record whose payload +0x10 is `buf` (or the one past the table) freed.
void ResFreeByBuffer(GuestRam& g, uint32_t buf, uint32_t sp, RecoverCallees& c);
// 0x80031064 / 0x80031170: up to n free records of the music range / player p's range (keeping the
// table's reserve +0xA5C free unless `all`) taken (0x100) into s32 list[]; returns how many.
int32_t ResTakeMusic(GuestRam& g, uint32_t list, int32_t n, uint32_t sp, RecoverCallees& c);
int32_t ResTakeStream(GuestRam& g, uint32_t list, int32_t n, int32_t all, int32_t p, uint32_t sp, RecoverCallees& c);
// 0x80031368(rec, kind): armed for a read of `kind` (8 music, 0x10 stream).
void ResArm(GuestRam& g, uint32_t rec, uint32_t kind, uint32_t sp, RecoverCallees& c);
// 0x80030CD8(list, n, kind, contiguous, arg): the records a read request takes; returns how many.
int32_t ResTake(GuestRam& g, uint32_t list, uint32_t n, uint32_t kind, uint32_t contiguous, uint32_t arg,
                uint32_t sp, RecoverCallees& c);
// 0x80023358(list, n): the records of an unsent request freed.
void StreamUnwind(GuestRam& g, uint32_t list, int32_t n, uint32_t sp, RecoverCallees& c);
// 0x80023148(cursor, n, contiguous, arg): up to n chunks from the cursor queued into free records, the
// cursor moved on 16 KiB each; 1, or -1 (at the end / refused / queue full, the rest unwound).
int32_t StreamRequest(GuestRam& g, uint32_t cursor, uint32_t n, uint32_t contiguous, uint32_t arg, uint32_t sp,
                      RecoverCallees& c);
// 0x800312D0(p): the free records of player p's range beyond the one-player reserve of two.
int32_t ResFreeCount(GuestRam& g, int32_t p);
// 0x80023C4C: +0x6C = the current record has free records and is not at its end.
void StreamFreeCount(GuestRam& g);
// 0x80023CAC: the turn (gp+0x874) moves to the other player when this one has nothing to read; then one
// request of up to 5 - (reading) chunks for it.
void StreamRead(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80023900 + 0x800243EC(type, windows, p): the residency test of a resource of player p: 0 in a
// window, 1 not yet reached, -1 past or on another road.
int32_t ResWindow(GuestRam& g, uint32_t type, uint32_t windows, uint32_t p);
// 0x80030E58(p): every loaded resource of player p's range tested; the ones past are unloaded
// (0x800318E8); then the texture slots swept (0x80033198).
void ResRelease(GuestRam& g, uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x800237B8: every player's frame (track, entry tags on a road change, seek, release, free count), then
// the read.
void StreamPlayers(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80030894(index, kind, arg): the read of record `index` done - it becomes loaded; a stream chunk is
// queued for the pump, a music chunk loaded at once.
void ResReadDone(GuestRam& g, uint32_t index, uint32_t kind, uint32_t arg, uint32_t sp, RecoverCallees& c);
// 0x80030608: the queued chunks - duplicates freed, texture halves paired, the rest marked - and every
// marked one loaded (0x80031604).
void ResPump(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80031604(rec, p): a chunk to its loader by its key type; loaded (+2), retried, or freed.
void ResLoad(GuestRam& g, uint32_t rec, uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x800318E8(rec, p): a loaded chunk to its unloader, then freed.
void ResUnload(GuestRam& g, uint32_t rec, uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x80031E1C(road, rec, p): the entry coordinate +0x1C of one cell chunk (rec != 0) or of every loaded
// cell chunk of player p's range, relative to `road`.
void ResEntryTag(GuestRam& g, uint32_t road, uint32_t rec, uint32_t p);
// 0x80023960(windows, p): the finish / start cell's entry and distance words of player p's record.
void StreamEntry(GuestRam& g, uint32_t windows, uint32_t p);
// 0x8002305C: the game frame's stream step (players, pump, music, the stall test on game_state +0x28).
void StreamFrame(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80022C64: the CD-stall grade (0, 1 short, 2 long) from gp+0x194 / gp+0x190.
int32_t StreamStall(GuestRam& g);
// 0x80023870: 1 when a player's bike or view has no cell (EntityCell < 0).
uint32_t StreamStallCells(GuestRam& g, uint32_t sp, RecoverCallees& c);

// ============================================================================ the loaders
// 0x80032A20(payload, id, type, ext, rec+0x1C, p): a scene cell into player p's slot table; 1 loaded, 3
// already there, 0 no room / not yet, 2 no id.
uint32_t CellLoad(GuestRam& g, uint32_t payload, uint32_t id, uint32_t type, uint32_t ext, uint32_t entry,
                  uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x8003234C(slot, payload): the cell's texture keys and body relocated in place.
void CellFix(GuestRam& g, uint32_t slot, uint32_t payload);
// 0x80032810(id, type, p): player p's slot of `id` emptied (type 9: its region 7 only); 1 when a slot was.
uint32_t CellUnload(GuestRam& g, uint32_t id, uint32_t type, uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x8008C45C(id, p): what lives in cell `id` put to sleep or released (bikes, riders, cars, peds, props,
// volumes).
void CellRelease(GuestRam& g, uint32_t id, uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x8003CEC4(payload, id, p): a road object; 1 bound (the population passes rerun), 0 not yet, 2 / 3 dropped.
uint32_t PieceLoad(GuestRam& g, uint32_t payload, uint32_t id, uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x8003CA50(id, obj, half): the object into the resident piece list; 1 new, 0 full, -3 there, -1 the
// other half (two players).
int32_t PieceAdd(GuestRam& g, uint32_t id, uint32_t obj, uint32_t half);
// 0x8003CFCC(obj, id): the object's blocks found and bound to its BTT_ record; 1, or 0 with no record.
uint32_t PieceBind(GuestRam& g, uint32_t obj, uint32_t id);
// 0x8003D844(id, p): a road object released; 1 / 2 / 0.
uint32_t PieceUnload(GuestRam& g, uint32_t id, uint32_t p, uint32_t sp, RecoverCallees& c);
// 0x8003CB6C(id, half): its slot of the piece list dropped; the slot index when the other half stays,
// 6 when the other half moved, -1 freed / not there.
int32_t PieceDrop(GuestRam& g, uint32_t id, uint32_t half);
// 0x8003D39C(newObj, oldObj): every cursor on the moved object rebased (two players).
void PieceRebase(GuestRam& g, uint32_t newObj, uint32_t oldObj);
// 0x80034DEC(key, buf, kind, half, v, windows, p): a texture chunk into player p's texture slots; 1, or 3
// already there.
uint32_t TexLoad(GuestRam& g, uint32_t key, uint32_t buf, uint32_t kind, uint32_t half, uint32_t v,
                 uint32_t windows, uint32_t p, uint32_t sp, RecoverCallees& c);
int32_t TexFind(GuestRam& g, uint32_t key, uint32_t p);                  // 0x80022218
void TexLink(GuestRam& g, uint32_t slot, uint32_t kind, uint32_t p);     // 0x800324CC
int32_t VramFind(GuestRam& g, uint32_t kind, uint32_t key, uint32_t p);  // 0x800335B4
uint32_t VramBind(GuestRam& g, uint32_t page, uint32_t slot, uint32_t kind, uint32_t p, uint32_t sp,
                  RecoverCallees& c);                                    // 0x80033B50
void VramUpload(GuestRam& g, uint32_t page, uint32_t key, uint32_t a, uint32_t b, uint32_t p, uint32_t sp,
                RecoverCallees& c);                                      // 0x8002227C
int32_t TexFree(GuestRam& g, uint32_t key, uint32_t kind, uint32_t p, uint32_t sp, RecoverCallees& c); // 0x80034D38
void TexUnlinkAll(GuestRam& g, uint32_t key, uint32_t p, uint32_t sp, RecoverCallees& c);             // 0x800326BC
void VramFree(GuestRam& g, int32_t page, uint32_t kind, uint32_t p);     // 0x80033634
void TexSweep(GuestRam& g, uint32_t p, uint32_t sp, RecoverCallees& c);  // 0x80033198
// 0x800136BC(payload, windows, key): a panorama into the sky block's 20 slots; 1, 3 there, 0 full, 2 not.
uint32_t PanoLoad(GuestRam& g, uint32_t payload, uint32_t windows, uint32_t key);
// 0x80013828(payload): its slot freed unless shown (+0x10 / +0x12); 1, else 2.
uint32_t PanoFree(GuestRam& g, uint32_t payload);

// ============================================================================ the loader's set-up
// RASHCDI 0x8005D410 (+ 0x8005D38C, 0x8005D3F4; 0x8005D338's heap block is `buffers`): the resource
// table's ranges by player count, counters zero, 32 records over `buffers`, the queue empty.
void ResInit(GuestRam& g, uint32_t buffers);
// RASHCDI 0x8005D63C's texture part (players 1 or 2): the texture slots, the VRAM page tables and
// their counts, the cell slot table and the draw lists.
void TexInit(GuestRam& g, int32_t players);

} // namespace rr::sim
