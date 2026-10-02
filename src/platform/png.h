#pragma once
// Minimal PNG writer so that every rendering change can be checked without a human looking at
// a window. Uses stored (uncompressed) deflate blocks - no zlib dependency, larger files.
#include <cstdint>
#include <string>
#include <vector>

namespace rr {

// `rgba` is width*height*4 bytes, top row first.
void WritePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba);

} // namespace rr
