#pragma once
// Loads the world of one race: the road along the whole route, and the scene cells beside it.
//
// The pieces this is assembled from are all proven elsewhere - the race graph and the stream table
// of contents (docs\formats\road.md), the road slices inside type-3 chunks (docs\formats\road_chunk.md)
// and the scene cells (docs\formats\scene_cell.md). What this adds is the ordering: a race is a
// chain of legs, each leg is one road driven in one direction, and a road's chunks appear in its
// stream in distance order.
#include "rrformats/cell.h"
#include "rrformats/chunk.h"
#include "rrformats/road.h"
#include "rrvfs/disc_image.h"

#include <string>
#include <vector>

namespace rr {

// Diagnostic record of one road object that went into the path.
struct RoadObjectPlacement {
    int leg = 0;
    int road = 0;
    int direction = 0;
    uint32_t ownerRoad = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    size_t slices = 0;
    size_t runs = 0;
};

// Why a junction did or did not stitch. Diagnostics only.
struct JunctionReport {
    int node = -1;
    int roadFrom = -1;
    int roadTo = -1;
    int armIndex = -1;   // from the turn table, -1 if the turn is not listed
    size_t armCount = 0; // runs in the intersection core
    double directGap = 0.0;
    double bestSeam = 0.0;    // the WORST seam along the chosen chain of arms
    int bestArm = -1;         // arms in that chain, or -1 when nothing bridged
    double chainLength = 0.0; // how far the route actually travels through the junction
};

struct RaceWorld {
    int set = 0;
    int raceId = 0;
    std::vector<RouteLeg> legs;
    // The drivable centre line of the whole route, in route order. Slice positions are absolute
    // world coordinates, so legs simply concatenate; `distance` is REBASED to a running arc length
    // over this path rather than the per-road distance the file carries.
    std::vector<RoadSlice> path;
    std::vector<CellData> cells;
    // Per slice of `path`: which road it came from and how far along that road it sits, in world
    // units. `path[i].distance` is rebased to the whole route, so these keep what the file said -
    // and that is what the chunk residency windows are expressed in.
    std::vector<uint16_t> pathRoad;
    std::vector<uint16_t> pathRoadDistance;
    // 1 for the slices that came from a junction's arms rather than from a road piece. Those have no
    // distance along any road, so `pathRoadDistance` is only a carried-over placeholder there - and
    // anything that asks "where on the road am I" has to know the difference.
    std::vector<uint8_t> pathIsJunction;
    // Two independent figures for the same route, in world units: what the race graph declares, and
    // what our assembled centre line actually measures. Comparing them is the end-to-end check on
    // the whole assembly - legs, junction crossings and start/finish trimming together.
    double declaredLength = 0.0;
    double assembledLength = 0.0;
    // Set when the race's start or finish point lies past every slice we have for its road, i.e.
    // inside a junction fan. A fan carries no distance-along-a-road at all (road_chunk.md 2.8), so
    // such a point cannot be placed by road distance and the leg is dropped instead. Diagnostic,
    // because it is a real limit of the assembly rather than a failure of it.
    bool startInsideJunction = false;
    bool finishInsideJunction = false;
    size_t roadChunksRead = 0;
    size_t cellChunksRead = 0;
    size_t chunksScanned = 0;
    size_t junctionsStitched = 0;
    size_t junctionCoresFound = 0;
    size_t junctionsFromTurnTable = 0;
    std::vector<JunctionReport> junctionReports;
    std::vector<RoadObjectPlacement> placements;
};

// `set` is 1 or 2 (the two road sets), `raceId` the id used by ROADGRF<set>.TXT and RACE<set>_<id>.STP.
// `chunkBudget` caps how many stream chunks may be read, because a long route covers thousands of
// them; 0 means no cap.
RaceWorld LoadRaceWorld(const DiscImage& disc, int set, int raceId, size_t chunkBudget = 0);

} // namespace rr
