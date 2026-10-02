#include "game/race_session.h"
#include "game/anim_detail.h" // maximum detail's full animation
#include "game/cheats.h" // the cheat menu
#include "game/audio/root_counter.h" // the console's root counter 2 at a race start
#include "game/rider_model.h"
#include "game/cell_view.h"
#include "game/stream_files_product.h" // the stream's file set-up
#include "game/mp_arena.h" // the two-player stat array by BuildGrid's heap order
#include "game/sim/view_pass.h"

#include "game/shell/handover.h" // a race the front end started (header-only)
#include "game/sim/anim.h"
#include "game/sim/pose.h"      // the ported pose side
#include "game/sim/present.h"   // the ported presentation pass
#include "game/rider_pose.h"
#include "game/sim/bike_step.h"
#include "game/sim/camera.h"
#include "game/sim/camera_collide.h" // CameraCollide in RaceStep's camera slot
#include "game/sim/camera_director.h" // the intro director's ShotSetup / SplineSlopes
#include "rrformats/level_bundle.h"  // the shot table from GAMEBIN1.DAT
#include "game/bike_pose_product.h"  // the bike's part slots
#include "game/sim/collision.h"
#include "game/coll_product.h" // the contact/impact ports served under the pass
#include "game/sim/partners.h"   // the collision partners
#include "game/junction_product.h" // SLUS 0x8003E338 in the pool loop
#include "game/sim/bike_react.h" // the rider pass's first pool loop
#include "game/sim/hit_speed.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/ground.h"
#include "game/sim/hud.h"
#include "game/sim/integrator.h"
#include "game/sim/population.h"
#include "game/sim/traffic.h"
#include "game/sim/traffic_leaves.h"
#include "game/sim/traffic_bind.h"
#include "game/sim/traffic_drive.h"
#include "game/sim/police.h"
#include "game/traffic_arena.h"
#include "game/world_pop_product.h" // the cell walker, props, collision volumes
#include "game/cell_sort_product.h" // the frame's cell sort
#include "game/sim/world_pop.h"     // VolumeLists and the pool controls used at set-up
#include "game/solid_product.h" // solid roadside objects: poles, props
#include "game/animobj_product.h" // the original's animation objects
#include "game/peds_product.h" // the pedestrians of pool 2
#include "game/loader_product.h" // the set-up path PORTED
#include "game/loader2_cam_product.h" // loader2_cam: the camera set-up, the collision set-up PORTED
#include "game/route_product.h" // GrfLoad + RoadLoad + StreamSetUp / StreamStart PORTED
#include "game/sim/peds.h"
#include "game/takedown_product.h" // the takedowns
#include "game/strike_product.h" // op 9's strike, RaceOverSignal
#include "game/pause_product.h"  // the pause test, the stall switch, the pause menu
#include "game/passes_product.h" // the top-level passes whole
#include "game/sim/passes.h"
#include "game/sim/hazard.h" // HazardPass 0x800A13C4, the rider / engine pass's last child
#include "game/hazard_product.h" // the hazard objects: set-up, spawner, draw
#include "game/sim/cell_draw.h" // EntityCell 0x8008B99C at set-up (SLUS 0x800119C0's live arm)
#include "game/sim/road_query.h"
#include "game/sim/road_runtime.h"
#include "game/sim/spine.h"
#include "game/sim/stance.h"
#include "game/sim/vec.h"
#include "game/sim/ai_globals.h"
#include "game/ai_race.h"
#include "game/grid_loader.h"
#include "game/rumble_product.h" // the pad rumble
#include "game/race_modes.h" // the game modes: the loader's per-race-type setup (rules.md 9)
#include "game/jail_session.h" // the Jailbreak mode and the two-seat bike (rules.md 16)
#include "game/sim/recover_walk.h" // AxisRotation SLUS 0x8003FB34 (the population callees)
#include "game/weapon_session.h"
#include "game/model_runtime.h"
#include "game/sim/stream.h"      // the streamer (StreamPopCall)
#include "game/frame_ot.h"        // the frame's ordering tables (SLUS 0x8001C1AC)
#include "game/sky_product.h"     // the heap ring of the PORTED heap manager
#include "game/stream_product.h" // the original's streamer (stream_session.cpp)
#include "rrformats/camera_ca.h"
#include "rrformats/chunk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>

namespace rr::game {
namespace {

using rr::sim::EntityView;
using rr::sim::GuestRam;
namespace ent = rr::sim::ent;

// The two images the ported code's tables live in, and how a guest address becomes a file offset.
constexpr uint32_t kExeLoad = 0x80010000u;
constexpr size_t kExeHeader = 0x800;           // the PS-X EXE header
constexpr uint32_t kOverlayLoad = 0x8005B5E8u; // file offset 0 maps here

// SLUS_010.53 - static game data.
constexpr uint32_t kSinCosTable = 0x8005624C;
constexpr uint32_t kAtanTable = 0x8005285C;
constexpr uint32_t kAtanU16Table = 0x800527E0;
constexpr uint32_t kSqrtTable = 0x800560CC;
constexpr uint32_t kCrashTable = 0x800537D8;
constexpr uint32_t kAltKindTable = 0x800541D4;
constexpr uint32_t kPlanTable = 0x80052FAC;
constexpr uint32_t kTabSpeedClass = 0x80052FA0;
constexpr uint32_t kTabThinkA = 0x80052FD0;
constexpr uint32_t kTabThinkB = 0x80052FF4;
constexpr uint32_t kTabCopFlat = 0x80053030;
constexpr uint32_t kTabAltA = 0x8005303C;
constexpr uint32_t kTabAltB = 0x80053054;
constexpr uint32_t kTabSpeedCap = 0x80053138;
constexpr uint32_t kTabCopBase = 0x80053144;
constexpr uint32_t kTabCopStep = 0x80053150;
constexpr uint32_t kTabCopMul = 0x8005315C;
constexpr uint32_t kProfile = 0x800531AC;
constexpr uint32_t kMilestones = 0x80053174;
constexpr uint32_t kWipeoutRateByte = 0x800531F1; // read by 0x8007246C

// RASHCDG.BIN - the race overlay.
constexpr uint32_t kArmTable = 0x8005B9B8;
constexpr uint32_t kArmNone = 0x800BA79C;
constexpr uint32_t kArmRace = 0x800BA6E0;

// ---- THE ARENA LAYOUT. The addresses `rr-race` has, wherever it has one (read out of that
// capture): the pools, the stat array, the view objects, the
// descriptor, the bank slots (0x800CF5D8 is also the constant the descriptor initialiser RASHCDI
// 0x8005D130 writes), the anim objects and programs, the race graph and the route block.
constexpr size_t kEntitySize = 1096;
constexpr uint32_t kRiderBytes = 628;             // pool 1, BuildGrid 0x80067BD4
constexpr uint32_t kRiderDefBytes = 72;
constexpr uint32_t kArenaPool0 = 0x801B65D4;      // pool 0, stride 1096
constexpr uint32_t kArenaPool1 = 0x801BB2EC;      // pool 1, stride 628
constexpr uint32_t kArenaRiderDefs = 0x800D5758;  // the 20 rider records, 72 bytes by AI index (grid_loader.h)
constexpr uint32_t kRiderBiAt = 0x801FE7C8;       // ours: LEVEL<n>.BI while the records load (BuildGrid's sp+256
                                                  // on the world pass's stack, which is not in use yet)
constexpr uint32_t kArenaGameState = 0x800D5D38;  // game_state, *(0x8005B2F8)
constexpr uint32_t kGameStateBytes = 0x60;        // up to the finishing-order table
constexpr uint32_t kFinishOrder = 0x800D5D98;     // 16-byte records indexed by place
constexpr uint32_t kFinishOrderBytes = 16u * 20u;
constexpr uint32_t kJailbreakClock = 0x800D9C4C;
constexpr uint32_t kAttackerMask = 0x800CCAC0;    // one u16 per player
constexpr uint32_t kPoolTable = 0x800CE4D0;       // population.md 1, 7 x 16 bytes
constexpr uint32_t kPool0Live = 0x8005B1F8, kPool0High = 0x8005AD38; // rr-race's own pointers
constexpr uint32_t kPool1Live = 0x8005B218, kPool1High = 0x8005AD3C;
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPlayerBikes = 0x8005B268;     // player p's bike at +4p
constexpr uint32_t kPlayer2Bike = 0x8005B21C;
constexpr uint32_t kP1EntityPtr = 0x8005B38C;
constexpr uint32_t kFightTablePtr = 0x8005AD4C;   // -> FIGHT.BIN (not loaded: 0)
constexpr uint32_t kHandleTable = 0x800CE540;     // 8-byte records indexed by the handle
constexpr uint32_t kPadRecords = 0x800D7128;      // 192-byte records
constexpr uint32_t kViewArray = 0x800CD898;       // View[2], stride 1132
constexpr uint32_t kViewStride = 1132;
constexpr uint32_t kStatBlockBytes = 448;
constexpr uint32_t kStatArrayPtr = 0x8005B248;
constexpr uint32_t kArenaStatArray = 0x801B5ECC;
constexpr uint32_t kCrashWindow = 0x800D3974;     // ENV.EN +0x94, read by 0x80074EA0
constexpr uint32_t kArenaGp = 0x8005AC8C;         // SLUS_010.53's gp: 0x8005B580 - 2292
constexpr uint32_t kArenaMapAt = 0x801A8A4C;      // where rr-race holds ROAD1.MAP
constexpr uint32_t kArenaObjectsFrom = 0x800E0000; // ours: the road objects, packed
constexpr uint32_t kArenaObjectsTo = 0x801A7000;
constexpr uint32_t kArenaGraph = 0x801A7BCC;      // rr-race's STREAM1.GRF
constexpr uint32_t kArenaRouteBlock = 0x801A87B4; // rr-race's route block, when it fits below the map
constexpr uint32_t kArenaRouteFallback = 0x801A7000; // ours, when it does not
constexpr uint32_t kRouteHeader = 0x800D6170;     // road.md 1.3, 40 bytes
constexpr uint32_t kArenaAnimFiles = 0x801BE13C;  // rr-race's first bank file
constexpr uint32_t kArenaAnimFilesTo = 0x801EE000;
constexpr uint32_t kArenaAnimObjects = 0x801EE634;
constexpr uint32_t kArenaAnimPrograms = 0x801FA39C;
constexpr uint32_t kAnimSlots = 0x800CF5D8;
constexpr uint32_t kAnimSlotCapacity = 10;
constexpr uint32_t kArenaStepSp = 0x801FF000;     // ours: the stack the world pass runs on
constexpr uint32_t kArenaAiDriveSp = 0x801FE000;  // ours: AiDrive's sp for the look-ahead
constexpr uint32_t kRecoverSeamSp = 0x801FE400;   // ours: the rider-recovery calls made from a seam (recover_race.h)
constexpr uint32_t kCellChunkBytes = 0x4000;
constexpr uint32_t kScratchpad = 0x1F800000;
constexpr uint32_t kListDormant = 0x8005B270;
constexpr uint32_t kListRiding = 0x8005B298;
constexpr uint32_t kListThrown = 0x8005B2D8;
constexpr uint32_t kListDown = 0x8005B350;
constexpr uint32_t kListSpin = 0x8005B378;

// The camera arena.
constexpr uint32_t kCameraFileAt = 0x800CD7B8;    // CAMERA.CA's first 224 bytes (RASHCDI 0x80069618)
constexpr uint16_t kView0Handle = 0x9F;           // `li a2,159` at RASHCDI 0x800676F8
constexpr uint32_t kRaceStepFrame = 32;           // `addiu sp,sp,-32` at SLUS 0x8001252C
constexpr uint32_t kCameraInitFrame = 40;         // `addiu sp,sp,-40` at SLUS 0x8002F308
// The window-test data and the spine's fixed words.
constexpr uint32_t kPieceList = 0x800D4B10;       // 6 x 16 bytes, up to the pool-2 block 0x800D4B70
constexpr uint32_t kPieceLast = 0x8005B31C;       // s32, the list's last index
constexpr int kPieceSlots = 6;
constexpr uint32_t kVolumeRadius = 0x500000;      // gp+204 = 80.0 in all 18 race captures
constexpr uint32_t kTrafficSwitch = 0x8005ACC4;   // 1 in every capture; 0 here (traffic off)
constexpr uint32_t kRiderPassFrame = 248;         // `addiu sp,sp,-248` at RASHCDG 0x8007B840

// Frames the tail callees are called at (their prologues).
constexpr uint32_t kGroundFrameSz = 120;  // `addiu sp,sp,-120` at 0x8007504C
constexpr uint32_t kGroundSlotA = 24, kGroundSlotB = 40, kGroundSlotC = 72;
constexpr uint32_t kContactFrameSz = 136; // `addiu sp,sp,-136` at 0x8007FA4C

int32_t ReadS32(const uint8_t* v, size_t at) {
    return static_cast<int32_t>(static_cast<uint32_t>(v[at]) | (static_cast<uint32_t>(v[at + 1]) << 8) |
                                (static_cast<uint32_t>(v[at + 2]) << 16) |
                                (static_cast<uint32_t>(v[at + 3]) << 24));
}
int32_t ReadS32(const std::vector<uint8_t>& v, size_t at) {
    if (at + 4 > v.size()) return 0;
    return ReadS32(v.data(), at);
}
int32_t ReadS32(const ArenaBytes& v, size_t at) { return ReadS32(v.p, at); }
int16_t ReadS16(const uint8_t* v, size_t at) {
    return static_cast<int16_t>(static_cast<uint16_t>(v[at] | (v[at + 1] << 8)));
}

void WriteS32(uint8_t* v, size_t at, int32_t value) {
    const uint32_t u = static_cast<uint32_t>(value);
    v[at] = static_cast<uint8_t>(u);
    v[at + 1] = static_cast<uint8_t>(u >> 8);
    v[at + 2] = static_cast<uint8_t>(u >> 16);
    v[at + 3] = static_cast<uint8_t>(u >> 24);
}
void WriteS32(std::vector<uint8_t>& v, size_t at, int32_t value) { WriteS32(v.data(), at, value); }
void WriteS16(uint8_t* v, size_t at, int16_t value) {
    const uint16_t u = static_cast<uint16_t>(value);
    v[at] = static_cast<uint8_t>(u);
    v[at + 1] = static_cast<uint8_t>(u >> 8);
}

template <typename T>
std::vector<T> Reinterpret(const std::vector<uint8_t>& bytes) {
    std::vector<T> out(bytes.size() / sizeof(T));
    if (!out.empty()) std::memcpy(out.data(), bytes.data(), out.size() * sizeof(T));
    return out;
}

// A raw host view of guest RAM (KSEG0/KUSEG main RAM only), or null - the product's form of the
// bench's `RawAt` (rows_bike_step.inc): the ported callees of the step run on raw views of the one
// arena, so a record two chases reach is one record, as on the console.
uint8_t* RawAt(uint8_t* ram, uint32_t a, uint32_t bytes) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + bytes > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

bool ReadRam(const std::string& path, std::vector<uint8_t>& ram, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open " + path;
        return false;
    }
    ram.assign(GuestRam::kRamSize, 0);
    const size_t got = std::fread(ram.data(), 1, ram.size(), f);
    std::fclose(f);
    if (got != ram.size()) {
        error = path + " is not a 2 MiB RAM image";
        return false;
    }
    return true;
}

// CameraInit SLUS 0x8002F308(target, view, handle, mode, director, 1), transcribed from our
// disassembly of SLUS 0x8002F308..0x8002F4D4, followed by the store its caller
// makes in the jal's shadow at RASHCDI 0x80067720 (`sw zero,44(s1)`: +0x2C = 0). It is 116
// instructions of stores and three calls - memset 0x8001E100, memcpy 0x8001E0B4 and the PORTED
// RouteBind SLUS 0x8003AF9C - and it has no bench row of its own: what vouches for it is the byte
// comparison `CheckCameraArena` makes against the 18 race captures.
// `skipIntro` is the session's one deviation: the arm that switches the intro
// director on for a rider whose runtime record has byte 0 bit 5 (every grid rider during the countdown)
// or when game_state +0x04 bit 0 is set is not taken, because the director's ShotSetup RASHCDG
// 0x800853E4 is not ported; the chase camera starts at once.
void InitViewRecord(GuestRam& g, uint32_t v, uint32_t target, uint16_t handle, uint32_t mode, uint32_t director,
                    uint32_t sp, rr::sim::RoadRuntimeCallees& road, bool skipIntro = false) {
    g.W32(v + 0x238, target);                                   // 0x8002F33C
    for (uint32_t k = 0; k < 172; ++k) g.W8(v + k, 0);          // memset(v, 0, 172)
    g.W32(v + 0x130, 0x18000u);
    g.W32(v + 0x134, 8192u);
    g.W16(v + 0x140, 1);
    g.W32(v + 0x224, 0x100u);
    g.W16(v + 0xAC, handle);
    g.W32(v + 0x220, mode);
    g.W32(v + 0x21C, mode);
    g.W32(v + 0xB4, 0);
    g.W32(v + 0x138, 0x1CCCCu);
    g.W32(v + 0x13C, 0);
    g.W32(v + 0x228, 0);
    g.W32(v + 0x294, 0);
    g.W32(v + 0x248, 0);
    g.W32(v + 0x26C, 0);
    g.W32(v + 0x268, 0);
    g.W32(v + 0x45C, 0x10000u);                                 // the beqz's delay slot, 0x8002F3AC
    if (director != 0) {                                        // 0x8002F3B0
        g.W32(v + 0x304, 1);
        g.W32(v + 0x228, g.U32(v + 0x228) | 0x40u);
    } else {                                                    // 0x8002F3C8
        uint32_t on = (g.U8(g.U32(target + 0x43C)) & 0x20u) ? 1u : 0u;
        if (on == 0) on = (g.U8(g.U32(kGameStatePtr) + 4u) & 1u) ? 1u : 0u;
        if (skipIntro) on = 0;
        g.W32(v + 0x304, on);
        if (on != 0) {
            g.W32(v + 0x21C, 7);
            g.W32(v + 0x224, g.U32(v + 0x224) & 0xFFFFFEFFu);
        } else {
            g.W32(v + 0x224, g.U32(v + 0x224) | 6u);
        }
    }
    auto copy = [&](uint32_t off, uint32_t n) {                 // memcpy(v + off, target + off, n)
        for (uint32_t k = 0; k < n; ++k) g.W8(v + off + k, g.U8(target + off + k));
    };
    copy(0x148, 32);                                            // the road cursor
    copy(0x168, 12);                                            // the road key
    rr::sim::RouteBind(g, v + 0xAC, 0, target, sp - kCameraInitFrame, road);   // 0x8002F460
    g.W32(v + 0x1EC, g.U32(target + 0x1EC));
    g.W32(v + 0x1F0, g.U32(target + 0x1F0));
    copy(0x174, 56);
    g.W32(v + 0xB8, g.U32(target + 0x1F8));                     // the eye: the target's contact point
    g.W32(v + 0xBC, g.U32(target + 0x1FC));
    g.W32(v + 0x2D4, 0);
    g.W32(v + 0x2D0, 0);
    g.W32(v + 0x2E0, 0);
    g.W32(v + 0x2DC, 0);
    g.W32(v + 0xC0, g.U32(target + 0x200));
    g.W32(v + 0x2C, 0);                                         // RASHCDI 0x80067720
}

// The resident road-piece list 0x800D4B10 as the streamer leaves it: six 16-byte slots, a resident
// object {id, 0, object, 0} (its BTT_ record's +0x0C), a free one {-1, 0, 0, 0}, and the last index
// at 0x8005B31C. Measured in all 18 race captures (0x800D4B70 is the pool-2 control block, so there
// are exactly six slots).
struct PieceSlot {
    int32_t id = -1;
    uint32_t object = 0;
};
void WritePieceList(GuestRam& g, const PieceSlot (&slots)[kPieceSlots], int32_t last) {
    for (int k = 0; k < kPieceSlots; ++k) {
        const uint32_t a = kPieceList + 16u * static_cast<uint32_t>(k);
        g.W32(a + 0u, static_cast<uint32_t>(slots[k].id));
        g.W32(a + 4u, 0);
        g.W32(a + 8u, slots[k].id == -1 ? 0u : slots[k].object);
        g.W32(a + 12u, 0);
    }
    g.W32(kPieceLast, static_cast<uint32_t>(last));
}

// Names a race image the way the route check does (game_state +0x30 the set, +0x40 the race id, the
// route header's [RMAGIC] must agree), or explains why it is not one.
bool ImageNamesRace(const DiscImage& disc, GuestRam& snap, int& set, int32_t& raceId, std::string& why);

} // namespace

// ------------------------------------------------------------------------------------ GameTables

void GameTables::Load(const DiscImage& disc) {
    const auto exeFile = disc.Find("SLUS_010.53");
    const auto overlayFile = disc.Find("RASHCDG.BIN");
    if (!exeFile || !overlayFile)
        throw std::runtime_error("this disc carries no SLUS_010.53 / RASHCDG.BIN");
    exe_ = disc.ReadFile(*exeFile);
    overlay_ = disc.ReadFile(*overlayFile);
    if (exe_.size() < kExeHeader || std::memcmp(exe_.data(), "PS-X EXE", 8) != 0)
        throw std::runtime_error("SLUS_010.53 is not a PS-X EXE");

    sincos = Reinterpret<int16_t>(ExeWindow(kSinCosTable, 4096 * 4));
    atan = Reinterpret<int32_t>(ExeWindow(kAtanTable, 64 * 4));
    atanU16 = Reinterpret<uint16_t>(ExeWindow(kAtanU16Table, 64 * 2));
    sqrtTable = Reinterpret<int16_t>(ExeWindow(kSqrtTable, 0x1000));
    // SqrtGte's table as a WINDOW with 2 KiB below it (ai.h).
    sqrtWindow = Reinterpret<int16_t>(ExeWindow(kSqrtTable - 0x800u, 0x2000));
    // The reciprocal-square-root table *(gp+2260), built by the PORTED fill loop of SLUS 0x8002E080.
    rsqrtTable.assign(1024, 0);
    rr::sim::FillRsqrtTable(rsqrtTable.data(), sqrtWindow.data() + 0x800 / 2);
    crashTable = ExeWindow(kCrashTable, 0x1000);
    altKind = ExeWindow(kAltKindTable, 0x400);
    planTable = Reinterpret<int32_t>(ExeWindow(kPlanTable, 16 * 4));
    tabSpeedClass = Reinterpret<int32_t>(ExeWindow(kTabSpeedClass, 12));
    tabThinkA = Reinterpret<int32_t>(ExeWindow(kTabThinkA, 36));
    tabThinkB = Reinterpret<int32_t>(ExeWindow(kTabThinkB, 60));
    tabCopFlat = Reinterpret<int32_t>(ExeWindow(kTabCopFlat, 12));
    tabAltA = Reinterpret<int32_t>(ExeWindow(kTabAltA, 24));
    tabAltB = Reinterpret<int32_t>(ExeWindow(kTabAltB, 36));
    tabSpeedCap = Reinterpret<int32_t>(ExeWindow(kTabSpeedCap, 12));
    tabCopBase = Reinterpret<int16_t>(ExeWindow(kTabCopBase + 2, 12));
    tabCopStep = Reinterpret<int16_t>(ExeWindow(kTabCopStep + 2, 12));
    tabCopMul = Reinterpret<int32_t>(ExeWindow(kTabCopMul, 12));
    const std::vector<uint8_t> profileBytes = ExeWindow(kProfile, 32 * 60);
    profile.assign(profileBytes.begin(), profileBytes.end());
    armTable = Reinterpret<uint32_t>(OverlayWindow(kArmTable, 19 * 4));
    armNone = kArmNone;
    armRace = kArmRace;
}

std::vector<uint8_t> GameTables::ExeWindow(uint32_t address, size_t bytes) const {
    std::vector<uint8_t> out(bytes, 0);
    if (address < kExeLoad) return out;
    const size_t at = kExeHeader + static_cast<size_t>(address - kExeLoad);
    for (size_t i = 0; i < bytes && at + i < exe_.size(); ++i) out[i] = exe_[at + i];
    return out;
}

std::vector<uint8_t> GameTables::OverlayWindow(uint32_t address, size_t bytes) const {
    std::vector<uint8_t> out(bytes, 0);
    if (address < kOverlayLoad) return out;
    const size_t at = static_cast<size_t>(address - kOverlayLoad);
    for (size_t i = 0; i < bytes && at + i < overlay_.size(); ++i) out[i] = overlay_[at + i];
    return out;
}

// ------------------------------------------------------------------------------- the road arena

std::map<uint32_t, std::vector<uint8_t>> ReadRoadObjects(const DiscImage& disc, int set) {
    std::map<uint32_t, std::vector<uint8_t>> out;
    const auto stream = disc.Find("DATA/STREAM" + std::to_string(set) + ".STR");
    if (!stream) throw std::runtime_error("STREAM" + std::to_string(set) + ".STR is not on disc");
    std::vector<uint8_t> head(0x90);
    for (uint64_t offset = 0; offset + rr::kChunkSize <= stream->size; offset += rr::kChunkSize) {
        disc.ReadForm1(stream->lba, offset, head.data(), head.size());
        if ((head[3] >> 4) != static_cast<uint8_t>(rr::ChunkType::Road)) continue;
        if (std::memcmp(head.data() + 0x8C, "GRPT", 4) != 0) continue;
        const uint32_t id = static_cast<uint32_t>(head[0x20]) | (static_cast<uint32_t>(head[0x21]) << 8) |
                            (static_cast<uint32_t>(head[0x22]) << 16) | (static_cast<uint32_t>(head[0x23]) << 24);
        if (out.count(id) != 0) continue;
        std::vector<uint8_t> chunk(rr::kChunkSize);
        disc.ReadForm1(stream->lba, offset, chunk.data(), chunk.size());
        out.emplace(id, std::move(chunk));
    }
    return out;
}

bool CheckRoadArena(const DiscImage& disc, int set, const std::string& ramPath, std::string& report) {
    std::vector<uint8_t> ram;
    std::string err;
    if (!ReadRam(ramPath, ram, err)) {
        report = "arenacheck: " + err;
        return false;
    }
    GuestRam snap(ram.data(), 0);
    const auto mapFile = disc.Find("DATA/ROAD" + std::to_string(set) + ".MAP");
    if (!mapFile) {
        report = "arenacheck: no ROAD" + std::to_string(set) + ".MAP on the disc";
        return false;
    }
    const std::vector<uint8_t> map = disc.ReadFile(*mapFile);
    const uint32_t g = snap.U32(rr::sim::kRoadGraphPtr);
    const uint32_t mapAt = g - 8u;
    rr::sim::RoadArena arena;
    const char* error = nullptr;
    if (arena.LoadRoadMap(map.data(), map.size(), mapAt, &error) != g) {
        report = std::string("arenacheck: ") + (error ? error : "the map loaded at another G");
        return false;
    }
    const std::map<uint32_t, std::vector<uint8_t>> objects = ReadRoadObjects(disc, set);
    std::vector<std::pair<uint32_t, uint32_t>> resident;
    const int32_t count = snap.S16(g + 0x28);
    const uint32_t btt = snap.U32(g + 0x1C);
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t obj = snap.U32(btt + 32u * static_cast<uint32_t>(i) + 12u);
        if (obj != 0) resident.emplace_back(snap.U32(obj), obj);
    }
    std::string ids;
    for (const auto& r : resident) {
        const auto it = objects.find(r.first);
        if (it == objects.end()) {
            report = "arenacheck: resident object " + std::to_string(r.first) + " is not in the stream";
            return false;
        }
        if (arena.LoadRoadObject(it->second.data(), it->second.size(), r.second - 0x20u, &error) != r.second) {
            report = std::string("arenacheck: ") + (error ? error : "object placed wrongly");
            return false;
        }
        ids += (ids.empty() ? "" : ", ") + std::to_string(r.first);
    }
    size_t compared = 0, differ = 0;
    uint32_t first = 0;
    auto compare = [&](uint32_t at, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            const size_t o = (at + i) & (GuestRam::kRamSize - 1u);
            ++compared;
            if (arena.Ram()[o] != ram[o]) {
                if (differ == 0) first = at + static_cast<uint32_t>(i);
                ++differ;
            }
        }
    };
    compare(rr::sim::kRoadGraphPtr, 4);
    compare(mapAt, map.size());
    for (const auto& r : resident) compare(r.second - 0x20u, rr::kChunkSize);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "arenacheck: ROAD%d.MAP (%zu bytes at 0x%08X, G 0x%08X) and %zu resident road "
                  "object(s) [%s], rebuilt from the disc at the image's own addresses: %zu bytes "
                  "compared, %zu differ%s",
                  set, map.size(), mapAt, g, resident.size(), ids.c_str(), compared, differ,
                  differ ? "" : " - the loader's fix-ups are reproduced exactly");
    report = buf;
    if (differ != 0) {
        char more[64];
        std::snprintf(more, sizeof(more), " (first at 0x%08X)", first);
        report += more;
    }
    return differ == 0;
}

// =============================================================================== the route arena
//
// What the race loader builds for the road layer, rebuilt
// from the disc:
//
//   * the race graph `STREAM<n>.GRF` at *(gp+472): the file, byte for byte, with its two block
//     offsets +0x14 (nodes) and +0x18 (roads) turned into addresses;
//   * the route block `ROADGRF<n>.TXT`'s race-graph parser RASHCDI 0x8006A0C8 builds, transcribed
//     from our disassembly of RASHCDI and not from road.md 1.3 where the two disagree:
//       header 0x800D6170 (40 bytes, zeroed by 0x80069DEC): +0x00 the allocation, +0x04 [RMAGIC],
//         +0x08 [NUM_ENTRIES], +0x0A..+0x10 the four density keys, +0x12 [RACEINTS], +0x14 START,
//         +0x18 FINISH, +0x1C the race's text block, +0x20 its length, +0x24 the records;
//       the allocation, 120 (n + 1) + 56 bytes: START (28, memset -1: road, dist << 16, dir, node,
//         checker[3]) at +0, FINISH (the same) at +28, the n records at +56 and the START RECORD at
//         index n;
//       a record (0x8006A588..0x8006A748): memset -1, node, dist << 12, span << 12, then the links
//         PACKED (`link[+0x0C]++`, each {road, dir, (u32 len >> 6) << 16, u16 nodeA, u16 nodeB} by
//         the helper 0x8006A014 from the graph), the route roads PACKED at +0x54 (count +0x10), the
//         next nodes PACKED at +0x64 (`sw v1,100(s0); addiu s0,s0,4` - road.md 1.2's "positional" is
//         wrong), +0x74 = +0x76 = 0;
//       the start record (0x8006A2A8..0x8006A354, when [START] is read): node -1, +4 = +8 = -4096,
//         one link {START.road, START.dir} (the helper), links 1..3 zeroed with road -1, route
//         road START.road, next node START.node, the other route/next slots -1, +0x74 = 0xFFFF,
//         +0x76 = 0.
//   +0x76 of a record is the bound-slot mask the PORTED RouteBind keeps at run time.
namespace {

struct RaceBlock {
    size_t begin = 0, end = 0;   // [BEGIN] .. the next [END], exclusive (the header's +0x1C/+0x20)
    int32_t numEntries = 0;
    int32_t raceId = -1;
    uint32_t rmagic = 0;
    int32_t raceInts = -1;
    std::vector<std::string> lines;
};

int32_t ParseIntAt(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    const char* start = s.c_str() + pos;
    char* endp = nullptr;
    const long long v = std::strtoll(start, &endp, 10);
    pos += static_cast<size_t>(endp - start);
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<unsigned long long>(v)));
}

std::vector<int32_t> ParseInts(const std::string& s) {
    std::vector<int32_t> out;
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r')) ++pos;
        if (pos >= s.size()) break;
        const size_t before = pos;
        out.push_back(ParseIntAt(s, pos));
        if (pos == before) break;
    }
    return out;
}

// Finds the block of race `raceId` (or, with raceId < 0, the one whose [RMAGIC] is `rmagic`).
bool FindRaceBlock(const std::string& txt, int32_t raceId, uint32_t rmagic, RaceBlock& out) {
    {
        const size_t k = txt.find("[NUM_ENTRIES]=");
        if (k != std::string::npos) {
            size_t pos = k + 14;
            out.numEntries = ParseIntAt(txt, pos);
        }
    }
    size_t at = 0;
    while ((at = txt.find("[BEGIN]", at)) != std::string::npos) {
        const size_t end = txt.find("[END]", at);
        if (end == std::string::npos) return false;
        RaceBlock b;
        b.begin = at;
        b.end = end;
        b.numEntries = out.numEntries;
        size_t ls = at;
        while (ls < end) {
            size_t le = txt.find('\n', ls);
            if (le == std::string::npos || le > end) le = end;
            std::string line = txt.substr(ls, le - ls);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) b.lines.push_back(line);
            ls = le + 1;
        }
        for (const std::string& l : b.lines) {
            size_t pos = l.find('=');
            if (pos == std::string::npos) continue;
            ++pos;
            if (l.rfind("[RACEID]", 0) == 0) b.raceId = ParseIntAt(l, pos);
            else if (l.rfind("[RMAGIC]", 0) == 0) b.rmagic = static_cast<uint32_t>(ParseIntAt(l, pos));
            else if (l.rfind("[RACEINTS]", 0) == 0) b.raceInts = ParseIntAt(l, pos);
        }
        if ((raceId >= 0 && b.raceId == raceId) || (raceId < 0 && b.rmagic == rmagic)) {
            out = std::move(b);
            return true;
        }
        at = end;
    }
    return false;
}

// The route block for `block`, laid out for an allocation at `alloc`; `graph` is the GRF file.
// `header` gets the 40 bytes of 0x800D6170 (with `textAt` as the +0x1C word). With `mutate` the next
// nodes go to their TEXT slot (the control of the check).
bool BuildRouteBlock(const RaceBlock& block, const std::vector<uint8_t>& graph, uint32_t alloc,
                     uint32_t textAt, bool mutate, std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& header, std::string& error) {
    const int32_t n = block.raceInts;
    if (n < 0) {
        error = "the race has no [RACEINTS] (the parser's -1 path writes through a null record)";
        return false;
    }
    if (graph.size() < 0x1C || std::memcmp(graph.data(), "RGTS", 4) != 0) {
        error = "the race graph is not an RGTS block";
        return false;
    }
    const uint32_t roadsOff = static_cast<uint32_t>(ReadS32(graph, 0x18));
    const int32_t roadCount = ReadS32(graph, 0x10);
    bytes.assign(56u + 120u * static_cast<size_t>(n + 1), 0);
    uint8_t* start = bytes.data();
    uint8_t* finish = bytes.data() + 28;
    std::memset(start, 0xFF, 28);  // 0x8006A184
    std::memset(finish, 0xFF, 28); // 0x8006A1A8
    auto rec = [&](int32_t i) { return bytes.data() + 56 + 120 * static_cast<size_t>(i); };
    // 0x8006A014: the link's length and end nodes, from the graph (GraphRoad / GraphNode).
    auto fillLink = [&](uint8_t* link) {
        const int32_t road = ReadS32(link, 0);
        if (road == -1) return;
        if (road < 0 || road >= roadCount) {
            error = "a link names a road past the race graph";
            return;
        }
        const size_t r = roadsOff + 16u * static_cast<size_t>(road);
        WriteS16(link, 12, -1);
        WriteS16(link, 14, -1);
        const uint32_t len = static_cast<uint32_t>(ReadS32(graph, r + 4));
        WriteS32(link, 8, static_cast<int32_t>((len >> 6) << 16));
        WriteS16(link, 12, static_cast<int16_t>(ReadS32(graph, r + 8)));  // lhu 8(s0), GraphNode != 0
        WriteS16(link, 14, static_cast<int16_t>(ReadS32(graph, r + 12)));
    };
    int32_t rowsLeft = -1;
    int32_t row = 0;
    for (const std::string& l : block.lines) {
        const size_t eq = l.find('=');
        auto values = [&]() { return ParseInts(eq == std::string::npos ? std::string() : l.substr(eq + 1)); };
        if (l.rfind("[START]", 0) == 0) {
            const std::vector<int32_t> v = values();
            if (v.size() < 4) { error = "[START] has fewer than 4 values"; return false; }
            WriteS32(start, 0, v[0]);
            WriteS32(start, 4, static_cast<int32_t>(static_cast<uint32_t>(v[1]) << 16));
            WriteS32(start, 8, v[2]);
            WriteS32(start, 12, v[3]);
            uint8_t* s = rec(n);                                   // 0x8006A28C
            WriteS32(s, 4, -4096);
            WriteS32(s, 8, -4096);
            WriteS32(s, 0, -1);
            WriteS32(s, 12, 1);
            WriteS32(s, 16, 1);
            WriteS32(s, 20, v[0]);
            WriteS32(s, 24, v[2]);
            fillLink(s + 20);
            for (int k = 0; k < 4; ++k) {                          // 0x8006A2F0..0x8006A320
                if (k > 0) {
                    std::memset(s + 20 + 16 * k, 0, 16);
                    WriteS32(s, 20 + 16 * static_cast<size_t>(k), -1);
                }
                WriteS32(s, 0x54 + 4 * static_cast<size_t>(k), -1);
                WriteS32(s, 0x64 + 4 * static_cast<size_t>(k), -1);
            }
            WriteS16(s, 0x74, static_cast<int16_t>(0xFFFF));
            WriteS16(s, 0x76, 0);
            WriteS32(s, 0x54, v[0]);
            WriteS32(s, 0x64, v[3]);
        } else if (l.rfind("[FINISH_CHECKER]", 0) == 0) {
            const std::vector<int32_t> v = values();
            for (size_t k = 0; k < 3 && k < v.size(); ++k) WriteS32(finish, 16 + 4 * k, v[k]);
        } else if (l.rfind("[START_CHECKER]", 0) == 0) {
            const std::vector<int32_t> v = values();
            for (size_t k = 0; k < 3 && k < v.size(); ++k) WriteS32(start, 16 + 4 * k, v[k]);
        } else if (l.rfind("[FINISH]", 0) == 0) {
            const std::vector<int32_t> v = values();
            if (v.size() < 4) { error = "[FINISH] has fewer than 4 values"; return false; }
            WriteS32(finish, 0, v[0]);
            WriteS32(finish, 4, static_cast<int32_t>(static_cast<uint32_t>(v[1]) << 16));
            WriteS32(finish, 8, v[2]);
            WriteS32(finish, 12, v[3]);
        } else if (l.rfind("[RACEINTS]", 0) == 0) {
            rowsLeft = n;
            row = 0;
        } else if (!l.empty() && l[0] != '[' && rowsLeft > 0) {
            const std::vector<int32_t> v = ParseInts(l);
            uint8_t* r = rec(row);
            std::memset(r, 0xFF, 120);                             // 0x8006A58C
            if (v.empty()) continue;
            WriteS32(r, 0, v[0]);
            if (v[0] == -1) { rowsLeft = 0; continue; }            // 0x8006A5B0
            auto at = [&](size_t k) { return k < v.size() ? v[k] : -1; };
            WriteS32(r, 4, static_cast<int32_t>(static_cast<uint32_t>(at(1)) << 12));
            WriteS32(r, 12, 0);
            WriteS32(r, 8, static_cast<int32_t>(static_cast<uint32_t>(at(2)) << 12));
            for (size_t k = 0; k < 4; ++k) {
                const int32_t road = at(3 + 2 * k), dir = at(4 + 2 * k);
                if (road == -1) continue;
                const int32_t c = ReadS32(r, 12);
                WriteS32(r, 20 + 16 * static_cast<size_t>(c), road);
                WriteS32(r, 24 + 16 * static_cast<size_t>(c), dir);
                fillLink(r + 20 + 16 * c);
                WriteS32(r, 12, c + 1);
            }
            WriteS32(r, 16, 0);
            for (size_t k = 0; k < 4; ++k) {
                const int32_t v2 = at(11 + k);
                if (v2 == -1) continue;
                const int32_t c = ReadS32(r, 16);
                WriteS32(r, 0x54 + 4 * static_cast<size_t>(c), v2);
                WriteS32(r, 16, c + 1);
            }
            size_t slot = 0;
            for (size_t k = 0; k < 4; ++k) {
                const int32_t v2 = at(15 + k);
                if (v2 == -1) continue;
                WriteS32(r, 0x64 + 4 * (mutate ? k : slot), v2);   // 0x8006A72C: packed
                ++slot;
            }
            WriteS16(r, 0x74, 0);
            WriteS16(r, 0x76, 0);
            ++row;
            if (--rowsLeft == 0) rowsLeft = -1;
        }
    }
    if (!error.empty()) return false;
    header.assign(40, 0);
    WriteS32(header, 0x00, static_cast<int32_t>(alloc));
    WriteS32(header, 0x04, static_cast<int32_t>(block.rmagic));
    WriteS16(header.data(), 0x08, static_cast<int16_t>(block.numEntries));
    WriteS16(header.data(), 0x12, static_cast<int16_t>(n));
    WriteS32(header, 0x14, static_cast<int32_t>(alloc));
    WriteS32(header, 0x18, static_cast<int32_t>(alloc + 28u));
    WriteS32(header, 0x1C, static_cast<int32_t>(textAt));
    WriteS32(header, 0x20, static_cast<int32_t>(block.end - block.begin));
    WriteS32(header, 0x24, static_cast<int32_t>(alloc + 56u));
    return true;
}

std::vector<uint8_t> RelocatedGraph(const std::vector<uint8_t>& grf, uint32_t at) {
    std::vector<uint8_t> g = grf;
    WriteS32(g, 0x14, static_cast<int32_t>(at + static_cast<uint32_t>(ReadS32(grf, 0x14))));
    WriteS32(g, 0x18, static_cast<int32_t>(at + static_cast<uint32_t>(ReadS32(grf, 0x18))));
    return g;
}

std::string ReadText(const DiscImage& disc, const std::string& name) {
    const auto f = disc.Find(name);
    if (!f) return std::string();
    const std::vector<uint8_t> b = disc.ReadFile(*f);
    return std::string(b.begin(), b.end());
}

} // namespace

bool CheckRouteArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate) {
    std::vector<uint8_t> ram;
    std::string err;
    if (!ReadRam(ramPath, ram, err)) {
        report = "routearenacheck: " + err;
        return false;
    }
    GuestRam snap(ram.data(), kArenaGp);
    const uint32_t rmagic = snap.U32(kRouteHeader + 4u);
    const uint32_t graphAt = snap.U32(kArenaGp + rr::sim::kRaceGraphGp);
    // The race the loader parsed: the road set is game_state+0x30 and the
    // race id game_state+0x40, the id 0x80069C60 looks the block up by (0x80069E64). [RMAGIC] alone
    // does not name a race - races 3 and 4 of set 1 share 0x3862BDCB - so it is checked, not used.
    const uint32_t gs = snap.U32(kGameStatePtr);
    const int set = (gs >= 0x80000000u && gs < 0x80200000u) ? static_cast<int>(snap.U32(gs + 0x30u)) : 0;
    const int32_t raceId = (set != 0) ? snap.S32(gs + 0x40u) : -1;
    RaceBlock block;
    std::string txt;
    bool found = false;
    if (set == 1 || set == 2) {
        txt = ReadText(disc, "DATA/ROADGRF" + std::to_string(set) + ".TXT");
        found = !txt.empty() && FindRaceBlock(txt, raceId, 0, block) && block.rmagic == rmagic;
    }
    if (!found || graphAt < 0x80000000u || graphAt >= 0x80200000u) {
        char b[240];
        std::snprintf(b, sizeof(b),
                      "routearenacheck %s: REFUSED - not a race image (game_state set %d race %d, [RMAGIC] "
                      "0x%08X at 0x800D6174, race graph pointer 0x%08X: no race of ROADGRF1/2.TXT matches)",
                      ramPath.c_str(), set, raceId, rmagic, graphAt);
        report = b;
        return false;
    }
    const auto grfFile = disc.Find("DATA/STREAM" + std::to_string(set) + ".GRF");
    if (!grfFile) {
        report = "routearenacheck: no STREAM" + std::to_string(set) + ".GRF on the disc";
        return false;
    }
    const std::vector<uint8_t> grf = disc.ReadFile(*grfFile);
    size_t compared = 0, differ = 0, notCompared = 0;
    uint32_t first = 0;
    auto cmp = [&](uint32_t at, const uint8_t* want, size_t n, const std::function<bool(size_t)>& skip) {
        for (size_t i = 0; i < n; ++i) {
            if (skip && skip(i)) {
                ++notCompared;
                continue;
            }
            ++compared;
            if (snap.U8(at + static_cast<uint32_t>(i)) != want[i]) {
                if (differ == 0) first = at + static_cast<uint32_t>(i);
                ++differ;
            }
        }
    };
    // 1. The race graph, relocated.
    const std::vector<uint8_t> g = RelocatedGraph(grf, graphAt);
    cmp(graphAt, g.data(), g.size(), nullptr);
    const size_t graphDiffer = differ;
    // 2. The route block, at the image's own allocation.
    const uint32_t alloc = snap.U32(kRouteHeader);
    std::vector<uint8_t> bytes, header;
    if (!BuildRouteBlock(block, grf, alloc, 0, mutate, bytes, header, err)) {
        report = "routearenacheck: " + err;
        return false;
    }
    cmp(kRouteHeader, header.data(), header.size(), [](size_t i) { return i >= 0x1C && i < 0x20; });
    cmp(alloc, bytes.data(), bytes.size(), [](size_t i) {
        return i >= 56 && ((i - 56) % 120 == 0x76 || (i - 56) % 120 == 0x77);
    });
    // 3. The text block the header's +0x1C names must BE that race's block, if it is still resident.
    const uint32_t textAt = snap.U32(kRouteHeader + 0x1Cu);
    size_t textSame = 0;
    const size_t textLen = block.end - block.begin;
    for (size_t i = 0; i < textLen && textAt >= 0x80000000u; ++i)
        if (snap.U8(textAt + static_cast<uint32_t>(i)) == static_cast<uint8_t>(txt[block.begin + i])) ++textSame;
    char b[640];
    std::snprintf(b, sizeof(b),
                  "routearenacheck %s%s: race %d of set %d ([RMAGIC] 0x%08X), %d route record(s) + the start "
                  "record; STREAM%d.GRF (%zu bytes at 0x%08X, +0x14/+0x18 relocated): %zu differ; the route "
                  "block (header 0x800D6170 + %zu bytes at 0x%08X) rebuilt from ROADGRF%d.TXT by the "
                  "parser's rules: %zu bytes compared in all, %zu differ%s; %zu byte(s) not compared "
                  "(each record's +0x76 bound-slot mask and the header's text pointer); the text block the "
                  "header names equals the race's own in %zu of %zu bytes",
                  ramPath.c_str(), mutate ? " [MUTATED: next nodes at their text slot]" : "", block.raceId,
                  set, rmagic, block.raceInts, set, g.size(), graphAt, graphDiffer, bytes.size(), alloc, set,
                  compared, differ, differ ? "" : " - the parser's layout is reproduced exactly", notCompared,
                  textSame, textLen);
    report = b;
    if (differ != 0) {
        char more[64];
        std::snprintf(more, sizeof(more), " (first at 0x%08X)", first);
        report += more;
    }
    // 4. The ported route loader: the PORTED parser RASHCDI 0x8006A0C8 (route_parse.h, with the product's host string calls) run
    // on a copy of the image - ROADGRF<n>.TXT laid into the image's own text buffer *(res+0xA38) as RoadText reads it,
    // the header and the allocation filled with 0xA5 first, the malloc answered with the image's own allocation - and
    // the header (its text pointer INCLUDED) and the route block compared with the image. -mutate lays the text one
    // byte later (the text pointer must then differ).
    size_t portCompared = 0, portDiffer = 0;
    std::string portWhy;
    {
        std::vector<uint8_t> copy = ram;
        GuestRam pg(copy.data(), kArenaGp);
        rr::game::LoaderOverlayReset(disc);
        rr::game::LoaderOverlay ov(pg, disc); // RASHCDI's keys (the image holds RASHCDG there, as the race runs)
        const uint32_t res = pg.U32(rr::sim::kLdResListPtr);
        const uint32_t buf = pg.U32(res + 2616u) + (mutate ? 1u : 0u);
        pg.WriteBlock(buf, reinterpret_cast<const uint8_t*>(txt.data()), static_cast<uint32_t>(txt.size()));
        pg.W8(buf + static_cast<uint32_t>(txt.size()), 0);
        for (uint32_t k = 0; k < 40u; ++k) pg.W8(kRouteHeader + k, 0xA5);
        for (uint32_t k = 0; k < static_cast<uint32_t>(bytes.size()); ++k) pg.W8(alloc + k, 0xA5);
        struct Host final : rr::sim::LoaderCallees {
            GuestRam& g;
            uint32_t alloc;
            std::string why;
            Host(GuestRam& gg, uint32_t a) : g(gg), alloc(a) {}
            bool Call(uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0) override {
                v0 = 0;
                if (fn == rr::sim::kLdMalloc) { v0 = alloc; return true; }        // the image's own block
                if (fn == rr::sim::kLdMemset) {                                    // SLUS 0x8001E100, by words
                    const uint32_t w = a[1] | (a[1] << 8) | (a[1] << 16) | (a[1] << 24);
                    for (uint32_t k = 0; k < a[2]; k += 4) g.W32(a[0] + k, w);
                    return !g.Faulted();
                }
                why = "unanswered callee 0x" + [&] { char h[12]; std::snprintf(h, sizeof(h), "%08X", fn); return std::string(h); }();
                return false;
            }
        } host(pg, alloc);
        rr::sim::RouteCallees rc(pg, host);
        if (!rr::sim::RouteParse(pg, buf, static_cast<uint32_t>(txt.size()), 0x801FF000u, rc) || pg.Faulted())
            portWhy = host.why.empty() ? std::string(rc.error ? rc.error : "a view fault") : host.why;
        for (uint32_t k = 0; k < 40u; ++k) {
            ++portCompared;
            if (pg.U8(kRouteHeader + k) != snap.U8(kRouteHeader + k)) ++portDiffer;
        }
        for (uint32_t k = 0; k < static_cast<uint32_t>(bytes.size()); ++k) {
            if (k >= 56u && ((k - 56u) % 120u == 0x76u || (k - 56u) % 120u == 0x77u)) continue;
            ++portCompared;
            if (pg.U8(alloc + k) != snap.U8(alloc + k)) ++portDiffer;
        }
    }
    {
        char more[240];
        std::snprintf(more, sizeof(more),
                      "; the PORTED parser 0x8006A0C8 on the image (RASHCDI laid, host string calls, text at *(res+0xA38)%s): header + "
                      "route block %zu bytes compared (text pointer included), %zu differ%s%s",
                      mutate ? " + 1" : "", portCompared, portDiffer, portWhy.empty() ? "" : ", REFUSED: ", portWhy.c_str());
        report += more;
    }
    return differ == 0 && portDiffer == 0 && portWhy.empty();
}

// =============================================================================== the animation arena
//
// The bank loader RASHCDI 0x8005BCDC(desc, name) loads a whole `ANIMTBL*.PSX` file, walks its DMD3
// chain (0x8005BB6C) turning each block's +0x14 into the address of its payload (0x8005BAF8) and
// records the block in a clip table; the slot records live at 0x800CF5D8 (the constant the
// descriptor initialiser RASHCDI 0x8005D130 writes into +0x10, with capacity 10 and every slot
// cleared). Measured in all 18 race images: the six resident banks are, in SLOT
// order, ANIMTBL2, 3, B, 1, W, P, each file followed by its table, the allocator stepping
// `(size + 8) & ~7` bytes per allocation; the bank table 0x800CE190 maps stance banks 0..5 to slots
// 3, 0, 1, 4, 2, 5. The long set is the only one any capture holds.
namespace {

struct AnimBankFile {
    const char* name;
    uint32_t stanceBank; // the index in 0x800CE190 that names this slot
};
constexpr AnimBankFile kAnimBankOrder[6] = {{"ANIMTBL2", 1}, {"ANIMTBL3", 2}, {"ANIMTBLB", 4},
                                            {"ANIMTBL1", 0}, {"ANIMTBLW", 3}, {"ANIMTBLP", 5}};

std::vector<uint32_t> Dmd3Chain(const std::vector<uint8_t>& f) {
    std::vector<uint32_t> out;
    size_t o = 0;
    while (o + 0x18 <= f.size() && std::memcmp(f.data() + o, "DMD3", 4) == 0) {
        out.push_back(static_cast<uint32_t>(o));
        const uint32_t size = static_cast<uint32_t>(ReadS32(f, o + 4));
        if (size == 0) break;
        o += size;
    }
    return out;
}

// The file with the 0x8005BAF8 fix-up applied, for a load at `at`; `mutate` leaves it out.
std::vector<uint8_t> FixedBank(const std::vector<uint8_t>& f, uint32_t at, bool mutate) {
    std::vector<uint8_t> out = f;
    if (!mutate)
        for (uint32_t o : Dmd3Chain(f)) WriteS32(out, o + 0x14, static_cast<int32_t>(at + o + 0x18u));
    return out;
}

} // namespace

bool CheckAnimArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate) {
    std::vector<uint8_t> ram;
    std::string err;
    if (!ReadRam(ramPath, ram, err)) {
        report = "animarenacheck: " + err;
        return false;
    }
    GuestRam snap(ram.data(), kArenaGp);
    const uint32_t desc = rr::sim::kAnimDescriptor;
    const uint32_t objects = snap.U32(desc), programs = snap.U32(desc + 4u);
    const int32_t count = snap.S32(desc + 12u);
    const uint32_t slots = snap.U32(desc + 16u);
    const int32_t used = snap.S32(desc + 20u), capacity = snap.S32(desc + 24u);
    if (objects < 0x80000000u || objects >= 0x80200000u || slots != kAnimSlots || used <= 0 || used > 10) {
        char b[200];
        std::snprintf(b, sizeof(b),
                      "animarenacheck %s: REFUSED - no animation descriptor at 0x800CE170 (objects 0x%08X, "
                      "slots 0x%08X, used %d): not a race image",
                      ramPath.c_str(), objects, slots, used);
        report = b;
        return false;
    }
    std::map<std::string, std::vector<uint8_t>> files;
    for (const AnimBankFile& f : kAnimBankOrder) {
        const auto e = disc.Find(std::string("DATA/") + f.name + ".PSX");
        if (!e) {
            report = std::string("animarenacheck: DATA\\") + f.name + ".PSX is not on the disc";
            return false;
        }
        files.emplace(f.name, disc.ReadFile(*e));
    }
    size_t compared = 0, differ = 0, wiring = 0, wiringBad = 0;
    uint32_t first = 0;
    auto cmp = [&](uint32_t at, const uint8_t* want, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            ++compared;
            if (snap.U8(at + static_cast<uint32_t>(i)) != want[i]) {
                if (differ == 0) first = at + static_cast<uint32_t>(i);
                ++differ;
            }
        }
    };
    std::string lines;
    // The descriptor's constant fields and the slot records, as this session writes them.
    {
        uint8_t d[4];
        WriteS32(d, 0, static_cast<int32_t>(kAnimSlots));
        cmp(desc + 16u, d, 4);
        WriteS32(d, 0, 6);
        cmp(desc + 20u, d, 4);
        WriteS32(d, 0, static_cast<int32_t>(kAnimSlotCapacity));
        cmp(desc + 24u, d, 4);
        WriteS32(d, 0, 0);
        cmp(desc + 28u, d, 4);
    }
    for (int32_t k = 0; k < used && k < 6; ++k) {
        const uint32_t s = slots + 12u * static_cast<uint32_t>(k);
        const AnimBankFile& bf = kAnimBankOrder[k];
        const std::vector<uint8_t>& f = files[bf.name];
        const std::vector<uint32_t> chain = Dmd3Chain(f);
        const uint32_t table = snap.U32(s + 4u), at = snap.U32(s + 8u);
        // {u8 0, u8 1 (used), u16 clips, u32 table, u32 file}: the table right after the file, at
        // the allocator's `(size + 8) & ~7` step - the rule this session lays its banks out by
        uint8_t rec[12] = {0, 1, 0, 0};
        WriteS16(rec, 2, static_cast<int16_t>(chain.size()));
        WriteS32(rec, 4, static_cast<int32_t>(at + ((static_cast<uint32_t>(f.size()) + 8u) & ~7u)));
        WriteS32(rec, 8, static_cast<int32_t>(at));
        const size_t before = differ;
        cmp(s, rec, 12);
        const std::vector<uint8_t> fixed = FixedBank(f, at, mutate);
        cmp(at, fixed.data(), fixed.size());
        std::vector<uint8_t> tab(4u * chain.size());
        for (size_t i = 0; i < chain.size(); ++i) WriteS32(tab, 4 * i, static_cast<int32_t>(at + chain[i]));
        cmp(table, tab.data(), tab.size());
        uint8_t bank[4];
        WriteS32(bank, 0, static_cast<int32_t>(s));
        cmp(rr::sim::kAnimBankTable + 4u * bf.stanceBank, bank, 4);
        char b[200];
        std::snprintf(b, sizeof(b), "  slot %d: %s.PSX (%zu bytes, %zu clips) at 0x%08X, table 0x%08X: %zu differ\n",
                      k, bf.name, f.size(), chain.size(), at, table, differ - before);
        lines += b;
    }
    {
        uint8_t z[8] = {};
        cmp(rr::sim::kAnimBankTable + 24u, z, 8); // banks 6 and 7: none in the long set
    }
    // The wiring: object i's owner and program, rider i's +0x21C, the bike's +0x354.
    const uint32_t pool0 = snap.U32(0x8005B3A0), pool1 = snap.U32(0x8005B3A4);
    for (int32_t i = 0; i < count && i < 18; ++i) {
        const uint32_t a = objects + rr::sim::kAnimObjectBytes * static_cast<uint32_t>(i);
        const uint32_t r = pool1 + kRiderBytes * static_cast<uint32_t>(i);
        const uint32_t b = pool0 + static_cast<uint32_t>(kEntitySize) * static_cast<uint32_t>(i);
        wiring += 5;
        if (snap.U32(a) != r) ++wiringBad;
        if (snap.U32(a + 4u) != programs + rr::sim::kAnimProgramBytes * static_cast<uint32_t>(i)) ++wiringBad;
        if (snap.U32(r + 0x21Cu) != a) ++wiringBad;
        if (snap.U32(r + 0x254u) != b) ++wiringBad;
        if (snap.U32(b + 0x354u) != r) ++wiringBad;
    }
    char head[512];
    std::snprintf(head, sizeof(head),
                  "animarenacheck %s%s: descriptor 0x800CE170 (objects 0x%08X, programs 0x%08X, %d objects, "
                  "slots 0x%08X, %d used of %d); the %d bank file(s) placed at the image's own addresses with "
                  "the loader's +0x14 fix-up, their clip tables, the slot records and the bank table: %zu bytes "
                  "compared, %zu differ%s; wiring (object owner/program, rider +0x21C/+0x254, bike +0x354) "
                  "over %d riders: %zu of %zu hold\n",
                  ramPath.c_str(), mutate ? " [MUTATED: no +0x14 fix-up]" : "", objects, programs, count, slots,
                  used, capacity, used, compared, differ, differ ? "" : " - reproduced exactly",
                  std::min(count, 18), wiring - wiringBad, wiring);
    report = head + lines;
    if (differ != 0) {
        char more[64];
        std::snprintf(more, sizeof(more), "  first difference at 0x%08X\n", first);
        report += more;
    }
    return differ == 0 && wiringBad == 0;
}

// =============================================================================== the camera arena
namespace {

bool ImageNamesRace(const DiscImage& disc, GuestRam& snap, int& set, int32_t& raceId, std::string& why) {
    const uint32_t gs = snap.U32(kGameStatePtr);
    set = (gs >= 0x80000000u && gs < 0x80200000u) ? static_cast<int>(snap.U32(gs + 0x30u)) : 0;
    raceId = (set != 0) ? snap.S32(gs + 0x40u) : -1;
    const uint32_t rmagic = snap.U32(kRouteHeader + 4u);
    const uint32_t p1 = snap.U32(kP1EntityPtr);
    RaceBlock block;
    bool found = false;
    if (set == 1 || set == 2) {
        const std::string txt = ReadText(disc, "DATA/ROADGRF" + std::to_string(set) + ".TXT");
        found = !txt.empty() && FindRaceBlock(txt, raceId, 0, block) && block.rmagic == rmagic;
    }
    if (!found || p1 < 0x80000000u || p1 >= 0x80200000u) {
        char b[240];
        std::snprintf(b, sizeof(b),
                      "REFUSED - not a race image (game_state set %d race %d, [RMAGIC] 0x%08X, player 1's bike "
                      "*(0x8005B38C) = 0x%08X)",
                      set, raceId, rmagic, p1);
        why = b;
        return false;
    }
    return true;
}

} // namespace

bool CheckCameraArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate) {
    std::vector<uint8_t> ram;
    std::string err;
    if (!ReadRam(ramPath, ram, err)) {
        report = "camarenacheck: " + err;
        return false;
    }
    GuestRam snap(ram.data(), kArenaGp);
    int set = 0;
    int32_t raceId = -1;
    if (!ImageNamesRace(disc, snap, set, raceId, err)) {
        report = "camarenacheck " + ramPath + ": " + err;
        return false;
    }
    const uint32_t p1 = snap.U32(kP1EntityPtr);
    const uint32_t gs = snap.U32(kGameStatePtr);
    const int players = static_cast<int>(snap.U32(gs + 0x30u));
    // 1. CAMERA*.CA: the file the loader's suffix rule names (0x800675B0), its first 224 bytes.
    const std::string name = rr::CameraFileName(players, snap.U32(p1 + 0xB4u));
    std::string discName = name;
    std::replace(discName.begin(), discName.end(), '\\', '/');
    const auto f = disc.Find(discName);
    if (!f) {
        report = "camarenacheck: " + name + " is not on the disc";
        return false;
    }
    const std::vector<uint8_t> file = disc.ReadFile(*f);
    const size_t from = mutate ? 4u : 0u;
    if (file.size() < from + rr::kCameraBytesRead) {
        report = "camarenacheck: " + name + " is shorter than the 224 bytes the loader reads";
        return false;
    }
    // 2. View record 0, built on a copy of the image by the session's own InitViewRecord from the
    // image's own player bike (whose cursor, key and contact point the record copies).
    std::vector<uint8_t> work = ram;
    GuestRam w(work.data(), kArenaGp);
    for (uint32_t k = 0; k < kViewStride; ++k) w.W8(kViewArray + k, 0xA5);
    rr::sim::RoadRuntimeNative road;
    std::string how = "InitViewRecord (transcribed, RRJB_LOADER2_CAM=off)";
    if (rr::game::Loader2CamPorted()) { // the PORTED CameraSetUp as the product runs it, the file off the disc image
        for (uint32_t k = 0; k < rr::kCameraBytesRead; ++k) w.W8(kCameraFileAt + k, 0xA5);
        bool ok = false;
        how = rr::game::PortedCameraSetUp(w, disc, kArenaStepSp, false, ok, static_cast<uint32_t>(from));
        if (!ok) {
            report = "camarenacheck: " + how;
            return false;
        }
        how = "the PORTED CameraSetUp RASHCDI 0x80067564 / CameraFileRead / CameraInit (loader2_cam)";
    } else {
        InitViewRecord(w, kViewArray, p1, kView0Handle, 0, 0, kArenaStepSp, road);
        if (w.Faulted()) {
            report = "camarenacheck: InitViewRecord faulted on this image";
            return false;
        }
        w.WriteBlock(kCameraFileAt, file.data() + from, static_cast<uint32_t>(rr::kCameraBytesRead));
    }
    size_t compared = 0, differ = 0;
    uint32_t first = 0;
    auto cmp = [&](uint32_t at, uint32_t n) {
        for (uint32_t k = 0; k < n; ++k) {
            ++compared;
            if (snap.U8(at + k) != w.U8(at + k)) {
                if (differ == 0) first = at + k;
                ++differ;
            }
        }
    };
    cmp(kCameraFileAt, static_cast<uint32_t>(rr::kCameraBytesRead));
    const size_t fileDiffer = differ;
    // The fields CameraInit and its caller write to a constant and nothing in a race frame rewrites
    // (camera.cpp writes none of them; +0x238 only by SetMode / the director, to a player's bike).
    const struct { uint32_t off, n; } kFields[] = {{0x2C, 4},  {0xAC, 2},  {0xB4, 4},  {0x130, 4}, {0x134, 4},
                                                   {0x138, 4}, {0x140, 2}, {0x238, 4}, {0x45C, 4}};
    uint32_t fieldBytes = 0;
    for (const auto& fl : kFields) {
        cmp(kViewArray + fl.off, fl.n);
        fieldBytes += fl.n;
    }
    char b[1000];
    std::snprintf(b, sizeof(b),
                  "camarenacheck %s%s: race %d of set %d, %d player(s), player 1's bike 0x%08X (+0xB4 = %u); %s: "
                  "224 bytes at 0x800CD7B8, %zu differ; view record 0 at 0x800CD898 built by %s: %u bytes of "
                  "init-constant fields compared "
                  "(+0x2C +0xAC +0xB4 +0x130 +0x134 +0x138 +0x140 +0x238 +0x45C), %zu differ; %u bytes of the "
                  "record not compared (the camera state ViewUpdate rewrites every frame); in all %zu compared, %zu "
                  "differ%s",
                  ramPath.c_str(), mutate ? " [MUTATED: CAMERA.CA read from offset 4]" : "", raceId, set, players, p1,
                  snap.U32(p1 + 0xB4u), name.c_str(), fileDiffer, how.c_str(), fieldBytes, differ - fileDiffer,
                  kViewStride - fieldBytes, compared, differ, differ ? "" : " - reproduced exactly");
    report = b;
    if (differ != 0) {
        char more[64];
        std::snprintf(more, sizeof(more), " (first at 0x%08X)", first);
        report += more;
    }
    return differ == 0;
}

bool CheckPopArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate) {
    std::vector<uint8_t> ram;
    std::string err;
    if (!ReadRam(ramPath, ram, err)) {
        report = "poparenacheck: " + err;
        return false;
    }
    GuestRam snap(ram.data(), kArenaGp);
    int set = 0;
    int32_t raceId = -1;
    if (!ImageNamesRace(disc, snap, set, raceId, err)) {
        report = "poparenacheck " + ramPath + ": " + err;
        return false;
    }
    const int32_t last = snap.S32(kPieceLast);
    if (last < -1 || last >= kPieceSlots) {
        report = "poparenacheck " + ramPath + ": REFUSED - the piece list's last index is out of its six slots";
        return false;
    }
    // 1. The piece list, written by the session's own WritePieceList from the image's slot ids; each
    // resident object's address is its BTT_ record's +0x0C, found through the PORTED BttRecord.
    PieceSlot slots[kPieceSlots];
    std::vector<uint32_t> resident;
    for (int k = 0; k < kPieceSlots; ++k) {
        const int32_t id = snap.S32(kPieceList + 16u * static_cast<uint32_t>(k));
        slots[k].id = id;
        if (id == -1) continue;
        const uint32_t btt = rr::sim::RoadBttRecord(snap, id);
        if (btt == 0) {
            report = "poparenacheck " + ramPath + ": slot id " + std::to_string(id) + " has no BTT_ record";
            return false;
        }
        slots[k].object = mutate ? btt : snap.U32(btt + 12u);
        resident.push_back(static_cast<uint32_t>(id));
    }
    std::vector<uint8_t> work(GuestRam::kRamSize, 0);
    GuestRam w(work.data(), kArenaGp);
    WritePieceList(w, slots, last);
    w.W32(kArenaGp + rr::sim::kPopVolumeRadiusGp, kVolumeRadius);
    w.W32(rr::sim::kEffectPoolWalkPtr, rr::sim::kEffectPool);
    size_t compared = 0, differ = 0;
    uint32_t first = 0;
    auto cmp = [&](uint32_t at, uint32_t n) {
        for (uint32_t k = 0; k < n; ++k) {
            ++compared;
            if (snap.U8(at + k) != w.U8(at + k)) {
                if (differ == 0) first = at + k;
                ++differ;
            }
        }
    };
    cmp(kPieceList, 16u * kPieceSlots);
    cmp(kPieceLast, 4);
    cmp(kArenaGp + rr::sim::kPopVolumeRadiusGp, 4);
    cmp(rr::sim::kEffectPoolWalkPtr, 4);
    // The effect records as the session seeds them (all free, word 0 = 0x3F): byte 0 of every record the
    // image holds free (state bits 6..9 zero) - the link bits and the low state bits.
    size_t freeRecords = 0;
    for (uint32_t k = 0; k < 20u; ++k) {
        const uint32_t rec = rr::sim::kEffectPool + rr::sim::kEffectRecordBytes * k;
        w.W32(rec, 0x3Fu);
        if (((snap.U32(rec) >> 6) & 15u) != 0) continue;
        ++freeRecords;
        cmp(rec, 1);
    }
    // 2. OUR residency rule, against what the image holds: the objects whose residency window holds
    // the player's road coordinate must all be resident.
    const std::map<uint32_t, std::vector<uint8_t>> objects = ReadRoadObjects(disc, set);
    const uint32_t pb = snap.U32(kP1EntityPtr);
    const uint32_t key = snap.U32(pb + 0x168u);
    const int32_t along = snap.S32(pb + 0x170u) >> 16;
    size_t named = 0, namedResident = 0;
    std::string names;
    if ((key >> 16) == 0) {
        for (const auto& kv : objects) {
            const rr::ChunkHeader h = rr::ParseChunkHeader(kv.second);
            bool holds = false;
            for (const rr::ResidencyWindow& rw : h.windows)
                if (rw.road == (key & 0xFFFFu) && static_cast<int32_t>(rw.from) <= along &&
                    along <= static_cast<int32_t>(rw.to))
                    holds = true;
            if (!holds) continue;
            ++named;
            const bool in = std::find(resident.begin(), resident.end(), kv.first) != resident.end();
            if (in) ++namedResident;
            names += (names.empty() ? "" : ", ") + std::to_string(kv.first) + (in ? "" : " (NOT resident)");
        }
    }
    char b[700];
    std::snprintf(b, sizeof(b),
                  "poparenacheck %s%s: race %d of set %d; the piece list 0x800D4B10 (6 slots, last index %d, %zu "
                  "resident) rebuilt by WritePieceList, the last index, gp+204 = 0x%08X, 0x800D8068 = 0x%08X and "
                  "byte 0 of the %zu free effect record(s): %zu bytes compared, %zu differ%s; our residency rule on "
                  "the player's road %u at %d names %zu object(s) [%s], %zu of them resident in the image",
                  ramPath.c_str(), mutate ? " [MUTATED: slot +0x08 = the BTT_ record]" : "", raceId, set, last,
                  resident.size(), kVolumeRadius, rr::sim::kEffectPool, freeRecords, compared, differ,
                  differ ? "" : " - reproduced exactly", key & 0xFFFFu, along, named, names.c_str(), namedResident);
    report = b;
    if (differ != 0) {
        char more[64];
        std::snprintf(more, sizeof(more), " (first at 0x%08X)", first);
        report += more;
    }
    return differ == 0 && namedResident == named;
}

// ------------------------------------------------------------------------------- the seam objects
//
// Everything in this section is a callee the original has and this port does not. None of them
// pretends the call succeeded: each records itself in the session's seam list, which `rrgame`
// prints; its effects are absent.
void RaceSession::NoteSeam(const std::string& what) {
    if (std::find(seams_.begin(), seams_.end(), what) == seams_.end()) seams_.push_back(what);
}

uint32_t RaceSession::ArenaWord(uint32_t address) const {
    const uint8_t* p = At(address & ~3u);
    return static_cast<uint32_t>(ReadS32(p, 0));
}
uint16_t RaceSession::ArenaHalf(uint32_t address) const {
    const uint8_t* p = At(address & ~1u);
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

namespace {

// RASHCDG 0x80090270 RaceGo, PORTED (spine.h, row `race_go`): the countdown's "GO" gives every awake
// racer its first command's start delay +0x3C0 by grid place.
class GoEvent final : public rr::sim::RaceGoEvent {
public:
    GoEvent(RaceSession* s, GuestRam& g, FrameLog& log) : session_(s), g_(g), log_(log) {}
    void Go() override {
        rr::sim::RaceGo(g_);
        ++log_.raceGo;
        if (g_.Faulted()) {
            g_.ClearFault();
            session_->NoteSeam("RASHCDG 0x80090270 RaceGo (PORTED) met an address the console would fault on");
        }
    }

private:
    RaceSession* session_;
    GuestRam& g_;
    FrameLog& log_;
};

// `SLUS 0x80017BA0 PlaySound3D` - ported (sound.cpp, bench row `play_sound_3d`).
class EngineSoundOut final : public rr::sim::EngineSound {
public:
    explicit EngineSoundOut(SoundRuntime* s) : sounds_(s) {}
    void PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        ++calls;
        sounds_->PlaySound3D(x, z, id, bank);
    }
    size_t calls = 0;

private:
    SoundRuntime* sounds_;
};

// SLUS 0x80027540 CrashEmit, PORTED (spine.h, row `crash_emit`). It reads the part pair of the
// crashing bike out of its model record (+0x00 and the part list at +0x04), which BuildGrid and the
// model loader fill and this session does not (both 0): the call is then REFUSED here rather than
// run on the zero page. A call that faults anyway leaves the bike and the effect pool as they were.
class CrashEmitPort final : public rr::sim::CrashEmitter {
public:
    CrashEmitPort(RaceSession* s, GuestRam& g, uint32_t e, FrameLog& log) : session_(s), g_(g), e_(e), log_(log) {}
    void Emit(int32_t which, int32_t kind) override {
        ++log_.crashEmits;
        if (g_.U32(e_ + 0x00u) == 0 || g_.U32(e_ + 0x04u) == 0) {
            ++log_.crashEmitsDeclined;
            session_->NoteSeam("SLUS 0x80027540 CrashEmit (PORTED) was not run: the bike's model record +0x00 and "
                               "part list +0x04 are 0 (BuildGrid RASHCDI 0x80067B00 and the model loader are not "
                               "ported), and the emitter reads its part pair through them");
            return;
        }
        {   // The emitter reads its part pair through the LAST part slot's DPD3 pointer (+0 of the slot)
            // and that record's polygon list (+0x14). The product's part slots (rider_pose.h) carry the
            // pose's 3x3 but a DPD3 pointer of 0 (the model loader is not ported): refused, not run on
            // the kernel area's zeroes.
            const uint32_t parts = g_.U32(e_ + 0x04u);
            const uint32_t count = g_.U16(g_.U32(e_ + 0x00u) + 0x18u);
            const uint32_t dpd3 = count != 0 ? g_.U32(parts + 24u * count - 24u) : 0u;
            if (dpd3 < 0x80000000u || g_.U32(dpd3 + 0x14u) < 0x80000000u) {
                ++log_.crashEmitsDeclined;
                session_->NoteSeam("SLUS 0x80027540 CrashEmit (PORTED) was not run: the bike's last part slot "
                                   "carries no DPD3 record (the model loader is not ported), and the emitter reads "
                                   "its part pair through that record's polygon list");
                return;
            }
        }
        uint8_t bike[kEntitySize];
        std::vector<uint8_t> pool(20u * rr::sim::kEffectRecordBytes);
        g_.ReadBlock(e_, bike, kEntitySize);
        g_.ReadBlock(rr::sim::kEffectPool, pool.data(), static_cast<uint32_t>(pool.size()));
        rr::sim::CrashEmit(g_, e_, static_cast<uint32_t>(which), static_cast<uint32_t>(kind));
        if (g_.Faulted()) {
            g_.ClearFault();
            g_.WriteBlock(e_, bike, kEntitySize);
            g_.WriteBlock(rr::sim::kEffectPool, pool.data(), static_cast<uint32_t>(pool.size()));
            ++log_.crashEmitsDeclined;
            session_->NoteSeam("SLUS 0x80027540 CrashEmit (PORTED) met an address the console would fault on; "
                               "the bike and the effect records were restored");
        }
    }

private:
    RaceSession* session_;
    GuestRam& g_;
    uint32_t e_;
    FrameLog& log_;
};

// The pose side of the rider animation machine and everything the rider layer calls that is not
// ported - the product's `RiderSeams`:
//   * TransitionCapture 0x8005CB70 and ApplyFrame 0x8005D2A8 are the PORTED pose;
//   * the two combat children (return 3 / 0, the recipe's) and the eight callees of the knock-off
//     and the launch: each named, its effects absent, v0 0.
class ProductRiderSeams final : public rr::sim::RiderSeams {
public:
    ProductRiderSeams(RaceSession* s, GuestRam& g) : session_(s), g_(g) {}
    // The pose side is PORTED (src\game\sim\pose.h, rows_pose.inc): the riders'
    // channels, part slots and root words are the console's.
    uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t blend) override {
        rr::sim::AnimPose pose(g_);
        const uint32_t v = pose.TransitionCapture(a, op, blend);
        if (pose.Failed() && !g_.Faulted())
            session_->NoteSeam("RASHCDG 0x8005CB70 TransitionCapture (PORTED) refused: QuatToMatrix would trap");
        return v;
    }
    uint32_t ApplyFrame(uint32_t a) override {
        rr::sim::AnimPose pose(g_);
        const uint32_t v = pose.ApplyFrame(a);
        if (pose.Failed() && !g_.Faulted())
            session_->NoteSeam("RASHCDG 0x8005D2A8 ApplyFrame (PORTED) refused: QuatToMatrix would trap");
        return v;
    }
    uint32_t CombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return session_->FightCombatLeave(r, cur, ev, p, *this); // RASHCDG 0x800BFD24, PORTED (fight_session.cpp)
    }
    uint32_t CombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return session_->FightCombatEnter(r, cur, ev, p, *this); // RASHCDG 0x800BFC5C, PORTED (fight_session.cpp)
    }
    // RASHCDG 0x800BF51C / SLUS 0x80018440, PORTED (takedown_product.h; RRJB_TAKEDOWN=off: named seams)
    uint32_t Takedown(uint32_t b) override { return ProductTakedown(g_, b, Note()); }
    uint32_t RiderOffSound(uint32_t h, uint32_t m) override { return ProductRiderOffSound(session_->Sounds(), h, m, Note()); }
    uint32_t Arrest(uint32_t cop, uint32_t b, uint32_t how) override { // RASHCDG 0x80096F30, PORTED (race_modes.cpp)
        session_->ModeArrest(cop, b, how, 0);
        return 0;
    }
    // RiderLaunch's pose initialisers, RiderSync, LaunchLift and Settle: PORTED (recover_fall.h).
    uint32_t PoseInit(uint32_t fn, uint32_t r, uint32_t a1) override { return Recover(fn, {r, a1}); }
    uint32_t RiderSync(uint32_t r) override { return Recover(0x8008DF74u, {r}); }
    uint32_t CrashEvent(uint32_t h, uint32_t kind) override { // SLUS 0x8001A760 RiderSpeech, PORTED (speech_session.cpp)
        session_->RiderSpeech(h, static_cast<int32_t>(kind), 0, this);
        return 0;
    }
    uint32_t LaunchLift(uint32_t d, uint32_t sp, uint32_t c, uint32_t k) override { return Recover(0x8007E868u, {d, sp, c, k}); }
    uint32_t Settle(uint32_t r) override { return Recover(0x8009246Cu, {r}); }

private:
    uint32_t Recover(uint32_t fn, std::initializer_list<uint32_t> a) {
        uint32_t v0 = 0;
        session_->RecoverCall(fn, a, kRecoverSeamSp, *this, &v0);
        return v0;
    }
    uint32_t Seam(const std::string& what) {
        session_->NoteSeam(what + " is not ported: its effects are absent");
        return 0;
    }
    TakedownNote Note() { return [this](const std::string& s) { session_->NoteSeam(s); }; }
    RaceSession* session_;
    GuestRam& g_;
};

// The stance event for the AI's command machine (AiPushCommand / AiPopCommand's trigger): the
// PORTED StanceLayer, `p` = 2 as the AI's call sites pass it.
class StanceSink final : public rr::sim::AiStanceSink {
public:
    StanceSink(GuestRam& g, ProductRiderSeams& seams, FrameLog& log) : g_(g), seams_(seams), log_(log) {}
    void PlayIdleStance(uint16_t event, uint32_t rider) override {
        const uint16_t before = g_.U16(rider + 0x220u);
        rr::sim::StanceLayer layer(g_, seams_);
        layer.Event(event, rider, 2);
        ++log_.stanceEvents;
        if (g_.U16(rider + 0x220u) != before) ++log_.stanceChanged;
    }

private:
    GuestRam& g_;
    ProductRiderSeams& seams_;
    FrameLog& log_;
};

// The pieces StampResult RASHCDG 0x800BC7CC reaches, all PORTED: the stance event (stance.h), the
// AI command stack's clear and push (ai.h) on raw views of the arena.
struct ProductStampCallees final : rr::sim::StampResultCallees {
    GuestRam& g;
    uint8_t* ram;
    ProductRiderSeams& seams;
    FrameLog& log;
    ProductStampCallees(GuestRam& gg, uint8_t* r, ProductRiderSeams& s, FrameLog& l) : g(gg), ram(r), seams(s), log(l) {}
    bool StanceEvent(uint32_t ev, uint32_t rider, uint32_t p) override {
        const uint16_t before = g.U16(rider + 0x220u);
        rr::sim::StanceLayer layer(g, seams);
        layer.Event(ev, rider, p);
        ++log.stanceEvents;
        if (g.U16(rider + 0x220u) != before) ++log.stanceChanged;
        return !layer.Failed() && !g.Faulted();
    }
    bool ClearCommands(uint32_t e) override {
        uint8_t* ep = RawAt(ram, e, 1096);
        if (ep == nullptr) return false;
        rr::sim::AiClearCommands(ep);
        return true;
    }
    bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t e) override {
        uint8_t cmd[8] = {static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), static_cast<uint8_t>(target),
                          static_cast<uint8_t>(target >> 8), 0, 0, 0, 0};
        return PushBytes(cmd, mode, e);
    }
    bool PushBytes(uint8_t cmd[8], int32_t mode, uint32_t e) {
        static const std::vector<uint8_t> noFight(12u * 256u, 0);
        uint8_t* ep = RawAt(ram, e, 1096);
        const uint32_t R = ep != nullptr ? g.U32(e + 0x354u) : 0u;
        uint8_t* rider = RawAt(ram, R, 640);
        if (ep == nullptr || rider == nullptr) return false;
        StanceSink sink(g, seams, log);
        rr::sim::AiPushEnv pe;
        pe.raceClock = g.S32(g.U32(kGameStatePtr) + 0x10u);
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = RawAt(ram, kAltKindTable, 8u * 256u);
        const uint32_t fp = g.U32(kFightTablePtr); // FIGHT.BIN (fight_session.cpp LoadFightTable)
        pe.fightRecords = fp != 0u ? RawAt(ram, fp, 12u * 64u) : noFight.data();
        pe.stance = &sink;
        rr::sim::AiPushCommand(cmd, mode, ep, pe);
        return !g.Faulted();
    }
};

// A refusal made by the product on behalf of an unported callee: named in the seam list, counted,
// and the address kept so the log can say which function stopped the transition.
struct Refusals {
    RaceSession* session = nullptr;
    FrameLog* log = nullptr;
    uint32_t last = 0;
    bool Refuse(uint32_t address, const char* what) {
        last = address;
        if (log != nullptr) log->lastRefusal = address;
        char b[300];
        std::snprintf(b, sizeof(b),
                      "%s 0x%08X %s is not ported: a population transition that reaches it is REFUSED and the "
                      "arena restored to what it was before that bike's pass",
                      address < 0x8005B5E8u ? "SLUS" : "RASHCDG", address, what);
        session->NoteSeam(b);
        return false;
    }
};

// The rider dismount RASHCDG 0x800C3104 (PORTED, traffic.h) on the arena: its three callees are the
// PORTED stance layer (the event through the stamp callees, so it is counted like every other).
struct ProductDismount final : rr::sim::DismountCallees {
    GuestRam& g;
    ProductStampCallees& stamp;
    ProductDismount(GuestRam& gg, ProductStampCallees& s) : g(gg), stamp(s) {}
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override { return stamp.StanceEvent(ev, r, p); }
    bool StanceLeave(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override {
        rr::sim::StanceLayer layer(g, stamp.seams);
        layer.Leave(ev, r, p);
        return !layer.Failed() && !g.Faulted();
    }
    bool AnimStop(uint32_t a, uint32_t) override {
        rr::sim::StanceLayer layer(g, stamp.seams);
        layer.anim().Stop(a);
        return !layer.Failed() && !g.Faulted();
    }
    bool Run(uint32_t r, int32_t how, uint32_t sp) { return rr::sim::RiderDismount(g, r, how, sp, *this) && !g.Faulted(); }
};

// The remount RASHCDG 0x800903F4 (PORTED, crash.h) as the population passes reach it: its ported
// callees native, the unported ones refused.
struct ProductRemountCallees final : rr::sim::RemountCallees {
    GuestRam& g;
    ProductStampCallees& stamp;
    Refusals& refuse;
    const rr::sim::BikeTables& t;
    ProductRemountCallees(GuestRam& gg, ProductStampCallees& s, Refusals& r, const rr::sim::BikeTables& tt)
        : g(gg), stamp(s), refuse(r), t(tt) {}
    bool RowsFromHeading(uint32_t B) override { // PORTED (traffic.h)
        rr::sim::RowsFromHeading(g, B);
        return !g.Faulted();
    }
    bool ResetBikeState(uint32_t B) override { return rr::sim::ResetBikeState(g, B) && !g.Faulted(); }
    bool ReleaseRiderObject(uint32_t R) override { // PORTED (traffic_bind.h)
        return rr::sim::ReleaseRiderObject(g, R) && !g.Faulted();
    }
    bool Attach(uint32_t B, uint32_t R, int32_t kind, int32_t seat) override {
        rr::sim::Attach(g, B, R, kind, seat);
        return !g.Faulted();
    }
    bool Dismount(uint32_t R, int32_t how) override { // PORTED (traffic.h)
        ProductDismount d(g, stamp);
        return d.Run(R, how, 0);
    }
    bool ClearCommands(uint32_t B) override { return stamp.ClearCommands(B); }
    bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t B) override {
        return stamp.PushCommand(op, target, mode, B);
    }
    bool ReFace(uint32_t B) override { return refuse.session->RecoverCall(0x80096564u, {B}, kRecoverSeamSp, stamp.seams); }
    bool ViewReset(uint32_t view) override { // CameraSpringReset 0x80086AF8, PORTED (camera.h)
        struct NoSeams final : rr::sim::CameraSeams {
            bool GetRCnt(uint32_t, uint32_t&) override { return false; }
            bool ShotSetup(GuestRam&, uint32_t, uint32_t) override { return false; }
            bool SplineSlopes(GuestRam&, uint32_t, uint32_t, uint32_t, int32_t) override { return false; }
            bool RaceOverSignal(uint32_t) override { return false; }
        } none;
        rr::sim::RoadRuntimeNative road;
        rr::sim::CameraPort port(g, t, none, road);
        port.SpringReset(view);
        return !port.Failed();
    }
    bool PlayerVoice(uint32_t h, int32_t a1) override { // a named seam (no effect): recover_race.h
        return refuse.session->RecoverCall(0x80018440u, {h, static_cast<uint32_t>(a1)}, kRecoverSeamSp, stamp.seams);
    }
    bool PlayerBind(uint32_t h, uint32_t B) override { // CamTarget SLUS 0x800235B0, native (recover_race.h)
        return refuse.session->RecoverCall(0x800235B0u, {h, B}, kRecoverSeamSp, stamp.seams);
    }
};

// rr::sim::PopulationCallees over the arena: the ported callees
// as the bench runs them, the unported ones REFUSED (and the caller restores the arena).
struct ProductPopCallees final : rr::sim::PopulationCallees {
    RaceSession* session;
    GuestRam& g;
    uint8_t* ram;
    ProductRiderSeams& seams;
    ProductStampCallees& stamp;
    Refusals& refuse;
    const rr::sim::BikeTables& t;
    std::function<int32_t(uint32_t, int32_t)> targetSpeed;
    rr::sim::RoadRuntimeNative road;
    ProductPopCallees(RaceSession* s, GuestRam& gg, uint8_t* r, ProductRiderSeams& se, ProductStampCallees& st,
                      Refusals& rf, const rr::sim::BikeTables& tt)
        : session(s), g(gg), ram(r), seams(se), stamp(st), refuse(rf), t(tt) {}

    bool ResetBike(uint32_t e, uint32_t) override { return rr::sim::ResetBikeState(g, e) && !g.Faulted(); }
    bool CopLeave(uint32_t e, uint32_t) override { // PORTED (traffic_bind.h)
        rr::sim::CopLeave(g, e);
        return !g.Faulted();
    }
    bool CopJoin(uint32_t e, uint32_t) override { // PORTED (traffic_bind.h)
        rr::sim::CopJoin(g, e);
        return !g.Faulted();
    }
    bool AxisRotation(uint32_t axis, int32_t ang, uint32_t out, uint32_t sp) override { // PORTED (recover_walk.h)
        rr::sim::AxisRotation(g, axis, ang, out, sp, t);
        return !g.Faulted();
    }
    bool RowsFromHeading(uint32_t e, uint32_t) override { // PORTED (traffic.h)
        rr::sim::RowsFromHeading(g, e);
        return !g.Faulted();
    }
    bool RiderDismount(uint32_t r, int32_t how, uint32_t sp) override { // PORTED (traffic.h)
        ProductDismount d(g, stamp);
        return d.Run(r, how, sp);
    }
    bool Remount(uint32_t b, int32_t fromRoad, uint32_t) override {
        ProductRemountCallees rc(g, stamp, refuse, t);
        return rr::sim::Remount(g, b, fromRoad, rc) && !g.Faulted();
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override { return stamp.StanceEvent(ev, r, p); }
    bool BankSwitch(uint32_t a, uint32_t bank, uint32_t) override {
        rr::sim::StanceLayer layer(g, seams);
        layer.anim().BankSwitch(a, bank);
        return !layer.Failed() && !g.Faulted();
    }
    bool SeatRelease(uint32_t b, uint32_t r, uint32_t idx, uint32_t) override {
        rr::sim::StanceLayer layer(g, seams);
        layer.anim().SeatRelease(b, r, idx);
        return !layer.Failed() && !g.Faulted();
    }
    bool BuildObb(uint32_t e) override {
        uint8_t* ep = RawAt(ram, e, 1096);
        if (ep == nullptr) return false;
        rr::sim::EntityView v(ep);
        int32_t rev = 0;
        if ((g.U16(e + 0xACu) >> 5) == 0) {
            const uint32_t owner = v.U32(ent::kOwner);
            rev = owner != 0 ? g.S32(owner + 604u) : 0;
        }
        rr::sim::BuildObb(v, rev);
        return true;
    }
    bool ClearCommands(uint32_t e, uint32_t) override { return stamp.ClearCommands(e); }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t) override {
        uint8_t bytes[8];
        g.ReadBlock(cmd, bytes, 8);
        const bool ok = stamp.PushBytes(bytes, mode, e);
        g.WriteBlock(cmd, bytes, 8);
        return ok;
    }
    bool TargetSpeed(uint32_t e, int32_t dt, uint32_t, int32_t& v0) override {
        v0 = targetSpeed(e, dt);
        return !g.Faulted();
    }
    // The spawner's and the traffic's: all PORTED but the cell walker.
    bool CellWalker(uint32_t sp) override { // RASHCDG 0x8009C308, PORTED (world_pop_product.h)
        rr::game::WorldProductCallees wc(g, *this, t);
        wc.note = [this](const std::string& n) { session->NoteSeam(n); };
        if (g.U32(rr::sim::kPedSwitch) != 0) // the pedestrians: the PORTED spawner 0x800CB8C8 (peds.h)
            wc.pedSpawn = [this](const uint32_t* a, uint32_t sp2, uint32_t& v0) {
                rr::game::PedTotals& pt = rr::game::PedRunTotals();
                ++pt.asked;
                const bool ok = session->RecoverCall(rr::sim::kPedSpawnFn, {a[0], a[1], a[2]}, sp2, seams, &v0);
                if (!ok) ++pt.spawnRefused;
                else if (v0 != 0) ++pt.spawned;
                return ok;
            };
        return rr::game::RunCellWalker(g, sp, wc);
    }
    bool PoliceSched(int32_t acc, uint32_t sp) override;
    bool Budget(int32_t kind, uint32_t, uint32_t& v0) override {
        v0 = rr::sim::Budget(g, static_cast<uint32_t>(kind));
        return !g.Faulted();
    }
    bool ShareFlags(uint32_t flags, uint32_t sp) override {
        rr::sim::ShareFlags(g, flags, sp);
        return !g.Faulted();
    }
    bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) override {
        rr::sim::RoadWalk(g, from, out, dist, sp);
        return !g.Faulted();
    }
    bool Spacing(uint32_t rec, uint32_t, uint32_t& v0) override {
        v0 = rr::sim::Spacing(g, rec);
        return !g.Faulted();
    }
    bool Release(uint32_t h, int32_t pool, uint32_t) override { return rr::sim::PoolRelease(g, h, pool) && !g.Faulted(); }
    bool ModelBind(uint32_t car, int32_t pool, uint32_t cls, uint32_t, uint32_t& v0) override {
        return rr::sim::ModelBind(g, car, pool, cls, 0, v0) && !g.Faulted();
    }
    bool CarSetup(uint32_t car, uint32_t sp) override {
        rr::sim::CarSetup(g, car, 0, sp);
        return !g.Faulted();
    }
    bool NodeLanes(uint32_t obj, int32_t roadId, uint32_t dirOut, uint32_t, uint32_t& v0) override {
        v0 = rr::sim::NodeLanes(g, obj, roadId, dirOut);
        return !g.Faulted();
    }
    rr::sim::RoadRuntimeCallees& Road() override { return road; }
};

// The police scheduler's two seam callees, both PORTED (traffic_leaves.h).
struct ProductPoliceCallees final : rr::sim::PoliceCallees {
    GuestRam& g;
    explicit ProductPoliceCallees(GuestRam& gg) : g(gg) {}
    bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) override {
        rr::sim::RoadWalk(g, from, out, dist, sp);
        return !g.Faulted();
    }
    bool RoadLength(uint32_t cursor, uint32_t, uint32_t& v0) override {
        v0 = static_cast<uint32_t>(rr::sim::CursorRoadEnd(g, cursor));
        return !g.Faulted();
    }
};
bool ProductPopCallees::PoliceSched(int32_t acc, uint32_t sp) {
    ProductPoliceCallees pc(g);
    return rr::sim::PoliceSched(g, acc, sp, pc, *this) && !g.Faulted();
}

// The traffic pass's callees (traffic_drive.h): the population's, the road end, the ported emitter.
struct ProductDriveCallees final : rr::sim::TrafficDriveCallees {
    GuestRam& g;
    rr::sim::PopulationCallees& pop;
    std::function<void(int32_t, int32_t, int32_t, int32_t)> sound;
    ProductDriveCallees(GuestRam& gg, rr::sim::PopulationCallees& p) : g(gg), pop(p) {}
    rr::sim::PopulationCallees& Population() override { return pop; }
    bool RoadLength(uint32_t cursor, uint32_t, uint32_t& v0) override {
        v0 = static_cast<uint32_t>(rr::sim::CursorRoadEnd(g, cursor));
        return !g.Faulted();
    }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank, uint32_t) override {
        if (sound) sound(x, z, id, bank);
        return true;
    }
};

// The camera's four unported callees (camera.h CameraSeams): GetRCnt answered by a counter of ours
// (it drives only the off-road shake), the three director callees refused.
struct ProductCameraSeams final : rr::sim::CameraSeams {
    RaceSession* session;
    uint32_t& counter;
    ProductCameraSeams(RaceSession* s, uint32_t& c) : session(s), counter(c) {}
    bool GetRCnt(uint32_t, uint32_t& value) override {
        session->NoteSeam("the camera's GetRCnt SLUS 0x80043F00 (root counter 2, the off-road shake) is answered by "
                          "a deterministic counter of ours: the hardware timer is not modelled");
        counter = counter * 1103515245u + 12345u;
        value = (counter >> 16) & 0xFFFFu;
        return true;
    }
    bool ShotSetup(GuestRam& g, uint32_t v, uint32_t script) override { // PORTED (camera_director.h)
        return rr::sim::ShotSetup(g, v, script);
    }
    bool SplineSlopes(GuestRam& g, uint32_t x, uint32_t y, uint32_t out, int32_t n) override {
        // PORTED (camera_director.h); ViewUpdate (frame 208) calls it from RaceStep's camera slot
        return rr::sim::SplineSlopes(g, x, y, out, n, kArenaStepSp - kRaceStepFrame - 208u);
    }
    bool RaceOverSignal(uint32_t) override {
        // SLUS 0x80018C1C(0) restores the sound volumes its (1) call saved for the pause (0x800D6C00.. <-
        // 0x800D6C20.., the voices re-pitched): PORTED (sim/race_over.h), run on the sound world.
        rr::game::ProductRaceOverSignal(session->Sounds(), 0, [this](const std::string& s) { session->NoteSeam(s); },
                                        "SLUS 0x80018C1C(0) (the director's end: the audio un-pause) is not run - "
                                        "RRJB_STRIKE=off; the camera frame goes on");
        return true;
    }
};

struct SessionFinishCalls final : rr::sim::FinishTestCalls {
    RaceSession* session = nullptr;
    std::function<bool(int32_t*)> place;
    int32_t MilestoneAdvance() override { return session->ModeMilestone(); } // 0x800C8D4C, PORTED (race_modes.cpp)
    void MilestoneFirst() override { session->ModeMilestoneFirst(); }         // 0x800C92F8, PORTED (race_modes.cpp)
    std::function<void()> stampResult; // RASHCDG 0x800BC7CC, PORTED (spine.h)
    void StampResult() override { stampResult(); }
    std::function<void(uint32_t, int32_t)> recordFinish;
    void RecordFinish(uint32_t handle, int32_t flag) override { recordFinish(handle, flag); }
    int32_t RoadAdvance() override {
        session->NoteSeam("SLUS 0x800394F0 the road-graph step is not ported");
        return 0;
    }
    bool ComputePlaceMode1(int32_t* p) override { return place(p); }
};

struct FinishAdapter final : rr::sim::RaceFinishTest {
    std::function<int32_t(int32_t)> fn;
    size_t calls = 0;
    size_t declined = 0;
    int32_t Test(int32_t index) override {
        ++calls;
        return fn(index);
    }
};

} // namespace

// ------------------------------------------------------------------------------------ the session

RaceSession::~RaceSession() { sounds_.SetEffectTarget(nullptr); }

// RoadNote's two effect spawners, PORTED (spine.h EffectBurst SLUS 0x80027778, EffectSpray
// 0x80027974), on player 0's bike in THIS arena, where the effect records 0x800D39B0 and the bike's
// budget word +0x24 live. Root counter 2, which their jitter reads, is a counter of ours. Nothing
// draws the records (the effect renderer is not ported) and nothing ages or frees them (the pass
// that does is not identified): the pool fills and then refuses further spawns, as the original's
// would with its effect pass stopped.
class RaceSession::EffectSpawner final : public SoundEffectTarget {
public:
    explicit EffectSpawner(RaceSession& s) : s_(s) {}
    void Burst(uint32_t bike, int32_t kind, int32_t life, int32_t tag) override { // bike 0: player 0's (own-arena mode)
        GuestRam g(s_.arena_.Ram(), kArenaGp);
        const rr::sim::SpineIo io = Io();
        rr::sim::EffectBurst(g, bike != 0u ? bike : kArenaPool0, static_cast<uint32_t>(kind), static_cast<uint32_t>(life),
                             static_cast<uint32_t>(tag), io);
        ++s_.log_.bursts;
        if (bike != 0u) ++rr::game::MpSoundCounters().p2Bursts;
        Done(g, "SLUS 0x80027778 EffectBurst");
    }
    void Spray(uint32_t bike, int32_t kind) override {
        GuestRam g(s_.arena_.Ram(), kArenaGp);
        const rr::sim::SpineIo io = Io();
        rr::sim::EffectSpray(g, bike != 0u ? bike : kArenaPool0, static_cast<uint32_t>(kind), io);
        ++s_.log_.sprays;
        if (bike != 0u) ++rr::game::MpSoundCounters().p2Sprays;
        Done(g, "SLUS 0x80027974 EffectSpray");
    }

private:
    rr::sim::SpineIo Io() {
        s_.fxCounter_ = s_.fxCounter_ * 1103515245u + 12345u;
        rr::sim::SpineIo io;
        io.rootCounter = (s_.fxCounter_ >> 16) & 0xFFFFu;
        s_.NoteSeam("RoadNote's effect spawners run PORTED (SLUS 0x80027778 / 0x80027974); the root counter 2 "
                    "their jitter reads is a counter of ours; the PORTED effect pass that draws, ages and frees "
                    "the records 0x800D39B0 is run by the host after the frame (fx_runtime.h)");
        return io;
    }
    void Done(GuestRam& g, const char* what) {
        if (!g.Faulted()) return;
        g.ClearFault();
        s_.NoteSeam(std::string(what) + " (PORTED) met an address the console would fault on");
    }
    RaceSession& s_;
};

RaceSession::RaceSession(const DiscImage& disc, const RaceWorld& world, int opponents,
                         double startDistance, int players)
    : players_(players == 2 ? 2 : 1), world_(world), startDistance_(startDistance) {
    if (world_.path.size() < 2) throw std::runtime_error("this race assembled no drivable road");
    tables_.Load(disc);
    // The arena starts as the console does: the executable and the race overlay at their load
    // addresses. Everything below is written on top.
    LoadImages();
    streamPorted_ = rr::game::StreamPorted(); // the streamer (stream_session.cpp); RRJB_STREAM=ours: the session's rules
    routePorted_ = streamPorted_ && rr::game::RoutePorted(); // the route loader (route_product.h); RRJB_ROUTE=ours: the control

    countdown_ = Word(0x8005B230);
    eventAcc_ = Word(0x8005B30C);
    planCount_ = Word(0x8005B2A8);
    frameFlag_ = Word(0x8005B580);
    liveBikes_ = Word(kPool0Live);
    bikeCap_ = Word(0x8005B1FC);
    spreadDiv_ = Word(0x8005B244);
    postLimit_ = Word(0x8005B228);
    skipResults_ = Word(0x8005B220);
    clockStamp_ = Word(0x800CCA84);
    spreadBand_ = Word(0x800CCA88);
    timeLimit_ = Word(0x8005ACC8);
    timeBase_ = Word(0x8005ACD0);
    startDir_ = Word(0x8005B2E8);
    randSeed_ = reinterpret_cast<uint32_t*>(Word(kArenaGp + rr::sim::kRandSeedGp));
    // The race globals the loader clears or sets: ours, in the values it
    // leaves. The countdown and its limit are the director's.
    for (int32_t* w : {countdown_, eventAcc_, planCount_, frameFlag_, skipResults_, clockStamp_, spreadBand_,
                       timeLimit_, timeBase_, startDir_})
        *w = 0;
    *postLimit_ = 5 << 16; // *(0x8005B228), 5.00 s in 16.16 (every capture)
    // BuildRace RASHCDI 0x8006982C (0x80069994..0x800699C8): SeedRand((3305 * (GetRCnt(2) & 0xFF)) >> 6) before
    // BuildGrid - a live run reads the console's counter now (root_counter.h); a scripted run keeps 0x12345678.
    if (LiveRootCounter()) {
        const uint32_t t = ConsoleRootCounter2() & 0xFFu;
        *randSeed_ = static_cast<uint32_t>(static_cast<int32_t>(3305u * t) >> 6);
        NoteSeam("the LCG seed gp+2076 is BuildRace's: (3305 * (root counter 2 & 0xFF)) >> 6 = " +
                 std::to_string(*randSeed_) + " (the console's counter at this moment, root_counter.h)");
    } else {
        *randSeed_ = 0x12345678u;
        NoteSeam("the LCG seed gp+2076 starts at 0x12345678 in a scripted run (a live run takes BuildRace RASHCDI "
                 "0x8006982C's (3305 * (root counter 2 & 0xFF)) >> 6)");
    }
    // The grid (grid_session.cpp): the original's BuildGrid unless RRJB_GRID=ours or a --start field.
    {
        const char* gv = std::getenv("RRJB_GRID");
        gridPorted_ = !(gv != nullptr && std::strcmp(gv, "ours") == 0) && startDistance_ <= 0.0;
    }
    rr::game::Loader2Totals().gridPlanned = gridPorted_ && rr::game::Loader2On(); // loader2: BuildRace PORTED

    gameState_ = ArenaBytes{At(kArenaGameState), kGameStateBytes};
    std::memset(gameState_.p, 0, kGameStateBytes);
    gameState_[0x30] = 1; // one player, which is also what picks road set 1
    gameState_[0x34] = 1;
    reinterpret_cast<uint32_t*>(Word(kGameStatePtr))[0] = kArenaGameState;
    NoteSeam("game_state 0x800D5D38 is ours where the frontend would fill it: one player, race type 0, "
             "bank 0 (the frontend is not ported)");
    // A race the front end started (rrgame without --race, src\game\shell\handover.h): the fields the
    // PORTED commit RASHCDF 0x8007F37C wrote - race type +0x04..+0x0B, the option and environment
    // bytes +0x38..+0x3B, the bank +0x3C, the race id +0x40, +0x44 and the bikes +0x48/+0x4C - and
    // the session and player records 0x800D80D8..0x800D82F7 replace the defaults above.
    if (const rr::shell::Handover& fh = rr::shell::PendingHandover(); fh.active) {
        for (uint32_t o = 0x04; o < 0x0C; ++o) gameState_[o] = fh.gameState[o];
        for (uint32_t o = 0x38; o < 0x50 && o < kGameStateBytes; ++o) gameState_[o] = fh.gameState[o];
        std::memcpy(At(0x800D80D8u), fh.sessionAndPlayers, sizeof(fh.sessionAndPlayers));
        // ... and the twenty rider records 0x800D5758, where the commit copies the career identities (the
        // weapons +0x2C..+0x33); the loader keeps them for the records it does not reset (grid_loader.h).
        std::memcpy(At(kArenaRiderDefs), fh.riders, sizeof(fh.riders));
        // The resident SLUS data (handover.h): the jukebox's track flags MusicPickShuffle reads and the Time
        // Trial records the HUD and the results read, as the shell left them.
        size_t recordsEdited = 0; // the records' bytes the shell holds differently from the EXE's own table
        for (uint32_t i = 0; fh.resident && i < rr::shell::kResidentRecordBytes; ++i)
            if (*At(rr::shell::kResidentRecords + i) != fh.records[i]) ++recordsEdited;
        rr::shell::WriteResident(fh, arena_.Ram());
        if (fh.resident) {
            uint32_t on = 0, played = 0;
            for (uint32_t t = 0; t < rr::shell::kResidentAlbumTracks; ++t) {
                on |= (fh.albumFlags[t] & 1u) << t;
                played |= ((fh.albumFlags[t] >> 1) & 1u) << t;
            }
            char line[320];
            std::snprintf(line, sizeof(line),
                          "the resident SLUS data from the shell (handover.h): album flags 0x80053588 - tracks on 0x%05X, "
                          "played this cycle 0x%05X; the Time Trial records 0x80053A88 (1584 bytes, %zu differ from the "
                          "EXE's table)", on, played, recordsEdited);
            NoteSeam(line);
        }
        NoteSeam("game_state, the session/player records and the rider records are the front end's (the ported "
                 "commit 0x8007F37C)");
        if (players == 0 && fh.gameState[0x30] == 2) players_ = 2; // the commit's two-player modes 0x10/0x11/0x18
        player2Ph_ = fh.player2BikePh;
    } else {
        // No front end: the rider records hold what a console fresh from boot would - zero (OURS).
        std::memset(At(kArenaRiderDefs), 0, 20u * kRiderDefBytes);
        if (players_ == 2) gameState_[0x04] = 0x10; // a development two-player start: the head-to-head type
    }
    MpSetupGame(disc); // game_state+0x30, VIEWS.VI and the split layout (mp_session.cpp)

    // The race's grid block (grid_loader.h): the slot of each entry, in file order - the player the first
    // entry, the opponents the next racing entries (slot < 17). Without the file: slots 16, 1, 2, ...
    std::vector<rr::game::StartEntry> grid;
    {
        std::string why;
        if (!rr::game::ReadStartBlock(disc, world.set, world.raceId, grid, why)) NoteSeam(why);
    }
    // The race type's own grid (race_modes.h ModeGrid: Time Trial's entry count and police), and for a race
    // the front end started the original's field: every racing entry of the block (race_modes.cpp).
    if (const std::string m = rr::game::ModeGrid(gameState_.data(), grid); !m.empty()) NoteSeam(m);
    if (const std::string m = rr::game::TwoRiderGrid(gameState_.data(), players_, grid); !m.empty()) NoteSeam(m); // jail_session.h
    if (rr::shell::PendingHandover().active || gridPorted_) opponents = static_cast<int>(grid.size()); // the whole field
    std::vector<int32_t> racerSlots;
    std::vector<size_t> copEntries; // the police entries (slot >= 17), spawned after the racers
    // The players are the first grid entries (BuildGrid's flags: entry 0 player 1, entry 1 player 2);
    // the opponents the next racing entries.
    const size_t np = static_cast<size_t>(players_);
    for (size_t k = np; k < grid.size(); ++k) {
        if (grid[k].slot < 17) racerSlots.push_back(grid[k].slot);
        else copEntries.push_back(k);
    }
    int racerCap = 18 - players_ - static_cast<int>(copEntries.size());
    // A front-end race takes the block's own racing entries and no more: a racer past them would get an
    // OURS grid slot, whose AI index can land on a player's (two players: AI index 1 is player 2's).
    if (rr::shell::PendingHandover().active || gridPorted_) racerCap = std::min(racerCap, static_cast<int>(racerSlots.size()));
    const size_t racers = np + static_cast<size_t>(std::clamp(opponents, 0, racerCap));
    const size_t count = racers + copEntries.size();
    bikes_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        RaceBike& bike = bikes_[i];
        bike.entityAddress = ArenaEntity(i);
        bike.ownerAddress = kArenaPool1 + kRiderBytes * static_cast<uint32_t>(i);
        // provisional: the rider-record loader (below, grid_loader.h) moves it to its AI index's record
        bike.riderDefAddress = kArenaRiderDefs + kRiderDefBytes * static_cast<uint32_t>(i);
        bike.entity = ArenaBytes{At(bike.entityAddress), kEntitySize};
        bike.owner = ArenaBytes{At(bike.ownerAddress), kRiderBytes};
        bike.riderDef = ArenaBytes{At(bike.riderDefAddress), kRiderDefBytes};
        std::memset(bike.entity.p, 0, kEntitySize);
        std::memset(bike.owner.p, 0, kRiderBytes);
        bike.isPlayer = (i < np);
        bike.gridSlot = i < np ? (i < grid.size() ? grid[i].slot : static_cast<int32_t>(16 - i))
                               : (i - np < racerSlots.size() ? racerSlots[i - np] : static_cast<int32_t>(i));
        if (i >= racers) {
            bike.isCop = true;
            bike.gridSlot = grid[copEntries[i - racers]].slot;
            bike.gridAlong = grid[copEntries[i - racers]].along;
        }
        if (i < np && i < grid.size()) bike.gridAlong = grid[i].along;
    }
    if (!copEntries.empty() && !gridPorted_)
        NoteSeam("the grid's police entries (slot >= 17 in STARTDF" + std::string(world.set == 2 ? "B" : "A") +
                 ".BIN) are spawned DORMANT as the race loader spawns them (grid_loader.h: SpawnBike's police "
                 "stores, SaveCopRecord, the census, then SLUS 0x800119C0's Transition(e, 1) - the retirement); "
                 "OURS: their pool slots are the last, after this session's racers");
    *liveBikes_ = static_cast<int32_t>(count);
    *bikeCap_ = static_cast<int32_t>(racers); // *(0x8005B1FC): the players and the non-police (BuildGrid 0x800681C4)
    *spreadDiv_ = static_cast<int32_t>(count);
    std::memset(At(kFinishOrder), 0, kFinishOrderBytes);
    if (rr::game::LoaderPorted()) { // the ported loader (loader_product.h)
        GuestRam lg(arena_.Ram(), kArenaGp);
        const uint8_t state = gameState_[0x00], first = gameState_[0x03];
        // the rim / gtp files are transient: the object area before BuildArena fills it (OURS: where)
        NoteSeam(rr::game::LoaderEnterRace(lg, arena_.Ram(), disc, kArenaStepSp,
                                           arenaFreeFrom_ != 0 ? arenaFreeFrom_ : kArenaObjectsFrom, kArenaObjectsTo));
        if (!rr::game::PassesWhole()) { // RRJB_PASSES=session: the session's own frame ordering keeps its state bytes
            gameState_[0x00] = state;
            gameState_[0x03] = first;
        }
    } else if (rr::game::PassesWhole()) {
        // RASHCDI 0x8006A7C0 (TRANSCRIBED from our listing, the loader is not benched): the table's 18 entries
        // 0x800D5DA8 + 16 k = {-1, -1, 0, 0} - ResultsPrepare 0x8003F708 reads -1 as a free place.
        GuestRam fg(arena_.Ram(), kArenaGp);
        for (uint32_t k = 0; k < 18u; ++k) {
            fg.W32(kFinishOrder + 16u + 16u * k, 0xFFFFFFFFu);
            fg.W32(kFinishOrder + 20u + 16u * k, 0xFFFFFFFFu);
        }
        // EnterRace RASHCDI 0x80063B90's two stores (TRANSCRIBED; EnterRace is not ported): game_state+0x03 = 1,
        // the first race frame (0x80063BA4), and the state byte 3 (0x80063500). The PORTED pad poll clock
        // region turns the state to 1 on the first frame and main's first-race-frame arm runs (Frame).
        gameState_[0x03] = 1;
        gameState_[0x00] = 3;
    }
    std::memset(At(kJailbreakClock), 0, 4);
    std::memset(At(kAttackerMask), 0, 16);
    std::memset(At(kHandleTable), 0, 8u * 64u);
    std::memset(At(kPadRecords), 0, 192u * 4u);
    std::memset(At(kViewArray), 0, 2u * kViewStride);
    reinterpret_cast<uint32_t*>(Word(kFightTablePtr))[0] = 0;
    reinterpret_cast<uint32_t*>(Word(rr::sim::kStanceEventList))[0] = 0;
    NoteSeam("the pad reader SLUS 0x8001CB3C runs its PORTED rider-controls region for player 1 only (pad_product.h: "
             "the axes 0x800CE540 for a 0x73 pad, the +0x230 merge, the walk bits); its pause / multitap "
             "parts are not run" + std::string(rr::game::RumbleOn() ? " (its rumble regions: rumble_product.h)" : "; nor its rumble regions (RRJB_RUMBLE=off)"));
    if (!rr::game::Loader2Totals().gridPlanned) // loader2: AnimNoise RASHCDI 0x80063158 PORTED (SpawnBike's call)
        NoteSeam("*(0x8005B3E4) (ANIMNOIZ.DAT, the animation sound events) is 0: the animation sounds are not loaded");
    LoadFightTable(disc); // FIGHT.BIN and the pad record's control table (fight_session.cpp)
    MpSetupPads();        // ... and player 2's (mp_session.cpp)

    SetupGrid();
    JailPlan(); // the two-seat bikes' passenger slots (jail_session.cpp)
    {   // DATA\GLOBALS.BI as RASHCDI 0x80064610 scatters it (ai_globals.h; the AI tables read from here on).
        // The original loads it before the race loader 0x80063B90 (SLUS 0x800122F4 / 0x800122FC), whose
        // BuildGrid reads its class blocks (GridRiderAdjust 0x800650A0).
        std::vector<uint8_t> globals;
        if (const auto f = disc.Find("DATA/GLOBALS.BI")) globals = disc.ReadFile(*f);
        GuestRam g(arena_.Ram(), kArenaGp);
        std::string why;
        if (rr::sim::LoadAiGlobals(g, globals, why))
            NoteSeam("DATA\\GLOBALS.BI is scattered as RASHCDI 0x80064610 does (46 copies, profile row " +
                     std::to_string(rr::sim::AiProfileRow(g)) + "): the AI weights, class blocks, speed tables "
                     "and the rubber band profile are the file's, read from the arena");
        else
            NoteSeam("DATA\\GLOBALS.BI was NOT loaded (" + why + "): the AI tables are the executable image's");
    }
    if (!gridPorted_) {   // the rider records and the AI index maps from LEVEL<bank+1>.BI, as BuildGrid leaves them (grid_loader.h)
        const std::string name = "DATA/LEVEL" + std::to_string(static_cast<int32_t>(gameState_[0x3C]) + 1) + ".BI";
        std::vector<uint8_t> bi;
        if (const auto f = disc.Find(name)) bi = disc.ReadFile(*f);
        std::vector<rr::game::RiderSeat> seats;
        for (const RaceBike& b : bikes_) {
            rr::game::RiderSeat st;
            st.entity = b.entityAddress;
            st.slot = b.gridSlot;
            st.player = b.isPlayer ? static_cast<int>(&b - bikes_.data()) : -1; // player p = pool-0 slot p
            seats.push_back(st);
        }
        GuestRam cg(arena_.Ram(), kArenaGp);
        std::string note;
        rr::game::LoadRiderRecords(cg, bi, kRiderBiAt, seats, note);
        NoteSeam(name + ": " + note);
        { std::vector<uint32_t> rs; for (const RaceBike& b : bikes_) rs.push_back(b.ownerAddress); NoteSeam(rr::game::SetRiderBoxes(cg, disc, gameState_[0x3C], rs)); } // rider_model.h
        for (size_t i = 0; i < bikes_.size(); ++i) {
            RaceBike& b = bikes_[i];
            b.riderDefAddress = cg.U32(b.entityAddress + ent::kRiderDef);
            b.riderDef = ArenaBytes{At(b.riderDefAddress), kRiderDefBytes};
            // the initial place: BuildGrid takes it from ComputePlace 0x800138E8 (0x8006822C) before the
            // cursors are seated; OURS: the grid order, which the place pass replaces from the first frame
            b.riderDef[0x27] = static_cast<uint8_t>(i + 1);
            // the model init SLUS 0x8002FAD4 SpawnBike runs on the bike (0x80066314) replaces the loader's
            // +0x4C with 1.0 (0x8003006C) - not ported for bikes; the value every race capture holds
            cg.W32(b.entityAddress + 0x4Cu, 0x10000u);
        }
        NoteSeam("the bike's +0x4C is 1.0, the model init SLUS 0x8002FAD4's store 0x8003006C (not ported for "
                 "bikes); the initial place riderDef +0x27 is the grid order (ComputePlace 0x800138E8 at the grid "
                 "is the session's place pass)");
    }
    BuildArena(disc);
    BuildRouteArena(disc);
    if (rr::game::LoaderPorted()) { // the ported loader: RaceReset's model tables and the bike bank first, as the loader
        GuestRam bg(arena_.Ram(), kArenaGp);
        rr::game::LoaderTotals().tablesReady = false;
        NoteSeam(rr::game::LoaderBikeBank(bg, disc, static_cast<int>(gameState_[0x3C]), arenaFreeFrom_, kArenaObjectsTo));
    }
    {   // the traffic's regions: the model tables, the car file, pool 3 (traffic_arena.h)
        GuestRam tg(arena_.Ram(), kArenaGp);
        // the ported loader: the PORTED PopulationReset 0x80068D54 (run by the traffic arena) zeroes the pools' counts
        // as BuildRace does before BuildGrid; the session's own layout (RRJB_GRID=ours) keeps its counts (OURS)
        const uint32_t kCounts[] = {0x8005B1F8u, 0x8005B1FCu, 0x8005B218u, 0x8005AD38u, 0x8005AD3Cu};
        uint32_t keep[5];
        for (int k = 0; k < 5; ++k) keep[k] = tg.U32(kCounts[k]);
        const TrafficArenaReport tr =
            BuildTrafficArena(tg, disc, world_.raceId, static_cast<int>(gameState_[0x30]), arenaFreeFrom_, kArenaObjectsTo);
        if (!gridPorted_)
            for (int k = 0; k < 5; ++k) tg.W32(kCounts[k], keep[k]);
        trafficReady_ = tr.ok;
        char b[400];
        std::snprintf(b, sizeof(b),
                      "the traffic arena (traffic_arena.h, RASHCDI 0x8005BE40 / 0x8005C630 / 0x8005CA10 / 0x80068D54 "
                      "%s): %s, %zu model(s), %d car class(es) from %d CTKP chunk(s), %d LECT/KNBP/TSLP "
                      "chunk(s) not uploaded (no VRAM), placed OURS at 0x%08X..0x%08X%s%s",
                      rr::game::LoaderPorted() ? "PORTED, the race loader" : "transcribed", tr.carFile.c_str(), tr.models.size(), tr.classCars, tr.ctkpChunks, tr.texArmsSkipped, tr.fileAt,
                      tr.end, tr.ok ? "" : " - FAILED: ", tr.error.c_str());
        NoteSeam(b);
    }
    { GuestRam wg(arena_.Ram(), kArenaGp); NoteSeam(BuildWeaponArenaLine(wg, disc, gameState_[0x3C], arenaFreeFrom_, kArenaObjectsTo)); } // weapon_session.h
    { GuestRam mg(arena_.Ram(), kArenaGp); NoteSeam(BuildModelArenaLine(mg, disc, gameState_[0x3C], arenaFreeFrom_, kArenaObjectsTo)); } // model_runtime.h
    { GuestRam qg(arena_.Ram(), kArenaGp); NoteSeam(BuildRsqrtArena(qg, tables_.rsqrtTable.data(), arenaFreeFrom_, kArenaObjectsTo)); } // shadow_product.h
    // A two-seat race's passenger stance bank must have its room in this region (JailReserve, below) or the
    // short set runs past the pose arena's part arrays into the animation objects; its packet heap is cut after it
    const bool twoSeatHeap = (gameState_[4] & 8u) != 0u && std::getenv("RRJB_JAIL_HEAP") == nullptr;
    if (!twoSeatHeap) NoteSeam(BuildPacketHeap(arenaFreeFrom_, kArenaObjectsTo)); // shadow_product.h (the original's heap extent)
    NoteSeam(ReserveFrameOts(arenaFreeFrom_, kArenaObjectsTo, players_)); // frame_ot.h (SLUS 0x8001C1AC's OTs)
    if (!rr::game::LoaderPorted() || !rr::game::PedsEnabled()) { // the ported EnterRace set it
        GuestRam pg(arena_.Ram(), kArenaGp);
        pg.W32(rr::sim::kPedSwitch, rr::game::PedSwitchFor(pg)); // EnterRace RASHCDI 0x80063BB8 (peds_product.h)
    }
    {   // the HAZARD set by BuildRace's block copy and HazardPick (hazard_product.h), the world arena (world_pop_product.h), then
        // HazardSetup 0x8006AEF4 (RASHCDI 0x800639B4, after the HAZARD file is loaded)
        GuestRam wg(arena_.Ram(), kArenaGp);
        rr::game::HazardRace hz;
        std::string hzLine;
        hazardSet_ = rr::game::HazardSetForRace(wg, disc, world_.set, world_.raceId, gameState_[4], hz, hzLine);
        NoteSeam(hzLine);
        NoteSeam(rr::game::BuildWorldArenaLine(wg, disc, hazardSet_, arenaFreeFrom_, kArenaObjectsTo));
        NoteSeam(rr::game::BuildHazardArenaLine(wg, hz, gameState_[4], arenaFreeFrom_, kArenaObjectsTo));
    }
    { GuestRam pg(arena_.Ram(), kArenaGp); NoteSeam(rr::game::BuildPedArena(pg, disc, world_.raceId, arenaFreeFrom_, kArenaObjectsTo)); } // peds_product.h
    if (rr::game::StreamFilesPorted() && rr::game::StreamFilesTotals().setUps != 0) // 0x8002428C PORTED loaded it (stream_files_product.h)
        NoteSeam(rr::game::StreamFilesLine());
    else { GuestRam rg(arena_.Ram(), kArenaGp); NoteSeam(rr::game::BuildRlsArena(rg, disc, world_.set, arenaFreeFrom_, kArenaObjectsTo)); } // cell_view.h
    if (rr::game::Loader2Totals().gridPlanned) { // loader2: BuildRace's mallocs at the grid (AnimNoise's 3 KiB file,
        // PartSlotsInit's part arrays, EscapeLoad's STARTJBA arrays) - OURS: the 8 KiB of low RAM after FIGHT.BIN
        // (0x8000E000..0x80010000, empty in the product's arena; the object area is spent by then - the spare 16 KiB
        // blocks the sound heap takes are what is left of it). The console's heap has them wherever its blocks fall.
        rr::game::Loader2Counts& L2 = rr::game::Loader2Totals();
        L2.heapNext = 0x8000E000u;
        L2.heapEnd = 0x80010000u;
    }
    JailReserve(disc); // the two-seat races' passenger stance bank, before the cell buffers (jail_session.cpp)
    if (twoSeatHeap) { // the heap takes what is left but the spare 16 KiB blocks (the sound heap, a second
        // player's stat array) and 256 bytes before the first of them (JailRigParts' slots); RRJB_JAIL_HEAP: the control
        const uint32_t blocks = players_ == 2 ? 2u : 1u;
        const uint32_t base = (arenaFreeFrom_ + 15u) & ~15u;
        uint32_t lim = std::min<uint32_t>(kArenaObjectsTo - blocks * kCellChunkBytes - 0x100u, base + kPacketHeapBytes);
        if (((lim + 0xFFFu) & ~0xFFFu) - lim < 0x100u) lim -= 0x100u;
        NoteSeam(BuildPacketHeap(arenaFreeFrom_, lim, 0x3000u));
    }
    BuildCellCatalog(disc);
    if (streamPorted_) StreamStart(disc); // SetUpRace's SLUS 0x80023020, before BuildRace (stream_session.cpp)
    if (!BuildGridPorted(disc)) LoadStatBlocks(disc); // the ported grid: BuildGrid loads the stat blocks itself
    {   // SaveCopRecord 0x800644C8 per police bike (the last wins) and BuildRace's census 0x80068684
        GuestRam cg(arena_.Ram(), kArenaGp);
        for (const RaceBike& b : bikes_)
            if (b.isCop && !gridPorted_) rr::game::SaveCopRecord(cg, b.entityAddress); // (SpawnBike's own with the ported grid)
        if (!rr::game::Loader2Totals().buildRaceRan) rr::game::PoliceCensus(cg); // (loader2: CensusInit 0x80068684 PORTED)
    }
    LoadEnvTable(disc);
    BuildAnimArena(disc);
    if (OriginalBind()) { // loader2: the ORIGINAL's binding stands (ModelBind SLUS 0x8002FAD4 PORTED at SpawnBike and
        // SpawnPassenger, RegistryBind leaving the last LOD; ViewPass 0x8008CFDC's LodChoice picks each frame's LOD)
        JailRigParts(); // the rig's part slots: RegistryBind's (jail_session.cpp reports them)
        GuestRam mg(arena_.Ram(), kArenaGp);
        size_t bikesBound = 0, ridersBound = 0;
        for (const RaceBike& b : bikes_) {
            bikesBound += mg.U32(b.entityAddress + 0x60u) != 0u ? 1u : 0u;
            ridersBound += mg.U32(b.ownerAddress + 0x60u) != 0u ? 1u : 0u;
        }
        char mbl[520];
        std::snprintf(mbl, sizeof(mbl),
                      "the machines' model binding is the ORIGINAL's: ModelBind SLUS 0x8002FAD4 PORTED "
                      "bound %zu of %zu bike(s) and %zu rider(s) at SpawnBike (RegistryBind 0x8002FDEC: the class's model, the "
                      "last LOD, the part array on heap 0, the table key), the weapon objects at the grid, the passengers at "
                      "SpawnPassenger; each frame's LOD is ViewPass 0x8008CFDC's (PORTED); the session's BindModels (LOD 0) "
                      "is RRJB_LOADER2=off's / RRJB_LOADER_BIND's",
                      bikesBound, bikes_.size(), ridersBound);
        NoteSeam(mbl);
    } else {   // the machines' models at LOD 0, after the pose's records (model_runtime.h)
        std::vector<ModelBinding> mb;
        GuestRam cm(arena_.Ram(), kArenaGp); // the model by class, as SpawnBike's ModelBind picks it
        for (const RaceBike& b : bikes_) {
            mb.push_back({b.entityAddress, rr::game::BikeClassModel(cm, cm.U32(b.entityAddress + 0xB4u), ProductBikeModel(gameState_[0x3C], b.isCop))});
            mb.push_back({b.ownerAddress, rr::game::RiderClassModel(cm, cm.U32(b.ownerAddress + 0xB4u), kProductRiderModel)});
        }
        for (const Passenger& ps : passengers_) mb.push_back({ps.rider, kProductRiderModel}); // jail_session.cpp
        JailRigParts(); // the rig's six part slots (jail_session.cpp)
        GuestRam mg(arena_.Ram(), kArenaGp);
        NoteSeam(BindModels(mg, mb));
    }
    BuildPopulationArena();
    if (!gridPorted_) SeatCursors(); // the ported grid seated them (SpawnBike's CursorSeat / RoadPosition)
    NoteGridLayout(gridPorted_ ? "ORIGINAL: BuildGrid PORTED" : "OURS: the session's layout");
    JailSpawn(disc); // SpawnPassenger 0x800670FC, STARTJBA.BIN (jail_session.cpp)
    ModeSetUp(); // the loader's per-race-type setup (race_modes.cpp: time limit, the player cop, Jailbreak)
    if (rr::game::AnimObjOn()) { // RASHCDI 0x8005D1A0 with the loader's count, once pool 1 is complete (animobj_product.h)
        GuestRam ag(arena_.Ram(), kArenaGp);
        NoteSeam(rr::game::AnimObjectsSetup(ag, kArenaAnimObjects, 0x801FC000u, animObjects_, animPrograms_,
                                            rr::game::LoaderPorted() ? rr::game::LoaderTotals().animCount : -1));
    }
    StartStances();
    BuildCameraArena(disc);
    if (streamPorted_) { // SetUpRace's CamTarget SLUS 0x800235B0 (0, player 1's bike) and (1, player 2's): the entity
        GuestRam cg(arena_.Ram(), kArenaGp); // each stream record follows (StreamTrack 0x80023A14)
        cg.W32(0x80053478u + 4u, cg.U32(kP1EntityPtr));
        cg.W32(0x80053478u + 0x80u + 4u, cg.U32(kPlayer2Bike));
    }
    BuildHud(disc);
    for (size_t i = 0; i < bikes_.size(); ++i) UpdateDisplayDistance(i);

    // ---- the sound system.
    surfaceTable_ = tables_.ExeWindow(0x800525C0u, 64);
    {
        std::string error;
        std::vector<uint8_t> rashnz;
        if (const auto f = disc.Find("DATA/RASHNZ_E.DAT")) rashnz = disc.ReadFile(*f);
        if (rashnz.empty()) {
            NoteSeam("DATA\\RASHNZ_E.DAT is not on this disc - the ported emitter runs with no "
                     "banks, so every sound it starts is silent");
        } else if (!sounds_.LoadBanks(rashnz, error)) {
            NoteSeam("DATA\\RASHNZ_E.DAT did not parse (" + error + ") - no banks are registered");
        }
    }
    sounds_.SetExecutable(tables_.Exe());
    // The sound state lives in THIS arena: its heap blocks take one of the cell
    // buffers (16 KiB of the object area nothing else uses), and AudioFrame runs on the world.
    if (!freeCellBuffers_.empty()) {
        const uint32_t heap = freeCellBuffers_.back();
        freeCellBuffers_.pop_back();
        --cellBuffersTotal_;
        sounds_.Attach(arena_.Ram(), heap, kCellChunkBytes);
    }
    if (const auto f = disc.Find("DATA/AUDTAUNT.STR")) sounds_.SetLoaderSpeechBytes(static_cast<uint32_t>(f->size)); // the ported loader
    uint32_t noise[3] = {0, 0, 0}; // gp+1880 / 1916 / 1948: AnimNoise RASHCDI 0x80063158's words (loader2)
    {
        GuestRam ng(arena_.Ram(), kArenaGp);
        for (uint32_t k = 0; k < 3u; ++k) noise[k] = ng.U32(kArenaGp + (k == 0 ? 1880u : k == 1 ? 1916u : 1948u));
    }
    sounds_.Reset();
    if (noise[2] != 0u) { // AudioReset SLUS 0x80019D9C is the boot's (0x800163D4, before any race); the product runs it in
        // its sound reset after SpawnBike loaded ANIMNOIZ.DAT (OURS order): the three words are put back as the race has them
        GuestRam ng(arena_.Ram(), kArenaGp);
        for (uint32_t k = 0; k < 3u; ++k) ng.W32(kArenaGp + (k == 0 ? 1880u : k == 1 ? 1916u : 1948u), noise[k]);
    }
    if (rr::game::Loader2Totals().siren != -2) { // loader2: SetUpRace's SirenSet SLUS 0x80018DC8 PORTED (race_modes.cpp)
        GuestRam sg(arena_.Ram(), kArenaGp);     // wrote gp+1908 by the hazard set; the sound reset's 0x17 gives way to it
        sg.W32(kArenaGp + 1908u, static_cast<uint32_t>(rr::game::Loader2Totals().siren));
    }
    if (!sounds_.LoaderLine().empty()) { // the ported loader: the sound set-up PORTED (sound_loader.cpp)
        NoteSeam(sounds_.LoaderLine());
        if (sounds_.LoaderRan()) {
            ++rr::game::LoaderTotals().spuAttr;
            ++rr::game::LoaderTotals().soundInit;
            ++rr::game::LoaderTotals().soundLoad;
        }
    }
    {   // the race's music: the PORTED shuffle, stream voices and key-on; the disc
        // reads of the stream player are these
        const auto album = disc.Find("DATA/ALBUM.ALB");
        const auto intro = disc.Find("DATA/INTRO.ALB");
        // one player only, as the original: StreamSetUp SLUS 0x80022F90 opens the album, the race init 0x80012330
        // starts it (0x800247A0) and 0x80011BB0 waits for its lead-in only when game_state+0x30 == 1 - a two-player
        // race runs the countdown's voice 0x800164B4 instead
        if (players_ != 1 && sounds_.Attached())
            NoteSeam("two players: no race music, as the original (SLUS 0x80022F90 / 0x80012330 / 0x80011BB0 open and "
                     "start the album only when game_state+0x30 == 1; the countdown's voice 0x800164B4 runs instead)");
        if (sounds_.Attached() && album && intro && players_ == 1) {
            // MusicPickShuffle's GetRCnt(2): the console's counter now (root_counter.h; a live run only)
            if (LiveRootCounter()) {
                const uint16_t c = ConsoleRootCounter2();
                sounds_.ReadRootCounterAs(c);
                NoteSeam("root counter 2 for MusicPickShuffle: " + std::to_string(c) +
                         " (the console's counter at this moment, root_counter.h): the walk starts at track " +
                         std::to_string((18u * (c & 0xFFu)) >> 8));
            }
            const bool ok = sounds_.StartMusic([&disc, album, intro](int file, uint32_t offset, uint8_t* dst, uint32_t bytes) {
                const rr::DiscFile& f = file == 0 ? *album : *intro;
                if (static_cast<uint64_t>(offset) + bytes > f.size) return false;
                disc.ReadForm1(f.lba, offset, dst, bytes);
                return true;
            });
            NoteSeam(ok ? "the race music streams ALBUM.ALB track " + std::to_string(sounds_.MusicTrack()) +
                              " (the PORTED shuffle's pick) after its INTRO.ALB lead-in, into the SPU model's "
                              "two rings; the CD reads and the SPU-IRQ refill of the stream player are ours"
                        : std::string("the race music did not start (the PORTED shuffle or the disc reads failed)"));
        }
    }
    // SpeechInit's GetRCnt(2), the race's first AUDTAUNT.STR record: the console's counter now (root_counter.h)
    if (LiveRootCounter()) {
        const uint16_t c = ConsoleRootCounter2();
        sounds_.ReadRootCounterAs(c);
        NoteSeam("root counter 2 for SpeechInit: " + std::to_string(c) + " (the console's counter at this moment, "
                 "root_counter.h): the stream starts at AUDTAUNT.STR record " + std::to_string(((c & 0xFFu) * 133u) >> 8));
    }
    StartSpeech(disc); // the riders' voices: SpeechInit and AUDTAUNT.STR (speech_session.cpp)
    sounds_.SetTables(tables_.atan.data(), tables_.sincos.data(), surfaceTable_.data());
    sounds_.SetGameState(gameState_.data());
    effects_ = std::make_unique<EffectSpawner>(*this);
    sounds_.SetEffectTarget(effects_.get());
    if (sounds_.Attached()) {
        NoteSeam("the sound runs PORTED on this arena: AudioFrame SLUS 0x80018FAC as the frame's "
                 "last call (listener, reverb depth, EngineNote/RoadNote, the object sounds of the nearest "
                 "bikes, the cue voice), AudioVSyncTick/SoundService twice a frame, ResetSoundState, "
                 "PatchBank, AudioReset and the loader's SoundRecordsInit RASHCDI 0x80063448 at the start; "
                 "into the SPU model with ADSR and main volume, no Gaussian interpolation, no reverb");
        std::string sliderLine; // the front end's sliders reach the race (handover.h)
        {
            const rr::shell::Handover& fh = rr::shell::PendingHandover();
            if (fh.active && fh.sliders) {
                sliderLine = "the options' seven volume sliders 0x800D6C00 are the front end's (handover.h; VolumeSet RASHCDF "
                             "0x8007F20C wrote them):";
                for (uint32_t k = 0; k < 7; ++k) sliderLine += " " + std::to_string(fh.volume[k]);
                sliderLine += ", saved copy 0x800D6C20:";
                for (uint32_t k = 0; k < 7; ++k) sliderLine += " " + std::to_string(fh.volumeSaved[k]);
            } else {
                sliderLine = "the volume sliders 0x800D6C00 are 41 29 29 65 65 49 24 - a direct --race start: every "
                             "capture's values (the options' defaults 7 through VolumeSet; a front-end start carries the "
                             "player's own through handover.h)";
            }
        }
        if (sounds_.LoaderRan())
            NoteSeam(std::string("sound set-up: ") + sliderLine + ", " +
                     (rr::game::Loader2Totals().siren != -2
                          ? "the siren's sound index gp+1908 = " + std::to_string(rr::game::Loader2Totals().siren) +
                                " by SirenSet SLUS 0x80018DC8 (PORTED, SetUpRace's call with HazardPick's pair)"
                          : std::string("the siren's sound index 0x17 (every capture's; SirenSet SLUS 0x80018DC8 is the "
                                        "ported loader's, RRJB_LOADER2=off)")) +
                     "; the SPU heap: sound_loader.cpp's line (SpuMalloc SLUS 0x8004F3C8 PORTED); the render camera "
                     "SLUS 0x8002F17C, of which only the yaw copy view+0x2E8 -> *(0x8005AEC0)+0x7C runs");
        else
        NoteSeam("sound set-up not ported, named: SoundInit's two mallocs and LoadBank's SPU heap/DMA "
                 "(our blocks, the capture's SPU bases), the race loader RASHCDI 0x800627F8's writes (the "
                 "one-player capture values), " + sliderLine + ", libspu's main volume (0x3FFF, "
                 "the capture's), and the render "
                 "camera SLUS 0x8002F17C, of which only the yaw copy view+0x2E8 -> *(0x8005AEC0)+0x7C runs");
    } else {
        NoteSeam("SLUS 0x80018FAC AudioFrame is not run (no heap block for the sound state): the listener "
                 "is placed by us from the player's bike each frame");
    }

    // SLUS 0x800119C0's per-bike loop, its dormant arm (0x80011A88): Transition(e, 1) - the PORTED
    // retirement (population.h) - for every bike the loader spawned dormant (the police, grid_loader.h).
    // OURS: run after the whole set-up, where the original interleaves it with each rider's stance event.
    {
        GuestRam g(arena_.Ram(), kArenaGp);
        g.SetScratchpad(scratchpad_.data());
        const rr::sim::BikeTables t = SessionTables();
        ProductRiderSeams seams(this, g);
        Refusals refusals;
        refusals.session = this;
        refusals.log = &log_;
        ProductStampCallees stamp(g, arena_.Ram(), seams, log_);
        ProductPopCallees pop(this, g, arena_.Ram(), seams, stamp, refusals, t);
        pop.targetSpeed = [this](uint32_t e, int32_t d) { return TargetSpeedAt(e, d); };
        {   // the loader's stream start (RASHCDI 0x800637D0 -> SLUS 0x80023020: 0x800235C8 / 0x80023714 load the cells
            // at the grid during the loading screen, so they are resident when 0x800119C0 runs) - OURS: the
            // product's streamer rules - the resident road pieces and the cells - and the resource list, once here
            // as in every frame; with the ported streamer the PORTED stream start already ran (StreamStart)
            if (!streamPorted_) {
                RoadStreamPass(g);
                CellStreamPass(g);
                std::vector<rr::game::LoadedCell> loaded;
                for (const CatalogCell& c : cells_) if (c.at != 0) loaded.push_back({c.at, c.body});
                rr::game::WriteResourceList(g, std::move(loaded));
            }
        }
        for (size_t i = 0; i < bikes_.size(); ++i) {
            if (g.S16(ArenaEntity(i) + 0x140u) != 0) { // the live arm (0x80011A70 / 0x80011A78): EntityCell, BuildObb
                rr::sim::EntityCell(g, ArenaEntity(i));                               // PORTED (cell_draw.h)
                if (!pop.BuildObb(ArenaEntity(i)) || g.Faulted()) {
                    g.ClearFault();
                    NoteSeam("SLUS 0x800119C0's BuildObb 0x8008BA18 of a live grid bike refused");
                }
                continue;
            }
            if (!rr::sim::Transition(g, ArenaEntity(i), 1, kArenaStepSp, pop) || g.Faulted()) {
                g.ClearFault();
                NoteSeam("SLUS 0x800119C0's Transition(e, 1) of a dormant grid bike (PORTED) refused");
            }
        }
    }

    // The collision set-up: SLUS 0x800119C0's tail calls RASHCDG 0x800A41EC and then the collision
    // pass at dt = 0 (the prime pass), which builds the broad-phase grid, the chain nodes and the
    // contact list once before the first frame.
    // 0x800A41EC, transcribed from our listing (12 instructions, 0x800A41EC..0x800A4218, no calls):
    //   sb 128,256(0x800CCFA8); sb 255,257(0x800CCFA8)   - node 128, the pair loops' sentinel {0x80, 0xFF}
    //   sw 1,0x800CCF78; sw zero,0x800CCF7C              - the current chain words
    // The overlay image holds 0 there; with node 128 = {0, 0} an exhausted chain cursor re-enters node
    // 0 and the pass's pair loops never end (the race hangs at the GO).
    // Every one of the 18 race captures holds {0x80, 0xFF}. PORTED (camera_setup.h, row
    // collision_set_up); RRJB_LOADER2_CAM=off keeps these four stores (the control).
    std::string collSetUp = "transcribed, the node-128 sentinel and the chain words";
    {
        GuestRam g(arena_.Ram(), kArenaGp);
        if (rr::game::Loader2CamPorted()) { // loader2_cam: RASHCDG 0x800A41EC PORTED (camera_setup.h)
            collSetUp = rr::game::PortedCollisionSetUp(g);
        } else { // RRJB_LOADER2_CAM=off: the transcription
            g.W8(rr::sim::kCollNodes + 256u, 128);
            g.W8(rr::sim::kCollNodes + 257u, 255);
            g.W32(rr::sim::kChainCur, 1);
            g.W32(rr::sim::kChainCur + 4u, 0);
        }
    }
    CollisionPass(0);
    NoteSeam(std::string("the collision set-up RASHCDG 0x800A41EC (") + collSetUp + std::string(") and the prime pass (the collision pass at dt = 0, SLUS 0x800119C0's tail) ran at the "
                         "end of the set-up") + (log_.collisionDeclined ? "; the prime pass DECLINED" : "") +
             "; their siblings there (0x8008D89C, 0x8009C308) run next (SetupWorldWalk)");
    SetupWorldWalk(); // SLUS 0x800119C0's next two calls, VolumeLists and the cell walker (solid_product.h)
}


// SLUS 0x800119C0 after the prime pass (0x80011B28 / 0x80011B30): VolumeLists 0x8008D89C and the cell walker
// 0x8009C308, both PORTED (world_pop.h) - the props and collision volumes of the cells resident at the start
// exist before the first frame instead of after SpawnerPass's first walk.
void RaceSession::SetupWorldWalk() {
    GuestRam g(arena_.Ram(), kArenaGp);
    g.SetScratchpad(scratchpad_.data());
    const rr::sim::BikeTables t = SessionTables();
    ProductRiderSeams seams(this, g);
    Refusals refusals;
    refusals.session = this;
    refusals.log = &log_;
    ProductStampCallees stamp(g, arena_.Ram(), seams, log_);
    ProductPopCallees pop(this, g, arena_.Ram(), seams, stamp, refusals, t);
    pop.targetSpeed = [this](uint32_t e, int32_t d) { return TargetSpeedAt(e, d); };
    // The start cells are resident (the stream start above) and every live bike has its cell +0xB0 (the
    // per-bike loop's EntityCell), so this walk finds the props and volumes around the grid, as the
    // original's does (with neither, it finds none).
    rr::sim::VolumeLists(g);
    // what the walk starts from: player 1's cell, the cells around it, how many of them have a resident slot
    const uint32_t bike0 = g.U32(rr::sim::kWpPlayerBikes);
    const int32_t around = rr::sim::CellsAround(g, kArenaStepSp - 1024u, 0, kArenaStepSp - 512u);
    int32_t slotted = 0;
    for (int32_t k = 0; k < around && k < 8; ++k)
        if (rr::sim::CellSlotFor(g, g.U32(kArenaStepSp - 1024u + 4u * static_cast<uint32_t>(k)), 0) != 0) ++slotted;
    g.ClearFault();
    const bool ok = pop.CellWalker(kArenaStepSp - 48u) && !g.Faulted();
    g.ClearFault();
    char b[400];
    std::snprintf(b, sizeof(b),
                  "SLUS 0x800119C0's VolumeLists 0x8008D89C and cell walker 0x8009C308 (PORTED) at set-up%s: %u prop(s), "
                  "%u collision volume(s) live before the first frame (player 1's cell 0x%08X, %d cell(s) around it, %d "
                  "with a resident slot; view 0's eye (%d, %d), the bike (%d, %d))",
                  ok ? "" : " REFUSED", g.U32(rr::sim::kWpPool4Ctrl), g.U32(rr::sim::kWpPool6Ctrl), g.U32(bike0 + 0xB0u),
                  around, slotted, g.S32(rr::sim::kWpViews + 184u) >> 16, g.S32(rr::sim::kWpViews + 192u) >> 16,
                  g.S32(bike0 + 184u) >> 16, g.S32(bike0 + 192u) >> 16);
    NoteSeam(b);
}

void RaceSession::LoadImages() {
    const std::vector<uint8_t>& exe = tables_.Exe();
    const std::vector<uint8_t>& ovl = tables_.Overlay();
    GuestRam g(arena_.Ram(), kArenaGp);
    g.WriteBlock(kExeLoad, exe.data() + kExeHeader, static_cast<uint32_t>(exe.size() - kExeHeader));
    g.WriteBlock(kOverlayLoad, ovl.data(), static_cast<uint32_t>(ovl.size()));
}

uint32_t RaceSession::ArenaEntity(size_t i) const {
    return kArenaPool0 + static_cast<uint32_t>(kEntitySize) * static_cast<uint32_t>(i);
}

// The grid. `BuildGrid RASHCDI 0x80067B00` - which allocates the pools and fills the entities - is
// not ported, so this is OUR layout: the player on the centre line and the opponents strung out
// behind in two columns, each record seeded with the fields the ported code reads and a capture
// shows BuildGrid leaving there. It is initialisation, not simulation.
void RaceSession::SetupGrid() {
    GuestRam g(arena_.Ram(), kArenaGp);
    const uint32_t n = static_cast<uint32_t>(bikes_.size());
    // The pool table 0x800CE4D0 (population.md 1): pool 0 and pool 1, each {base, stride, &live,
    // &high} with rr-race's own count words.
    g.W32(kPoolTable + 0x00u, kArenaPool0);
    g.W32(kPoolTable + 0x04u, static_cast<uint32_t>(kEntitySize));
    g.W32(kPoolTable + 0x08u, kPool0Live);
    g.W32(kPoolTable + 0x0Cu, kPool0High);
    g.W32(kPoolTable + 0x10u, kArenaPool1);
    g.W32(kPoolTable + 0x14u, kRiderBytes);
    g.W32(kPoolTable + 0x18u, kPool1Live);
    g.W32(kPoolTable + 0x1Cu, kPool1High);
    g.W32(kPool0High, n - 1u);
    g.W32(kPool1Live, n);
    g.W32(kPool1High, n - 1u);
    g.W32(rr::sim::kRoadPool0Ptr, kArenaPool0);
    g.W32(rr::sim::kRoadPool1Ptr, kArenaPool1);
    g.W32(kPlayerBikes, kArenaPool0);
    // player 2's bike (BuildGrid's SpawnBike with flag bit 3): pool-0 slot 1
    g.W32(kPlayerBikes + 4u, players_ == 2 ? ArenaEntity(1) : 0u);
    g.W32(kPlayer2Bike, players_ == 2 ? ArenaEntity(1) : 0u);
    g.W32(kP1EntityPtr, kArenaPool0);

    for (size_t i = 0; i < bikes_.size(); ++i) {
        RaceBike& bike = bikes_[i];
        EntityView view(bike.entity.data());
        view.SetU16(ent::kHandle, static_cast<uint16_t>(i)); // pool 0, slot i
        WriteS32(bike.entity.p, ent::kClass, 0);
        // +0x140, the shared header's "live" word: 1 for every bike. The activation pass RASHCDG
        // 0x80093E6C that recomputes it from the camera window each frame is not ported (named).
        WriteS16(bike.entity.p, ent::kLiveState, 1);
        // The half extents and the mass: the values rr-grid's player bike holds (BuildGrid derives
        // them from the bike record; ours as constants, named below).
        WriteS32(bike.entity.p, ent::kHalfX, 0x5C00);
        WriteS32(bike.entity.p, ent::kHalfY, 0x10C00);
        WriteS32(bike.entity.p, ent::kHalfZ, 0x15000);
        WriteS32(bike.entity.p, 0x13C, 0x01DB0000); // 475.0
        // The seat record +0x38 (the rider) / +0x3C, as every capture's bike holds it.
        WriteS32(bike.entity.p, 0x38, static_cast<int32_t>(bike.ownerAddress));
        WriteS32(bike.entity.p, 0x3C, 2);
        uint32_t flagsA = 0;
        if (!bike.isPlayer) flagsA |= 0x08000000u; // bit 27: under AI control (bike.h, ai.h)
        view.SetU32(ent::kFlagsA, flagsA);
        view.SetU32(ent::kFlagsB, 0);
        view.SetU32(ent::kFlagsC, 0);
        bike.entity[ent::kGear] = 1;
        bike.entity[0x3A0] = 8; // every capture's bikes
        WriteS32(bike.entity.p, ent::kOwner, static_cast<int32_t>(bike.ownerAddress));
        WriteS32(bike.entity.p, ent::kRiderDef, static_cast<int32_t>(bike.riderDefAddress));
        // The command stack: opcode 4, "race", on top - every racer's command in every capture.
        bike.entity[0x3B2] = 1;
        bike.entity[0x3B4 + 8] = 4;
        // ... and its target halfword +0x3BE = 224, "nobody", as every capture holds it: the HUD reads the
        // top command's target as "whom this rider is after"; no ported AI code reads it for
        // opcode 4 (ai.cpp only for 14..17).
        bike.entity[0x3B4 + 8 + 2] = 224;

        // The rider record (pool 1): handle 0x20 | slot, live, the bike it sits on (+0x34 and
        // +0x254), on the bike (+0x25C = 1), no stance yet (224), byte +9 = 7 (every capture's).
        uint8_t* r = bike.owner.p;
        WriteS16(r, 0xAC, static_cast<int16_t>(0x20 | i));
        WriteS16(r, ent::kLiveState, 1);
        WriteS32(r, 0x34, static_cast<int32_t>(bike.entityAddress));
        WriteS32(r, 0x254, static_cast<int32_t>(bike.entityAddress));
        WriteS32(r, 0x25C, 1);
        WriteS16(r, 0x220, 224);
        r[0x09] = 7;

        // The runtime rider record (+0x43C above, provisional) is the rider-record loader's (grid_loader.h).

        // Where the bike starts on the route. Ours: the player at `startDistance`, the opponents 8
        // units apart behind it in two columns - the whole field moved forward so that the last row
        // is still on the route (these bikes now really occupy their places, so a field clamped to
        // the route's first slice would stack them on one point).
        const std::vector<rr::RoadSlice>& path = world_.path;
        const double first = static_cast<double>(path.front().distance) / 65536.0;
        const double last = static_cast<double>(path.back().distance) / 65536.0;
        size_t racers = 0;
        for (const RaceBike& b : bikes_) racers += b.isCop ? 0u : 1u;
        const double head = std::max(startDistance_, first + 8.0 * static_cast<double>(racers - 1));
        double along = head - 8.0 * static_cast<double>(i);
        double lateral = (i == 0) ? 0.0 : ((i % 2) == 0 ? 6.0 : -6.0);
        if (players_ == 2 && i < 2) { // OURS as the rest of this grid: the two players side by side at the front
            along = head;
            lateral = i == 0 ? -4.0 : 4.0;
        }
        if (bike.isCop) { // the file's along offsets, 125/128 of them, from the player's
            along = head + static_cast<double>(bikes_[0].gridAlong - bike.gridAlong) / 65536.0 * 125.0 / 128.0;
            lateral = 0.0;
        }
        const double wanted = std::clamp(along, first, last);
        size_t lo = 0, hi = path.size() - 1;
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (static_cast<double>(path[mid].distance) / 65536.0 <= wanted) lo = mid;
            else hi = mid;
        }
        const rr::RoadSlice& a = path[lo];
        const rr::RoadSlice& b = path[std::min(lo + 1, path.size() - 1)];
        const double da = static_cast<double>(a.distance) / 65536.0;
        const double db = static_cast<double>(b.distance) / 65536.0;
        const double t = db > da ? std::clamp((wanted - da) / (db - da), 0.0, 1.0) : 0.0;
        int32_t p[3];
        int16_t rows[9];
        for (int k = 0; k < 3; ++k) {
            const double pa = static_cast<double>(a.pos[k]) / 65536.0;
            const double pb = static_cast<double>(b.pos[k]) / 65536.0;
            const double lat = static_cast<double>(a.m[k]) / 4096.0;
            p[k] = static_cast<int32_t>(std::lround((pa + (pb - pa) * t + lat * lateral) * 65536.0));
            rows[3 + k] = static_cast<int16_t>(-a.m[3 + k]);    // +0x20A: the NEGATED slice normal
            rows[6 + k] = a.m[6 + k];                           // +0x210: the facing, down the route
        }
        // +0x204, the lateral row, = r1 x r2, so that r2 = r0 x r1 - the handedness every captured
        // bike's +0x204 matrix has (world.cpp negates only the tangent of a leg driven backwards).
        for (int k = 0; k < 3; ++k) {
            const int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
            const double c = (static_cast<double>(rows[3 + k1]) * rows[6 + k2] -
                              static_cast<double>(rows[3 + k2]) * rows[6 + k1]) / 4096.0;
            rows[k] = static_cast<int16_t>(std::lround(c));
        }
        for (uint32_t k = 0; k < 3; ++k) {
            WriteS32(bike.entity.p, ent::kObbCentre + 4u * k, p[k]);
            WriteS32(bike.entity.p, 0x1F8 + 4u * k, p[k]);
            WriteS32(bike.entity.p, 0x1D4 + 4u * k, p[k]);
        }
        for (uint32_t k = 0; k < 9; ++k) {
            WriteS16(bike.entity.p, 0x204 + 2u * k, rows[k]);
            WriteS16(bike.entity.p, ent::kAxes + 2u * k, rows[k]);
        }
        for (uint32_t k = 0; k < 3; ++k) WriteS16(bike.entity.p, 0x1C2 + 2u * k, rows[6 + k]);
        bike.pathHint = lo;
    }
    // The five bike lists, laid down once: every bike on the riding list, in slot
    // order. From here on the PORTED list migration 0x80071BCC moves bikes between them.
    for (const RaceBike& b : bikes_)
        if (b.isCop) rr::game::SpawnCopBike(g, b.entityAddress); // SpawnBike's police stores (grid_loader.h)
    LinkBikeLists(g);
    if (!gridPorted_) NoteSeam("RRJB_GRID=ours / --start: the PORTED BuildGrid RASHCDI 0x80067B00 (grid_build.h) is not run: the grid, the pool records' seeds (half extents "
             "0x5C00/0x10C00/0x15000 and mass 475.0 as rr-grid's player bike holds them, the seat record, "
             "the rider record's handle / bike / mount state, class 1 for every opponent) and the initial "
             "list order are OURS");
}

size_t RaceSession::LinkBikeLists(GuestRam& g) {
    const uint32_t heads[] = {kListDormant, kListRiding, kListThrown, kListDown, kListSpin};
    uint32_t tail[5];
    for (int h = 0; h < 5; ++h) {
        g.W32(heads[h], heads[h]);
        g.W32(heads[h] + 4u, heads[h]);
        tail[h] = heads[h];
    }
    size_t riding = 0;
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint32_t e = ArenaEntity(i);
        const uint32_t fc = g.U32(e + 0x238);
        int h;
        if (g.S16(e + 0x140) == 0) h = 0;                    // 0x80071BE0
        else if (fc & 0x600u) h = (fc & 0x400u) ? 2 : 4;     // 0x80071C44..C68
        else if (fc & 0x1FFu) h = 3;                         // 0x80071C6C
        else h = 1;
        if (h == 1) ++riding;
        const uint32_t node = e + 1088u;
        g.W32(tail[h] + 4u, node);
        g.W32(node, tail[h]);
        g.W32(node + 4u, heads[h]);
        g.W32(heads[h], node);
        tail[h] = node;
    }
    return riding;
}

// ------------------------------------------------------- the road arena, the route arena
namespace {

size_t RoadObjectBytes(const std::vector<uint8_t>& chunk) {
    size_t o = 0x8C;
    while (o + 8 <= chunk.size()) {
        const uint32_t s = static_cast<uint32_t>(chunk[o + 4]) | (static_cast<uint32_t>(chunk[o + 5]) << 8) |
                           (static_cast<uint32_t>(chunk[o + 6]) << 16) | (static_cast<uint32_t>(chunk[o + 7]) << 24);
        const uint8_t c0 = chunk[o];
        if (s < 8 || o + s > chunk.size() || !((c0 >= 'A' && c0 <= 'Z'))) break;
        o += s;
    }
    return o;
}

} // namespace

void RaceSession::BuildArena(const DiscImage& disc) {
    const std::vector<uint8_t> map = [&] {
        const auto f = disc.Find("DATA/ROAD" + std::to_string(world_.set) + ".MAP");
        if (!f) throw std::runtime_error("ROAD" + std::to_string(world_.set) + ".MAP is not on disc");
        return disc.ReadFile(*f);
    }();
    const char* error = nullptr;
    // with the ported route loader the map is RoadLoad's (MapLoad RASHCDI 0x8006ABC8 PORTED, run in StreamBuild after the resource table)
    if (!routePorted_ && arena_.LoadRoadMap(map.data(), map.size(), kArenaMapAt, &error) == 0)
        throw std::runtime_error(std::string("road arena: ") + (error ? error : "?"));
    if (streamPorted_) { // the road objects come and go with the original's streamer
        arenaFreeFrom_ = kArenaObjectsFrom;
        StreamBuild(disc);
        return;
    }
    std::set<uint16_t> roads;
    for (const RouteLeg& leg : world_.legs) roads.insert(static_cast<uint16_t>(leg.road));
    const std::map<uint32_t, std::vector<uint8_t>> objects = ReadRoadObjects(disc, world_.set);
    uint32_t at = kArenaObjectsFrom;
    size_t wanted = 0;
    for (const auto& kv : objects) {
        const rr::ChunkHeader h = rr::ParseChunkHeader(kv.second);
        bool onRoute = false;
        for (const rr::ResidencyWindow& w : h.windows)
            if (roads.count(w.road) != 0) onRoute = true;
        if (!onRoute) continue;
        ++wanted;
        const size_t bytes = RoadObjectBytes(kv.second);
        if (at + bytes > kArenaObjectsTo) continue;
        const uint32_t obj = arena_.LoadRoadObject(kv.second.data(), bytes, at, &error);
        if (obj == 0) throw std::runtime_error(std::string("road arena: ") + (error ? error : "?"));
        arenaObjectList_.push_back(obj);
        roadObjects_.push_back(ArenaRoadObject{kv.first, obj, h.windows});
        at = (at + static_cast<uint32_t>(bytes) + 15u) & ~15u;
    }
    arenaFreeFrom_ = at;
    arenaObjects_ = arenaObjectList_.size();
    if (arenaObjects_ < wanted)
        NoteSeam("the road arena holds " + std::to_string(arenaObjects_) + " of the " +
                 std::to_string(wanted) + " road objects this route touches: the rest did not fit");
    NoteSeam("the road arena is OURS where it is not the disc's: every road object of the route is "
             "resident at once (the original streams them)");
}

// The effect records, the per-bike chain heads, the fixed words the population passes read and the
// piece list, as every race capture holds them (`--poparenacheck` compares them).
void RaceSession::BuildPopulationArena() {
    GuestRam g(arena_.Ram(), kArenaGp);
    // loader2: RaceReset's FxReset RASHCDI 0x8005D5E4 PORTED freed the records and set the walk pointer; the
    // ported ModelBind's RegistryBind emptied every chain head; gp+204 = 80.0 is SLUS_010.53's own data word
    const bool l2 = rr::game::Loader2On() && rr::game::Loader2Totals().fxReset != 0u && OriginalBind();
    if (!l2) {
    // Twenty free records: word 0 = 0x3F (state 0, the chain link 63 = "end") - what every free record
    // of every race capture holds. A link of 0 would make EffectLink's chain walk loop for ever.
    for (uint32_t k = 0; k < 20u * rr::sim::kEffectRecordBytes; k += 4) g.W32(rr::sim::kEffectPool + k, 0);
    for (uint32_t k = 0; k < 20u; ++k) g.W32(rr::sim::kEffectPool + rr::sim::kEffectRecordBytes * k, 0x3Fu);
    g.W32(rr::sim::kEffectPoolWalkPtr, rr::sim::kEffectPool);   // 0x800D39B0 in all 18 captures
    g.W32(rr::sim::kEffectLastStamp, 0);
    for (size_t i = 0; i < bikes_.size(); ++i) g.W8(ArenaEntity(i) + 0x49u, 0xFF); // an empty effect chain
    // ... and each RIDER's: SpawnBike's ModelBind(rider, 1, cls, 1) (RASHCDI 0x800663E8, SHA-1 9a8b79d8...)
    // runs RegistryBind SLUS 0x8002FDEC, whose `sb v0,73(s0)` at 0x8002FF6C (v0 = 0xFF, SLUS SHA-1 67ed165a...) empties the chain; every
    // race capture (rr-race / rr-grid / rr-pack and the 14 vr_capture dumps) holds -1 on all 8 riders but a
    // live chain. Our zeroed pool-1 record left 0, i.e. "record 0 is my chain": the rider's first
    // EffectBurst (RiderOnGround 0x80098078, a tumbling player) linked record 0 to itself, and the next
    // EffectLink SLUS 0x800271CC walked that loop for ever (race 1/2 --autosteer --hold T, frame 1473).
    for (const RaceBike& b : bikes_) g.W8(b.ownerAddress + 0x49u, 0xFF);
    g.W32(kArenaGp + rr::sim::kPopVolumeRadiusGp, kVolumeRadius);
    } // !l2
    g.W32(rr::sim::kPopLiveSeen, 0);
    // The traffic and police switches, 1 in every capture, when the traffic arena is built (traffic_arena.h);
    // the spawner pass, the schedulers, the car spawner and the traffic pass then run PORTED (18).
    g.W32(kTrafficSwitch, trafficReady_ ? 1u : 0u);
    g.W32(rr::sim::kPoliceOn, trafficReady_ ? 1u : 0u);
    PieceSlot none[kPieceSlots];
    if (!streamPorted_) WritePieceList(g, none, -1); // else the PORTED stream start filled it
    if (!trafficReady_)
        NoteSeam("the traffic is OFF: *(0x8005ACC4) = 0 because the traffic arena was not built");
    if (l2)
        NoteSeam("each bike's and each rider's effect-chain head +0x49 = -1 by RegistryBind SLUS 0x8002FDEC (PORTED, SpawnBike's "
                 "ModelBind), the effect records 0x800D39B0 freed and their walk pointer 0x800D8068 set by FxReset RASHCDI "
                 "0x8005D5E4 (PORTED, RaceReset's call), gp+204 = 80.0 SLUS_010.53's data word");
    else
    NoteSeam("each bike's and each rider's effect-chain head +0x49 = -1 (RegistryBind SLUS 0x8002FDEC's store), the effect records 0x800D39B0 (free) with their walk "
             "pointer 0x800D8068 and gp+204 = 80.0 are seeded as every race capture holds them (BuildGrid and the "
             "loader are not ported; rrgame --poparenacheck compares them)");
}

// OURS, the road streamer's stand-in for the resident piece list the PORTED window test reads: a
// road object of the route is listed while one of its residency windows holds the player's road
// coordinate (the rule `--poparenacheck` checks against the captures), in the first free of the six
// slots, freed ({-1, 0, 0, 0}) when no window holds it any more; the last index is a high-water mark
// (in every capture it stays at 2 while slot 0 is free).
void RaceSession::RoadStreamPass(GuestRam& g) {
    const uint32_t word = g.U32(ArenaEntity(0) + 0x168u);
    const int32_t along = g.S32(ArenaEntity(0) + 0x170u) >> 16;
    if ((word >> 16) != 0) {
        for (int k = 0; k < kPieceSlots; ++k) log_.piecesListed += pieceSlots_[k] >= 0 ? 1u : 0u;
        return; // on a junction: the list stays as it is
    }
    // two players: player 2's road coordinate keeps its objects listed too (OURS, as the rule is)
    const uint32_t word2 = players_ == 2 ? g.U32(ArenaEntity(1) + 0x168u) : 0xFFFF0000u;
    const int32_t along2 = players_ == 2 ? g.S32(ArenaEntity(1) + 0x170u) >> 16 : 0;
    auto wanted = [&](const ArenaRoadObject& o) {
        for (const ResidencyWindow& w : o.windows) {
            if (w.road == (word & 0xFFFFu) && static_cast<int32_t>(w.from) <= along && along <= static_cast<int32_t>(w.to))
                return true;
            if ((word2 >> 16) == 0 && w.road == (word2 & 0xFFFFu) && static_cast<int32_t>(w.from) <= along2 &&
                along2 <= static_cast<int32_t>(w.to))
                return true;
        }
        return false;
    };
    for (int k = 0; k < kPieceSlots; ++k)
        if (pieceSlots_[k] >= 0 && !wanted(roadObjects_[static_cast<size_t>(pieceSlots_[k])])) pieceSlots_[k] = -1;
    for (size_t i = 0; i < roadObjects_.size(); ++i) {
        if (!wanted(roadObjects_[i])) continue;
        bool listed = false;
        for (int k = 0; k < kPieceSlots; ++k) listed = listed || pieceSlots_[k] == static_cast<int32_t>(i);
        if (listed) continue;
        int free = -1;
        for (int k = 0; k < kPieceSlots && free < 0; ++k)
            if (pieceSlots_[k] < 0) free = k;
        if (free < 0) {
            NoteSeam("the resident piece list's six slots were all taken: a road object was left out (ours)");
            break;
        }
        pieceSlots_[free] = static_cast<int32_t>(i);
        pieceHigh_ = std::max(pieceHigh_, free);
    }
    PieceSlot slots[kPieceSlots];
    for (int k = 0; k < kPieceSlots; ++k) {
        if (pieceSlots_[k] < 0) continue;
        const ArenaRoadObject& o = roadObjects_[static_cast<size_t>(pieceSlots_[k])];
        slots[k].id = static_cast<int32_t>(o.id);
        slots[k].object = o.address;
        ++log_.piecesListed;
    }
    WritePieceList(g, slots, pieceHigh_);
    NoteSeam("the resident piece list 0x800D4B10 the PORTED window test reads is OURS where the road streamer "
             "SLUS 0x80031784 would fill it: a route object is listed while a residency window holds the player's "
             "road coordinate (every road object of the route is nevertheless loaded in the arena)");
}

// CAMERA.CA and view record 0.
void RaceSession::BuildCameraArena(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kArenaGp);
    const uint32_t target = ArenaEntity(0);                     // *(0x8005B38C)
    const std::string name = rr::CameraFileName(players_, g.U32(target + 0xB4u));
    if (!rr::game::Loader2CamPorted()) { // RRJB_LOADER2_CAM=off: the transcription (the port reads it itself)
        std::string discName = name;
        std::replace(discName.begin(), discName.end(), '\\', '/');
        const auto f = disc.Find(discName);
        std::vector<uint8_t> file;
        if (f) file = disc.ReadFile(*f);
        if (file.size() < rr::kCameraBytesRead) throw std::runtime_error(name + " is missing or shorter than 224 bytes");
        g.WriteBlock(kCameraFileAt, file.data(), static_cast<uint32_t>(rr::kCameraBytesRead));
    }
    for (uint32_t k = 0; k < 2u * kViewStride; ++k) g.W8(kViewArray + k, 0);
    rr::sim::RoadRuntimeNative road;
    // CameraInit(target, view 0, 0x9F, mode 0, director = 0 (*(0x8005B220) is 0 at the call, so the
    // intro director is not asked for: RASHCDI 0x8006766C..0x8006768C), 1).
    const bool introWanted = (g.U8(g.U32(target + 0x43Cu)) & 0x20u) != 0 || (g.U8(g.U32(kGameStatePtr) + 4u) & 1u) != 0;
    // The director's shot table: RASHCDI 0x800626E4 copies 832 bytes of the race's
    // DATA\GAMEBIN1.DAT bundle section type 6 (the dispatcher 0x80061C24's entry 6) to 0x800D83B0 - rr-race's
    // table is bundle 3's (race 4). With it, the intro director runs as CameraInit starts it
    // (ShotSetup / SplineSlopes PORTED, camera_director.h).
    bool shots = false;
    if (const auto gb = disc.Find("DATA/GAMEBIN1.DAT")) {
        try {
            const std::vector<uint8_t> bin = disc.ReadFile(*gb);
            const rr::LevelBundle bundle = rr::ParseLevelBundle(bin, rr::LevelBundleIndexForRace(world_.raceId));
            if (const rr::LevelBundleSection* s = bundle.Find(6); s != nullptr && s->payload + 832u <= bin.size()) {
                g.WriteBlock(rr::sim::kShotTableAddr, bin.data() + s->payload, 832u);
                shots = true;
            }
        } catch (const std::exception&) {
        }
    }
    if (rr::game::Loader2CamPorted()) { // loader2_cam: CameraSetUp RASHCDI 0x80067564 PORTED (camera_setup.h)
        bool ok = false;
        NoteSeam(rr::game::PortedCameraSetUp(g, disc, kArenaStepSp, !shots, ok));
        if (!ok) throw std::runtime_error("the camera set-up (PORTED CameraSetUp) refused");
        if (g.U32(kViewArray + 0x304u) != 0)
            NoteSeam("CameraInit SLUS 0x8002F308 (PORTED) starts the INTRO director on the race's shot table from "
                     "GAMEBIN1.DAT section 6 - PORTED ShotSetup RASHCDG 0x800853E4 / SplineSlopes SLUS 0x8002F634 "
                     "(camera_director.h)");
        return;
    }
    InitViewRecord(g, kViewArray, target, kView0Handle, 0, 0, kArenaStepSp, road, !shots);
    if (g.Faulted()) {
        g.ClearFault();
        NoteSeam("view record 0's CameraInit met an address the console would fault on");
    }
    if (introWanted && !shots)
        NoteSeam("CameraInit SLUS 0x8002F308 would start the INTRO director here, but the race's level bundle has no "
                 "shot table (GAMEBIN1.DAT section 6): the chase camera starts at once - OURS");
    else if (introWanted)
        NoteSeam("CameraInit SLUS 0x8002F308 starts the INTRO director (the player's runtime record byte 0 bit 5 on "
                 "the grid: +0x304 = 1, mode 7) on the race's shot table from GAMEBIN1.DAT section 6 - PORTED "
                 "ShotSetup RASHCDG 0x800853E4 / SplineSlopes SLUS 0x8002F634 (camera_director.h)");
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the camera is the PORTED ViewUpdate RASHCDG 0x800881B4 on view record 0 (0x800CD898), initialised "
                  "as CameraInit SLUS 0x8002F308 does (transcribed, checked by rrgame --camarenacheck), with %s's first "
                  "224 bytes at 0x800CD7B8; view record 0's +0x21C mode %u, +0x224 0x%X, +0x304 %u",
                  name.c_str(), g.U32(kViewArray + 0x21Cu), g.U32(kViewArray + 0x224u), g.U32(kViewArray + 0x304u));
    NoteSeam(b);
    MpSetupCamera(!shots); // view record 1 when there is a second player
}

// CameraSetUp RASHCDI 0x80067564's second arm (0x80067710..0x80067764), transcribed from our listing:
// when *(0x8005B21C) (player 2's bike) is set, CameraInit(target, view 0 + 1132, 0x9E, mode 0, the same
// director flag, 1) - the target the cop mission's (s0) or player 2's bike - then view 0 +0x30 = 0x8000,
// view 1 +0x2C = 0x8000, view 1 +0x30 = 0 and view 1 +0x224 |= 0x01000000.
void RaceSession::MpSetupCamera(bool skipIntro) {
    GuestRam g(arena_.Ram(), kArenaGp);
    const uint32_t p2 = g.U32(kPlayer2Bike);
    if (p2 == 0) return;
    rr::sim::RoadRuntimeNative road;
    const uint32_t v1 = kViewArray + kViewStride;
    InitViewRecord(g, v1, p2, static_cast<uint16_t>(kView0Handle - 1u), 0, 0, kArenaStepSp, road, skipIntro);
    g.W32(kViewArray + 0x30u, 0x8000u);
    g.W32(v1 + 0x2Cu, 0x8000u);
    g.W32(v1 + 0x30u, 0);
    g.W32(v1 + 0x224u, g.U32(v1 + 0x224u) | 0x01000000u);
    if (g.Faulted()) {
        g.ClearFault();
        NoteSeam("view record 1's CameraInit met an address the console would fault on");
    }
    char b[200];
    std::snprintf(b, sizeof(b),
                  "two players: view record 1 (0x800CDD04) is player 2's camera, initialised as CameraSetUp RASHCDI "
                  "0x80067710 does (CameraInit handle 0x9E); +0x21C mode %u, +0x224 0x%X",
                  g.U32(v1 + 0x21Cu), g.U32(v1 + 0x224u));
    NoteSeam(b);
}

void RaceSession::ViewCamera(float eye[3], float look[3], float up[3], int p) const {
    const uint8_t* v = At(kViewArray + kViewStride * static_cast<uint32_t>(p == 1 && players_ == 2 ? 1 : 0));
    int16_t row[3];
    for (uint32_t k = 0; k < 3; ++k) {
        eye[k] = static_cast<float>(static_cast<double>(ReadS32(v, 0xB8 + 4u * k)) / 65536.0);
        look[k] = static_cast<float>(static_cast<double>(ReadS32(v, 0x22C + 4u * k)) / 65536.0);
        row[k] = ReadS16(v, 0x1B6 + 2u * k);
    }
    const float sense = row[1] <= 0 ? 1.0f : -1.0f;
    for (uint32_t k = 0; k < 3; ++k) up[k] = sense * static_cast<float>(row[k]) / 4096.0f;
    if (row[0] == 0 && row[1] == 0 && row[2] == 0) { up[0] = 0.0f; up[1] = -1.0f; up[2] = 0.0f; }
}

bool RaceSession::BikeLive(size_t index) const { return ReadS16(bikes_[index].entity.p, 0x140) != 0; }

void RaceSession::BuildRouteArena(const DiscImage& disc) {
    if (routePorted_) return; // built by the ported loaders in StreamBuild (BuildRouteArenaPorted)
    GuestRam g(arena_.Ram(), kArenaGp);
    const std::string txt = ReadText(disc, "DATA/ROADGRF" + std::to_string(world_.set) + ".TXT");
    const auto grfFile = disc.Find("DATA/STREAM" + std::to_string(world_.set) + ".GRF");
    RaceBlock block;
    if (txt.empty() || !grfFile || !FindRaceBlock(txt, world_.raceId, 0, block))
        throw std::runtime_error("route arena: race " + std::to_string(world_.raceId) + " has no block in ROADGRF" +
                                 std::to_string(world_.set) + ".TXT");
    const std::vector<uint8_t> grf = disc.ReadFile(*grfFile);
    // 1. The race graph at *(gp+472), relocated.
    const std::vector<uint8_t> graph = RelocatedGraph(grf, kArenaGraph);
    g.WriteBlock(kArenaGraph, graph.data(), static_cast<uint32_t>(graph.size()));
    g.W32(kArenaGp + rr::sim::kRaceGraphGp, kArenaGraph);
    // 2. The route block, where rr-race has it when it fits under the map, else at an address of ours.
    const uint32_t size = 56u + 120u * static_cast<uint32_t>(std::max(block.raceInts, 0) + 1);
    const uint32_t alloc = (kArenaRouteBlock + size <= kArenaMapAt) ? kArenaRouteBlock : kArenaRouteFallback;
    std::vector<uint8_t> bytes, header;
    std::string error;
    if (!BuildRouteBlock(block, grf, alloc, 0, false, bytes, header, error))
        throw std::runtime_error("route arena: " + error);
    g.WriteBlock(alloc, bytes.data(), static_cast<uint32_t>(bytes.size()));
    g.WriteBlock(kRouteHeader, header.data(), static_cast<uint32_t>(header.size()));
    routeCount_ = block.raceInts;
    routeRecords_ = alloc + 56u;
    finishRecord_ = alloc + 28u;
    char b[480];
    std::snprintf(b, sizeof(b),
                  "the route arena is the loader's, rebuilt from the disc (rrgame --routearenacheck): STREAM%d.GRF "
                  "at *(gp+472) = 0x%08X, and ROADGRF%d.TXT race %d parsed by RASHCDI 0x8006A0C8's rules into "
                  "%d route record(s) + the start record at 0x%08X and the finish record at 0x%08X. OURS: the "
                  "header's text pointer +0x1C is 0 (the text buffer is transient)",
                  world_.set, kArenaGraph, world_.set, world_.raceId, routeCount_, routeRecords_, finishRecord_);
    NoteSeam(b);
}

// The ported route loader (route_product.h): GrfLoad SLUS 0x800244E0 and RoadLoad RASHCDI 0x8006AC6C PORTED on the arena - the
// race graph, the route block (the parser 0x8006A0C8) and ROAD<n>.MAP (0x8006ABC8) where the original's heap puts them.
// The control counters: the transcription's bytes (BuildRouteBlock, RelocatedGraph, RoadArena::LoadRoadMap) at the
// ported addresses, compared byte for byte (the header's text pointer, which the transcription leaves 0, excepted).
void RaceSession::BuildRouteArenaPorted(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kArenaGp);
    std::string why;
    if (!rr::game::RouteLoadPorted(g, disc, world_.set, world_.raceId, rr::game::kLoaderSp, why))
        throw std::runtime_error("route arena: " + why);
    rr::game::RouteCounts& t = rr::game::RouteTotals();
    const uint32_t alloc = g.U32(kRouteHeader);
    routeCount_ = g.S16(kRouteHeader + 18u);
    routeRecords_ = alloc + 56u;
    finishRecord_ = alloc + 28u;
    const std::string txt = ReadText(disc, "DATA/ROADGRF" + std::to_string(world_.set) + ".TXT");
    const auto grfFile = disc.Find("DATA/STREAM" + std::to_string(world_.set) + ".GRF");
    const auto mapFile = disc.Find("DATA/ROAD" + std::to_string(world_.set) + ".MAP");
    RaceBlock block;
    std::vector<uint8_t> bytes, header;
    std::string error;
    if (!txt.empty() && grfFile && mapFile && FindRaceBlock(txt, world_.raceId, 0, block)) {
        const std::vector<uint8_t> grf = disc.ReadFile(*grfFile);
        auto differ = [&](uint32_t at, const std::vector<uint8_t>& want, size_t skipFrom, size_t skipTo) {
            size_t d = 0;
            for (size_t i = 0; i < want.size(); ++i) {
                if (i >= skipFrom && i < skipTo) continue;
                ++t.compared;
                if (g.U8(at + static_cast<uint32_t>(i)) != want[i]) ++d;
            }
            return d;
        };
        if (BuildRouteBlock(block, grf, alloc, 0, false, bytes, header, error)) {
            t.routeDiffer = differ(alloc, bytes, 0, 0);
            t.headerDiffer = differ(kRouteHeader, header, 0x1C, 0x20);
        }
        const uint32_t graphAt = g.U32(kArenaGp + rr::sim::kRaceGraphGp);
        t.graphDiffer = differ(graphAt, RelocatedGraph(grf, graphAt), 0, 0);
        const std::vector<uint8_t> map = disc.ReadFile(*mapFile);
        const uint32_t mapAt = g.U32(rr::sim::kRpMapFile);
        rr::sim::RoadArena control;
        const char* err = nullptr;
        if (control.LoadRoadMap(map.data(), map.size(), mapAt, &err) != 0) {
            std::vector<uint8_t> want(map.size());
            GuestRam cg(control.Ram(), kArenaGp);
            cg.ReadBlock(mapAt, want.data(), static_cast<uint32_t>(want.size()));
            t.mapDiffer = differ(mapAt, want, 0, 0);
            if (cg.U32(rr::sim::kRoadGraphPtr) != g.U32(rr::sim::kRoadGraphPtr)) ++t.mapDiffer;
        } else {
            ++t.mapDiffer;
        }
    } else {
        NoteSeam("the route: the transcription's control could not be built for race " + std::to_string(world_.raceId));
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the route arena is the ORIGINAL loaders', PORTED (route_product.h): STREAM%d.GRF at *(gp+472) = 0x%08X, "
                  "ROADGRF%d.TXT race %d parsed by RASHCDI 0x8006A0C8 into %d route record(s) + the start record at 0x%08X "
                  "and the finish record at 0x%08X, ROAD%d.MAP at 0x%08X (G 0x%08X)",
                  world_.set, g.U32(kArenaGp + rr::sim::kRaceGraphGp), world_.set, world_.raceId, routeCount_, routeRecords_,
                  finishRecord_, world_.set, g.U32(rr::sim::kRpMapFile), g.U32(rr::sim::kRoadGraphPtr));
    NoteSeam(b);
}

// The stat array: blocks 0..2 = the first 1344 bytes of LEVEL<bank+1>.PH, block 3
// the first 448 bytes of the player's bike .PH, e[+0x22C] = *(0x8005B248) + 448 * idx.
void RaceSession::LoadStatBlocks(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kArenaGp);
    const int32_t bank = static_cast<int32_t>(gameState_[0x3C]);
    const std::string level = "DATA/LEVEL" + std::to_string(bank + 1) + ".PH";
    // The bike the front end's chooser picked (game_state+0x48 -> its .PH), else the default.
    const rr::shell::Handover& fh = rr::shell::PendingHandover();
    const std::string playerPh = (fh.active && !fh.playerBikePh.empty()) ? fh.playerBikePh : "DATA/CRUISEA1.PH";
    std::vector<uint8_t> levelBytes, playerBytes;
    if (const auto f = disc.Find(level)) levelBytes = disc.ReadFile(*f);
    if (const auto f = disc.Find(playerPh)) playerBytes = disc.ReadFile(*f);
    if (levelBytes.size() < 3u * kStatBlockBytes)
        NoteSeam(level + " is missing or shorter than 1344 bytes - the opponents' stat blocks are "
                         "zero where the file ends");
    if (playerBytes.size() < kStatBlockBytes)
        NoteSeam(playerPh + " is missing or shorter than 448 bytes");
    // BuildGrid allocates 448 * (players == 1 ? 4 : 5) bytes: with two players block
    // 4 is player 2's bike (game_state+0x4C). The array's block ends where pool 0's starts, as BuildGrid's
    // mallocs leave it (mp_arena.h); RRJB_MPARENA=off: one of the free 16 KiB cell buffers.
    const uint32_t blocks = players_ == 2 ? 5u : 4u;
    uint32_t statArray = kArenaStatArray;
    if (players_ == 2 && rr::game::MpArenaOriginal()) statArray = rr::game::StatArrayAt(2u);
    else if (players_ == 2 && !freeCellBuffers_.empty()) {
        statArray = freeCellBuffers_.back();
        freeCellBuffers_.pop_back();
        --cellBuffersTotal_;
    }
    std::vector<uint8_t> array(blocks * kStatBlockBytes, 0);
    std::copy_n(levelBytes.begin(), std::min<size_t>(levelBytes.size(), 3u * kStatBlockBytes), array.begin());
    std::copy_n(playerBytes.begin(), std::min<size_t>(playerBytes.size(), kStatBlockBytes),
                array.begin() + 3u * kStatBlockBytes);
    if (blocks == 5u) {
        const std::string p2 = player2Ph_.empty() ? std::string("DATA/CRUISEA1.PH") : player2Ph_;
        std::vector<uint8_t> p2Bytes;
        if (const auto f = disc.Find(p2)) p2Bytes = disc.ReadFile(*f);
        if (p2Bytes.size() < kStatBlockBytes) NoteSeam(p2 + " (player 2's bike) is missing or shorter than 448 bytes");
        std::copy_n(p2Bytes.begin(), std::min<size_t>(p2Bytes.size(), kStatBlockBytes), array.begin() + 4u * kStatBlockBytes);
        NoteSeam("two players: stat block 4 is player 2's bike " + p2 + "; the five-block array is at 0x" +
                 [&] { char h[12]; std::snprintf(h, sizeof(h), "%08X", statArray); return std::string(h); }() +
                 (statArray == rr::game::StatArrayAt(2u) ? " (the original's: its block ends at pool 0's, mp_arena.h)"
                                                          : " (OURS: a free cell buffer, RRJB_MPARENA=off)"));
    }
    g.WriteBlock(statArray, array.data(), static_cast<uint32_t>(array.size()));
    g.W32(kStatArrayPtr, statArray);
    for (RaceBike& bike : bikes_) {
        const int32_t p = static_cast<int32_t>(&bike - bikes_.data());
        bike.statsBlock = bike.isPlayer ? 3 + p : static_cast<int32_t>(bike.riderDef[0x01] & 0x0Fu);
        bike.statsAddress = statArray + kStatBlockBytes * static_cast<uint32_t>(bike.statsBlock);
        bike.stats = At(bike.statsAddress);
        WriteS32(bike.entity.p, 0x22C, static_cast<int32_t>(bike.statsAddress));
    }
    NoteSeam("the stat blocks entity[+0x22C] are the race loader's: " + level +
             " blocks 0..2 and " + playerPh + " block 3; OURS: which bike .PH the player rides "
             "(the frontend is not ported) and every opponent's class nibble riderDef[+1] = 1");
}

void RaceSession::LoadEnvTable(const DiscImage& disc) {
    // In all 19 captured RAM images the bytes at 0x800D38E0 equal DATA\ENV.EN for exactly the first
    // 0xCC bytes.
    constexpr uint32_t kEnvBytes = 0xCC;
    std::vector<uint8_t> env;
    if (const auto f = disc.Find("DATA/ENV.EN")) env = disc.ReadFile(*f);
    if (env.size() < kEnvBytes) {
        NoteSeam("DATA\\ENV.EN is missing or short - the step's per-surface table is zero");
        env.resize(kEnvBytes, 0);
    }
    GuestRam g(arena_.Ram(), kArenaGp);
    g.WriteBlock(rr::sim::kEnvTable, env.data(), kEnvBytes);
}

// ------------------------------------------------------- the rider animation arena
void RaceSession::BuildAnimArena(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kArenaGp);
    const uint32_t desc = rr::sim::kAnimDescriptor;
    // RASHCDI 0x8005D130: capacity 10, objects/programs/+8/count 0, slots 0x800CF5D8, used 0, every
    // slot's first four bytes cleared.
    for (uint32_t k = 0; k < 8; ++k) g.W32(desc + 4u * k, 0);
    g.W32(desc + 24u, kAnimSlotCapacity);
    g.W32(desc + 16u, kAnimSlots);
    for (uint32_t k = 0; k < kAnimSlotCapacity; ++k) {
        g.W32(kAnimSlots + 12u * k, 0);
        g.W32(kAnimSlots + 12u * k + 4u, 0);
        g.W32(kAnimSlots + 12u * k + 8u, 0);
    }
    // eight words: the model registry starts at 0x800CE1B0 (RaceReset's ModelTablesInit clears it there; every
    // capture holds model 100 in its slot 0) - clearing sixteen would wipe registry slots 0 and 1
    for (uint32_t k = 0; k < 8u; ++k) g.W32(rr::sim::kAnimBankTable + 4u * k, 0);
    // The six banks, in the loader's slot order, stepping `(size + 8) & ~7` per allocation.
    uint32_t at = kArenaAnimFiles;
    uint32_t loaded = 0;
    for (const rr::game::AnimBankName& bf : rr::game::AnimBankSet(gameState_[4])) { // by race type (jail_session.h)
        const auto e = disc.Find(std::string("DATA/") + bf.name + ".PSX");
        if (!e) {
            NoteSeam(std::string("DATA\\") + bf.name + ".PSX is not on the disc: its stance bank is empty");
            continue;
        }
        const std::vector<uint8_t> f = disc.ReadFile(*e);
        const std::vector<uint32_t> chain = Dmd3Chain(f);
        const uint32_t mainAt = at; // the passenger bank goes to the area JailReserve kept (jail_session.cpp)
        if (bf.stanceBank == 6u && jailBankAt_ != 0u) at = jailBankAt_;
        const uint32_t table = at + ((static_cast<uint32_t>(f.size()) + 8u) & ~7u);
        const uint32_t next = table + ((4u * static_cast<uint32_t>(chain.size()) + 8u) & ~7u);
        if (next > kArenaAnimFilesTo) {
            NoteSeam(std::string("the animation arena has no room for ") + bf.name + ".PSX");
            continue;
        }
        const std::vector<uint8_t> fixed = FixedBank(f, at, false);
        g.WriteBlock(at, fixed.data(), static_cast<uint32_t>(fixed.size()));
        for (size_t i = 0; i < chain.size(); ++i) g.W32(table + 4u * static_cast<uint32_t>(i), at + chain[i]);
        const uint32_t slot = kAnimSlots + 12u * loaded;
        g.W8(slot + 0u, 0);
        g.W8(slot + 1u, 1);
        g.W16(slot + 2u, static_cast<uint16_t>(chain.size()));
        g.W32(slot + 4u, table);
        g.W32(slot + 8u, at);
        g.W32(rr::sim::kAnimBankTable + 4u * bf.stanceBank, slot);
        ++loaded;
        at = (bf.stanceBank == 6u && jailBankAt_ != 0u) ? mainAt : next;
    }
    g.W32(desc + 20u, loaded);
    // The objects and programs: the ORIGINAL's (animobj_product.h) - RASHCDI 0x8005D1A0 once pool 1 is
    // complete and SLUS 0x800119C0's ViewSlot loop in StartStances (animobj_product.h). RRJB_ANIMOBJ=off (the
    // negative control) keeps the session's own wiring below.
    const bool ourObjects = !rr::game::AnimObjOn();
    if (ourObjects) {
    // The objects and programs, one per rider (rr-race's addresses; 23 there, one per rider here).
    animObjects_ = kArenaAnimObjects;
    animPrograms_ = kArenaAnimPrograms;
    g.W32(desc + 0u, animObjects_);
    g.W32(desc + 4u, animPrograms_);
    // +0x0C the object count AnimationPass walks and +0x08 the objects in use - the word the object
    // release at RASHCDI 0x800678E8 / RASHCDG 0x800959E4 decrements and the allocator SLUS 0x80012884
    // counts against +0x0C (AnimationPass does nothing while it is 0). Both OURS: one per rider.
    // The capacity is rr-race's 23 (OURS: the loader's rule is not read) so that the allocator SLUS
    // 0x80012884 has free objects for the animated re-seat 0x8009277C (a bike's climb object) and a
    // released cop's rider; the objects past the riders' are free (+0x24 = 0), their programs wired.
    constexpr uint32_t kAnimObjectCapacity = 23;
    const uint32_t objects = std::max<uint32_t>(kAnimObjectCapacity, static_cast<uint32_t>(bikes_.size()));
    g.W32(desc + 12u, objects);
    g.W32(desc + 8u, static_cast<uint32_t>(bikes_.size()));
    for (uint32_t i = static_cast<uint32_t>(bikes_.size()); i < objects; ++i) {
        const uint32_t a = animObjects_ + rr::sim::kAnimObjectBytes * i;
        const uint32_t prog = animPrograms_ + rr::sim::kAnimProgramBytes * i;
        for (uint32_t k = 0; k < rr::sim::kAnimObjectBytes; k += 4) g.W32(a + k, 0);
        for (uint32_t k = 0; k < rr::sim::kAnimProgramBytes; k += 4) g.W32(prog + k, 0);
        g.W32(a + rr::sim::animf::kProgram, prog);
        g.W32(a + rr::sim::animf::kLength, 5);
        g.W32(a + rr::sim::animf::kBank, g.U32(rr::sim::kAnimBankTable));
    }
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint32_t a = animObjects_ + rr::sim::kAnimObjectBytes * static_cast<uint32_t>(i);
        const uint32_t prog = animPrograms_ + rr::sim::kAnimProgramBytes * static_cast<uint32_t>(i);
        for (uint32_t k = 0; k < rr::sim::kAnimObjectBytes; k += 4) g.W32(a + k, 0);
        for (uint32_t k = 0; k < rr::sim::kAnimProgramBytes; k += 4) g.W32(prog + k, 0);
        g.W32(a + rr::sim::animf::kOwner, bikes_[i].ownerAddress);
        g.W32(a + rr::sim::animf::kProgram, prog);
        g.W32(a + rr::sim::animf::kLength, 5);
        g.W32(a + rr::sim::animf::kBank, g.U32(rr::sim::kAnimBankTable)); // stance bank 0 (riders)
        WriteS32(bikes_[i].owner.p, 0x21C, static_cast<int32_t>(a));
    }
    for (size_t k = 0; k < passengers_.size() && bikes_.size() + k < objects; ++k) { // a passenger's rider, OURS as the others' (jail_session.cpp)
        const uint32_t a = animObjects_ + rr::sim::kAnimObjectBytes * static_cast<uint32_t>(bikes_.size() + k);
        g.W32(a + rr::sim::animf::kOwner, passengers_[k].rider);
        g.W32(passengers_[k].rider + 0x21Cu, a);
        g.W32(desc + 8u, g.U32(desc + 8u) + 1u);
    }
    } // ourObjects
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the rider animation arena is the loader's, rebuilt from the disc (rrgame --animarenacheck): "
                  "%u bank file(s) ANIMTBL2/3/B/1/W/P with the +0x14 fix-up, their clip tables, the slot records "
                  "0x800CF5D8 and the bank table 0x800CE190; %s, the short banks never",
                  loaded, ourObjects ? "OURS (RRJB_ANIMOBJ=off): one object and program per rider at rr-race's addresses, "
                                       "+0x24 left 0" : "the objects the loader's RASHCDI 0x8005D1A0 makes (PORTED, below)");
    NoteSeam(b);
    {   // rider_pose.h: each rider's model and part slots, which the ported Pose writes
        std::vector<rr::game::PoseBinding> pb;
        for (const RaceBike& bk : bikes_) pb.push_back({bk.entityAddress, bk.ownerAddress});
        for (const Passenger& ps : passengers_) pb.push_back({ps.bike, ps.rider}); // jail_session.cpp
        NoteSeam(rr::game::BuildPoseArena(g, disc, at, pb, posePairsAt_, OriginalBind()));
    }
}

// Each rider's riding stance, started through the PORTED stance event 0x800C4550 on its PORTED
// clock. OURS: that the grid starts every rider in stance 11 with p = 0 - the stance and the hard
// start (op 1, flags 0x40, keys 0..1 of clip 9) every capture's player rider holds; the code that
// puts the riders there at the start of a race (BuildGrid / the GO event) is not read.
void RaceSession::StartStances() {
    GuestRam g(arena_.Ram(), kArenaGp);
    ProductRiderSeams seams(this, g);
    rr::sim::StanceLayer layer(g, seams);
    size_t started = 0;
    if (gridPorted_ && rr::game::AnimObjOn()) {
        // SLUS 0x800119C0's per-bike loop, PORTED whole (anim_objects.h): ViewSlot gives each rider (and a
        // sidecar's passenger rider) the loader's object, BankSwitch, the stance event 6 / 4 through the layer
        const std::string line = rr::game::AnimObjectsStart(g, kArenaStepSp, [&](uint32_t ev, uint32_t r, uint32_t p) {
            layer.Event(ev, r, p);
            if (layer.Failed() || g.Faulted()) {
                g.ClearFault();
                return false;
            }
            return true;
        });
        std::string st;
        for (const RaceBike& bike : bikes_) st += (st.empty() ? "" : " ") + std::to_string(g.U16(bike.ownerAddress + 0x220u));
        NoteSeam(line + "; stances now: " + st);
        return;
    }
    if (!gridPorted_ && rr::game::AnimObjOn()) { // OURS (RRJB_GRID=ours): the riders take the loader's objects in slot order
        for (const RaceBike& bike : bikes_) g.W32(bike.ownerAddress + 0x21Cu, rr::sim::ViewSlot(g, rr::sim::kAnimDescriptor, bike.ownerAddress));
        for (const Passenger& ps : passengers_)
            if (ps.rider != 0u) g.W32(ps.rider + 0x21Cu, rr::sim::ViewSlot(g, rr::sim::kAnimDescriptor, ps.rider));
        NoteSeam("each rider's animation object is the loader's (RASHCDI 0x8005D1A0, PORTED), handed out by ViewSlot "
                 "0x80012884 in slot order (OURS order: RRJB_GRID=ours / --start does not run 0x800119C0's loop): " +
                 std::to_string(g.U32(rr::sim::kAnimDescriptor + 8u)) + " of " +
                 std::to_string(g.U32(rr::sim::kAnimDescriptor + 12u)) + " in use");
    }
    if (gridPorted_) {
        // SLUS 0x800119C0's per-bike loop (0x80011A10..0x80011A5C), pool 0 in slot order: the rider's
        // animation object (OURS: the session's, BuildAnimArena), BankSwitch SLUS 0x80012858(object, the bank
        // table's entry 0) when that entry is set, then the stance event 0x800C4550(rider handle & 1 ? 4 : 6,
        // rider, 1) - both PORTED.
        size_t switched = 0, even = 0, odd = 0;
        for (RaceBike& bike : bikes_) {
            const uint32_t r = bike.ownerAddress;
            const uint32_t obj = g.U32(r + 0x21Cu), bank = g.U32(rr::sim::kAnimBankTable);
            if (bank != 0u && obj != 0u) {
                layer.anim().BankSwitch(obj, bank);
                ++switched;
            }
            const uint32_t ev = (g.U16(r + 0xACu) & 1u) ? 4u : 6u;
            layer.Event(ev, r, 1);
            (ev == 6u ? even : odd) += 1;
        }
        if (layer.Failed() || g.Faulted()) {
            g.ClearFault();
            NoteSeam("the PORTED stance layer declined a rider's starting stance (the view faulted)");
        }
        std::string st;
        for (const RaceBike& bike : bikes_) st += (st.empty() ? "" : " ") + std::to_string(g.U16(bike.ownerAddress + 0x220u));
        NoteSeam("each rider's starting stance is the ORIGINAL's: SLUS 0x800119C0's loop, BankSwitch 0x80012858 on " +
                 std::to_string(switched) + " rider(s), the stance event 0x800C4550(6 / 4, rider, 1) for " +
                 std::to_string(even) + " even / " + std::to_string(odd) + " odd rider handle(s) (PORTED); stances now: " + st);
        return;
    }
    for (RaceBike& bike : bikes_) {
        layer.Event(11, bike.ownerAddress, 0);
        if (g.U16(bike.ownerAddress + 0x220u) == 11) ++started;
    }
    if (layer.Failed() || g.Faulted()) {
        g.ClearFault();
        NoteSeam("the PORTED stance layer declined a rider's starting stance (the view faulted)");
    }
    char b[260];
    std::snprintf(b, sizeof(b),
                  "each rider's starting stance is OURS: the PORTED stance event 0x800C4550(11, rider, 0) at the "
                  "grid put %zu of %zu riders into stance 11 (what every capture's player rider holds; the code "
                  "that does this in the original is not read)",
                  started, bikes_.size());
    NoteSeam(b);
}

// Each bike's road cursor e[+0x148..+0x167], on the arena slice nearest its box centre. OURS: the
// original's cursors are seeded by BuildGrid (RASHCDI 0x80067B00), which is not ported.
void RaceSession::SeatCursors() {
    GuestRam g(arena_.Ram(), kArenaGp);
    for (size_t i = 0; i < bikes_.size(); ++i) {
        RaceBike& bike = bikes_[i];
        int32_t p[3];
        for (uint32_t k = 0; k < 3; ++k) p[k] = ReadS32(bike.entity.p, ent::kObbCentre + 4u * k);
        uint32_t bestObj = 0, bestSlice = 0;
        int32_t bestIndex = 0;
        double best = 1e300;
        for (uint32_t obj : arenaObjectList_) {
            const int32_t n = g.S16(obj + 0x16);
            const uint32_t slct = g.U32(obj + 0x34);
            for (int32_t k = 0; k < n; ++k) {
                const uint32_t s = slct + 52u * static_cast<uint32_t>(k);
                double d = 0.0;
                for (uint32_t c = 0; c < 3; ++c) {
                    const double diff = static_cast<double>(g.S32(s + 20u + 4u * c)) - p[c];
                    d += diff * diff;
                }
                if (d < best) { best = d; bestObj = obj; bestSlice = s; bestIndex = k; }
            }
        }
        if (bestObj == 0) continue;
        const int32_t nsub = g.S16(bestObj + 0x14);
        const uint32_t subt = g.U32(bestObj + 0x30);
        int32_t subIndex = 0;
        for (int32_t s = 0; s < nsub; ++s) {
            const int32_t first = g.S16(subt + 28u * static_cast<uint32_t>(s) + 8u);
            const int32_t count = g.S16(subt + 28u * static_cast<uint32_t>(s) + 10u);
            if (bestIndex >= first && bestIndex < first + count) subIndex = s;
        }
        const int32_t npc = g.S16(bestObj + 0x12);
        const uint32_t grpt = g.U32(bestObj + 0x2C);
        int32_t pieceIndex = 0, pieceFirst = -1;
        for (int32_t q = 0; q < npc; ++q) {
            const int32_t f = g.S16(grpt + 32u * static_cast<uint32_t>(q) + 20u);
            if (f <= subIndex && f > pieceFirst) { pieceFirst = f; pieceIndex = q; }
        }
        WriteS32(bike.entity.p, 0x148, static_cast<int32_t>(bestObj));
        WriteS32(bike.entity.p, 0x14C, static_cast<int32_t>(grpt + 32u * static_cast<uint32_t>(pieceIndex)));
        WriteS32(bike.entity.p, 0x150, static_cast<int32_t>(subt + 28u * static_cast<uint32_t>(subIndex)));
        WriteS32(bike.entity.p, 0x154, static_cast<int32_t>(bestSlice));
        for (size_t k = 0x158; k < 0x168; k += 4) WriteS32(bike.entity.p, k, 0);
        // The PORTED RoadTrack SLUS 0x8003701C then does from the cursor exactly what the original
        // does each frame: the search, RoadProject, RoadPosition (+0x168/+0x16C/+0x170).
        rr::sim::RoadTrack(g, bike.entityAddress, kArenaStepSp);
    }
    if (g.Faulted()) {
        g.ClearFault();
        NoteSeam("the PORTED RoadTrack met an address the console would fault on while seating the grid");
    }
    // `--start`: a field placed part way down the route has not crossed the junctions before it, and
    // the PORTED RouteBind only ever steps a binding from junction to junction - so each bike is bound
    // (OURS, a development control) to the route record it would hold having got there: the one whose
    // route road (+0x54, the PORTED RouteLegHasRoad SLUS 0x8003F580) is the road the PORTED RoadTrack
    // put it on, with its slot bit in that record's +0x76 as RouteBindFirst sets it. From the line
    // (`--start 0`) nothing is seeded: RouteBindFirst binds every bike to the start record itself.
    if (startDistance_ > 0.0) {
        size_t seeded = 0;
        for (size_t i = 0; i < bikes_.size(); ++i) {
            const uint32_t e = ArenaEntity(i);
            const uint32_t word = g.U32(e + 0x168);
            if ((word >> 16) != 0) continue;
            for (int32_t r = 0; r < routeCount_; ++r) {
                const uint32_t rec = routeRecords_ + 120u * static_cast<uint32_t>(r);
                if (rr::sim::RouteLegHasRoad(g, rec, static_cast<int32_t>(word & 0xFFFFu)) == 0) continue;
                g.W32(e + 0x1AC, rec);
                g.W16(rec + 0x76, static_cast<uint16_t>(g.U16(rec + 0x76) | (1u << (i & 31u))));
                ++seeded;
                break;
            }
        }
        char b[300];
        std::snprintf(b, sizeof(b),
                      "--start: %zu of %zu bikes were bound by us to the route record whose route road is the "
                      "road they start on (the PORTED RouteBind steps a binding only at junctions, which a field "
                      "placed part way down the route has not crossed)",
                      seeded, bikes_.size());
        NoteSeam(b);
    }
    NoteSeam("each bike's road cursor is seeded by us on the arena slice nearest its box centre (RRJB_GRID=ours / "
             "--start: the PORTED BuildGrid is not run); from there the PORTED RoadTrack keeps it");
}

// ------------------------------------------------------- the scene cells the ground query walks
namespace {

uint32_t LoadU32(const std::vector<uint8_t>& v, size_t at) { return static_cast<uint32_t>(ReadS32(v, at)); }

uint32_t PlaceCellChunk(GuestRam& g, uint32_t at, const std::vector<uint8_t>& chunk, uint32_t region7,
                        bool relocate = true) {
    g.WriteBlock(at, chunk.data(), static_cast<uint32_t>(chunk.size()));
    const uint32_t nWords = LoadU32(chunk, 0x20);
    const uint32_t bodyOff = 0x20u + 4u * nWords;
    for (uint32_t k = 0; k < 8 && relocate; ++k)
        g.W32(at + bodyOff + 0x20u + 4u * k, at + 0x20u + LoadU32(chunk, bodyOff + 0x20u + 4u * k));
    if ((chunk[3] >> 4) == 8) g.W32(at + bodyOff + 0x3Cu, region7);
    return at + bodyOff;
}

struct StreamCellSet {
    std::vector<std::vector<uint8_t>> cells;
    std::map<uint32_t, std::vector<uint8_t>> nine;
};
StreamCellSet ReadStreamCells(const DiscImage& disc, int set,
                              const std::function<bool(const rr::ChunkHeader&)>& keep) {
    StreamCellSet out;
    const auto stream = disc.Find("DATA/STREAM" + std::to_string(set) + ".STR");
    if (!stream) throw std::runtime_error("STREAM" + std::to_string(set) + ".STR is not on disc");
    std::vector<uint8_t> head(rr::kChunkSize, 0);
    std::set<uint32_t> seen, wantNine;
    std::vector<uint64_t> cellAt;
    std::map<uint32_t, uint64_t> nineAt;
    for (uint64_t offset = 0; offset + rr::kChunkSize <= stream->size; offset += rr::kChunkSize) {
        disc.ReadForm1(stream->lba, offset, head.data(), 0x20);
        const uint8_t type = head[3] >> 4;
        if (type != 0 && type != 8 && type != 9) continue;
        const rr::ChunkHeader h = rr::ParseChunkHeader(head);
        if (type == 9) {
            nineAt.emplace(h.id, offset);
            continue;
        }
        if (!seen.insert(h.id).second || !keep(h)) continue;
        cellAt.push_back(offset);
        if (type == 8) wantNine.insert(h.id);
    }
    for (uint64_t offset : cellAt) {
        std::vector<uint8_t> chunk(rr::kChunkSize);
        disc.ReadForm1(stream->lba, offset, chunk.data(), chunk.size());
        out.cells.push_back(std::move(chunk));
    }
    for (uint32_t id : wantNine) {
        const auto it = nineAt.find(id);
        if (it == nineAt.end()) continue;
        std::vector<uint8_t> chunk(rr::kChunkSize);
        disc.ReadForm1(stream->lba, it->second, chunk.data(), chunk.size());
        out.nine.emplace(id, std::move(chunk));
    }
    return out;
}

} // namespace

void RaceSession::BuildCellCatalog(const DiscImage& disc) {
    if (streamPorted_) { // the cells come and go with the PORTED streamer; the rest of the object
        // area stays the session's 16 KiB spare blocks (the sound heap, the two-player stat array)
        for (uint32_t a = (arenaFreeFrom_ + 0xFFFu) & ~0xFFFu; a + kCellChunkBytes <= kArenaObjectsTo; a += kCellChunkBytes)
            freeCellBuffers_.push_back(a);
        std::reverse(freeCellBuffers_.begin(), freeCellBuffers_.end());
        cellBuffersTotal_ = freeCellBuffers_.size();
        return;
    }
    std::set<uint16_t> roads;
    for (const RouteLeg& leg : world_.legs) roads.insert(static_cast<uint16_t>(leg.road));
    StreamCellSet set = ReadStreamCells(disc, world_.set, [&roads](const rr::ChunkHeader& h) {
        for (const rr::ResidencyWindow& w : h.windows)
            if (roads.count(w.road) != 0) return true;
        return false;
    });
    for (std::vector<uint8_t>& chunk : set.cells) {
        CatalogCell c;
        const rr::ChunkHeader h = rr::ParseChunkHeader(chunk);
        c.id = h.id;
        c.type = h.type;
        c.windows = h.windows;
        if (c.type == 8) {
            const auto it = set.nine.find(c.id);
            if (it != set.nine.end()) c.type9 = it->second;
        }
        c.chunk = std::move(chunk);
        cells_.push_back(std::move(c));
    }
    // The 16 KiB buffers the cells are placed in: what is left of the object area (OURS).
    GuestRam g(arena_.Ram(), kArenaGp);
    for (uint32_t a = (arenaFreeFrom_ + 0xFFFu) & ~0xFFFu; a + kCellChunkBytes <= kArenaObjectsTo; a += kCellChunkBytes)
        freeCellBuffers_.push_back(a);
    std::reverse(freeCellBuffers_.begin(), freeCellBuffers_.end());
    cellBuffersTotal_ = freeCellBuffers_.size();
    for (uint32_t s = 0; s < rr::sim::kCellSlotCount; ++s) {
        const uint32_t slot = rr::sim::kCellSlotTable + rr::sim::kCellSlotBytes * s;
        g.W32(slot + 0x00u, 0xFFFFFFFFu);
        g.W32(slot + 0x04u, 0);
        g.W32(slot + 0x08u, 0xFFFFFFFFu);
    }
    char what[400];
    std::snprintf(what, sizeof(what),
                  "the scene cells the PORTED ground query walks are the disc's, placed with the "
                  "loader's relocation (SLUS 0x8003234C) in the slot the rule of SLUS 0x80032A20 "
                  "picks; OURS: when a cell is loaded and freed (%zu cells of this route's roads; "
                  "resident while a residency window holds the player's road and distance - the "
                  "streamer is not ported) and where in the arena (%zu buffers)",
                  cells_.size(), cellBuffersTotal_);
    NoteSeam(what);
}

void RaceSession::CellStreamPass(GuestRam& g) {
    // Player p's cells go into its half of the slot table - 0..11 player 1's, 12..23 player 2's, the
    // half the PORTED ground query 0x800A8498 searches for a bike nearer view record 1 (ground.cpp). A
    // cell both players want keeps one buffer (OURS, as the residency rule is).
    auto slotAddr = [](int32_t s) {
        return rr::sim::kCellSlotTable + rr::sim::kCellSlotBytes * static_cast<uint32_t>(s);
    };
    struct Key {
        bool known = false;
        uint16_t road = 0;
        int32_t along = 0;
    } key[2];
    const int np = std::min(players_, static_cast<int>(bikes_.size()));
    for (int p = 0; p < np; ++p) {
        // SLUS 0x80023A14: the entity CamTarget 0x800235B0 set - the bike, or the rider while thrown (cell_view.h)
        const uint32_t se = rr::game::StreamTarget(g, p, bikes_[static_cast<size_t>(p)].entityAddress);
        const uint32_t word = g.U32(se + 0x168u);
        key[p].along = g.S32(se + 0x170u) >> 16;
        key[p].known = (word >> 16) == 0;
        key[p].road = static_cast<uint16_t>(word & 0xFFFFu);
    }
    auto wanted = [&](const CatalogCell& c, int p) {
        for (const rr::ResidencyWindow& w : c.windows)
            if (w.road == key[p].road && static_cast<int32_t>(w.from) <= key[p].along &&
                key[p].along <= static_cast<int32_t>(w.to))
                return true;
        return false;
    };
    auto slotOf = [](CatalogCell& c, int p) -> int32_t& { return p == 0 ? c.slot : c.slot2; };
    for (int p = 0; p < np; ++p) {
        if (!key[p].known) continue;
        for (CatalogCell& c : cells_) {
            int32_t& cs = slotOf(c, p);
            if (cs < 0 || wanted(c, p)) continue;
            const uint32_t slot = slotAddr(cs);
            g.W32(slot + 0x04u, 0);
            g.W32(slot + 0x08u, 0xFFFFFFFFu);
            g.W32(slot + 0x0Cu, 0);
            g.W32(slot + 0x00u, 0xFFFFFFFFu);
            cs = -1;
            if (c.slot >= 0 || c.slot2 >= 0) continue; // the other player still has it
            freeCellBuffers_.push_back(c.at);
            if (c.at9 != 0) freeCellBuffers_.push_back(c.at9);
            std::sort(freeCellBuffers_.begin(), freeCellBuffers_.end(), std::greater<uint32_t>());
            c.at = c.at9 = c.body = 0;
            ++log_.cellUnloads;
        }
    }
    for (int p = 0; p < np; ++p) {
        if (!key[p].known) continue;
        const int32_t lo = 12 * p;
        for (CatalogCell& c : cells_) {
            if (slotOf(c, p) >= 0 || !wanted(c, p)) continue;
            int32_t free = -1;
            for (int32_t s = lo; s < lo + 12 && free < 0; ++s)
                if (g.U32(slotAddr(s)) == 0xFFFFFFFFu) free = s;
            if (free < 0) {
                NoteSeam("a scene cell found all twelve of the player's slots taken and was not "
                         "loaded, exactly as 0x80032A20 does (0x80032ABC)");
                continue;
            }
            uint32_t region7 = c.at9 != 0 ? c.at9 + 0x20u : 0u;
            if (c.at == 0) {
                const size_t need = c.type9.empty() ? 1u : 2u;
                if (freeCellBuffers_.size() < need) {
                    NoteSeam("the arena has no room left for a scene cell (OURS: the buffers), so it was "
                             "not loaded");
                    continue;
                }
                c.at = freeCellBuffers_.back();
                freeCellBuffers_.pop_back();
                if (!c.type9.empty()) {
                    c.at9 = freeCellBuffers_.back();
                    freeCellBuffers_.pop_back();
                    g.WriteBlock(c.at9, c.type9.data(), static_cast<uint32_t>(c.type9.size()));
                    region7 = c.at9 + 0x20u;
                }
                c.body = PlaceCellChunk(g, c.at, c.chunk, region7);
                rr::game::RelocateRegionZero(g, c.body); // SLUS 0x800135E8, the slot fill's region-0 arrays
                ++log_.cellLoads;
            }
            const uint32_t slot = slotAddr(free);
            g.W32(slot + 0x00u, c.id);
            g.W32(slot + 0x04u, c.body);
            g.W32(slot + 0x08u, c.id);
            g.W32(slot + 0x0Cu, (c.type == 0 || region7 != 0) ? 3u : 1u);
            g.W32(slot + 0x4Cu, c.body - 48u); // 0x80032AFC: the extent array (the cell walker reads it)
            slotOf(c, p) = free;
        }
    }
    for (const CatalogCell& c : cells_)
        if (c.at != 0) ++log_.cellsResident;
}


bool CheckCellArena(const DiscImage& disc, int set, const std::string& ramPath, std::string& report,
                    bool mutate) {
    std::vector<uint8_t> ram;
    std::string err;
    if (!ReadRam(ramPath, ram, err)) {
        report = "cellarenacheck: " + err;
        return false;
    }
    GuestRam snap(ram.data(), kArenaGp);
    const StreamCellSet all = ReadStreamCells(disc, set, [](const rr::ChunkHeader&) { return true; });
    std::map<uint32_t, const std::vector<uint8_t>*> byId;
    for (const std::vector<uint8_t>& c : all.cells) byId.emplace(rr::ParseChunkHeader(c).id, &c);

    std::vector<uint8_t> expect(GuestRam::kRamSize, 0);
    GuestRam want(expect.data(), kArenaGp);
    size_t slots = 0, compared = 0, differ = 0, missing = 0;
    std::string lines;
    for (uint32_t s = 0; s < rr::sim::kCellSlotCount; ++s) {
        const uint32_t slot = rr::sim::kCellSlotTable + rr::sim::kCellSlotBytes * s;
        const uint32_t id = snap.U32(slot);
        if (id == 0xFFFFFFFFu) continue;
        ++slots;
        const auto it = byId.find(id & 0x0FFFFFFFu);
        if (it == byId.end()) {
            ++missing;
            char b[96];
            std::snprintf(b, sizeof(b), "  slot %u: id 0x%07X is not in STREAM%d.STR\n", s, id, set);
            lines += b;
            continue;
        }
        const std::vector<uint8_t>& chunk = *it->second;
        const uint32_t body = snap.U32(slot + 4u);
        const uint32_t nWords = LoadU32(chunk, 0x20);
        const uint32_t at = body - (0x20u + 4u * nWords);
        uint32_t region7 = 0;
        const bool type8 = (chunk[3] >> 4) == 8;
        if (type8) {
            region7 = snap.U32(body + 0x3Cu);
            if (region7 != 0) {
                const auto n9 = all.nine.find(id & 0x0FFFFFFFu);
                if (n9 != all.nine.end())
                    want.WriteBlock(region7 - 0x20u, n9->second.data(), static_cast<uint32_t>(n9->second.size()));
            }
        }
        if (at < 0x80000000u || at + rr::kChunkSize > 0x80200000u) {
            ++missing;
            continue;
        }
        PlaceCellChunk(want, at, chunk, region7, !mutate);
        size_t d = 0, n = 0;
        auto cmp = [&](uint32_t a, uint32_t len) {
            for (uint32_t k = 0; k < len; ++k) {
                ++n;
                if (want.U8(a + k) != snap.U8(a + k)) ++d;
            }
        };
        cmp(body, 0x40);
        const uint32_t r2 = snap.U32(body + 0x28u), r6 = snap.U32(body + 0x38u);
        const uint32_t A = snap.U16(body + 4u), B = snap.U16(body + 6u);
        if (r6 < r2 || r6 - r2 > rr::kChunkSize || A + 3u * B > 64u || at < 0x80000000u ||
            at + rr::kChunkSize > 0x80200000u) {
            ++missing;
            char b[128];
            std::snprintf(b, sizeof(b), "  slot %2u: id 0x%07X names no cell layout in this image\n", s, id);
            lines += b;
            continue;
        }
        cmp(r2, r6 - r2);
        for (uint32_t gi = 0; gi < A + 3u * B; ++gi) {
            const bool band0 = gi < A + B;
            const uint32_t base = band0 ? r6 : snap.U32(body + 0x3Cu);
            if (base == 0) continue;
            const uint32_t grp = r2 + 12u * gi;
            uint32_t p = base + snap.U32(grp);
            const uint32_t tri = snap.U32(grp + 4u), quad = snap.U32(grp + 8u);
            if (tri > 4096u || quad > 4096u) { d += 1; break; }
            for (uint32_t k = 0; k < tri; ++k, p += 20u) { cmp(p + 2u, 2); cmp(p + 14u, 6); }
            for (uint32_t k = 0; k < quad; ++k, p += 24u) { cmp(p + 2u, 2); cmp(p + 16u, 8); }
        }
        compared += n;
        differ += d;
        char b[160];
        std::snprintf(b, sizeof(b), "  slot %2u: cell 0x%07X type %d at 0x%08X%s: %zu bytes compared, %zu differ\n",
                      s, id & 0x0FFFFFFFu, type8 ? 8 : 0, at,
                      type8 ? (region7 ? " (+ its type-9 half)" : " (type-9 half not resident)") : "", n, d);
        lines += b;
    }
    char head[256];
    std::snprintf(head, sizeof(head),
                  "cellarenacheck %s: %zu resident cell(s), %zu byte(s) the ground query reads compared, "
                  "%zu differ, %zu slot(s) with no cell of the disc behind them\n",
                  ramPath.c_str(), slots, compared, differ, missing);
    report = head + lines;
    return slots > 0 && differ == 0 && missing == 0 && !snap.Faulted() && !want.Faulted();
}

// =============================================================================== the per-bike step
namespace {

// Everything a bike's pointers lead to, resolved out of the arena (the bench's `RawLinks`).
rr::sim::BikeLinks ArenaLinks(uint8_t* ram, GuestRam& g, uint32_t ea) {
    rr::sim::BikeLinks L;
    const uint32_t ownerAddr = g.U32(ea + ent::kOwner);
    const uint32_t riderAddr = g.U32(ea + ent::kRider);
    L.owner = RawAt(ram, ownerAddr, 0x260);
    if (riderAddr != 0) {
        L.rider = RawAt(ram, riderAddr, 1096);
        if (L.rider != nullptr) L.riderOwner = RawAt(ram, g.U32(riderAddr + ent::kOwner), 0x260);
    }
    L.slice = RawAt(ram, g.U32(ea + 0x154), 32);
    L.obstacle = RawAt(ram, g.U32(ea + 0x33C), 64);
    L.stats = RawAt(ram, g.U32(ea + ent::kStats), 0x1C0);
    L.contact.contact = RawAt(ram, g.U32(ea + 0x340), 0x120);
    if (L.contact.contact != nullptr) {
        const uint32_t h = static_cast<uint32_t>(L.contact.contact[0]) |
                           (static_cast<uint32_t>(L.contact.contact[1]) << 8);
        if ((h >> 5) == 0) L.contact.bike = RawAt(ram, g.U32(0x8005B3A0) + 1096u * (h & 31u), 1096);
        // hang55: the car (pool 3, 0x800CF660 + 512 slot, read at SLUS 0x80020858) and the prop (pool 4,
        // *(0x800CD6D4) + 596 slot and its record's kind word, read at 0x800207E8) that ReleaseContact
        // 0x8002076C follows, resolved as the bench's RawLinks resolves them. Without them a bike launched
        // or crashed while in contact with a prop or riding a car had ReleaseContact refuse half-way
        // through BikeCrashLaunch (flagsC bit 11 already cleared): its list migration was dropped, the
        // bike kept flagsC bit 10 on the riding list with no gravity, and the camera's lead point on it
        // overflowed into the eye that sent the view's RoadSliceSearch into an endless walk (1/55 f5470).
        else if ((h >> 5) == 3) L.contact.traffic = RawAt(ram, 0x800CF660u + 512u * (h & 31u), 512);
        else if ((h >> 5) == 4) {
            const uint32_t prop = g.U32(0x800CD6D4u) + 596u * (h & 31u);
            L.contact.prop = RawAt(ram, prop, 596);
            if (L.contact.prop != nullptr) {
                const uint8_t* rec = RawAt(ram, g.U32(prop), 16);
                if (rec != nullptr) L.contact.propKindWord = static_cast<uint16_t>(rec[14] | (rec[15] << 8));
            }
        }
    }
    L.wipeoutRateByte = g.U8(kWipeoutRateByte);
    return L;
}

// The list migration's callees: the two PORTED crash functions, and the PORTED rider layer.
struct ProductMigrateCallees final : rr::sim::BikeListMigrateCallees {
    uint8_t* ram;
    GuestRam& g;
    const rr::sim::BikeTables& t;
    ProductRiderSeams& seams;
    FrameLog& log;
    ProductMigrateCallees(uint8_t* r, GuestRam& gg, const rr::sim::BikeTables& tt, ProductRiderSeams& s, FrameLog& l)
        : ram(r), g(gg), t(tt), seams(s), log(l) {}
    bool CrashLaunch(uint32_t e) override {
        uint8_t* ep = RawAt(ram, e, 1096);
        return ep != nullptr && rr::sim::BikeCrashLaunch(EntityView(ep), ArenaLinks(ram, g, e), t);
    }
    bool WipeoutStart(uint32_t e) override {
        uint8_t* ep = RawAt(ram, e, 1096);
        return ep != nullptr && rr::sim::BikeWipeoutStart(EntityView(ep), ArenaLinks(ram, g, e));
    }
    bool RiderKnockOff(uint32_t r, uint32_t) override {
        rr::sim::RiderLayer layer(g, ram, seams);
        layer.KnockOff(r);
        ++log.riderLayerCalls;
        return !layer.Failed();
    }
    bool RiderLaunch(uint32_t r, uint32_t) override {
        rr::sim::RiderLayer layer(g, ram, seams);
        layer.Launch(r);
        ++log.riderLayerCalls;
        return !layer.Failed();
    }
};

struct ProductStepCallees final : rr::sim::BikeStepCallees {
    RaceSession* session;
    uint8_t* ram;
    GuestRam& g;
    const rr::sim::BikeTables& t;
    const GameTables& tables;
    ProductRiderSeams& seams;
    FrameLog& log;
    uint32_t stepSp;
    ProductStepCallees(RaceSession* s, uint8_t* r, GuestRam& gg, const rr::sim::BikeTables& tt, const GameTables& gt,
                       ProductRiderSeams& se, FrameLog& l, uint32_t sp)
        : session(s), ram(r), g(gg), t(tt), tables(gt), seams(se), log(l), stepSp(sp) {}
    uint8_t* At(uint32_t a) { return ram + (a & 0x1FFFFFu); }
    bool ListMigrate(uint32_t e, int32_t) override {
        ++log.stepMigrations;
        ProductMigrateCallees mc(ram, g, t, seams, log);
        return rr::sim::BikeListMigrate(g, e, stepSp, t, mc);
    }
    bool PassengerLaunch(uint32_t e) override {
        ProductMigrateCallees mc(ram, g, t, seams, log);
        return rr::sim::BikePassengerLaunch(g, e, stepSp, mc);
    }
    bool CrashTimer(uint32_t e, int32_t dt) override {
        CrashEmitPort crash(session, g, e, log);
        EntityView view(At(e));
        rr::sim::BikeCrashTimer(view, dt, g.S32(kCrashWindow), tables.crashTable.data(), crash);
        return true;
    }
    bool StanceEvent(int32_t ev, uint32_t rider, int32_t p) override {
        const uint16_t before = g.U16(rider + 0x220u);
        rr::sim::StanceLayer layer(g, seams);
        layer.Event(static_cast<uint32_t>(ev), rider, static_cast<uint32_t>(p));
        ++log.stanceEvents;
        if (g.U16(rider + 0x220u) != before) ++log.stanceChanged;
        return !layer.Failed();
    }
    std::vector<uint32_t> Walk(uint32_t list) {
        std::vector<uint32_t> out;
        uint32_t n = g.U32(list + 4u);
        while (n != list && out.size() < 64) {
            out.push_back(n - 1088u);
            n = g.U32(n + 4u);
        }
        return out;
    }
    bool SteerDriver(uint32_t list, int32_t dt) override {
        const std::vector<uint32_t> es = Walk(list);
        std::vector<EntityView> riders;
        riders.reserve(es.size());
        std::vector<rr::sim::BikeSteerNode> nodes;
        for (uint32_t e : es) {
            const uint32_t ra = g.U32(e + ent::kRider);
            riders.emplace_back(ra != 0u ? At(ra) : nullptr);
            const uint32_t owner = g.U32(e + ent::kOwner);
            nodes.push_back(rr::sim::BikeSteerNode{EntityView(At(e)), ra != 0u ? &riders.back() : nullptr,
                                                   At(g.U32(e + ent::kStats)), At(kHandleTable),
                                                   g.U8(owner + 0x23Cu), g.S32(owner + 0x25Cu)});
        }
        const uint32_t gs = g.U32(kGameStatePtr);
        rr::sim::BikeSteerDriver(nodes.data(), nodes.size(), dt, g.S32(gs + 0x30u), g.S32(gs + 0x34u),
                                 At(kPadRecords), tables.atan.data(), tables.sincos.data());
        log.steerBikes += nodes.size();
        return true;
    }
    bool SteerPass(uint32_t list, int32_t dt) override {
        std::vector<rr::sim::BikeSteerPassNode> nodes;
        for (uint32_t e : Walk(list)) {
            rr::sim::BikeSteerPassNode n{EntityView(At(e))};
            n.ownerFlagByte = g.U8(g.U32(e + ent::kOwner) + 0x23Cu);
            const uint32_t ra = g.U32(e + ent::kRider);
            n.passenger = ra != 0u ? At(ra) : nullptr;
            n.stats = At(g.U32(e + ent::kStats));
            n.riderDefByte0 = g.U8(g.U32(e + ent::kRiderDef));
            nodes.push_back(n);
        }
        rr::sim::BikeSteerPassEnv env;
        env.numPlayers = g.U32(g.U32(kGameStatePtr) + 0x30u);
        env.handleTable = At(kHandleTable);
        env.handleTableBytes = 0x200000u - (kHandleTable & 0x1FFFFFu);
        env.atan = tables.atan.data();
        env.sincos = tables.sincos.data();
        return rr::sim::BikeSteerPass(nodes.data(), nodes.size(), dt, env);
    }
};

// The tail's callees (regions G..J), the product's form of the bench's `TailCallees`
// (rows_bike_step.inc): every one is the PORTED function on raw views of the arena - the activation
// pass, the downed-rider pass and EndRace's three callees included.
struct ProductTailCallees final : rr::sim::BikeStepTailCallees {
    // The population passes (population.h), wired by WorldPass: their callees, and the arena copy a
    // refused transition is restored from.
    rr::sim::PopulationCallees* pop = nullptr;
    Refusals* refusals = nullptr;
    ProductStampCallees* stamp = nullptr;
    std::function<void()> snapshot, restore;
    RaceSession* session;
    uint8_t* ram;
    GuestRam& g;
    const rr::sim::BikeTables& t;
    FrameLog& log;
    uint8_t* spad;
    uint32_t playerEntity = kArenaPool0;
    int32_t lastDepth = 0;
    uint8_t lastSurface = 0;
    rr::sim::RoadRuntimeNative road;
    ProductTailCallees(RaceSession* s, uint8_t* r, uint8_t* sp, GuestRam& gg, const rr::sim::BikeTables& tt,
                       FrameLog& l)
        : session(s), ram(r), g(gg), t(tt), log(l), spad(sp) {}

    bool Integrate(uint32_t e, int32_t dt) override {
        uint8_t* ep = RawAt(ram, e, 1096);
        if (ep == nullptr) return false;
        // The active list lives in the scratchpad (0x1F800000 its end pointer); the integrator
        // appends through the four bytes AT the cursor (the bench's `ListSlotAt`).
        rr::sim::BikeActiveList list;
        list.cursor = g.U32(kScratchpad);
        if (list.cursor >= kScratchpad && list.cursor <= kScratchpad + 0x400u - 4u)
            list.slot = spad + (list.cursor - kScratchpad);
        else
            list.slot = RawAt(ram, list.cursor, 4);
        if (list.slot == nullptr) return false;
        ++log.integratorRan;
        if (!rr::sim::BikeIntegrate(EntityView(ep), dt, e, ArenaLinks(ram, g, e), t, list)) return false;
        if (list.pushed) {
            g.W32(kScratchpad, list.cursor);
            ++log.integratorListed;
        }
        return true;
    }

    bool GroundFrame(uint32_t ea, uint32_t refAddr, uint32_t sp) override {
        uint8_t* ep = RawAt(ram, ea, 1096);
        if (ep == nullptr) return false;
        EntityView ev(ep);
        const uint32_t riderAddr = ev.U32(ent::kRider);
        uint8_t* rider = riderAddr != 0 ? RawAt(ram, riderAddr, 1096) : nullptr;
        if (riderAddr != 0 && rider == nullptr) return false;
        const uint32_t gsp = sp - kGroundFrameSz;
        const uint32_t bufA = gsp + kGroundSlotA, bufB = gsp + kGroundSlotB, bufC = gsp + kGroundSlotC;
        rr::sim::BikeGroundFrameSlots slots;
        for (uint32_t k = 0; k < 3; ++k) {
            slots.pointA[k] = g.S32(bufA + 4u * k);
            slots.pointB[k] = g.S32(bufB + 4u * k);
            slots.normalC[k] = g.S16(bufC + 2u * k);
        }
        rr::sim::BikeGroundEnv ge;
        ge.frame = &slots;
        auto slice = [this](uint32_t addr) {
            rr::sim::BikeGroundSlice s;
            s.address = addr;
            if (addr >= 0x80000000u && addr < 0x80200000u) {
                for (uint32_t k = 0; k < 3; ++k) {
                    s.row[k] = g.S16(addr + 8u + 2u * k);
                    s.origin[k] = g.S32(addr + 20u + 4u * k);
                }
            }
            return s;
        };
        auto refresh = [&]() {
            ge.tracked = slice(ev.U32(0x154));
            ge.ground = slice(ev.U32(0x100));
            ge.listNode = ev.U32(0x440);
            const uint32_t rdAddr = ev.U32(ent::kRiderDef);
            ge.riderDefPlace = (rdAddr >= 0x80000000u) ? g.U8(rdAddr + 0x27u) : 0u;
            ge.rider = rider;
            ge.riderTracked = rider != nullptr ? slice(EntityView(rider).U32(0x154)) : rr::sim::BikeGroundSlice{};
        };
        struct GroundQ final : rr::sim::BikeGroundQuery {
            GuestRam& g;
            FrameLog& log;
            RaceSession* session;
            uint32_t ea, riderAddr, bufA, bufB, bufC, mainRef;
            const uint16_t* rsqrt = nullptr;
            std::function<void()> refresh;
            GroundQ(GuestRam& gg, FrameLog& l, RaceSession* s, uint32_t a, uint32_t r, uint32_t p, uint32_t q,
                    uint32_t c, uint32_t mr)
                : g(gg), log(l), session(s), ea(a), riderAddr(r), bufA(p), bufB(q), bufC(c), mainRef(mr) {}
            int32_t Query(rr::sim::BikeGroundSite site, const int32_t*, int32_t outPoint[3], int16_t outNormal[3],
                          int32_t prevId) override {
                const bool third = (site == rr::sim::BikeGroundSite::kRider);
                const uint32_t pointAddr = (site == rr::sim::BikeGroundSite::kAlt) ? bufB : bufA;
                const uint32_t normAddr = third ? bufC : (ea + 0x10Cu);
                for (uint32_t k = 0; k < 3; ++k) g.W32(pointAddr + 4u * k, static_cast<uint32_t>(outPoint[k]));
                for (uint32_t k = 0; k < 3; ++k) g.W16(normAddr + 2u * k, static_cast<uint16_t>(outNormal[k]));
                uint32_t refAddr = 0;
                if (site == rr::sim::BikeGroundSite::kMain) refAddr = mainRef;
                else if (site == rr::sim::BikeGroundSite::kAlt) refAddr = ea + 0x0F4u;
                // RASHCDG 0x800A7BF8, PORTED (ground.h), with exactly the five arguments the original
                // passes - its output buffers are the original ground frame's own frame slots.
                const rr::sim::GroundResult gr =
                    rr::sim::GroundQuery(g, third ? riderAddr : ea, refAddr, pointAddr, normAddr,
                                         static_cast<uint32_t>(prevId), rsqrt);
                ++log.groundQueries;
                if (gr.declined || g.Faulted()) {
                    ++log.groundDeclined;
                    g.ClearFault();
                    session->NoteSeam("the PORTED ground query RASHCDG 0x800A7BF8 declined (a cell polygon "
                                      "shape the shipped data does not have, or an address outside the arena)");
                } else if (gr.hit) {
                    ++log.groundHits;
                    // bit 11 of the answer: the FINE mesh (band 1 of region 7)
                    if (gr.value & 0x800u) ++log.groundFineHits;
                    if (ea == kArenaPool0) {
                        ++log.playerGroundHits;
                        if (gr.value & 0x800u) ++log.playerGroundFineHits;
                    }
                } else if (gr.value == 0xFFFFFFFFu) {
                    ++log.groundMisses;
                } else {
                    ++log.groundExhausted;
                }
                for (uint32_t k = 0; k < 3; ++k) outPoint[k] = g.S32(pointAddr + 4u * k);
                for (uint32_t k = 0; k < 3; ++k) outNormal[k] = g.S16(normAddr + 2u * k);
                refresh();
                return static_cast<int32_t>(gr.value);
            }
        } q(g, log, session, ea, riderAddr, bufA, bufB, bufC, refAddr);
        q.refresh = refresh;
        q.rsqrt = t.rsqrt;
        ge.query = &q;
        refresh();
        int32_t ref[3], before[3];
        for (uint32_t k = 0; k < 3; ++k) {
            ref[k] = g.S32(refAddr + 4u * k);
            before[k] = static_cast<int32_t>(ev.U32(0x1F8 + 4u * k));
        }
        ++log.groundFrames;
        rr::sim::BikeGroundFrame(ev, ref, ge);
        for (uint32_t k = 0; k < 3; ++k)
            if (static_cast<int32_t>(ev.U32(0x1F8 + 4u * k)) != before[k]) { ++log.groundMoved; break; }
        // The player's ground-frame outputs, read HERE: +0x10C..+0x123 are overwritten later in the
        // frame by the collision box corners BikeRiderPose rebuilds (+0xC4..+0x123, bike.h).
        lastDepth = static_cast<int32_t>(ev.U32(0x104));
        lastSurface = ep[0x216];
        if (ea == playerEntity) {
            log.playerGroundHeight = static_cast<int32_t>(ev.U32(0x104));
            log.playerGroundAnswer = ev.U32(0x218);
            for (uint32_t k = 0; k < 3; ++k) log.playerGroundNormal[k] = ev.S16(0x112 + 2u * k);
            log.playerSurface = ep[0x216];
        }
        return !g.Faulted();
    }

    bool ContactFrame(uint32_t ea, uint32_t sp) override {
        uint8_t* ep = RawAt(ram, ea, 1096);
        if (ep == nullptr) return false;
        struct RebindImpl final : rr::sim::BikeRoadRebind {
            GuestRam& g;
            rr::sim::RoadRuntimeNative& road;
            FrameLog& log;
            uint32_t ea, sp;
            bool ok = true;
            RebindImpl(GuestRam& gg, rr::sim::RoadRuntimeNative& r, FrameLog& l, uint32_t a, uint32_t s)
                : g(gg), road(r), log(l), ea(a), sp(s) {}
            void Rebind(int32_t flag) override {
                ++log.contactRebinds;
                rr::sim::RoadRebind(g, ea, flag, sp, road); // SLUS 0x800374D4, PORTED
                if (g.Faulted()) ok = false;
            }
        } rebind(g, road, log, ea, sp - kContactFrameSz);
        ++log.contactRan;
        const uint32_t fcBefore = g.U32(ea + ent::kFlagsC);
        const uint32_t tag = g.U32(ea + 0x33C);
        if (!rr::sim::BikeContactFrame(EntityView(ep), ArenaLinks(ram, g, ea), t, rebind)) return false;
        if (!(fcBefore & 0x400u) && (g.U32(ea + ent::kFlagsC) & 0x400u)) {
            ++log.launches;
            char b[300];
            std::snprintf(b, sizeof(b), " [bike %u launched: depth +0x104 %.3f, surface +0x216 %d, speed %.2f, +0x184 0x%X, "
                          "tag slice +0x33C 0x%08X with +0x32 = %d, +0x16C %d]",
                          (ea - kArenaPool0) / 1096u, lastDepth / 65536.0, static_cast<int8_t>(lastSurface),
                          static_cast<int32_t>(g.U32(ea + ent::kSpeed)) / 65536.0, g.U32(ea + 0x184), tag,
                          tag ? g.S16(tag + 50u) : 0, static_cast<int32_t>(g.U32(ea + 0x16C)));
            log.launchNote += b;
        }
        return rebind.ok;
    }

    rr::sim::RoadRuntimeCallees& RoadSeams() override { return road; }

    bool RiderPose(uint32_t ea, int32_t dt) override {
        uint8_t* ep = RawAt(ram, ea, 1096);
        if (ep == nullptr) return false;
        EntityView bike(ep);
        const uint32_t riderAddr = bike.U32(ent::kRider);
        uint8_t* rp = riderAddr != 0 ? RawAt(ram, riderAddr, 1096) : nullptr;
        if (riderAddr != 0 && rp == nullptr) return false;
        EntityView rider(rp);
        auto ownerRev = [this](EntityView& v) {
            const uint32_t o = v.U32(ent::kOwner);
            return (o != 0) ? g.S32(o + 604) : 0;
        };
        const int32_t bikeRev = ownerRev(bike);
        const int32_t riderRev = rp != nullptr ? ownerRev(rider) : 0;
        ++log.riderPoses;
        rr::sim::BikeRiderPose(bike, dt, bikeRev, rp != nullptr ? &rider : nullptr, riderRev, t.atan, t.sincos);
        return true;
    }

    // RASHCDG 0x80093E6C ActivationPass (26 instructions: Activate(e, 0) for every pool-0 slot, the
    // high index and the stride read through the pool table 0x800CE4D0), its loop run HERE so that a
    // bike whose transition reaches an unported callee is restored on its own and the pass goes on to
    // the next slot - the ported ActivationPass stops at the first refusal. Every slot's work is the
    // PORTED Activate 0x80093ED4 (the window test, Transition, Placement / Retire).
    bool ActivationPass(uint32_t sp) override {
        ++log.activationSeams;
        if (rr::game::PassesWhole()) { // the PORTED ActivationPass 0x80093E6C whole (population.h)
            rr::game::PassTotals& pt = rr::game::PassRunTotals();
            ++pt.activationPasses;
            std::vector<int16_t> before;
            {
                int32_t k = g.S32(g.U32(kPoolTable + 12u));
                for (uint32_t e = g.U32(kPoolTable); k >= 0 && before.size() < 64; --k, e += g.U32(kPoolTable + 4u))
                    before.push_back(g.S16(e + 0x140u));
            }
            log.activations += before.size();
            snapshot();
            if (!(rr::sim::ActivationPass(g, sp, *pop) && !g.Faulted())) {
                restore();
                ++pt.activationRefused;
                ++log.transitionsRefused;
                session->NoteSeam("the PORTED ActivationPass RASHCDG 0x80093E6C refused a pass (a transition reached a "
                                  "callee that refused, or a fault): the arena was restored whole");
                return true;
            }
            uint32_t e = g.U32(kPoolTable);
            for (int16_t b : before) {
                if (((g.S16(e + 0x140u) ^ b) & 1) != 0) ++log.transitions;
                e += g.U32(kPoolTable + 4u);
            }
            return true;
        }
        const uint32_t F = sp - 32u;                                // `addiu sp,sp,-32` at 0x80093E6C
        int32_t n = g.S32(g.U32(kPoolTable + 12u));
        uint32_t e = g.U32(kPoolTable);
        while (n >= 0) {
            snapshot();
            const int16_t before = g.S16(e + 0x140u);
            ++log.activations;
            const bool ok = rr::sim::Activate(g, e, 0, F, *pop) && !g.Faulted();
            if (!ok) {
                restore();
                ++log.transitionsRefused;
            } else if (((g.S16(e + 0x140u) ^ before) & 1) != 0) {
                ++log.transitions;
            }
            e += g.U32(kPoolTable + 4u);
            --n;
        }
        session->NoteSeam("RASHCDG 0x80093E6C the activation pass runs PORTED (Activate 0x80093ED4, the window test "
                          "SLUS 0x80039F68 on view record 0's eye, Placement 0x8009432C, Retire 0x80093FE4); its "
                          "26-instruction slot loop is the session's, so that one refused transition is restored "
                          "without stopping the pass");
        return true;
    }
    // RASHCDG 0x800950E8 DownedRiderPass: Downed(r, 0) for every pool-0 bike's rider in mount state 3
    // or 4 and its passenger's, the loop run here for the same reason; each call the PORTED Downed
    // 0x800951B8.
    bool DownedRiderPass(uint32_t sp) override {
        ++log.downedSeams;
        if (rr::game::PassesWhole()) { // the PORTED DownedRiderPass 0x800950E8 whole (population.h)
            rr::game::PassTotals& pt = rr::game::PassRunTotals();
            ++pt.downedPasses;
            snapshot();
            if (!(rr::sim::DownedRiderPass(g, sp, *pop) && !g.Faulted())) {
                restore();
                ++pt.downedRefused;
                ++log.downedRefused;
                session->NoteSeam("the PORTED DownedRiderPass RASHCDG 0x800950E8 refused a pass: the arena was restored whole");
            }
            return true;
        }
        const uint32_t F = sp - 32u;
        int32_t n = g.S32(g.U32(kPoolTable + 12u));
        uint32_t e = g.U32(kPoolTable);
        auto one = [&](uint32_t r) {
            snapshot();
            ++log.downedCalls;
            if (!(rr::sim::Downed(g, r, 0, F, *pop) && !g.Faulted())) {
                restore();
                ++log.downedRefused;
            }
        };
        while (n >= 0) {
            const uint32_t r = g.U32(e + 852u);
            if ((g.U32(r + 604u) - 3u) < 2u) one(r);
            if (g.U8(g.U32(e + 852u) + 572u) & 0x10u) {
                const uint32_t pr = g.U32(g.U32(e + 856u) + 852u);
                if ((g.U32(pr + 604u) - 3u) < 2u) one(pr);
            }
            e += g.U32(kPoolTable + 4u);
            --n;
        }
        return true;
    }
    bool EndRace(uint32_t b, int32_t reason, uint32_t) override {
        const uint32_t handle = g.U16(b + 0xACu);
        rr::sim::EndRaceEnv ee;
        ee.entity = RawAt(ram, b, 1096);
        ee.riderDef = RawAt(ram, g.U32(b + ent::kRiderDef), 72);
        ee.owner = RawAt(ram, g.U32(b + ent::kOwner), kRiderBytes);
        ee.view = RawAt(ram, kViewArray + kViewStride * handle, 0x310);
        ee.gameState = RawAt(ram, g.U32(kGameStatePtr), 64);
        ee.postDelay = reinterpret_cast<int32_t*>(RawAt(ram, 0x8005B230u, 4));
        // EndRace's three callees, PORTED (spine.h): StampResult 0x800BC7CC, ResetBikeState SLUS
        // 0x8002090C and ViewEvent 0x8008A998 on the bike's own view record.
        struct Calls final : rr::sim::EndRaceCalls {
            RaceSession* s;
            GuestRam& g;
            uint32_t bike, view;
            ProductStampCallees& stamp;
            FrameLog& log;
            Calls(RaceSession* ss, GuestRam& gg, uint32_t b, uint32_t v, ProductStampCallees& st, FrameLog& l)
                : s(ss), g(gg), bike(b), view(v), stamp(st), log(l) {}
            void StampResult() override {
                ++log.stampResults;
                if (!rr::sim::StampResult(g, bike, stamp) || g.Faulted()) {
                    g.ClearFault();
                    s->NoteSeam("RASHCDG 0x800BC7CC StampResult (PORTED, under EndRace) refused: a callee failed "
                                "or an address faulted");
                }
            }
            void WipeoutEffect() override {
                ++log.resets;
                if (!rr::sim::ResetBikeState(g, bike) || g.Faulted()) {
                    g.ClearFault();
                    s->NoteSeam("SLUS 0x8002090C ResetBikeState (PORTED, under EndRace) refused");
                }
            }
            void ViewEvent(int32_t reason) override {
                ++log.viewEvents;
                rr::sim::ViewEvent(g, view, static_cast<uint32_t>(reason));
                if (g.Faulted()) {
                    g.ClearFault();
                    s->NoteSeam("RASHCDG 0x8008A998 ViewEvent (PORTED, under EndRace) met a fault");
                }
            }
        } calls(session, g, b, kViewArray + kViewStride * handle, *stamp, log);
        ee.calls = &calls;
        ++log.endRaces;
        return rr::sim::EndRace(reason, ee);
    }
};

} // namespace

/// RaceTick's third child, the world pass RASHCDG 0x8008AC80: PORTED WHOLE (passes.h) -
// TrafficPass 0x8009A298, the per-bike step 0x80075EE0, the rider-off pass 0x8008F068, PropPass 0x800A2898,
// PedPass 0x800CB304 while *(0x8005B254), Pool5Pass 0x8009ACA4, VolumePass 0x8009AB60, each PORTED. Before
// it, OURS: the cell and road-piece residency passes. RRJB_PASSES=session (the negative control) runs the
// children in the session's own order instead, with the PORTED view distance 0x8008DBCC per bike first
// (the original computes it in GameFrame step 4, ViewPass, after RaceStep - which AnimationPass runs).
void RaceSession::WorldPass(int32_t dt) {
    const bool whole = rr::game::PassesWhole();
    if (!whole)
        NoteSeam("RRJB_PASSES=session: the world pass RASHCDG 0x8008AC80 is the session's own ordering of its "
                 "PORTED children (the negative control of the world passes)");
    GuestRam g(arena_.Ram(), kArenaGp);
    g.SetScratchpad(scratchpad_.data());
    if (!whole) // the PORTED view distance 0x8008DBCC per bike, from view record 0's eye as ViewUpdate left it LAST frame
        for (size_t i = 0; i < bikes_.size(); ++i) rr::sim::ViewDistance(g, ArenaEntity(i)); // 0x8008DBCC
    if (!streamPorted_) { // else the PORTED streamer ran at GameFrame's start (StreamFrameStep)
        CellStreamPass(g);
        RoadStreamPass(g);
    }

    rr::sim::BikeTables t;
    t.sincos = tables_.sincos.data();
    t.asin = tables_.atanU16.data();
    t.atan = tables_.atan.data();
    t.rsqrt = tables_.rsqrtTable.data();
    t.sqrt = tables_.sqrtWindow.data() + 0x800 / 2;
    ProductRiderSeams seams(this, g);
    ProductStepCallees calls(this, arena_.Ram(), g, t, tables_, seams, log_, kArenaStepSp - rr::sim::kBikeStepFrame);
    ProductTailCallees tail(this, arena_.Ram(), scratchpad_.data(), g, t, log_);
    // Region J's two population passes: their callees over the arena,
    // and the whole-arena copy a refused transition is restored from.
    Refusals refusals;
    refusals.session = this;
    refusals.log = &log_;
    ProductStampCallees stamp(g, arena_.Ram(), seams, log_);
    ProductPopCallees pop(this, g, arena_.Ram(), seams, stamp, refusals, t);
    pop.targetSpeed = [this](uint32_t e, int32_t d) { return TargetSpeedAt(e, d); };
    tail.pop = &pop;
    tail.refusals = &refusals;
    tail.stamp = &stamp;
    tail.snapshot = [this]() { SnapshotArena(); };
    tail.restore = [this, &g]() { RestoreArena(g); };
    // The PORTED TrafficPass 0x8009A298 - every pool-3 car's window test,
    // release, drive; a refused frame restores the whole arena.
    auto traffic = [&](int32_t d, uint32_t sp) {
        if (!trafficReady_) return true;
        SnapshotArena();
        ProductDriveCallees drive(g, pop);
        drive.sound = [this](int32_t x, int32_t z, int32_t id, int32_t bank) { sounds_.PlaySound3D(x, z, id, bank); };
        log_.trafficRan = true;
        if (!rr::sim::TrafficPass(g, d, sp, drive) || g.Faulted()) {
            log_.trafficDeclined = true;
            char b[200];
            std::snprintf(b, sizeof(b),
                          "the PORTED TrafficPass RASHCDG 0x8009A298 declined a frame (view fault at 0x%08X, or a callee "
                          "refused): the arena was restored",
                          g.FaultAddress());
            NoteSeam(b);
            g.ClearFault();
            RestoreArena(g);
        }
        log_.carsLive = g.S32(rr::sim::kPopPool3Ctrl);
        return true;
    };
    auto step = [&](int32_t d, uint32_t sp) {
        log_.stepRan = true;
        if (!rr::sim::BikeStep(g, d, sp, t, calls, tail)) {
            log_.stepDeclined = true;
            log_.stepFault = g.FaultAddress();
            char b[200];
            std::snprintf(b, sizeof(b),
                          "the PORTED per-bike step RASHCDG 0x80075EE0 declined a frame (view fault at 0x%08X, or a "
                          "ported callee refused): the bikes keep what it wrote up to there",
                          g.FaultAddress());
            NoteSeam(b);
            g.ClearFault();
        }
        return true;
    };
    // RASHCDG 0x8008F068 (PORTED, recover_fall.h): every rider off its bike.
    auto riderOff = [&](int32_t d, uint32_t sp) {
        if (!RecoverCall(0x8008F068u, {static_cast<uint32_t>(d)}, sp, seams)) g.ClearFault();
        return true;
    };
    // The props 0x800A2898, pool 5 0x8009ACA4 and the collision volumes 0x8009AB60, PORTED (world_pop_product.h)
    rr::game::WorldProductCallees wc(g, pop, t);
    wc.note = [this](const std::string& n) { NoteSeam(n); };
    wc.pedPass = [this, &seams](int32_t d, uint32_t sp) { // PedPass 0x800CB304, PORTED (peds.h)
        rr::game::PedTotals& pt = rr::game::PedRunTotals();
        ++pt.passes;
        const bool ok = RecoverCall(rr::sim::kPedsPassFn, {static_cast<uint32_t>(d)}, sp, seams);
        if (!ok) ++pt.passRefused;
        return ok;
    };
    auto worldChild = [&](uint32_t fn, int32_t d, uint32_t sp) {
        if (!rr::game::RunWorldChild(g, fn, d, sp, wc)) {
            char b[160];
            std::snprintf(b, sizeof(b), "the PORTED world-pass child RASHCDG 0x%08X (world_pop.h) refused a frame (a fault "
                          "or a declined ground query): what it wrote up to there stays", fn);
            NoteSeam(b);
            g.ClearFault();
        }
        return true;
    };
    if (whole) {
        struct Children final : rr::sim::WorldBikePassCallees {
            std::function<bool(int32_t, uint32_t)> traffic, step, riderOff, ped;
            std::function<bool(uint32_t, int32_t, uint32_t)> child;
            bool TrafficPass(int32_t d, uint32_t sp) override { return traffic(d, sp); }
            bool BikeStep(int32_t d, uint32_t sp) override { return step(d, sp); }
            bool RiderOffPass(int32_t d, uint32_t sp) override { return riderOff(d, sp); }
            bool PropPass(int32_t d, uint32_t sp) override { return child(0x800A2898u, d, sp); }
            bool PedPass(int32_t d, uint32_t sp) override { return ped(d, sp); }
            bool Pool5Pass(uint32_t sp) override { return child(0x8009ACA4u, 0, sp); }
            bool VolumePass(uint32_t sp) override { return child(0x8009AB60u, 0, sp); }
        } ch;
        ch.traffic = traffic;
        ch.step = step;
        ch.riderOff = riderOff;
        ch.child = worldChild;
        ch.ped = [&](int32_t d, uint32_t sp) {
            if (!wc.pedPass(d, sp)) g.ClearFault();
            return true;
        };
        ++rr::game::PassRunTotals().worldPasses;
        if (!rr::sim::WorldBikePass(g, dt, kArenaStepSp + rr::sim::kTopPassFrame, ch)) {
            NoteSeam("the PORTED WorldBikePass RASHCDG 0x8008AC80 stopped (a view fault)");
            g.ClearFault();
        }
        return;
    }
    ++rr::game::PassRunTotals().sessionWorld;
    traffic(dt, kArenaStepSp);
    step(dt, kArenaStepSp);
    riderOff(dt, kArenaStepSp);
    if (!rr::game::RunWorldPasses(g, dt, kArenaStepSp, wc)) {
        NoteSeam("the PORTED prop / pool-5 / volume passes (world_pop.h) refused a frame (a fault or a declined ground "
                 "query): what they wrote up to there stays");
        g.ClearFault();
    }
}

// RaceTick's second child, the PORTED SpawnerPass RASHCDG 0x8008CD88: the
// accumulator, the cell walker (a named seam, not run), and once a round the PORTED police and traffic
// schedulers with the car spawner under them. A refused round restores the whole arena.
void RaceSession::SpawnerTick(int32_t dt) {
    GuestRam g(arena_.Ram(), kArenaGp);
    g.SetScratchpad(scratchpad_.data());
    if (!trafficReady_) {
        NoteSeam("RASHCDG 0x8008CD88 SpawnerPass is not run: the traffic arena was not built (see its line)");
        return;
    }
    rr::sim::BikeTables t;
    t.sincos = tables_.sincos.data();
    t.asin = tables_.atanU16.data();
    t.atan = tables_.atan.data();
    t.rsqrt = tables_.rsqrtTable.data();
    t.sqrt = tables_.sqrtWindow.data() + 0x800 / 2;
    ProductRiderSeams seams(this, g);
    Refusals refusals;
    refusals.session = this;
    refusals.log = &log_;
    ProductStampCallees stamp(g, arena_.Ram(), seams, log_);
    ProductPopCallees pop(this, g, arena_.Ram(), seams, stamp, refusals, t);
    pop.targetSpeed = [this](uint32_t e, int32_t d) { return TargetSpeedAt(e, d); };
    SnapshotArena();
    log_.spawnerRan = true;
    if (!rr::sim::SpawnerPass(g, dt, kArenaStepSp, pop) || g.Faulted()) {
        log_.spawnerDeclined = true;
        char b[220];
        std::snprintf(b, sizeof(b),
                      "the PORTED SpawnerPass RASHCDG 0x8008CD88 declined a round (view fault at 0x%08X, last refused "
                      "callee 0x%08X): the arena was restored",
                      g.FaultAddress(), refusals.last);
        NoteSeam(b);
        g.ClearFault();
        RestoreArena(g);
    }
    log_.carsLive = g.S32(rr::sim::kPopPool3Ctrl);
    log_.copsOut = g.S32(rr::sim::kPopCopsOut);
}

// A PORTED population function by address with the session's population callees (race_session.h): the
// Jailbreak escape scene's Placement 0x8009432C(e) and CarSpawn 0x8009AD48(rec, a1) (jail_session.cpp).
bool RaceSession::PopCall(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t sp, uint32_t& v0) {
    GuestRam g(arena_.Ram(), kArenaGp);
    g.SetScratchpad(scratchpad_.data());
    const rr::sim::BikeTables t = SessionTables();
    ProductRiderSeams seams(this, g);
    Refusals refusals;
    refusals.session = this;
    refusals.log = &log_;
    ProductStampCallees stamp(g, arena_.Ram(), seams, log_);
    ProductPopCallees pop(this, g, arena_.Ram(), seams, stamp, refusals, t);
    pop.targetSpeed = [this](uint32_t e, int32_t d) { return TargetSpeedAt(e, d); };
    v0 = 0;
    bool ok = false;
    if (fn == 0x8009432Cu) ok = rr::sim::Placement(g, a0, sp, pop);
    else if (fn == 0x8009AD48u) ok = rr::sim::CarSpawn(g, a0, a1, sp, pop, v0);
    else if (fn == 0x800A2630u) { // PropAlloc4 (world_pop.h): the late escape scene's roadblock (jail_session.cpp)
        rr::game::WorldProductCallees wc(g, pop, t);
        wc.note = [this](const std::string& n) { NoteSeam(n); };
        ok = rr::sim::PropAlloc4(g, a0, a1, sp, t, wc, v0);
    }
    if (!ok || g.Faulted()) {
        char b[160];
        std::snprintf(b, sizeof(b), "RASHCDG 0x%08X (PORTED, population.h) refused a call from the escape scene (fault "
                      "at 0x%08X, last refused callee 0x%08X)", fn, g.FaultAddress(), refusals.last);
        NoteSeam(b);
        g.ClearFault();
        return false;
    }
    return true;
}

// The rider pass's FIRST POOL LOOP [0x8007B894, 0x8007C23C) of RASHCDG 0x8007B840 (bike_react.h
// RiderPassPoolLoop): its callees on the product's ported functions; the three unported
// ones are named seams (the junction margin answers 0 = no junction record: the edge planes of a bike
// inside a junction fan are then the slice's own).
struct ProductPoolCallees final : rr::sim::PoolLoopCallees {
    RaceSession* session;
    uint8_t* ram;
    GuestRam& g;
    const rr::sim::BikeTables& t;
    ProductMigrateCallees& mc;
    ProductTailCallees& tail;
    ProductPoolCallees(RaceSession* s, uint8_t* r, GuestRam& gg, const rr::sim::BikeTables& tt,
                       ProductMigrateCallees& m, ProductTailCallees& tl)
        : session(s), ram(r), g(gg), t(tt), mc(m), tail(tl) {}
    bool ListMigrate(uint32_t e, int32_t, uint32_t sp) override { return rr::sim::BikeListMigrate(g, e, sp, t, mc); }
    bool JunctionMargin(uint32_t e, uint32_t sp, uint32_t& v0) override {
        if (rr::game::JunctionOn()) return rr::game::PoolJunctionMargin(g, e, sp, t, v0); // PORTED (junction_product.h)
        rr::game::PoolJunctionMargin(g, e, sp, t, v0); // counts the ask, answers 0
        session->NoteSeam("SLUS 0x8003E338 (the junction margin record) is SWITCHED OFF (RRJB_JUNCTION=off): the "
                          "rider pass's pool loop gets 0, so a bike inside a junction fan keeps the slice's own edge planes");
        return true;
    }
    bool ImpactTurn(uint32_t e, uint32_t partner, uint32_t n, int32_t mode, uint32_t sp, uint32_t& v0) override {
        return rr::sim::ImpactTurn(g, e, partner, n, mode, sp, t, v0); // PORTED (hit_speed.h)
    }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        session->Sounds().PlaySound3D(x, z, id, bank);
        return true;
    }
    bool RecoverEnd(uint32_t e, int32_t, uint32_t sp) override { return rr::sim::BikeRecoverEnd(g, e, sp, t, mc); }
    bool Launch(uint32_t e, int32_t k, uint32_t sp) override {
        if (rr::game::PartnersOn()) { // PORTED (partners.h)
            ++rr::sim::PartnerCounters().launches;
            return rr::sim::Launch(g, e, k, sp, t);
        }
        session->NoteSeam("RASHCDG 0x80084BE8 (the launch off a released contact) is not ported: no effect");
        return true;
    }
    bool ReleaseContact(uint32_t e) override {
        uint8_t* ep = RawAt(ram, e, 1096);
        return ep != nullptr && rr::sim::ReleaseContact(EntityView(ep), ArenaLinks(ram, g, e).contact);
    }
    bool RoadRebind(uint32_t e, int32_t flag, uint32_t sp) override {
        rr::sim::RoadRebind(g, e, flag, sp, tail.road); // SLUS 0x800374D4, PORTED
        return !g.Faulted();
    }
    bool GroundFrame(uint32_t e, uint32_t ref, uint32_t sp) override { return tail.GroundFrame(e, ref, sp); }
    bool SteerLean(uint32_t e, uint32_t) override {
        uint8_t* ep = RawAt(ram, e, 1096);
        const uint8_t* st = ep != nullptr ? RawAt(ram, g.U32(e + ent::kStats), 0x1C0) : nullptr;
        if (st == nullptr) return false;
        rr::sim::BikeSteerLean(EntityView(ep), st, t.sincos);
        return true;
    }
    bool TurnFacing(uint32_t e, int32_t ang, uint32_t) override {
        if (rr::game::PartnersOn()) { // PORTED (partners.h)
            ++rr::sim::PartnerCounters().turns;
            rr::sim::TurnFacing(g, e, ang);
            return !g.Faulted();
        }
        session->NoteSeam("RASHCDG 0x8007ED64 (the facing turn after a re-seat) is not ported: no effect");
        return true;
    }
};

// RaceTick's fifth child, the rider / engine pass RASHCDG 0x8008ACE8: PORTED WHOLE (passes.h) - the rider
// pass 0x8007B840, itself PORTED WHOLE (its four regions, the engine 0x80079B20 on
// 0x8005B2D8 / 0x8005B298 / 0x8005B350, ImpactStatePass 0x80078DB4, the rider loop, the heading writer
// 0x8007AC04, DormantDrive 0x80095724), then PropAnimPass 0x800A2A64, PedRelease 0x800CB4F8 while
// *(0x8005B254), HazardPass 0x800A13C4. Each child below is ONE lambda that both paths call - a new
// child of either pass goes into its lambda, never beside it. RRJB_PASSES=session (the negative control)
// runs them in the session's own order: no class-list walk [0x8007C9DC, 0x8007DCD4), the engine on
// the three lists in one call.
void RaceSession::RiderPass(int32_t dt) {
    const bool whole = rr::game::PassesWhole();
    if (!whole)
        NoteSeam("RRJB_PASSES=session: the rider / engine pass RASHCDG 0x8008ACE8 and the rider pass 0x8007B840 are "
                 "the session's own ordering of their PORTED parts, without the class-list walk [0x8007C9DC, "
                 "0x8007DCD4) (the negative control of the world passes)");
    GuestRam g(arena_.Ram(), kArenaGp);
    g.SetScratchpad(scratchpad_.data());
    auto walk = [&g](uint32_t list) {
        std::vector<uint32_t> out;
        uint32_t n = g.U32(list + 4u);
        while (n != list && out.size() < 64) {
            out.push_back(n - 1088u);
            n = g.U32(n + 4u);
        }
        return out;
    };
    const rr::sim::BikeTables t = SessionTables();
    const uint32_t rsp = kArenaStepSp - rr::sim::kRiderPassFrame; // the rider pass's own frame
    // the PORTED first pool loop [0x8007B894, 0x8007C23C), before the thrown walk
    auto poolLoop = [&](int32_t d, uint32_t sp) {
        ProductRiderSeams ps(this, g);
        ProductMigrateCallees pmc(arena_.Ram(), g, t, ps, log_);
        ProductTailCallees ptail(this, arena_.Ram(), scratchpad_.data(), g, t, log_);
        ProductPoolCallees pool(this, arena_.Ram(), g, t, pmc, ptail);
        if (!rr::sim::RiderPassPoolLoop(g, d, sp, t, pool) || g.Faulted()) {
            NoteSeam("the rider pass's pool loop [0x8007B894, 0x8007C23C) (PORTED) declined a frame");
            g.ClearFault();
        }
        return true;
    };
    // THE FALL: the PORTED walk of the thrown list
    // [0x8007C23C, 0x8007C9A8) - gravity +0x1E4 and the air drag *(0x800D3960) on the velocity, the
    // speed and heading from it, the tumble rows - BEFORE the engine, as the original orders it. Its
    // callees are the PORTED stance event, passenger launch and crash timer (ProductStepCallees), at
    // the rider pass's own frame.
    auto thrownWalk = [&](int32_t d, uint32_t sp) {
        ProductRiderSeams seams(this, g);
        ProductStepCallees step(this, arena_.Ram(), g, t, tables_, seams, log_, sp);
        struct Thrown final : rr::sim::ThrownWalkCallees {
            ProductStepCallees& s;
            explicit Thrown(ProductStepCallees& ss) : s(ss) {}
            bool StanceEvent(int32_t ev, uint32_t rider, int32_t p, uint32_t) override {
                return s.StanceEvent(ev, rider, p);
            }
            bool PassengerLaunch(uint32_t e, uint32_t callSp) override {
                s.stepSp = callSp;
                return s.PassengerLaunch(e);
            }
            bool CrashTimer(uint32_t e, int32_t dd, uint32_t) override { return s.CrashTimer(e, dd); }
        } thrown(step);
        const std::vector<uint32_t> flying = walk(kListThrown);
        std::vector<int32_t> vy0(flying.size());
        for (size_t k = 0; k < flying.size(); ++k) vy0[k] = g.S32(flying[k] + 0x1CCu);
        log_.thrownWalked = flying.size();
        thrownWalks_ += flying.size();
        if (!rr::sim::RiderPassThrownWalk(g, d, sp, t, thrown) || g.Faulted()) {
            log_.thrownDeclined = true;
            ++thrownDeclined_;
            char b[200];
            std::snprintf(b, sizeof(b),
                          "the thrown walk [0x8007C23C, 0x8007C9A8) (PORTED) declined a frame (view fault at 0x%08X, "
                          "Normalize's overflow, or a callee refused)",
                          g.FaultAddress());
            NoteSeam(b);
            g.ClearFault();
        } else {
            if (air_.size() != bikes_.size()) air_.resize(bikes_.size());
            for (size_t k = 0; k < flying.size(); ++k) {
                const uint32_t off = flying[k] - kArenaPool0;
                const size_t i = off / static_cast<uint32_t>(kEntitySize);
                if (off % static_cast<uint32_t>(kEntitySize) != 0 || i >= air_.size()) continue;
                air_[i].sumDvy += static_cast<int64_t>(g.S32(flying[k] + 0x1CCu)) - vy0[k];
                ++air_[i].walkedFrames;
            }
        }
        return true;
    };
    // The PORTED engine 0x80079B20 (bike.h BikeEngineStep) on the bikes of `lists`, in list order.
    EngineSoundOut sound(&sounds_);
    log_.engineBikes = 0;
    auto engine = [&](std::initializer_list<uint32_t> lists, int32_t d) {
        std::vector<rr::sim::BikeEngineNode> nodes;
        for (uint32_t list : lists)
            for (uint32_t e : walk(list))
                nodes.push_back(rr::sim::BikeEngineNode{EntityView(At(e)), At(g.U32(e + ent::kStats)),
                                                        g.S32(g.U32(e + ent::kOwner) + 0x25Cu)});
        rr::sim::BikeEngineStep(nodes.data(), nodes.size(), d, *randSeed_, tables_.atanU16.data(), sound);
        log_.engineBikes += nodes.size();
        return true;
    };
    // [0x8007C9DC, 0x8007DCD4): the walk of the class list 0x8005B350 (passes.h RiderPassClassWalk, PORTED):
    // ClipDone 0x8005BE58 on the animation machine, PlaySound3D on the sound runtime, root counter 2 our
    // deterministic counter (the hardware timer is not modelled - ImpactStatePass's rule).
    auto classWalk = [&](int32_t d, uint32_t sp) {
        struct Cls final : rr::sim::ClassWalkCallees {
            RaceSession* s;
            GuestRam& g;
            uint32_t& counter;
            Cls(RaceSession* ss, GuestRam& gg, uint32_t& c) : s(ss), g(gg), counter(c) {}
            bool ClipDone(uint32_t anim, uint32_t, uint32_t& v0) override {
                ProductRiderSeams rs(s, g);
                rr::sim::AnimMachine m(g, rs);
                v0 = m.ClipDone(anim);
                return !m.Failed();
            }
            bool GetRCnt(uint32_t, uint32_t, uint32_t& v0) override {
                counter = counter * 1103515245u + 12345u;
                v0 = (counter >> 16) & 0xFFFFu;
                return true;
            }
            bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank, uint32_t) override {
                s->Sounds().PlaySound3D(x, z, id, bank);
                return true;
            }
        } cls(this, g, rcntState_);
        rr::game::PassTotals& pt = rr::game::PassRunTotals();
        const size_t n = walk(kListDown).size();
        pt.classBikes += n;
        if (n != 0) ++pt.classFrames;
        if (!rr::sim::RiderPassClassWalk(g, d, sp, t, cls) || g.Faulted()) {
            ++pt.classRefused;
            char b[200];
            std::snprintf(b, sizeof(b), "the rider pass's class-list walk [0x8007C9DC, 0x8007DCD4) (PORTED) declined a "
                          "frame (view fault at 0x%08X or Normalize's overflow): what it wrote stays", g.FaultAddress());
            NoteSeam(b);
            g.ClearFault();
        }
        return true;
    };
    // ImpactStatePass's two callees live with the sound system (crash.h): GetRCnt answered by a
    // deterministic counter of ours, PlaySound3D by the PORTED emitter in the sound runtime.
    struct Snd final : rr::sim::ImpactSound {
        SoundRuntime& s;
        uint32_t& counter;
        RaceSession* session;
        Snd(SoundRuntime& ss, uint32_t& c, RaceSession* se) : s(ss), counter(c), session(se) {}
        bool GetRCnt(uint32_t, uint32_t& value) override {
            session->NoteSeam("ImpactStatePass's GetRCnt(root counter 2) is answered by a deterministic counter "
                              "of ours: the hardware timer is not modelled");
            counter = counter * 1103515245u + 12345u;
            value = (counter >> 16) & 0xFFFFu;
            return true;
        }
        bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
            s.PlaySound3D(x, z, id, bank);
            return true;
        }
    } snd(sounds_, rcntState_, this);
    log_.impactBikes = 0;
    auto impact = [&](uint32_t list) {
        log_.impactBikes += walk(list).size();
        if (!rr::sim::ImpactStatePass(g, list, t, snd) || g.Faulted()) {
            log_.impactDeclined = true;
            g.ClearFault();
            NoteSeam("RASHCDG 0x80078DB4 ImpactStatePass (PORTED) declined a frame (a "
                     "state it refuses rather than read BIOS ROM)");
        }
        return true;
    };
    ProductRiderSeams rs(this, g);
    ProductMigrateCallees mc(arena_.Ram(), g, t, rs, log_);
    auto migrate = [&](uint32_t e, uint32_t sp) { // RASHCDG 0x80071BCC, PORTED (bike_step.h BikeListMigrate)
        if (!(rr::sim::BikeListMigrate(g, e, sp, t, mc) && !g.Faulted())) {
            g.ClearFault();
            NoteSeam("RASHCDG 0x80071BCC BikeListMigrate (PORTED) refused in the rider pass: the bike stays on its "
                     "list with what its callees wrote (hang55)");
        }
        return true;
    };
    auto riderGround =[&](uint32_t r, int32_t d, uint32_t sp) { // RASHCDG 0x8008F404, PORTED (recover_race.h)
        if (!RecoverCall(0x8008F404u, {r, static_cast<uint32_t>(d)}, sp, rs)) g.ClearFault();
        return true;
    };
    log_.headingBikes = 0;
    auto heading = [&](uint32_t list) {
        if (list == kListRiding) log_.headingBikes = walk(kListRiding).size();
        if (!rr::sim::BikeHeadingPass(g, list, t)) {
            log_.headingDeclined = true;
            g.ClearFault();
            NoteSeam("RASHCDG 0x8007AC04 (ported) declined a frame: the view faulted or Normalize would "
                     "have raised the console's overflow exception");
        }
        return true;
    };
    // [0x8007DDF4, 0x8007E804): the walk of the crashed-down list 0x8005B378 (PORTED, recover_fall.h)
    auto downWalk = [&](int32_t d, uint32_t sp) {
        RecoverCall(0x8007DDF4u, {static_cast<uint32_t>(d)}, sp, rs);
        g.ClearFault();
        return true;
    };
    // The rider pass's last loop (0x8007E804..0x8007E834): the PORTED DormantDrive 0x80095724(e, dt) per bike
    // of the dormant list 0x8005B270. A dormant racer advances along its route at its target speed without
    // physics.
    ProductRiderSeams seams(this, g);
    Refusals refusals;
    refusals.session = this;
    refusals.log = &log_;
    ProductStampCallees stamp(g, arena_.Ram(), seams, log_);
    ProductPopCallees pop(this, g, arena_.Ram(), seams, stamp, refusals, t);
    pop.targetSpeed = [this](uint32_t e, int32_t d) { return TargetSpeedAt(e, d); };
    auto dormant = [&](uint32_t e, int32_t d, uint32_t sp) {
        SnapshotArena();
        ++log_.dormantDrives;
        if (!(rr::sim::DormantDrive(g, e, d, sp, pop) && !g.Faulted())) {
            RestoreArena(g);
            ++log_.dormantRefused;
            NoteSeam("RASHCDG 0x80095724 DormantDrive (PORTED) refused a bike (a fault, or the route walk's "
                     "65536-stretch bound): the arena was restored");
        }
        return true;
    };
    // ---- RASHCDG 0x8008ACE8's children after the rider pass
    // the PORTED prop animation pass 0x800A2A64, then the solid objects' measurement (solid_product.h)
    auto propAnim = [&](int32_t d, uint32_t sp) {
        rr::game::SolidEnginePass(g, d, sp, t, pop, [this](const std::string& n) { NoteSeam(n); });
        return true;
    };
    auto pedRelease = [&](int32_t d, uint32_t sp) { // PedRelease 0x800CB4F8 (PORTED, peds.h)
        rr::game::PedTotals& pt = rr::game::PedRunTotals();
        ++pt.releases;
        if (!RecoverCall(rr::sim::kPedsReleaseFn, {static_cast<uint32_t>(d)}, sp, seams)) {
            ++pt.releaseRefused;
            g.ClearFault();
        }
        rr::game::PedsFrame(g);
        return true;
    };
    // HazardPass 0x800A13C4 (PORTED, hazard.h): the events near player 1 armed, every live record moved on
    // its keys; with no event and no live record (an arena without hazard objects) it reads and
    // writes nothing but the player's +0x140.
    auto hazard = [&](int32_t d, uint32_t sp) {
        rr::game::PassTotals& pt = rr::game::PassRunTotals();
        ++pt.hazardPasses;
        if (!rr::game::RunHazardPass(g, d, sp, t) || g.Faulted()) { // hazard_product.h: the pass and its counters
            ++pt.hazardRefused;
            NoteSeam("RASHCDG 0x800A13C4 HazardPass (PORTED) refused a frame: what it wrote stays");
            g.ClearFault();
        }
        return true;
    };
    if (whole) {
        g.W32(0x8005B220u, static_cast<uint32_t>(*skipResults_)); // OURS: the session's skip-results flag, into the arena
        struct Rider final : rr::sim::RiderPassCallees {
            std::function<bool(int32_t, uint32_t)> pool, thrown, cls, down;
            std::function<bool(uint32_t, int32_t)> engine;
            std::function<bool(uint32_t)> impact, heading;
            std::function<bool(uint32_t, uint32_t)> migrate;
            std::function<bool(uint32_t, int32_t, uint32_t)> ground, dormant;
            bool PoolLoop(int32_t d, uint32_t sp) override { return pool(d, sp); }
            bool ThrownWalk(int32_t d, uint32_t sp) override { return thrown(d, sp); }
            bool Engine(uint32_t list, int32_t d, uint32_t) override { return engine(list, d); }
            bool ClassWalk(int32_t d, uint32_t sp) override { return cls(d, sp); }
            bool ImpactState(uint32_t list, uint32_t) override { return impact(list); }
            bool ListMigrate(uint32_t e, int32_t, uint32_t sp) override { return migrate(e, sp); }
            bool RiderGround(uint32_t r, int32_t d, uint32_t sp) override { return ground(r, d, sp); }
            bool Heading(uint32_t list, int32_t, uint32_t) override { return heading(list); }
            bool DownWalk(int32_t d, uint32_t sp) override { return down(d, sp); }
            bool DormantDrive(uint32_t e, int32_t d, uint32_t sp) override { return dormant(e, d, sp); }
        } rider;
        rider.pool = poolLoop;
        rider.thrown = thrownWalk;
        rider.engine = [&](uint32_t list, int32_t d) { return engine({list}, d); };
        rider.cls = classWalk;
        rider.impact = impact;
        rider.migrate = migrate;
        rider.ground = riderGround;
        rider.heading = heading;
        rider.down = downWalk;
        rider.dormant = dormant;
        struct Engine final : rr::sim::RiderEnginePassCallees {
            GuestRam* g = nullptr;
            Rider* rider = nullptr;
            std::function<bool(int32_t, uint32_t)> prop, ped, hazard;
            bool RiderPass(int32_t d, uint32_t sp) override {
                rr::game::PassTotals& pt = rr::game::PassRunTotals();
                ++pt.riderPasses;
                if (!rr::sim::RiderPass(*g, d, sp, *rider)) {
                    ++pt.riderPassRefused;
                    g->ClearFault();
                }
                return true;
            }
            bool PropAnimPass(int32_t d, uint32_t sp) override { return prop(d, sp); }
            bool PedRelease(int32_t d, uint32_t sp) override { return ped(d, sp); }
            bool HazardPass(int32_t d, uint32_t sp) override { return hazard(d, sp); }
        } eng;
        eng.g = &g;
        eng.rider = &rider;
        eng.prop = propAnim;
        eng.ped = pedRelease;
        eng.hazard = hazard;
        ++rr::game::PassRunTotals().engineWholePasses;
        if (!rr::sim::RiderEnginePass(g, dt, kArenaStepSp + rr::sim::kTopPassFrame, eng)) {
            NoteSeam("the PORTED RiderEnginePass RASHCDG 0x8008ACE8 stopped (a view fault)");
            g.ClearFault();
        }
        return;
    }
    // ---- RRJB_PASSES=session: the previous order
    ++rr::game::PassRunTotals().sessionEngine;
    poolLoop(dt, rsp);
    thrownWalk(dt, rsp);
    engine({kListThrown, kListRiding, kListDown}, dt);
    impact(kListDown);
    impact(kListThrown);
    { // [0x8007DCEC, 0x8007DDD4): the migration and 0x8008F404 per rider off its bike (PORTED, recover_race.h)
        RecoverRiderLoop(dt, rsp, rs, [&](uint32_t e) {
            return rr::sim::BikeListMigrate(g, e, rsp, t, mc) && !g.Faulted();
        });
        g.ClearFault();
    }
    g.W32(0x8005B220u, static_cast<uint32_t>(*skipResults_));
    heading(kListRiding);
    heading(kListDown);
    downWalk(dt, rsp);
    {
        uint32_t node = g.U32(kListDormant + 4u);
        for (int guard = 0; node != kListDormant && guard < 64; ++guard) {
            dormant(node - 1088u, dt, rsp);
            node = g.U32(node + 4u);
        }
    }
    propAnim(dt, kArenaStepSp);
    if (g.U32(rr::sim::kPedSwitch) != 0) pedRelease(dt, kArenaStepSp);
    hazard(dt, kArenaStepSp);
}

// The target speed the PORTED AiTargetSpeed 0x80095BF8 gives bike `e` (DormantDrive's callee), on
// the same environment the drive pass uses.
int32_t RaceSession::TargetSpeedAt(uint32_t e, int32_t dt) {
    GuestRam g(arena_.Ram(), kArenaGp);
    uint8_t* ep = RawAt(arena_.Ram(), e, 1096);
    uint8_t* rd = ep != nullptr ? RawAt(arena_.Ram(), g.U32(e + ent::kRiderDef), 72) : nullptr;
    uint8_t* st = ep != nullptr ? RawAt(arena_.Ram(), g.U32(e + ent::kStats), 512) : nullptr;
    if (ep == nullptr || rd == nullptr || st == nullptr) return 0;
    return rr::sim::AiTargetSpeed(ep, rd, st, dt, SpeedEnv());
}

rr::sim::AiSpeedEnv RaceSession::SpeedEnv() {
    rr::sim::AiSpeedEnv speedEnv;
    speedEnv.gameState = gameState_.data();
    speedEnv.player1 = {bikes_[0].entity.data(), bikes_[0].riderDef.data()};
    speedEnv.player2 = {nullptr, nullptr};
    speedEnv.playerSlot[0] = {bikes_[0].entity.data(), bikes_[0].riderDef.data()};
    speedEnv.playerSlot[1] = {nullptr, nullptr};
    if (players_ == 2) { // *(0x8005B21C) and *(0x8005B268 + 4): pool-0 slot 1 (mp_session.cpp)
        speedEnv.player2 = {bikes_[1].entity.data(), bikes_[1].riderDef.data()};
        speedEnv.playerSlot[1] = {bikes_[1].entity.data(), bikes_[1].riderDef.data()};
    }
    speedEnv.liveBikes = *liveBikes_;
    speedEnv.bikeCap = *bikeCap_;
    speedEnv.spreadDiv = *spreadDiv_;
    speedEnv.raceFlags = raceFlags_;
    speedEnv.clockStamp = clockStamp_;
    speedEnv.spreadBand = spreadBand_;
    // The tables as the arena holds them: GLOBALS.BI's (ai_globals.h) where the loader writes them, the
    // executable image elsewhere (0x80052FA0 is not a loader destination).
    auto w32 = [&](uint32_t a) { return reinterpret_cast<const int32_t*>(At(a)); };
    auto w16 = [&](uint32_t a) { return reinterpret_cast<const int16_t*>(At(a)); };
    speedEnv.tabSpeedClass = w32(kTabSpeedClass);
    speedEnv.tabThinkA = w32(kTabThinkA);
    speedEnv.tabThinkB = w32(kTabThinkB);
    speedEnv.tabCopFlat = w32(kTabCopFlat);
    speedEnv.tabAltA = w32(kTabAltA);
    speedEnv.tabAltB = w32(kTabAltB);
    speedEnv.tabSpeedCap = w32(kTabSpeedCap);
    speedEnv.tabCopBase = w16(kTabCopBase + 2);
    speedEnv.tabCopStep = w16(kTabCopStep + 2);
    speedEnv.tabCopMul = w32(kTabCopMul);
    speedEnv.profile = reinterpret_cast<const int8_t*>(At(kProfile));
    return speedEnv;
}

// The whole arena and the scratchpad, copied before a population transition and put back when it
// is refused: a refused transition leaves no trace.
// The streamer's population callees (stream_session.cpp): what PieceLoad 0x8003CEC4 / PieceUnload 0x8003D844 /
// CellRelease 0x8008C45C reach - the activation and downed-rider passes, the window test, the pool release and
// the two sleepers - PORTED (population.h, traffic_bind.h), on the session's population callees. A refused pass
// restores the arena as the world pass does.
bool RaceSession::StreamPopCall(uint32_t fn, const uint32_t* a, uint32_t sp, uint32_t& v0) {
    GuestRam g(arena_.Ram(), kArenaGp);
    g.SetScratchpad(scratchpad_.data());
    const rr::sim::BikeTables t = SessionTables();
    ProductRiderSeams seams(this, g);
    Refusals refusals;
    refusals.session = this;
    refusals.log = &log_;
    ProductStampCallees stamp(g, arena_.Ram(), seams, log_);
    ProductPopCallees pop(this, g, arena_.Ram(), seams, stamp, refusals, t);
    pop.targetSpeed = [this](uint32_t e, int32_t d) { return TargetSpeedAt(e, d); };
    const bool pass = fn == rr::sim::kStActivateFn || fn == rr::sim::kStDownedFn;
    if (pass) SnapshotArena();
    bool ok = false;
    v0 = 0;
    switch (fn) {
    case rr::sim::kStActivateFn: ok = rr::sim::ActivationPass(g, sp, pop); break;
    case rr::sim::kStDownedFn: ok = rr::sim::DownedRiderPass(g, sp, pop); break;
    case rr::sim::kStNearPieceFn: v0 = rr::sim::RoadWindow(g, a[0], sp); ok = true; break;
    case rr::sim::kStPoolDropFn: ok = rr::sim::PoolRelease(g, a[0], static_cast<int32_t>(a[1])); break;
    case rr::sim::kStSleepFn: ok = rr::sim::Activate(g, a[0], a[1], sp, pop); break;
    case rr::sim::kStRiderSleepFn: ok = rr::sim::Downed(g, a[0], a[1], sp, pop); break;
    default: break;
    }
    ok = ok && !g.Faulted();
    if (!ok) {
        g.ClearFault();
        if (pass) {
            RestoreArena(g);
            ok = true; // the pass is undone whole, as the world pass undoes it
        }
        char b[160];
        std::snprintf(b, sizeof(b), "the streamer's population callee 0x%08X (PORTED) refused%s", fn,
                      pass ? ": the arena was restored whole" : "");
        NoteSeam(b);
    }
    return ok;
}

void RaceSession::SnapshotArena() {
    restore_.assign(arena_.Ram(), arena_.Ram() + GuestRam::kRamSize);
    restoreSpad_ = scratchpad_;
}
void RaceSession::RestoreArena(GuestRam& g) {
    g.ClearFault();
    std::memcpy(arena_.Ram(), restore_.data(), GuestRam::kRamSize);
    scratchpad_ = restoreSpad_;
}

namespace {

// The collision pass's unported callees by name (collision.h `coll`).
std::string CollSeamName(uint32_t fn) {
    namespace c = rr::sim::coll;
    switch (fn) {
    case c::kBikeVsBike: return "BikeVsBike RASHCDG 0x800AB7A0";
    case c::kPointResolve: return "PointResolve RASHCDG 0x800B09C4";
    case c::kBoxResolve: return "BoxResolve RASHCDG 0x800B0D8C";
    case c::kPoleResolve: return "PoleResolve RASHCDG 0x800AE794";
    case c::kBikeVsRider: return "BikeVsRider RASHCDG 0x800AD04C";
    case c::kBikeVsTraffic: return "BikeVsTraffic RASHCDG 0x800AC5BC";
    case c::kRiderNone: return "the rider/pedestrian partner RASHCDG 0x800B2AF8";
    case c::kRiderVsTraffic: return "the rider/traffic partner RASHCDG 0x800B2844";
    case c::kRiderVsShape: return "the rider/shape partner RASHCDG 0x800B2B00";
    case c::kTrafficVsTraffic: return "the traffic partner RASHCDG 0x800B2D44";
    case c::kTrafficVsProp: return "the traffic/prop partner RASHCDG 0x800B2D88";
    case c::kPropVsShape: return "the prop partner RASHCDG 0x800B2E64";
    case c::kImpactTurn: return "ImpactTurn RASHCDG 0x80083F30";
    case c::kRiderGetUp: return "the rider get-up RASHCDG 0x800B208C";
    case c::kPropTopple: return "PropTopple RASHCDG 0x800B3344";
    case c::kBikeWallHit: return "BikeWallHit RASHCDG 0x800B12A0";
    case c::kRiderWallHit: return "RiderWallHit RASHCDG 0x800B2794";
    case c::kPedHit: return "PedHit RASHCDG 0x800A9868";
    case c::kPadMotor: return "the pad motors SLUS 0x8001DD74";
    case c::kBikeTrafficReact: return "BikeTrafficReact RASHCDG 0x800AC958";
    case c::kPoleReact: return "PoleReact RASHCDG 0x800AF0A0";
    case c::kBoxReact: return "BoxReact RASHCDG 0x800B11B4";
    case c::kBikeBikeReact: return "BikeBikeReact RASHCDG 0x800AC130";
    default: break;
    }
    char b[48];
    std::snprintf(b, sizeof(b), "unnamed callee 0x%08X", fn);
    return b;
}

// The collision pass's callees in the product: the PORTED emitter and
// ReleaseContact natively; PadRumble and every unported callee recorded as named seams.
struct ProductCollisionCallees final : rr::sim::CollisionCallees {
    RaceSession* session;
    uint8_t* ram;
    GuestRam& g;
    std::map<uint32_t, size_t> asked;
    size_t sounds = 0, releases = 0, rumbles = 0;
    rr::game::CollProductEnv native; // the PORTED contact/impact callees, served first
    ProductCollisionCallees(RaceSession* s, uint8_t* r, GuestRam& gg) : session(s), ram(r), g(gg) {}
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        session->Sounds().PlaySound3D(x, z, id, bank); // SLUS 0x80017BA0, PORTED
        ++sounds;
        return true;
    }
    bool ReleaseContact(uint32_t e) override { // SLUS 0x8002076C, PORTED (integrator.h)
        uint8_t* ep = RawAt(ram, e, 1096);
        if (ep == nullptr) return false;
        ++releases;
        return rr::sim::ReleaseContact(EntityView(ep), ArenaLinks(ram, g, e).contact);
    }
    bool Rumble(uint32_t e, uint32_t other, int32_t speed, int32_t k, int32_t div, uint32_t sp) override {
        ++rumbles;
        if (rr::game::RumbleOn()) return rr::game::ProductPadRumble(g, e, other, speed, k, div, sp); // rumble_product.h
        session->NoteSeam("RASHCDG 0x800B658C PadRumble (ported, collision.h) is not run by the product: its only "
                          "effect is the pad motors SLUS 0x8001DD74, and the product drives no pad motors - a "
                          "landing or a bounce does not shake the pad");
        return true;
    }
    bool Unported(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        if (bool ok = true; rr::game::ServeCollProduct(g, *this, native, fn, a, n, sp, v0, ok)) return ok;
        if (fn == 0x800A9868u && n >= 6) { // PedHit, PORTED (ped_hit.h): the rider / pedestrian hit
            ProductRiderSeams rs(session, g);
            v0 = 0;
            return session->RecoverCall(fn, {a[0], a[1], a[2], a[3], a[4], a[5]}, sp, rs, &v0);
        }
        if (fn == 0x800B208Cu && n >= 1) { // the rider meets the ground: RiderLand, PORTED (recover_air.h)
            ProductRiderSeams rs(session, g);
            v0 = 0;
            return session->RecoverCall(fn, {a[0]}, sp, rs);
        }
        if (fn == 0x8001A760u && n >= 2) { // RiderSpeech (BikeBikeGate's knock-on line), PORTED (speech_session.cpp)
            ProductRiderSeams rs(session, g);
            v0 = 0;
            return session->RiderSpeech(a[0], static_cast<int32_t>(a[1]), sp, &rs);
        }
        ++asked[fn];
        v0 = 0;
        session->NoteSeam(CollSeamName(fn) + " is not ported: the PORTED collision pass 0x800A4774 asks for it "
                                              "and gets v0 = 0 with no effect (no push, no reaction)");
        return true;
    }
};

} // namespace

// The collision pass RASHCDG 0x800A4774 - RaceTick's fourth child, between the world pass and the
// rider/engine pass - PORTED: the grid, the chain build, the
// bike-against-bike phase, the kind loop with WallContact and AirContact (the ground contact of an
// airborne or crashed bike: TouchDown 0x800B16F4 is THE LANDING), the deferred handler and
// ChainReaction, all native. Its unported callees (the pair resolvers, the reactions, BikeWallHit)
// are named seams answering v0 = 0: bikes still pass through each other and the walls do not push.
// RaceTick calls it with the frame's dt, from the same frame as the world pass (kArenaStepSp); the
// set-up calls it once with dt = 0 (the prime pass of SLUS 0x800119C0, CORRECTIONS 5).
void RaceSession::CollisionPass(int32_t dt) {
    GuestRam g(arena_.Ram(), kArenaGp);
    g.SetScratchpad(scratchpad_.data());
    rr::sim::BikeTables t;
    t.sincos = tables_.sincos.data();
    t.asin = tables_.atanU16.data();
    t.atan = tables_.atan.data();
    t.rsqrt = tables_.rsqrtTable.data();
    t.sqrt = tables_.sqrtWindow.data() + 0x800 / 2;
    ProductCollisionCallees c(this, arena_.Ram(), g);
    c.native.tables = &t; // the PORTED pair/impact callees run natively (coll_product.h)
    c.native.rcnt = &rcntState_;
    c.native.note = [this](const std::string& s) { NoteSeam(s); };
    c.native.stanceEvent = [this, &g](uint32_t ev, uint32_t r, uint32_t p, uint32_t& v) {
        ProductRiderSeams seams(this, g);
        rr::sim::StanceLayer layer(g, seams);
        v = layer.Event(ev, r, p);
        return !layer.Failed() && !g.Faulted();
    };
    // Measurement: which bikes are airborne (flagsC bit 10) going in; a bike that is and was not
    // last frame was launched this frame (by the contact response's BikeObstacleTest, region H).
    if (air_.size() != bikes_.size()) air_.resize(bikes_.size());
    std::vector<uint32_t> fcIn(bikes_.size());
    std::vector<int32_t> vyIn(bikes_.size());
    // The deepest of the eight box corners +0xC4 behind the ground plane through +0x1F8 with normal
    // -(+0x20A) - what AirContact compares with the clearance +0x2C4, in world
    // units, computed here in double for the log only.
    const auto deepest = [&g](uint32_t e) {
        double best = -1e30;
        for (uint32_t k = 0; k < 8; ++k) {
            double d = 0.0;
            for (uint32_t j = 0; j < 3; ++j)
                d += (static_cast<double>(g.S32(e + 0xC4u + 12u * k + 4u * j)) - g.S32(e + 0x1F8u + 4u * j)) *
                     static_cast<double>(g.S16(e + 0x20Au + 2u * j));
            best = std::max(best, d / 65536.0 / 4096.0);
        }
        return best;
    };
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint32_t e = ArenaEntity(i);
        fcIn[i] = g.U32(e + ent::kFlagsC);
        vyIn[i] = g.S32(e + 0x1CCu);
        AirTrack& a = air_[i];
        if ((fcIn[i] & 0x400u) && a.air) {
            char b[240];
            std::snprintf(b, sizeof(b),
                          "        FLY bike %zu f%u: flagsC 0x%08X vy +0x1CC %.3f box y %.3f ground +0x1FC %.3f deepest "
                          "corner %.3f (+0x2C4 %.3f) pitch +0x268 %.3f\n",
                          i, log_.frame, fcIn[i], vyIn[i] / 65536.0, g.S32(e + 0xBCu) / 65536.0,
                          g.S32(e + 0x1FCu) / 65536.0, deepest(e), g.S32(e + 0x2C4u) / 65536.0,
                          g.S32(e + 0x268u) / 65536.0);
            log_.airNote += b;
        }
        if ((fcIn[i] & 0x400u) && !a.air) {
            a = AirTrack();
            a.air = true;
            a.launchFrame = log_.frame;
            a.launchSpeed = g.S32(e + ent::kSpeed);
            a.launchVy = vyIn[i];
            ++collLaunches_;
            char b[320];
            std::snprintf(b, sizeof(b),
                          "        AIR bike %zu f%u: launched, flagsC 0x%08X +0x1E0 %.3f +0x240 %.3f vy +0x1CC %.3f "
                          "box y %.3f ground +0x1FC %.3f ground height +0x104 %.3f deepest corner %.3f (+0x2C4 %.3f) "
                          "pitch +0x268 %.3f\n",
                          i, log_.frame, fcIn[i], a.launchSpeed / 65536.0, g.S32(e + 0x240u) / 65536.0,
                          vyIn[i] / 65536.0, g.S32(e + 0xBCu) / 65536.0, g.S32(e + 0x1FCu) / 65536.0,
                          g.S32(e + 0x104u) / 65536.0, deepest(e), g.S32(e + 0x2C4u) / 65536.0,
                          g.S32(e + 0x268u) / 65536.0);
            log_.airNote += b;
        }
        if (a.air) a.lastVy = vyIn[i];
    }
    log_.collisionRan = true;
    if (!rr::sim::CollisionPass(g, dt, kArenaStepSp, t, c) || g.Faulted()) {
        log_.collisionDeclined = true;
        log_.collisionFault = g.FaultAddress();
        ++collDeclined_;
        char b[240];
        std::snprintf(b, sizeof(b),
                      "RASHCDG 0x800A4774 CollisionPass (PORTED) declined a frame (view fault at 0x%08X, or "
                      "Normalize's overflow exception): the bikes keep what it wrote up to there",
                      g.FaultAddress());
        NoteSeam(b);
        g.ClearFault();
    }
    for (const auto& [fn, n] : c.asked) {
        log_.collisionSeamCalls += n;
        collSeamTotals_[fn] += n;
        ++collSeamFrames_[fn];
        char b[96];
        std::snprintf(b, sizeof(b), " 0x%08X x%zu", fn, n);
        log_.collisionNote += b;
    }
    if (!c.native.served.empty()) log_.collisionNote += " native:" + rr::game::CollServedNote(c.native.served);
    log_.collisionSounds = c.sounds;
    log_.releaseContacts = c.releases;
    log_.rumbles = c.rumbles;
    collSounds_ += c.sounds;
    collReleases_ += c.releases;
    collRumbles_ += c.rumbles;
    ++collFrames_;
    // The landings: flagsC bit 10 cleared by the pass (TouchDown 0x800B18A4).
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint32_t e = ArenaEntity(i);
        const uint32_t fc = g.U32(e + ent::kFlagsC);
        if (!((fcIn[i] & 0x400u) && !(fc & 0x400u))) continue;
        ++log_.touchDowns;
        ++collTouchDowns_;
        AirTrack& a = air_[i];
        a.air = false;
        a.landed = true;
        a.landFrame = log_.frame;
        const int32_t s240 = g.S32(e + 0x240u), s35c = g.S32(e + 0x35Cu);
        char b[420];
        std::snprintf(b, sizeof(b),
                      "        LAND bike %zu f%u: airborne %u frame(s) since f%u; vy at contact %.3f (launch %.3f), "
                      "mean d(vy) per walk %.4f over %u walk(s); +0x35C (speed at contact) %.3f, +0x240 after "
                      "TouchDown %.3f = %.4f of it, %.4f of the launch +0x1E0 %.3f; flagsC 0x%08X; +0x300 %.3f\n",
                      i, log_.frame, log_.frame - a.launchFrame, a.launchFrame, vyIn[i] / 65536.0,
                      a.launchVy / 65536.0,
                      a.walkedFrames ? static_cast<double>(a.sumDvy) / 65536.0 / a.walkedFrames : 0.0,
                      a.walkedFrames, s35c / 65536.0, s240 / 65536.0,
                      s35c ? static_cast<double>(s240) / s35c : 0.0,
                      a.launchSpeed ? static_cast<double>(s240) / a.launchSpeed : 0.0, a.launchSpeed / 65536.0, fc,
                      g.S32(e + 0x300u) / 65536.0);
        log_.airNote += b;
    }
    g.ClearFault();
}

// After the frame: which bikes are airborne, and when a landed bike is back on the riding list
// 0x8005B298 (the PORTED list migration moves it, on TouchDown's flagsC bit 27).
void RaceSession::TrackAirborne(GuestRam& g) {
    if (air_.size() != bikes_.size()) air_.resize(bikes_.size());
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint32_t e = ArenaEntity(i);
        const uint32_t fc = g.U32(e + ent::kFlagsC);
        AirTrack& a = air_[i];
        if (fc & 0x400u) ++log_.airborne;
        if (a.air && !(fc & 0x400u)) {
            a.air = false;
            char b[160];
            std::snprintf(b, sizeof(b), "        AIR bike %zu f%u: flagsC bit 10 cleared OUTSIDE the collision pass "
                                        "(flagsC 0x%08X)\n", i, log_.frame, fc);
            log_.airNote += b;
        }
        if (a.landed && !a.relisted) {
            uint32_t n = g.U32(e + 0x444u);
            for (int guard = 0; guard < 64 && n != 0; ++guard) {
                if (n == kListDormant || n == kListRiding || n == kListThrown || n == kListDown || n == kListSpin) break;
                n = g.U32(n + 4u);
            }
            if (n == kListRiding) {
                a.relisted = true;
                ++collRelisted_;
                char b[200];
                std::snprintf(b, sizeof(b),
                              "        RELISTED bike %zu f%u: on the riding list 0x8005B298 again (landed f%u); "
                              "flagsC 0x%08X +0x240 %.3f\n",
                              i, log_.frame, a.landFrame, fc, g.S32(e + 0x240u) / 65536.0);
                log_.airNote += b;
            }
        }
    }
    maxAirborne_ = std::max(maxAirborne_, log_.airborne);
}

std::string RaceSession::CollisionTotals() const {
    char b[700];
    std::snprintf(b, sizeof(b),
                  "the PORTED collision pass RASHCDG 0x800A4774: %zu pass(es) (the prime pass included), %zu "
                  "declined; %zu landing(s) (TouchDown), %zu launch(es) seen, %zu landed bike(s) back on the "
                  "riding list, at most %zu airborne at once; PlaySound3D %zu, ReleaseContact %zu, PadRumble "
                  "asked %zu (%s); the thrown walk [0x8007C23C, 0x8007C9A8) ran on %zu bike-frame(s), "
                  "declined %zu\n",
                  collFrames_, collDeclined_, collTouchDowns_, collLaunches_, collRelisted_, maxAirborne_,
                  collSounds_, collReleases_, collRumbles_,
                  rr::game::RumbleOn() ? "run, with the PORTED motors: rumble_product.h" : "not run: RRJB_RUMBLE=off",
                  thrownWalks_, thrownDeclined_);
    std::string out = b;
    out += "the collision pass's unported callees asked for (v0 = 0, no effect):";
    if (collSeamTotals_.empty()) out += " none";
    out += "\n";
    for (const auto& [fn, n] : collSeamTotals_) {
        const auto f = collSeamFrames_.find(fn);
        std::snprintf(b, sizeof(b), "  %-52s %8zu call(s) on %6zu pass(es)\n", CollSeamName(fn).c_str(), n,
                      f != collSeamFrames_.end() ? f->second : 0);
        out += b;
    }
    out += rr::game::CollServedTotals(); // the natively served contact/impact callees (coll_product.h)
    return out;
}

// GameFrame step 6: AnimationPass 0x8005E1D8 over the descriptor, game_state+0x1C = the frame's ticks
// (RaceStep wrote it).
void RaceSession::AnimationPass(bool view) {
    GuestRam g(arena_.Ram(), kArenaGp);
    // GameFrame step 4 (0x80011DAC, right after RaceStep): RASHCDG 0x8008CFDC ViewPass, PORTED (view_pass.h)
    // - the view distances, the near bits and LodChoice 0x800667C4 on every bike, rider and car,
    // whose LOD bytes +0x0A the renderer draws each machine at (it replaced rider_pose.h's PoseLodPass).
    // `view` false: only the animation half (GameFrame runs ViewPass while racing only, not while paused).
    if (view && !rr::sim::ViewPass(g)) {
        g.ClearFault();
        NoteSeam("RASHCDG 0x8008CFDC ViewPass (PORTED) met a fault");
    }
    ProductRiderSeams seams(this, g);
    rr::sim::AnimMachine m(g, seams);
    size_t playing = 0;
    for (size_t i = 0; i < bikes_.size(); ++i)
        if (g.U32(animObjects_ + rr::sim::kAnimObjectBytes * static_cast<uint32_t>(i) + 0x24u) & 2u) ++playing;
    log_.animObjects = playing;
    rr::game::AnimObjFrame(g); // the animation objects' measurement: two owners on one object (animobj_product.h)
    // GameFrame step 5 (0x80011DBC, after ViewPass, before AnimationPass, in every state): AnimSounds SLUS 0x80018E54,
    // PORTED - the ANIMNOIZ.DAT events of the playing objects, i.e. the sound of a landed blow, a swing, a kick
    // RRJB_ANIMSOUNDS=off: not run (the negative control of the gate).
    {
        static const bool off = [] {
            const char* v = std::getenv("RRJB_ANIMSOUNDS");
            return v != nullptr && std::strcmp(v, "off") == 0;
        }();
        if (!off && sounds_.EngineMode() && !sounds_.AnimSounds(rr::sim::kAnimDescriptor))
            NoteSeam("SLUS 0x80018E54 AnimSounds (PORTED) was not run or faulted: the animation's sounds are partial");
    }
    rr::game::AnimDetailSnapshot(arena_.Ram()); // OURS: a read-only copy for maximum detail's drawn pose (off: nothing)
    m.Pass(rr::sim::kAnimDescriptor);
    if (m.Failed()) {
        log_.animDeclined = true;
        g.ClearFault();
        NoteSeam("RASHCDG 0x8005E1D8 AnimationPass (PORTED) declined a frame (the view faulted or a tick would "
                 "not terminate)");
    }
}

void RaceSession::BikePlacement(size_t index, float position[3], float tangent[3], float lateral[3],
                                float normal[3]) const {
    const uint8_t* e = bikes_[index].entity.p;
    for (uint32_t k = 0; k < 3; ++k) {
        position[k] = static_cast<float>(static_cast<double>(ReadS32(e, ent::kObbCentre + 4u * k)) / 65536.0);
        lateral[k] = static_cast<float>(ReadS16(e, 0x204 + 2u * k)) / 4096.0f;
        normal[k] = -static_cast<float>(ReadS16(e, 0x20A + 2u * k)) / 4096.0f;
        tangent[k] = static_cast<float>(ReadS16(e, 0x210 + 2u * k)) / 4096.0f;
    }
}

// OURS, display only: the slice of the assembled path nearest the box centre, searched around the
// last one.
void RaceSession::UpdateDisplayDistance(size_t i) {
    RaceBike& bike = bikes_[i];
    const std::vector<rr::RoadSlice>& path = world_.path;
    double p[3];
    for (uint32_t k = 0; k < 3; ++k) p[k] = static_cast<double>(ReadS32(bike.entity.p, ent::kObbCentre + 4u * k));
    auto d2 = [&](size_t s) {
        double d = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double v = static_cast<double>(path[s].pos[k]) - p[k];
            d += v * v;
        }
        return d;
    };
    const size_t lo = bike.pathHint > 64 ? bike.pathHint - 64 : 0;
    const size_t hi = std::min(path.size(), bike.pathHint + 65);
    size_t best = bike.pathHint < path.size() ? bike.pathHint : 0;
    double bestD = d2(best);
    for (size_t s = lo; s < hi; ++s)
        if (const double d = d2(s); d < bestD) { bestD = d; best = s; }
    bike.pathHint = best;
    bike.routeDistance = static_cast<double>(path[best].distance - path.front().distance) / 65536.0;
}

double RaceSession::RouteLength() const {
    return static_cast<double>(world_.path.back().distance - world_.path.front().distance) / 65536.0;
}

void RaceSession::ApplyInput(const PadState& pad) {
    // The merge into the bike's flag word +0x230 (`SLUS 0x8001D338`) is the PORTED pad reader region
    // (pad_product.h, run in Frame after the slots are stamped); the camera controls stay here - inside the pad
    // reader's `state == 1` block (0x8001CF8C), so not while paused (RRJB_PAUSE=off: always).
    if (!rr::game::PauseOn() || gameState_[0] == 1) CameraControls(pad);
}

// The pad handler's two camera controls for view record 0, transcribed from our disassembly
// (SLUS 0x8001D700..0x8001D7D8; the handler SLUS 0x8001CB3C itself is not ported):
//   control 0's press edge, while +0x304 == 0: PlaySound3D(0, 0, 107, 0) and CameraSetMode(v,
//     (+0x21C + 1) & 3) - the PORTED CameraSetMode 0x8008A998 (camera.h; spine.h ports the same
//     function as ViewEvent);
//   control 2 held: in modes 0..3 with +0x224 bit 0 clear, +0x224 |= 7 (look behind, re-place, snap);
//   control 2 not held: with bit 0 set, +0x224 = (+0x224 & ~1) | 6.
void RaceSession::CameraControls(const PadState& pad, int p) {
    GuestRam g(arena_.Ram(), kArenaGp);
    const uint32_t v = kViewArray + kViewStride * static_cast<uint32_t>(p);
    if (pad.cameraNext && g.U32(v + 0x304u) == 0) {
        ++log_.cameraKeys;
        sounds_.PlaySound3D(0, 0, 107, 0);
        rr::sim::BikeTables t;
        t.sincos = tables_.sincos.data();
        t.asin = tables_.atanU16.data();
        t.atan = tables_.atan.data();
        t.rsqrt = tables_.rsqrtTable.data();
        t.sqrt = tables_.sqrtWindow.data() + 0x800 / 2;
        ProductCameraSeams seams(this, camCounter_);
        rr::sim::RoadRuntimeNative road;
        rr::sim::CameraPort port(g, t, seams, road);
        port.SetMode(v, (g.U32(v + 0x21Cu) + 1u) & 3u);
        if (port.Failed()) {
            g.ClearFault();
            NoteSeam("the PORTED CameraSetMode 0x8008A998 refused the camera control");
        }
    }
    const uint32_t f = g.U32(v + 0x224u);
    if (pad.lookBack) {
        if (g.U32(v + 0x21Cu) < 4u && !(f & 1u)) {
            g.W32(v + 0x224u, f | 7u);
            ++log_.cameraKeys;
        }
    } else if (f & 1u) {
        g.W32(v + 0x224u, (f & ~1u) | 6u);
    }
}

// ------------------------------------------------------------------------------------ the frame
namespace {

struct SessionChildren final : rr::sim::RaceTickChildren {
    std::function<void(int32_t)> director, world, engine, collision, spawner;
    std::function<void(const char*)> seam;
    std::function<void(int32_t)> plan; // ai_race.h RunAiPlan, PORTED
    void AiPlan(int32_t acc) override { plan(acc); }
    void RaceDirector(int32_t dt) override { director(dt); }
    void SpawnerPass(int32_t dt) override { spawner(dt); } // RaceSession::SpawnerTick, PORTED (population.h)
    void WorldBikePass(int32_t dt) override { world(dt); }
    void CollisionPass(int32_t dt) override { collision(dt); } // RaceSession::CollisionPass, the hook
    void RiderEnginePass(int32_t dt) override { engine(dt); }
    std::function<void(int32_t)> presentation; // present.h: the PORTED 0x80090814
    void PresentationPass(int32_t dt) override {
        if (presentation) presentation(dt);
        else seam("RASHCDG 0x80090814 the presentation pass is not ported");
    }
};

// The PORTED presentation pass 0x80090814 (present.h): its seams are the PORTED
// stance event, RiderLaunch and RiderKnockOff on the product's rider seams; SLUS 0x800273EC (the riding
// lean's effect record) PORTED (weapon.h ObjectEffect, takedown_product.h).
class ProductPresentSeams final : public rr::sim::PresentSeams {
public:
    ProductPresentSeams(RaceSession* s, GuestRam& g, uint8_t* ram) : s_(s), g_(g), ram_(ram), rs_(s, g) {}
    uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t b) override { return rs_.TransitionCapture(a, op, b); }
    uint32_t ApplyFrame(uint32_t a) override { return rs_.ApplyFrame(a); }
    uint32_t CombatLeave(uint32_t r, uint32_t c, uint32_t e, uint32_t p) override { return rs_.CombatLeave(r, c, e, p); }
    uint32_t CombatEnter(uint32_t r, uint32_t c, uint32_t e, uint32_t p) override { return rs_.CombatEnter(r, c, e, p); }
    uint32_t StanceEvent(uint32_t ev, uint32_t r, uint32_t p) override {
        rr::sim::StanceLayer layer(g_, rs_);
        return layer.Event(ev, r, p);
    }
    uint32_t RiderLaunch(uint32_t r) override {
        rr::sim::RiderLayer layer(g_, ram_, rs_);
        return layer.Launch(r);
    }
    uint32_t RiderKnockOff(uint32_t r) override {
        rr::sim::RiderLayer layer(g_, ram_, rs_);
        return layer.KnockOff(r);
    }
    uint32_t ObjectSound(uint32_t e, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) override { // PORTED
        return ProductLeanEffect(g_, e, a1, a2, a3, a4, [this](const std::string& s) { s_->NoteSeam(s); });
    }

private:
    RaceSession* s_;
    GuestRam& g_;
    uint8_t* ram_;
    ProductRiderSeams rs_;
};

// RaceStep's camera slot (race.h RaceStepCamera): the PORTED ViewUpdate RASHCDG
// 0x800881B4 on view record p at RaceStep's own depth; a refusal restores the record's 1132 bytes.
// CameraCollide RASHCDG 0x800A421C (which RaceStep runs when +0x224 & 0x100, with CameraOrient after
// it) is not ported: `Test` names it and answers 0, so the eye can enter a wall.
struct SessionCamera final : rr::sim::RaceStepCamera {
    RaceSession* session = nullptr;
    GuestRam* g = nullptr;
    uint8_t* ram = nullptr;
    const rr::sim::BikeTables* t = nullptr;
    FrameLog* log = nullptr;
    uint32_t* counter = nullptr;
    rr::sim::RoadRuntimeNative road;
    uint8_t* View(int32_t p) override {
        return ram + ((kViewArray + kViewStride * static_cast<uint32_t>(p)) & (GuestRam::kRamSize - 1u));
    }
    void Update(int32_t p, int32_t dt) override {
        const uint32_t v = kViewArray + kViewStride * static_cast<uint32_t>(p);
        std::vector<uint8_t> before(kViewStride);
        g->ReadBlock(v, before.data(), kViewStride);
        ProductCameraSeams seams(session, *counter);
        rr::sim::CameraPort port(*g, *t, seams, road);
        port.ViewUpdate(v, dt, kArenaStepSp - kRaceStepFrame);
        log->cameraRan = true;
        if (port.Failed()) {
            log->cameraDeclined = true;
            log->cameraFailAt = port.FailAddress();
            char b[240];
            std::snprintf(b, sizeof(b),
                          "the PORTED ViewUpdate RASHCDG 0x800881B4 refused a frame (%s at 0x%08X): the view record "
                          "kept its previous bytes",
                          port.FailReason(), port.FailAddress());
            session->NoteSeam(b);
            g->ClearFault();
            g->WriteBlock(v, before.data(), kViewStride);
        }
    }
    // RASHCDG 0x800A421C CameraCollide, PORTED (camera_collide.h): the eye pushed out of the
    // road walls, the listed static volumes and the ground; its callees natively (WallContact's unported
    // ones as the collision pass serves them). A refusal restores the view record and answers 0.
    int32_t Test(int32_t p) override {
        const uint32_t v = kViewArray + kViewStride * static_cast<uint32_t>(p);
        std::vector<uint8_t> before(kViewStride);
        g->ReadBlock(v, before.data(), kViewStride);
        ProductCollisionCallees c(session, ram, *g);
        c.native.tables = t;
        c.native.note = [this](const std::string& s) { session->NoteSeam(s); };
        uint32_t v0 = 0;
        if (!rr::sim::CameraCollide(*g, v, kArenaStepSp - kRaceStepFrame, *t, c, v0)) {
            ++log->collideSkipped;
            session->NoteSeam("the PORTED CameraCollide RASHCDG 0x800A421C refused a frame: the view record kept its "
                              "pre-call bytes");
            g->ClearFault();
            g->WriteBlock(v, before.data(), kViewStride);
            return 0;
        }
        if (v0 != 0) ++log->collidePushes;
        return static_cast<int32_t>(v0);
    }
    void Apply(int32_t p) override { // RASHCDG 0x80086E1C CameraOrient, PORTED (camera.h)
        const uint32_t v = kViewArray + kViewStride * static_cast<uint32_t>(p);
        ProductCameraSeams seams(session, *counter);
        rr::sim::CameraPort port(*g, *t, seams, road);
        port.Orient(v, kArenaStepSp - kRaceStepFrame);
        if (port.Failed()) {
            g->ClearFault();
            session->NoteSeam("the PORTED CameraOrient RASHCDG 0x80086E1C refused the re-orient after CameraCollide");
        }
    }
};

struct SessionDirectorCalls final : rr::sim::RaceDirectorCallbacks {
    std::function<void(const char*)> seam;
    std::function<void(int32_t, uint32_t, uint32_t*, uint32_t*)> drivePass;
    std::function<void(int32_t, uint32_t, uint32_t)> runCommands;
    // the two-rider arms (jail_session.cpp): Remount 0x800903F4(bike, 1), FightUpdate 0x800C035C(partner, target, dt),
    // RiderRecover 0x80092E04(partner, dt) - PORTED, run by the session; unset: named seams
    std::function<void(int32_t)> remount;
    std::function<void(int32_t, uint32_t, int32_t)> partnerCommand;
    std::function<void(int32_t, int32_t)> partnerFinish;
    void RiderRemount(int32_t p) override {
        if (remount) remount(p);
        else seam("RASHCDG 0x800903F4 the remount is not ported (its ten callees are not)");
    }
    std::function<void()> results; // SLUS 0x8003F708 ResultsPrepare, PORTED (passes.h); unset: RRJB_PASSES=session
    void ResultsPrepare() override {
        if (results) results();
        else seam("RRJB_PASSES=session: SLUS 0x8003F708 ResultsPrepare (the places of the riders still racing) is not run");
    }
    std::function<void()> raceOver; // SLUS 0x80018C1C(1), PORTED (strike_product.h)
    void RaceOverSignal() override {
        if (raceOver) raceOver();
        else seam("SLUS 0x80018C1C the race-over signal is not ported");
    }
    void AiDrivePass(int32_t dt, uint32_t skip, uint32_t* maskB, uint32_t* maskC) override {
        drivePass(dt, skip, maskB, maskC);
    }
    void AiRunCommands(int32_t dt, uint32_t skip, uint32_t maskB) override { runCommands(dt, skip, maskB); }
    std::function<void(int32_t, uint32_t, uint32_t)> brain; // ai_race.h RunAiBrainPass, PORTED
    void AiBrainPass(int32_t dt, uint32_t skip, uint32_t maskC) override { brain(dt, skip, maskC); }
    void PartnerCommand(int32_t p, uint32_t target, int32_t dt) override { if (partnerCommand) partnerCommand(p, target, dt); }
    void PartnerFinish(int32_t p, int32_t dt) override { if (partnerFinish) partnerFinish(p, dt); }
};

} // namespace

void RaceSession::Frame(const PadState& pad, int32_t ticks, const PadState* pad2) {
    log_ = FrameLog();
    log_.frame = ++frameNumber_;
    log_.ticks = ticks;
    log_.pad = pad;
    uint8_t* const ram = arena_.Ram();
    GuestRam g(ram, kArenaGp);

    ApplyInput(pad);

    // ---- the listener and the PORTED engine note, as AudioFrame calls them once a
    // game frame: SetListener with the bike's position, velocity +0x1C8/+0x1D0 and (ours) its own
    // heading angle for the camera yaw; then EngineNote / RoadNote on the bike, its rider record and
    // its stat block. RoadNote pulls the game's LCG while the bike slides: the seed goes in and out.
    if (!sounds_.Attached()) { // the world mode runs AudioFrame at the END of the frame instead
        EntityView player(bikes_[0].entity.data());
        sounds_.SetListener(0, static_cast<int32_t>(player.U32(ent::kObbCentre + 0)),
                            static_cast<int32_t>(player.U32(ent::kObbCentre + 8)),
                            static_cast<int32_t>(player.U32(0x1C8)), static_cast<int32_t>(player.U32(0x1D0)),
                            static_cast<int32_t>(player.U32(ent::kFacing)));
        sounds_.EngineFrame(std::span<const uint8_t>(bikes_[0].entity.p, kEntitySize),
                            std::span<const uint8_t>(bikes_[0].owner.p, kRiderBytes),
                            std::span<const uint8_t>(bikes_[0].stats, 0x1C0), *randSeed_);
    }

    GoEvent go(this, g, log_);
    const auto seam = [this](const char* what) { NoteSeam(what); };
    const uint8_t* altKind = At(kAltKindTable);
    ProductRiderSeams riderSeams(this, g);
    modeSeams_ = &riderSeams; // the mode functions' stance seams, this frame only (race_modes.cpp)
    struct ModeScope { rr::sim::StanceSeams*& p; ~ModeScope() { p = nullptr; } } modeScope{modeSeams_};
    raceFlags_ = g.U32(rr::sim::kModeArrestWord); // *(0x8005AD48), the arena's (the player cop's FSM writes it)
    StanceSink stance(g, riderSeams, log_);
    const bool wholeClock = rr::game::PassesWhole();
    if (wholeClock) {
        // The frame's clock (passes.h). OURS: the VSync callback SLUS 0x8001B700's `+0x0C += 5`
        // per vertical blank is the frame's ticks (1/300 s) the platform measured. Then the PORTED pad poll clock
        // region [0x8001CD00, 0x8001CD7C) of SLUS 0x8001CB3C - the racing state on the first race frame, +0x18
        // the frame's ticks - with the pause test before it (below; RRJB_PAUSE=off answers "no pause"), and
        // the poll's +0x14 = +0x0C (0x8001CD94).
        const uint32_t gsA = g.U32(kGameStatePtr);
        g.W32(gsA + 0x0Cu, g.U32(gsA + 0x0Cu) + static_cast<uint32_t>(std::max(ticks, 0)));
        // The pause (pause_product.h): the frame copy's press codes first (the slot stamp), then the PORTED
        // pause test region 0x8001CBEC..0x8001CD00 - its s0 is the clock region's; RRJB_PAUSE=off: 0, no pause.
        uint32_t pauseS0 = 0;
        if (rr::game::PauseOn()) {
            FightPadStamp(pad, 0);
            if (players_ == 2) FightPadStamp(pad2 != nullptr ? *pad2 : PadState{}, 1);
            pauseS0 = PausePoll();
        }
        if (rr::sim::PadPollClock(g, pauseS0) != 0) // 0x8001CE04..0x8001CE1C: RaceOverSignal(state == 3) (strike_product.h)
            rr::game::ProductRaceOverSignal(sounds_, g.S8(g.U32(kGameStatePtr)) == 3 ? 1u : 0u,
                                            [this](const std::string& s) { NoteSeam(s); },
                                            "SLUS 0x80018C1C(state == 3), the pad poll's audio pause call on a first race "
                                            "frame (0x8001CDB4), is not run: the product's sound runtime has no pause to take back");
        g.W32(gsA + 0x14u, g.U32(gsA + 0x0Cu));
        ++rr::game::PassRunTotals().clockFrames;
    }
    FightPadPass(pad, riderSeams); // the pad handler's combat tail, after its flag merge (fight_session.cpp)
    TauntPad(riderSeams);          // its L2 arm: the taunt (speech_session.cpp)
    log_.padControls = RunPadControls(ram, kArenaGp, bikes_[0].entityAddress, pad.device); // PORTED (pad_product.h)
    if (!log_.padControls.ok) { g.ClearFault(); NoteSeam("the PORTED pad reader region SLUS 0x8001CFB0 faulted"); }
    if (players_ == 2) MpPlayerPad(pad2 != nullptr ? *pad2 : PadState{}, riderSeams); // mp_session.cpp
    rr::game::RumbleFrame(ram, kArenaGp); // the actuator service and the pad reader's rumble regions (rumble_product.h)
    const int16_t routeArmed = g.S16(rr::sim::kRouteRecordCount);

    auto binding = [&](const uint8_t* entity) {
        rr::sim::RouteBinding b;
        const uint32_t ro = static_cast<uint32_t>(ReadS32(entity, 0x1AC));
        b.routeObject = RawAt(ram, ro, 120);
        if (b.routeObject != nullptr) {
            b.firstWord = static_cast<uint32_t>(ReadS32(b.routeObject, 0));
            b.legs = b.routeObject + 20;
            b.legCount = ReadS32(b.routeObject, 0x0C);
        }
        b.routeArmed = routeArmed;
        return b;
    };

    // ---- the progress pass's view of the field
    std::vector<rr::sim::ProgressNode> progressNodes(bikes_.size());
    for (size_t i = 0; i < bikes_.size(); ++i) {
        progressNodes[i].entity = bikes_[i].entity.data();
        progressNodes[i].riderDef = bikes_[i].riderDef.data();
        progressNodes[i].owner = bikes_[i].owner.data();
    }

    // ---- the PORTED finish test, RASHCDG 0x800B9958, on the arena's records: the finish record
    // *(0x800D6188) and the route records are the route arena's, the slice is the bike's own +0x154.
    std::vector<rr::sim::PlaceNode> placePool(bikes_.size());
    auto refreshPlacePool = [&]() {
        for (size_t i = 0; i < bikes_.size(); ++i) {
            placePool[i].entity = bikes_[i].entity.data();
            placePool[i].riderDef = bikes_[i].riderDef.data();
            placePool[i].binding = binding(bikes_[i].entity.data());
        }
    };
    refreshPlacePool();
    rr::sim::ComputePlaceEnv sharedPlaceEnv;
    sharedPlaceEnv.gameState = gameState_.data();
    sharedPlaceEnv.liveBikes = *liveBikes_;
    sharedPlaceEnv.raceFlags = raceFlags_;
    sharedPlaceEnv.pool = placePool.data();
    sharedPlaceEnv.poolHigh = static_cast<int32_t>(bikes_.size()) - 1;
    sharedPlaceEnv.poolCount = static_cast<int32_t>(bikes_.size());

    size_t finishSubject = 0;
    SessionFinishCalls finishCalls;
    finishCalls.session = this;
    ProductStampCallees stampCalls(g, ram, riderSeams, log_);
    finishCalls.stampResult = [&]() {
        ++log_.stampResults;
        if (!rr::sim::StampResult(g, bikes_[finishSubject].entityAddress, stampCalls) || g.Faulted()) {
            g.ClearFault();
            NoteSeam("RASHCDG 0x800BC7CC StampResult (PORTED, under FinishTest) refused: a callee failed or an "
                     "address faulted");
        }
    };
    finishCalls.recordFinish = [&](uint32_t handle, int32_t flag) {
        if (handle >= bikes_.size()) {
            NoteSeam("SLUS 0x8003F680 was asked for a handle this session has no bike for");
            return;
        }
        rr::sim::FinishOrderEnv fo;
        fo.entity = bikes_[handle].entity.data();
        fo.riderDef = bikes_[handle].riderDef.data();
        fo.table = At(kFinishOrder);
        fo.tableBytes = static_cast<int32_t>(kFinishOrderBytes);
        if (!rr::sim::RecordFinish(flag, fo))
            NoteSeam("SLUS 0x8003F680 declined: the place it was handed falls outside the table window");
    };
    finishCalls.place = [&](int32_t* p) {
        refreshPlacePool();
        return rr::sim::ComputePlace(bikes_[finishSubject].entity.data(), bikes_[finishSubject].riderDef.data(),
                                     placePool[finishSubject].binding, 1, sharedPlaceEnv, p);
    };

    FinishAdapter finish;
    finish.fn = [&](int32_t index) -> int32_t {
        const size_t i = static_cast<size_t>(index);
        if (index < 0 || i >= bikes_.size()) return 0;
        finishSubject = i;
        uint8_t* e = bikes_[i].entity.data();
        rr::sim::FinishTestEnv fe;
        fe.entity = e;
        fe.riderDef = bikes_[i].riderDef.data();
        fe.owner = bikes_[i].owner.data();
        fe.partnerOwner = nullptr; // a two-rider bike's partner rider (jail_session.cpp), below
        fe.gameState = gameState_.data();
        fe.entityRoute = RawAt(ram, static_cast<uint32_t>(ReadS32(e, 0x1AC)), 120);
        fe.ownerRoute = RawAt(ram, static_cast<uint32_t>(ReadS32(bikes_[i].owner.p, 0x1AC)), 120);
        fe.partnerRoute = nullptr;
        if (const uint32_t pb = static_cast<uint32_t>(ReadS32(e, 0x358)); pb != 0u) { // the passenger (jail_session.cpp)
            const uint32_t pr = static_cast<uint32_t>(ReadS32(At(pb), 0x354));
            fe.partnerOwner = RawAt(ram, pr, 628);
            if (fe.partnerOwner != nullptr) fe.partnerRoute = RawAt(ram, static_cast<uint32_t>(ReadS32(fe.partnerOwner, 0x1AC)), 120);
        }
        fe.routeRecord = At(finishRecord_);
        const uint32_t sl = static_cast<uint32_t>(ReadS32(e, 0x154));
        const uint8_t* slice = RawAt(ram, sl, 52);
        fe.sliceValid = slice != nullptr;
        if (slice != nullptr) {
            fe.slice.m = reinterpret_cast<const int16_t*>(slice + 2);
            fe.slice.pos = reinterpret_cast<const int32_t*>(slice + 20);
            fe.sliceAlongBase = ReadS32(slice, 40);
            fe.sliceIndex = ReadS16(slice, 0);
        }
        const uint8_t* sub = RawAt(ram, static_cast<uint32_t>(ReadS32(e, 0x150)), 28);
        fe.chunkValid = sub != nullptr;
        if (sub != nullptr) fe.chunkLast = ReadS16(sub, 10);
        fe.milestones = At(kMilestones);
        fe.milestoneCount = 256;
        fe.routeArmed = routeArmed;
        fe.raceOverFlag = skipResults_;
        fe.timeLimit = timeLimit_;
        fe.timeBase = timeBase_;
        fe.startDir = startDir_;
        fe.postDelay = countdown_;
        fe.jailbreakClock = At(kJailbreakClock);
        fe.finishOrder = At(kFinishOrder + 16u);
        fe.finishOrderCount = static_cast<int32_t>(kFinishOrderBytes) - 16;
        fe.player2 = players_ == 2 ? bikes_[1].entity.data() : nullptr;
        fe.p1Entity = bikes_[0].entity.data();
        fe.p1RiderDef = bikes_[0].riderDef.data();
        fe.playerRiderDef[0] = bikes_[0].riderDef.data();
        fe.playerRiderDef[1] = players_ == 2 ? bikes_[1].riderDef.data() : nullptr;
        fe.calls = &finishCalls;
        int32_t answer = 0;
        if (!rr::sim::FinishTest(fe, &answer)) {
            ++finish.declined;
            NoteSeam("RASHCDG 0x800B9958 declined a case rather than guessing: it reached "
                     "something this session did not resolve");
            return 0;
        }
        return answer;
    };

    // ---- the drive pass's view: the slices are the bikes' own +0x154, the look-ahead the PORTED
    // SLUS 0x800386DC on the arena at AiDrive's own sp.
    std::vector<rr::sim::AiDriveNode> driveNodes(bikes_.size());
    std::vector<std::unique_ptr<rr::sim::GuestAiRoadQuery>> aiRoads;
    struct CountingRoad final : rr::sim::AiRoadQuery {
        rr::sim::AiRoadQuery* inner = nullptr;
        size_t* count = nullptr;
        bool LookAhead(int32_t ahead, int32_t along, int32_t dir, uint32_t cursorOffset, uint32_t aimOffset,
                       int16_t sliceAxis[3], int32_t sliceOrigin[3]) override {
            ++*count;
            return inner->LookAhead(ahead, along, dir, cursorOffset, aimOffset, sliceAxis, sliceOrigin);
        }
    };
    std::vector<CountingRoad> aiCounted(bikes_.size());
    auto refreshDriveNodes = [&]() {
        for (size_t i = 0; i < bikes_.size(); ++i) {
            uint8_t* e = bikes_[i].entity.data();
            const uint8_t* slice = RawAt(ram, static_cast<uint32_t>(ReadS32(e, 0x154)), 52);
            driveNodes[i].entity = e;
            driveNodes[i].riderDef = bikes_[i].riderDef.data();
            driveNodes[i].stats = bikes_[i].stats;
            driveNodes[i].slicePos = slice ? reinterpret_cast<const int32_t*>(slice + 20) : nullptr;
            driveNodes[i].sliceTangent = slice ? reinterpret_cast<const int16_t*>(slice + 14) : nullptr;
            driveNodes[i].rider = bikes_[i].owner.data();
            driveNodes[i].riderAddress = bikes_[i].ownerAddress;
        }
    };
    for (size_t i = 0; i < bikes_.size(); ++i) {
        aiRoads.push_back(std::make_unique<rr::sim::GuestAiRoadQuery>(g, ArenaEntity(i), nullptr, kArenaAiDriveSp));
        aiCounted[i].inner = aiRoads.back().get();
        aiCounted[i].count = &log_.aiLookAheads;
        driveNodes[i].road = &aiCounted[i];
    }
    const rr::sim::AiSpeedEnv speedEnv = SpeedEnv();

    const std::vector<uint8_t> noFight(12u * 64u, 0);
    rr::sim::AiDrivePassEnv drivePassEnv;
    drivePassEnv.gameState = gameState_.data();
    drivePassEnv.raceBank = static_cast<int32_t>(gameState_[0x3C]);
    drivePassEnv.altKindTable = altKind;
    drivePassEnv.fightRecords =
        g.U32(kFightTablePtr) != 0u ? RawAt(ram, g.U32(kFightTablePtr), 12u * 64u) : noFight.data();
    drivePassEnv.attackerMask = reinterpret_cast<const uint16_t*>(At(kAttackerMask));
    drivePassEnv.stance = &stance;
    drivePassEnv.speed = speedEnv;
    struct CopArm final : rr::sim::AiPlayerCopArm { // AiDrive's player-cop arm (ai.h; race_modes.cpp)
        RaceSession* s;
        uint8_t* ram;
        CopArm(RaceSession* x, uint8_t* r) : s(x), ram(r) {}
        uint32_t A(uint8_t* e) const { return 0x80000000u + static_cast<uint32_t>(e - ram); }
        bool Fsm(uint8_t* e, int32_t dt, uint32_t& v0) override { return s->ModeArrestFsm(A(e), dt, kArenaAiDriveSp - 64u, v0); }
        bool ViewReset(uint8_t* e) override { return s->ModeViewReset(A(e)); }
    } copArm(this, ram);
    drivePassEnv.copArm = &copArm;

    SessionDirectorCalls calls;
    calls.seam = seam;
    calls.raceOver = [this] { // strike_product.h: the race loop's audio pause at the race's end (0x800B9608)
        rr::game::ProductRaceOverSignal(sounds_, 1, [this](const std::string& s) { NoteSeam(s); },
                                        "SLUS 0x80018C1C the race-over signal is not ported");
    };
    if (wholeClock)
        calls.results = [&]() { // SLUS 0x8003F708, PORTED; RecordFinish SLUS 0x8003F680 PORTED (race.h) on the arena
            struct Rec final : rr::sim::ResultsCallees {
                RaceSession* s;
                explicit Rec(RaceSession* ss) : s(ss) {}
                bool RecordFinish(uint32_t handle, int32_t flag) override {
                    if (handle >= s->bikes_.size()) return false;
                    rr::sim::FinishOrderEnv fo;
                    fo.entity = s->bikes_[handle].entity.data();
                    fo.riderDef = s->bikes_[handle].riderDef.data();
                    fo.table = s->At(kFinishOrder);
                    fo.tableBytes = static_cast<int32_t>(kFinishOrderBytes);
                    return rr::sim::RecordFinish(flag, fo);
                }
            } rec(this);
            ++rr::game::PassRunTotals().resultsPrepared;
            if (!rr::sim::ResultsPrepare(g, rec) || g.Faulted()) {
                g.ClearFault();
                NoteSeam("SLUS 0x8003F708 ResultsPrepare (PORTED) refused: a handle this session has no bike for, or a fault");
            }
        };
    calls.drivePass = [&](int32_t dt, uint32_t skip, uint32_t* maskB, uint32_t* maskC) {
        refreshDriveNodes();
        log_.aiDrivePassComplete =
            rr::sim::AiDrivePass(driveNodes.data(), driveNodes.size(), dt, skip, maskB, maskC, drivePassEnv);
        if (g.Faulted()) {
            NoteSeam("the PORTED look-ahead met an address the console would fault on in this session's arena");
            g.ClearFault();
        }
        if (!log_.aiDrivePassComplete) NoteSeam("RASHCDG 0x800BA304 the drive pass stopped at an arm that is not ported");
    };
    // The AI's product hooks (ai_race.h): the stance event is StampResult's PORTED StanceLayer.
    rr::game::AiProductHooks aiHooks;
    aiHooks.seam = [this](const std::string& s) { NoteSeam(s); };
    aiHooks.stanceEvent = [&](uint32_t ev, uint32_t r, uint32_t p) { return stampCalls.StanceEvent(ev, r, p); };
    aiHooks.fight = [&](uint32_t e, uint32_t target) { return FightCommand(e, target, riderSeams); }; // op 16
    if (rr::game::StrikeOn()) // op 9's strike (strike_product.h); RRJB_STRIKE=off: the named seam
        aiHooks.strike = [&](uint32_t e, uint32_t t) { return StrikeCommand(e, t, riderSeams); };
    aiHooks.leaveRace = [&](uint32_t e, int32_t d, uint32_t sp) { // op 18 (recover_race.h)
        return RecoverCall(0x80092E04u, {e, static_cast<uint32_t>(d)}, sp, riderSeams);
    };
    aiHooks.copRelease = [&](uint32_t e, uint32_t sp) { return RecoverCall(0x80092AD4u, {e}, sp, riderSeams); };
    aiHooks.endRace = [&](uint32_t e, int32_t how, uint32_t sp) { // the arrest's EndRace (cop_race.h), PORTED
        return RecoverCall(0x80092C7Cu, {e, static_cast<uint32_t>(how)}, sp, riderSeams);
    };
    aiHooks.riderSpeech = [&](uint32_t h, int32_t crash, uint32_t sp) { return RiderSpeech(h, crash, sp, &riderSeams); };
    aiHooks.arrest = [&](uint32_t e, uint32_t t, uint32_t how, uint32_t sp) { return ModeArrest(e, t, how, sp); }; // race_modes.cpp
    aiHooks.jailRelease = [&](uint32_t e, uint32_t sp) { return ModeJailRelease(e, sp); };
    aiHooks.jailbreakFinish = [&](uint32_t sp) { return ModeJailFinish(sp); }; // jail_session.cpp
    aiHooks.bustedMusic = [this]() { // SLUS 0x8001B3C8, PORTED (takedown_product.h)
        return ProductBustedMusic(sounds_, [this](const std::string& s) { NoteSeam(s); });
    };
    aiHooks.computePlace = [&](uint32_t bike, int32_t mode, int32_t* out) {   // SLUS 0x800138E8, PORTED
        refreshPlacePool();
        for (size_t i = 0; i < bikes_.size(); ++i)
            if (bikes_[i].entityAddress == bike)
                return rr::sim::ComputePlace(bikes_[i].entity.data(), bikes_[i].riderDef.data(), placePool[i].binding,
                                             mode, sharedPlaceEnv, out);
        return false;
    };
    // The planner's and the brain's own stacks (ours, each untouched by anything else: the brain reads
    // back a halfword of its previous frame).
    constexpr uint32_t kAiPlanSp = 0x801FC800u, kAiBrainSp = 0x801FD000u;
    calls.brain = [&](int32_t dt, uint32_t skip, uint32_t maskC) {
        log_.aiBrainRan = true;
        log_.aiBrainOk = rr::game::RunAiBrainPass(ram, kArenaGp, dt, skip, maskC, kAiBrainSp, aiHooks, log_.ai);
        if (!log_.aiBrainOk) {
            g.ClearFault();
            NoteSeam("RASHCDG 0x800BCD48 (PORTED, ai_brain.h) refused a frame");
        }
        rr::game::CheatAfterAi(ram, kArenaGp); // cheats.h: the sparring partners' pace and aim, before the bike step
    };
    calls.remount = [&](int32_t p) { // RASHCDG 0x800903F4(player p's bike, 1), PORTED (recover_race.h)
        RecoverCall(0x800903F4u, {bikes_[static_cast<size_t>(p)].entityAddress, 1u}, kArenaStepSp, riderSeams);
    };
    calls.partnerCommand = [&](int32_t p, uint32_t target, int32_t) { // FightUpdate 0x800C035C on the partner (fight_session.cpp)
        FightCommand(g.U32(bikes_[static_cast<size_t>(p)].entityAddress + 0x358u), target, riderSeams);
    };
    calls.partnerFinish = [&](int32_t p, int32_t dt) { // RiderRecover 0x80092E04 on the partner, PORTED (recover_race.h)
        RecoverCall(0x80092E04u, {g.U32(bikes_[static_cast<size_t>(p)].entityAddress + 0x358u), static_cast<uint32_t>(dt)},
                    kArenaStepSp, riderSeams);
    };
    calls.runCommands = [&](int32_t dt, uint32_t skip, uint32_t maskB) {
        // RASHCDG 0x800BA4CC on the arena, every arm (ai_race.h RunAiCommandPass; ai_cmd.h)
        log_.aiRunCommandsComplete =
            rr::game::RunAiCommandPass(ram, kArenaGp, dt, skip, maskB, kArenaAiDriveSp, aiHooks, log_.ai);
        if (!log_.aiRunCommandsComplete) {
            g.ClearFault();
            NoteSeam("RASHCDG 0x800BA4CC (PORTED, ai_cmd.h) refused a frame: it met an address this arena "
                     "does not hold");
        }
    };

    std::vector<rr::sim::RaceDirectorPlayer> directorPlayers(static_cast<size_t>(players_));
    for (size_t p = 0; p < directorPlayers.size(); ++p) { // player p: pool-0 slot p, view record p
        directorPlayers[p].entity = bikes_[p].entity.data();
        directorPlayers[p].riderDef = bikes_[p].riderDef.data();
        directorPlayers[p].owner = bikes_[p].owner.data();
        directorPlayers[p].partner = nullptr;
        directorPlayers[p].partnerOwner = nullptr;
        if (const uint32_t pb = static_cast<uint32_t>(ReadS32(bikes_[p].entity.p, 0x358)); pb != 0u) { // jail_session.cpp
            directorPlayers[p].partner = RawAt(ram, pb, 1096);
            if (directorPlayers[p].partner != nullptr)
                directorPlayers[p].partnerOwner = RawAt(ram, static_cast<uint32_t>(ReadS32(directorPlayers[p].partner, 0x354)), 628);
        }
        directorPlayers[p].view = At(kViewArray + kViewStride * static_cast<uint32_t>(p));
    }

    rr::sim::RaceDirectorEnv directorEnv;
    directorEnv.gameState = gameState_.data();
    directorEnv.players = directorPlayers.data();
    directorEnv.playerCount = players_;
    directorEnv.kindTable = altKind;
    directorEnv.postDelay = countdown_;
    directorEnv.postLimit = postLimit_;
    directorEnv.skipResults = skipResults_;
    directorEnv.calls = &calls;
    directorEnv.progress.gameState = gameState_.data();
    directorEnv.progress.liveBikes = liveBikes_;
    directorEnv.progress.bikes = progressNodes.data();
    directorEnv.progress.count = static_cast<int32_t>(progressNodes.size());
    directorEnv.progress.finish = &finish;

    SessionChildren children;
    children.seam = seam;
    children.plan = [&](int32_t acc) {   // RASHCDG 0x800B8018 (ai_race.h RunAiPlan, ai_plan.h)
        log_.aiPlanRan = true;
        log_.aiPlanOk = rr::game::RunAiPlan(ram, kArenaGp, acc, kAiPlanSp, aiHooks, log_.ai);
        if (!log_.aiPlanOk) {
            g.ClearFault();
            NoteSeam("RASHCDG 0x800B8018 (PORTED, ai_plan.h) refused a pass");
        }
    };
    children.director = [&](int32_t dt) {
        log_.raceDirectorRan = true;
        const bool complete = rr::sim::RaceDirector(dt, directorEnv);
        log_.progressPassComplete = complete;
        if (!complete) NoteSeam("RASHCDG 0x800B9414 stopped early - it reached a player this session did not resolve");
    };
    children.world = [&](int32_t dt) { WorldPass(dt); };
    children.engine = [&](int32_t dt) { RiderPass(dt); };
    children.collision = [&](int32_t dt) { CollisionPass(dt); };
    children.spawner = [&](int32_t dt) { SpawnerTick(dt); };
    children.presentation = [&](int32_t dt) { // RASHCDG 0x80090814, PORTED (present.h)
        GuestRam pg(arena_.Ram(), kArenaGp);
        ProductPresentSeams ps(this, pg, arena_.Ram());
        rr::sim::PresentLayer layer(pg, ps);
        layer.Pass(dt);
        if (layer.Failed()) {
            pg.ClearFault();
            NoteSeam("RASHCDG 0x80090814 the presentation pass (PORTED) declined a frame");
        }
    };

    rr::sim::BikeTables camTables;
    camTables.sincos = tables_.sincos.data();
    camTables.asin = tables_.atanU16.data();
    camTables.atan = tables_.atan.data();
    camTables.rsqrt = tables_.rsqrtTable.data();
    camTables.sqrt = tables_.sqrtWindow.data() + 0x800 / 2;
    GuestRam camRam(ram, kArenaGp);
    camRam.SetScratchpad(scratchpad_.data());
    SessionCamera camera;
    camera.session = this;
    camera.g = &camRam;
    camera.ram = ram;
    camera.t = &camTables;
    camera.log = &log_;
    camera.counter = &camCounter_;

    rr::sim::RaceStepEnv step;
    step.gameState = gameState_.data();
    step.frameFlag = frameFlag_;
    step.camera = &camera;
    step.tick.eventAcc = eventAcc_;
    step.tick.planCount = planCount_;
    step.tick.gameState = gameState_.data();
    step.tick.planTable = reinterpret_cast<const int32_t*>(At(kPlanTable)); // GLOBALS.BI, in the arena
    step.tick.children = &children;
    step.tick.countdown.player1 = bikes_[0].entity.data();
    step.tick.countdown.rider1 = bikes_[0].riderDef.data();
    step.tick.countdown.player2 = players_ == 2 ? bikes_[1].entity.data() : nullptr;
    step.tick.countdown.rider2 = players_ == 2 ? bikes_[1].riderDef.data() : nullptr;
    step.tick.countdown.raceClock = gameState_.data() + 0x10;
    step.tick.countdown.countdown = countdown_;
    step.tick.countdown.eventAcc = eventAcc_;
    step.tick.countdown.go = &go;

    if (wholeClock) {
        const uint32_t gsB = g.U32(kGameStatePtr);
        if (g.U8(gsB + 3u) != 0) {
            // main SLUS 0x80012360..0x800123E8, THE FIRST RACE FRAME, with the PORTED callees:
            // the post-race delay 5.00 s, two view halfwords cleared, RaceTick(1094), the camera per player at
            // 1094, ViewPass 0x8008CFDC and AnimationPass 0x8005E1D8 (AnimationPass() runs both), the byte cleared.
            *postLimit_ = 0x00050000;
            g.W16(0x800CD542u, 0);
            g.W16(0x800CD540u, 0);
            rr::sim::RaceTick(1094, step.tick);
            camera.Update(0, 1094);
            if (g.U32(gsB + 0x30u) >= 2u) camera.Update(1, 1094);
            AnimationPass();
            g.W8(gsB + 3u, 0);
            ++rr::game::PassRunTotals().firstFrames;
        } else {
            rr::sim::FrameDelta(g); // SLUS 0x8001C428, PORTED: game_state+0x20
        }
    } else {
        gameState_[0x18] = static_cast<uint8_t>(std::clamp(ticks, 0, 255));
    }
    if (streamPorted_) StreamFrameStep(); // GameFrame's first step: SLUS 0x8002305C, PORTED (stream_session.cpp)
    PauseStall(); // GameFrame's stall switch 0x80011C70, PORTED (pause_product.cpp)
    rr::sim::RaceStep(step);
    // GameFrame step 6, after the race tick - while racing, or with the countdown / post-race word positive
    // (0x80011D98 / 0x80011DC8: not while paused; RRJB_PAUSE=off runs it every frame).
    if (!rr::game::PauseOn() || gameState_[0] == 1 || *countdown_ > 0) AnimationPass(gameState_[0] == 1 || !rr::game::PauseOn());
    { // the cell streamer runs every frame, the countdown included (SLUS 0x800237B8 from the main loop; OURS: its
      // rule is CellStreamPass's), then 0x800358C0's draw list, PORTED (cell_view.h)
        GuestRam cg(ram, kArenaGp);
        if (!streamPorted_) { // else the PORTED streamer keeps the slots and the resource list itself
            CellStreamPass(cg);
            std::vector<rr::game::LoadedCell> loaded; // OURS: the resource list mirrors the resident cells (cell_view.h)
            for (const CatalogCell& c : cells_) if (c.at != 0) loaded.push_back({c.at, c.body});
            rr::game::WriteResourceList(cg, std::move(loaded));
        }
        rr::game::StreamSample(cg, frameNumber_, ArenaEntity(0)); // the streamer's census, both modes
        rr::game::CellDrawPass(cg, players_);
        if (streamPorted_) rr::game::StreamSkyPass(cg, frameNumber_); // the sky's pick RASHCDG 0x800650D0
        rr::game::DrawLoopCells(cg, players_); // 0x8008D56C's cell tests 0x8008B99C, PORTED: +0xB0 of views, bikes, riders
        rr::game::DrawLoopPools(cg, players_); // 0x8008D56C's pools 2..5 (cars, props): +0xB0, +0x0C.., part 0
        rr::game::CellSortPass(cg, players_); // 0x800358C0's 0x800353C4 / 0x80036438 / 0x80035680, PORTED (cell_sort_product.h)
        if (rr::game::HazardsOn()) rr::game::RunHazardDraw(cg, players_, kArenaStepSp); // GameFrame's 0x800A2138 per view (hazard_product.h)
    }

    log_.dt = 218 * static_cast<int32_t>(gameState_[0x1C]);
    log_.raceClock = ReadS32(gameState_.p, 0x10);
    log_.countdown = *countdown_;
    log_.countdownRunning = (bikes_[0].riderDef[0] & 0x40u) != 0;
    if (!log_.countdownRunning) countdownOver_ = true;
    log_.finishTestsRequested = finish.calls;
    log_.finishTestsDeclined = finish.declined;
    for (const RaceBike& b : bikes_)
        if (ReadS32(b.riderDef, 0x28) != 0) ++log_.ridersFinished;

    // The place, from the PORTED ComputePlace, for the HUD.
    refreshPlacePool();
    rr::sim::ComputePlaceEnv placeEnv = sharedPlaceEnv;
    for (size_t i = 0; i < bikes_.size(); ++i) {
        int32_t place = 0;
        if (!rr::sim::ComputePlace(bikes_[i].entity.data(), bikes_[i].riderDef.data(), placePool[i].binding, 0,
                                   placeEnv, &place))
            continue;
        bikes_[i].place = place;
        if (!wholeClock && ReadS32(bikes_[i].riderDef, 0x28) == 0)
            bikes_[i].riderDef[0x27] = static_cast<uint8_t>(std::clamp(place, 0, 246));
    }
    if (!wholeClock)
        NoteSeam("RRJB_PASSES=session: the live place byte riderDef[+0x27] of an unfinished rider is written from the "
                 "PORTED ComputePlace by this session every frame (its writer is the PORTED AiPlan 0x800B8018, "
                 "0x800B8350)");

    // ---- the PORTED race HUD, HudFrame RASHCDG 0x8005E848 (GameFrame step 10):
    // it decides which of the layout's items to show this frame and with what values, and links them
    // into the HUD's ordering-table slot, which the renderer walks.
    if (hudReady_) {
        HudBeginFrame(g, hudAt_);
        class Callees final : public rr::sim::HudCallees {
        public:
            Callees(RaceSession* s, std::function<bool(uint32_t, int32_t, int32_t*)> place)
                : s_(s), place_(std::move(place)) {}
            bool PlaySound(uint32_t, int32_t id) override {
                ++s_->log_.hudSounds;
                s_->sounds_.PlaySound3D(0, 0, id, 0); // SLUS 0x80017BA0, PORTED
                return true;
            }
            bool StopCountdownVoice(uint32_t) override {
                if (s_->sounds_.CountdownVoiceStop()) { // SLUS 0x80016528, PORTED (countdown_voice.h)
                    ++rr::game::RumbleCounters().countdownStops;
                    rr::game::RumbleCounters().countdownHandle = s_->sounds_.CountdownVoiceStarted();
                    return true;
                }
                s_->NoteSeam("HUD: SLUS 0x80016528 (StopVoice on the countdown's voice handle gp+1964, PORTED: "
                             "countdown_voice.h) is not run: no sound world is attached to this race's arena (the "
                             "sound state's heap block takes a free cell buffer; with RRJB_MPARENA=off a two-player race's "
                             "stat array takes the last one), so 0x800164B4 started no voice either");
                return true;
            }
            bool ComputePlace(uint32_t, uint32_t bike, int32_t mode, int32_t* place) override {
                return place_(bike, mode, place);
            }
            bool HeapOverflow(uint32_t, uint32_t, uint32_t, uint32_t*) override {
                s_->NoteSeam("HUD: the packet heap filled (SLUS 0x80021C98, the heap manager, is not ported) - the "
                             "frame's HUD was refused");
                return false;
            }
            bool Unported(uint32_t, uint32_t address, const uint32_t*, int) override {
                ++s_->log_.hudSkipped;
                char what[200];
                std::snprintf(what, sizeof(what),
                              "HUD: RASHCDG 0x%08X is not ported and is skipped - what it draws is missing%s", address,
                              address == 0x800C52A0u ? " (the radar strip's rider marks)" : " (a cop-mission element)");
                s_->NoteSeam(what);
                return true;
            }

        private:
            RaceSession* s_;
            std::function<bool(uint32_t, int32_t, int32_t*)> place_;
        } callees(this, [&](uint32_t bike, int32_t mode, int32_t* out) {
            for (size_t i = 0; i < bikes_.size(); ++i)
                if (bikes_[i].entityAddress == bike)
                    return rr::sim::ComputePlace(bikes_[i].entity.data(), bikes_[i].riderDef.data(),
                                                 placePool[i].binding, mode, placeEnv, out); // SLUS 0x800138E8, PORTED
            return false;
        });
        log_.hudRan = rr::sim::HudFrame(g, callees, kArenaStepSp);
        // For the log only: the mask HudFrame's sign used (the function writes nothing but its own frame).
        log_.hudMask = rr::sim::HudWrongWayMask(g, bikes_[0].entityAddress, kArenaStepSp);
        if (!log_.hudRan || g.Faulted()) {
            char what[160];
            std::snprintf(what, sizeof(what), "HUD: HudFrame refused (a callee refused or 0x%08X faulted)",
                          g.FaultAddress());
            NoteSeam(what);
            g.ClearFault();
            log_.hudRefused = true;
        }
    }
    PauseMenuFrame(); // GameFrame's `state 3 / 4: PauseMenu 0x8002D2F4`, PORTED (pause_product.cpp)

    for (size_t i = 0; i < bikes_.size(); ++i) {
        UpdateDisplayDistance(i);
        if (i > 0 && EntityView(bikes_[i].entity.data()).S16(ent::kSteer) != 0) ++log_.aiSteered;
        if (ReadS32(bikes_[i].entity, 0x184) & 1) ++log_.offRoad;
    }

    const uint8_t* pe = bikes_[0].entity.p;
    EntityView player(bikes_[0].entity.data());
    log_.playerSpeed = static_cast<int32_t>(player.U32(ent::kSpeed));
    log_.playerSpeed240 = static_cast<int32_t>(player.U32(ent::kSpeedCopy));
    log_.playerDrive = static_cast<int32_t>(player.U32(ent::kDrive));
    log_.playerRevs = static_cast<int32_t>(player.U32(ent::kRevs));
    log_.playerGear = static_cast<int8_t>(bikes_[0].entity[ent::kGear]);
    log_.playerSteer = player.S16(ent::kSteer);
    log_.playerLean = player.S16(ent::kLean);
    log_.playerNetAccel = ReadS32(pe, 0x1E4);
    log_.playerThrottleAmt = ReadS32(pe, 0x24C);
    log_.playerLeanF = ReadS32(pe, 0x2A4);
    log_.playerLatForce = ReadS32(pe, 0x2E8);
    for (uint32_t k = 0; k < 3; ++k) {
        log_.playerHeading[k] = ReadS16(pe, 0x1C2 + 2u * k);
        log_.playerFacing[k] = ReadS16(pe, 0x210 + 2u * k);
        log_.playerBox[k] = ReadS32(pe, ent::kObbCentre + 4u * k);
    }
    log_.playerSlice = static_cast<uint32_t>(ReadS32(pe, 0x154));
    log_.playerSliceIndex = log_.playerSlice ? g.S16(log_.playerSlice) : 0;
    log_.playerRoadWord = static_cast<uint32_t>(ReadS32(pe, 0x168));
    log_.playerDirection = ReadS32(pe, 0x16C);
    log_.playerAlong = ReadS32(pe, 0x170);
    log_.playerLateralRoad = ReadS32(pe, 0x158);
    log_.playerFlags184 = static_cast<uint32_t>(ReadS32(pe, 0x184));
    log_.playerProgress = ReadS32(pe, 0x144);
    log_.playerRoute = static_cast<uint32_t>(ReadS32(pe, 0x1AC));
    log_.playerViewDistance = ReadS32(pe, 0x2C);
    log_.playerStance = static_cast<uint16_t>(bikes_[0].owner[0x220] | (bikes_[0].owner[0x221] << 8));
    log_.playerMount = ReadS32(bikes_[0].owner, 0x25C);
    for (uint32_t k = 0; k < 3; ++k) log_.playerFlags[k] = static_cast<uint32_t>(ReadS32(pe, 0x230 + 4u * k));
    log_.playerRiderFlags = static_cast<uint32_t>(ReadS32(bikes_[0].owner, 0x228));
    {
        uint32_t n = static_cast<uint32_t>(ReadS32(pe, 0x444)); // walk forward to the head
        for (int guard = 0; guard < 64 && n != 0; ++guard) {
            if (n == kListDormant || n == kListRiding || n == kListThrown || n == kListDown || n == kListSpin) break;
            n = g.U32(n + 4u);
        }
        log_.playerList = n;
    }
    const uint32_t obj = static_cast<uint32_t>(ReadS32(bikes_[0].owner, 0x21C));
    if (obj != 0) {
        log_.playerAnimFrame = g.U32(obj + 0x10u);
        log_.playerAnimFlags = g.U32(obj + 0x24u);
    }

    // The camera, read back out of view record 0, measured against the player's bike.
    {
        const uint8_t* v = At(kViewArray);
        double d2 = 0.0, behind = 0.0, a2 = 0.0;
        for (uint32_t k = 0; k < 3; ++k) {
            log_.viewEye[k] = ReadS32(v, 0xB8 + 4u * k);
            log_.viewLook[k] = ReadS32(v, 0x22C + 4u * k);
            log_.viewAim[k] = ReadS32(v, 0x23C + 4u * k);
            const double d = (static_cast<double>(log_.viewEye[k]) - log_.playerBox[k]) / 65536.0;
            d2 += d * d;
            behind += d * static_cast<double>(log_.playerFacing[k]) / 4096.0;
            const double da = (static_cast<double>(log_.viewEye[k]) - log_.viewAim[k]) / 65536.0;
            a2 += da * da;
        }
        log_.eyeToBike = std::sqrt(d2);
        log_.eyeBehind = behind;
        log_.eyeAbove = -(static_cast<double>(log_.viewEye[1]) - log_.playerBox[1]) / 65536.0;
        log_.eyeToAim = std::sqrt(a2);
        log_.viewMode = static_cast<uint32_t>(ReadS32(v, 0x21C));
        log_.viewFlags = static_cast<uint32_t>(ReadS32(v, 0x224));
        log_.viewDirector = static_cast<uint32_t>(ReadS32(v, 0x304));
    }
    // The population after the frame, and the effect records in use.
    for (size_t i = 0; i < bikes_.size(); ++i) {
        if (BikeLive(i)) ++log_.liveBikes;
        else ++log_.dormantBikes;
    }
    for (uint32_t k = 0; k < 20; ++k)
        if (((g.U32(rr::sim::kEffectPool + rr::sim::kEffectRecordBytes * k) >> 6) & 15u) != 0) ++log_.effectRecordsBusy;
    for (size_t i = 0; i < bikes_.size(); ++i) // the draw loop's per-bike set-up, PORTED (bike_pose_product.h)
        if (BikeLive(i) && g.S32(bikes_[i].entityAddress + 0xB0u) > 0 && // 0x8008D5CC: in a loaded cell
            !RunBikeParts(ram, kArenaGp, bikes_[i].entityAddress, tables_.atanU16.data())) {
            g.ClearFault();
            NoteSeam("the PORTED bike part set-up RASHCDG 0x80084E10 / 0x80066EC4 met a fault");
        }
    // SLUS 0x80018FAC AudioFrame, PORTED - the last call of GameFrame's body, on the
    // world pass's own stack (nothing is live below it by now)
    if (sounds_.Attached()) sounds_.Frame(kArenaStepSp);
    TrackAirborne(g);
    g.ClearFault();
}

// The race HUD's arena (hud_arena.h): the loader transcription at the product's own placement, which
// must be unused. The rider-record fields the HUD reads (health, machine, name id +0x26, weapon +0x2E)
// are the rider-record loader's (grid_loader.h LoadRiderRecords).
void RaceSession::BuildHud(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kArenaGp);
    // game_state+0x00 = 1, "racing": the value every race capture holds, which the pad poll SLUS 0x8001CD14
    // writes when the race starts (rules.md 3.5, not ported). HudFrame's sign and panel slides are gated
    // on it; no other ported code reads the byte (RaceDirector only writes 2 / 6 into it).
    if (!rr::game::PassesWhole()) {
        gameState_[0x00] = 1;
        NoteSeam("HUD: game_state+0x00 is set to 1 (racing) by this session (RRJB_PASSES=session: the PORTED pad "
                 "poll clock region 0x8001CD00 that writes it at 0x8001CD14 on the first race frame is off)");
    }
    hudAt_ = ProductHudPlacement(players_);
    for (uint32_t a = hudAt_.items; a < hudAt_.heapEnd; a += 4)
        if (g.U32(a) != 0) {
            char what[160];
            std::snprintf(what, sizeof(what), "HUD: not built - its placement 0x%08X..0x%08X is in use (0x%08X)",
                          hudAt_.items, hudAt_.heapEnd, a);
            NoteSeam(what);
            return;
        }
    std::string report;
    if (!BuildHudArena(g, disc, hudAt_, hudVram_, report)) {
        NoteSeam("HUD: the loader transcription refused:\n" + report);
        g.ClearFault();
        return;
    }
    ApplyPacketHeap(g, hudAt_.heapRecord, hudAt_.heapBase, hudAt_.heapEnd); // shadow_product.h
    ApplyFrameOts(g, hudAt_.heapRecord); // frame_ot.h: +0xF4.., +0x108 as SLUS 0x8001C1AC
    NoteSeam(SkyHeapSetUp(g, hudAt_.heapRecord, hudAt_.heapBase, hudAt_.heapEnd)); // sky_product.h
    // libgpu's VRAM size words, which the radar strip's draw-area clamps read (SLUS 0x80049A94): set by
    // ResetGraph, not ported; 1024 x 512 as every race capture holds them. OURS.
    g.W16(rr::sim::kGpuVramSize, 1024);
    g.W16(rr::sim::kGpuVramSize + 2u, 512);
    hudReady_ = true;
    NoteSeam("HUD: the item records, the font's header block, the string table and the packet heap sit at "
             "OURS placements 0x800DA000..0x800DFF00 (the original mallocs them; `--hudarenacheck` rebuilds "
             "them at a capture's own addresses and compares); the ordering-table clear and heap reset per "
             "frame are ours (the original's frame does them for its whole table)");
}

} // namespace rr::game
