#include "render/frame_shot.h"

#include "platform/png.h"
#include "render/gl_api.h"

#include <cstdio>
#include <cstring>

namespace rr::render {

std::vector<uint8_t> ReadFrame(int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

void SaveShot(const std::string& path, int width, int height, const std::vector<uint8_t>& pixels) {
    // GL hands back the bottom row first; PNG wants the top row first.
    std::vector<uint8_t> flipped(pixels.size());
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(flipped.data() + static_cast<size_t>(y) * rowBytes,
                    pixels.data() + static_cast<size_t>(height - 1 - y) * rowBytes, rowBytes);
    rr::WritePng(path, width, height, flipped);
    std::printf("wrote %s (%dx%d)\n", path.c_str(), width, height);
}

} // namespace rr::render
