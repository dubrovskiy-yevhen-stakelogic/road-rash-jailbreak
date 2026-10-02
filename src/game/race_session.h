#pragma once
// One running race: the ported original gameplay, the game data it reads, and - named as such -
// what stands in for the parts of the original that are not ported yet.
//
// THE WORLD LIVES IN ONE GUEST ARENA. Every record the ported code touches - the
// pool-0 bikes, the pool-1 riders, the runtime rider records, game_state, the race globals, the road
// map and its objects, the race graph and the route records, the stat blocks, the rider animation
// banks, objects and programs - sits in a 2 MiB image laid out like the console's RAM, at the
// addresses `rr-race` has them wherever that capture has one, and the executable images
// (`SLUS_010.53`, `RASHCDG.BIN`) are loaded into it first, as the console has them. The ported
// functions that walk guest pointers run on that image directly; the ported functions that take
// host pointers are handed raw views INTO it. Nothing is copied back and forth.
//
// WHAT IS THE ORIGINAL AND WHAT IS OURS: `SeamReport` (the `Seams()` list below) is built from the
// seam objects themselves and names every unported callee a run actually asked for.
//
// The per-frame order:
//   RaceStep SLUS 0x80012524 -> RaceTick RASHCDG 0x8008AB00 -> its six children:
//     RaceDirector 0x800B9414 (PORTED: ProgressPass, FinishTest, the AI passes)
//     the world pass 0x8008AC80 (not ported; its child the WHOLE per-bike step RASHCDG 0x80075EE0 is,
//       regions A..J, with every callee under it native - region J's activation and downed-rider
//       passes and EndRace's three callees included - but the named rider-layer seams and the
//       population transitions' unported callees, which are REFUSED with the arena restored)
//     the collision pass 0x800A4774 (not ported: the hook RaceSession::CollisionPass)
//     the rider/engine pass 0x8008ACE8 (not ported; its children BikeEngineStep 0x80079B20,
//       ImpactStatePass 0x80078DB4, BikeHeadingPass 0x8007AC04 and DormantDrive 0x80095724 are, run on
//       the lists as it runs them)
//   then RaceStep's camera slot: the PORTED ViewUpdate RASHCDG 0x800881B4 on view record 0x800CD898
//   (CameraCollide 0x800A421C not ported, skipped), and GameFrame's step 6: AnimationPass RASHCDG
//   0x8005E1D8 over the descriptor 0x800CE170.
#include "game/audio/sound_runtime.h"
#include "game/hud_arena.h"
#include "game/ai_race.h"
#include "game/fight_session.h"
#include "game/pad_product.h"
namespace rr::sim { struct StanceSeams; struct RiderSeams; } // stance.h
#include "game/recover_race.h"
#include "game/sim/ai.h"
#include "game/sim/bike.h"
#include "game/sim/race.h"
#include "game/sim/road_query.h"
#include "game/world.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rr::game {

// Every type-3 road object of `STREAM<set>.STR`, by object id (the BTT_ seq, 0..112), as the
// 16 KiB chunk the game loads - the first occurrence of each, since the stream repeats a resource
// at every position that needs it.
std::map<uint32_t, std::vector<uint8_t>> ReadRoadObjects(const DiscImage& disc, int set);

// The oracle check of `rr::sim::RoadArena`: rebuilds `ROAD<set>.MAP` and every road object a
// captured RAM image holds resident, from the disc, at that image's own guest addresses, and
// compares all of it with the image byte for byte. `report` says what was compared and how many
// bytes differ; the answer is true only at 0.
bool CheckRoadArena(const DiscImage& disc, int set, const std::string& ramPath, std::string& report);

// The oracle check of the cell placement the ground query walks. True only at 0.
// `mutate` is its negative control: the chunks are placed WITHOUT the region relocation, so every
// resident cell must differ.
bool CheckCellArena(const DiscImage& disc, int set, const std::string& ramPath, std::string& report,
                    bool mutate = false);

// The oracle check of the ROUTE ARENA the session builds: the race graph
// `STREAM<n>.GRF` at *(gp+472) with its two offsets relocated, and the route block the race-graph
// parser RASHCDI 0x8006A0C8 builds out of `ROADGRF<n>.TXT` - the 40-byte header at 0x800D6170, the
// START and FINISH records, the RACEINTS 120-byte route records and the hidden start record at index
// RACEINTS. The image names its own race (the `[RMAGIC]` at 0x800D6174 is looked up in both sets'
// text files); everything is rebuilt at the image's own addresses and compared byte for byte. NOT
// compared, and counted in the report: each record's `+0x76` (the bound-slot mask RouteBind keeps at
// run time) and the header's `+0x1C` (a pointer into the transient text buffer). `mutate` is the
// negative control: the next-node array is written at its TEXT slot instead of packed, which the
// parser does not do - every image must then differ. Refuses (false) on an image that holds no race.
bool CheckRouteArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate = false);

// The oracle check of the RIDER ANIMATION ARENA: for every bank slot the descriptor 0x800CE170
// names, the `ANIMTBL*.PSX` file of that clip count is placed at the slot's own file address with
// the loader's fix-up (every DMD3 block's +0x14 made a pointer to its payload, RASHCDI 0x8005BAF8),
// its clip table rebuilt, and compared byte for byte with the image, together with the slot records,
// the descriptor's constant fields and the bank table 0x800CE190; plus the object wiring every
// session depends on (object i's owner = rider i, its program = programs + 60 i, rider i's +0x21C =
// object i). `mutate`: the +0x14 fix-up is left out - every image must differ. Refuses on an image
// with no descriptor.
bool CheckAnimArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate = false);

// The oracle check of the CAMERA ARENA: `CAMERA.CA`'s first 224 bytes at 0x800CD7B8
// (the file `rr::CameraFileName` names for the image's player count and player 1's bike kind +0xB4;
// RASHCDI 0x80069618 reads exactly 224 bytes) and every field of view record 0 at 0x800CD898 that
// `CameraInit` SLUS 0x8002F308 and its caller RASHCDI 0x80067708 write to a constant and no race-frame
// code rewrites (+0x2C, +0xAC, +0xB4, +0x130, +0x134, +0x138, +0x140, +0x238 = player 1's bike,
// +0x45C) - built by the SAME `InitViewRecord` the session runs, on a scratch image, from the image's
// own player bike, and compared byte for byte. The rest of the 1132-byte record is the camera's state
// ViewUpdate rewrites every frame, and is counted as not compared. `mutate`: CAMERA.CA is read from
// file offset 4 instead of 0 - every image must differ. Refuses an image that holds no race.
bool CheckCameraArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate = false);

// The oracle check of the WINDOW-TEST DATA the ported population passes read: the
// resident road-piece list 0x800D4B10 (six 16-byte slots {id, 0, BTT_(id)+0x0C, 0}, a free slot
// {-1, 0, 0, 0}) with its last index 0x8005B31C, written by the session's own `WritePieceList` from
// the image's slot ids, plus the fixed words gp+204 (the collision-volume radius 80.0) and 0x800D8068
// (the effect records' walk pointer, 0x800D39B0) - byte for byte. It also checks OUR residency rule
// (a road object is listed while one of its residency windows holds the player's road coordinate):
// every object the rule names must be resident in the image. `mutate`: slot +0x08 gets the BTT_
// record's address instead of the object's - every image must differ. Refuses a non-race image.
bool CheckPopArena(const DiscImage& disc, const std::string& ramPath, std::string& report, bool mutate = false);

// The tables the ported code indexes, read out of the player's own images at run time. Nothing is
// baked into this repository.
class GameTables {
public:
    void Load(const DiscImage& disc);
    std::vector<uint8_t> ExeWindow(uint32_t address, size_t bytes) const;
    std::vector<uint8_t> OverlayWindow(uint32_t address, size_t bytes) const;
    const std::vector<uint8_t>& Exe() const { return exe_; }
    const std::vector<uint8_t>& Overlay() const { return overlay_; }

    std::vector<int16_t> sincos;      // SLUS 0x8005624C, 4096 x {sin, cos}
    std::vector<int32_t> atan;        // SLUS 0x8005285C, RatAtan2's table
    std::vector<uint16_t> atanU16;    // SLUS 0x800527E0, the engine's own 61-entry table
    std::vector<int16_t> sqrtTable;   // SLUS 0x800560CC
    std::vector<int16_t> sqrtWindow;  // the same table with 2 KiB of the image below it (entry 0 at +0x400)
    std::vector<uint16_t> rsqrtTable; // *(gp+2260): built by the ported SLUS 0x8002E080 loop
    std::vector<uint8_t> crashTable;  // SLUS 0x800537D8, 6 bytes per entity class
    std::vector<uint8_t> altKind;     // SLUS 0x800541D4, 8-byte records
    std::vector<int32_t> planTable;   // SLUS 0x80052FAC, the AI planner's period table
    std::vector<int32_t> tabSpeedClass, tabThinkA, tabThinkB, tabCopFlat, tabAltA, tabAltB, tabSpeedCap,
        tabCopMul;
    std::vector<int16_t> tabCopBase, tabCopStep;
    std::vector<int8_t> profile;      // SLUS 0x800531AC, the chosen 60-byte difficulty profile
    std::vector<uint32_t> armTable;   // RASHCDG 0x8005B9B8, the 19 command arms
    uint32_t armNone = 0;             // RASHCDG 0x800BA79C, the do-nothing arm
    uint32_t armRace = 0;             // RASHCDG 0x800BA6E0, the command-4 arm

private:
    std::vector<uint8_t> exe_, overlay_;
};

// VR physical combat (fight_physical.h, tools\rrgame\vr_melee.h) - OURS: a tracked fist or a held
// weapon that touched a rival rider's posed body this frame. The pad handler's combat tail applies it on THAT rider
// through the PORTED fight code (fight_session.cpp FightPhysicalBlow); only the VR host ever sets it.
struct PhysicalBlowRequest {
    bool pending = false;
    uint32_t victim = 0;     // the touched rider's bike, a pool-0 entity address
    bool right = true;       // the hand: right = combat action 1 (R1), left = action 2 (L1)
    bool weapon = false;     // the held weapon (or the hand holding it) touched: the weapon's own blow
    float strength = 1.0f;   // the original's per-blow damage times this (1 = the original's)
    int part = -1;           // the rider part touched (the log)
    float speed = 0.0f;      // the contact speed relative to the target, m/s (the log)
    bool snatch = false;     // a free hand closed on the weapon `victim` swings - WeaponSteal's path
                             // (fight_physical.h PhysicalSnatchRun), no blow
};

// The player's controls. The original keeps no input
// field of its own: `SLUS 0x8001D338` merges these straight into the bike's flag word `+0x230`,
// which is why they are held state here and not events.
struct PadState {
    bool throttle = false; // Cross:  sets 0x1|0x2, releasing clears 0x2
    bool brake = false;    // Square: sets 0x20|0x40, releasing clears 0x40
    bool left = false;     // sets 0x80|0x100
    bool right = false;    // sets 0x80|0x200
    // The camera controls the pad handler applies to view record 0 (SLUS 0x8001D700..0x8001D7D8):
    bool cameraNext = false; // control 0 (pad 0xFFFE): pressed this frame -> the next chase camera
    bool lookBack = false;   // control 2 (pad 0xDFFF): held -> the look-behind column of CAMERA.CA
    // The combat buttons, as PAD BITS (the repacked word the post-processor masks): the
    // combat input map 0x800CCB78 turns them into actions 1..8 through the pad record's control table
    // R1 = action 1, L1 = action 2, R2 = action 3, with Up / Down co-held the others. Held state; the pad record's stamps make the edges (fight_session.cpp).
    bool r1 = false, l1 = false, r2 = false; // repacked bits 3, 2, 1
    bool padUp = false, padDown = false;     // repacked bits 12, 14 (the d-pad; the arrows are Cross / Square)
    bool taunt = false;                      // L2: repacked bit 0, slot 9 = control 8 (speech_session.cpp)
    bool start = false;                      // Start: repacked bit 11, slot 12 = control 1, the pause (pause_product.h)
    bool triangle = false;                   // Triangle: repacked bit 4, slot 6, the pause menu's back
    PadDevice device;                        // the device id / analogue sticks (pad_product.h)
    PhysicalBlowRequest blow;                // VR physical combat: a contact to apply this frame
};

// A raw view of a record inside the arena. The arena never moves, so a view stays valid for the
// session's life; `data()` and `[]` are what the ported host-pointer functions and the log read.
struct ArenaBytes {
    uint8_t* p = nullptr;
    size_t n = 0;
    uint8_t* data() const { return p; }
    size_t size() const { return n; }
    uint8_t& operator[](size_t i) const { return p[i]; }
};

// One motorcycle: its records IN THE ARENA.
struct RaceBike {
    ArenaBytes entity;   // 1096 bytes, pool 0 slot i (rr-race's 0x801B65D4 + 1096 i)
    ArenaBytes owner;    // the pool-1 rider record at entity[+0x354] (0x801BB2EC + 628 i), 628 bytes
    ArenaBytes riderDef; // the runtime rider record at entity[+0x43C], 72 bytes
    uint32_t entityAddress = 0, ownerAddress = 0, riderDefAddress = 0;
    uint8_t* stats = nullptr;  // the 448-byte stat block at entity[+0x22C], in the arena
    uint32_t statsAddress = 0;
    int32_t statsBlock = -1;   // idx: 3 for the player, riderDef[+1] & 0xF for an opponent
    bool isPlayer = false;
    int32_t gridSlot = 0;      // the grid entry's slot (STARTDF<set>.BIN, grid_loader.h): SpawnBike's AI-index input
    bool isCop = false;        // a police entry of the grid (slot >= 17): spawned dormant, as the loader does
    int32_t gridAlong = 0;     // the entry's along offset (16.16, the file's), for a police bike's placement

    // --- OURS, display only: how far along the route `world.cpp` assembled the bike's box centre
    // lies (the nearest slice of that path), for the renderer's residency windows and the log. The
    // simulation never reads it.
    double routeDistance = 0.0;
    size_t pathHint = 0;
    int32_t place = 0;          // from the ported ComputePlace, for the HUD
};

// What the frame actually called, counted. `rrgame` prints this, so "the loop runs the ported
// spine" is a measurement and not a claim.
struct FrameLog {
    uint32_t frame = 0;
    int32_t ticks = 0;
    int32_t dt = 0;          // 16.16 seconds, 218 * ticks
    int32_t raceClock = 0;   // game_state+0x10, in ticks
    int32_t countdown = 0;   // *(0x8005B230), 16.16 seconds
    bool countdownRunning = false;
    bool raceDirectorRan = false;
    bool progressPassComplete = false;
    bool aiDrivePassComplete = false;
    bool aiRunCommandsComplete = false;
    rr::game::FightCounts fight; // fight_session.h: combat
    rr::game::AiPassCounts ai;   // ai_race.h: the AI passes on the arena (command pass, planner, brain)
    bool aiPlanRan = false, aiPlanOk = false, aiBrainRan = false, aiBrainOk = false;
    size_t engineBikes = 0;
    size_t steerBikes = 0;
    size_t aiSteered = 0;    // opponents with a non-zero steering angle +0x27C after the frame
    // The PORTED per-bike step RASHCDG 0x80075EE0, run whole (BikeStep).
    bool stepRan = false;
    bool stepDeclined = false;
    uint32_t stepFault = 0;       // the first address the view refused, when it declined
    size_t stepMigrations = 0;    // the PORTED list migration 0x80071BCC, calls
    size_t integratorRan = 0;     // region G: the PORTED integrator 0x8007F0BC, calls
    size_t integratorListed = 0;  // ... of which appended to the active list
    size_t groundFrames = 0;      // region H: the PORTED ground frame 0x8007504C, calls
    size_t groundMoved = 0;       // ... that moved the contact point +0x1F8
    size_t contactRan = 0;        // region H: the PORTED contact response 0x8007FA4C, calls
    size_t contactRebinds = 0;    // ... that ran the PORTED re-bind SLUS 0x800374D4
    size_t riderPoses = 0;        // region I: the PORTED rider pose 0x800807F0, calls
    size_t endRaces = 0;          // region J: the PORTED EndRace 0x80092C7C, calls
    size_t activationSeams = 0;   // region J: RASHCDG 0x80093E6C, asked for (NOT ported)
    size_t downedSeams = 0;       // region J: RASHCDG 0x800950E8, asked for (NOT ported)
    size_t stanceEvents = 0;      // the PORTED stance event 0x800C4550, calls (all callers)
    size_t stanceChanged = 0;     // ... that changed a rider's stance +0x220
    size_t riderLayerCalls = 0;   // the PORTED RiderKnockOff / RiderLaunch, calls
    size_t animObjects = 0;       // objects AnimationPass 0x8005E1D8 found playing
    bool animDeclined = false;
    size_t impactBikes = 0;       // the PORTED ImpactStatePass 0x80078DB4: bikes on its two lists
    bool impactDeclined = false;
    size_t headingBikes = 0;      // the PORTED heading writer 0x8007AC04: bikes on the riding list
    bool headingDeclined = false;
    // The PORTED road query / layer, counted inside region H and AiDrive.
    size_t aiLookAheads = 0;
    size_t offRoad = 0;           // bikes with +0x184 bit 0 after the frame
    // The PORTED ground query RASHCDG 0x800A7BF8, reached through the ground frame.
    size_t groundQueries = 0, groundHits = 0, groundExhausted = 0, groundMisses = 0, groundDeclined = 0;
    size_t groundFineHits = 0;       // ... hits whose answer has bit 11: the FINE mesh
    size_t playerGroundHits = 0, playerGroundFineHits = 0; // the same for the player's bike
    size_t cellsResident = 0, cellLoads = 0, cellUnloads = 0;
    size_t finishTestsRequested = 0; // calls into the PORTED RASHCDG 0x800B9958 this frame
    size_t finishTestsDeclined = 0;
    size_t ridersFinished = 0;       // riders whose riderDef[+0x28] is stamped
    // The player's fields after the frame, read back out of the arena.
    int32_t playerSpeed = 0;         // +0x1E0 (engine)
    int32_t playerSpeed240 = 0;      // +0x240 (region E)
    int32_t playerDrive = 0;         // +0x250
    int32_t playerRevs = 0;          // +0x25C
    int32_t playerGear = 0;          // +0x351
    int32_t playerSteer = 0;         // +0x27C
    int32_t playerLean = 0;          // +0x34A
    int32_t playerNetAccel = 0;      // +0x1E4
    int32_t playerThrottleAmt = 0;   // +0x24C
    int32_t playerLeanF = 0;         // +0x2A4
    int32_t playerLatForce = 0;      // +0x2E8
    int16_t playerHeading[3] = {0, 0, 0}; // +0x1C2
    int16_t playerFacing[3] = {0, 0, 0};  // +0x210
    int32_t playerBox[3] = {0, 0, 0};     // +0xB8
    uint32_t playerSlice = 0;        // +0x154
    int32_t playerSliceIndex = 0;
    uint32_t playerRoadWord = 0;     // +0x168
    int32_t playerDirection = 0;     // +0x16C
    int32_t playerAlong = 0;         // +0x170
    int32_t playerLateralRoad = 0;   // +0x158
    uint32_t playerFlags184 = 0;     // +0x184
    int32_t playerProgress = 0;      // +0x144 (the PORTED ProgressPass 0x8003B520)
    uint32_t playerRoute = 0;        // +0x1AC, the route record the PORTED RouteBind names
    int32_t playerGroundHeight = 0;  // +0x104
    uint32_t playerGroundAnswer = 0; // +0x218
    int16_t playerGroundNormal[3] = {0, 0, 0}; // +0x112
    uint8_t playerSurface = 0;       // +0x216
    int32_t playerViewDistance = 0;  // +0x2C
    uint16_t playerStance = 0;       // rider +0x220
    int32_t playerMount = 0;         // rider +0x25C
    uint32_t playerAnimFrame = 0;    // the rider's animation object +0x10
    uint32_t playerAnimFlags = 0;    // ... +0x24
    uint32_t playerFlags[3] = {0, 0, 0}; // +0x230 / +0x234 / +0x238
    uint32_t playerRiderFlags = 0;   // rider +0x228
    uint32_t playerList = 0;         // the list head the player's node +0x440 leads back to
    // Launches: bikes whose flagsC gained bit 10 (thrown, `BikeObstacleTest`'s 0xC00) in the contact
    // response this frame, with what the ground frame had just left (+0x104 depth, +0x216 surface,
    // +0x1E0 speed) - the inputs of 0x80075628's two "wall" tests.
    size_t launches = 0;
    std::string launchNote;
    PadState pad;

    // ---- the PORTED camera ViewUpdate RASHCDG 0x800881B4 (RaceStep's camera slot), view record 0.
    bool cameraRan = false;
    bool cameraDeclined = false;     // the port refused; the view bytes were restored
    uint32_t cameraFailAt = 0;
    size_t collideSkipped = 0;       // RASHCDG 0x800A421C asked for (+0x224 & 0x100) and refused
    size_t collidePushes = 0;        // RASHCDG 0x800A421C answered non-zero (eye pushed; CameraOrient re-run)
    PadControlsResult padControls;   // the PORTED pad reader region SLUS 0x8001CFB0 (pad_product.h)
    int32_t viewEye[3] = {0, 0, 0};  // +0xB8
    int32_t viewLook[3] = {0, 0, 0}; // +0x22C
    int32_t viewAim[3] = {0, 0, 0};  // +0x23C
    uint32_t viewMode = 0, viewFlags = 0, viewDirector = 0; // +0x21C, +0x224, +0x304
    double eyeToBike = 0.0;          // |eye - the player's box centre +0xB8|, world units
    double eyeBehind = 0.0;          // along the bike's facing +0x210 (negative = behind the bike)
    double eyeAbove = 0.0;           // along -Y (the PlayStation's Y points down)
    double eyeToAim = 0.0;           // |eye - aim point +0x23C|
    // ---- the population passes of region J and the rider pass (population.h), run PORTED.
    size_t liveBikes = 0, dormantBikes = 0; // pool-0 bikes with +0x140 != 0 / == 0 after the frame
    size_t activations = 0;          // the PORTED Activate 0x80093ED4 calls (one per slot)
    size_t transitions = 0;          // ... that placed or retired a bike
    size_t transitionsRefused = 0;   // ... that reached an unported callee; the arena was restored
    size_t downedCalls = 0, downedRefused = 0; // the PORTED Downed 0x800951B8
    size_t dormantDrives = 0, dormantRefused = 0; // the PORTED DormantDrive 0x80095724 on the dormant list
    uint32_t lastRefusal = 0;        // the address of the unported callee last refused
    size_t piecesListed = 0;         // road objects in the resident piece list 0x800D4B10 (ours)
    // ---- the traffic: SpawnerPass 0x8008CD88 and TrafficPass 0x8009A298, PORTED
    bool spawnerRan = false, spawnerDeclined = false, trafficRan = false, trafficDeclined = false;
    int32_t carsLive = 0;            // *(0x800CF650), pool 3's live count after the frame
    int32_t copsOut = 0;             // *(0x800D86F0)
    // ---- the spine seams, PORTED
    size_t raceGo = 0;               // RASHCDG 0x80090270 ran
    size_t crashEmits = 0, crashEmitsDeclined = 0; // SLUS 0x80027540
    size_t stampResults = 0, resets = 0, viewEvents = 0; // 0x800BC7CC, SLUS 0x8002090C, 0x8008A998
    size_t bursts = 0, sprays = 0;   // SLUS 0x80027778 / 0x80027974 through RoadNote
    size_t effectRecordsBusy = 0;    // effect records 0x800D39B0 not free after the frame
    size_t cameraKeys = 0;           // the pad's camera controls (SLUS 0x8001D700..0x8001D7D8)
    // ---- the PORTED collision pass RASHCDG 0x800A4774 and the rider pass's PORTED thrown walk
    // [0x8007C23C, 0x8007C9A8)
    bool collisionRan = false, collisionDeclined = false;
    uint32_t collisionFault = 0;
    size_t collisionSeamCalls = 0;   // unported callees the pass asked for (v0 = 0), all names
    size_t collisionSounds = 0, releaseContacts = 0, rumbles = 0;
    size_t touchDowns = 0;           // bikes whose flagsC bit 10 (airborne) the pass cleared: TouchDown
    size_t thrownWalked = 0;         // bikes on the thrown list 0x8005B2D8 the walk ran on
    bool thrownDeclined = false;
    size_t airborne = 0;             // bikes with flagsC bit 10 after the frame
    std::string collisionNote;       // the unported callees asked for this frame: " name xN"
    std::string airNote;             // per-bike LAUNCH / LAND / RELISTED events (one line each)
    // ---- the PORTED race HUD, HudFrame RASHCDG 0x8005E848
    bool hudRan = false;
    bool hudRefused = false;
    size_t hudSounds = 0;            // the countdown's cues it started through the ported emitter
    size_t hudSkipped = 0;           // unported HUD callees it reached and skipped (named in Seams())
    uint32_t hudMask = 0;            // the wrong-way / turn mask SLUS 0x8003C590 gave
};

class RaceSession {
public:
    // `startDistance` puts the whole field that many world units along the assembled route instead
    // of on the line. A development control: the grid builder RASHCDI 0x80067B00 is not ported, so
    // where the field starts is ours either way.
    // `players` (mp_session.cpp): 0 = the front end's game_state+0x30
    // (1 without a front end), 2 = the two-player split screen (a development start: race type 0x10).
    RaceSession(const DiscImage& disc, const RaceWorld& world, int opponents,
                double startDistance = 0.0, int players = 0);
    ~RaceSession();

    // `pad2`: player 2's controls in a two-player race (ignored with one player).
    void Frame(const PadState& pad, int32_t ticks, const PadState* pad2 = nullptr);

    // ---- two players (mp_session.cpp): game_state+0x30, player p's bike (pool-0 slot p), the split
    // layout *(0x800D6C68) (VIEWS.VI: 0 top/bottom, 1 side by side, 2 staggered) and player p's
    // rectangle {x, y, w, h} of the 384 x 240 draw area (*(0x8005B474) + 8p); false with one player.
    int Players() const { return players_; }
    // DEVELOPMENT (rrgame --split-mode N, without the front end): the view mode a two-player race uses
    // instead of the session's chooser value; -1 = none.
    static int& DevSplitMode() {
        static int mode = -1;
        return mode;
    }
    int SplitMode() const;
    bool SplitRect(int p, int16_t rect[4]) const;
    // The HUD ordering-table words the frame's HUD packets hang from, in the order the GPU draws them
    // (player 2's slot 7 before player 1's slot 8).
    std::vector<uint32_t> HudListHeads() const;
    // The pause menu (pause_product.cpp): its slots 6..2 of the frame's 2D ordering table *(0x8005B5AC),
    // in the GPU's order - drawn after the HUD's.
    std::vector<uint32_t> PauseListHeads() const;

    const std::vector<RaceBike>& Bikes() const { return bikes_; }
    const RaceBike& Player() const { return bikes_[0]; }
    double RouteDistance(size_t index) const { return bikes_[index].routeDistance; }
    double RouteLength() const;
    const FrameLog& Log() const { return log_; }
    size_t ArenaObjects() const { return arenaObjects_; }
    const std::vector<std::string>& Seams() const { return seams_; }
    // Run totals of the PORTED collision pass and thrown walk: the unported callees by name and
    // count, landings, launches.
    std::string CollisionTotals() const;
    bool CountdownOver() const { return countdownOver_; }
    bool CanFinish() const { return true; }
    bool RaceOver() const { return gameState_[0] == 6 || gameState_[0] == 2; }
    uint8_t GameStateByte() const { return gameState_[0]; }
    // The route arena this session built: route records, the finish record.
    int32_t RouteRecordCount() const { return routeCount_; }
    uint32_t FinishRecord() const { return finishRecord_; }
    // Guest reads of the arena, for the log.
    uint32_t ArenaWord(uint32_t address) const;
    uint16_t ArenaHalf(uint32_t address) const;

    // The camera the host draws from, out of view record 0 at 0x800CD898, which the PORTED
    // ViewUpdate RASHCDG 0x800881B4 fills: the eye +0xB8, the look point +0x22C and the up row of the
    // render frame +0x1B6 (rows lateral / up / forward, 4096 = 1.0), in world units. The up row is
    // returned with its sign chosen so that its Y is <= 0 (the PlayStation's Y points down).
    void ViewCamera(float eye[3], float look[3], float up[3], int p = 0) const;
    // +0x140 of pool-0 bike `i`: 0 = dormant (only a road coordinate), else live.
    bool BikeLive(size_t index) const;

    // Where the renderer should put bike `i`: its box centre +0xB8 and the rows of its ground frame
    // (+0x210 facing, +0x204 lateral, -(+0x20A) the road normal), all the original's.
    void BikePlacement(size_t index, float position[3], float tangent[3], float lateral[3],
                       float normal[3]) const;

    void NoteSeam(const std::string& what);

    // Combat (fight_session.cpp): the op-16 arm of the command pass - FightUpdate
    // RASHCDG 0x800C035C on bike `e` at `target` (a pool-0 slot, 224 = none) - and the stance layer's
    // two combat children 0x800BFD24 / 0x800BFC5C, all PORTED, run on the arena with `seams`.
    bool FightCommand(uint32_t e, uint32_t target, rr::sim::StanceSeams& seams);
    // Op 9's strike (fight_session.cpp): Strike RASHCDG 0x800C1370 on bike `e` at bike `t`.
    bool StrikeCommand(uint32_t e, uint32_t t, rr::sim::StanceSeams& seams);
    // The game modes (race_modes.cpp; sim\modes.h, rules.md 9): the PORTED player-cop and Jailbreak functions
    // on the arena (Arrest 0x80096F30, ArrestFsm 0x80096818, JailRelease 0x800A0708, MilestoneAdvance
    // 0x800C8D4C, MilestoneFirst 0x800C92F8) with the frame's rider seams; `sp` 0 = the seam stack. ModePlace
    // is ComputePlace SLUS 0x800138E8 on the arena's live arrest word; ModeViewReset AiDrive's camera tail.
    bool ModeArrest(uint32_t cop, uint32_t t, uint32_t how, uint32_t sp);
    bool ModeArrestFsm(uint32_t e, int32_t dt, uint32_t sp, uint32_t& v0);
    bool ModeJailRelease(uint32_t e, uint32_t sp);
    int32_t ModeMilestone();
    void ModeMilestoneFirst();
    bool ModeViewReset(uint32_t e);
    bool ModePlace(uint32_t e, int32_t mode, int32_t& v0);
    std::string ModeTotals() const;
    // The Jailbreak mode and the two-seat bike (jail_session.cpp; sim\jail.h, rules.md 16): the PORTED escape
    // scene 0x800C9420 on the arena (`sp` 0 = the seam stack), the passengers SpawnPassenger 0x800670FC built
    // (the pool-0 bike and pool-1 rider of each player's sidecar), and a PORTED population function by address
    // (Placement 0x8009432C, CarSpawn 0x8009AD48) with the session's population callees (race_session.cpp).
    bool ModeEscapeScene(int32_t block, uint32_t sp);
    struct Passenger {
        size_t host = 0;              // the player (pool-0 slot) whose sidecar it is
        uint32_t bike = 0, rider = 0; // its pool-0 bike and pool-1 rider
        bool spawned = false;
    };
    const std::vector<Passenger>& Passengers() const { return passengers_; }
    bool PopCall(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t sp, uint32_t& v0);
    // The jail stop (JailbreakFinish 0x800C9E74, the AI drive's op 2 for player 1 in phases 1 / 2) and the
    // boarding (JailBoard 0x800CA05C: RiderRecover's late call), PORTED (sim\jail.h), with the frame's seams.
    bool ModeJailFinish(uint32_t sp);
    bool ModeJailBoard(uint32_t late, uint32_t sp);
    uint32_t FightCombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, rr::sim::StanceSeams& seams);
    uint32_t FightCombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, rr::sim::StanceSeams& seams);
    // The rider off the bike and back on it (recover_race.h; defined in recover_race.cpp):
    // one function of that domain by guest address - its own ports or a callee - on the arena, with
    // `seams` as the stance layer's and `sp` where the original makes the call; and the rider pass's rider
    // loop [0x8007DCEC, 0x8007DDD4) with the list migration `migrate` (PORTED 0x80071BCC) per bike.
    bool RecoverCall(uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp, rr::sim::RiderSeams& seams,
                     uint32_t* v0 = nullptr);
    bool RecoverRiderLoop(int32_t dt, uint32_t sp, rr::sim::RiderSeams& seams,
                          const std::function<bool(uint32_t e)>& migrate);
    std::string RecoverTotals() const;
    rr::sim::BikeTables SessionTables() const; // the five engine tables every ported caller is handed
    // DEVELOPMENT (rrgame --opponent-health): every opponent's health riderDef+0x0F AND its regeneration
    // ceiling +0x0E := hp - a data edit, not a placement. The ceiling too: AiPlan's regeneration
    // (RASHCDG 0x800B8284) lifts +0x0F back to +0x0E by 2 a plan tick, so +0x0F alone would be healed back
    // before the first fight.
    void DevOpponentHealth(uint8_t hp);
    void DevPlayerWeapon(int weapon, int swings); // DEVELOPMENT (rrgame --weapon), weapon_session.cpp
    void DevOpponentWeapons(int weapon, int swings); // DEVELOPMENT (rrgame --opponent-weapons), weapon_session.cpp

    SoundRuntime& Sounds() { return sounds_; }
    const SoundRuntime& Sounds() const { return sounds_; }
    // The riders' voices (speech_session.cpp): AUDTAUNT.STR to the sound runtime; the PORTED
    // RiderSpeech SLUS 0x8001A760(h, crash) at `sp` (0 = ours) with the caller's stance seams for the
    // provocation's push; the pad reader's L2 arm for player 1 (after FightPadPass). Taunts = L2 presses.
    void StartSpeech(const DiscImage& disc);
    bool RiderSpeech(uint32_t h, int32_t crash, uint32_t sp, rr::sim::StanceSeams* seams);
    void TauntPad(rr::sim::StanceSeams& seams, int p = 0);
    size_t Taunts() const { return taunts_; }
    size_t Grudges() const { return grudges_; } // AI riders a taunt turned on the speaker (grudge byte 15)

    // The race HUD: the arena the HUD loader transcription built, the HUD page of
    // VRAM, and the ordering-table word HudFrame links the frame's HUD packets into.
    bool HudReady() const { return hudReady_; }
    const HudVram& HudPage() const { return hudVram_; }
    uint32_t HudListHead() const { return ArenaWord(hudAt_.ot); }
    const uint8_t* ArenaRam() const { return arena_.Ram(); }
    int HazardSet() const { return hazardSet_; } // the HAZARD<n> the world arena loaded (world_pop_product.h)
    // The arena itself, for the effect pass the host runs after the frame (fx_runtime.h).
    uint8_t* MutableArenaRam() { return arena_.Ram(); }
    // rrgame --parity (parity_session.cpp): the arena becomes a captured console state's 2 MiB RAM, the bike
    // list is rebound to its pool 0, so the product's draw renders the state the original's packets came from.
    std::string AdoptCapture(const std::vector<uint8_t>& ram);

private:
    void LoadImages();
    void SetupGrid();
    void ModeSetUp(); // the loader's per-race-type setup after the cursors are seated (race_modes.cpp)
    void JailPlan();                        // the passengers' pool slots, before the per-rider set-up (jail_session.cpp)
    void JailSpawn(const DiscImage& disc);  // SpawnPassenger 0x800670FC and STARTJBA.BIN, after the cursors
    void JailReserve(const DiscImage& disc); // the arena room for the passenger bank (bit-3 races)
    void JailRigParts();                    // a sidecar bike's part array sized for its rig, before the binding
    uint32_t jailBankAt_ = 0;               // where BuildAnimArena puts stance bank 6 (0 = with the others)
    uint32_t posePairsAt_ = 0;              // the pose arena's pairs 1.. in the object area (0 = rr-race's)
    // the ported loader: the ported grid's ModelBind binding stands (no BindModels override): the ported grid ran,
    // RRJB_LOADER2 is on and RRJB_LOADER_BIND (the grid's ModelBind control) is not set
    bool OriginalBind() const;
    std::vector<Passenger> passengers_;
    uint32_t jailCounter_ = 0;              // OURS: root counter 2 for the escape scene's GetRCnt
    size_t escapeScenes_ = 0;
    size_t jailStops_ = 0, jailBoards_ = 0;
    rr::sim::StanceSeams* modeSeams_ = nullptr; // the frame's rider seams, for the mode functions
    struct ModeCounts {
        size_t arrests = 0, fsm = 0, releases = 0, milestones = 0;
    } modeCounts_;
    void ApplyInput(const PadState& pad);
    // Combat (fight_session.cpp): FIGHT.BIN into the arena, and the pad handler's combat tail for player
    // `p` (its pad record 0x800D7128 + 192p, its bike pool-0 slot p).
    void LoadFightTable(const DiscImage& disc);
    void FightPadPass(const PadState& pad, rr::sim::StanceSeams& seams, int p = 0);
    void FightPadStamp(const PadState& pad, int p = 0);   // its slot stamp alone (while paused)
    void FightPadDecode(rr::sim::GuestRam& g, uint32_t gs, uint32_t rec, rr::sim::StanceSeams& seams, int p);
    void FightPhysicalBlow(const PhysicalBlowRequest& req, rr::sim::StanceSeams& seams); // VR physical combat
    bool padStamped_[2] = {false, false};
    // ---- the pause (pause_product.cpp)
    uint32_t PausePoll();       // the pad poll's pause test, PORTED region 0x8001CBEC; returns its s0
    void PauseStall();          // GameFrame's stall switch, PORTED region 0x80011C70
    void PauseMenuFrame();      // GameFrame's `state 3 / 4: PauseMenu 0x8002D2F4`, PORTED
    friend class ProductPauseCallees;
    // ---- two players (mp_session.cpp)
    int players_ = 1;
    void MpSetupGame(const DiscImage& disc);   // VIEWS.VI and the view mode (SLUS 0x8001B5EC / 0x8001B67C)
    void MpSetupCamera(bool skipIntro);         // view record 1 (RASHCDI 0x80067710..0x80067764)
    void MpSetupPads();                         // player 2's pad record's control table (SLUS 0x8001C4A8)
    void MpPlayerPad(const PadState& pad2, rr::sim::StanceSeams& seams); // player 2's pad handler part
    std::string player2Ph_;                     // player 2's bike .PH (game_state+0x4C)
    uint32_t padClock_ = 0; // game_state+0x0C, the pad record's clock (ours: its writer is not ported)
    size_t taunts_ = 0;     // L2 presses the pad reader's arm turned into RiderSpeech (speech_session.cpp)
    size_t grudges_ = 0;    // RiderSpeech calls that wrote a grudge byte (speech_session.cpp)
    void BuildArena(const DiscImage& disc);
    void BuildRouteArena(const DiscImage& disc);
    void BuildRouteArenaPorted(const DiscImage& disc); // GrfLoad + RoadLoad PORTED (route_product.h)
    bool routePorted_ = false;                          // the ported route loader on (RRJB_ROUTE=ours: the transcription)
    void BuildAnimArena(const DiscImage& disc);
    void LoadStatBlocks(const DiscImage& disc);
    void LoadEnvTable(const DiscImage& disc);
    void SeatCursors();
    void StartStances();
    // The ported grid (grid_session.cpp): BuildGrid RASHCDI 0x80067B00 / SpawnBike 0x80065A94 PORTED on the
    // arena (grid_build.h) and BuildRace's 0x80067784 after it; SLUS 0x800119C0's start stances. Off with
    // RRJB_GRID=ours (and for a --start field): the session's own layout, SeatCursors and StartStances.
    bool gridPorted_ = false;
    bool BuildGridPorted(const DiscImage& disc);
    void NoteGridLayout(const char* who); // one seam line per bike: road coordinate, lateral, box centre
    void BuildCellCatalog(const DiscImage& disc);
    void CellStreamPass(rr::sim::GuestRam& g);
    // OURS: which road objects the resident piece list 0x800D4B10 holds (the road streamer SLUS
    // 0x80031784 is not ported): those whose residency window holds the player's road coordinate.
    void RoadStreamPass(rr::sim::GuestRam& g);
    // CAMERA.CA at 0x800CD7B8 and view record 0, initialised as CameraInit SLUS 0x8002F308 does.
    void BuildCameraArena(const DiscImage& disc);
    // The effect records 0x800D39B0, the walk pointer, the per-bike chain heads, gp+204, the traffic
    // switch and the piece list, as the loader leaves them.
    void BuildPopulationArena();
    // The collision pass RASHCDG 0x800A4774 (RaceTick's fourth child, and the prime pass with dt = 0 at
    // the end of the set-up): PORTED (rr::sim::CollisionPass), its unported callees named seams.
    void CollisionPass(int32_t dt);
    void SetupWorldWalk(); // SLUS 0x800119C0: VolumeLists 0x8008D89C and the cell walker once at set-up (solid_product.h)
    // The per-bike airborne record behind FrameLog::airNote (measurement only, nothing reads it back).
    void TrackAirborne(rr::sim::GuestRam& g);
    // The camera controls of the pad handler (SLUS 0x8001D700..0x8001D7D8), transcribed.
    void CameraControls(const PadState& pad, int p = 0);
    int32_t TargetSpeedAt(uint32_t e, int32_t dt);
    rr::sim::AiSpeedEnv SpeedEnv();
    void SnapshotArena();
    void RestoreArena(rr::sim::GuestRam& g);
    // RaceTick's second child: the PORTED SpawnerPass 0x8008CD88 (police and traffic schedulers).
    void SpawnerTick(int32_t dt);
    // The world pass's child: the whole per-bike step (BikeStep), with the view distance and the
    // cell residency before it.
    void WorldPass(int32_t dt);
    // The rider/engine pass's ported children.
    void RiderPass(int32_t dt);
    // GameFrame step 6.
    void AnimationPass(bool view = true); // view: ViewPass too (GameFrame runs it while racing only)
    size_t LinkBikeLists(rr::sim::GuestRam& g);
    void UpdateDisplayDistance(size_t i);
    uint32_t ArenaEntity(size_t i) const;
    uint8_t* At(uint32_t address) { return arena_.Ram() + (address & (rr::sim::GuestRam::kRamSize - 1u)); }
    const uint8_t* At(uint32_t address) const {
        return arena_.Ram() + (address & (rr::sim::GuestRam::kRamSize - 1u));
    }
    int32_t* Word(uint32_t address) { return reinterpret_cast<int32_t*>(At(address)); }

    const RaceWorld& world_;
    rr::sim::RoadArena arena_;
    std::vector<uint8_t> scratchpad_ = std::vector<uint8_t>(1024, 0);
    size_t arenaObjects_ = 0;
    std::vector<uint32_t> arenaObjectList_;
    uint32_t arenaFreeFrom_ = 0;
    bool trafficReady_ = false; // traffic_arena.h built (the spawner and the traffic pass may run)
    int hazardSet_ = -1;        // world_pop_product.h HazardSetFor, picked when the world arena is built
    double startDistance_ = 0.0;
    GameTables tables_;
    std::vector<RaceBike> bikes_;
    ArenaBytes gameState_;

    // The globals the original keeps in its data and BSS, AT THEIR GUEST ADDRESSES in the arena, so
    // the ported code that reads them through the view and the ported code handed a host pointer
    // see one word.
    int32_t* countdown_ = nullptr;   // 0x8005B230
    int32_t* eventAcc_ = nullptr;    // 0x8005B30C
    int32_t* planCount_ = nullptr;   // 0x8005B2A8
    int32_t* frameFlag_ = nullptr;   // 0x8005B580
    int32_t* liveBikes_ = nullptr;   // 0x8005B1F8
    int32_t* bikeCap_ = nullptr;     // 0x8005B1FC
    int32_t* spreadDiv_ = nullptr;   // 0x8005B244
    int32_t* postLimit_ = nullptr;   // 0x8005B228
    int32_t* skipResults_ = nullptr; // 0x8005B220
    int32_t* clockStamp_ = nullptr;  // 0x800CCA84
    int32_t* spreadBand_ = nullptr;  // 0x800CCA88
    int32_t* timeLimit_ = nullptr;   // 0x8005ACC8
    int32_t* timeBase_ = nullptr;    // 0x8005ACD0
    int32_t* startDir_ = nullptr;    // 0x8005B2E8
    uint32_t* randSeed_ = nullptr;   // gp+2076 = 0x8005B4A8
    uint32_t raceFlags_ = 0;         // *(0x8005AD48)

    // The route arena (BuildRouteArena).
    int32_t routeCount_ = -1;
    uint32_t routeRecords_ = 0;   // *(0x800D6194)
    uint32_t finishRecord_ = 0;   // *(0x800D6188)

    // The rider animation arena (BuildAnimArena).
    uint32_t animObjects_ = 0, animPrograms_ = 0;

    // The resident scene cells (BuildCellCatalog / CellStreamPass).
    struct CatalogCell {
        uint32_t id = 0;
        uint8_t type = 0;
        std::vector<uint8_t> chunk;
        std::vector<uint8_t> type9;
        std::vector<ResidencyWindow> windows;
        int32_t slot = -1;       // player 1's slot 0..11
        int32_t slot2 = -1;      // player 2's slot 12..23 (two players)
        uint32_t at = 0, at9 = 0, body = 0;
    };
    std::vector<CatalogCell> cells_;
    std::vector<uint32_t> freeCellBuffers_;
    size_t cellBuffersTotal_ = 0;
    struct ArenaRoadObject {
        uint32_t id = 0, address = 0;
        std::vector<ResidencyWindow> windows;
    };
    std::vector<ArenaRoadObject> roadObjects_;
    int32_t pieceSlots_[6] = {-1, -1, -1, -1, -1, -1}; // index into roadObjects_, or -1
    int32_t pieceHigh_ = -1;                            // the list's last index, a high-water mark (ours)
    // ---- the stream (stream_session.cpp): the original's streamer PORTED
    // (src\game\sim\stream.h); off with RRJB_STREAM=ours (the rules above).
    bool streamPorted_ = false;
    const DiscImage* streamDisc_ = nullptr;             // the host's CD: the disc outlives the session
    uint32_t streamBuffers_ = 0;                        // the 32 x 16 KiB block of the resource table
    uint32_t streamFrames_ = 0;
    void StreamBuild(const DiscImage& disc);            // the loader's stream set-up (after the road map)
    void StreamStart(const DiscImage& disc);            // SLUS 0x80023020 at SetUpRace's point
    void StreamFrameStep();                             // SLUS 0x8002305C, GameFrame's first step
    bool StreamPopCall(uint32_t fn, const uint32_t* a, uint32_t sp, uint32_t& v0); // race_session.cpp
    std::vector<uint8_t> restore_;                      // the arena copy a refused transition restores
    std::vector<uint8_t> restoreSpad_;
    uint32_t camCounter_ = 0;                           // OURS: root counter 2 for the camera's GetRCnt
    uint32_t fxCounter_ = 0;                            // OURS: root counter 2 for the effect jitter
    RecoverCounts recoverCounts_;                       // recover_race.h: calls / refusals by address
    bool lookBackHeld_ = false;
    class EffectSpawner;
    std::unique_ptr<EffectSpawner> effects_;

    std::vector<uint8_t> surfaceTable_; // SLUS 0x800525C0, 52 u8 read off the disc
    SoundRuntime sounds_;
    uint32_t rcntState_ = 0; // OURS: root counter 2 for ImpactStatePass's GetRCnt

    // The collision pass's run totals and the per-bike airborne record (measurement only).
    struct AirTrack {
        bool air = false, landed = false, relisted = false;
        uint32_t launchFrame = 0, landFrame = 0;
        int32_t launchSpeed = 0, launchVy = 0, lastVy = 0;
        int64_t sumDvy = 0;
        uint32_t walkedFrames = 0;
    };
    std::vector<AirTrack> air_;
    std::map<uint32_t, size_t> collSeamTotals_;  // unported callee -> calls over the run
    std::map<uint32_t, size_t> collSeamFrames_;  // unported callee -> frames it was asked for
    size_t collFrames_ = 0, collDeclined_ = 0, collTouchDowns_ = 0, collLaunches_ = 0, collRelisted_ = 0;
    size_t collSounds_ = 0, collReleases_ = 0, collRumbles_ = 0, thrownWalks_ = 0, thrownDeclined_ = 0;
    size_t maxAirborne_ = 0;

    // The race HUD: built once by the loader transcription, run once a frame.
    void BuildHud(const DiscImage& disc);
    HudPlacement hudAt_;
    HudVram hudVram_;
    bool hudReady_ = false;

    FrameLog log_;
    std::vector<std::string> seams_;
    bool countdownOver_ = false;
    uint32_t frameNumber_ = 0;
};

} // namespace rr::game
