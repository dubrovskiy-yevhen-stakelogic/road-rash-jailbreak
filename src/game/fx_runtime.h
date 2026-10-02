#pragma once
// The effects in the product: the sprite sheet the race loader builds, and the PORTED effect pass
// (src\game\sim\effects.h) run on the race arena every frame, its packets handed to the renderer
// (src\render\fx_draw.h).
//
// THE SHEET - `RASHCDI 0x80061FAC` (the level bundle's section type 5, dispatcher 0x80061C24 entry 5),
// transcribed from our own listing of RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06,
// loaded at 0x8005B5E8): eleven 4bpp TIMs, each uploaded at the configuration table's group 13 (SLUS
// 0x800533B4 + 88 (players - 1), bytes +53 / +54) plus the per-sprite offset RASHCDI 0x8006B4C0 [4 i],
// and its CLUTs (a running count k over all sprites) at SLUS 0x80053254 + 176 (players - 1): x = +0x68 +
// (k % +0x6C) * +0x6E, y = +0x6A + k / +0x6C. The 28-byte descriptors go to 0x800D4270: {s32 u, s32 v,
// s32 w, s32 h, u16 tpage, u16 CLUT count, u16 CLUT[4]}. The product has no VRAM: the uploads land in
// `FxVram`, a 1024 x 512 image of the rectangles the loader writes. `CheckFxSheet` compares both with a
// captured race state.
//
// THE FRAME - what the original's render does around the pass that the product has to do itself:
//   * RenderCamera SLUS 0x8002F17C (PORTED) for view 0, the render camera the pass reads;
//   * the ordering-table depth ranges RASHCDG 0x800674D4 writes into the scratchpad per view
//     (0x1F800000..0x1F80001C, transcribed); the scratchpad is this runtime's own (the session's holds
//     its active list), and the pass reads nothing else there;
//   * `+9 &= 0xF7` on every entity drawn (the last store of 0x800667C4, which the pose side runs for
//     riders only);
//   * the pass itself, `EffectPass 0x8002823C`, per live bike in pool order (OURS: the original walks
//     its per-cell draw list, which the product does not build).
// OURS, named: the ordering table (EXE's 0x8005ADFC entries, carved from the top of the HUD's packet
// heap, whose end moves down by as much), the libgpu ClearOTagR layout each frame, the depth range
// words *(0x8005B4D4) = 0 and *(0x8005B4D8) = 5 (every race capture's; their writer is not found).
//
// The records whose point the MODEL DRAW captures (states 3, 4, 5, 6: 0x80029CA4 inside the emitter
// 0x800251E4, on the model draw's camera-space vertex buffers) are drawable when `modelPass` ran: the
// PORTED model draw (model_runtime.h) fills those buffers before the pass.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/sim/effects.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

struct FxVram {
    static constexpr int kWidth = 1024, kHeight = 512;
    std::vector<uint16_t> px = std::vector<uint16_t>(static_cast<size_t>(kWidth) * kHeight, 0);
    uint16_t At(int x, int y) const { return px[static_cast<size_t>(y & 511) * kWidth + static_cast<size_t>(x & 1023)]; }
    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
    };
    std::vector<Rect> uploads; // every LoadImage of the loader, in order
};

// Builds the sheet: the descriptors into `g` at 0x800D4270, the pixels into `vram`. `mutate` drops
// the per-sprite offset table (the check's negative control). Returns false with `report` naming why.
bool BuildFxSheet(rr::sim::GuestRam& g, const DiscImage& disc, int raceId, int players, FxVram& vram,
                  std::string& report, bool mutate = false);

// `rrgame --fxsheetcheck <state dir>`: the sheet built on a blank image for the race and player count
// the capture names, against its RAM (the 308 descriptor bytes) and VRAM (every uploaded rectangle).
int CheckFxSheet(const DiscImage& disc, const std::string& stateDir, bool mutate);

// One packet of the frame, as the GPU would take it.
struct FxPacket {
    uint32_t address = 0;
    uint32_t function = 0;   // the emitter
    uint32_t record = 0;     // the effect record drawn
    uint32_t state = 0;      // its state (bits 6..9) when drawn
    uint32_t word1 = 0;      // command | colour
    int16_t x[4] = {}, y[4] = {};
    uint8_t u[4] = {}, v[4] = {};
    uint16_t tpage = 0, clut = 0;
    int32_t depth = 0;       // OTZ * 4 (camera-space z, 1/64 world unit), or the captured depth
    bool drawable = true;    // false for the states whose point the model draw captures
    uint32_t view = 0;       // the view whose pass linked it (two players: 0 and 1)
    uint32_t entity = 0;     // the entity whose EffectPass linked it (0: the model draw's glow sprites) - its cell's
                             // table (0x80067770)
};

// RASHCDG 0x800674D4's ordering-table depth ranges into the scratchpad `g` has attached (transcribed).
void FxDepthRanges(rr::sim::GuestRam& g);

// Reads one packet out of guest RAM.
FxPacket ReadFxPacket(const uint8_t* ram, uint32_t address);

// What the pass runs on: the product's stand-in for the original's per-cell draw list (built by the
// visibility test RASHCDG 0x80067AC4, not ported): every live bike of `bikes`, the rider of each when it
// is off its bike (+0x34 == 0: a top-level entity then; on the bike it is the bike's +0x38 child and
// the pass reaches it from there), and every live pool-3 car (+0xAC and +0x140 non-zero, slots up to
// the pool's high-water mark, as traffic_draw.h draws them).
std::vector<uint32_t> FxEntities(const uint8_t* ram, const std::vector<uint32_t>& bikes);

class FxRuntime {
public:
    // At race start, after the HUD arena (its packet heap) exists. Returns a seam line.
    std::string Setup(rr::sim::GuestRam& g, const DiscImage& disc, int raceId, int players);
    // rrgame --parity: the arena became a capture's RAM - the pass's ordering table becomes the
    // capture's own (its frame record +0x108, the original's placement), so the pass's OT clear does not land on
    // whatever the capture keeps where the product's heap was. False when the capture's record does not allow it.
    bool RecarveOt(uint8_t* arena);
    // rrgame --parity: every capture is RAM taken after its frame's draw cycle
    // (ModelLod's `+9 &= 0xF7`, then the pass - rr-pack's player bike holds +9 = 0x2B, bit 3 set), and the
    // parity frame IS that frame: the next Frame() skips the per-frame clear, so the pass only draws what the
    // capture's own pass already aged, exactly as the original's second view does. Without it the sprays
    // (state 2) would take one more step than the original drew them.
    void KeepCaptureAgesOnce() { keepAges_ = true; }
    bool Ready() const { return ready_; }
    // Once per frame after the session's frame: the render camera, the depth ranges, the OT, and the
    // PORTED pass over `entities`. False (with Error()) when the pass refused or faulted.
    // `views` = the player count: the original's render (SLUS 0x80011C4C) runs, per view v, RenderCamera(v)
    // (via 0x8002F2E8), the GTE offset at v's split rectangle's centre (0x8004D184), and the cell draw
    // 0x80035958(v) -> 0x800674D4 -> 0x80067770 -> EffectPass(e, v); the "updated this frame" bit +9 & 8 is
    // cleared once per frame (ModelLod 0x800667C4 in the draw cycle 0x8008CFDC), so view 1's pass only draws
    // what view 0's already aged (states 5 / 6 excepted, as EffectPass has it).
    bool Frame(uint8_t* arena, const std::vector<uint32_t>& entities, int views = 1);
    // The model draw that runs before the pass (model_runtime.h), on the pass's view of the arena (the
    // scratchpad and the depth ranges in place): it leaves the entities it drew in `drawn` and returns true;
    // the pass then walks only those (the original's draw list), and the records whose points it captured
    // (states 3..6) are drawable.
    // `view` and the GTE offset (ofx, ofy) of the view drawn.
    std::function<bool(rr::sim::GuestRam&, const std::vector<uint32_t>&, std::vector<uint32_t>&, uint32_t view,
                       int32_t ofx, int32_t ofy)>
        modelPass;
    const std::vector<FxPacket>& Packets() const { return packets_; }
    // The pass's environment while `modelPass` runs (null otherwise): the emitter's glow sprites (fx_glow.h)
    // link into the same ordering table and are told to the same sink, as state 0 (no effect record).
    rr::sim::FxEnv* ActiveEnv() const { return active_; }
    // The GTE offset view `v`'s pass projected with in the last frame (192 / 120 for one player).
    int32_t ViewOffsetX(uint32_t v) const { return ofx_[v & 1u]; }
    int32_t ViewOffsetY(uint32_t v) const { return ofy_[v & 1u]; }
    const FxVram& Vram() const { return vram_; }
    const std::string& Error() const { return error_; }
    // Run totals, for the log.
    size_t frames = 0, refusals = 0, packetsTotal = 0, packetsDrawn = 0, maxLive = 0, sirens = 0;
    size_t byState[16] = {}; // packets linked, by the drawn record's state

private:
    struct Sirens;
    FxVram vram_;
    std::vector<uint8_t> spad_ = std::vector<uint8_t>(1024, 0);
    std::vector<FxPacket> packets_;
    uint32_t ot_ = 0, otLength_ = 0;
    int32_t ofx_[2] = {192, 192}, ofy_[2] = {120, 120};
    bool ready_ = false;
    bool keepAges_ = false; // KeepCaptureAgesOnce
    rr::sim::FxEnv* active_ = nullptr;
    std::string error_;
};

} // namespace rr::game
