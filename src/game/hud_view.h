#pragma once
// The race HUD as the player sees it: the packets HudFrame (src\game\sim\hud.h) linked into the HUD's
// ordering-table slot, walked the way the GPU's DMA walks them, and rasterised the way the PlayStation
// GPU draws them (sprites, flat tiles, textured and flat polygons, lines, draw-mode and draw-area
// words) into a premultiplied RGBA overlay of the 384 x 240 draw area, which the renderer lays over the
// 3D frame.
//
// The rasteriser is ours; what it is checked against is the machine: `rrgame --hudcheck` compares
// (1) the packets our port links with the packets the ORIGINAL's GPU stream carried for the same state
// and (2) the pixels this rasteriser puts on the HUD's opaque texels with the pixels the interpreter's
// GPU model left in VRAM for the same frame.
#include "game/hud_arena.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rr::game {

struct HudPacket {
    uint32_t address = 0;          // guest address of the packet's tag word
    std::vector<uint32_t> words;   // the GP0 words, tag excluded
};

// Walks the linked list from the ordering-table word at `head` (its low 24 bits are the first packet),
// as DMA channel 2's linked-list mode does, until the 0xFFFFFF terminator. Zero-length links (empty
// ordering-table entries) are followed and not returned. At most `limit` packets.
std::vector<HudPacket> WalkHudList(const uint8_t* ram, uint32_t head, size_t limit = 4096);

struct HudOverlay {
    static constexpr int kWidth = 384, kHeight = 240;
    // Premultiplied colour and coverage: the frame under it shows through by (1 - alpha).
    std::vector<float> rgba = std::vector<float>(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
    std::vector<uint8_t> opaque = std::vector<uint8_t>(static_cast<size_t>(kWidth) * kHeight, 0); // last write opaque
    std::vector<uint16_t> opaqueColour = std::vector<uint16_t>(static_cast<size_t>(kWidth) * kHeight, 0); // 15-bit
    void Clear();
    // 8-bit premultiplied RGBA for the GPU.
    std::vector<uint8_t> Bytes() const;
};

struct HudRasterStats {
    size_t sprites = 0, tiles = 0, polygons = 0, lines = 0, modes = 0, areas = 0, unknown = 0;
    size_t subtractive = 0; // semi-transparency mode 2 pixels, which a premultiplied overlay approximates
};

// Draws `packets` over `out` (which it does not clear). `originX/Y` is the frame's drawing offset - the
// VRAM position of the overlay's top-left pixel: the packets' vertices are relative to it, the draw-area
// words absolute (the product draws its frame at 0,0).
HudRasterStats RasterizeHud(const std::vector<HudPacket>& packets, const HudVram& vram, HudOverlay& out,
                            int32_t originX = 0, int32_t originY = 0);

// The comparison of our HUD with the original's for the same state (file header). `stateDir` holds the
// capture (ram.bin), `traceDir` the output of
//   rrverify trace --state <stateDir> --frames 2 --no-cop2 --dump-vram --watch 0x80000000:0x200000:ram
//                  --probe 0x8005E848:hud_entry --probe 0x8005F028:hud_exit --out <traceDir>
// `mutate` is the negative control: our run reads the player's speed with one bit flipped, so the
// packets must then differ.
bool CheckHudAgainstTrace(const DiscImage& disc, const std::string& stateDir, const std::string& traceDir,
                          std::string& report, bool mutate, std::vector<uint8_t>* shownOut = nullptr,
                          std::vector<uint8_t>* composedOut = nullptr);
// `shownOut` / `composedOut` (384 x 240 RGBA) receive the frame the console showed at capture time and
// our HUD laid over it, for a human to look at.

// The PORTED HudFrame on a capture adopted as the arena (rrgame --parity): the
// player's two HUD slots *(0x8005B590) / *(0x8005B5A0) emptied as the frame start empties them, the packet heap
// left where the capture's frame had it, then HudFrame with the check's callees (ComputePlace from the capture's own
// records). False when it refused. RRJB_PARITY_HUD=off: not run (the capture's own slot is drawn).
bool RunHudFrameOnCapture(uint8_t* ram);

} // namespace rr::game
