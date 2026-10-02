// The streamer in the product: the original's streamer SLUS 0x8002305C / 0x80023020
// PORTED in src\game\sim\stream.{h,cpp} and stream_load.cpp (rrverify rows_stream.inc), run on the session's
// arena with the session's callees. It decides which road pieces, scene cells, textures and panoramas are
// resident and when - the resident piece list 0x800D4B10, the cell slot table 0x800D87E8, the resource table
// *(0x8005ACBC) - where the session's own rules (RoadStreamPass / CellStreamPass, RRJB_STREAM=ours) stood.
//
// Also PORTED: the CD access layer (stream_cd.h: the request ring, the drive's
// start / completion, the stall stamps), the sky's panorama pick RASHCDG 0x800650D0 / 0x80065174 / 0x800654B4
// (the renderer shows the panorama of slot +0x12), the cell texture binding 0x800325BC with the draw's readiness
// test 0x800363F0 (cell_view.cpp), type-10 banks into the speech slots (SpeechBankLoad, the upload's release
// 0x80030FA0), the file names by game_state+0x30 / +0x40 (0x80023498 / 0x80023714).
//
// What is OURS here, named:
//   * THE DRIVE (SLUS 0x80014634 seek, 0x80014894 read): a read of n sectors takes n * 300 / 150 ticks of
//     game_state+0x0C (2x speed: 150 sectors a second, 300 ticks a second; RRJB_CD_SPEED=<sectors/s> changes it),
//     back to back after the previous one, no seek time; its completion 0x80022EEC (PORTED) runs at the first
//     game frame at or after the read's end, before the stream frame. RRJB_CD=instant is the control (the
//     read completes the moment it is queued);
//   * the load-time waits 0x80022A78 (VSync(0) until the drive is idle) drain the drive in the drive's own time;
//     game_state+0x0C does not run during the load, so the drive's clock is rebased to it after the start;
//   * the heap block of the table's 32 buffers (0x8005D338's malloc), the TOC and the sky block sit in the
//     arena's object area (OURS: where); the files are handles 3 (STREAM<n>.STR) and 4 (the race's .STP); a
//     development start of road set 2 with one player (game_state+0x30 = 1, which the front end never makes)
//     keeps the set's files, and a direct start (game_state+0x40 = 0: no front end commit) the race's id;
//   * the renderer's side of a texture load: the polygons' texture words rewritten in place (0x80033F14,
//     0x800333F4, 0x800348CC) and LoadImage 0x80048A6C are not run - the renderer maps every texture of the race
//     itself (race_scene.cpp); which cells are DRAWN follows the ported binding and readiness test;
//   * the sky's MDEC column-pair decode RASHCDG 0x800662BC is the renderer's (it decodes a panorama whole), so
//     +0x18 (a decode running) stays 0 and the pick runs every frame;
//   * the music streamer's frame 0x800247E8 / chunk 0x800248E4 are the sound runtime's (not run here);
//   * the SPU upload of a stream bank completes at the end of the frame's stream step.
#include "game/race_session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "game/sim/cell_draw.h"     // EntityCell 0x8008B99C
#include "game/sim/population.h"    // CopDrop 0x8002820C
#include "game/sim/stream.h"
#include "game/sim/stream_cd.h"     // the CD access layer
#include "game/sim/traffic_bind.h"  // CopLeave 0x8002847C, ObjectFree 0x800CC0B0
#include "game/sim/world_pop.h"     // OtherSlot 0x80013360, RecordStamp 0x8009C41C
#include "game/stream_product.h"
#include "game/loader_product.h"   // PieceListInit / ResTableInit / SkyInit PORTED
#include "game/sky_product.h"      // the sky's programs
#include "game/route_product.h"    // GrfLoad / RoadLoad / StreamSetUp / StreamStart PORTED
#include "game/stream_files_product.h" // the stream's files and the .STP start PORTED
#include "game/mp2_product.h"       // the two-player leftovers: 0x8001339C, 0x800A3ECC
#include "game/world_pop_product.h" // RelocateRegionZero SLUS 0x800135E8

namespace rr::game {

bool StreamPorted() {
    const char* v = std::getenv("RRJB_STREAM");
    return v == nullptr || std::strcmp(v, "ours") != 0;
}

namespace {
bool EnvIs(const char* name, const char* value) {
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, value) == 0;
}
} // namespace

bool StreamCdTimed() { return StreamPorted() && !EnvIs("RRJB_CD", "instant"); }
bool StreamSkyPorted() { return StreamPorted() && !EnvIs("RRJB_SKY", "ours"); }
bool StreamCellReadyPorted() { return StreamPorted() && !EnvIs("RRJB_CELLREADY", "off"); }
bool StreamSpeechPorted() { return StreamPorted() && !EnvIs("RRJB_STREAM_SPEECH", "off"); }

namespace {

using rr::sim::GuestRam;
namespace s = rr::sim;

constexpr uint32_t kGp = 0x8005AC8C;               // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kStrHandle = 3, kStpHandle = 4;
constexpr uint32_t kStreamSp = 0x801FF000;         // race_session.cpp kArenaStepSp: the frame's stack
constexpr uint32_t kCdIsrSp = 0x801FEC00;          // OURS: the stack the read completion runs on
constexpr uint32_t kRecords = 32, kBufferBytes = 0x4000;
constexpr uint32_t kSkyBytes = 0x3048;             // 0x800609B0's malloc
constexpr uint32_t kRouteHeader = 0x800D6170;      // +0x14 -> the [START] record {road, along, dir}
constexpr uint32_t kP1Bike = 0x8005B38C, kP2Bike = 0x8005B21C, kPlayerBikes = 0x8005B268;

// THE DRIVE (host side, OURS): one read at a time, in ticks of game_state+0x0C.
struct Drive {
    bool inflight = false;
    uint32_t seekFile = 0, seekPos = 0, file = 0, pos = 0, buf = 0, bytes = 0;
    int64_t startAt = 0, finishAt = 0, freeAt = 0, clock = 0;
    uint32_t speed = 150; // sectors a second (2x)
    int64_t Units(uint32_t n) const { return static_cast<int64_t>((n + 2047u) / 2048u) * 300 / speed; }
};

struct Totals {
    bool ported = false;
    Drive drive;
    size_t cdQueued = 0, cdStarted = 0, cdDone = 0, cdDropped = 0, cdFull = 0, stallFrames = 0, stall28 = 0;
    int64_t cdLatencyMax = 0, cdLatencySum = 0, cdQueueMax = 0;
    size_t speechLoaded = 0, speechReleased = 0, speechNotRunning = 0;
    size_t skyPasses = 0, skyChanges = 0, skyUploads = 0, skyNone = 0, texBinds = 0;
    int32_t skyShown = -2;
    std::string skySeries, names;
    size_t reads = 0, stpReads = 0, readsFailed = 0;
    size_t loadsByType[16] = {}, unloadsByType[16] = {};
    size_t speechDropped = 0, texSeams = 0, musicSeams = 0, popCalls = 0, refusedCalls = 0;
    size_t maxPieces = 0, maxCells = 0, maxRecords = 0;
    uint64_t sumPieces = 0, sumCells = 0, sumRecords = 0, samples = 0;
    size_t pieceIn = 0, pieceOut = 0, cellIn = 0, cellOut = 0; // list / slot changes between frames
    std::vector<uint32_t> lastPieces, lastCells;
    std::string series; // every 300 frames: frame, pieces [ids], cells, records, the player's road:along
    std::string startLine;
};
Totals& T() {
    static Totals t;
    return t;
}

std::vector<uint32_t> Pieces(GuestRam& g) {
    std::vector<uint32_t> v;
    for (uint32_t k = 0; k < 6; ++k) {
        const uint32_t id = g.U32(s::kStPieces + 16u * k);
        if (id != 0xFFFFFFFFu) v.push_back(id);
    }
    std::sort(v.begin(), v.end());
    return v;
}
std::vector<uint32_t> Cells(GuestRam& g) {
    std::vector<uint32_t> v;
    for (uint32_t k = 0; k < 24; ++k) {
        const uint32_t slot = s::kStCellSlots + 0x70u * k;
        if (g.U32(slot) != 0xFFFFFFFFu && g.U32(slot + 4u) != 0) v.push_back(g.U32(slot));
    }
    std::sort(v.begin(), v.end());
    return v;
}
size_t RecordsInUse(GuestRam& g) {
    const uint32_t res = g.U32(s::kStResList);
    const uint32_t n = res != 0 ? std::min<uint32_t>(g.U32(res + 0xA58u), 64u) : 0u;
    size_t used = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (g.U32(res + 0x2Cu + 36u * i) != 0) ++used;
    return used;
}
size_t Diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) { // |a \ b|
    size_t n = 0;
    for (uint32_t x : a)
        if (!std::binary_search(b.begin(), b.end(), x)) ++n;
    return n;
}

// The files of the stream: 0x80023498 opens "STREAM" <game_state+0x30> ".STR/.TOC", 0x80023714 "race%d_%ld.stp" with
// game_state+0x30 and +0x40. `set` / `raceId` are the product's (the fallbacks named in the header).
struct StreamFiles {
    int players = 1, set = 1, raceId = 1;
    std::string note;
};
StreamFiles FilesOf(GuestRam& g, int set, int32_t raceId) {
    StreamFiles f;
    const uint32_t gs = g.U32(s::kStGameState);
    f.players = static_cast<int>(g.U32(gs + 0x30u));
    const int32_t id = g.S32(gs + 0x40u);
    f.set = f.players;
    f.raceId = id;
    if (f.players != set) {
        f.set = set;
        f.note += "game_state+0x30 = " + std::to_string(f.players) + " names STREAM" + std::to_string(f.players) +
                  " / RACE" + std::to_string(f.players) + "_ but the product runs road set " + std::to_string(set) +
                  " (a development start the front end never makes): the set's files are read; ";
    }
    if (id <= 0) {
        f.raceId = raceId;
        f.note += "game_state+0x40 = 0 (a direct start, no front end commit): the race's id " + std::to_string(raceId) +
                  " names the .STP; ";
    }
    return f;
}

// The callees of the stream functions in the product.
class ProductStreamCallees final : public s::RecoverCallees {
public:
    GuestRam& g;
    const DiscImage* disc = nullptr;
    int set = 1;          // STREAM<set>.STR
    int stpSet = 1;       // RACE<stpSet>_<raceId>.STP
    int32_t raceId = 1;
    bool timed = true;    // the ported CD layer and the drive model (RRJB_CD=instant: the read done at once)
    bool check = false;   // --streamcheck: no speech, no texture binding (only residency is compared)
    std::function<bool(uint32_t, const uint32_t*, uint32_t, uint32_t&)> pop;
    std::function<void(const std::string&)> seam;
    // a type-10 chunk to the speech slots: (rec, id, index, v0, done) -> false when the voices are not running
    std::function<bool(uint32_t, uint32_t, uint32_t, uint32_t&, std::vector<std::pair<uint32_t, uint32_t>>&)> speech;
    std::vector<std::pair<uint32_t, uint32_t>> uploads; // the SPU uploads in flight (fn, arg)
    explicit ProductStreamCallees(GuestRam& gg) : g(gg) {}

    void Files(const StreamFiles& f) {
        set = f.set;
        stpSet = f.set;
        raceId = f.raceId;
    }

    bool Call(uint32_t fn, const uint32_t* a, int, uint32_t sp, uint32_t& v0) override {
        v0 = 0;
        Totals& t = T();
        Drive& d = t.drive;
        switch (fn) {
        case s::kStCritEnterFn:
        case s::kStCritLeaveFn: return true; // no interrupts to hold off
        case s::kStPrintFn:                  // the console's debug print
            if (a[0] == 0x80010B94u) ++t.cdDropped; // "CdAccess: Dbl Buf Overflow" (a request of a read record)
            return true;
        case s::kStCdQueueFn:
            if (!timed) return Read(a[0], v0);
            ++t.cdQueued;
            v0 = static_cast<uint32_t>(s::CdQueue(g, a[0], sp, *this)); // PORTED, SLUS 0x80022D20
            if (v0 != 0) ++t.cdFull;
            t.cdQueueMax = std::max<int64_t>(t.cdQueueMax, g.U32(s::kCdCount));
            return !g.Faulted();
        case s::kCdSeekFn: // THE DRIVE: the seek only names the position
            d.seekFile = a[0];
            d.seekPos = a[1];
            return true;
        case s::kCdReadFn: // THE DRIVE: the read starts when the previous one is over
            d.inflight = true;
            d.file = a[0];
            d.buf = a[1];
            d.bytes = a[2];
            d.pos = d.seekPos;
            d.startAt = std::max(d.clock, d.freeAt);
            d.finishAt = d.startAt + d.Units(a[2]);
            ++t.cdStarted;
            return true;
        case s::kStDoneFn: // the request's callback, from the completion 0x80022EEC
            s::ResReadDone(g, a[0], a[1], a[2], sp, *this);
            return !g.Faulted();
        case s::kStMusicFrameFn:
        case s::kStMusicDoneFn: ++t.musicSeams; return true;
        case s::kStEntityCellFn: v0 = static_cast<uint32_t>(s::EntityCell(g, a[0])); return !g.Faulted();
        case s::kStRegionZeroFn:
            RelocateRegionZero(g, g.U32(a[0]));
            Mp2CellCopy(g, a[0], a[1]); // its tail SLUS 0x8001339C (PORTED, mp2_product.h; RRJB_MP2=off: not run)
            return !g.Faulted();
        case s::kStCellTexFn:
            if (check || !StreamCellReadyPorted()) {
                ++t.texSeams;
                return true;
            }
            v0 = s::CellTexBind(g, a[0], a[1]); // PORTED, SLUS 0x800325BC
            ++t.texBinds;
            return !g.Faulted();
        case s::kStCellTexPassFn:
        case s::kStTexPassAllFn:
        case s::kStTexUnlinkFn:
        case s::kStLoadImageFn: ++t.texSeams; return true;
        case s::kStSpeechLoadFn: {
            v0 = 2;
            std::vector<std::pair<uint32_t, uint32_t>> done;
            if (check || !speech || !StreamSpeechPorted()) {
                ++t.speechDropped;
                return true;
            }
            if (!speech(a[0], a[1], a[2], v0, done)) {
                v0 = 2; // the voices are not running yet (the .STP start comes before SpeechInit): dropped
                ++t.speechNotRunning;
                return true;
            }
            if (v0 == 1) ++t.speechLoaded;
            else ++t.speechDropped;
            uploads.insert(uploads.end(), done.begin(), done.end());
            return true;
        }
        case s::kStOtherSlotFn: v0 = s::OtherSlot(g, a[0], a[1]); return !g.Faulted();
        case s::kStStampFn: s::RecordStamp(g, a[0], a[1], a[2], a[3]); return !g.Faulted();
        case s::kStFxStopFn: s::CopLeave(g, a[0]); return !g.Faulted();
        case s::kStFxFreeFn: s::CopDrop(g, a[0]); return !g.Faulted();
        case s::kStPedFreeFn: return s::ObjectFree(g, a[0]) && !g.Faulted();
        case s::kStJailCellFn:
            if (Mp2Class50Release(g, a[0], a[1])) return !g.Faulted(); // PORTED (mp2_product.h)
            if (seam) seam("RASHCDG 0x800A3ECC (the two-player cell release's partner, CellRelease 0x8008C45C) is not "
                           "ported: not run");
            return true;
        case s::kSkyNopFn: return true;                    // RASHCDG 0x8005E840: an empty function
        case s::kSkyUploadFn: // DecodeStart PORTED on the host's MDEC (sky_product.h); off: the renderer's
            ++t.skyUploads;
            if (!check) SkyDecodeFromStream(g, sp);
            return true;
        case s::kStActivateFn:
        case s::kStDownedFn:
        case s::kStNearPieceFn:
        case s::kStPoolDropFn:
        case s::kStSleepFn:
        case s::kStRiderSleepFn:
            ++t.popCalls;
            if (pop && pop(fn, a, sp, v0)) return true;
            ++t.refusedCalls;
            return false;
        default: {
            ++t.refusedCalls;
            char b[140];
            std::snprintf(b, sizeof(b), "the streamer asked for 0x%08X, which the product does not answer", fn);
            if (seam) seam(b);
            return false;
        }
        }
    }

    // THE DRIVE: every read whose end is at or before `now` completed - its bytes into its buffer, then the
    // PORTED completion 0x80022EEC (the callback ReadDone 0x80030894 and the next request, whose read starts at
    // this one's end). `now` in ticks of game_state+0x0C.
    void Advance(int64_t now) {
        Drive& d = T().drive;
        while (d.inflight && d.finishAt <= now) {
            Transfer(d.file, d.pos, d.buf, d.bytes);
            d.inflight = false;
            d.freeAt = d.finishAt;
            d.clock = d.finishAt;
            Totals& t = T();
            ++t.cdDone;
            t.cdLatencyMax = std::max(t.cdLatencyMax, d.finishAt - d.startAt);
            t.cdLatencySum += d.finishAt - d.startAt;
            s::CdDone(g, kCdIsrSp, *this); // PORTED
            if (g.Faulted()) return;
        }
        d.clock = std::max(d.clock, now);
    }
    // The load's waits 0x80022A78: until the drive is idle, in the drive's own time.
    void Drain() {
        Drive& d = T().drive;
        while (d.inflight && !g.Faulted()) Advance(d.finishAt);
    }
    // The SPU uploads over: the PORTED callback 0x80016464 and its buffer release 0x80030FA0.
    void Uploads(const std::function<bool(uint32_t, const std::function<bool(uint32_t)>&)>& done) {
        std::vector<std::pair<uint32_t, uint32_t>> u;
        u.swap(uploads);
        for (const auto& [fn, arg] : u) {
            if (fn != rr::sim::kSpeechUploadCb) continue;
            const bool ok = done(arg, [this](uint32_t index) {
                s::ResFreeIrq(g, index, kCdIsrSp, *this); // PORTED
                ++T().speechReleased;
                return !g.Faulted();
            });
            if (!ok && seam) seam("the speech upload's completion SLUS 0x80016464 did not run");
        }
    }

private:
    std::string Name(uint32_t file) const {
        return file == kStpHandle ? "DATA/RACE" + std::to_string(stpSet) + "_" + std::to_string(raceId) + ".STP"
                                  : "DATA/STREAM" + std::to_string(set) + ".STR";
    }
    void Transfer(uint32_t file, uint32_t pos, uint32_t buf, uint32_t bytes) {
        Totals& t = T();
        std::vector<uint8_t> data(bytes, 0);
        const auto f = disc != nullptr ? disc->Find(Name(file)) : std::nullopt;
        if (f && static_cast<uint64_t>(pos) + bytes <= f->size) disc->ReadForm1(f->lba, pos, data.data(), data.size());
        else ++t.readsFailed;
        g.WriteBlock(buf, data.data(), bytes);
        ++t.reads;
        if (file == kStpHandle) ++t.stpReads;
    }
    // RRJB_CD=instant: the request {kind, file, position, record index, buffer, arg,
    // callback, bytes} read from the disc now and completed by the PORTED ReadDone 0x80030894.
    bool Read(uint32_t req, uint32_t& v0) {
        const uint32_t kind = g.U32(req), file = g.U32(req + 4u), pos = g.U32(req + 8u), index = g.U32(req + 12u),
                       buf = g.U32(req + 16u), arg = g.U32(req + 20u), bytes = g.U32(req + 28u);
        Transfer(file, pos, buf, bytes);
        s::ResReadDone(g, index, kind, arg, kCdIsrSp, *this);
        v0 = 0;
        return !g.Faulted();
    }
};

} // namespace

// The census of every frame, both modes: the counter line and the series of the race log.
void StreamSample(GuestRam& g, uint32_t frame, uint32_t bike) {
    Totals& t = T();
    const std::vector<uint32_t> p = Pieces(g), c = Cells(g);
    const size_t r = RecordsInUse(g);
    ++t.samples;
    t.sumPieces += p.size();
    t.sumCells += c.size();
    t.sumRecords += r;
    t.maxPieces = std::max(t.maxPieces, p.size());
    t.maxCells = std::max(t.maxCells, c.size());
    t.maxRecords = std::max(t.maxRecords, r);
    t.pieceIn += Diff(p, t.lastPieces);
    t.pieceOut += Diff(t.lastPieces, p);
    t.cellIn += Diff(c, t.lastCells);
    t.cellOut += Diff(t.lastCells, c);
    t.lastPieces = p;
    t.lastCells = c;
    if (frame % 300u != 1u) return;
    std::string ids;
    for (uint32_t id : p) ids += (ids.empty() ? "" : ",") + std::to_string(id);
    char b[200];
    std::snprintf(b, sizeof(b), " f%u p%zu[%s] c%zu r%zu @%u:%d;", frame, p.size(), ids.c_str(), c.size(), r,
                  g.U16(bike + 0x168u), g.S32(bike + 0x170u) >> 16);
    t.series += b;
}

std::string StreamTotals() {
    const Totals& t = T();
    const double n = t.samples != 0 ? static_cast<double>(t.samples) : 1.0;
    char b[1100];
    std::snprintf(b, sizeof(b),
                  "stream: %s - resident per frame: road pieces avg %.2f max %zu (%zu listed / %zu dropped), cells avg "
                  "%.2f max %zu (%zu in / %zu out), table records in use avg %.2f max %zu, over %llu frames; CD reads %zu "
                  "(%zu of the .STP, %zu failed); loads by key type 0/1/2/3/4/8/9/10 = %zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu, "
                  "unloads %zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu; SPU banks dropped %zu; population calls %zu; refused %zu\n",
                  t.ported ? "the original's streamer PORTED (stream.h)" : "OURS (RRJB_STREAM=ours: the session's rules)",
                  t.sumPieces / n, t.maxPieces, t.pieceIn, t.pieceOut, t.sumCells / n, t.maxCells, t.cellIn, t.cellOut,
                  t.sumRecords / n, t.maxRecords, static_cast<unsigned long long>(t.samples), t.reads, t.stpReads,
                  t.readsFailed, t.loadsByType[0], t.loadsByType[1], t.loadsByType[2], t.loadsByType[3],
                  t.loadsByType[4], t.loadsByType[8], t.loadsByType[9], t.loadsByType[10], t.unloadsByType[0],
                  t.unloadsByType[1], t.unloadsByType[2], t.unloadsByType[3], t.unloadsByType[4], t.unloadsByType[8],
                  t.unloadsByType[9], t.unloadsByType[10], t.speechDropped, t.popCalls, t.refusedCalls);
    char c[1400];
    const double done = t.cdDone != 0 ? static_cast<double>(t.cdDone) : 1.0;
    std::snprintf(c, sizeof(c),
                  "stream2: CD %s - %zu request(s) filed (%zu refused: ring full), %zu read(s) started, %zu completed, %zu "
                  "dropped (Dbl Buf Overflow); read time avg %.1f max %lld ticks (1/300 s), ring depth max %lld; frames with a "
                  "read in flight past 0xF0 ticks (the stall grade) %zu, frames with game_state+0x28 set %zu; speech banks "
                  "(type 10) %s: %zu loaded into the slots, %zu dropped by SpeechBankLoad, %zu before SpeechInit, %zu "
                  "buffer(s) released by the upload's callback; sky %s: %zu pick(s), %zu change(s) of the panorama shown, "
                  "%zu frame(s) with none, %zu MDEC column decode(s) (the renderer's); cell texture bindings 0x800325BC %s: "
                  "%zu, renderer-side texture calls not run %zu\n",
                  StreamCdTimed() ? "TIMED (the ported layer, 2x drive)" : "INSTANT (RRJB_CD=instant)", t.cdQueued,
                  t.cdFull, t.cdStarted, t.cdDone, t.cdDropped, static_cast<double>(t.cdLatencySum) / done,
                  static_cast<long long>(t.cdLatencyMax), static_cast<long long>(t.cdQueueMax), t.stallFrames, t.stall28,
                  StreamSpeechPorted() ? "PORTED" : "OFF (RRJB_STREAM_SPEECH=off)", t.speechLoaded, t.speechDropped,
                  t.speechNotRunning, t.speechReleased, StreamSkyPorted() ? "PORTED" : "OURS (RRJB_SKY=ours)", t.skyPasses,
                  t.skyChanges, t.skyNone, t.skyUploads, StreamCellReadyPorted() ? "PORTED" : "OFF (RRJB_CELLREADY=off)",
                  t.texBinds, t.texSeams);
    return std::string(b) + c + (t.names.empty() ? "" : "stream files: " + t.names + "\n") +
           (t.startLine.empty() ? "" : "stream start: " + t.startLine + "\n") + "stream series:" + t.series + "\n" +
           "stream sky series:" + t.skySeries + "\n";
}

// The loads and unloads of a frame, by key type: the records whose state changed (loaded +2 / freed).
namespace {
struct RecordWatch {
    uint32_t key[64] = {}, flags[64] = {};
    void Before(GuestRam& g) {
        const uint32_t res = g.U32(s::kStResList);
        for (uint32_t i = 0; i < kRecords; ++i) {
            flags[i] = g.U32(res + 0x2Cu + 36u * i);
            key[i] = g.U32(res + 0x2Cu + 36u * i + 8u);
        }
    }
    void After(GuestRam& g) {
        const uint32_t res = g.U32(s::kStResList);
        Totals& t = T();
        for (uint32_t i = 0; i < kRecords; ++i) {
            const uint32_t f = g.U32(res + 0x2Cu + 36u * i);
            const uint32_t k = g.U32(res + 0x2Cu + 36u * i + 8u);
            const bool wasLoaded = (flags[i] & 2u) != 0, isLoaded = (f & 2u) != 0;
            if (wasLoaded && (!isLoaded || k != key[i])) ++t.unloadsByType[(key[i] >> 28) & 15u];
            if (isLoaded && (!wasLoaded || k != key[i])) ++t.loadsByType[(k >> 28) & 15u];
        }
    }
};
} // namespace

// The loader's stream set-up, after the road map (BuildArena): the table's 32 buffers, RASHCDI 0x8005D410
// (ResInit), 0x8005D63C's texture part (TexInit), 0x8006A8FC's empty piece list, SLUS 0x80023498's files
// (STREAM<set>.TOC through 0x80023F08's fix-up, the .STR's handle and size, both stream records reset by
// 0x80024354, the turn 0) and the sky block of 0x800609B0.
void RaceSession::StreamBuild(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kGp);
    T() = Totals();
    T().ported = true;
    streamDisc_ = &disc;
    auto take = [this](uint32_t bytes, uint32_t align) {
        const uint32_t at = (arenaFreeFrom_ + align - 1u) & ~(align - 1u);
        arenaFreeFrom_ = at + bytes;
        return at;
    };
    streamBuffers_ = take(kRecords * kBufferBytes, 16u);
    for (uint32_t k = 0; k < kRecords * kBufferBytes; k += 4) g.W32(streamBuffers_ + k, 0);
    // RASHCDI 0x8006A8FC (from the road-map load 0x8006AC6C): the piece list empty
    for (uint32_t k = 0; k < 6u; ++k) {
        g.W32(s::kStPieces + 16u * k, 0xFFFFFFFFu);
        for (uint32_t w = 4; w < 16u; w += 4) g.W32(s::kStPieces + 16u * k + w, 0);
    }
    g.W32(s::kStPiecesUsed, 0);
    g.W32(s::kStPiecesLast, 0xFFFFFFFFu);
    // RASHCDI 0x8005D410 / 0x8005D63C
    const int32_t players = static_cast<int32_t>(g.U32(g.U32(s::kStGameState) + 0x30u));
    const uint32_t res = g.U32(s::kStResList);
    g.W32(res + 0xA3Cu, 0); // 0x8005D338 takes its block once per boot; OURS: once per session
    std::string loaderError;
    if (!(LoaderPorted() && LoaderStreamSetUp(g, nullptr, &disc, streamBuffers_, 0, kLoaderSp, loaderError)))
        s::ResInit(g, streamBuffers_); // RRJB_LOADER=off: the listing's port without a row (the ported loader has it)
    s::TexInit(g, players);
    g.W32(kGp + s::kGpStCellCount, 0);
    // SLUS 0x80023498: STREAM<game_state+0x30>.TOC (0x80023F08: +0x14 / +0x18 made absolute), the .STR, the records
    // reset (the name by the original's rule, stream_cd.h)
    const StreamFiles files = FilesOf(g, world_.set, world_.raceId);
    T().names = "STREAM" + std::to_string(files.set) + ".STR, RACE" + std::to_string(files.set) + "_" +
                std::to_string(files.raceId) + ".STP (game_state+0x30 = " + std::to_string(files.players) +
                ", +0x40 = " + std::to_string(g.S32(g.U32(s::kStGameState) + 0x40u)) + ")" +
                (files.note.empty() ? "" : "; OURS: " + files.note);
    if (const char* v = std::getenv("RRJB_CD_SPEED"); v != nullptr && std::atoi(v) > 0)
        T().drive.speed = static_cast<uint32_t>(std::atoi(v));
    {
        ProductStreamCallees cc(g);
        g.W32(kGp + s::kGpCdInit, 0); // the layer's once-per-boot reset (OURS: once per session)
        s::CdInit(g, kStreamSp, cc);  // PORTED, SLUS 0x800229B0
    }
    std::string set;
    uint32_t tocAt = 0;
    auto streamFiles = [&]() { // SLUS 0x80023498
    set = std::to_string(files.set);
    std::vector<uint8_t> toc;
    if (const auto f = disc.Find("DATA/STREAM" + set + ".TOC")) toc = disc.ReadFile(*f);
    tocAt = take(static_cast<uint32_t>(toc.size()) + 16u, 16u);
    g.WriteBlock(tocAt, toc.data(), static_cast<uint32_t>(toc.size()));
    if (toc.size() >= 0x1C) {
        g.W32(tocAt + 0x18u, tocAt + g.U32(tocAt + 0x18u));
        g.W32(tocAt + 0x14u, tocAt + g.U32(tocAt + 0x14u));
    }
    g.W32(kGp + s::kGpStToc, tocAt);
    g.W32(kGp + s::kGpStStr, kStrHandle);
    g.W32(kGp + s::kGpStLastPos, 0);
    uint32_t strSize = 0;
    if (const auto f = disc.Find("DATA/STREAM" + set + ".STR")) strSize = static_cast<uint32_t>(f->size);
    g.W32(kGp + s::kGpStStrSize, strSize);
    for (uint32_t p = 0; p < 2; ++p) {
        s::StreamSelect(g, p);
        s::StreamReset(g);
    }
    g.W32(kGp + s::kGpStTurn, 0);
    g.W32(kGp + s::kGpStCdBusy, 0);
    };
    if (routePorted_) { // SetUpRace's GrfLoad / RoadLoad, then StreamSetUp 0x80022F78 PORTED (route_product.h)
        BuildRouteArenaPorted(disc);
        std::string why;
        if (StreamFilesPorted() && RouteTotals().heapNext != 0) { // every child PORTED (stream_files_product.h)
            ProductStreamCallees cc(g);
            cc.disc = &disc;
            cc.Files(files);
            cc.timed = StreamCdTimed();
            cc.seam = [this](const std::string& line) { NoteSeam(line); };
            StreamFilesEnv env;
            env.disc = &disc;
            env.set = world_.set;
            env.raceId = world_.raceId;
            env.rest = &cc;
            env.field = [&cc]() { cc.Advance(T().drive.clock + 5); }; // OURS: a field is 5 ticks of the drive
            env.heapFrom = RouteTotals().heapNext;
            env.heapTo = RouteTotals().heapTo;
            if (!StreamFilesSetUp(g, env, kLoaderSp, why)) throw std::runtime_error("the stream set-up: " + why);
            set = std::to_string(files.set);
            tocAt = g.U32(kGp + s::kGpStToc);
        } else if (!RouteStreamSetUp(g, kLoaderSp, streamFiles, why)) {
            throw std::runtime_error("the stream set-up: " + why);
        }
    } else {
        streamFiles();
    }
    NoteSeam(SkySetUpBefore(g)); // 0x80060BE8's cache origin and DctVlc tables (sky_product.h)
    // RASHCDI 0x800609B0: the sky block (its fields; the GPU calls after it are the renderer's)
    const uint32_t sky = take(kSkyBytes, 16u);
    for (uint32_t k = 0; k < kSkyBytes; k += 4) g.W32(sky + k, 0);
    g.W32(s::kStSkyPtr, sky);
    const uint16_t b394 = g.U16(0x8005B394u), b390 = g.U16(0x8005B390u);
    g.W32(sky, kSkyBytes);
    g.W16(sky + 0x12u, 0xFFFF);
    g.W16(sky + 0x20u, 1);
    g.W16(sky + 0x3Au, b394);
    g.W16(sky + 0x3Eu, b394);
    for (uint32_t o : {0x40u, 0x42u, 0x44u, 0x46u}) g.W16(sky + o, static_cast<uint16_t>(b394 + 0x100u));
    g.W32(sky + 0x18u, 0);
    g.W16(sky + 0x10u, 0);
    g.W16(sky + 0x14u, 0);
    g.W32(sky + 4u, 0xFFFFFFFFu);
    g.W16(sky + 0x22u, 0);
    g.W32(sky + 8u, 0xFFFFFFFFu);
    g.W32(sky + 0xCu, 0xFFFFFFFFu);
    g.W16(sky + 0x24u, 0);
    g.W16(sky + 0x38u, b390);
    g.W16(sky + 0x3Cu, b390);
    g.W16(sky + 0x16u, 2);
    if (LoaderPorted()) // SkyInit PORTED over the fields above (its malloc: this block, heap 1)
        if (!LoaderStreamSetUp(g, nullptr, &disc, 0, sky, kLoaderSp, loaderError)) NoteSeam("the race loader: " + loaderError);
    NoteSeam(SkySetUpAfter(g)); // TileRecords 0x80060AA8 PORTED, the sky OT
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the streamer is the ORIGINAL's (stream.h, PORTED): the resource table *(0x8005ACBC) = 0x%08X with 32 "
                  "records over 32 x 16 KiB at 0x%08X (OURS: where 0x8005D338's malloc sits), STREAM%s.TOC at 0x%08X, the "
                  "sky block at 0x%08X; RRJB_STREAM=ours is the negative control (the session's residency rules)",
                  res, streamBuffers_, set.c_str(), tocAt, sky);
    NoteSeam(b);
}

// SetUpRace's SLUS 0x80023020, after the road map and the files: StreamPlace at the [START] record, then
// 0x80023714 per player - 0x80024168: the race's .STP opened, its header read (0x28 bytes), the cursor over its
// payload, up to three requests of 0x40 chunks each followed by the pump 0x80030608, the cursor moved to the
// start road's TOC range at the header's resume offset +0x24. The players' bike pointers are 0 here, as BuildGrid
// 0x80067B00 and the previous race's release (RASHCDI 0x80067A..) leave them before BuildRace runs.
void RaceSession::StreamStart(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kGp);
    const uint32_t keep[4] = {g.U32(kP1Bike), g.U32(kP2Bike), g.U32(kPlayerBikes), g.U32(kPlayerBikes + 4u)};
    g.W32(kP1Bike, 0);
    g.W32(kP2Bike, 0);
    g.W32(kPlayerBikes, 0);
    g.W32(kPlayerBikes + 4u, 0);
    ProductStreamCallees c(g);
    c.disc = &disc;
    const StreamFiles files = FilesOf(g, world_.set, world_.raceId);
    c.Files(files);
    c.timed = StreamCdTimed();
    c.pop = [this](uint32_t fn, const uint32_t* a, uint32_t sp, uint32_t& v0) { return StreamPopCall(fn, a, sp, v0); };
    c.seam = [this](const std::string& line) { NoteSeam(line); };
    c.speech = [this](uint32_t rec, uint32_t id, uint32_t index, uint32_t& v0, std::vector<std::pair<uint32_t, uint32_t>>& done) {
        return sounds_.StreamSpeechBank(rec, id, index, v0, done);
    };
    RecordWatch w;
    w.Before(g);
    const uint32_t start = g.U32(kRouteHeader + 0x14u);
    std::vector<uint8_t> hdr(0x28, 0);
    const std::string stp = "DATA/RACE" + std::to_string(files.set) + "_" + std::to_string(files.raceId) + ".STP";
    const auto f = disc.Find(stp);
    if (f) disc.ReadForm1(f->lba, 0, hdr.data(), hdr.size());
    auto h32 = [&hdr](size_t o) {
        uint32_t v = 0;
        std::memcpy(&v, hdr.data() + o, 4);
        return v;
    };
    const uint32_t players = g.U32(g.U32(s::kStGameState) + 0x30u);
    uint32_t requests = 0;
    auto perPlayer = [&]() { // SLUS 0x80023714
    for (uint32_t p = 0; p < players && f; ++p) {
        s::StreamSelect(g, p);
        const uint32_t st = g.U32(kGp + s::kGpStCur);
        s::StreamCursor(g, st + 8u, kStpHandle, h32(0x14), h32(0x18));
        g.W32(st + 0x38u, 0);
        g.W32(st + 0x20u, 1);
        for (int k = 0; k < 3; ++k) {
            if (s::StreamAtEnd(g, st + 8u) != 0) break;
            s::StreamRequest(g, st + 8u, 0x40, 0, g.U32(st) << 16, kStreamSp - 0x100u, c);
            ++requests;
            c.Drain(); // 0x80022A78(2) twice: the drive idle (the completions ReadDone 0x80030894 ran)
            s::ResPump(g, kStreamSp - 0x100u, c);
        }
        g.W32(st + 0x20u, 0);
        s::StreamAtEnd(g, st + 8u);
        s::StreamRange(g);
        s::StreamLimit(g, h32(0x24));
    }
    };
    if (routePorted_) { // StreamStart SLUS 0x80023020 PORTED - StreamPlace 0x800235C8, then 0x80023714
        std::string why;
        if (StreamFilesPorted() && StreamFilesTotals().setUps != 0) { // 0x80023714 / 0x80024168 PORTED
            const size_t before = T().stpReads + T().cdQueued;
            StreamFilesEnv env;
            env.disc = &disc;
            env.set = world_.set;
            env.raceId = world_.raceId;
            env.rest = &c;
            env.field = [&c]() { c.Advance(T().drive.clock + 5); }; // OURS: a field is 5 ticks of the drive
            if (!StreamFilesStart(g, env, kLoaderSp, why)) NoteSeam("the stream files: " + why);
            requests = static_cast<uint32_t>(T().stpReads + T().cdQueued - before);
        } else if (!RouteStreamStart(g, kLoaderSp, perPlayer, why)) {
            NoteSeam("the route: " + why);
        }
    } else {
        s::StreamPlace(g, g.U32(start), g.S16(start + 6u), g.S32(start + 8u));
        perPlayer();
    }
    c.Uploads([this](uint32_t arg, const std::function<bool(uint32_t)>& release) { return sounds_.StreamSpeechDone(arg, release); });
    // OURS: the load's VSync waits did not run game_state+0x0C; the drive's clock is rebased to it
    T().drive.clock = T().drive.freeAt = static_cast<int64_t>(g.U32(g.U32(s::kStGameState) + 0x0Cu));
    w.After(g);
    g.W32(kP1Bike, keep[0]);
    g.W32(kP2Bike, keep[1]);
    g.W32(kPlayerBikes, keep[2]);
    g.W32(kPlayerBikes + 4u, keep[3]);
    // the road objects the grid's cursor seating may search (SeatCursors, OURS: with RRJB_GRID=ours)
    arenaObjectList_.clear();
    for (uint32_t k = 0; k < 6; ++k)
        if (g.U32(s::kStPieces + 16u * k) != 0xFFFFFFFFu) arenaObjectList_.push_back(g.U32(s::kStPieces + 16u * k + 8u));
    arenaObjects_ = arenaObjectList_.size();
    const std::vector<uint32_t> pc = Pieces(g), cc = Cells(g);
    std::string ids;
    for (uint32_t id : pc) ids += (ids.empty() ? "" : ",") + std::to_string(id);
    char b[400];
    std::snprintf(b, sizeof(b),
                  "%s (%s, %u request(s)): [START] road %u along %d dir %d; resident: road pieces [%s], %zu cell(s), %zu of "
                  "32 records in use, the stream cursor at 0x%08X of [0x%08X, 0x%08X)",
                  stp.c_str(), f ? "read" : "NOT ON THE DISC", requests, g.U32(start), g.S16(start + 6u),
                  g.S32(start + 8u), ids.c_str(), cc.size(), RecordsInUse(g), g.U32(s::kStStates + 0x0Cu),
                  g.U32(s::kStStates + 0x14u), g.U32(s::kStStates + 0x18u));
    T().startLine = b;
    NoteSeam(std::string("the stream start SLUS 0x80023020 (PORTED): ") + b);
}

// ============================================================================ rrgame --streamcheck
namespace {

struct Held {
    std::vector<uint32_t> pieces, cells, records, textures, panoramas;
    uint32_t road = 0, along = 0, dir = 0, clock = 0, cursor = 0;
};
Held HeldOf(GuestRam& g) {
    Held h;
    h.pieces = Pieces(g);
    h.cells = Cells(g);
    const uint32_t res = g.U32(s::kStResList);
    for (uint32_t i = 0; i < kRecords; ++i) {
        const uint32_t rec = res + 0x2Cu + 36u * i;
        if ((g.U32(rec) & 0x13u) == 0x13u) h.records.push_back(g.U32(rec + 8u));
    }
    std::sort(h.records.begin(), h.records.end());
    for (uint32_t k = 0; k < 24; ++k) {
        const uint32_t sl = s::kStTexSlots + 0x30u * k;
        if (g.U32(sl + 8u) != 0xFFFFFFFFu) h.textures.push_back(g.U32(sl + 8u));
    }
    std::sort(h.textures.begin(), h.textures.end());
    const uint32_t sky = g.U32(s::kStSkyPtr);
    for (uint32_t k = 0; sky != 0 && k < 20; ++k)
        if (g.U32(sky + 0x2E4u + 12u * k) != 0) h.panoramas.push_back(g.U32(sky + 0x2E4u + 12u * k + 8u));
    std::sort(h.panoramas.begin(), h.panoramas.end());
    const uint32_t st = s::kStStates;
    h.road = g.U32(st + 0x48u);
    h.along = g.U32(st + 0x44u);
    h.dir = g.U32(st + 0x40u);
    h.cursor = g.U32(st + 0x0Cu);
    h.clock = g.U32(g.U32(s::kStGameState) + 0x10u);
    return h;
}

// Every console key held by us (`missing` counts the ones not), and the extra ones.
void Compare(const std::vector<uint32_t>& console, const std::vector<uint32_t>& ours, size_t& missing, size_t& extra,
             std::string& names) {
    std::vector<uint32_t> b = ours;
    for (uint32_t x : console) {
        const auto it = std::find(b.begin(), b.end(), x);
        if (it == b.end()) {
            ++missing;
            char t[24];
            std::snprintf(t, sizeof(t), " -%X", x);
            names += t;
        } else {
            b.erase(it);
        }
    }
    extra += b.size();
    for (uint32_t x : b) {
        char t[24];
        std::snprintf(t, sizeof(t), " +%X", x);
        names += t;
    }
}

// The callees of a residency check: the CD is the host's; the texture / music / population sides are not run
// (counted) - only what the streamer decides is compared.
struct CheckCallees final : s::RecoverCallees {
    s::RecoverCallees* cd = nullptr;
    size_t notRun = 0;
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        switch (fn) {
        case s::kStCdQueueFn:
        case s::kCdSeekFn:
        case s::kCdReadFn:
        case s::kStDoneFn:
        case s::kStPrintFn:
        case s::kStCritEnterFn:
        case s::kStCritLeaveFn:
        case s::kStRegionZeroFn:
        case s::kStSpeechLoadFn:
        case s::kStOtherSlotFn: return cd->Call(fn, a, n, sp, v0);
        default: ++notRun; v0 = 0; return true;
        }
    }
};

bool ReadImage(const std::string& path, std::vector<uint8_t>& ram) {
    ram.assign(GuestRam::kRamSize, 0);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    const size_t n = std::fread(ram.data(), 1, ram.size(), f);
    std::fclose(f);
    return n == ram.size();
}

} // namespace

bool CheckStream(const DiscImage& disc, const std::string& dir, std::string& report, bool mutate) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".bin") files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    if (files.size() < 2) {
        report = "streamcheck: fewer than two RAM images in " + dir + "\n";
        return false;
    }
    std::vector<uint8_t> arena, image;
    if (!ReadImage(files[0], arena)) {
        report = "streamcheck: " + files[0] + " is not a 2 MiB image\n";
        return false;
    }
    GuestRam g(arena.data(), kGp);
    const int set = static_cast<int>(g.U32(g.U32(s::kStGameState) + 0x30u));
    const int32_t raceId = static_cast<int32_t>(g.U32(g.U32(s::kStGameState) + 0x40u));
    ProductStreamCallees host(g);
    host.disc = &disc;
    host.set = set;
    host.stpSet = set;
    host.raceId = raceId;
    host.check = true;
    host.timed = StreamCdTimed();
    CheckCallees cc;
    cc.cd = &host;
    // the drive (stream_cd.h): its clock is the image's game_state+0x0C; a read the image has in flight (the layer
    // busy, gp+0x860 its request) starts over now
    T() = Totals();
    const uint32_t gs0 = g.U32(s::kStGameState);
    T().drive.clock = T().drive.freeAt = static_cast<int64_t>(g.U32(gs0 + 0x0Cu));
    size_t resumed = 0;
    if (host.timed && g.U32(s::kCdBusy) != 0) {
        const uint32_t q = g.U32(kGp + s::kGpCdCur);
        uint32_t v0 = 0;
        const uint32_t seek[3] = {g.U32(q + 4u), g.U32(q + 8u), 0};
        host.Call(s::kCdSeekFn, seek, 3, kStreamSp, v0);
        const uint32_t read[4] = {g.U32(q + 4u), g.U32(q + 16u), g.U32(q + 28u), s::kCdDoneLabel};
        host.Call(s::kCdReadFn, read, 4, kStreamSp, v0);
        resumed = 1;
    }
    size_t fails = 0, compared = 0;
    std::string lines;
    Held prev = HeldOf(g);
    for (size_t k = 1; k < files.size(); ++k) {
        if (!ReadImage(files[k], image)) {
            lines += "  " + files[k] + ": not a 2 MiB image\n";
            ++fails;
            continue;
        }
        GuestRam want(image.data(), kGp);
        const Held w = HeldOf(want);
        // the entity the stream record follows takes the image's road coordinate (+0x168..+0x173)
        const uint32_t e = g.U32(s::kStStates + 4u), we = want.U32(s::kStStates + 4u);
        for (uint32_t o = 0x168u; o < 0x174u; o += 4u) g.W32(e + o, want.U32(we + o));
        const uint32_t frames = (w.clock - prev.clock) / 5u;
        for (uint32_t f = 0; f < frames; ++f) {
            const uint32_t gsf = g.U32(s::kStGameState);
            g.W32(gsf + 0x0Cu, g.U32(gsf + 0x0Cu) + 5u); // a vertical blank (0x8001B700)
            if (host.timed) host.Advance(static_cast<int64_t>(g.U32(gsf + 0x0Cu)));
            if (mutate) { // the control: the release pass skipped - nothing is ever freed
                for (uint32_t p = 0; p < g.U32(g.U32(s::kStGameState) + 0x30u); ++p) {
                    s::StreamSelect(g, p);
                    s::StreamTrack(g);
                    s::StreamSeek(g);
                    s::StreamFreeCount(g);
                }
                s::StreamRead(g, kStreamSp, cc);
                s::ResPump(g, kStreamSp, cc);
            } else {
                s::StreamFrame(g, kStreamSp, cc);
            }
        }
        const Held o = HeldOf(g);
        const bool steady = w.records.size() >= 28u;
        size_t miss = 0, extra = 0;
        std::string names;
        Compare(w.pieces, o.pieces, miss, extra, names);
        Compare(w.cells, o.cells, miss, extra, names);
        Compare(w.records, o.records, miss, extra, names);
        const bool ok = miss == 0 && (!steady || extra == 0) && !g.Faulted();
        ++compared;
        if (!ok) ++fails;
        char b[400];
        std::snprintf(b, sizeof(b),
                      "  %s: %u frame(s) to road %u along %d dir %d; console pieces %zu cells %zu records %zu (%s), ours %zu / "
                      "%zu / %zu: %zu missing, %zu extra",
                      fs::path(files[k]).filename().string().c_str(), frames, w.road, static_cast<int32_t>(w.along),
                      static_cast<int32_t>(w.dir), w.pieces.size(), w.cells.size(), w.records.size(),
                      steady ? "table full" : "refilling", o.pieces.size(), o.cells.size(), o.records.size(), miss, extra);
        lines += b;
        lines += ok ? "\n" : " - MISMATCH:" + names + "\n";
        prev = w;
    }
    char head[600];
    std::snprintf(head, sizeof(head),
                  "streamcheck%s %s: set %d race %d, %zu image(s) after the first compared, %zu mismatch(es); callees not "
                  "run (the texture / music / population sides): %zu; the CD %s: %zu read(s) completed (%zu resumed from "
                  "the first image), read time max %lld ticks\n",
                  mutate ? "-mutate" : "", dir.c_str(), set, raceId, compared, fails, cc.notRun,
                  host.timed ? "TIMED (the ported layer, the 2x drive)" : "INSTANT (RRJB_CD=instant)", T().cdDone, resumed,
                  static_cast<long long>(T().cdLatencyMax));
    report = head + lines;
    return fails == 0;
}

// SLUS 0x8002305C, the first step of GameFrame.
void RaceSession::StreamFrameStep() {
    GuestRam g(arena_.Ram(), kGp);
    ProductStreamCallees c(g);
    c.disc = streamDisc_;
    c.Files(FilesOf(g, world_.set, world_.raceId));
    c.timed = StreamCdTimed();
    c.pop = [this](uint32_t fn, const uint32_t* a, uint32_t sp, uint32_t& v0) { return StreamPopCall(fn, a, sp, v0); };
    c.seam = [this](const std::string& line) { NoteSeam(line); };
    c.speech = [this](uint32_t rec, uint32_t id, uint32_t index, uint32_t& v0, std::vector<std::pair<uint32_t, uint32_t>>& done) {
        return sounds_.StreamSpeechBank(rec, id, index, v0, done);
    };
    RecordWatch w;
    w.Before(g);
    static const bool trace = std::getenv("RRJB_STREAM_TRACE") != nullptr; // DEVELOPMENT: where a frame stops
    if (trace) std::fprintf(stderr, "stream frame %u begin\n", streamFrames_ + 1), std::fflush(stderr);
    // THE DRIVE: the reads over by now completed (their interrupt ran during the previous frame)
    if (c.timed) c.Advance(static_cast<int64_t>(g.U32(g.U32(s::kStGameState) + 0x0Cu)));
    s::StreamFrame(g, kStreamSp, c);
    // the SPU uploads of the frame's speech banks over: 0x80016464 and the buffer release 0x80030FA0
    c.Uploads([this](uint32_t arg, const std::function<bool(uint32_t)>& release) { return sounds_.StreamSpeechDone(arg, release); });
    if (s::StreamStall(g) != 0) ++T().stallFrames;
    if (g.U16(g.U32(s::kStGameState) + 0x28u) != 0) ++T().stall28;
    if (trace) std::fprintf(stderr, "stream frame %u end\n", streamFrames_ + 1), std::fflush(stderr);
    w.After(g);
    ++streamFrames_;
    if (g.Faulted()) {
        g.ClearFault();
        NoteSeam("the stream frame SLUS 0x8002305C (PORTED) met a fault");
    }
}
// The sky's pick in the draw (RASHCDG 0x80064B9C -> 0x800650D0(view 0), PORTED stream_cd.h), after the cell draw
// list has moved the release-list cursor gp+0x87C the pick reads.
void StreamSkyPass(GuestRam& g, uint32_t frame) {
    if (!StreamSkyPorted() || g.U32(s::kStSkyPtr) == 0) return;
    ProductStreamCallees c(g);
    s::SkyView(g, 0, kStreamSp, c);
    if (g.Faulted()) {
        g.ClearFault();
        return;
    }
    Totals& t = T();
    ++t.skyPasses;
    const int32_t shown = g.S16(g.U32(s::kStSkyPtr) + 0x12u);
    if (shown < 0) ++t.skyNone;
    if (shown != t.skyShown) {
        if (t.skyShown != -2) ++t.skyChanges;
        t.skyShown = shown;
        uint32_t id = 0;
        StreamSkyShown(g, id);
        char b[80];
        std::snprintf(b, sizeof(b), " f%u slot%d id%u @%u:%d;", frame, shown, id, g.U32(s::kStStates + 0x48u),
                      g.S32(s::kStStates + 0x44u));
        t.skySeries += b;
    }
}

bool StreamSkyShown(GuestRam& g, uint32_t& id) {
    id = 0;
    const uint32_t sky = g.U32(s::kStSkyPtr);
    if (!StreamSkyPorted() || sky == 0) return false;
    const int32_t shown = g.S16(sky + 0x12u);
    if (shown < 0 || shown >= 20) return false;
    const uint32_t key = g.U32(sky + 0x2E4u + 12u * static_cast<uint32_t>(shown) + 8u);
    if (key == 0) return false;
    id = key & 0x0FFFFFFFu;
    return true;
}

} // namespace rr::game
