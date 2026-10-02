#pragma once
// The product side of the AI domain: what race_session.cpp calls through its
// small hooks. The ported code itself lives in src\game\sim\ai*.{h,cpp}.
#include <cstdint>
#include <functional>
#include <string>

#include "rrformats/chunk.h"
#include "rrvfs/disc_image.h"
#include <vector>

namespace rr::game {

// DEVELOPMENT CHECK (rrgame --aiglobalscheck <ram.bin | dir>): every destination of GLOBALS.BI's
// loader (sim/ai_globals.h) in a captured RAM image equals the file's bytes under that image's own
// game_state; a directory checks every `ram.bin` below it and every `*.bin` in it. `mutate` takes the
// profile one row off and must FAIL on every image.
bool CheckAiGlobalsArena(const DiscImage& disc, const std::string& path, std::string& report, bool mutate);

// What only the session can do for the AI's ported code: the stance event (its PORTED StanceLayer with
// the session's rider seams) and naming a seam in the run's list.
struct AiProductHooks {
    std::function<bool(uint32_t ev, uint32_t rider, uint32_t p)> stanceEvent;
    std::function<void(const std::string&)> seam;
    // The op-16 arm, FightUpdate RASHCDG 0x800C035C (PORTED, fight_session.cpp): unset = named seam.
    std::function<bool(uint32_t e, uint32_t target)> fight;
    // Op 9's strike, Strike RASHCDG 0x800C1370(e, target bike) (PORTED, sim\strike.h): unset = named seam.
    std::function<bool(uint32_t e, uint32_t target)> strike;
    // SLUS 0x800138E8 ComputePlace(bike, mode) - the session's PORTED one on its route bindings.
    std::function<bool(uint32_t bike, int32_t mode, int32_t* place)> computePlace;
    // The op-18 arm, RiderRecover RASHCDG 0x80092E04, and op 1's release arm for a climbing rider, ClimbDone
    // RASHCDG 0x80092AD4 (both PORTED, recover_race.h): unset = named seams.
    std::function<bool(uint32_t e, int32_t dt, uint32_t sp)> leaveRace;
    std::function<bool(uint32_t e, uint32_t sp)> copRelease;
    // SLUS 0x8001A760 RiderSpeech(h, crash) at `sp` - AiChooseCommand's taunt before a fight (PORTED,
    // speech_session.cpp): unset = named seam.
    std::function<bool(uint32_t h, int32_t crash, uint32_t sp)> riderSpeech;
    // RASHCDG 0x80092C7C EndRace(e, how) at `sp` - the arrest's (cop_race.h; PORTED, the session's).
    std::function<bool(uint32_t e, int32_t how, uint32_t sp)> endRace;
    // The player cop (race_modes.cpp, modes.h): Arrest RASHCDG 0x80096F30(e, t, how) for the cop-mode scan
    // and JailRelease 0x800A0708(e) for CopIdle's quota arm, both PORTED, at `sp`. Unset: named seams.
    std::function<bool(uint32_t e, uint32_t t, uint32_t how, uint32_t sp)> arrest;
    std::function<bool(uint32_t e, uint32_t sp)> jailRelease;
    // Jailbreak: op 2's arm for player 1 in phases 1 / 2, JailbreakFinish RASHCDG 0x800C9E74 (PORTED, jail_session.cpp).
    std::function<bool(uint32_t sp)> jailbreakFinish;
    // The arrest's busted music SLUS 0x8001B3C8 (PORTED, takedown_product.h): unset = named seam.
    std::function<bool()> bustedMusic;
};

// Per frame: how many of each thing the AI passes did (rrgame's frame log).
struct AiPassCounts {
    int unported = 0;          // calls of an unported callee answered "no effect" (named in the seam list)
    int pops = 0, aims = 0;    // AiPopCommand, SetAimDelta calls
    int pushes = 0;            // AiPushCommand calls (planner and brain)
    int byOp[20] = {};         // top opcodes the command pass dispatched (19 = >= 19)
    int copTails = 0, copStops = 0, copIntercepts = 0, arrests = 0; // the police (cop_race.h)
};

// RASHCDG 0x800BA4CC, the command pass (sim/ai_cmd.h AiCommandPass) on the arena `ram` (2 MiB, guest
// layout) at stack `sp`. Every arm runs: 2, 5..9 and the tests native, 4 the PORTED AiCmdRace, the pops,
// the aim slides and the stance event PORTED; the cops' 0x800BAA2C / 0x8009E444 / 0x800BBEBC PORTED
// (cop_race.h); 0x80092AD4 / 0x80092E04 / 0x800C035C / the strike 0x800C1370 / the jailbreak finish 0x800C9E74
// through the hooks (PORTED); an unset hook answers with no effect (v0 = 0), named.
bool RunAiCommandPass(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t skip, uint32_t maskB, uint32_t sp,
                      const AiProductHooks& hooks, AiPassCounts& counts);

// RASHCDG 0x800B8018, the planner (sim/ai_plan.h AiPlan) on the arena, `acc` as RaceTick passes it. Its
// feasibility tests 0x800BC1EC / 0x800BBD44 are the PORTED ai_cmd.h ones, the command stack and the
// stance event PORTED, ComputePlace the session's; the cop tail 0x8009DC90, the cop-mode arrest scan
// 0x80097388 and the rider voice SLUS 0x8001A760 answer with no effect, named.
bool RunAiPlan(uint8_t* ram, uint32_t gp, int32_t acc, uint32_t sp, const AiProductHooks& hooks,
               AiPassCounts& counts);

// RASHCDG 0x800BCD48, the brain pass (sim/ai_brain.h AiBrainPass) on the arena: AiBrain with the
// spatial query over the collision pass's grid; its two tests 0x800BB448 / 0x800BBBB8 the PORTED
// ai_cmd.h ones; push / pop / aim PORTED. Nothing unported.
bool RunAiBrainPass(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t skip, uint32_t maskC, uint32_t sp,
                    const AiProductHooks& hooks, AiPassCounts& counts);

} // namespace rr::game

namespace rr::game {

// DEVELOPMENT CONTROL (rrgame --autosteer): a SCRIPTED STEERING PLAYER - ours, at the pad only. Each
// frame it holds Left or Right from the player bike's own road cursor, as a person holding the bike
// near the centre line would: the lateral offset +0x158 in the travel sense, damped by its change since
// the last frame; when the bike faces more than 60 degrees off the road's sense (the heading +0x1C2
// against the slice's tangent) it turns back toward it off the throttle. The travel sense is +0x16C as it
// was when the bike entered its current road (+0x168), so a spin cannot flip it. The simulation sees
// nothing but the pad bits. It exists so that a scripted run can ride the route to the finish and the
// field's finishing order can be measured.
struct AutoSteer {
    int32_t prevLat = 0;
    bool have = false;
    uint32_t road = 0xFFFFFFFFu;
    int routeDir = 1;
    int sense = -1;           // which of Left / Right reduces the offset (-1: Left when u > 0, measured)
    int turnSense = 1;        // which of Left / Right turns the heading toward the road (measured)
    // With the race's assembled route (world.cpp's `path`, the renderer's): pure pursuit of the route
    // point 30 units ahead of the nearest one, so a junction takes the route's branch.
    const std::vector<rr::RoadSlice>* path = nullptr;
    size_t hint = 0;
    // --autosteer-lane L: pursue a line L world units beside the route (+ = right of the travel direction):
    // a lane rather than the centre line, where some roads keep median poles (road 11 of race 30)
    double lane = 0.0;
    // frames on which a pole (a pool-6 volume or a pole prop, as solid_product.cpp lists them) lay in the
    // corridor to the pursuit point and the script aimed beside it instead (a player steers round a pole;
    // the pure pursuit alone rode into one on 1/1 and pushed against it for the rest of the race)
    size_t poleDodges = 0;
    // -1 = hold Left, +1 = hold Right, 0 = neither. `ram` = the arena (2 MiB, guest layout), `entity` the
    // player's pool-0 address. `throttle` / `brake`: what to hold on the other two controls.
    int Decide(const uint8_t* ram, uint32_t entity, bool& throttle, bool& brake);
};

} // namespace rr::game

namespace rr::game {

// The rider records and the two AI index maps are loaded by grid_loader.h LoadRiderRecords (the PORTED
// RASHCDI 0x80064C0C / 0x80066E1C / 0x800650A0 and SpawnBike's record stores); --aiglobalscheck still
// compares the byte mapping of LEVEL<n>.BI with the captures, --ridercheck the whole loader.

} // namespace rr::game
