#pragma once
// DATA\CAMERA*.CA - the race camera's tuning records.
//
// The race loader (RASHCDI 0x80069618) opens `DATA\CAMERA<suffix>.ca` and reads ONLY ITS FIRST 224
// BYTES (`li a2,224` at 0x80069664) into guest 0x800CD7B8: four 56-byte records of fourteen s32
// 16.16 words, one per selectable chase camera (the view record's +0x21C / +0x220, 0..3). Every
// other byte of the file is never read by the game. The suffix is chosen at 0x800675B0: "2" with two
// players, then "S" when player 1's bike kind (+0xB4) is 6..8 or 15..17 - so CAMERA.CA, CAMERAS.CA,
// CAMERA2.CA, CAMERA2S.CA.
//
// The field names are what ViewUpdate (RASHCDG 0x800881B4) and CameraAim (0x80087420) do with each
// word; the second column of a pair is selected by view +0x224 bit 0 (the look-behind camera).
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace rr {

constexpr size_t kCameraRecordWords = 14;
constexpr size_t kCameraRecordBytes = 56;
constexpr size_t kCameraRecords = 4;
constexpr size_t kCameraBytesRead = kCameraRecords * kCameraRecordBytes; // 224

struct CameraRecord {
    int32_t yawRate = 0;          // +0x00  the aim-yaw chase rate (0x80087F90), reset value of +0x26C/+0x1C8
    int32_t eyeZ[2] = {0, 0};     // +0x04  eye along the aim frame's forward row (+0x284 target)
    int32_t eyeY[2] = {0, 0};     // +0x0C  eye along the up row (+0x280 target, minus the distance spring)
    int32_t lookZ[2] = {0, 0};    // +0x14  look point along forward (+0x2A8 target)
    int32_t lagK[2] = {0, 0};     // +0x1C / +0x24  speed follower stiffness: bike accelerating / not
    int32_t lagC[2] = {0, 0};     // +0x20 / +0x28  speed follower damping:   bike accelerating / not
    int32_t springC = 0;          // +0x2C  the distance spring's damping (0x80086B1C)
    int32_t springK = 0;          // +0x30  the distance spring's stiffness
    int32_t shake = 0;            // +0x34  off-road shake gain (per 60.0 of speed)
    std::array<int32_t, kCameraRecordWords> raw{};
};

struct CameraSet {
    std::array<CameraRecord, kCameraRecords> records{};
};

// Throws std::runtime_error when the file is shorter than the 224 bytes the game reads.
CameraSet ParseCameraCa(std::span<const uint8_t> file);

// "DATA\CAMERA" + suffix + ".CA" as the loader builds it (0x80067590..0x80067664).
std::string CameraFileName(int players, uint32_t player1BikeKind);

} // namespace rr
