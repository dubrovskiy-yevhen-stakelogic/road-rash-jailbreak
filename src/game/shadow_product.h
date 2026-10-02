#pragma once
// The model shadow in the product: the PORTED SLUS 0x80025EE0
// (src\game\sim\shadow.h) run at the tail of the emitter the model runtime serves (model_runtime.h
// CaptureHead), on the camera-space vertices the PORTED model draw just left - exactly the original's
// inputs - and each quad it links handed to the renderer as four world points (the RTPT's camera-space
// MACs through the render camera *(0x8005AEC0 + 4 view): +0x5C its rows, +0x1C its eye in 1/64 world
// unit) with the packet's colour. The renderer draws them as the GPU does (race_scene.cpp DrawShadow:
// mode 2 B - F, each pixel darkened once, no depth test).
//
// RRJB_SHADOW=ours: the port is not run and the renderer draws its own approximation (model 100's /
// 150's hull in a load-time pose, the bike's leaned up as the ground normal) - the negative control.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/fx_runtime.h" // FxPacket: the two-player shadow's sprite (mp2_product.h)
#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

struct ShadowQuad {
    float world[4][3] = {}; // q0..q3 (the packet's order is q0, q1, q3, q2)
    float colour[3] = {};   // 0..1, the packet's (subtracted from what is under it)
    uint32_t view = 0;
    uint32_t object = 0;
    int16_t sxy[4][2] = {}; // the packet's four screen points, in ITS order (v0 v1 v3 v2)
    int slot = -1;          // the ordering-table slot 0x80025EE0 linked it into
};

// The PORTED model draw's vertices of one bike or rider as the emitter received them: world =
// eye + rel per model vertex of the LOD drawn (the camera-space MACs *(0x8005ACB0) through the render camera).
struct CapturedObject {
    uint32_t model = 0; // *(+0x60)'s id
    int lod = -1;       // +0x08
    double eye[3] = {}; // the render camera's eye, world units
    std::vector<float> rel;
    // each vertex's SXY *(0x8005ACB4) + 4 v and MAC3 (+8 of *(0x8005ACB0) + 16 v, model
    // units: 64 << exp a world unit) - the numbers the emitter's packets are made of
    std::vector<uint32_t> sxy;
    std::vector<int32_t> mac3;
    int exp = 0;
};

struct ShadowFrame {
    std::vector<ShadowQuad> quads; // this frame's, all views
    std::vector<FxPacket> sprites; // two players: SLUS 0x80026960's sprite packets, all views (mp2_product.h)
    std::map<uint64_t, CapturedObject> poses; // (view << 32) | object: this frame's bikes and riders
    void Clear() {
        quads.clear();
        sprites.clear();
        poses.clear();
    }
    // run totals, for the log
    size_t calls = 0, packets = 0, twoPlayer = 0, refused = 0, objects = 0;
    uint32_t firstRefused = 0;
};

bool ShadowPortOn();
// The traffic cars, the props and the pedestrians through the PORTED model draw (the render's model pass, rrgame
// main.cpp) and drawn at its SXY; RRJB_GTE_OBJECTS=off (the negative control): the props and the pedestrians not in
// the model pass, only the bikes' and riders' vertices captured.
bool GteObjectsOn();

// The level loader's light stores, transcribed from our own listing of RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06) - what the product's arena lacked, so the shadow (and the model
// draw's light bytes) ran on the EXE's defaults (light (-2364, 2364, -2364), shadow colour 0x404080: a green
// shadow on the yellow lines):
//   0x8006250C (bundle section type 2, payload words w0..w8): 0x80052364 / 68 = -sin / -cos (w7 * 11),
//     0x80052366 first sin((w8 * 11) / 4) - the value Scale 0x8002EE50(-100 << 16) sees for 0x8005234C.. (>> 10) -
//     then sin(w8 * 11); 0x80052388 / 8C the two angles; 0x80052354 w6, 0x80052358.. w0 w1 w2, 0x8005236C w3,
//     0x80052370.. FixMul(0x10000 - w3, w0 / w1 / w2), 0x80052340 w4, 0x80052344 w5, 0x8005233C the bytes 2 of
//     w0 w1 w2 as a colour word;
//   0x80062430 (type 7): 0x8005237C = payload +0x480, 0x80052380 = u16(+0x484) - 0x100, 0x80052384 = 0x100 - that
//     (as u16), 1152 bytes to 0x800D4CA8, and the SHADOW COLOUR 0x80052348 = (255 - c) / 6 of the table's first
//     entry's three bytes (the multiply by 0x2AAAAAAB, signed).
// Returns a seam line. RRJB_LEVEL_LIGHT=exe: not run (the control).
std::string LevelLightStores(rr::sim::GuestRam& g, const rr::DiscImage& disc, int raceId);

// The reciprocal-square-root table *(gp+2260) = *(0x8005B560) in the arena: the game mallocs 2 KiB and fills it by
// SLUS 0x8002E080 (PORTED, road_query.h FillRsqrtTable; the session keeps its host copy `table`). The product's
// guest-memory ports that call Normalize 0x8002E468 through the pointer - the shadow's 45-degree bend - read it
// there; with the pointer 0 every normalised vector came out 0 and the shadow fell straight under the machine.
// OURS, named: where the 2 KiB sit (the session's bump region). Only when the pointer is not already in RAM.
std::string BuildRsqrtArena(rr::sim::GuestRam& g, const uint16_t* table, uint32_t& from, uint32_t limit);

// The frame's packet heap with the original's extent. The console's frame record *(0x8005B470) has its heap cursor
// +0x10C run from below 0x800E26E8 to *(0x8005B4D0) = 0x800F0460 (rr-race; rr-pack 0x800F63A0) - some 57 KiB - and
// its ordering table apart (0x800FB974). The tail of the HUD placement alone, 0x800DD400..0x800DFF00 (11008 B)
// with the effects' OT (5200 B) cut from its top, leaves 5808 B for the HUD, the effects, the glows and the
// shadows (~65 x 36 B a machine) - too little once the ported shadow runs (the passes refuse frames).
// BuildPacketHeap takes kPacketHeapBytes from the session's bump region (OURS, named: where; the size is the
// original's order); ApplyPacketHeap (after the HUD arena is built) points the HUD placement's heap, the record's
// cursor and *(0x8005B4D0) at it, so the effects' OT is cut from its top (FxRuntime::Setup). Returns a
// seam line; RRJB_PACKET_HEAP=hud: not moved (the control).
// `minBytes` below kPacketHeapBytes lets it take what is left up to `limit` when that is at least `minBytes` (the
// two-seat races, whose passenger stance bank must go into the same region first: race_session.cpp).
constexpr uint32_t kPacketHeapBytes = 0x10000;
std::string BuildPacketHeap(uint32_t& from, uint32_t limit, uint32_t minBytes = kPacketHeapBytes);
bool ApplyPacketHeap(rr::sim::GuestRam& g, uint32_t heapRecord, uint32_t& heapBase, uint32_t& heapEnd);

// The emitter's tail (SLUS 0x80025E00..0x80025EAC) for `obj` drawn in `view` at the GTE offset (ofx, ofy);
// `w24` is +0x24 as the emitter read it on entry.
void ShadowTail(rr::sim::GuestRam& g, uint32_t obj, uint32_t view, uint32_t w24, int32_t ofx, int32_t ofy,
                ShadowFrame& out);

// `rrgame --parity <capture> --shadowcheck <orig_prims.csv>`: this frame's shadow packets of view 0 against the
// original's GP0 0x2A packets of the same capture (psxgpu.py frame --prims), as multisets of four screen points:
// PASS when every one is equal (and there are some). The report line ends in "shadowcheck verdict PASS" / "FAIL".
std::string ShadowPacketCheck(const ShadowFrame& frame, const std::string& primsCsv);

} // namespace rr::game
