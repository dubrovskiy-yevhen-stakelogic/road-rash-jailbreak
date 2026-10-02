#pragma once
// The stream's files and the set-up's CD waits, the remaining
// pieces of StreamSetUp SLUS 0x80022F78 / StreamStart SLUS 0x80023020. Ported from our own disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// each accepted only by its row of `rrverify phys` (tools\rrverify\rows_stream3.inc). Memory model and the call seam
// are recover.h's: the callees the host serves (the CD file layer, malloc, sprintf, VSync, the kernel's critical
// section, the music stream player's set-up) go through `c` at the original's own stack pointers; the ported ones
// (StreamSelect / StreamReset / StreamCursor / StreamAtEnd / StreamRequest / StreamRange / StreamLimit / ResPump,
// CdReset / CdInit, the release list's fix-up) run natively.
#include <cstdint>

#include "game/sim/recover.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- entry points and frames
constexpr uint32_t kSfFilesFn = 0x80023498, kSfFilesFrame = 56;       // the stream's files
constexpr uint32_t kSfTocLoadFn = 0x80023F08, kSfTocLoadFrame = 40;   // STREAM<n>.TOC into a heap block
constexpr uint32_t kSfRlsLoadFn = 0x8002428C, kSfRlsLoadFrame = 32;   // STREAM<n>.RLS into a heap block
constexpr uint32_t kSfStpFn = 0x80023714, kSfStpFrame = 40;           // each player's .STP start
constexpr uint32_t kSfStpStartFn = 0x80024168, kSfStpStartFrame = 72; // one player's .STP start
constexpr uint32_t kSfCdFlushFn = 0x80022A1C, kSfCdFlushFrame = 24;   // the ring flushed, then the wait
constexpr uint32_t kSfCdWaitFn = 0x80022A78, kSfCdWaitFrame = 24;     // the wait for the idle drive
constexpr uint32_t kSfCdAbortFn = 0x800229F4, kSfCdAbortFrame = 24;   // the flush after a timed-out wait
constexpr uint32_t kSfAlbumFn = 0x80024630, kSfAlbumFrame = 88;       // the album (one player)
constexpr uint32_t kSfAlbumClearFn = 0x80024FF8;                      // a leaf: the music request table emptied

// ---------------------------------------------------------------------------- the host's callees
constexpr uint32_t kSfSprintf = 0x80043FD4;   // (buf, fmt, ...)
constexpr uint32_t kSfOpen = 0x80023100;      // (name): "<gp+0x1A0 prefix><name>" opened, mode 0 -> handle / < 0
constexpr uint32_t kSfSize = 0x800148BC;      // (handle) -> bytes
constexpr uint32_t kSfMalloc = 0x8001447C;    // (bytes, heap) -> block
constexpr uint32_t kSfRead = 0x80014780;      // (handle, buf, bytes, 0) -> bytes / < 0
constexpr uint32_t kSfClose = 0x8001460C;     // (handle)
constexpr uint32_t kSfVSync = 0x80047724;     // (0): one field
constexpr uint32_t kSfMusicSetUp = 0x80020BEC; // the music stream player's set-up (15 arguments) -> < 0 refused
constexpr uint32_t kSfMusicCb = 0x80024F78;   // its callback (an argument)

// ---------------------------------------------------------------------------- data
constexpr uint32_t kSfFmt = 0x8005AE38, kSfStream = 0x8005AE40, kSfToc = 0x8005AE48, kSfRls = 0x8005AE50,
                   kSfStr = 0x8005AE58;      // "%s%d%s", "STREAM", ".TOC", ".RLS", ".STR"
constexpr uint32_t kSfStpFmt = 0x80010BC8;   // "race%d_%ld.stp"
constexpr uint32_t kSfAlbum = 0x800CD670;    // the album's stream record
constexpr uint32_t kSfMusicReqs = 0x800D7E90; // 30 x {-1, 0} and three counters at +0xF0

// 0x80023498: STREAM<game_state+0x30>.TOC (gp+0x86C) and .RLS (gp+0x1A4 / gp+0x88C, 0 when it did not load), the
// release cursors gp+0x87C / gp+0x880 = 0, the .STR's handle gp+0x870 and size gp+0x884, gp+0x888 = 0, both stream
// records reset, the turn gp+0x874 = 0.
bool StreamFiles(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80023F08(out, name): the file into a malloc'd block, its +0x18 / +0x14 made absolute, *out = the block; 1, or 0
// when the read failed.
bool TocLoad(GuestRam& g, uint32_t out, uint32_t name, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// 0x8002428C(name): the release list into a malloc'd block, relocated (RlsRelocate); 1, or 0 (no file / read failed).
bool RlsLoad(GuestRam& g, uint32_t name, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// 0x80023714: "race<game_state+0x30>_<+0x40>.stp", then every player's StpStart.
bool StpPlayers(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80024168(name): the current record's cursor over the .STP's payload (its 0x28-byte header), up to three requests
// of 0x40 chunks each with the drive drained (two waits, the flush on a timeout) and the pump, the file closed, the
// cursor moved to the record's TOC range at the header's resume offset; 1, or 0 (no file / header read failed).
bool StpStart(GuestRam& g, uint32_t name, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// 0x80022A1C(mode): inside the critical section the current request's callback cleared (drive busy) and the ring
// emptied (CdReset); then CdWait(mode), whose value it returns.
bool CdFlush(GuestRam& g, uint32_t mode, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// 0x80022A78(mode): mode 2 - up to 200 VSync(0) while the drive is busy; mode 1 - spin until it is idle (the
// interrupt's; refused here while busy: nothing in a race waits so); returns the busy word.
bool CdWait(GuestRam& g, uint32_t mode, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// 0x800229F4(mode): CdFlush(mode), gp+0x18C = 0; returns the busy word read after the flush.
bool CdAbort(GuestRam& g, uint32_t mode, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// 0x80024630: the music request table emptied, *(0x8005B4E8) = *(0x80053658) = 0, ALBUM.ALB and ALBUM2.ALB (gp+0x1DC /
// gp+0x1E0) opened into the album record 0x800CD670, gp+0x564 = 0, the music stream player set up (0x80020BEC).
bool AlbumOpen(GuestRam& g, uint32_t sp, RecoverCallees& c);
// 0x80024FF8 (a leaf).
void AlbumClear(GuestRam& g);

} // namespace rr::sim
