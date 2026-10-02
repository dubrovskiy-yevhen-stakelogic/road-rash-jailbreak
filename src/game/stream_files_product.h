#pragma once
// The stream's files in the product: StreamSetUp SLUS 0x80022F78 and StreamStart SLUS
// 0x80023020 with ALL their children PORTED (stream_files.h) - the album 0x80024630, the stream's files 0x80023498
// (STREAM<n>.TOC through 0x80023F08, the release list STREAM<n>.RLS through 0x8002428C, the .STR), the CD flush
// 0x80022A1C with its wait 0x80022A78, StreamPlace 0x800235C8 and each player's .STP start 0x80023714 / 0x80024168.
// The TOC and the release list land in the original's heap blocks after the route loaders' (SLUS 0x8001447C's block
// rule continued from ROAD<n>.MAP's block: rr-race's TOC 0x801ABAB4 and RLS 0x801AC45C).
//
// The host's, named: the CD file layer (open / size / read / close on the disc image; the handles as every capture
// numbers them: ALBUM.ALB 0, ALBUM2.ALB 2, the stream's files 3, the .STP 4), sprintf (%s, %d, %ld), malloc (the
// block rule), VSync (one field of the drive model: 5 ticks), the music stream player's set-up 0x80020BEC (the sound
// runtime's: answered 0). OURS: a development start's names (game_state +0x30 / +0x40 not the product's set / race)
// are opened as the product's set / race's files (counted in the log).
//
// RRJB_STREAM_FILES=session is the negative control: the session's transcriptions (stream_session.cpp) and the
// release list where cell_view.cpp BuildRlsArena places it.
#include <cstdint>
#include <functional>
#include <string>

#include "game/sim/recover.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

bool StreamFilesPorted();

struct StreamFilesCounts {
    size_t setUps = 0, starts = 0, albums = 0, files = 0, flushes = 0, stpPlayers = 0, places = 0, opens = 0,
           opensFailed = 0, reads = 0, blocks = 0, waits = 0, fields = 0, musicSetUps = 0, renamed = 0, forwarded = 0;
    uint32_t heapFrom = 0, heapNext = 0, toc = 0, rls = 0, rlsTable = 0, rlsBytes = 0, str = 0;
    std::string names, refused;
};
StreamFilesCounts& StreamFilesTotals();
std::string StreamFilesLine();

// The host of the ported set-up / start: the callees above answered here, everything else forwarded to `rest`
// (the session's stream callees: the CD queue, the drive, the loaders' seams).
struct StreamFilesEnv {
    const DiscImage* disc = nullptr;
    int set = 1;                     // the product's road set (STREAM<set>, RACE<set>_)
    int32_t raceId = 1;              // the product's race
    rr::sim::RecoverCallees* rest = nullptr;
    std::function<void()> field;     // VSync(0): one field of the drive
    uint32_t heapFrom = 0, heapTo = 0; // the set-up's blocks (0: none handed over - refused)
};
// StreamSetUp SLUS 0x80022F78 PORTED with its children. False with `why` when refused.
bool StreamFilesSetUp(rr::sim::GuestRam& g, StreamFilesEnv& env, uint32_t sp, std::string& why);
// StreamStart SLUS 0x80023020 PORTED with its children.
bool StreamFilesStart(rr::sim::GuestRam& g, StreamFilesEnv& env, uint32_t sp, std::string& why);

} // namespace rr::game
