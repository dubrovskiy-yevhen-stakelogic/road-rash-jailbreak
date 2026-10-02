#pragma once
// The race set-up's camera, light and collision steps in the product: three transcriptions replaced by ports
// (src\game\sim\camera_setup.{h,cpp}, bench rows tools\rrverify\rows_loader2_cam.inc):
//   * the camera set-up: CameraSetUp RASHCDI 0x80067564 (row camera_set_up) with CameraFileRead RASHCDI 0x80069618
//     (row camera_file_read) and CameraInit SLUS 0x8002F308 (row camera_init) - in place of race_session.cpp's
//     InitViewRecord / MpSetupCamera; the CD read is the disc image's (open / read / close answered by the host);
//   * the level's light stores: LevelLight RASHCDI 0x8006250C (row level_light) and LevelShade RASHCDI 0x80062430
//     (row level_shade) on the level bundle's type-2 / type-7 sections - in place of shadow_product.cpp's
//     transcription;
//   * the collision set-up RASHCDG 0x800A41EC (row collision_set_up) - in place of race_session.cpp's four stores.
// RRJB_LOADER2_CAM=off is the negative control: the transcriptions stand.
#include <cstdint>
#include <string>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

bool Loader2CamPorted();

// CameraSetUp on the arena `g` at stack `sp`, RASHCDI laid over 0x8005B5E8 for the call. `noShots`: the race's level
// bundle has no shot table, so the intro director CameraInit would start cannot run - the view records are then
// put back on the chase camera (OURS, named in the returned seam line). `fileOffset` > 0 is a check's mutation (the
// camera file read from that offset). Returns the seam line; `ok` false when the port refused.
std::string PortedCameraSetUp(rr::sim::GuestRam& g, const DiscImage& disc, uint32_t sp, bool noShots, bool& ok,
                              uint32_t fileOffset = 0);

// LevelLight / LevelShade on the race's GAMEBIN1.DAT bundle sections 2 and 7, each payload staged on the guest stack
// below `sp` for the call (the loader has it in a heap block). Returns the seam line.
std::string PortedLevelLight(rr::sim::GuestRam& g, const DiscImage& disc, int raceId, uint32_t sp);

// CollisionSetUp RASHCDG 0x800A41EC. Returns the seam fragment (PORTED, the row, the count).
std::string PortedCollisionSetUp(rr::sim::GuestRam& g);

} // namespace rr::game
