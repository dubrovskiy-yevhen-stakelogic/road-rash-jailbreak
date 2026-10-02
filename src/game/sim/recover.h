#pragma once
// The rider off the bike and back on it - the fall, the tumble on the ground, the walk back to the
// bike (AI op 18) and the animated re-seat - ported from our own disassembly of the player's images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// Each function is accepted only by its own row of
// `rrverify phys` (tools\rrverify\rows_recover*.inc): 0 mismatches over the whole guest RAM outside
// the stack window, on the default seed and on `--seed 0xA5A5C3C312345678 --cases 2048`.
//
// This header is the COMMON part: the memory model, the call seam and the leaves every file of the
// domain shares. The functions themselves are declared in recover_walk.h (the walk back and the
// re-seat), recover_fall.h (the fall off the bike) and recover_ground.h (the rider on the ground).
//
// THE MEMORY MODEL is road_query.h's guest-address view (`GuestRam`), as population.h: every function
// takes guest addresses and walks the records through them.
//
// STACK. As in road_runtime.h: every function with a frame takes `sp`, the stack pointer at its entry,
// computes its own frame `F = sp - frame` exactly as its prologue does, keeps the locals whose ADDRESS
// it hands to a callee at the original's frame offsets (in guest memory), and hands every callee the
// `sp` the original makes that call at (F). Callee-saved register spills are not written.
//
// THE CALL SEAM. Every `jal` to a function that is not one of the pure leaves below goes through
// `RecoverCallees::Call(fn, args, n, sp, v0)` with the callee's ADDRESS: the bench answers it with the
// original code (and compares the call sequence with the guest's), the product with the native ports
// (src\game\recover_race.cpp). So each row proves exactly one function's own body.
//
// THE PURE LEAVES run natively inside the ports (the bench does not declare them): FixMul, FixDiv,
// ApproxLen3, Rand (GuestRand), Asin, RatAtan2, RatTan, DotLcm, MulAdd, MulAdd32, Scale, Normalize,
// Normalize32, SqrtGte, Length3, SumSquares, ScaleTo16, CopyHalfwords SLUS 0x8003FA18, the word copy
// SLUS 0x8001E0B4, the GTE OP, the road-record lookups (RoadNodeRecord, RoadNodeArm, RouteLegOfRoad,
// RouteFindLegView), BikeResetOrientation 0x8008CF74, and this domain's own leaves AxisRotation,
// VecMat and AngleBetween (each with its own row).
//
// Faults. A load or store the console would not survive is not performed; `GuestRam` records the
// first such address and the caller must FAIL the call. Nothing here guesses a value.
#include <cstdint>
#include <initializer_list>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- globals
constexpr uint32_t kRcGameStatePtr = 0x8005B2F8; // -> game_state; +0x10 clock, +0x30 players, +0x39 phase
constexpr uint32_t kRcStanceTable  = 0x800541D4; // SLUS: 8-byte stance records, +2 the stance's kind
constexpr uint32_t kRcViewArray    = 0x800CD898; // view records, stride 1132
constexpr uint32_t kRcViewStride   = 1132;
constexpr uint32_t kRcAnimDesc     = 0x800CE170; // the rider animation descriptor (+8 used, +12 capacity)
constexpr uint32_t kRcPoolTable    = 0x800CE4D0; // pool 0: +0 base, +4 stride, +12 -> high index
constexpr uint32_t kRcPlayer1Bike  = 0x8005B38C; // player 1's bike
constexpr uint32_t kRcSinCos       = 0x8005624C; // SLUS: 4096 x {s16 sin; s16 cos}

// The engine's tables out of guest memory (the arena and the bench's clone both hold the executable
// image): sincos 0x8005624C, asin 0x800527E0, atan 0x8005285C, rsqrt *(gp+2260), sqrt 0x800560CC.
// `ram` is the base of the 2 MiB image the view `g` is over.
BikeTables RecoverTables(uint8_t* ram, GuestRam& g);

// ---------------------------------------------------------------------------- the call seam
struct RecoverCallees {
    virtual ~RecoverCallees() = default;
    // Run the function at guest address `fn` with the o32 arguments `a[0..n)` (the first four in
    // registers, the rest at sp+16..), `sp` the stack pointer the original makes the call at. `v0` is
    // the callee's return value (0 when it returns none). False: the caller could not run it - the port
    // then returns false and nothing it wrote is to be trusted.
    virtual bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) = 0;
};

namespace rc {
inline bool Call(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp,
                 uint32_t* v0 = nullptr) {
    uint32_t a[12] = {};
    int n = 0;
    for (uint32_t x : args)
        if (n < 12) a[n++] = x;
    uint32_t r = 0;
    if (!c.Call(fn, a, n, sp, r)) return false;
    if (v0 != nullptr) *v0 = r;
    return true;
}
constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
// The handle test `(h >> 5) == 1 && (h & 31) < players` (a player's RIDER, pool 1).
bool IsPlayerRider(GuestRam& g, uint32_t h);
// `h < players` (a player's BIKE, pool 0), sltu.
bool IsPlayerBike(GuestRam& g, uint32_t h);
// The GTE OP (sf = 1, lm = 0) over guest halfword triples: out = sat16((d x ir) >> 12).
void GteOp(GuestRam& g, uint32_t d, uint32_t ir, uint32_t out);
// SLUS 0x8002E468 Normalize(v) in place over guest halfwords. Returns false where the console raises
// its overflow exception (nothing written then); `sum` is the v0 the original returns, x*x + y*y + z*z.
bool GNormalize(GuestRam& g, uint32_t v, const BikeTables& t, int32_t& sum);
// SLUS 0x8003FA18 CopyHalfwords(n, src, dst).
void CopyHalfwords(GuestRam& g, int32_t n, uint32_t src, uint32_t dst);
// The reciprocal idiom `0x80000000 / ((|x| >> 1) + ((|x| - 2) >> 31))` (divu), negated for x < 0.
int32_t Recip(int32_t x);
} // namespace rc

} // namespace rr::sim
