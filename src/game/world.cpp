#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace rr {
namespace {

std::vector<uint8_t> ReadDiscFile(const DiscImage& disc, const std::string& path) {
    const auto found = disc.Find(path);
    if (!found) throw std::runtime_error("world: not on disc: " + path);
    return disc.ReadFile(*found);
}

float Distance(const RoadSlice& a, const RoadSlice& b) {
    const float dx = WorldX(b) - WorldX(a);
    const float dy = WorldY(b) - WorldY(a);
    const float dz = WorldZ(b) - WorldZ(a);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------------------------
// Crossing a junction.
//
// A junction ships two objects and nine arms between them, and the route crosses it along a CHAIN
// of those arms, not a single one - see the comment at the call site for the measurement that
// showed this. Finding the chain is a small search: at most nine arms, either way round, and the
// route never needs more than a handful of them.

// One run of a junction object, with its polyline length precomputed.
struct JunctionArm {
    const RoadObject* object = nullptr;
    size_t first = 0;
    size_t count = 0;
    double length = 0.0;
};

using ChainStep = std::pair<size_t, bool>; // arm index, travelled in reverse

// `tail` picks which end of the arm: an arm travelled in reverse is entered at its last slice.
const RoadSlice& ArmEnd(const JunctionArm& arm, bool tail) {
    return arm.object->slices[tail ? arm.first + arm.count - 1 : arm.first];
}

// Depth-first over the arms, scoring a chain by its WORST seam and breaking ties on total length.
// Both parts matter: the worst seam is what a driver actually falls through, and preferring the
// shorter chain at equal cost is what stops the search wandering the long way round the junction.
struct ChainSearch {
    ChainSearch(const std::vector<JunctionArm>& armList, const RoadSlice& goalSlice, double seamLimit,
                size_t depth)
        : arms(armList), goal(goalSlice), used(armList.size(), 0), bestWorst(seamLimit), maxDepth(depth) {}

    void Step(const RoadSlice& at, double worst, double length) {
        if (chain.size() >= maxDepth) return;
        for (size_t a = 0; a < arms.size(); ++a) {
            if (used[a]) continue;
            for (int orientation = 0; orientation < 2; ++orientation) {
                const bool reversed = orientation != 0;
                const double seam = Distance(at, ArmEnd(arms[a], reversed));
                const double nextWorst = std::max(worst, seam);
                if (nextWorst > bestWorst) continue; // no continuation can beat what we have
                const RoadSlice& out = ArmEnd(arms[a], !reversed);
                const double close = Distance(out, goal);
                const double nextLength = length + seam + arms[a].length;
                const double total = std::max(nextWorst, close);
                used[a] = 1;
                chain.emplace_back(a, reversed);
                if (total < bestWorst || (total == bestWorst && !best.empty() && nextLength + close < bestLength)) {
                    bestWorst = total;
                    bestLength = nextLength + close;
                    best = chain;
                }
                Step(out, nextWorst, nextLength);
                chain.pop_back();
                used[a] = 0;
            }
        }
    }

    const std::vector<JunctionArm>& arms;
    const RoadSlice& goal;
    std::vector<char> used;
    std::vector<ChainStep> chain;
    std::vector<ChainStep> best;
    double bestWorst;
    double bestLength = 0.0;
    size_t maxDepth;
};

} // namespace

RaceWorld LoadRaceWorld(const DiscImage& disc, int set, int raceId, size_t chunkBudget) {
    RaceWorld world;
    world.set = set;
    world.raceId = raceId;

    const std::string suffix = std::to_string(set);
    const std::vector<uint8_t> textBytes = ReadDiscFile(disc, "DATA/ROADGRF" + suffix + ".TXT");
    const RaceGraph graph = ParseRaceGraph(
        std::string_view(reinterpret_cast<const char*>(textBytes.data()), textBytes.size()));
    const RoadNetwork network = ParseRoadNetwork(ReadDiscFile(disc, "DATA/STREAM" + suffix + ".GRF"));
    const StreamToc toc = ParseStreamToc(ReadDiscFile(disc, "DATA/STREAM" + suffix + ".TOC"));
    const RoadMap roadMap = ParseRoadMap(ReadDiscFile(disc, "DATA/ROAD" + suffix + ".MAP"));
    const auto stream = disc.Find("DATA/STREAM" + suffix + ".STR");
    if (!stream) throw std::runtime_error("world: STREAM" + suffix + ".STR is not on disc");

    const Race* race = nullptr;
    for (const Race& candidate : graph.races)
        if (candidate.raceId == raceId) race = &candidate;
    if (!race) throw std::runtime_error("world: no race " + std::to_string(raceId) + " in set " + suffix);
    world.legs = BuildRouteLegs(*race, network);

    std::set<uint32_t> seenRoad, seenCell;
    // Every object that is not part of a leg's own road, keyed by its GRPT owner. An intersection
    // owns TWO objects (road_chunk.md: 63 road pieces + 2 per intersection = 113), so a map that
    // keeps one per key loses half the junction.
    std::multimap<uint32_t, RoadObject> junctions;
    std::vector<std::vector<RoadSlice>> legPaths;
    std::vector<std::vector<uint16_t>> legRoadIdsPerLeg, legRoadDistancesPerLeg;
    std::vector<uint8_t> chunk(kChunkSize);

    for (const RouteLeg& leg : world.legs) {
        // BOTH streams of the road, not just the one for the direction of travel. Measured: road 28
        // has two objects in its forward stream and three in its reverse one, and the extra object is
        // exactly the stretch that runs up to the junction. Reading one stream per leg left a 590 to
        // 1003 unit hole at every junction, which no junction arm could bridge - because the missing
        // road was never loaded, not because the arm was chosen wrongly.
        const StreamRange ranges[2] = {StreamRangeFor(toc, leg.road, +1), StreamRangeFor(toc, leg.road, -1)};
        std::vector<RoadObject> legRoads;

        for (const StreamRange& range : ranges)
        for (uint32_t offset = 0; offset < range.size; offset += kChunkSize) {
            if (chunkBudget && world.chunksScanned >= chunkBudget) break;
            ++world.chunksScanned;
            disc.ReadForm1(stream->lba, range.offset + offset, chunk.data(), chunk.size());
            const ChunkHeader header = ParseChunkHeader(chunk);
            if (header.type == static_cast<uint8_t>(ChunkType::Road)) {
                // A road's stream carries whatever has to be resident while driving it, which
                // includes the junctions at its ends and pieces of the roads beyond them. Only the
                // objects whose residency window names THIS road belong to this leg; without that
                // filter the legs interleave and the assembled centre line jumps across the map.
                bool onThisLeg = false;
                for (const ResidencyWindow& window : header.windows)
                    if (window.road == static_cast<uint16_t>(leg.road)) onThisLeg = true;
                if (!onThisLeg) continue;
                if (!seenRoad.insert(header.id).second) continue; // a resource repeats in the stream
                legRoads.push_back(ParseRoadChunk(chunk));
                ++world.roadChunksRead;
            } else if (header.type == 0 || header.type == 8) {
                if (!seenCell.insert(header.id).second) continue;
                world.cells.push_back(ParseCellChunk(chunk));
                ++world.cellChunksRead;
            }
        }

        // The objects of a road tile its parameter space end to start, and GRPT says where each one
        // sits. Ordering by `start` is therefore the road's own order - not a guess - and driving
        // the road backwards walks it in reverse.
        //
        // Intersection cores are set aside rather than thrown away: they hold the junction's several
        // arms, and one of those arms is the bit of tarmac that carries the route from this leg to
        // the next. Without them the assembled path has exactly one gap per junction.
        // File a junction by the NODE in its chunk header (+0x28), not by its GRPT owner. Only one of
        // a junction's two objects carries the node in GRPT; the other names one of the roads that
        // meet there, so keying on GRPT filed that half under a road id and the stitcher never saw
        // it. That half holds the two stubs that reach the roads the route joins.
        for (const RoadObject& object : legRoads)
            if (object.IsJunctionPart()) junctions.emplace(object.headerOwner, object);
        legRoads.erase(std::remove_if(legRoads.begin(), legRoads.end(),
                                      [&](const RoadObject& object) {
                                          return object.IsJunctionPart() ||
                                                 object.ownerRoad != static_cast<uint32_t>(leg.road);
                                      }),
                       legRoads.end());
        std::sort(legRoads.begin(), legRoads.end(), [](const RoadObject& a, const RoadObject& b) {
            return a.startAlongRoad < b.startAlongRoad;
        });
        if (leg.direction < 0) std::reverse(legRoads.begin(), legRoads.end());

        std::vector<RoadSlice> legPath;
        std::vector<uint16_t> legRoadIds, legRoadDistances;
        for (const RoadObject& object : legRoads) {
            RoadObjectPlacement placement;
            placement.leg = static_cast<int>(&leg - world.legs.data());
            placement.road = leg.road;
            placement.direction = leg.direction;
            placement.ownerRoad = object.ownerRoad;
            placement.start = object.startAlongRoad;
            placement.end = object.endAlongRoad;
            placement.slices = object.slices.size();
            placement.runs = object.runs.size();
            world.placements.push_back(placement);

            // Most road-piece objects hold a single run and all of it is road. The objects next to a
            // junction hold several disjoint arms in one slice array, and only one of them continues
            // the road we are driving. Pick that one geometrically: the arm whose nearer end is
            // closest to where the path has reached. This is our rule, not the original's - the
            // original resolves arms through the junction tables - but it is checkable, and
            // `rrtool raceworld` reports any jump it fails to remove.
            std::vector<RoadSlice> run;
            if (object.runs.size() <= 1) {
                run = object.slices;
            } else {
                double best = 0.0;
                bool haveBest = false;
                for (const SliceRun& candidate : object.runs) {
                    if (candidate.count < 2) continue;
                    const RoadSlice& first = object.slices[candidate.first];
                    const RoadSlice& last = object.slices[candidate.first + candidate.count - 1];
                    double score = 0.0;
                    if (legPath.empty()) {
                        score = -static_cast<double>(candidate.count); // nothing to join yet: take the longest
                    } else {
                        const RoadSlice& tail = legPath.back();
                        score = std::min(Distance(tail, first), Distance(tail, last));
                    }
                    if (!haveBest || score < best) {
                        best = score;
                        haveBest = true;
                        run.assign(object.slices.begin() + static_cast<ptrdiff_t>(candidate.first),
                                   object.slices.begin() + static_cast<ptrdiff_t>(candidate.first + candidate.count));
                    }
                }
            }
            if (run.size() < 2) continue;
            if (leg.direction < 0) {
                std::reverse(run.begin(), run.end());
                // Travelling the other way means the tangent points the other way; row 2 of the
                // slice matrix is that tangent.
                for (RoadSlice& slice : run)
                    for (size_t k = 6; k < 9; ++k) slice.m[k] = static_cast<int16_t>(-slice.m[k]);
            }
            legPath.insert(legPath.end(), run.begin(), run.end());
            for (const RoadSlice& slice : run) {
                legRoadIds.push_back(static_cast<uint16_t>(leg.road));
                legRoadDistances.push_back(static_cast<uint16_t>(slice.distance >> 16));
            }
        }
        // Trim the first and last legs to where the race itself begins and ends. A leg is a whole
        // road, but a race starts and finishes PART WAY along one: `[START]` and `[FINISH]` of
        // ROADGRF give those two points as a distance along the road, in world units
        // (road_chunk.md 2.5). Untrimmed, the assembled route is the roads' full length instead of
        // the race's, and the start line sits wherever the road happens to begin.
        const size_t legIndex = static_cast<size_t>(&leg - world.legs.data());
        const bool isFirst = legIndex == 0;
        const bool isLast = legIndex + 1 == world.legs.size();
        if ((isFirst || isLast) && !legPath.empty()) {
            size_t from = 0, to = legPath.size();
            if (isFirst && leg.road == race->start.road) {
                const int32_t at = race->start.distance;
                while (from < to && (leg.direction > 0 ? legRoadDistances[from] < at
                                                       : legRoadDistances[from] > at))
                    ++from;
                // Running off the end means the start point sits PAST every slice we have for this
                // road - it is inside the junction fan, which road_chunk.md 2.8 shows has no 1-D
                // parameter at all (the core object's GRPT start and end are both 0). Then the
                // right answer is that none of this leg is raced, not that all of it is.
                if (from == to) world.startInsideJunction = true;
            }
            if (isLast && leg.road == race->finish.road) {
                const int32_t at = race->finish.distance;
                while (to > from && (leg.direction > 0 ? legRoadDistances[to - 1] > at
                                                       : legRoadDistances[to - 1] < at))
                    --to;
                if (to == from) world.finishInsideJunction = true;
            }
            if (from > 0 || to < legPath.size()) {
                legPath = std::vector<RoadSlice>(legPath.begin() + static_cast<ptrdiff_t>(from),
                                                 legPath.begin() + static_cast<ptrdiff_t>(to));
                legRoadIds = std::vector<uint16_t>(legRoadIds.begin() + static_cast<ptrdiff_t>(from),
                                                   legRoadIds.begin() + static_cast<ptrdiff_t>(to));
                legRoadDistances =
                    std::vector<uint16_t>(legRoadDistances.begin() + static_cast<ptrdiff_t>(from),
                                          legRoadDistances.begin() + static_cast<ptrdiff_t>(to));
            }
        }

        legPaths.push_back(std::move(legPath));
        legRoadIdsPerLeg.push_back(std::move(legRoadIds));
        legRoadDistancesPerLeg.push_back(std::move(legRoadDistances));
    }

    // Stitch the legs together through the junctions. Between two legs the route crosses a node, and
    // the intersection core for that node holds one arm of tarmac that runs from the incoming road to
    // the outgoing one. Choose it by how well it bridges the two ends - the original resolves this
    // through its junction tables, which we have not ported, so this is our rule and it is measured
    // rather than assumed (`rrtool raceworld` prints every jump that survives).
    world.junctionCoresFound = junctions.size();
    for (size_t i = 0; i < legPaths.size(); ++i) {
        if (legPaths[i].empty()) continue;
        if (!world.path.empty()) {
            const RoadSlice& tail = world.path.back();
            const RoadSlice& head = legPaths[i].front();
            const double direct = Distance(tail, head);
            const uint32_t node = static_cast<uint32_t>(world.legs[i - 1].toNode);
            const auto range = junctions.equal_range(node);
            if (direct > 30.0 && range.first != range.second) {
                JunctionReport report;
                report.node = world.legs[i - 1].toNode;
                report.roadFrom = world.legs[i - 1].road;
                report.roadTo = world.legs[i].road;
                report.armIndex = roadMap.ArmIndexFor(report.node, report.roadFrom, report.roadTo);
                report.directGap = direct;
                report.bestSeam = 1e18;

                // A junction is not crossed by ONE arm. Measured on node 16 of set 1 race 20: the
                // route leaves road 28, runs 239 units up a stub of the half-1 object, 257 units up
                // an arm of the core to the junction centre, then back down a second core arm (251)
                // and a second stub (254) to reach road 21 - four arms, about 1000 units, against a
                // straight-line gap of 918. That is why every single-arm search stalled at a seam of
                // roughly half the gap: no one arm spans a junction, and searching harder for one
                // could not have worked.
                //
                // So search for a CHAIN. The cost of a chain is the WORST seam along it, never the
                // sum: scoring by the sum lets a chain in that turns one large gap into several
                // medium ones, which is worse, not better (measured earlier, it took the jump count
                // from 8 up to 13). Ties go to the shorter chain, which is what keeps the search from
                // taking the long way round the junction.
                constexpr double kSeamLimit = 60.0;
                constexpr size_t kMaxChain = 6;

                std::vector<JunctionArm> arms;
                for (auto it = range.first; it != range.second; ++it)
                    for (const SliceRun& run : it->second.runs) {
                        if (run.count < 2) continue;
                        JunctionArm arm;
                        arm.object = &it->second;
                        arm.first = run.first;
                        arm.count = run.count;
                        for (size_t k = 1; k < run.count; ++k)
                            arm.length += Distance(it->second.slices[run.first + k - 1],
                                                   it->second.slices[run.first + k]);
                        arms.push_back(arm);
                    }
                report.armCount = arms.size();

                ChainSearch search(arms, head, kSeamLimit, kMaxChain);
                search.Step(tail, 0.0, 0.0);
                const std::vector<ChainStep>& bestChain = search.best;

                std::vector<RoadSlice> bestArm;
                for (const ChainStep& step : bestChain) {
                    const JunctionArm& arm = arms[step.first];
                    std::vector<RoadSlice> piece(
                        arm.object->slices.begin() + static_cast<ptrdiff_t>(arm.first),
                        arm.object->slices.begin() + static_cast<ptrdiff_t>(arm.first + arm.count));
                    if (step.second) {
                        std::reverse(piece.begin(), piece.end());
                        // Travelling the arm the other way means the tangent points the other way;
                        // row 2 of the slice matrix is that tangent.
                        for (RoadSlice& slice : piece)
                            for (size_t k = 6; k < 9; ++k) slice.m[k] = static_cast<int16_t>(-slice.m[k]);
                    }
                    bestArm.insert(bestArm.end(), piece.begin(), piece.end());
                }
                report.bestSeam = bestChain.empty() ? direct : search.bestWorst;
                report.bestArm = bestChain.empty() ? -1 : static_cast<int>(bestChain.size());
                report.chainLength = bestChain.empty() ? 0.0 : search.bestLength;
                world.junctionReports.push_back(report);
                if (!bestArm.empty()) {
                    world.path.insert(world.path.end(), bestArm.begin(), bestArm.end());
                    // A junction arm belongs to no road; attribute it to the road being left, which
                    // is what its residency window would name.
                    for (size_t k = 0; k < bestArm.size(); ++k) {
                        world.pathRoad.push_back(static_cast<uint16_t>(world.legs[i - 1].road));
                        world.pathRoadDistance.push_back(world.pathRoadDistance.empty()
                                                             ? 0
                                                             : world.pathRoadDistance.back());
                        world.pathIsJunction.push_back(1);
                    }
                    ++world.junctionsStitched;
                }
            }
        }
        world.path.insert(world.path.end(), legPaths[i].begin(), legPaths[i].end());
        world.pathRoad.insert(world.pathRoad.end(), legRoadIdsPerLeg[i].begin(), legRoadIdsPerLeg[i].end());
        world.pathRoadDistance.insert(world.pathRoadDistance.end(), legRoadDistancesPerLeg[i].begin(),
                                      legRoadDistancesPerLeg[i].end());
        world.pathIsJunction.insert(world.pathIsJunction.end(), legPaths[i].size(), 0);
    }

    // Rebase `distance` as the running arc length of the assembled path, so a driver can index it
    // with one number regardless of which road a slice came from.
    double running = 0.0;
    for (size_t i = 0; i < world.path.size(); ++i) {
        if (i > 0) running += Distance(world.path[i - 1], world.path[i]);
        world.path[i].distance = static_cast<uint32_t>(running * 65536.0);
    }
    world.assembledLength = running;

    // What the race graph declares for the same route, to compare that against. `distToFinish` of
    // the first node on the route already covers everything beyond it, so the only other term is the
    // run-in: the part of the start road before that node. Both are world units (road_chunk.md 2.5).
    const Intersection* startRow = nullptr;
    for (const Intersection& row : race->intersections)
        if (row.node == race->start.node) startRow = &row;
    if (startRow) {
        int32_t roadLength = 0;
        for (const NetworkRoad& road : network.roads)
            if (road.id == race->start.road) roadLength = road.length >> 6;
        const int32_t runIn =
            race->start.direction > 0 ? roadLength - race->start.distance : race->start.distance;
        world.declaredLength = static_cast<double>(runIn) + static_cast<double>(startRow->distToFinish);
    } else {
        world.declaredLength = std::abs(static_cast<double>(race->finish.distance - race->start.distance));
    }
    return world;
}

} // namespace rr
