// rrverify phys - the bit-exact bench for the native bike physics.
//
// One row per ported guest function. Each row runs the ORIGINAL function inside the R3000A+GTE
// interpreter on a real captured machine state, then runs our C++ on a clone of the same memory,
// and compares:
//
//   * the return value in v0,
//   * every byte of the 2 MiB of guest RAM outside the 4 KiB stack window both runs used,
//   * every byte of the 1 KiB scratchpad.
//
// A row passes only with 0 mismatches over BOTH input families:
//
//   * dump-derived - world positions, orientation matrices and speeds read out of the 18 live
//     motorcycles of every RAM image the project has (the four savestates plus the 14 mid-race RAM
//     dumps of work\oracle\vr_capture\ramdumps). These are the numbers the original really saw.
//   * randomised  - full-range 32-bit words and full-range int16 vectors. A function that only
//     agrees on the captured state is memorised, not ported, and this half is what says so.
//
// Development tool only; nothing here is linked into the game.
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/sim/ai.h"
#include "game/sim/bike.h"
#include "game/sim/fixed.h"
#include "game/sim/input.h"
#include "game/sim/integrator.h"
#include "game/sim/bike_step.h" // rows_bike_step.inc
#include "game/sim/crash.h"     // rows_crash.inc
#include "game/sim/ground.h"    // rows_ground.inc, and bike_ground_frame's query
#include "game/sim/stance.h"    // rows_anim.inc (with anim.h)
#include "game/sim/pose.h"      // rows_pose.inc
#include "game/sim/present.h"   // rows_pose.inc
#include "game/sim/population.h" // rows_population.inc
#include "game/sim/ai_plan.h"    // rows_ai_plan.inc
#include "game/sim/ai_brain.h"   // rows_ai_brain.inc
#include "game/sim/traffic.h"        // rows_traffic.inc (and its sub-files below)
#include "game/sim/traffic_leaves.h"
#include "game/sim/traffic_bind.h"
#include "game/sim/traffic_drive.h"
#include "game/sim/police.h"
#include "game/sim/camera.h"     // rows_camera.inc
#include "game/sim/collision.h"  // rows_collision.inc
#include "game/sim/contact.h"      // rows_coll.inc (and bike_react.h, hit_speed.h, impact_solve.h, resolvers.h)
#include "game/sim/bike_react.h"
#include "game/sim/hit_speed.h"
#include "game/sim/impact_solve.h"
#include "game/sim/resolvers.h"
#include "game/sim/hud.h"        // rows_hud.inc
#include "game/sim/ai_cmd.h"     // rows_ai.inc
#include "game/sim/fight.h"      // rows_fight.inc
#include "game/shell/shell_logic.h" // rows_shell.inc
#include "game/sim/race.h"
#include "game/sim/spine.h"      // rows_spine.inc
#include "game/sim/effects.h"    // rows_fx.inc
#include "game/sim/road.h"
#include "game/sim/road_query.h"
#include "game/sim/road_runtime.h"
#include "game/sim/sound.h"
#include "game/sim/sound_engine.h"
#include "game/sim/sound_frame.h" // rows_engine_note.inc
#include "game/sim/speech.h"      // rows_speech.inc (the riders' voices)
#include "game/sim/recover_walk.h"   // rows_recover.inc
#include "game/sim/recover_fall.h"   // rows_recover.inc
#include "game/sim/recover_ground.h" // rows_recover.inc
#include "game/sim/recover_air.h"    // rows_recover.inc
#include "game/sim/rider_record.h"   // rows_riders.inc (the race loader's rider records)
#include "game/sim/camera_collide.h" // rows_feel.inc
#include "game/sim/pad_reader.h"     // rows_feel.inc
#include "game/sim/ped_hit.h"        // rows_feel.inc
#include "game/sim/camera_director.h" // rows_feel.inc
#include "game/sim/bike_parts.h"      // rows_feel.inc
#include "game/sim/weapon.h"         // rows_weapon.inc
#include "game/sim/cops.h"           // rows_cops.inc
#include "game/sim/modes.h"          // rows_modes.inc
#include "game/sim/split_view.h"     // rows_mp.inc (two players)
#include "game/sim/model_draw.h"     // rows_model.inc
#include "game/sim/cell_draw.h"      // rows_vis.inc
#include "game/sim/subdiv.h"         // rows_subdiv.inc
#include "game/sim/world_pop.h"  // rows_world.inc
#include "game/sim/jail.h"       // rows_jail.inc (the Jailbreak mode)
#include "game/sim/view_pass.h"      // rows_vis.inc
#include "game/sim/solid.h"          // rows_solid.inc
#include "game/sim/hazard.h"         // rows_hazards.inc (the hazard objects)
#include "game/sim/partners.h"       // rows_partners.inc
#include "game/sim/junction.h"       // rows_junction.inc
#include "game/sim/takedown.h"       // rows_takedown.inc (the takedown, rider-off sound, busted music)
#include "game/sim/grid_build.h"     // rows_grid.inc (the race loader's starting grid)
#include "game/sim/loader.h"         // rows_loader.inc
#include "game/sim/camera_setup.h"   // rows_loader2_cam.inc (the camera set-up, the light stores)
#include "game/sim/spu_heap.h"       // rows_loader2_spu.inc (libspu's SPU-RAM allocator)
#include "rrformats/level_bundle.h"   // rows_loader.inc: the effect sheet's section
#include "game/sim/peds.h"           // rows_peds.inc (the pedestrians)
#include "game/sim/passes.h"         // rows_passes.inc
#include "game/sim/strike.h"         // rows_strike.inc (op 9's strike)
#include "game/sim/race_over.h"      // rows_strike.inc (the audio pause SLUS 0x80018C1C)
#include "game/sim/pause.h"          // rows_pause.inc
#include "game/sim/loader2.h"        // rows_loader2.inc (BuildRace, SetUpRace's children)
#include "game/sim/route_parse.h"    // rows_route.inc (the route block parser, the road-map loader)
#include "game/sim/fx_glow.h"        // rows_fxdraw.inc (the emitter's glow sprites)
#include "game/sim/anim_objects.h"  // rows_animobj.inc (the animation objects' initialiser and hand-out)
#include "game/sim/stream_cd.h"      // rows_stream2.inc (the CD layer, the sky pick, the cell textures)
#include "game/sim/stream.h"         // rows_stream.inc
#include "game/sim/shadow.h"         // rows_look.inc (the model shadow)
#include "game/sim/mp_world.h"       // rows_mp2.inc (two players: the cell copy, the class-50 release)
#include "game/sim/shadow2p.h"       // rows_mp2.inc (two players: the shadow 0x80026960)
#include "game/sim/cell_sort.h"      // rows_cells.inc
#include "game/sim/stream_files.h"   // rows_stream3.inc
#include "game/sim/sky_draw.h"       // rows_sky2.inc
#include "game/sim/sky_split.h"      // rows_sky3.inc
#include "game/sim/pad_motor.h"       // rows_rumble.inc
#include "game/sim/countdown_voice.h" // rows_rumble.inc
#include "game/sim/vec.h"
#include "interp/r3000.h"
#include "interp/snapshot.h"
#include "interp/trace.h"

namespace {

using rr::interp::Cpu;
using rr::interp::Memory;
using rr::interp::Trap;
using rr::interp::TrapKind;

// ---------------------------------------------------------------- addresses
// All in the resident executable SLUS_010.53, SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1,
// text base 0x80010000 (file offset 0x800). Every one of them was read out of our own disassembly
// and then confirmed by a live call trace of one real race frame.
constexpr uint32_t kFixMul      = 0x8001FC90;
constexpr uint32_t kFixDiv      = 0x80010028;
constexpr uint32_t kApproxLen3  = 0x8001FCB0;
constexpr uint32_t kRand        = 0x8001FC58;
constexpr uint32_t kDotLcm      = 0x8002E698;
constexpr uint32_t kVecMulAdd   = 0x8002EAD8;
constexpr uint32_t kVecMulAdd32 = 0x8002E570; // the 16.16-direction form, 35 instructions, no callees
constexpr uint32_t kVecScale    = 0x8002EE50;
constexpr uint32_t kRoadProject = 0x80036800;
constexpr uint32_t kRatAtan2    = 0x80020018;
constexpr uint32_t kNormalize   = 0x8002E468;
constexpr uint32_t kBlend32     = 0x8002E6F8;
constexpr uint32_t kBlend16     = 0x8002EB78;
constexpr uint32_t kBlend16To32 = 0x8002ECB8;
constexpr uint32_t kFindRoadPiece = 0x80039C90;
// Race overlay RASHCDG.BIN, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, resident in the
// rr-race snapshot at 0x8005B5E8, so these are called with nothing swapped in.
constexpr uint32_t kClampLerpMin = 0x80074FB4;
constexpr uint32_t kImpactDir    = 0x80075B08;
constexpr uint32_t kBikeIdleStep = 0x80075BE4;
constexpr uint32_t kBuildObb     = 0x8008BA18;
constexpr uint32_t kSteerLean    = 0x80073D74;
constexpr uint32_t kSolveSteer   = 0x80074170;
constexpr uint32_t kHalfCopy     = 0x8003FA18;
constexpr uint32_t kRiderPose    = 0x800807F0;
constexpr uint32_t kBuildObbAlt  = 0x8008BD2C;
constexpr uint32_t kAltKindTable = 0x800541D4; // 8-byte records, read by 0x8008BDAC
constexpr uint32_t kApplySteering = 0x80074570;
constexpr uint32_t kAimTarget     = 0x80072FB4;
constexpr uint32_t kSteerDriver   = 0x80073874;
constexpr uint32_t kAiTable       = 0x800D7128; // 192-byte records, read at 0x80073914/0x8007399C
constexpr uint32_t kGameStatePtr  = 0x8005B2F8; // -> game_state (rules.md 1.1)
constexpr uint32_t kAxisCurve     = 0x8001CA58;
constexpr uint32_t kAxisCfg       = 0x800D38E0; // ENV.EN; +0xB0 dead zone, +0xB2 segment width
constexpr uint32_t kHandleTable   = 0x800CE540; // 8-byte records indexed by the entity handle,
                                                // read at 0x8007351C (`lw a2,4(v1)`)
constexpr uint32_t kScale32      = 0x8002E810;
constexpr uint32_t kRatTan       = 0x8001FEB4;
constexpr uint32_t kSinCosTable  = 0x8005624C; // 4096 x {s16 sin; s16 cos}, read by 0x8001FEBC
constexpr uint32_t kEngineStep   = 0x80079B20; // RASHCDG, the engine and drivetrain
constexpr uint32_t kPlaySound3D  = 0x80017BA0; // its one unported callee, called at 0x8007A6C0
constexpr uint32_t kAtanU16Table = 0x800527E0; // 61 x u16, read by 0x8007AB34 (4096 = one turn)
// The engine indexes its stat block with `(revs - floor) / step`, which nothing bounds, so the
// native side is handed a window around the block rather than the block alone. See the row.
constexpr uint32_t kStatsWindowBack = 65536;
constexpr uint32_t kStatsWindowFwd  = 65536;
constexpr uint32_t kCrashTimer   = 0x80074E6C; // RASHCDG, the per-bike crash timer
constexpr uint32_t kCrashLut     = 0x80027028; // SLUS, its ported callee
constexpr uint32_t kCrashEmit    = 0x80027540; // SLUS, its oracle-supplied one (3 arguments)
constexpr uint32_t kCrashTable   = 0x800537D8; // 6 bytes per entity class, read by 0x80027048
constexpr uint32_t kCrashWindow  = 0x800D3974; // the timer's ceiling, read by 0x80074EA0

// ---------------------------------------------------------------- the opponent AI (A)
// RASHCDG unless marked SLUS. Every one of these was read out of our own disassembly of the player's
// own images.
constexpr uint32_t kAiProject      = 0x800B6AAC; // lateral/longitudinal projection
constexpr uint32_t kAiHandleInList = 0x800A8C48; // packed-handle membership
constexpr uint32_t kAiNibbleAdd    = 0x800BD34C; // clamp-nibble add
constexpr uint32_t kAiNibbleDecay  = 0x800BCEEC; // nibble decay toward rest
constexpr uint32_t kAiClearCmds    = 0x800BCD10; // AiClearCommands
constexpr uint32_t kAiPushCmd      = 0x800BCA68; // AiPushCommand
constexpr uint32_t kAiStance       = 0x800C4550; // the stance event, 3 args, at 0x800BCCE8. PORTED
                                                  // (stance.h); rows that still declare it do so
                                                  // as a WITNESS of the call (AnimNativeStanceEvent)
constexpr uint32_t kFightRecPtr    = 0x8005AD4C; // -> the 12-byte FIGHT.BIN records
constexpr uint32_t kMemSet32       = 0x8001E100; // SLUS, the game's own word-wise memset
constexpr uint32_t kSumSquares     = 0x8002F0F4; // SLUS
constexpr uint32_t kSqrtGte        = 0x8004CF74; // SLUS, the GTE-assisted square root
constexpr uint32_t kSqrtTable      = 0x800560CC; // SLUS, its 16-bit table (index NOT bounded)
constexpr uint32_t kLength3        = 0x8002E548; // SLUS
constexpr uint32_t kSetAimDelta    = 0x80093CAC; // SetAimDelta
constexpr uint32_t kAiCmdRace      = 0x800BA7F4; // the command-4 handler
constexpr uint32_t kAiRaceFlags    = 0x8005AD48; // the word its gate reads (bits 0 and 2)
// AiTargetSpeed and the globals it reaches through. Every one of these was read out of the
// function's own `lui`/`lw` pairs, not assumed.
constexpr uint32_t kAiTargetSpeed  = 0x80095BF8;
constexpr uint32_t kPlayer1Ptr     = 0x8005B38C; // -> player 1's bike (not 0x800CB38C)
constexpr uint32_t kPlayer2Ptr     = 0x8005B21C; // -> player 2's bike
constexpr uint32_t kPlayerArr      = 0x8005B268; // -> player p's bike, stride 4
constexpr uint32_t kJailbreakPtr   = 0x800CCC18; // -> the bike the Jailbreak arm reads +0x1E0 from
constexpr uint32_t kLiveBikes      = 0x8005B1F8;
constexpr uint32_t kBikeCap        = 0x8005B1FC;
constexpr uint32_t kSpreadDiv      = 0x8005B244;
constexpr uint32_t kAltFlag        = 0x8005B2B0;
constexpr uint32_t kCopScale       = 0x800D86F8;
constexpr uint32_t kClockStamp     = 0x800CCA84;
constexpr uint32_t kSpreadBand     = 0x800CCA88;
constexpr uint32_t kTabSpeedClass  = 0x80052FA0;
constexpr uint32_t kTabThinkA      = 0x80052FD0;
constexpr uint32_t kTabThinkB      = 0x80052FF4;
constexpr uint32_t kTabCopFlat     = 0x80053030;
constexpr uint32_t kTabAltA        = 0x8005303C;
constexpr uint32_t kTabAltB        = 0x80053054;
constexpr uint32_t kTabSpeedCap    = 0x80053138;
constexpr uint32_t kTabCopBase     = 0x80053144;
constexpr uint32_t kTabCopStep     = 0x80053150;
constexpr uint32_t kTabCopMul      = 0x8005315C;
// The drive pass and the two functions it reaches besides AiDrive.
constexpr uint32_t kAiRecoverLine  = 0x800BD388; // the off-the-line correction, 83 instructions
constexpr uint32_t kAiPopCmd       = 0x800BC8DC; // AiPopCommand, recursive, 99 instructions
constexpr uint32_t kAiDrive        = 0x800954A0; // AiDrive itself
constexpr uint32_t kAiDrivePass    = 0x800BA304; // the drive pass
// SLUS 0x800386DC, the road look-ahead. SEVEN o32 arguments, three of them on the stack, and the
// seventh is a 32-byte output buffer in the CALLER's own frame (`AiDrive` keeps it at its sp+32 and
// reads the slice pointer back out of its +0x0C). It is PORTED with its whole tree
// (src\game\sim\road_query.cpp, rows `road_*` below) and `ai_drive` / `ai_drive_pass` call the port.
constexpr uint32_t kProfile        = 0x800531AC; // the chosen 60-byte difficulty profile
constexpr uint32_t kProfileWindow  = 4096;       // its index is unbounded - hand a window
// The command pass and its dispatch table.
// ---------------------------------------------------------------- the race spine
//
// The per-frame spine of a race. `RaceTick` is the one call the whole simulation hangs off; its six
// children are the whole frame and none of them is ported, so all six - plus the AI planner and the
// "GO" event - are supplied by the oracle. Every address below was read out of our
// own disassembly of RASHCDG.BIN, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c.
constexpr uint32_t kTickCountdown = 0x8008AD38; // RASHCDG, 348 bytes
constexpr uint32_t kGoEvent       = 0x80090270; // RASHCDG, 388 bytes, NO arguments, no `jal`
constexpr uint32_t kRaceTick      = 0x8008AB00; // RASHCDG, 384 bytes
constexpr uint32_t kAiPlan        = 0x800B8018; // RASHCDG, called from 0x8008AB64 and 0x8008AC20
constexpr uint32_t kRaceDirector  = 0x800B9414; // child 1
constexpr uint32_t kSpawnerPass   = 0x8008CD88; // child 2
constexpr uint32_t kWorldBikePass = 0x8008AC80; // child 3
constexpr uint32_t kCollisionPass = 0x800A4774; // child 4
constexpr uint32_t kRiderPass     = 0x8008ACE8; // child 5
constexpr uint32_t kPresentPass   = 0x80090814; // child 6
constexpr uint32_t kCountdownWord = 0x8005B230; // 16.16 s, counts DOWN before the start
constexpr uint32_t kEventAcc      = 0x8005B30C; // the periodic accumulator, read at 0x8008AB14
constexpr uint32_t kPlanCount     = 0x8005B2A8; // the AI planner pass counter
constexpr uint32_t kPlanTable     = 0x80052FAC; // SLUS; its index is NOT bounded - hand a window
constexpr uint32_t kPlanWindow    = 4096;
constexpr uint32_t kRaceTickFrame = 32;         // `addiu sp,sp,-32` at 0x8008AB00
constexpr uint32_t kTickCountdownFrame = 24;    // `addiu sp,sp,-24` at 0x8008AD40

constexpr uint32_t kRaceStep      = 0x80012524; // SLUS, 296 bytes
constexpr uint32_t kRaceStepFrame = 32;         // `addiu sp,sp,-32` at 0x8001252C
constexpr uint32_t kViewUpdate    = 0x800881B4; // RASHCDG, the camera, 10212 bytes, 2 arguments
constexpr uint32_t kViewPostTest  = 0x800A421C; // RASHCDG, the `view+0x224 & 0x100` arm
constexpr uint32_t kViewPostApply = 0x80086E1C; // RASHCDG, ... and what it runs on a true answer
constexpr uint32_t kViewArray     = 0x800CD898; // View[2], stride 1132
constexpr uint32_t kViewStride    = 1132;
constexpr uint32_t kFrameFlag     = 0x8005B580; // zeroed once per frame at 0x80012584

constexpr uint32_t kProgressPass  = 0x800B9794; // RASHCDG, 452 bytes
constexpr uint32_t kProgressFrame = 48;         // `addiu sp,sp,-48` at 0x800B9794
constexpr uint32_t kFinishTest    = 0x800B9958; // RASHCDG, 2476 bytes, its one callee
constexpr uint32_t kRiderStride   = 628;        // pool 1, BuildGrid 0x80067BD4

constexpr uint32_t kDirectorFrame = 72;         // `addiu sp,sp,-72` at 0x800B941C
constexpr uint32_t kRiderRemount  = 0x800903F4; // RASHCDG, the fallen-rider arm, 2 arguments
constexpr uint32_t kResultsPrep   = 0x8003F708; // SLUS, no arguments
constexpr uint32_t kRaceOverSig   = 0x80018C1C; // SLUS, 1 argument
constexpr uint32_t kAiBrainPass   = 0x800BCD48; // RASHCDG, the third AI pass
constexpr uint32_t kPartnerCmd    = 0x800C035C; // RASHCDG, the two-rider command arm
constexpr uint32_t kPartnerFinish = 0x80092E04; // RASHCDG, the two-rider finish arm
constexpr uint32_t kPostLimit     = 0x8005B228; // 5.00 s in 16.16
constexpr uint32_t kSkipResults   = 0x8005B220; // non-zero -> state 2 instead of state 6

// ---------------------------------------------------------------- the finish test and EndRace.
// Every address read out of our own disassembly of
// RASHCDG.BIN, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, and of SLUS_010.53.
constexpr uint32_t kFinishFrame      = 72;          // `addiu sp,sp,-72` at 0x800B9958
constexpr uint32_t kMilestoneAdvance = 0x800C8D4C;  // RASHCDG, no arguments, a 5-arm jump table
constexpr uint32_t kMilestoneFirst   = 0x800C92F8;  // RASHCDG, no arguments
constexpr uint32_t kResultStamp      = 0x800BC7CC;  // RASHCDG, 1 argument (the entity)
constexpr uint32_t kRecordFinish     = 0x8003F680;  // SLUS, 2 arguments (handle, flag)
constexpr uint32_t kRoadAdvance      = 0x800394F0;  // SLUS, 2 arguments, the road-graph step
constexpr uint32_t kMilestoneTable   = 0x80053174;  // SLUS, 4-byte records {u16 road; u16 along}
constexpr uint32_t kMilestoneBytes   = 1024;        // the index is gameState[+0x39], a whole byte
constexpr uint32_t kFinishOrder      = 0x800D5DA8;  // 16-byte records indexed by place - 1
constexpr uint32_t kFinishOrderBytes = 320;         // 20 records; the original bounds it by nothing
constexpr uint32_t kRouteRecordPtr   = 0x800D6188;  // -> {u32 road; s32 line; s32 side; s32 leg}
constexpr uint32_t kTimeLimit        = 0x8005ACC8;  // the race time limit, ticks
constexpr uint32_t kTimeBase         = 0x8005ACD0;  // ... and the tick it is measured from
constexpr uint32_t kStartDir         = 0x8005B2E8;  // the milestone's travel direction, +-1
constexpr uint32_t kJailbreakClock   = 0x800D9C4C;  // stamped with the race clock on the bit-2 arm
constexpr uint32_t kPlayerArrHigh    = 0x8005B26C;  // *(0x8005B268 + 4), the second player's bike

// ---------------------------------------------------------------- the ground frame of the
// per-bike step. RASHCDG, SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c.
constexpr uint32_t kGroundFrame   = 0x8007504C; // 1500 bytes, 375 instructions, no `jalr`
constexpr uint32_t kGroundQuery   = 0x800A7BF8; // its callee, 2208 bytes, 5 arguments: PORTED (ground.h)
constexpr uint32_t kGroundFrameSz = 120;        // `addiu sp,sp,-120` at 0x8007504C
// The three output buffers the original keeps in that frame, and what the callee writes into each.
// `a2` is a 3 x 16.16 point and `a3` an s16[3] normal; the lengths are read out of the transform's
// own reads of them (0x800751B4 / 0x80075114) rather than out of the callee.
constexpr uint32_t kGroundSlotA   = 24;         // sp+24, used by the FIRST and the THIRD call
constexpr uint32_t kGroundSlotB   = 40;         // sp+40, the second call's point
constexpr uint32_t kGroundSlotC   = 72;         // sp+72, the third call's normal
constexpr uint32_t kGroundPointBytes  = 12;
constexpr uint32_t kGroundNormalBytes = 6;
// A fixed fill for all three slots, written into the GUEST's frame by the row's setup and into the
// port's buffers by its native side. The original's frame locals are uninitialised and its callee
// may return -1 without writing them (`0x800A8460`), so without this the two sides would read two
// different patches of old stack and the row would be comparing noise. With it, an unwritten buffer
// is identical on both sides - and a row that only ever saw unwritten buffers would prove nothing
// about them, which is why the family drives the query deliberately.
inline uint8_t GroundSlotFill(uint32_t slotOffset, uint32_t i) {
    return static_cast<uint8_t>(0x37u * (slotOffset + i) + 0x5Au);
}

// ---------------------------------------------------------------- the sound emitter.
// Ten functions, 2164 bytes, ALL in SLUS_010.53 and all resident on this machine. Every
// address below was read out of our own disassembly of the player's own image, SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1.
constexpr uint32_t kSoundKeyOn   = 0x8001EB28; // 28 bytes: S[+0x14] |= a0
constexpr uint32_t kSoundKeyOff  = 0x8001EB44; // 28 bytes: S[+0x18] |= a0
constexpr uint32_t kLookupSound  = 0x8001E86C; // 80 bytes (not 36 - see the row)
constexpr uint32_t kAllocVoice   = 0x8001F9C4; // 404 bytes
constexpr uint32_t kSound3DParams = 0x80019E40; // 640 bytes, NINE o32 arguments
constexpr uint32_t kStartVoice   = 0x8001F174; // 520 bytes, five o32 arguments
constexpr uint32_t kSetListener  = 0x80016768; // 60 bytes, SIX o32 arguments
constexpr uint32_t kSurfaceSound = 0x80017B30; // 60 bytes, a leaf
constexpr uint32_t kQueueListenerSound = 0x80017B6C; // 52 bytes, a leaf
constexpr uint32_t kSoundSys      = 0x800D6870; // the sound-system record
constexpr uint32_t kSoundSysBytes = 0x188;
constexpr uint32_t kSoundMaster   = 0x800D6C0C; // the 3D master volume, 65 in every capture
constexpr uint32_t kSurfaceTable  = 0x800525C0; // 52 u8, read by SurfaceSound
constexpr uint32_t kGpListener    = 1920;       // *(gp+1920) -> the listener array
constexpr uint32_t kGpDefaultBank = 1912;       // *(gp+1912), substituted for bank 0
constexpr uint32_t kGpMutedBank   = 1940;       // *(gp+1940); a call naming it is dropped
constexpr uint32_t kGpSoundSerial = 2068;       // 0x8005B4A0 - the SOUND-HANDLE SERIAL COUNTER,
                                                // eight bytes below the LCG seed at gp+2076 (and
                                                // easily mistaken for it)
constexpr uint32_t kVoiceBytes    = 44;
constexpr uint32_t kVoiceSlots    = 24;         // `li s0,24` at 0x8001E664
constexpr uint32_t kListenerStride = 72;
constexpr uint32_t kListenerRecords = 4;        // how many records the bench resolves for the port
// The window handed to the port per bank slot. The sound record's offset is a u32 out of game data
// and the original bounds it by NOTHING, so this is a window, not a block.
constexpr uint32_t kBankWindow    = 4096;

constexpr uint32_t kEndRace       = 0x80092C7C; // RASHCDG, 392 bytes
constexpr uint32_t kEndRaceFrame  = 32;         // `addiu sp,sp,-32` at 0x80092C7C
constexpr uint32_t kWipeoutEffect = 0x8002090C; // SLUS, 1 argument
constexpr uint32_t kViewEvent     = 0x8008A998; // RASHCDG, 2 arguments (view, reason)

// Progress and place, a closed tree of four resident functions.
constexpr uint32_t kRouteFindLeg  = 0x8003B4B0; // SLUS, 112 bytes
constexpr uint32_t kRouteValid    = 0x8003B8F4; // SLUS, 120 bytes
constexpr uint32_t kProgressOf    = 0x8003B96C; // SLUS, 60 bytes
constexpr uint32_t kComputePlace  = 0x800138E8; // SLUS, 508 bytes
constexpr uint32_t kRouteArmed    = 0x800D6182; // s16; -1 means "no route is loaded"
constexpr uint32_t kEntRouteObj   = 0x1AC;      // entity[+0x1AC], the route object
constexpr uint32_t kEntRoadPos    = 0x168;      // entity[+0x168], roadId | (kind << 16)
constexpr uint32_t kEntProgress   = 0x144;      // entity[+0x144], 20.12 distance remaining

constexpr uint32_t kAiRunCommands  = 0x800BA4CC;
constexpr uint32_t kArmTable       = 0x8005B9B8; // 19 words; the dispatcher builds this address
                                                 // itself at 0x800BA5B4/0x800BA5B8
constexpr uint32_t kArmNone        = 0x800BA79C; // the do-nothing arm, shared by eight opcodes
constexpr uint32_t kArmRace        = 0x800BA6E0; // the command-4 arm
constexpr uint32_t kPoolBasePtr    = 0x8005B3A0; // -> pool-0 slot 0, stride 1096
constexpr uint32_t kAttackerMask   = 0x800CCAC0; // u16 per player
// The square-root table's index is `(x >> s) - 64`, and a negative `x` - which SumSquares produces
// as soon as the squared length wraps - reads up to 640 bytes BELOW the table. As with the engine's
// stat block the port is handed a window, not the block.
constexpr uint32_t kSqrtWindowHalf = 4096;

constexpr uint32_t kAtanTable   = 0x8005285C; // read by 0x800200E4; 18 words are reachable
constexpr uint32_t kRandSeedGpOffset = 2076;  // 0x8001FC5C: lw v0,2076(gp)
constexpr uint32_t kRsqrtTableGpOffset = 2260; // 0x8002E4B8: lw t3,2260(gp)
constexpr uint32_t kMallocEntry = 0x8001447C; // void *malloc(size_t, int heapId)
constexpr uint32_t kPoolTable   = 0x800CE4D0; // docs\formats\population.md 1
constexpr uint32_t kSentinel    = 0x00000DEC;

// The scratch block every row gets from the game's own allocator: big enough for a whole
// 1096-byte pool-0 entity plus its arguments.
constexpr uint32_t kScratchBytes = 8192;

// Entity fields used as dump-derived input (docs\formats\population.md 1.2).
constexpr uint32_t kEntObb      = 0x0B8; // 8 oriented-bounding-box corners, 3 x 16.16
constexpr uint32_t kEntMatrixA  = 0x1B0; // 3x3 orientation, int16, 4096 = 1.0
constexpr uint32_t kEntWorldPos = 0x1D4; // world position, 3 x 16.16
constexpr uint32_t kEntSpeed    = 0x1E0; // 16.16
constexpr uint32_t kEntMatrixB  = 0x204; // a second 3x3 orientation, int16
constexpr uint32_t kEntLateral  = 0x158; // 16.16 offset from the road centre line
constexpr uint32_t kEntAlong    = 0x170; // 16.16 distance along the road

struct Machine {
    Memory mem;
    Cpu cpu;
    Machine() : cpu(mem) {}
};

struct CallResult {
    Trap trap;
    uint32_t v0 = 0;
    uint64_t steps = 0;
    bool ok() const { return trap.kind == TrapKind::Halted; }
};

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

// A whole-register image of the snapshot's CPU, so a clone can be given the machine's real `gp`
// (and everything else) before it is asked to execute guest code of its own.
struct CpuRegs {
    uint32_t r[32]{};
    uint32_t hi = 0, lo = 0;
    uint32_t gteDr[32]{};
    uint32_t gteCr[32]{};
};
CpuRegs CaptureRegs(Cpu& cpu) {
    CpuRegs s;
    std::memcpy(s.r, cpu.regs, sizeof(s.r));
    s.hi = cpu.hi;
    s.lo = cpu.lo;
    std::memcpy(s.gteDr, cpu.gte().dr, sizeof(s.gteDr));
    std::memcpy(s.gteCr, cpu.gte().cr, sizeof(s.gteCr));
    return s;
}
void RestoreRegs(const CpuRegs& s, Cpu& cpu) {
    std::memcpy(cpu.regs, s.r, sizeof(cpu.regs));
    cpu.hi = s.hi;
    cpu.lo = s.lo;
    std::memcpy(cpu.gte().dr, s.gteDr, sizeof(s.gteDr));
    std::memcpy(cpu.gte().cr, s.gteCr, sizeof(s.gteCr));
}

// ---------------------------------------------------------------- the oracle-supplied callee
//
// Without this, a native port has to be a CLOSED TREE - every callee ported too. A callee cannot
// simply be skipped either: skipping the 3D sound emitter under the engine (0x80079B20), for one,
// changes compared state.
//
// So a row may instead declare a callee as "supplied by the oracle". The port asks for it through
// an interface; this runs the ORIGINAL machine code for it, in the interpreter, ON THE CANDIDATE'S
// OWN CLONE, with the arguments the PORT computed. Three consequences, and they are the whole point:
//
//   * the callee's effects land in the state the bench diffs, so nothing is hidden;
//   * it runs on the candidate's memory, so a wrong argument or a wrong decision by our code still
//     produces a mismatch;
//   * the call itself is recorded on both sides - the guest run through `Cpu::ObserveCalls`, the
//     native run here - and the two sequences must match exactly, so a call our port omits, adds,
//     reorders or mis-argues is a failure even when the callee happens to change nothing.
// An argument that is an OUTPUT BUFFER IN THE CALLER'S OWN FRAME. o32 lets a caller hand a callee
// a pointer to a local of its own to be filled in; `SLUS 0x800386DC`'s seventh argument is exactly
// that, a 32-byte road cursor that `AiDrive` keeps at `sp+32` of its own frame and reads back after
// the call. A native port has no guest frame, so the bench hands the callee a buffer of its own.
// This declaration encodes two consequences:
//   * the POINTER differs between the guest and the port by construction, so it is left out of the
//     argument comparison (comparing it would be comparing two unrelated addresses), and
//   * the CONTENT is compared instead: `bytes` bytes at the guest's pointer against `bytes` bytes
//     at the port's, once per case, after the row's function has returned.
//
// TWO REFINEMENTS, both forced by `RASHCDG 0x800A7BF8`:
//
//   1. A declaration may name SEVERAL arguments of one callee. `0x800A7BF8` fills a 12-byte point
//      through `a2` and a 6-byte normal through `a3`, and its third call site inside
//      `RASHCDG 0x8007504C` keeps BOTH in the caller's own frame. Each capture now remembers which
//      argument it came from (`Cpu::FrameOutCapture::arg`) so the two sides are paired by argument
//      index; before that, one buffer per observation meant the second silently overwrote the first.
//   2. Whether a declared argument really IS a caller-frame buffer is decided PER CALL, by where
//      the pointer points: only when BOTH the guest's and the port's value lie inside the excluded
//      stack window is the pointer left out of the argument comparison. That is exactly the
//      condition under which the two pointers cannot be compared (the two stacks differ by
//      construction) and under which the bytes are invisible to the whole-RAM diff. The same
//      callee's `a3` is the entity address `e+0x10C` at two of `0x8007504C`'s three call sites and
//      a frame local at the third, so a per-callee "always skip it" rule would have thrown away a
//      real argument comparison at two sites out of three. A pointer that is in the window on ONE
//      side only fails the comparison, which is the right answer.
struct OracleFrameOut {
    int index = 0;      // 0-based o32 argument index: 0..3 = a0..a3, 4..7 = sp+16..sp+28
    uint32_t bytes = 0; // how many bytes the callee writes there, read out of its own code
};

struct OracleCallee {
    uint32_t address = 0;
    int arity = 4;       // how many of a0..a3 the callee really takes
    int stackArgs = 0;   // how many further o32 arguments it takes at sp+16 .. sp+44 (up to 8)
    std::vector<OracleFrameOut> frameOuts;
    // Extra stack depth below the row's own `oracleFrame`, for a callee the original reaches
    // through one or more PORTED functions rather than from the row's entry directly. `race_step`
    // needs it: its six children are called by `RaceTick`, which is ported and has a 32-byte frame
    // of its own, and the "GO" event another 24 below that inside `TickCountdown`. Matching the
    // depth makes the guest's and the port's frames coincide; see `Row::oracleFrame`.
    uint32_t frame = 0;
};

// The bench's own slice of the excluded stack window, used only by the seam. The window is
// `sp-4096 .. sp+19` and every address below is inside it, so
// nothing here is compared on either side - which is the point: the guest's equivalents (the
// caller's outgoing-argument area and the caller's frame local) are inside the same window in the
// guest run.
//
//   sp - 512  .. sp - 1     legacy oracle calls (a0..a3 only) still run at `sp`; their frames live
//                           here, exactly as before this extension.
//   sp - 960  .. sp - 513   the caller-frame output buffers, bump-allocated downward from sp-512.
//   sp - 1024 .. sp - 961   the outgoing o32 argument area of a stack-argument oracle call.
//   sp - 1024               the `sp` such a call is made at; the callee's own frame grows down
//                           from here and has 3072 bytes before it leaves the window.
constexpr uint32_t kOracleArgFrameDrop = 1024; // stack-argument calls run at sp - this
constexpr uint32_t kFrameOutTop = 512;         // buffers are allocated downward from sp - this
constexpr uint32_t kFrameOutBytes = 448;       // ... and may not reach the outgoing argument area

struct NativeEnv {
    Machine* clone = nullptr;
    uint32_t scratch = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    std::vector<Cpu::CallObservation> calls; // the calls the PORT made, in order
    std::string failure;
    // The negative control for the seam (`--skip-oracle`): the port's request is swallowed instead
    // of executed. That is exactly the "stub the callee" shortcut the bench rejects, so the row
    // must then FAIL - and its failure is what keeps that rejection reproducible.
    bool skipOracle = false;
    uint32_t frameOutNext = 0; // bump pointer, set up per case
    const std::vector<OracleCallee>* callees = nullptr; // the row's declarations
    uint64_t maxSteps = 5'000'000; // the row's instruction budget, for the calls made through here
    // The negative control for the SPLICE (`--splice-control`): a splice row's
    // hook skips the region instead of running the port on it. Every splice row must then FAIL.
    bool spliceControl = false;

    uint32_t CallOracle(uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
    // The general form: up to twelve o32 arguments, the first four in registers and the rest in the
    // caller's outgoing-argument area. `n > 4` moves the call down to its own frame, because the
    // bench's own `sp+20` and `sp+24` are OUTSIDE the excluded stack window and writing them would
    // show up in the diff as a difference the port did not cause.
    uint32_t CallOracleArgs(uint32_t address, const uint32_t* a, int n);
    // A buffer for an argument the original keeps in its own frame. It lives inside the excluded
    // stack window and is reused across calls of one case, exactly as the original reuses its own
    // frame slot.
    uint32_t FrameOutBuffer(uint32_t bytes);
};

uint32_t NativeEnv::FrameOutBuffer(uint32_t bytes) {
    const uint32_t aligned = (bytes + 7u) & ~7u;
    const uint32_t next = frameOutNext - aligned;
    if (next < sp - kFrameOutTop - kFrameOutBytes) {
        if (failure.empty()) failure = "the seam's caller-frame buffer area is exhausted";
        return 0;
    }
    frameOutNext = next;
    return next;
}

uint32_t NativeEnv::CallOracleArgs(uint32_t address, const uint32_t* a, int n) {
    if (skipOracle) return 0;
    Cpu::CallObservation o;
    o.address = address;
    for (int k = 0; k < n && k < 12; ++k) o.a[k] = a[k];
    uint32_t extraFrame = 0;
    if (callees != nullptr)
        for (const OracleCallee& oc : *callees)
            if (oc.address == address) extraFrame = oc.frame;
    const uint32_t callSp = ((n > 4) ? (sp - kOracleArgFrameDrop) : sp) - extraFrame;
    o.sp = callSp;
    for (int k = 4; k < n && k < 12; ++k)
        clone->mem.PokeWord(callSp + 16u + 4u * static_cast<uint32_t>(k - 4), a[k]);
    // The port's side of the call sequence is recorded by the SAME mechanism as the guest's: the
    // clone carries this row's call observers too, so `pc` reaching a declared entry is what makes
    // an entry, whether our code asked for the call or a callee the oracle is executing made it on
    // its own. That second case is not hypothetical - `RASHCDG 0x800881B4` (the camera) calls
    // `0x80086E1C` twice internally, and `RaceStep` calls the same function directly, so a bench
    // that recorded only the port's explicit requests would compare a 9-call guest sequence against
    // an 8-call port one and call the port wrong. Measured on `race_step` before this was fixed.
    const size_t outer = calls.size();
    clone->cpu.callObservations.clear();
    const CallResult r = CallGuest(clone->cpu, address, {a[0], a[1], a[2], a[3]}, callSp, maxSteps);
    if (clone->cpu.callObservations.empty())
        calls.push_back(o); // the entry was not observed: keep the request itself in the sequence
    else
        calls.insert(calls.end(), clone->cpu.callObservations.begin(),
                     clone->cpu.callObservations.end());
    // The caller-frame output buffers, picked up the moment the callee returned - the same instant
    // the interpreter captures the guest's, and the only instant at which either is still intact.
    // (The interpreter cannot do it for the clone: its `ra` is the bench's sentinel and `Run` stops
    // on it before the pick-up runs, so this stays the bench's job.)
    if (callees != nullptr) {
        for (const OracleCallee& oc : *callees) {
            if (oc.address != address) continue;
            for (const OracleFrameOut& fo : oc.frameOuts) {
                if (fo.index >= n || fo.bytes == 0 || fo.bytes > Cpu::kMaxFrameOut) continue;
                if (outer >= calls.size()) continue;
                Cpu::CallObservation& back = calls[outer];
                if (Cpu::FrameOutCapture* f = back.FrameOutFor(fo.index)) {
                    f->len = fo.bytes;
                    clone->mem.ReadBlock(a[fo.index], f->bytes, fo.bytes);
                }
            }
        }
    }
    if (!r.ok() && failure.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", address);
        failure = std::string("oracle callee ") + buf + " trapped: " + r.trap.ToString();
    }
    return r.v0;
}

uint32_t NativeEnv::CallOracle(uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2,
                               uint32_t a3) {
    const uint32_t a[4] = {a0, a1, a2, a3};
    return CallOracleArgs(address, a, 4);
}

// A deterministic 64-bit PRNG (splitmix64). The bench's own, so a case is reproducible from its
// index alone and the guest run and the native run are given byte-identical inputs.
class Rng {
public:
    explicit Rng(uint64_t seed) : s_(seed) {}
    uint64_t Next() {
        uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t U32() { return static_cast<uint32_t>(Next() >> 32); }
    int32_t S32() { return static_cast<int32_t>(U32()); }
    int16_t S16() { return static_cast<int16_t>(U32() >> 16); }
    // A value in the neighbourhood of the game's own magnitudes: 16.16 world units in +-2^15.
    int32_t World() { return static_cast<int32_t>(U32()) >> (1 + static_cast<int>(Next() % 12u)); }
    // A rotation-matrix-sized term.
    int16_t Unit() { return static_cast<int16_t>(static_cast<int32_t>(U32() % 8193u) - 4096); }

private:
    uint64_t s_;
};

// ---------------------------------------------------------------- dump-derived input

struct Sample {
    int32_t p[3]{};   // world position, 16.16
    int32_t q[3]{};   // a second world position (the first OBB corner)
    int16_t mA[9]{};  // orientation matrix at +0x1B0
    int16_t mB[9]{};  // orientation matrix at +0x204
    int32_t speed = 0;
    int32_t lateral = 0;
    int32_t along = 0;
    std::vector<uint8_t> entity; // the whole 1096-byte pool-0 slot, as the console had it
    std::string origin;
};

int32_t ReadS32(const std::vector<uint8_t>& ram, uint32_t address) {
    const uint32_t off = address & 0x1FFFFFu;
    if (off + 4 > ram.size()) return 0;
    uint32_t v = 0;
    std::memcpy(&v, ram.data() + off, 4);
    return static_cast<int32_t>(v);
}
int16_t ReadS16(const std::vector<uint8_t>& ram, uint32_t address) {
    const uint32_t off = address & 0x1FFFFFu;
    if (off + 2 > ram.size()) return 0;
    uint16_t v = 0;
    std::memcpy(&v, ram.data() + off, 2);
    return static_cast<int16_t>(v);
}

// Reads the 18 live motorcycles (entity pool 0) out of one RAM image.
void HarvestRam(const std::vector<uint8_t>& ram, const std::string& origin, std::vector<Sample>& out) {
    const uint32_t base = static_cast<uint32_t>(ReadS32(ram, kPoolTable));
    const int32_t stride = ReadS32(ram, kPoolTable + 4);
    const uint32_t highPtr = static_cast<uint32_t>(ReadS32(ram, kPoolTable + 12));
    if (base < 0x80000000u || base >= 0x80200000u || stride <= 0 || stride > 4096) return;
    if (highPtr < 0x80000000u) return;
    const int32_t high = ReadS32(ram, highPtr);
    if (high < 0 || high > 63) return;
    for (int32_t slot = 0; slot <= high; ++slot) {
        const uint32_t e = base + static_cast<uint32_t>(slot) * static_cast<uint32_t>(stride);
        Sample s;
        for (int i = 0; i < 3; ++i) {
            s.p[i] = ReadS32(ram, e + kEntWorldPos + 4u * static_cast<uint32_t>(i));
            s.q[i] = ReadS32(ram, e + kEntObb + 4u * static_cast<uint32_t>(i));
        }
        for (int i = 0; i < 9; ++i) {
            s.mA[i] = ReadS16(ram, e + kEntMatrixA + 2u * static_cast<uint32_t>(i));
            s.mB[i] = ReadS16(ram, e + kEntMatrixB + 2u * static_cast<uint32_t>(i));
        }
        s.speed = ReadS32(ram, e + kEntSpeed);
        s.lateral = ReadS32(ram, e + kEntLateral);
        s.along = ReadS32(ram, e + kEntAlong);
        const uint32_t eo = e & 0x1FFFFFu;
        if (eo + static_cast<uint32_t>(stride) <= ram.size()) {
            s.entity.assign(ram.begin() + eo, ram.begin() + eo + static_cast<size_t>(stride));
        }
        s.origin = origin + " slot " + std::to_string(slot);
        out.push_back(s);
    }
}

std::vector<Sample> HarvestSamples(const std::string& stateRoot, const std::string& dumpRoot,
                                   std::string& note) {
    namespace fs = std::filesystem;
    std::vector<Sample> out;
    std::vector<std::string> files;
    std::error_code ec;
    if (fs::is_directory(stateRoot, ec)) {
        for (const auto& d : fs::directory_iterator(stateRoot, ec)) {
            if (!d.is_directory()) continue;
            const fs::path p = d.path() / "ram.bin";
            if (fs::exists(p, ec)) files.push_back(p.string());
        }
    }
    if (fs::is_directory(dumpRoot, ec)) {
        for (const auto& f : fs::directory_iterator(dumpRoot, ec)) {
            if (!f.is_regular_file()) continue;
            if (f.path().extension() == ".bin") files.push_back(f.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    size_t images = 0;
    for (const std::string& f : files) {
        std::vector<uint8_t> ram;
        std::string error;
        if (!rr::interp::ReadWholeFile(f, ram, error)) continue;
        if (ram.size() < 0x200000u) continue;
        const size_t before = out.size();
        HarvestRam(ram, fs::path(f).parent_path().filename().string(), out);
        if (out.size() != before) ++images;
    }
    note = std::to_string(images) + " RAM image(s), " + std::to_string(out.size()) + " live bikes";
    return out;
}

// ---------------------------------------------------------------- the row interface

struct Args {
    std::array<uint32_t, 4> a{{0, 0, 0, 0}};
    // The fifth o32 argument, which the caller leaves at sp+16. Two of the vector helpers take one.
    uint32_t a4 = 0;
    // Arguments six to nine, at sp+20, sp+24, sp+28 and sp+32. `SetListener SLUS 0x80016768` takes
    // six (`vz` at sp+16 and `camYaw` at sp+20) and `Sound3DParams SLUS 0x80019E40` takes nine.
    // The bench writes them into BOTH machines before the call, exactly as it already does for
    // `a4`, so the two runs see byte-identical outgoing argument areas; none of them is inside the
    // excluded stack window (`sp-4096 .. sp+19`), and nothing in the game writes that area - o32
    // only lets a callee spill its incoming `a0..a3` into `sp+0..sp+15`.
    std::array<uint32_t, 4> more{{0, 0, 0, 0}};
    bool operator==(const Args& o) const { return a == o.a && a4 == o.a4 && more == o.more; }
};

// Everything a row needs to build one case. `scratch` is a block the bench allocated with the
// game's own malloc, so it exists at the same address in the guest run and in the clone.
struct CaseContext {
    Memory* mem = nullptr;
    uint32_t scratch = 0;
    Rng* rng = nullptr;
    const Sample* sample = nullptr; // null for a randomised case
    const Sample* any = nullptr;    // always a real captured entity, for rows that need one
    uint32_t gp = 0;
    // The stack pointer the row's function will be called at. Only a row that has to prepare the
    // ORIGINAL's own frame needs it - `bike_ground_frame` fills the three output buffers the
    // original keeps at `sp-120+24/+40/+72` so that an unwritten buffer reads the same on both
    // sides. Everything it writes is inside the excluded stack window.
    uint32_t sp = 0;
    // The machine's SPU control register file (`Cpu::spuControl`) and its constant
    // root-counter value (`Cpu::rootCounterValue`), for a row that has to vary
    // them per case. Both are reset to the snapshot's values before every setup, on both machines,
    // so a row that does not touch them sees exactly what every row saw before they existed.
    uint16_t* spuControl = nullptr;
    uint32_t* rootCounter = nullptr;
};

struct Row {
    const char* name;
    const char* signature;
    uint32_t entry;
    // Writes the inputs into `mem` and fills `args`. Must be a pure function of the context, so the
    // guest run and the native run see byte-identical memory.
    std::function<void(CaseContext&, Args&)> setup;
    // Runs our native C++ against the clone's memory, writing exactly the bytes the guest wrote.
    // Returns the value the guest should have left in v0 (or 0 when the row does not compare v0).
    std::function<uint32_t(Memory&, uint32_t scratch, uint32_t gp, const Args&)> native;
    bool compareV0 = true;
    // The same thing as `native`, for a port that needs the oracle-supplied-callee seam; when it is
    // set it is used instead of `native`. Declared last so that the rows above keep their existing
    // aggregate initialisers.
    std::function<uint32_t(NativeEnv&, const Args&)> nativeEx;
    // Guest entry points this row's port is allowed to have the oracle execute for it. The guest
    // run is watched at the same addresses and the two call sequences must match exactly. `arity`
    // is how many o32 argument registers the callee actually takes, read out of its own
    // disassembly: the original's caller does not set the rest, so they are not part of the call
    // and comparing them would be comparing leftovers.
    std::vector<OracleCallee> oracleCallees;
    // The size of the ORIGINAL function's own stack frame, so that the port's oracle calls are made
    // at the depth the original made them at. It matters only for a row whose oracle-supplied
    // callees are large enough to push their own frames past the excluded stack window: without it
    // the guest's children run at `sp - oracleFrame` and the port's at `sp`, so every byte either
    // writes below `sp - 4096` lands at a different address on the two sides and the diff is full
    // of differences the port did not cause. Setting it makes the two sides' frames coincide, which
    // is strictly MORE of RAM compared, not less - it does not exclude anything. `race_tick` is the
    // first row that needs it (`addiu sp,sp,-32` at 0x8008AB00).
    uint32_t oracleFrame = 0;
    // The instruction budget of one case, for both the guest run and the port's oracle calls. The
    // default is ample for a leaf; `race_tick` runs a whole race frame twice per case and needs
    // more. A row that runs out of budget FAILS with a trap - it never passes quietly.
    uint64_t maxSteps = 5'000'000;
    // Compare the SPU control register file of the two machines after the case,
    // halfword for halfword, AND the ordered list of every SPU register store each side made
    // (`Cpu::spuStoreLog` - including the stores the old concession only drops, so what a port
    // hands the SPU is compared even where nothing reads it back). For a row whose port writes the
    // SPU through `Cpu::ConcessionStore`; `--mutate-spu-file` is the negative control of both.
    bool compareSpuControl = false;
    // A row whose function reaches a BIOS kernel function - the HUD's text path calls A(1Bh) strlen
    // through the SLUS stub 0x800448F4 - runs the kernel's own code (the snapshot's RAM kernel and
    // bios.bin) on both machines instead of trapping at the vector. Off for every other row.
    bool allowBiosCalls = false;
};

// ---------------------------------------------------------------- memory helpers

void PutS32(Memory& m, uint32_t a, int32_t v) { m.PokeWord(a, static_cast<uint32_t>(v)); }
void WriteU16Bench(Memory& m, uint32_t a, uint16_t v) { m.WriteBlock(a, &v, 2); }
void PutS16(Memory& m, uint32_t a, int16_t v) {
    const uint16_t u = static_cast<uint16_t>(v);
    m.WriteBlock(a, &u, 2);
}
int32_t GetS32(const Memory& m, uint32_t a) { return static_cast<int32_t>(m.PeekWord(a)); }
int16_t GetS16(const Memory& m, uint32_t a) {
    return static_cast<int16_t>(static_cast<uint16_t>(m.PeekByte(a) |
                                                     (static_cast<uint16_t>(m.PeekByte(a + 1)) << 8)));
}

void PutVec3S32(Memory& m, uint32_t a, const int32_t v[3]) {
    for (int i = 0; i < 3; ++i) PutS32(m, a + 4u * static_cast<uint32_t>(i), v[i]);
}
void PutVec3S16(Memory& m, uint32_t a, const int16_t v[3]) {
    for (int i = 0; i < 3; ++i) PutS16(m, a + 2u * static_cast<uint32_t>(i), v[i]);
}
void GetVec3S32(const Memory& m, uint32_t a, int32_t v[3]) {
    for (int i = 0; i < 3; ++i) v[i] = GetS32(m, a + 4u * static_cast<uint32_t>(i));
}
void GetVec3S16(const Memory& m, uint32_t a, int16_t v[3]) {
    for (int i = 0; i < 3; ++i) v[i] = GetS16(m, a + 2u * static_cast<uint32_t>(i));
}

// A set of guest memory regions handed to a port as plain byte buffers, with the two operations a
// row needs around an oracle-supplied call: `Commit` writes the port's copies back into the clone
// so that the original machine code runs on the CANDIDATE's state, and `Reread` reads them back
// afterwards so that whatever the callee changed is what the port sees next. This is the same
// caller-resolves-every-pointer rule as everywhere else, just with enough
// regions that doing it by hand per field would be unreadable. Regions are keyed by address, so
// two chases that land on the same record share one buffer and cannot disagree.
class GuestMirror {
public:
    explicit GuestMirror(Memory& m) : m_(m) {}
    uint8_t* Map(uint32_t address, uint32_t bytes, bool writable) {
        if (address < 0x80000000u || address + bytes > 0x80200000u) return nullptr;
        for (std::unique_ptr<Region>& r : regions_) {
            if (r->address != address || r->data.size() < bytes) continue;
            r->writable = r->writable || writable;
            return r->data.data();
        }
        auto r = std::make_unique<Region>();
        r->address = address;
        r->writable = writable;
        r->data.assign(bytes, 0);
        m_.ReadBlock(address, r->data.data(), r->data.size());
        uint8_t* p = r->data.data();
        regions_.push_back(std::move(r));
        return p;
    }
    void Commit() {
        for (std::unique_ptr<Region>& r : regions_)
            if (r->writable) m_.WriteBlock(r->address, r->data.data(), r->data.size());
    }
    void Reread() {
        for (std::unique_ptr<Region>& r : regions_)
            m_.ReadBlock(r->address, r->data.data(), r->data.size());
    }

private:
    struct Region {
        uint32_t address = 0;
        bool writable = false;
        std::vector<uint8_t> data;
    };
    Memory& m_;
    std::vector<std::unique_ptr<Region>> regions_;
};

// ---------------------------------------------------------------- the sound emitter
//
// Everything `PlaySound3D SLUS 0x80017BA0` and its tree reach outside their own arguments,
// resolved out of guest RAM by the BENCH - the caller - and handed to the port as byte views with
// their guest addresses (the caller chases every pointer). `Commit` writes the mutable
// ones back, so whatever the port changed lands in the state the whole-RAM diff compares.
//
// Three of the views are WINDOWS rather than blocks, and for the same reason the engine's stat
// block is: the sound record's offset `bank[+0x10 + 4*i]` is a u32 read
// out of game data, and the original bounds it by nothing at all. When the original's own index
// would leave the window the port sets `outOfWindow` and the bench FAILS the case - it never
// reads memory it was not given and never quietly returns a different answer.
class SoundMirror {
public:
    SoundMirror(Memory& m, uint32_t gp) : m_(m), gp_(gp) {
        state_.assign(kSoundSysBytes, 0);
        m_.ReadBlock(kSoundSys, state_.data(), state_.size());
        voicesAddr_ = Word(kSoundSys + 0x0C);
        voices_.assign(kVoiceBytes * kVoiceSlots, 0);
        if (voicesAddr_ != 0) m_.ReadBlock(voicesAddr_, voices_.data(), voices_.size());
        serial_ = m_.PeekWord(gp_ + kGpSoundSerial);
        gameStateAddr_ = m_.PeekWord(kGameStatePtr);
        gameState_.assign(0x40, 0);
        if (gameStateAddr_ != 0) m_.ReadBlock(gameStateAddr_, gameState_.data(), gameState_.size());
        listenerAddr_ = m_.PeekWord(gp_ + kGpListener);
        listeners_.assign(kListenerStride * kListenerRecords, 0);
        if (listenerAddr_ != 0)
            m_.ReadBlock(listenerAddr_, listeners_.data(), listeners_.size());
        // The bank pointer table. `bankCount + 1` slots, NOT `bankCount`: the original's bound
        // test at 0x8001F1B8 is `slt`, so `bank == count` is accepted and reads one word past the
        // `malloc(4*count)` table. Reproducing that needs the extra slot.
        const int32_t bankCount = static_cast<int32_t>(Word(kSoundSys + 0x00));
        const uint32_t slots =
            (bankCount >= 0 && bankCount < 64) ? static_cast<uint32_t>(bankCount) + 1u : 0u;
        const uint32_t table = Word(kSoundSys + 0x04);
        bankData_.resize(slots);
        banks_.resize(slots);
        for (uint32_t i = 0; i < slots; ++i) {
            const uint32_t p = (table != 0) ? m_.PeekWord(table + 4u * i) : 0u;
            banks_[i].address = p;
            if (p < 0x80000000u || p >= 0x80200000u) {
                banks_[i].address = p; // a non-RAM pointer is still "not null" to the original
                continue;
            }
            bankData_[i].assign(kBankWindow, 0);
            m_.ReadBlock(p, bankData_[i].data(), bankData_[i].size());
            banks_[i].data = bankData_[i].data();
            banks_[i].size = kBankWindow;
        }
    }

    rr::sim::SoundSystemEnv System(rr::sim::VoiceRestartSink* restart = nullptr) {
        rr::sim::SoundSystemEnv e;
        e.state = rr::sim::SoundBytes(state_.data(), static_cast<uint32_t>(state_.size()));
        e.voices = rr::sim::SoundBytes(voices_.data(), static_cast<uint32_t>(voices_.size()));

        e.serial = &serial_;
        e.banks = banks_.empty() ? nullptr : banks_.data();
        e.bankSlots = static_cast<uint32_t>(banks_.size());
        e.restart = restart;
        e.outOfWindow = &outOfWindow;
        return e;
    }
    rr::sim::Sound3DEnv Listener() {
        rr::sim::Sound3DEnv e;
        if (listenerAddr_ != 0)
            e.listeners =
                rr::sim::SoundBytes(listeners_.data(), static_cast<uint32_t>(listeners_.size()));
        e.gameState = gameState_.data();
        e.atanTable = atan_;
        // Only the doppler reads the sine table, and `PlaySound3D` never asks for the doppler, so
        // the 16 KiB is loaded on request rather than per case.
        e.sinCos = sinCos_.empty() ? nullptr : sinCos_.data();
        e.outOfWindow = &outOfWindow;
        return e;
    }
    void LoadAtanTable() {
        for (int i = 0; i < 20; ++i)
            atan_[i] = GetS32(m_, kAtanTable + 4u * static_cast<uint32_t>(i));
    }
    void LoadSinCosTable() {
        sinCos_.assign(8192, 0);
        m_.ReadBlock(kSinCosTable, sinCos_.data(), sinCos_.size() * sizeof(int16_t));
    }

    void Commit() {
        m_.WriteBlock(kSoundSys, state_.data(), state_.size());
        if (voicesAddr_ != 0) m_.WriteBlock(voicesAddr_, voices_.data(), voices_.size());
        if (listenerAddr_ != 0)
            m_.WriteBlock(listenerAddr_, listeners_.data(), listeners_.size());
        m_.PokeWord(gp_ + kGpSoundSerial, serial_);
    }
    rr::sim::SoundBytes Listeners() {
        if (listenerAddr_ == 0) return rr::sim::SoundBytes();
        return rr::sim::SoundBytes(listeners_.data(), static_cast<uint32_t>(listeners_.size()));
    }

    uint32_t Word(uint32_t address) const { return m_.PeekWord(address); }
    bool outOfWindow = false;

private:
    Memory& m_;
    uint32_t gp_;
    std::vector<uint8_t> state_;
    std::vector<uint8_t> voices_;
    uint32_t voicesAddr_ = 0;
    uint32_t serial_ = 0;
    std::vector<uint8_t> gameState_;
    uint32_t gameStateAddr_ = 0;
    std::vector<uint8_t> listeners_;
    uint32_t listenerAddr_ = 0;
    std::vector<std::vector<uint8_t>> bankData_;
    std::vector<rr::sim::SoundBankRef> banks_;
    int32_t atan_[20]{};
    std::vector<int16_t> sinCos_;
};

// Plants a whole sound-system state: the three voice lists, the two key masks, the serial counter
// and the voice records. Every index is kept inside the RANGES THE ORIGINAL'S OWN CODE PRODUCES -
// voice 0..23, ring slot 0..24, free-stack top -1..23 - because the original bounds none of them
// and a list entry of 0x40000000 would have the console write a megabyte past the voice table.
// That is a named bound on the input family, not a weakening of the
// comparison: a violated bound produces a mismatch, never a silent pass.
void PlantSoundState(CaseContext& c, uint32_t voicesAddr) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    // The free stack: empty (the live state, and therefore the steal path) five times in six.
    const int32_t freeTop = (rng.Next() % 6u == 0)
                                ? static_cast<int32_t>(rng.U32() % kVoiceSlots)
                                : -1;
    PutS32(m, kSoundSys + 0x174, freeTop);
    for (uint32_t i = 0; i < kVoiceSlots; ++i)
        PutS32(m, kSoundSys + 0xB0 + 4u * i,
               (static_cast<int32_t>(i) <= freeTop)
                   ? static_cast<int32_t>(rng.U32() % kVoiceSlots)
                   : -1);
    // The steal ring. Equal head and tail is the "nothing to steal" arm, which with an empty free
    // stack is the only way `AllocVoice` returns NULL at all.
    const int32_t head = static_cast<int32_t>(rng.U32() % 25u);
    const int32_t tail = (rng.Next() % 5u == 0) ? head : static_cast<int32_t>(rng.U32() % 25u);
    PutS32(m, kSoundSys + 0x178, head);
    PutS32(m, kSoundSys + 0x17C, tail);
    for (uint32_t i = 0; i < 25; ++i)
        PutS32(m, kSoundSys + 0x110 + 4u * i,
               (rng.Next() % 8u == 0) ? -1 : static_cast<int32_t>(rng.U32() % kVoiceSlots));
    // The reserved table, including the arm where all 22 are taken and the voice ends up on
    // NEITHER list - a leak the original allows but that has never been seen in play.
    const bool full = rng.Next() % 6u == 0;
    for (uint32_t i = 0; i < 22; ++i)
        PutS32(m, kSoundSys + 0x58 + 4u * i,
               (!full && rng.Next() % 3u == 0) ? -1 : static_cast<int32_t>(rng.U32() % kVoiceSlots));
    PutS32(m, kSoundSys + 0x14, static_cast<int32_t>(rng.U32()));  // keyOn
    PutS32(m, kSoundSys + 0x18, static_cast<int32_t>(rng.U32()));  // keyOff
    PutS32(m, kSoundSys + 0x184, (rng.Next() % 5u == 0) ? static_cast<int32_t>(rng.U32()) : 0);
    // The serial counter. 0x07FFFFFF is the one value that makes the next serial wrap to 0 and
    // then be forced to 1 (0x8001FB20), so it is driven on purpose rather than waited for.
    PutS32(m, c.gp + kGpSoundSerial,
           (rng.Next() % 8u == 0) ? 0x07FFFFFF : static_cast<int32_t>(rng.U32() & 0x07FFFFFFu));
    // The voice records. `+0x1C`, the SPU channel, is randomised over the whole word on purpose:
    // the original shifts it with `sllv`/`sll`, which mask the amount to five bits, and the handle
    // is `(channel << 27) | serial`, so a channel outside 0..23 is arithmetic, not an error.
    for (uint32_t v = 0; v < kVoiceSlots; ++v) {
        const uint32_t a = voicesAddr + kVoiceBytes * v;
        PutS32(m, a + 0x04, static_cast<int32_t>(rng.U32() & 0x07FFFFFFu));
        PutS32(m, a + 0x10, static_cast<int32_t>(rng.U32() % 3u));
        PutS32(m, a + 0x1C,
               (rng.Next() % 4u == 0) ? static_cast<int32_t>(rng.U32()) : static_cast<int32_t>(v));
        PutS32(m, a + 0x20, static_cast<int32_t>(rng.U32()));
    }
}

// Plants one bank at `address`: the header `LookupSound` and
// `PatchBank`, an offset table, and one 16-byte sound record per non-zero offset. Offsets are
// deliberately mixed: zero ("the bank has no sound i"), in range, and unaligned so that
// `StartVoice`'s `(desc & 3)` rejection at 0x8001F220 is exercised.
void PlantBank(CaseContext& c, uint32_t address, uint32_t windowBytes) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t count = rng.U32() % 40u;
    for (uint32_t i = 0; i < windowBytes; i += 4) m.PokeWord(address + i, 0);
    m.PokeWord(address + 0, 2);
    const uint8_t header[4] = {static_cast<uint8_t>(count), static_cast<uint8_t>(count),
                               static_cast<uint8_t>(count), 0x7F};
    m.WriteBlock(address + 4, header, 4);
    const uint32_t first = 0x10u + 4u * count;
    uint32_t next = (first + 3u) & ~3u;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t roll = rng.Next() % 10u;
        uint32_t off = 0;
        if (roll == 0) {
            off = 0; // the bank has no sound i
        } else {
            if (next + 32u >= windowBytes) next = (first + 3u) & ~3u;
            off = next;
            next += 16;
            if (roll == 1) off += 1u + static_cast<uint32_t>(rng.Next() % 3u); // unaligned
        }
        m.PokeWord(address + 0x10u + 4u * i, off);
        if (off != 0 && off + 16u <= windowBytes) {
            const uint8_t rec[4] = {1, static_cast<uint8_t>(rng.U32()),
                                    static_cast<uint8_t>(rng.U32()), 0x7F};
            m.WriteBlock(address + off, rec, 4);
            const uint8_t vol = static_cast<uint8_t>(rng.U32());
            m.WriteBlock(address + off + 4, &vol, 1);
            const uint8_t unk = static_cast<uint8_t>(rng.U32());
            m.WriteBlock(address + off + 5, &unk, 1);
            WriteU16Bench(m, address + off + 6, static_cast<uint16_t>(rng.U32()));  // adsr1
            WriteU16Bench(m, address + off + 8, static_cast<uint16_t>(rng.U32()));  // adsr2
            WriteU16Bench(m, address + off + 10, static_cast<uint16_t>(rng.U32())); // pitch
            m.PokeWord(address + off + 12, rng.U32());                              // spuAddr
        }
    }
}

// Picks a sound index that the bank in slot `bankIndex` really holds, most of the time. The point
// of `start_voice` and `play_sound_3d` is what happens when a voice is actually allocated and the
// steal ring turns over; an index past the bank's own count just makes `LookupSound` return 0 and
// the row would then be measuring its reject arm over and over. One case in eight is still drawn
// from the whole range, and one offset in ten of a planted bank is zero, so both reject arms stay
// exercised.
int32_t PickSoundIndex(CaseContext& c, int32_t bankIndex) {
    const uint32_t table = static_cast<uint32_t>(GetS32(*c.mem, kSoundSys + 0x04));
    int32_t count = 0;
    if (table >= 0x80000000u && bankIndex >= 0 && bankIndex < 64) {
        const uint32_t b = c.mem->PeekWord(table + 4u * static_cast<uint32_t>(bankIndex));
        if (b >= 0x80000000u && b < 0x80200000u)
            count = static_cast<int32_t>(c.mem->PeekByte(b + 4));
    }
    if (count <= 0 || c.rng->Next() % 8u == 0)
        return (c.rng->Next() % 4u == 0) ? c.rng->S32()
                                         : static_cast<int32_t>(c.rng->U32() % 140u);
    return static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(count + 1));
}

// A 16-slot bank table in the scratch block, so that the null slots, the live slots and the
// one-past-the-end slot the `slt` bound at 0x8001F1B8 lets through are all under the row's
// control. Slot 0 always holds a bank, because it is the slot `*(gp+1912)` names and therefore the
// one every `bank == 0` call ends up in.
int32_t PlantBankTable(CaseContext& c) {
    const uint32_t table = c.scratch;
    const uint32_t bankA = c.scratch + 64u;
    const uint32_t bankB = c.scratch + 832u;
    PlantBank(c, bankA, 768);
    PlantBank(c, bankB, 768);
    const int32_t bankCount = static_cast<int32_t>(c.rng->U32() % 9u);
    PutS32(*c.mem, kSoundSys + 0x00, bankCount);
    PutS32(*c.mem, kSoundSys + 0x04, static_cast<int32_t>(table));
    for (uint32_t i = 0; i < 16; ++i) {
        const uint64_t roll = c.rng->Next() % 5u;
        c.mem->PokeWord(table + 4u * i, (i == 0) ? bankA : ((roll == 0) ? 0u : ((roll == 1) ? bankB : bankA)));
    }
    return bankCount;
}

// ---------------------------------------------------------------- the integrator
//
// `RASHCDG 0x8007F0BC` and `0x8007FA4C`, the two remaining callees of the per-bike step, and every
// function under them that was not already ported. Their pointer chases are many - the owner
// record, the rider, the rider's owner, the road slice, the contact record and whatever its handle
// names, the obstacle, the stat block - so every case PLANTS all of them in the scratch block and
// the native side resolves them back out of guest RAM through a `GuestMirror` (the caller chases
// every pointer). The entity itself is a real captured bike (dump-derived) or a real bike with every
// scalar randomised; the records behind its pointers are planted, because the captured pointers of
// the other seventeen RAM images point into memory this snapshot does not share.
constexpr uint32_t kAsinTable      = 0x800527E0; // SLUS, 61 x u16 - an ARCSINE table
constexpr uint32_t kAsinEntries    = 61;
constexpr uint32_t kAsinFn         = 0x8001FF3C; // SLUS
constexpr uint32_t kRotMatrixFn    = 0x8004D2A4; // SLUS
constexpr uint32_t kMulMatrix0Fn   = 0x8003FA40; // SLUS
constexpr uint32_t kNormalize32Fn  = 0x8002E14C; // SLUS
constexpr uint32_t kReleaseContactFn = 0x8002076C; // SLUS
constexpr uint32_t kCrashLaunchFn  = 0x80071D24; // RASHCDG
constexpr uint32_t kWipeoutStartFn = 0x800723FC; // RASHCDG
constexpr uint32_t kIntegrateFn    = 0x8007F0BC; // RASHCDG
constexpr uint32_t kObstacleTestFn = 0x80075628; // RASHCDG
constexpr uint32_t kContactFrameFn = 0x8007FA4C; // RASHCDG
constexpr uint32_t kRoadRebindFn   = 0x800374D4; // SLUS, the road re-bind (2 arguments; ported)
constexpr uint32_t kGripLimitFn    = 0x8007EF60; // RASHCDG, 75 instructions
constexpr uint32_t kPassengerSteerFn = 0x80072C7C; // RASHCDG, 206 instructions
constexpr uint32_t kSteerPassFn    = 0x80074C84; // RASHCDG, 53 instructions
constexpr uint32_t kContactFrameSz = 136;        // `addiu sp,sp,-136` at 0x8007FA4C
constexpr uint32_t kWipeoutRateByte = 0x800531F1; // read by 0x8007246C
constexpr uint32_t kPool0BasePtr   = 0x8005B3A0; // read by 0x800208D8
constexpr uint32_t kPool4BasePtr   = 0x800CD6D4; // read by 0x800207E8
constexpr uint32_t kTrafficRecords = 0x800CF660; // 0x800D0000 - 2464, 512-byte records (0x80020858)
constexpr uint32_t kActiveCursor   = 0x1F800000; // the list cursor 0x8007FA14 appends through
constexpr uint32_t kOwnerBytes     = 0x260;
constexpr uint32_t kContactBytes   = 0x120;
constexpr uint32_t kStatsBytes     = 0x1C0;

struct IntegratorLayout {
    uint32_t e = 0, rider = 0, owner = 0, riderOwner = 0, slice = 0, contact = 0, obstacle = 0,
             stats = 0, list = 0, aux = 0;
};
IntegratorLayout IntegratorAddresses(uint32_t scratch) {
    IntegratorLayout L;
    L.e = scratch;
    L.rider = scratch + 1104;
    L.owner = scratch + 2208;
    L.riderOwner = scratch + 2816;
    L.slice = scratch + 3424;
    L.contact = scratch + 3456;
    L.obstacle = scratch + 3744;
    L.stats = scratch + 3808;
    L.list = scratch + 4256;  // 96 bytes the list cursor may point into
    L.aux = scratch + 4400;   // argument buffers of the leaf rows
    return L;
}

// How a row wants the per-frame flag words driven. Everything not named here keeps the captured
// value (dump-derived) or gets a random one (randomised).
enum class IntegratorMode { kIntegrate, kCrashLaunch, kWipeout, kObstacle, kContact, kRelease };

void PutU32V(std::vector<uint8_t>& v, uint32_t off, uint32_t x) {
    for (uint32_t k = 0; k < 4; ++k) v[off + k] = static_cast<uint8_t>(x >> (8 * k));
}
uint32_t GetU32V(const std::vector<uint8_t>& v, uint32_t off) {
    return static_cast<uint32_t>(v[off]) | (static_cast<uint32_t>(v[off + 1]) << 8) |
           (static_cast<uint32_t>(v[off + 2]) << 16) | (static_cast<uint32_t>(v[off + 3]) << 24);
}
void PutU16V(std::vector<uint8_t>& v, uint32_t off, uint16_t x) {
    v[off] = static_cast<uint8_t>(x);
    v[off + 1] = static_cast<uint8_t>(x >> 8);
}

// Plants one whole integrator case. `live` builds the entity on a live pool-0 bike of THIS
// machine, which `bike_contact_frame` needs for the same reason `bike_ground_frame` does:
// its callee walks the machine's own road through the
// entity's road cursor.
IntegratorLayout PlantIntegratorCase(CaseContext& c, IntegratorMode mode, bool live) {
    namespace ent = rr::sim::ent;
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const IntegratorLayout L = IntegratorAddresses(c.scratch);
    const bool derived = c.sample != nullptr;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    auto unit = [&rng](std::vector<uint8_t>& v, uint32_t off) {
        for (uint32_t k = 0; k < 3; ++k) PutU16V(v, off + 2u * k, static_cast<uint16_t>(rng.Unit()));
    };

    std::vector<uint8_t> e(1096, 0);
    const uint32_t poolBase = m.PeekWord(kPoolTable);
    const int32_t stride = static_cast<int32_t>(m.PeekWord(kPoolTable + 4u));
    uint32_t slots = 1;
    if (const uint32_t hp = m.PeekWord(kPoolTable + 12u); hp >= 0x80000000u) {
        const int32_t high = static_cast<int32_t>(m.PeekWord(hp));
        if (high >= 0 && high < 64) slots = static_cast<uint32_t>(high) + 1u;
    }
    if (live) {
        if (poolBase >= 0x80000000u && stride >= 1096)
            m.ReadBlock(poolBase + (rng.U32() % slots) * static_cast<uint32_t>(stride), e.data(), 1096);
    } else if (derived && c.sample->entity.size() == 1096) {
        e = c.sample->entity;
    } else if (c.any != nullptr && c.any->entity.size() == 1096) {
        e = c.any->entity;
    }
    const bool randomise = !derived;

    // ---- orientation: real in the dump-derived family, random unit-scale rows otherwise
    if (randomise) {
        for (uint32_t r = 0; r < 3; ++r) unit(e, 0x1B0 + 6u * r);
        for (uint32_t r = 0; r < 3; ++r) unit(e, 0x204 + 6u * r);
        unit(e, 0x1C2);
        unit(e, 0x210);
        unit(e, 0x32E);
    }
    // The union at +0xF4..+0x123: a captured image shows box corners there, but
    // what these functions read is what the ground frame left - two normals and two points.
    unit(e, 0x10C);
    unit(e, 0x112);
    if (randomise || chance(2)) unit(e, 0x20A);
    if (randomise) {
        for (uint32_t k = 0; k < 3; ++k) {
            PutU32V(e, 0xB8 + 4u * k, static_cast<uint32_t>(rng.World()));
            PutU32V(e, 0x1F8 + 4u * k, GetU32V(e, 0xB8 + 4u * k) + static_cast<uint32_t>(rng.S32() >> 13));
            PutU32V(e, 0x310 + 4u * k, GetU32V(e, 0xB8 + 4u * k) + static_cast<uint32_t>(rng.S32() >> 13));
            PutU32V(e, 0x1D4 + 4u * k, static_cast<uint32_t>(rng.World()));
            PutU32V(e, 0x1C8 + 4u * k, static_cast<uint32_t>(rng.S32() >> 10));
        }
        PutU32V(e, 0x130, rng.U32() % 0x40000u);
        PutU32V(e, 0x134, rng.U32() % 0x40000u);
    }
    {
        // One case in eight puts the road point and the second contact exactly on the contact
        // point, which is the only way the contact frame's inlined Normalize32 meets a zero vector
        // (its `m == 0` arm at 0x8007FE6C).
        const bool onPoint = chance(8);
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t base = GetU32V(e, 0x1F8 + 4u * k);
            PutU32V(e, 0xF4 + 4u * k, onPoint ? base : base + static_cast<uint32_t>(rng.S32() >> 13));
            PutU32V(e, 0x118 + 4u * k, onPoint ? base : base + static_cast<uint32_t>(rng.S32() >> 13));
        }
    }
    // +0x31C..+0x327 is a union too: three 16.16 offsets for the integrator, an s16 axis for the
    // tumble. Half the cases keep the captured bytes, half plant a unit-scale axis.
    if (randomise || chance(2)) {
        unit(e, 0x31C);
        if (chance(2)) {
            for (uint32_t k = 0; k < 3; ++k)
                PutU32V(e, 0x31C + 4u * k, static_cast<uint32_t>(rng.S32() >> 14));
        }
    }
    // NAMED BOUND: the s16 VIEW of +0x31C..+0x321 is kept within +-16384
    // per component. The tumble arm of 0x80071D24 takes a GTE outer product of it and normalises
    // the result with the trapping `add` at 0x8002E4A4; a view near +-32767 on two axes makes the
    // CONSOLE raise an arithmetic-overflow exception - measured: 2 of 2694 cases on the second
    // seed before this bound, the guest's own trap - which is a fact about the original, not a
    // comparison. Halving the three halfwords keeps the offsets' order of magnitude.
    for (uint32_t k = 0; k < 3; ++k) {
        const int16_t v = static_cast<int16_t>(GetU32V(e, 0x31C + 2u * k) & 0xFFFFu);
        if (v > 16384 || v < -16384) PutU16V(e, 0x31C + 2u * k, static_cast<uint16_t>(v >> 2));
    }

    // ---- scalars
    auto scalar = [&](uint32_t off, int shift, uint32_t zeroOdds) {
        if (randomise) PutU32V(e, off, static_cast<uint32_t>(rng.S32() >> shift));
        if (chance(zeroOdds)) PutU32V(e, off, 0);
    };
    scalar(ent::kSpeed, 9, 4);
    scalar(ent::kSpeedCopy, 9, 4);
    scalar(0x1E8, 14, 3);
    scalar(0x2A4, 14, 3);
    scalar(0x2E8, 14, 2);
    scalar(0x268, 14, 3);
    scalar(0x28C, 14, 3);
    scalar(0x27C, 13, 3);
    scalar(0x290, 12, 3);
    scalar(0x2A8, 12, 3);
    scalar(0x2D4, 14, 3);
    scalar(0x2D8, 14, 3);
    scalar(0x24C, 14, 2);
    scalar(0x308, 12, 3);
    scalar(0x30C, 12, 3);
    scalar(0x300, 12, 3);
    PutU32V(e, 0x104, static_cast<uint32_t>(rng.S32() >> 13));
    if (chance(3)) PutU32V(e, 0x104, static_cast<uint32_t>(-static_cast<int32_t>(rng.U32() % 0x8000u)));
    PutU32V(e, 0x16C, chance(2) ? 1u : 0xFFFFFFFFu);
    if (chance(4)) PutU32V(e, 0x16C, rng.U32());
    e[0x216] = chance(4) ? 4u : static_cast<uint8_t>(rng.U32());
    // Speeds around the obstacle test's own thresholds, so each of its comparisons is reached.
    if (mode == IntegratorMode::kObstacle || mode == IntegratorMode::kContact) {
        static const uint32_t kSpeeds[] = {0xB2D0E, 0xB2D0F, 0x23C360, 0x23C361, 0x35A510, 0x35A511};
        if (chance(2)) PutU32V(e, ent::kSpeed, kSpeeds[rng.U32() % 6u] + (rng.U32() % 3u) - 1u);
        if (chance(3)) PutU32V(e, ent::kSpeedCopy, 0x1BAFFFEu + (rng.U32() % 0x400000u));
        // ... and just above it, where the cap `-(0x1BAFFFE / speedCopy) >>> 4` is exactly -1.0 and
        // the slope arm divides by a non-positive `1 - s^2` (0x80075A68).
        if (chance(6)) PutU32V(e, ent::kSpeedCopy, 0x1BAFFFFu + (rng.U32() % 256u));
        // The inlined arcsine's top band (|x| >= 0xFFF8) is reached only by a heading whose
        // vertical component is 4095/4096.
        if (chance(6)) PutU16V(e, 0x1C4, chance(2) ? 4095u : static_cast<uint16_t>(-4095));
        // A heading with (almost) no horizontal part: the `flat < 6` arm at 0x80075A0C.
        if (chance(6)) {
            PutU16V(e, 0x1C2, static_cast<uint16_t>(static_cast<int32_t>(rng.U32() % 17u) - 8));
            PutU16V(e, 0x1C4, static_cast<uint16_t>((chance(2) ? 1 : -1) *
                                                    static_cast<int32_t>(3000u + rng.U32() % 1096u)));
            PutU16V(e, 0x1C6, static_cast<uint16_t>(static_cast<int32_t>(rng.U32() % 17u) - 8));
        }
    }
    // The thrown arm of 0x80071D24 takes its "cos == 0" branch only when the axis-2 row's vertical
    // component is exactly +-1.0, which a random orientation never is.
    if (mode == IntegratorMode::kCrashLaunch || mode == IntegratorMode::kIntegrate)
        if (chance(6)) PutU16V(e, 0x1BE, chance(2) ? 4096u : static_cast<uint16_t>(-4096));

    // ---- flags
    uint32_t fa = GetU32V(e, ent::kFlagsA), fb = GetU32V(e, ent::kFlagsB), fc = GetU32V(e, ent::kFlagsC);
    if (randomise) { fa = rng.U32(); fb = rng.U32() & 0x0FF7FFFFu; fc = rng.U32() & 0x0003FFFFu; }
    auto setBit = [&](uint32_t& w, uint32_t bits, uint32_t odds) {
        w = chance(odds) ? (w | bits) : (w & ~bits);
    };
    setBit(fa, 0x300, 2);
    setBit(fa, 0x08000000, 2);
    setBit(fb, 0x8, 6);
    setBit(fb, 0x8000, 4);
    setBit(fb, 0x40000, 4);
    setBit(fb, 0x20000, 3);
    setBit(fb, 0x3400, 4);
    setBit(fb, 0x400000, 4);
    setBit(fb, 0x2000000, 2);
    setBit(fb, 0x4000000, 3);
    // The obstacle test's early-out (`flagsB & 0x61D800`) is kept mostly open where it matters.
    if ((mode == IntegratorMode::kObstacle || mode == IntegratorMode::kContact) && !chance(8))
        fb &= ~0x0061D800u;
    fc &= ~0x0FFFFFFFu;
    const uint32_t kinds[] = {0x20, 0x40, 0x80, 0x100};
    switch (mode) {
        case IntegratorMode::kCrashLaunch:
            fc |= chance(2) ? 0x400u : 0x200u;
            if (chance(3)) fc |= 0x600u;
            break;
        case IntegratorMode::kWipeout:
            fc |= kinds[rng.U32() % 4u];
            if (chance(3)) fc |= kinds[rng.U32() % 4u];
            break;
        case IntegratorMode::kIntegrate:
            if (chance(3)) fc |= chance(2) ? 0x400u : 0x200u;
            if (chance(2)) fc |= kinds[rng.U32() % 4u];
            break;
        case IntegratorMode::kContact:
            if (chance(4)) fc |= chance(2) ? 0x400u : 0x200u;
            break;
        default:
            if (chance(8)) fc |= 0x200u;
            break;
    }
    if (chance(2)) fc |= 0x800u;
    if (chance(4)) fc |= 1u + rng.U32() % 15u;
    if (chance(4)) fc |= 0x4000u;
    if (chance(3)) fc |= 0x8000u;
    if (chance(3)) fc |= 0x10000u;
    if (chance(3)) fc |= 0x20000u;
    if (chance(2)) fc |= 0x400000u;
    if (chance(8)) fc |= 0x800000u;
    PutU32V(e, ent::kFlagsA, fa);
    PutU32V(e, ent::kFlagsB, fb);
    PutU32V(e, ent::kFlagsC, fc);
    {
        uint32_t w = GetU32V(e, 0x184);
        setBit(w, 1, 2);
        setBit(w, 0x40, 2);
        PutU32V(e, 0x184, w);
    }

    // ---- the pointers, all into the scratch block
    const bool hasRider = !chance(4);
    PutU32V(e, ent::kOwner, L.owner);
    PutU32V(e, ent::kRider, hasRider ? L.rider : 0u);
    PutU32V(e, 0x154, L.slice);
    const bool hasContact = !chance(5);
    PutU32V(e, 0x340, hasContact ? L.contact : 0u);
    // NAMED BOUND: flagsB bit 18 ("riding a contact") only with a contact. With `e[+0x340] == 0`
    // the original's `lh v0,260(a0)` at 0x8007F3E0 (and 0x800806F4 in the contact frame) reads
    // guest 0x104 - the kernel's area - and the port declines that rather than follow it.
    if (!hasContact) {
        uint32_t w = GetU32V(e, ent::kFlagsB);
        PutU32V(e, ent::kFlagsB, w & ~0x40000u);
    }
    PutU32V(e, 0x33C, chance(3) ? 0u : L.obstacle);
    PutU32V(e, ent::kStats, L.stats);
    if (chance(6)) PutU32V(e, 0x440, 0);
    else if (GetU32V(e, 0x440) == 0) PutU32V(e, 0x440, 0x80000000u | (rng.U32() & 0x1FFFFCu));

    // The owner: +0x228 flags, +0x23C a byte whose bit 4 says "follow the rider's owner too",
    // +0x25C the value the `< 2` tests read. With no rider that bit would make the ORIGINAL
    // dereference guest 0x354; the port refuses that, so the family never asks for it.
    std::vector<uint8_t> owner(kOwnerBytes);
    for (uint8_t& b : owner) b = static_cast<uint8_t>(rng.U32());
    PutU32V(owner, 604, rng.U32() % 4u);
    if (!hasRider) owner[572] &= static_cast<uint8_t>(~0x10u);
    std::vector<uint8_t> riderOwner(kOwnerBytes);
    for (uint8_t& b : riderOwner) b = static_cast<uint8_t>(rng.U32());
    PutU32V(riderOwner, 604, rng.U32() % 4u);

    std::vector<uint8_t> rider = (c.any != nullptr && c.any->entity.size() == 1096) ? c.any->entity
                                                                                    : std::vector<uint8_t>(1096);
    unit(rider, 0x32E);
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t base = GetU32V(e, 0x1F8 + 4u * k);
        PutU32V(rider, 0xB8 + 4u * k, base + static_cast<uint32_t>(rng.S32() >> 13));
        PutU32V(rider, 0x310 + 4u * k, base + static_cast<uint32_t>(rng.S32() >> 13));
    }
    PutU32V(rider, 0x1E8, rng.U32());
    PutU32V(rider, ent::kOwner, L.riderOwner);

    std::vector<uint8_t> slice(32);
    for (uint8_t& b : slice) b = static_cast<uint8_t>(rng.U32());
    for (uint32_t r = 0; r < 3; ++r) unit(slice, 2 + 6u * r);

    // The contact record: a handle at +0, a point at +12, two axes at +260 and +272.
    std::vector<uint8_t> contact(kContactBytes);
    for (uint8_t& b : contact) b = static_cast<uint8_t>(rng.U32());
    {
        uint32_t pool = 0, slot = 0;
        const uint32_t base4 = m.PeekWord(kPool4BasePtr);
        const bool propsOk = base4 >= 0x80000000u && base4 < 0x80200000u - 596u * 32u;
        switch (rng.U32() % 6u) {
            case 0: pool = 0; slot = rng.U32() % slots; break;
            case 1: case 2: pool = 3; slot = rng.U32() % 32u; break;
            case 3:
                if (propsOk) {
                    // PLANTED, named: `rr-race` has no live prop - every pool-4 record's `+0`
                    // pointer is 0 in it - so the case gives one slot a descriptor of its own (in
                    // the scratch block) whose kind word at +14 is drawn over the three kinds the
                    // arm acts on and everything else, and a random +0x250 flag word.
                    pool = 4;
                    slot = rng.U32() % 32u;
                    const uint32_t prop = base4 + 596u * slot;
                    const uint32_t desc = L.aux + 64u;
                    m.PokeWord(prop, desc);
                    m.PokeWord(prop + 592u, rng.U32());
                    const uint16_t kind = chance(2) ? static_cast<uint16_t>((3u + rng.U32() % 3u) << 7)
                                                    : static_cast<uint16_t>(rng.U32());
                    WriteU16Bench(m, desc + 14u, kind);
                } else {
                    pool = 3;
                    slot = rng.U32() % 32u;
                }
                break;
            case 4: pool = 1 + rng.U32() % 2u; slot = rng.U32() % 32u; break;
            default: pool = 5 + rng.U32() % 60u; slot = rng.U32() % 32u; break;
        }
        PutU16V(contact, 0, static_cast<uint16_t>((pool << 5) | slot));
        for (uint32_t k = 0; k < 3; ++k)
            PutU32V(contact, 12 + 4u * k, static_cast<uint32_t>(rng.S32() >> 14));
        unit(contact, 260);
        unit(contact, 272);
    }
    std::vector<uint8_t> obstacle(64);
    for (uint8_t& b : obstacle) b = static_cast<uint8_t>(rng.U32());
    if (!chance(3)) {
        // The obstacle arm of 0x80075628 needs +0x32 to agree in sign with the road direction at
        // +0x16C and to be smaller in magnitude than the speed's integer part, and the slice's
        // tangent to line up with the heading - none of which random bytes ever do.
        const int32_t dir = static_cast<int32_t>(GetU32V(e, 0x16C));
        int32_t side = static_cast<int32_t>(rng.U32() % 48u);
        if (chance(3) ? (dir >= 0) : (dir < 0)) side = -side;
        PutU16V(obstacle, 50, static_cast<uint16_t>(side));
        if ((mode == IntegratorMode::kObstacle || mode == IntegratorMode::kContact) && chance(2)) {
            PutU32V(e, ent::kSpeed, 0x10000u + (rng.U32() % 0x3C0000u));
            PutU32V(e, 0x184, GetU32V(e, 0x184) & ~1u); // the obstacle arm is the "no ground" one
        }
    }
    if (!chance(3)) {
        for (uint32_t k = 0; k < 3; ++k)
            PutU16V(slice, 14 + 2u * k,
                    static_cast<uint16_t>(static_cast<int16_t>(GetU32V(e, 0x1C2 + 2u * k) & 0xFFFFu) +
                                          static_cast<int16_t>(static_cast<int32_t>(rng.U32() % 257u) - 128)));
        if (chance(2))
            for (uint32_t k = 0; k < 3; ++k)
                PutU16V(slice, 14 + 2u * k, static_cast<uint16_t>(-static_cast<int16_t>(
                                                GetU32V(slice, 14 + 2u * k) & 0xFFFFu)));
    }
    std::vector<uint8_t> stats(kStatsBytes);
    for (uint8_t& b : stats) b = static_cast<uint8_t>(rng.U32());
    PutU32V(stats, 440, static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 4096u) - 2048));

    // The spin's rate byte at 0x800531F1 is 0xC0 in this snapshot, which never meets the 13107
    // floor at 0x80072480; drawn so that both sides of it are taken.
    if (mode == IntegratorMode::kWipeout || mode == IntegratorMode::kIntegrate) {
        const uint8_t rate = chance(3) ? static_cast<uint8_t>(rng.U32() % 13u) : static_cast<uint8_t>(rng.U32());
        m.WriteBlock(kWipeoutRateByte, &rate, 1);
    }
    m.WriteBlock(L.e, e.data(), e.size());
    m.WriteBlock(L.rider, rider.data(), rider.size());
    m.WriteBlock(L.owner, owner.data(), owner.size());
    m.WriteBlock(L.riderOwner, riderOwner.data(), riderOwner.size());
    m.WriteBlock(L.slice, slice.data(), slice.size());
    m.WriteBlock(L.contact, contact.data(), contact.size());
    m.WriteBlock(L.obstacle, obstacle.data(), obstacle.size());
    m.WriteBlock(L.stats, stats.data(), stats.size());
    // The active-list cursor: into the scratchpad (where the game keeps it) or into the scratch
    // block, never onto the cursor word itself - the original re-reads the cursor AFTER its store,
    // so a cursor that pointed at itself would be a different computation, not a port bug.
    m.PokeWord(kActiveCursor, chance(2) ? (0x1F800004u + 4u * (rng.U32() % 200u))
                                        : (L.list + 4u * (rng.U32() % 24u)));
    return L;
}

// The native side's view of one planted case: every pointer the ported code needs, resolved out of
// the clone's guest RAM by the caller, and the game-data tables out of the player's own images.
class IntegratorHarness {
public:
    IntegratorHarness(Memory& m, uint32_t ea, uint32_t gp) : m_(m), mir_(m) {
        e = mir_.Map(ea, 1096, true);
        const rr::sim::EntityView ev(e);
        const uint32_t ownerAddr = ev.U32(rr::sim::ent::kOwner);
        const uint32_t riderAddr = ev.U32(rr::sim::ent::kRider);
        links.owner = mir_.Map(ownerAddr, kOwnerBytes, true);
        if (riderAddr != 0) {
            links.rider = mir_.Map(riderAddr, 1096, true);
            if (links.rider != nullptr)
                links.riderOwner = mir_.Map(rr::sim::EntityView(links.rider).U32(rr::sim::ent::kOwner),
                                            kOwnerBytes, true);
        }
        links.slice = mir_.Map(ev.U32(0x154), 32, false);
        links.obstacle = mir_.Map(ev.U32(0x33C), 64, false);
        links.stats = mir_.Map(ev.U32(rr::sim::ent::kStats), kStatsBytes, false);
        const uint32_t ct = ev.U32(0x340);
        links.contact.contact = mir_.Map(ct, kContactBytes, false);
        if (links.contact.contact != nullptr) {
            const uint32_t h = static_cast<uint32_t>(links.contact.contact[0]) |
                               (static_cast<uint32_t>(links.contact.contact[1]) << 8);
            const uint32_t pool = h >> 5, slot = h & 31u;
            if (pool == 0)
                links.contact.bike = mir_.Map(m.PeekWord(kPool0BasePtr) + 1096u * slot, 1096, true);
            else if (pool == 3)
                links.contact.traffic = mir_.Map(kTrafficRecords + 512u * slot, 512, false);
            else if (pool == 4) {
                const uint32_t prop = m.PeekWord(kPool4BasePtr) + 596u * slot;
                links.contact.prop = mir_.Map(prop, 596, true);
                const uint32_t rec = m.PeekWord(prop);
                links.contact.propKindWord = static_cast<uint16_t>(m.PeekByte(rec + 14u) |
                                                                   (m.PeekByte(rec + 15u) << 8));
            }
        }
        links.wipeoutRateByte = m.PeekByte(kWipeoutRateByte);
        sincos_.assign(8192, 0);
        m.ReadBlock(kSinCosTable, sincos_.data(), sincos_.size() * sizeof(int16_t));
        asin_.assign(kAsinEntries, 0);
        m.ReadBlock(kAsinTable, asin_.data(), asin_.size() * sizeof(uint16_t));
        for (int i = 0; i < 20; ++i) atan_[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));
        rsqrt_.assign(2048, 0);
        m.ReadBlock(m.PeekWord(gp + kRsqrtTableGpOffset), rsqrt_.data(), rsqrt_.size() * sizeof(uint16_t));
        sqrt_.assign(kSqrtWindowHalf, 0);
        m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, sqrt_.data(), sqrt_.size() * 2u);
        tables.sincos = sincos_.data();
        tables.asin = asin_.data();
        tables.atan = atan_;
        tables.rsqrt = rsqrt_.data();
        tables.sqrt = sqrt_.data() + kSqrtWindowHalf / 4u;
    }
    rr::sim::EntityView Entity() { return rr::sim::EntityView(e); }
    void Commit() { mir_.Commit(); }
    void Reread() { mir_.Reread(); }

    uint8_t* e = nullptr;
    rr::sim::BikeLinks links;
    rr::sim::BikeTables tables;

private:
    Memory& m_;
    GuestMirror mir_;
    std::vector<int16_t> sincos_;
    std::vector<uint16_t> asin_;
    int32_t atan_[20]{};
    std::vector<uint16_t> rsqrt_;
    std::vector<int16_t> sqrt_;
};

void AddRoadQueryRows(std::vector<Row>& rows); // below BuildRows
// Rows kept in separate files, one per subsystem (rows_*.inc, included at the end of this
// namespace).
void AddBikeStepRows(std::vector<Row>& rows);    // rows_bike_step.inc
void AddRoadRuntimeRows(std::vector<Row>& rows); // rows_road_runtime.inc
void AddRoadRebindRows(std::vector<Row>& rows);  // rows_road_runtime.inc (the re-bind)
void AddEngineNoteRows(std::vector<Row>& rows);  // rows_engine_note.inc
void AddGroundRows(std::vector<Row>& rows);      // rows_ground.inc
void AddAnimRows(std::vector<Row>& rows);        // rows_anim.inc
// rows_anim.inc: RASHCDG 0x800C4550 run natively, the call witnessed in env.calls (the AI rows, region E)
uint32_t AnimNativeStanceEvent(NativeEnv& env, uint32_t ev, uint32_t rider, uint32_t p, uint32_t sp);
void AddCrashRows(std::vector<Row>& rows);       // rows_crash.inc
void AddPopulationRows(std::vector<Row>& rows);  // rows_population.inc
void AddCameraRows(std::vector<Row>& rows);      // rows_camera.inc
void AddSpineRows(std::vector<Row>& rows);       // rows_spine.inc
void AddCollisionRows(std::vector<Row>& rows);   // rows_collision.inc
void AddHudRows(std::vector<Row>& rows);         // rows_hud.inc
void AddTrafficRows(std::vector<Row>& rows);     // rows_traffic.inc
void AddCollRows(std::vector<Row>& rows);        // rows_coll.inc
void AddPoseRows(std::vector<Row>& rows);        // rows_pose.inc
void AddAiBrainRows(std::vector<Row>& rows);     // rows_ai_brain.inc
void AddAiRows(std::vector<Row>& rows);          // rows_ai.inc (the command pass)
void AddAiPlanRows(std::vector<Row>& rows);      // rows_ai_plan.inc (the planner)
void AddFightRows(std::vector<Row>& rows);       // rows_fight.inc (combat)
void AddSoundFrameRows(std::vector<Row>& rows);  // rows_engine_note.inc (AudioFrame)
void AddShellRows(std::vector<Row>& rows);       // rows_shell.inc (front end)
void AddRecoverRows(std::vector<Row>& rows);     // rows_recover.inc (back on the bike)
void AddRiderRows(std::vector<Row>& rows);       // rows_riders.inc, the race loader's rider records
void AddFeelRows(std::vector<Row>& rows);        // rows_feel.inc
void AddWeaponRows(std::vector<Row>& rows);      // rows_weapon.inc (weapons)
void AddSpeechRows(std::vector<Row>& rows);      // rows_speech.inc (the riders' voices)
void AddCopRows(std::vector<Row>& rows);         // rows_cops.inc (the police chase, the arrest)
void AddFxRows(std::vector<Row>& rows);          // rows_fx.inc (the effect pass)
void AddModeRows(std::vector<Row>& rows);        // rows_modes.inc, rules.md 9 (the game modes: player cop, Jailbreak)
void AddMpRows(std::vector<Row>& rows);          // rows_mp.inc (two players on one screen)
void AddModelRows(std::vector<Row>& rows);       // rows_model.inc (the model draw's geometry)
void AddVisRows(std::vector<Row>& rows);         // rows_vis.inc, scene_cell.md 14
void AddWorldRows(std::vector<Row>& rows);       // rows_world.inc
void AddJailRows(std::vector<Row>& rows);        // rows_jail.inc, rules.md 16 (the Jailbreak mode, the two-seat bike)
void AddPolishRows(std::vector<Row>& rows);      // rows_polish.inc (the L2 arm per player)
void AddSolidRows(std::vector<Row>& rows);       // rows_solid.inc (poles, props)
void AddHazardRows(std::vector<Row>& rows);      // rows_hazards.inc (the hazard objects)
void AddPartnersRows(std::vector<Row>& rows);    // rows_partners.inc
void AddTakedownRows(std::vector<Row>& rows);    // rows_takedown.inc (takedown, rider-off, busted)
void AddGridRows(std::vector<Row>& rows);        // rows_grid.inc (the starting grid)
void AddPedsRows(std::vector<Row>& rows);        // rows_peds.inc (the pedestrians)
void AddPassesRows(std::vector<Row>& rows);      // rows_passes.inc
void AddStrikeRows(std::vector<Row>& rows);      // rows_strike.inc
void AddFxDrawRows(std::vector<Row>& rows);      // rows_fxdraw.inc (the glow sprites)
void AddJunctionRows(std::vector<Row>& rows);    // rows_junction.inc
void AddStreamRows(std::vector<Row>& rows);      // rows_stream.inc
void AddSubdivRows(std::vector<Row>& rows);      // rows_subdiv.inc, scene_cell.md 13.8
void AddLookRows(std::vector<Row>& rows);        // rows_look.inc (the model shadow)
void AddAnimObjRows(std::vector<Row>& rows);     // rows_animobj.inc
void AddMp2Rows(std::vector<Row>& rows);         // rows_mp2.inc (the two-player leftovers)
void AddMenuRows(std::vector<Row>& rows);        // rows_menu.inc (the menu widgets' drawing)
void AddStream2Rows(std::vector<Row>& rows);     // rows_stream2.inc
void AddLoaderRows(std::vector<Row>& rows);      // rows_loader.inc
void AddPauseRows(std::vector<Row>& rows);       // rows_pause.inc
void AddLoader2SpuRows(std::vector<Row>& rows);  // rows_loader2_spu.inc (libspu's SPU-RAM allocator)
void AddLoader2CamRows(std::vector<Row>& rows);  // rows_loader2_cam.inc (the camera set-up, the light stores)
void AddLoader2Rows(std::vector<Row>& rows);     // rows_loader2.inc (BuildRace, SetUpRace's children)
void AddMenu2Rows(std::vector<Row>& rows);       // rows_menu2.inc (the panel films, the logos)
void AddRouteRows(std::vector<Row>& rows);       // rows_route.inc (the route block parser, the road map)
void AddCellsRows(std::vector<Row>& rows);       // rows_cells.inc (the frame's cell sort)
void AddStream3Rows(std::vector<Row>& rows);     // rows_stream3.inc (the stream's files)
void AddSky2Rows(std::vector<Row>& rows);        // rows_sky2.inc (the sky's own programs)
void AddSky3Rows(std::vector<Row>& rows);        // rows_sky3.inc (gradient, two-player sky, heap)
// rows_spine.inc: a spine seam (src\game\sim\spine.h) run natively, the call witnessed in env.calls;
// and the stance event's seams for a row whose port runs StampResult 0x800BC7CC natively.
uint32_t SpineNativeCall(NativeEnv& env, uint32_t address, const uint32_t* a, int n);
std::vector<OracleCallee> SpineStampSeams(uint32_t depth);
std::vector<OracleCallee> SpineFinishSeams(uint32_t depth); // FinishTest 0x800B9958 run natively
uint32_t RqDone(NativeEnv& env, const rr::sim::GuestRam& g, uint32_t v);

// ---------------------------------------------------------------- the rows

void AddRumbleRows(std::vector<Row>& rows);       // rows_rumble.inc (the pad motors, the countdown voice)
void AddAnimSoundRows(std::vector<Row>& rows);    // rows_anim_sound.inc (the ANIMNOIZ events)
std::vector<Row> BuildRows() {
    std::vector<Row> rows;

    rows.push_back(Row{
        "fix_mul", "s32 FixMul(s32 a, s32 b)", kFixMul,
        [](CaseContext& c, Args& args) {
            if (c.sample) {
                args.a[0] = static_cast<uint32_t>(c.sample->p[c.rng->Next() % 3]);
                args.a[1] = static_cast<uint32_t>(c.sample->speed);
            } else {
                args.a[0] = c.rng->U32();
                args.a[1] = c.rng->U32();
            }
        },
        [](Memory&, uint32_t, uint32_t, const Args& args) {
            return static_cast<uint32_t>(rr::sim::FixMul(static_cast<int32_t>(args.a[0]),
                                                         static_cast<int32_t>(args.a[1])));
        }});

    rows.push_back(Row{
        "fix_div", "u32 FixDiv(u32 a, u32 b)", kFixDiv,
        [](CaseContext& c, Args& args) {
            if (c.sample) {
                args.a[0] = static_cast<uint32_t>(c.sample->along);
                args.a[1] = static_cast<uint32_t>(c.sample->speed);
            } else {
                args.a[0] = c.rng->U32();
                args.a[1] = c.rng->U32();
            }
            // b == 0 and b == 1 both make the guest divide by zero. That is a real input the
            // original can be handed, so it is exercised deliberately rather than avoided.
            const uint64_t roll = c.rng->Next() % 16u;
            if (roll == 0) args.a[1] = 0;
            else if (roll == 1) args.a[1] = 1;
        },
        [](Memory&, uint32_t, uint32_t, const Args& args) {
            return rr::sim::FixDiv(args.a[0], args.a[1]);
        }});

    rows.push_back(Row{
        "approx_len3", "s32 ApproxLen3(s32 x, s32 y, s32 z)", kApproxLen3,
        [](CaseContext& c, Args& args) {
            if (c.sample) {
                for (int i = 0; i < 3; ++i) args.a[i] = static_cast<uint32_t>(c.sample->p[i] - c.sample->q[i]);
            } else {
                for (int i = 0; i < 3; ++i) args.a[i] = c.rng->U32();
                // Force the extreme the MIPS abs idiom wraps on, now and then.
                if (c.rng->Next() % 32u == 0) args.a[c.rng->Next() % 3u] = 0x80000000u;
            }
        },
        [](Memory&, uint32_t, uint32_t, const Args& args) {
            return static_cast<uint32_t>(rr::sim::ApproxLen3(static_cast<int32_t>(args.a[0]),
                                                             static_cast<int32_t>(args.a[1]),
                                                             static_cast<int32_t>(args.a[2])));
        }});

    rows.push_back(Row{
        "rand", "u32 Rand(void)  [seed at gp+2076]", kRand,
        [](CaseContext& c, Args& args) {
            // The seed is a global: the whole-RAM diff is what checks that our port writes it back.
            const uint32_t seed = c.sample ? static_cast<uint32_t>(c.sample->speed) : c.rng->U32();
            c.mem->PokeWord(c.gp + kRandSeedGpOffset, seed);
            args.a[0] = 0;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args&) {
            uint32_t seed = m.PeekWord(gp + kRandSeedGpOffset);
            const uint32_t r = rr::sim::Rand(seed);
            m.PokeWord(gp + kRandSeedGpOffset, seed);
            return r;
        }});

    rows.push_back(Row{
        "dot_lcm", "s32 DotLcm(const s16 v[3], const s16 m[3])  [GTE MVMVA]", kDotLcm,
        [](CaseContext& c, Args& args) {
            int16_t v[3], m[3];
            if (c.sample) {
                const size_t rowA = c.rng->Next() % 3u;
                const size_t rowB = c.rng->Next() % 3u;
                for (int i = 0; i < 3; ++i) {
                    v[i] = c.sample->mA[rowA * 3 + static_cast<size_t>(i)];
                    m[i] = c.sample->mB[rowB * 3 + static_cast<size_t>(i)];
                }
            } else {
                for (int i = 0; i < 3; ++i) {
                    v[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                    m[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                }
            }
            PutVec3S16(*c.mem, c.scratch + 0, v);
            PutVec3S16(*c.mem, c.scratch + 8, m);
            args.a[0] = c.scratch + 0;
            args.a[1] = c.scratch + 8;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            int16_t v[3], mm[3];
            GetVec3S16(m, scratch + 0, v);
            GetVec3S16(m, scratch + 8, mm);
            return static_cast<uint32_t>(rr::sim::DotLcm(v, mm));
        }});

    rows.push_back(Row{
        "vec_muladd", "void MulAdd(const s32 base[3], const s16 dir[3], s32 t, s32 out[3])", kVecMulAdd,
        [](CaseContext& c, Args& args) {
            int32_t base[3];
            int16_t dir[3];
            int32_t t;
            if (c.sample) {
                for (int i = 0; i < 3; ++i) base[i] = c.sample->p[i];
                const size_t row = c.rng->Next() % 3u;
                for (int i = 0; i < 3; ++i) dir[i] = c.sample->mA[row * 3 + static_cast<size_t>(i)];
                t = c.sample->speed;
            } else {
                for (int i = 0; i < 3; ++i) base[i] = c.rng->World();
                for (int i = 0; i < 3; ++i) dir[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                t = c.rng->S32();
            }
            PutVec3S32(*c.mem, c.scratch + 0, base);
            PutVec3S16(*c.mem, c.scratch + 16, dir);
            args.a[0] = c.scratch + 0;
            args.a[1] = c.scratch + 16;
            args.a[2] = static_cast<uint32_t>(t);
            args.a[3] = c.scratch + 32;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            int32_t base[3], out[3];
            int16_t dir[3];
            GetVec3S32(m, scratch + 0, base);
            GetVec3S16(m, scratch + 16, dir);
            rr::sim::MulAdd(base, dir, static_cast<int32_t>(args.a[2]), out);
            PutVec3S32(m, scratch + 32, out);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "vec_muladd32", "void MulAdd32(const s32 base[3], const s32 dir[3], s32 t, s32 out[3])",
        kVecMulAdd32,
        [](CaseContext& c, Args& args) {
            int32_t base[3], dir[3], t;
            if (c.sample) {
                for (int i = 0; i < 3; ++i) base[i] = c.sample->p[i];
                // A real 16.16 direction: the difference between two captured world points, which
                // is the shape the per-bike step's velocity integrator actually feeds it.
                for (int i = 0; i < 3; ++i)
                    dir[i] = static_cast<int32_t>(static_cast<uint32_t>(c.sample->q[i]) -
                                                  static_cast<uint32_t>(c.sample->p[i]));
                t = c.sample->speed;
            } else {
                for (int i = 0; i < 3; ++i) base[i] = c.rng->World();
                for (int i = 0; i < 3; ++i) dir[i] = (c.rng->Next() & 1u) ? c.rng->S32() : c.rng->World();
                t = c.rng->S32();
            }
            PutVec3S32(*c.mem, c.scratch + 0, base);
            PutVec3S32(*c.mem, c.scratch + 16, dir);
            args.a[0] = c.scratch + 0;
            args.a[1] = c.scratch + 16;
            args.a[2] = static_cast<uint32_t>(t);
            args.a[3] = c.scratch + 32;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            int32_t base[3], dir[3], out[3];
            GetVec3S32(m, scratch + 0, base);
            GetVec3S32(m, scratch + 16, dir);
            rr::sim::MulAdd32(base, dir, static_cast<int32_t>(args.a[2]), out);
            PutVec3S32(m, scratch + 32, out);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "vec_scale", "void Scale(s32 t, const s16 dir[3], s32 out[3])", kVecScale,
        [](CaseContext& c, Args& args) {
            int16_t dir[3];
            int32_t t;
            if (c.sample) {
                const size_t row = c.rng->Next() % 3u;
                for (int i = 0; i < 3; ++i) dir[i] = c.sample->mB[row * 3 + static_cast<size_t>(i)];
                t = c.sample->speed;
            } else {
                for (int i = 0; i < 3; ++i) dir[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                t = c.rng->S32();
            }
            PutVec3S16(*c.mem, c.scratch + 0, dir);
            args.a[0] = static_cast<uint32_t>(t);
            args.a[1] = c.scratch + 0;
            args.a[2] = c.scratch + 16;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            int16_t dir[3];
            int32_t out[3];
            GetVec3S16(m, scratch + 0, dir);
            rr::sim::Scale(static_cast<int32_t>(args.a[0]), dir, out);
            PutVec3S32(m, scratch + 16, out);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "road_project", "void RoadProject(const s32 p[3], const Slice*, s32 *lat, s32 *along)",
        kRoadProject,
        [](CaseContext& c, Args& args) {
            // Slice layout (docs\formats\road_chunk.md 3): u16 index; i16 m[9]; i32 pos[3].
            int32_t p[3], pos[3];
            int16_t m[9];
            if (c.sample) {
                for (int i = 0; i < 3; ++i) {
                    p[i] = c.sample->p[i];
                    pos[i] = c.sample->q[i];
                }
                for (int i = 0; i < 9; ++i) m[i] = c.sample->mA[i];
            } else {
                for (int i = 0; i < 3; ++i) {
                    p[i] = c.rng->World();
                    pos[i] = c.rng->World();
                }
                for (int i = 0; i < 9; ++i) m[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
            }
            const uint32_t slice = c.scratch + 16;
            c.mem->PokeWord(slice, 0); // the u16 index plus the first matrix entry - rewritten below
            for (int i = 0; i < 9; ++i) PutS16(*c.mem, slice + 2u + 2u * static_cast<uint32_t>(i), m[i]);
            PutVec3S32(*c.mem, slice + 20u, pos);
            PutVec3S32(*c.mem, c.scratch + 0, p);
            args.a[0] = c.scratch + 0;
            args.a[1] = slice;
            // Exercise both null-pointer arms, which are real call sites (0x800370AC passes a null
            // `along`), not a hypothetical.
            const uint64_t roll = c.rng->Next() % 8u;
            args.a[2] = (roll == 1) ? 0u : (c.scratch + 72u);
            args.a[3] = (roll == 2) ? 0u : (c.scratch + 76u);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            const uint32_t slice = scratch + 16;
            int32_t p[3], pos[3];
            int16_t mm[9];
            GetVec3S32(m, scratch + 0, p);
            for (int i = 0; i < 9; ++i) mm[i] = GetS16(m, slice + 2u + 2u * static_cast<uint32_t>(i));
            GetVec3S32(m, slice + 20u, pos);
            rr::sim::RoadSliceView view{mm, pos};
            int32_t lateral = 0, along = 0;
            rr::sim::RoadProject(p, view, args.a[2] ? &lateral : nullptr, args.a[3] ? &along : nullptr);
            if (args.a[2]) PutS32(m, args.a[2], lateral);
            if (args.a[3]) PutS32(m, args.a[3], along);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "rat_atan2", "s32 RatAtan2(s32 y, s32 x)  [4096 = one turn]", kRatAtan2,
        [](CaseContext& c, Args& args) {
            if (c.sample) {
                args.a[0] = static_cast<uint32_t>(c.sample->p[0] - c.sample->q[0]);
                args.a[1] = static_cast<uint32_t>(c.sample->p[2] - c.sample->q[2]);
            } else {
                args.a[0] = static_cast<uint32_t>(c.rng->World());
                args.a[1] = static_cast<uint32_t>(c.rng->World());
            }
            // The zero arms are the ones with the hand-written early-outs, so hit them often.
            const uint64_t roll = c.rng->Next() % 12u;
            if (roll == 0) args.a[0] = 0;
            else if (roll == 1) args.a[1] = 0;
            else if (roll == 2) { args.a[0] = 0; args.a[1] = 0; }
            else if (roll == 3) args.a[1] = args.a[0]; // |x| == |y|, the table's last entry
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            int32_t table[20];
            for (int i = 0; i < 20; ++i) table[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));
            return static_cast<uint32_t>(rr::sim::RatAtan2(static_cast<int32_t>(args.a[0]),
                                                           static_cast<int32_t>(args.a[1]), table));
        }});

    rows.push_back(Row{
        "vec_blend32", "void Blend32(const s32 a[3], const s32 b[3], s32 out[3], s32 wa, s32 wb)",
        kBlend32,
        [](CaseContext& c, Args& args) {
            int32_t a[3], b[3];
            int32_t wa, wb;
            if (c.sample) {
                for (int i = 0; i < 3; ++i) { a[i] = c.sample->p[i]; b[i] = c.sample->q[i]; }
                wa = c.sample->speed;
                wb = c.sample->lateral;
            } else {
                for (int i = 0; i < 3; ++i) { a[i] = c.rng->World(); b[i] = c.rng->World(); }
                wa = c.rng->S32();
                wb = c.rng->S32();
            }
            PutVec3S32(*c.mem, c.scratch + 0, a);
            PutVec3S32(*c.mem, c.scratch + 16, b);
            args.a[0] = c.scratch + 0;
            args.a[1] = c.scratch + 16;
            args.a[2] = c.scratch + 32;
            args.a[3] = static_cast<uint32_t>(wa);
            args.a4 = static_cast<uint32_t>(wb);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            int32_t a[3], b[3], out[3];
            GetVec3S32(m, scratch + 0, a);
            GetVec3S32(m, scratch + 16, b);
            rr::sim::Blend32(a, b, out, static_cast<int32_t>(args.a[3]), static_cast<int32_t>(args.a4));
            PutVec3S32(m, scratch + 32, out);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "vec_blend16", "void Blend16(const s16 a[3], const s16 b[3], s16 out[3], s32 wa, s32 wb)",
        kBlend16,
        [](CaseContext& c, Args& args) {
            int16_t a[3], b[3];
            int32_t wa, wb;
            if (c.sample) {
                const size_t rowA = c.rng->Next() % 3u;
                const size_t rowB = c.rng->Next() % 3u;
                for (int i = 0; i < 3; ++i) {
                    a[i] = c.sample->mA[rowA * 3 + static_cast<size_t>(i)];
                    b[i] = c.sample->mB[rowB * 3 + static_cast<size_t>(i)];
                }
                wa = c.sample->speed;
                wb = 0x10000 - c.sample->speed;
            } else {
                for (int i = 0; i < 3; ++i) {
                    a[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                    b[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                }
                wa = c.rng->S32();
                wb = c.rng->S32();
            }
            PutVec3S16(*c.mem, c.scratch + 0, a);
            PutVec3S16(*c.mem, c.scratch + 8, b);
            args.a[0] = c.scratch + 0;
            args.a[1] = c.scratch + 8;
            args.a[2] = c.scratch + 16;
            args.a[3] = static_cast<uint32_t>(wa);
            args.a4 = static_cast<uint32_t>(wb);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            int16_t a[3], b[3], out[3];
            GetVec3S16(m, scratch + 0, a);
            GetVec3S16(m, scratch + 8, b);
            rr::sim::Blend16(a, b, out, static_cast<int32_t>(args.a[3]), static_cast<int32_t>(args.a4));
            PutVec3S16(m, scratch + 16, out);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "vec_normalize", "void Normalize(s16 v[3])  [GTE SQR + the gp+2260 rsqrt table]", kNormalize,
        [](CaseContext& c, Args& args) {
            int16_t v[3];
            if (c.sample) {
                const size_t row = c.rng->Next() % 3u;
                for (int i = 0; i < 3; ++i) v[i] = c.sample->mA[row * 3 + static_cast<size_t>(i)];
            } else {
                // Bounded so that x*x + y*y + z*z cannot overflow the ORIGINAL's trapping `add`
                // at 0x8002E4A0: 3 * 26754^2 < 2^31. Handing it a larger vector would make the
                // console raise an arithmetic-overflow exception, which is a fact about the
                // original worth recording but not a comparison.
                for (int i = 0; i < 3; ++i)
                    v[i] = static_cast<int16_t>(static_cast<int32_t>(c.rng->U32() % 53509u) - 26754);
                if (c.rng->Next() % 16u == 0) { v[0] = 0; v[1] = 0; v[2] = 0; }
            }
            PutVec3S16(*c.mem, c.scratch + 0, v);
            args.a[0] = c.scratch + 0;
        },
        [](Memory& m, uint32_t scratch, uint32_t gp, const Args&) {
            const uint32_t tableAddr = m.PeekWord(gp + kRsqrtTableGpOffset);
            std::vector<uint16_t> table(2048);
            for (size_t i = 0; i < table.size(); ++i)
                table[i] = static_cast<uint16_t>(m.PeekByte(tableAddr + 2u * static_cast<uint32_t>(i)) |
                                                 (static_cast<uint16_t>(m.PeekByte(
                                                      tableAddr + 2u * static_cast<uint32_t>(i) + 1u))
                                                  << 8));
            int16_t v[3];
            GetVec3S16(m, scratch + 0, v);
            rr::sim::Normalize(v, table.data());
            PutVec3S16(m, scratch + 0, v);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "clamp_lerp_min", "s32 ClampLerpMin(s32 a, s32 b, s32 t)  [RASHCDG]", kClampLerpMin,
        [](CaseContext& c, Args& args) {
            if (c.sample) {
                args.a[0] = static_cast<uint32_t>(c.sample->speed);
                args.a[1] = static_cast<uint32_t>(c.sample->lateral);
                args.a[2] = static_cast<uint32_t>(c.sample->along & 0x1FFFF);
            } else {
                args.a[0] = c.rng->U32();
                args.a[1] = c.rng->U32();
                // Half the cases inside [0, 1.0], half outside, so both clamp arms are exercised.
                args.a[2] = (c.rng->Next() & 1u) ? (c.rng->U32() % 0x10001u) : c.rng->U32();
            }
        },
        [](Memory&, uint32_t, uint32_t, const Args& args) {
            return static_cast<uint32_t>(rr::sim::ClampLerpMin(static_cast<int32_t>(args.a[0]),
                                                               static_cast<int32_t>(args.a[1]),
                                                               static_cast<int32_t>(args.a[2])));
        }});

    rows.push_back(Row{
        "vec_blend16to32", "void Blend16To32(const s16 a[3], const s16 b[3], s32 out[3], s32, s32)",
        kBlend16To32,
        [](CaseContext& c, Args& args) {
            int16_t a[3], b[3];
            int32_t wa, wb;
            if (c.sample) {
                const size_t rowA = c.rng->Next() % 3u;
                const size_t rowB = c.rng->Next() % 3u;
                for (int i = 0; i < 3; ++i) {
                    a[i] = c.sample->mA[rowA * 3 + static_cast<size_t>(i)];
                    b[i] = c.sample->mB[rowB * 3 + static_cast<size_t>(i)];
                }
                wa = c.sample->speed;
                wb = c.sample->lateral;
            } else {
                for (int i = 0; i < 3; ++i) {
                    a[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                    b[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                }
                wa = c.rng->S32();
                wb = c.rng->S32();
            }
            PutVec3S16(*c.mem, c.scratch + 0, a);
            PutVec3S16(*c.mem, c.scratch + 8, b);
            args.a[0] = c.scratch + 0;
            args.a[1] = c.scratch + 8;
            args.a[2] = c.scratch + 16;
            args.a[3] = static_cast<uint32_t>(wa);
            args.a4 = static_cast<uint32_t>(wb);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            int16_t a[3], b[3];
            int32_t out[3];
            GetVec3S16(m, scratch + 0, a);
            GetVec3S16(m, scratch + 8, b);
            rr::sim::Blend16To32(a, b, out, static_cast<int32_t>(args.a[3]), static_cast<int32_t>(args.a4));
            PutVec3S32(m, scratch + 16, out);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "road_find_piece", "const Entry *FindRoadPiece(const RoadObject *, s32 key)", kFindRoadPiece,
        [](CaseContext& c, Args& args) {
            // The road object and its 32-byte piece table are built in the scratch block; the KEYS
            // are the road ids and along-distances of real captured motorcycles, so the comparison
            // the function actually performs is driven by dump-derived values even though the
            // container is synthetic (the live tables are not reachable from a RAM dump alone).
            const uint32_t obj = c.scratch;
            const uint32_t entries = c.scratch + 64;
            const int32_t count = static_cast<int32_t>(c.rng->U32() % 25u);
            const int16_t kind = (c.rng->Next() % 4u == 0) ? static_cast<int16_t>(c.rng->U32() & 7u)
                                                           : static_cast<int16_t>(1);
            for (uint32_t i = 0; i < 64; i += 4) c.mem->PokeWord(obj + i, 0);
            PutS16(*c.mem, obj + 0x10, kind);
            PutS16(*c.mem, obj + 0x12, static_cast<int16_t>(count));
            PutS32(*c.mem, obj + 0x2C, static_cast<int32_t>(entries));
            int32_t chosen = 0;
            for (int32_t i = 0; i < 25; ++i) {
                const uint32_t e = entries + static_cast<uint32_t>(i) * 32u;
                for (uint32_t k = 0; k < 32; k += 4) c.mem->PokeWord(e + k, c.rng->U32());
                const int32_t key = c.sample ? (c.sample->along ^ (c.sample->lateral * (i + 1)))
                                             : c.rng->S32();
                PutS32(*c.mem, e + 12, key);
                // Most entries carry a zero halfword at +0x02, which is what makes them eligible.
                PutS16(*c.mem, e + 2, (c.rng->Next() % 4u == 0) ? c.rng->S16() : static_cast<int16_t>(0));
                if (i == 0 || c.rng->Next() % 5u == 0) chosen = key;
            }
            args.a[0] = (c.rng->Next() % 16u == 0) ? 0u : obj;
            args.a[1] = static_cast<uint32_t>((c.rng->Next() & 1u) ? chosen : c.rng->S32());
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            if (args.a[0] == 0) return 0u;
            const uint32_t obj = args.a[0];
            const int16_t kind = GetS16(m, obj + 0x10);
            const int16_t count = GetS16(m, obj + 0x12);
            const uint32_t entries = static_cast<uint32_t>(GetS32(m, obj + 0x2C));
            std::vector<uint8_t> table(static_cast<size_t>(25) * rr::sim::kRoadPieceEntrySize);
            for (size_t i = 0; i < table.size(); ++i) table[i] = m.PeekByte(entries + static_cast<uint32_t>(i));
            const int32_t idx = rr::sim::FindRoadPieceIndex(kind, count, table.data(),
                                                            static_cast<int32_t>(args.a[1]));
            if (idx < 0) return 0u;
            return entries + static_cast<uint32_t>(idx) * rr::sim::kRoadPieceEntrySize;
        }});

    rows.push_back(Row{
        "impact_dir", "s32 SetImpactDirection(Bike*, const s16 hit[3])  [RASHCDG]", kImpactDir,
        [](CaseContext& c, Args& args) {
            // A real 1096-byte motorcycle, copied out of a real RAM image, with the three inputs
            // the function actually branches on randomised around it.
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&e](uint32_t off, uint16_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            // Randomise exactly the gate bits, so both the "rejected" and the "latched" paths run.
            const uint32_t roll = c.rng->U32();
            put32(rr::sim::ent::kFlagsC, ((roll & 3u) == 0u) ? (c.rng->U32() & 0x60Fu) : 0u);
            put32(rr::sim::ent::kFlagsB, ((roll & 12u) == 0u) ? 0x400u : (c.rng->U32() & ~0x400u));
            for (int i = 0; i < 3; ++i) {
                const int16_t h = (roll & 16u) ? c.rng->Unit() : c.rng->S16();
                put16(rr::sim::ent::kHeading + 2u * static_cast<uint32_t>(i), static_cast<uint16_t>(h));
            }
            int16_t hit[3];
            for (int i = 0; i < 3; ++i) {
                int16_t h = (c.rng->Next() & 1u) ? c.rng->Unit() : c.rng->S16();
                // -32768 in both x and z is the one input pair on which the original's own
                // normalisation raises an arithmetic-overflow exception; see vec.h.
                if (h == static_cast<int16_t>(-32768)) h = static_cast<int16_t>(-32767);
                hit[i] = h;
            }
            if (c.rng->Next() % 3u == 0) hit[1] = static_cast<int16_t>(c.rng->U32() % 8000u);
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            PutVec3S16(*c.mem, c.scratch + 1104, hit);
            args.a[0] = c.scratch;
            args.a[1] = c.scratch + 1104;
        },
        [](Memory& m, uint32_t scratch, uint32_t gp, const Args& args) {
            const uint32_t tableAddr = m.PeekWord(gp + kRsqrtTableGpOffset);
            std::vector<uint16_t> table(2048);
            for (size_t i = 0; i < table.size(); ++i)
                table[i] = static_cast<uint16_t>(m.PeekByte(tableAddr + 2u * static_cast<uint32_t>(i)) |
                                                 (static_cast<uint16_t>(m.PeekByte(
                                                      tableAddr + 2u * static_cast<uint32_t>(i) + 1u))
                                                  << 8));
            std::vector<uint8_t> e(1096);
            for (size_t i = 0; i < e.size(); ++i) e[i] = m.PeekByte(scratch + static_cast<uint32_t>(i));
            int16_t hit[3];
            GetVec3S16(m, args.a[1], hit);
            rr::sim::EntityView view(e.data());
            const int32_t r = rr::sim::SetImpactDirection(view, hit, table.data());
            m.WriteBlock(scratch, e.data(), e.size());
            return static_cast<uint32_t>(r);
        }});

    rows.push_back(Row{
        "bike_idle_step", "void BikeIdleStep(Bike*, s32 dt)  [RASHCDG, the standing-still model]",
        kBikeIdleStep,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            // The stat block the entity points at: 29 words at +60 plus the two at +176 / +184.
            const uint32_t stats = c.scratch + 1152;
            for (uint32_t i = 0; i < 192; i += 4) c.mem->PokeWord(stats + i, 0);
            for (int32_t i = 0; i < 29; ++i) {
                const int32_t v = c.sample ? ((c.sample->speed >> (i & 7)) ^ (c.sample->along >> 3))
                                           : c.rng->S32();
                PutS32(*c.mem, stats + 60u + 4u * static_cast<uint32_t>(i), v);
            }
            PutS32(*c.mem, stats + 176, c.sample ? c.sample->along : c.rng->S32());
            PutS32(*c.mem, stats + 184, c.sample ? c.sample->lateral : c.rng->S32());
            put32(rr::sim::ent::kStats, stats);
            // Bit 8 of flagsA picks the wobble's sign and bits 8..9 gate the wobble at all, so all
            // four combinations are driven.
            const uint32_t roll = c.rng->U32();
            put32(rr::sim::ent::kFlagsA, (roll & 0x300u) | (c.rng->U32() & ~0x300u));
            // The idle accumulators start either where the console had them or anywhere at all.
            for (uint32_t off : {rr::sim::ent::kIdleRev, rr::sim::ent::kIdleYaw, rr::sim::ent::kIdleA,
                                 rr::sim::ent::kIdleB, rr::sim::ent::kIdleLean}) {
                if (!c.sample || (c.rng->Next() & 1u) != 0)
                    put32(off, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            }
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            c.mem->PokeWord(c.gp + kRandSeedGpOffset, c.rng->U32());
            args.a[0] = c.scratch;
            args.a[1] = c.sample ? static_cast<uint32_t>(0x884) : c.rng->U32();
        },
        [](Memory& m, uint32_t scratch, uint32_t gp, const Args& args) {
            std::vector<uint8_t> e(1096);
            for (size_t i = 0; i < e.size(); ++i) e[i] = m.PeekByte(scratch + static_cast<uint32_t>(i));
            rr::sim::EntityView view(e.data());
            const uint32_t statsAddr = view.U32(rr::sim::ent::kStats);
            std::vector<uint8_t> stats(192);
            for (size_t i = 0; i < stats.size(); ++i)
                stats[i] = m.PeekByte(statsAddr + static_cast<uint32_t>(i));
            uint32_t seed = m.PeekWord(gp + kRandSeedGpOffset);
            rr::sim::BikeIdleStep(view, static_cast<int32_t>(args.a[1]), stats.data(), seed);
            m.PokeWord(gp + kRandSeedGpOffset, seed);
            m.WriteBlock(scratch, e.data(), e.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "vec_scale32", "void Scale32(s32 t, const s32 v[3], s32 out[3])", kScale32,
        [](CaseContext& c, Args& args) {
            int32_t v[3];
            int32_t t;
            if (c.sample) {
                for (int i = 0; i < 3; ++i) v[i] = c.sample->p[i];
                t = c.sample->speed;
            } else {
                for (int i = 0; i < 3; ++i) v[i] = c.rng->S32();
                t = c.rng->S32();
            }
            PutVec3S32(*c.mem, c.scratch + 0, v);
            args.a[0] = static_cast<uint32_t>(t);
            args.a[1] = c.scratch + 0;
            args.a[2] = c.scratch + 16;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            int32_t v[3], out[3];
            GetVec3S32(m, scratch + 0, v);
            rr::sim::Scale32(static_cast<int32_t>(args.a[0]), v, out);
            PutVec3S32(m, scratch + 16, out);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "rat_tan", "s32 RatTan(s32 angle)  [4096 = one turn]", kRatTan,
        [](CaseContext& c, Args& args) {
            // Every one of the 4096 table entries is reachable, so sweep them as well as the
            // captured angles: the sign logic changes quadrant by quadrant.
            args.a[0] = c.sample ? static_cast<uint32_t>(c.sample->lateral >> 8) : c.rng->U32();
            if (c.rng->Next() % 3u == 0) args.a[0] = c.rng->U32() % 4096u;
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            std::vector<int16_t> table(8192);
            for (size_t i = 0; i < table.size(); ++i)
                table[i] = GetS16(m, kSinCosTable + 2u * static_cast<uint32_t>(i));
            return static_cast<uint32_t>(rr::sim::RatTan(static_cast<int32_t>(args.a[0]), table.data()));
        }});

    rows.push_back(Row{
        "build_obb", "void BuildObb(Entity*)  [RASHCDG, the collision bounding box]", kBuildObb,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&e](uint32_t off, uint16_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            // Drive all four pool arms: 0 (with and without the crouch), 3, 4 below and above the
            // slot-30 threshold, and one pool the function ignores.
            static const uint32_t kPools[6] = {0, 0, 3, 4, 4, 2};
            const uint32_t pool = kPools[c.rng->Next() % 6u];
            const uint32_t slot = (c.rng->Next() & 1u) ? (c.rng->U32() % 32u) : 31u;
            put16(rr::sim::ent::kHandle, static_cast<uint16_t>((pool << 5) | slot));
            put32(rr::sim::ent::kClass, (c.rng->Next() & 1u) ? (c.rng->U32() % 40u) : c.rng->U32());
            // The owner pointer is dereferenced unconditionally on the pool-0 arm, so it has to be
            // a real address: park a small owner object at the top of the scratch block.
            const uint32_t owner = c.scratch + 1152;
            for (uint32_t i = 0; i < 640; i += 4) c.mem->PokeWord(owner + i, 0);
            c.mem->PokeWord(owner + 604, (c.rng->Next() & 1u) ? 2u : c.rng->U32() % 2u);
            put32(rr::sim::ent::kOwner, owner);
            for (uint32_t off : {rr::sim::ent::kHalfX, rr::sim::ent::kHalfY, rr::sim::ent::kHalfZ}) {
                put32(off, static_cast<uint32_t>(c.sample ? (c.sample->speed >> 4)
                                                          : (c.rng->S32() >> (c.rng->Next() % 12u))));
            }
            if (!c.sample || (c.rng->Next() & 1u) != 0) {
                for (int i = 0; i < 9; ++i)
                    put16(rr::sim::ent::kAxes + 2u * static_cast<uint32_t>(i),
                          static_cast<uint16_t>((c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit()));
            }
            for (int i = 0; i < 3; ++i)
                put32(rr::sim::ent::kObbCentre + 4u * static_cast<uint32_t>(i),
                      static_cast<uint32_t>(c.sample ? c.sample->p[i] : c.rng->World()));
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            args.a[0] = c.scratch;
            args.a[1] = c.rng->U32();
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            std::vector<uint8_t> e(1096);
            for (size_t i = 0; i < e.size(); ++i) e[i] = m.PeekByte(scratch + static_cast<uint32_t>(i));
            rr::sim::EntityView view(e.data());
            const uint32_t owner = view.U32(rr::sim::ent::kOwner);
            const int32_t ownerIdleRev = (owner != 0) ? static_cast<int32_t>(m.PeekWord(owner + 604)) : 0;
            rr::sim::BuildObb(view, ownerIdleRev);
            m.WriteBlock(scratch, e.data(), e.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "bike_steer_lean", "void BikeSteerLean(Bike*)  [RASHCDG, steering and lean]", kSteerLean,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            const uint32_t stats = c.scratch + 1152;
            for (uint32_t i = 0; i < 320; i += 4) c.mem->PokeWord(stats + i, 0);
            // The four stat words the model reads. Their real magnitudes are unknown, so both
            // "small, plausible" and "anything at all" are driven.
            const bool tame = (c.rng->Next() & 1u) != 0;
            PutS32(*c.mem, stats + 4, tame ? (c.rng->S32() >> 12) : c.rng->S32());
            PutS32(*c.mem, stats + 232, tame ? (c.rng->S32() >> 10) : c.rng->S32());
            PutS32(*c.mem, stats + 280, tame ? (c.rng->S32() >> 14) : c.rng->S32());
            PutS32(*c.mem, stats + 284, tame ? (c.rng->S32() >> 14) : c.rng->S32());
            put32(rr::sim::ent::kStats, stats);
            // steer == 0 and speed <= 0.5 are the two early-out arms; hit both often.
            const uint64_t roll = c.rng->Next() % 8u;
            put32(rr::sim::ent::kSteer, (roll == 0) ? 0u
                                                    : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            put32(rr::sim::ent::kSpeedCopy,
                  (roll == 1) ? static_cast<uint32_t>(c.rng->U32() % 0x8001u)
                              : (c.sample ? static_cast<uint32_t>(c.sample->speed) : c.rng->U32() >> 4));
            put32(rr::sim::ent::kSteerBias, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            put32(rr::sim::ent::kLeanBias, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            put32(rr::sim::ent::kLeanGain, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            e[rr::sim::ent::kMode] = static_cast<uint8_t>(c.rng->U32() % 5u);
            e[rr::sim::ent::kLean] = static_cast<uint8_t>(c.rng->U32());
            e[rr::sim::ent::kLean + 1] = static_cast<uint8_t>(c.rng->U32());
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            args.a[0] = c.scratch;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            std::vector<uint8_t> e(1096);
            for (size_t i = 0; i < e.size(); ++i) e[i] = m.PeekByte(scratch + static_cast<uint32_t>(i));
            rr::sim::EntityView view(e.data());
            const uint32_t statsAddr = view.U32(rr::sim::ent::kStats);
            std::vector<uint8_t> stats(320);
            for (size_t i = 0; i < stats.size(); ++i)
                stats[i] = m.PeekByte(statsAddr + static_cast<uint32_t>(i));
            std::vector<int16_t> table(8192);
            for (size_t i = 0; i < table.size(); ++i)
                table[i] = GetS16(m, kSinCosTable + 2u * static_cast<uint32_t>(i));
            rr::sim::BikeSteerLean(view, stats.data(), table.data());
            m.WriteBlock(scratch, e.data(), e.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "bike_solve_steer", "void BikeSolveSteer(Bike*)  [RASHCDG, the inverse steering model]",
        kSolveSteer,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            const uint32_t stats = c.scratch + 1152;
            for (uint32_t i = 0; i < 320; i += 4) c.mem->PokeWord(stats + i, 0);
            const bool tame = (c.rng->Next() & 1u) != 0;
            PutS32(*c.mem, stats + 4, tame ? (c.rng->S32() >> 12) : c.rng->S32());
            PutS32(*c.mem, stats + 228, tame ? (c.rng->S32() >> 10) : c.rng->S32());
            PutS32(*c.mem, stats + 232, tame ? (c.rng->S32() >> 10) : c.rng->S32());
            PutS32(*c.mem, stats + 240, tame ? (c.rng->S32() >> 10) : c.rng->S32());
            PutS32(*c.mem, stats + 280, tame ? (c.rng->S32() >> 14) : c.rng->S32());
            PutS32(*c.mem, stats + 284, tame ? (c.rng->S32() >> 14) : c.rng->S32());
            put32(rr::sim::ent::kStats, stats);
            const uint64_t roll = c.rng->Next() % 6u;
            put32(rr::sim::ent::kSpeedCopy,
                  (roll == 0) ? (c.rng->U32() % 6555u)
                              : (c.sample ? static_cast<uint32_t>(c.sample->speed) : c.rng->U32() >> 4));
            put32(rr::sim::ent::kLatAccel, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            put32(rr::sim::ent::kSteerBias, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            put32(rr::sim::ent::kLeanBias, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            put32(rr::sim::ent::kLeanGain, (roll == 1) ? 0u
                                                       : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            put32(rr::sim::ent::kSteer, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            e[rr::sim::ent::kMode] = static_cast<uint8_t>(c.rng->U32() % 5u);
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            args.a[0] = c.scratch;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            std::vector<uint8_t> e(1096);
            for (size_t i = 0; i < e.size(); ++i) e[i] = m.PeekByte(scratch + static_cast<uint32_t>(i));
            rr::sim::EntityView view(e.data());
            const uint32_t statsAddr = view.U32(rr::sim::ent::kStats);
            std::vector<uint8_t> stats(320);
            for (size_t i = 0; i < stats.size(); ++i)
                stats[i] = m.PeekByte(statsAddr + static_cast<uint32_t>(i));
            std::vector<int16_t> sincos(8192);
            for (size_t i = 0; i < sincos.size(); ++i)
                sincos[i] = GetS16(m, kSinCosTable + 2u * static_cast<uint32_t>(i));
            int32_t atan[20];
            for (int i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));
            rr::sim::BikeSolveSteer(view, stats.data(), atan, sincos.data());
            m.WriteBlock(scratch, e.data(), e.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "copy_halfwords", "void CopyHalfwords(s32 n, const u16 *src, u16 *dst)", kHalfCopy,
        [](CaseContext& c, Args& args) {
            const int32_t n = (c.rng->Next() % 8u == 0) ? (c.rng->S32() >> 24)
                                                        : static_cast<int32_t>(c.rng->U32() % 33u);
            for (uint32_t i = 0; i < 256; i += 4) c.mem->PokeWord(c.scratch + i, c.rng->U32());
            args.a[0] = static_cast<uint32_t>(n);
            args.a[1] = c.scratch + 0;
            args.a[2] = c.scratch + 128;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            const int32_t n = static_cast<int32_t>(args.a[0]);
            for (int32_t i = 0; i < n; ++i) {
                const uint16_t v = static_cast<uint16_t>(
                    m.PeekByte(scratch + 2u * static_cast<uint32_t>(i)) |
                    (static_cast<uint16_t>(m.PeekByte(scratch + 2u * static_cast<uint32_t>(i) + 1u)) << 8));
                m.WriteBlock(scratch + 128u + 2u * static_cast<uint32_t>(i), &v, 2);
            }
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "bike_rider_pose", "void BikeRiderPose(Bike*, s32 dt)  [RASHCDG, pose + both bounding boxes]",
        kRiderPose,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            auto fill = [&c](std::vector<uint8_t>& e, uint32_t ownerAddr) {
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                auto put16 = [&e](uint32_t off, uint16_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                };
                put16(rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() & 0xFFu));
                put32(rr::sim::ent::kClass, c.rng->U32() % 40u);
                put32(rr::sim::ent::kOwner, ownerAddr);
                for (uint32_t off : {rr::sim::ent::kHalfX, rr::sim::ent::kHalfY, rr::sim::ent::kHalfZ})
                    put32(off, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
                for (int i = 0; i < 9; ++i)
                    put16(rr::sim::ent::kAxes + 2u * static_cast<uint32_t>(i),
                          static_cast<uint16_t>(c.rng->Unit()));
                for (int i = 0; i < 3; ++i)
                    put32(rr::sim::ent::kObbCentre + 4u * static_cast<uint32_t>(i),
                          static_cast<uint32_t>(c.rng->World()));
                for (uint32_t off : {rr::sim::ent::kPoseA, rr::sim::ent::kPoseB, rr::sim::ent::kPoseC})
                    put16(off, static_cast<uint16_t>(c.rng->U32()));
            };
            std::vector<uint8_t> bike = src.entity;
            if (bike.size() < 1096) bike.resize(1096, 0);
            std::vector<uint8_t> rider(1096, 0);
            const uint32_t riderAddr = c.scratch + 1152;
            const uint32_t ownerAddr = c.scratch + 2304;
            for (uint32_t i = 0; i < 640; i += 4) c.mem->PokeWord(ownerAddr + i, 0);
            c.mem->PokeWord(ownerAddr + 604, (c.rng->Next() & 1u) ? 2u : 0u);
            fill(bike, ownerAddr);
            fill(rider, ownerAddr);
            auto put32b = [&bike](uint32_t off, uint32_t v) {
                bike[off] = static_cast<uint8_t>(v);
                bike[off + 1] = static_cast<uint8_t>(v >> 8);
                bike[off + 2] = static_cast<uint8_t>(v >> 16);
                bike[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            const uint64_t roll = c.rng->Next() % 6u;
            put32b(rr::sim::ent::kRider, (roll == 0) ? 0u : riderAddr);
            bike[rr::sim::ent::kMode] = static_cast<uint8_t>(c.rng->U32() % 4u);
            put32b(rr::sim::ent::kSpeed,
                   c.sample ? static_cast<uint32_t>(c.sample->speed) : (c.rng->U32() >> 4));
            put32b(rr::sim::ent::kFlagsC, (c.rng->Next() & 1u) ? (c.rng->U32() & 0x7FFu) : 0u);
            put32b(rr::sim::ent::kFlagsB, (c.rng->Next() & 1u) ? 0x08000000u : 0u);
            put32b(rr::sim::ent::kIdleLean,
                   (roll == 1) ? 0u : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            put32b(rr::sim::ent::kSteer, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            bike[rr::sim::ent::kFacingX] = static_cast<uint8_t>(c.rng->U32());
            bike[rr::sim::ent::kFacingX + 1] = static_cast<uint8_t>(c.rng->U32());
            bike[rr::sim::ent::kFacingZ] = static_cast<uint8_t>(c.rng->U32());
            bike[rr::sim::ent::kFacingZ + 1] = static_cast<uint8_t>(c.rng->U32());
            c.mem->WriteBlock(c.scratch, bike.data(), bike.size());
            c.mem->WriteBlock(riderAddr, rider.data(), rider.size());
            args.a[0] = c.scratch;
            args.a[1] = c.sample ? 0x884u : c.rng->U32();
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            auto load = [&m](uint32_t addr, size_t n) {
                std::vector<uint8_t> v(n);
                for (size_t i = 0; i < n; ++i) v[i] = m.PeekByte(addr + static_cast<uint32_t>(i));
                return v;
            };
            std::vector<uint8_t> bike = load(scratch, 1096);
            rr::sim::EntityView bikeView(bike.data());
            const uint32_t riderAddr = bikeView.U32(rr::sim::ent::kRider);
            std::vector<uint8_t> rider;
            if (riderAddr != 0) rider = load(riderAddr, 1096);
            rr::sim::EntityView riderView(rider.empty() ? nullptr : rider.data());
            auto ownerRev = [&m](const rr::sim::EntityView& v) {
                const uint32_t o = const_cast<rr::sim::EntityView&>(v).U32(rr::sim::ent::kOwner);
                return (o != 0) ? static_cast<int32_t>(m.PeekWord(o + 604)) : 0;
            };
            const int32_t bikeRev = ownerRev(bikeView);
            const int32_t riderRev = rider.empty() ? 0 : ownerRev(riderView);
            std::vector<int16_t> sincos(8192);
            for (size_t i = 0; i < sincos.size(); ++i)
                sincos[i] = GetS16(m, kSinCosTable + 2u * static_cast<uint32_t>(i));
            int32_t atan[20];
            for (int i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));
            rr::sim::BikeRiderPose(bikeView, static_cast<int32_t>(args.a[1]), bikeRev,
                                   rider.empty() ? nullptr : &riderView, riderRev, atan, sincos.data());
            m.WriteBlock(scratch, bike.data(), bike.size());
            if (!rider.empty()) m.WriteBlock(riderAddr, rider.data(), rider.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "build_obb_alt", "void BuildObbAlt(Entity*)  [RASHCDG, the alternate bounding box]",
        kBuildObbAlt,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&e](uint32_t off, uint16_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            // Half the cases take the alternate path, half delegate to the ordinary builder.
            e[rr::sim::ent::kAltState] =
                (c.rng->Next() & 1u) ? 1u : static_cast<uint8_t>(c.rng->U32() & 0x7Fu);
            // The kind index is kept under 60 because the bench only mirrors 512 bytes of the
            // game's table; the guest would happily index the whole 64 KiB behind it.
            put16(rr::sim::ent::kAltKind, static_cast<uint16_t>(c.rng->U32() % 60u));
            put16(rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() & 0xFFu));
            put32(rr::sim::ent::kClass, c.rng->U32() % 40u);
            const uint32_t owner = c.scratch + 1152;
            for (uint32_t i = 0; i < 640; i += 4) c.mem->PokeWord(owner + i, 0);
            c.mem->PokeWord(owner + 604, (c.rng->Next() & 1u) ? 2u : 0u);
            put32(rr::sim::ent::kOwner, owner);
            for (uint32_t off : {rr::sim::ent::kHalfX, rr::sim::ent::kHalfY, rr::sim::ent::kHalfZ})
                put32(off, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            for (int i = 0; i < 9; ++i) {
                put16(rr::sim::ent::kAxes + 2u * static_cast<uint32_t>(i),
                      static_cast<uint16_t>(c.rng->Unit()));
                put16(rr::sim::ent::kAltAxes + 2u * static_cast<uint32_t>(i),
                      static_cast<uint16_t>(c.rng->Unit()));
            }
            for (int i = 0; i < 3; ++i) {
                put32(rr::sim::ent::kObbCentre + 4u * static_cast<uint32_t>(i),
                      static_cast<uint32_t>(c.sample ? c.sample->p[i] : c.rng->World()));
                put32(rr::sim::ent::kAltOffset + 4u * static_cast<uint32_t>(i),
                      static_cast<uint32_t>(c.rng->World()));
            }
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            args.a[0] = c.scratch;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            std::vector<uint8_t> e(1096);
            for (size_t i = 0; i < e.size(); ++i) e[i] = m.PeekByte(scratch + static_cast<uint32_t>(i));
            std::vector<uint8_t> table(512);
            for (size_t i = 0; i < table.size(); ++i)
                table[i] = m.PeekByte(kAltKindTable + static_cast<uint32_t>(i));
            rr::sim::EntityView view(e.data());
            const uint32_t owner = view.U32(rr::sim::ent::kOwner);
            const int32_t ownerIdleRev = (owner != 0) ? static_cast<int32_t>(m.PeekWord(owner + 604)) : 0;
            rr::sim::BuildObbAlt(view, table.data(), ownerIdleRev);
            m.WriteBlock(scratch, e.data(), e.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "bike_apply_steering",
        "void BikeApplySteering(Bike*, s32 hasRider, s32 extra, s32 rate)  [RASHCDG]",
        kApplySteering,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> bike = src.entity;
            if (bike.size() < 1096) bike.resize(1096, 0);
            std::vector<uint8_t> rider(1096, 0);
            auto put32 = [&bike](uint32_t off, uint32_t v) {
                bike[off] = static_cast<uint8_t>(v);
                bike[off + 1] = static_cast<uint8_t>(v >> 8);
                bike[off + 2] = static_cast<uint8_t>(v >> 16);
                bike[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            const uint32_t riderAddr = c.scratch + 1152;
            const uint32_t stats = c.scratch + 2304;
            const uint32_t riderDef = c.scratch + 2816;
            for (uint32_t i = 0; i < 512; i += 4) c.mem->PokeWord(stats + i, 0);
            c.mem->PokeWord(riderDef, c.rng->U32());
            // The stat words this function reads. Tame values keep the servo and the AI path in
            // their normal ranges; the other half of the cases is unrestricted.
            const bool tame = (c.rng->Next() & 1u) != 0;
            auto stat = [&](uint32_t off, int shift) {
                PutS32(*c.mem, stats + off, tame ? (c.rng->S32() >> shift) : c.rng->S32());
            };
            for (uint32_t off : {224u, 232u, 308u, 312u, 316u, 356u, 360u, 364u, 368u, 372u, 376u,
                                 380u, 384u, 416u})
                stat(off, 10);
            stat(4, 12);
            stat(228, 10);
            stat(240, 10);
            stat(280, 14);
            stat(284, 14);
            PutS16(*c.mem, stats + 446, (c.rng->Next() % 3u == 0) ? c.rng->S16() : static_cast<int16_t>(0));
            put32(rr::sim::ent::kStats, stats);
            put32(rr::sim::ent::kRiderDef, riderDef);
            put32(rr::sim::ent::kRider, riderAddr);
            // flagsA drives everything: bit 15 with bit 20 clear selects the timed servo, and bits
            // 7 / 16 / 17 / 21 / 22 / 24 / 27 select its arms. Half the cases are forced onto the
            // servo path and half are left free.
            uint32_t flagsA = c.rng->U32();
            if (c.rng->Next() & 1u) flagsA = (flagsA & ~0x108000u) | 0x8000u;
            put32(rr::sim::ent::kFlagsA, flagsA);
            put32(rr::sim::ent::kFlagsB, c.rng->U32());
            put32(rr::sim::ent::kFlagsC, c.rng->U32());
            put32(rr::sim::ent::kSpeed,
                  c.sample ? static_cast<uint32_t>(c.sample->speed) : (c.rng->U32() >> 4));
            put32(rr::sim::ent::kSpeedCopy,
                  c.sample ? static_cast<uint32_t>(c.sample->speed) : (c.rng->U32() >> 4));
            const uint64_t roll = c.rng->Next() % 8u;
            put32(rr::sim::ent::kSteer,
                  (roll == 0) ? 0u : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            put32(rr::sim::ent::kSteerPhase,
                  (roll == 1) ? 0u : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            for (uint32_t off : {rr::sim::ent::kSteerRate, rr::sim::ent::kSteerSpan,
                                 rr::sim::ent::kSteerFrom, rr::sim::ent::kSteerScratch,
                                 rr::sim::ent::kDriftLimit, rr::sim::ent::kLatAccel,
                                 rr::sim::ent::kIdleLean, rr::sim::ent::kSteerBias,
                                 rr::sim::ent::kLeanBias, rr::sim::ent::kLeanGain})
                put32(off, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            bike[rr::sim::ent::kMode] = static_cast<uint8_t>(c.rng->U32() % 4u);
            // The rider needs its own lateral field; nothing else of it is read.
            const uint32_t riderLat = static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u));
            rider[rr::sim::ent::kLatAccel + 0] = static_cast<uint8_t>(riderLat);
            rider[rr::sim::ent::kLatAccel + 1] = static_cast<uint8_t>(riderLat >> 8);
            rider[rr::sim::ent::kLatAccel + 2] = static_cast<uint8_t>(riderLat >> 16);
            rider[rr::sim::ent::kLatAccel + 3] = static_cast<uint8_t>(riderLat >> 24);
            c.mem->WriteBlock(c.scratch, bike.data(), bike.size());
            c.mem->WriteBlock(riderAddr, rider.data(), rider.size());
            args.a[0] = c.scratch;
            args.a[1] = (c.rng->Next() & 1u) ? 1u : 0u;                       // hasRider
            args.a[2] = (c.rng->Next() % 3u == 0)
                            ? 0u
                            : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)); // extra
            args.a[3] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 16u));
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            auto load = [&m](uint32_t addr, size_t n) {
                std::vector<uint8_t> v(n);
                for (size_t i = 0; i < n; ++i) v[i] = m.PeekByte(addr + static_cast<uint32_t>(i));
                return v;
            };
            std::vector<uint8_t> bike = load(scratch, 1096);
            rr::sim::EntityView bikeView(bike.data());
            const uint32_t riderAddr = bikeView.U32(rr::sim::ent::kRider);
            std::vector<uint8_t> rider = load(riderAddr, 1096);
            rr::sim::EntityView riderView(rider.data());
            std::vector<uint8_t> stats = load(bikeView.U32(rr::sim::ent::kStats), 512);
            const uint8_t riderDefByte0 = m.PeekByte(bikeView.U32(rr::sim::ent::kRiderDef));
            std::vector<int16_t> sincos(8192);
            for (size_t i = 0; i < sincos.size(); ++i)
                sincos[i] = GetS16(m, kSinCosTable + 2u * static_cast<uint32_t>(i));
            int32_t atan[20];
            for (int i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));
            rr::sim::BikeApplySteering(bikeView, static_cast<int32_t>(args.a[1]), &riderView,
                                       static_cast<int32_t>(args.a[2]), static_cast<int32_t>(args.a[3]),
                                       stats.data(), atan, sincos.data(), riderDefByte0);
            m.WriteBlock(scratch, bike.data(), bike.size());
            m.WriteBlock(riderAddr, rider.data(), rider.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "bike_aim_target", "void BikeAimTarget(Bike*)  [RASHCDG, the AI's lateral target]",
        kAimTarget,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> bike = src.entity;
            if (bike.size() < 1096) bike.resize(1096, 0);
            std::vector<uint8_t> rider(1096, 0);
            auto put32 = [&bike](uint32_t off, uint32_t v) {
                bike[off] = static_cast<uint8_t>(v);
                bike[off + 1] = static_cast<uint8_t>(v >> 8);
                bike[off + 2] = static_cast<uint8_t>(v >> 16);
                bike[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&bike](uint32_t off, uint16_t v) {
                bike[off] = static_cast<uint8_t>(v);
                bike[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            const uint32_t riderAddr = c.scratch + 1152;
            const uint32_t stats = c.scratch + 2304;
            for (uint32_t i = 0; i < 512; i += 4) c.mem->PokeWord(stats + i, 0);
            const bool tame = (c.rng->Next() & 1u) != 0;
            for (uint32_t off : {224u, 232u, 288u, 292u, 356u, 360u, 376u, 380u, 384u, 388u, 392u, 396u})
                PutS32(*c.mem, stats + off, tame ? (c.rng->S32() >> 10) : c.rng->S32());
            put32(rr::sim::ent::kStats, stats);
            put32(rr::sim::ent::kRider, (c.rng->Next() % 5u == 0) ? 0u : riderAddr);
            // The handle indexes the runtime table at 0x800CE540; keep it small so the guest stays
            // inside the 512 bytes the bench mirrors.
            put16(rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() % 60u));
            // Bit 27 picks the geometric aim, bit 20 the speed-band one; both are driven, and so
            // are bits 8, 9, 18, 22.
            uint32_t flagsA = c.rng->U32();
            const uint64_t roll = c.rng->Next() % 4u;
            if (roll == 0) flagsA |= 0x08000000u;
            else if (roll == 1) flagsA = (flagsA & ~0x08000000u) | 0x00100000u;
            else if (roll == 2) flagsA &= ~0x08100000u;
            put32(rr::sim::ent::kFlagsA, flagsA);
            put32(rr::sim::ent::kFlagsB, c.rng->U32());
            put32(rr::sim::ent::kFlagsC, c.rng->U32());
            put32(rr::sim::ent::kSpeed,
                  c.sample ? static_cast<uint32_t>(c.sample->speed) : (c.rng->U32() >> 4));
            put32(rr::sim::ent::kSpeedCopy,
                  c.sample ? static_cast<uint32_t>(c.sample->speed) : (c.rng->U32() >> 4));
            for (int i = 0; i < 3; ++i) {
                put32(rr::sim::ent::kAimFrom + 4u * static_cast<uint32_t>(i),
                      static_cast<uint32_t>(c.sample ? c.sample->p[i] : c.rng->World()));
                // Half the cases make the two points nearly equal, which is what drives the
                // "squared length too small" and "too large" guards at 0x800731A0/0x800731B4.
                put32(rr::sim::ent::kAimTo + 4u * static_cast<uint32_t>(i),
                      static_cast<uint32_t>((c.rng->Next() & 1u)
                                                ? (c.sample ? c.sample->p[i] : c.rng->World())
                                                : c.rng->World()));
                put16(rr::sim::ent::kAimAxis + 2u * static_cast<uint32_t>(i),
                      static_cast<uint16_t>(c.rng->Unit()));
                put16(rr::sim::ent::kHeading + 2u * static_cast<uint32_t>(i),
                      static_cast<uint16_t>(c.rng->Unit()));
            }
            for (uint32_t off : {rr::sim::ent::kSteer, rr::sim::ent::kSteerRate,
                                 rr::sim::ent::kSteerScratch, rr::sim::ent::kLatAccel})
                put32(off, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            const uint32_t riderLat = static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u));
            for (int i = 0; i < 4; ++i)
                rider[rr::sim::ent::kLatAccel + static_cast<uint32_t>(i)] =
                    static_cast<uint8_t>(riderLat >> (8 * i));
            c.mem->WriteBlock(c.scratch, bike.data(), bike.size());
            c.mem->WriteBlock(riderAddr, rider.data(), rider.size());
            args.a[0] = c.scratch;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            auto load = [&m](uint32_t addr, size_t n) {
                std::vector<uint8_t> v(n);
                for (size_t i = 0; i < n; ++i) v[i] = m.PeekByte(addr + static_cast<uint32_t>(i));
                return v;
            };
            std::vector<uint8_t> bike = load(scratch, 1096);
            rr::sim::EntityView bikeView(bike.data());
            const uint32_t riderAddr = bikeView.U32(rr::sim::ent::kRider);
            std::vector<uint8_t> rider;
            if (riderAddr != 0) rider = load(riderAddr, 1096);
            rr::sim::EntityView riderView(rider.empty() ? nullptr : rider.data());
            std::vector<uint8_t> stats = load(bikeView.U32(rr::sim::ent::kStats), 512);
            std::vector<uint8_t> handleTable(1024);
            for (size_t i = 0; i < handleTable.size(); ++i)
                handleTable[i] = m.PeekByte(kHandleTable + static_cast<uint32_t>(i));
            int32_t atan[20];
            for (int i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));
            rr::sim::BikeAimTarget(bikeView, rider.empty() ? nullptr : &riderView, stats.data(), atan,
                                   handleTable.data());
            m.WriteBlock(scratch, bike.data(), bike.size());
            if (!rider.empty()) m.WriteBlock(riderAddr, rider.data(), rider.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "bike_steer_driver", "void BikeSteerDriver(list, s32 dt)  [RASHCDG, the per-frame driver]",
        kSteerDriver,
        [](CaseContext& c, Args& args) {
            // Three bikes on a real circular list: the head sentinel at scratch+0, the entities at
            // +64/+1280/+2496 with their list nodes at entity+0x440, their riders at +4096 and a
            // shared stat block and owner object above them.
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t head = c.scratch;
            const uint32_t stats = c.scratch + 6656;
            const uint32_t owner = c.scratch + 7168;
            for (uint32_t i = 0; i < 512; i += 4) c.mem->PokeWord(stats + i, 0);
            for (uint32_t i = 0; i < 640; i += 4) c.mem->PokeWord(owner + i, 0);
            const bool tame = (c.rng->Next() & 1u) != 0;
            for (uint32_t off : {224u, 232u, 288u, 292u, 356u, 360u, 376u, 380u, 384u, 388u, 392u,
                                 396u, 4u, 228u, 240u, 280u, 284u, 308u, 312u, 316u, 364u, 368u,
                                 372u, 416u})
                PutS32(*c.mem, stats + off, tame ? (c.rng->S32() >> 10) : c.rng->S32());
            PutS16(*c.mem, stats + 446, (c.rng->Next() % 3u == 0) ? c.rng->S16() : static_cast<int16_t>(0));
            {
                const uint8_t ob = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(owner + 0x23C, &ob, 1);
            }
            PutS32(*c.mem, owner + 0x25C, static_cast<int32_t>(c.rng->U32() % 4u));
            // game_state's two player counts, written identically on both machines.
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x30, c.rng->U32() % 4u);
            c.mem->PokeWord(gs + 0x34, c.rng->U32() % 4u);

            for (uint32_t i = 0; i < 3; ++i) {
                const uint32_t ea = c.scratch + 16u + 1104u * i;
                const uint32_t ra = c.scratch + 3328u + 1104u * i;
                std::vector<uint8_t> e = src.entity;
                if (e.size() < 1096) e.resize(1096, 0);
                std::vector<uint8_t> rider(1096, 0);
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                auto put16 = [&e](uint32_t off, uint16_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                };
                put32(rr::sim::ent::kStats, stats);
                put32(rr::sim::ent::kOwner, owner);
                put32(rr::sim::ent::kRider, (c.rng->Next() % 6u == 0) ? 0u : ra);
                put16(rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() % 6u));
                // Bit 27 + bit 13 select crash recovery, bit 14 its first frame; the rest of the
                // gates are bits 8/9/19/20 of flagsA, 0x40 / 0x18000000 of flagsB and 0x1FF /
                // 0x600 of flagsC.
                uint32_t flagsA = c.rng->U32();
                const uint64_t roll = c.rng->Next() % 4u;
                if (roll == 0) flagsA |= 0x08002000u;
                else if (roll == 1) flagsA = (flagsA & ~0x08000000u);
                put32(rr::sim::ent::kFlagsA, flagsA);
                put32(rr::sim::ent::kFlagsB, c.rng->U32());
                put32(rr::sim::ent::kFlagsC, (c.rng->Next() & 1u) ? (c.rng->U32() & ~0x1FFu)
                                                                  : c.rng->U32());
                put32(0x2D0, (c.rng->Next() % 4u == 0) ? c.rng->U32() : 0u);
                put32(0x2D4, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
                put32(rr::sim::ent::kSpeed,
                      (c.rng->Next() & 1u) ? 0u : static_cast<uint32_t>(c.rng->S32() >> 4));
                put32(rr::sim::ent::kSpeedCopy,
                      (c.rng->Next() & 1u) ? 0u : static_cast<uint32_t>(c.rng->S32() >> 4));
                for (int k = 0; k < 9; ++k) {
                    put16(0x204u + 2u * static_cast<uint32_t>(k), static_cast<uint16_t>(c.rng->Unit()));
                    put16(rr::sim::ent::kAxes + 2u * static_cast<uint32_t>(k),
                          static_cast<uint16_t>(c.rng->Unit()));
                }
                for (int k = 0; k < 3; ++k) {
                    put16(rr::sim::ent::kImpact + 2u * static_cast<uint32_t>(k),
                          static_cast<uint16_t>(c.rng->Unit()));
                    put16(rr::sim::ent::kHeading + 2u * static_cast<uint32_t>(k),
                          static_cast<uint16_t>(c.rng->Unit()));
                    put16(rr::sim::ent::kAimAxis + 2u * static_cast<uint32_t>(k),
                          static_cast<uint16_t>(c.rng->Unit()));
                    put32(rr::sim::ent::kAimFrom + 4u * static_cast<uint32_t>(k),
                          static_cast<uint32_t>(c.rng->World()));
                    put32(rr::sim::ent::kAimTo + 4u * static_cast<uint32_t>(k),
                          static_cast<uint32_t>(c.rng->World()));
                }
                for (uint32_t off : {rr::sim::ent::kSteer, rr::sim::ent::kSteerRate,
                                     rr::sim::ent::kSteerScratch, rr::sim::ent::kLatAccel,
                                     rr::sim::ent::kSteerBias, rr::sim::ent::kLeanBias,
                                     rr::sim::ent::kLeanGain, rr::sim::ent::kIdleLean,
                                     rr::sim::ent::kIdleB, 0x2A0u, 0x28Cu})
                    put32(off, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
                put16(rr::sim::ent::kLean, static_cast<uint16_t>(c.rng->U32()));
                e[rr::sim::ent::kMode] = static_cast<uint8_t>(c.rng->U32() % 4u);
                // The list node: entity + 0x440, `next` at +4.
                const uint32_t nextNode = (i == 2) ? head : (c.scratch + 16u + 1104u * (i + 1) + 0x440u);
                put32(0x440u, 0);
                put32(0x444u, nextNode);
                const uint32_t riderLat = static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u));
                const uint32_t riderFlags = c.rng->U32();
                for (int k = 0; k < 4; ++k) {
                    rider[rr::sim::ent::kLatAccel + static_cast<uint32_t>(k)] =
                        static_cast<uint8_t>(riderLat >> (8 * k));
                    rider[rr::sim::ent::kFlagsA + static_cast<uint32_t>(k)] =
                        static_cast<uint8_t>(riderFlags >> (8 * k));
                }
                c.mem->WriteBlock(ea, e.data(), e.size());
                c.mem->WriteBlock(ra, rider.data(), rider.size());
            }
            c.mem->PokeWord(head + 0, 0);
            c.mem->PokeWord(head + 4, c.scratch + 16u + 0x440u);
            args.a[0] = head;
            args.a[1] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 16u));
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            auto load = [&m](uint32_t addr, size_t n) {
                std::vector<uint8_t> v(n);
                for (size_t i = 0; i < n; ++i) v[i] = m.PeekByte(addr + static_cast<uint32_t>(i));
                return v;
            };
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            const int32_t numPlayers = static_cast<int32_t>(m.PeekWord(gs + 0x30));
            const int32_t gs34 = static_cast<int32_t>(m.PeekWord(gs + 0x34));
            std::vector<uint8_t> aiTable = load(kAiTable, 4096);
            std::vector<uint8_t> handleTable = load(kHandleTable, 1024);
            std::vector<int16_t> sincos(8192);
            for (size_t i = 0; i < sincos.size(); ++i)
                sincos[i] = GetS16(m, kSinCosTable + 2u * static_cast<uint32_t>(i));
            int32_t atan[20];
            for (int i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));

            // Walk the guest list exactly as the original does, resolving each entity's four
            // pointer chases; the driver then works purely on the resolved nodes.
            std::vector<std::vector<uint8_t>> bikes, riders, statsBlocks;
            std::vector<uint32_t> bikeAddr, riderAddr;
            uint32_t node = m.PeekWord(args.a[0] + 4);
            while (node != args.a[0] && bikes.size() < 32) {
                const uint32_t ea = node - 1088u;
                bikes.push_back(load(ea, 1096));
                bikeAddr.push_back(ea);
                rr::sim::EntityView v(bikes.back().data());
                const uint32_t ra = v.U32(rr::sim::ent::kRider);
                riderAddr.push_back(ra);
                riders.push_back(ra ? load(ra, 1096) : std::vector<uint8_t>());
                statsBlocks.push_back(load(v.U32(rr::sim::ent::kStats), 512));
                node = m.PeekWord(node + 4);
            }
            std::vector<rr::sim::EntityView> riderViews;
            riderViews.reserve(riders.size());
            for (auto& r : riders) riderViews.emplace_back(r.empty() ? nullptr : r.data());
            std::vector<rr::sim::BikeSteerNode> nodes;
            nodes.reserve(bikes.size());
            for (size_t i = 0; i < bikes.size(); ++i) {
                rr::sim::EntityView v(bikes[i].data());
                const uint32_t ownerAddr = v.U32(rr::sim::ent::kOwner);
                nodes.push_back(rr::sim::BikeSteerNode{
                    v, riders[i].empty() ? nullptr : &riderViews[i], statsBlocks[i].data(),
                    handleTable.data(), m.PeekByte(ownerAddr + 0x23Cu),
                    static_cast<int32_t>(m.PeekWord(ownerAddr + 0x25Cu))});
            }
            rr::sim::BikeSteerDriver(nodes.data(), nodes.size(), static_cast<int32_t>(args.a[1]),
                                     numPlayers, gs34, aiTable.data(), atan, sincos.data());
            for (size_t i = 0; i < bikes.size(); ++i) {
                m.WriteBlock(bikeAddr[i], bikes[i].data(), bikes[i].size());
                if (!riders[i].empty()) m.WriteBlock(riderAddr[i], riders[i].data(), riders[i].size());
            }
            (void)scratch;
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "axis_curve", "s32 AxisCurve(u8 raw, const u16 *curve)  [the analogue throttle/steer curve]",
        kAxisCurve,
        [](CaseContext& c, Args& args) {
            // Both curve tables the game uses are six halfwords long as far as this code can read;
            // the dead zone and the segment width are written into the game's own parameter block
            // so that all four arms (dead zone, inside a segment, past the last segment,
            // saturated) are driven.
            const uint32_t curve = c.scratch;
            for (uint32_t i = 0; i < 8; ++i)
                PutS16(*c.mem, curve + 2u * i, static_cast<int16_t>(c.rng->U32() % 0x10001u));
            const uint16_t dead = static_cast<uint16_t>(c.rng->U32() % 40u);
            const uint16_t width = (c.rng->Next() % 8u == 0)
                                       ? 0u
                                       : static_cast<uint16_t>(1u + c.rng->U32() % 60u);
            WriteU16Bench(*c.mem, kAxisCfg + 176, dead);
            WriteU16Bench(*c.mem, kAxisCfg + 178, width);
            args.a[0] = c.rng->U32() % 256u;
            args.a[1] = curve;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            uint16_t curve[8];
            for (int i = 0; i < 8; ++i)
                curve[i] = static_cast<uint16_t>(GetS16(m, scratch + 2u * static_cast<uint32_t>(i)));
            std::vector<uint8_t> cfg(192);
            for (size_t i = 0; i < cfg.size(); ++i) cfg[i] = m.PeekByte(kAxisCfg + static_cast<uint32_t>(i));
            return static_cast<uint32_t>(
                rr::sim::AxisCurve(static_cast<uint8_t>(args.a[0]), curve, cfg.data()));
        }});

    // ------------------------------------------------------------------ the engine, 0x80079B20
    // The first row that uses the oracle-supplied-callee seam: everything is ours except
    // `SLUS 0x80017BA0`, which the interpreter executes on the candidate's own clone.
    rows.push_back(Row{
        "bike_engine", "void BikeEngineStep(list, s32 dt)  [RASHCDG, engine + drivetrain]",
        kEngineStep,
        [](CaseContext& c, Args& args) {
            // Two bikes on a real circular list, the shared stat block well clear of both.
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t head = c.scratch;
            const uint32_t owner = c.scratch + 2240;
            const uint32_t stats = c.scratch + 3072;
            for (uint32_t i = 0; i < 656; i += 4) c.mem->PokeWord(owner + i, 0);
            PutS32(*c.mem, owner + 0x25C, static_cast<int32_t>(c.rng->U32() % 4u));

            // The stat block. Its layout is the `.PH`'s: the gear ratios at
            // +0x14 + 4*gear, the torque curve at +0x3C, the rev floor/ceiling/step at
            // +0xB0/+0xB4/+0xB8, a drive scale at +0x150 and the gear count at +0x1BC.
            for (uint32_t i = 0; i < 1024; i += 4)
                c.mem->PokeWord(stats + i, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 10u)));
            for (uint32_t g = 0; g < 20; ++g) {
                int32_t r = static_cast<int32_t>(0x1000u + c.rng->U32() % 0x400000u);
                if (c.rng->Next() % 8u == 0) r = -r;
                PutS32(*c.mem, stats + 12u + 4u * g, r);
            }
            // The rev range and the curve step. The step is kept at or above 4.0 and the range
            // under 96.0 because the original's curve index, `(revs - floor) / step`, is bounded by
            // NOTHING: a degenerate stat block makes the console index hundreds of kilobytes past
            // the block. That is a property of the original worth recording, and it is why the
            // native side is handed a 128 KiB window rather than the block alone.
            const int32_t minRev = (c.rng->S32() >> 12);
            const int32_t span = static_cast<int32_t>(c.rng->U32() % 0x600001u);
            PutS32(*c.mem, stats + 176, minRev);
            PutS32(*c.mem, stats + 180, static_cast<int32_t>(static_cast<uint32_t>(minRev) +
                                                             static_cast<uint32_t>(span)));
            PutS32(*c.mem, stats + 184,
                   (c.rng->Next() % 16u == 0)
                       ? 0
                       : static_cast<int32_t>(0x40000u + c.rng->U32() % 0x3C0000u));
            PutS32(*c.mem, stats + 336, static_cast<int32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            {
                const uint8_t gearCount = static_cast<uint8_t>(c.rng->U32() % 17u);
                c.mem->WriteBlock(stats + 444, &gearCount, 1);
            }

            for (uint32_t i = 0; i < 2; ++i) {
                const uint32_t ea = c.scratch + 16u + 1104u * i;
                std::vector<uint8_t> e = src.entity;
                if (e.size() < 1096) e.resize(1096, 0);
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                put32(rr::sim::ent::kStats, stats);
                put32(rr::sim::ent::kOwner, (c.rng->Next() % 8u == 0) ? 0u : owner);
                e[rr::sim::ent::kGear] = static_cast<uint8_t>(
                    static_cast<int8_t>(static_cast<int32_t>(c.rng->U32() % 15u) - 2));
                e[rr::sim::ent::kMode] = static_cast<uint8_t>(c.rng->U32() % 4u);
                // The arms are selected by flagsA bits 1/6/23/27, flagsB bits 0/5/6/9/21/28 and
                // flagsC 0x7FF/0x600/0x800/0xC/0x20000, so every one of those is driven on purpose
                // as well as at random.
                uint32_t flagsA = c.rng->U32();
                uint32_t flagsB = c.rng->U32();
                uint32_t flagsC = c.rng->U32();
                switch (c.rng->Next() % 6u) {
                case 0: flagsA = (flagsA & ~0x08000042u) | 0x42u; flagsC &= ~0x7FFu;
                        flagsB &= ~0x40000u; break;               // the standing-still arm
                case 1: flagsA |= 0x00800002u; break;             // the rev limiter
                case 2: flagsA = (flagsA & ~0x00800000u) | 2u; flagsB |= 1u; break;
                case 3: flagsA &= ~0x00800002u; break;
                case 4: flagsB = (flagsB & ~0x00200000u) | 0x40u; flagsC &= ~0x600u; break;
                case 5: flagsB = (flagsB & ~0x00200060u) | 0x20u; flagsC &= ~0x600u; break;
                }
                put32(rr::sim::ent::kFlagsA, flagsA);
                put32(rr::sim::ent::kFlagsB, flagsB);
                put32(rr::sim::ent::kFlagsC, flagsC);
                // Bounded on purpose - see the note on the stat block above.
                put32(rr::sim::ent::kRevs, static_cast<uint32_t>(c.rng->S32() >> 8));
                put32(rr::sim::ent::kSpeedCopy,
                      (c.rng->Next() % 6u == 0) ? static_cast<uint32_t>(c.rng->U32() % 13108u)
                                                : static_cast<uint32_t>(c.rng->S32() >> 9));
                put32(rr::sim::ent::kSpeed, static_cast<uint32_t>(c.rng->S32() >> 9));
                for (uint32_t off : {rr::sim::ent::kThrottle, rr::sim::ent::kThrottleVel,
                                     rr::sim::ent::kThrottleAccel, rr::sim::ent::kThrottleFrom,
                                     rr::sim::ent::kThrottleTime, rr::sim::ent::kWheelieAngle,
                                     rr::sim::ent::kWheelieVel, rr::sim::ent::kWheelieRate,
                                     rr::sim::ent::kWheelieTo, rr::sim::ent::kWheelieTime,
                                     rr::sim::ent::kWheelieBias, rr::sim::ent::kEngBrake,
                                     rr::sim::ent::kEngHold, rr::sim::ent::kEngWish,
                                     rr::sim::ent::kIdleTimer, rr::sim::ent::kHalfY}) {
                    put32(off, (c.rng->Next() % 5u == 0)
                                   ? 0u
                                   : static_cast<uint32_t>(c.rng->S32() >> (8 + c.rng->Next() % 10u)));
                }
                for (uint32_t k = 0; k < 3; ++k)
                    put32(rr::sim::ent::kObbCentre + 4u * k, static_cast<uint32_t>(c.rng->World()));
                e[rr::sim::ent::kLeanSrc] = static_cast<uint8_t>(c.rng->U32());
                e[rr::sim::ent::kLeanSrc + 1] = static_cast<uint8_t>(c.rng->U32());
                e[rr::sim::ent::kLeanOut] = static_cast<uint8_t>(c.rng->U32());
                e[rr::sim::ent::kLeanOut + 1] = static_cast<uint8_t>(c.rng->U32());
                // The list node: entity + 0x440, `next` at +4.
                put32(0x440u, 0);
                put32(0x444u, (i == 1) ? head : (c.scratch + 16u + 1104u * (i + 1) + 0x440u));
                c.mem->WriteBlock(ea, e.data(), e.size());
            }
            c.mem->PokeWord(head + 0, 0);
            c.mem->PokeWord(head + 4, c.scratch + 16u + 0x440u);
            args.a[0] = head;
            // dt is kept inside +-1.0 so that the rev ramp (up to 20000/s) cannot wrap the 16.16
            // multiply; the dump-derived half uses the real frame delta of the traced race frame.
            args.a[1] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> 15);
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) {
            Memory& m = env.clone->mem;
            // The stat block is game data reached through a guest pointer, and the original indexes
            // it with an unbounded index, so the port is handed a 128 KiB window around it rather
            // than the nominal block. The window wraps at 2 MiB, exactly as the R3000's own address
            // decoding does.
            auto window = [&m](uint32_t address) {
                const std::vector<uint8_t>& ram = m.ram();
                std::vector<uint8_t> w(kStatsWindowBack + kStatsWindowFwd);
                const uint32_t start = (Memory::RamOffset(address) - kStatsWindowBack) & 0x1FFFFFu;
                for (uint32_t i = 0; i < kStatsWindowBack + kStatsWindowFwd; ++i)
                    w[i] = ram[(start + i) & 0x1FFFFFu];
                return w;
            };
            uint16_t atan[64];
            for (uint32_t i = 0; i < 64; ++i)
                atan[i] = static_cast<uint16_t>(GetS16(m, kAtanU16Table + 2u * i));

            // Walk the guest list exactly as the original does; the port gets the entities in its
            // order with the two pointer chases resolved.
            std::vector<std::vector<uint8_t>> bikes, statsWin;
            std::vector<uint32_t> bikeAddr;
            std::vector<int32_t> ownerIdleRev;
            uint32_t node = m.PeekWord(args.a[0] + 4);
            while (node != args.a[0] && bikes.size() < 32) {
                const uint32_t ea = node - 1088u;
                std::vector<uint8_t> e(1096);
                m.ReadBlock(ea, e.data(), e.size());
                rr::sim::EntityView v(e.data());
                statsWin.push_back(window(v.U32(rr::sim::ent::kStats)));
                ownerIdleRev.push_back(
                    static_cast<int32_t>(m.PeekWord(v.U32(rr::sim::ent::kOwner) + 0x25Cu)));
                bikes.push_back(std::move(e));
                bikeAddr.push_back(ea);
                node = m.PeekWord(node + 4);
            }

            // NO SEAM. Skipping the emitter `SLUS 0x80017BA0` changes compared state, but not the
            // game's shared LCG: the word the emitter perturbs is the SOUND-HANDLE SERIAL COUNTER
            // at gp+2068 = 0x8005B4A0, eight bytes below the seed at gp+2076, and it steps by
            // exactly one. The emitter is ported and benched in its own right (`play_sound_3d`),
            // so the engine calls OUR PlaySound3D here, against this clone's own memory, and the
            // whole row is native code.
            //
            // Nothing is written back around the call: the emitter's tree touches the
            // sound-system record at 0x800D6870, the voice table, the listener array and that one
            // serial word, and none of them aliases a bike image or the LCG seed.
            uint32_t seed = m.PeekWord(env.gp + kRandSeedGpOffset);
            struct NativeSound final : rr::sim::EngineSound {
                Memory& mem;
                uint32_t gp;
                bool outOfWindow = false;
                NativeSound(Memory& mm, uint32_t g) : mem(mm), gp(g) {}
                void PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
                    SoundMirror mirror(mem, gp);
                    mirror.LoadAtanTable();
                    rr::sim::PlaySound3DEnv e;
                    e.mutedBank = GetS32(mem, gp + kGpMutedBank);
                    e.defaultBank = GetS32(mem, gp + kGpDefaultBank);
                    e.master3d = GetS32(mem, kSoundMaster);
                    e.listener = mirror.Listener();
                    e.system = mirror.System();
                    rr::sim::PlaySound3D(x, z, id, bank, e);
                    mirror.Commit();
                    if (mirror.outOfWindow) outOfWindow = true;
                }
            } sound(m, env.gp);

            std::vector<rr::sim::BikeEngineNode> nodes;
            nodes.reserve(bikes.size());
            for (size_t i = 0; i < bikes.size(); ++i) {
                nodes.push_back(rr::sim::BikeEngineNode{rr::sim::EntityView(bikes[i].data()),
                                                        statsWin[i].data() + kStatsWindowBack,
                                                        ownerIdleRev[i]});
            }
            rr::sim::BikeEngineStep(nodes.data(), nodes.size(),
                                    static_cast<int32_t>(args.a[1]), seed, atan, sound);
            m.PokeWord(env.gp + kRandSeedGpOffset, seed);
            for (size_t i = 0; i < bikes.size(); ++i)
                m.WriteBlock(bikeAddr[i], bikes[i].data(), bikes[i].size());
            if (sound.outOfWindow && env.failure.empty())
                env.failure = "the emitter's bank offset left the window the bench handed over";
            return 0u;
        }});

    rows.push_back(Row{
        "crash_table_hit", "s32 CrashTableHit(Bike*, s32 which)  [SLUS, the per-class crash table]",
        kCrashLut,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            const uint32_t cls = c.rng->U32() % 256u; // bounded: the table index is 6 * class
            for (int i = 0; i < 4; ++i)
                e[rr::sim::ent::kClass + static_cast<uint32_t>(i)] =
                    static_cast<uint8_t>(cls >> (8 * i));
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            args.a[0] = c.scratch;
            args.a[1] = c.rng->U32() % 2u;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            std::vector<uint8_t> e(1096);
            m.ReadBlock(scratch, e.data(), e.size());
            std::vector<uint8_t> table(4096);
            m.ReadBlock(kCrashTable, table.data(), table.size());
            return static_cast<uint32_t>(rr::sim::CrashTableHit(
                rr::sim::EntityView(e.data()), static_cast<int32_t>(args.a[1]), table.data()));
        }});

    // The second row that uses the oracle-supplied-callee seam, and the one that shows it is not a
    // one-off: here only ONE of the two callees is oracle-supplied, the other is ported.
    rows.push_back(Row{
        "bike_crash_timer", "void BikeCrashTimer(Bike*, s32 dt)  [RASHCDG, wipeout + its effects]",
        kCrashTimer,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            // Both arms of every gate: flagsA bits 1/2/3/6/11, flagsB bit 9, and a crash count
            // that is positive often enough to reach the emitter.
            uint32_t flagsA = c.rng->U32();
            if (c.rng->Next() % 3u == 0) flagsA |= 0x0000000Eu;
            put32(rr::sim::ent::kFlagsA, flagsA);
            put32(rr::sim::ent::kFlagsB, (c.rng->Next() & 1u) ? (c.rng->U32() | 0x200u)
                                                              : c.rng->U32());
            put32(rr::sim::ent::kCrashTimer,
                  static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            put32(rr::sim::ent::kClass, c.rng->U32() % 256u); // bounded, as in crash_table_hit
            e[rr::sim::ent::kCrashCount] =
                static_cast<uint8_t>(static_cast<int8_t>(static_cast<int32_t>(c.rng->U32() % 9u) - 2));
            c.mem->PokeWord(kCrashWindow, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            args.a[0] = c.scratch;
            args.a[1] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> 12);
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) {
            Memory& m = env.clone->mem;
            const uint32_t ea = args.a[0];
            std::vector<uint8_t> e(1096);
            m.ReadBlock(ea, e.data(), e.size());
            std::vector<uint8_t> table(4096);
            m.ReadBlock(kCrashTable, table.data(), table.size());

            struct OracleEmitter final : rr::sim::CrashEmitter {
                NativeEnv& env;
                std::vector<uint8_t>& e;
                uint32_t ea;
                OracleEmitter(NativeEnv& v, std::vector<uint8_t>& b, uint32_t a)
                    : env(v), e(b), ea(a) {}
                void Emit(int32_t which, int32_t kind) override {
                    env.clone->mem.WriteBlock(ea, e.data(), e.size());
                    // PORTED (spine.h CrashEmit), run natively and witnessed (rows_spine.inc)
                    const uint32_t ca[3] = {ea, static_cast<uint32_t>(which), static_cast<uint32_t>(kind)};
                    SpineNativeCall(env, kCrashEmit, ca, 3);
                    env.clone->mem.ReadBlock(ea, e.data(), e.size());
                }
            } emitter(env, e, ea);

            rr::sim::BikeCrashTimer(rr::sim::EntityView(e.data()),
                                    static_cast<int32_t>(args.a[1]),
                                    static_cast<int32_t>(m.PeekWord(kCrashWindow)), table.data(),
                                    emitter);
            m.WriteBlock(ea, e.data(), e.size());
            return 0u;
        },
        /*oracleCallees=*/{OracleCallee{kCrashEmit, 3}}});

    // ================================================================= the opponent AI's drive
    // The thirteen functions that make a field of opponents drive the route at a rubber-banded
    // speed. The leaves first.

    rows.push_back(Row{
        "ai_project", "s32 AiProject(const s32 p[3], const s16 axis[3], const s32 org[3])",
        kAiProject,
        [](CaseContext& c, Args& args) {
            int32_t p[3], org[3];
            int16_t axis[3];
            if (c.sample) {
                for (int i = 0; i < 3; ++i) {
                    p[i] = c.sample->p[i];
                    org[i] = c.sample->q[i];
                }
                const size_t row = c.rng->Next() % 3u;
                for (int i = 0; i < 3; ++i) axis[i] = c.sample->mA[row * 3 + static_cast<size_t>(i)];
            } else {
                for (int i = 0; i < 3; ++i) {
                    p[i] = (c.rng->Next() & 1u) ? c.rng->World() : c.rng->S32();
                    org[i] = (c.rng->Next() & 1u) ? c.rng->World() : c.rng->S32();
                    axis[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
                }
            }
            PutVec3S32(*c.mem, c.scratch + 0, p);
            PutVec3S16(*c.mem, c.scratch + 16, axis);
            PutVec3S32(*c.mem, c.scratch + 32, org);
            args.a[0] = c.scratch + 0;
            args.a[1] = c.scratch + 16;
            args.a[2] = c.scratch + 32;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            int32_t p[3], org[3];
            int16_t axis[3];
            GetVec3S32(m, scratch + 0, p);
            GetVec3S16(m, scratch + 16, axis);
            GetVec3S32(m, scratch + 32, org);
            return static_cast<uint32_t>(rr::sim::AiProject(p, axis, org));
        }});

    rows.push_back(Row{
        "ai_handle_in_list", "s32 AiHandleInList(u32 handle, u32 list)  [packed 5-bit fields]",
        kAiHandleInList,
        [](CaseContext& c, Args& args) {
            // A real packed list: up to six 5-bit `handle + 1` fields, lowest first, terminated by
            // a zero field. Half the cases put the queried handle somewhere inside it, so both
            // answers are exercised instead of only "not found".
            const uint32_t handle =
                c.sample ? static_cast<uint32_t>(
                               c.sample->entity.size() >= 1096
                                   ? static_cast<uint32_t>(c.sample->entity[0x0AC]) |
                                         (static_cast<uint32_t>(c.sample->entity[0x0AD]) << 8)
                                   : 0u)
                         : c.rng->U32();
            const uint32_t fields = 1u + c.rng->U32() % 6u;
            const uint32_t plant = c.rng->U32() % (fields + 1u);
            uint32_t list = 0;
            for (uint32_t i = 0; i < fields; ++i) {
                uint32_t f = (i == plant) ? ((handle & 0x1Fu) + 1u) & 0x1Fu
                                          : (c.rng->U32() % 31u) + 1u;
                if (c.rng->Next() % 12u == 0) f = 0; // an early terminator
                list |= (f & 0x1Fu) << (5u * i);
            }
            if (c.rng->Next() % 16u == 0) list = 0;
            args.a[0] = (c.rng->Next() & 1u) ? handle : c.rng->U32();
            args.a[1] = list;
        },
        [](Memory&, uint32_t, uint32_t, const Args& args) {
            return static_cast<uint32_t>(rr::sim::AiHandleInList(args.a[0], args.a[1]));
        }});

    rows.push_back(Row{
        "ai_nibble_add", "void AiNibbleAdd(u8 *p, s32 delta)  [low nibble, clamped to 0..15]",
        kAiNibbleAdd,
        [](CaseContext& c, Args& args) {
            const uint8_t b = static_cast<uint8_t>(c.rng->U32());
            c.mem->WriteBlock(c.scratch, &b, 1);
            // Small deltas are the game's own range; the full 32-bit ones prove the clamp's
            // wrap-around idiom rather than only its happy path.
            args.a[0] = c.scratch;
            args.a[1] = (c.rng->Next() % 4u == 0)
                            ? c.rng->U32()
                            : static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 41u) - 20);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            uint8_t b = m.PeekByte(scratch);
            rr::sim::AiNibbleAdd(&b, static_cast<int32_t>(args.a[1]));
            m.WriteBlock(scratch, &b, 1);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "ai_nibble_decay", "void AiNibbleDecay(u8 *p, s32 step)  [low nibble toward the high one]",
        kAiNibbleDecay,
        [](CaseContext& c, Args& args) {
            const uint8_t b = static_cast<uint8_t>(c.rng->U32());
            c.mem->WriteBlock(c.scratch, &b, 1);
            args.a[0] = c.scratch;
            args.a[1] = (c.rng->Next() % 4u == 0)
                            ? c.rng->U32()
                            : static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 41u) - 20);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            uint8_t b = m.PeekByte(scratch);
            rr::sim::AiNibbleDecay(&b, static_cast<int32_t>(args.a[1]));
            m.WriteBlock(scratch, &b, 1);
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "mem_set32", "void *MemSet32(void *dst, u32 value, u32 length)  [SLUS, word-wise]",
        kMemSet32,
        [](CaseContext& c, Args& args) {
            // BOUNDED INPUT FAMILY, named: the length must be a multiple of four, because the
            // original decrements it by four and loops until it is exactly zero - anything else
            // never terminates on the console. That is a property of the original, recorded here
            // rather than papered over, and it is why this row cannot simply take a random length.
            args.a[0] = c.scratch + 64u;
            args.a[1] = (c.rng->Next() & 1u) ? c.rng->U32() : (c.rng->U32() % 256u);
            args.a[2] = 4u * (c.rng->U32() % 257u);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            std::vector<uint8_t> block(2048);
            m.ReadBlock(scratch + 64u, block.data(), block.size());
            rr::sim::MemSet32(block.data(), args.a[1], args.a[2]);
            m.WriteBlock(scratch + 64u, block.data(), block.size());
            return args.a[0];
        }});

    rows.push_back(Row{
        "ai_clear_commands", "void AiClearCommands(Entity *e)  [memset the stack, depth = 0]",
        kAiClearCmds,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e(2048, 0);
            const size_t n = std::min<size_t>(src.entity.size(), 2048);
            for (size_t i = 0; i < n; ++i) e[i] = src.entity[i];
            for (uint32_t i = 0; i < 136; ++i)
                e[0x3B4u + i] = static_cast<uint8_t>(c.rng->U32());
            // BOUNDED INPUT FAMILY, named: the depth byte is read as a SIGNED byte and multiplied
            // by 8 to make the memset length, and a negative length makes the original loop about
            // 2^30 times. The game's own invariant is 0..16 (AiPushCommand resets above that); this
            // row exercises 0..127, which is every value that terminates.
            e[0x3B2] = static_cast<uint8_t>(c.rng->U32() % 128u);
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            args.a[0] = c.scratch;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            std::vector<uint8_t> e(2048);
            m.ReadBlock(scratch, e.data(), e.size());
            rr::sim::AiClearCommands(e.data());
            m.WriteBlock(scratch, e.data(), e.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "vec_sum_squares", "s32 SumSquares(const s32 v[3])  [SLUS, the 16.16 squared length]",
        kSumSquares,
        [](CaseContext& c, Args& args) {
            int32_t v[3];
            if (c.sample) {
                for (int i = 0; i < 3; ++i) v[i] = c.sample->p[i] - c.sample->q[i];
            } else {
                for (int i = 0; i < 3; ++i)
                    v[i] = (c.rng->Next() & 1u) ? c.rng->World() : c.rng->S32();
            }
            PutVec3S32(*c.mem, c.scratch, v);
            args.a[0] = c.scratch;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            int32_t v[3];
            GetVec3S32(m, scratch, v);
            return static_cast<uint32_t>(rr::sim::SumSquares(v));
        }});

    rows.push_back(Row{
        "sqrt_gte", "s32 SqrtGte(s32 x, const s16 *table)  [SLUS, GTE LZCS/LZCR + a table]",
        kSqrtGte,
        [](CaseContext& c, Args& args) {
            const uint64_t roll = c.rng->Next() % 8u;
            if (roll == 0) args.a[0] = 0;
            else if (roll == 1) args.a[0] = 0xFFFFFFFFu;
            else if (roll == 2) args.a[0] = static_cast<uint32_t>(c.rng->U32() % 0x10000u);
            else args.a[0] = c.sample ? static_cast<uint32_t>(c.sample->speed) : c.rng->U32();
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            std::vector<int16_t> w(kSqrtWindowHalf); // 2 * kSqrtWindowHalf bytes, table in the middle
            m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, w.data(), w.size() * 2u);
            return static_cast<uint32_t>(
                rr::sim::SqrtGte(static_cast<int32_t>(args.a[0]), w.data() + kSqrtWindowHalf / 4u));
        }});

    rows.push_back(Row{
        "vec_length3", "s32 Length3(const s32 v[3])  [SLUS, SumSquares then the square root]",
        kLength3,
        [](CaseContext& c, Args& args) {
            int32_t v[3];
            if (c.sample) {
                for (int i = 0; i < 3; ++i) v[i] = c.sample->p[i] - c.sample->q[i];
            } else {
                for (int i = 0; i < 3; ++i)
                    v[i] = (c.rng->Next() & 1u) ? c.rng->World() : c.rng->S32();
                if (c.rng->Next() % 8u == 0)
                    for (int i = 0; i < 3; ++i) v[i] = 0;
            }
            PutVec3S32(*c.mem, c.scratch, v);
            args.a[0] = c.scratch;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            int32_t v[3];
            GetVec3S32(m, scratch, v);
            std::vector<int16_t> w(kSqrtWindowHalf);
            m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, w.data(), w.size() * 2u);
            return static_cast<uint32_t>(rr::sim::Length3(v, w.data() + kSqrtWindowHalf / 4u));
        }});

    rows.push_back(Row{
        "set_aim_delta",
        "void SetAimDelta(Entity*, const s32 t[3], const s16 d[3], const s32 *s, s32 mode)",
        kSetAimDelta,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&e](uint32_t off, uint16_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            for (uint32_t k = 0; k < 3; ++k) {
                put32(rr::sim::ent::kAimPoint + 4u * k, static_cast<uint32_t>(c.rng->World()));
                put32(rr::sim::ent::kAimDelta + 4u * k, static_cast<uint32_t>(c.rng->World()));
            }
            put32(rr::sim::ent::kAimRate, c.rng->U32());
            put32(rr::sim::ent::kAimBlend, c.rng->U32());
            // > 0 is the early-out arm; it must be taken on a fair share of the cases.
            put16(rr::sim::ent::kAimSuppress,
                  static_cast<uint16_t>((c.rng->Next() % 3u == 0)
                                            ? static_cast<uint16_t>(1u + c.rng->U32() % 30000u)
                                            : static_cast<uint16_t>(c.rng->S16() & ~0x7FFF)));
            c.mem->WriteBlock(c.scratch, e.data(), e.size());

            int32_t target[3];
            int16_t dir[3];
            for (int i = 0; i < 3; ++i) {
                target[i] = c.rng->World();
                dir[i] = (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit();
            }
            PutVec3S32(*c.mem, c.scratch + 1152u, target);
            PutVec3S16(*c.mem, c.scratch + 1168u, dir);
            // The scalar, including the sub-threshold and the |INT32_MIN| edges.
            int32_t scalar;
            const uint64_t roll = c.rng->Next() % 8u;
            if (roll == 0) scalar = static_cast<int32_t>(c.rng->U32() % 131u);
            else if (roll == 1) scalar = INT32_MIN;
            else if (roll == 2) scalar = -(c.rng->World());
            else scalar = c.rng->World();
            PutS32(*c.mem, c.scratch + 1184u, scalar);

            args.a[0] = c.scratch;
            // All four argument shapes the five callers use: a target point, a direction plus a
            // scalar, and the two that fall straight out.
            const uint64_t shape = c.rng->Next() % 4u;
            args.a[1] = (shape == 0) ? (c.scratch + 1152u) : 0u;
            args.a[2] = (shape == 3) ? 0u : (c.scratch + 1168u);
            args.a[3] = (shape == 2) ? 0u : (c.scratch + 1184u);
            args.a4 = c.rng->U32() % 2u;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            std::vector<uint8_t> e(1096);
            m.ReadBlock(scratch, e.data(), e.size());
            int32_t target[3], scalar;
            int16_t dir[3];
            GetVec3S32(m, args.a[1], target);
            GetVec3S16(m, args.a[2], dir);
            scalar = GetS32(m, args.a[3]);
            std::vector<int16_t> w(kSqrtWindowHalf);
            m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, w.data(), w.size() * 2u);
            rr::sim::SetAimDelta(rr::sim::EntityView(e.data()), args.a[1] ? target : nullptr,
                                 args.a[2] ? dir : nullptr, args.a[3] ? &scalar : nullptr,
                                 static_cast<int32_t>(args.a4), w.data() + kSqrtWindowHalf / 4u);
            m.WriteBlock(scratch, e.data(), e.size());
            return 0u;
        },
        /*compareV0=*/false});

    // The AI's first oracle-supplied-callee row. Everything is ours
    // except `RASHCDG 0x800C4550`, the idle-stance trigger the push arm fires when the bike is
    // under AI control and its rider's record is of category 3.
    rows.push_back(Row{
        "ai_push_command", "s32 AiPushCommand(Cmd *c, s32 mode, Entity *e)  [the AI command stack]",
        kAiPushCmd,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t ea = c.scratch;
            const uint32_t cmd = c.scratch + 1152u;
            const uint32_t rider = c.scratch + 1216u;
            const uint32_t fight = c.scratch + 2560u;

            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            for (uint32_t i = 0; i < 136; ++i)
                e[0x3B4u + i] = static_cast<uint8_t>(c.rng->U32());
            // BOUNDED INPUT FAMILY, named: the depth byte indexes the stack as a SIGNED byte and
            // the original bounds it nowhere, so a depth of -128 would address 1 KiB below the
            // entity. Depths -8..19 keep every access inside the 1096-byte entity and still cover
            // the whole shape of the function: the >= 16 reset arm, the empty stack, and the
            // search walking down to slot 1. The negative end stops at -8 for a second, sharper
            // reason: at depth -13 the push lands exactly on `+0x354`, i.e. the original overwrites
            // the rider pointer it is about to chase, and the caller here resolves that chase
            // BEFORE the call. That case would compare a port that used the
            // old pointer against an original that used the new one - a property of the bench's
            // seam, not of the port, so it is excluded by name instead of being hidden.
            {
                const int32_t d = (c.rng->Next() % 4u == 0)
                                      ? -static_cast<int32_t>(c.rng->U32() % 9u)
                                      : static_cast<int32_t>(c.rng->U32() % 20u);
                e[0x3B2] = static_cast<uint8_t>(static_cast<int8_t>(d));
            }
            // Bit 27 is the "under AI control" gate on the only arm that calls out.
            put32(rr::sim::ent::kFlagsA,
                  (c.rng->Next() % 3u == 0) ? (c.rng->U32() & ~0x08000000u)
                                            : (c.rng->U32() | 0x08000000u));
            // The rider object at `+0x354`. It is a ZEROED block in the scratch, and that is a
            // decision about the ORACLE-SUPPLIED CALLEE, not about the port: `RASHCDG 0x800C4550`
            // reaches on through `rider[+0x21C]` and then through the rider's animation state, so a
            // rider made of noise - or a live rider lifted out of a different RAM image - walks the
            // interpreter into unmapped memory. With a zeroed rider the callee takes its own
            // early-out at 0x800C3F2C and returns 0 without touching anything.
            //
            // What this row therefore proves about 0x800C4550 is exactly what the seam is for: that
            // our port calls it, once, at the right moment, with the right three arguments, and
            // that whatever it does lands inside the compared state. It proves nothing about what
            // it does with a live rider behind it.
            const uint32_t riderAddr = rider;
            put32(rr::sim::ent::kOwner, riderAddr);
            c.mem->WriteBlock(ea, e.data(), e.size());
            {
                const std::vector<uint8_t> r(640, 0);
                c.mem->WriteBlock(rider, r.data(), r.size());
            }
            // `+0x220` indexes the 8-byte records at 0x800541D4 and `+0x239` the 12-byte fight
            // records. BOUNDED INPUT FAMILY, named: the first index is a u16 the original does not
            // bound, so it is held under 256 here - a limit on the inputs, not on the comparison.
            const uint32_t kind = c.rng->U32() % 256u;
            WriteU16Bench(*c.mem, riderAddr + 0x220u, static_cast<uint16_t>(kind));
            {
                const uint8_t id = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(riderAddr + 0x239u, &id, 1);
            }
            // Half the cases force that record's category to 3 so the call really happens; the
            // write goes into both machines' RAM identically, so the diff stays clean.
            if (c.rng->Next() & 1u)
                WriteU16Bench(*c.mem, kAltKindTable + 8u * kind + 2u, 3);

            // The fight records, and the game's own pointer to them redirected into the scratch so
            // that the record this case reaches is one the bench wrote rather than whatever the
            // snapshot happened to hold.
            for (uint32_t i = 0; i < 3072; i += 4) c.mem->PokeWord(fight + i, c.rng->U32());
            c.mem->PokeWord(kFightRecPtr, fight);

            // The command record.
            WriteU16Bench(*c.mem, cmd + 0, static_cast<uint16_t>(c.rng->U32() % 20u));
            WriteU16Bench(*c.mem, cmd + 2, static_cast<uint16_t>(c.rng->U32() % 8u));
            WriteU16Bench(*c.mem, cmd + 4, static_cast<uint16_t>(c.rng->U32()));
            WriteU16Bench(*c.mem, cmd + 6, static_cast<uint16_t>(c.rng->U32()));
            // Plant the command in a slot often enough that mode 2's rotate really runs.
            {
                const int32_t d = static_cast<int8_t>(e[0x3B2]);
                if (d > 0 && (c.rng->Next() % 3u) != 0) {
                    const uint32_t k = 1u + c.rng->U32() % static_cast<uint32_t>(d);
                    WriteU16Bench(*c.mem, ea + 0x3B4u + 8u * k + 0, c.mem->PeekByte(cmd) |
                                      static_cast<uint16_t>(c.mem->PeekByte(cmd + 1) << 8));
                    WriteU16Bench(*c.mem, ea + 0x3B4u + 8u * k + 2, c.mem->PeekByte(cmd + 2) |
                                      static_cast<uint16_t>(c.mem->PeekByte(cmd + 3) << 8));
                }
            }
            // The race clock the stamp is computed from.
            c.mem->PokeWord(c.mem->PeekWord(kGameStatePtr) + 0x10u, c.rng->U32());

            args.a[0] = cmd;
            args.a[1] = c.rng->U32() % 4u; // modes 0..3; only 1 and 2 have behaviour
            args.a[2] = ea;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& args) {
            Memory& m = env.clone->mem;
            const uint32_t ea = args.a[2];
            const uint32_t cmdAddr = args.a[0];
            const uint32_t riderAddr = m.PeekWord(ea + rr::sim::ent::kOwner);

            std::vector<uint8_t> e(1096), cmd(8), rider(640);
            m.ReadBlock(ea, e.data(), e.size());
            m.ReadBlock(cmdAddr, cmd.data(), cmd.size());
            m.ReadBlock(riderAddr, rider.data(), rider.size());
            std::vector<uint8_t> altKind(2048), fight(3072);
            m.ReadBlock(kAltKindTable, altKind.data(), altKind.size());
            m.ReadBlock(m.PeekWord(kFightRecPtr), fight.data(), fight.size());

            struct OracleStance final : rr::sim::AiStanceSink {
                NativeEnv& env;
                std::vector<uint8_t>&e, &cmd, &rider;
                uint32_t ea, cmdAddr, riderAddr;
                OracleStance(NativeEnv& v, std::vector<uint8_t>& a, std::vector<uint8_t>& b,
                             std::vector<uint8_t>& c2, uint32_t x, uint32_t y, uint32_t z)
                    : env(v), e(a), cmd(b), rider(c2), ea(x), cmdAddr(y), riderAddr(z) {}
                void PlayIdleStance(uint16_t event, uint32_t riderArg) override {
                    // Commit everything the port is holding locally, run the ORIGINAL callee on the
                    // candidate's own clone with the arguments our code computed, then pick the
                    // state back up.
                    env.clone->mem.WriteBlock(ea, e.data(), e.size());
                    env.clone->mem.WriteBlock(cmdAddr, cmd.data(), cmd.size());
                    env.clone->mem.WriteBlock(riderAddr, rider.data(), rider.size());
                    AnimNativeStanceEvent(env, event, riderArg, 2, env.sp); // ported, witnessed (rows_anim.inc)
                    env.clone->mem.ReadBlock(ea, e.data(), e.size());
                    env.clone->mem.ReadBlock(cmdAddr, cmd.data(), cmd.size());
                    env.clone->mem.ReadBlock(riderAddr, rider.data(), rider.size());
                }
            } stance(env, e, cmd, rider, ea, cmdAddr, riderAddr);

            rr::sim::AiPushEnv pe;
            pe.raceClock = static_cast<int32_t>(m.PeekWord(m.PeekWord(kGameStatePtr) + 0x10u));
            pe.rider = rider.data();
            pe.riderAddress = riderAddr;
            pe.altKindTable = altKind.data();
            pe.fightRecords = fight.data();
            pe.stance = &stance;

            const int32_t r = rr::sim::AiPushCommand(cmd.data(), static_cast<int32_t>(args.a[1]),
                                                     e.data(), pe);
            m.WriteBlock(ea, e.data(), e.size());
            m.WriteBlock(cmdAddr, cmd.data(), cmd.size());
            m.WriteBlock(riderAddr, rider.data(), rider.size());
            return static_cast<uint32_t>(r);
        },
        /*oracleCallees=*/{OracleCallee{kAiStance, 3}}});

    rows.push_back(Row{
        "ai_cmd_race", "void AiCmdRace(Entity *e)  [RASHCDG, the command-4 handler: the racing line]",
        kAiCmdRace,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t ea = c.scratch;
            const uint32_t riderDef = c.scratch + 1152u;
            const uint32_t slice = c.scratch + 1280u;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&e](uint32_t off, uint16_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            put32(rr::sim::ent::kRiderDef, riderDef);
            put32(0x154, slice);
            put16(rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() % 24u));
            // `+0x188` is compared against 4 on the inner arm, so it is driven to 4 deliberately.
            put16(0x188, (c.rng->Next() % 3u == 0) ? 4u : static_cast<uint16_t>(c.rng->U32()));
            put16(rr::sim::ent::kAimLateral, static_cast<uint16_t>(c.rng->S16()));
            put32(0x158, static_cast<uint32_t>(c.rng->World()));
            put32(0x16C, static_cast<uint32_t>(c.rng->World()));
            for (uint32_t k = 0; k < 3; ++k) {
                put16(rr::sim::ent::kAimSliceAxis + 2u * k, static_cast<uint16_t>(c.rng->Unit()));
                put32(rr::sim::ent::kAimPoint + 4u * k, static_cast<uint32_t>(c.rng->World()));
                put32(rr::sim::ent::kAimDelta + 4u * k, static_cast<uint32_t>(c.rng->World()));
            }
            put32(rr::sim::ent::kAimRate, c.rng->U32());
            put32(rr::sim::ent::kAimBlend, c.rng->U32());
            put16(rr::sim::ent::kAimSuppress,
                  static_cast<uint16_t>((c.rng->Next() % 3u == 0)
                                            ? static_cast<uint16_t>(1u + c.rng->U32() % 30000u)
                                            : static_cast<uint16_t>(c.rng->S16() & ~0x7FFF)));
            c.mem->WriteBlock(ea, e.data(), e.size());

            std::vector<uint8_t> rd(72, 0);
            for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
            // The class nibble decides one of the three gates; 2 is a cop.
            rd[1] = static_cast<uint8_t>((rd[1] & 0xF0u) | (c.rng->U32() % 4u));
            c.mem->WriteBlock(riderDef, rd.data(), rd.size());
            for (uint32_t k = 0; k < 4; ++k)
                PutS16(*c.mem, slice + 2u * k, (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit());

            // The two globals the gate reads, written into both machines identically.
            c.mem->PokeWord(kAiRaceFlags, c.rng->U32());
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x10u, (c.rng->Next() & 1u) ? (c.rng->U32() % 1800u) : c.rng->U32());
            {
                const uint8_t b4 = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(gs + 4u, &b4, 1);
                const uint8_t phase = static_cast<uint8_t>(c.rng->U32() % 8u);
                c.mem->WriteBlock(gs + 0x39u, &phase, 1);
            }
            args.a[0] = ea;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            std::vector<uint8_t> e(1096), rd(72), gs(64);
            m.ReadBlock(args.a[0], e.data(), e.size());
            const uint32_t rdAddr = m.PeekWord(args.a[0] + rr::sim::ent::kRiderDef);
            m.ReadBlock(rdAddr, rd.data(), rd.size());
            m.ReadBlock(m.PeekWord(kGameStatePtr), gs.data(), gs.size());
            int16_t slice[4];
            const uint32_t sliceAddr = m.PeekWord(args.a[0] + 0x154u);
            for (int k = 0; k < 4; ++k) slice[k] = GetS16(m, sliceAddr + 2u * static_cast<uint32_t>(k));
            std::vector<int16_t> w(kSqrtWindowHalf);
            m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, w.data(), w.size() * 2u);

            rr::sim::AiCmdRaceEnv env;
            env.gameState = gs.data();
            env.riderDef = rd.data();
            env.slice = slice;
            env.flags = m.PeekWord(kAiRaceFlags);
            env.sqrtTable = w.data() + kSqrtWindowHalf / 4u;
            rr::sim::AiCmdRace(rr::sim::EntityView(e.data()), env);
            m.WriteBlock(args.a[0], e.data(), e.size());
            m.WriteBlock(rdAddr, rd.data(), rd.size());
            (void)scratch;
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "ai_target_speed",
        "s32 AiTargetSpeed(Entity *e, s32 dt)  [RASHCDG, the rubber band and the police ramp]",
        kAiTargetSpeed,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t ea       = c.scratch;
            const uint32_t riderDef = c.scratch + 1152u;
            const uint32_t stats    = c.scratch + 1280u;
            const uint32_t p1e      = c.scratch + 1792u;
            const uint32_t p1r      = c.scratch + 2896u;
            const uint32_t p2e      = c.scratch + 2976u;
            const uint32_t p2r      = c.scratch + 4080u;

            auto buildBike = [&](uint32_t addr, uint32_t rdAddr) {
                std::vector<uint8_t> b = src.entity;
                if (b.size() < 1096) b.resize(1096, 0);
                auto put32 = [&b](uint32_t off, uint32_t v) {
                    b[off] = static_cast<uint8_t>(v);
                    b[off + 1] = static_cast<uint8_t>(v >> 8);
                    b[off + 2] = static_cast<uint8_t>(v >> 16);
                    b[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                // The progress along the route: kept in a sane forward range so that the field
                // spread `3 - ((progress + 4096) >> 12) * 4 / spreadDiv` lands in the band the
                // difficulty profile is actually indexed by. A named limit on the input family.
                put32(0x144, (c.rng->Next() % 6u == 0)
                                 ? static_cast<uint32_t>(c.rng->S32() >> 12)
                                 : static_cast<uint32_t>(c.rng->U32() % 0x100000u));
                put32(rr::sim::ent::kSpeed, static_cast<uint32_t>(c.rng->S32() >> 9));
                put32(rr::sim::ent::kRiderDef, rdAddr);
                c.mem->WriteBlock(addr, b.data(), b.size());
                std::vector<uint8_t> rd(72, 0);
                for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
                // 248 and above is "jailed / escaped"; both sides of that test matter.
                rd[0x27] = static_cast<uint8_t>((c.rng->Next() % 5u == 0)
                                                    ? (248u + c.rng->U32() % 8u)
                                                    : (c.rng->U32() % 20u));
                const uint32_t fin = (c.rng->Next() % 3u == 0) ? c.rng->U32() : 0u;
                rd[0x28] = static_cast<uint8_t>(fin);
                rd[0x29] = static_cast<uint8_t>(fin >> 8);
                rd[0x2A] = static_cast<uint8_t>(fin >> 16);
                rd[0x2B] = static_cast<uint8_t>(fin >> 24);
                rd[1] = static_cast<uint8_t>((rd[1] & 0xF0u) | (c.rng->U32() % 4u));
                c.mem->WriteBlock(rdAddr, rd.data(), rd.size());
            };
            buildBike(p1e, p1r);
            buildBike(p2e, p2r);

            // The AI bike under test.
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&e](uint32_t off, uint16_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            put32(rr::sim::ent::kStats, stats);
            put32(rr::sim::ent::kRiderDef, riderDef);
            put32(0x144, (c.rng->Next() % 5u == 0)
                             ? static_cast<uint32_t>(c.rng->S32() >> 12)
                             : static_cast<uint32_t>(c.rng->U32() % 0x100000u));
            put32(rr::sim::ent::kSpeed, static_cast<uint32_t>(c.rng->S32() >> 9));
            put16(0x140, (c.rng->Next() & 1u) ? 0u : static_cast<uint16_t>(c.rng->U32()));
            put32(rr::sim::ent::kFlagsB, c.rng->U32());
            put32(rr::sim::ent::kCrashTimer,
                  static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            // Bit 4 gates the whole police arm and bit 3 the rubber-band recompute, so both are
            // driven on purpose as well as at random.
            e[rr::sim::ent::kAiState] =
                static_cast<uint8_t>(c.rng->U32() | ((c.rng->Next() % 3u == 0) ? 0x18u : 0u));
            e[rr::sim::ent::kRubberBand] = static_cast<uint8_t>(c.rng->U32());
            // BOUNDED INPUT FAMILY, named: the command-stack depth is a signed byte the original
            // does not bound, and the cop arm reads the top slot through it. -8..15 keeps that read
            // inside the 1096-byte entity; the game's own range is 0..16.
            {
                const int32_t d = (c.rng->Next() % 4u == 0)
                                      ? -static_cast<int32_t>(c.rng->U32() % 9u)
                                      : static_cast<int32_t>(c.rng->U32() % 16u);
                e[0x3B2] = static_cast<uint8_t>(static_cast<int8_t>(d));
            }
            for (uint32_t i = 0; i < 136; ++i) {
                // Opcode 4 is the one the cop arm's tail tests for, so it is planted often.
                e[0x3B4u + i] = static_cast<uint8_t>(c.rng->U32());
            }
            if (c.rng->Next() % 3u != 0) {
                const int32_t d = static_cast<int8_t>(e[0x3B2]);
                const uint32_t off = 0x3B4u + 8u * static_cast<uint32_t>(d & 0x1F);
                if (off + 4 <= 1096) {
                    e[off] = 4;
                    e[off + 1] = 0;
                }
            }
            c.mem->WriteBlock(ea, e.data(), e.size());

            std::vector<uint8_t> rd(72, 0);
            for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
            // Class 2 is a cop: a third of the cases go down the police ramp.
            rd[1] = static_cast<uint8_t>((rd[1] & 0xF0u) |
                                         ((c.rng->Next() % 3u == 0) ? 2u : (c.rng->U32() % 4u)));
            // Byte 0's bit 3 ("out of the active set") short-circuits the rubber band, so it is
            // cleared on two cases out of three to leave the interpolation itself reachable.
            if (c.rng->Next() % 3u != 0) rd[0] = static_cast<uint8_t>(rd[0] & ~0x08u);
            rd[0x27] = static_cast<uint8_t>(c.rng->U32() % 20u);
            c.mem->WriteBlock(riderDef, rd.data(), rd.size());

            std::vector<uint8_t> st(512, 0);
            for (size_t i = 0; i < st.size(); i += 4) {
                const uint32_t v = static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 10u));
                st[i] = static_cast<uint8_t>(v);
                st[i + 1] = static_cast<uint8_t>(v >> 8);
                st[i + 2] = static_cast<uint8_t>(v >> 16);
                st[i + 3] = static_cast<uint8_t>(v >> 24);
            }
            c.mem->WriteBlock(stats, st.data(), st.size());

            // The globals, written identically into both machines.
            c.mem->PokeWord(kPlayer1Ptr, p1e);
            c.mem->PokeWord(kPlayer2Ptr, p2e);
            c.mem->PokeWord(kPlayerArr + 0, p1e);
            c.mem->PokeWord(kPlayerArr + 4, p2e);
            c.mem->PokeWord(kJailbreakPtr, (c.rng->Next() % 4u == 0) ? 0u : p1e);
            c.mem->PokeWord(kLiveBikes, c.rng->U32() % 24u);
            c.mem->PokeWord(kBikeCap, c.rng->U32() % 24u);
            // BOUNDED INPUT FAMILY, named: the divisor is only kept out of a degenerate range so
            // the field-spread band lands where the 60-byte profile lives; zero IS exercised,
            // because the R3000's `div` defines it and the port reproduces that.
            c.mem->PokeWord(kSpreadDiv, (c.rng->Next() % 16u == 0) ? 0u : (1u + c.rng->U32() % 64u));
            c.mem->PokeWord(kAiRaceFlags, c.rng->U32());
            c.mem->PokeWord(kAltFlag, c.rng->U32() % 2u);
            c.mem->PokeWord(kCopScale, static_cast<uint32_t>(c.rng->S32() >> 8));
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            const uint32_t clock = (c.rng->Next() & 1u) ? (c.rng->U32() % 200000u) : c.rng->U32();
            c.mem->PokeWord(gs + 0x10u, clock);
            // Half the cases leave the cached stamp equal to the clock, which skips the field-spread
            // recompute and lets the band below drive the profile index directly.
            c.mem->PokeWord(kClockStamp, (c.rng->Next() & 1u) ? clock : c.rng->U32());
            c.mem->PokeWord(kSpreadBand, c.rng->U32() % 5u);
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 3u);
            // BOUNDED INPUT FAMILY, named: the race-type index reaches eleven different tables as
            // `t`, `t + 3` and `t + 6`, and the original bounds none of them. 0..2 is the game's own
            // range; the tables are handed as windows anyway.
            c.mem->PokeWord(gs + 0x3Cu, c.rng->U32() % 3u);
            {
                const uint8_t b4 = (c.rng->Next() % 4u == 0)
                                       ? 44u
                                       : static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(gs + 4u, &b4, 1);
                const uint8_t phase = static_cast<uint8_t>(c.rng->U32() % 6u);
                c.mem->WriteBlock(gs + 0x39u, &phase, 1);
            }

            args.a[0] = ea;
            args.a[1] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> 12);
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            const uint32_t ea = args.a[0];
            std::vector<uint8_t> e(1096), rd(72), st(512), gs(64);
            m.ReadBlock(ea, e.data(), e.size());
            const uint32_t rdAddr = m.PeekWord(ea + rr::sim::ent::kRiderDef);
            const uint32_t stAddr = m.PeekWord(ea + rr::sim::ent::kStats);
            m.ReadBlock(rdAddr, rd.data(), rd.size());
            m.ReadBlock(stAddr, st.data(), st.size());
            m.ReadBlock(m.PeekWord(kGameStatePtr), gs.data(), gs.size());

            // Every pointer chase is the caller's, as everywhere in this bench: the four
            // candidate reference bikes and their rider records are resolved
            // here, the CHOICE between them is the port's.
            std::vector<std::vector<uint8_t>> blocks;
            auto load = [&](uint32_t addr, size_t n) -> const uint8_t* {
                blocks.emplace_back(n, static_cast<uint8_t>(0));
                if (addr != 0) m.ReadBlock(addr, blocks.back().data(), n);
                return blocks.back().data();
            };
            const uint32_t p1 = m.PeekWord(kPlayer1Ptr);
            const uint32_t p2 = m.PeekWord(kPlayer2Ptr);
            const uint32_t s0 = m.PeekWord(kPlayerArr + 0);
            const uint32_t s1 = m.PeekWord(kPlayerArr + 4);
            const uint32_t jb = m.PeekWord(kJailbreakPtr);
            blocks.reserve(16);

            rr::sim::AiSpeedEnv env;
            env.gameState = gs.data();
            env.player1.entity = load(p1, 1096);
            env.player1.riderDef = load(m.PeekWord(p1 + rr::sim::ent::kRiderDef), 72);
            env.player2.entity = load(p2, 1096);
            env.player2.riderDef = load(m.PeekWord(p2 + rr::sim::ent::kRiderDef), 72);
            env.playerSlot[0].entity = load(s0, 1096);
            env.playerSlot[0].riderDef = load(m.PeekWord(s0 + rr::sim::ent::kRiderDef), 72);
            env.playerSlot[1].entity = load(s1, 1096);
            env.playerSlot[1].riderDef = load(m.PeekWord(s1 + rr::sim::ent::kRiderDef), 72);
            env.jailbreakBike = (jb != 0) ? load(jb, 1096) : nullptr;
            env.liveBikes = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            env.bikeCap = static_cast<int32_t>(m.PeekWord(kBikeCap));
            env.spreadDiv = static_cast<int32_t>(m.PeekWord(kSpreadDiv));
            env.raceFlags = m.PeekWord(kAiRaceFlags);
            env.altFlag = m.PeekWord(kAltFlag);
            env.copScale = static_cast<int32_t>(m.PeekWord(kCopScale));
            int32_t clockStamp = static_cast<int32_t>(m.PeekWord(kClockStamp));
            int32_t spreadBand = static_cast<int32_t>(m.PeekWord(kSpreadBand));
            env.clockStamp = &clockStamp;
            env.spreadBand = &spreadBand;

            // The eleven tables, as windows around their nominal addresses.
            std::vector<std::vector<uint8_t>> tables;
            tables.reserve(16);
            auto table = [&](uint32_t addr, uint32_t bytes) -> const uint8_t* {
                tables.emplace_back(static_cast<size_t>(bytes), static_cast<uint8_t>(0));
                m.ReadBlock(addr, tables.back().data(), bytes);
                return tables.back().data();
            };
            auto t32 = [&](uint32_t addr) {
                return reinterpret_cast<const int32_t*>(table(addr, 256));
            };
            auto t16 = [&](uint32_t addr) {
                return reinterpret_cast<const int16_t*>(table(addr, 256));
            };
            env.tabSpeedClass = t32(kTabSpeedClass);
            env.tabThinkA = t32(kTabThinkA);
            env.tabThinkB = t32(kTabThinkB);
            env.tabCopFlat = t32(kTabCopFlat);
            env.tabAltA = t32(kTabAltA);
            env.tabAltB = t32(kTabAltB);
            env.tabSpeedCap = t32(kTabSpeedCap);
            env.tabCopBase = t16(kTabCopBase);
            env.tabCopStep = t16(kTabCopStep);
            env.tabCopMul = t32(kTabCopMul);
            std::vector<uint8_t> prof(2u * kProfileWindow);
            m.ReadBlock(kProfile - kProfileWindow, prof.data(), prof.size());
            env.profile = reinterpret_cast<const int8_t*>(prof.data() + kProfileWindow);

            const int32_t r =
                rr::sim::AiTargetSpeed(e.data(), rd.data(), st.data(),
                                       static_cast<int32_t>(args.a[1]), env);
            m.WriteBlock(ea, e.data(), e.size());
            m.WriteBlock(rdAddr, rd.data(), rd.size());
            m.WriteBlock(stAddr, st.data(), st.size());
            m.PokeWord(kClockStamp, static_cast<uint32_t>(clockStamp));
            m.PokeWord(kSpreadBand, static_cast<uint32_t>(spreadBand));
            (void)scratch;
            return static_cast<uint32_t>(r);
        }});

    rows.push_back(Row{
        "ai_run_commands",
        "void AiRunCommands(s32 dt, u32 skip, u32 maskB)  [RASHCDG, the command pass]",
        kAiRunCommands,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            // The pool-0 base pointer is redirected into the scratch so the pass walks three bikes
            // the bench built rather than the snapshot's own eighteen; the stride of 1096 is the
            // dispatcher's own and is not changed.
            const uint32_t pool = c.scratch;
            const uint32_t bikes = 3;
            const uint32_t rdBase = c.scratch + 3328u;
            const uint32_t slBase = c.scratch + 3600u;
            c.mem->PokeWord(kPoolBasePtr, pool);
            c.mem->PokeWord(kLiveBikes, bikes);
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 4u);
            {
                const uint8_t b4 = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(gs + 4u, &b4, 1);
                const uint8_t phase = static_cast<uint8_t>(c.rng->U32() % 8u);
                c.mem->WriteBlock(gs + 0x39u, &phase, 1);
            }
            c.mem->PokeWord(gs + 0x10u, (c.rng->Next() & 1u) ? (c.rng->U32() % 1800u) : c.rng->U32());
            c.mem->PokeWord(kAiRaceFlags, c.rng->U32());
            for (uint32_t p = 0; p < 4; ++p)
                WriteU16Bench(*c.mem, kAttackerMask + 2u * p, static_cast<uint16_t>(c.rng->U32()));

            for (uint32_t i = 0; i < bikes; ++i) {
                const uint32_t ea = pool + 1096u * i;
                const uint32_t rd = rdBase + 72u * i;
                const uint32_t sl = slBase + 16u * i;
                std::vector<uint8_t> e = src.entity;
                if (e.size() < 1096) e.resize(1096, 0);
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                auto put16 = [&e](uint32_t off, uint16_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                };
                put32(rr::sim::ent::kRiderDef, rd);
                put32(0x154, sl);
                put32(rr::sim::ent::kFlagsA,
                      (c.rng->Next() % 3u == 0) ? (c.rng->U32() & ~0x08000000u)
                                                : (c.rng->U32() | 0x08000000u));
                put16(rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() % 24u));
                put16(0x188, (c.rng->Next() % 3u == 0) ? 4u : static_cast<uint16_t>(c.rng->U32()));
                put16(rr::sim::ent::kAimLateral, static_cast<uint16_t>(c.rng->S16()));
                put32(0x158, static_cast<uint32_t>(c.rng->World()));
                put32(0x16C, static_cast<uint32_t>(c.rng->World()));
                for (uint32_t k = 0; k < 3; ++k) {
                    put16(rr::sim::ent::kAimSliceAxis + 2u * k, static_cast<uint16_t>(c.rng->Unit()));
                    put32(rr::sim::ent::kAimPoint + 4u * k, static_cast<uint32_t>(c.rng->World()));
                    put32(rr::sim::ent::kAimDelta + 4u * k, static_cast<uint32_t>(c.rng->World()));
                }
                put32(rr::sim::ent::kAimRate, c.rng->U32());
                put32(rr::sim::ent::kAimBlend, c.rng->U32());
                put16(rr::sim::ent::kAimSuppress,
                      static_cast<uint16_t>((c.rng->Next() % 3u == 0)
                                                ? static_cast<uint16_t>(1u + c.rng->U32() % 30000u)
                                                : static_cast<uint16_t>(c.rng->S16() & ~0x7FFF)));
                // BOUNDED INPUT FAMILY, named twice over. First the depth: a signed byte the
                // original does not bound, held at 0..15 here so that the slot this row plants an
                // opcode into is the slot the dispatcher reads. (The negative-depth shape, where
                // the original indexes BELOW the stack, is covered by `ai_push_command` and
                // `ai_target_speed`, which both drive it deliberately.)
                e[0x3B2] = static_cast<uint8_t>(c.rng->U32() % 16u);
                for (uint32_t k = 0; k < 136; ++k)
                    e[0x3B4u + k] = static_cast<uint8_t>(c.rng->U32());
                // And then the OPCODE. Only two of the nineteen arms are ported, so the family is
                // restricted to the opcodes that reach them: 4 (the racing line) and the eight that
                // share the do-nothing arm, plus values >= 19 which the dispatcher skips outright.
                // The other seventeen handlers are benched by their own rows -
                // this row does not pretend to cover them, and the restriction is stated here
                // rather than hidden in a stub.
                {
                    static const uint16_t kPortedOps[] = {4, 4, 4, 0, 3, 10, 11, 13, 14, 15};
                    const int32_t d = static_cast<int8_t>(e[0x3B2]);
                    const uint32_t off = 0x3B4u + 8u * static_cast<uint32_t>(d);
                    uint16_t op = kPortedOps[c.rng->U32() % 10u];
                    if (c.rng->Next() % 8u == 0) op = static_cast<uint16_t>(19u + c.rng->U32() % 64u);
                    if (off + 8 <= 1096) {
                        e[off] = static_cast<uint8_t>(op);
                        e[off + 1] = static_cast<uint8_t>(op >> 8);
                        const uint16_t tgt = static_cast<uint16_t>(c.rng->U32() % 6u);
                        e[off + 2] = static_cast<uint8_t>(tgt);
                        e[off + 3] = static_cast<uint8_t>(tgt >> 8);
                        e[off + 6] = static_cast<uint8_t>(c.rng->U32());
                        e[off + 7] = static_cast<uint8_t>(c.rng->U32());
                    }
                }
                c.mem->WriteBlock(ea, e.data(), e.size());

                std::vector<uint8_t> rdv(72, 0);
                for (size_t k = 0; k < rdv.size(); ++k) rdv[k] = static_cast<uint8_t>(c.rng->U32());
                rdv[1] = static_cast<uint8_t>((rdv[1] & 0xF0u) | (c.rng->U32() % 4u));
                c.mem->WriteBlock(rd, rdv.data(), rdv.size());
                for (uint32_t k = 0; k < 4; ++k)
                    PutS16(*c.mem, sl + 2u * k, (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit());
            }

            args.a[0] = c.sample ? 0x884u : c.rng->U32();  // dt
            args.a[1] = c.rng->U32() % 8u;                 // skip
            args.a[2] = c.rng->U32() % 8u;                 // maskB
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            const uint32_t pool = m.PeekWord(kPoolBasePtr);
            const uint32_t count = m.PeekWord(kLiveBikes);
            std::vector<std::vector<uint8_t>> ents(count), rds(count);
            std::vector<std::array<int16_t, 8>> slices(count);
            std::vector<uint32_t> entAddr(count), rdAddr(count);
            std::vector<rr::sim::AiCommandNode> nodes(count);
            std::vector<uint8_t> gs(64);
            m.ReadBlock(m.PeekWord(kGameStatePtr), gs.data(), gs.size());
            for (uint32_t i = 0; i < count; ++i) {
                entAddr[i] = pool + 1096u * i;
                ents[i].assign(1096, 0);
                m.ReadBlock(entAddr[i], ents[i].data(), 1096);
                rdAddr[i] = m.PeekWord(entAddr[i] + rr::sim::ent::kRiderDef);
                rds[i].assign(72, 0);
                m.ReadBlock(rdAddr[i], rds[i].data(), 72);
                const uint32_t sl = m.PeekWord(entAddr[i] + 0x154u);
                for (int k = 0; k < 8; ++k)
                    slices[i][static_cast<size_t>(k)] = GetS16(m, sl + 2u * static_cast<uint32_t>(k));
                nodes[i].entity = ents[i].data();
                nodes[i].riderDef = rds[i].data();
                nodes[i].slice = slices[i].data();
            }
            std::vector<uint16_t> mask(8);
            for (uint32_t p = 0; p < 8; ++p)
                mask[p] = static_cast<uint16_t>(GetS16(m, kAttackerMask + 2u * p));
            std::vector<uint32_t> arms(19);
            for (uint32_t k = 0; k < 19; ++k) arms[k] = m.PeekWord(kArmTable + 4u * k);
            std::vector<int16_t> w(kSqrtWindowHalf);
            m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, w.data(), w.size() * 2u);

            rr::sim::AiRunEnv env;
            env.gameState = gs.data();
            env.raceFlags = m.PeekWord(kAiRaceFlags);
            env.sqrtTable = w.data() + kSqrtWindowHalf / 4u;
            env.attackerMask = mask.data();
            env.armTable = arms.data();
            env.armNone = kArmNone;
            env.armRace = kArmRace;
            const bool ok = rr::sim::AiRunCommands(nodes.data(), count,
                                                   static_cast<int32_t>(args.a[0]), args.a[1],
                                                   args.a[2], env);
            if (!ok) {
                // Leave the candidate's memory deliberately wrong rather than guess: the row must
                // FAIL loudly if the input family ever reaches an unported arm.
                return 0u;
            }
            for (uint32_t i = 0; i < count; ++i) {
                m.WriteBlock(entAddr[i], ents[i].data(), 1096);
                m.WriteBlock(rdAddr[i], rds[i].data(), 72);
            }
            for (uint32_t p = 0; p < 8; ++p) WriteU16Bench(m, kAttackerMask + 2u * p, mask[p]);
            (void)scratch;
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "ai_recover_line",
        "s32 AiRecoverLine(Entity *e)  [RASHCDG, the drive pass's off-the-line correction]",
        kAiRecoverLine,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t ea = c.scratch;
            const uint32_t slice = c.scratch + 1152u;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            auto put16 = [&e](uint32_t off, uint16_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
            };
            // Bit 27 is the "under AI control" gate; both sides matter.
            put32(rr::sim::ent::kFlagsA, (c.rng->Next() % 4u == 0)
                                             ? (c.rng->U32() & ~0x08000000u)
                                             : (c.rng->U32() | 0x08000000u));
            // `+0x184`: bit 0 arms the whole function, bit 4 selects the moving arm, and bits
            // 8..19 are the half width the lateral band is measured against. All three are driven
            // deliberately as well as at random.
            {
                uint32_t road = c.rng->U32();
                if (c.rng->Next() % 4u != 0) road |= 1u;
                if (c.rng->Next() % 3u != 0) road |= 0x10u;
                road = (road & ~0x000FFF00u) | ((c.rng->U32() % 4096u) << 8);
                put32(0x184, road);
            }
            // The speed copy straddles the 0x00023FFF threshold that picks the arm.
            put32(rr::sim::ent::kSpeedCopy,
                  (c.rng->Next() & 1u) ? (c.rng->U32() % 0x60000u)
                                       : static_cast<uint32_t>(c.rng->S32() >> 8));
            put32(0x158, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            put32(0x15C, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            put32(0x154, slice);
            for (uint32_t k = 0; k < 3; ++k) {
                put32(rr::sim::ent::kAimPoint + 4u * k, static_cast<uint32_t>(c.rng->World()));
                put16(rr::sim::ent::kAimSliceAxis + 2u * k,
                      static_cast<uint16_t>((c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit()));
            }
            put16(rr::sim::ent::kAimLateral, static_cast<uint16_t>(c.rng->U32()));
            put16(rr::sim::ent::kAimSuppress, static_cast<uint16_t>(c.rng->U32()));
            put32(rr::sim::ent::kCmdSpeed, c.rng->U32());
            e[rr::sim::ent::kAiState] = static_cast<uint8_t>(c.rng->U32());
            c.mem->WriteBlock(ea, e.data(), e.size());
            // One 52-byte road slice: the origin at +20 and the tangent (row 2 of the 3x3) at +14
            // are the two fields the slow arm reads.
            for (uint32_t i = 0; i < 52; i += 4) PutS32(*c.mem, slice + i, c.rng->World());
            for (uint32_t k = 0; k < 3; ++k)
                PutS16(*c.mem, slice + 14u + 2u * k,
                       (c.rng->Next() & 1u) ? c.rng->S16() : c.rng->Unit());
            // `gameState[+0x3C]`, the race-type bank the fast arm loads the commanded speed from.
            c.mem->PokeWord(c.mem->PeekWord(kGameStatePtr) + 0x3Cu, c.rng->U32() % 3u);
            args.a[0] = ea;
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            const uint32_t ea = args.a[0];
            std::vector<uint8_t> e(1096);
            m.ReadBlock(ea, e.data(), e.size());
            const uint32_t sliceAddr = m.PeekWord(ea + 0x154u);
            int32_t pos[3];
            int16_t tangent[3];
            for (uint32_t k = 0; k < 3; ++k) {
                pos[k] = GetS32(m, sliceAddr + 20u + 4u * k);
                tangent[k] = GetS16(m, sliceAddr + 14u + 2u * k);
            }
            const int32_t bank =
                static_cast<int32_t>(m.PeekWord(m.PeekWord(kGameStatePtr) + 0x3Cu));
            const int32_t r = rr::sim::AiRecoverLine(rr::sim::EntityView(e.data()), pos, tangent,
                                                     bank);
            m.WriteBlock(ea, e.data(), e.size());
            return static_cast<uint32_t>(r);
        }});

    rows.push_back(Row{
        "ai_pop_command",
        "void AiPopCommand(Entity *e)  [RASHCDG, the recursive pop the drive pass calls]",
        kAiPopCmd,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t ea = c.scratch;
            const uint32_t riderDef = c.scratch + 1152u;
            const uint32_t rider = c.scratch + 1280u;
            const uint32_t fight = c.scratch + 2048u;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            put32(rr::sim::ent::kRiderDef, riderDef);
            put32(rr::sim::ent::kOwner, rider);
            put32(rr::sim::ent::kFlagsA, (c.rng->Next() % 3u == 0)
                                             ? (c.rng->U32() & ~0x08000000u)
                                             : (c.rng->U32() | 0x08000000u));
            put32(rr::sim::ent::kCrashTimer, c.rng->U32());
            // BOUNDED INPUT FAMILY, named: the depth is a signed byte the original bounds nowhere,
            // and both the pop and the recursion index the stack through it. -4..16 is the game's
            // own range plus a margin, and it keeps the 8-byte clear inside the 1096-byte entity.
            {
                const int32_t d = (c.rng->Next() % 6u == 0)
                                      ? -static_cast<int32_t>(c.rng->U32() % 5u)
                                      : static_cast<int32_t>(c.rng->U32() % 17u);
                e[0x3B2] = static_cast<uint8_t>(static_cast<int8_t>(d));
            }
            // The whole 16-slot stack. Opcode 16 on the slot below the top is what makes the
            // original recurse, and 10..16 on the top is what clears the combo nibble, so both are
            // planted on purpose rather than waited for.
            for (uint32_t i = 0; i < 136; ++i) e[0x3B4u + i] = static_cast<uint8_t>(c.rng->U32());
            for (uint32_t k = 0; k <= 16; ++k) {
                const uint32_t off = 0x3B4u + 8u * k;
                uint16_t op = static_cast<uint16_t>(c.rng->U32() % 20u);
                if (c.rng->Next() % 3u == 0) op = 16;
                else if (c.rng->Next() % 3u == 0) op = static_cast<uint16_t>(10u + c.rng->U32() % 7u);
                e[off] = static_cast<uint8_t>(op);
                e[off + 1] = static_cast<uint8_t>(op >> 8);
                const uint16_t tgt = static_cast<uint16_t>(c.rng->U32() % 6u);
                e[off + 2] = static_cast<uint8_t>(tgt);
                e[off + 3] = static_cast<uint8_t>(tgt >> 8);
            }
            c.mem->WriteBlock(ea, e.data(), e.size());

            std::vector<uint8_t> rd(72, 0);
            for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
            c.mem->WriteBlock(riderDef, rd.data(), rd.size());

            // The rider is a ZEROED block for exactly the reason given for `ai_push_command`:
            // the oracle-supplied callee 0x800C4550 reaches on
            // through `rider[+0x21C]` and a rider made of noise walks the interpreter into unmapped
            // memory. The same limit therefore applies to this row.
            {
                const std::vector<uint8_t> r(640, 0);
                c.mem->WriteBlock(rider, r.data(), r.size());
            }
            const uint32_t kind = c.rng->U32() % 256u;
            WriteU16Bench(*c.mem, rider + 0x220u, static_cast<uint16_t>(kind));
            {
                const uint8_t id = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(rider + 0x239u, &id, 1);
            }
            if (c.rng->Next() & 1u) WriteU16Bench(*c.mem, kAltKindTable + 8u * kind + 2u, 3);
            for (uint32_t i = 0; i < 3072; i += 4) c.mem->PokeWord(fight + i, c.rng->U32());
            c.mem->PokeWord(kFightRecPtr, fight);

            // The globals the tail reads: the player count, the mode byte (36 and 44 both stop the
            // recursion) and the per-player attacker masks.
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 6u);
            {
                const uint8_t mode = (c.rng->Next() % 4u == 0)
                                         ? ((c.rng->Next() & 1u) ? 36u : 44u)
                                         : static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(gs + 4u, &mode, 1);
            }
            for (uint32_t p = 0; p < 8; ++p) {
                uint32_t m = c.rng->U32() % 8u;
                if (c.rng->Next() % 3u == 0) m = 1u << (c.rng->U32() % 4u); // exactly one attacker
                WriteU16Bench(*c.mem, kAttackerMask + 2u * p, static_cast<uint16_t>(m));
            }
            args.a[0] = ea;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) {
            Memory& m = env.clone->mem;
            const uint32_t ea = args.a[0];
            const uint32_t rdAddr = m.PeekWord(ea + rr::sim::ent::kRiderDef);
            const uint32_t riderAddr = m.PeekWord(ea + rr::sim::ent::kOwner);
            std::vector<uint8_t> e(1096), rd(72), rider(640), gs(64);
            m.ReadBlock(ea, e.data(), e.size());
            m.ReadBlock(rdAddr, rd.data(), rd.size());
            m.ReadBlock(riderAddr, rider.data(), rider.size());
            m.ReadBlock(m.PeekWord(kGameStatePtr), gs.data(), gs.size());
            std::vector<uint8_t> altKind(2048), fight(3072);
            m.ReadBlock(kAltKindTable, altKind.data(), altKind.size());
            m.ReadBlock(m.PeekWord(kFightRecPtr), fight.data(), fight.size());
            std::vector<uint16_t> mask(8);
            for (uint32_t p = 0; p < 8; ++p)
                mask[p] = static_cast<uint16_t>(GetS16(m, kAttackerMask + 2u * p));

            struct OracleStance final : rr::sim::AiStanceSink {
                NativeEnv& env;
                std::vector<uint8_t>&e, &rd, &rider;
                uint32_t ea, rdAddr, riderAddr;
                OracleStance(NativeEnv& v, std::vector<uint8_t>& a, std::vector<uint8_t>& b,
                             std::vector<uint8_t>& c2, uint32_t x, uint32_t y, uint32_t z)
                    : env(v), e(a), rd(b), rider(c2), ea(x), rdAddr(y), riderAddr(z) {}
                void PlayIdleStance(uint16_t event, uint32_t riderArg) override {
                    env.clone->mem.WriteBlock(ea, e.data(), e.size());
                    env.clone->mem.WriteBlock(rdAddr, rd.data(), rd.size());
                    env.clone->mem.WriteBlock(riderAddr, rider.data(), rider.size());
                    AnimNativeStanceEvent(env, event, riderArg, 2, env.sp); // ported, witnessed (rows_anim.inc)
                    env.clone->mem.ReadBlock(ea, e.data(), e.size());
                    env.clone->mem.ReadBlock(rdAddr, rd.data(), rd.size());
                    env.clone->mem.ReadBlock(riderAddr, rider.data(), rider.size());
                }
            } stance(env, e, rd, rider, ea, rdAddr, riderAddr);

            rr::sim::AiPopEnv pe;
            pe.gameState = gs.data();
            pe.riderDef = rd.data();
            pe.rider = rider.data();
            pe.riderAddress = riderAddr;
            pe.altKindTable = altKind.data();
            pe.fightRecords = fight.data();
            pe.attackerMask = mask.data();
            pe.stance = &stance;
            rr::sim::AiPopCommand(e.data(), pe);
            m.WriteBlock(ea, e.data(), e.size());
            m.WriteBlock(rdAddr, rd.data(), rd.size());
            m.WriteBlock(riderAddr, rider.data(), rider.size());
            return 0u;
        },
        /*oracleCallees=*/{OracleCallee{kAiStance, 3}}});

    rows.push_back(Row{
        "ai_drive",
        "void AiDrive(Entity *e, s32 dt)  [RASHCDG, the aim point and the commanded speed]",
        kAiDrive,
        [](CaseContext& c, Args& args) {
            const uint32_t ea       = c.scratch;
            const uint32_t riderDef = c.scratch + 1152u;
            const uint32_t stats    = c.scratch + 1280u;
            const uint32_t p1e      = c.scratch + 1792u;
            const uint32_t p1r      = c.scratch + 2896u;
            const uint32_t p2e      = c.scratch + 2976u;
            const uint32_t p2r      = c.scratch + 4080u;

            // BOUNDED INPUT FAMILY, named, and it is the sharpest bound in this row. The 32-byte
            // road cursor at `e[+0x148]` is four POINTERS into live road data plus the position
            // inside the current slice, and the oracle-supplied road query walks them. A cursor
            // lifted out of one of the other seventeen RAM images points at road this machine does
            // not contain, so every case takes its cursor - and the whole entity it belongs to -
            // from a live pool-0 bike of the BENCH'S OWN snapshot. The dump-derived half of the row
            // is therefore "one of this machine's 18 real bikes", not "one of 324 bikes from 18
            // machines"; the scalars layered on top below are what the two families differ in.
            const uint32_t poolBase = c.mem->PeekWord(kPoolTable);
            const uint32_t highPtr = c.mem->PeekWord(kPoolTable + 12u);
            const int32_t stride = static_cast<int32_t>(c.mem->PeekWord(kPoolTable + 4u));
            uint32_t slots = 1;
            if (highPtr >= 0x80000000u) {
                const int32_t high = static_cast<int32_t>(c.mem->PeekWord(highPtr));
                if (high >= 0 && high < 64) slots = static_cast<uint32_t>(high) + 1u;
            }
            const uint32_t slot = c.rng->U32() % slots;
            std::vector<uint8_t> e(1096, 0);
            if (poolBase >= 0x80000000u && stride >= 1096)
                c.mem->ReadBlock(poolBase + slot * static_cast<uint32_t>(stride), e.data(), 1096);

            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            put32(rr::sim::ent::kStats, stats);
            put32(rr::sim::ent::kRiderDef, riderDef);
            // Bit 27 gates the whole road-query half; a quarter of the cases clear it so the
            // commanded-speed half is exercised on its own too.
            put32(rr::sim::ent::kFlagsA, (c.rng->Next() % 4u == 0)
                                             ? (c.rng->U32() & ~0x08000000u)
                                             : (c.rng->U32() | 0x08000000u));
            // The speed that scales the look-ahead: a real captured one on the derived half.
            put32(rr::sim::ent::kSpeed,
                  static_cast<uint32_t>(c.sample ? c.sample->speed : (c.rng->S32() >> 9)));
            // `+0x16C`, the direction of travel: its SIGN picks between the two road-walk function
            // pointers (SLUS 0x80037A30 and 0x80037FBC), so it is flipped deliberately.
            if (c.rng->Next() & 1u) {
                const uint32_t cur = static_cast<uint32_t>(e[0x16C]) |
                                     (static_cast<uint32_t>(e[0x16D]) << 8) |
                                     (static_cast<uint32_t>(e[0x16E]) << 16) |
                                     (static_cast<uint32_t>(e[0x16F]) << 24);
                put32(0x16C, 0u - cur);
            }
            put32(0x144, (c.rng->Next() % 5u == 0)
                             ? static_cast<uint32_t>(c.rng->S32() >> 12)
                             : static_cast<uint32_t>(c.rng->U32() % 0x100000u));
            put32(rr::sim::ent::kFlagsB, c.rng->U32());
            put32(rr::sim::ent::kCrashTimer,
                  static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
            e[rr::sim::ent::kAiState] =
                static_cast<uint8_t>(c.rng->U32() | ((c.rng->Next() % 3u == 0) ? 0x18u : 0u));
            e[rr::sim::ent::kRubberBand] = static_cast<uint8_t>(c.rng->U32());
            {
                const int32_t d = (c.rng->Next() % 4u == 0)
                                      ? -static_cast<int32_t>(c.rng->U32() % 9u)
                                      : static_cast<int32_t>(c.rng->U32() % 16u);
                e[0x3B2] = static_cast<uint8_t>(static_cast<int8_t>(d));
            }
            for (uint32_t i = 0; i < 136; ++i) e[0x3B4u + i] = static_cast<uint8_t>(c.rng->U32());
            c.mem->WriteBlock(ea, e.data(), e.size());

            std::vector<uint8_t> rd(72, 0);
            for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
            // Bit 7 of byte 0 is "ride the route backwards" - the other half of the direction test.
            rd[1] = static_cast<uint8_t>((rd[1] & 0xF0u) |
                                         ((c.rng->Next() % 3u == 0) ? 2u : (c.rng->U32() % 4u)));
            if (c.rng->Next() % 3u != 0) rd[0] = static_cast<uint8_t>(rd[0] & ~0x08u);
            rd[0x27] = static_cast<uint8_t>(c.rng->U32() % 20u);
            c.mem->WriteBlock(riderDef, rd.data(), rd.size());

            std::vector<uint8_t> st(512, 0);
            for (size_t i = 0; i < st.size(); i += 4) {
                const uint32_t v = static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 10u));
                st[i] = static_cast<uint8_t>(v);
                st[i + 1] = static_cast<uint8_t>(v >> 8);
                st[i + 2] = static_cast<uint8_t>(v >> 16);
                st[i + 3] = static_cast<uint8_t>(v >> 24);
            }
            c.mem->WriteBlock(stats, st.data(), st.size());

            // The two reference bikes AiTargetSpeed picks between, exactly as `ai_target_speed`
            // builds them.
            auto buildBike = [&](uint32_t addr, uint32_t rdAddr) {
                std::vector<uint8_t> b(1096, 0);
                if (poolBase >= 0x80000000u && stride >= 1096)
                    c.mem->ReadBlock(poolBase + (c.rng->U32() % slots) *
                                                    static_cast<uint32_t>(stride),
                                     b.data(), 1096);
                auto p32 = [&b](uint32_t off, uint32_t v) {
                    b[off] = static_cast<uint8_t>(v);
                    b[off + 1] = static_cast<uint8_t>(v >> 8);
                    b[off + 2] = static_cast<uint8_t>(v >> 16);
                    b[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                p32(0x144, (c.rng->Next() % 6u == 0)
                               ? static_cast<uint32_t>(c.rng->S32() >> 12)
                               : static_cast<uint32_t>(c.rng->U32() % 0x100000u));
                p32(rr::sim::ent::kSpeed, static_cast<uint32_t>(c.rng->S32() >> 9));
                p32(rr::sim::ent::kRiderDef, rdAddr);
                c.mem->WriteBlock(addr, b.data(), b.size());
                std::vector<uint8_t> r(72, 0);
                for (size_t i = 0; i < r.size(); ++i) r[i] = static_cast<uint8_t>(c.rng->U32());
                r[0x27] = static_cast<uint8_t>((c.rng->Next() % 5u == 0)
                                                   ? (248u + c.rng->U32() % 8u)
                                                   : (c.rng->U32() % 20u));
                const uint32_t fin = (c.rng->Next() % 3u == 0) ? c.rng->U32() : 0u;
                r[0x28] = static_cast<uint8_t>(fin);
                r[0x29] = static_cast<uint8_t>(fin >> 8);
                r[0x2A] = static_cast<uint8_t>(fin >> 16);
                r[0x2B] = static_cast<uint8_t>(fin >> 24);
                r[1] = static_cast<uint8_t>((r[1] & 0xF0u) | (c.rng->U32() % 4u));
                c.mem->WriteBlock(rdAddr, r.data(), r.size());
            };
            buildBike(p1e, p1r);
            buildBike(p2e, p2r);

            c.mem->PokeWord(kPlayer1Ptr, p1e);
            c.mem->PokeWord(kPlayer2Ptr, p2e);
            c.mem->PokeWord(kPlayerArr + 0, p1e);
            c.mem->PokeWord(kPlayerArr + 4, p2e);
            c.mem->PokeWord(kJailbreakPtr, (c.rng->Next() % 4u == 0) ? 0u : p1e);
            c.mem->PokeWord(kLiveBikes, c.rng->U32() % 24u);
            c.mem->PokeWord(kBikeCap, c.rng->U32() % 24u);
            c.mem->PokeWord(kSpreadDiv, (c.rng->Next() % 16u == 0) ? 0u : (1u + c.rng->U32() % 64u));
            c.mem->PokeWord(kAiRaceFlags, c.rng->U32());
            c.mem->PokeWord(kAltFlag, c.rng->U32() % 2u);
            c.mem->PokeWord(kCopScale, static_cast<uint32_t>(c.rng->S32() >> 8));
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            const uint32_t clock = (c.rng->Next() & 1u) ? (c.rng->U32() % 200000u) : c.rng->U32();
            c.mem->PokeWord(gs + 0x10u, clock);
            c.mem->PokeWord(kClockStamp, (c.rng->Next() & 1u) ? clock : c.rng->U32());
            c.mem->PokeWord(kSpreadBand, c.rng->U32() % 5u);
            // `gameState[+0x30]` is the player count the handle is compared against; 0..20 puts
            // this machine's real handles (0..17) on both sides of it, so the human-player arm is
            // exercised as well as the opponent one.
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 21u);
            c.mem->PokeWord(gs + 0x3Cu, c.rng->U32() % 3u);
            {
                // BOUNDED INPUT FAMILY, named: bit 0 of `gameState[+0x04]` is cleared on every
                // case. With it SET and a human player's bike under AI control the original calls
                // RASHCDG 0x80096818, 0x8008A998 and 0x80086AF8, none of which is read or ported;
                // the port returns false there instead of guessing, and this row does not drive it
                // into that arm. `ai_target_speed` covers bit 0 for the arms below it.
                uint8_t b4 = (c.rng->Next() % 4u == 0) ? 44u : static_cast<uint8_t>(c.rng->U32());
                b4 = static_cast<uint8_t>(b4 & ~1u);
                c.mem->WriteBlock(gs + 4u, &b4, 1);
                const uint8_t phase = static_cast<uint8_t>(c.rng->U32() % 6u);
                c.mem->WriteBlock(gs + 0x39u, &phase, 1);
            }

            args.a[0] = ea;
            args.a[1] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> 12);
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t ea = args.a[0];
            const uint32_t rdAddr = m.PeekWord(ea + rr::sim::ent::kRiderDef);
            const uint32_t stAddr = m.PeekWord(ea + rr::sim::ent::kStats);
            std::vector<uint8_t> e(1096), rd(72), st(512), gs(64);
            m.ReadBlock(ea, e.data(), e.size());
            m.ReadBlock(rdAddr, rd.data(), rd.size());
            m.ReadBlock(stAddr, st.data(), st.size());
            m.ReadBlock(m.PeekWord(kGameStatePtr), gs.data(), gs.size());

            // SLUS 0x800386DC is PORTED (src\game\sim\road_query.cpp, row `road_look_ahead`), so
            // it does not go through the oracle seam. It runs on the clone's own RAM through the
            // guest-address view, at the stack pointer the original AiDrive has (the bench's `sp`
            // minus AiDrive's 88-byte frame), so its frame - and the output cursor AiDrive keeps at
            // its own sp+32 - land where the guest's do. The slice the output cursor names is read
            // by the PORT (GuestAiRoadQuery).
            rr::sim::GuestRam guestRam(m.ram().data(), env.gp);
            rr::sim::GuestAiRoadQuery road(guestRam, ea, e.data(),
                                           env.sp - rr::sim::GuestAiRoadQuery::kAiDriveFrame);

            std::vector<std::vector<uint8_t>> blocks;
            blocks.reserve(16);
            auto load = [&](uint32_t addr, size_t n) -> const uint8_t* {
                blocks.emplace_back(n, static_cast<uint8_t>(0));
                if (addr != 0) m.ReadBlock(addr, blocks.back().data(), n);
                return blocks.back().data();
            };
            const uint32_t p1 = m.PeekWord(kPlayer1Ptr);
            const uint32_t p2 = m.PeekWord(kPlayer2Ptr);
            const uint32_t s0 = m.PeekWord(kPlayerArr + 0);
            const uint32_t s1 = m.PeekWord(kPlayerArr + 4);
            const uint32_t jb = m.PeekWord(kJailbreakPtr);

            rr::sim::AiDriveEnv de;
            de.gameState = gs.data();
            de.riderDef = rd.data();
            de.stats = st.data();
            de.road = &road;
            rr::sim::AiSpeedEnv& se = de.speed;
            se.gameState = gs.data();
            se.player1.entity = load(p1, 1096);
            se.player1.riderDef = load(m.PeekWord(p1 + rr::sim::ent::kRiderDef), 72);
            se.player2.entity = load(p2, 1096);
            se.player2.riderDef = load(m.PeekWord(p2 + rr::sim::ent::kRiderDef), 72);
            se.playerSlot[0].entity = load(s0, 1096);
            se.playerSlot[0].riderDef = load(m.PeekWord(s0 + rr::sim::ent::kRiderDef), 72);
            se.playerSlot[1].entity = load(s1, 1096);
            se.playerSlot[1].riderDef = load(m.PeekWord(s1 + rr::sim::ent::kRiderDef), 72);
            se.jailbreakBike = (jb != 0) ? load(jb, 1096) : nullptr;
            se.liveBikes = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            se.bikeCap = static_cast<int32_t>(m.PeekWord(kBikeCap));
            se.spreadDiv = static_cast<int32_t>(m.PeekWord(kSpreadDiv));
            se.raceFlags = m.PeekWord(kAiRaceFlags);
            se.altFlag = m.PeekWord(kAltFlag);
            se.copScale = static_cast<int32_t>(m.PeekWord(kCopScale));
            int32_t clockStamp = static_cast<int32_t>(m.PeekWord(kClockStamp));
            int32_t spreadBand = static_cast<int32_t>(m.PeekWord(kSpreadBand));
            se.clockStamp = &clockStamp;
            se.spreadBand = &spreadBand;

            std::vector<std::vector<uint8_t>> tables;
            tables.reserve(16);
            auto table = [&](uint32_t addr, uint32_t bytes) -> const uint8_t* {
                tables.emplace_back(static_cast<size_t>(bytes), static_cast<uint8_t>(0));
                m.ReadBlock(addr, tables.back().data(), bytes);
                return tables.back().data();
            };
            auto t32 = [&](uint32_t addr) { return reinterpret_cast<const int32_t*>(table(addr, 256)); };
            auto t16 = [&](uint32_t addr) { return reinterpret_cast<const int16_t*>(table(addr, 256)); };
            se.tabSpeedClass = t32(kTabSpeedClass);
            se.tabThinkA = t32(kTabThinkA);
            se.tabThinkB = t32(kTabThinkB);
            se.tabCopFlat = t32(kTabCopFlat);
            se.tabAltA = t32(kTabAltA);
            se.tabAltB = t32(kTabAltB);
            se.tabSpeedCap = t32(kTabSpeedCap);
            se.tabCopBase = t16(kTabCopBase);
            se.tabCopStep = t16(kTabCopStep);
            se.tabCopMul = t32(kTabCopMul);
            std::vector<uint8_t> prof(2u * kProfileWindow);
            m.ReadBlock(kProfile - kProfileWindow, prof.data(), prof.size());
            se.profile = reinterpret_cast<const int8_t*>(prof.data() + kProfileWindow);

            const bool ok = rr::sim::AiDrive(e.data(), static_cast<int32_t>(args.a[1]), de);
            RqDone(env, guestRam, 0u);
            if (!ok) {
                // The unported human-player arm. Leave the candidate's memory deliberately wrong
                // rather than guess, so that the row FAILS loudly if the family ever reaches it.
                return 0u;
            }
            m.WriteBlock(ea, e.data(), e.size());
            m.WriteBlock(rdAddr, rd.data(), rd.size());
            m.WriteBlock(stAddr, st.data(), st.size());
            m.PokeWord(kClockStamp, static_cast<uint32_t>(clockStamp));
            m.PokeWord(kSpreadBand, static_cast<uint32_t>(spreadBand));
            return 0u;
        },
        /*oracleCallees=*/
        {}});

    rows.push_back(Row{
        "ai_drive_pass",
        "void AiDrivePass(s32 dt, u32 skip, u32 *maskB, u32 *maskC)  [RASHCDG, the drive pass]",
        kAiDrivePass,
        [](CaseContext& c, Args& args) {
            // The pass walks the machine's OWN pool with a stride of 1096 (`addiu s2,s2,1096` at
            // 0x800BA498), and every bike it drives goes through the road query - so, exactly as in
            // `ai_drive`, the bikes have to be this snapshot's real ones, with their real road
            // cursors. The bench therefore edits two live pool-0 slots in place rather than
            // building bikes in the scratch block, and the scratch holds only what hangs off them.
            // BOUNDED INPUT FAMILY, named: two bikes, not the game's eighteen, because the four
            // per-bike blocks below (rider record, stat block, rider object, plus the shared fight
            // records) do not fit eighteen times into the 8 KiB the bench allocates.
            const uint32_t rd[2]    = {c.scratch + 0u, c.scratch + 128u};
            const uint32_t st[2]    = {c.scratch + 256u, c.scratch + 768u};
            const uint32_t rider[2] = {c.scratch + 1280u, c.scratch + 1920u};
            const uint32_t fight    = c.scratch + 2560u;
            const uint32_t pr[2]    = {c.scratch + 5632u, c.scratch + 5760u};
            const uint32_t maskB    = c.scratch + 5888u;
            const uint32_t maskC    = c.scratch + 5904u;

            const uint32_t poolBase = c.mem->PeekWord(kPoolBasePtr);
            const uint32_t count = 2;
            c.mem->PokeWord(kLiveBikes, count);

            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t ea = poolBase + 1096u * i;
                std::vector<uint8_t> e(1096, 0);
                c.mem->ReadBlock(ea, e.data(), e.size());
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                put32(rr::sim::ent::kStats, st[i]);
                put32(rr::sim::ent::kRiderDef, rd[i]);
                put32(rr::sim::ent::kOwner, rider[i]);
                put32(rr::sim::ent::kFlagsA, (c.rng->Next() % 4u == 0)
                                                 ? (c.rng->U32() & ~0x08000000u)
                                                 : (c.rng->U32() | 0x08000000u));
                // Bit 9 of flagsB is the maskC bit the pass collects.
                put32(rr::sim::ent::kFlagsB,
                      (c.rng->Next() & 1u) ? (c.rng->U32() | 0x200u) : (c.rng->U32() & ~0x200u));
                put32(rr::sim::ent::kSpeed,
                      (c.rng->Next() & 1u) ? (c.rng->U32() % 0x40000u)
                                           : static_cast<uint32_t>(c.rng->S32() >> 9));
                {
                    uint16_t awake = static_cast<uint16_t>(c.rng->U32());
                    if (c.rng->Next() % 4u == 0) awake = 0;
                    e[0x140] = static_cast<uint8_t>(awake);
                    e[0x141] = static_cast<uint8_t>(awake >> 8);
                }
                // `+0x184` arms AiRecoverLine, the same three fields `ai_recover_line` drives.
                {
                    uint32_t road = c.rng->U32();
                    if (c.rng->Next() % 4u != 0) road |= 1u;
                    if (c.rng->Next() % 3u != 0) road |= 0x10u;
                    road = (road & ~0x000FFF00u) | ((c.rng->U32() % 4096u) << 8);
                    put32(0x184, road);
                }
                put32(rr::sim::ent::kSpeedCopy,
                      (c.rng->Next() & 1u) ? (c.rng->U32() % 0x60000u)
                                           : static_cast<uint32_t>(c.rng->S32() >> 8));
                if (c.rng->Next() & 1u) {
                    const uint32_t cur = static_cast<uint32_t>(e[0x16C]) |
                                         (static_cast<uint32_t>(e[0x16D]) << 8) |
                                         (static_cast<uint32_t>(e[0x16E]) << 16) |
                                         (static_cast<uint32_t>(e[0x16F]) << 24);
                    put32(0x16C, 0u - cur);
                }
                put32(0x144, (c.rng->Next() % 5u == 0)
                                 ? static_cast<uint32_t>(c.rng->S32() >> 12)
                                 : static_cast<uint32_t>(c.rng->U32() % 0x100000u));
                put32(rr::sim::ent::kCrashTimer,
                      static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
                e[rr::sim::ent::kAiState] =
                    static_cast<uint8_t>(c.rng->U32() | ((c.rng->Next() % 3u == 0) ? 0x18u : 0u));
                e[rr::sim::ent::kRubberBand] = static_cast<uint8_t>(c.rng->U32());
                {
                    const int32_t d = (c.rng->Next() % 6u == 0)
                                          ? -static_cast<int32_t>(c.rng->U32() % 5u)
                                          : static_cast<int32_t>(c.rng->U32() % 17u);
                    e[0x3B2] = static_cast<uint8_t>(static_cast<int8_t>(d));
                }
                for (uint32_t k = 0; k < 136; ++k)
                    e[0x3B4u + k] = static_cast<uint8_t>(c.rng->U32());
                // The top opcode chooses the pass's two arms; 3, 4 and 5..17 are the three steps
                // inside the first one, so all four are planted rather than waited for.
                for (uint32_t k = 0; k <= 16; ++k) {
                    const uint32_t off = 0x3B4u + 8u * k;
                    uint16_t op;
                    switch (c.rng->U32() % 5u) {
                        case 0: op = 3; break;
                        case 1: op = 4; break;
                        case 2: op = static_cast<uint16_t>(5u + c.rng->U32() % 13u); break;
                        case 3: op = 16; break;
                        default: op = static_cast<uint16_t>(c.rng->U32() % 24u); break;
                    }
                    e[off] = static_cast<uint8_t>(op);
                    e[off + 1] = static_cast<uint8_t>(op >> 8);
                    const uint16_t tgt = static_cast<uint16_t>(c.rng->U32() % 6u);
                    e[off + 2] = static_cast<uint8_t>(tgt);
                    e[off + 3] = static_cast<uint8_t>(tgt >> 8);
                }
                c.mem->WriteBlock(ea, e.data(), e.size());

                std::vector<uint8_t> r(72, 0);
                for (size_t k = 0; k < r.size(); ++k) r[k] = static_cast<uint8_t>(c.rng->U32());
                // `+0x28` (a word) separates the two arms of the pass: zero means still racing.
                const uint32_t fin = (c.rng->Next() & 1u) ? 0u : c.rng->U32();
                r[0x28] = static_cast<uint8_t>(fin);
                r[0x29] = static_cast<uint8_t>(fin >> 8);
                r[0x2A] = static_cast<uint8_t>(fin >> 16);
                r[0x2B] = static_cast<uint8_t>(fin >> 24);
                r[0x27] = static_cast<uint8_t>((c.rng->Next() % 5u == 0)
                                                   ? (248u + c.rng->U32() % 8u)
                                                   : (c.rng->U32() % 20u));
                r[1] = static_cast<uint8_t>((r[1] & 0xF0u) |
                                            ((c.rng->Next() % 3u == 0) ? 2u : (c.rng->U32() % 4u)));
                if (c.rng->Next() % 3u != 0) r[0] = static_cast<uint8_t>(r[0] & ~0x08u);
                c.mem->WriteBlock(rd[i], r.data(), r.size());

                std::vector<uint8_t> s(512, 0);
                for (size_t k = 0; k < s.size(); k += 4) {
                    const uint32_t v = static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 10u));
                    s[k] = static_cast<uint8_t>(v);
                    s[k + 1] = static_cast<uint8_t>(v >> 8);
                    s[k + 2] = static_cast<uint8_t>(v >> 16);
                    s[k + 3] = static_cast<uint8_t>(v >> 24);
                }
                c.mem->WriteBlock(st[i], s.data(), s.size());

                // A ZEROED rider, for the same reason as `ai_pop_command` and `ai_push_command`:
                // the stance trigger 0x800C4550 walks a live rider into unmapped memory
                // (see `ai_push_command`).
                const std::vector<uint8_t> z(640, 0);
                c.mem->WriteBlock(rider[i], z.data(), z.size());
                const uint32_t kind = c.rng->U32() % 256u;
                WriteU16Bench(*c.mem, rider[i] + 0x220u, static_cast<uint16_t>(kind));
                const uint8_t id = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(rider[i] + 0x239u, &id, 1);
                if (c.rng->Next() & 1u) WriteU16Bench(*c.mem, kAltKindTable + 8u * kind + 2u, 3);
            }
            for (uint32_t i = 0; i < 3072; i += 4) c.mem->PokeWord(fight + i, c.rng->U32());
            c.mem->PokeWord(kFightRecPtr, fight);

            // AiTargetSpeed's reference bikes: pool slots 2 and 3, which the pass does not reach.
            for (uint32_t k = 0; k < 2; ++k) {
                const uint32_t pe = poolBase + 1096u * (2u + k);
                c.mem->PokeWord(pe + rr::sim::ent::kRiderDef, pr[k]);
                c.mem->PokeWord(pe + 0x144u, (c.rng->Next() % 6u == 0)
                                                 ? static_cast<uint32_t>(c.rng->S32() >> 12)
                                                 : static_cast<uint32_t>(c.rng->U32() % 0x100000u));
                std::vector<uint8_t> r(72, 0);
                for (size_t j = 0; j < r.size(); ++j) r[j] = static_cast<uint8_t>(c.rng->U32());
                r[0x27] = static_cast<uint8_t>((c.rng->Next() % 5u == 0)
                                                   ? (248u + c.rng->U32() % 8u)
                                                   : (c.rng->U32() % 20u));
                const uint32_t fin = (c.rng->Next() % 3u == 0) ? c.rng->U32() : 0u;
                r[0x28] = static_cast<uint8_t>(fin);
                r[0x29] = static_cast<uint8_t>(fin >> 8);
                r[0x2A] = static_cast<uint8_t>(fin >> 16);
                r[0x2B] = static_cast<uint8_t>(fin >> 24);
                r[1] = static_cast<uint8_t>((r[1] & 0xF0u) | (c.rng->U32() % 4u));
                c.mem->WriteBlock(pr[k], r.data(), r.size());
            }
            c.mem->PokeWord(kPlayer1Ptr, poolBase + 1096u * 2u);
            c.mem->PokeWord(kPlayer2Ptr, poolBase + 1096u * 3u);
            c.mem->PokeWord(kPlayerArr + 0, poolBase + 1096u * 2u);
            c.mem->PokeWord(kPlayerArr + 4, poolBase + 1096u * 3u);
            c.mem->PokeWord(kJailbreakPtr,
                            (c.rng->Next() % 4u == 0) ? 0u : (poolBase + 1096u * 2u));
            c.mem->PokeWord(kBikeCap, c.rng->U32() % 24u);
            c.mem->PokeWord(kSpreadDiv, (c.rng->Next() % 16u == 0) ? 0u : (1u + c.rng->U32() % 64u));
            c.mem->PokeWord(kAiRaceFlags, c.rng->U32());
            c.mem->PokeWord(kAltFlag, c.rng->U32() % 2u);
            c.mem->PokeWord(kCopScale, static_cast<uint32_t>(c.rng->S32() >> 8));
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            const uint32_t clock = (c.rng->Next() & 1u) ? (c.rng->U32() % 200000u) : c.rng->U32();
            c.mem->PokeWord(gs + 0x10u, clock);
            c.mem->PokeWord(kClockStamp, (c.rng->Next() & 1u) ? clock : c.rng->U32());
            c.mem->PokeWord(kSpreadBand, c.rng->U32() % 5u);
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 21u);
            c.mem->PokeWord(gs + 0x3Cu, c.rng->U32() % 3u);
            {
                // The same named bound as `ai_drive`: bit 0 clear, so the unported
                // 0x80096818 arm of AiDrive is never reached.
                uint8_t b4 = (c.rng->Next() % 4u == 0) ? 44u : static_cast<uint8_t>(c.rng->U32());
                b4 = static_cast<uint8_t>(b4 & ~1u);
                c.mem->WriteBlock(gs + 4u, &b4, 1);
                const uint8_t phase = static_cast<uint8_t>(c.rng->U32() % 6u);
                c.mem->WriteBlock(gs + 0x39u, &phase, 1);
            }
            for (uint32_t p = 0; p < 8; ++p) {
                uint32_t m = c.rng->U32() % 8u;
                if (c.rng->Next() % 3u == 0) m = 1u << (c.rng->U32() % 4u);
                WriteU16Bench(*c.mem, kAttackerMask + 2u * p, static_cast<uint16_t>(m));
            }
            c.mem->PokeWord(maskB, c.rng->U32());
            c.mem->PokeWord(maskC, c.rng->U32());

            args.a[0] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> 12);
            args.a[1] = c.rng->U32() % 4u; // skip
            args.a[2] = maskB;
            args.a[3] = maskC;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t poolBase = m.PeekWord(kPoolBasePtr);
            const uint32_t count = m.PeekWord(kLiveBikes);
            std::vector<uint8_t> gs(64);
            m.ReadBlock(m.PeekWord(kGameStatePtr), gs.data(), gs.size());
            std::vector<uint8_t> altKind(2048), fight(3072);
            m.ReadBlock(kAltKindTable, altKind.data(), altKind.size());
            m.ReadBlock(m.PeekWord(kFightRecPtr), fight.data(), fight.size());
            std::vector<uint16_t> mask(8);
            for (uint32_t p = 0; p < 8; ++p)
                mask[p] = static_cast<uint16_t>(GetS16(m, kAttackerMask + 2u * p));

            // The PORTED road look-ahead (row `road_look_ahead`), one view per bike: AiDrive runs
            // at the pass's sp minus the pass's 56-byte frame (`addiu sp,sp,-56` at 0x800BA304),
            // and the look-ahead below AiDrive's own 88.
            rr::sim::GuestRam guestRam(m.ram().data(), env.gp);
            const uint32_t aiDriveSp = env.sp - 56u - rr::sim::GuestAiRoadQuery::kAiDriveFrame;

            struct OracleStance final : rr::sim::AiStanceSink {
                NativeEnv& env;
                std::vector<std::vector<uint8_t>>&ents, &rds, &riders;
                std::vector<uint32_t>&entAddr, &rdAddr, &riderAddr;
                OracleStance(NativeEnv& v, std::vector<std::vector<uint8_t>>& a,
                             std::vector<std::vector<uint8_t>>& b,
                             std::vector<std::vector<uint8_t>>& c2, std::vector<uint32_t>& x,
                             std::vector<uint32_t>& y, std::vector<uint32_t>& z)
                    : env(v), ents(a), rds(b), riders(c2), entAddr(x), rdAddr(y), riderAddr(z) {}
                void PlayIdleStance(uint16_t event, uint32_t riderArg) override {
                    for (size_t i = 0; i < ents.size(); ++i) {
                        env.clone->mem.WriteBlock(entAddr[i], ents[i].data(), ents[i].size());
                        env.clone->mem.WriteBlock(rdAddr[i], rds[i].data(), rds[i].size());
                        env.clone->mem.WriteBlock(riderAddr[i], riders[i].data(), riders[i].size());
                    }
                    AnimNativeStanceEvent(env, event, riderArg, 2, env.sp); // ported, witnessed (rows_anim.inc)
                    for (size_t i = 0; i < ents.size(); ++i) {
                        env.clone->mem.ReadBlock(entAddr[i], ents[i].data(), ents[i].size());
                        env.clone->mem.ReadBlock(rdAddr[i], rds[i].data(), rds[i].size());
                        env.clone->mem.ReadBlock(riderAddr[i], riders[i].data(), riders[i].size());
                    }
                }
            };

            std::vector<std::vector<uint8_t>> ents(count), rds(count), sts(count), riders(count);
            std::vector<uint32_t> entAddr(count), rdAddr(count), stAddr(count), riderAddr(count);
            std::vector<std::array<int32_t, 3>> pos(count);
            std::vector<std::array<int16_t, 3>> tangent(count);
            std::vector<rr::sim::AiDriveNode> nodes(count);
            std::vector<std::unique_ptr<rr::sim::GuestAiRoadQuery>> roads;
            roads.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                entAddr[i] = poolBase + 1096u * i;
                ents[i].assign(1096, 0);
                m.ReadBlock(entAddr[i], ents[i].data(), 1096);
                rdAddr[i] = m.PeekWord(entAddr[i] + rr::sim::ent::kRiderDef);
                stAddr[i] = m.PeekWord(entAddr[i] + rr::sim::ent::kStats);
                riderAddr[i] = m.PeekWord(entAddr[i] + rr::sim::ent::kOwner);
                rds[i].assign(72, 0);
                sts[i].assign(512, 0);
                riders[i].assign(640, 0);
                m.ReadBlock(rdAddr[i], rds[i].data(), 72);
                m.ReadBlock(stAddr[i], sts[i].data(), 512);
                m.ReadBlock(riderAddr[i], riders[i].data(), 640);
                const uint32_t slice = m.PeekWord(entAddr[i] + 0x154u);
                for (uint32_t k = 0; k < 3; ++k) {
                    pos[i][k] = GetS32(m, slice + 20u + 4u * k);
                    tangent[i][k] = GetS16(m, slice + 14u + 2u * k);
                }
                roads.push_back(std::make_unique<rr::sim::GuestAiRoadQuery>(guestRam, entAddr[i],
                                                                             ents[i].data(), aiDriveSp));
                nodes[i].entity = ents[i].data();
                nodes[i].riderDef = rds[i].data();
                nodes[i].stats = sts[i].data();
                nodes[i].slicePos = pos[i].data();
                nodes[i].sliceTangent = tangent[i].data();
                nodes[i].rider = riders[i].data();
                nodes[i].riderAddress = riderAddr[i];
                nodes[i].road = roads[i].get();
            }
            OracleStance stance(env, ents, rds, riders, entAddr, rdAddr, riderAddr);

            std::vector<std::vector<uint8_t>> blocks;
            blocks.reserve(16);
            auto load = [&](uint32_t addr, size_t n) -> const uint8_t* {
                blocks.emplace_back(n, static_cast<uint8_t>(0));
                if (addr != 0) m.ReadBlock(addr, blocks.back().data(), n);
                return blocks.back().data();
            };
            const uint32_t p1 = m.PeekWord(kPlayer1Ptr);
            const uint32_t p2 = m.PeekWord(kPlayer2Ptr);
            const uint32_t s0 = m.PeekWord(kPlayerArr + 0);
            const uint32_t s1 = m.PeekWord(kPlayerArr + 4);
            const uint32_t jb = m.PeekWord(kJailbreakPtr);

            rr::sim::AiDrivePassEnv pe;
            pe.gameState = gs.data();
            pe.raceBank = static_cast<int32_t>(m.PeekWord(m.PeekWord(kGameStatePtr) + 0x3Cu));
            pe.altKindTable = altKind.data();
            pe.fightRecords = fight.data();
            pe.attackerMask = mask.data();
            pe.stance = &stance;
            rr::sim::AiSpeedEnv& se = pe.speed;
            se.gameState = gs.data();
            se.player1.entity = load(p1, 1096);
            se.player1.riderDef = load(m.PeekWord(p1 + rr::sim::ent::kRiderDef), 72);
            se.player2.entity = load(p2, 1096);
            se.player2.riderDef = load(m.PeekWord(p2 + rr::sim::ent::kRiderDef), 72);
            se.playerSlot[0].entity = load(s0, 1096);
            se.playerSlot[0].riderDef = load(m.PeekWord(s0 + rr::sim::ent::kRiderDef), 72);
            se.playerSlot[1].entity = load(s1, 1096);
            se.playerSlot[1].riderDef = load(m.PeekWord(s1 + rr::sim::ent::kRiderDef), 72);
            se.jailbreakBike = (jb != 0) ? load(jb, 1096) : nullptr;
            se.liveBikes = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            se.bikeCap = static_cast<int32_t>(m.PeekWord(kBikeCap));
            se.spreadDiv = static_cast<int32_t>(m.PeekWord(kSpreadDiv));
            se.raceFlags = m.PeekWord(kAiRaceFlags);
            se.altFlag = m.PeekWord(kAltFlag);
            se.copScale = static_cast<int32_t>(m.PeekWord(kCopScale));
            int32_t clockStamp = static_cast<int32_t>(m.PeekWord(kClockStamp));
            int32_t spreadBand = static_cast<int32_t>(m.PeekWord(kSpreadBand));
            se.clockStamp = &clockStamp;
            se.spreadBand = &spreadBand;

            std::vector<std::vector<uint8_t>> tables;
            tables.reserve(16);
            auto table = [&](uint32_t addr, uint32_t bytes) -> const uint8_t* {
                tables.emplace_back(static_cast<size_t>(bytes), static_cast<uint8_t>(0));
                m.ReadBlock(addr, tables.back().data(), bytes);
                return tables.back().data();
            };
            auto t32 = [&](uint32_t addr) { return reinterpret_cast<const int32_t*>(table(addr, 256)); };
            auto t16 = [&](uint32_t addr) { return reinterpret_cast<const int16_t*>(table(addr, 256)); };
            se.tabSpeedClass = t32(kTabSpeedClass);
            se.tabThinkA = t32(kTabThinkA);
            se.tabThinkB = t32(kTabThinkB);
            se.tabCopFlat = t32(kTabCopFlat);
            se.tabAltA = t32(kTabAltA);
            se.tabAltB = t32(kTabAltB);
            se.tabSpeedCap = t32(kTabSpeedCap);
            se.tabCopBase = t16(kTabCopBase);
            se.tabCopStep = t16(kTabCopStep);
            se.tabCopMul = t32(kTabCopMul);
            std::vector<uint8_t> prof(2u * kProfileWindow);
            m.ReadBlock(kProfile - kProfileWindow, prof.data(), prof.size());
            se.profile = reinterpret_cast<const int8_t*>(prof.data() + kProfileWindow);

            uint32_t mb = m.PeekWord(args.a[2]);
            uint32_t mc = m.PeekWord(args.a[3]);
            const bool ok = rr::sim::AiDrivePass(nodes.data(), count,
                                                 static_cast<int32_t>(args.a[0]), args.a[1], &mb,
                                                 &mc, pe);
            RqDone(env, guestRam, 0u);
            if (!ok) return 0u; // the unported arm: leave the candidate wrong rather than guess
            for (uint32_t i = 0; i < count; ++i) {
                m.WriteBlock(entAddr[i], ents[i].data(), 1096);
                m.WriteBlock(rdAddr[i], rds[i].data(), 72);
                m.WriteBlock(stAddr[i], sts[i].data(), 512);
                m.WriteBlock(riderAddr[i], riders[i].data(), 640);
            }
            m.PokeWord(args.a[2], mb);
            m.PokeWord(args.a[3], mc);
            m.PokeWord(kClockStamp, static_cast<uint32_t>(clockStamp));
            m.PokeWord(kSpreadBand, static_cast<uint32_t>(spreadBand));
            return 0u;
        },
        /*oracleCallees=*/
        {OracleCallee{kAiStance, 3}}});

    // DIAGNOSTIC, not an acceptance row: it asserts that `SLUS 0x80017BA0` - the 3D sound emitter
    // the engine calls at 0x8007A6C0 - leaves no trace in the state this bench compares. If it
    // passes, the engine can be proven with that one call skipped; if it fails, it cannot, and the
    // failure names the first byte it changed. Excluded from the default run.
    // ================================================================= the race spine
    // The first native code in this project that is part of the race LOOP rather than of one
    // entity's model.

    rows.push_back(Row{
        "tick_countdown",
        "s32 TickCountdown(s32 dt)  [RASHCDG, the pre-race countdown and its GO]",
        kTickCountdown,
        [](CaseContext& c, Args& args) {
            const uint32_t b1 = c.scratch + 0u;
            const uint32_t r1 = c.scratch + 1152u;
            const uint32_t b2 = c.scratch + 1280u;
            const uint32_t r2 = c.scratch + 2432u;

            const Sample& src = c.sample ? *c.sample : *c.any;
            auto buildBike = [&](uint32_t addr, uint32_t riderAddr) {
                std::vector<uint8_t> e = src.entity;
                if (e.size() < 1096) e.resize(1096, 0);
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                // +0x2D0, the word the countdown pins to 0xFFFF0000 and releases to 0: start it
                // from noise so that both stores are visible in the diff.
                put32(0x2D0, c.rng->U32());
                put32(rr::sim::ent::kRiderDef, riderAddr);
                c.mem->WriteBlock(addr, e.data(), e.size());
                std::vector<uint8_t> rd(72, 0);
                for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(riderAddr, rd.data(), rd.size());
            };
            buildBike(b1, r1);
            buildBike(b2, r2);

            // The two gate bits of `riderDef+0x00`, driven so that all three arms of the function
            // are taken about equally often: no countdown at all, "arm it", and "count it down".
            uint8_t f1 = c.mem->PeekByte(r1);
            switch (c.rng->Next() % 3u) {
                case 0: f1 = static_cast<uint8_t>(f1 & ~0x40u); break;                 // return 1
                case 1: f1 = static_cast<uint8_t>(f1 | 0x60u); break;                  // arm
                default: f1 = static_cast<uint8_t>((f1 | 0x40u) & ~0x20u); break;      // tick down
            }
            c.mem->WriteBlock(r1, &f1, 1);

            // Half the cases are a two-player race, so every one of the three `*(0x8005B21C)`
            // guards is exercised on both sides.
            const bool twoPlayer = (c.rng->Next() & 1u) != 0;
            c.mem->PokeWord(kPlayer1Ptr, b1);
            c.mem->PokeWord(kPlayer2Ptr, twoPlayer ? b2 : 0u);

            const int32_t dt = c.sample ? 0x884 : (c.rng->S32() >> (c.rng->Next() % 12u));
            // The countdown word. Half the cases straddle `dt` within +-1024 units of 16.16, which
            // is what drives BOTH sides of the expiry test - the "GO" that calls 0x80090270 and the
            // ordinary frame that does not.
            int32_t cd;
            switch (c.rng->Next() % 4u) {
                case 0:
                case 1:
                    cd = static_cast<int32_t>(static_cast<uint32_t>(dt) +
                                              (c.rng->U32() % 2049u) - 1024u);
                    break;
                case 2: cd = c.rng->S32() >> (c.rng->Next() % 8u); break;
                default: cd = static_cast<int32_t>(c.rng->U32() % 0x40000u); break;
            }
            c.mem->PokeWord(kCountdownWord, static_cast<uint32_t>(cd));
            c.mem->PokeWord(kEventAcc, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 12u)));
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x10u, c.rng->U32());

            args.a[0] = static_cast<uint32_t>(dt);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t b1 = m.PeekWord(kPlayer1Ptr);
            const uint32_t b2 = m.PeekWord(kPlayer2Ptr);
            const uint32_t r1 = m.PeekWord(b1 + rr::sim::ent::kRiderDef);
            const uint32_t r2 = (b2 != 0) ? m.PeekWord(b2 + rr::sim::ent::kRiderDef) : 0;
            const uint32_t gs = m.PeekWord(kGameStatePtr);

            std::vector<uint8_t> e1(1096), e2(1096), rd1(72), rd2(72), state(64);
            m.ReadBlock(b1, e1.data(), e1.size());
            m.ReadBlock(r1, rd1.data(), rd1.size());
            if (b2 != 0) {
                m.ReadBlock(b2, e2.data(), e2.size());
                m.ReadBlock(r2, rd2.data(), rd2.size());
            }
            m.ReadBlock(gs, state.data(), state.size());
            int32_t countdown = static_cast<int32_t>(m.PeekWord(kCountdownWord));
            int32_t acc = static_cast<int32_t>(m.PeekWord(kEventAcc));

            // The seam. `RASHCDG 0x80090270` takes no arguments, so nothing is compared about the
            // call except that it happened, once, at this point in the function - which is exactly
            // the property the countdown has to get right.
            struct OracleGo final : rr::sim::RaceGoEvent {
                NativeEnv& env;
                std::function<void()> commit, reread;
                OracleGo(NativeEnv& v, std::function<void()> c, std::function<void()> r)
                    : env(v), commit(std::move(c)), reread(std::move(r)) {}
                void Go() override {
                    commit();
                    const uint32_t a[4] = {0, 0, 0, 0};
                    SpineNativeCall(env, kGoEvent, a, 0); // PORTED (spine.h RaceGo), witnessed
                    reread();
                }
            };
            auto commit = [&]() {
                m.WriteBlock(b1, e1.data(), e1.size());
                m.WriteBlock(r1, rd1.data(), rd1.size());
                if (b2 != 0) {
                    m.WriteBlock(b2, e2.data(), e2.size());
                    m.WriteBlock(r2, rd2.data(), rd2.size());
                }
                m.WriteBlock(gs, state.data(), state.size());
                m.PokeWord(kCountdownWord, static_cast<uint32_t>(countdown));
                m.PokeWord(kEventAcc, static_cast<uint32_t>(acc));
            };
            auto reread = [&]() {
                m.ReadBlock(b1, e1.data(), e1.size());
                m.ReadBlock(r1, rd1.data(), rd1.size());
                if (b2 != 0) {
                    m.ReadBlock(b2, e2.data(), e2.size());
                    m.ReadBlock(r2, rd2.data(), rd2.size());
                }
                m.ReadBlock(gs, state.data(), state.size());
                countdown = static_cast<int32_t>(m.PeekWord(kCountdownWord));
                acc = static_cast<int32_t>(m.PeekWord(kEventAcc));
            };
            OracleGo go(env, commit, reread);

            rr::sim::CountdownEnv ce;
            ce.player1 = e1.data();
            ce.rider1 = rd1.data();
            ce.player2 = (b2 != 0) ? e2.data() : nullptr;
            ce.rider2 = (b2 != 0) ? rd2.data() : nullptr;
            ce.raceClock = state.data() + 0x10;
            ce.countdown = &countdown;
            ce.eventAcc = &acc;
            ce.go = &go;

            const int32_t v0 =
                rr::sim::TickCountdown(static_cast<int32_t>(args.a[0]), ce);
            commit();
            return static_cast<uint32_t>(v0);
        },
        /*oracleCallees=*/{OracleCallee{kGoEvent, 0}},
        /*oracleFrame=*/kTickCountdownFrame});

    rows.push_back(Row{
        "race_tick",
        "void RaceTick(s32 dt)  [RASHCDG, the per-frame simulation step and its six children]",
        kRaceTick,
        [](CaseContext& c, Args& args) {
            const uint32_t b1 = c.scratch + 0u;
            const uint32_t r1 = c.scratch + 1152u;
            const uint32_t b2 = c.scratch + 1280u;
            const uint32_t r2 = c.scratch + 2432u;

            const Sample& src = c.sample ? *c.sample : *c.any;
            auto buildBike = [&](uint32_t addr, uint32_t riderAddr) {
                std::vector<uint8_t> e = src.entity;
                if (e.size() < 1096) e.resize(1096, 0);
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                put32(0x2D0, c.rng->U32());
                put32(rr::sim::ent::kRiderDef, riderAddr);
                c.mem->WriteBlock(addr, e.data(), e.size());
                std::vector<uint8_t> rd(72, 0);
                for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(riderAddr, rd.data(), rd.size());
            };
            buildBike(b1, r1);
            buildBike(b2, r2);

            uint8_t f1 = c.mem->PeekByte(r1);
            switch (c.rng->Next() % 3u) {
                case 0: f1 = static_cast<uint8_t>(f1 & ~0x40u); break;
                case 1: f1 = static_cast<uint8_t>(f1 | 0x60u); break;
                default: f1 = static_cast<uint8_t>((f1 | 0x40u) & ~0x20u); break;
            }
            c.mem->WriteBlock(r1, &f1, 1);
            const bool twoPlayer = (c.rng->Next() & 1u) != 0;
            c.mem->PokeWord(kPlayer1Ptr, b1);
            c.mem->PokeWord(kPlayer2Ptr, twoPlayer ? b2 : 0u);

            const int32_t dt = c.sample ? 0x884 : (c.rng->S32() >> (c.rng->Next() % 12u));
            int32_t cd;
            switch (c.rng->Next() % 4u) {
                case 0:
                case 1:
                    cd = static_cast<int32_t>(static_cast<uint32_t>(dt) +
                                              (c.rng->U32() % 2049u) - 1024u);
                    break;
                case 2: cd = c.rng->S32() >> (c.rng->Next() % 8u); break;
                default: cd = static_cast<int32_t>(c.rng->U32() % 0x40000u); break;
            }
            c.mem->PokeWord(kCountdownWord, static_cast<uint32_t>(cd));

            // The accumulator, straddling the 0x8000 threshold both before and after `dt` is added,
            // so that all four arms of the planner test are reached.
            int32_t acc;
            switch (c.rng->Next() % 4u) {
                case 0: acc = 0x8000 - dt + (c.rng->S32() >> 20); break;
                case 1: acc = 0x8000 + (c.rng->S32() >> 20); break;
                case 2: acc = static_cast<int32_t>(c.rng->U32() % 0x20000u); break;
                default: acc = c.rng->S32() >> (c.rng->Next() % 12u); break;
            }
            c.mem->PokeWord(kEventAcc, static_cast<uint32_t>(acc));
            // Zero often enough that the countdown half of the function is really entered; the
            // original stops calling TickCountdown for good once this is non-zero.
            c.mem->PokeWord(kPlanCount, (c.rng->Next() % 2u == 0) ? 0u : (c.rng->U32() % 64u));

            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x10u, c.rng->U32());
            // `gameState+0x03`, the one-shot "first race frame" byte: the only thing that lets the
            // chain run while the countdown holds. Read with `lb`, so its sign is irrelevant.
            {
                const uint8_t one = (c.rng->Next() % 3u == 0) ? static_cast<uint8_t>(c.rng->U32())
                                                              : 0u;
                c.mem->WriteBlock(gs + 3u, &one, 1);
                // `raceType`: bits 0 and 2 pick the arm of the planner table, and 0x2C is Jailbreak.
                const uint8_t rt = (c.rng->Next() % 4u == 0) ? 44u
                                                             : static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(gs + 4u, &rt, 1);
            }
            // BOUNDED INPUT FAMILY, named: `gameState+0x3C` indexes `SLUS 0x80052FAC` and the
            // original bounds it by nothing at all. The bench hands the port a 4 KiB window on
            // either side of the table and keeps the bank inside 0..63 so that the original's own
            // index stays inside it. A violated bound would produce a mismatch, never a silent pass.
            c.mem->PokeWord(gs + 0x3Cu, c.rng->U32() % 64u);

            args.a[0] = static_cast<uint32_t>(dt);
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t b1 = m.PeekWord(kPlayer1Ptr);
            const uint32_t b2 = m.PeekWord(kPlayer2Ptr);
            const uint32_t r1 = m.PeekWord(b1 + rr::sim::ent::kRiderDef);
            const uint32_t r2 = (b2 != 0) ? m.PeekWord(b2 + rr::sim::ent::kRiderDef) : 0;
            const uint32_t gs = m.PeekWord(kGameStatePtr);

            std::vector<uint8_t> e1(1096), e2(1096), rd1(72), rd2(72), state(64);
            int32_t countdown = 0, acc = 0, planCount = 0;
            auto reread = [&]() {
                m.ReadBlock(b1, e1.data(), e1.size());
                m.ReadBlock(r1, rd1.data(), rd1.size());
                if (b2 != 0) {
                    m.ReadBlock(b2, e2.data(), e2.size());
                    m.ReadBlock(r2, rd2.data(), rd2.size());
                }
                m.ReadBlock(gs, state.data(), state.size());
                countdown = static_cast<int32_t>(m.PeekWord(kCountdownWord));
                acc = static_cast<int32_t>(m.PeekWord(kEventAcc));
                planCount = static_cast<int32_t>(m.PeekWord(kPlanCount));
            };
            auto commit = [&]() {
                m.WriteBlock(b1, e1.data(), e1.size());
                m.WriteBlock(r1, rd1.data(), rd1.size());
                if (b2 != 0) {
                    m.WriteBlock(b2, e2.data(), e2.size());
                    m.WriteBlock(r2, rd2.data(), rd2.size());
                }
                m.WriteBlock(gs, state.data(), state.size());
                m.PokeWord(kCountdownWord, static_cast<uint32_t>(countdown));
                m.PokeWord(kEventAcc, static_cast<uint32_t>(acc));
                m.PokeWord(kPlanCount, static_cast<uint32_t>(planCount));
            };
            reread();

            // Every one of the eight callees is the ORIGINAL machine code, run on the candidate's
            // own clone at the depth the original ran it at, with the argument our code computed.
            // Everything the port is holding locally is written back before each call and picked up
            // again afterwards, because between them these eight rewrite the whole world.
            struct OracleChain final : rr::sim::RaceTickChildren, rr::sim::RaceGoEvent {
                NativeEnv& env;
                std::function<void()> commit, reread;
                OracleChain(NativeEnv& v, std::function<void()> c, std::function<void()> r)
                    : env(v), commit(std::move(c)), reread(std::move(r)) {}
                void One(uint32_t address, int32_t a0, int n) {
                    commit();
                    const uint32_t a[4] = {static_cast<uint32_t>(a0), 0, 0, 0};
                    env.CallOracleArgs(address, a, n);
                    reread();
                }
                void Go() override { // PORTED (spine.h RaceGo), run natively and witnessed
                    commit();
                    const uint32_t a[4] = {0, 0, 0, 0};
                    SpineNativeCall(env, kGoEvent, a, 0);
                    reread();
                }
                void AiPlan(int32_t arg) override { One(kAiPlan, arg, 1); }
                void RaceDirector(int32_t dt) override { One(kRaceDirector, dt, 1); }
                void SpawnerPass(int32_t dt) override { One(kSpawnerPass, dt, 1); }
                void WorldBikePass(int32_t dt) override { One(kWorldBikePass, dt, 1); }
                void CollisionPass(int32_t dt) override { One(kCollisionPass, dt, 1); }
                void RiderEnginePass(int32_t dt) override { One(kRiderPass, dt, 1); }
                void PresentationPass(int32_t dt) override { One(kPresentPass, dt, 1); }
            };
            OracleChain chain(env, commit, reread);

            std::vector<uint8_t> tab(2u * kPlanWindow);
            m.ReadBlock(kPlanTable - kPlanWindow, tab.data(), tab.size());

            rr::sim::RaceTickEnv te;
            te.eventAcc = &acc;
            te.planCount = &planCount;
            te.gameState = state.data();
            te.planTable = reinterpret_cast<const int32_t*>(tab.data() + kPlanWindow);
            te.children = &chain;
            te.countdown.player1 = e1.data();
            te.countdown.rider1 = rd1.data();
            te.countdown.player2 = (b2 != 0) ? e2.data() : nullptr;
            te.countdown.rider2 = (b2 != 0) ? rd2.data() : nullptr;
            te.countdown.raceClock = state.data() + 0x10;
            te.countdown.countdown = &countdown;
            te.countdown.eventAcc = &acc;
            te.countdown.go = &chain;

            rr::sim::RaceTick(static_cast<int32_t>(args.a[0]), te);
            commit();
            return 0u;
        },
        /*oracleCallees=*/
        {OracleCallee{kGoEvent, 0, 0, {}, kTickCountdownFrame}, OracleCallee{kAiPlan, 1},
         OracleCallee{kRaceDirector, 1}, OracleCallee{kSpawnerPass, 1},
         OracleCallee{kWorldBikePass, 1}, OracleCallee{kCollisionPass, 1},
         OracleCallee{kRiderPass, 1}, OracleCallee{kPresentPass, 1}},
        /*oracleFrame=*/kRaceTickFrame,
        /*maxSteps=*/200'000'000});

    rows.push_back(Row{
        "race_step",
        "void RaceStep(void)  [SLUS, dt = 218 * ticks, the tick and the per-player camera]",
        kRaceStep,
        [](CaseContext& c, Args& args) {
            (void)args; // RaceStep takes no o32 arguments
            const uint32_t b1 = c.scratch + 0u;
            const uint32_t r1 = c.scratch + 1152u;
            const uint32_t b2 = c.scratch + 1280u;
            const uint32_t r2 = c.scratch + 2432u;

            const Sample& src = c.sample ? *c.sample : *c.any;
            auto buildBike = [&](uint32_t addr, uint32_t riderAddr) {
                std::vector<uint8_t> e = src.entity;
                if (e.size() < 1096) e.resize(1096, 0);
                auto put32 = [&e](uint32_t off, uint32_t v) {
                    e[off] = static_cast<uint8_t>(v);
                    e[off + 1] = static_cast<uint8_t>(v >> 8);
                    e[off + 2] = static_cast<uint8_t>(v >> 16);
                    e[off + 3] = static_cast<uint8_t>(v >> 24);
                };
                put32(0x2D0, c.rng->U32());
                put32(rr::sim::ent::kRiderDef, riderAddr);
                c.mem->WriteBlock(addr, e.data(), e.size());
                std::vector<uint8_t> rd(72, 0);
                for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(riderAddr, rd.data(), rd.size());
            };
            buildBike(b1, r1);
            buildBike(b2, r2);

            uint8_t f1 = c.mem->PeekByte(r1);
            switch (c.rng->Next() % 3u) {
                case 0: f1 = static_cast<uint8_t>(f1 & ~0x40u); break;
                case 1: f1 = static_cast<uint8_t>(f1 | 0x60u); break;
                default: f1 = static_cast<uint8_t>((f1 | 0x40u) & ~0x20u); break;
            }
            c.mem->WriteBlock(r1, &f1, 1);
            c.mem->PokeWord(kPlayer1Ptr, b1);
            c.mem->PokeWord(kPlayer2Ptr, (c.rng->Next() & 1u) ? b2 : 0u);

            // The frame delta, `game_state+0x18`, in 1/300 s units. Driven over the clamp at 31 and
            // over zero, because both are branches of this function.
            int32_t ticks;
            switch (c.rng->Next() % 5u) {
                case 0: ticks = 0; break;
                case 1: ticks = 10; break; // the 30 Hz frame the live trace measured
                case 2: ticks = static_cast<int32_t>(c.rng->U32() % 64u); break;
                case 3: ticks = -static_cast<int32_t>(c.rng->U32() % 64u); break;
                default: ticks = c.rng->S32() >> (c.rng->Next() % 20u); break;
            }
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x18u, static_cast<uint32_t>(ticks));
            c.mem->PokeWord(gs + 0x1Cu, c.rng->U32());
            c.mem->PokeWord(gs + 0x10u, c.rng->U32());
            c.mem->PokeWord(kFrameFlag, c.rng->U32());

            const int32_t dt = 218 * ticks;
            int32_t cd;
            switch (c.rng->Next() % 4u) {
                case 0:
                case 1:
                    cd = static_cast<int32_t>(static_cast<uint32_t>(dt) +
                                              (c.rng->U32() % 2049u) - 1024u);
                    break;
                case 2: cd = c.rng->S32() >> (c.rng->Next() % 8u); break;
                default: cd = static_cast<int32_t>(c.rng->U32() % 0x40000u); break;
            }
            c.mem->PokeWord(kCountdownWord, static_cast<uint32_t>(cd));
            int32_t acc;
            switch (c.rng->Next() % 4u) {
                case 0: acc = 0x8000 - dt + (c.rng->S32() >> 20); break;
                case 1: acc = 0x8000 + (c.rng->S32() >> 20); break;
                case 2: acc = static_cast<int32_t>(c.rng->U32() % 0x20000u); break;
                default: acc = c.rng->S32() >> (c.rng->Next() % 12u); break;
            }
            c.mem->PokeWord(kEventAcc, static_cast<uint32_t>(acc));
            c.mem->PokeWord(kPlanCount, (c.rng->Next() % 2u == 0) ? 0u : (c.rng->U32() % 64u));
            {
                const uint8_t one = (c.rng->Next() % 3u == 0) ? static_cast<uint8_t>(c.rng->U32())
                                                              : 0u;
                c.mem->WriteBlock(gs + 3u, &one, 1);
                const uint8_t rt = (c.rng->Next() % 4u == 0) ? 44u
                                                             : static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(gs + 4u, &rt, 1);
            }
            c.mem->PokeWord(gs + 0x3Cu, c.rng->U32() % 64u);

            // BOUNDED INPUT FAMILY, named, and this one is sharp. `game_state+0x30` is the player
            // count; the original indexes the two-entry view array at 0x800CD898 with it, and the
            // race director it reaches through the tick indexes `*(0x8005B268)` with it as well.
            // This snapshot is a ONE-PLAYER race - `0x8005B26C` is null and view 1 is all zeroes -
            // so a count of 2 does not describe a machine that ever existed: the director
            // dereferences the null second player and the original faults. Measured, not assumed:
            // with 0..2 the guest run itself trapped on 243 of 1160 cases with a misaligned load at
            // `RASHCDG 0x800B94B4`. The row therefore drives 0 and 1, and the two-player half of
            // the loop is left to a capture that has two players, which does not exist yet. The p2
            // arms of `TickCountdown` itself ARE
            // exercised, by `tick_countdown` and `race_tick`, which do not go through the director.
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 2u);
            // `view+0x224` bit 0x100 gates the post-update arm. The snapshot has it SET on view 0
            // and clear on view 1; a third of the cases flip view 0's, so both sides of the test
            // are reached on a live camera record as well as on the empty one.
            if (c.rng->Next() % 3u == 0) {
                const uint32_t a = kViewArray + 0x224u;
                c.mem->PokeWord(a, c.mem->PeekWord(a) ^ 0x100u);
            }
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            (void)args; // RaceStep takes none - everything it needs is in game_state
            Memory& m = env.clone->mem;
            const uint32_t b1 = m.PeekWord(kPlayer1Ptr);
            const uint32_t b2 = m.PeekWord(kPlayer2Ptr);
            const uint32_t r1 = m.PeekWord(b1 + rr::sim::ent::kRiderDef);
            const uint32_t r2 = (b2 != 0) ? m.PeekWord(b2 + rr::sim::ent::kRiderDef) : 0;
            const uint32_t gs = m.PeekWord(kGameStatePtr);

            std::vector<uint8_t> e1(1096), e2(1096), rd1(72), rd2(72), state(64),
                view(kViewStride);
            int32_t countdown = 0, acc = 0, planCount = 0, frameFlag = 0;
            int32_t viewIndex = -1;
            auto reread = [&]() {
                m.ReadBlock(b1, e1.data(), e1.size());
                m.ReadBlock(r1, rd1.data(), rd1.size());
                if (b2 != 0) {
                    m.ReadBlock(b2, e2.data(), e2.size());
                    m.ReadBlock(r2, rd2.data(), rd2.size());
                }
                m.ReadBlock(gs, state.data(), state.size());
                countdown = static_cast<int32_t>(m.PeekWord(kCountdownWord));
                acc = static_cast<int32_t>(m.PeekWord(kEventAcc));
                planCount = static_cast<int32_t>(m.PeekWord(kPlanCount));
                frameFlag = static_cast<int32_t>(m.PeekWord(kFrameFlag));
                if (viewIndex >= 0)
                    m.ReadBlock(kViewArray + kViewStride * static_cast<uint32_t>(viewIndex),
                                view.data(), view.size());
            };
            auto commit = [&]() {
                m.WriteBlock(b1, e1.data(), e1.size());
                m.WriteBlock(r1, rd1.data(), rd1.size());
                if (b2 != 0) {
                    m.WriteBlock(b2, e2.data(), e2.size());
                    m.WriteBlock(r2, rd2.data(), rd2.size());
                }
                m.WriteBlock(gs, state.data(), state.size());
                m.PokeWord(kCountdownWord, static_cast<uint32_t>(countdown));
                m.PokeWord(kEventAcc, static_cast<uint32_t>(acc));
                m.PokeWord(kPlanCount, static_cast<uint32_t>(planCount));
                m.PokeWord(kFrameFlag, static_cast<uint32_t>(frameFlag));
                if (viewIndex >= 0)
                    m.WriteBlock(kViewArray + kViewStride * static_cast<uint32_t>(viewIndex),
                                 view.data(), view.size());
            };
            reread();

            struct OracleChain final : rr::sim::RaceTickChildren,
                                       rr::sim::RaceGoEvent,
                                       rr::sim::RaceStepCamera {
                NativeEnv& env;
                Memory& m;
                std::vector<uint8_t>& view;
                int32_t& viewIndex;
                std::function<void()> commit, reread;
                OracleChain(NativeEnv& v, Memory& mm, std::vector<uint8_t>& vw, int32_t& vi,
                            std::function<void()> c, std::function<void()> r)
                    : env(v), m(mm), view(vw), viewIndex(vi), commit(std::move(c)),
                      reread(std::move(r)) {}
                void Two(uint32_t address, uint32_t a0, uint32_t a1, int n) {
                    commit();
                    const uint32_t a[4] = {a0, a1, 0, 0};
                    last = env.CallOracleArgs(address, a, n);
                    reread();
                }
                uint32_t last = 0;
                void Go() override { // PORTED (spine.h RaceGo), run natively and witnessed
                    commit();
                    const uint32_t a[4] = {0, 0, 0, 0};
                    SpineNativeCall(env, kGoEvent, a, 0);
                    reread();
                }
                void AiPlan(int32_t arg) override {
                    Two(kAiPlan, static_cast<uint32_t>(arg), 0, 1);
                }
                void RaceDirector(int32_t dt) override {
                    Two(kRaceDirector, static_cast<uint32_t>(dt), 0, 1);
                }
                void SpawnerPass(int32_t dt) override {
                    Two(kSpawnerPass, static_cast<uint32_t>(dt), 0, 1);
                }
                void WorldBikePass(int32_t dt) override {
                    Two(kWorldBikePass, static_cast<uint32_t>(dt), 0, 1);
                }
                void CollisionPass(int32_t dt) override {
                    Two(kCollisionPass, static_cast<uint32_t>(dt), 0, 1);
                }
                void RiderEnginePass(int32_t dt) override {
                    Two(kRiderPass, static_cast<uint32_t>(dt), 0, 1);
                }
                void PresentationPass(int32_t dt) override {
                    Two(kPresentPass, static_cast<uint32_t>(dt), 0, 1);
                }
                uint32_t Address(int32_t i) const {
                    return kViewArray + kViewStride * static_cast<uint32_t>(i);
                }
                uint8_t* View(int32_t i) override {
                    viewIndex = i;
                    m.ReadBlock(Address(i), view.data(), view.size());
                    return view.data();
                }
                void Update(int32_t i, int32_t dt) override {
                    viewIndex = i;
                    Two(kViewUpdate, Address(i), static_cast<uint32_t>(dt), 2);
                }
                int32_t Test(int32_t i) override {
                    viewIndex = i;
                    Two(kViewPostTest, Address(i), 0, 1);
                    return static_cast<int32_t>(last);
                }
                void Apply(int32_t i) override {
                    viewIndex = i;
                    Two(kViewPostApply, Address(i), 0, 1);
                }
            };
            OracleChain chain(env, m, view, viewIndex, commit, reread);

            std::vector<uint8_t> tab(2u * kPlanWindow);
            m.ReadBlock(kPlanTable - kPlanWindow, tab.data(), tab.size());

            rr::sim::RaceStepEnv se;
            se.gameState = state.data();
            se.frameFlag = &frameFlag;
            se.camera = &chain;
            rr::sim::RaceTickEnv& te = se.tick;
            te.eventAcc = &acc;
            te.planCount = &planCount;
            te.gameState = state.data();
            te.planTable = reinterpret_cast<const int32_t*>(tab.data() + kPlanWindow);
            te.children = &chain;
            te.countdown.player1 = e1.data();
            te.countdown.rider1 = rd1.data();
            te.countdown.player2 = (b2 != 0) ? e2.data() : nullptr;
            te.countdown.rider2 = (b2 != 0) ? rd2.data() : nullptr;
            te.countdown.raceClock = state.data() + 0x10;
            te.countdown.countdown = &countdown;
            te.countdown.eventAcc = &acc;
            te.countdown.go = &chain;

            rr::sim::RaceStep(se);
            commit();
            return 0u;
        },
        /*oracleCallees=*/
        // The six children and the planner are reached through the PORTED `RaceTick`, so they sit
        // 32 bytes deeper than `RaceStep`'s own frame, and the "GO" event another 24 below that
        // inside the ported `TickCountdown`. The camera calls are made from `RaceStep` itself.
        {OracleCallee{kGoEvent, 0, 0, {}, kRaceTickFrame + kTickCountdownFrame},
         OracleCallee{kAiPlan, 1, 0, {}, kRaceTickFrame},
         OracleCallee{kRaceDirector, 1, 0, {}, kRaceTickFrame},
         OracleCallee{kSpawnerPass, 1, 0, {}, kRaceTickFrame},
         OracleCallee{kWorldBikePass, 1, 0, {}, kRaceTickFrame},
         OracleCallee{kCollisionPass, 1, 0, {}, kRaceTickFrame},
         OracleCallee{kRiderPass, 1, 0, {}, kRaceTickFrame},
         OracleCallee{kPresentPass, 1, 0, {}, kRaceTickFrame},
         OracleCallee{kViewUpdate, 2}, OracleCallee{kViewPostTest, 1},
         OracleCallee{kViewPostApply, 1}},
        /*oracleFrame=*/kRaceStepFrame,
        /*maxSteps=*/200'000'000});

    rows.push_back(Row{
        "progress_pass",
        "void ProgressPass(u32 *skip)  [RASHCDG, the per-bike finish test and the AI skip mask]",
        kProgressPass,
        [](CaseContext& c, Args& args) {
            // BOUNDED INPUT FAMILY, named: this pass walks the machine's OWN pool (base
            // `*(0x8005B3A0)`, stride 1096) and hands each bike to the 2476-byte finish test, which
            // reaches the rider record, the road and the race globals. So, exactly as `ai_drive`
            // does, the row edits the live pool-0 slots IN PLACE instead of building bikes in the
            // scratch block, and only the fields this function itself branches on are randomised.
            const uint32_t poolBase = c.mem->PeekWord(kPoolBasePtr);
            int32_t live = static_cast<int32_t>(c.mem->PeekWord(kLiveBikes));
            if (live < 0 || live > 24) live = 0;
            for (int32_t i = 0; i < live; ++i) {
                const uint32_t ea = poolBase + 1096u * static_cast<uint32_t>(i);
                const uint32_t rd = c.mem->PeekWord(ea + rr::sim::ent::kRiderDef);
                const uint32_t ow = c.mem->PeekWord(ea + rr::sim::ent::kOwner);
                // `+0x3A0` bit 0x10 - whether a police bike stays in the pass at all.
                uint8_t rf = c.mem->PeekByte(ea + rr::sim::ent::kRaceFlags);
                rf = static_cast<uint8_t>((rf & ~0x50u) | (c.rng->U32() & 0x50u));
                c.mem->WriteBlock(ea + rr::sim::ent::kRaceFlags, &rf, 1);
                // `+0x140`, the "this slot is live" halfword of the shared entity header.
                WriteU16Bench(*c.mem, ea + rr::sim::ent::kLiveState,
                              static_cast<uint16_t>((c.rng->Next() % 3u == 0) ? 0u
                                                                             : (c.rng->U32() & 3u)));
                if (rd >= 0x80000000u) {
                    // The rank class (low nibble of `riderDef+0x01`), the result code and the
                    // finish time: 2 is police, 248.. is a result code, and 254/255 are the two the
                    // pass treats specially.
                    uint8_t k = c.mem->PeekByte(rd + 1);
                    k = static_cast<uint8_t>((k & 0xF0u) | (c.rng->U32() % 4u));
                    c.mem->WriteBlock(rd + 1, &k, 1);
                    uint8_t place;
                    switch (c.rng->Next() % 4u) {
                        case 0: place = static_cast<uint8_t>(c.rng->U32() % 20u); break;
                        case 1: place = static_cast<uint8_t>(246u + c.rng->U32() % 10u); break;
                        case 2: place = 255u; break;
                        default: place = static_cast<uint8_t>(c.rng->U32()); break;
                    }
                    c.mem->WriteBlock(rd + 0x27u, &place, 1);
                    c.mem->PokeWord(rd + 0x28u,
                                    (c.rng->Next() % 3u == 0) ? 0u : (c.rng->U32() % 0x10000u));
                }
                if (ow >= 0x80000000u) {
                    c.mem->PokeWord(ow + 0x228u, c.rng->U32());
                    c.mem->PokeWord(ow + 0x25Cu, (c.rng->Next() & 1u) ? (c.rng->U32() % 6u)
                                                                      : c.rng->U32());
                    WriteU16Bench(*c.mem, ow + rr::sim::ent::kLiveState,
                                  static_cast<uint16_t>(c.rng->U32() & 3u));
                }
            }
            // The count is re-read on every iteration, so it is driven below the real 18 as well.
            c.mem->PokeWord(kLiveBikes,
                            (c.rng->Next() % 4u == 0)
                                ? (1u + c.rng->U32() % static_cast<uint32_t>(live > 0 ? live : 1))
                                : static_cast<uint32_t>(live));
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            // The player count the handle is compared against. The pool's real handles are 0..17,
            // so 0..2 puts them on both sides of the test.
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 3u);

            const uint32_t skip = c.scratch + 0u;
            c.mem->PokeWord(skip, c.rng->U32());
            args.a[0] = skip;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t poolBase = m.PeekWord(kPoolBasePtr);
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            int32_t declared = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            if (declared < 0 || declared > 24) declared = 0;

            struct Slot {
                uint32_t ea = 0, rd = 0, ow = 0;
                std::vector<uint8_t> entity, riderDef, owner;
            };
            std::vector<Slot> slots(static_cast<size_t>(declared));
            std::vector<uint8_t> state(64);
            int32_t live = 0;
            auto reread = [&]() {
                for (Slot& s : slots) {
                    m.ReadBlock(s.ea, s.entity.data(), s.entity.size());
                    if (s.rd != 0) m.ReadBlock(s.rd, s.riderDef.data(), s.riderDef.size());
                    if (s.ow != 0) m.ReadBlock(s.ow, s.owner.data(), s.owner.size());
                }
                m.ReadBlock(gs, state.data(), state.size());
                live = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            };
            auto commit = [&]() {
                for (Slot& s : slots) {
                    m.WriteBlock(s.ea, s.entity.data(), s.entity.size());
                    if (s.rd != 0) m.WriteBlock(s.rd, s.riderDef.data(), s.riderDef.size());
                    if (s.ow != 0) m.WriteBlock(s.ow, s.owner.data(), s.owner.size());
                }
                m.WriteBlock(gs, state.data(), state.size());
                m.PokeWord(kLiveBikes, static_cast<uint32_t>(live));
            };
            for (size_t i = 0; i < slots.size(); ++i) {
                Slot& s = slots[i];
                s.ea = poolBase + 1096u * static_cast<uint32_t>(i);
                s.rd = m.PeekWord(s.ea + rr::sim::ent::kRiderDef);
                s.ow = m.PeekWord(s.ea + rr::sim::ent::kOwner);
                if (s.rd < 0x80000000u) s.rd = 0;
                if (s.ow < 0x80000000u) s.ow = 0;
                s.entity.assign(1096, 0);
                s.riderDef.assign(72, 0);
                s.owner.assign(kRiderStride, 0);
            }
            reread();

            struct OracleFinish final : rr::sim::RaceFinishTest {
                NativeEnv& env;
                std::vector<Slot>& slots;
                std::function<void()> commit, reread;
                OracleFinish(NativeEnv& v, std::vector<Slot>& s, std::function<void()> c,
                             std::function<void()> r)
                    : env(v), slots(s), commit(std::move(c)), reread(std::move(r)) {}
                int32_t Test(int32_t index) override {
                    commit();
                    const uint32_t a[4] = {slots[static_cast<size_t>(index)].ea, 0, 0, 0};
                    // PORTED (race.h FinishTest, the finish_test row's harness), witnessed
                    const uint32_t v0 = SpineNativeCall(env, kFinishTest, a, 1);
                    reread();
                    return static_cast<int32_t>(v0);
                }
            } finish(env, slots, commit, reread);

            std::vector<rr::sim::ProgressNode> nodes(slots.size());
            for (size_t i = 0; i < slots.size(); ++i) {
                nodes[i].entity = slots[i].entity.data();
                nodes[i].riderDef = slots[i].riderDef.data();
                nodes[i].owner = slots[i].owner.data();
            }

            rr::sim::ProgressPassEnv pe;
            pe.gameState = state.data();
            pe.liveBikes = &live;
            pe.bikes = nodes.empty() ? nullptr : nodes.data();
            pe.count = static_cast<int32_t>(nodes.size());
            pe.finish = &finish;

            uint32_t skip = m.PeekWord(args.a[0]);
            if (!rr::sim::ProgressPass(&skip, pe)) {
                // The loop reached a bike the bench did not resolve. Leave the candidate wrong
                // rather than guess, so the row fails loudly if the family ever gets there.
                return 0u;
            }
            commit();
            m.PokeWord(args.a[0], skip);
            return 0u;
        },
        /*oracleCallees=*/SpineFinishSeams(0), // the finish test, now native (rows_spine.inc)
        /*oracleFrame=*/kProgressFrame,
        /*maxSteps=*/200'000'000});

    rows.push_back(Row{
        "race_director",
        "void RaceDirector(s32 dt)  [RASHCDG, the end-of-race test and the three AI passes]",
        kRaceDirector,
        [](CaseContext& c, Args& args) {
            // Same bounded family as `progress_pass` - the director walks the machine's own pool
            // through it - plus the globals the end-of-race test lives in.
            const uint32_t poolBase = c.mem->PeekWord(kPoolBasePtr);
            int32_t live = static_cast<int32_t>(c.mem->PeekWord(kLiveBikes));
            if (live < 0 || live > 24) live = 0;
            for (int32_t i = 0; i < live; ++i) {
                const uint32_t ea = poolBase + 1096u * static_cast<uint32_t>(i);
                const uint32_t rd = c.mem->PeekWord(ea + rr::sim::ent::kRiderDef);
                const uint32_t ow = c.mem->PeekWord(ea + rr::sim::ent::kOwner);
                uint8_t rf = c.mem->PeekByte(ea + rr::sim::ent::kRaceFlags);
                rf = static_cast<uint8_t>((rf & ~0x50u) | (c.rng->U32() & 0x50u));
                c.mem->WriteBlock(ea + rr::sim::ent::kRaceFlags, &rf, 1);
                WriteU16Bench(*c.mem, ea + rr::sim::ent::kLiveState,
                              static_cast<uint16_t>((c.rng->Next() % 3u == 0) ? 0u
                                                                             : (c.rng->U32() & 3u)));
                // The speed: zero is one of the remount arm's gates.
                c.mem->PokeWord(ea + rr::sim::ent::kSpeed,
                                (c.rng->Next() % 3u == 0) ? 0u
                                                          : static_cast<uint32_t>(c.rng->S32() >> 9));
                if (rd >= 0x80000000u) {
                    uint8_t k = c.mem->PeekByte(rd + 1);
                    k = static_cast<uint8_t>((k & 0xF0u) | (c.rng->U32() % 4u));
                    c.mem->WriteBlock(rd + 1, &k, 1);
                    uint8_t place;
                    switch (c.rng->Next() % 4u) {
                        case 0: place = static_cast<uint8_t>(c.rng->U32() % 20u); break;
                        case 1: place = static_cast<uint8_t>(246u + c.rng->U32() % 10u); break;
                        case 2: place = 255u; break;
                        default: place = static_cast<uint8_t>(c.rng->U32()); break;
                    }
                    c.mem->WriteBlock(rd + 0x27u, &place, 1);
                    c.mem->PokeWord(rd + 0x28u,
                                    (c.rng->Next() % 3u == 0) ? 0u : (c.rng->U32() % 0x10000u));
                }
                if (ow >= 0x80000000u) {
                    c.mem->PokeWord(ow + 0x228u, c.rng->U32());
                    c.mem->PokeWord(ow + 0x25Cu, (c.rng->Next() & 1u) ? (c.rng->U32() % 6u)
                                                                      : c.rng->U32());
                    WriteU16Bench(*c.mem, ow + rr::sim::ent::kLiveState,
                                  static_cast<uint16_t>(c.rng->U32() & 3u));
                    // BOUNDED INPUT FAMILY, named, and it was measured rather than assumed. Bit
                    // 0x10 of `owner[+0x23C]` opens the remount arm, and that arm dereferences
                    // `entity[+0x358]` - the SECOND RIDER of a two-rider bike - with no null guard
                    // (0x800B94A4). Every bike of this snapshot has `+0x358 == 0`, so setting the
                    // bit makes the ORIGINAL fault, not the port: with it randomised the guest run
                    // trapped on 243 of 1160 cases with a misaligned load at 0x800B94B4. The row
                    // therefore clears it, and the two-rider arms of this function - the remount,
                    // `0x800C035C` and `0x80092E04` - are NOT exercised by it. They need a capture
                    // with a two-rider bike, which the project does not have (the same gap as the
                    // two-player paths).
                    uint8_t g = c.mem->PeekByte(ow + 0x23Cu);
                    g = static_cast<uint8_t>(g & ~0x10u);
                    c.mem->WriteBlock(ow + 0x23Cu, &g, 1);
                }
            }
            c.mem->PokeWord(kLiveBikes,
                            (c.rng->Next() % 4u == 0)
                                ? (1u + c.rng->U32() % static_cast<uint32_t>(live > 0 ? live : 1))
                                : static_cast<uint32_t>(live));

            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            // BOUNDED INPUT FAMILY, named: one-player and no-player races only, for the reason
            // `race_step` states - `*(0x8005B26C)` is null in this snapshot, so a count of 2 makes
            // the director dereference a player that does not exist.
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 2u);
            {
                const uint8_t st = static_cast<uint8_t>(c.rng->U32() % 8u);
                c.mem->WriteBlock(gs + 0u, &st, 1); // the state byte this function WRITES
                const uint8_t phase = static_cast<uint8_t>(c.rng->U32() % 4u);
                c.mem->WriteBlock(gs + 0x39u, &phase, 1);
            }

            const int32_t dt = c.sample ? 0x884 : (c.rng->S32() >> (c.rng->Next() % 12u));
            // The post-race accumulator and its 5.00 s limit, driven so that the wait both expires
            // and does not: this is the branch that ends a race.
            const int32_t limit = (c.rng->Next() % 3u == 0)
                                      ? 0x50000
                                      : static_cast<int32_t>(c.rng->U32() % 0x100000u);
            c.mem->PokeWord(kPostLimit, static_cast<uint32_t>(limit));
            int32_t delay;
            switch (c.rng->Next() % 4u) {
                case 0: delay = limit - dt + (c.rng->S32() >> 22); break;
                case 1: delay = -static_cast<int32_t>(c.rng->U32() % 0x1000u); break;
                case 2: delay = static_cast<int32_t>(c.rng->U32() % 0x100000u); break;
                default: delay = c.rng->S32() >> (c.rng->Next() % 12u); break;
            }
            c.mem->PokeWord(kCountdownWord, static_cast<uint32_t>(delay));
            c.mem->PokeWord(kSkipResults, (c.rng->Next() % 3u == 0) ? c.rng->U32() : 0u);
            // `view+0x228` bit 0x8 skips the wait entirely.
            {
                const uint32_t a = kViewArray + 0x228u;
                uint32_t v = c.mem->PeekWord(a);
                v = (v & ~0x8u) | (c.rng->U32() & 0x8u);
                c.mem->PokeWord(a, v);
            }
            args.a[0] = static_cast<uint32_t>(dt);
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t poolBase = m.PeekWord(kPoolBasePtr);
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            int32_t declared = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            if (declared < 0 || declared > 24) declared = 0;

            struct Slot {
                uint32_t ea = 0, rd = 0, ow = 0;
                std::vector<uint8_t> entity, riderDef, owner;
            };
            std::vector<Slot> slots(static_cast<size_t>(declared));
            std::vector<uint8_t> state(64), view(kViewStride);
            int32_t live = 0, postDelay = 0, postLimit = 0, skipResults = 0;
            auto reread = [&]() {
                for (Slot& s : slots) {
                    m.ReadBlock(s.ea, s.entity.data(), s.entity.size());
                    if (s.rd != 0) m.ReadBlock(s.rd, s.riderDef.data(), s.riderDef.size());
                    if (s.ow != 0) m.ReadBlock(s.ow, s.owner.data(), s.owner.size());
                }
                m.ReadBlock(gs, state.data(), state.size());
                m.ReadBlock(kViewArray, view.data(), view.size());
                live = static_cast<int32_t>(m.PeekWord(kLiveBikes));
                postDelay = static_cast<int32_t>(m.PeekWord(kCountdownWord));
                postLimit = static_cast<int32_t>(m.PeekWord(kPostLimit));
                skipResults = static_cast<int32_t>(m.PeekWord(kSkipResults));
            };
            auto commit = [&]() {
                for (Slot& s : slots) {
                    m.WriteBlock(s.ea, s.entity.data(), s.entity.size());
                    if (s.rd != 0) m.WriteBlock(s.rd, s.riderDef.data(), s.riderDef.size());
                    if (s.ow != 0) m.WriteBlock(s.ow, s.owner.data(), s.owner.size());
                }
                m.WriteBlock(gs, state.data(), state.size());
                m.PokeWord(kLiveBikes, static_cast<uint32_t>(live));
                m.PokeWord(kCountdownWord, static_cast<uint32_t>(postDelay));
            };
            for (size_t i = 0; i < slots.size(); ++i) {
                Slot& s = slots[i];
                s.ea = poolBase + 1096u * static_cast<uint32_t>(i);
                s.rd = m.PeekWord(s.ea + rr::sim::ent::kRiderDef);
                s.ow = m.PeekWord(s.ea + rr::sim::ent::kOwner);
                if (s.rd < 0x80000000u) s.rd = 0;
                if (s.ow < 0x80000000u) s.ow = 0;
                s.entity.assign(1096, 0);
                s.riderDef.assign(72, 0);
                s.owner.assign(kRiderStride, 0);
            }
            reread();

            struct Calls final : rr::sim::RaceFinishTest, rr::sim::RaceDirectorCallbacks {
                NativeEnv& env;
                Memory& m;
                std::vector<Slot>& slots;
                std::vector<uint32_t>& playerAddr;
                std::function<void()> commit, reread;
                Calls(NativeEnv& v, Memory& mm, std::vector<Slot>& s, std::vector<uint32_t>& pa,
                      std::function<void()> c, std::function<void()> r)
                    : env(v), m(mm), slots(s), playerAddr(pa), commit(std::move(c)),
                      reread(std::move(r)) {}
                uint32_t Call(uint32_t address, const uint32_t* a, int n) {
                    commit();
                    const uint32_t v0 = env.CallOracleArgs(address, a, n);
                    reread();
                    return v0;
                }
                int32_t Test(int32_t index) override {
                    const uint32_t a[4] = {slots[static_cast<size_t>(index)].ea, 0, 0, 0};
                    commit(); // PORTED (race.h FinishTest), witnessed (rows_spine.inc)
                    const uint32_t v0 = SpineNativeCall(env, kFinishTest, a, 1);
                    reread();
                    return static_cast<int32_t>(v0);
                }
                void RiderRemount(int32_t player) override {
                    const uint32_t a[4] = {playerAddr[static_cast<size_t>(player)], 1, 0, 0};
                    Call(kRiderRemount, a, 2);
                }
                void ResultsPrepare() override {
                    const uint32_t a[4] = {0, 0, 0, 0};
                    Call(kResultsPrep, a, 0);
                }
                void RaceOverSignal() override {
                    const uint32_t a[4] = {1, 0, 0, 0};
                    Call(kRaceOverSig, a, 1);
                }
                void AiDrivePass(int32_t dt, uint32_t skip, uint32_t* maskB,
                                 uint32_t* maskC) override {
                    // ONE 8-byte buffer: the original's two masks are `sp+20` and `sp+24` of its own
                    // frame, i.e. adjacent words, so a single capture covers both and the second
                    // pointer is declared as a zero-length frame-out purely to keep it out of the
                    // argument comparison.
                    const uint32_t buf = env.FrameOutBuffer(8);
                    m.PokeWord(buf, *maskB);
                    m.PokeWord(buf + 4u, *maskC);
                    const uint32_t a[4] = {static_cast<uint32_t>(dt), skip, buf, buf + 4u};
                    commit();
                    env.CallOracleArgs(kAiDrivePass, a, 4);
                    *maskB = m.PeekWord(buf);
                    *maskC = m.PeekWord(buf + 4u);
                    reread();
                }
                void AiRunCommands(int32_t dt, uint32_t skip, uint32_t maskB) override {
                    const uint32_t a[4] = {static_cast<uint32_t>(dt), skip, maskB, 0};
                    Call(kAiRunCommands, a, 3);
                }
                void AiBrainPass(int32_t dt, uint32_t skip, uint32_t maskC) override {
                    const uint32_t a[4] = {static_cast<uint32_t>(dt), skip, maskC, 0};
                    Call(kAiBrainPass, a, 3);
                }
                void PartnerCommand(int32_t player, uint32_t arg, int32_t dt) override {
                    const uint32_t partner =
                        m.PeekWord(playerAddr[static_cast<size_t>(player)] + 0x358u);
                    const uint32_t a[4] = {partner, arg, static_cast<uint32_t>(dt), 0};
                    Call(kPartnerCmd, a, 3);
                }
                void PartnerFinish(int32_t player, int32_t dt) override {
                    const uint32_t partner =
                        m.PeekWord(playerAddr[static_cast<size_t>(player)] + 0x358u);
                    const uint32_t a[4] = {partner, static_cast<uint32_t>(dt), 0, 0};
                    Call(kPartnerFinish, a, 2);
                }
            };

            // The players. `*(0x8005B268 + 4p)` is a pool-0 bike, so wherever it IS one of the
            // slots above the director shares that slot's buffers - two views of one entity would
            // otherwise fight each other on the way back into guest memory.
            std::vector<uint32_t> playerAddr;
            std::vector<rr::sim::RaceDirectorPlayer> players;
            std::vector<std::vector<uint8_t>> spare;
            spare.reserve(16);
            for (uint32_t p = 0; p < 2; ++p) {
                const uint32_t ea = m.PeekWord(kPlayerArr + 4u * p);
                playerAddr.push_back(ea);
                rr::sim::RaceDirectorPlayer pl;
                if (ea != 0) {
                    for (Slot& s : slots) {
                        if (s.ea != ea) continue;
                        pl.entity = s.entity.data();
                        pl.riderDef = s.riderDef.data();
                        pl.owner = s.owner.data();
                        break;
                    }
                }
                players.push_back(pl);
            }
            if (players[0].entity == nullptr && playerAddr[0] != 0) {
                // The player's bike is not one of the pool slots the bench resolved: decline.
                return 0u;
            }
            // Only view 0 is resolved, which is all a one-player race reaches.
            for (rr::sim::RaceDirectorPlayer& pl : players) pl.view = view.data();

            std::vector<uint8_t> kindTable(8u * 4096u);
            m.ReadBlock(kAltKindTable, kindTable.data(), kindTable.size());

            std::vector<rr::sim::ProgressNode> nodes(slots.size());
            for (size_t i = 0; i < slots.size(); ++i) {
                nodes[i].entity = slots[i].entity.data();
                nodes[i].riderDef = slots[i].riderDef.data();
                nodes[i].owner = slots[i].owner.data();
            }

            Calls calls(env, m, slots, playerAddr, commit, reread);

            rr::sim::RaceDirectorEnv de;
            de.gameState = state.data();
            de.players = players.data();
            de.playerCount = 1; // only view 0 and pool slot 0 are resolved; see the bound above
            de.kindTable = kindTable.data();
            de.postDelay = &postDelay;
            de.postLimit = &postLimit;
            de.skipResults = &skipResults;
            de.calls = &calls;
            de.progress.gameState = state.data();
            de.progress.liveBikes = &live;
            de.progress.bikes = nodes.empty() ? nullptr : nodes.data();
            de.progress.count = static_cast<int32_t>(nodes.size());
            de.progress.finish = &calls;

            if (!rr::sim::RaceDirector(static_cast<int32_t>(args.a[0]), de)) return 0u;
            commit();
            return 0u;
        },
        /*oracleCallees=*/
        [] {
            std::vector<OracleCallee> v{
                OracleCallee{kRiderRemount, 2}, OracleCallee{kResultsPrep, 0},
                OracleCallee{kRaceOverSig, 1},
                OracleCallee{kAiDrivePass, 4, 0, {OracleFrameOut{2, 8}, OracleFrameOut{3, 0}}},
                OracleCallee{kAiRunCommands, 3}, OracleCallee{kAiBrainPass, 3},
                OracleCallee{kPartnerCmd, 3}, OracleCallee{kPartnerFinish, 2}};
            // the finish test, now native, inside the ported ProgressPass (rows_spine.inc)
            for (const OracleCallee& oc : SpineFinishSeams(kProgressFrame)) v.push_back(oc);
            return v;
        }(),
        /*oracleFrame=*/kDirectorFrame,
        /*maxSteps=*/200'000'000});

    // ---------------------------------------------------------------- progress and place
    // These four are a CLOSED TREE - no seam at all.

    rows.push_back(Row{
        "route_find_leg", "const Leg *FindRouteLeg(const RouteObject *, u32 key)  [SLUS]",
        kRouteFindLeg,
        [](CaseContext& c, Args& args) {
            const uint32_t obj = c.scratch + 64u;
            // BOUNDED INPUT FAMILY, named: the leg count at `o[+0x0C]` is not bounded by the
            // original - it walks the table for as many entries as the count says, wherever they
            // land. The row drives 0..8 (which fit the scratch block) and NEGATIVE counts, which is
            // the `0 < count` guard's other side; it does not drive a huge positive count, because
            // that tests how far off the end of a synthetic object the interpreter gets, not the
            // port.
            const uint32_t count = c.rng->U32() % 9u;
            c.mem->PokeWord(obj + 0x0Cu,
                            (c.rng->Next() % 8u == 0)
                                ? static_cast<uint32_t>(-static_cast<int32_t>(c.rng->U32() % 64u))
                                : count);
            std::vector<uint32_t> keys(count ? count : 1u, 0u);
            for (uint32_t i = 0; i < count; ++i) {
                keys[i] = c.rng->U32() % 8u;
                for (uint32_t k = 0; k < 4; ++k)
                    c.mem->PokeWord(obj + 20u + 16u * i + 4u * k,
                                    (k == 0) ? keys[i] : c.rng->U32());
            }
            // Half the searches hit an entry that is really there, so both exits are driven.
            args.a[1] = (count != 0 && (c.rng->Next() & 1u)) ? keys[c.rng->U32() % count]
                                                            : (c.rng->U32() % 8u);
            args.a[0] = (c.rng->Next() % 8u == 0) ? 0u : obj;
            // The global that says whether a route is loaded at all: -1 short-circuits everything.
            PutS16(*c.mem, kRouteArmed,
                   static_cast<int16_t>((c.rng->Next() % 3u == 0) ? -1 : (c.rng->S32() >> 20)));
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            const uint32_t obj = args.a[0];
            if (obj == 0) {
                // A null object and an unloaded route both leave the original returning 0.
                return rr::sim::RouteFindLeg(nullptr, 0, args.a[1], GetS16(m, kRouteArmed)) >= 0
                           ? 1u
                           : 0u;
            }
            const int32_t count = GetS32(m, obj + 0x0Cu);
            std::vector<uint8_t> legs(count > 0 && count < 256
                                          ? static_cast<size_t>(count) * 16u
                                          : size_t{16});
            m.ReadBlock(obj + 20u, legs.data(), legs.size());
            const int32_t i = rr::sim::RouteFindLeg(legs.data(), count, args.a[1],
                                                    GetS16(m, kRouteArmed));
            // The caller turns the index back into the guest address the original returns.
            return (i < 0) ? 0u : (obj + 20u + 16u * static_cast<uint32_t>(i));
        }});

    rows.push_back(Row{
        "route_binding_valid", "s32 RouteBindingValid(void *entity_plus_0xAC)  [SLUS]", kRouteValid,
        [](CaseContext& c, Args& args) {
            const uint32_t ea = c.scratch + 0u;
            const uint32_t obj = c.scratch + 2048u;
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            put32(kEntRouteObj, (c.rng->Next() % 5u == 0) ? 0u : obj);
            // `kind` in the high half: 1 takes the compare arm, anything else the table search.
            const uint32_t kind = (c.rng->Next() & 1u) ? 1u : (c.rng->U32() % 4u);
            put32(kEntRoadPos, (kind << 16) | (c.rng->U32() % 8u));
            put32(kEntProgress, c.rng->U32());
            c.mem->WriteBlock(ea, e.data(), e.size());

            const uint32_t count = c.rng->U32() % 9u;
            c.mem->PokeWord(obj + 0x00u, c.rng->U32() % 8u); // the first word the kind-1 arm reads
            c.mem->PokeWord(obj + 0x0Cu, count);
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t k = 0; k < 4; ++k)
                    c.mem->PokeWord(obj + 20u + 16u * i + 4u * k,
                                    (k == 0) ? (c.rng->U32() % 8u) : c.rng->U32());
            PutS16(*c.mem, kRouteArmed,
                   static_cast<int16_t>((c.rng->Next() % 4u == 0) ? -1 : (c.rng->S32() >> 20)));
            args.a[0] = ea + rr::sim::ent::kHandle; // the original is called with `entity + 0xAC`
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            std::vector<uint8_t> e(1096);
            m.ReadBlock(scratch, e.data(), e.size());
            rr::sim::RouteBinding b;
            std::vector<uint8_t> legs;
            const uint32_t obj = m.PeekWord(scratch + kEntRouteObj);
            if (obj >= 0x80000000u) {
                b.firstWord = m.PeekWord(obj);
                b.legCount = GetS32(m, obj + 0x0Cu);
                legs.assign(b.legCount > 0 && b.legCount < 256
                                ? static_cast<size_t>(b.legCount) * 16u
                                : size_t{16},
                            0);
                m.ReadBlock(obj + 20u, legs.data(), legs.size());
                b.legs = legs.data();
                b.routeObject = e.data(); // only its non-null-ness and the two fields above matter
            }
            b.routeArmed = GetS16(m, kRouteArmed);
            return static_cast<uint32_t>(rr::sim::RouteBindingValid(e.data(), b));
        }});

    rows.push_back(Row{
        "progress_of", "s32 ProgressOf(Entity *e)  [SLUS, entity+0x144 or the 0x7FFFF000 sentinel]",
        kProgressOf,
        [](CaseContext& c, Args& args) {
            const uint32_t ea = c.scratch + 0u;
            const uint32_t obj = c.scratch + 2048u;
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            auto put32 = [&e](uint32_t off, uint32_t v) {
                e[off] = static_cast<uint8_t>(v);
                e[off + 1] = static_cast<uint8_t>(v >> 8);
                e[off + 2] = static_cast<uint8_t>(v >> 16);
                e[off + 3] = static_cast<uint8_t>(v >> 24);
            };
            put32(kEntRouteObj, (c.rng->Next() % 5u == 0) ? 0u : obj);
            const uint32_t kind = (c.rng->Next() & 1u) ? 1u : (c.rng->U32() % 4u);
            put32(kEntRoadPos, (kind << 16) | (c.rng->U32() % 8u));
            put32(kEntProgress, static_cast<uint32_t>(c.rng->S32() >> 4));
            c.mem->WriteBlock(ea, e.data(), e.size());
            const uint32_t count = c.rng->U32() % 9u;
            c.mem->PokeWord(obj + 0x00u, c.rng->U32() % 8u);
            c.mem->PokeWord(obj + 0x0Cu, count);
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t k = 0; k < 4; ++k)
                    c.mem->PokeWord(obj + 20u + 16u * i + 4u * k,
                                    (k == 0) ? (c.rng->U32() % 8u) : c.rng->U32());
            PutS16(*c.mem, kRouteArmed,
                   static_cast<int16_t>((c.rng->Next() % 4u == 0) ? -1 : (c.rng->S32() >> 20)));
            args.a[0] = ea;
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args&) {
            std::vector<uint8_t> e(1096);
            m.ReadBlock(scratch, e.data(), e.size());
            rr::sim::RouteBinding b;
            std::vector<uint8_t> legs;
            const uint32_t obj = m.PeekWord(scratch + kEntRouteObj);
            if (obj >= 0x80000000u) {
                b.firstWord = m.PeekWord(obj);
                b.legCount = GetS32(m, obj + 0x0Cu);
                legs.assign(b.legCount > 0 && b.legCount < 256
                                ? static_cast<size_t>(b.legCount) * 16u
                                : size_t{16},
                            0);
                m.ReadBlock(obj + 20u, legs.data(), legs.size());
                b.legs = legs.data();
                b.routeObject = e.data();
            }
            b.routeArmed = GetS16(m, kRouteArmed);
            return static_cast<uint32_t>(rr::sim::ProgressOf(e.data(), b));
        }});

    rows.push_back(Row{
        "compute_place", "s32 ComputePlace(Entity *e, s32 mode)  [SLUS, the rider's place]",
        kComputePlace,
        [](CaseContext& c, Args& args) {
            // BOUNDED INPUT FAMILY, named: the walk is over the machine's OWN pool 0, so the row
            // edits the live slots in place - only the fields this function branches on - exactly
            // as `progress_pass` does. Nothing else here reaches guest code at all: this family is
            // a closed tree and the row uses no seam.
            const uint32_t poolBase = c.mem->PeekWord(kPoolTable);
            const uint32_t highPtr = c.mem->PeekWord(kPoolTable + 12u);
            const int32_t stride = static_cast<int32_t>(c.mem->PeekWord(kPoolTable + 4u));
            int32_t high = -1;
            if (highPtr >= 0x80000000u) high = static_cast<int32_t>(c.mem->PeekWord(highPtr));
            if (high < 0 || high > 24) high = 0;
            const uint32_t obj = c.scratch + 0u;
            const uint32_t count = c.rng->U32() % 9u;
            c.mem->PokeWord(obj + 0x00u, c.rng->U32() % 8u);
            c.mem->PokeWord(obj + 0x0Cu, count);
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t k = 0; k < 4; ++k)
                    c.mem->PokeWord(obj + 20u + 16u * i + 4u * k,
                                    (k == 0) ? (c.rng->U32() % 8u) : c.rng->U32());
            PutS16(*c.mem, kRouteArmed,
                   static_cast<int16_t>((c.rng->Next() % 4u == 0) ? -1 : (c.rng->S32() >> 20)));

            for (int32_t i = 0; i <= high; ++i) {
                const uint32_t ea = poolBase + static_cast<uint32_t>(i * stride);
                c.mem->PokeWord(ea + kEntRouteObj, (c.rng->Next() % 5u == 0) ? 0u : obj);
                const uint32_t kind = (c.rng->Next() & 1u) ? 1u : (c.rng->U32() % 4u);
                c.mem->PokeWord(ea + kEntRoadPos, (kind << 16) | (c.rng->U32() % 8u));
                c.mem->PokeWord(ea + kEntProgress, static_cast<uint32_t>(c.rng->S32() >> 4));
                const uint32_t rd = c.mem->PeekWord(ea + rr::sim::ent::kRiderDef);
                if (rd < 0x80000000u) continue;
                uint8_t k = c.mem->PeekByte(rd + 1);
                k = static_cast<uint8_t>((k & 0xF0u) | (c.rng->U32() % 4u));
                c.mem->WriteBlock(rd + 1, &k, 1);
                uint8_t place;
                switch (c.rng->Next() % 4u) {
                    case 0: place = static_cast<uint8_t>(c.rng->U32() % 20u); break;
                    case 1: place = static_cast<uint8_t>(244u + c.rng->U32() % 12u); break;
                    case 2: place = 255u; break;
                    default: place = static_cast<uint8_t>(c.rng->U32()); break;
                }
                c.mem->WriteBlock(rd + 0x27u, &place, 1);
                c.mem->PokeWord(rd + 0x28u, (c.rng->Next() % 3u == 0)
                                                ? 0u
                                                : static_cast<uint32_t>(c.rng->S32() >> 12));
            }
            c.mem->PokeWord(kLiveBikes, c.rng->U32() % 24u);
            c.mem->PokeWord(kAiRaceFlags, c.rng->U32());
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 21u); // the handles are 0..17
            {
                const uint8_t rt = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(gs + 4u, &rt, 1);
            }
            args.a[0] = poolBase + static_cast<uint32_t>((c.rng->U32() % (high + 1u)) *
                                                         static_cast<uint32_t>(stride));
            args.a[1] = c.rng->U32() % 2u; // mode 0 and mode 1 are two different countings
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            const uint32_t poolBase = m.PeekWord(kPoolTable);
            const uint32_t highPtr = m.PeekWord(kPoolTable + 12u);
            const uint32_t stride = m.PeekWord(kPoolTable + 4u);
            int32_t high = -1;
            if (highPtr >= 0x80000000u) high = static_cast<int32_t>(m.PeekWord(highPtr));
            if (high < 0 || high > 24) high = -1;
            const uint32_t gs = m.PeekWord(kGameStatePtr);

            // Every pointer chase is the caller's, as everywhere in this bench.
            struct Held {
                std::vector<uint8_t> entity, riderDef, legs;
            };
            std::vector<Held> held(static_cast<size_t>(high + 1));
            std::vector<rr::sim::PlaceNode> nodes(held.size());
            const int16_t armed = GetS16(m, kRouteArmed);
            auto bind = [&](uint32_t ea, Held& h, rr::sim::RouteBinding& b) {
                const uint32_t obj = m.PeekWord(ea + kEntRouteObj);
                b.routeArmed = armed;
                if (obj < 0x80000000u) return;
                b.firstWord = m.PeekWord(obj);
                b.legCount = GetS32(m, obj + 0x0Cu);
                h.legs.assign(b.legCount > 0 && b.legCount < 256
                                  ? static_cast<size_t>(b.legCount) * 16u
                                  : size_t{16},
                              0);
                m.ReadBlock(obj + 20u, h.legs.data(), h.legs.size());
                b.legs = h.legs.data();
                b.routeObject = h.legs.data(); // non-null-ness is all the port reads it for
            };
            for (int32_t i = 0; i <= high; ++i) {
                Held& h = held[static_cast<size_t>(i)];
                const uint32_t ea = poolBase + static_cast<uint32_t>(i) * stride;
                h.entity.assign(1096, 0);
                m.ReadBlock(ea, h.entity.data(), h.entity.size());
                const uint32_t rd = m.PeekWord(ea + rr::sim::ent::kRiderDef);
                h.riderDef.assign(72, 0);
                if (rd >= 0x80000000u) m.ReadBlock(rd, h.riderDef.data(), h.riderDef.size());
                nodes[static_cast<size_t>(i)].entity = h.entity.data();
                nodes[static_cast<size_t>(i)].riderDef = h.riderDef.data();
                bind(ea, h, nodes[static_cast<size_t>(i)].binding);
            }

            std::vector<uint8_t> state(64), self(1096), selfRd(72);
            m.ReadBlock(gs, state.data(), state.size());
            m.ReadBlock(args.a[0], self.data(), self.size());
            const uint32_t selfRdAddr = m.PeekWord(args.a[0] + rr::sim::ent::kRiderDef);
            if (selfRdAddr >= 0x80000000u) m.ReadBlock(selfRdAddr, selfRd.data(), selfRd.size());
            Held selfHeld;
            rr::sim::RouteBinding selfBinding;
            bind(args.a[0], selfHeld, selfBinding);

            rr::sim::ComputePlaceEnv pe;
            pe.gameState = state.data();
            pe.liveBikes = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            pe.raceFlags = m.PeekWord(kAiRaceFlags);
            pe.pool = nodes.empty() ? nullptr : nodes.data();
            pe.poolHigh = high;
            pe.poolCount = static_cast<int32_t>(nodes.size());

            int32_t place = 0;
            if (!rr::sim::ComputePlace(self.data(), selfRd.data(), selfBinding,
                                       static_cast<int32_t>(args.a[1]), pe, &place))
                return 0u;
            return static_cast<uint32_t>(place);
        }});

    rows.push_back(Row{
        "finish_test",
        "s32 FinishTest(Entity *e)  [RASHCDG 0x800B9958, 2476 bytes - a rider can finish]",
        kFinishTest,
        [](CaseContext& c, Args& args) {
            // BOUNDED INPUT FAMILY, named. The finish test reaches the machine's own entity pool,
            // its road slices, its rider records and eleven race globals, so - exactly as
            // `progress_pass` and `ai_drive` do - the row edits the LIVE pool-0 slots in place and
            // randomises only the fields this function itself branches on. The route record and
            // the route objects are built in the scratch block, because the two crossing arms
            // compare road ids against them and a captured pair matches by accident on no case.
            const uint32_t poolBase = c.mem->PeekWord(kPoolBasePtr);
            const uint32_t stride = 1096u;
            int32_t live = static_cast<int32_t>(c.mem->PeekWord(kLiveBikes));
            if (live < 1 || live > 24) live = 1;
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            // Half the cases put the bike under test in slot 0..2, because every clock arm of the
            // function is gated on `handle < gameState[+0x30]` and the pool's handles are 0..17:
            // drawing the slot uniformly would leave those four arms on a ninth of the cases.
            const uint32_t self = (c.rng->Next() & 1u)
                                      ? (c.rng->U32() % 3u) % static_cast<uint32_t>(live)
                                      : (c.rng->U32() % static_cast<uint32_t>(live));
            const uint32_t ea = poolBase + stride * self;

            // --- the race globals ---------------------------------------------------------
            // 33, 36, 44 and 17 are the four race types the function tests by name; bits 0 and 2
            // are the two it tests as flags.
            static const uint8_t kTypes[8] = {33, 36, 44, 17, 0x2C, 0x05, 0x04, 0x2D};
            const uint8_t rt = kTypes[c.rng->U32() % 8u];
            c.mem->WriteBlock(gs + 4u, &rt, 1);
            // The race clock, straddling both 108000 ticks (6:00) and 216000 (12:00) deliberately.
            static const int32_t kClocks[6] = {0, 107999, 108001, 215999, 216001, 300000};
            const int32_t clock = (c.rng->Next() % 3u == 0)
                                      ? kClocks[c.rng->U32() % 6u]
                                      : static_cast<int32_t>(c.rng->U32() % 400000u);
            c.mem->PokeWord(gs + 0x10u, static_cast<uint32_t>(clock));
            c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 3u);
            const uint8_t milestone = static_cast<uint8_t>(c.rng->U32() % 4u);
            c.mem->WriteBlock(gs + 0x39u, &milestone, 1);
            c.mem->PokeWord(kSkipResults, c.rng->U32() % 5u);
            c.mem->PokeWord(kTimeLimit,
                            (c.rng->Next() & 1u) ? (c.rng->U32() % 400000u)
                                                 : static_cast<uint32_t>(clock));
            c.mem->PokeWord(kTimeBase, c.rng->U32() % 400000u);
            c.mem->PokeWord(kStartDir, (c.rng->Next() & 1u) ? 1u : 0xFFFFFFFFu);
            PutS16(*c.mem, kRouteArmed, static_cast<int16_t>((c.rng->Next() % 4u == 0) ? -1 : 0));
            // The race-type-17 arm chases `*(0x8005B268 + 4)` with no null guard and this capture
            // is a ONE-PLAYER race, so the row plants a real bike there. That is a state the
            // shipped game does not reach; it is what makes the arm reachable at all, and both
            // sides of the comparison see it.
            c.mem->PokeWord(kPlayerArrHigh,
                            poolBase + stride * ((self + 1u) % static_cast<uint32_t>(live)));

            // --- the route record and three route objects, in the scratch block -----------
            const uint32_t rec = c.scratch + 0u;
            const uint32_t objA = c.scratch + 64u;
            const uint32_t objB = c.scratch + 256u;
            const uint32_t objC = c.scratch + 448u;
            const uint32_t road = c.rng->U32() % 8u;
            c.mem->PokeWord(rec + 0u, road);
            c.mem->PokeWord(rec + 4u, static_cast<uint32_t>(c.rng->S32() >> 8));  // the line
            c.mem->PokeWord(rec + 8u, c.rng->U32());                              // which side
            c.mem->PokeWord(rec + 12u,
                            (c.rng->Next() % 3u == 0) ? 0xFFFFFFFFu : (c.rng->U32() % 8u));
            c.mem->PokeWord(kRouteRecordPtr, rec);
            const uint32_t objs[3] = {objA, objB, objC};
            for (uint32_t o : objs) {
                c.mem->PokeWord(o + 0u, (c.rng->Next() & 1u) ? c.mem->PeekWord(rec + 12u)
                                                             : (c.rng->U32() % 8u));
                // `routeObject[+0x76] >> handle & 1` is what arms the finish line for this rider,
                // and the pool's handles run to 17 - past the end of a halfword - so half the
                // cases set every bit rather than leaving the arm to chance.
                WriteU16Bench(*c.mem, o + 118u,
                              static_cast<uint16_t>((c.rng->Next() & 1u) ? 0xFFFFu : c.rng->U32()));
            }
            const uint32_t msRoad = c.mem->PeekWord(kMilestoneTable + 4u * milestone) & 0xFFFFu;

            // --- the bikes ----------------------------------------------------------------
            for (int32_t i = 0; i < live; ++i) {
                const uint32_t e = poolBase + stride * static_cast<uint32_t>(i);
                const uint32_t rd = c.mem->PeekWord(e + rr::sim::ent::kRiderDef);
                const uint32_t ow = c.mem->PeekWord(e + rr::sim::ent::kOwner);
                c.mem->PokeWord(e + 0x1ACu, (c.rng->Next() % 5u == 0) ? 0u : objA);
                // entity[+0x168] - the road id and the route record's `kind`. A third of the
                // cases plant the route record's own road and a third the milestone's, so that the
                // two crossing arms are entered rather than missed by chance.
                uint32_t pos;
                switch (c.rng->Next() % 3u) {
                    case 0: pos = road; break;
                    case 1: pos = msRoad; break;
                    default: pos = ((c.rng->U32() % 3u) << 16) | (c.rng->U32() % 8u); break;
                }
                c.mem->PokeWord(e + 0x168u, pos);
                c.mem->PokeWord(e + 0x16Cu, (c.rng->Next() & 1u) ? 1u : 0xFFFFFFFFu);
                c.mem->PokeWord(e + 0x170u, static_cast<uint32_t>(c.rng->S32() >> 6));
                c.mem->PokeWord(e + rr::sim::ent::kFlagsA,
                                (c.mem->PeekWord(e + rr::sim::ent::kFlagsA) & ~0x08000000u) |
                                    ((c.rng->Next() & 1u) ? 0x08000000u : 0u));
                WriteU16Bench(*c.mem, e + rr::sim::ent::kLiveState,
                              static_cast<uint16_t>((c.rng->Next() % 3u == 0)
                                                        ? 0u
                                                        : (c.rng->U32() & 3u)));
                c.mem->PokeWord(e + 0x2Cu, static_cast<uint32_t>(c.rng->S32() >> 20));
                c.mem->PokeWord(e + 0x30u, static_cast<uint32_t>(c.rng->S32() >> 20));
                if (rd >= 0x80000000u) {
                    uint8_t k = c.mem->PeekByte(rd + 1);
                    k = static_cast<uint8_t>((k & 0xF0u) | (c.rng->U32() % 4u));
                    c.mem->WriteBlock(rd + 1, &k, 1);
                    uint8_t place;
                    switch (c.rng->Next() % 4u) {
                        case 0: place = static_cast<uint8_t>(1u + c.rng->U32() % 20u); break;
                        case 1: place = static_cast<uint8_t>(246u + c.rng->U32() % 10u); break;
                        case 2: place = 255u; break;
                        default: place = static_cast<uint8_t>(c.rng->U32()); break;
                    }
                    c.mem->WriteBlock(rd + 0x27u, &place, 1);
                    // `riderDef[+0x28] != 0` is the whole of arm 1: it returns 1 at once. The bike
                    // under test therefore gets a zero stamp five times in six, so that the four
                    // arms behind it are what the row actually measures.
                    const uint32_t zeroOdds = (e == ea) ? 6u : 3u;
                    c.mem->PokeWord(rd + 0x28u, (c.rng->Next() % zeroOdds != 0)
                                                    ? 0u
                                                    : (c.rng->U32() % 0x10000u));
                }
                if (ow >= 0x80000000u) {
                    uint8_t f = c.mem->PeekByte(ow + 0x23Cu);
                    f = static_cast<uint8_t>((f & ~0x10u) | (c.rng->U32() & 0x10u));
                    c.mem->WriteBlock(ow + 0x23Cu, &f, 1);
                    c.mem->PokeWord(ow + 0x25Cu, c.rng->U32() % 6u);
                    c.mem->PokeWord(ow + 0x168u, (c.rng->Next() & 1u) ? road : c.rng->U32() % 8u);
                    c.mem->PokeWord(ow + 0x170u, static_cast<uint32_t>(c.rng->S32() >> 6));
                    c.mem->PokeWord(ow + 0x1ACu, (c.rng->Next() % 5u == 0) ? 0u : objB);
                }
                // `entity[+0x358]`, the second rider of a two-rider bike. It is 0 on every bike of
                // every capture this project has, so the row plants a
                // real pool slot there - which is what makes the partner arm of the finish line
                // reachable at all. The original dereferences it with no null guard, so it is
                // planted only where the `owner[+0x23C] & 0x10` gate that reaches it is set.
                const uint32_t partner =
                    poolBase + stride * (c.rng->U32() % static_cast<uint32_t>(live));
                c.mem->PokeWord(e + 0x358u, partner);
                const uint32_t pOw = c.mem->PeekWord(partner + rr::sim::ent::kOwner);
                if (pOw >= 0x80000000u) c.mem->PokeWord(pOw + 0x1ACu, objC);
            }
            args.a[0] = ea;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t ea = args.a[0];
            const uint32_t gsA = m.PeekWord(kGameStatePtr);
            const uint32_t rdA = m.PeekWord(ea + rr::sim::ent::kRiderDef);
            const uint32_t owA = m.PeekWord(ea + rr::sim::ent::kOwner);
            const uint32_t partnerA = m.PeekWord(ea + 0x358u);
            const uint32_t partnerOwA = (partnerA >= 0x80000000u)
                                            ? m.PeekWord(partnerA + rr::sim::ent::kOwner)
                                            : 0u;
            const uint32_t sliceA = m.PeekWord(ea + 0x154u);
            const uint32_t chunkA = m.PeekWord(ea + 0x150u);
            const uint32_t p1A = m.PeekWord(kPlayer1Ptr);

            GuestMirror mir(m);
            rr::sim::FinishTestEnv fe;
            fe.entity = mir.Map(ea, 1096, true);
            fe.riderDef = mir.Map(rdA, 72, true);
            fe.owner = mir.Map(owA, kRiderStride, true);
            fe.gameState = mir.Map(gsA, 64, true);
            fe.partnerOwner = mir.Map(partnerOwA, kRiderStride, true);
            fe.entityRoute = mir.Map(m.PeekWord(ea + 0x1ACu), 128, false);
            if (owA >= 0x80000000u) fe.ownerRoute = mir.Map(m.PeekWord(owA + 0x1ACu), 128, false);
            if (partnerOwA >= 0x80000000u)
                fe.partnerRoute = mir.Map(m.PeekWord(partnerOwA + 0x1ACu), 128, false);
            fe.routeRecord = mir.Map(m.PeekWord(kRouteRecordPtr), 16, false);
            fe.milestones = mir.Map(kMilestoneTable, kMilestoneBytes, false);
            fe.milestoneCount = static_cast<int32_t>(kMilestoneBytes / 4u);
            fe.raceOverFlag = reinterpret_cast<const int32_t*>(mir.Map(kSkipResults, 4, false));
            fe.timeLimit = reinterpret_cast<const int32_t*>(mir.Map(kTimeLimit, 4, false));
            fe.timeBase = reinterpret_cast<const int32_t*>(mir.Map(kTimeBase, 4, false));
            fe.startDir = reinterpret_cast<const int32_t*>(mir.Map(kStartDir, 4, false));
            fe.postDelay = reinterpret_cast<int32_t*>(mir.Map(kCountdownWord, 4, true));
            fe.jailbreakClock = mir.Map(kJailbreakClock, 4, true);
            fe.finishOrder = mir.Map(kFinishOrder, kFinishOrderBytes, true);
            fe.finishOrderCount = static_cast<int32_t>(kFinishOrderBytes);
            fe.routeArmed = GetS16(m, kRouteArmed);
            // The identity comparison the original makes on guest ADDRESSES: `*(0x8005B21C)` is
            // this bike or it is not, and only the first is a case the tail below can run on.
            fe.player2 = (m.PeekWord(kPlayer2Ptr) == ea) ? fe.entity : nullptr;
            if (p1A >= 0x80000000u) {
                fe.p1Entity = mir.Map(p1A, 1096, true);
                fe.p1RiderDef = mir.Map(m.PeekWord(p1A + rr::sim::ent::kRiderDef), 72, true);
            }
            for (int k = 0; k < 2; ++k) {
                const uint32_t b = m.PeekWord(kPlayerArr + 4u * static_cast<uint32_t>(k));
                if (b >= 0x80000000u)
                    fe.playerRiderDef[k] =
                        mir.Map(m.PeekWord(b + rr::sim::ent::kRiderDef), 72, true);
            }

            // The road slice and the road chunk are read into typed storage, because a
            // `RoadSliceView` is int16 and int32 arrays and a byte mirror cannot supply those
            // without an aliasing cast.
            int16_t sliceM[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
            int32_t slicePos[3] = {0, 0, 0};
            auto refreshRoad = [&]() {
                if (sliceA >= 0x80000000u) {
                    for (uint32_t i = 0; i < 9; ++i) sliceM[i] = GetS16(m, sliceA + 2u + 2u * i);
                    for (uint32_t i = 0; i < 3; ++i) slicePos[i] = GetS32(m, sliceA + 20u + 4u * i);
                    fe.sliceAlongBase = GetS32(m, sliceA + 40u);
                    fe.sliceIndex = GetS16(m, sliceA);
                    fe.slice.m = sliceM;
                    fe.slice.pos = slicePos;
                    fe.sliceValid = true;
                }
                if (chunkA >= 0x80000000u) {
                    fe.chunkLast = GetS16(m, chunkA + 10u);
                    fe.chunkValid = true;
                }
            };
            refreshRoad();

            auto oracle = [&](uint32_t address, uint32_t a0, uint32_t a1, int n) -> uint32_t {
                mir.Commit();
                const uint32_t a[4] = {a0, a1, 0, 0};
                const uint32_t v = env.CallOracleArgs(address, a, n);
                mir.Reread();
                refreshRoad();
                return v;
            };

            struct Calls final : rr::sim::FinishTestCalls {
                std::function<int32_t()> milestoneAdvance, roadAdvance;
                std::function<void()> milestoneFirst, stampResult;
                std::function<void(uint32_t, int32_t)> recordFinish;
                std::function<bool(int32_t*)> computePlace;
                int32_t MilestoneAdvance() override { return milestoneAdvance(); }
                void MilestoneFirst() override { milestoneFirst(); }
                void StampResult() override { stampResult(); }
                void RecordFinish(uint32_t h, int32_t f) override { recordFinish(h, f); }
                int32_t RoadAdvance() override { return roadAdvance(); }
                bool ComputePlaceMode1(int32_t* p) override { return computePlace(p); }
            } calls;
            calls.milestoneAdvance = [&]() {
                // a0 is `entity[+0x16C]` at the original's own call site (0x800B9E44 loads it and
                // nothing writes a0 before 0x800B9E8C), so the argument is passed and compared.
                return static_cast<int32_t>(oracle(
                    kMilestoneAdvance,
                    static_cast<uint32_t>(GetS32(m, ea + 0x16Cu)), 0, 1));
            };
            calls.milestoneFirst = [&]() { oracle(kMilestoneFirst, 0, 0, 0); };
            // These three are PORTED (spine.h StampResult, race.h RecordFinish, road_query.h
            // RoadNextObjectMissing): run natively on the clone, witnessed (rows_spine.inc).
            auto native = [&](uint32_t address, uint32_t a0, uint32_t a1, int n) -> uint32_t {
                mir.Commit();
                const uint32_t a[2] = {a0, a1};
                const uint32_t v = SpineNativeCall(env, address, a, n);
                mir.Reread();
                refreshRoad();
                return v;
            };
            calls.stampResult = [&]() { native(kResultStamp, ea, 0, 1); };
            calls.recordFinish = [&](uint32_t h, int32_t f) {
                native(kRecordFinish, h, static_cast<uint32_t>(f), 2);
            };
            calls.roadAdvance = [&]() {
                return static_cast<int32_t>(
                    native(kRoadAdvance, ea + 0x148u,
                           static_cast<uint32_t>(GetS32(m, ea + 0x16Cu)), 2));
            };
            calls.computePlace = [&](int32_t* place) {
                // `ComputePlace` is PORTED. Only its pool walk is resolved here, which is the
                // caller-chases-the-pointers rule and not a seam: no
                // guest code runs for this call on either side.
                mir.Commit();
                const uint32_t poolBase = m.PeekWord(kPoolTable);
                const uint32_t highPtr = m.PeekWord(kPoolTable + 12u);
                const uint32_t stride = m.PeekWord(kPoolTable + 4u);
                int32_t high = -1;
                if (highPtr >= 0x80000000u) high = static_cast<int32_t>(m.PeekWord(highPtr));
                if (high < 0 || high > 24) return false;
                const int16_t armed = GetS16(m, kRouteArmed);
                struct Held {
                    std::vector<uint8_t> entity, riderDef, legs;
                };
                std::vector<Held> held(static_cast<size_t>(high + 1));
                std::vector<rr::sim::PlaceNode> nodes(held.size());
                auto bind = [&](uint32_t a, Held& h, rr::sim::RouteBinding& b) {
                    const uint32_t obj = m.PeekWord(a + kEntRouteObj);
                    b.routeArmed = armed;
                    if (obj < 0x80000000u) return;
                    b.firstWord = m.PeekWord(obj);
                    b.legCount = GetS32(m, obj + 0x0Cu);
                    h.legs.assign(b.legCount > 0 && b.legCount < 256
                                      ? static_cast<size_t>(b.legCount) * 16u
                                      : size_t{16},
                                  0);
                    m.ReadBlock(obj + 20u, h.legs.data(), h.legs.size());
                    b.legs = h.legs.data();
                    b.routeObject = h.legs.data();
                };
                for (int32_t i = 0; i <= high; ++i) {
                    Held& h = held[static_cast<size_t>(i)];
                    const uint32_t a = poolBase + static_cast<uint32_t>(i) * stride;
                    h.entity.assign(1096, 0);
                    m.ReadBlock(a, h.entity.data(), h.entity.size());
                    const uint32_t rd = m.PeekWord(a + rr::sim::ent::kRiderDef);
                    h.riderDef.assign(72, 0);
                    if (rd >= 0x80000000u) m.ReadBlock(rd, h.riderDef.data(), h.riderDef.size());
                    nodes[static_cast<size_t>(i)].entity = h.entity.data();
                    nodes[static_cast<size_t>(i)].riderDef = h.riderDef.data();
                    bind(a, h, nodes[static_cast<size_t>(i)].binding);
                }
                std::vector<uint8_t> state(64), self(1096), selfRd(72);
                m.ReadBlock(gsA, state.data(), state.size());
                m.ReadBlock(ea, self.data(), self.size());
                if (rdA >= 0x80000000u) m.ReadBlock(rdA, selfRd.data(), selfRd.size());
                Held selfHeld;
                rr::sim::RouteBinding selfBinding;
                bind(ea, selfHeld, selfBinding);
                rr::sim::ComputePlaceEnv pe;
                pe.gameState = state.data();
                pe.liveBikes = static_cast<int32_t>(m.PeekWord(kLiveBikes));
                pe.raceFlags = m.PeekWord(kAiRaceFlags);
                pe.pool = nodes.empty() ? nullptr : nodes.data();
                pe.poolHigh = high;
                pe.poolCount = static_cast<int32_t>(nodes.size());
                return rr::sim::ComputePlace(self.data(), selfRd.data(), selfBinding, 1, pe, place);
            };
            fe.calls = &calls;

            int32_t result = 0;
            if (!rr::sim::FinishTest(fe, &result)) {
                // The port declined. Leave the candidate wrong rather than guess, so that the row
                // fails loudly if the input family ever reaches one of the named holes.
                return 0xDEADDEADu;
            }
            mir.Commit();
            return static_cast<uint32_t>(result);
        },
        /*oracleCallees=*/
        // the two milestone callees on the oracle; the other three witnessed (ported), plus the
        // stance event StampResult reaches (rows_spine.inc)
        [] {
            std::vector<OracleCallee> v{OracleCallee{kMilestoneAdvance, 1}, OracleCallee{kMilestoneFirst, 0},
                                        OracleCallee{kResultStamp, 1}, OracleCallee{kRecordFinish, 2},
                                        OracleCallee{kRoadAdvance, 2}};
            for (const OracleCallee& oc : SpineStampSeams(40)) v.push_back(oc); // StampResult's frame
            return v;
        }(),
        /*oracleFrame=*/kFinishFrame,
        /*maxSteps=*/100'000'000});

    rows.push_back(Row{
        "record_finish",
        "void RecordFinish(u32 handle, s32 flag)  [SLUS 0x8003F680, the finishing-order table]",
        kRecordFinish,
        [](CaseContext& c, Args& args) {
            const uint32_t poolBase = c.mem->PeekWord(kPoolBasePtr);
            int32_t live = static_cast<int32_t>(c.mem->PeekWord(kLiveBikes));
            if (live < 1 || live > 24) live = 1;
            const uint32_t self = c.rng->U32() % static_cast<uint32_t>(live);
            const uint32_t ea = poolBase + 1096u * self;
            const uint32_t rd = c.mem->PeekWord(ea + rr::sim::ent::kRiderDef);
            if (rd >= 0x80000000u) {
                // 1..18 is the only window that reaches the table; 0, 19 and the result codes are
                // driven deliberately so that both sides of that test are exercised.
                uint8_t place;
                switch (c.rng->Next() % 4u) {
                    case 0: place = static_cast<uint8_t>(1u + c.rng->U32() % 18u); break;
                    case 1: place = 0; break;
                    case 2: place = static_cast<uint8_t>(19u + c.rng->U32() % 4u); break;
                    default: place = static_cast<uint8_t>(c.rng->U32()); break;
                }
                c.mem->WriteBlock(rd + 0x27u, &place, 1);
                c.mem->PokeWord(rd + 0x28u, c.rng->U32());
            }
            args.a[0] = self;         // the pool index; the original turns it into 1096 * handle
            args.a[1] = c.rng->U32(); // the flag, stored verbatim
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t ea = m.PeekWord(kPoolBasePtr) + 1096u * args.a[0];
            GuestMirror mir(m);
            rr::sim::FinishOrderEnv fe;
            fe.entity = mir.Map(ea, 1096, false);
            fe.riderDef = mir.Map(m.PeekWord(ea + rr::sim::ent::kRiderDef), 72, false);
            fe.table = mir.Map(kFinishOrder - 16u, 16u * 20u, true);
            fe.tableBytes = static_cast<int32_t>(16u * 20u);
            if (!rr::sim::RecordFinish(static_cast<int32_t>(args.a[1]), fe)) return 0xDEADDEADu;
            mir.Commit();
            return 0u;
        }});

    rows.push_back(Row{
        "end_race",
        "void EndRace(Entity *e, s32 reason)  [RASHCDG 0x80092C7C, what a finished race writes]",
        kEndRace,
        [](CaseContext& c, Args& args) {
            const uint32_t poolBase = c.mem->PeekWord(kPoolBasePtr);
            const uint32_t stride = 1096u;
            int32_t live = static_cast<int32_t>(c.mem->PeekWord(kLiveBikes));
            if (live < 1 || live > 24) live = 1;
            const uint32_t self = c.rng->U32() % static_cast<uint32_t>(live);
            const uint32_t ea = poolBase + stride * self;
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x10u, c.rng->U32() % 400000u);
            const uint32_t rd = c.mem->PeekWord(ea + rr::sim::ent::kRiderDef);
            if (rd >= 0x80000000u) {
                c.mem->PokeWord(rd + 0x28u,
                                (c.rng->Next() % 3u == 0) ? 0u : (c.rng->U32() % 0x10000u));
                uint8_t place = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(rd + 0x27u, &place, 1);
            }
            const uint32_t ow = c.mem->PeekWord(ea + rr::sim::ent::kOwner);
            if (ow >= 0x80000000u) c.mem->PokeWord(ow + 0x25Cu, c.rng->U32() % 5u);
            c.mem->PokeWord(ea + rr::sim::ent::kFlagsA, c.rng->U32());
            c.mem->PokeWord(ea + 0x238u, c.rng->U32());
            // The view record the function writes back into, at 0x800CD898 + 1132 * HANDLE.
            const uint32_t view =
                kViewArray + kViewStride * static_cast<uint32_t>(GetS16(*c.mem, ea + 0x0ACu) &
                                                                 0xFFFF);
            c.mem->PokeWord(view + 0x228u, (c.rng->Next() & 1u) ? (c.rng->U32() | 4u)
                                                                : (c.rng->U32() & ~4u));
            args.a[0] = ea;
            // 9 is the wipeout and 10 the arm that writes the view outright; the rest are busts.
            static const uint32_t kReasons[6] = {9, 10, 0, 1, 5, 12};
            args.a[1] = kReasons[c.rng->U32() % 6u];
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t ea = args.a[0];
            const uint32_t gsA = m.PeekWord(kGameStatePtr);
            const uint32_t handle = static_cast<uint32_t>(GetS16(m, ea + 0x0ACu)) & 0xFFFFu;
            const uint32_t viewA = kViewArray + kViewStride * handle;

            GuestMirror mir(m);
            rr::sim::EndRaceEnv ee;
            ee.entity = mir.Map(ea, 1096, true);
            ee.riderDef = mir.Map(m.PeekWord(ea + rr::sim::ent::kRiderDef), 72, true);
            ee.owner = mir.Map(m.PeekWord(ea + rr::sim::ent::kOwner), kRiderStride, true);
            ee.view = mir.Map(viewA, 0x310, true);
            ee.gameState = mir.Map(gsA, 64, true);
            ee.postDelay = reinterpret_cast<int32_t*>(mir.Map(kCountdownWord, 4, true));

            auto oracle = [&](uint32_t address, uint32_t a0, uint32_t a1, int n) {
                mir.Commit();
                const uint32_t a[4] = {a0, a1, 0, 0};
                env.CallOracleArgs(address, a, n);
                mir.Reread();
            };
            struct Calls final : rr::sim::EndRaceCalls {
                std::function<void()> stamp, wipeout;
                std::function<void(int32_t)> viewEvent;
                void StampResult() override { stamp(); }
                void WipeoutEffect() override { wipeout(); }
                void ViewEvent(int32_t reason) override { viewEvent(reason); }
            } calls;
            // All three PORTED (spine.h), run natively on the clone and witnessed (rows_spine.inc).
            auto native = [&](uint32_t address, uint32_t a0, uint32_t a1, int n) {
                mir.Commit();
                const uint32_t a[2] = {a0, a1};
                SpineNativeCall(env, address, a, n);
                mir.Reread();
            };
            calls.stamp = [&]() { native(kResultStamp, ea, 0, 1); };
            calls.wipeout = [&]() { native(kWipeoutEffect, ea, 0, 1); };
            calls.viewEvent = [&](int32_t reason) {
                native(kViewEvent, viewA, static_cast<uint32_t>(reason), 2);
            };
            (void)oracle;
            ee.calls = &calls;

            if (!rr::sim::EndRace(static_cast<int32_t>(args.a[1]), ee)) return 0xDEADDEADu;
            mir.Commit();
            return 0u;
        },
        /*oracleCallees=*/
        // the three witnessed (ported), plus the stance event StampResult reaches (rows_spine.inc)
        [] {
            std::vector<OracleCallee> v{OracleCallee{kResultStamp, 1}, OracleCallee{kWipeoutEffect, 1},
                                        OracleCallee{kViewEvent, 2}};
            for (const OracleCallee& oc : SpineStampSeams(40)) v.push_back(oc); // StampResult's frame
            return v;
        }(),
        /*oracleFrame=*/kEndRaceFrame,
        /*maxSteps=*/50'000'000});

    // ------------------------------------------------------------------ the ground frame,
    // RASHCDG 0x8007504C.
    rows.push_back(Row{
        "bike_ground_frame",
        "void BikeGroundFrame(Bike*, const s32 ref[3])  [RASHCDG, road frame + contact point]",
        kGroundFrame,
        [](CaseContext& c, Args& args) {
            // BOUNDED INPUT FAMILY, named, and it is the same bound `ai_drive` carries: the
            // ground query `0x800A7BF8` walks THIS
            // machine's collision world through `e[+0x2C]`, `e[+0x30]`, the heading at `+0x1C2` and
            // the road slice at `+0x154`, so every case is built on a LIVE pool-0 bike of the
            // bench's own snapshot. A bike lifted out of one of the other seventeen RAM images
            // points at geometry `rr-race` does not contain.
            const uint32_t ea = c.scratch;
            const uint32_t riderAddr = c.scratch + 1096u;
            const uint32_t riderDef = c.scratch + 2192u;

            const uint32_t poolBase = c.mem->PeekWord(kPoolTable);
            const uint32_t highPtr = c.mem->PeekWord(kPoolTable + 12u);
            const int32_t stride = static_cast<int32_t>(c.mem->PeekWord(kPoolTable + 4u));
            uint32_t slots = 1;
            if (highPtr >= 0x80000000u) {
                const int32_t high = static_cast<int32_t>(c.mem->PeekWord(highPtr));
                if (high >= 0 && high < 64) slots = static_cast<uint32_t>(high) + 1u;
            }
            auto liveBike = [&](std::vector<uint8_t>& out) {
                out.assign(1096, 0);
                if (poolBase >= 0x80000000u && stride >= 1096)
                    c.mem->ReadBlock(poolBase + (c.rng->U32() % slots) *
                                                    static_cast<uint32_t>(stride),
                                     out.data(), 1096);
            };
            auto put32 = [](std::vector<uint8_t>& v, uint32_t off, uint32_t x) {
                v[off] = static_cast<uint8_t>(x);
                v[off + 1] = static_cast<uint8_t>(x >> 8);
                v[off + 2] = static_cast<uint8_t>(x >> 16);
                v[off + 3] = static_cast<uint8_t>(x >> 24);
            };
            auto put16 = [](std::vector<uint8_t>& v, uint32_t off, uint16_t x) {
                v[off] = static_cast<uint8_t>(x);
                v[off + 1] = static_cast<uint8_t>(x >> 8);
            };
            auto get32 = [](const std::vector<uint8_t>& v, uint32_t off) {
                return static_cast<uint32_t>(v[off]) | (static_cast<uint32_t>(v[off + 1]) << 8) |
                       (static_cast<uint32_t>(v[off + 2]) << 16) |
                       (static_cast<uint32_t>(v[off + 3]) << 24);
            };

            std::vector<uint8_t> e, r, other;
            liveBike(e);
            liveBike(r);
            liveBike(other);

            // `+0x100` is the slice the road search `SLUS 0x80036B14` bound this frame
            // (`sw v0,256(s2)` at SLUS 0x80037288). A CAPTURED bike does not have one there: the
            // box builder 0x8008BA18 overwrites +0xC4..+0x123 later in the same frame, so the row
            // has to plant a real slice. Half the cases plant the slice the bike is tracked on,
            // which is what makes both sides of the `bne a0,v1` at 0x8007546C reachable.
            put32(e, 0x100, (c.rng->Next() & 1u) ? get32(e, 0x154) : get32(other, 0x154));
            // ... and `+0xF4` is the point that search left, which the second query uses as its
            // reference. Jittered around the bike's own contact point so the query stays in world.
            for (uint32_t k = 0; k < 3; ++k)
                put32(e, 0x0F4u + 4u * k,
                      get32(e, 0x1F8u + 4u * k) + static_cast<uint32_t>(c.rng->S32() >> 22));

            // Bit 0 of `+0x184` is the whole point of this row. With it SET the transform asks the
            // world - three calls to the unported query - and with it clear it takes the road
            // slice. A traced frame had it
            // clear on every bike, so a family that did not drive it deliberately would never
            // exercise the interesting half.
            put32(e, 0x184, (c.rng->Next() & 1u) ? (get32(e, 0x184) | 1u) : (get32(e, 0x184) & ~1u));
            put32(r, 0x184, (c.rng->Next() & 1u) ? (get32(r, 0x184) | 1u) : (get32(r, 0x184) & ~1u));

            // The two previous surface ids. The captured value is kept a third of the time so that
            // the `ret == prev` arm - the one that CLEARS flagsA bit 6 and leaves flagsB bit 25
            // clear - is reachable at all.
            auto prevId = [&](uint32_t captured) -> uint32_t {
                switch (c.rng->U32() % 3u) {
                    case 0: return captured;
                    case 1: return 0;
                    default: return c.rng->U32() & 0x07FFFFFFu;
                }
            };
            put32(e, 0x218, prevId(get32(e, 0x218)));
            put32(e, 0x23C, prevId(get32(e, 0x23C)));
            put32(r, 0x218, prevId(get32(r, 0x218)));

            // A third of the cases stop the machine, which is the only way into the creep arm at
            // 0x80075180 / 0x80075568 and the ported `Scale` under it.
            put32(e, rr::sim::ent::kSpeed,
                  (c.rng->Next() % 3u == 0)
                      ? 0u
                      : static_cast<uint32_t>(c.sample ? c.sample->speed : (c.rng->S32() >> 9)));
            put32(r, rr::sim::ent::kSpeed,
                  (c.rng->Next() % 3u == 0) ? 0u : static_cast<uint32_t>(c.rng->S32() >> 9));

            if (c.rng->Next() % 4u == 0) put32(e, 0x174, 0);
            if (c.rng->Next() % 4u == 0) put32(r, 0x174, 0);
            e[0x18A] = static_cast<uint8_t>(c.rng->U32());
            r[0x18A] = static_cast<uint8_t>(c.rng->U32());
            e[0x216] = static_cast<uint8_t>(c.rng->U32());
            r[0x216] = static_cast<uint8_t>(c.rng->U32());
            e[0x217] = static_cast<uint8_t>(c.rng->U32());
            r[0x217] = static_cast<uint8_t>(c.rng->U32());
            // The fallback normal at `+0x20A`, used whenever the surface is too steep or absent.
            for (uint32_t k = 0; k < 3; ++k)
                put16(e, 0x20Au + 2u * k, static_cast<uint16_t>(c.rng->Unit()));
            // Only bit 25 of flagsB is driven; the rest is left as the console had it, because the
            // query reads flag words of its own and this row is not trying to fuzz it.
            put32(e, rr::sim::ent::kFlagsB,
                  (get32(e, rr::sim::ent::kFlagsB) & ~0x02000000u) |
                      ((c.rng->Next() & 1u) ? 0x02000000u : 0u));
            // The normal buffer the first two calls share IS the entity field at +0x10C.
            for (uint32_t k = 0; k < 3; ++k)
                put16(e, 0x10Cu + 2u * k, static_cast<uint16_t>(c.rng->Unit()));

            put32(e, rr::sim::ent::kRider, (c.rng->Next() % 4u == 0) ? 0u : riderAddr);
            if (c.rng->Next() % 8u == 0) put32(e, 0x440, 0);
            put32(e, rr::sim::ent::kRiderDef, riderDef);

            std::vector<uint8_t> rd(72, 0);
            for (size_t i = 0; i < rd.size(); ++i) rd[i] = static_cast<uint8_t>(c.rng->U32());
            // 255 is "not placed", the one value that stops the creep arm.
            rd[0x27] = static_cast<uint8_t>((c.rng->Next() % 4u == 0) ? 255u : (c.rng->U32() % 20u));

            c.mem->WriteBlock(ea, e.data(), e.size());
            c.mem->WriteBlock(riderAddr, r.data(), r.size());
            c.mem->WriteBlock(riderDef, rd.data(), rd.size());

            // The three output buffers the ORIGINAL keeps in its own 120-byte frame. They are
            // uninitialised there and the callee may return -1 without writing them, so both sides
            // are given the same fill and an unwritten buffer compares equal instead of comparing
            // two different patches of old stack. Everything written here is inside the excluded
            // stack window on both machines.
            const uint32_t frameBase = c.sp - kGroundFrameSz;
            for (uint32_t i = 0; i < kGroundPointBytes; ++i) {
                const uint8_t a = GroundSlotFill(kGroundSlotA, i);
                const uint8_t b = GroundSlotFill(kGroundSlotB, i);
                c.mem->WriteBlock(frameBase + kGroundSlotA + i, &a, 1);
                c.mem->WriteBlock(frameBase + kGroundSlotB + i, &b, 1);
            }
            for (uint32_t i = 0; i < kGroundNormalBytes; ++i) {
                const uint8_t v = GroundSlotFill(kGroundSlotC, i);
                c.mem->WriteBlock(frameBase + kGroundSlotC + i, &v, 1);
            }

            args.a[0] = ea;
            // The original's own two reference points: `e+0x1F8` when `flagsC & 0x600` is clear at
            // the per-bike-step call site 0x80078BA0, `e+0xB8` otherwise and at 0x8007BEA8. The
            // first ALIASES this function's own output, which is why it is driven here.
            args.a[1] = ea + ((c.rng->Next() & 1u) ? 0x1F8u : 0x0B8u);
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const uint32_t ea = args.a[0];
            std::vector<uint8_t> e(1096), rider(1096);
            m.ReadBlock(ea, e.data(), e.size());
            const uint32_t riderAddr = m.PeekWord(ea + rr::sim::ent::kRider);
            if (riderAddr != 0) m.ReadBlock(riderAddr, rider.data(), rider.size());

            const uint32_t bufA = env.FrameOutBuffer(kGroundPointBytes);
            const uint32_t bufB = env.FrameOutBuffer(kGroundPointBytes);
            const uint32_t bufC = env.FrameOutBuffer(kGroundNormalBytes);
            if (bufA == 0 || bufB == 0 || bufC == 0) return 0u;

            rr::sim::BikeGroundFrameSlots slots;
            {
                uint8_t raw[kGroundPointBytes];
                for (uint32_t i = 0; i < kGroundPointBytes; ++i) raw[i] = GroundSlotFill(kGroundSlotA, i);
                std::memcpy(slots.pointA, raw, sizeof(slots.pointA));
                for (uint32_t i = 0; i < kGroundPointBytes; ++i) raw[i] = GroundSlotFill(kGroundSlotB, i);
                std::memcpy(slots.pointB, raw, sizeof(slots.pointB));
                uint8_t rawC[kGroundNormalBytes];
                for (uint32_t i = 0; i < kGroundNormalBytes; ++i) rawC[i] = GroundSlotFill(kGroundSlotC, i);
                std::memcpy(slots.normalC, rawC, sizeof(slots.normalC));
            }

            rr::sim::BikeGroundEnv ge;
            ge.frame = &slots;

            auto word = [](const std::vector<uint8_t>& v, uint32_t off) {
                return static_cast<uint32_t>(v[off]) | (static_cast<uint32_t>(v[off + 1]) << 8) |
                       (static_cast<uint32_t>(v[off + 2]) << 16) |
                       (static_cast<uint32_t>(v[off + 3]) << 24);
            };
            // Every pointer chase is the CALLER's, as everywhere else on this bench: the two
            // road slices, the list node, the rider and the place
            // byte are resolved here and handed over as values.
            auto slice = [&m](uint32_t addr) {
                rr::sim::BikeGroundSlice s;
                s.address = addr;
                if (addr >= 0x80000000u && addr < 0x80200000u) {
                    for (uint32_t k = 0; k < 3; ++k) {
                        s.row[k] = GetS16(m, addr + 8u + 2u * k);
                        s.origin[k] = GetS32(m, addr + 20u + 4u * k);
                    }
                }
                return s;
            };
            auto refresh = [&]() {
                ge.tracked = slice(word(e, 0x154));
                ge.ground = slice(word(e, 0x100));
                ge.listNode = word(e, 0x440);
                const uint32_t rdAddr = word(e, rr::sim::ent::kRiderDef);
                ge.riderDefPlace = (rdAddr >= 0x80000000u) ? m.PeekByte(rdAddr + 0x27u) : 0u;
                ge.rider = (riderAddr != 0) ? rider.data() : nullptr;
                ge.riderTracked = (riderAddr != 0) ? slice(word(rider, 0x154))
                                                   : rr::sim::BikeGroundSlice{};
            };

            struct OracleGround final : rr::sim::BikeGroundQuery {
                NativeEnv& env;
                std::vector<uint8_t>&e, &rider;
                uint32_t ea, riderAddr, bufA, bufB, bufC, mainRef;
                std::function<void()> refresh;
                OracleGround(NativeEnv& v, std::vector<uint8_t>& a, std::vector<uint8_t>& b,
                             uint32_t x, uint32_t y, uint32_t p, uint32_t q, uint32_t s, uint32_t mr)
                    : env(v), e(a), rider(b), ea(x), riderAddr(y), bufA(p), bufB(q), bufC(s),
                      mainRef(mr) {}
                int32_t Query(rr::sim::BikeGroundSite site, const int32_t* refPoint,
                              int32_t outPoint[3], int16_t outNormal[3], int32_t prevId) override {
                    Memory& mem = env.clone->mem;
                    // Commit what the port is holding, so the ORIGINAL runs on the CANDIDATE's own
                    // state with the arguments our code computed.
                    mem.WriteBlock(ea, e.data(), e.size());
                    if (riderAddr != 0) mem.WriteBlock(riderAddr, rider.data(), rider.size());
                    const bool third = (site == rr::sim::BikeGroundSite::kRider);
                    const uint32_t pointAddr =
                        (site == rr::sim::BikeGroundSite::kAlt) ? bufB : bufA;
                    // `a3` is the ENTITY field at +0x10C at the first two call sites and a second
                    // frame local only at the third. The bench passes exactly that, which is why
                    // the per-call argument comparison (see OracleFrameOut) still checks it at two sites.
                    const uint32_t normAddr = third ? bufC : (ea + 0x10Cu);
                    for (uint32_t k = 0; k < 3; ++k)
                        mem.PokeWord(pointAddr + 4u * k, static_cast<uint32_t>(outPoint[k]));
                    for (uint32_t k = 0; k < 3; ++k) PutS16(mem, normAddr + 2u * k, outNormal[k]);

                    uint32_t refAddr = 0;
                    if (site == rr::sim::BikeGroundSite::kMain) refAddr = mainRef;
                    else if (site == rr::sim::BikeGroundSite::kAlt) refAddr = ea + 0x0F4u;
                    // The reference point is a POINTER in the original and a value list in the
                    // port, so the bench resolves the address - and then checks that the port
                    // really computed the three words that live at it. Without this the seam would
                    // hand over the right reference point however wrong the port's own was.
                    if ((refAddr == 0) != (refPoint == nullptr)) {
                        if (env.failure.empty())
                            env.failure = "the port asked for the wrong reference point";
                    } else if (refPoint != nullptr) {
                        for (uint32_t k = 0; k < 3; ++k)
                            if (GetS32(mem, refAddr + 4u * k) != refPoint[k] && env.failure.empty())
                                env.failure = "the port's reference point is not the one the "
                                              "original passes at that call site";
                    }

                    const uint32_t a[5] = {third ? riderAddr : ea, refAddr, pointAddr, normAddr,
                                           static_cast<uint32_t>(prevId)};
                    // The query is the PORTED RASHCDG 0x800A7BF8 (ground.h, rows
                    // `ground_query` and below), run on the candidate's own memory with exactly the
                    // five arguments the original's call site passes - no oracle.
                    std::vector<uint16_t> rsqrt(2048);
                    mem.ReadBlock(mem.PeekWord(env.gp + kRsqrtTableGpOffset), rsqrt.data(), rsqrt.size() * 2u);
                    rr::sim::GuestRam gv(mem.ram().data(), env.gp);
                    const rr::sim::GroundResult gr =
                        rr::sim::GroundQuery(gv, a[0], a[1], a[2], a[3], a[4], rsqrt.data());
                    if ((gr.declined || gv.Faulted()) && env.failure.empty())
                        env.failure = "the ported ground query declined or faulted";
                    const int32_t r = static_cast<int32_t>(gr.value);
                    mem.ReadBlock(ea, e.data(), e.size());
                    if (riderAddr != 0) mem.ReadBlock(riderAddr, rider.data(), rider.size());
                    for (uint32_t k = 0; k < 3; ++k)
                        outPoint[k] = static_cast<int32_t>(mem.PeekWord(pointAddr + 4u * k));
                    for (uint32_t k = 0; k < 3; ++k)
                        outNormal[k] = GetS16(mem, normAddr + 2u * k);
                    refresh(); // the callee may have moved anything the caller resolved
                    return r;
                }
            } ground(env, e, rider, ea, riderAddr, bufA, bufB, bufC, args.a[1]);
            ground.refresh = refresh;
            ge.query = &ground;
            refresh();

            int32_t ref[3];
            const uint32_t refOff = args.a[1] - ea;
            for (uint32_t k = 0; k < 3; ++k)
                ref[k] = static_cast<int32_t>(word(e, refOff + 4u * k));

            rr::sim::BikeGroundFrame(rr::sim::EntityView(e.data()), ref, ge);
            m.WriteBlock(ea, e.data(), e.size());
            if (riderAddr != 0) m.WriteBlock(riderAddr, rider.data(), rider.size());
            return 0u;
        },
        /*oracleCallees=*/
        {}, // no oracle-supplied callee since the query is ported (was 0x800A7BF8, 4 + 1 args)
        /*oracleFrame=*/kGroundFrameSz,
        /*maxSteps=*/20'000'000});

    // ================================================================ the integrator
    //
    // RASHCDG 0x8007F0BC and 0x8007FA4C, two callees of the per-bike step, and the ten functions
    // under them. Leaves first.

    // `s32 Asin(s32 x)`. ONE NAMED BOUND: x != INT32_MIN, the one input whose `negu` leaves it
    // negative and whose table index lands half a megabyte below the table (the port refuses it).
    rows.push_back(Row{
        "asin", "s32 Asin(s32 x)  [SLUS, 16.16 -> 4096 per turn, the 61-entry table]", kAsinFn,
        [](CaseContext& c, Args& args) {
            int32_t x;
            if (c.sample) {
                const int16_t v = c.sample->mA[c.rng->Next() % 9u];
                x = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(v)) << 4);
                if (c.rng->Next() % 3u == 0) x = c.sample->speed >> 4;
            } else {
                static const int32_t kEdges[] = {0x7FFF, 0x8000, 0xBFFF, 0xC000, 0xFFF0, 0xFFF7,
                                                 0xFFF8, 0xFFFE, 0xFFFF, 0x10000, 0, 1, 0x1FFF, 0x2000};
                switch (c.rng->U32() % 4u) {
                    case 0: x = kEdges[c.rng->U32() % 14u]; break;
                    case 1: x = c.rng->S32(); break;
                    default: x = static_cast<int32_t>(c.rng->U32() % 0x22000u) - 0x11000; break;
                }
                if (c.rng->Next() & 1u) x = -x;
                if (x == INT32_MIN) x = INT32_MAX;
            }
            args.a[0] = static_cast<uint32_t>(x);
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            uint16_t t[kAsinEntries];
            m.ReadBlock(kAsinTable, t, sizeof(t));
            int32_t out = 0;
            if (!rr::sim::Asin(static_cast<int32_t>(args.a[0]), t, out)) return 0xDEADDEADu;
            return static_cast<uint32_t>(out);
        }});

    // `MATRIX *RotMatrix(const SVECTOR *angles, MATRIX *m)` - returns `m` in v0.
    rows.push_back(Row{
        "rot_matrix", "MATRIX *RotMatrix(const s16 ang[3], s16 m[9])  [SLUS, Rz*Ry*Rx]", kRotMatrixFn,
        [](CaseContext& c, Args& args) {
            const IntegratorLayout L = IntegratorAddresses(c.scratch);
            int16_t a[3];
            for (int i = 0; i < 3; ++i) {
                if (c.sample) a[i] = c.sample->mB[c.rng->Next() % 9u];
                else a[i] = c.rng->S16();
                if (c.rng->Next() % 6u == 0) a[i] = static_cast<int16_t>(-4096 + (c.rng->U32() % 3u) * 4096);
            }
            PutVec3S16(*c.mem, L.aux, a);
            for (uint32_t k = 0; k < 20; k += 4) c.mem->PokeWord(L.aux + 16u + k, c.rng->U32());
            args.a[0] = L.aux;
            args.a[1] = L.aux + 16u;
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            std::vector<int16_t> sc(8192);
            m.ReadBlock(kSinCosTable, sc.data(), sc.size() * 2u);
            int16_t a[3], out[9];
            GetVec3S16(m, args.a[0], a);
            rr::sim::RotMatrix(a, out, sc.data());
            for (uint32_t i = 0; i < 9; ++i) PutS16(m, args.a[1] + 2u * i, out[i]);
            return args.a[1];
        }});

    // `void MulMatrix0(const MATRIX *a, const MATRIX *b, MATRIX *out)` on the GTE. A fifth of the
    // cases alias `out` with `b` and a tenth with `a`, the two ways a caller could.
    rows.push_back(Row{
        "mul_matrix0", "void MulMatrix0(const s16 a[9], const s16 b[9], s16 out[9])  [SLUS, GTE MVMVA]",
        kMulMatrix0Fn,
        [](CaseContext& c, Args& args) {
            const IntegratorLayout L = IntegratorAddresses(c.scratch);
            for (uint32_t i = 0; i < 9; ++i) {
                int16_t av, bv;
                if (c.sample) { av = c.sample->mA[i]; bv = c.sample->mB[i]; }
                else if (c.rng->Next() % 2u) { av = c.rng->Unit(); bv = c.rng->Unit(); }
                else { av = c.rng->S16(); bv = c.rng->S16(); }
                PutS16(*c.mem, L.aux + 2u * i, av);
                PutS16(*c.mem, L.aux + 32u + 2u * i, bv);
            }
            PutS16(*c.mem, L.aux + 18u, c.rng->S16()); // R33's high half: loaded, never used
            for (uint32_t k = 0; k < 20; k += 4) c.mem->PokeWord(L.aux + 64u + k, c.rng->U32());
            args.a[0] = L.aux;
            args.a[1] = L.aux + 32u;
            const uint64_t roll = c.rng->Next() % 10u;
            args.a[2] = (roll < 2) ? args.a[1] : ((roll == 2) ? args.a[0] : L.aux + 64u);
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            int16_t a[9], b[9], out[9];
            for (uint32_t i = 0; i < 9; ++i) {
                a[i] = GetS16(m, args.a[0] + 2u * i);
                b[i] = GetS16(m, args.a[1] + 2u * i);
                out[i] = GetS16(m, args.a[2] + 2u * i);
            }
            rr::sim::MulMatrix0(a, b, out);
            for (uint32_t i = 0; i < 9; ++i) PutS16(m, args.a[2] + 2u * i, out[i]);
            return 0u;
        },
        /*compareV0=*/false});

    // `s32 Normalize32(s32 v[3])`, 16.16 in place. Returns what the original leaves in v0.
    rows.push_back(Row{
        "normalize32", "s32 Normalize32(s32 v[3])  [SLUS, 16.16, GTE LZCS + the rsqrt table]",
        kNormalize32Fn,
        [](CaseContext& c, Args& args) {
            const IntegratorLayout L = IntegratorAddresses(c.scratch);
            int32_t v[3];
            for (int i = 0; i < 3; ++i) {
                if (c.sample) v[i] = c.sample->p[i] - c.sample->q[i];
                else if (c.rng->Next() % 3u == 0) v[i] = c.rng->S32();
                else v[i] = c.rng->World();
            }
            const uint64_t roll = c.rng->Next() % 16u;
            if (roll == 0) { v[0] = 0; v[1] = 0; v[2] = 0; }
            else if (roll == 1) v[c.rng->Next() % 3u] = INT32_MIN;
            else if (roll == 2) { v[0] = 1; v[1] = 0; v[2] = 0; }
            PutVec3S32(*c.mem, L.aux, v);
            args.a[0] = L.aux;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            std::vector<uint16_t> table(2048);
            m.ReadBlock(m.PeekWord(gp + kRsqrtTableGpOffset), table.data(), table.size() * 2u);
            int32_t v[3];
            GetVec3S32(m, args.a[0], v);
            const int32_t r = rr::sim::Normalize32(v, table.data());
            PutVec3S32(m, args.a[0], v);
            return static_cast<uint32_t>(r);
        }});

    // `void ReleaseContact(Bike *e)`. The contact record's handle is drawn over pools 0, 3, 4, the
    // two it ignores and junk, so every arm - including "nothing" - is taken.
    rows.push_back(Row{
        "release_contact", "void ReleaseContact(Bike *e)  [SLUS, what leaving a contact does to it]",
        kReleaseContactFn,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantIntegratorCase(c, IntegratorMode::kRelease, false).e;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            IntegratorHarness h(env.clone->mem, args.a[0], env.gp);
            if (!rr::sim::ReleaseContact(h.Entity(), h.links.contact)) {
                env.failure = "the port declined a case it cannot resolve";
                return 0u;
            }
            h.Commit();
            return 0u;
        }});

    rows.push_back(Row{
        "bike_crash_launch", "void BikeCrashLaunch(Bike *e)  [RASHCDG, the first frame of a crash]",
        kCrashLaunchFn,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantIntegratorCase(c, IntegratorMode::kCrashLaunch, false).e;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            IntegratorHarness h(env.clone->mem, args.a[0], env.gp);
            if (!rr::sim::BikeCrashLaunch(h.Entity(), h.links, h.tables)) {
                env.failure = "the port declined a case it cannot resolve";
                return 0u;
            }
            h.Commit();
            return 0u;
        }});

    rows.push_back(Row{
        "bike_wipeout_start", "void BikeWipeoutStart(Bike *e)  [RASHCDG, the four wipeout kinds]",
        kWipeoutStartFn,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantIntegratorCase(c, IntegratorMode::kWipeout, false).e;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            IntegratorHarness h(env.clone->mem, args.a[0], env.gp);
            if (!rr::sim::BikeWipeoutStart(h.Entity(), h.links)) {
                env.failure = "the port declined a case it cannot resolve";
                return 0u;
            }
            h.Commit();
            return 0u;
        }});

    // `void BikeIntegrate(Bike *e, s32 dt)` - the velocity integrator, and the active-list append
    // through the scratchpad cursor at 0x1F800000. No seam: all of its callees are ported.
    rows.push_back(Row{
        "bike_integrate", "void BikeIntegrate(Bike *e, s32 dt)  [RASHCDG 0x8007F0BC, the integrator]",
        kIntegrateFn,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantIntegratorCase(c, IntegratorMode::kIntegrate, false).e;
            // 0x884 is the delta the traced frame carried.
            const uint64_t roll = c.rng->Next() % 4u;
            args.a[1] = (roll == 0) ? 0x884u : ((roll == 1) ? (c.rng->U32() >> 12) : (c.rng->U32() % 0x2000u));
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            IntegratorHarness h(m, args.a[0], env.gp);
            rr::sim::BikeActiveList list;
            list.cursor = m.PeekWord(kActiveCursor);
            uint8_t slot[4];
            m.ReadBlock(list.cursor, slot, 4);
            list.slot = slot;
            if (!rr::sim::BikeIntegrate(h.Entity(), static_cast<int32_t>(args.a[1]), args.a[0], h.links,
                                        h.tables, list)) {
                env.failure = "the port declined a case it cannot resolve";
                return 0u;
            }
            h.Commit();
            if (list.pushed) {
                // The caller resolved the cursor; it writes the four bytes back where they live and
                // the advanced cursor into the scratchpad word.
                m.WriteBlock(list.cursor - 4u, slot, 4);
                m.PokeWord(kActiveCursor, list.cursor);
            }
            return 0u;
        }});

    // `s32 BikeObstacleTest(Bike *e, const s16 n[3], s32 depth)` - wall or slope.
    rows.push_back(Row{
        "bike_obstacle_test", "s32 BikeObstacleTest(Bike *e, const s16 n[3], s32 depth)  [RASHCDG]",
        kObstacleTestFn,
        [](CaseContext& c, Args& args) {
            const IntegratorLayout L = PlantIntegratorCase(c, IntegratorMode::kObstacle, false);
            args.a[0] = L.e;
            // The two normals the contact frame passes, or a planted one.
            const uint64_t roll = c.rng->Next() % 3u;
            if (roll == 2) {
                const int16_t n[3] = {c.rng->Unit(), c.rng->Unit(), c.rng->Unit()};
                PutVec3S16(*c.mem, L.aux, n);
                args.a[1] = L.aux;
            } else {
                args.a[1] = L.e + (roll == 0 ? 0x112u : 0x10Cu);
            }
            args.a[2] = static_cast<uint32_t>(c.rng->S32() >> 13);
            if (c.rng->Next() % 3u == 0) args.a[2] = 0x8000u + (c.rng->U32() % 3u) - 1u;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            IntegratorHarness h(m, args.a[0], gp);
            int16_t n[3];
            GetVec3S16(m, args.a[1], n);
            int32_t r = 0;
            if (!rr::sim::BikeObstacleTest(h.Entity(), n, static_cast<int32_t>(args.a[2]), h.links,
                                           h.tables, r))
                return 0xDEADDEADu;
            h.Commit();
            return static_cast<uint32_t>(r);
        }});

    // `void BikeContactFrame(Bike *e)` - the contact response. Its one callee outside this file, the
    // road re-bind `SLUS 0x800374D4(e, flag)`, is ported with everything under it and
    // runs natively at the original's call depth (the contact frame's sp, `oracleFrame`). BOUNDED
    // INPUT FAMILY, named, and the same bound `bike_ground_frame` carries: the entity is a live
    // pool-0 bike of THIS machine with every scalar randomised, because the re-bind walks the
    // machine's own road through the entity's road cursor.
    rows.push_back(Row{
        "bike_contact_frame", "void BikeContactFrame(Bike *e)  [RASHCDG 0x8007FA4C, the contact response]",
        kContactFrameFn,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantIntegratorCase(c, IntegratorMode::kContact, true).e;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            IntegratorHarness h(m, args.a[0], env.gp);
            struct NativeRebind final : rr::sim::BikeRoadRebind {
                NativeEnv& env;
                IntegratorHarness& h;
                uint32_t ea;
                NativeRebind(NativeEnv& v, IntegratorHarness& hh, uint32_t a) : env(v), h(hh), ea(a) {}
                void Rebind(int32_t flag) override {
                    // Commit what the port holds, run the PORTED 0x800374D4 on guest memory at the
                    // stack pointer the original calls it at, and read back what it changed.
                    h.Commit();
                    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
                    g.SetScratchpad(env.clone->mem.scratchpad().data());
                    rr::sim::RoadRuntimeNative six;
                    rr::sim::RoadRebind(g, ea, flag, env.sp, six);
                    RqDone(env, g, 0u);
                    h.Reread();
                }
            } rebind(env, h, args.a[0]);
            if (!rr::sim::BikeContactFrame(h.Entity(), h.links, h.tables, rebind)) {
                if (env.failure.empty()) env.failure = "the port declined a case it cannot resolve";
                return 0u;
            }
            h.Commit();
            return 0u;
        },
        /*oracleCallees=*/{},
        /*oracleFrame=*/kContactFrameSz,
        /*maxSteps=*/20'000'000});

    // ---------------------------------------------------------------- two more closed trees of the
    // per-bike step. No seam, no unported callee.

    // `s32 BikeGripLimit(const u8 *stats, s32 a, s32 b, s32 c)`, RASHCDG 0x8007EF60.
    rows.push_back(Row{
        "bike_grip_limit", "s32 BikeGripLimit(const u8 *stats, s32 a, s32 b, s32 c)  [RASHCDG 0x8007EF60]",
        kGripLimitFn,
        [](CaseContext& c, Args& args) {
            const uint32_t stats = c.scratch + 64u;
            for (uint32_t i = 0; i < 256; i += 4) c.mem->PokeWord(stats + i, c.rng->U32());
            // +0xC0 scales `b`, +0xC8 divides; zero and negative divisors take the idiom's other arms.
            const uint64_t roll = c.rng->Next() % 4u;
            PutS32(*c.mem, stats + 192, c.rng->S32() >> (8 + c.rng->Next() % 12u));
            PutS32(*c.mem, stats + 200, (roll == 0) ? 0 : (c.rng->S32() >> (8 + c.rng->Next() % 12u)));
            args.a[0] = stats;
            for (int k = 1; k < 4; ++k) {
                int32_t v;
                if (c.sample) {
                    const Sample& s = *c.sample;
                    v = (k == 1) ? static_cast<int32_t>(GetU32V(s.entity, 0x2E0))
                                 : ((k == 2) ? static_cast<int32_t>(GetU32V(s.entity, 0x28C)) : s.speed);
                    if (c.rng->Next() % 3u == 0) v = c.rng->S32() >> (6 + c.rng->Next() % 14u);
                } else {
                    v = c.rng->S32() >> (c.rng->Next() % 20u);
                }
                args.a[k] = static_cast<uint32_t>(v);
            }
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            std::vector<uint8_t> stats(256);
            m.ReadBlock(args.a[0], stats.data(), stats.size());
            std::vector<int16_t> w(kSqrtWindowHalf);
            m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, w.data(), w.size() * 2u);
            return static_cast<uint32_t>(rr::sim::BikeGripLimit(
                stats.data(), static_cast<int32_t>(args.a[1]), static_cast<int32_t>(args.a[2]),
                static_cast<int32_t>(args.a[3]), w.data() + kSqrtWindowHalf / 4u));
        }});

    // `s32 BikePassengerSteer(Bike *e, s32 dt)`, RASHCDG 0x80072C7C. The handle is kept under 60 so
    // the original's unbounded index into 0x800CE540 stays inside the 1 KiB window the port gets
    // (the same bound `bike_aim_target` carries); the port refuses anything past it.
    rows.push_back(Row{
        "bike_passenger_steer", "s32 BikePassengerSteer(Bike *e, s32 dt)  [RASHCDG 0x80072C7C]",
        kPassengerSteerFn,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            std::vector<uint8_t> e = src.entity;
            if (e.size() < 1096) e.resize(1096, 0);
            std::vector<uint8_t> p = c.any->entity;
            if (p.size() < 1096) p.resize(1096, 0);
            const uint32_t pa = c.scratch + 1104u, stats = c.scratch + 2208u;
            for (uint32_t i = 0; i < 512; i += 4) c.mem->PokeWord(stats + i, c.rng->U32());
            PutS32(*c.mem, stats + 428, c.rng->S32() >> (8 + c.rng->Next() % 10u));
            PutS32(*c.mem, stats + 432, c.rng->S32() >> (8 + c.rng->Next() % 10u));
            PutU32V(e, rr::sim::ent::kRider, pa);
            PutU32V(e, rr::sim::ent::kStats, stats);
            PutU16V(e, rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() % 60u));
            const uint64_t sroll = c.rng->Next() % 4u;
            PutU32V(e, rr::sim::ent::kSpeedCopy,
                    (sroll == 0) ? 0u : ((sroll == 1) ? 0x8000u + (c.rng->U32() % 3u) - 1u
                                                      : static_cast<uint32_t>(c.rng->S32() >> 8)));
            PutU32V(p, 720, (c.rng->Next() % 6u == 0) ? c.rng->U32() : 0u);
            uint32_t pf = c.rng->U32();
            if (c.rng->Next() & 1u) pf &= ~0x300u;
            if (c.rng->Next() & 1u) pf |= 0x100000u; else pf &= ~0x100000u;
            PutU32V(p, 560, pf);
            const uint64_t proll = c.rng->Next() % 4u;
            PutU32V(p, 672, (proll == 0) ? 0u : static_cast<uint32_t>(c.rng->S32() >> (12 + c.rng->Next() % 8u)));
            // The target word the handle selects: sometimes equal to the current value.
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            const uint32_t players = c.rng->U32() % 4u;
            c.mem->PokeWord(gs + 0x30, players);
            const uint32_t h = GetU32V(e, rr::sim::ent::kHandle) & 0xFFFFu;
            const uint32_t idx = (players < 2u) ? h + 1u : h + 2u;
            c.mem->PokeWord(kHandleTable + 8u * idx + 4u,
                            (c.rng->Next() % 4u == 0) ? GetU32V(p, 672)
                                                      : static_cast<uint32_t>(c.rng->S32() >> (12 + c.rng->Next() % 8u)));
            c.mem->WriteBlock(c.scratch, e.data(), e.size());
            c.mem->WriteBlock(pa, p.data(), p.size());
            args.a[0] = c.scratch;
            args.a[1] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 16u));
        },
        [](Memory& m, uint32_t scratch, uint32_t, const Args& args) {
            std::vector<uint8_t> e(1096), p(1096), stats(512), table(1024);
            m.ReadBlock(scratch, e.data(), e.size());
            rr::sim::EntityView ev(e.data());
            const uint32_t pa = ev.U32(rr::sim::ent::kRider);
            m.ReadBlock(pa, p.data(), p.size());
            m.ReadBlock(ev.U32(rr::sim::ent::kStats), stats.data(), stats.size());
            m.ReadBlock(kHandleTable, table.data(), table.size());
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            int32_t out = 0;
            if (!rr::sim::BikePassengerSteer(ev, static_cast<int32_t>(args.a[1]), p.data(), stats.data(),
                                             m.PeekWord(gs + 0x30), table.data(),
                                             static_cast<uint32_t>(table.size()), out))
                return 0xDEADDEADu;
            m.WriteBlock(pa, p.data(), p.size());
            return static_cast<uint32_t>(out);
        }});

    // `void BikeSteerPass(list, s32 dt)`, RASHCDG 0x80074C84: three bikes on a real circular list,
    // two owner records (so bit 4 of `+0x23C` differs between bikes), a passenger on most bikes.
    rows.push_back(Row{
        "bike_steer_pass", "void BikeSteerPass(list, s32 dt)  [RASHCDG 0x80074C84, region B]",
        kSteerPassFn,
        [](CaseContext& c, Args& args) {
            const Sample& src = c.sample ? *c.sample : *c.any;
            const uint32_t head = c.scratch;
            const uint32_t stats = c.scratch + 6656u;
            const uint32_t ownerA = c.scratch + 7168u, ownerB = c.scratch + 7424u;
            const uint32_t riderDef = c.scratch + 8;
            for (uint32_t i = 0; i < 512; i += 4) c.mem->PokeWord(stats + i, 0);
            const bool tame = (c.rng->Next() & 1u) != 0;
            for (uint32_t off : {224u, 232u, 308u, 312u, 316u, 356u, 360u, 364u, 368u, 372u, 376u,
                                 380u, 384u, 416u, 428u, 432u})
                PutS32(*c.mem, stats + off, tame ? (c.rng->S32() >> 10) : c.rng->S32());
            PutS32(*c.mem, stats + 4, c.rng->S32() >> 12);
            PutS32(*c.mem, stats + 228, c.rng->S32() >> 10);
            PutS32(*c.mem, stats + 240, c.rng->S32() >> 10);
            PutS32(*c.mem, stats + 280, c.rng->S32() >> 14);
            PutS32(*c.mem, stats + 284, c.rng->S32() >> 14);
            PutS16(*c.mem, stats + 446, (c.rng->Next() % 3u == 0) ? c.rng->S16() : static_cast<int16_t>(0));
            // Owner B's `+0x23C` never has bit 4, and it is the owner every riderless bike gets:
            // with bit 4 and no passenger the original's `lw v0,720(s0)` at 0x80072CA0 reads guest
            // 0x2D0, which the port refuses.
            for (uint32_t o : {ownerA, ownerB}) {
                uint8_t ob = static_cast<uint8_t>(c.rng->U32());
                if (o == ownerB) ob &= static_cast<uint8_t>(~0x10u);
                c.mem->WriteBlock(o + 0x23C, &ob, 1);
            }
            {
                const uint8_t d0 = static_cast<uint8_t>(c.rng->U32());
                c.mem->WriteBlock(riderDef, &d0, 1);
            }
            const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
            c.mem->PokeWord(gs + 0x30, c.rng->U32() % 4u);
            for (uint32_t i = 0; i < 3; ++i) {
                const uint32_t ea = c.scratch + 16u + 1104u * i;
                const uint32_t ra = c.scratch + 3328u + 1104u * i;
                std::vector<uint8_t> e = src.entity;
                if (e.size() < 1096) e.resize(1096, 0);
                std::vector<uint8_t> rider(1096, 0);
                PutU32V(e, rr::sim::ent::kStats, stats);
                const bool riderless = c.rng->Next() % 5u == 0;
                PutU32V(e, rr::sim::ent::kOwner, (!riderless && (c.rng->Next() & 1u)) ? ownerA : ownerB);
                PutU32V(e, rr::sim::ent::kRider, riderless ? 0u : ra);
                PutU32V(e, rr::sim::ent::kRiderDef, riderDef);
                PutU16V(e, rr::sim::ent::kHandle, static_cast<uint16_t>(c.rng->U32() % 60u));
                uint32_t flagsA = c.rng->U32();
                if (c.rng->Next() & 1u) flagsA |= 0x80000u;
                if (c.rng->Next() & 1u) flagsA = (flagsA & ~0x108000u) | 0x8000u;
                PutU32V(e, rr::sim::ent::kFlagsA, flagsA);
                PutU32V(e, rr::sim::ent::kFlagsB, c.rng->U32());
                PutU32V(e, rr::sim::ent::kFlagsC, c.rng->U32());
                PutU32V(e, rr::sim::ent::kSpeed, (c.rng->Next() & 1u) ? 0u : (c.rng->U32() >> 8));
                PutU32V(e, rr::sim::ent::kSpeedCopy, (c.rng->Next() % 3u == 0) ? 0u : (c.rng->U32() >> 8));
                for (uint32_t off : {rr::sim::ent::kSteer, rr::sim::ent::kSteerPhase, rr::sim::ent::kSteerRate,
                                     rr::sim::ent::kSteerSpan, rr::sim::ent::kSteerFrom,
                                     rr::sim::ent::kSteerScratch, rr::sim::ent::kDriftLimit,
                                     rr::sim::ent::kLatAccel, rr::sim::ent::kIdleLean,
                                     rr::sim::ent::kSteerBias, rr::sim::ent::kLeanBias, rr::sim::ent::kLeanGain})
                    PutU32V(e, off, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
                e[rr::sim::ent::kMode] = static_cast<uint8_t>(c.rng->U32() % 4u);
                // +0x440 is the node's first word; 0x80074D10 tests it for zero as part of "has a
                // rider". A fifth of the bikes carry a zero there.
                if (c.rng->Next() % 5u == 0) PutU32V(e, 0x440, 0);
                else if (GetU32V(e, 0x440) == 0) PutU32V(e, 0x440, 0x80000001u);
                PutU32V(e, 0x444, (i == 2) ? head : (c.scratch + 16u + 1104u * (i + 1) + 0x440u));
                // The passenger's own fields (0x80072C7C).
                PutU32V(rider, 720, (c.rng->Next() % 6u == 0) ? c.rng->U32() : 0u);
                uint32_t pf = c.rng->U32();
                if (c.rng->Next() & 1u) pf &= ~0x300u;
                PutU32V(rider, 560, pf);
                PutU32V(rider, 672, static_cast<uint32_t>(c.rng->S32() >> (12 + c.rng->Next() % 8u)));
                PutU32V(rider, rr::sim::ent::kLatAccel, static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 14u)));
                c.mem->WriteBlock(ea, e.data(), e.size());
                c.mem->WriteBlock(ra, rider.data(), rider.size());
            }
            c.mem->PokeWord(head + 0, 0);
            c.mem->PokeWord(head + 4, c.scratch + 16u + 0x440u);
            args.a[0] = head;
            args.a[1] = c.sample ? 0x884u : static_cast<uint32_t>(c.rng->S32() >> (c.rng->Next() % 16u));
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& nenv, const Args& args) -> uint32_t {
            Memory& m = nenv.clone->mem;
            std::vector<uint8_t> handleTable(1024);
            m.ReadBlock(kHandleTable, handleTable.data(), handleTable.size());
            std::vector<int16_t> sincos(8192);
            m.ReadBlock(kSinCosTable, sincos.data(), sincos.size() * 2u);
            int32_t atan[20];
            for (int i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * static_cast<uint32_t>(i));
            GuestMirror mir(m);
            std::vector<rr::sim::BikeSteerPassNode> nodes;
            uint32_t node = m.PeekWord(args.a[0] + 4);
            while (node != args.a[0] && nodes.size() < 32) {
                const uint32_t ea = node - 1088u;
                uint8_t* e = mir.Map(ea, 1096, true);
                if (e == nullptr) {
                    nenv.failure = "the list walked off guest RAM";
                    return 0u;
                }
                rr::sim::EntityView v(e);
                rr::sim::BikeSteerPassNode n{v};
                n.ownerFlagByte = m.PeekByte(v.U32(rr::sim::ent::kOwner) + 0x23Cu);
                const uint32_t ra = v.U32(rr::sim::ent::kRider);
                n.passenger = (ra != 0) ? mir.Map(ra, 1096, true) : nullptr;
                n.stats = mir.Map(v.U32(rr::sim::ent::kStats), 512, false);
                n.riderDefByte0 = m.PeekByte(v.U32(rr::sim::ent::kRiderDef));
                nodes.push_back(n);
                node = m.PeekWord(node + 4);
            }
            rr::sim::BikeSteerPassEnv env;
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            env.numPlayers = m.PeekWord(gs + 0x30);
            env.handleTable = handleTable.data();
            env.handleTableBytes = static_cast<uint32_t>(handleTable.size());
            env.atan = atan;
            env.sincos = sincos.data();
            if (!rr::sim::BikeSteerPass(nodes.data(), nodes.size(), static_cast<int32_t>(args.a[1]), env)) {
                nenv.failure = "the port declined a case it cannot resolve";
                return 0u;
            }
            mir.Commit();
            return 0u;
        }});

    // ================================================================ the sound emitter
    //
    // The ten functions the whole race speaks to the sound
    // system through. All of them are in SLUS_010.53 and all of them are resident on this very
    // machine, so none of these rows needs a second snapshot.
    //
    // `play_sound_3d` is the point of the exercise. It supersedes the diagnostic row
    // `sound_emitter_is_invisible` below, and it is why `bike_engine` declares no
    // oracle-supplied callee: the emitter is OURS, and the 17 to 19 bytes it was
    // measured changing - the serial counter at 0x8005B4A0 plus the voice bookkeeping - are
    // bytes the port has to reproduce exactly rather than bytes the bench has to work around.
    //
    // ONE NAMED BOUND applies to `start_voice` and to everything above it: `restart` is held at 0.
    // A non-zero `restart` is the only thing that reaches `SLUS 0x80050678` -> `0x800506A8`, and
    // with `*(0x8005A408) & 1 == 0` - its value in every capture - that function does an `lhu`
    // from `*(0x8005A41C) + 2*204 = 0x1F801D98`. `Cpu::spuWritesAreDropped`
    // drops SPU STORES only; every SPU load still traps, deliberately, so that arm cannot be put
    // on the oracle seam either - the bench would stop rather than answer. `PlaySound3D` passes
    // `restart = 0` at 0x80017C98 on every one of its 46 call sites, so nothing the race plays
    // reaches it; the port implements the arm and the bench does not drive it, and that is stated
    // here rather than hidden.

    rows.push_back(Row{
        "key_on", "void KeyOn(u32 mask)  [SLUS, S+0x14 |= mask]", kSoundKeyOn,
        [](CaseContext& c, Args& args) {
            PutS32(*c.mem, kSoundSys + 0x14, static_cast<int32_t>(c.rng->U32()));
            args.a[0] = (c.rng->Next() % 4u == 0) ? 0u : c.rng->U32();
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            std::vector<uint8_t> s(kSoundSysBytes);
            m.ReadBlock(kSoundSys, s.data(), s.size());
            rr::sim::SoundKeyOn(rr::sim::SoundBytes(s.data(), kSoundSysBytes), args.a[0]);
            m.WriteBlock(kSoundSys, s.data(), s.size());
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "key_off", "void KeyOff(u32 mask)  [SLUS, S+0x18 |= mask]", kSoundKeyOff,
        [](CaseContext& c, Args& args) {
            PutS32(*c.mem, kSoundSys + 0x18, static_cast<int32_t>(c.rng->U32()));
            args.a[0] = (c.rng->Next() % 4u == 0) ? 0u : c.rng->U32();
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            std::vector<uint8_t> s(kSoundSysBytes);
            m.ReadBlock(kSoundSys, s.data(), s.size());
            rr::sim::SoundKeyOff(rr::sim::SoundBytes(s.data(), kSoundSysBytes), args.a[0]);
            m.WriteBlock(kSoundSys, s.data(), s.size());
            return 0u;
        },
        /*compareV0=*/false});

    // `u8 *LookupSound(Bank *b, int i)`. It looks 36 bytes long, but the function
    // really runs to 0x8001E8BC (80 bytes) - `bgez a1,0x8001E890` at 0x8001E880 jumps FORWARD over
    // the `return 0`, and the offset lookup and the null test are on the other side of it. The
    // 36-byte reading stops at that jump.
    rows.push_back(Row{
        "lookup_sound", "u8 *LookupSound(Bank *b, int i)  [SLUS, a pure leaf over game data]",
        kLookupSound,
        [](CaseContext& c, Args& args) {
            uint32_t bank;
            if (c.sample) {
                // The machine's own resident bank 0 - 110 sounds, the bank every `PlaySound3D`
                // index on this disc addresses.
                const uint32_t table = static_cast<uint32_t>(GetS32(*c.mem, kSoundSys + 0x04));
                bank = (table != 0) ? static_cast<uint32_t>(GetS32(*c.mem, table)) : 0u;
                if (bank < 0x80000000u || bank >= 0x80200000u) bank = 0;
            } else {
                bank = 0;
            }
            if (bank == 0) {
                bank = c.scratch + 64u;
                PlantBank(c, bank, 768);
            }
            args.a[0] = bank;
            const uint64_t roll = c.rng->Next() % 8u;
            if (roll == 0) args.a[1] = static_cast<uint32_t>(c.rng->S32()); // the whole range
            else if (roll == 1) args.a[1] = static_cast<uint32_t>(-1 - static_cast<int32_t>(c.rng->U32() % 64u));
            else args.a[1] = c.rng->U32() % 140u;
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            std::vector<uint8_t> window(kBankWindow);
            m.ReadBlock(args.a[0], window.data(), window.size());
            rr::sim::SoundBankRef b;
            b.address = args.a[0];
            b.data = window.data();
            b.size = kBankWindow;
            bool ok = true;
            const uint32_t off = rr::sim::LookupSound(b, static_cast<int32_t>(args.a[1]), ok);
            if (!ok) return 0xDEADBEEFu; // the window was too small: fail the case loudly
            return (off != 0) ? (b.address + off) : 0u;
        }});

    // `Voice *AllocVoice(int reserved)`. THIS row shows that the one word
    // outside the sound-system record that it touches is the serial counter at gp+2068 =
    // 0x8005B4A0, and it steps by exactly one. The LCG seed eight bytes above it at gp+2076 is
    // never read and never written, here or anywhere else in the emitter's tree.
    rows.push_back(Row{
        "alloc_voice", "Voice *AllocVoice(int reserved)  [SLUS, the free stack + the steal ring]",
        kAllocVoice,
        [](CaseContext& c, Args& args) {
            const uint32_t voices = static_cast<uint32_t>(GetS32(*c.mem, kSoundSys + 0x0C));
            if (!c.sample) PlantSoundState(c, voices);
            args.a[0] = (c.rng->Next() % 3u == 0) ? 1u : 0u;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            SoundMirror mirror(m, gp);
            const int32_t v = rr::sim::AllocVoice(static_cast<int32_t>(args.a[0]), mirror.System());
            mirror.Commit();
            if (mirror.outOfWindow) return 0xDEADBEEFu;
            if (v < 0) return 0u;
            return static_cast<uint32_t>(GetS32(m, kSoundSys + 0x0C)) +
                   kVoiceBytes * static_cast<uint32_t>(v);
        }});

    // `void Sound3DParams(int p, s32 x, s32 z, s32 vx2, s32 vz2, s32 *vol, s32 *pan, s32 *pitch,
    //                     s32 shift)` - NINE o32 arguments, five of them on the stack.
    //
    // The only row in this group that drives the DOPPLER: `PlaySound3D` passes `pitch = NULL`, so
    // nothing the race plays is ever pitch-shifted by relative velocity. Here the
    // pointer is non-null on most cases, which is what exercises `RatAtan2`, the sine table at
    // 0x8005624C and `FixDiv`'s divide-by-zero behaviour.
    rows.push_back(Row{
        "sound3d_params", "void Sound3DParams(p, x, z, vx2, vz2, *vol, *pan, *pitch, shift)",
        kSound3DParams,
        [](CaseContext& c, Args& args) {
            const uint32_t listeners = c.scratch + 1600u;
            // Four 72-byte listener records. The dump-derived half plants a real captured bike's
            // world position and velocity as the listener's, and aims the sound at another one.
            for (uint32_t r = 0; r < kListenerRecords; ++r) {
                const uint32_t L = listeners + kListenerStride * r;
                for (uint32_t i = 0; i < kListenerStride; i += 4) c.mem->PokeWord(L + i, 0);
                const Sample& s = c.sample ? *c.sample : *c.any;
                PutS32(*c.mem, L + 0x00, static_cast<int32_t>(c.rng->U32() % 4096u));
                PutS32(*c.mem, L + 0x04, c.sample ? s.p[0] : c.rng->World());
                PutS32(*c.mem, L + 0x08, c.sample ? s.p[2] : c.rng->World());
                PutS32(*c.mem, L + 0x0C, c.sample ? s.q[0] : c.rng->World());
                PutS32(*c.mem, L + 0x10, c.sample ? s.q[2] : c.rng->World());
                PutS32(*c.mem, L + 0x14, static_cast<int32_t>(c.rng->U32() % 256u));
            }
            // 1 in 8: the NULL listener base, the original's own early-out at 0x80019E88.
            c.mem->PokeWord(c.gp + kGpListener, (c.rng->Next() % 8u == 0) ? 0u : listeners);
            const uint32_t gs = static_cast<uint32_t>(GetS32(*c.mem, kGameStatePtr));
            // Pan is computed in a ONE-PLAYER game only (0x80019F98), so both sides of that gate
            // are driven on purpose.
            PutS32(*c.mem, gs + 0x30, (c.rng->Next() % 3u == 0) ? 2 : 1);

            args.a[0] = c.rng->U32() % kListenerRecords;
            const Sample& s = c.sample ? *c.sample : *c.any;
            args.a[1] = static_cast<uint32_t>(c.sample ? s.q[0] : c.rng->World());
            args.a[2] = static_cast<uint32_t>(c.sample ? s.q[2] : c.rng->World());
            args.a[3] = static_cast<uint32_t>(c.rng->World());
            args.a4 = static_cast<uint32_t>(c.rng->World());
            // The three output slots, and the shift. `shift` is used by `srav`, which masks the
            // amount to five bits, so the whole range is legal; `PlaySound3D` passes 0.
            args.more[0] = c.scratch + 1904u;                      // outVol
            args.more[1] = c.scratch + 1908u;                      // outPan
            args.more[2] = (c.rng->Next() % 4u == 0) ? 0u : (c.scratch + 1912u); // outPitch
            args.more[3] = (c.rng->Next() % 2u == 0) ? 0u : c.rng->U32();
            for (uint32_t i = 0; i < 12; i += 4)
                c.mem->PokeWord(c.scratch + 1904u + i, c.rng->U32());
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            SoundMirror mirror(m, gp);
            mirror.LoadAtanTable();
            mirror.LoadSinCosTable();
            int32_t vol = GetS32(m, args.more[0]);
            int32_t pan = GetS32(m, args.more[1]);
            int32_t pitch = (args.more[2] != 0) ? GetS32(m, args.more[2]) : 0;
            rr::sim::Sound3DParams(static_cast<int32_t>(args.a[0]), static_cast<int32_t>(args.a[1]),
                                   static_cast<int32_t>(args.a[2]), static_cast<int32_t>(args.a[3]),
                                   static_cast<int32_t>(args.a4), &vol, &pan,
                                   (args.more[2] != 0) ? &pitch : nullptr,
                                   static_cast<int32_t>(args.more[3]), mirror.Listener());
            if (mirror.outOfWindow) return 0xDEADBEEFu;
            PutS32(m, args.more[0], vol);
            PutS32(m, args.more[1], pan);
            if (args.more[2] != 0) PutS32(m, args.more[2], pitch);
            return 0u;
        },
        /*compareV0=*/false});

    // `s32 StartVoice(int bank, int soundIndex, int restart, int reserved, SoundParams *p)`.
    // `restart` is held at 0 - see the note above this group.
    rows.push_back(Row{
        "start_voice", "s32 StartVoice(bank, id, restart, reserved, SoundParams*)  [SLUS]",
        kStartVoice,
        [](CaseContext& c, Args& args) {
            const uint32_t voices = static_cast<uint32_t>(GetS32(*c.mem, kSoundSys + 0x0C));
            int32_t bankCount = GetS32(*c.mem, kSoundSys + 0x00);
            if (!c.sample) {
                PlantSoundState(c, voices);
                bankCount = PlantBankTable(c);
            }
            // The SoundParams record: pitch -1 ("use the descriptor's own") half the time, a pan
            // over the whole 0..255 circle including the back half where the LEFT channel is
            // phase-inverted, and a negative pan now and then because the two instructions at
            // 0x8001F278/0x8001F298 say what happens then and nothing else pins it down.
            const uint32_t params = c.scratch + 1888u;
            PutS32(*c.mem, params + 0, (c.rng->Next() % 2u == 0)
                                           ? -1
                                           : static_cast<int32_t>(c.rng->U32() % 0x4000u));
            PutS32(*c.mem, params + 4, static_cast<int32_t>(c.rng->S32() >> (c.rng->Next() % 20u)));
            const uint64_t panRoll = c.rng->Next() % 8u;
            PutS32(*c.mem, params + 8,
                   (panRoll == 0) ? static_cast<int32_t>(c.rng->S32())
                                  : static_cast<int32_t>(c.rng->U32() % 256u));
            // Three cases in four name a slot the bound at 0x8001F1B8 accepts; the rest spread
            // over -1 .. count+1 so the two reject arms and the one-past-the-end read are driven.
            const int32_t bank =
                (c.rng->Next() % 4u != 0)
                    ? static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(bankCount + 1))
                    : (static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(bankCount + 3)) - 1);
            args.a[0] = static_cast<uint32_t>(bank);
            args.a[1] = static_cast<uint32_t>(PickSoundIndex(c, bank));
            args.a[2] = 0; // `restart` - the named bound above
            args.a[3] = (c.rng->Next() % 4u == 0) ? 1u : 0u;
            args.a4 = params;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            SoundMirror mirror(m, gp);
            rr::sim::SoundParams p;
            p.pitch = GetS32(m, args.a4 + 0);
            p.volume = GetS32(m, args.a4 + 4);
            p.pan = GetS32(m, args.a4 + 8);
            const uint32_t handle = rr::sim::StartVoice(
                static_cast<int32_t>(args.a[0]), static_cast<int32_t>(args.a[1]),
                static_cast<int32_t>(args.a[2]), static_cast<int32_t>(args.a[3]), p,
                mirror.System());
            mirror.Commit();
            if (mirror.outOfWindow) return 0xDEADBEEFu;
            return handle;
        }});

    // ---------------------------------------------------------------- SLUS 0x80017BA0
    // `void PlaySound3D(s32 x, s32 z, s32 soundIndex, s32 bank)` - the row that takes the oracle
    // seam off `bike_engine`.
    rows.push_back(Row{
        "play_sound_3d", "void PlaySound3D(s32 x, s32 z, s32 id, s32 bank)  [SLUS, the emitter]",
        kPlaySound3D,
        [](CaseContext& c, Args& args) {
            const uint32_t voices = static_cast<uint32_t>(GetS32(*c.mem, kSoundSys + 0x0C));
            const uint32_t gs = static_cast<uint32_t>(GetS32(*c.mem, kGameStatePtr));
            int32_t bankCount = GetS32(*c.mem, kSoundSys + 0x00);
            if (!c.sample) {
                PlantSoundState(c, voices);
                bankCount = PlantBankTable(c);
                const uint32_t listeners = c.scratch + 1600u;
                for (uint32_t r = 0; r < kListenerRecords; ++r) {
                    const uint32_t L = listeners + kListenerStride * r;
                    for (uint32_t i = 0; i < kListenerStride; i += 4) c.mem->PokeWord(L + i, 0);
                    PutS32(*c.mem, L + 0x00, static_cast<int32_t>(c.rng->U32() % 4096u));
                    PutS32(*c.mem, L + 0x04, c.rng->World());
                    PutS32(*c.mem, L + 0x08, c.rng->World());
                    PutS32(*c.mem, L + 0x14, static_cast<int32_t>(c.rng->U32() % 256u));
                }
                c.mem->PokeWord(c.gp + kGpListener,
                                (c.rng->Next() % 10u == 0) ? 0u : listeners);
                // The player count drives the LOOP and there is one listener record per player,
                // so it is bounded at 3: it is 1 or 2 in the original. 0 is the early-out at
                // 0x80017BEC and is driven one case in eight.
                PutS32(*c.mem, gs + 0x30,
                       (c.rng->Next() % 8u == 0) ? 0
                                                 : static_cast<int32_t>(1u + c.rng->U32() % 2u));
                // 1 in every capture; a call naming it is dropped silently at 0x80017BD0.
                PutS32(*c.mem, c.gp + kGpMutedBank,
                       (c.rng->Next() % 8u == 0) ? static_cast<int32_t>(c.rng->U32() % 4u) : 1);
                PutS32(*c.mem, c.gp + kGpDefaultBank,
                       (c.rng->Next() % 8u == 0)
                           ? static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(bankCount + 2))
                           : 0);
                PutS32(*c.mem, kSoundMaster, static_cast<int32_t>(c.rng->S32() >> 24));
            }
            const Sample& s = c.sample ? *c.sample : *c.any;
            args.a[0] = static_cast<uint32_t>(c.sample ? s.p[0] : c.rng->World());
            args.a[1] = static_cast<uint32_t>(c.sample ? s.p[2] : c.rng->World());
            // The non-positional arm: `x == 0 && z == 0` skips the 3D maths, takes volume 127 and
            // pan 64, and stops after the first listener (0x80017C54).
            if (c.rng->Next() % 6u == 0) { args.a[0] = 0; args.a[1] = 0; }
            // Half the calls pass bank 0, which is what 45 of the disc's 46 call sites do;
            // the rest spread over -1 .. count+1.
            const int32_t bank =
                (c.rng->Next() % 2u == 0)
                    ? 0
                    : (static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(bankCount + 3)) - 1);
            args.a[3] = static_cast<uint32_t>(bank);
            const int32_t effective =
                (bank == 0) ? GetS32(*c.mem, c.gp + kGpDefaultBank) : bank;
            args.a[2] = static_cast<uint32_t>(PickSoundIndex(c, effective));
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            SoundMirror mirror(m, gp);
            mirror.LoadAtanTable();
            rr::sim::PlaySound3DEnv env;
            env.mutedBank = GetS32(m, gp + kGpMutedBank);
            env.defaultBank = GetS32(m, gp + kGpDefaultBank);
            env.master3d = GetS32(m, kSoundMaster);
            env.listener = mirror.Listener();
            env.system = mirror.System();
            rr::sim::PlaySound3D(static_cast<int32_t>(args.a[0]), static_cast<int32_t>(args.a[1]),
                                 static_cast<int32_t>(args.a[2]), static_cast<int32_t>(args.a[3]),
                                 env);
            mirror.Commit();
            if (mirror.outOfWindow) return 0xDEADBEEFu;
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "set_listener", "void SetListener(p, x, z, vx, vz, camYaw)  [SLUS, SIX o32 arguments]",
        kSetListener,
        [](CaseContext& c, Args& args) {
            const uint32_t listeners = c.scratch + 1600u;
            for (uint32_t i = 0; i < kListenerStride * kListenerRecords; i += 4)
                c.mem->PokeWord(listeners + i, c.rng->U32());
            c.mem->PokeWord(c.gp + kGpListener, listeners);
            const Sample& s = c.sample ? *c.sample : *c.any;
            args.a[0] = c.rng->U32() % kListenerRecords;
            args.a[1] = static_cast<uint32_t>(c.sample ? s.p[0] : c.rng->World());
            args.a[2] = static_cast<uint32_t>(c.sample ? s.p[2] : c.rng->World());
            args.a[3] = static_cast<uint32_t>(c.sample ? s.q[0] : c.rng->World());
            args.a4 = static_cast<uint32_t>(c.sample ? s.q[2] : c.rng->World());
            args.more[0] = static_cast<uint32_t>(c.rng->S32()); // camYaw, 4096 = one turn
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            SoundMirror mirror(m, gp);
            rr::sim::SetListener(static_cast<int32_t>(args.a[0]), static_cast<int32_t>(args.a[1]),
                                 static_cast<int32_t>(args.a[2]), static_cast<int32_t>(args.a[3]),
                                 static_cast<int32_t>(args.a4),
                                 static_cast<int32_t>(args.more[0]), mirror.Listeners());
            mirror.Commit();
            return 0u;
        },
        /*compareV0=*/false});

    // `u8 SurfaceSound(s32 k)`, a leaf over the 52-entry table at SLUS 0x800525C0.
    //
    // NAMED BOUND: `k` is kept at or above -2147483596. The clamp is the compiler's branchless
    // `max(k,0) + min(51-k,0)`, and `51 - k` is a plain `subu`: for a `k` below that the
    // subtraction WRAPS, the "min" term becomes a large negative number and the original indexes
    // 2 GiB away from the table - i.e. the console itself would fault. That is a property of the
    // original worth recording, not a hole in the port.
    rows.push_back(Row{
        "surface_sound", "u8 SurfaceSound(s32 k)  [SLUS, the 52-entry surface table]",
        kSurfaceSound,
        [](CaseContext& c, Args& args) {
            const uint64_t roll = c.rng->Next() % 8u;
            int32_t k;
            if (roll == 0) k = 0;
            else if (roll == 1) k = 51;
            else if (roll == 2) k = 52;
            else if (roll == 3) k = -1;
            else if (roll == 4) k = static_cast<int32_t>(c.rng->U32() % 64u);
            else k = c.rng->S32();
            if (k < -2147483596) k = -2147483596;
            args.a[0] = static_cast<uint32_t>(k);
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            uint8_t table[52];
            m.ReadBlock(kSurfaceTable, table, sizeof(table));
            return rr::sim::SurfaceSound(static_cast<int32_t>(args.a[0]), table);
        }});

    rows.push_back(Row{
        "queue_listener_sound", "void QueueListenerSound(s32 a, s32 id, s32 t, s32 p)  [SLUS]",
        kQueueListenerSound,
        [](CaseContext& c, Args& args) {
            const uint32_t listeners = c.scratch + 1600u;
            for (uint32_t i = 0; i < kListenerStride * kListenerRecords; i += 4)
                c.mem->PokeWord(listeners + i, c.rng->U32());
            c.mem->PokeWord(c.gp + kGpListener, listeners);
            args.a[0] = c.rng->U32();
            // id 109 is the one value that picks the second queue slot (0x80017B6C/70).
            args.a[1] = (c.rng->Next() % 3u == 0) ? 109u : (c.rng->U32() % 256u);
            args.a[2] = static_cast<uint32_t>(c.rng->S32() >> 16);
            args.a[3] = c.rng->U32() % kListenerRecords;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            SoundMirror mirror(m, gp);
            rr::sim::QueueListenerSound(
                static_cast<int32_t>(args.a[0]), static_cast<int32_t>(args.a[1]),
                static_cast<int32_t>(args.a[2]), static_cast<int32_t>(args.a[3]),
                mirror.Listeners());
            mirror.Commit();
            return 0u;
        },
        /*compareV0=*/false});

    rows.push_back(Row{
        "sound_emitter_is_invisible",
        "DIAGNOSTIC: does SLUS 0x80017BA0 (PlaySound3D) touch compared state?", 0x80017BA0,
        [](CaseContext& c, Args& args) {
            args.a[0] = static_cast<uint32_t>(c.rng->World()); // x
            args.a[1] = static_cast<uint32_t>(c.rng->World()); // z
            args.a[2] = c.rng->U32() % 128u;                   // sound id
            args.a[3] = 0;
        },
        [](Memory&, uint32_t, uint32_t, const Args&) { return 0u; },
        /*compareV0=*/false});

    AddRoadQueryRows(rows);
    AddBikeStepRows(rows);
    AddRoadRuntimeRows(rows);
    AddEngineNoteRows(rows);
    AddGroundRows(rows);
    AddAnimRows(rows);
    AddCrashRows(rows);
    AddRoadRebindRows(rows); // rows_road_runtime.inc; appended so no earlier row's case seeds move
    // A row's random cases are seeded by its index: new groups go LAST so no earlier row's cases move.
    AddPopulationRows(rows);
    AddCameraRows(rows);
    AddSpineRows(rows);
    AddCollisionRows(rows);
    AddHudRows(rows);
    AddTrafficRows(rows);
    AddCollRows(rows); // rows_coll.inc
    AddPoseRows(rows); // rows_pose.inc
    AddAiBrainRows(rows); // rows_ai_brain.inc
    AddAiRows(rows); // rows_ai.inc
    AddAiPlanRows(rows); // rows_ai_plan.inc
    AddFightRows(rows); // rows_fight.inc
    AddSoundFrameRows(rows); // rows_engine_note.inc; appended so no earlier row's seeds move
    AddShellRows(rows); // rows_shell.inc
    AddRecoverRows(rows); // rows_recover.inc
    AddRiderRows(rows); // rows_riders.inc (the loader's rider records); appended last
    AddFeelRows(rows); // rows_feel.inc
    AddWeaponRows(rows); // rows_weapon.inc (weapons in the hand)
    AddSpeechRows(rows); // rows_speech.inc (the riders' voices); appended so no earlier row's seeds move
    AddCopRows(rows); // rows_cops.inc (the police chase and the arrest); appended so no earlier row's seeds move
    AddFxRows(rows); // rows_fx.inc (the effect pass); appended so no earlier row's seeds move
    AddModeRows(rows); // rows_modes.inc (the game modes); appended so no earlier row's seeds move
    AddMpRows(rows); // rows_mp.inc (two players); appended LAST so no earlier row's seeds move
    AddModelRows(rows); // rows_model.inc (the model draw's geometry); appended so no earlier row's seeds move
    AddVisRows(rows); // rows_vis.inc (the cell draw list); appended LAST so no earlier row's seeds move
    AddWorldRows(rows); // rows_world.inc (the cell walker, props, volumes); appended LAST
    AddJailRows(rows); // rows_jail.inc (the Jailbreak mode, the two-seat bike); appended LAST
    AddPolishRows(rows); // rows_polish.inc; appended LAST
    AddSolidRows(rows); // rows_solid.inc; appended LAST
    AddTakedownRows(rows); // rows_takedown.inc; appended LAST
    AddPartnersRows(rows); // rows_partners.inc; appended LAST
    AddGridRows(rows); // rows_grid.inc (the starting grid); appended LAST
    AddHazardRows(rows); // rows_hazards.inc (the hazard objects); appended LAST
    AddPedsRows(rows); // rows_peds.inc (the pedestrians); appended LAST
    AddPassesRows(rows); // rows_passes.inc; appended LAST
    AddStrikeRows(rows); // rows_strike.inc; appended LAST
    AddFxDrawRows(rows); // rows_fxdraw.inc (the emitter's glow sprites); appended LAST
    AddJunctionRows(rows); // rows_junction.inc; appended LAST
    AddStreamRows(rows); // rows_stream.inc; appended LAST
    AddSubdivRows(rows); // rows_subdiv.inc; appended LAST
    AddLookRows(rows); // rows_look.inc; appended LAST
    AddAnimObjRows(rows); // rows_animobj.inc; appended LAST
    AddMp2Rows(rows); // rows_mp2.inc (the two-player leftovers); appended LAST
    AddMenuRows(rows); // rows_menu.inc (the menu widgets' drawing); appended LAST
    AddStream2Rows(rows); // rows_stream2.inc; appended LAST
    AddLoaderRows(rows); // rows_loader.inc; appended LAST
    AddPauseRows(rows); // rows_pause.inc; appended LAST
    AddLoader2SpuRows(rows); // rows_loader2_spu.inc (libspu's SPU-RAM allocator); appended LAST
    AddLoader2CamRows(rows); // rows_loader2_cam.inc (the camera set-up, the light stores); appended LAST
    AddLoader2Rows(rows); // rows_loader2.inc (BuildRace, SetUpRace's children); appended LAST
    AddMenu2Rows(rows); // rows_menu2.inc (the panel films, the logos); appended LAST
    AddRouteRows(rows); // rows_route.inc; appended LAST
    AddCellsRows(rows); // rows_cells.inc (the frame's cell sort, the OT set-up); appended LAST
    AddStream3Rows(rows); // rows_stream3.inc (the stream's files, the .STP start); appended LAST
    AddSky2Rows(rows); // rows_sky2.inc (the panorama, the clouds, the MDEC column decode); appended LAST
    AddSky3Rows(rows); // rows_sky3.inc (the gradient, the two-player sky, the heap manager); appended LAST
    AddRumbleRows(rows); // rows_rumble.inc (the pad motors, the countdown voice); appended LAST
    AddAnimSoundRows(rows); // rows_anim_sound.inc (AnimSounds / AnimSoundFire); appended LAST
    return rows;
}

// ================================================================ the road query
//
// In order: the leaf lookups and 0x800394F0; 0x8003697C; the
// two neighbour functions, separately; 0x8003CCA0 as its own row; the picker 0x80039048; the slice
// search 0x80036B14; the look-ahead 0x800386DC. The port is src\game\sim\road_query.{h,cpp} and it
// is written over a GUEST-ADDRESS VIEW of RAM, so every native side below
// is the same three steps: wrap the clone's RAM, call the port with the guest's own arguments, and
// fail the case loudly if the port met an address the console would not survive.
//
// Input families. rr-race holds three resident road objects - 67, 14 and 15, road 9 and junction 7
// - the real ROAD1.MAP network and a four-leg route.
//   * dump-derived: this machine's live bikes with their REAL cursors, the real network, and keys,
//     points and speeds out of the 324 captured bikes of all 18 RAM images;
//   * randomised: cursors re-seated on random resident objects, pieces, sub-objects and slices (at a
//     sub-object end two times in three - that is where the neighbour functions run), residency
//     stirred (non-resident neighbours made resident, resident ones dropped), the junction memory,
//     the handle's pool, the rider class, the route, the command-stack target and STALE STACK all
//     drawn; and for the table lookups a SYNTHETIC network planted in the scratch block with
//     0x8005B240 pointed at it, so the id checks, the bounds and the empty tables are driven too.
namespace rq {
constexpr uint32_t kMemCopy = 0x8001E0B4, kBtt = 0x80039A08, kBst = 0x80039AA0, kNode = 0x80039AFC,
                   kJunction = 0x80039B60, kTurnsFrom = 0x80039BB0, kTurnTarget = 0x80039C38,
                   kNodeArm = 0x8003A37C, kNextFwd = 0x8003C840, kNextBwd = 0x8003C948,
                   kLegOfRoad = 0x8003F3B4, kLegFor = 0x8003F408, kLegHas = 0x8003F580,
                   kLegContaining = 0x8003F5D0, kMissing = 0x800394F0, kAlong = 0x8003697C,
                   kNeighFwd = 0x80037A30, kNeighBwd = 0x80037FBC, kJunctionChoice = 0x8003CCA0,
                   kPick = 0x80039048, kSearch = 0x80036B14, kLookAhead = 0x800386DC;
constexpr uint32_t kGraphPtr = 0x8005B240;
constexpr uint32_t kRoute = 0x800D6170; // +0x12 s16 leg count, +0x24 -> legs (120 bytes each)
constexpr uint32_t kRiderAt = 8000;     // a 72-byte rider record at the top of the scratch block
constexpr uint32_t kSynthAt = 4096;     // the synthetic network, 0x920 bytes
} // namespace rq

rr::sim::GuestRam RqView(NativeEnv& env) {
    return rr::sim::GuestRam(env.clone->mem.ram().data(), env.gp);
}
// A fault is the console's own crash, not a value: the case fails, with the address.
uint32_t RqDone(NativeEnv& env, const rr::sim::GuestRam& g, uint32_t v) {
    if (g.Faulted() && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the port met an address the console faults on: 0x%08X",
                      g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

struct RqNet {
    uint32_t g = 0, btt = 0, bst = 0, bit = 0, ipt = 0, pdt = 0, gpdt = 0;
    int32_t nObj = 0, nRoad = 0, nJn = 0, nIpt = 0, nPdt = 0, nGpdt = 0;
    std::vector<uint32_t> resident; // what BTT_ +0x0C holds, in BTT_ order
};

RqNet RqRead(const Memory& m) {
    RqNet n;
    n.g = m.PeekWord(rq::kGraphPtr);
    n.btt = m.PeekWord(n.g + 0x1C);
    n.bst = m.PeekWord(n.g + 0x20);
    n.bit = m.PeekWord(n.g + 0x24);
    n.ipt = m.PeekWord(n.g + 0x30);
    n.pdt = m.PeekWord(n.g + 0x34);
    n.gpdt = m.PeekWord(n.g + 0x38);
    n.nObj = GetS16(m, n.g + 0x28);
    n.nRoad = GetS16(m, n.g + 0x2A);
    n.nJn = GetS16(m, n.g + 0x2C);
    n.nIpt = GetS16(m, n.g + 0x3C);
    n.nPdt = GetS16(m, n.g + 0x3E);
    n.nGpdt = GetS16(m, n.g + 0x40);
    for (int32_t i = 0; i < n.nObj && i < 512; ++i) {
        const uint32_t p = m.PeekWord(n.btt + 32u * static_cast<uint32_t>(i) + 12u);
        if (p >= 0x80000000u && p < 0x80200000u) n.resident.push_back(p);
    }
    return n;
}

uint32_t RqPick(Rng& r, int32_t count) {
    return (count > 0) ? (r.U32() % static_cast<uint32_t>(count)) : 0u;
}

// A PDT_ record's destination road, the value AiJunctionChoice and the traffic rule compare with.
int32_t RqSomeRoad(CaseContext& c, const RqNet& n) {
    if (n.nPdt > 0 && (c.rng->Next() & 1u))
        return GetS16(*c.mem, n.pdt + 12u * RqPick(*c.rng, n.nPdt) + 10u);
    return static_cast<int32_t>(c.rng->U32() % 40u);
}

// A 32-byte cursor at `at`, re-seated on a random resident object, piece, sub-object and slice
// (with the junction and a turn when the piece belongs to a junction core), exactly as roadq.py's
// synthetic generator does. Returns the slice.
uint32_t RqPlantCursor(CaseContext& c, const RqNet& n, uint32_t at) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (n.resident.empty()) return 0;
    const uint32_t obj = n.resident[r.U32() % n.resident.size()];
    const int32_t npc = GetS16(m, obj + 0x12);
    const uint32_t piece = m.PeekWord(obj + 0x2C) + 32u * RqPick(r, npc);
    const uint32_t sub =
        m.PeekWord(obj + 0x30) + 28u * static_cast<uint32_t>(static_cast<int32_t>(GetS16(m, piece + 20)));
    const int32_t first = GetS16(m, sub + 8);
    const int32_t cnt = GetS16(m, sub + 10);
    int32_t idx = 0;
    switch (r.U32() % 3u) {
        case 0: idx = 0; break;
        case 1: idx = std::max(0, cnt - 1); break;
        default: idx = static_cast<int32_t>(RqPick(r, cnt)); break;
    }
    const uint32_t slice = m.PeekWord(obj + 0x34) + 52u * static_cast<uint32_t>(first + idx);
    uint32_t node = 0, turn = 0;
    if (GetS16(m, obj + 16) != 0 && GetS32(m, obj + 12) != 1) {
        const int32_t id = GetS32(m, obj);
        for (int32_t q = 0; q < n.nIpt && q < 256; ++q) {
            const uint32_t rec = n.ipt + 12u * static_cast<uint32_t>(q);
            if (GetS16(m, rec) != id) continue;
            node = rec;
            turn = n.pdt + 12u * static_cast<uint32_t>(GetS16(m, rec + 6) +
                                                       static_cast<int32_t>(RqPick(r, GetS16(m, rec + 8))));
        }
    }
    m.PokeWord(at + 0, obj);
    m.PokeWord(at + 4, piece);
    m.PokeWord(at + 8, sub);
    m.PokeWord(at + 12, slice);
    PutS32(m, at + 16, r.S32() >> (8 + static_cast<int>(r.Next() % 8u)));
    const int32_t chord = GetS32(m, slice + 32);
    PutS32(m, at + 20, (chord > 0) ? static_cast<int32_t>(r.U32() % static_cast<uint32_t>(chord)) : 0);
    m.PokeWord(at + 24, (r.Next() % 10u < 7u) ? node : 0u);
    m.PokeWord(at + 28, (r.Next() % 10u < 7u) ? turn : 0u);
    return slice;
}

// Residency, stirred: some BTT_ records gain one of the three resident objects, some lose theirs,
// and the "arms" word +0x1C that NextObjectFwd/Bwd read is flipped now and then. This is what
// makes "the next object is a road piece", "the next object is missing" and the partner arms run.
void RqStirResidency(CaseContext& c, const RqNet& n) {
    if (n.resident.empty() || n.nObj <= 0) return;
    Memory& m = *c.mem;
    // Half the time the ACTUAL neighbours of the resident objects are streamed in (as one of the
    // three), which is what "the next object is a road piece" and the junction arms need.
    if (c.rng->Next() & 1u) {
        for (uint32_t o : n.resident) {
            const int32_t id = GetS32(m, o);
            std::vector<int32_t> nb;
            if (GetS16(m, o + 16) == 0) {
                const int32_t idx = id - n.nJn;
                if (idx >= 0 && idx < n.nRoad) {
                    const uint32_t b = n.bst + 8u * static_cast<uint32_t>(idx);
                    nb.push_back(GetS16(m, b + 2));
                    nb.push_back(GetS16(m, b + 4));
                }
            } else if (id >= 0 && id < n.nJn) {
                const uint32_t node = n.bit + 104u * static_cast<uint32_t>(id);
                nb.push_back(GetS16(m, node + 6));
                for (uint32_t k = 0; k < 4; ++k) nb.push_back(GetS16(m, node + 8u + 24u * k));
            }
            for (int32_t x : nb) {
                if (x < 0 || x >= n.nObj || (c.rng->Next() & 1u)) continue;
                const int32_t idx = (x < n.nJn) ? x + n.nRoad : x - n.nJn;
                m.PokeWord(n.btt + 32u * static_cast<uint32_t>(idx) + 12u,
                           n.resident[c.rng->U32() % n.resident.size()]);
            }
        }
    }
    const uint32_t k = c.rng->U32() % 10u;
    for (uint32_t i = 0; i < k; ++i) {
        const uint32_t rec = n.btt + 32u * RqPick(*c.rng, n.nObj);
        c.mem->PokeWord(rec + 12, (c.rng->Next() % 4u == 0)
                                      ? 0u
                                      : n.resident[c.rng->U32() % n.resident.size()]);
        if (c.rng->Next() % 5u == 0) c.mem->PokeWord(rec + 28, c.rng->U32() % 2u);
    }
}

// The route: lists re-drawn from the network's own destination roads, the count varied, and one
// case in eight with no route at all (the -1 every route function tests first).
void RqStirRoute(CaseContext& c, const RqNet& n) {
    Memory& m = *c.mem;
    const int32_t count = GetS16(m, rq::kRoute + 18);
    const uint32_t legs = m.PeekWord(rq::kRoute + 36);
    if (legs >= 0x80000000u && legs < 0x80200000u) {
        for (int32_t i = 0; i < count && i < 16; ++i) {
            const uint32_t leg = legs + 120u * static_cast<uint32_t>(i);
            if (c.rng->Next() % 3u != 0) continue;
            PutS32(m, leg + 16, static_cast<int32_t>(c.rng->U32() % 5u));
            for (uint32_t k = 0; k < 4; ++k) {
                const int32_t road = RqSomeRoad(c, n);
                PutS32(m, leg + 84u + 4u * k, road);
                PutS32(m, leg + 100u + 4u * k, (c.rng->Next() & 1u) ? road : RqSomeRoad(c, n));
            }
            if (c.rng->Next() % 3u == 0) PutS32(m, leg + 0, RqSomeRoad(c, n));
        }
    }
    if (c.rng->Next() % 8u == 0) PutS16(m, rq::kRoute + 18, -1);
}

// GPDT +0x08 is 1 in all 73 records of both ROAD<n>.MAP, so the "first slice
// when t.dir <= 0" and "entered from its far end" arms never run on shipped data. The randomised
// families flip a few, now and then.
void RqStirTurns(CaseContext& c, const RqNet& n, uint32_t oneIn = 4) {
    if (n.nGpdt <= 0 || (c.rng->Next() % oneIn) != 0) return;
    const uint32_t k = 1u + c.rng->U32() % static_cast<uint32_t>(2 * n.nGpdt);
    for (uint32_t i = 0; i < k; ++i)
        PutS16(*c.mem, n.gpdt + 12u * RqPick(*c.rng, n.nGpdt) + 8u,
               (c.rng->Next() % 3u == 0) ? static_cast<int16_t>(0) : static_cast<int16_t>(-1));
}

// The chord (SLCT +0x20) of the slices around a cursor made zero, negative or tiny one case in
// six: every chord on disc is positive, and the look-ahead's signed divide has an arm for each sign.
void RqStirChords(CaseContext& c, uint32_t slice) {
    if (c.rng->Next() % 6u != 0) return;
    for (int32_t k = -2; k <= 2; ++k) {
        if (c.rng->Next() & 1u) continue;
        const uint32_t s = slice + static_cast<uint32_t>(k * 52);
        const uint64_t roll = c.rng->Next() % 3u;
        PutS32(*c.mem, s + 32, (roll == 0) ? 0
                               : (roll == 1) ? -static_cast<int32_t>(c.rng->U32() % 0x80000u)
                                             : static_cast<int32_t>(c.rng->U32() % 64u));
    }
}

// SLCT +0x32, the slice tag: nonzero on 89 slices per set, which the three resident
// objects may not hold at all. Planted on a quarter of an object's slices one case in three.
void RqPlantTags(CaseContext& c, uint32_t obj) {
    if (c.rng->Next() % 3u != 0) return;
    const int32_t nslc = GetS16(*c.mem, obj + 0x16);
    const uint32_t base = c.mem->PeekWord(obj + 0x34);
    for (int32_t i = 0; i < nslc && i < 256; ++i)
        if (c.rng->Next() % 4u == 0)
            PutS16(*c.mem, base + 52u * static_cast<uint32_t>(i) + 50u,
                   static_cast<int16_t>(1 + c.rng->U32() % 200u));
}

// One sub-object of a resident object cut to ONE slice, one case in eight: the look-ahead's fifth
// `jalr` (0x80038C50, its answer ignored) runs only when the slice before A lies in a one-slice
// sub-object entered against the walk, and no sub-object of the three resident objects has one.
void RqStirSubCounts(CaseContext& c, const RqNet& n) {
    if (n.resident.empty() || c.rng->Next() % 8u != 0) return;
    const uint32_t obj = n.resident[c.rng->U32() % n.resident.size()];
    const int32_t nsub = GetS16(*c.mem, obj + 0x14);
    const uint32_t sub = c.mem->PeekWord(obj + 0x30) + 28u * RqPick(*c.rng, nsub);
    PutS16(*c.mem, sub + 10, 1);
}

// A cursor on the FIRST or LAST slice of a junction CORE piece's sub-object, with the junction and
// a turn set: from there the neighbour functions leave by the turn's arm, whose direction is -1 on
// half the arms, so the walk FLIPS. `cutCounts` also cuts every sub-object of that object to one
// slice. Returns the slice, or 0 when no resident object has a core piece.
uint32_t RqPlantCoreCursor(CaseContext& c, const RqNet& n, uint32_t at, bool first, bool cutCounts) {
    Memory& m = *c.mem;
    for (uint32_t obj : n.resident) {
        if (GetS16(m, obj + 16) == 0 || GetS32(m, obj + 12) == 1) continue;
        const int32_t id = GetS32(m, obj);
        uint32_t node = 0;
        for (int32_t q = 0; q < n.nIpt && q < 256; ++q)
            if (GetS16(m, n.ipt + 12u * static_cast<uint32_t>(q)) == id) node = n.ipt + 12u * static_cast<uint32_t>(q);
        if (node == 0) continue;
        const int32_t npc = GetS16(m, obj + 0x12);
        std::vector<uint32_t> cores;
        for (int32_t i = 0; i < npc && i < 64; ++i) {
            const uint32_t pc = m.PeekWord(obj + 0x2C) + 32u * static_cast<uint32_t>(i);
            if (GetS16(m, pc + 2) != 0) cores.push_back(pc);
        }
        if (cores.empty()) continue;
        const uint32_t piece = cores[c.rng->U32() % cores.size()];
        if (cutCounts) {
            const int32_t nsub = GetS16(m, obj + 0x14);
            for (int32_t i = 0; i < nsub && i < 64; ++i)
                PutS16(m, m.PeekWord(obj + 0x30) + 28u * static_cast<uint32_t>(i) + 10u, 1);
        }
        const uint32_t sub = m.PeekWord(obj + 0x30) + 28u * static_cast<uint32_t>(static_cast<int32_t>(GetS16(m, piece + 20)));
        const int32_t f = GetS16(m, sub + 8), cnt = GetS16(m, sub + 10);
        const uint32_t slice = m.PeekWord(obj + 0x34) + 52u * static_cast<uint32_t>(first ? f : f + std::max(0, cnt - 1));
        const uint32_t turn = n.pdt + 12u * static_cast<uint32_t>(GetS16(m, node + 6) +
                                                                  static_cast<int32_t>(RqPick(*c.rng, GetS16(m, node + 8))));
        m.PokeWord(at + 0, obj);
        m.PokeWord(at + 4, piece);
        m.PokeWord(at + 8, sub);
        m.PokeWord(at + 12, slice);
        m.PokeWord(at + 24, node);
        m.PokeWord(at + 28, turn);
        return slice;
    }
    return 0;
}

// Random words under the stack pointer, where the walkers' frames will be: the partial candidate
// and the index -1 pick read what is there.
void RqStackJunk(CaseContext& c, uint32_t bytes) {
    for (uint32_t i = 4; i <= bytes; i += 4) c.mem->PokeWord(c.sp - i, c.rng->U32());
}

// A live pool-0 bike of THIS machine - its cursor points at road this snapshot really holds
// - with, when `stir`, every field the road query reads drawn around it.
uint32_t RqBike(CaseContext& c, const RqNet& n, bool stir, bool stirResidency = true) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t pool = m.PeekWord(kPoolBasePtr);
    const uint32_t nb = std::max<uint32_t>(1u, std::min<uint32_t>(m.PeekWord(kLiveBikes), 18u));
    const uint32_t e = pool + 1096u * (r.U32() % nb);
    if (!stir) return e;
    // the junction memory, e[+0x3B3/+0x3B4/+0x3B8]
    const uint64_t jm = r.Next() % 3u;
    if (jm == 0) {
        m.PokeWord(e + 948, 0);
    } else if (jm == 1 && n.nIpt > 0) {
        m.PokeWord(e + 948, n.ipt + 12u * RqPick(r, n.nIpt));
        // a remembered index is 0 or 1: the callers accept only 0 < n < 3 candidates
        const uint8_t ch = static_cast<uint8_t>(r.U32() % 2u);
        m.WriteBlock(e + 947, &ch, 1);
        m.PokeWord(e + 952, (n.nPdt > 0) ? n.pdt + 12u * RqPick(r, n.nPdt) : 0u);
    }
    if (r.Next() & 1u) m.PokeWord(e + 0x184, m.PeekWord(e + 0x184) ^ 0x80u);
    if (r.Next() % 10u < 6u) RqPlantCursor(c, n, e + 0x148);
    // The handle: another slot or another pool one case in six. NAMED BOUND: a pool-0 handle
    // names one of the live slots. pool0 + 1096*h past them is not an entity, and the original
    // then chases e[+0x3B4] and e[+0x43C] out of whatever lies there - measured before this bound:
    // 2 cases of road_pick_neighbour and 1 of road_look_ahead trapped on exactly those two loads
    // (0x80039350, 0x80039328). Other pools
    // keep any slot 0..31, so the handle stays under 256 and the slice-tag store
    // (a pool-0 slot computed from ANY handle) stays inside RAM and below the stack.
    if (r.Next() % 6u == 0) {
        const uint32_t h = (r.Next() % 3u == 0) ? (r.U32() % nb)
                                                 : (((1u + r.U32() % 7u) << 5) | (r.U32() % 32u));
        WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>(h));
    }
    // the rider record: class nibble 2 one time in three (PickNeighbour and AiJunctionChoice)
    const uint32_t rider = c.scratch + rq::kRiderAt;
    for (uint32_t i = 0; i < 72; i += 4) m.PokeWord(rider + i, r.U32());
    {
        uint8_t b1 = static_cast<uint8_t>(r.U32());
        b1 = static_cast<uint8_t>((b1 & 0xF0u) | ((r.Next() % 3u == 0) ? 2u : (r.U32() % 16u)));
        m.WriteBlock(rider + 1, &b1, 1);
    }
    m.PokeWord(e + 0x43C, rider);
    if (r.Next() % 3u == 0) PutS32(m, e + 0x1E0, static_cast<int32_t>(r.U32() % 131u));
    // The command stack AiJunctionChoice follows: depth 1..4 (a slot at depth 0 would overlap the
    // junction memory itself), targets that are live bikes, 224, or another pool.
    {
        const uint8_t depth = static_cast<uint8_t>(1u + r.U32() % 4u);
        m.WriteBlock(e + 0x3B2, &depth, 1);
        for (uint32_t d = 1; d <= 4; ++d) {
            uint16_t h;
            switch (r.U32() % 4u) {
                case 0: h = 224; break;
                case 1: h = static_cast<uint16_t>(r.U32() % 256u); break;
                default: h = static_cast<uint16_t>(r.U32() % 18u); break;
            }
            WriteU16Bench(m, e + 0x3B6u + 8u * d, h);
        }
    }
    {
        const uint64_t roll = r.Next() % 6u;
        const int32_t count = GetS16(m, rq::kRoute + 18);
        if (roll == 0) m.PokeWord(e + 0x1AC, 0);
        else if (roll <= 2 && count > 0)
            m.PokeWord(e + 0x1AC, m.PeekWord(rq::kRoute + 36) + 120u * RqPick(r, count));
    }
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 0x30, r.U32() % 21u);
    {
        const uint8_t f = static_cast<uint8_t>(r.U32());
        m.WriteBlock(gs + 4, &f, 1);
    }
    // drawn either way, so the rest of the family does not depend on the switch
    const bool stirNow = (r.Next() & 1u) != 0;
    if (stirNow && stirResidency) RqStirResidency(c, n);
    RqStirRoute(c, n);
    return e;
}

// A point near `slice`: d units along its tangent and up to 12 across, as the search's callers
// place theirs. One case in twenty sits exactly on the slice start.
void RqPlantPoint(CaseContext& c, uint32_t slice, uint32_t at) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    static const int64_t kSpan[3] = {40, 400, 2500};
    const int64_t span = kSpan[r.U32() % 3u] * 65536;
    int64_t d = static_cast<int64_t>(r.Next() % static_cast<uint64_t>(2 * span + 1)) - span;
    int64_t lat = static_cast<int64_t>(r.Next() % (24ull * 65536ull + 1ull)) - 12 * 65536;
    if (r.Next() % 20u == 0) { d = 0; lat = 0; }
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t pos = GetS32(m, slice + 20u + 4u * k);
        const int64_t t2 = GetS16(m, slice + 14u + 2u * k);
        const int64_t t0 = GetS16(m, slice + 2u + 2u * k);
        PutS32(m, at + 4u * k, static_cast<int32_t>(pos + ((t2 * d) >> 12) + ((t0 * lat) >> 12)));
    }
}

// A small synthetic ROAD<n>.MAP header and its six tables at `base` (0x920 bytes), with
// 0x8005B240 pointed at it. Ids are consistent five times in six, counts are sometimes off by a
// few or negative, and the bytes around every table are planted too, because BstRecord reads
// BEFORE its table for a junction id.
RqNet RqPlantSyntheticNet(CaseContext& c, const RqNet& real, uint32_t base) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    RqNet n;
    n.g = base;
    n.btt = base + 0x50;
    n.bst = base + 0x280;
    n.bit = base + 0x320;
    n.ipt = base + 0x660;
    n.pdt = base + 0x6E0;
    n.gpdt = base + 0x860;
    for (uint32_t i = 0; i < 0x920; i += 4) m.PokeWord(base + i, r.U32());
    n.nRoad = static_cast<int32_t>(r.U32() % 9u);
    n.nJn = static_cast<int32_t>(r.U32() % 9u);
    n.nObj = n.nRoad + n.nJn;
    if (r.Next() % 5u == 0) n.nObj += static_cast<int32_t>(r.U32() % 5u) - 2;
    n.nIpt = static_cast<int32_t>(r.U32() % 9u);
    n.nPdt = static_cast<int32_t>(r.U32() % 33u);
    n.nGpdt = static_cast<int32_t>(r.U32() % 17u);
    auto maybeNeg = [&r](int32_t v) { return (r.Next() % 12u == 0) ? -1 - static_cast<int32_t>(r.U32() % 3u) : v; };
    for (uint32_t i = 0; i < 0x44; i += 4) m.PokeWord(base + i, 0);
    m.PokeWord(base + 0x1C, n.btt);
    m.PokeWord(base + 0x20, n.bst);
    m.PokeWord(base + 0x24, n.bit);
    m.PokeWord(base + 0x30, n.ipt);
    m.PokeWord(base + 0x34, n.pdt);
    m.PokeWord(base + 0x38, n.gpdt);
    PutS16(m, base + 0x28, static_cast<int16_t>(maybeNeg(n.nObj)));
    PutS16(m, base + 0x2A, static_cast<int16_t>(maybeNeg(n.nRoad)));
    PutS16(m, base + 0x2C, static_cast<int16_t>(maybeNeg(n.nJn)));
    PutS16(m, base + 0x3C, static_cast<int16_t>(maybeNeg(n.nIpt)));
    PutS16(m, base + 0x3E, static_cast<int16_t>(n.nPdt));
    PutS16(m, base + 0x40, static_cast<int16_t>(maybeNeg(n.nGpdt)));
    const auto smallId = [&r, &n]() { return static_cast<int16_t>(static_cast<int32_t>(r.U32() % static_cast<uint32_t>(n.nObj + 3)) - 1); };
    for (uint32_t i = 0; i < 16; ++i) {                          // BTT_
        const uint32_t rec = n.btt + 32u * i;
        int32_t id = (static_cast<int32_t>(i) < n.nRoad) ? static_cast<int32_t>(i) + n.nJn
                                                          : static_cast<int32_t>(i) - n.nRoad;
        if (r.Next() % 6u == 0) id = smallId();
        PutS32(m, rec + 0, id);
        PutS16(m, rec + 4, static_cast<int16_t>((id < n.nJn) ? 1 : 0));
        const uint64_t roll = r.Next() % 3u;
        m.PokeWord(rec + 12, (roll == 0 || real.resident.empty())
                                 ? 0u
                                 : real.resident[r.U32() % real.resident.size()]);
        m.PokeWord(rec + 28, r.U32() % 2u);
    }
    for (uint32_t i = 0; i < 16; ++i) {                          // BST_
        const uint32_t rec = n.bst + 8u * i;
        PutS16(m, rec + 0, (r.Next() % 6u == 0) ? smallId() : static_cast<int16_t>(static_cast<int32_t>(i) + n.nJn));
        PutS16(m, rec + 2, smallId());
        PutS16(m, rec + 4, smallId());
    }
    for (uint32_t i = 0; i < 8; ++i) {                           // BIT_, 104 bytes
        const uint32_t rec = n.bit + 104u * i;
        PutS16(m, rec + 0, (r.Next() % 6u == 0) ? smallId() : static_cast<int16_t>(i));
        PutS16(m, rec + 2, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 8u) - 1));
        PutS16(m, rec + 6, smallId());
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t arm = rec + 8u + 24u * k;
            PutS16(m, arm + 0, smallId());
            const uint64_t d = r.Next() % 6u;
            PutS16(m, arm + 2, (d < 2) ? 1 : (d < 4) ? -1 : (d == 4) ? 0 : r.S16());
            PutS16(m, arm + 4, static_cast<int16_t>(r.U32() % 13u));
        }
    }
    for (uint32_t i = 0; i < 8; ++i) {                           // IPT_
        const uint32_t rec = n.ipt + 12u * i;
        PutS16(m, rec + 0, smallId());
        PutS16(m, rec + 2, static_cast<int16_t>(r.U32() % 16u));
        PutS16(m, rec + 6, static_cast<int16_t>(r.U32() % 25u));
        PutS16(m, rec + 8, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 10u) - 1));
    }
    for (uint32_t i = 0; i < 32; ++i) {                          // PDT_
        const uint32_t rec = n.pdt + 12u * i;
        PutS16(m, rec + 2, static_cast<int16_t>(static_cast<int32_t>(r.U32() % static_cast<uint32_t>(n.nGpdt + 3)) - 1));
        PutS16(m, rec + 6, (r.Next() & 1u) ? 1 : -1);
        PutS16(m, rec + 8, static_cast<int16_t>(r.U32() % 13u));
        PutS16(m, rec + 10, static_cast<int16_t>(r.U32() % 13u));
    }
    for (uint32_t i = 0; i < 16; ++i) {                          // GPDT
        const uint32_t rec = n.gpdt + 12u * i;
        PutS16(m, rec + 2, static_cast<int16_t>(r.U32() % 4u));
        PutS16(m, rec + 4, static_cast<int16_t>(r.U32() % 4u));
        PutS16(m, rec + 8, (r.Next() & 1u) ? 1 : -1);
    }
    m.PokeWord(rq::kGraphPtr, base);
    n.resident = real.resident;
    return n;
}

// The network a lookup row runs on: the real one for the dump-derived half, the synthetic one for
// half of the randomised half.
RqNet RqNetFor(CaseContext& c) {
    const RqNet real = RqRead(*c.mem);
    if (c.sample == nullptr && (c.rng->Next() & 1u))
        return RqPlantSyntheticNet(c, real, c.scratch + rq::kSynthAt);
    return real;
}

// An object id to look up: a captured bike's road id (dump-derived), a resident object's, one in
// or just past each range, or anything at all.
int32_t RqKey(CaseContext& c, const RqNet& n) {
    Rng& r = *c.rng;
    if (c.sample != nullptr && (r.Next() & 1u)) {
        const std::vector<uint8_t>& e = c.sample->entity;
        if (e.size() >= kEntRoadPos + 2) {
            const int32_t road = static_cast<int16_t>(e[kEntRoadPos] | (e[kEntRoadPos + 1] << 8));
            return road + ((r.Next() & 1u) ? 0 : n.nJn);
        }
    }
    switch (r.U32() % 7u) {
        case 0: return r.S32();
        case 1: return -1 - static_cast<int32_t>(r.U32() % 4u);
        case 2: return (n.resident.empty()) ? 0 : GetS32(*c.mem, n.resident[r.U32() % n.resident.size()]);
        case 3: return n.nJn + static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::max(1, n.nRoad + 3)));
        default: return static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::max(1, n.nObj + 4)));
    }
}

uint32_t RqSomeRecord(CaseContext& c, uint32_t table, int32_t count, uint32_t stride) {
    return table + stride * RqPick(*c.rng, count + 1); // one past the end now and then
}

void AddRoadQueryRows(std::vector<Row>& rows) {
    using rr::sim::GuestRam;

    // ---------------------------------------------------------------- the leaves
    rows.push_back(Row{
        "mem_copy32", "void *MemCopyWords(void *dst, const void *src, u32 n)  [SLUS 0x8001E0B4]",
        rq::kMemCopy,
        [](CaseContext& c, Args& args) {
            for (uint32_t i = 0; i < 256; i += 4) c.mem->PokeWord(c.scratch + i, c.rng->U32());
            const uint32_t src = c.scratch + 4u * (c.rng->U32() % 16u);
            // Overlap both ways (the copy is forward, word by word), and 0 bytes.
            const uint32_t dst = (c.rng->Next() % 3u == 0) ? src + 4u * (c.rng->U32() % 4u)
                                                            : c.scratch + 128u + 4u * (c.rng->U32() % 8u);
            args.a[0] = dst;
            args.a[1] = (c.rng->Next() % 5u == 0) ? dst + 4u : src;
            args.a[2] = c.sample ? 32u : 4u * (c.rng->U32() % 17u);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::GuestCopyWords(g, a.a[0], a.a[1], a.a[2]));
        }});

    const auto idRow = [&rows](const char* name, const char* sig, uint32_t entry,
                               uint32_t (*fn)(GuestRam&, int32_t)) {
        rows.push_back(Row{
            name, sig, entry,
            [](CaseContext& c, Args& args) {
                const RqNet n = RqNetFor(c);
                args.a[0] = static_cast<uint32_t>(RqKey(c, n));
            },
            nullptr, /*compareV0=*/true,
            [fn](NativeEnv& env, const Args& a) -> uint32_t {
                GuestRam g = RqView(env);
                return RqDone(env, g, fn(g, static_cast<int32_t>(a.a[0])));
            }});
    };
    idRow("road_btt_record", "BTT *BttRecord(s32 id)  [SLUS 0x80039A08]", rq::kBtt, rr::sim::RoadBttRecord);
    idRow("road_bst_record", "BST *BstRecord(s32 id)  [SLUS 0x80039AA0]", rq::kBst, rr::sim::RoadBstRecord);
    idRow("road_node_record", "BIT *NodeRecord(s32 id)  [SLUS 0x80039AFC]", rq::kNode, rr::sim::RoadNodeRecord);
    idRow("road_junction_index", "IPT *JunctionIndex(s32 id)  [SLUS 0x80039B60]", rq::kJunction,
          rr::sim::RoadJunctionIndex);

    rows.push_back(Row{
        "road_turns_from", "s32 TurnsFrom(IPT *j, s32 road, PDT **out, s32 max)  [SLUS 0x80039BB0]",
        rq::kTurnsFrom,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqNetFor(c);
            const uint32_t out = c.scratch + 0x400;
            for (uint32_t i = 0; i < 64; i += 4) c.mem->PokeWord(out + i, c.rng->U32());
            const uint32_t j = (c.rng->Next() % 10u == 0) ? 0u : RqSomeRecord(c, n.ipt, n.nIpt, 12);
            int32_t road = static_cast<int32_t>(c.rng->U32() % 16u);
            if (j != 0 && (c.rng->Next() % 4u != 0)) {
                // a road the junction really has turns from, most of the time
                const int32_t base = GetS16(*c.mem, j + 6);
                const int32_t cnt = GetS16(*c.mem, j + 8);
                road = GetS16(*c.mem, n.pdt + 12u * static_cast<uint32_t>(base + static_cast<int32_t>(RqPick(*c.rng, cnt))) + 8u);
            }
            args.a[0] = j;
            args.a[1] = static_cast<uint32_t>(road);
            args.a[2] = out;
            args.a[3] = (c.rng->Next() % 4u != 0) ? 3u : static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 6u) - 1);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::RoadTurnsFrom(
                                      g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], static_cast<int32_t>(a.a[3]))));
        }});

    rows.push_back(Row{
        "road_turn_target", "GPDT *TurnTarget(PDT *t)  [SLUS 0x80039C38]", rq::kTurnTarget,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqNetFor(c);
            const uint64_t roll = c.rng->Next() % 8u;
            if (roll == 0) {
                args.a[0] = 0;
            } else if (roll == 1) {
                const uint32_t t = c.scratch + 0x400;
                c.mem->PokeWord(t, c.rng->U32());
                PutS16(*c.mem, t + 2, c.rng->S16());
                args.a[0] = t;
            } else {
                args.a[0] = RqSomeRecord(c, n.pdt, n.nPdt, 12);
            }
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RoadTurnTarget(g, a.a[0]));
        }});

    rows.push_back(Row{
        "road_node_arm", "Arm *NodeArm(BIT *node, s32 road)  [SLUS 0x8003A37C]", rq::kNodeArm,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqNetFor(c);
            uint32_t node = (c.rng->Next() % 10u == 0) ? 0u : RqSomeRecord(c, n.bit, n.nJn, 104);
            if (!c.sample && c.rng->Next() % 5u == 0) {
                node = c.scratch + 0x400;
                for (uint32_t i = 0; i < 104; i += 4) c.mem->PokeWord(node + i, c.rng->U32() & 0x000F000Fu);
                PutS16(*c.mem, node + 2, static_cast<int16_t>(static_cast<int32_t>(c.rng->U32() % 8u) - 1));
            }
            int32_t road = static_cast<int32_t>(c.rng->U32() % 16u);
            if (node != 0 && (c.rng->Next() % 3u != 0))
                road = GetS16(*c.mem, node + 8u + 24u * (c.rng->U32() % 4u) + 4u);
            args.a[0] = node;
            args.a[1] = static_cast<uint32_t>(road);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RoadNodeArm(g, a.a[0], static_cast<int32_t>(a.a[1])));
        }});

    // 0x80039C90 is already ported as `FindRoadPieceIndex` over a host view (row `road_find_piece`);
    // the road query calls it through the guest view, which is a second implementation and gets a
    // row of its own: real junction objects and road ids (dump-derived), and objects of noise with
    // drawn kinds, counts - negative ones included, which the `sltu` end test turns into "no
    // entries" - and keys (randomised).
    rows.push_back(Row{
        "road_find_piece_view", "Grpt *FindRoadPiece(Obj *o, s32 road)  [SLUS 0x80039C90, over guest addresses]",
        kFindRoadPiece,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            uint32_t obj = n.resident.empty() ? 0u : n.resident[c.rng->U32() % n.resident.size()];
            int32_t road = -1;
            if (obj != 0) {
                const int32_t npc = GetS16(*c.mem, obj + 0x12);
                road = GetS32(*c.mem, c.mem->PeekWord(obj + 0x2C) + 32u * RqPick(*c.rng, npc) + 12u);
            }
            if (!c.sample) {
                const uint64_t roll = c.rng->Next() % 4u;
                if (roll == 0) {
                    obj = c.scratch + 0x400;
                    const uint32_t entries = c.scratch + 0x480;
                    for (uint32_t i = 0; i < 0x40; i += 4) c.mem->PokeWord(obj + i, c.rng->U32());
                    PutS16(*c.mem, obj + 0x10, static_cast<int16_t>((c.rng->Next() % 4u == 0) ? (c.rng->U32() % 4u) : 1u));
                    PutS16(*c.mem, obj + 0x12, static_cast<int16_t>(static_cast<int32_t>(c.rng->U32() % 12u) - 2));
                    c.mem->PokeWord(obj + 0x2C, entries);
                    for (uint32_t i = 0; i < 10u * 32u; i += 4) c.mem->PokeWord(entries + i, c.rng->U32() % 16u);
                    road = static_cast<int32_t>(c.rng->U32() % 16u);
                } else if (roll == 1) {
                    road = c.rng->S32();
                } else if (roll == 2) {
                    obj = 0;
                }
            }
            args.a[0] = obj;
            args.a[1] = static_cast<uint32_t>(road);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            rr::sim::GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RoadFindPiece(g, a.a[0], static_cast<int32_t>(a.a[1])));
        }});

    const auto nextRow = [&rows](const char* name, const char* sig, uint32_t entry,
                                 uint32_t (*fn)(GuestRam&, uint32_t, int32_t)) {
        rows.push_back(Row{
            name, sig, entry,
            [](CaseContext& c, Args& args) {
                const RqNet real = RqRead(*c.mem);
                if (!c.sample) RqStirResidency(c, real);
                const RqNet n = RqNetFor(c);
                uint32_t obj = real.resident.empty() ? 0u : real.resident[c.rng->U32() % real.resident.size()];
                if (!c.sample && c.rng->Next() % 3u == 0) {
                    // an object whose id is drawn, so every kind of BTT_/BST_/BIT_ answer comes up
                    obj = c.scratch + 0x400;
                    for (uint32_t i = 0; i < 0x40; i += 4) c.mem->PokeWord(obj + i, 0);
                    PutS32(*c.mem, obj + 0, RqKey(c, n));
                }
                int32_t road = -1;
                if (c.rng->Next() % 4u != 0) {
                    const uint32_t node = RqSomeRecord(c, n.bit, n.nJn, 104);
                    road = GetS16(*c.mem, node + 8u + 24u * (c.rng->U32() % 4u) + 4u);
                } else if (c.rng->Next() & 1u) {
                    road = static_cast<int32_t>(c.rng->U32() % 64u);
                }
                args.a[0] = c.rng->U32(); // never read by the original
                args.a[1] = obj;
                args.a[2] = static_cast<uint32_t>(road);
            },
            nullptr, /*compareV0=*/true,
            [fn](NativeEnv& env, const Args& a) -> uint32_t {
                GuestRam g = RqView(env);
                return RqDone(env, g, fn(g, a.a[1], static_cast<int32_t>(a.a[2])));
            }});
    };
    nextRow("road_next_object_fwd", "Obj *NextObjectFwd(p, Obj *o, s32 road)  [SLUS 0x8003C840]",
            rq::kNextFwd, rr::sim::RoadNextObjectFwd);
    nextRow("road_next_object_bwd", "Obj *NextObjectBwd(p, Obj *o, s32 road)  [SLUS 0x8003C948]",
            rq::kNextBwd, rr::sim::RoadNextObjectBwd);

    // The route lookups. A synthetic leg in the scratch block half the randomised time, so the
    // count, both lists and the one-past-the-end leg of 0x8003F5D0 are all under the row's control.
    const auto routeSetup = [](CaseContext& c, Args& args, bool takesLeg) {
        const RqNet n = RqRead(*c.mem);
        if (!c.sample) RqStirRoute(c, n);
        const int32_t count = GetS16(*c.mem, rq::kRoute + 18);
        const uint32_t legs = c.mem->PeekWord(rq::kRoute + 36);
        uint32_t leg = legs + 120u * RqPick(*c.rng, count + 1);
        if (!c.sample && (c.rng->Next() & 1u)) {
            leg = c.scratch + 0x400;
            for (uint32_t i = 0; i < 120; i += 4) PutS32(*c.mem, leg + i, RqSomeRoad(c, n));
            PutS32(*c.mem, leg + 16, static_cast<int32_t>(c.rng->U32() % 6u) - 1);
        }
        if (c.rng->Next() % 10u == 0) leg = 0;
        int32_t road;
        const uint64_t roll = c.rng->Next() % 5u;
        if (leg != 0 && roll == 0) road = GetS32(*c.mem, leg);
        else if (leg != 0 && roll <= 2) road = GetS32(*c.mem, leg + ((roll == 1) ? 84u : 100u) + 4u * (c.rng->U32() % 4u));
        else if (roll == 3) road = -1;
        else road = RqSomeRoad(c, n);
        if (takesLeg) {
            args.a[0] = leg;
            args.a[1] = static_cast<uint32_t>(road);
        } else {
            args.a[0] = static_cast<uint32_t>(road);
        }
    };
    rows.push_back(Row{
        "route_leg_of_road", "Leg *RouteLegOfRoad(s32 road)  [SLUS 0x8003F3B4]", rq::kLegOfRoad,
        [routeSetup](CaseContext& c, Args& args) { routeSetup(c, args, false); },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RouteLegOfRoad(g, static_cast<int32_t>(a.a[0])));
        }});
    rows.push_back(Row{
        "route_leg_for", "Leg *RouteLegFor(Leg *leg, s32 road)  [SLUS 0x8003F408]", rq::kLegFor,
        [routeSetup](CaseContext& c, Args& args) { routeSetup(c, args, true); },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RouteLegFor(g, a.a[0], static_cast<int32_t>(a.a[1])));
        }});
    rows.push_back(Row{
        "route_leg_has_road", "s32 RouteLegHasRoad(Leg *leg, s32 road)  [SLUS 0x8003F580]", rq::kLegHas,
        [routeSetup](CaseContext& c, Args& args) { routeSetup(c, args, true); },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::RouteLegHasRoad(g, a.a[0], static_cast<int32_t>(a.a[1]))));
        }});
    rows.push_back(Row{
        "route_leg_containing", "Leg *RouteLegContaining(s32 road)  [SLUS 0x8003F5D0]",
        rq::kLegContaining,
        [routeSetup](CaseContext& c, Args& args) { routeSetup(c, args, false); },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RouteLegContaining(g, static_cast<int32_t>(a.a[0])));
        }});

    rows.push_back(Row{
        "road_next_object_missing", "s32 NextObjectMissing(const Cursor *c, s32 dir)  [SLUS 0x800394F0]",
        rq::kMissing,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            uint32_t cur;
            if (c.sample) {
                cur = RqBike(c, n, false) + 0x148u;               // a live bike's own cursor
            } else {
                RqStirResidency(c, n);
                cur = c.scratch + 0x400;
                RqPlantCursor(c, n, cur);
                if (c.rng->Next() % 12u == 0) c.mem->PokeWord(cur, 0);
            }
            static const int32_t kDir[5] = {1, -1, 0, 1, -1};
            args.a[0] = cur;
            args.a[1] = (c.rng->Next() % 6u == 0) ? c.rng->U32() : static_cast<uint32_t>(kDir[c.rng->U32() % 5u]);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::RoadNextObjectMissing(g, a.a[0], static_cast<int32_t>(a.a[1]))));
        }});

    // ---------------------------------------------------------------- 0x8003697C
    rows.push_back(Row{
        "road_along_from_anchor",
        "s32 AlongFromAnchor(s32 oldDir, s32 newDir, Slice *s, const s32 p[3])  [SLUS 0x8003697C]",
        rq::kAlong,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t tmp = c.scratch + 0x400;
            uint32_t slice = RqPlantCursor(c, n, tmp);
            if (!c.sample && c.rng->Next() % 3u == 0) {
                // a slice made of noise: full-range tangent, origin and chord
                slice = c.scratch + 0x440;
                for (uint32_t i = 0; i < 52; i += 4) c.mem->PokeWord(slice + i, c.rng->U32());
            }
            const uint32_t point = c.scratch + 0x480;
            if (c.sample) PutVec3S32(*c.mem, point, (c.rng->Next() & 1u) ? c.sample->p : c.sample->q);
            else if (c.rng->Next() & 1u) RqPlantPoint(c, slice, point);
            else for (uint32_t k = 0; k < 3; ++k) PutS32(*c.mem, point + 4u * k, c.rng->World());
            static const int32_t kDir[4] = {1, -1, 0, 2};
            const auto dir = [&c]() {
                return (c.rng->Next() % 8u == 0) ? c.rng->U32() : static_cast<uint32_t>(kDir[c.rng->U32() % 4u]);
            };
            args.a[0] = dir();
            args.a[1] = dir();
            args.a[2] = slice;
            args.a[3] = point;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::RoadAlongFromAnchor(
                                      g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), a.a[2], a.a[3])));
        }});

    // ---------------------------------------------------------------- 0x80037A30 / 0x80037FBC
    // `out` and `dirs` are PRE-FILLED with noise: the partial candidate writes 8 of a slot's 32
    // bytes and leaves the rest as they were, and that is compared here byte
    // for byte because both arrays are in the scratch block.
    const auto neighbourRow = [&rows](const char* name, const char* sig, uint32_t entry, bool fwd) {
        rows.push_back(Row{
            name, sig, entry,
            [](CaseContext& c, Args& args) {
                const RqNet n = RqRead(*c.mem);
                const uint32_t e = RqBike(c, n, c.sample == nullptr);
                uint32_t in = e + 0x148u;
                if (!c.sample && (c.rng->Next() & 1u)) {
                    in = c.scratch + 0x400;
                    RqPlantCursor(c, n, in);
                }
                if (!c.sample) RqStirTurns(c, n, 2);
                const uint32_t out = c.scratch + 0x200;
                const uint32_t dirs = c.scratch + 0x300;
                for (uint32_t i = 0; i < 96; i += 4) c.mem->PokeWord(out + i, c.rng->U32());
                for (uint32_t i = 0; i < 16; i += 4) c.mem->PokeWord(dirs + i, c.rng->U32());
                if (!c.sample) RqStackJunk(c, 256);
                args.a[0] = e + 0xACu;
                args.a[1] = in;
                args.a[2] = out;
                args.a[3] = dirs;
                args.a4 = (c.rng->Next() % 4u != 0) ? 3u : static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 6u) - 1);
            },
            nullptr, /*compareV0=*/true,
            [fwd](NativeEnv& env, const Args& a) -> uint32_t {
                GuestRam g = RqView(env);
                const int32_t v = fwd ? rr::sim::RoadNeighboursForward(g, a.a[0], a.a[1], a.a[2], a.a[3],
                                                                      static_cast<int32_t>(a.a4), env.sp)
                                      : rr::sim::RoadNeighboursBackward(g, a.a[0], a.a[1], a.a[2], a.a[3],
                                                                       static_cast<int32_t>(a.a4), env.sp);
                return RqDone(env, g, static_cast<uint32_t>(v));
            }});
    };
    neighbourRow("road_neighbours_fwd",
                 "s32 NeighboursForward(p, Cursor *in, Cursor out[3], s32 dirs[], s32 max)  [SLUS 0x80037A30]",
                 rq::kNeighFwd, true);
    neighbourRow("road_neighbours_bwd",
                 "s32 NeighboursBackward(p, Cursor *in, Cursor out[3], s32 dirs[], s32 max)  [SLUS 0x80037FBC]",
                 rq::kNeighBwd, false);

    // A candidate array the way the neighbour functions leave one: slots re-seated on resident
    // road, the turn-table fields (+0x18 junction, +0x1C turn) set on most, and 32 bytes of noise
    // BEFORE the array for the index -1 pick.
    const auto plantCandidates = [](CaseContext& c, const RqNet& n, uint32_t cand, uint32_t dirs) {
        for (uint32_t i = 0; i < 32; i += 4) c.mem->PokeWord(cand - 32u + i, c.rng->U32());
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t slot = cand + 32u * k;
            RqPlantCursor(c, n, slot);
            c.mem->PokeWord(slot + 24, (n.nIpt > 0) ? n.ipt + 12u * RqPick(*c.rng, n.nIpt) : 0u);
            c.mem->PokeWord(slot + 28, (n.nPdt > 0 && c.rng->Next() % 5u != 0) ? n.pdt + 12u * RqPick(*c.rng, n.nPdt) : 0u);
        }
        for (uint32_t k = 0; k < 4; ++k) {
            const uint64_t roll = c.rng->Next() % 5u;
            c.mem->PokeWord(dirs - 4u + 4u * k, (roll < 2) ? 1u : (roll < 4) ? 0xFFFFFFFFu : c.rng->U32());
        }
    };

    // ---------------------------------------------------------------- 0x8003CCA0
    rows.push_back(Row{
        "ai_junction_choice", "s32 AiJunctionChoice(Entity *e, Cursor cand[], s32 n)  [SLUS 0x8003CCA0]",
        rq::kJunctionChoice,
        [plantCandidates](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RqBike(c, n, c.sample == nullptr);
            if (c.sample) RqStirRoute(c, n); // the live route, with its lists re-drawn now and then
            const uint32_t cand = c.scratch + 0x220;
            const uint32_t dirs = c.scratch + 0x300;
            plantCandidates(c, n, cand, dirs);
            const uint64_t roll = c.rng->Next() % 8u;
            args.a[0] = e;
            args.a[1] = cand;
            args.a[2] = (roll < 4) ? 2u : (roll < 6) ? 3u : static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 6u) - 1);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::AiJunctionChoice(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]))));
        }});

    // ---------------------------------------------------------------- 0x80039048
    rows.push_back(Row{
        "road_pick_neighbour",
        "Slice *PickNeighbour(p, Cursor cand[], s32 dirs[], s32 n, Cursor *out, s32 *dirOut)  [SLUS 0x80039048]",
        rq::kPick,
        [plantCandidates](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RqBike(c, n, c.sample == nullptr);
            const uint32_t cand = c.scratch + 0x220;
            const uint32_t dirs = c.scratch + 0x304;
            plantCandidates(c, n, cand, dirs);
            if (c.rng->Next() % 3u == 0) c.mem->PokeWord(cand + 28, 0); // "not a turn-table candidate"
            const uint32_t out = c.scratch + 0x340;
            RqPlantCursor(c, n, out);
            if (c.rng->Next() & 1u) c.mem->PokeWord(out + 8, c.mem->PeekWord(cand + 8));
            const uint32_t dirOut = c.scratch + 0x380;
            c.mem->PokeWord(dirOut, c.rng->U32());
            if (!c.sample) {
                // the remembered junction IS this one, one case in four
                if (c.rng->Next() % 4u == 0) {
                    c.mem->PokeWord(e + 948, c.mem->PeekWord(cand + 24));
                    const uint8_t ch = static_cast<uint8_t>(c.rng->U32() % 2u);
                    c.mem->WriteBlock(e + 947, &ch, 1);
                }
                // every candidate's destination one of the four roads the traffic rule refuses
                if (c.rng->Next() % 5u == 0) {
                    static const int16_t kRefused[4] = {11, 12, 20, 26};
                    for (uint32_t k = 0; k < 3; ++k) {
                        const uint32_t t = c.scratch + 0x3A0u + 12u * k;
                        for (uint32_t i = 0; i < 12; i += 4) c.mem->PokeWord(t + i, c.rng->U32());
                        PutS16(*c.mem, t + 10, kRefused[c.rng->U32() % 4u]);
                        c.mem->PokeWord(cand + 32u * k + 28u, t);
                    }
                }
            }
            // p: the bike (pool 0), a traffic handle (pool 3), any other pool, or none at all.
            uint32_t p = e + 0xACu;
            const uint64_t pr = c.rng->Next() % 8u;
            if (!c.sample && pr < 3) {
                p = c.scratch + 0x390;
                // pool 3 two times in three, any other pool the rest - with the bound RqBike names:
                // a pool-0 handle is one of the live slots (measured without it on the second seed:
                // 4 of 2048 cases trapped at 0x80039350 on a junk e[+0x3B4] past the live pool)
                const uint32_t nb = std::max<uint32_t>(1u, std::min<uint32_t>(c.mem->PeekWord(kLiveBikes), 18u));
                const uint32_t pool = c.rng->U32() % 8u;
                const uint16_t h = (pr < 2) ? static_cast<uint16_t>(0x60u | (c.rng->U32() % 32u))
                                   : (pool == 0) ? static_cast<uint16_t>(c.rng->U32() % nb)
                                                 : static_cast<uint16_t>((pool << 5) | (c.rng->U32() % 32u));
                WriteU16Bench(*c.mem, p, h);
                const uint32_t veh = kTrafficRecords + 512u * (h & 0x1Fu);
                c.mem->PokeWord(veh + 180, (c.rng->Next() % 3u == 0) ? c.rng->U32() : 0u);
                const uint8_t b = static_cast<uint8_t>(c.rng->U32() % 24u);
                c.mem->WriteBlock(veh + 511, &b, 1);
                // cand[0]'s road on the route half the time, and the "follow a bike" flag with it
                const int32_t count = GetS16(*c.mem, rq::kRoute + 18);
                if (count > 0 && (c.rng->Next() & 1u)) {
                    const uint32_t leg = c.mem->PeekWord(rq::kRoute + 36) + 120u * RqPick(*c.rng, count);
                    c.mem->PokeWord(leg, c.mem->PeekWord(c.mem->PeekWord(cand + 4) + 12u));
                    const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
                    uint8_t f = static_cast<uint8_t>(c.rng->U32());
                    if (c.rng->Next() & 1u) f = static_cast<uint8_t>(f | 1u);
                    c.mem->WriteBlock(gs + 4, &f, 1);
                }
            } else if (!c.sample && pr == 3) {
                p = 0;
            }
            const uint64_t roll = c.rng->Next() % 8u;
            args.a[0] = p;
            args.a[1] = cand;
            args.a[2] = dirs;
            args.a[3] = (roll < 3) ? 2u : (roll < 5) ? 1u : (roll < 7) ? 3u : c.rng->U32() % 5u;
            args.a4 = out;
            args.more[0] = dirOut;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RoadPickNeighbour(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]),
                                                             a.a4, a.more[0]));
        }});

    // ---------------------------------------------------------------- 0x80036B14
    rows.push_back(Row{
        "road_slice_search", "Slice *RoadSliceSearch(u8 *p, Cursor *c, const s32 point[3])  [SLUS 0x80036B14]",
        rq::kSearch,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            // NAMED BOUND: no residency stir here. A stirred BTT_ can close the road into a RING
            // (an object that is its own successor), and on a ring the original walks forever -
            // measured: with the stir, 5 of 512 randomised cases ran 200 million instructions
            // without returning (consistent with a ring; the loop itself was not traced). The
            // neighbour and look-ahead rows stir, their walks being bounded; the search walks the
            // snapshot's own residency, whose non-resident neighbours drive its missing-object exit.
            const uint32_t e = RqBike(c, n, c.sample == nullptr, /*stirResidency=*/false);
            uint32_t cur = e + 0x148u;
            uint32_t point = e + 0xB8u;                          // 0x8003701C: the box centre
            if (c.sample) {
                // the bike's own box centre, or a captured bike's from another RAM image
                if (c.rng->Next() & 1u) {
                    point = c.scratch + 0x480;
                    PutVec3S32(*c.mem, point, c.sample->q);
                }
            } else {
                if (c.rng->Next() % 3u == 0) {
                    // 0x80037104's form: a PRIVATE copy with +0x10..+0x1C zeroed
                    cur = c.scratch + 0x400;
                    for (uint32_t i = 0; i < 16; i += 4) c.mem->PokeWord(cur + i, c.mem->PeekWord(e + 0x148u + i));
                    for (uint32_t i = 16; i < 32; i += 4) c.mem->PokeWord(cur + i, 0);
                }
                point = c.scratch + 0x480;
                RqPlantPoint(c, c.mem->PeekWord(cur + 12), point);
                RqPlantTags(c, c.mem->PeekWord(cur + 0));
                RqStackJunk(c, 512);
            }
            args.a[0] = e + 0xACu;
            args.a[1] = cur;
            args.a[2] = point;
            args.a[3] = c.rng->U32(); // never read
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            return RqDone(env, g, rr::sim::RoadSliceSearch(g, a.a[0], a.a[1], a.a[2], env.sp));
        }});

    // ---------------------------------------------------------------- SLUS 0x8002E080
    // The builder of the run-time reciprocal-square-root table, which the
    // contact response needs and the product did not have. It takes no arguments: its inputs are
    // the heap state and SqrtGte's table at 0x800560CC. The dump-derived half runs it on the
    // snapshot's own table; the randomised half plants noise over the table AND the 640 bytes below
    // it that a negative argument reaches, so every shift and the `r == 0` arm (`sw zero,16(sp)` at
    // 0x8002E0F4) run. The allocation is the game's own `malloc` (0x8001447C, two arguments),
    // supplied by the oracle at the builder's frame depth (`addiu sp,sp,-40`).
    rows.push_back(Row{
        "rsqrt_table_build", "void BuildRsqrtTable(void)  [SLUS 0x8002E080, *(gp+2260) = malloc(2048)]",
        0x8002E080,
        [](CaseContext& c, Args& args) {
            (void)args;
            if (c.sample) return;
            for (uint32_t i = 0; i < 640u + 256u; i += 2) {
                const uint64_t roll = c.rng->Next() % 8u;
                WriteU16Bench(*c.mem, kSqrtTable - 640u + i,
                              (roll == 0) ? static_cast<uint16_t>(0)
                                          : static_cast<uint16_t>(c.rng->U32() >> ((roll < 4) ? 16 : 20)));
            }
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args&) -> uint32_t {
            Memory& m = env.clone->mem;
            std::vector<int16_t> window(kSqrtWindowHalf, 0);
            m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, window.data(), window.size() * 2u);
            const uint32_t ma[2] = {2048, 0}; // PORTED (spine.h SpineMalloc), witnessed
            const uint32_t table = SpineNativeCall(env, kMallocEntry, ma, 2);
            m.PokeWord(env.gp + rr::sim::kRsqrtTableGp, table);
            std::vector<uint16_t> out(1024, 0);
            rr::sim::FillRsqrtTable(out.data(), window.data() + kSqrtWindowHalf / 4u);
            // through the view, so a failed allocation (0) writes where the console would: guest 0
            rr::sim::GuestRam g = RqView(env);
            for (uint32_t i = 0; i < 1024; ++i) g.W16(table + 2u * i, out[i]);
            return RqDone(env, g, 0u);
        },
        {OracleCallee{kMallocEntry, 2}}, /*oracleFrame=*/40});

    // ---------------------------------------------------------------- 0x800386DC
    rows.push_back(Row{
        "road_look_ahead",
        "void RoadLookAhead(e, s32 ahead, s32 along, s32 dir, Cursor*, s32 out[3], Cursor *outCur)  [SLUS 0x800386DC]",
        rq::kLookAhead,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RqBike(c, n, c.sample == nullptr);
            const uint32_t outCursor = c.scratch + 0x400;
            for (uint32_t i = 0; i < 32; i += 4) c.mem->PokeWord(outCursor + i, c.rng->U32());
            const int32_t chord = GetS32(*c.mem, c.mem->PeekWord(e + 0x154) + 32u);
            int32_t ahead, along, dir;
            if (c.sample) {
                // AiDrive's own call (0x80095574): the bike's real along-distance and direction of
                // travel, and an `ahead` in AiDrive's clamp range (15..80 units) out of a captured
                // speed.
                ahead = 0x000F0000 + static_cast<int32_t>(static_cast<uint32_t>(c.sample->speed) % 0x00410001u);
                along = GetS32(*c.mem, e + 0x15C);
                dir = GetS32(*c.mem, e + 0x16C);
            } else {
                static const int32_t kSpan[3] = {80, 600, 4000};
                ahead = static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(kSpan[c.rng->U32() % 3u] * 65536));
                // within the current slice one case in three: that is when the target can lie
                // before A or beyond B and the degenerate quadratic arm runs
                if (c.rng->Next() % 3u == 0)
                    ahead = (chord > 0) ? static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(chord / 2 + 1)) : 0;
                along = (chord > 3) ? static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(chord + 2 * (chord / 3))) - chord / 3
                                    : 0;
                static const int32_t kDir[5] = {1, 1, -1, -1, 0};
                dir = (c.rng->Next() % 10u == 0) ? c.rng->S32() : kDir[c.rng->U32() % 5u];
                if (c.rng->Next() % 8u == 0) {
                    // The boundary family: the target exactly half-way (the
                    // `slt` at 0x80038A44), exactly on A's start (t == 0: the `blez` arms of the
                    // signed divide at 0x80038CA0) or exactly on its end (t == len, 0x80038EC0).
                    ahead = 0;
                    const uint64_t b = c.rng->Next() % 3u;
                    along = (b == 0) ? (chord >> 1) : (b == 1) ? 0 : chord;
                    dir = (b == 0) ? ((c.rng->Next() & 1u) ? 1 : -1) : 1;
                }
                RqStirTurns(c, n);
                RqStirChords(c, c.mem->PeekWord(e + 0x154));
                RqStirSubCounts(c, n);
                // along up to three chords past the start one case in four: with a small `ahead`
                // and a walk backward that puts the target BEYOND B (the quadratic arm's second
                // form) even when B was entered against the walk
                if (chord > 3 && c.rng->Next() % 4u == 0)
                    along = static_cast<int32_t>(c.rng->U32() % (3u * static_cast<uint32_t>(chord)));
                // The CORE family, one case in eight: the cursor at a core sub-object's end with a
                // turn, `ahead` 0, and the target either just BEFORE A walking forward (every
                // sub-object of the object cut to one slice, so the slice before A is a one-slice
                // sub-object entered against the walk: the fifth, ignored `jalr` at 0x80038C50) or
                // three to eight chords BEYOND it walking backward (B entered against the walk and
                // then the far quadratic arm, 0x80038E1C and 0x80038E78).
                if (c.rng->Next() % 8u == 0) {
                    const bool before = (c.rng->Next() & 1u) != 0;
                    const uint32_t sl = RqPlantCoreCursor(c, n, e + 0x148u, before, before);
                    const int32_t ch = (sl != 0) ? GetS32(*c.mem, sl + 32) : 0;
                    if (sl != 0 && ch > 2) {
                        ahead = 0;
                        dir = before ? 1 : -1;
                        along = before ? -1 - static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(ch / 2))
                                       : static_cast<int32_t>(static_cast<uint32_t>(ch) * (3u + c.rng->U32() % 6u));
                    }
                }
                RqStackJunk(c, 640);
            }
            args.a[0] = e;
            args.a[1] = static_cast<uint32_t>(ahead);
            args.a[2] = static_cast<uint32_t>(along);
            args.a[3] = static_cast<uint32_t>(dir);
            args.a4 = e + 0x148u;
            args.more[0] = e + 0x370u;
            args.more[1] = (c.rng->Next() % 10u == 0) ? 0u : outCursor;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RqView(env);
            rr::sim::RoadLookAhead(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                                   static_cast<int32_t>(a.a[3]), a.a4, a.more[0], a.more[1], env.sp);
            return RqDone(env, g, 0u);
        }});
    // A walk through a sub-object the family cut to one slice can run past its object's SLCT array
    // before it meets an index the end test stops on: one case of the second seed ran more than the
    // default 5 million instructions (and matched the port bit for bit once it could finish).
    rows.back().maxSteps = 200'000'000;
}

// ---------------------------------------------------------------- the runner

struct Options {
    std::string stateDir = "work\\oracle\\state\\rr-race";
    std::string stateRoot = "work\\oracle\\state";
    std::string dumpRoot = "work\\oracle\\vr_capture\\ramdumps";
    int randomCases = 512;
    int derivedCasesPerSample = 2;
    uint64_t seed = 0x5251524A42303031ull; // "RQRJB001"
    std::string only;
    bool list = false;
    bool verbose = false;
    // Negative control. With it on, the native side's answer is perturbed by one bit (v0, and one
    // byte of the scratch block so that memory-writing rows are covered too). Every row must then
    // FAIL. A bench that cannot be made to fail proves nothing, so this switch is part of the
    // evidence rather than a debugging aid.
    bool mutateNative = false;
    // The negative control for the oracle-supplied-callee seam: swallow the port's requests instead
    // of executing them. Every row that uses the seam must then FAIL.
    bool skipOracle = false;
    // The negative control for the CALLER-FRAME OUTPUT BUFFER comparison, and nothing else: flip
    // one bit of the bytes the port's callee left in the bench's buffer. Exactly the rows that
    // declare such a buffer must then FAIL, and every other row must still PASS - which is what
    // says that the new comparison is doing work rather than being vacuously green.
    bool mutateFrameOut = false;
    // The negative control for the bike step's SPLICE rows: the region is skipped
    // instead of spliced. Exactly the splice rows must FAIL.
    bool spliceControl = false;
    // The two controls of the SPU control register file. `--no-spu-file` runs
    // with the file switched OFF, i.e. exactly the bench as it was before it existed: every row
    // that does not depend on it must still pass, and the rows that drive libspu's register pair
    // must FAIL (on a trap). `--mutate-spu-file` flips one bit of the port's register file after
    // every case: exactly the rows that compare it must FAIL.
    bool noSpuFile = false;
    bool mutateSpuFile = false;
    // A diagnostic, not a control: for the rows whose function extent is listed in
    // `kCoverageRanges`, count how many distinct instructions of the ORIGINAL the guest run
    // executed over the whole row. A green row whose family never reaches half its function proves
    // half a function; this says which.
    bool coverage = false;
};

// The extent [entry, end) of each function whose coverage `--coverage` measures, read out of our
// own disassembly (the address after the delay slot of its last `jr ra`).
struct CoverageRange {
    uint32_t entry, end;
};
constexpr CoverageRange kCoverageRanges[] = {
    {kAsinFn, 0x80020018},        {kRotMatrixFn, 0x8004D530},  {kMulMatrix0Fn, 0x8003FB34},
    {kNormalize32Fn, 0x8002E388}, {kReleaseContactFn, 0x8002090C}, {kCrashLaunchFn, 0x800723FC},
    {kWipeoutStartFn, 0x80072994}, {kIntegrateFn, 0x8007FA4C},  {kObstacleTestFn, 0x80075B08},
    {kContactFrameFn, 0x800807F0}, {kGroundFrame, 0x80075628},  {kGripLimitFn, 0x8007F08C},
    {kPassengerSteerFn, 0x80072FB4}, {kSteerPassFn, 0x80074D58},
    // the road query, SLUS_010.53
    {rq::kMemCopy, 0x8001E0DC},   {rq::kBtt, 0x80039AA0},       {rq::kBst, 0x80039AFC},
    {rq::kNode, 0x80039B60},      {rq::kJunction, 0x80039BB0},  {rq::kTurnsFrom, 0x80039C38},
    {rq::kTurnTarget, 0x80039C90}, {rq::kNodeArm, 0x8003A3CC},  {rq::kNextFwd, 0x8003C948},
    {rq::kNextBwd, 0x8003CA50},   {rq::kLegOfRoad, 0x8003F408}, {rq::kLegFor, 0x8003F4D8},
    {rq::kLegHas, 0x8003F5D0},    {rq::kLegContaining, 0x8003F680}, {rq::kMissing, 0x800396A8},
    {rq::kAlong, 0x80036B14},     {rq::kNeighFwd, 0x80037FBC},  {rq::kNeighBwd, 0x80038550},
    {rq::kJunctionChoice, 0x8003CEC4}, {rq::kPick, 0x800394F0}, {rq::kSearch, 0x8003701C},
    {rq::kLookAhead, 0x80039048}, {0x8002E080, 0x8002E14C}, {kFindRoadPiece, 0x80039CFC},
    // the road runtime layer, SLUS_010.53; rows in rows_road_runtime.inc
    {0x8003E67C, 0x8003E754}, {0x8003DCB8, 0x8003DDB0}, {0x8003F1F0, 0x8003F204}, {0x8003B4B0, 0x8003B520},
    {0x80036800, 0x8003697C}, {0x8002EAD8, 0x8002EB78}, {0x8003EF34, 0x8003F1F0}, {0x8003EE68, 0x8003EF34},
    {0x8003DE28, 0x8003DF54}, {0x8003DDB0, 0x8003DE28}, {0x8003DF54, 0x8003DFF4}, {0x8003662C, 0x80036800},
    {0x8003DFF4, 0x8003E150}, {0x8003E150, 0x8003E1E8}, {0x8003BE1C, 0x8003BFE8}, {0x8003B61C, 0x8003B8F4},
    {0x8003B520, 0x8003B61C}, {0x8003AF9C, 0x8003B024}, {0x8003A9D8, 0x8003AE24}, {0x8003AE24, 0x8003AF9C},
    {0x8003701C, 0x80037104}, {0x800396A8, 0x80039A08}, {0x80037104, 0x80037338}, {0x80037338, 0x80037450},
    {0x80037450, 0x800374D4}, {0x800374D4, 0x80037524},
    // under the re-bind, SLUS_010.53; rows in rows_road_runtime.inc (AddRoadRebindRows)
    {0x800245DC, 0x800245F4}, {0x800245F4, 0x80024610}, {0x8003A5F4, 0x8003A700}, {0x8003F4D8, 0x8003F580},
    {0x8003B024, 0x8003B1C4}, {0x8003B1C4, 0x8003B4B0}, {0x8003BD2C, 0x8003BE1C}, {0x8003E61C, 0x8003E67C},
    {0x8003E45C, 0x8003E61C}, {0x8003ED14, 0x8003EE68}, {0x8003EB58, 0x8003ED14}, {0x8003E754, 0x8003EB58},
    {0x8003F204, 0x8003F3B4}, {0x8003BFE8, 0x8003C42C},
    // the engine note, SLUS_010.53; rows in rows_engine_note.inc
    {0x800506A8, 0x80050964}, {0x80043F00, 0x80043F38}, {0x8001FB58, 0x8001FBD4}, {0x8001F934, 0x8001F9C4},
    {0x8001F7EC, 0x8001F874}, {0x8001F874, 0x8001F900}, {0x8001F900, 0x8001F934}, {0x80019C54, 0x80019D9C},
    {0x8001F6A4, 0x8001F7EC}, {0x800167A4, 0x800169D0}, {0x800169D0, 0x80016E4C}, {0x80016E4C, 0x8001769C},
    {0x80050D08, 0x80050EC4}, {0x8001EB7C, 0x8001EBF0}, {0x8001EE94, 0x8001EFAC}, {0x80019990, 0x80019C54},
    {0x8001654C, 0x80016768}, {0x80019D9C, 0x80019E40},
    // AudioFrame and the object sounds; rows in rows_engine_note.inc (AddSoundFrameRows)
    {0x80018FAC, 0x80019990}, {0x8001769C, 0x80017814}, {0x80017814, 0x800179D8}, {0x800179D8, 0x80017B30},
    {0x80017F64, 0x80018260}, {0x800184AC, 0x80018C1C}, {0x8001B244, 0x8001B3C8}, {0x8008AE94, 0x8008B428},
    {0x8001F054, 0x8001F080}, {0x8001F0D4, 0x8001F174}, {0x8001E5A8, 0x8001E614}, {0x8001FBD4, 0x8001FC58},
    {0x8001EAA4, 0x8001EB28}, {0x80063448, 0x80063500}, {0x80024B20, 0x80024CB4}, {0x8001F37C, 0x8001F544},
    {0x8001F544, 0x8001F5D4},
    // the rider animation clock and the stance layer, RASHCDG + SLUS 0x80012858; rows_anim.inc
    {0x8005C4EC, 0x8005C52C}, {0x8005C418, 0x8005C4EC}, {0x8005BE58, 0x8005BEF4}, {0x8005BEF4, 0x8005BF6C},
    {0x8005BE0C, 0x8005BE20}, {0x8005BE20, 0x8005BE44}, {0x8005BE44, 0x8005BE58}, {0x8005C338, 0x8005C36C},
    {0x8005C36C, 0x8005C39C}, {0x80012858, 0x80012884}, {0x80068D20, 0x80068D50}, {0x8005E2BC, 0x8005E558},
    {0x8005C39C, 0x8005C418}, {0x8005C52C, 0x8005C58C}, {0x8005C58C, 0x8005C8F4}, {0x8005C8F4, 0x8005CB04},
    {0x8005CB04, 0x8005CB70}, {0x8005BD74, 0x8005BE0C}, {0x8005BF6C, 0x8005C018}, {0x8005C018, 0x8005C0B0},
    {0x8005C0B0, 0x8005C140}, {0x8005C140, 0x8005C338}, {0x8005E1D8, 0x8005E2BC}, {0x800C2FF4, 0x800C3104},
    {0x800C37B0, 0x800C3950}, {0x800C4500, 0x800C4550}, {0x800C4454, 0x800C4500}, {0x800C3E9C, 0x800C4454},
    {0x800C4550, 0x800C45D8}, {0x80090D84, 0x80091468}, {0x80091468, 0x8009246C},
    // the crash chain, RASHCDG; rows in rows_crash.inc
    {0x80078DB4, 0x80079B20}, {0x800849D8, 0x80084BE8}, {0x80084564, 0x800849D8}, {0x8002EA20, 0x8002EAD8},
    {0x800903F4, 0x80090814}, {0x80081D7C, 0x80083864},
    // the ground query, RASHCDG; rows in rows_ground.inc
    {0x800B6E08, 0x800B6F40}, {0x800B6844, 0x800B6AAC}, {0x800A8498, 0x800A8BE0}, {0x800A7BF8, 0x800A8498},
    {0x8008DBCC, 0x8008DCA0},
    // op 9's strike and the audio pause; rows in rows_strike.inc
    {0x800C1370, 0x800C159C}, {0x80018C1C, 0x80018DC8},
    // the hit sounds: the animation's sound events, SLUS; rows in rows_anim_sound.inc
    {0x80018E54, 0x80018FAC}, {0x80017DA0, 0x80017F64},
};
// rows_bike_step.inc: the extent a bike-step row's coverage is measured over, by row name - a
// splice row's entry is the whole step, but what it proves is one region of it.
const CoverageRange* BikeStepCoverage(const char* rowName);
const CoverageRange* PopulationCoverage(const char* rowName); // rows_population.inc, by row name
const CoverageRange* PoliceCoverage(const char* rowName);       // rows_police.inc (via rows_traffic.inc)
const CoverageRange* TrafficDriveCoverage(const char* rowName); // rows_traffic_drive.inc (via rows_traffic.inc)
const CoverageRange* CameraCoverage(const char* rowName);     // rows_camera.inc, by row name
const CoverageRange* CollisionCoverage(const char* rowName);  // rows_collision.inc, by row name
const CoverageRange* CollCoverage(const char* rowName);       // rows_coll.inc, by row name
const CoverageRange* PoseCoverage(const char* rowName);       // rows_pose.inc, by row name
const CoverageRange* AiBrainCoverage(const char* rowName);    // rows_ai_brain.inc, by row name
const CoverageRange* RiderCoverage(const char* rowName);      // rows_riders.inc, by row name
const CoverageRange* SpeechCoverage(const char* rowName);     // rows_speech.inc, by row name
const CoverageRange* SolidCoverage(const char* rowName);      // rows_solid.inc, by row name
const CoverageRange* PassesCoverage(const char* rowName);     // rows_passes.inc, by row name
const CoverageRange* HazardCoverage(const char* rowName);     // rows_hazards.inc, by row name
const CoverageRange* PartnersCoverage(const char* rowName);   // rows_partners.inc, by row name
const CoverageRange* CopsCoverage(const char* rowName);       // rows_cops.inc, by row name

struct RowResult {
    int derived = 0, random = 0;
    // Coverage, so a row that quietly returned early on every case cannot look like a pass.
    int effective = 0;   // the guest actually changed the scratch block
    int v0NonZero = 0;   // the guest returned something other than 0
    int v0Fail = 0, ramFail = 0, spadFail = 0, trapFail = 0;
    // The oracle-supplied-callee seam: how many such calls the GUEST made over the whole row
    // (coverage), and how many cases in which our port's call sequence did not match it.
    int oracleCalls = 0, calleeFail = 0;
    // The caller-frame output buffers: how many were compared byte for byte (coverage) and how many
    // differed. A declared buffer that is never compared is a hole, so the count is printed.
    int frameOutCompared = 0, frameOutFail = 0;
    uint32_t firstRamDiff = 0;
    // The SPU control register file, for rows that declare `compareSpuControl`.
    int spuCompared = 0, spuFail = 0;
    size_t spuStores = 0;
    std::string firstSpu;
    std::string firstTrap;
    std::string firstCallee;
    std::string firstFrameOut;
    bool pass() const {
        return v0Fail == 0 && ramFail == 0 && spadFail == 0 && trapFail == 0 && calleeFail == 0 &&
               frameOutFail == 0 && spuFail == 0;
    }
};

int CmdPhysImpl(const Options& opt) {
    Machine base;
    rr::interp::SnapshotInfo info;
    std::string error;
    if (!rr::interp::LoadSnapshot(opt.stateDir, base.mem, base.cpu, info, error)) {
        std::printf("FAIL: %s\n", error.c_str());
        return 1;
    }
    // Every function benched here lives in the resident executable, so the overlay window is left
    // exactly as the console had it: RASHCDG resident, nothing swapped, nothing shimmed.
    base.cpu.trapOnBiosVector = true; // none of these functions may reach the kernel
    // The one concession a bench that executes a whole race frame needs, stated in
    // `Cpu::rootCountersAreConstant`: six sites in RASHCDG read root counter 2 through
    // `SLUS 0x80043F00` inside the tick, and this harness has no timer. Both sides read the same
    // constant, so the comparison is unaffected; every other MMIO access still traps.
    base.cpu.rootCountersAreConstant = true;
    base.cpu.spuWritesAreDropped = true;
    // The SPU control register file, seeded from the snapshot's own SPU block
    // when `spuctl.bin` is there and from zero otherwise; reset to that before every case.
    base.cpu.spuControlRegisterFile = !opt.noSpuFile;
    uint16_t pristineSpu[16] = {};
    if (info.hasSpuControl) std::memcpy(pristineSpu, info.spuControl, sizeof(pristineSpu));
    const uint32_t gp = base.cpu.regs[28];
    const uint32_t sp = base.cpu.regs[29] & ~7u;
    // The snapshot's whole register file, so that a clone asked to execute guest code of its own
    // (the oracle-supplied-callee seam) starts from the machine's real `gp` rather than from zero.
    const CpuRegs pristineRegs = CaptureRegs(base.cpu);

    const std::vector<uint8_t> pristineRam = base.mem.ram();
    const std::vector<uint8_t> pristineSpad = base.mem.scratchpad();
    // The snapshot's BIOS image (`bios.bin` beside it, when one is there). It is NOT part
    // of the diff - it is ROM - but the clone has to hold the same bytes as the base machine, or a
    // guest function that reads the BIOS region reads real code on one side and zeroes on the
    // other. That asymmetry was measured, not guessed: `race_tick` case 1035 has the original
    // dereference a null player-2 pointer, land on the word at guest 0x43C (`0xBFC06FDC`, a kernel
    // vector), and branch on what it finds there - the base machine read the real BIOS and took the
    // early-out, the clone read zeroes and fell through into a store into ROM.
    const std::vector<uint8_t> pristineBios = base.mem.bios();

    std::string harvestNote;
    const std::vector<Sample> samples = HarvestSamples(opt.stateRoot, opt.dumpRoot, harvestNote);

    std::printf("rrverify phys - the bike physics bench\n");
    std::printf("  state          %s (frame %u, pc 0x%08X)\n", opt.stateDir.c_str(), info.frameNumber,
                info.pc);
    std::printf("  gp             0x%08X   sp 0x%08X   stack window excluded 0x%08X..0x%08X\n", gp, sp,
                sp - 4096u, sp + 19u);
    std::printf("  dump-derived   %s\n", harvestNote.c_str());
    std::printf("  randomised     %d case(s) per row, seed 0x%016" PRIX64 "\n", opt.randomCases, opt.seed);
    std::printf("  compared       v0 + 2 MiB RAM outside the stack window + 1 KiB scratchpad\n");
    std::printf("  SPU ctl file   %s, 0x1F801D80..0x1F801D9B, seeded from %s\n\n",
                opt.noSpuFile ? "OFF (--no-spu-file)" : "on",
                info.hasSpuControl ? "the snapshot (spuctl.bin)" : "zero (no spuctl.bin)");

    if (samples.empty()) {
        std::printf("FAIL: no dump-derived samples were found under %s / %s\n", opt.stateRoot.c_str(),
                    opt.dumpRoot.c_str());
        return 1;
    }

    const std::vector<Row> rows = BuildRows();
    if (opt.list) {
        for (const Row& r : rows) std::printf("  %-14s 0x%08X  %s\n", r.name, r.entry, r.signature);
        return 0;
    }

    // Scratch buffers reused across every case, so the per-case work is a memcpy, not a malloc.
    std::vector<uint8_t> ramAfterSetup(pristineRam.size());
    int rowsRun = 0, rowsPassed = 0;
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const Row& row = rows[ri];
        base.cpu.trapOnBiosVector = !row.allowBiosCalls;
        if (!opt.only.empty() && opt.only != row.name) continue;
        // Diagnostics are opt-in: they answer a question, they are not acceptance rows. Matched by
        // exact name - a name prefix once silently dropped an acceptance row (`sound_service`).
        if (opt.only.empty() && std::strcmp(row.name, "sound_emitter_is_invisible") == 0) continue;
        ++rowsRun;
        RowResult res;

        // `--coverage`: probes on every instruction of the row's function, if its extent is known.
        const CoverageRange* cov = nullptr;
        if (opt.coverage)
            for (const CoverageRange& cr : kCoverageRanges)
                if (cr.entry == row.entry) cov = &cr;
        if (opt.coverage && cov == nullptr) cov = BikeStepCoverage(row.name);
        if (opt.coverage && cov == nullptr) cov = PopulationCoverage(row.name); // rows_population.inc
        if (opt.coverage && cov == nullptr) cov = PoliceCoverage(row.name);       // rows_police.inc
        if (opt.coverage && cov == nullptr) cov = TrafficDriveCoverage(row.name); // rows_traffic_drive.inc
        if (opt.coverage && cov == nullptr) cov = CameraCoverage(row.name);     // rows_camera.inc
        if (opt.coverage && cov == nullptr) cov = CollisionCoverage(row.name);  // rows_collision.inc
        if (opt.coverage && cov == nullptr) cov = CollCoverage(row.name);       // rows_coll.inc
        if (opt.coverage && cov == nullptr) cov = PoseCoverage(row.name);       // rows_pose.inc
        if (opt.coverage && cov == nullptr) cov = AiBrainCoverage(row.name);    // rows_ai_brain.inc
        if (opt.coverage && cov == nullptr) cov = RiderCoverage(row.name);      // rows_riders.inc
        if (opt.coverage && cov == nullptr) cov = SpeechCoverage(row.name);     // rows_speech.inc
        if (opt.coverage && cov == nullptr) cov = SolidCoverage(row.name);      // rows_solid.inc
        if (opt.coverage && cov == nullptr) cov = PassesCoverage(row.name);     // rows_passes.inc
        if (opt.coverage && cov == nullptr) cov = HazardCoverage(row.name);     // rows_hazards.inc
        if (opt.coverage && cov == nullptr) cov = PartnersCoverage(row.name);   // rows_partners.inc
        if (opt.coverage && cov == nullptr) cov = CopsCoverage(row.name);       // rows_cops.inc
        rr::interp::Tracer covTracer;
        std::vector<uint8_t> covered;
        if (cov != nullptr) {
            for (uint32_t a = cov->entry; a < cov->end; a += 4) covTracer.AddProbe(a, std::string());
            covered.assign((cov->end - cov->entry) / 4u, 0);
            base.cpu.tracer = &covTracer;
        }

        // Watch the guest run at every entry this row's port is allowed to delegate to the oracle,
        // so the two call sequences can be compared.
        base.cpu.ClearCallObservers();
        for (const OracleCallee& oc : row.oracleCallees) {
            base.cpu.ObserveCalls(oc.address);
            for (const OracleFrameOut& fo : oc.frameOuts) {
                // A ZERO-length declaration takes an argument out of the pointer comparison without
                // asking for a content capture. `race_director` needs it: `0x800BA304`'s two output
                // masks are ADJACENT words of the original's frame (`sp+20` and `sp+24`), so one
                // 8-byte capture on argument 2 already covers both, while argument 3 is still a
                // pointer that differs between the guest and the port by construction. The
                // interpreter keeps one buffer per observation, so declaring a second non-empty
                // capture on the same call would silently overwrite the first.
                if (fo.bytes == 0) continue;
                base.cpu.ObserveCallFrameOut(oc.address, fo.index, fo.bytes);
            }
        }

        const size_t derivedCases = samples.size() * static_cast<size_t>(opt.derivedCasesPerSample);
        const size_t totalCases = derivedCases + static_cast<size_t>(opt.randomCases);

        for (size_t ci = 0; ci < totalCases; ++ci) {
            const bool isDerived = ci < derivedCases;
            const Sample* sample = isDerived ? &samples[ci % samples.size()] : nullptr;
            const uint64_t caseSeed = opt.seed ^ (static_cast<uint64_t>(ri) << 40) ^
                                      (static_cast<uint64_t>(ci) * 0x100000001B3ull);

            // ---- guest
            base.mem.ram() = pristineRam;
            base.mem.scratchpad() = pristineSpad;
            std::memcpy(base.cpu.spuControl, pristineSpu, sizeof(pristineSpu));
            base.cpu.rootCounterValue = 0;
            std::vector<Cpu::SpuStore> guestSpuStores, portSpuStores;
            base.cpu.spuStoreLog = nullptr;
            const CallResult alloc = CallGuest(base.cpu, kMallocEntry, {kScratchBytes, 0, 0, 0}, sp, 5'000'000);
            if (!alloc.ok() || alloc.v0 == 0) {
                ++res.trapFail;
                if (res.firstTrap.empty()) res.firstTrap = "guest malloc failed: " + alloc.trap.ToString();
                continue;
            }
            const uint32_t scratch = alloc.v0;
            Args args;
            {
                Rng rng(caseSeed);
                CaseContext ctx{&base.mem, scratch, &rng, sample, &samples[0], gp, sp};
                ctx.spuControl = base.cpu.spuControl;
                ctx.rootCounter = &base.cpu.rootCounterValue;
                row.setup(ctx, args);
            }
            base.mem.PokeWord(sp + 16u, args.a4); // the fifth o32 argument slot
            for (uint32_t k = 0; k < 4; ++k) base.mem.PokeWord(sp + 20u + 4u * k, args.more[k]);
            bool guestWrote = false;
            // The machine as the ROW'S SETUP left it, which is what the coverage test below has to
            // measure against: comparing the finished run with the pristine snapshot would count
            // the setup's own writes and make `wrote` vacuous for every row. The buffer is reused
            // across cases, so this is one memcpy and no allocation.
            ramAfterSetup = base.mem.ram();
            std::vector<uint8_t> scratchBefore(kScratchBytes);
            base.mem.ReadBlock(scratch, scratchBefore.data(), scratchBefore.size());
            const uint32_t seedBefore = base.mem.PeekWord(gp + kRandSeedGpOffset);
            base.cpu.callObservations.clear();
            // Start the guest run from the SNAPSHOT's register file, exactly as the clone does
            // (`RestoreRegs` below). Without this the base machine carries the previous case's
            // registers into this one, so a guest function that reads a register its caller did not
            // set would see something the clone cannot reproduce - and the divergence would look
            // like a port bug. It also makes a case reproducible from its index alone.
            RestoreRegs(pristineRegs, base.cpu);
            covTracer.probeHits.clear();
            if (row.compareSpuControl) base.cpu.spuStoreLog = &guestSpuStores;
            const CallResult run = CallGuest(base.cpu, row.entry, args.a, sp, row.maxSteps);
            base.cpu.spuStoreLog = nullptr;
            if (cov != nullptr) {
                for (const rr::interp::ProbeHit& ph : covTracer.probeHits)
                    if (ph.pc >= cov->entry && ph.pc < cov->end) covered[(ph.pc - cov->entry) / 4u] = 1;
                covTracer.probeHits.clear();
            }
            const std::vector<Cpu::CallObservation> guestCalls = base.cpu.callObservations;
            {
                std::vector<uint8_t> scratchAfter(kScratchBytes);
                base.mem.ReadBlock(scratch, scratchAfter.data(), scratchAfter.size());
                if (scratchAfter != scratchBefore ||
                    base.mem.PeekWord(gp + kRandSeedGpOffset) != seedBefore)
                    guestWrote = true;
            }
            if (run.v0 != 0) ++res.v0NonZero;
            if (!run.ok()) {
                ++res.trapFail;
                if (res.firstTrap.empty())
                    res.firstTrap = "case " + std::to_string(ci) + " (guest): " + run.trap.ToString();
                continue;
            }

            // ---- native, on a clone of the very same starting memory
            Machine clone;
            clone.mem.ram() = pristineRam;
            clone.mem.scratchpad() = pristineSpad;
            clone.mem.bios() = pristineBios;
            std::memcpy(clone.cpu.cop0, base.cpu.cop0, sizeof(clone.cpu.cop0));
            clone.cpu.trapOnBiosVector = !row.allowBiosCalls;
            clone.cpu.rootCountersAreConstant = base.cpu.rootCountersAreConstant;
            clone.cpu.rootCounterValue = 0;
            clone.cpu.spuWritesAreDropped = base.cpu.spuWritesAreDropped;
            clone.cpu.spuControlRegisterFile = base.cpu.spuControlRegisterFile;
            std::memcpy(clone.cpu.spuControl, pristineSpu, sizeof(pristineSpu));
            clone.cpu.spuStoreLog = nullptr;
            const CallResult alloc2 = CallGuest(clone.cpu, kMallocEntry, {kScratchBytes, 0, 0, 0}, sp, 5'000'000);
            if (!alloc2.ok() || alloc2.v0 != scratch) {
                ++res.trapFail;
                if (res.firstTrap.empty()) res.firstTrap = "the clone's allocator diverged";
                continue;
            }
            Args args2;
            {
                Rng rng(caseSeed);
                CaseContext ctx{&clone.mem, scratch, &rng, sample, &samples[0], gp, sp};
                ctx.spuControl = clone.cpu.spuControl;
                ctx.rootCounter = &clone.cpu.rootCounterValue;
                row.setup(ctx, args2);
            }
            if (!(args2 == args)) {
                ++res.trapFail;
                if (res.firstTrap.empty()) res.firstTrap = "the row's setup is not deterministic";
                continue;
            }
            clone.mem.PokeWord(sp + 16u, args2.a4); // the same fifth-argument slot, so it diffs clean
            for (uint32_t k = 0; k < 4; ++k) clone.mem.PokeWord(sp + 20u + 4u * k, args2.more[k]);
            std::vector<uint8_t> beforeNative;
            if (opt.mutateNative) beforeNative = clone.mem.ram();
            uint32_t nativeV0;
            NativeEnv env;
            if (row.compareSpuControl) clone.cpu.spuStoreLog = &portSpuStores;
            if (row.nativeEx) {
                // Only the oracle-supplied-callee seam needs the clone to be able to run guest
                // code, so only it pays for a full register file.
                RestoreRegs(pristineRegs, clone.cpu);
                // The same call observers the guest run carries, so that the two sequences are
                // recorded by the same mechanism and a call made INSIDE an oracle-executed callee
                // appears on both sides or on neither.
                clone.cpu.ClearCallObservers();
                for (const OracleCallee& oc : row.oracleCallees) clone.cpu.ObserveCalls(oc.address);
                env.clone = &clone;
                env.scratch = scratch;
                env.gp = gp;
                env.sp = sp - row.oracleFrame;
                env.frameOutNext = env.sp - kFrameOutTop;
                env.callees = &row.oracleCallees;
                env.maxSteps = row.maxSteps;
                env.skipOracle = opt.skipOracle;
                env.spliceControl = opt.spliceControl;
                nativeV0 = row.nativeEx(env, args2);
            } else {
                nativeV0 = row.native(clone.mem, scratch, gp, args2);
            }
            res.oracleCalls += static_cast<int>(guestCalls.size());
            if (!env.failure.empty()) {
                ++res.trapFail;
                if (res.firstTrap.empty())
                    res.firstTrap = "case " + std::to_string(ci) + ": " + env.failure +
                                    " [guest made " + std::to_string(guestCalls.size()) +
                                    " oracle call(s), the port " + std::to_string(env.calls.size()) +
                                    "]";
                continue;
            }
            // "Inside the excluded stack window" - the one place where a pointer genuinely cannot be
            // compared, because the guest's is a local of the original caller's frame and the
            // port's is a buffer this bench put there. Used per call, per argument, below.
            const uint32_t windowLow = sp - 4096u, windowHigh = sp + 19u;
            auto inWindow = [windowLow, windowHigh](uint32_t a) {
                return a >= windowLow && a <= windowHigh;
            };
            auto sameCalls = [&row, &inWindow](const std::vector<Cpu::CallObservation>& g,
                                               const std::vector<Cpu::CallObservation>& n) {
                if (g.size() != n.size()) return false;
                for (size_t k = 0; k < g.size(); ++k) {
                    if (g[k].address != n[k].address) return false;
                    const OracleCallee* oc = nullptr;
                    for (const OracleCallee& cand : row.oracleCallees)
                        if (cand.address == g[k].address) oc = &cand;
                    const int total = oc ? (oc->arity + oc->stackArgs) : 4;
                    for (int j = 0; j < total && j < 12; ++j) {
                        bool declared = false;
                        if (oc != nullptr)
                            for (const OracleFrameOut& fo : oc->frameOuts)
                                if (fo.index == j) declared = true;
                        // Declared AND actually in the caller's frame on both sides. An argument
                        // that is a real address on this particular call - `0x800A7BF8`'s `a3` is
                        // `e+0x10C` at two of its three call sites - is compared as strictly as any
                        // other, and one that is a frame local on one side only fails here.
                        const bool isFrameOut =
                            declared && inWindow(g[k].a[j]) && inWindow(n[k].a[j]);
                        if (isFrameOut) {
                            // The pointer differs by construction (the guest's is a local of its
                            // own frame, the port's is the bench's buffer), so only its NULL-ness
                            // is an argument here; the bytes behind it are compared separately
                            // below.
                            if ((g[k].a[j] == 0) != (n[k].a[j] == 0)) return false;
                            continue;
                        }
                        if (g[k].a[j] != n[k].a[j]) return false;
                    }
                }
                return true;
            };
            if (!sameCalls(guestCalls, env.calls)) {
                ++res.calleeFail;
                if (res.firstCallee.empty()) {
                    char buf[256];
                    const Cpu::CallObservation* g = guestCalls.empty() ? nullptr : &guestCalls[0];
                    const Cpu::CallObservation* n = env.calls.empty() ? nullptr : &env.calls[0];
                    std::snprintf(buf, sizeof(buf),
                                  "case %zu: guest made %zu oracle-callee call(s) "
                                  "(first a=0x%08X,0x%08X,0x%08X,0x%08X), the port made %zu "
                                  "(first a=0x%08X,0x%08X,0x%08X,0x%08X)",
                                  ci, guestCalls.size(), g ? g->a[0] : 0u, g ? g->a[1] : 0u,
                                  g ? g->a[2] : 0u, g ? g->a[3] : 0u, env.calls.size(),
                                  n ? n->a[0] : 0u, n ? n->a[1] : 0u, n ? n->a[2] : 0u,
                                  n ? n->a[3] : 0u);
                    res.firstCallee = buf;
                    // The two sequences, entry by entry: with more than one declared callee the
                    // counts alone do not say WHICH call was dropped or added.
                    res.firstCallee += "\n                 guest:";
                    for (const Cpu::CallObservation& o : guestCalls) {
                        char b2[20];
                        std::snprintf(b2, sizeof(b2), " %08X", o.address);
                        res.firstCallee += b2;
                    }
                    res.firstCallee += "\n                 port: ";
                    for (const Cpu::CallObservation& o : env.calls) {
                        char b2[20];
                        std::snprintf(b2, sizeof(b2), " %08X", o.address);
                        res.firstCallee += b2;
                    }
                }
            }
            if (opt.mutateNative) {
                // Flip one bit of the row's own answer: v0, and the first byte the native side
                // actually wrote. Nothing else is touched, so a row that still passes would mean
                // the bench is not looking at that row's output at all.
                nativeV0 ^= 1u;
                std::vector<uint8_t>& after = clone.mem.ram();
                bool flipped = false;
                for (size_t i = 0; i < after.size(); ++i) {
                    if (after[i] != beforeNative[i]) { after[i] ^= 1u; flipped = true; break; }
                }
                // A port that wrote nothing (the original is `jr ra`, or this case takes an exit
                // with no store) has no byte of its own to flip. Plant a stray store at the first
                // argument's byte instead: a port that writes where the original does not must
                // fail too, and whole-RAM comparison is what proves that.
                if (!flipped) {
                    const uint32_t a = args.a[0] & 0x1FFFFFu;
                    const uint32_t lo = (sp - 4096u) & 0x1FFFFFu, hi = (sp + 20u) & 0x1FFFFFu;
                    const uint32_t at = (a >= lo && a < hi) ? 0x10000u : a;
                    after[at] ^= 1u;
                }
            }

            if (opt.mutateFrameOut) {
                bool flipped = false;
                for (Cpu::CallObservation& o : env.calls) {
                    for (Cpu::FrameOutCapture& f : o.frameOuts) {
                        if (f.arg < 0 || f.len == 0) continue;
                        f.bytes[0] ^= 1u;
                        flipped = true;
                        break;
                    }
                    if (flipped) break;
                }
            }

            // ---- the caller-frame output buffers
            //
            // Both pointers live in the excluded stack window - the guest's because it is a local
            // of the original caller's frame, the port's because the bench put it there - so the
            // whole-RAM diff below says nothing about either. The BYTES are therefore compared
            // here, explicitly, per call, out of the snapshot each side took at the instant its
            // callee returned. Nothing about these buffers is taken on trust except their length,
            // which is read out of the callee's own `memcpy`.
            if (guestCalls.size() == env.calls.size()) {
                for (size_t k = 0; k < guestCalls.size(); ++k) {
                    const Cpu::CallObservation& g = guestCalls[k];
                    const Cpu::CallObservation& n = env.calls[k];
                    // Paired by ARGUMENT INDEX, not by slot order: a callee with two caller-frame
                    // buffers may have them captured in either order on the two sides.
                    for (int j = 0; j < 12; ++j) {
                        const Cpu::FrameOutCapture* gf = g.FrameOutFor(j);
                        const Cpu::FrameOutCapture* nf = n.FrameOutFor(j);
                        if (gf == nullptr && nf == nullptr) continue;
                        ++res.frameOutCompared;
                        const uint32_t gl = gf ? gf->len : 0, nl = nf ? nf->len : 0;
                        const bool same =
                            gl == nl && (gl == 0 || std::memcmp(gf->bytes, nf->bytes, gl) == 0);
                        if (same) continue;
                        ++res.frameOutFail;
                        if (res.firstFrameOut.empty()) {
                            uint32_t b = 0;
                            while (b < gl && b < nl && gf->bytes[b] == nf->bytes[b]) ++b;
                            char buf[260];
                            std::snprintf(
                                buf, sizeof(buf),
                                "case %zu: the caller-frame buffer of 0x%08X argument %d (call %zu, "
                                "%u vs %u byte(s)) differs at byte %u (guest 0x%02X, port 0x%02X)",
                                ci, g.address, j, k, gl, nl, b, (gf && b < gl) ? gf->bytes[b] : 0,
                                (nf && b < nl) ? nf->bytes[b] : 0);
                            res.firstFrameOut = buf;
                        }
                    }
                }
            }

            // ---- compare
            if (row.compareV0 && nativeV0 != run.v0) {
                ++res.v0Fail;
                if (opt.verbose && res.v0Fail <= 8) {
                    std::printf("    %-12s case %zu (%s) v0 guest=0x%08X native=0x%08X  a=(0x%08X,0x%08X,0x%08X,0x%08X)\n",
                                row.name, ci, isDerived ? "derived" : "random", run.v0, nativeV0,
                                args.a[0], args.a[1], args.a[2], args.a[3]);
                }
            }
            const std::vector<uint8_t>& A = base.mem.ram();
            const std::vector<uint8_t>& B = clone.mem.ram();
            // The excluded window is the 4 KiB the callee may use below `sp`, PLUS the 20 bytes
            // above it. Those 20 bytes are not ours: o32 gives a callee the right to spill its
            // incoming `a0..a3` into the caller's frame at `sp+0..sp+15` (RASHCDG 0x800745B0 does
            // exactly that with `sw a2,64(sp)`), and `sp+16` is where this bench puts a fifth
            // argument. Nothing in the game uses that area as an output.
            const uint32_t stackLow = sp - 4096u;
            const uint32_t stackHigh = sp + 19u;
            size_t ramDiffs = 0;
            uint32_t firstDiff = 0;
            for (size_t i = 0; i < A.size(); ++i) {
                const uint32_t addr = 0x80000000u + static_cast<uint32_t>(i);
                if (addr >= stackLow && addr <= stackHigh) continue;
                if (A[i] != B[i]) {
                    if (ramDiffs == 0) firstDiff = addr;
                    ++ramDiffs;
                }
                // Coverage: did the GUEST change anything at all this case? The scratch block and
                // the LCG seed answer that for a row whose function works out of the scratch block,
                // but the race-spine rows work on the machine's own pool and globals instead, and
                // for them the scratch test is vacuous. Folded into the diff loop so it costs one
                // extra compare rather than another pass over 2 MiB.
                if (!guestWrote && A[i] != ramAfterSetup[i]) guestWrote = true;
            }
            if (ramDiffs != 0) {
                ++res.ramFail;
                if (res.firstRamDiff == 0) res.firstRamDiff = firstDiff;
                if (opt.verbose && res.ramFail <= 8) {
                    // The two words at the first difference, and what the row's setup left there,
                    // because "3 bytes differ" says nothing about WHICH side is wrong.
                    auto word = [](const std::vector<uint8_t>& v, uint32_t a) {
                        const size_t o = a & 0x1FFFFFu;
                        if (o + 4 > v.size()) return 0u;
                        return static_cast<uint32_t>(v[o]) | (static_cast<uint32_t>(v[o + 1]) << 8) |
                               (static_cast<uint32_t>(v[o + 2]) << 16) |
                               (static_cast<uint32_t>(v[o + 3]) << 24);
                    };
                    const uint32_t aligned = firstDiff & ~3u;
                    std::printf("    %-12s case %zu (%s) RAM differs in %zu byte(s), first 0x%08X "
                                "(guest 0x%08X, port 0x%08X, before 0x%08X)\n",
                                row.name, ci, isDerived ? "derived" : "random", ramDiffs, firstDiff,
                                word(A, aligned), word(B, aligned), word(ramAfterSetup, aligned));
                }
            }
            if (base.mem.scratchpad() != clone.mem.scratchpad()) ++res.spadFail;
            // ---- the SPU control register file
            clone.cpu.spuStoreLog = nullptr;
            if (opt.mutateSpuFile) {
                clone.cpu.spuControl[ci % 14u] ^= 1u;
                if (!portSpuStores.empty()) portSpuStores.back().value ^= 1u;
            }
            if (row.compareSpuControl) {
                ++res.spuCompared;
                res.spuStores += guestSpuStores.size();
                if (!(guestSpuStores == portSpuStores)) {
                    ++res.spuFail;
                    if (res.firstSpu.empty()) {
                        size_t k = 0;
                        while (k < guestSpuStores.size() && k < portSpuStores.size() &&
                               guestSpuStores[k] == portSpuStores[k])
                            ++k;
                        char buf[200];
                        std::snprintf(buf, sizeof(buf),
                                      "case %zu: SPU store #%zu differs (guest %zu store(s), port %zu; "
                                      "guest 0x%08X=0x%X, port 0x%08X=0x%X)",
                                      ci, k, guestSpuStores.size(), portSpuStores.size(),
                                      k < guestSpuStores.size() ? guestSpuStores[k].address : 0u,
                                      k < guestSpuStores.size() ? guestSpuStores[k].value : 0u,
                                      k < portSpuStores.size() ? portSpuStores[k].address : 0u,
                                      k < portSpuStores.size() ? portSpuStores[k].value : 0u);
                        res.firstSpu = buf;
                    }
                }
                for (uint32_t k = 0; k < 16; ++k) {
                    if (base.cpu.spuControl[k] == clone.cpu.spuControl[k]) continue;
                    ++res.spuFail;
                    if (res.firstSpu.empty()) {
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "case %zu: SPU control register 0x%08X differs (guest 0x%04X, "
                                      "port 0x%04X)",
                                      ci, Cpu::kSpuControlFirst + 2u * k, base.cpu.spuControl[k],
                                      clone.cpu.spuControl[k]);
                        res.firstSpu = buf;
                    }
                    break;
                }
            }
            if (guestWrote) ++res.effective;

            if (isDerived) ++res.derived; else ++res.random;
        }

        const bool pass = res.pass();
        if (pass) ++rowsPassed;
        std::printf("%-14s 0x%08X  derived %-5d random %-4d | wrote %-5d v0!=0 %-5d | v0 %-3d ram %-3d "
                    "spad %-3d trap %-3d  %s\n",
                    row.name, row.entry, res.derived, res.random, res.effective, res.v0NonZero,
                    res.v0Fail, res.ramFail, res.spadFail, res.trapFail, pass ? "PASS" : "FAIL");
        if (!row.oracleCallees.empty())
            std::printf("               oracle-supplied callees: %d guest call(s), %d case(s) where "
                        "the port's call sequence differed\n",
                        res.oracleCalls, res.calleeFail);
        if (res.frameOutCompared != 0 || res.frameOutFail != 0)
            std::printf("               caller-frame output buffers: %d compared byte for byte, "
                        "%d differed\n",
                        res.frameOutCompared, res.frameOutFail);
        if (!res.firstFrameOut.empty()) std::printf("               %s\n", res.firstFrameOut.c_str());
        if (row.compareSpuControl)
            std::printf("               SPU: register file + %zu register store(s) compared over %d "
                        "case(s), %d mismatch(es)\n",
                        res.spuStores, res.spuCompared, res.spuFail);
        if (!res.firstSpu.empty()) std::printf("               %s\n", res.firstSpu.c_str());
        if (!res.firstCallee.empty()) std::printf("               %s\n", res.firstCallee.c_str());
        if (!res.firstTrap.empty()) std::printf("               first trap: %s\n", res.firstTrap.c_str());
        if (cov != nullptr) {
            base.cpu.tracer = nullptr;
            size_t hit = 0;
            for (uint8_t b : covered) hit += b;
            std::printf("               coverage: %zu of %zu instructions of 0x%08X..0x%08X executed\n", hit,
                        covered.size(), cov->entry, cov->end - 4u);
            std::string gaps;
            for (size_t i = 0; i < covered.size();) {
                if (covered[i]) { ++i; continue; }
                size_t j = i;
                while (j < covered.size() && !covered[j]) ++j;
                char b[48];
                std::snprintf(b, sizeof(b), " %08X..%08X", cov->entry + 4u * static_cast<uint32_t>(i),
                              cov->entry + 4u * static_cast<uint32_t>(j - 1));
                gaps += b;
                i = j;
            }
            if (!gaps.empty()) std::printf("               never executed:%s\n", gaps.c_str());
        }
        if (res.firstRamDiff != 0) std::printf("               first RAM difference at 0x%08X\n", res.firstRamDiff);
    }

    std::printf("\n--- phys summary ---\n");
    std::printf("rows run            %d\n", rowsRun);
    std::printf("rows PASS           %d\n", rowsPassed);
    std::printf("rows FAIL           %d\n", rowsRun - rowsPassed);
    std::printf("verdict             %s\n", (rowsRun > 0 && rowsPassed == rowsRun) ? "PASS" : "FAIL");
    return (rowsRun > 0 && rowsPassed == rowsRun) ? 0 : 1;
}

// ---------------------------------------------------------------- rows from separate passes
#include "rows_bike_step.inc"
#include "rows_road_runtime.inc"
#include "rows_engine_note.inc"
#include "rows_ground.inc"
#include "rows_anim.inc"
#include "rows_crash.inc"
#include "rows_population.inc"
#include "rows_camera.inc"
#include "rows_spine.inc"
#include "rows_collision.inc"
#include "rows_hud.inc"
#include "rows_traffic.inc"
#include "rows_coll.inc"
#include "rows_pose.inc"
#include "rows_ai_brain.inc"
#include "rows_ai.inc"
#include "rows_ai_plan.inc"
#include "rows_fight.inc"
#include "rows_shell.inc"
#include "rows_shell_career.inc"
#include "rows_shell_screens.inc"
#include "rows_recover.inc"
#include "rows_riders.inc"
#include "rows_feel.inc"
#include "rows_weapon.inc"
#include "rows_speech.inc"
#include "rows_cops.inc"
#include "rows_fx.inc"
#include "rows_modes.inc"
#include "rows_mp.inc"
#include "rows_model.inc"
#include "rows_vis.inc"
#include "rows_world.inc"
#include "rows_jail.inc"
#include "rows_polish.inc"
#include "rows_solid.inc"
#include "rows_takedown.inc"
#include "rows_partners.inc"
#include "rows_grid.inc"
#include "rows_hazards.inc"
#include "rows_peds.inc"
#include "rows_passes.inc"
#include "rows_strike.inc"
#include "rows_fxdraw.inc"
#include "rows_junction.inc"
#include "rows_stream.inc"
#include "rows_subdiv.inc"
#include "rows_look.inc"
#include "rows_animobj.inc"
#include "rows_mp2.inc"
#include "rows_menu.inc"
#include "rows_stream2.inc"
#include "rows_loader.inc"
#include "rows_pause.inc"
#include "rows_loader2_spu.inc"
#include "rows_loader2_cam.inc"
#include "rows_loader2.inc"
#include "rows_menu2.inc"
#include "rows_route.inc"
#include "rows_cells.inc"
#include "rows_stream3.inc"
#include "rows_sky2.inc"
#include "rows_sky3.inc"
#include "rows_rumble.inc"
#include "rows_anim_sound.inc"

} // namespace

int CmdPhys(int argc, char** argv) {
    Options opt;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--state" && i + 1 < argc) opt.stateDir = argv[++i];
        else if (a == "--state-root" && i + 1 < argc) opt.stateRoot = argv[++i];
        else if (a == "--dumps" && i + 1 < argc) opt.dumpRoot = argv[++i];
        else if (a == "--cases" && i + 1 < argc) opt.randomCases = std::atoi(argv[++i]);
        else if (a == "--derived-per-sample" && i + 1 < argc) opt.derivedCasesPerSample = std::atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) opt.seed = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--only" && i + 1 < argc) opt.only = argv[++i];
        else if (a == "--list") opt.list = true;
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--mutate") opt.mutateNative = true;
        else if (a == "--skip-oracle") opt.skipOracle = true;
        else if (a == "--splice-control") opt.spliceControl = true;
        else if (a == "--mutate-frame-out") opt.mutateFrameOut = true;
        else if (a == "--no-spu-file") opt.noSpuFile = true;
        else if (a == "--mutate-spu-file") opt.mutateSpuFile = true;
        else if (a == "--coverage") opt.coverage = true;
        else {
            std::printf("rrverify phys [--state <dir>] [--state-root <dir>] [--dumps <dir>]\n"
                        "              [--cases N] [--derived-per-sample N] [--seed N]\n"
                        "              [--only <row>] [--list] [--verbose] [--mutate]\n"
                        "              [--skip-oracle] [--mutate-frame-out] [--coverage] [--splice-control]\n"
                        "              [--no-spu-file] [--mutate-spu-file]\n");
            return 2;
        }
    }
    return CmdPhysImpl(opt);
}
