#include "rrformats/camera_ca.h"

#include <stdexcept>

namespace rr {

CameraSet ParseCameraCa(std::span<const uint8_t> file) {
    if (file.size() < kCameraBytesRead) throw std::runtime_error("CAMERA.CA shorter than the 224 bytes the game reads");
    CameraSet set;
    for (size_t r = 0; r < kCameraRecords; ++r) {
        CameraRecord& rec = set.records[r];
        for (size_t w = 0; w < kCameraRecordWords; ++w) {
            const size_t o = r * kCameraRecordBytes + 4 * w;
            rec.raw[w] = static_cast<int32_t>(static_cast<uint32_t>(file[o]) | (static_cast<uint32_t>(file[o + 1]) << 8) |
                                              (static_cast<uint32_t>(file[o + 2]) << 16) |
                                              (static_cast<uint32_t>(file[o + 3]) << 24));
        }
        rec.yawRate = rec.raw[0];
        rec.eyeZ[0] = rec.raw[1];
        rec.eyeZ[1] = rec.raw[2];
        rec.eyeY[0] = rec.raw[3];
        rec.eyeY[1] = rec.raw[4];
        rec.lookZ[0] = rec.raw[5];
        rec.lookZ[1] = rec.raw[6];
        rec.lagK[0] = rec.raw[7];
        rec.lagC[0] = rec.raw[8];
        rec.lagK[1] = rec.raw[9];
        rec.lagC[1] = rec.raw[10];
        rec.springC = rec.raw[11];
        rec.springK = rec.raw[12];
        rec.shake = rec.raw[13];
    }
    return set;
}

std::string CameraFileName(int players, uint32_t player1BikeKind) {
    const bool special = (player1BikeKind - 6u < 3u) || (player1BikeKind - 15u < 3u);
    std::string suffix = (players == 2) ? "2" : "";
    if (special) suffix += "S";
    return "DATA\\CAMERA" + suffix + ".CA";
}

} // namespace rr
