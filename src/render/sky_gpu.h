#pragma once
// The sky's packets drawn the way the GPU draws them: the POLY_FT4s
// the PORTED sky draw RASHCDG 0x80064B9C linked into the sky OT (game\sky_product.h) - the cloud ring (0x2F: raw,
// semi-transparent, 4-bit CLUT texels) and the panorama tiles (0x2C: opaque, modulated by *(0x8005237C), 15-bit
// texels of the MDEC tile cache) - in the GPU's order, at their screen points, over the gradient and under the
// world. The GPU's rules as measured for the GTE projection (gte_proj.h): the console pixel (x, y) covered and
// sampled at its integer corner (every vertex half a console pixel on), the quad as the triangles v0 v1 v2 and
// v1 v2 v3, u / v interpolated affinely and truncated (+1/256, GL's float landing a hair under an integer); texel
// 0x0000 transparent; an STP texel of a semi-transparent command blended by the tpage's mode.
//
// Placement: DrawRequest::gteMap (the console pixel -> NDC of the frame's own projection), on the float camera and the
// wide picture too (the sky's programs draw on past the console's field there, sky_product.h). Also drawn: the
// gradient's POLY_G4 (0x38: colour interpolated in 8 bits, truncated to 5 - no dither, as the
// reference psxgpu.py) and the two-player sky's SPRT (0x64.., texel (u + dx, v + dy) at pixel (x + dx, y + dy), the
// tpage of its DR_TPAGE word). With RRJB_SKY2=off the renderer draws its own cylinder, cloud ring and gradient.
#include "game/sky_product.h"
#include "render/gl_api.h"

#include <cstdint>
#include <vector>

namespace rr::render {

struct DrawRequest;

// This frame's packets and the VRAM their texels come from (tools\rrgame\main.cpp, after the sky draw); null: none.
// `anyPicture` false: only on the 4:3 GTE picture (RRJB_SKY3=off).
void SkyGpuSubmit(const std::vector<rr::game::SkyPacket>* packets, const rr::game::SkyVram* vram, bool anyPicture = true);
// RaceScene::Draw's hook: true when the ported sky was drawn (the caller then skips its own clouds and panorama).
bool SkyGpuDraw(const DrawRequest& request);
// The submitted packets hold the PORTED gradient's POLY_G4s (the caller then skips its own gradient).
bool SkyGpuHasGradient();
// For the log: frames drawn, packets drawn, texture pages rebuilt.
struct SkyGpuStats {
    size_t frames = 0, packets = 0, pages = 0, fallbacks = 0;
};
const SkyGpuStats& SkyGpuCounters();

} // namespace rr::render
