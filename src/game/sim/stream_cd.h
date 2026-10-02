#pragma once
// The rest of the original's streamer: the CD access layer the
// read requests go through, the sky's panorama pick, the cell's texture binding and the draw's readiness test,
// the speech bank's buffer release. Ported from our own disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8 (the sky)
//
// each accepted only by its row of `rrverify phys` (tools\rrverify\rows_stream2.inc). Memory model, stack and
// the call seam are stream.h's (recover.h).
//
// THE CD ACCESS LAYER. A read request (32 bytes: +0 kind, +4 file, +8 position, +0xC record index, +0x10 buffer,
// +0x14 arg, +0x18 callback, +0x1C bytes) is copied into a ring of 50 (0x800D7740; tail *(0x80053468), head
// *(0x8005346C), count *(0x80053470), music count *(0x80053474)); the first one into an idle drive
// (*(0x80053464) == 0) starts it: the next request is taken (gp+0x860 -> it), a request whose record is already
// read is dropped ("Dbl Buf Overflow"), the drive seeks (SLUS 0x80014634) and reads (0x80014894) with the
// completion 0x80022EEC, the start stamped gp+0x190 = game_state+0x0C and gp+0x194 = 1 (what the stall grade
// 0x80022C64 reads). The completion clears gp+0x194, counts gp+0x85C, calls the request's callback (ReadDone
// 0x80030894) and starts the next. The drive itself (seek, read, its time) is the host's.
#include <cstdint>

#include "game/sim/stream.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- addresses
constexpr uint32_t kCdRing = 0x800D7740;       // 50 x 32-byte requests
constexpr uint32_t kCdRingSize = 50;
constexpr uint32_t kCdBusy = 0x80053464;       // the drive runs a request
constexpr uint32_t kCdTail = 0x80053468, kCdHead = 0x8005346C, kCdCount = 0x80053470, kCdMusicCount = 0x80053474;
constexpr uint32_t kGpCdInit = 0x18C, kGpCdDone = 0x85C, kGpCdCur = 0x860, kGpCdStarts = 0x864;
constexpr uint32_t kCdQuit = 0x8005B220;       // the game's quit word (GameFrame: state 2 instead of the wait)
constexpr uint32_t kStRenderCams = 0x8005AEC0; // -> the render camera of view 0 / 1 (+0x7C its yaw)
constexpr uint32_t kSkyStartCol = 0x800D5EE8;  // s16[2]: the view's first sky column and +24
constexpr uint32_t kSkySplit = 0x8005B2D0;     // nonzero: no sky pick (0x800650D0)
constexpr uint32_t kRlsCursorWord = 0x8005B508; // gp+0x87C: player 0's release-list record (its +0: a panorama key)
constexpr uint32_t kStreamStateSel = 0x8005AE34; // gp+0x1A8: the current stream record

// the host's side (and the callees)
constexpr uint32_t kCdSeekFn = 0x80014634;      // (file, pos, 0): THE DRIVE
constexpr uint32_t kCdReadFn = 0x80014894;      // (file, buf, bytes, cb) -> < 0 refused: THE DRIVE
constexpr uint32_t kCdDoneLabel = 0x80022EEC;   // the completion (a label inside 0x80022D20's neighbourhood)
constexpr uint32_t kSkyNopFn = 0x8005E840;      // RASHCDG (0xFF00): an empty function
constexpr uint32_t kSkyUploadFn = 0x800662BC;   // RASHCDG: the MDEC column-pair decode to VRAM (the renderer's)

// the ported functions
constexpr uint32_t kCdQueueFn = 0x80022D20, kCdNextFn = 0x80022B0C, kCdTakeFn = 0x80022E38, kCdStartFn = 0x80022BEC,
                   kCdResetFn = 0x80022CC4, kCdInitFn = 0x800229B0, kResFreeIrqFn = 0x80030FA0;
constexpr uint32_t kSkyViewFn = 0x800650D0, kSkyFrameFn = 0x80065174, kSkyPickFn = 0x800654B4, kSkyTurnFn = 0x800656A8;
constexpr uint32_t kCellTexBindFn = 0x800325BC; // (CellReady 0x800363F0 is cell_draw.h's)

// ============================================================================ the CD access layer
// 0x80022D20(req): the request filed (0), or -100 with the ring full; the drive started when idle.
int32_t CdQueue(GuestRam& g, uint32_t req, uint32_t sp, RecoverCallees& c);
// 0x80022B0C(a0): with a0 == 0 the next request started (gp+0x860), or the drive marked idle.
void CdNext(GuestRam& g, uint32_t a0, uint32_t sp, RecoverCallees& c);
// 0x80022E38(out): *out = the oldest request; 0, or -99 when none.
int32_t CdTake(GuestRam& g, uint32_t out);
// 0x80022BEC(req): the seek and the read started, the start stamped.
void CdStart(GuestRam& g, uint32_t req, uint32_t sp, RecoverCallees& c);
// 0x80022EEC: the read of gp+0x860 done - its callback, then the next request.
void CdDone(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80022CC4: the ring emptied.
void CdReset(GuestRam& g);
// 0x800229B0: the layer's once-per-boot reset and the stall stamps cleared.
void CdInit(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80030FA0(index): record `index` freed from the CD / SPU side (*(0x800541D0) held: no critical section).
void ResFreeIrq(GuestRam& g, uint32_t index, uint32_t sp, RecoverCallees& c);

// ============================================================================ the sky (RASHCDG)
// 0x800650D0(view): the view's first sky column (from its camera's yaw) and, with one player and no split, the
// sky's frame 0x80065174.
void SkyView(GuestRam& g, uint32_t view, uint32_t sp, RecoverCallees& c);
// 0x80065174: unless a column decode runs (+0x18), the columns seen (+0x30 / +0x32) and the panorama to show
// (+0x10, SkyPick at the stream record's road and along << 6): a new one (+0x12 = +0x10) gets its column tables
// and the decode restarted (0x800662BC); the same one scrolls by SkyTurn.
void SkyFrame(GuestRam& g, uint32_t sp, RecoverCallees& c);
// OURS (the wide picture, sky_split.h): SkyFrame's columns seen +0x30 / +0x32 opened by `left` / `right`
// columns and a new panorama's first decode 26 + left + right - the original's decode simply kept over the wider
// field. 0 / 0 (the default, the bench): the original, bit for bit.
void SetSkyFrameWide(int32_t left, int32_t right);
// 0x800654B4(road, along): the slot of the panorama to show - the shown one while it is the release list's
// panorama (*(*(0x8005B508))), else the slot holding that one, else the resident panorama whose window middle on
// `road` is nearest `along`; -1 with none resident. A new pick's 59 column pointers (+0x3D8) and +4 are set.
int32_t SkyPick(GuestRam& g, uint32_t road, uint32_t along);
// 0x800656A8(a, b, c, d): the columns to decode when the view turns from [a, b] to [c, d] (s16, of 110);
// positive: to the right, negative: to the left, 0: none.
int32_t SkyTurn(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

// ============================================================================ the cells' textures
// 0x800325BC(slot, p): a cell slot's two page keys (+0x50, +0x58) bound to player p's texture slots (+0x68,
// +0x60) where resident; returns 1.
uint32_t CellTexBind(GuestRam& g, uint32_t slot, uint32_t p);

} // namespace rr::sim
