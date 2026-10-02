#pragma once
// The streamer in the product (stream_session.cpp): the original's streamer PORTED
// (src\game\sim\stream.h) runs in the session; RRJB_STREAM=ours is the negative control (the session's own
// residency rules RoadStreamPass / CellStreamPass).
#include <cstdint>
#include <string>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

// False with RRJB_STREAM=ours.
bool StreamPorted();
// The per-frame census of what is resident (the piece list, player 0's cell slots, the table's records in use),
// both modes; `bike` is player 1's bike (its road coordinate goes into the series).
void StreamSample(rr::sim::GuestRam& g, uint32_t frame, uint32_t bike);
// The run's counter line and series for the race log.
std::string StreamTotals();
// The streamer's switches (stream_session.cpp header): the CD layer and the drive's time (RRJB_CD=instant: the
// read done when queued), the sky pick (RRJB_SKY=ours: the renderer's nearest-window rule), the cells' texture
// binding and readiness (RRJB_CELLREADY=off), type-10 banks into the speech slots (RRJB_STREAM_SPEECH=off). All
// false with RRJB_STREAM=ours.
bool StreamCdTimed();
bool StreamSkyPorted();
bool StreamCellReadyPorted();
bool StreamSpeechPorted();
// The sky's pick RASHCDG 0x800650D0 (view 0) in the frame's draw; `frame` for the log's series.
void StreamSkyPass(rr::sim::GuestRam& g, uint32_t frame);
// The panorama the sky shows (slot +0x12's key, its id without the type nibble); false when none / not ported.
bool StreamSkyShown(rr::sim::GuestRam& g, uint32_t& id);
// rrgame --streamcheck <dir>: the console's own residency against the PORTED streamer. The first RAM image of
// the directory (ram_*.bin, a race capture sequence) becomes the arena; for each next image the stream
// record's entity takes that image's road coordinate and the streamer runs the frames between the two (the
// race clock's ticks / 5); then the resident sets are compared with the image's: the piece list, player 0's
// cell slots, the table's loaded keys, the texture slots, the panorama slots. The CD is the ported layer with the 2x drive
// (a vertical blank of +0x0C per replayed frame; RRJB_CD=instant: at once), so a set may run AHEAD of the console's (a superset) while the console's reads are in flight;
// the check requires that every key the console holds is held, and reports the extra ones. `mutate`: the
// release pass is skipped (records never freed) - must FAIL.
bool CheckStream(const rr::DiscImage& disc, const std::string& dir, std::string& report, bool mutate);

} // namespace rr::game
