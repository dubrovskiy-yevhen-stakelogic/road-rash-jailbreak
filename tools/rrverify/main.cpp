// rrverify - the per-function bit-exact bench.
//
// It loads a captured machine snapshot into the R3000A+GTE interpreter, calls ONE original guest
// function with a sentinel return address, runs until it returns, and then compares what the guest
// wrote against what our native C++ writes into a clone of the same memory.
//
// Development tool only. Nothing here is linked into the game.
//
// Subcommands:
//   snapshot <state-dir>              report the loaded machine state
//   gte      <state-dir>              what the captured GTE register file and the draw captures can
//                                     and cannot prove about our COP2
//   road     [options]                THE ACCEPTANCE GATE: run the original ROADGRF<n>.TXT parser
//                                     (RASHCDI 0x8006A0C8) over all 100 shipped races and compare
//                                     its output structures against rr::ParseRaceGraph
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "interp/r3000.h"
#include "interp/snapshot.h"
#include "rrformats/road.h"
#include "rrvfs/disc_image.h"

namespace {

using rr::interp::Cpu;
using rr::interp::Memory;
using rr::interp::Trap;
using rr::interp::TrapKind;

// ---------------------------------------------------------------- addresses (docs\formats\road.md and our
// own disassembly of RASHCDI.BIN sha1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06)

constexpr uint32_t kOverlayWindow      = 0x8005B5E8; // all three RASHCD*.BIN load here
constexpr uint32_t kResidencyMask      = 0x8005ACA8; // bit0 resident, bit1 = I, bit2 = G, bit4 = F
constexpr uint32_t kSessionCtxPtr      = 0x8005B2F8; // -> 0x800D5D38
constexpr uint32_t kRgtsPtr            = 0x8005AE64; // gp+0x1D8, -> the in-RAM STREAM<n>.GRF blob
constexpr uint32_t kRaceGraphGlobal    = 0x800D6170; // the 40-byte parser result
constexpr uint32_t kParserEntry        = 0x8006A0C8; // road_parse_txt(const char *text, int len)
constexpr uint32_t kMallocEntry        = 0x8001447C; // void *malloc(size_t, int heapId)
constexpr uint32_t kRgtsRelocate       = 0x80024610; // turns RGTS +0x14/+0x18 into absolute pointers
constexpr uint32_t kCtxRaceIdOffset    = 0x40;
// RASHCDI 0x8005D338 allocates one 512 KiB scratch buffer and parks the pointer at
// *(0x800CE5B8 + 0xA38); 0x8006A7F8 reads DATA\ROADgrf<n>.txt straight into it and passes it to the
// parser. Both words are live in the snapshot, so the bench uses the game's own buffer.
constexpr uint32_t kTextBufferPtr      = 0x800CEFF0;
constexpr uint32_t kTextBufferSizeSel  = 0x800CF010; // value << 14 == the buffer size

constexpr uint32_t kSentinel = 0x00000DEC; // never fetched: Run() stops as soon as pc reaches it

// ---------------------------------------------------------------- small helpers

struct Machine {
    Memory mem;
    Cpu cpu;
    Machine() : cpu(mem) {}
};

uint32_t ReadU32(const Memory& m, uint32_t a) { return m.PeekWord(a); }
int32_t ReadS32(const Memory& m, uint32_t a) { return static_cast<int32_t>(m.PeekWord(a)); }
uint16_t ReadU16(const Memory& m, uint32_t a) {
    return static_cast<uint16_t>(m.PeekByte(a) | (static_cast<uint16_t>(m.PeekByte(a + 1)) << 8));
}
int16_t ReadS16(const Memory& m, uint32_t a) { return static_cast<int16_t>(ReadU16(m, a)); }

void WriteU32(Memory& m, uint32_t a, uint32_t v) { m.PokeWord(a, v); }
void WriteU16(Memory& m, uint32_t a, uint16_t v) { m.WriteBlock(a, &v, 2); }
void Fill(Memory& m, uint32_t a, uint8_t value, uint32_t n) {
    std::vector<uint8_t> buf(n, value);
    m.WriteBlock(a, buf.data(), buf.size());
}

struct CallResult {
    Trap trap;
    uint32_t v0 = 0;
    uint64_t steps = 0;
    bool ok() const { return trap.kind == TrapKind::Halted; }
};

// Calls a guest function the way the game's own code does: arguments in a0..a3, return address in
// ra, and a stack. The only artificial part is `ra`, which points at an address that is never
// executed - that is how the run is detected as finished.
CallResult CallGuest(Cpu& cpu, uint32_t address, const std::array<uint32_t, 4>& args, uint32_t sp,
                     uint64_t maxSteps) {
    cpu.regs[4] = args[0];
    cpu.regs[5] = args[1];
    cpu.regs[6] = args[2];
    cpu.regs[7] = args[3];
    cpu.regs[29] = sp;
    cpu.regs[31] = kSentinel;
    cpu.pc = address;
    cpu.npc = address + 4;
    cpu.loadDelayReg = Cpu::kNoLoadDelay;
    cpu.loadDelayValue = 0;
    const uint64_t before = cpu.instructionsRetired;
    CallResult r;
    r.trap = cpu.Run(kSentinel, maxSteps);
    r.v0 = cpu.regs[2];
    r.steps = cpu.instructionsRetired - before;
    return r;
}

std::string Join(const std::string& dir, const std::string& name) {
    std::string s = dir;
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s.push_back('\\');
    return s + name;
}

// Reads a game file either from our disc extract or, with --disc, straight out of the player's disc
// image through rrvfs.
class DataSource {
public:
    bool useDisc = false;
    std::string extractDir = "work\\disc_us";
    std::string discPath;

    bool Read(const std::string& relative, std::vector<uint8_t>& out, std::string& error) const {
        if (useDisc) {
            try {
                rr::DiscImage disc(discPath);
                std::string p = "/" + relative;
                for (char& c : p) if (c == '\\') c = '/';
                auto f = disc.Find(p);
                if (!f) { error = "not on the disc: " + p; return false; }
                out = disc.ReadFile(*f);
                return true;
            } catch (const std::exception& e) {
                error = std::string("disc read failed: ") + e.what();
                return false;
            }
        }
        return rr::interp::ReadWholeFile(Join(extractDir, relative), out, error);
    }
};

// ---------------------------------------------------------------- the guest-side record layout
//
// Offsets proven by disassembly of RASHCDI 0x80069DEC / 0x8006A0C8 / 0x8006A014, cross-checked
// against docs\formats\road.md section 1.3.

constexpr uint32_t kEndpointSize = 28;
constexpr uint32_t kIntersectionSize = 120;

struct Endpoint {
    int32_t road = 0, distanceShifted = 0, direction = 0, node = 0;
    std::array<int32_t, 3> checker{-1, -1, -1};
    bool operator==(const Endpoint&) const = default;
};

struct Link {
    int32_t road = -1, dir = -1, lengthShifted = -1;
    uint16_t nodeA = 0xFFFF, nodeB = 0xFFFF;
    bool operator==(const Link&) const = default;
};

struct IntersectionRecord {
    int32_t node = -1;
    int32_t distToFinishShifted = -1;
    int32_t nodeSpanShifted = -1;
    int32_t linkCount = -1;
    int32_t routeCount = -1;
    std::array<Link, 4> links{};
    std::array<int32_t, 4> routeRoad{-1, -1, -1, -1};
    std::array<int32_t, 4> nextNode{-1, -1, -1, -1};
    uint16_t tail0 = 0xFFFF, tail1 = 0xFFFF;
    bool operator==(const IntersectionRecord&) const = default;
};

struct RaceGraphGlobal {
    uint32_t allocation = 0;
    uint32_t rmagic = 0;
    uint16_t numEntries = 0;
    uint16_t vehicleDensity = 0, vehicleRate = 0, reactiveDensity = 0, reactiveRate = 0;
    int16_t raceInts = 0;
    uint32_t startPtr = 0, finishPtr = 0, textPtr = 0;
    int32_t textLength = 0;
    uint32_t intersectionsPtr = 0;
};

RaceGraphGlobal ReadGlobal(const Memory& m, uint32_t base) {
    RaceGraphGlobal g;
    g.allocation = ReadU32(m, base + 0x00);
    g.rmagic = ReadU32(m, base + 0x04);
    g.numEntries = ReadU16(m, base + 0x08);
    g.vehicleDensity = ReadU16(m, base + 0x0A);
    g.vehicleRate = ReadU16(m, base + 0x0C);
    g.reactiveDensity = ReadU16(m, base + 0x0E);
    g.reactiveRate = ReadU16(m, base + 0x10);
    g.raceInts = ReadS16(m, base + 0x12);
    g.startPtr = ReadU32(m, base + 0x14);
    g.finishPtr = ReadU32(m, base + 0x18);
    g.textPtr = ReadU32(m, base + 0x1C);
    g.textLength = ReadS32(m, base + 0x20);
    g.intersectionsPtr = ReadU32(m, base + 0x24);
    return g;
}

Endpoint ReadEndpoint(const Memory& m, uint32_t a) {
    Endpoint e;
    e.road = ReadS32(m, a + 0x00);
    e.distanceShifted = ReadS32(m, a + 0x04);
    e.direction = ReadS32(m, a + 0x08);
    e.node = ReadS32(m, a + 0x0C);
    for (int i = 0; i < 3; ++i) e.checker[static_cast<size_t>(i)] = ReadS32(m, a + 0x10 + 4u * static_cast<uint32_t>(i));
    return e;
}

IntersectionRecord ReadIntersection(const Memory& m, uint32_t a) {
    IntersectionRecord r;
    r.node = ReadS32(m, a + 0x00);
    r.distToFinishShifted = ReadS32(m, a + 0x04);
    r.nodeSpanShifted = ReadS32(m, a + 0x08);
    r.linkCount = ReadS32(m, a + 0x0C);
    r.routeCount = ReadS32(m, a + 0x10);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t l = a + 0x14 + i * 16;
        Link& k = r.links[i];
        k.road = ReadS32(m, l + 0x00);
        k.dir = ReadS32(m, l + 0x04);
        k.lengthShifted = ReadS32(m, l + 0x08);
        k.nodeA = ReadU16(m, l + 0x0C);
        k.nodeB = ReadU16(m, l + 0x0E);
    }
    for (uint32_t i = 0; i < 4; ++i) r.routeRoad[i] = ReadS32(m, a + 0x54 + i * 4);
    for (uint32_t i = 0; i < 4; ++i) r.nextNode[i] = ReadS32(m, a + 0x64 + i * 4);
    r.tail0 = ReadU16(m, a + 0x74);
    r.tail1 = ReadU16(m, a + 0x76);
    return r;
}

// ---------------------------------------------------------------- the native side
//
// What our C++ parse SAYS the original should have built. This is the emitter a native port needs
// anyway: it turns an rr::Race into the exact byte image the original parser produces.

const rr::NetworkRoad* FindRoad(const rr::RoadNetwork& net, int32_t id) {
    for (const auto& r : net.roads) if (r.id == id) return &r;
    return nullptr;
}

Link MakeLink(const rr::RoadNetwork& net, int32_t road, int32_t dir, bool& roadMissing) {
    Link k;
    k.road = road;
    k.dir = dir;
    // 0x8006A014: length >> 6 << 16, then node A / node B of the RGTS road record as u16.
    const rr::NetworkRoad* r = FindRoad(net, road);
    if (r == nullptr) { roadMissing = true; return k; }
    k.lengthShifted = static_cast<int32_t>((static_cast<uint32_t>(r->length) >> 6) << 16);
    k.nodeA = static_cast<uint16_t>(r->nodeA);
    k.nodeB = static_cast<uint16_t>(r->nodeB);
    return k;
}

struct NativeResult {
    RaceGraphGlobal global;
    Endpoint start, finish;
    std::vector<IntersectionRecord> intersections; // raceInts + 1 entries
    bool roadMissing = false;
};

// One intersection row, built from the 19 integers of a text row exactly as 0x8006A588..0x8006A748
// does: the three scalars, then the packed link / routeRoad / nextNode arrays.
IntersectionRecord MakeRow(const rr::RoadNetwork& net, const rr::Intersection& x, bool& roadMissing) {
    IntersectionRecord r;
    r.node = x.node;
    r.distToFinishShifted = static_cast<int32_t>(static_cast<uint32_t>(x.distToFinish) << 12);
    r.nodeSpanShifted = static_cast<int32_t>(static_cast<uint32_t>(x.nodeSpan) << 12);
    // The original PACKS: link[count++], routeRoad[count++], nextNode[cursor++]; entries equal to
    // -1 are skipped. Our rr::Intersection keeps everything at its TEXT slot instead.
    int32_t linkCount = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (x.linkRoad[i] == -1) continue;
        r.links[static_cast<size_t>(linkCount)] = MakeLink(net, x.linkRoad[i], x.linkDir[i], roadMissing);
        ++linkCount;
    }
    r.linkCount = linkCount;
    int32_t routeCount = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (x.routeRoad[i] == -1) continue;
        r.routeRoad[static_cast<size_t>(routeCount)] = x.routeRoad[i];
        ++routeCount;
    }
    r.routeCount = routeCount;
    size_t nextCursor = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (x.nextNode[i] == -1) continue;
        r.nextNode[nextCursor++] = x.nextNode[i];
    }
    r.tail0 = 0;
    r.tail1 = 0;
    return r;
}

NativeResult BuildNative(const rr::RaceGraph& graph, const rr::Race& race, const rr::RoadNetwork& net,
                         bool modelRowLoopQuirk) {
    NativeResult out;
    out.global.numEntries = static_cast<uint16_t>(graph.declaredEntries);
    out.global.rmagic = race.rmagic;
    out.global.raceInts = static_cast<int16_t>(race.intersections.size());

    out.start.road = race.start.road;
    out.start.distanceShifted = static_cast<int32_t>(static_cast<uint32_t>(race.start.distance) << 16);
    out.start.direction = race.start.direction;
    out.start.node = race.start.node;
    out.start.checker = race.startChecker;

    out.finish.road = race.finish.road;
    out.finish.distanceShifted = static_cast<int32_t>(static_cast<uint32_t>(race.finish.distance) << 16);
    out.finish.direction = race.finish.direction;
    out.finish.node = race.finish.node;
    out.finish.checker = race.finishChecker;

    for (const rr::Intersection& x : race.intersections) out.intersections.push_back(MakeRow(net, x, out.roadMissing));

    if (modelRowLoopQuirk && race.intersections.empty()) {
        // The original's row loop is not bounded by the count. `[RACEINTS]=n` turns row mode on
        // (0x8006A558) and every following line of the block is then parsed as a row; the counter
        // is decremented AFTER the row and row mode is left only when it reaches exactly zero
        // (0x8006A74C..0x8006A760). With n == 0 the loop therefore runs once anyway, over the next
        // line of the block, and its result lands in slot 0 - on top of the synthesised start-line
        // junction. Model that by parsing an all-blank row through the ordinary row path.
        // A digit-free line gives 19 zeros through atoi(), not 19 "absent" markers.
        rr::Intersection blank;
        blank.node = 0;
        blank.distToFinish = 0;
        blank.nodeSpan = 0;
        blank.linkRoad = {0, 0, 0, 0};
        blank.linkDir = {0, 0, 0, 0};
        blank.routeRoad = {0, 0, 0, 0};
        blank.nextNode = {0, 0, 0, 0};
        out.intersections.assign(1, MakeRow(net, blank, out.roadMissing));
        return out;
    }

    // The synthesised "start line" junction at index RACEINTS (0x8006A2B4).
    {
        IntersectionRecord r;
        r.node = -1;
        r.distToFinishShifted = -4096;
        r.nodeSpanShifted = -4096;
        r.linkCount = 1;
        r.routeCount = 1;
        r.links[0] = MakeLink(net, race.start.road, race.start.direction, out.roadMissing);
        for (size_t i = 1; i < 4; ++i) r.links[i] = Link{-1, 0, 0, 0, 0};
        r.routeRoad = {-1, -1, -1, -1};
        r.nextNode = {-1, -1, -1, -1};
        r.routeRoad[0] = race.start.road;
        r.nextNode[0] = race.start.node;
        r.tail0 = 0xFFFF;
        r.tail1 = 0;
        out.intersections.push_back(r);
    }
    return out;
}

// Writes the native result into guest memory exactly as the original does, so that a whole-RAM
// diff is meaningful.
void EmitNative(Memory& m, uint32_t globalBase, uint32_t alloc, const NativeResult& n,
                uint32_t textPtr, int32_t textLength) {
    Fill(m, globalBase, 0, 40);
    WriteU32(m, globalBase + 0x00, alloc);
    WriteU32(m, globalBase + 0x04, n.global.rmagic);
    WriteU16(m, globalBase + 0x08, n.global.numEntries);
    WriteU16(m, globalBase + 0x0A, n.global.vehicleDensity);
    WriteU16(m, globalBase + 0x0C, n.global.vehicleRate);
    WriteU16(m, globalBase + 0x0E, n.global.reactiveDensity);
    WriteU16(m, globalBase + 0x10, n.global.reactiveRate);
    WriteU16(m, globalBase + 0x12, static_cast<uint16_t>(n.global.raceInts));
    WriteU32(m, globalBase + 0x14, alloc);
    WriteU32(m, globalBase + 0x18, alloc + kEndpointSize);
    WriteU32(m, globalBase + 0x1C, textPtr);
    WriteU32(m, globalBase + 0x20, static_cast<uint32_t>(textLength));
    WriteU32(m, globalBase + 0x24, alloc + 2 * kEndpointSize);

    const auto emitEndpoint = [&](uint32_t at, const Endpoint& e) {
        Fill(m, at, 0xFF, kEndpointSize);
        WriteU32(m, at + 0x00, static_cast<uint32_t>(e.road));
        WriteU32(m, at + 0x04, static_cast<uint32_t>(e.distanceShifted));
        WriteU32(m, at + 0x08, static_cast<uint32_t>(e.direction));
        WriteU32(m, at + 0x0C, static_cast<uint32_t>(e.node));
        for (uint32_t i = 0; i < 3; ++i)
            WriteU32(m, at + 0x10 + i * 4, static_cast<uint32_t>(e.checker[i]));
    };
    emitEndpoint(alloc, n.start);
    emitEndpoint(alloc + kEndpointSize, n.finish);

    uint32_t at = alloc + 2 * kEndpointSize;
    for (const IntersectionRecord& r : n.intersections) {
        Fill(m, at, 0xFF, kIntersectionSize);
        WriteU32(m, at + 0x00, static_cast<uint32_t>(r.node));
        WriteU32(m, at + 0x04, static_cast<uint32_t>(r.distToFinishShifted));
        WriteU32(m, at + 0x08, static_cast<uint32_t>(r.nodeSpanShifted));
        WriteU32(m, at + 0x0C, static_cast<uint32_t>(r.linkCount));
        WriteU32(m, at + 0x10, static_cast<uint32_t>(r.routeCount));
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t l = at + 0x14 + i * 16;
            const Link& k = r.links[i];
            WriteU32(m, l + 0x00, static_cast<uint32_t>(k.road));
            WriteU32(m, l + 0x04, static_cast<uint32_t>(k.dir));
            WriteU32(m, l + 0x08, static_cast<uint32_t>(k.lengthShifted));
            WriteU16(m, l + 0x0C, k.nodeA);
            WriteU16(m, l + 0x0E, k.nodeB);
        }
        for (uint32_t i = 0; i < 4; ++i) WriteU32(m, at + 0x54 + i * 4, static_cast<uint32_t>(r.routeRoad[i]));
        for (uint32_t i = 0; i < 4; ++i) WriteU32(m, at + 0x64 + i * 4, static_cast<uint32_t>(r.nextNode[i]));
        WriteU16(m, at + 0x74, r.tail0);
        WriteU16(m, at + 0x76, r.tail1);
        at += kIntersectionSize;
    }
}

// ---------------------------------------------------------------- comparison

struct Mismatch {
    std::string field;
    int64_t guest = 0;
    int64_t native = 0;
};

void Cmp(std::vector<Mismatch>& out, const char* field, int64_t guest, int64_t native) {
    if (guest != native) out.push_back({field, guest, native});
}
void Cmp(std::vector<Mismatch>& out, const std::string& field, int64_t guest, int64_t native) {
    if (guest != native) out.push_back({field, guest, native});
}

void CompareEndpoint(std::vector<Mismatch>& out, const char* what, const Endpoint& g, const Endpoint& n) {
    Cmp(out, std::string(what) + ".road", g.road, n.road);
    Cmp(out, std::string(what) + ".dist<<16", g.distanceShifted, n.distanceShifted);
    Cmp(out, std::string(what) + ".dir", g.direction, n.direction);
    Cmp(out, std::string(what) + ".node", g.node, n.node);
    for (int i = 0; i < 3; ++i)
        Cmp(out, std::string(what) + ".checker[" + std::to_string(i) + "]",
            g.checker[static_cast<size_t>(i)], n.checker[static_cast<size_t>(i)]);
}

void CompareIntersection(std::vector<Mismatch>& out, size_t index, const IntersectionRecord& g,
                         const IntersectionRecord& n) {
    const std::string p = "ints[" + std::to_string(index) + "].";
    Cmp(out, p + "node", g.node, n.node);
    Cmp(out, p + "distToFinish<<12", g.distToFinishShifted, n.distToFinishShifted);
    Cmp(out, p + "nodeSpan<<12", g.nodeSpanShifted, n.nodeSpanShifted);
    Cmp(out, p + "linkCount", g.linkCount, n.linkCount);
    Cmp(out, p + "routeCount", g.routeCount, n.routeCount);
    for (size_t i = 0; i < 4; ++i) {
        const std::string q = p + "link[" + std::to_string(i) + "].";
        Cmp(out, q + "road", g.links[i].road, n.links[i].road);
        Cmp(out, q + "dir", g.links[i].dir, n.links[i].dir);
        Cmp(out, q + "len<<16", g.links[i].lengthShifted, n.links[i].lengthShifted);
        Cmp(out, q + "nodeA", g.links[i].nodeA, n.links[i].nodeA);
        Cmp(out, q + "nodeB", g.links[i].nodeB, n.links[i].nodeB);
    }
    for (size_t i = 0; i < 4; ++i) Cmp(out, p + "routeRoad[" + std::to_string(i) + "]", g.routeRoad[i], n.routeRoad[i]);
    for (size_t i = 0; i < 4; ++i) Cmp(out, p + "nextNode[" + std::to_string(i) + "]", g.nextNode[i], n.nextNode[i]);
    Cmp(out, p + "tail0", g.tail0, n.tail0);
    Cmp(out, p + "tail1", g.tail1, n.tail1);
}

// ---------------------------------------------------------------- commands

int CmdSnapshot(const std::string& dir) {
    Machine machine;
    rr::interp::SnapshotInfo info;
    std::string error;
    if (!rr::interp::LoadSnapshot(dir, machine.mem, machine.cpu, info, error)) {
        std::printf("FAIL: %s\n", error.c_str());
        return 1;
    }
    const Cpu& c = machine.cpu;
    std::printf("snapshot        %s\n", dir.c_str());
    std::printf("  frame         %u\n", info.frameNumber);
    std::printf("  pc / npc      0x%08X / 0x%08X\n", c.pc, c.npc);
    std::printf("  sp / gp / ra  0x%08X / 0x%08X / 0x%08X\n", c.regs[29], c.regs[28], c.regs[31]);
    std::printf("  SR            0x%08X  (CU2 %s, IsC %s)\n", c.cop0[12],
                machine.cpu.Cop2Enabled() ? "on" : "off", machine.cpu.CacheIsolated() ? "on" : "off");
    std::printf("  scratchpad    %s\n", info.hasScratchpad ? "loaded" : "MISSING");
    std::printf("  bios          %s\n", info.hasBios ? "loaded" : "MISSING");
    std::printf("  residency     0x%08X at 0x8005ACA8\n", ReadU32(machine.mem, kResidencyMask));
    std::printf("  session ctx   0x%08X (set %d, race %d)\n", ReadU32(machine.mem, kSessionCtxPtr),
                ReadS32(machine.mem, ReadU32(machine.mem, kSessionCtxPtr) + 0x30),
                ReadS32(machine.mem, ReadU32(machine.mem, kSessionCtxPtr) + kCtxRaceIdOffset));
    const uint32_t rgts = ReadU32(machine.mem, kRgtsPtr);
    char tag[5] = {};
    machine.mem.ReadBlock(rgts, tag, 4);
    std::printf("  RGTS blob     0x%08X tag '%s' gmagic 0x%08X nodes %d roads %d\n", rgts, tag,
                ReadU32(machine.mem, rgts + 8), ReadS32(machine.mem, rgts + 0x0C),
                ReadS32(machine.mem, rgts + 0x10));
    std::printf("  heap free list head 0x%08X, heap 0x%08X + 0x%X\n", ReadU32(machine.mem, 0x800D6500),
                ReadU32(machine.mem, 0x800548D4), ReadU32(machine.mem, 0x800548D8));
    return 0;
}

// ---------------------------------------------------------------- the road gate

struct RoadOptions {
    std::string stateDir = "work\\oracle\\state\\rr-race";
    DataSource data;
    int onlySet = 0;   // 0 = both
    int onlyRace = 0;  // 0 = all
    bool verbose = false;
    bool ramDiff = true;
    bool forceLoadRgts = false;
    // Off by default: the gate must show the divergence, not hide it. Turning it on tests the
    // hypothesis that the divergence is entirely explained by the row-loop off-by-one.
    bool modelRowLoopQuirk = false;
    // Verification switch: leave the BIOS vector trapping on, to demonstrate that the parser
    // really does go through the kernel A-vector rather than working by accident.
    bool trapBios = false;
};

// Prepares a machine: snapshot -> swap RASHCDG for RASHCDI in the overlay window, exactly as
// load_overlay(2) at 0x800118A0 does (straight file read to 0x8005B5E8, no relocation, then the
// residency mask is reset to 1 and ORed with the id).
bool PrepareMachine(Machine& machine, const RoadOptions& opt, int set, std::string& error) {
    rr::interp::SnapshotInfo info;
    if (!rr::interp::LoadSnapshot(opt.stateDir, machine.mem, machine.cpu, info, error)) return false;

    std::vector<uint8_t> overlay;
    if (!opt.data.Read("RASHCDI.BIN", overlay, error)) return false;
    if (!rr::interp::PlaceImage(machine.mem, overlay, kOverlayWindow, error)) return false;
    WriteU32(machine.mem, kResidencyMask, 1u | 2u);

    // The snapshot was taken mid-race, so the kernel, the heap and the RGTS blob of road set 1 are
    // all live. Let the real BIOS kernel service the six A-vector string functions the parser uses
    // (strncmp/strlen/strchr/strncpy/atoi/printf) instead of shimming them.
    machine.cpu.trapOnBiosVector = opt.trapBios;

    const uint32_t residentRgts = ReadU32(machine.mem, kRgtsPtr);
    const uint32_t residentMagic = ReadU32(machine.mem, residentRgts + 8);
    const uint32_t wantedMagic = (set == 1) ? 0x3862BB66u : 0x3862B8BDu;
    if (opt.forceLoadRgts || residentMagic != wantedMagic) {
        // Reproduce the three things the game's own RGTS loader (0x800244E0) does after the file
        // read: malloc the blob, copy the file into it, store the pointer at gp+0x1D8, and call the
        // relocation helper that turns +0x14/+0x18 into absolute pointers.
        std::vector<uint8_t> grf;
        const std::string name = (set == 1) ? "DATA\\STREAM1.GRF" : "DATA\\STREAM2.GRF";
        if (!opt.data.Read(name, grf, error)) return false;
        const uint32_t sp = machine.cpu.regs[29] & ~7u;
        const CallResult alloc = CallGuest(machine.cpu, kMallocEntry,
                                           {static_cast<uint32_t>(grf.size()), 0, 0, 0}, sp, 50'000'000);
        if (!alloc.ok() || alloc.v0 == 0) {
            error = "guest malloc for the RGTS blob failed: " + alloc.trap.ToString();
            return false;
        }
        machine.mem.WriteBlock(alloc.v0, grf.data(), grf.size());
        WriteU32(machine.mem, kRgtsPtr, alloc.v0);
        const CallResult reloc = CallGuest(machine.cpu, kRgtsRelocate, {alloc.v0, 0, 0, 0}, sp, 50'000'000);
        if (!reloc.ok()) {
            error = "guest RGTS relocation failed: " + reloc.trap.ToString();
            return false;
        }
    }
    return true;
}

// The two words the guest writes at global+0x1C / +0x20 are the address of the `[BEGIN]` line of
// the matching race block and the byte distance from it to the `[END]` line (0x80069C60 ->
// 0x80069DA4 / 0x80069DB4). Reproduced here so the whole-RAM diff has something to compare.
bool FindRaceBlock(const std::vector<uint8_t>& text, int raceId, size_t& beginOffset, int32_t& length) {
    const std::string s(reinterpret_cast<const char*>(text.data()), text.size());
    size_t at = 0;
    size_t lastBegin = std::string::npos;
    bool haveRace = false;
    while (at < s.size()) {
        size_t eol = s.find('\n', at);
        if (eol == std::string::npos) eol = s.size();
        const std::string_view line(s.data() + at, eol - at);
        if (line.rfind("[BEGIN]", 0) == 0) {
            lastBegin = at;
            haveRace = false;
        } else if (line.rfind("[RACEID]=", 0) == 0) {
            haveRace = (std::atoi(std::string(line.substr(9)).c_str()) == raceId);
        } else if (line.rfind("[END]", 0) == 0) {
            if (haveRace && lastBegin != std::string::npos) {
                beginOffset = lastBegin;
                length = static_cast<int32_t>(at - lastBegin);
                return true;
            }
        }
        at = eol + 1;
    }
    return false;
}

int CmdRoad(const RoadOptions& opt) {
    int totalRaces = 0, passedRaces = 0, failedRaces = 0;
    int ramOk = 0, ramFail = 0;
    bool anyHardFailure = false;

    for (int set = 1; set <= 2; ++set) {
        if (opt.onlySet != 0 && opt.onlySet != set) continue;

        std::vector<uint8_t> text;
        std::string error;
        const std::string textName = (set == 1) ? "DATA\\ROADGRF1.TXT" : "DATA\\ROADGRF2.TXT";
        if (!opt.data.Read(textName, text, error)) {
            std::printf("FAIL: %s\n", error.c_str());
            return 1;
        }
        std::vector<uint8_t> grf;
        if (!opt.data.Read(set == 1 ? "DATA\\STREAM1.GRF" : "DATA\\STREAM2.GRF", grf, error)) {
            std::printf("FAIL: %s\n", error.c_str());
            return 1;
        }
        rr::RoadNetwork network;
        rr::RaceGraph graph;
        try {
            network = rr::ParseRoadNetwork(grf);
            graph = rr::ParseRaceGraph(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
        } catch (const std::exception& e) {
            std::printf("FAIL: our C++ parser threw on set %d: %s\n", set, e.what());
            return 1;
        }

        Machine machine;
        if (!PrepareMachine(machine, opt, set, error)) {
            std::printf("FAIL: cannot prepare the machine for set %d: %s\n", set, error.c_str());
            return 1;
        }

        // A pristine copy of the prepared RAM. Every race starts from it, so the heap never drifts
        // and the clone used for the native side is byte-identical to the guest's starting point.
        const std::vector<uint8_t> pristineRam = machine.mem.ram();
        const uint32_t sp = (machine.cpu.regs[29] & ~7u);
        const uint32_t ctx = ReadU32(machine.mem, kSessionCtxPtr);

        // The game's own 512 KiB text scratch buffer, exactly the one 0x8006A7F8 reads the file into.
        const uint32_t textAddr = ReadU32(machine.mem, kTextBufferPtr);
        const uint64_t textCapacity = static_cast<uint64_t>(ReadU32(machine.mem, kTextBufferSizeSel)) << 14;
        if (textAddr == 0 || text.size() > textCapacity) {
            std::printf("FAIL: the guest text buffer at *(0x%08X) is 0x%08X with capacity %llu, too "
                        "small for %zu bytes\n",
                        kTextBufferPtr, textAddr, static_cast<unsigned long long>(textCapacity), text.size());
            return 1;
        }

        std::printf("\n=== road set %d: %zu races in %s, our C++ declares %d entries ===\n", set,
                    graph.races.size(), textName.c_str(), graph.declaredEntries);
        std::printf("    text buffer 0x%08X (%llu bytes), parser 0x%08X, sp 0x%08X, sentinel ra 0x%08X\n",
                    textAddr, static_cast<unsigned long long>(textCapacity), kParserEntry, sp, kSentinel);

        for (const rr::Race& race : graph.races) {
            if (opt.onlyRace != 0 && race.raceId != opt.onlyRace) continue;
            ++totalRaces;

            // ---- guest run
            machine.mem.ram() = pristineRam;
            WriteU32(machine.mem, ctx + kCtxRaceIdOffset, static_cast<uint32_t>(race.raceId));

            machine.mem.WriteBlock(textAddr, text.data(), text.size());

            const CallResult run = CallGuest(machine.cpu, kParserEntry,
                                             {textAddr, static_cast<uint32_t>(text.size()), 0, 0}, sp, 200'000'000);
            if (!run.ok()) {
                std::printf("set%d race%-3d FAIL  guest parser trapped: %s\n", set, race.raceId,
                            run.trap.ToString().c_str());
                ++failedRaces;
                anyHardFailure = true;
                continue;
            }

            const RaceGraphGlobal g = ReadGlobal(machine.mem, kRaceGraphGlobal);
            std::vector<Mismatch> bad;

            const NativeResult native = BuildNative(graph, race, network, opt.modelRowLoopQuirk);
            if (native.roadMissing) {
                std::printf("set%d race%-3d FAIL  our RGTS parse has no road for a link\n", set, race.raceId);
                ++failedRaces;
                anyHardFailure = true;
                continue;
            }

            Cmp(bad, "global.rmagic", g.rmagic, native.global.rmagic);
            Cmp(bad, "global.numEntries", g.numEntries, native.global.numEntries);
            Cmp(bad, "global.vehicleDensity", g.vehicleDensity, native.global.vehicleDensity);
            Cmp(bad, "global.vehicleRate", g.vehicleRate, native.global.vehicleRate);
            Cmp(bad, "global.reactiveDensity", g.reactiveDensity, native.global.reactiveDensity);
            Cmp(bad, "global.reactiveRate", g.reactiveRate, native.global.reactiveRate);
            Cmp(bad, "global.raceInts", g.raceInts, native.global.raceInts);
            Cmp(bad, "global.startPtr==alloc", g.startPtr, g.allocation);
            Cmp(bad, "global.finishPtr==alloc+28", g.finishPtr, g.allocation + kEndpointSize);
            Cmp(bad, "global.intersectionsPtr==alloc+56", g.intersectionsPtr, g.allocation + 2 * kEndpointSize);

            if (g.allocation != 0 && bad.empty()) {
                CompareEndpoint(bad, "start", ReadEndpoint(machine.mem, g.startPtr), native.start);
                CompareEndpoint(bad, "finish", ReadEndpoint(machine.mem, g.finishPtr), native.finish);
                const size_t count = native.intersections.size();
                for (size_t i = 0; i < count; ++i) {
                    CompareIntersection(bad, i, ReadIntersection(machine.mem, g.intersectionsPtr +
                                                                 static_cast<uint32_t>(i) * kIntersectionSize),
                                        native.intersections[i]);
                }
            }

            // ---- whole-RAM diff against a clone the native side wrote into
            std::string ramNote;
            if (opt.ramDiff && g.allocation != 0) {
                Machine clone;
                clone.mem.ram() = pristineRam;
                clone.mem.scratchpad() = machine.mem.scratchpad();
                // Only COP0 matters for the clone (SR.IsC / SR.CU2); the allocator's state is all
                // in RAM, which is the pristine copy.
                std::memcpy(clone.cpu.cop0, machine.cpu.cop0, sizeof(clone.cpu.cop0));
                clone.cpu.trapOnBiosVector = false;
                WriteU32(clone.mem, ctx + kCtxRaceIdOffset, static_cast<uint32_t>(race.raceId));
                clone.mem.WriteBlock(textAddr, text.data(), text.size());
                // The same allocator, the same size, the same heap state -> the same address.
                const uint32_t needed = (g.raceInts >= 0)
                                            ? static_cast<uint32_t>(120 * (g.raceInts + 1) + 56)
                                            : 28u;
                const CallResult a2 = CallGuest(clone.cpu, kMallocEntry, {needed, 0, 0, 0}, sp, 50'000'000);
                size_t beginOffset = 0;
                int32_t blockLength = 0;
                const bool foundBlock = FindRaceBlock(text, race.raceId, beginOffset, blockLength);
                if (a2.ok() && a2.v0 == g.allocation && foundBlock) {
                    EmitNative(clone.mem, kRaceGraphGlobal, a2.v0, native,
                               textAddr + static_cast<uint32_t>(beginOffset), blockLength);
                    // Diff everything except the guest stack, which both runs scribbled differently.
                    const uint32_t stackLow = sp - 4096;
                    size_t diffs = 0;
                    uint32_t firstDiff = 0;
                    const auto& A = machine.mem.ram();
                    const auto& B = clone.mem.ram();
                    for (size_t i = 0; i < A.size(); ++i) {
                        const uint32_t addr = 0x80000000u + static_cast<uint32_t>(i);
                        if (addr >= stackLow && addr <= sp) continue;
                        if (A[i] != B[i]) {
                            if (diffs == 0) firstDiff = addr;
                            ++diffs;
                        }
                    }
                    if (diffs == 0) {
                        ++ramOk;
                        ramNote = " ram=ok";
                    } else {
                        ++ramFail;
                        char buf[96];
                        std::snprintf(buf, sizeof(buf), " ram=FAIL(%zu bytes, first 0x%08X)", diffs, firstDiff);
                        ramNote = buf;
                    }
                } else {
                    ++ramFail;
                    ramNote = " ram=skipped(clone allocator diverged)";
                }
            }

            if (bad.empty()) {
                ++passedRaces;
                std::printf("set%d race%-3d ok    ints=%-3d steps=%-9" PRIu64 "%s\n", set, race.raceId,
                            g.raceInts, run.steps, ramNote.c_str());
            } else {
                ++failedRaces;
                std::printf("set%d race%-3d FAIL  %zu field(s)%s\n", set, race.raceId, bad.size(),
                            ramNote.c_str());
                const size_t show = opt.verbose ? bad.size() : std::min<size_t>(bad.size(), 8);
                for (size_t i = 0; i < show; ++i) {
                    std::printf("             %-34s guest=%-12lld native=%lld\n", bad[i].field.c_str(),
                                static_cast<long long>(bad[i].guest), static_cast<long long>(bad[i].native));
                }
                if (show < bad.size()) std::printf("             ... %zu more\n", bad.size() - show);
            }
        }
    }

    std::printf("\n--- gate summary ---\n");
    std::printf("races compared      %d\n", totalRaces);
    std::printf("field-by-field ok   %d\n", passedRaces);
    std::printf("field-by-field FAIL %d\n", failedRaces);
    if (opt.ramDiff) std::printf("whole-RAM ok/FAIL   %d / %d\n", ramOk, ramFail);
    std::printf("verdict             %s\n", (failedRaces == 0 && totalRaces > 0) ? "PASS" : "FAIL");
    return (failedRaces == 0 && totalRaces > 0 && !anyHardFailure) ? 0 : 1;
}

int Usage() {
    std::printf(
        "rrverify - development oracle bench (R3000A + GTE interpreter vs our native C++)\n"
        "\n"
        "  rrverify snapshot <state-dir>\n"
        "  rrverify gte      <state-dir> [--capture <vr_capture-dir>]\n"
        "  rrverify trace    [--state <dir>] [--out <dir>] [--sav <file>|--no-sav] [--imask N]\n"
        "                    [--frames N] [--vblanks N] [--max-steps N] [--frame-instructions N]\n"
        "                    [--watch ADDR:LEN[:name]] [--probe ADDR[:name]] [--watch-dod3]\n"
        "                    [--pad <hex>]  the digital pad the SIO0 model answers with, active\n"
        "                                   low; 0xFFFF (the default) is nothing pressed\n"
        "                    [--calls] [--no-cop2] [--no-gpu] [--explore] [--dump-vram]\n"
        "                    [--capture <scene.csv>|--no-capture]\n"
        "                    resume the snapshot and record a real frame\n"
        "  rrverify replay   [--in <cop2.bin>]   replay a recorded COP2 log through our GTE\n"
        "  rrverify phys     [--state <dir>] [--state-root <dir>] [--dumps <dir>] [--cases N]\n"
        "                    [--derived-per-sample N] [--seed N] [--only <row>] [--list] [--verbose]\n"
        "                    the bike-physics bench: one row per ported guest function, compared\n"
        "                    over the whole guest RAM\n"
        "  rrverify road     [--state <dir>] [--data <dir>] [--disc <image.bin>]\n"
        "                    [--set 1|2] [--race N] [--verbose] [--no-ram-diff] [--force-load-rgts]\n"
        "                    [--model-row-loop-quirk] [--trap-bios]\n"
        "\n"
        "  --force-load-rgts       load and relocate STREAM<n>.GRF with the game's own code instead\n"
        "                          of reusing the blob already resident in the snapshot\n"
        "  --model-row-loop-quirk  model the original's unbounded intersection-row loop.\n"
        "                          Off by default: the gate\n"
        "                          must show the divergence, not hide it.\n"
        "  --trap-bios             stop on the first jump through a BIOS vector instead of letting\n"
        "                          the real kernel in the snapshot service it\n");
    return 2;
}

} // namespace

int CmdGte(const std::string& stateDir, const std::string& captureDir);
int CmdTrace(int argc, char** argv);
int CmdReplay(int argc, char** argv);
int CmdTexBind(int argc, char** argv);
int CmdCellBind(int argc, char** argv);
int CmdPhys(int argc, char** argv);
int CmdMenuPrims(int argc, char** argv); // menu_cmd.cpp
int CmdMdec(int argc, char** argv);      // mdec_cmd.cpp

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::string cmd = argv[1];

    if (cmd == "snapshot") {
        if (argc < 3) return Usage();
        return CmdSnapshot(argv[2]);
    }
    if (cmd == "gte") {
        std::string state = (argc > 2 && argv[2][0] != '-') ? argv[2] : "work\\oracle\\state\\rr-race";
        std::string capture = "work\\oracle\\vr_capture";
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture = argv[++i];
        }
        return CmdGte(state, capture);
    }
    if (cmd == "trace") return CmdTrace(argc, argv);
    if (cmd == "replay") return CmdReplay(argc, argv);
    if (cmd == "texbind") return CmdTexBind(argc, argv);
    if (cmd == "cellbind") return CmdCellBind(argc, argv);
    if (cmd == "phys") return CmdPhys(argc, argv);
    if (cmd == "menuprims") return CmdMenuPrims(argc, argv);
    if (cmd == "mdec") return CmdMdec(argc, argv);
    if (cmd == "road") {
        RoadOptions opt;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--state" && i + 1 < argc) opt.stateDir = argv[++i];
            else if (a == "--data" && i + 1 < argc) opt.data.extractDir = argv[++i];
            else if (a == "--disc" && i + 1 < argc) { opt.data.useDisc = true; opt.data.discPath = argv[++i]; }
            else if (a == "--set" && i + 1 < argc) opt.onlySet = std::atoi(argv[++i]);
            else if (a == "--race" && i + 1 < argc) opt.onlyRace = std::atoi(argv[++i]);
            else if (a == "--verbose") opt.verbose = true;
            else if (a == "--no-ram-diff") opt.ramDiff = false;
            else if (a == "--force-load-rgts") opt.forceLoadRgts = true;
            else if (a == "--model-row-loop-quirk") opt.modelRowLoopQuirk = true;
            else if (a == "--trap-bios") opt.trapBios = true;
            else return Usage();
        }
        return CmdRoad(opt);
    }
    return Usage();
}
