#pragma once
// The traffic cars (pool 3) drawn with their own models and sheets.
//
// Which models: the race loader `RASHCDI 0x8005C630(raceId)` builds the file name as
// "%s%s%s%s%s" = "DATA\", "car", the two digits of `game_state+0x40` (the race id), the per-player-count
// suffix of the table at 0x8006B4B8 ("a" one player, "b" two) and ".GEO" - so a one-player race N
// loads DATA\CAR<NN>A.GEO (`rrgame --trafficarenacheck`: the four race captures hold CAR04A's models).
// Which model a car shows is the ported model binder's choice: the registry slot at car +0x60, whose
// +0x00 is the model id. Where: the box centre +0xB8 (16.16) and the rows +0x1B0 / +0x1B6 / +0x1BC,
// exactly as the bikes are drawn (tools\rrgame).
//
// The sheet: the model's texture id DOD3+0x1C names a LECT chunk (kind 3, an 8bpp
// 128x60 TIM) of the race's level bundle, section type 9 (DATA\GAMEBIN1.DAT, the section RASHCDI
// 0x8006275C walks). The palette is the OBJECT's: a1 = car +0x24 bits 12..17 (the
// ported binder draws 14 + Rand() % 15 for most models) -> VRAM (640 + (a1 % 3) * 128, 511 - a1 / 3),
// which for a1 0..28 is KNBP block k = 45 - 3 * (a1 / 3) + a1 % 3 of DATA\BBLEVEL<bank+1>.TEX (the
// positional upload rule a1 = 3*((47-k)/3) + k%3) - MEASURED here against `rr-race`'s VRAM for every a1 0..28
// (the traffic car there has a1 = 16 -> BBLEVEL1.TEX +0x1914, block 25 = block 31's bytes). a1 29..47:
// RegistryBind SLUS 0x8002FDEC gives model 300 a1 = 47 and model 315 a1 = 46 (309 gets
// 28, every other car 14 + Rand() % 15), and the level bundle's LECT loader RASHCDI 0x8005DDB8 (its kind-1
// arm) puts sheet 116's CLUT in slot 47, sheet 149's in slot 46 and sheet 151's in slot 41 - the first 128
// entries of that sheet's own TIM CLUT (0x80048A6C, 128 x 1; any other sheet's slot comes from a running
// counter and nothing is uploaded) - and models 300 / 315 ARE sheets 116 / 149 (DOD3+0x1C). So a car in
// slot 46 / 47 / 41 takes that fixed sheet's CLUT; slots 29..40, 42..45 hold the racers' and the roadside
// sheets' palettes, which no car's binding reaches (drawn with its own CLUT, named).
//
// The level of detail: the car's +0x08, which the PORTED ModelLod 0x800667C4 and ModelVisible 0x80067AC4
// (model_runtime.h) choose by distance each frame through LodSelect; group k of the model is drawn.
#include "render/coplanar.h"
#include "render/gl_api.h"
#include "render/gpu_texture.h"
#include "render/mat4.h"
#include "rrformats/rmd3.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rr::render {

class RaceScene;
struct CapturedVerts;

class TrafficDraw {
public:
    // Parses DATA\CAR<raceId>A.GEO and the sheets; a GL context must be current. False when the file is missing.
    bool Load(const rr::DiscImage& disc, int raceId, int bank);
    const std::string& FileName() const { return file_; }
    size_t ModelCount() const { return models_.size(); }
    // Draws every live pool-3 car of the guest image `ram` (2 MiB) with `scene`'s program. Returns the
    // number of cars drawn; `unknownModel` counts live cars whose model id is not in the file.
    // `keep` (optional): a live car is drawn only when it answers true - the product passes
    // the model draw's cell test and ModelVisible's range for kind 3 (cell_view.h InDrawnCell / InDrawRange).
    // `captured` (optional): a car's vertices as the PORTED model draw left them this view; a car
    // drawn at the LOD it was captured at is drawn at that capture's own SXY (RaceScene::GteObjectSoup).
    size_t Draw(const RaceScene& scene, const Mat4& viewProj, const uint8_t* ram, float scale,
                size_t* unknownModel = nullptr, const std::function<bool(uint32_t)>* keep = nullptr,
                const std::function<const CapturedVerts*(uint32_t)>* captured = nullptr);
    size_t Textured() const { return textured_; }
    size_t OwnClut() const { return ownClut_; }
    size_t LodDraws(size_t lod) const { return lod < 4 ? lodDraws_[lod] : 0; } // cars drawn at LOD k
    bool forceLod0 = false; // the PC graphics settings' maximum detail: every car at LOD 0 (+0x08 not read)

    // DEVELOPMENT (tools\rrgame\carflick_probe.cpp): the cars the last Draw drew, and a mode that draws each
    // car as the flat id colour (slot + 1, 0, 255) of uDebug 2 - the probe's mask of which pixel is which car.
    struct ProbeCar {
        int slot = 0;
        uint32_t model = 0;
        float pos[3] = {};
        int lodByte = 0;
        uint32_t flags = 0; // +0x24
    };
    static std::vector<ProbeCar>& ProbeCars();
    static bool& ProbeIds();

private:
    struct Range {
        GLint first = 0;
        GLsizei count = 0;
        uint32_t texId = 0;
        bool lit = false;   // the model light (race_scene.h ApplyObjectLight): normals present
        int modelClass = 0; // DOD3 +0x0E bits 3..6
        size_t group = 0;   // index into groups_
        uint32_t model = 0;
    };
    const GpuIndexedTexture* Sheet(uint32_t texId, uint32_t a1);
    std::map<uint32_t, std::vector<Range>> models_; // model id -> each group's (LOD's) triangles
    std::map<uint32_t, std::vector<uint8_t>> tims_; // texture id -> the LECT's TIM bytes
    std::vector<uint8_t> bankTex_;                  // DATA\BBLEVEL<bank+1>.TEX (the KNBP bank)
    std::map<uint64_t, GpuIndexedTexture> sheets_;  // (texture id, a1) -> uploaded
    GLuint vao_ = 0, vbo_ = 0;
    GLuint streamVao_ = 0, streamVbo_ = 0;           // a car at its captured SXY, per draw
    std::vector<rr::TriangleSoup::Vertex> soupCpu_;  // the static soup, for it
    ModelLayers layers_; // per group (groups_ order) its coplanar decals (coplanar.h)
    std::vector<rr::ModelGroup> groups_;
    std::vector<rr::TriangleSoup::Vertex> gteSoup_;
    std::string file_;
    size_t textured_ = 0, ownClut_ = 0;
    size_t lodDraws_[4] = {};
};

} // namespace rr::render
