#pragma once
// src\game\sim\camera_setup - the race loader's camera set-up, the level's light stores and the collision set-up,
// ported from our own disassembly of the player's images:
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8
//   RASHCDG.BIN  (the race overlay, resident at 0x8005B5E8 while racing)
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
//   CameraSetUp     RASHCDI 0x80067564, frame 176  DATA\CAMERA\<x>.ca by player count and player 1's bike kind,
//                                                  two sprintfs, CameraFileRead, CameraInit of view 0 (handle 0x9F)
//                                                  and - with player 2's bike *(0x8005B21C) - of view 1 (0x9E)
//   CameraFileRead  RASHCDI 0x80069618, frame 32   open / read 224 bytes to 0x800CD7B8 / close (or the print)
//   CameraInit      SLUS    0x8002F308, frame 40   a view record's set-up from its target (memset, two memcpys,
//                                                  RouteBind SLUS 0x8003AF9C at step 0)
//   LevelLight      RASHCDI 0x8006250C, frame 64   the level bundle's type-2 section: the light vector and colour
//   LevelShade      RASHCDI 0x80062430, frame 32   the level bundle's type-7 section: the shade table 0x800D4CA8
//                                                  and the shadow colour 0x80052348
//   CollisionSetUp  RASHCDG 0x800A41EC, a leaf     the chain sentinel node 128 {0x80, 0xFF}, the chain words
//
// MEMORY MODEL as loader.h: `sp` is the stack pointer at the function's entry; calls are made at sp - its frame.
// The CD, the prints and the sprintfs are seams (`LoaderCallees`): the bench's oracle (or planted stubs), the
// product's host reads. CameraInit's own leaves (memset / memcpy by words) and RouteBind run natively (ported,
// benched on their own rows); Scale 0x8002EE50 / FixMul 0x8001FC90 / memcpy likewise in the light stores.
#include <cstdint>

#include "game/sim/loader.h"
#include "game/sim/road_runtime.h"

namespace rr::sim {

constexpr uint32_t kCamSetUpFn = 0x80067564, kCamSetUpFrame = 176;
constexpr uint32_t kCamFileReadFn = 0x80069618, kCamFileReadFrame = 32;
constexpr uint32_t kCameraInitFn = 0x8002F308, kCamInitFrame = 40;
constexpr uint32_t kCamFileBuf = 0x800CD7B8, kCamFileRead = 224;
constexpr uint32_t kCamView0 = 0x800CD898, kCamViewStride = 1132;
constexpr uint32_t kCamNameBuf = 0x800D4C20;     // the "DATA\CAMERA\<x>" buffer (the first sprintf's)
constexpr uint32_t kCamIntroSwitch = 0x8005B220; // with the shot table's first word, CameraInit's director flag
constexpr uint32_t kCamPlayer2Bike = 0x8005B21C;
constexpr uint32_t kCdOpen = 0x8001458C, kCdRead = 0x80014780, kCdClose = 0x8001460C, kPrintf = 0x80044894;
constexpr uint32_t kLevelLightFn = 0x8006250C, kLevelLightFrame = 64;
constexpr uint32_t kLevelShadeFn = 0x80062430, kLevelShadeFrame = 32;
constexpr uint32_t kLightBlock = 0x8005233C;     // the light block the two stores fill
constexpr uint32_t kShadeTable = 0x800D4CA8;     // 1152 bytes
constexpr uint32_t kCollSetUpFn = 0x800A41EC;

// void CameraInit(Entity *target, View *v, u16 handle, s32 mode, s32 director, s32 /*unused*/)  [SLUS 0x8002F308]
// The sixth argument is never read. Returns false only when the view faulted.
bool CameraInit(GuestRam& g, uint32_t target, uint32_t v, uint32_t handle, uint32_t mode, uint32_t director,
                uint32_t sp, RoadRuntimeCallees& road);

// void CameraFileRead(char *name)  [RASHCDI 0x80069618]
bool CameraFileRead(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c);

// void CameraSetUp(void)  [RASHCDI 0x80067564]; its three children (sprintf 0x80043FD4, CameraFileRead, CameraInit)
// through `c`.
bool CameraSetUp(GuestRam& g, uint32_t sp, LoaderCallees& c);

// void LevelLight(u32 unused, u32 *section)  [RASHCDI 0x8006250C]
bool LevelLight(GuestRam& g, uint32_t section, uint32_t sp);

// void LevelShade(u32 unused, u8 *section)  [RASHCDI 0x80062430]
bool LevelShade(GuestRam& g, uint32_t section);

// void CollisionSetUp(void)  [RASHCDG 0x800A41EC]
void CollisionSetUp(GuestRam& g);

} // namespace rr::sim
