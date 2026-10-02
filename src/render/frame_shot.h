#pragma once
// A frame of the bound framebuffer as pixels, and a PNG of it - portable (desktop GL and OpenGL ES): the desktop
// window's shots (render/window_win32.h), the VR eye images and the Quest's shots use the same two functions.
#include <cstdint>
#include <string>
#include <vector>

namespace rr::render {

// The bound READ framebuffer's pixels (RGBA8), bottom row first. On the desktop window: call it BEFORE SwapBuffers -
// the back buffer is undefined afterwards, and a shot taken after the swap comes out black.
std::vector<uint8_t> ReadFrame(int width, int height);

// Writes an already-read frame out as a PNG, flipping it to top-row-first.
void SaveShot(const std::string& path, int width, int height, const std::vector<uint8_t>& pixels);

} // namespace rr::render
