#include "rrformats/road.h"

#include <charconv>
#include <cstring>
#include <stdexcept>

namespace rr {
namespace {

constexpr int32_t kNotOnRoute = 0x7FFFFFFF;

uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("road: read past end of file");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

int32_t ReadS32(std::span<const uint8_t> d, size_t off) {
    return static_cast<int32_t>(ReadU32(d, off));
}

uint16_t ReadU16(std::span<const uint8_t> d, size_t off) {
    if (off + 2 > d.size()) throw std::runtime_error("road: read past end of file");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

void Expect(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(std::string("road: ") + what);
}

bool TagIs(std::span<const uint8_t> d, size_t off, const char* tag) {
    return off + 4 <= d.size() && std::memcmp(d.data() + off, tag, 4) == 0;
}

// The original parser is line based: it matches the bracketed key with strncmp, takes the value
// after the first '=', and then reads further ints separated by spaces.
std::vector<int32_t> ParseInts(std::string_view s) {
    std::vector<int32_t> out;
    size_t at = 0;
    while (at < s.size()) {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r')) ++at;
        if (at >= s.size()) break;
        const size_t begin = at;
        if (s[at] == '-' || s[at] == '+') ++at;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
        if (at == begin) { // not a number - skip the token
            while (at < s.size() && s[at] != ' ') ++at;
            continue;
        }
        int32_t value = 0;
        const std::from_chars_result r = std::from_chars(s.data() + begin, s.data() + at, value);
        if (r.ec == std::errc()) out.push_back(value);
    }
    return out;
}

std::string_view TrimLine(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    return line;
}

bool KeyIs(std::string_view line, const char* key) {
    const size_t n = std::strlen(key);
    return line.size() >= n && line.compare(0, n, key) == 0;
}

std::string_view ValuePart(std::string_view line) {
    const size_t eq = line.find('=');
    return eq == std::string_view::npos ? std::string_view{} : line.substr(eq + 1);
}

RaceEndpoint MakeEndpoint(const std::vector<int32_t>& v) {
    RaceEndpoint e;
    if (v.size() > 0) e.road = v[0];
    if (v.size() > 1) e.distance = v[1];
    if (v.size() > 2) e.direction = v[2];
    if (v.size() > 3) e.node = v[3];
    return e;
}

} // namespace

RaceGraph ParseRaceGraph(std::string_view text) {
    RaceGraph graph;
    Race current;
    bool inBlock = false;
    size_t remainingIntersections = 0;

    size_t at = 0;
    while (at <= text.size()) {
        const size_t end = text.find('\n', at);
        const std::string_view line = TrimLine(text.substr(at, (end == std::string_view::npos ? text.size() : end) - at));
        at = (end == std::string_view::npos) ? text.size() + 1 : end + 1;
        if (line.empty()) continue;

        if (remainingIntersections > 0 && !KeyIs(line, "[")) {
            const std::vector<int32_t> v = ParseInts(line);
            Expect(v.size() == 19, "intersection row does not have 19 integers");
            Intersection x;
            x.node = v[0];
            x.distToFinish = v[1];
            x.nodeSpan = v[2];
            for (size_t i = 0; i < 4; ++i) {
                x.linkRoad[i] = v[3 + i * 2];
                x.linkDir[i] = v[4 + i * 2];
                x.routeRoad[i] = v[11 + i];
                x.nextNode[i] = v[15 + i];
            }
            current.intersections.push_back(x);
            --remainingIntersections;
            continue;
        }

        if (KeyIs(line, "[NUM_ENTRIES]")) {
            const std::vector<int32_t> v = ParseInts(ValuePart(line));
            if (!v.empty()) graph.declaredEntries = v[0];
        } else if (KeyIs(line, "[BEGIN]")) {
            current = Race{};
            inBlock = true;
        } else if (KeyIs(line, "[END]")) {
            Expect(inBlock, "[END] without [BEGIN]");
            graph.races.push_back(std::move(current));
            current = Race{};
            inBlock = false;
        } else if (KeyIs(line, "[RACEID]")) {
            const std::vector<int32_t> v = ParseInts(ValuePart(line));
            if (!v.empty()) current.raceId = v[0];
        } else if (KeyIs(line, "[START_CHECKER]")) {
            const std::vector<int32_t> v = ParseInts(ValuePart(line));
            for (size_t i = 0; i < 3 && i < v.size(); ++i) current.startChecker[i] = v[i];
        } else if (KeyIs(line, "[FINISH_CHECKER]")) {
            const std::vector<int32_t> v = ParseInts(ValuePart(line));
            for (size_t i = 0; i < 3 && i < v.size(); ++i) current.finishChecker[i] = v[i];
        } else if (KeyIs(line, "[START]")) {
            current.start = MakeEndpoint(ParseInts(ValuePart(line)));
        } else if (KeyIs(line, "[FINISH]")) {
            current.finish = MakeEndpoint(ParseInts(ValuePart(line)));
        } else if (KeyIs(line, "[GMAGIC]")) {
            const std::vector<int32_t> v = ParseInts(ValuePart(line));
            if (!v.empty()) current.gmagic = static_cast<uint32_t>(v[0]);
        } else if (KeyIs(line, "[RMAGIC]")) {
            const std::vector<int32_t> v = ParseInts(ValuePart(line));
            if (!v.empty()) current.rmagic = static_cast<uint32_t>(v[0]);
        } else if (KeyIs(line, "[RACEINTS]")) {
            const std::vector<int32_t> v = ParseInts(ValuePart(line));
            Expect(!v.empty() && v[0] >= 0, "[RACEINTS] without a count");
            remainingIntersections = static_cast<size_t>(v[0]);
            current.intersections.reserve(remainingIntersections);
        }
    }
    Expect(!inBlock, "file ends inside a [BEGIN] block");
    Expect(remainingIntersections == 0, "file ends with intersection rows still expected");
    return graph;
}

RoadNetwork ParseRoadNetwork(std::span<const uint8_t> grf) {
    Expect(TagIs(grf, 0, "RGTS"), "STREAM<n>.GRF does not start with RGTS");
    RoadNetwork net;
    const uint32_t blockSize = ReadU32(grf, 0x04);
    net.gmagic = ReadU32(grf, 0x08);
    const uint32_t nodeCount = ReadU32(grf, 0x0C);
    const uint32_t roadCount = ReadU32(grf, 0x10);
    const uint32_t nodeOffset = ReadU32(grf, 0x14);
    const uint32_t roadOffset = ReadU32(grf, 0x18);
    Expect(nodeOffset + nodeCount * 40 == roadOffset, "node array does not end where the road array begins");
    Expect(roadOffset + roadCount * 16 == blockSize, "road array does not end at the block size");

    net.nodes.reserve(nodeCount);
    for (uint32_t i = 0; i < nodeCount; ++i) {
        const size_t o = nodeOffset + i * 40;
        NetworkNode n;
        n.id = ReadS32(grf, o + 0x00);
        n.linkCount = ReadS32(grf, o + 0x04);
        Expect(n.id == static_cast<int32_t>(i), "node id is not its index");
        Expect(n.linkCount >= 0 && n.linkCount <= 4, "node link count out of range");
        for (size_t k = 0; k < 4; ++k) {
            n.linkRoad[k] = ReadS32(grf, o + 0x08 + k * 8);
            n.linkDir[k] = ReadS32(grf, o + 0x0C + k * 8);
        }
        net.nodes.push_back(n);
    }
    net.roads.reserve(roadCount);
    for (uint32_t i = 0; i < roadCount; ++i) {
        const size_t o = roadOffset + i * 16;
        NetworkRoad r;
        r.id = ReadS32(grf, o + 0x00);
        r.length = ReadS32(grf, o + 0x04);
        r.nodeA = ReadS32(grf, o + 0x08);
        r.nodeB = ReadS32(grf, o + 0x0C);
        Expect(r.id == static_cast<int32_t>(i), "road id is not its index");
        net.roads.push_back(r);
    }
    return net;
}

StreamToc ParseStreamToc(std::span<const uint8_t> toc) {
    Expect(TagIs(toc, 0, "COTS"), "STREAM<n>.TOC does not start with COTS");
    StreamToc out;
    const uint32_t blockSize = ReadU32(toc, 0x04);
    out.gmagic = ReadU32(toc, 0x08);
    const uint32_t roadCount = ReadU32(toc, 0x0C);
    const uint32_t nodeCount = ReadU32(toc, 0x10);
    const uint32_t roadOffset = ReadU32(toc, 0x14);
    const uint32_t nodeOffset = ReadU32(toc, 0x18);
    Expect(nodeOffset + nodeCount * 68 == roadOffset, "node array does not end where the road array begins");
    Expect(roadOffset + roadCount * 20 == blockSize, "road array does not end at the block size");

    out.roads.reserve(roadCount);
    for (uint32_t i = 0; i < roadCount; ++i) {
        const size_t o = roadOffset + i * 20;
        StreamRoad r;
        r.road = ReadS32(toc, o + 0x00);
        r.forwardOffset = ReadU32(toc, o + 0x04);
        r.forwardSize = ReadU32(toc, o + 0x08);
        r.reverseOffset = ReadU32(toc, o + 0x0C);
        r.reverseSize = ReadU32(toc, o + 0x10);
        out.roads.push_back(r);
    }
    return out;
}

const TurnEntry* RoadMap::FindTurn(int32_t node, int32_t roadFrom, int32_t roadTo) const {
    for (const Intersection2& junction : intersections) {
        if (junction.node != node) continue;
        for (uint16_t i = 0; i < junction.turnCount; ++i) {
            const size_t index = static_cast<size_t>(junction.turnBase) + i;
            if (index >= turns.size()) break;
            const TurnEntry& turn = turns[index];
            if (turn.roadFrom == roadFrom && turn.roadTo == roadTo) return &turn;
        }
    }
    return nullptr;
}

int RoadMap::ArmIndexFor(int32_t node, int32_t roadFrom, int32_t roadTo) const {
    const TurnEntry* turn = FindTurn(node, roadFrom, roadTo);
    if (!turn) return -1;
    if (turn->pairId >= pairArmIndex.size()) return -1;
    return static_cast<int>(pairArmIndex[turn->pairId]);
}

RoadMap ParseRoadMap(std::span<const uint8_t> map) {
    Expect(TagIs(map, 0, "MAP_"), "ROAD<n>.MAP does not start with MAP_");
    RoadMap out;
    out.gmagic = ReadU32(map, 0x08);

    // Block chain: char tag[4]; u32 size; walked by offset += size, ending exactly at the file end.
    size_t at = ReadU32(map, 0x04);
    while (at + 8 <= map.size()) {
        const uint32_t size = ReadU32(map, at + 4);
        if (size < 8 || at + size > map.size()) break;
        const size_t body = at + 8;
        const size_t records = (size - 8) / 12;
        if (TagIs(map, at, "IPT_")) {
            for (size_t i = 0; i < records; ++i) {
                const size_t o = body + i * 12;
                Intersection2 junction;
                junction.bttSeqBase = ReadU16(map, o + 0);
                junction.node = ReadU16(map, o + 2);
                junction.linkCount = ReadU16(map, o + 4);
                junction.turnBase = ReadU16(map, o + 6);
                junction.turnCount = ReadU16(map, o + 8);
                out.intersections.push_back(junction);
            }
        } else if (TagIs(map, at, "PDT_")) {
            for (size_t i = 0; i < records; ++i) {
                const size_t o = body + i * 12;
                TurnEntry turn;
                turn.pairIndex = ReadU16(map, o + 0);
                turn.pairId = ReadU16(map, o + 2);
                turn.always1 = ReadU16(map, o + 4);
                turn.direction = static_cast<int16_t>(ReadU16(map, o + 6));
                turn.roadFrom = ReadU16(map, o + 8);
                turn.roadTo = ReadU16(map, o + 10);
                out.turns.push_back(turn);
            }
        } else if (TagIs(map, at, "GPDT")) {
            // Halfword 2 of each record is the junction arm index for that road pair.
            for (size_t i = 0; i < records; ++i) out.pairArmIndex.push_back(ReadU16(map, body + i * 12 + 4));
        }
        at += size;
    }
    Expect(!out.intersections.empty() && !out.turns.empty(), "ROAD<n>.MAP has no IPT_ or PDT_ block");
    return out;
}

std::vector<int32_t> WalkRoute(const Race& race) {
    std::vector<int32_t> route;
    if (race.intersections.empty()) return route;

    const auto rowFor = [&](int32_t node) -> const Intersection* {
        for (const Intersection& x : race.intersections)
            if (x.node == node) return &x;
        return nullptr;
    };

    int32_t node = race.start.node;
    for (size_t guard = 0; guard <= race.intersections.size(); ++guard) {
        route.push_back(node);
        if (node == race.finish.node) return route;
        const Intersection* row = rowFor(node);
        if (!row) throw std::runtime_error("road: route reaches a node with no intersection row");
        int32_t next = -1;
        for (size_t slot = 0; slot < 4; ++slot)
            if (row->routeRoad[slot] != -1 && row->nextNode[slot] != -1) {
                next = row->nextNode[slot];
                break;
            }
        if (next < 0) throw std::runtime_error("road: route row has no outgoing road");
        node = next;
    }
    throw std::runtime_error("road: route does not close on the finish node");
}

std::vector<RouteLeg> BuildRouteLegs(const Race& race, const RoadNetwork& network) {
    std::vector<RouteLeg> legs;
    if (race.intersections.empty()) {
        // No junctions: the whole race runs on the start road.
        RouteLeg leg;
        leg.road = race.start.road;
        leg.direction = race.start.direction;
        leg.fromNode = race.start.node;
        leg.toNode = race.finish.node;
        legs.push_back(leg);
        return legs;
    }

    const auto rowFor = [&](int32_t node) -> const Intersection* {
        for (const Intersection& x : race.intersections)
            if (x.node == node) return &x;
        return nullptr;
    };

    // The approach to the first junction is the start road, driven in the start direction.
    RouteLeg first;
    first.road = race.start.road;
    first.direction = race.start.direction;
    first.fromNode = -1; // the race begins part way along this road
    first.toNode = race.start.node;
    legs.push_back(first);

    const std::vector<int32_t> route = WalkRoute(race);
    for (size_t i = 0; i < route.size(); ++i) {
        const Intersection* row = rowFor(route[i]);
        if (!row) break;
        int32_t slot = -1;
        for (size_t k = 0; k < 4; ++k)
            if (row->routeRoad[k] != -1) { slot = static_cast<int32_t>(k); break; }
        if (slot < 0) break; // the finish junction has no outgoing road

        RouteLeg leg;
        leg.road = row->routeRoad[static_cast<size_t>(slot)];
        // The row carries the OUTGOING direction for the route road.
        leg.direction = row->linkDir[static_cast<size_t>(slot)];
        leg.fromNode = row->node;
        leg.toNode = row->nextNode[static_cast<size_t>(slot)];
        if (leg.road >= 0 && leg.road < static_cast<int32_t>(network.roads.size())) legs.push_back(leg);
    }
    return legs;
}

StreamRange StreamRangeFor(const StreamToc& toc, int32_t road, int32_t direction) {
    for (const StreamRoad& r : toc.roads)
        if (r.road == road)
            return direction >= 0 ? StreamRange{r.forwardOffset, r.forwardSize}
                                  : StreamRange{r.reverseOffset, r.reverseSize};
    throw std::runtime_error("road: road " + std::to_string(road) + " has no stream record");
}

size_t CheckRouteDistances(const Race& race, const RoadNetwork& network, std::vector<std::string>& failures) {
    if (race.intersections.empty()) return 0;

    const auto rowFor = [&](int32_t node) -> const Intersection* {
        for (const Intersection& x : race.intersections)
            if (x.node == node) return &x;
        return nullptr;
    };

    const std::vector<int32_t> route = WalkRoute(race);
    size_t checked = 0;
    for (size_t i = 0; i < route.size(); ++i) {
        const Intersection* row = rowFor(route[i]);
        if (!row) continue;
        if (row->distToFinish == kNotOnRoute) continue;

        // The road the route leaves this junction by.
        int32_t routeRoad = -1;
        for (size_t slot = 0; slot < 4; ++slot)
            if (row->routeRoad[slot] != -1) {
                routeRoad = row->routeRoad[slot];
                break;
            }
        if (routeRoad < 0 || routeRoad >= static_cast<int32_t>(network.roads.size())) continue;
        const int32_t roadLength = network.roads[static_cast<size_t>(routeRoad)].length >> 6;

        int32_t expected = 0;
        if (i + 1 < route.size()) {
            const Intersection* next = rowFor(route[i + 1]);
            if (!next || next->distToFinish == kNotOnRoute) continue;
            expected = row->nodeSpan + roadLength + next->distToFinish;
        } else {
            // Last junction: what remains is the run-out along the finish road.
            continue;
        }
        ++checked;
        if (expected != row->distToFinish)
            failures.push_back("race " + std::to_string(race.raceId) + " node " + std::to_string(row->node) +
                               ": distToFinish " + std::to_string(row->distToFinish) + ", expected " +
                               std::to_string(expected));
    }
    return checked;
}

} // namespace rr
