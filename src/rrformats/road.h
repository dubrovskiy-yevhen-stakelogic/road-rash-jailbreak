#pragma once
// The road network of Road Rash: Jailbreak - the race graph (`ROADGRF<n>.TXT`), the node/road
// network (`RGTS` in `STREAM<n>.GRF`) and the stream table of contents (`COTS` in `STREAM<n>.TOC`).
// Layouts proven in docs\formats\road.md; this is the authoritative parser (the Python probe
// tools\scout\road.py stays as an independent cross-check).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rr {

// ------------------------------------------------------------------ ROADGRF<n>.TXT

// One junction on (or off) a race route. Slots 0..3 of `link` are the roads meeting this node.
//
// NOTE, do not "fix" this to match the original's memory image: the game's parser PACKS its arrays
// (`0x8006A6D0` appends `routeRoad`, and running the original in our interpreter showed `0x8006A72C`
// appends `nextNode` too - which contradicts docs\formats\road.md section 1.2). We instead keep every array at its TEXT slot.
// That is safe here, and it is checked rather than assumed: over all 441 intersection rows of
// ROADGRF1/2 the route road sits at the same slot as its own link (0 misaligned) AND at the same
// slot as its next node (0 different), so slot i of `routeRoad`, `nextNode` and `link` describe the
// same road either way. The route walk closes for all 100 races, which is the behavioural proof.
struct Intersection {
    int32_t node = 0;
    int32_t distToFinish = 0; // road units >> 6; 0x7FFFFFFF marks a row that is not on the route
    int32_t nodeSpan = 0;
    std::array<int32_t, 4> linkRoad{-1, -1, -1, -1};
    std::array<int32_t, 4> linkDir{0, 0, 0, 0};
    std::array<int32_t, 4> routeRoad{-1, -1, -1, -1};
    std::array<int32_t, 4> nextNode{-1, -1, -1, -1};
};

struct RaceEndpoint {
    int32_t road = 0;
    int32_t distance = 0; // along `road`, road units >> 6
    int32_t direction = 0; // +1 = nodeA -> nodeB
    int32_t node = -1;
};

struct Race {
    int32_t raceId = 0;
    RaceEndpoint start;
    RaceEndpoint finish;
    uint32_t gmagic = 0;
    uint32_t rmagic = 0;
    std::array<int32_t, 3> startChecker{-1, -1, -1}; // meaning unknown
    std::array<int32_t, 3> finishChecker{-1, -1, -1};
    std::vector<Intersection> intersections;
};

struct RaceGraph {
    int32_t declaredEntries = 0;
    std::vector<Race> races;
};

RaceGraph ParseRaceGraph(std::string_view text);

// ------------------------------------------------------------------ RGTS (STREAM<n>.GRF)

struct NetworkNode {
    int32_t id = 0;
    int32_t linkCount = 0;
    std::array<int32_t, 4> linkRoad{};
    std::array<int32_t, 4> linkDir{}; // outgoing: +1 when this node is road.nodeA
};

struct NetworkRoad {
    int32_t id = 0;
    int32_t length = 0; // the text graph uses length >> 6
    int32_t nodeA = 0;
    int32_t nodeB = 0;
};

struct RoadNetwork {
    uint32_t gmagic = 0;
    std::vector<NetworkNode> nodes;
    std::vector<NetworkRoad> roads;
};

RoadNetwork ParseRoadNetwork(std::span<const uint8_t> grf);

// ------------------------------------------------------------------ COTS (STREAM<n>.TOC)

// A road is streamed as two separate byte ranges of STREAM<n>.STR, one per driving direction.
// Every offset and size is a multiple of the 0x4000 chunk.
struct StreamRoad {
    int32_t road = 0;
    uint32_t forwardOffset = 0;
    uint32_t forwardSize = 0;
    uint32_t reverseOffset = 0;
    uint32_t reverseSize = 0;
};

struct StreamToc {
    uint32_t gmagic = 0;
    std::vector<StreamRoad> roads;
};

StreamToc ParseStreamToc(std::span<const uint8_t> toc);

// ------------------------------------------------------------------ derived

// Walks a race route: [START].node -> routeRoad -> nextNode -> ... -> [FINISH].node.
// Returns the visited node ids. Throws if the chain does not close, which is a real failure -
// the invariant holds for all 100 shipped races.
std::vector<int32_t> WalkRoute(const Race& race);

// Re-checks the proven distance identity
//   distToFinish(n) == nodeSpan(n) + (road.length >> 6) + distToFinish(next)
// with the finish node using the run-out to [FINISH].distance. Returns the number of rows checked
// and fills `failures` with a description of each mismatch.
size_t CheckRouteDistances(const Race& race, const RoadNetwork& network, std::vector<std::string>& failures);

// One stretch of the race: a road driven in one direction between two junctions.
struct RouteLeg {
    int32_t road = -1;
    int32_t direction = 0; // +1 = nodeA -> nodeB
    int32_t fromNode = -1;
    int32_t toNode = -1;
};

// The ordered roads of a race, from [START] to [FINISH]. A race with no intersections is a single
// leg on the start road.
std::vector<RouteLeg> BuildRouteLegs(const Race& race, const RoadNetwork& network);

// The byte range of STREAM<n>.STR that holds one leg. Chunks are 0x4000 bytes, so the leg occupies
// `size / 0x4000` consecutive chunks starting at `offset / 0x4000`.
struct StreamRange {
    uint32_t offset = 0;
    uint32_t size = 0;
};

StreamRange StreamRangeFor(const StreamToc& toc, int32_t road, int32_t direction);

// ------------------------------------------------------------------ MAP_ (ROAD<n>.MAP)

// One ordered pair of roads meeting at an intersection - "coming in on `roadFrom`, leaving on
// `roadTo`". Layout from docs\formats\road.md section 7, which listed the two middle fields as
// unknown. They are not:
//   * `pairId` identifies the UNORDERED road pair. Measured over all 146 turn rows of set 1: the two
//     rows of a pair always carry the same value, 0 inconsistencies, and the values form a dense
//     space 0..72 - exactly the 73 records of the `GPDT` block.
//   * `GPDT[pairId]` halfword 2 is then the LOCAL ARM INDEX at that junction. Measured on both road
//     sets, all 50 nodes: the arm indices of a node's distinct pair ids are exactly 0..k-1, with
//     0 exceptions.
//   * `always1` is 1 in every row of both sets.
// So a turn resolves to "take arm N of this junction, travelling in `direction`".
struct TurnEntry {
    uint16_t pairIndex = 0; // index of this ordered pair within the node
    uint16_t pairId = 0;    // unordered pair id, indexes GPDT
    uint16_t always1 = 0;
    int16_t direction = 0;
    uint16_t roadFrom = 0;
    uint16_t roadTo = 0;
};

struct Intersection2 {
    uint16_t bttSeqBase = 0;
    uint16_t node = 0;
    uint16_t linkCount = 0;
    uint16_t turnBase = 0;  // first TurnEntry of this node
    uint16_t turnCount = 0; // linkCount * (linkCount - 1)
};

struct RoadMap {
    uint32_t gmagic = 0;
    std::vector<Intersection2> intersections;
    std::vector<TurnEntry> turns;
    std::vector<uint16_t> pairArmIndex; // GPDT halfword 2 per pair id: the local arm index

    // The turn that carries a route from `roadFrom` to `roadTo` at `node`, or nullptr.
    const TurnEntry* FindTurn(int32_t node, int32_t roadFrom, int32_t roadTo) const;
    // The junction arm that turn selects, or -1 when the turn is not in the table.
    int ArmIndexFor(int32_t node, int32_t roadFrom, int32_t roadTo) const;
};

RoadMap ParseRoadMap(std::span<const uint8_t> map);

} // namespace rr
