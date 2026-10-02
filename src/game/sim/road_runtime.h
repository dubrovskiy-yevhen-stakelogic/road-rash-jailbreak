#pragma once
// The runtime road layer - pass H of the per-bike step, the road re-bind, the road classifier and
// the progress scalar - ported from the resident executable `SLUS_010.53`, SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text base 0x80010000.
//
// This file carries the addresses line by line. Each function is accepted by its own row of `rrverify phys`
// (tools\rrverify\rows_road_runtime.inc): 0 mismatches over the whole guest RAM outside the stack
// window and the whole scratchpad, on dump-derived and randomised inputs.
//
// The memory model is road_query.h's GUEST-ADDRESS VIEW (`GuestRam`), unchanged: these functions
// follow the road cursor, the resident object's seven block pointers, the route records, the finish
// record, the rider definition, the passenger and the scratchpad list, and which pointer is chased
// next depends on what the last one held.
//
// STACK. Every function takes `sp`, the value of the stack pointer at its entry, and computes its
// own frame exactly as the original's prologue does (`F = sp - frame`). Locals whose ADDRESS the
// original hands to a callee (RoadClassify's 32-byte cursor, ProgressScalar's cursor and turn
// direction, RoadProbeAhead's cursor) live at the original's frame offsets, because the callee
// reads and writes them there. Dead locals the original writes and nothing reads (SegmentStraddle's
// four spills, RoadProject's and DotLcm's spill, RoadReseat's per-arm differences, RoadsideZones'
// period table copy) are written too, because a later function at the same depth can read stale
// stack and the cost is one store each. Callee-saved register spills are NOT
// written: their values are the caller's registers, which a port does not have.
//
// THE SIX UNDER THE RE-BIND. RoadClassCore, NodeWedge, RoadsideRunNode, RouteBindFirst,
// RouteBindStep and RoadShortcut are reached through
// `RoadRuntimeCallees`, one method per function, each given the stack pointer at which the original
// makes the call. They are PORTED (the section "under the re-bind" below, each with
// its own bench row) and `RoadRuntimeNative` implements the interface with the ports - that is what
// the bench rows and the product use. The interface stays so a caller can still ask the oracle for
// one of them (the bench's negative controls do) or refuse one (`RoadRuntimeRecorder`). A caller that
// passes the original a fifth argument stores it at `sp + 16` itself, as the original caller does.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- globals
constexpr uint32_t kRoadListHead      = 0x1F800000; // scratchpad: end pointer of the active list
constexpr uint32_t kRoadListFirst     = 0x1F800004; // scratchpad: the first entity pointer
constexpr uint32_t kRouteRecordCount  = 0x800D6182; // s16, -1 = no route records
constexpr uint32_t kRouteFinishPtr    = 0x800D6188; // -> the finish record {road, along, dir, key}
constexpr uint32_t kZonePeriodTable   = 0x80010DFC; // u32[6] {15,15,15,7,1,1}, copied by 0x8003A9D8
constexpr uint32_t kZoneJumpTable     = 0x80010E14; // u32[8], 0x8003A9D8's `jr v0` at 0x8003AB78

// ---------------------------------------------------------------------------- the seams
class RoadRuntimeCallees {
public:
    virtual ~RoadRuntimeCallees() = default;
    // SLUS 0x8003E754 RoadClassCore(e, node, out, cursorOut, zone): zone is the fifth o32 argument
    // (sp+16), put there by 0x8003EED8. Called only from RoadClassNode.
    virtual uint32_t RoadClassCore(GuestRam& m, uint32_t e, uint32_t node, uint32_t out,
                                   uint32_t cursorOut, int32_t zone, uint32_t sp) = 0;
    // SLUS 0x8003EB58 NodeWedge(point, node, armA, armB, hint): hint at sp+16 (0x8003DDF8).
    virtual uint32_t NodeWedge(GuestRam& m, uint32_t point, uint32_t node, uint32_t armA,
                               uint32_t armB, int32_t hint, uint32_t sp) = 0;
    // SLUS 0x8003F204 RoadsideRunNode(e, zone).
    virtual uint32_t RoadsideRunNode(GuestRam& m, uint32_t e, int32_t zone, uint32_t sp) = 0;
    // SLUS 0x8003B024 RouteBindFirst(p, e, pool, x) and 0x8003B1C4 RouteBindStep(p, pool, e, x).
    virtual uint32_t RouteBindFirst(GuestRam& m, uint32_t p, uint32_t e, uint32_t pool, uint32_t x,
                                    uint32_t sp) = 0;
    virtual uint32_t RouteBindStep(GuestRam& m, uint32_t p, uint32_t pool, uint32_t e, uint32_t x,
                                   uint32_t sp) = 0;
    // SLUS 0x8003BFE8 RoadShortcut(e).
    virtual uint32_t RoadShortcut(GuestRam& m, uint32_t e, uint32_t sp) = 0;
};

// A host that wants to refuse the six instead of running `RoadRuntimeNative`: every request is
// refused and remembered. A caller that sees `Declined()` after a call MUST throw away everything that call did
// (restore its copy of the memory it handed in) - the functions above ran on past the refusal with
// 0 for the missing callee's answer and without its stores, which is NOT what the console does.
class RoadRuntimeRecorder final : public RoadRuntimeCallees {
public:
    uint32_t RoadClassCore(GuestRam&, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override { return Refuse(0x8003E754); }
    uint32_t NodeWedge(GuestRam&, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override { return Refuse(0x8003EB58); }
    uint32_t RoadsideRunNode(GuestRam&, uint32_t, int32_t, uint32_t) override { return Refuse(0x8003F204); }
    uint32_t RouteBindFirst(GuestRam&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { return Refuse(0x8003B024); }
    uint32_t RouteBindStep(GuestRam&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { return Refuse(0x8003B1C4); }
    uint32_t RoadShortcut(GuestRam&, uint32_t, uint32_t) override { return Refuse(0x8003BFE8); }
    bool Declined() const { return first_ != 0; }
    uint32_t FirstDeclined() const { return first_; }
    void Clear() { first_ = 0; }

private:
    uint32_t Refuse(uint32_t address) {
        if (first_ == 0) first_ = address;
        return 0;
    }
    uint32_t first_ = 0;
};

// ---------------------------------------------------------------------------- the leaves
// 0x8003E67C, 216 bytes: the edge class of `lateral` in the half of cross-section group `group` that
// `side` selects (2: the right half at +136, anything else the left at +8). Writes the class and the
// sub-id as two halfwords at `out`, returns 1 when off the road.
int32_t RoadEdgeClass(GuestRam& m, uint32_t group, int32_t lateral, int32_t side, uint32_t out);
// 0x8003DCB8, 248 bytes: 1 when the entity's box centre lies between the two end points of roadside
// segment `r` along the slice tangent (XZ only).
int32_t SegmentStraddle(GuestRam& m, uint32_t e, uint32_t r, uint32_t sp);
// 0x8003F1F0, 20 bytes: e[+0x1EC] = e[+0x154] + 0x2C; returns that value.
uint32_t RoadsideRunOfSlice(GuestRam& m, uint32_t e);
// 0x8003B4B0, 112 bytes, over guest addresses: the 16-byte leg of route record `o` (count at +0x0C,
// legs at +0x14) whose first word is `key`, or 0; 0 when no route records exist. The same function
// as `RouteFindLeg` (race.h, host view, index answer).
uint32_t RouteFindLegView(GuestRam& m, uint32_t o, uint32_t key);
// 0x80036800, 380 bytes, over guest addresses: lateral and along of point `p` on `slice`. The same
// function as `RoadProject` (vec.h, host view); each output is written only if its pointer is set.
void RoadProjectView(GuestRam& m, uint32_t p, uint32_t slice, uint32_t lateralOut,
                     uint32_t alongOut, uint32_t sp);
// 0x8002EAD8, 160 bytes, over guest addresses, one component at a time as the original does.
void MulAddView(GuestRam& m, uint32_t base, uint32_t dir, int32_t t, uint32_t out);

// ---------------------------------------------------------------------------- the classifier
// 0x8003EF34, 700 bytes: the cross-section lookup. Writes the output block `out` = e + 0x174
// - including +0x184 bits 0..3, bit 0 = OFF THE ROAD - and returns 1 when it
// searched the sub-object's XSIH run.
int32_t RoadCrossSection(GuestRam& m, uint32_t cursor, uint32_t pos, uint32_t out);
// 0x8003EE68, 204 bytes: the node arm of the classifier.
uint32_t RoadClassNode(GuestRam& m, uint32_t e, uint32_t out, uint32_t cursorOut, int32_t zone,
                       uint32_t sp, RoadRuntimeCallees& calls);
// 0x8003DE28, 300 bytes: the classifier dispatch (e, mode, cursorOut, zone) - FOUR arguments.
uint32_t RoadClass(GuestRam& m, uint32_t e, int32_t mode, uint32_t cursorOut, int32_t zone,
                   uint32_t sp, RoadRuntimeCallees& calls);
// 0x8003DDB0, 120 bytes: the junction wedge the entity is in, or -1.
int32_t NodeZone(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls);
// 0x8003DF54, 160 bytes: the roadside-run pointers +0x1EC / +0x1F0. Returns the callee's v0.
uint32_t RoadsideRun(GuestRam& m, uint32_t e, int32_t mode, int32_t zone, uint32_t sp,
                     RoadRuntimeCallees& calls);
// 0x8003662C, 468 bytes: +0x168 / +0x16C / +0x170 at `out` from `cursor` and `heading`.
void RoadPosition(GuestRam& m, uint32_t heading, uint32_t cursor, uint32_t out, uint32_t sp);
// 0x8003DFF4, 348 bytes: classify one entity. Returns the original's v0, (-r) & 0x40.
uint32_t RoadClassify(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls);

// ---------------------------------------------------------------------------- progress and route
// 0x8003BE1C, 460 bytes: the junction sub-object a route turn of crossing length `len` onto road
// `road` uses, and its direction in `*dirOut` (a guest address), or 0.
uint32_t TurnSubObject(GuestRam& m, uint32_t cursor, int32_t len, int32_t road, uint32_t dirOut);
// 0x8003B61C, 728 bytes: the progress scalar e[+0x144], `p` = e + 0xAC.
int32_t ProgressScalar(GuestRam& m, uint32_t p, uint32_t sp);
// 0x8003AF9C, 136 bytes: the route-binding dispatch. Returns the callee's v0 (0 for p == 0, where
// the original leaves its caller's v0 - not a value a port can know).
uint32_t RouteBind(GuestRam& m, uint32_t p, int32_t step, uint32_t x, uint32_t sp,
                   RoadRuntimeCallees& calls);
// 0x8003A9D8, 1100 bytes: the roadside zones, entity header +0x24 bits 1..9. Returns the
// original's v0 (the new +0x24 on the "no run" exit, 0 otherwise).
uint32_t RoadsideZones(GuestRam& m, uint32_t e, uint32_t sp);

// ---------------------------------------------------------------------------- tracking
// 0x8003701C, 232 bytes: the entity's own slice search and road position. Returns the new +0x184.
uint32_t RoadTrack(GuestRam& m, uint32_t e, uint32_t sp);
// 0x800396A8, 864 bytes: re-seat the cursor onto the nearest junction arm. Returns the original's v0.
uint32_t RoadReseat(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls);
// 0x80037104, 564 bytes: the probe point ahead, and the passenger. Returns the original's v0.
uint32_t RoadProbeAhead(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls);

// ---------------------------------------------------------------------------- pass H and the re-bind
// The four list walks over the scratchpad list 0x1F800004 .. *(0x1F800000). The view must have the
// scratchpad attached (`GuestRam::SetScratchpad`). Each returns the original's v0, 0.
uint32_t RoadTrackPass(GuestRam& m, uint32_t sp, RoadRuntimeCallees& calls);   // 0x80037338, 280 bytes
uint32_t RoadClassPass(GuestRam& m, uint32_t sp, RoadRuntimeCallees& calls);   // 0x8003E150, 152 bytes
uint32_t RouteCheckPass(GuestRam& m, uint32_t sp, RoadRuntimeCallees& calls);  // 0x8003AE24, 376 bytes
uint32_t ProgressPass(GuestRam& m, uint32_t sp);                               // 0x8003B520, 252 bytes
// 0x80037450, 132 bytes: the whole per-entity road update; returns e[+0x184] bit 6.
uint32_t RoadRebindBody(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls);
// 0x800374D4, 80 bytes: the re-bind (optional re-seat, then the body); returns e[+0x184] bit 6.
uint32_t RoadRebind(GuestRam& m, uint32_t e, int32_t flag, uint32_t sp, RoadRuntimeCallees& calls);

// ============================================================================ under the re-bind
// Every function returns the original's `v0`
// (the rows compare it). Two of them can leave the CALLER's `v0` untouched on one exit - a value no
// port can know - so they take it as `callerV0`: RouteBindFirst with p == 0 (RouteBind never passes
// that) and RoadsideRunNode with e == 0 (its one caller, 0x8003DFB0, holds -1 there).
// Two more ported leaves are used here under the names road_query.h gave them:
// `RouteLegOfRoad` is 0x8003F3B4 RouteRecordByKey (its argument is a node id at every call site)
// and `RouteLegHasRoad` is 0x8003F580 RouteHasExit.

constexpr uint32_t kRoadPool1Ptr = 0x8005B3A4; // -> pool-1 slot 0 (the riders), stride 628
constexpr uint32_t kRaceGraphGp  = 472;        // gp+472: the resident STREAM<n>.GRF (magic RGTS)

// 0x800245DC / 0x800245F4, 24 / 28 bytes: race-graph road (16 bytes) / node (40 bytes) by id, no
// bounds check: *(gp+472)+0x18 + 16 id, *(gp+472)+0x14 + 40 id.
uint32_t GraphRoad(GuestRam& m, uint32_t id);
uint32_t GraphNode(GuestRam& m, uint32_t id);
// 0x8003A5F4, 268 bytes: the distance from pos = {key, dir, along} to one end node of its road on
// the race graph; the node id (only a junction of degree >= 3, else -1) goes to `nodeOut`. Mode 0
// takes the NEARER end, mode 1 the start when dir < 0, others the end.
int32_t RoadEndNode(GuestRam& m, uint32_t pos, uint32_t nodeOut, int32_t mode);
// 0x8003F4D8, 168 bytes: the first of the `count` route records with a leg on `road`, or 0.
uint32_t RouteRecordOfRoad(GuestRam& m, uint32_t road);
// 0x8003B024, 416 bytes: the first route binding p[+0x100] (p = e + 0xAC; `e` is the pool-0 entity
// or 0, `x` an ENTITY whose record is adopted, or 0).
uint32_t RouteBindFirst(GuestRam& m, uint32_t p, uint32_t e, uint32_t pool, uint32_t x,
                        uint32_t callerV0);
// 0x8003B1C4, 748 bytes: advancing the binding. `x` is never read. The node word of RoadEndNode
// lives at the original's sp+16, so the function needs `sp`.
uint32_t RouteBindStep(GuestRam& m, uint32_t p, uint32_t pool, uint32_t e, uint32_t x, uint32_t sp);
// 0x8003BD2C, 240 bytes, a leaf: the turn sub-object for the ordered road pair (from, to) at the
// cursor's junction; its GPDT direction to *dirOut and the PDT_ record to *turnOut (the original's
// fifth argument, read from sp+16 - the caller stores it).
uint32_t TurnByRoads(GuestRam& m, uint32_t cursor, int32_t from, int32_t to, uint32_t dirOut,
                     uint32_t turnOut);
// 0x8003E61C, 96 bytes: mid(0x10000 - t, a) + mid(t, b); the first product's lo/hi spilled to
// sp-8 / sp-4 (dead stores the original makes; written).
uint32_t Lerp16(GuestRam& m, uint32_t t, uint32_t a, uint32_t b, uint32_t sp);
// 0x8003E45C, 448 bytes: the core's edge class (4 / 1 / 0) of `lat` between groups gA and gB at
// fraction f; four stack arguments (sideA, sideB, dir, out). Returns 1 off the road.
int32_t CoreEdgeClass(GuestRam& m, uint32_t gA, uint32_t gB, uint32_t f, int32_t lat, int32_t sideA,
                      int32_t sideB, int32_t dir, uint32_t out, uint32_t sp);
// 0x8003ED14, 340 bytes: the core's blended cross-section into `out` (= e + 0x174); five stack
// arguments (gB, lat, f, out, dir). Returns 0.
uint32_t CoreCrossSection(GuestRam& m, uint32_t gA, uint32_t xsih, int32_t dA, int32_t dB, uint32_t gB,
                          int32_t lat, uint32_t f, uint32_t out, int32_t dir, uint32_t sp);
// 0x8003EB58, 444 bytes: the wedge between two adjacent arms of BIT_ record `node` that `point` is
// in, or -1; the arm pointers to *armA / *armB when set. `hint` is the fifth argument (sp+16). The
// distances live at the original's sp+16.. and the turned arm vector at sp+32.. (memory, as the
// original: a fifth arm's distance overwrites the vector). THE NOT-FOUND EXIT returns n - 1 with
// armA = arm n, one past the last.
int32_t NodeWedge(GuestRam& m, uint32_t point, uint32_t node, uint32_t armA, uint32_t armB,
                  int32_t hint, uint32_t sp);
// 0x8003E754, 1028 bytes: the classifier on a junction core. `zone` is the fifth
// argument (sp+16). Spills a1/a2 to the caller's home slots sp+4/sp+8 and reads them back from
// there. NO NULL CHECK on TurnByRoads' answer: with 0 it reads guest 0x0C, 0x08 and 0x12 - main RAM,
// the kernel's low memory - through the view exactly as the console does.
uint32_t RoadClassCore(GuestRam& m, uint32_t e, uint32_t node, uint32_t out, uint32_t cursorOut,
                       int32_t zone, uint32_t sp, RoadRuntimeCallees& calls);
// 0x8003F204, 432 bytes: the node's one roadside-zone entry at e+0x1F4.
uint32_t RoadsideRunNode(GuestRam& m, uint32_t e, int32_t zone, uint32_t sp, RoadRuntimeCallees& calls,
                         uint32_t callerV0);
// 0x8003BFE8, 1092 bytes: off the road within 50 units of a junction, re-bind to a nearer arm road.
// Its 32-byte cursor at sp+16 is NOT reset between arms: the lateral and the
// words +0x18/+0x1C carry over from the previous arm's search.
uint32_t RoadShortcut(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls);

// The six, answered by the ports above. What the bench rows and the product pass as `calls`.
class RoadRuntimeNative final : public RoadRuntimeCallees {
public:
    uint32_t RoadClassCore(GuestRam& m, uint32_t e, uint32_t node, uint32_t out, uint32_t cursorOut,
                           int32_t zone, uint32_t sp) override;
    uint32_t NodeWedge(GuestRam& m, uint32_t point, uint32_t node, uint32_t armA, uint32_t armB,
                       int32_t hint, uint32_t sp) override;
    uint32_t RoadsideRunNode(GuestRam& m, uint32_t e, int32_t zone, uint32_t sp) override;
    uint32_t RouteBindFirst(GuestRam& m, uint32_t p, uint32_t e, uint32_t pool, uint32_t x,
                            uint32_t sp) override;
    uint32_t RouteBindStep(GuestRam& m, uint32_t p, uint32_t pool, uint32_t e, uint32_t x,
                           uint32_t sp) override;
    uint32_t RoadShortcut(GuestRam& m, uint32_t e, uint32_t sp) override;
};

} // namespace rr::sim
