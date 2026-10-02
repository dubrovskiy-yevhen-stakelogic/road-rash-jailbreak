#pragma once
// The product side of the rider-recovery domain: the ported rider
// fall, the rider on the ground, the walk back (AI op 18) and the re-seat (src\game\sim\recover*.h),
// run on the session's guest arena. What race_session.cpp calls through its small hooks.
//
// `ProductRecover` answers the ports' call seam (rr::sim::RecoverCallees) natively: this domain's own
// ports, the ported functions of the other domains (Remount, EndRace, the stance layer and the animation
// machine, the road layer, the camera's two resets, the AI stack, the ground query, BuildObbAlt ...),
// each on the same arena. Two callees are not ported and are answered as NAMED seams: the rider-off
// voice SLUS 0x80018440 (a sound; no effect, v0 = 0) and 0x800CA05C (Jailbreak phase 2 only: refused).
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "game/sim/recover.h"
#include "game/sim/stance.h"

namespace rr::game {

struct RecoverProductHooks {
    // The session's rider seams (pose, combat, knock-off / launch children) - the stance layer and the
    // animation machine run with them, as everywhere else in the session.
    rr::sim::RiderSeams* riderSeams = nullptr;
    // The engine's tables (the session's: the reciprocal-square-root table is not behind *(gp+2260) in
    // the arena). Null: read out of the arena (recover.h RecoverTables).
    const rr::sim::BikeTables* tables = nullptr;
    // The value root counter 2 reads for EffectBurst's jitter (the session's own counter).
    std::function<uint32_t()> rootCounter;
    // SLUS 0x80017BA0 PlaySound3D(x, z, id, bank) - the session's PORTED emitter in its sound runtime.
    std::function<void(int32_t x, int32_t z, int32_t id, int32_t bank)> sound;
    std::function<void(const std::string&)> seam;
    // Jailbreak: JailBoard RASHCDG 0x800CA05C(late) at `sp` (PORTED, jail_session.cpp): unset = refused.
    std::function<bool(uint32_t late, uint32_t sp)> jailBoard;
};

// Per-run tallies for the frame log and the end-of-run summary.
struct RecoverCounts {
    std::map<uint32_t, uint64_t> calls;    // native calls by guest address
    std::map<uint32_t, uint64_t> refused;  // refusals by guest address
};

class ProductRecover final : public rr::sim::RecoverCallees {
public:
    ProductRecover(uint8_t* ram, uint32_t gp, const RecoverProductHooks& hooks, RecoverCounts& counts);
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override;

    rr::sim::GuestRam& g() { return g_; }
    const rr::sim::BikeTables& t() const { return t_; }
    uint8_t* ram() { return ram_; }
    bool Refuse(uint32_t fn, const char* why);
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p);

private:
    uint8_t* ram_;
    rr::sim::GuestRam g_;
    rr::sim::BikeTables t_;
    const RecoverProductHooks& hooks_;
    RecoverCounts& counts_;
};

// ---- the entry points (each on the arena `ram`, gp `gp`, at the stack pointer the original's caller
// makes the call at). False: a port or a callee refused; the arena may be partly written (the caller
// names it in the seam list).

// RASHCDG 0x80092E04, AI op 18 (the command pass's arm).
bool RunRiderRecover(uint8_t* ram, uint32_t gp, uint32_t e, int32_t dt, uint32_t sp, const RecoverProductHooks& h,
                     RecoverCounts& counts);
// RASHCDG 0x80092AD4, op 1's arm for a rider with +0x228 bit 6 (the climb).
bool RunClimbDone(uint8_t* ram, uint32_t gp, uint32_t B, uint32_t sp, const RecoverProductHooks& h,
                  RecoverCounts& counts);
// RASHCDG 0x8008F068, the world pass's child after the per-bike step: every rider off its bike.
bool RunRiderOffPass(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t sp, const RecoverProductHooks& h,
                     RecoverCounts& counts);
// The rider pass's rider loop [0x8007DCEC, 0x8007DDD4): per pool-0 slot, the list migration 0x80071BCC
// when flagsC & 0x08001800 (`migrate`, the session's PORTED BikeListMigrate), then RASHCDG
// 0x8008F404(R, dt) on the bike's rider when it is off the bike (mount >= 2) and live, and on a
// passenger's the same way.
bool RunRiderGroundLoop(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t sp, const RecoverProductHooks& h,
                        RecoverCounts& counts, const std::function<bool(uint32_t e)>& migrate);
// One callee by address (RiderLaunch's seams, Remount's re-face): the same dispatch.
bool RunRecoverCall(uint8_t* ram, uint32_t gp, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp,
                    const RecoverProductHooks& h, RecoverCounts& counts, uint32_t* v0 = nullptr);

// The name of a function this domain dispatches, for the logs.
std::string RecoverFnName(uint32_t fn);

} // namespace rr::game
