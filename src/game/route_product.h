#pragma once
// The ported route loaders in the product: the race's route arena built by the original's
// own loaders, PORTED - SetUpRace's GrfLoad SLUS 0x800244E0, RoadLoad RASHCDI 0x8006AC6C (RoadText -> the route
// block parser 0x8006A0C8, RoadClear, RoadRecords -> the road-map loader 0x8006ABC8), StreamSetUp SLUS 0x80022F78
// and StreamStart SLUS 0x80023020 - on the session's arena, in the original's order after the stream resource table.
//
// The host's, named: the CD file layer (open / size / read / close / LoadFile: the disc image), the malloc (SLUS
// 0x8001447C's block rule over a bump region from rr-race's first route block 0x801A7BC8: the race graph, the route
// block and the map land where rr-race has them), sprintf (%s / %d), the kernel's string calls (route_parse.h Bios*,
// benched). A direct start (no front end commit) runs with game_state +0x30 / +0x40 set to the product's road set and
// race for the call (OURS, named in the race log).
//
// RRJB_ROUTE=ours is the negative control: the session's transcription of the parser's rules (race_session.cpp
// BuildRouteBlock) and the host's ROAD<n>.MAP loader stand.
#include <cstdint>
#include <functional>
#include <string>

#include "game/sim/route_parse.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

bool RoutePorted();

struct RouteCounts {
    size_t loads = 0, ported = 0, bios = 0, forwarded = 0, opens = 0, reads = 0, blocks = 0, loadFiles = 0,
           setUps = 0, starts = 0, places = 0, perPlayer = 0;
    uint32_t grf = 0, alloc = 0, map = 0, graph = 0, text = 0, textLen = 0;
    uint32_t heapNext = 0, heapTo = 0; // the heap's next block after the loaders (the stream's files follow)
    int32_t raceInts = -2;
    size_t routeDiffer = 0, headerDiffer = 0, graphDiffer = 0, mapDiffer = 0, compared = 0; // against the transcription
    std::string note, refused;
};
RouteCounts& RouteTotals();
std::string RouteLine();

// GrfLoad and RoadLoad PORTED on the arena for road set `set`, race `raceId`. False with `why` when refused.
bool RouteLoadPorted(rr::sim::GuestRam& g, const DiscImage& disc, int set, int32_t raceId, uint32_t sp, std::string& why);
// StreamSetUp SLUS 0x80022F78 PORTED: 0x80023498 (the stream's files) is `files` (the session's), the album
// 0x80024630 the sound runtime's (DEFERRED, named), 0x80022A1C(2) the CD flush (CdReset 0x80022CC4 PORTED).
bool RouteStreamSetUp(rr::sim::GuestRam& g, uint32_t sp, const std::function<void()>& files, std::string& why);
// StreamStart SLUS 0x80023020 PORTED: 0x800235C8 is StreamPlace (PORTED, stream.h), 0x80023714 `perPlayer` (the
// session's per-player .STP start).
bool RouteStreamStart(rr::sim::GuestRam& g, uint32_t sp, const std::function<void()>& perPlayer, std::string& why);

} // namespace rr::game
