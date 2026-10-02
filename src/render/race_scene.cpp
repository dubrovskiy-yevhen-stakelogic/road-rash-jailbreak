#include "render/race_scene.h"

#include "render/shaders.h"
#include "render/sky_gpu.h" // the sky's packets as the GPU draws them
#include "render/edge_rule.h" // the console's fill rule on GL
#include "game/sim/fixed.h" // ApproxLen3 SLUS 0x8001FCB0, the props' draw range
#include "rrformats/model_texture.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "rrformats/texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rr::render {
namespace {

void BindSoup(GLuint& vao, GLuint& vbo, const rr::TriangleSoup& soup) {
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);
    gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER,
                  static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                  soup.vertices.data(), GL_STATIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(sizeof(rr::TriangleSoup::Vertex));
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 3));
    gl.EnableVertexAttribArray(1);
    // Texel coordinates and the palette the primitive selects, straight out of the file.
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 6));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(3, 1, GL_UNSIGNED_SHORT, GL_FALSE, stride,
                           reinterpret_cast<void*>(sizeof(float) * 8 + sizeof(uint16_t)));
    gl.EnableVertexAttribArray(3);
    // The PS1 vertex colour and its mode, and the texture window (rmd3.h, TriangleSoup::Vertex).
    gl.VertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                           reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, shade)));
    gl.EnableVertexAttribArray(4);
    gl.VertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                           reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, window)));
    gl.EnableVertexAttribArray(5);
    // The primitive's ordering-table key rule (rmd3.h TriangleSoup::Vertex::ot; race_scene_ot.cpp).
    gl.VertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, ot)));
    gl.EnableVertexAttribArray(6);
}

} // namespace

void RaceScene::CreatePrograms() {
    // edge_rule.h: the console's fill rule for every triangle the GTE placed (RRJB_EDGE=gl: GL's own).
    program_ = BuildEdgeProgram(kVertexShader, kFragmentShader,
                                {{"vec3", "vNormal", EdgeVarying::kSmooth}, {"vec2", "vTexelP", EdgeVarying::kSmooth},
                                 {"float", "vPalette", EdgeVarying::kSmooth}, {"vec4", "vShadeP", EdgeVarying::kSmooth},
                                 {"vec2", "vTexelA", EdgeVarying::kNoPerspective}, {"vec4", "vShadeA", EdgeVarying::kNoPerspective},
                                 {"vec3", "vWindow", EdgeVarying::kFlat}, {"float", "vOneSided", EdgeVarying::kFlat},
                                 // the texel coordinates at the centroid, and whether the draw is an object
                                 {"vec2", "vTexelPC", EdgeVarying::kSmooth, true}, {"vec2", "vTexelAC", EdgeVarying::kNoPerspective, true},
                                 {"float", "vObject", EdgeVarying::kFlat}},
                                2, "scene");
    viewProjLocation_ = gl.GetUniformLocation(program_, "uViewProj");
    modelLocation_ = gl.GetUniformLocation(program_, "uModel");
    tintLocation_ = gl.GetUniformLocation(program_, "uTint");
    texturedLocation_ = gl.GetUniformLocation(program_, "uTextured");
    indexLocation_ = gl.GetUniformLocation(program_, "uIndex");
    paletteLocation_ = gl.GetUniformLocation(program_, "uPalette");
    texSizeLocation_ = gl.GetUniformLocation(program_, "uTexSize");
    paletteCountLocation_ = gl.GetUniformLocation(program_, "uPaletteCount");
    paletteSizeLocation_ = gl.GetUniformLocation(program_, "uPaletteSize");
    debugLocation_ = gl.GetUniformLocation(program_, "uDebug");
    debugIdLocation_ = gl.GetUniformLocation(program_, "uDebugId");
    nclipLocation_ = gl.GetUniformLocation(program_, "uNclip");
    affineLocation_ = gl.GetUniformLocation(program_, "uAffine");
    {
        const auto off = [](const char* name) {
            const char* v = std::getenv(name);
            return v != nullptr && std::string(v) == "off";
        };
        affine_ = !off("RRJB_AFFINE");
        subdivide_ = affine_ && !off("RRJB_SUBDIV");
    }
    modelLightLocation_ = gl.GetUniformLocation(program_, "uModelLight");
    lightXLocation_ = gl.GetUniformLocation(program_, "uLightX");
    lightYLocation_ = gl.GetUniformLocation(program_, "uLightY");
    lightZLocation_ = gl.GetUniformLocation(program_, "uLightZ");
    rampShiftLocation_ = gl.GetUniformLocation(program_, "uRampShift");
    unlitLocation_ = gl.GetUniformLocation(program_, "uUnlit");
    for (int k = 0; k < 32; ++k) {
        const std::string name = "uRamp[" + std::to_string(k) + "]";
        rampLocation_[k] = gl.GetUniformLocation(program_, name.c_str());
    }
    OtPrograms(); // race_scene_ot.cpp
    GtePrograms(); // gte_proj.cpp
    PcPrograms();  // race_scene_pc.cpp: the PC graphics settings' uniforms (smooth textures)
    // A uniform the shader stops *using* is optimised away and its location silently becomes -1,
    // which then makes every glUniform on it a no-op rather than an error. Worth knowing about
    // before cutting the shader down to bisect something.
}

void RaceScene::SetModelMatrix(const Mat4& m) const { gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, m.m); }
void RaceScene::SetTint(float r, float g, float b) const { gl.Uniform3f(tintLocation_, r, g, b); }
void RaceScene::SetTextured(int on) const { gl.Uniform1i(texturedLocation_, on); }
void RaceScene::SetDebug(int mode) const { gl.Uniform1i(debugLocation_, mode); }
void RaceScene::SetRiderRelative(const float m[9]) {
    for (int k = 0; k < 9; ++k) riderRelative_[k] = m[k];
}

void RaceScene::BindIndexed(const GpuIndexedTexture& texture) const {
    gl.Uniform1i(texturedLocation_, 1);
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture.indexTexture);
    gl.Uniform1i(indexLocation_, 0);
    gl.ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture.paletteTexture);
    gl.Uniform1i(paletteLocation_, 1);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.Uniform2f(texSizeLocation_, texture.width, texture.height);
    gl.Uniform1f(paletteCountLocation_, texture.paletteCount);
    gl.Uniform1f(paletteSizeLocation_, texture.paletteSize);
    BindSmooth(texture); // race_scene_pc.cpp (off unless the PC settings ask for smooth textures)
}

void RaceScene::SetCells(const rr::TriangleSoup& soup, std::vector<CellRange> ranges) {
    cellRanges_ = std::move(ranges);
    if (soup.vertices.empty()) return;
    cellVertexCount_ = static_cast<GLsizei>(soup.vertices.size());
    BindSoup(cellVao_, cellVbo_, soup);
    GteKeepSoup(soup); // gte_proj.cpp: each vertex's cell vertex, for the GTE's own SXY
    PcBoxes(soup);     // race_scene_pc.cpp: the runs' bounding boxes and the pages' palette rows
    PcLayers(soup);    // race_scene_pc.cpp: the coplanar overlapping primitives (the depth buffer's layers)
}

// The cell textures. A scene cell's header pair holds packed texture keys, and a key names a
// texture resource with the SAME id as the cell, shipped as type-1 and type-2 chunks in the same
// stream, right next to the cells. Band 0 samples the type-2 chunk: the band-0 fix-up pass
// `SLUS_010.53 0x80034428` hands its resolver the two bit-15 keys and the descriptors those
// resolved to, where band 1's pass at `0x80033F94` hands over the two keys without bit 15.
// Rule and evidence: docs\formats\scene_cell.md 12.
void RaceScene::LoadCellTextures(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs,
                                 const std::vector<rr::CellData>& cells, bool withBand1) {
    const std::vector<uint16_t> wanted = CellTextureKeys(cells, withBand1);
    // A type-2 key names ONE chunk and a type-1 key TWO, which is why the halves are collected in
    // file order and only then turned into a page: `0x80034dec` fills a texture record's `+0x0C`
    // from the first and `+0x10` from the second, and the VRAM comparison of scene_cell.md 12.4
    // confirms that order on a 64x256 rectangle.
    std::map<uint16_t, std::vector<std::vector<uint8_t>>> halves;
    const size_t scanned = ScanRaceStream(
        disc, set, legs,
        [](uint8_t type) {
            return type == static_cast<uint8_t>(rr::ChunkType::TextureA) ||
                   type == static_cast<uint8_t>(rr::ChunkType::TextureB);
        },
        [&](uint32_t, const std::vector<uint8_t>& texChunk) {
            const rr::ChunkHeader header = rr::ParseChunkHeader(texChunk);
            const uint16_t packed = rr::PackedTextureKey(header);
            if (std::find(wanted.begin(), wanted.end(), packed) == wanted.end()) return;
            const size_t need = header.type == static_cast<uint8_t>(rr::ChunkType::TextureB) ? 1u : 2u;
            std::vector<std::vector<uint8_t>>& got = halves[packed];
            if (got.size() >= need) return;
            // A resource is re-shipped along the stream for streaming, so the same payload comes
            // round again; only a payload we have not seen is a new half.
            for (const std::vector<uint8_t>& had : got)
                if (had == texChunk) return;
            got.push_back(texChunk);
        });
    for (const auto& entry : halves) {
        const std::vector<std::vector<uint8_t>>& got = entry.second;
        const bool typeTwo = (entry.first & 0x8000u) != 0;
        if (got.size() < (typeTwo ? 1u : 2u)) {
            std::printf("  key %04X: only %zu of %d chunks in the scanned range, not built\n", entry.first,
                        got.size(), typeTwo ? 1 : 2);
            continue;
        }
        cellAtlases_[entry.first] =
            typeTwo ? rr::BuildCellTexturePage(got[0]) : rr::BuildCellTexturePage(got[0], got[1]);
    }
    std::printf("cell textures: %zu of %zu keys found in %zu chunks scanned (%dx%d, %d palettes of %d)\n",
                cellAtlases_.size(), wanted.size(), scanned,
                cellAtlases_.empty() ? 0 : cellAtlases_.begin()->second.width,
                cellAtlases_.empty() ? 0 : cellAtlases_.begin()->second.height,
                cellAtlases_.empty() ? 0 : cellAtlases_.begin()->second.paletteCount,
                cellAtlases_.empty() ? 0 : cellAtlases_.begin()->second.paletteSize);
    for (uint16_t key : wanted)
        if (!cellAtlases_.count(key))
            std::printf("  key %04X: NO type-%d chunk in the scanned range\n", key, (key & 0x8000u) ? 2 : 1);
    // texRef 0x7800 is not a stream resource: the fix-up pass takes its page from the single
    // runtime record at guest `0x800D6160` (tpage 0x000E -> VRAM (896,0)), and that page is
    // `DATA\G_OBJ01.GTP` byte for byte. Its 14 palettes ship in its own first four rows.
    if (const auto gtpFile = disc.Find("DATA/G_OBJ01.GTP")) {
        cellAtlases_[rr::kCellTexRuntimePage] = rr::BuildRuntimeObjectPage(disc.ReadFile(*gtpFile));
        std::printf("runtime object page: DATA/G_OBJ01.GTP, %dx%d, %d palettes of %d\n",
                    cellAtlases_[rr::kCellTexRuntimePage].width,
                    cellAtlases_[rr::kCellTexRuntimePage].height,
                    cellAtlases_[rr::kCellTexRuntimePage].paletteCount,
                    cellAtlases_[rr::kCellTexRuntimePage].paletteSize);
    }
    for (const auto& entry : cellAtlases_)
        if (!cellTextures_.count(entry.first)) cellTextures_[entry.first] = UploadIndexedTexture(entry.second);
}

// The level's look (docs\formats\scene_cell.md 13): the 256-entry colour table and the road page of
// the race's DATA\GAMEBIN1.DAT bundle, and the constant tables the draw routines read out of the
// race overlay and the EXE. Every piece is read from the player's own disc.
bool RaceScene::LoadLevel(const rr::DiscImage& disc, int set, int raceId) {
    raceSet_ = set;
    const auto gamebin = disc.Find("DATA/GAMEBIN1.DAT");
    const auto overlay = disc.Find("RASHCDG.BIN");
    const auto exe = disc.Find("SLUS_010.53");
    if (!gamebin || !overlay || !exe) {
        std::fprintf(stderr, "level look: GAMEBIN1.DAT, RASHCDG.BIN or SLUS_010.53 is missing\n");
        return false;
    }
    try {
        const std::vector<uint8_t> file = disc.ReadFile(*gamebin);
        const std::vector<uint8_t> exeBytes = disc.ReadFile(*exe);
        const rr::LevelBundle bundle = rr::ParseLevelBundle(file, rr::LevelBundleIndexForRace(raceId));
        look_.colours = rr::ParseLevelColourTable(file, bundle);
        look_.haveColours = true;
        // The model light: the ramp and unlit step of the type-7 section, the sun vector of the
        // type-2 one through the EXE's sine table at 0x8005624C (entry k: sin, cos; RASHCDI 0x8006250C).
        try {
            const rr::LevelModelLight ml = rr::ParseLevelModelLight(file, bundle);
            for (int k = 0; k < 32; ++k)
                for (int c = 0; c < 3; ++c) modelRamp_[k][c] = static_cast<float>((ml.ramp[static_cast<size_t>(k)] >> (8 * c)) & 0xFFu);
            modelUnlit_ = ml.unlitStep;
            const rr::LevelSun sun = rr::ParseLevelSun(file, bundle);
            const auto sine = [&exeBytes](int32_t angle, int half) {
                const size_t at = 0x5624Cu - 0x10000u + 0x800u + 4u * static_cast<uint32_t>(angle & 0xFFF) + 2u * static_cast<uint32_t>(half);
                if (at + 2 > exeBytes.size()) throw std::runtime_error("SLUS_010.53: the sine table is past the end");
                return static_cast<int32_t>(static_cast<int16_t>(exeBytes[at] | (exeBytes[at + 1] << 8)));
            };
            sunVector_[0] = -sine(sun.yaw, 0);
            sunVector_[1] = sine(sun.elevationAngle, 0);
            sunVector_[2] = -sine(sun.yaw, 1);
            modelRampValid_ = true;
            std::printf("model light: ramp (%.0f,%.0f,%.0f)..(%.0f,%.0f,%.0f), unlit step %d, sun vector (%d, %d, %d)\n",
                        modelRamp_[0][0], modelRamp_[0][1], modelRamp_[0][2], modelRamp_[31][0], modelRamp_[31][1],
                        modelRamp_[31][2], modelUnlit_, sunVector_[0], sunVector_[1], sunVector_[2]);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "model light: %s (the old shade stays)\n", e.what());
        }
        look_.tables = rr::ReadCellDrawTables(disc.ReadFile(*overlay), exeBytes);
        look_.haveTables = true;
        roadPage_ = rr::BuildRoadPage(rr::ParseRoadTextures(file, bundle), exeBytes, set);
        rr::IndexedTexture road;
        road.width = 256;
        road.height = 256;
        road.bpp = 4;
        road.indices = roadPage_.indices;
        road.paletteCount = 12;
        road.paletteSize = 16;
        road.palettes.resize(12 * 16);
        for (size_t row = 0; row < 12; ++row)
            for (size_t i = 0; i < 16; ++i) road.palettes[row * 16 + i] = rr::Bgr555ToRgba(roadPage_.cluts[row][i]);
        cellAtlases_[rr::kCellTexBand2] = road;
        if (cellTextures_.count(rr::kCellTexBand2) == 0)
            cellTextures_[rr::kCellTexBand2] = UploadIndexedTexture(road);
        std::printf("level look: GAMEBIN1.DAT bundle %zu (race %d): colour table, road page at VRAM (%d,%d) "
                    "with %d of 3 TIMs placed and 12 palettes; overlay draw tables read\n",
                    bundle.index, raceId, roadPage_.pageX, roadPage_.pageY, roadPage_.timsPlaced);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "level look: %s\n", e.what());
        return false;
    }
    return true;
}

bool PropInRange(const PropInstance& instance, const int32_t eye16[3]) {
    int32_t d[3];
    for (int k = 0; k < 3; ++k)
        d[k] = static_cast<int32_t>(static_cast<uint32_t>(instance.pos[k]) - static_cast<uint32_t>(eye16[k])) >> 10;
    const int32_t dist = rr::sim::ApproxLen3(d[0], d[1], d[2]);
    const int32_t range = instance.group != 0 ? kPropDrawRange : kPropDrawRange + 32000;
    return dist >= 0 && dist <= range;
}

int PickHazardSet(const rr::DiscImage& disc, uint8_t mode, uint32_t seed) {
    if ((mode & 0x18u) == 8u) return 0; // 0x8005C82C: the digit comes from 0
    if (mode & 0x10u) return 1;         // 0x80063814: s0 = 1, 0x8006AD4C not called
    const auto env = disc.Find("DATA/ENV.EN");
    if (!env) return -1;
    const std::vector<uint8_t> bytes = disc.ReadFile(*env);
    if (bytes.size() < 0xCDu) return -1;
    const uint32_t count = static_cast<int8_t>(bytes[0xCC]) > 0 ? bytes[0xCC] : 0u; // `lb` at 0x8006AD60
    uint32_t sel = 0;
    if (count != 0) {
        seed = seed * 0x0019660Du + 0x3C6EF35Fu; // Rand, SLUS 0x8001FC58
        sel = seed % count;                      // `divu` at 0x8006AD94
    }
    const size_t at = 0xCDu + 2u * sel;
    if (at >= bytes.size()) return -1;
    const int v = static_cast<int8_t>(bytes[at]);
    return v < 1 ? 1 : (v > 5 ? 5 : v); // 0x8006AEC0..0x8006AEE8
}

// Roadside props. A kind-4 placement names group `cls` of model id 200 - the HAZARD<n>.GEO prop
// sheet, 39 separate props - at an absolute world position with a unit orientation. How the
// original places and draws one, read from RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c)
// and SLUS_010.53 (67ed165a2c517d4e6106fb0dfa324d66dd9a76f1), and checked against the packets the
// original draws for the rr-pack capture (rrview --propcheck):
//   * the class arms of the spawner 0x8009C654: class 50 goes to 0x80012BA8 (only when game_state+4
//     has bit 4), classes 0 and 9 to the hazard-object pool (0x800A0A20, not model 200); every other
//     class below the registry's group count (39) is group `cls` of model 200 (0x8009C5E4, the bind
//     0x8002FAD4 kind 6 -> LodSelect(obj, cls)). Classes 50, 0 and 9 are therefore not drawn here.
//   * the unit: a prop group has coordinate exponent 0 (DOD3+0x0E 0x0232), so its vertices go to the
//     GTE in the unit of its translation `(+0xB8 - eye) >> 10`, 1/64 world unit - the soup, folded to
//     the finest unit by LodFactor (x16), is 1/1024 world unit, as the machines are (kModelUnitsPerWorldUnit).
//   * the rotation: part 0's matrix (ModelDraw reads *(obj+4)+4) is, in all six props of rr-pack,
//     rows (nz, 0, nx), (0, 1, 0), (-nx, 0, nz) of the record's unit vector n (+0x0E, copied to +0x1BC):
//     the model's Z axis is n, its X axis (nz, 0, -nx) - a rotation; the same matrix with X
//     negated would be a reflection (mirrored signs, and the other plate of a pair facing the rider).
void RaceScene::LoadProps(const rr::DiscImage& disc, const std::vector<rr::CellData>& cells,
                          bool useTextures, bool texProbe, int hazardSet) {
    if (hazardSet < 0 || hazardSet > 5) hazardSet = 0;
    const std::string geoName = "DATA/HAZARD" + std::to_string(hazardSet) + ".GEO";
    const std::string texName = "DATA/HAZARD" + std::to_string(hazardSet) + ".TEX";
    const auto propFile = disc.Find(geoName);
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!propFile || !overlay) return;
    const std::vector<rr::Model> propModels = rr::ParseGeo(disc.ReadFile(*propFile));
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    rr::TriangleSoup propSoup;
    for (const rr::ModelGroup& group : propModels.front().groups) {
        propFirst_.push_back(static_cast<GLint>(propSoup.vertices.size()));
        const rr::TriangleSoup one = rr::BuildAssembledTriangleSoup(group, rr::AssembleGroup(group, skeleton));
        propSoup.vertices.insert(propSoup.vertices.end(), one.vertices.begin(), one.vertices.end());
        propCount_.push_back(static_cast<GLsizei>(one.vertices.size()));
        propLight_.emplace_back();
        SetPartLight(propLight_.back(), group); // the model light's inputs
        propGroups_.push_back(group);           // GteObjectSoup
    }
    propModelId_ = propModels.front().id;
    GteStreamArray(gteStreamVao_, gteStreamVbo_);
    BindSoup(propVao_, propVbo_, propSoup);
    propSoup_ = propSoup;
    { // the coplanar layers of each prop group (coplanar.h: a sign's face over its plate)
        std::vector<std::pair<GLint, GLsizei>> ranges;
        for (size_t g = 0; g < propFirst_.size(); ++g) ranges.emplace_back(propFirst_[g], propCount_[g]);
        propLayers_.Build(propVao_, propSoup.vertices, ranges, kModelUnitsPerWorldUnit, false);
    }

    if (texProbe) {
        const auto probeTexFile = disc.Find(texName);
        const rr::IndexedTexture atlas = rr::BuildPropAtlas(disc.ReadFile(*probeTexFile));
        std::printf("texprobe: atlas %dx%d, %d palettes of %d\n", atlas.width, atlas.height,
                    atlas.paletteCount, atlas.paletteSize);
        for (size_t g = 0; g < propFirst_.size(); ++g) {
            size_t opaque = 0, transparent = 0, black = 0, outside = 0, zeroNormal = 0;
            int minU = 999, maxU = -1, minV = 999, maxV = -1, minPage = 9999, maxPage = -1;
            for (GLsizei k = 0; k < propCount_[g]; ++k) {
                const rr::TriangleSoup::Vertex& v =
                    propSoup.vertices[static_cast<size_t>(propFirst_[g]) + static_cast<size_t>(k)];
                const int u = static_cast<int>(v.u), w = static_cast<int>(v.v);
                const int page = static_cast<int>(v.tpage);
                minU = std::min(minU, u); maxU = std::max(maxU, u);
                minV = std::min(minV, w); maxV = std::max(maxV, w);
                minPage = std::min(minPage, page); maxPage = std::max(maxPage, page);
                if (v.nx == 0.0f && v.ny == 0.0f && v.nz == 0.0f) ++zeroNormal;
                if (u >= atlas.width || w >= atlas.height || page >= atlas.paletteCount) { ++outside; continue; }
                const uint8_t index = atlas.indices[static_cast<size_t>(w) * atlas.width + u];
                const uint32_t rgba = atlas.palettes[static_cast<size_t>(page) * 16 + index];
                if ((rgba >> 24) < 128) ++transparent;
                else if ((rgba & 0x00FFFFFFu) == 0) ++black;
                else ++opaque;
            }
            std::printf("  group %2zu: %4d verts, u %3d..%3d v %3d..%3d tpage %d..%d | "
                        "colour %zu, black %zu, transparent %zu, out of range %zu, zero normal %zu\n",
                        g, propCount_[g], minU, maxU, minV, maxV, minPage, maxPage, opaque, black,
                        transparent, outside, zeroNormal);
        }
    }

    // The prop sheet: a 4bpp atlas whose palettes ship in its own trailing rows. Off by default
    // because it costs a second disc read.
    //
    // The props carry no normals: `DOD3+0x28` (the normal array) and `+0x30` (the vertex -> normal
    // index) are 0 in all 39 groups of HAZARD0.GEO, so every prop vertex reaches the shader with a
    // zero normal, whose `normalize()` would be NaN - written to the framebuffer as 0, pure black. The
    // fragment shader treats "no normal" as "unlit", which is also what the console does: it can only
    // Gouraud-shade from a normal array.
    try {
        if (!useTextures) throw std::runtime_error("off by default; pass --tex");
        const auto propTexFile = disc.Find(texName);
        if (propTexFile) {
            propAtlas_ = rr::BuildPropAtlas(disc.ReadFile(*propTexFile));
            propTexture_ = UploadIndexedTexture(propAtlas_);
            std::printf("prop atlas: %.0fx%.0f, %.0f palettes of %.0f\n", propTexture_.width,
                        propTexture_.height, propTexture_.paletteCount, propTexture_.paletteSize);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "prop atlas: %s\n", e.what());
    }

    constexpr float kPropScale = 1.0f / kModelUnitsPerWorldUnit; // the soup's unit (see above)
    size_t notModel200 = 0;
    for (const rr::CellData& cell : cells)
        for (const rr::CellPlacement& placement : cell.placements) {
            if (placement.kind != 4) continue; // only the roadside props have a model
            if (placement.cls == 0 || placement.cls == 9 || placement.cls == 50) { // other arms (above)
                ++notModel200;
                continue;
            }
            if (placement.cls >= propFirst_.size()) continue; // 0x8009C5E4: no such group
            PropInstance instance;
            instance.group = placement.cls;
            // Y is exactly 0 for every kind-4 record, so this is a pure yaw about the (down) Y axis.
            const float nx = placement.orientation[0] / 4096.0f, nz = placement.orientation[2] / 4096.0f;
            instance.centre[0] = rr::PlacementWorldX(placement);
            instance.centre[1] = rr::PlacementWorldY(placement);
            instance.centre[2] = rr::PlacementWorldZ(placement);
            for (int k = 0; k < 3; ++k) instance.pos[k] = placement.pos[k];
            // Column-major: model X -> (nz, 0, -nx), model Y -> (0, 1, 0), model Z -> (nx, 0, nz) = n,
            // the columns of the console's part-0 matrix (rows above).
            const float cols[3][3] = {{nz, 0.0f, -nx}, {0.0f, 1.0f, 0.0f}, {nx, 0.0f, nz}};
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k) instance.matrix[4 * c + k] = cols[c][k] * kPropScale;
            for (int k = 0; k < 3; ++k) instance.matrix[12 + k] = instance.centre[k];
            instance.matrix[15] = 1.0f;
            instance.cell = static_cast<int>(&cell - cells.data()); // the maximum-detail draw (race_render.cpp)
            props_.push_back(instance);
        }
    recordProps_ = props_; // the PC graphics settings: the maximum-detail draw adds those the pools do not hold
    std::printf("roadside props: %zu placed from DATA\\HAZARD%d.GEO, %zu prop models; %zu of class 0/9/50 "
                "left to their own spawner arms (not model 200)\n",
                props_.size(), hazardSet, propFirst_.size(), notModel200);
}

// The player's machine. Which model the player rides is read out of the captured state rather than
// guessed: the player object at guest `0x801B65D4` has `obj+0x00` pointing at the group-0 `DOD3`
// of the `RMD3` whose `modelId` is 100, and `obj+0x38` at an object whose model is 150 - the
// rider. Both are in `DATA\BBLEVEL1.GEO` for level 1. Their texture ids are the `DOD3+0x1C` of
// their own group 0, and the sub-meshes whose first primitive has `prim.clut` bit 7 set take the
// shared rim sheet `DATA\RIMA1.TIM` instead. One 128-entry palette serves the whole object.
void RaceScene::LoadMachine(const rr::DiscImage& disc, bool useTextures,
                            const std::vector<rr::PartSlot>& bikeSlots,
                            const std::vector<rr::PartSlot>& riderSlots) {
    const auto bikeFile = disc.Find("DATA/BBLEVEL1.GEO");
    const auto overlay = disc.Find("RASHCDG.BIN");
    const auto bikeTexFile = disc.Find("DATA/BBLEVEL1.TEX");
    const auto rimFile = disc.Find("DATA/RIMA1.TIM");
    if (!bikeFile || !overlay) return;
    const std::vector<rr::Model> machine = rr::ParseGeo(disc.ReadFile(*bikeFile));
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    const rr::Model* bikeModel = nullptr;
    const rr::Model* riderModel = nullptr;
    for (const rr::Model& model : machine) {
        if (model.id == 100) bikeModel = &model;
        if (model.id == 150) riderModel = &model;
    }
    if (bikeModel == nullptr) bikeModel = &machine.front();

    // The three sheets, in the order a run's `sheet` indexes them.
    if (useTextures && bikeTexFile) {
        const std::vector<uint8_t> tex = disc.ReadFile(*bikeTexFile);
        const uint32_t bikeTexId = bikeModel->groups.front().slot;
        riderSheets_.push_back(
            rr::BuildSheetWithBank(tex, static_cast<uint16_t>(bikeTexId), rr::kPlayerBikeKnbpBlock));
        if (rimFile)
            riderSheets_.push_back(
                rr::BuildTimSheetWithBank(disc.ReadFile(*rimFile), tex, rr::kPlayerBikeKnbpBlock));
        else
            riderSheets_.push_back(riderSheets_.front());
        // The rider is a second OBJECT and 8bpp palettes belong to the object, so it gets its own:
        // `a1 = 30` -> `TSLP` block 1, not the bike's `KNBP` block 46.
        if (riderModel)
            riderSheets_.push_back(rr::BuildSheetWithBank(tex,
                                                          static_cast<uint16_t>(riderModel->groups.front().slot),
                                                          rr::kPlayerRiderTslpBlock, "TSLP"));
        // The negative control's expectation: the same three sheets with the NEXT block of the same
        // bank, i.e. another racer's colour scheme on identical art.
        riderSheetsAlt_.push_back(
            rr::BuildSheetWithBank(tex, static_cast<uint16_t>(bikeTexId), rr::kPlayerBikeKnbpBlock + 1));
        if (rimFile)
            riderSheetsAlt_.push_back(
                rr::BuildTimSheetWithBank(disc.ReadFile(*rimFile), tex, rr::kPlayerBikeKnbpBlock + 1));
        else
            riderSheetsAlt_.push_back(riderSheetsAlt_.front());
        if (riderModel)
            riderSheetsAlt_.push_back(rr::BuildSheetWithBank(
                tex, static_cast<uint16_t>(riderModel->groups.front().slot), rr::kPlayerRiderTslpBlock + 1,
                "TSLP"));
        std::printf("rider sheets: bike LECT 0x%02X %dx%d KNBP block %d, rim %dx%d, "
                    "rider LECT 0x%02X TSLP block %d\n",
                    bikeTexId, riderSheets_[0].width, riderSheets_[0].height, rr::kPlayerBikeKnbpBlock,
                    riderSheets_[1].width, riderSheets_[1].height,
                    riderModel ? riderModel->groups.front().slot : 0u, rr::kPlayerRiderTslpBlock);
    }

    rr::TriangleSoup machineSoup;
    const auto appendGroup = [&](const rr::ModelGroup& group, int owner, int mainSheet, int rimSheet) {
        const rr::Assembly assembly = rr::AssembleGroup(group, skeleton);
        // With a measured pose, the part rotations come out of the captured slots and the parts
        // are walked again through `PoseGroup`. Slot 0 is left at the identity on purpose: it is
        // the OBJECT's world orientation in the console's frame (`RASHCDG 0x80066DC8` feeds
        // `parts + 4` to the GTE as the object matrix), and our renderer supplies that itself
        // through the model matrix.
        const std::vector<rr::PartSlot>& slots = owner == 0 ? bikeSlots : riderSlots;
        std::vector<rr::PartMatrix> local;
        if (!slots.empty() && slots.size() == group.subMeshes.size()) {
            local.assign(group.subMeshes.size(), rr::PartMatrix{});
            for (size_t i = 1; i < slots.size(); ++i) local[i] = slots[i].rot;
        }
        const rr::TriangleSoup one =
            local.empty() ? rr::BuildAssembledTriangleSoup(group, assembly)
                          : rr::BuildPosedTriangleSoup(group, rr::PoseGroup(group, skeleton, assembly, local));
        const size_t base = machineSoup.vertices.size();
        size_t cursor = 0;
        for (const rr::SubMesh& sub : group.subMeshes) {
            const size_t n = sub.prims.size() * 6;
            if (n != 0) {
                ModelPart part;
                part.first = static_cast<GLint>(base + cursor);
                part.count = static_cast<GLsizei>(n);
                // The engine reads `prim.clut` bit 7 ONCE per sub-mesh - a single `lw` at
                // `0x800257B0` - and it is uniform inside a sub-mesh in 2447 of the game's 2453
                // non-empty sub-meshes, so this reads it the same way.
                part.sheet = (sub.prims.front().clut & 0x0080u) ? rimSheet : mainSheet;
                part.owner = owner;
                part.sub = static_cast<int>(&sub - group.subMeshes.data()); // the head camera (head_camera.h)
                SetPartLight(part, group);
                bikeParts_.push_back(part);
            }
            cursor += n;
        }
        machineSoup.vertices.insert(machineSoup.vertices.end(), one.vertices.begin(), one.vertices.end());
    };
    const rr::ModelGroup& bikeGroup = bikeModel->groups.front();
    appendGroup(bikeGroup, 0, 0, 1);
    const size_t riderFirstVertex = machineSoup.vertices.size();
    if (riderModel && riderSheets_.size() > 2) appendGroup(riderModel->groups.front(), 1, 2, 2);
    else if (riderModel) appendGroup(riderModel->groups.front(), 1, 0, 0);
    if (riderModel) riderPose_ = MakeRiderPoseMesh(riderModel->groups.front(), skeleton, riderFirstVertex);
    bikePose_ = MakeRiderPoseMesh(bikeGroup, skeleton, 0); // the bike's own parts (DrawRequest::bikeLocal)
    // LODs 1..3 of both models (lod_pose_draw.h): each group's rest pose by the program the
    // original's draw picks for its part count, one run per sub-mesh, drawn when the request's LOD asks.
    const auto appendLod = [&](const rr::ModelGroup& group, LodOwner who, int lod, int mainSheet, int rimSheet) {
        const int owner = who == LodOwner::Bike ? 0 : 1;
        const size_t base = machineSoup.vertices.size();
        std::shared_ptr<LodPoseMesh> mesh = MakeLodPoseMesh(group, skeleton, who, base);
        const rr::TriangleSoup one = PosedLodSoup(*mesh, nullptr, 0);
        size_t cursor = 0;
        for (const rr::SubMesh& sub : group.subMeshes) {
            const size_t n = sub.prims.size() * 6;
            if (n != 0) {
                ModelPart part;
                part.first = static_cast<GLint>(base + cursor);
                part.count = static_cast<GLsizei>(n);
                part.sheet = (sub.prims.front().clut & 0x0080u) ? rimSheet : mainSheet;
                part.owner = owner;
                part.lod = lod;
                SetPartLight(part, group);
                bikeParts_.push_back(part);
            }
            cursor += n;
        }
        machineSoup.vertices.insert(machineSoup.vertices.end(), one.vertices.begin(), one.vertices.end());
        (owner == 0 ? bikeLod_ : riderLod_)[lod] = mesh;
    };
    for (int lod = 1; lod < 4; ++lod) {
        if (static_cast<size_t>(lod) < bikeModel->groups.size())
            appendLod(bikeModel->groups[static_cast<size_t>(lod)], LodOwner::Bike, lod, 0, 1);
        if (riderModel && static_cast<size_t>(lod) < riderModel->groups.size())
            appendLod(riderModel->groups[static_cast<size_t>(lod)], LodOwner::Rider, lod, riderSheets_.size() > 2 ? 2 : 0,
                      riderSheets_.size() > 2 ? 2 : 0);
    }

    // Object-to-object attachment, `RASHCDG 0x80066B98` (rmd3.md 9.5): the child's position is the
    // parent's plus the parent's vertex 3 when the parent group has fewer than 6 sub-meshes (the
    // bike has 5), taken from sub-mesh 0's `vertBase`. Sub-mesh 0 of the bike carries four
    // vertices and no polygons at all - it exists only to hold attachment points.
    const size_t attachIndex =
        bikeGroup.subMeshes.front().vertBase + (bikeGroup.subMeshes.size() < 6 ? 3u : 4u);
    if (attachIndex < bikeGroup.verts.size()) {
        const float factor = static_cast<float>(rr::LodFactor(bikeGroup));
        riderAttach_[0] = static_cast<float>(bikeGroup.verts[attachIndex].x) * factor;
        riderAttach_[1] = static_cast<float>(bikeGroup.verts[attachIndex].y) * factor;
        riderAttach_[2] = static_cast<float>(bikeGroup.verts[attachIndex].z) * factor;
    }
    // The seat per bike LOD, as ChildPlace RASHCDG 0x80066B98 (RASHCDG.BIN SHA-1 cfe43a77...) takes it for the
    // LOD LodSelect put the bike at: SeatVertex 0x80066A84 reads the LOD's part count (its DOD3 +24) and gives
    // sub-mesh 0's vertex 2 / 3 / 4 for 3 / 5 / 6 parts, in that LOD's vertices and scale shift; for any other
    // count it answers 0 and ChildPlace falls back to LOD 0's DOD3 (+36 vertices, +14 >> 12 the shift) at
    // the current LOD's sub-mesh-0 base + 3 (LOD 0 under 6 parts) or + 4 (model_draw.cpp, PORTED).
    for (int lod = 0; lod < 4; ++lod) {
        float* out = riderAttachLod_[lod];
        for (int k = 0; k < 3; ++k) out[k] = riderAttach_[k];
        if (lod == 0 || static_cast<size_t>(lod) >= bikeModel->groups.size()) continue;
        const rr::ModelGroup& lg = bikeModel->groups[static_cast<size_t>(lod)];
        if (lg.subMeshes.empty()) continue;
        const size_t n = lg.subMeshes.size();
        const int idx = n == 3 ? 2 : n == 5 ? 3 : n == 6 ? 4 : -1;
        const rr::ModelGroup& from = idx >= 0 ? lg : bikeGroup;
        const size_t at = lg.subMeshes.front().vertBase +
                          static_cast<size_t>(idx >= 0 ? idx : (bikeGroup.subMeshes.size() < 6 ? 3 : 4));
        if (at >= from.verts.size()) continue;
        const float f = static_cast<float>(rr::LodFactor(from));
        out[0] = static_cast<float>(from.verts[at].x) * f;
        out[1] = static_cast<float>(from.verts[at].y) * f;
        out[2] = static_cast<float>(from.verts[at].z) * f;
    }

    BindSoup(bikeVao_, bikeVbo_, machineSoup);
    { // the coplanar layers inside each rigid run of the machine's rest pose (coplanar.h)
        std::vector<std::pair<GLint, GLsizei>> ranges;
        for (const ModelPart& part : bikeParts_) ranges.emplace_back(part.first, part.count);
        machineLayers_.Build(bikeVao_, machineSoup.vertices, ranges, kModelUnitsPerWorldUnit, true);
    }
    LoadShadow(disc, bikeSlots, riderSlots);
    for (const rr::IndexedTexture& sheet : riderSheets_)
        riderSheetTextures_.push_back(UploadIndexedTexture(sheet));
    std::printf("player machine: bike model %u (%zu sub-meshes), rider model %u, "
                "%zu draw runs, %zu vertices, attach vertex (%.0f,%.0f,%.0f); per bike LOD 1..3 (SeatVertex) "
                "(%.0f,%.0f,%.0f) (%.0f,%.0f,%.0f) (%.0f,%.0f,%.0f)\n",
                bikeModel->id, bikeGroup.subMeshes.size(), riderModel ? riderModel->id : 0u,
                bikeParts_.size(), machineSoup.vertices.size(), riderAttach_[0], riderAttach_[1],
                riderAttach_[2], riderAttachLod_[1][0], riderAttachLod_[1][1], riderAttachLod_[1][2],
                riderAttachLod_[2][0], riderAttachLod_[2][1], riderAttachLod_[2][2], riderAttachLod_[3][0],
                riderAttachLod_[3][1], riderAttachLod_[3][2]);
}

void RaceScene::LoadSky(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs) {
    if (const auto exeFile = disc.Find("SLUS_010.53")) {
        try {
            const std::vector<uint8_t> exe = disc.ReadFile(*exeFile);
            skyBook_ = rr::MdecCodebook::FromExe(exe);
            // The EXE's {s16 sin; s16 cos}[4096] table at 0x8005624C: the panorama's column angles
            // are looked up in it (`RASHCDG 0x800644F4`).
            constexpr size_t kSinCos = 0x8005624Cu - 0x80010000u + 0x800u;
            if (exe.size() >= kSinCos + 4096 * 4) {
                sinCos_.resize(8192);
                for (size_t i = 0; i < 8192; ++i)
                    sinCos_[i] = static_cast<int16_t>(exe[kSinCos + i * 2] | (exe[kSinCos + i * 2 + 1] << 8));
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sky: %s\n", e.what());
        }
    }
    if (!skyBook_.Empty()) {
        const size_t scanned = ScanRaceStream(
            disc, set, legs, [](uint8_t type) { return type == static_cast<uint8_t>(rr::ChunkType::Panorama); },
            [&](uint32_t, const std::vector<uint8_t>& chunk) {
                const rr::ChunkHeader header = rr::ParseChunkHeader(chunk);
                // A resource is re-shipped along the stream, so keep one copy per id.
                for (const SkyChunk& had : skyChunks_)
                    if (had.header.id == header.id) return;
                skyChunks_.push_back({header, chunk});
                skyHeaders_.push_back(header);
            });
        std::printf("panorama: %zu type-4 chunks in %zu chunks scanned\n", skyChunks_.size(), scanned);
    }
    if (skyChunks_.empty()) return;

    skyProgram_ = BuildProgram(kSkyVertexShader, kSkyFragmentShader);
    skyViewProjLocation_ = gl.GetUniformLocation(skyProgram_, "uViewProj");
    skyTexLocation_ = gl.GetUniformLocation(skyProgram_, "uSky");
    skySizeLocation_ = gl.GetUniformLocation(skyProgram_, "uSkySize");
    skyDebugLocation_ = gl.GetUniformLocation(skyProgram_, "uDebug");
    skyWorldLocation_ = gl.GetUniformLocation(skyProgram_, "uWorld");

    // A cylinder centred on the camera. The panorama is 110 columns of 16 texels, so the natural
    // reading - and the only one that keeps its texels square - is that the 1760 texels close a
    // full circle and the 128 rows subtend the same angle per texel. Where the seam sits in world
    // yaw is NOT established: the console derives a start column from the camera, and this takes
    // world +Z as column 0. The panorama's bottom row sits at eye height, which puts the join
    // between backdrop and ground at the horizon.
    constexpr float kSkyRadius = 8000.0f;
    const float skyHeight = static_cast<float>(rr::Panorama::kHeight) * 2.0f * 3.14159265f * kSkyRadius /
                            static_cast<float>(rr::Panorama::kWidth);
    std::vector<float> ring; // x, y, z, u, v
    const int segments = rr::Panorama::kColumns * 2;
    const auto push = [&](int segment, float texelV, float y) {
        const float t = static_cast<float>(segment) / static_cast<float>(segments);
        const float angle = t * 2.0f * 3.14159265f;
        ring.push_back(std::sin(angle) * kSkyRadius);
        ring.push_back(y);
        ring.push_back(std::cos(angle) * kSkyRadius);
        ring.push_back(t * static_cast<float>(rr::Panorama::kWidth));
        ring.push_back(texelV);
    };
    for (int s = 0; s < segments; ++s) {
        // PS1 space is y-down, so the top of the backdrop is at negative y.
        push(s, 0.0f, -skyHeight);
        push(s + 1, 0.0f, -skyHeight);
        push(s, static_cast<float>(rr::Panorama::kHeight), 0.0f);
        push(s + 1, 0.0f, -skyHeight);
        push(s + 1, static_cast<float>(rr::Panorama::kHeight), 0.0f);
        push(s, static_cast<float>(rr::Panorama::kHeight), 0.0f);
    }
    skyVertexCount_ = static_cast<GLsizei>(ring.size() / 5);
    gl.GenVertexArrays(1, &skyVao_);
    gl.BindVertexArray(skyVao_);
    gl.GenBuffers(1, &skyVbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, skyVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ring.size() * sizeof(float)), ring.data(),
                  GL_STATIC_DRAW);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)),
                           reinterpret_cast<void*>(0));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)),
                           reinterpret_cast<void*>(sizeof(float) * 3));
    gl.EnableVertexAttribArray(1);
    glGenTextures(1, &skyTexture_);
}

void RaceScene::BindSky(size_t which) {
    if (which >= skyChunks_.size() || which == skyBound_) return;
    try {
        skyImage_ = rr::ParsePanoramaChunk(skyChunks_[which].bytes, skyBook_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "panorama %zu: %s\n", which, e.what());
        return;
    }
    skyBound_ = which;
    RebuildSkyMesh();
    glBindTexture(GL_TEXTURE_2D, skyTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rr::Panorama::kWidth, rr::Panorama::kHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, skyImage_.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::printf("panorama bound: id %u, road %u, distance %u, %d tiles of 16x16, %zu refs\n",
                skyChunks_[which].header.id, skyImage_.road, skyImage_.distance, skyImage_.tiles,
                skyImage_.refs.size());
}

// The panorama as `RASHCDG 0x800644F4` places it:
//  * column c spans the ring angles (c * 0x94F) >> 6 and ((c + 1) * 0x94F) >> 6 of 4096 - 110
//    columns closing the circle - at x = sin, z = cos of the EXE's table, radius 4096;
//  * rows are 279 units apart, row r at y = 279 r - 0x8B8, so row 8 is eye height;
//  * column c's tiles (its non-empty STEN bands, top to bottom) stack from row
//    OFFS[c] + (its first non-empty band) down, one row each;
//  * each tile is drawn ROTATED: the packets of the rr-race frame carry u running 15 -> 0 down the
//    quad and v 0 -> 15 across it, i.e. screen x is the decoded tile's row and screen y its column
//    reversed;
//  * TR = 0, RT = the camera matrix (0x80064B9C), so only the camera's rotation moves it; drawn
//    textured and modulated by the colour at 0x8005237C (0x808080, neutral, in all four captures).
void RaceScene::RebuildSkyMesh() {
    if (skyVbo_ == 0 || sinCos_.size() < 8192) return;
    constexpr float kScale = 8000.0f / 4096.0f; // the panorama's radius in world units, as before
    const auto ringPoint = [&](int column, float out[2]) {
        const uint32_t angle = (static_cast<uint32_t>(column) * 0x94Fu >> 6) & 0xFFFu;
        out[0] = static_cast<float>(sinCos_[angle * 2]) * kScale;
        out[1] = static_cast<float>(sinCos_[angle * 2 + 1]) * kScale;
    };
    std::vector<float> mesh; // x, y, z, u, v
    skyQuads_.clear();
    SkyQuad quad;
    int corner = 0;
    const auto push = [&](const float p[2], int row, float u, float v) {
        mesh.push_back(p[0]);
        mesh.push_back(static_cast<float>(279 * row - 0x8B8) * kScale);
        mesh.push_back(p[1]);
        mesh.push_back(u);
        mesh.push_back(v);
        if (corner < 4) {
            // The first four pushes of a tile are its corners in GPU order.
            quad.dir[corner][0] = p[0] / kScale;
            quad.dir[corner][1] = static_cast<float>(279 * row - 0x8B8);
            quad.dir[corner][2] = p[1] / kScale;
            quad.tex[corner][0] = u;
            quad.tex[corner][1] = v;
        }
        ++corner;
    };
    for (int c = 0; c < rr::Panorama::kColumns; ++c) {
        float left[2], right[2];
        ringPoint(c, left);
        ringPoint(c + 1, right);
        int first = -1, k = 0;
        for (int band = 0; band < rr::Panorama::kBands; ++band) {
            if (skyImage_.code[static_cast<size_t>(c) * rr::Panorama::kBands + static_cast<size_t>(band)] == 0) continue;
            if (first < 0) first = band;
            const int offs = (skyImage_.haveOffs && skyMutation_ != 2) ? skyImage_.offs[c] : 0;
            const int row = offs + first + k;
            ++k;
            quad.column = c;
            quad.band = band;
            quad.row = row;
            corner = 0;
            // Texel space of the assembled image: the tile's column origin x0, row origin y0.
            const float x0 = static_cast<float>(c * rr::Panorama::kTile);
            const float y0 = static_cast<float>(band * rr::Panorama::kTile);
            const float t = static_cast<float>(rr::Panorama::kTile);
            // Corners in the GPU's order: top-left, top-right, bottom-left, bottom-right.
            if (skyMutation_ == 1) {
                // The negative control: the tile as the image stores it, not turned.
                push(left, row, x0, y0);
                push(right, row, x0 + t, y0);
                push(left, row + 1, x0, y0 + t);
                push(right, row + 1, x0 + t, y0 + t);
                push(right, row + 1, x0 + t, y0 + t);
                push(left, row + 1, x0, y0 + t);
                // (mesh order for the mutation: TL TR BL / BR BR BL is not a pair of triangles,
                // so rewrite the six as (TL, TR, BL), (TR, BR, BL).)
                const size_t base = mesh.size() - 6 * 5;
                const float tr[5] = {mesh[base + 5], mesh[base + 6], mesh[base + 7], mesh[base + 8], mesh[base + 9]};
                for (int k2 = 0; k2 < 5; ++k2) mesh[base + 15 + static_cast<size_t>(k2)] = tr[k2];
            } else {
                push(left, row, x0 + t, y0);      // u 15, v 0
                push(right, row, x0 + t, y0 + t); // u 15, v 15
                push(left, row + 1, x0, y0);      // u 0,  v 0
                push(right, row + 1, x0, y0 + t); // u 0,  v 15  (the fourth corner)
                // The second triangle (TR, BR, BL): move the fourth corner behind a copy of TR.
                const size_t base = mesh.size() - 4 * 5;
                float br[5], tr[5], bl[5];
                for (int k2 = 0; k2 < 5; ++k2) {
                    tr[k2] = mesh[base + 5 + static_cast<size_t>(k2)];
                    bl[k2] = mesh[base + 10 + static_cast<size_t>(k2)];
                    br[k2] = mesh[base + 15 + static_cast<size_t>(k2)];
                }
                for (int k2 = 0; k2 < 5; ++k2) mesh[base + 15 + static_cast<size_t>(k2)] = tr[k2];
                for (float f : br) mesh.push_back(f);
                for (float f : bl) mesh.push_back(f);
            }
            skyQuads_.push_back(quad);
        }
    }
    skyVertexCount_ = static_cast<GLsizei>(mesh.size() / 5);
    gl.BindBuffer(GL_ARRAY_BUFFER, skyVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.size() * sizeof(float)), mesh.data(), GL_STATIC_DRAW);
    RebuildSkyBand(); // the same tiles as one continuous band (DrawRequest::worldSky)
}

// OURS (DrawRequest::worldSky): the tiles RebuildSkyMesh places - column c, grid row
// offs[c] + first band + k, turned (screen x = the tile's row y, screen y = 15 - its pixel x) - copied into one image
// laid out as the sky shows them, and a grid of quads (one per placed tile, corners shared with every neighbour) over
// it. The console's per-tile quads read the decoded image, where a tile's neighbour is the NEXT band of its column
// (drawn above or below it, or not at all): a headset's MSAA / filtering at a tile's edge read that texel, and the
// seams between the tiles showed. Here the texel past an edge is the one drawn there, so filtering is seamless too.
void RaceScene::RebuildSkyBand() {
    skyBandVertexCount_ = 0;
    if (skyVao_ == 0 || sinCos_.size() < 8192 || skyImage_.rgba.size() < static_cast<size_t>(rr::Panorama::kWidth) *
                                                                         rr::Panorama::kHeight * 4)
        return;
    constexpr int kTile = rr::Panorama::kTile;
    struct Placed {
        int column, band, row;
    };
    std::vector<Placed> placed;
    int firstRow = 1 << 20, lastRow = -(1 << 20);
    for (int c = 0; c < rr::Panorama::kColumns; ++c) {
        int first = -1, k = 0;
        for (int band = 0; band < rr::Panorama::kBands; ++band) {
            if (skyImage_.code[static_cast<size_t>(c) * rr::Panorama::kBands + static_cast<size_t>(band)] == 0) continue;
            if (first < 0) first = band;
            const int offs = (skyImage_.haveOffs && skyMutation_ != 2) ? skyImage_.offs[c] : 0;
            const int row = offs + first + k;
            ++k;
            placed.push_back({c, band, row});
            firstRow = std::min(firstRow, row);
            lastRow = std::max(lastRow, row);
        }
    }
    if (placed.empty()) return;
    skyBandWidth_ = rr::Panorama::kWidth;
    skyBandHeight_ = (lastRow - firstRow + 1) * kTile;
    std::vector<uint8_t> band(static_cast<size_t>(skyBandWidth_) * static_cast<size_t>(skyBandHeight_) * 4, 0);
    constexpr float kScale = 8000.0f / 4096.0f; // RebuildSkyMesh's radius
    std::vector<float> mesh; // x, y, z, u, v: the sky mesh's layout
    mesh.reserve(placed.size() * 30);
    const auto ringPoint = [&](int column, float out[2]) {
        const uint32_t angle = (static_cast<uint32_t>(column) * 0x94Fu >> 6) & 0xFFFu;
        out[0] = static_cast<float>(sinCos_[angle * 2]) * kScale;
        out[1] = static_cast<float>(sinCos_[angle * 2 + 1]) * kScale;
    };
    for (const Placed& t : placed) {
        const int bandRow = t.row - firstRow;
        for (int y = 0; y < kTile; ++y)
            for (int x = 0; x < kTile; ++x) {
                const size_t src = (static_cast<size_t>(t.band * kTile + y) * rr::Panorama::kWidth +
                                    static_cast<size_t>(t.column * kTile + x)) * 4;
                const size_t dst = (static_cast<size_t>(bandRow * kTile + (kTile - 1 - x)) * static_cast<size_t>(skyBandWidth_) +
                                    static_cast<size_t>(t.column * kTile + y)) * 4;
                std::memcpy(&band[dst], &skyImage_.rgba[src], 4);
            }
        float left[2], right[2];
        ringPoint(t.column, left);
        ringPoint(t.column + 1, right);
        const float top = static_cast<float>(279 * t.row - 0x8B8) * kScale;
        const float bottom = static_cast<float>(279 * (t.row + 1) - 0x8B8) * kScale;
        const float u0 = static_cast<float>(t.column * kTile), u1 = u0 + kTile;
        const float v0 = static_cast<float>(bandRow * kTile), v1 = v0 + kTile;
        const float corner[4][5] = {{left[0], top, left[1], u0, v0},
                                    {right[0], top, right[1], u1, v0},
                                    {left[0], bottom, left[1], u0, v1},
                                    {right[0], bottom, right[1], u1, v1}};
        static constexpr int kOrder[6] = {0, 1, 2, 1, 3, 2};
        for (int i : kOrder) mesh.insert(mesh.end(), corner[i], corner[i] + 5);
    }
    if (skyBandVao_ == 0) {
        gl.GenVertexArrays(1, &skyBandVao_);
        gl.BindVertexArray(skyBandVao_);
        gl.GenBuffers(1, &skyBandVbo_);
        gl.BindBuffer(GL_ARRAY_BUFFER, skyBandVbo_);
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)), reinterpret_cast<void*>(0));
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)),
                               reinterpret_cast<void*>(sizeof(float) * 3));
        gl.EnableVertexAttribArray(1);
        gl.BindVertexArray(0);
        glGenTextures(1, &skyBandTexture_);
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, skyBandVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.size() * sizeof(float)), mesh.data(), GL_STATIC_DRAW);
    skyBandVertexCount_ = static_cast<GLsizei>(mesh.size() / 5);
    glBindTexture(GL_TEXTURE_2D, skyBandTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, skyBandWidth_, skyBandHeight_, 0, GL_RGBA, GL_UNSIGNED_BYTE, band.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); // the cylinder closes: column 109 meets column 0
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    skyBandFilter_ = -1; // set by the draw (the smooth-textures setting)
}

bool RaceScene::SelectSkyId(uint32_t id) {
    for (size_t n = 0; n < skyChunks_.size(); ++n)
        if (skyChunks_[n].header.id == id) {
            BindSky(n);
            return true;
        }
    return false;
}

void RaceScene::SelectSky(bool roadKnown, uint16_t road, uint16_t roadDistance) {
    if (skyChunks_.empty()) return;
    size_t pick = 0;
    if (roadKnown) {
        int32_t bestError = INT32_MAX;
        for (size_t n = 0; n < skyChunks_.size(); ++n)
            for (const rr::ResidencyWindow& residency : skyChunks_[n].header.windows) {
                if (residency.road != road) continue;
                const int32_t middle =
                    (static_cast<int32_t>(residency.from) + static_cast<int32_t>(residency.to)) / 2;
                const int32_t error = std::abs(middle - static_cast<int32_t>(roadDistance));
                if (error < bestError) {
                    bestError = error;
                    pick = n;
                }
            }
    }
    BindSky(pick);
}

// The cloud ring, rrformats/sky_clouds.h. Everything but the texels is rebuilt the way the loader
// builds it, so the geometry is the console's own: 25 column pairs at angle 170 i of 4096 out of
// the EXE's sine table, radius 4096, from the section's top y down by its height.
void RaceScene::LoadCloudRing(const rr::DiscImage& disc) {
    if (!skyClouds_.valid) return;
    const auto exeFile = disc.Find("SLUS_010.53");
    if (!exeFile) return;
    const std::vector<uint8_t> exe = disc.ReadFile(*exeFile);
    constexpr size_t kSinCos = 0x8005624Cu - 0x80010000u + 0x800u; // {s16 sin; s16 cos}[4096]
    if (exe.size() < kSinCos + 4096 * 4) return;
    const auto s16 = [&](size_t at) { return static_cast<int16_t>(exe[at] | (exe[at + 1] << 8)); };
    cloudRing_.clear();
    for (int i = 0; i <= rr::SkyClouds::kSegments; ++i) {
        const size_t e = kSinCos + static_cast<size_t>(rr::SkyCloudAngle(i)) * 4;
        cloudRing_.push_back({s16(e), s16(e + 2)});
    }
    // The ring as GL draws it: world directions scaled out to the panorama's radius (depth test is
    // off, only the direction matters - TR is 0 in the original), two triangles per segment in the
    // GPU's own split of a quad, (0, 1, 2) and (1, 3, 2).
    constexpr float kScale = 8000.0f / static_cast<float>(rr::SkyClouds::kRadius);
    std::vector<float> ring; // x, y, z, u, v
    cloudQuads_.clear();
    for (int j = 0; j < rr::SkyClouds::kSegments; ++j) {
        // Negative controls (SetSkyMutation): 3 puts segment j where segment j+1 belongs, 4 gives it
        // the texels of slice j+1.
        const rr::SkyCloudSlice slice = rr::SkyCloudSliceOf(skyMutation_ == 4 ? j + 1 : j);
        const int shift = skyMutation_ == 3 ? 1 : 0;
        SkyQuad quad;
        quad.column = j;
        int cornerIndex = 0;
        const auto corner = [&](int point, bool bottom, float u, float v) {
            point += shift;
            if (point > rr::SkyClouds::kSegments) point -= rr::SkyClouds::kSegments; // point 24 is point 0
            const std::array<int32_t, 2>& p = cloudRing_[static_cast<size_t>(point)];
            if (cornerIndex < 4) {
                const int slot = cornerIndex == 3 ? -1 : cornerIndex; // pushes: TL, TR, BL, (TR), BR, BL
                if (slot >= 0) {
                    quad.dir[slot][0] = static_cast<float>(p[0]);
                    quad.dir[slot][1] = static_cast<float>(skyClouds_.top + (bottom ? skyClouds_.height : 0));
                    quad.dir[slot][2] = static_cast<float>(p[1]);
                    quad.tex[slot][0] = u;
                    quad.tex[slot][1] = v;
                }
            } else if (cornerIndex == 4) {
                quad.dir[3][0] = static_cast<float>(p[0]);
                quad.dir[3][1] = static_cast<float>(skyClouds_.top + (bottom ? skyClouds_.height : 0));
                quad.dir[3][2] = static_cast<float>(p[1]);
                quad.tex[3][0] = u;
                quad.tex[3][1] = v;
            }
            ++cornerIndex;
            ring.push_back(static_cast<float>(p[0]) * kScale);
            ring.push_back(static_cast<float>(skyClouds_.top + (bottom ? skyClouds_.height : 0)) * kScale);
            ring.push_back(static_cast<float>(p[1]) * kScale);
            ring.push_back(u);
            ring.push_back(v);
        };
        const float u0 = static_cast<float>(slice.u0), u1 = static_cast<float>(slice.u1);
        const float v0 = static_cast<float>(slice.v0), v1 = static_cast<float>(slice.v1);
        corner(j, false, u0, v0);
        corner(j + 1, false, u1, v0);
        corner(j, true, u0, v1);
        corner(j + 1, false, u1, v0);
        corner(j + 1, true, u1, v1);
        corner(j, true, u0, v1);
        cloudQuads_.push_back(quad);
    }
    cloudProgram_ = BuildProgram(kCloudVertexShader, kCloudFragmentShader);
    cloudViewProjLocation_ = gl.GetUniformLocation(cloudProgram_, "uViewProj");
    cloudTexLocation_ = gl.GetUniformLocation(cloudProgram_, "uClouds");
    cloudDebugLocation_ = gl.GetUniformLocation(cloudProgram_, "uDebug");
    cloudWorldLocation_ = gl.GetUniformLocation(cloudProgram_, "uWorld");
    cloudRectLocation_ = gl.GetUniformLocation(cloudProgram_, "uRect");
    gl.GenVertexArrays(1, &cloudVao_);
    gl.BindVertexArray(cloudVao_);
    gl.GenBuffers(1, &cloudVbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, cloudVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ring.size() * sizeof(float)), ring.data(), GL_STATIC_DRAW);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)), reinterpret_cast<void*>(0));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * static_cast<GLsizei>(sizeof(float)),
                           reinterpret_cast<void*>(sizeof(float) * 3));
    gl.EnableVertexAttribArray(1);
    // The texels through the section's own CLUT; alpha carries the STP rule (shaders.cpp).
    std::vector<uint8_t> rgba(static_cast<size_t>(skyClouds_.width) * static_cast<size_t>(skyClouds_.rows) * 4, 0);
    for (size_t i = 0; i < skyClouds_.indices.size(); ++i) {
        const uint16_t colour = skyClouds_.clut[skyClouds_.indices[i] & 0xF];
        const uint32_t c = rr::Bgr555ToRgba(colour);
        rgba[i * 4 + 0] = static_cast<uint8_t>(c);
        rgba[i * 4 + 1] = static_cast<uint8_t>(c >> 8);
        rgba[i * 4 + 2] = static_cast<uint8_t>(c >> 16);
        rgba[i * 4 + 3] = colour == 0 ? 0 : ((colour & 0x8000) ? 128 : 255);
    }
    glGenTextures(1, &cloudTexture_);
    glBindTexture(GL_TEXTURE_2D, cloudTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, skyClouds_.width, skyClouds_.rows, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    std::printf("sky clouds: GAMEBIN1.DAT bundle %zu section 3, %dx%d 4bpp, ring top %d height %d\n",
                skyClouds_.bundle, skyClouds_.width, skyClouds_.rows, skyClouds_.top, skyClouds_.height);
}

// The type-4 panorama is the SKYLINE, not the sky: in `rr-race` it is empty in bands 0..3 in all
// 110 columns. What fills the upper screen is a Gouraud quad the original emits before anything
// else, and its four colours are a 16-byte field of the level's bundle in `DATA\GAMEBIN1.DAT`.
// The geometry here is OUR horizon - a screen-space strip from the top of the viewport down to the
// line the camera's own horizontal plane projects to - not the original's, whose horizon line
// comes from a routine that has not been reversed; the colours are the disc's.
void RaceScene::LoadSkyGradient(const rr::DiscImage& disc, size_t bundle) {
    if (const auto gamebin = disc.Find("DATA/GAMEBIN1.DAT")) {
        try {
            const std::vector<uint8_t> bytes = disc.ReadFile(*gamebin);
            // The field is the payload of the bundle's type-1 section (level_bundle.h), found through
            // the file's own table - the fixed stride is wrong from bundle 12 on.
            const auto skyOf = [&](size_t index) {
                const rr::LevelBundle level = rr::ParseLevelBundle(bytes, index);
                const rr::LevelBundleSection* section = level.Find(1);
                if (!section) throw std::runtime_error("bundle has no type-1 section");
                return rr::ParseSkyGradientAt(bytes, section->payload, index);
            };
            skyGradient_ = skyOf(bundle);
            // The level's sun, section type 2 of the same bundle (RASHCDI 0x8006250C).
            try {
                const rr::LevelSun sun = rr::ParseLevelSun(bytes, rr::ParseLevelBundle(bytes, bundle));
                sunYaw_ = sun.yaw;
                sunElevation_ = sun.elevation;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "sky sun: %s\n", e.what());
            }
            // The cloud layer, section type 3 of the same bundle (rrformats/sky_clouds.h).
            try {
                skyClouds_ = rr::ParseSkyClouds(bytes, rr::ParseLevelBundle(bytes, bundle));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "sky clouds: %s\n", e.what());
            }
            // The negative control's expectation: the NEXT level bundle's sky, i.e. the same four
            // slots filled from a different course.
            // Two bundles can carry the same sky (races 1 and 19 do), and a table entry past the last
            // race is not a bundle at all; the control needs a real and DIFFERENT one.
            const size_t bundles = rr::LevelBundleCount(bytes);
            skyGradientAlt_ = rr::SkyGradient{};
            for (size_t step = 1; step < bundles; ++step) {
                try {
                    skyGradientAlt_ = skyOf((bundle + step) % bundles);
                } catch (const std::exception&) {
                    continue;
                }
                if (skyGradientAlt_.valid &&
                    !std::equal(&skyGradientAlt_.rgb[0][0], &skyGradientAlt_.rgb[0][0] + 12, &skyGradient_.rgb[0][0]))
                    break;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sky gradient: %s\n", e.what());
        }
    }
    LoadCloudRing(disc);
    if (!skyGradient_.valid) return;
    gradProgram_ = BuildProgram(kGradVertexShader, kGradFragmentShader);
    gradTopLocation_ = gl.GetUniformLocation(gradProgram_, "uTop");
    gradHorizonLocation_ = gl.GetUniformLocation(gradProgram_, "uHorizon");
    gradMidLeftLocation_ = gl.GetUniformLocation(gradProgram_, "uMidLeft");
    gradMidRightLocation_ = gl.GetUniformLocation(gradProgram_, "uMidRight");
    gradDebugLocation_ = gl.GetUniformLocation(gradProgram_, "uDebug");
    gl.GenVertexArrays(1, &gradVao_);
    gl.BindVertexArray(gradVao_);
    gl.GenBuffers(1, &gradVbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, gradVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(6 * 4 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * static_cast<GLsizei>(sizeof(float)),
                           reinterpret_cast<void*>(0));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * static_cast<GLsizei>(sizeof(float)),
                           reinterpret_cast<void*>(sizeof(float) * 2));
    gl.EnableVertexAttribArray(1);
    std::printf("sky gradient: GAMEBIN1.DAT bundle %zu, top %02X%02X%02X horizon %02X%02X%02X "
                "blend %02X%02X%02X..%02X%02X%02X\n",
                skyGradient_.bundle, skyGradient_.rgb[0][0], skyGradient_.rgb[0][1], skyGradient_.rgb[0][2],
                skyGradient_.rgb[1][0], skyGradient_.rgb[1][1], skyGradient_.rgb[1][2],
                skyGradient_.rgb[2][0], skyGradient_.rgb[2][1], skyGradient_.rgb[2][2],
                skyGradient_.rgb[3][0], skyGradient_.rgb[3][1], skyGradient_.rgb[3][2]);
}

void RaceScene::PrepareSkyGradient(const Mat4& viewProj, const float eye[3], const float target[3],
                                   int32_t sunAngle) {
    gradVertexCount_ = 0;
    gradSunAngle_ = sunAngle; // the world sky dome's colours (DrawSkyDome)
    // The camera's yaw in the console's unit (4096 per turn): atan2(forward.x, forward.z), which is
    // what the view record keeps at +0x7C (rr-race: 2024 there, 2024 from its camera matrix's third
    // row). The cloud ring and the panorama both start drawing from it.
    {
        const double fx = target[0] - eye[0], fz = target[2] - eye[2];
        const double turn = 4096.0 / (2.0 * 3.14159265358979323846);
        cameraYaw_ = static_cast<int32_t>(std::lround(std::atan2(fx, fz) * turn)) & 0xFFF;
        cloudFirst_ = rr::SkyCloudFirstSegment(cameraYaw_);
    }
    if (gradProgram_ == 0 || !skyGradient_.valid) return;
    // `viewProj` is column-major, so a direction (w = 0) transforms as clip = m[col*4 + row] * d.
    float forward[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    forward[1] = 0.0f;
    float length = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (length < 1e-6f) {
        forward[0] = 0.0f;
        forward[2] = 1.0f;
        length = 1.0f;
    }
    forward[0] /= length;
    forward[2] /= length;
    const auto project = [&](const float direction[3], float out[2]) {
        float clip[4] = {0, 0, 0, 0};
        for (int row = 0; row < 4; ++row)
            clip[row] = viewProj.m[0 * 4 + row] * direction[0] + viewProj.m[1 * 4 + row] * direction[1] +
                        viewProj.m[2 * 4 + row] * direction[2];
        const float w = std::fabs(clip[3]) < 1e-9f ? 1e-9f : clip[3];
        out[0] = clip[0] / w;
        out[1] = clip[1] / w;
    };
    // The original's two lines, `RASHCDG 0x80063C5C` (single-player path, game_state+0x30 == 1,
    // flag 0x8005AD24 clear - as in all four captures):
    //  * the HORIZON: RTPS of (sin yaw, 0, cos yaw) (0x80063C9C), and the band's bottom edge is that
    //    point's screen y plus 10 rows (0x800640B8..0x800640E8), a horizontal line across the screen -
    //    the four corners at 0x800523D0 read (0,0) (384,0) (384,h) (0,h), h = 91..94, in all captures;
    //  * the MIDDLE row, where the two blended colours sit: RTPS of (sin a, -sin e, cos a) with a =
    //    yaw - 0x1AA and e the per-level sun elevation at 0x8005238C (0x80063D7C..0x80063EC4, the
    //    sun-off-screen path). Its screen y is the lower edge of the upper quad (0x8006422C).
    // e is the level's (0x8005238C, the bundle's type-2 section: level_bundle.h ParseLevelSun) - 123 in all
    // four captures, and 123 from race 1/4's bundle.
    const int32_t kSunElevation = sunElevation_;
    const double unit = 2.0 * 3.14159265358979323846 / 4096.0;
    const int32_t yaw16 = cameraYaw_;
    float horizonDir[3] = {static_cast<float>(std::sin(yaw16 * unit)), 0.0f, static_cast<float>(std::cos(yaw16 * unit))};
    const int32_t a16 = yaw16 - 0x1AA;
    float midDir[3] = {static_cast<float>(std::sin(a16 * unit)), static_cast<float>(-std::sin(kSunElevation * unit)),
                       static_cast<float>(std::cos(a16 * unit))};
    float ph[2], pm[2];
    project(horizonDir, ph);
    project(midDir, pm);
    // 10 console rows of the 240-row frame, in NDC.
    const float horizonNdc = std::max(-8.0f, std::min(8.0f, ph[1] - 10.0f * 2.0f / consoleLines_));
    const float midNdc = std::max(horizonNdc, std::min(1.0f, pm[1]));

    // The blend weights. `RASHCDG 0x80063CC4` measures the yaw from a per-level sun angle and takes
    // the two screen-edge yaws +-426 of 4096 either side; which edge gets which sign is NOT
    // measured, so the assignment here is arbitrary and only decides which way round the middle
    // band's hue runs.
    const int32_t yaw = cameraYaw_ - sunAngle;
    rr::SkyBlendColour(skyGradient_, rr::SkyBlendWeight(yaw - rr::kSkyEdgeAngle), gradMidLeft_);
    rr::SkyBlendColour(skyGradient_, rr::SkyBlendWeight(yaw + rr::kSkyEdgeAngle), gradMidRight_);

    // Two quads stacked between the top of the viewport and the horizon, which is the shape the
    // 2-quad path of `RASHCDG 0x8006422C` emits: A across the top, the blend across the middle, B
    // along the horizon.
    struct GradVertex {
        float x, y, row, side;
    };
    const auto corner = [&](int rowIndex, int sideIndex) {
        const float row = static_cast<float>(rowIndex) * 0.5f;
        GradVertex v;
        v.x = sideIndex == 0 ? -1.0f : 1.0f;
        v.y = rowIndex == 0 ? 1.0f : (rowIndex == 1 ? midNdc : horizonNdc);
        v.row = row;
        v.side = static_cast<float>(sideIndex);
        return v;
    };
    std::vector<GradVertex> strip;
    for (int band = 0; band < 2; ++band) {
        const GradVertex tl = corner(band, 0), tr = corner(band, 1);
        const GradVertex bl = corner(band + 1, 0), br = corner(band + 1, 1);
        strip.push_back(tl);
        strip.push_back(tr);
        strip.push_back(bl);
        strip.push_back(tr);
        strip.push_back(br);
        strip.push_back(bl);
    }
    gradVertexCount_ = static_cast<GLsizei>(strip.size());
    gl.BindBuffer(GL_ARRAY_BUFFER, gradVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(strip.size() * sizeof(GradVertex)), strip.data(),
                  GL_DYNAMIC_DRAW);
}

// The whole frame, so a reporting pass can draw the SAME frame again with the subject reporting
// what it sampled instead of what it looks like. Drawing the identical scene in the identical order
// is what makes the two readbacks comparable: depth ties resolve the same way in every pass, so the
// same fragment wins every time.
void RaceScene::Draw(const DrawRequest& request) {
    const int subjectDebug = request.debugMode;
    const int otherDebug = request.debugMode == 6 ? 6 : (request.debugMode != 0 ? 4 : 0);
    const auto forSubject = [&](Subject s) { return request.subject == s ? subjectDebug : otherDebug; };
    const Mat4 identity;
    stats_ = FrameStats{};         // the PC graphics settings (race_scene_pc.cpp): what this view draws and culls
    PcFrustum(request.cullViewProj != nullptr ? *request.cullViewProj : request.viewProj);
    { // the clip z of the eye (its x, y, w are 0): row 2 of viewProj at the eye - the decal passes' pull,
        // summed in double (the terms run to thousands)
        const float* m = request.viewProj.m;
        eyeClipZ_ = static_cast<float>(static_cast<double>(m[2]) * request.eye[0] + static_cast<double>(m[6]) * request.eye[1] +
                                       static_cast<double>(m[10]) * request.eye[2] + static_cast<double>(m[14]));
        for (int k = 0; k < 3; ++k) eyeWorld_[k] = request.eye[k];
        // camera-relative transforms with the depth buffer (multiview.h SetRenderOrigin): the scene program's vertices
        // relative to the eye; the ordering-table order keeps the absolute ones (the console's frame, byte for byte)
        // DEVELOPMENT: RRJB_ORIGIN=off - the absolute transforms with the depth buffer too (the control)
        static const bool originOff = std::getenv("RRJB_ORIGIN") != nullptr && std::string(std::getenv("RRJB_ORIGIN")) == "off";
        SetRenderOrigin(otOrder_ || originOff ? nullptr : request.eye);
    }

    if (request.debugMode != 0) {
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    // The shadows' mask bit (DrawShadow): clear once per frame, so every bike's shadow darkens a
    // pixel at most once, as the GPU's mask bit does.
    glClear(GL_STENCIL_BUFFER_BIT);
    shadowWorld_.clear();
    if (request.haveCellView) lastCellView_ = request.cellView;
    // The sky gradient before even the backdrop, which is the order the original uses: orders 0..3
    // of the captured draw stream are this quad and order 4 the first panorama strip. Same
    // exclusion from the coverage (5) and silhouette (6) passes as the backdrop.
    // sky_gpu.h: the PORTED gradient's own POLY_G4s replace this one
    // OURS: a camera that is not the console's lays the gradient on a dome in world directions (DrawSkyDome)
    if (request.worldSky && request.debugMode == 0) DrawSkyDome(request);
    if (!request.worldSky && gradProgram_ != 0 && gradVertexCount_ > 0 && request.debugMode != 5 && request.debugMode != 6 &&
        !(request.debugMode == 0 && SkyGpuHasGradient())) {
        gl.UseProgram(gradProgram_);
        const auto colour = [](const uint8_t rgb[3], float out[3]) {
            for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(rgb[k]) / 255.0f;
        };
        float top[3], horizon[3], midLeft[3], midRight[3];
        colour(skyGradient_.rgb[0], top);
        colour(skyGradient_.rgb[1], horizon);
        colour(gradMidLeft_, midLeft);
        colour(gradMidRight_, midRight);
        gl.Uniform3f(gradTopLocation_, top[0], top[1], top[2]);
        gl.Uniform3f(gradHorizonLocation_, horizon[0], horizon[1], horizon[2]);
        gl.Uniform3f(gradMidLeftLocation_, midLeft[0], midLeft[1], midLeft[2]);
        gl.Uniform3f(gradMidRightLocation_, midRight[0], midRight[1], midRight[2]);
        gl.Uniform1i(gradDebugLocation_, forSubject(Subject::Gradient));
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        gl.BindVertexArray(gradVao_);
        glDrawArrays(GL_TRIANGLES, 0, gradVertexCount_);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }
    // The cloud ring over the gradient and under the panorama: the capture's draw stream has the two
    // gradient quads, then the six cloud segments (code 0x2F), then the panorama tiles.
    // sky_gpu.h: the original's own cloud and panorama packets, else the renderer's ring and cylinder
    const bool skyPorted = request.debugMode != 5 && request.debugMode != 6 && SkyGpuDraw(request);
    if (!skyPorted && request.debugMode != 5 && request.debugMode != 6) DrawClouds(request, forSubject(Subject::Clouds));
    // The backdrop next, behind everything else: depth test and depth writes off, so the world
    // paints over it wherever the world covers a pixel. It is deliberately left OUT of the coverage
    // (5) and silhouette (6) passes - those measure how much of the frame the WORLD covers, and a
    // backdrop that fills the frame would drive the hole count to zero without filling a hole.
    if (!skyPorted && skyVertexCount_ > 0 && skyBound_ != kNoSky && request.debugMode != 5 && request.debugMode != 6) {
        Mat4 atEye;
        for (int k = 0; k < 3; ++k) atEye.m[12 + k] = request.eye[k];
        gl.UseProgram(skyProgram_);
        UploadViewProj(skyViewProjLocation_, request.viewProj, &atEye); // Multiply(viewProj, atEye); per eye in stereo
        // RebuildSkyBand: a camera that is not the console's draws the continuous band at infinity,
        // filtered as the world's textures are (smooth: bilinear across the tiles' edges; else the nearest texel)
        const bool band = request.worldSky && request.debugMode == 0 && skyBandVertexCount_ > 0;
        gl.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, band ? skyBandTexture_ : skyTexture_);
        if (band && skyBandFilter_ != (pc_.smoothTextures ? 1 : 0)) {
            skyBandFilter_ = pc_.smoothTextures ? 1 : 0;
            const GLint filter = pc_.smoothTextures ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        }
        gl.Uniform1i(skyTexLocation_, 0);
        gl.Uniform2f(skySizeLocation_, static_cast<float>(band ? skyBandWidth_ : rr::Panorama::kWidth),
                     static_cast<float>(band ? skyBandHeight_ : rr::Panorama::kHeight));
        gl.Uniform1i(skyDebugLocation_, forSubject(Subject::Sky));
        gl.Uniform1i(skyWorldLocation_, band ? 1 : 0);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        gl.BindVertexArray(band ? skyBandVao_ : skyVao_);
        glDrawArrays(GL_TRIANGLES, 0, band ? skyBandVertexCount_ : skyVertexCount_);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }

    propsDrawn = 0;
    drawnProps.clear();
    drawnCells.clear();
    gl.UseProgram(program_);
    OtBeginDraw(); // the ordering-table order (race_scene_ot.cpp), or the z-buffer
    gl.Uniform1i(affineLocation_, affine_ ? 1 : 0); // kept by the program for every draw of this frame
    gl.Uniform1i(debugLocation_, 0);
    gl.Uniform1i(texturedLocation_, 0);
    UploadViewProj(viewProjLocation_, request.viewProj);
    gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, identity.m);
    GteBegin(request); // gte_proj.h: the cells at the GTE's own SXY

    // The roadside world, textured from the type-2 chunk its cell's header pair names - or, for a
    // run whose key is 0x7800, from `DATA\G_OBJ01.GTP`, the fixed page the fix-up pass reaches
    // through the runtime record at guest `0x800D6160`.
    if (cellVertexCount_ > 0) {
        gl.Uniform3f(tintLocation_, 0.44f, 0.50f, 0.42f); // roadside world, untextured
        gl.BindVertexArray(cellVao_);
        OtSource(cellVbo_);
        GteStaticSource(true);
        if (cellRanges_.empty()) {
            glDrawArrays(GL_TRIANGLES, 0, cellVertexCount_);
        } else {
            // Select cells the way the game does: every chunk header carries residency windows
            // saying "this is needed on road R between `from` and `to`", and the driver's own road
            // and distance along it say which windows are live. Falls back to a distance cull where
            // the route position is unknown.
            const auto resident = [&](const CellRange& range) {
                bool draw = false;
                if (request.currentRoadKnown && !range.windows.empty()) {
                    for (const rr::ResidencyWindow& residency : range.windows)
                        if (residency.road == request.currentRoad &&
                            request.currentRoadDistance >= residency.from &&
                            request.currentRoadDistance <= residency.to)
                            draw = true;
                } else {
                    const float dx = range.centre[0] - request.eye[0];
                    const float dy = range.centre[1] - request.eye[1];
                    const float dz = range.centre[2] - request.eye[2];
                    draw = dx * dx + dy * dy + dz * dz <= request.cellDrawRadius * request.cellDrawRadius;
                }
                return draw;
            };

            // The original's per-group level of detail, `SLUS 0x80035680` (scene_geometry.h
            // CellLodWord), for the soups built per group. The resident cells are visited nearest
            // first - OURS: the console walks its own resident list (gp+2284), whose order decides
            // which four cells may go fine when more than four are within reach.
            const bool perGroup = request.haveCellView && cellData_ != nullptr && cellRanges_.front().group >= 0;
            lodWords_.assign(cellData_ ? cellData_->size() : 0, 0u);
            std::vector<char> isResident(lodWords_.size(), 0);
            // The PC graphics settings (race_scene_pc.cpp): the cells of the chosen draw distance -
            // the view's draw list, the residency windows (what the streamer holds) or every loaded cell within the far
            // plane - nearest first, each at CellLodWord's choice with maximum detail (every group fine) when asked.
            const bool pcCells = perGroup && (pc_.maxDetail || pc_.drawDistance > 0);
            if (pcCells) {
                if (pc_.drawDistance == 0 && request.cellOrder)
                    for (size_t c : *request.cellOrder)
                        if (c < isResident.size()) isResident[c] = 1;
                if (pc_.drawDistance == 1 || (pc_.drawDistance == 0 && !request.cellOrder)) {
                    for (const CellRange& range : cellRanges_)
                        if (range.cell < isResident.size() && !isResident[range.cell] && resident(range)) isResident[range.cell] = 1;
                    if (request.cellOrder)
                        for (size_t c : *request.cellOrder)
                            if (c < isResident.size()) isResident[c] = 1;
                }
                if (pc_.drawDistance >= 2)
                    for (size_t c = 0; c < isResident.size() && c < cellBox_.size(); ++c) {
                        const std::array<float, 6>& b = cellBox_[c];
                        float d2 = 0.0f;
                        for (int k = 0; k < 3; ++k) {
                            const float d = std::max({b[k] - request.eye[k], 0.0f, request.eye[k] - b[3 + k]});
                            d2 += d * d;
                        }
                        if (b[0] <= b[3] && d2 <= pc_.farPlane * pc_.farPlane) isResident[c] = 1;
                    }
                std::vector<std::pair<float, size_t>> order;
                for (size_t c = 0; c < isResident.size(); ++c) {
                    if (!isResident[c] || c >= cellBox_.size()) continue;
                    const std::array<float, 6>& b = cellBox_[c];
                    float d2 = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float d = std::max({b[k] - request.eye[k], 0.0f, request.eye[k] - b[3 + k]});
                        d2 += d * d;
                    }
                    order.emplace_back(d2, c);
                }
                std::sort(order.begin(), order.end());
                int fineCells = 0;
                for (const auto& entry : order)
                    lodWords_[entry.second] = CellLodWord((*cellData_)[entry.second], request.cellView, raceSet_, fineCells,
                                                          true, nullptr, pc_.maxDetail);
                pcResident_ = isResident;
            } else if (perGroup && request.cellOrder) {
                for (size_t c : *request.cellOrder)
                    if (c < isResident.size()) isResident[c] = 1;
                if (!OtArenaLodWords()) { // the PORTED 0x80035680's words (race_scene_ot.cpp)
                    int fineCells = 0;
                    for (size_t c : *request.cellOrder)
                        if (c < lodWords_.size())
                            lodWords_[c] = CellLodWord((*cellData_)[c], request.cellView, raceSet_, fineCells, request.keepCulled);
                }
            } else if (perGroup) {
                for (const CellRange& range : cellRanges_)
                    if (range.cell < isResident.size() && !isResident[range.cell] && resident(range))
                        isResident[range.cell] = 1;
                std::vector<std::pair<float, size_t>> order;
                for (size_t c = 0; c < isResident.size(); ++c) {
                    if (!isResident[c]) continue;
                    const rr::CellData& cell = (*cellData_)[c];
                    float best = 1e30f;
                    for (size_t i = 0; i < cell.vertexCount; i += 7) {
                        const float d[3] = {rr::CellWorldX(cell, i) - request.eye[0], rr::CellWorldY(cell, i) - request.eye[1],
                                            rr::CellWorldZ(cell, i) - request.eye[2]};
                        best = std::min(best, d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    }
                    order.emplace_back(best, c);
                }
                std::sort(order.begin(), order.end());
                int fineCells = 0;
                for (const auto& entry : order)
                    lodWords_[entry.second] =
                        CellLodWord((*cellData_)[entry.second], request.cellView, raceSet_, fineCells, request.keepCulled);
            }
            if (otOrder_ && perGroup) { // the frame's tables (race_scene_ot.cpp): the cells in the order LOD visited them
                std::vector<size_t> visit;
                if (request.cellOrder && !pcCells) {
                    visit = *request.cellOrder;
                } else {
                    for (size_t c = 0; c < isResident.size(); ++c)
                        if (isResident[c]) visit.push_back(c);
                }
                OtPlan(visit, request.cellView);
            }
            const auto chosen = [&](const CellRange& range) {
                if (!perGroup) return resident(range);
                if (range.cell >= isResident.size() || !isResident[range.cell]) return false;
                const rr::CellData& cell = (*cellData_)[range.cell];
                const int a = cell.countA, b = cell.countB, g = range.group;
                if (g < a) return true; // groups [0, A) are always drawn, coarse
                const bool band1 = g >= a + b;
                const int k = band1 ? g - a - b : g - a;
                const uint32_t nibble = (lodWords_[range.cell] >> (4 * k)) & 0xFu;
                if (!(nibble & 1u)) return false;
                return band1 ? (nibble & 2u) != 0 : (nibble & 2u) == 0;
            };

            // The fine NEAR groups (nibble bits 1 and 2: `0x8006A630`) are built per frame instead, their near
            // polygons cut up the way the original cuts them (scene_geometry.h AppendFineNearGroup).
            subdivStats_ = SubdivStats{};
            nearRanges_.clear();
            const auto fineNear = [&](const CellRange& range) {
                if (!subdivide_ || !perGroup || !look_.haveTables) return false;
                const rr::CellData& cell = (*cellData_)[range.cell];
                const int a = cell.countA, b = cell.countB, g = range.group;
                if (g < a + b) return false;
                return ((lodWords_[range.cell] >> (4 * (g - a - b))) & 4u) != 0;
            };
            // DEVELOPMENT: RRJB_PICK=x,y reports every cell draw that changes that pixel of the frame
            static const char* pickEnv = std::getenv("RRJB_PICK");
            int pickX = -1, pickY = -1;
            if (pickEnv) std::sscanf(pickEnv, "%d,%d", &pickX, &pickY);
            uint8_t pickLast[4] = {0, 0, 0, 0};
            const auto pick = [&](const CellRange& range, const char* tag) {
                if (pickX < 0) return;
                GLint vp[4];
                glGetIntegerv(GL_VIEWPORT, vp);
                uint8_t px[4] = {0, 0, 0, 0};
                glReadPixels(pickX, vp[3] - 1 - pickY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                if (std::memcmp(px, pickLast, 4) != 0)
                    std::printf("pick: %s cell %zu group %d first %d count %d pass %d seq %d -> %u %u %u\n", tag, range.cell,
                                range.group, range.first, range.count,
                                range.cell < otCellPass_.size() ? otCellPass_[range.cell] : -1,
                                range.cell < otCellSeq_.size() ? otCellSeq_[range.cell] : -1, px[0], px[1], px[2]);
                std::memcpy(pickLast, px, 4);
            };
            gl.Uniform1i(nclipLocation_, cellNclip_); // the emitters' back-face test (SetCellNclip)
            if (!otOrder_ && !layersBuilt_) PcBuildLayers(); // race_scene_pc.cpp: the coplanar layers, once
            const float eyeClipZ = eyeClipZ_; // the clip z of the eye (Draw's start)
            for (const CellRange& range : cellRanges_) {
                if (!chosen(range)) continue;
                if (pc_.cull) { // the PC graphics settings: the run's bounding box against the view's frustum
                    const size_t r = static_cast<size_t>(&range - cellRanges_.data());
                    if (r < rangeBox_.size() && !BoxVisible(rangeBox_[r])) {
                        ++stats_.cellRunsCulled;
                        continue;
                    }
                }
                ++stats_.cellRuns;
                if (fineNear(range)) {
                    nearRanges_.push_back(range);
                    continue;
                }
                const auto bound = cellTextures_.find(range.texKey);
                const bool textured = bound != cellTextures_.end() && bound->second.Valid();
                if (textured) {
                    BindIndexed(bound->second);
                    gl.Uniform1i(debugLocation_, forSubject(Subject::Cells));
                } else {
                    gl.Uniform1i(texturedLocation_, 0);
                    gl.Uniform1i(debugLocation_, otherDebug);
                }
                gl.Uniform1f(debugIdLocation_, static_cast<float>(drawnCells.size()));
                drawnCells.push_back(&range);
                OtCell(range.cell, range.group, false); // its table and link order (race_scene_ot.cpp)
                GteFillRange(range); // gte_proj.cpp: its vertices at the vertex pass's SXY
                static const bool pickTriangles = std::getenv("RRJB_PICK_TRI") != nullptr; // DEVELOPMENT
                if (pickX >= 0 && pickTriangles) { // triangle by triangle, as the near groups below
                    for (GLint t = 0; t + 3 <= range.count; t += 3) {
                        glDrawArrays(GL_TRIANGLES, range.first + t, 3);
                        GLint vp[4];
                        glGetIntegerv(GL_VIEWPORT, vp);
                        uint8_t px[4] = {0, 0, 0, 0};
                        glReadPixels(pickX, vp[3] - 1 - pickY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        if (std::memcmp(px, pickLast, 4) != 0) {
                            std::printf("pick: coarse cell %zu group %d tri at %d -> %u %u %u\n", range.cell, range.group,
                                        range.first + t, px[0], px[1], px[2]);
                            std::memcpy(pickLast, px, 4);
                        }
                    }
                }
                glDrawArrays(GL_TRIANGLES, range.first, range.count);
                // the depth buffer: its coplanar decals once more on top (race_scene_pc.cpp)
                if (!otOrder_) PcDrawLayers(static_cast<size_t>(&range - cellRanges_.data()), eyeClipZ);
                pick(range, "coarse");
            }

            if (!nearRanges_.empty()) {
                rr::TriangleSoup nearSoup;
                GteScreens nearScreens; // each vertex's GTE point (none: the float camera's)
                for (CellRange& range : nearRanges_) {
                    const size_t first = nearSoup.vertices.size();
                    if (const GteCell* gc = GteCellOf(range.cell))
                        AppendFineNearGroupGte((*cellData_)[range.cell], *gc, range.group, range.texKey, request.cellView,
                                               look_, request.sideSqueeze, nearSoup, nearScreens, subdivStats_, gteStats_);
                    else
                        AppendFineNearGroup((*cellData_)[range.cell], range.group, range.texKey, request.cellView, look_,
                                            request.sideSqueeze, nearSoup, subdivStats_);
                    nearScreens.resize(nearSoup.vertices.size());
                    range.first = static_cast<GLint>(first);
                    range.count = static_cast<GLsizei>(nearSoup.vertices.size() - first);
                }
                if (nearVao_ == 0) {
                    BindSoup(nearVao_, nearVbo_, nearSoup);
                } else {
                    gl.BindVertexArray(nearVao_);
                    gl.BindBuffer(GL_ARRAY_BUFFER, nearVbo_);
                }
                gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(nearSoup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                              nearSoup.vertices.empty() ? nullptr : nearSoup.vertices.data(), GL_DYNAMIC_DRAW);
                if (gteOn_) GteAttachScreens(nearVao_, gteNearScreenVbo_, nearScreens);
                for (const CellRange& range : nearRanges_) {
                    if (range.count == 0) continue;
                    const auto bound = cellTextures_.find(range.texKey);
                    const bool textured = bound != cellTextures_.end() && bound->second.Valid();
                    if (textured) {
                        BindIndexed(bound->second);
                        gl.Uniform1i(debugLocation_, forSubject(Subject::Cells));
                    } else {
                        gl.Uniform1i(texturedLocation_, 0);
                        gl.Uniform1i(debugLocation_, otherDebug);
                    }
                    gl.Uniform1f(debugIdLocation_, static_cast<float>(drawnCells.size()));
                    drawnCells.push_back(&range);
                    OtCell(range.cell, range.group, false);
                    if (pickX >= 0) { // DEVELOPMENT: triangle by triangle
                        for (GLint t = 0; t + 3 <= range.count; t += 3) {
                            glDrawArrays(GL_TRIANGLES, range.first + t, 3);
                            GLint vp[4];
                            glGetIntegerv(GL_VIEWPORT, vp);
                            uint8_t px[4] = {0, 0, 0, 0};
                            glReadPixels(pickX, vp[3] - 1 - pickY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                            if (std::memcmp(px, pickLast, 4) != 0) {
                                const rr::TriangleSoup::Vertex* tv = &nearSoup.vertices[static_cast<size_t>(range.first + t)];
                                std::printf("pick: near tri %d ot %.0f uv (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) pos (%.1f,%.1f,%.1f) (%.1f,%.1f,%.1f) (%.1f,%.1f,%.1f) -> %u %u %u\n",
                                            t / 3, tv[0].ot, tv[0].u, tv[0].v, tv[1].u, tv[1].v, tv[2].u, tv[2].v, tv[0].x, tv[0].y,
                                            tv[0].z, tv[1].x, tv[1].y, tv[1].z, tv[2].x, tv[2].y, tv[2].z, px[0], px[1], px[2]);
                                std::memcpy(pickLast, px, 4);
                            }
                        }
                    } else {
                        glDrawArrays(GL_TRIANGLES, range.first, range.count);
                    }
                    pick(range, "near");
                }
                gl.BindVertexArray(cellVao_);
            }

            // Band 2, the road surface, built for this frame from the fine groups (CollectBand2).
            band2_ = Band2Frame{};
            if (perGroup && look_.haveTables && pc_.maxDetail) {
                PcDrawBand2(request, isResident, forSubject(Subject::Cells), otherDebug); // race_scene_pc.cpp
            } else if (perGroup && look_.haveTables) {
                // gte_proj.h: each cell's band 2 at the GTE's own numbers (AppendBand2Gte), built
                // below per (cell, group) straight into the soups; a cell without a transformed slot the float way.
                const bool band2Gte = gteOn_ && subdivide_;
                for (size_t c = 0; c < lodWords_.size() && !band2Gte; ++c) {
                    if (!isResident[c]) continue;
                    const rr::CellData& cell = (*cellData_)[c];
                    for (size_t k = 0; k < cell.countB; ++k) {
                        const uint32_t nibble = (lodWords_[c] >> (4 * k)) & 0xFu;
                        if ((nibble & 3u) == 3u) CollectBand2(cell, c, k, request.cellView, look_, band2_);
                    }
                }
                rr::TriangleSoup road, lines;
                GteScreens roadScreens, lineScreens;
                // The ordering-table order draws each (cell, group)'s strips and lines at that cell's table and link
                // order (race_scene_ot.cpp), so the soups are built one group at a time.
                struct OtPiece {
                    size_t cell;
                    int group;
                    GLint roadFirst, lineFirst;
                    GLsizei roadCount, lineCount;
                };
                std::vector<OtPiece> otPieces;
                const auto append = [&](const Band2Frame& frame) {
                    if (subdivide_)
                        AppendBand2SoupSubdivided(frame, request.cellView, look_, request.sideSqueeze, road, lines, subdivStats_);
                    else
                        AppendBand2Soup(frame, 4, road, lines);
                };
                if (band2Gte) {
                    for (size_t c = 0; c < lodWords_.size(); ++c) {
                        if (!isResident[c]) continue;
                        const rr::CellData& cell = (*cellData_)[c];
                        for (size_t k = 0; k < cell.countB; ++k) {
                            if (((lodWords_[c] >> (4 * k)) & 3u) != 3u) continue;
                            OtPiece piece{c, static_cast<int>(cell.countA + 2u * cell.countB + k),
                                          static_cast<GLint>(road.vertices.size()), static_cast<GLint>(lines.vertices.size()), 0, 0};
                            if (const GteCell* gc = GteCellOf(c)) {
                                AppendBand2Gte(cell, c, k, *gc, request.cellView, look_, request.sideSqueeze, road, roadScreens,
                                               lines, lineScreens, subdivStats_, gteStats_);
                            } else {
                                Band2Frame one;
                                CollectBand2(cell, c, k, request.cellView, look_, one);
                                append(one);
                            }
                            roadScreens.resize(road.vertices.size());
                            lineScreens.resize(lines.vertices.size());
                            piece.roadCount = static_cast<GLsizei>(road.vertices.size()) - piece.roadFirst;
                            piece.lineCount = static_cast<GLsizei>(lines.vertices.size()) - piece.lineFirst;
                            if (piece.roadCount > 0 || piece.lineCount > 0) otPieces.push_back(piece);
                        }
                    }
                } else if (!otOrder_) {
                    append(band2_);
                } else {
                    std::vector<std::pair<size_t, int>> keys;
                    for (const Band2Strip& st : band2_.strips)
                        if (std::find(keys.begin(), keys.end(), std::make_pair(st.cell, st.group)) == keys.end())
                            keys.emplace_back(st.cell, st.group);
                    for (const Band2Line& ln : band2_.lines)
                        if (std::find(keys.begin(), keys.end(), std::make_pair(ln.cell, ln.group)) == keys.end())
                            keys.emplace_back(ln.cell, ln.group);
                    for (const auto& key : keys) {
                        Band2Frame one;
                        for (const Band2Strip& st : band2_.strips)
                            if (st.cell == key.first && st.group == key.second) one.strips.push_back(st);
                        for (const Band2Line& ln : band2_.lines)
                            if (ln.cell == key.first && ln.group == key.second) one.lines.push_back(ln);
                        OtPiece piece{key.first, key.second, static_cast<GLint>(road.vertices.size()),
                                      static_cast<GLint>(lines.vertices.size()), 0, 0};
                        append(one);
                        piece.roadCount = static_cast<GLsizei>(road.vertices.size()) - piece.roadFirst;
                        piece.lineCount = static_cast<GLsizei>(lines.vertices.size()) - piece.lineFirst;
                        otPieces.push_back(piece);
                    }
                }
                const auto upload = [](GLuint& vao, GLuint& vbo, const rr::TriangleSoup& soup) {
                    if (vao == 0) {
                        BindSoup(vao, vbo, soup);
                    } else {
                        gl.BindVertexArray(vao);
                        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
                    }
                    gl.BufferData(GL_ARRAY_BUFFER,
                                  static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                                  soup.vertices.empty() ? nullptr : soup.vertices.data(), GL_DYNAMIC_DRAW);
                };
                const auto roadTexture = cellTextures_.find(rr::kCellTexBand2);
                if (!road.vertices.empty() && roadTexture != cellTextures_.end() && roadTexture->second.Valid()) {
                    upload(band2Vao_, band2Vbo_, road);
                    if (gteOn_) GteAttachScreens(band2Vao_, gteRoadScreenVbo_, roadScreens);
                    BindIndexed(roadTexture->second);
                    gl.Uniform1i(debugLocation_, forSubject(Subject::Cells));
                    band2Range_ = CellRange{};
                    band2Range_.first = 0;
                    band2Range_.count = static_cast<GLsizei>(road.vertices.size());
                    band2Range_.texKey = rr::kCellTexBand2;
                    band2Range_.band = 2;
                    gl.Uniform1f(debugIdLocation_, static_cast<float>(drawnCells.size()));
                    drawnCells.push_back(&band2Range_);
                    if (!otOrder_) {
                        glDrawArrays(GL_TRIANGLES, 0, band2Range_.count);
                    } else {
                        for (const OtPiece& piece : otPieces) {
                            if (piece.roadCount == 0) continue;
                            OtCell(piece.cell, piece.group, false);
                            glDrawArrays(GL_TRIANGLES, piece.roadFirst, piece.roadCount);
                        }
                    }
                }
                if (!lines.vertices.empty()) {
                    // The lane lines lie IN the road's plane; the console draws them after the
                    // strips with no depth buffer at all. A polygon offset puts them on top here.
                    upload(lineVao_, lineVbo_, lines);
                    if (gteOn_) GteAttachScreens(lineVao_, gteLineScreenVbo_, lineScreens);
                    gl.Uniform1i(texturedLocation_, 0);
                    gl.Uniform1i(debugLocation_, otherDebug);
                    BeginLines(); // with the depth buffer a decal pass (race_scene_pc.cpp)
                    if (!otOrder_) {
                        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(lines.vertices.size()));
                    } else { // chained before their strips: in front of them in the same slot
                        for (const OtPiece& piece : otPieces) {
                            if (piece.lineCount == 0) continue;
                            OtCell(piece.cell, piece.group, true);
                            glDrawArrays(GL_TRIANGLES, piece.lineFirst, piece.lineCount);
                        }
                    }
                    EndDecal();
                }
            }
        }
        gl.Uniform1i(texturedLocation_, 0);
        gl.Uniform1i(debugLocation_, 0);
        gl.Uniform1i(nclipLocation_, 0);
    }
    GteEnd(); // the float camera for everything after the cells
    OtObject(0); // the models from here on (the props, the machines and every other renderer's objects set their own)

    if (request.afterCells) request.afterCells(otherDebug);

    if (!props_.empty()) {
        gl.Uniform3f(tintLocation_, 0.34f, 0.46f, 0.30f); // fallback when untextured
        if (propTexture_.Valid()) BindIndexed(propTexture_);
        gl.BindVertexArray(propVao_);
        OtSource(propVbo_);
        // The back-face test of the model emitter (shaders.cpp uNclip). The console keeps a one-sided
        // primitive whose corners (i0, i1, i2) run clockwise on its y-down screen; our frame is the
        // same picture (not mirrored), so that is clockwise in GL window space too, i.e. what GL calls
        // BACK-facing: +1. Checked against the original's packets by rrview --propcheck (rr-pack).
        gl.Uniform1i(nclipLocation_, propNclip_);
        // The eye in the console's 16.16 world unit, for the draw range below.
        int32_t eye16[3];
        for (int k = 0; k < 3; ++k) eye16[k] = static_cast<int32_t>(std::lround(request.eye[k] * 65536.0));
        for (const PropInstance& instance : props_) {
            // ModelVisible RASHCDG 0x80067AC4, kind 6: invisible when the view distance +0x2C (ViewDistance
            // 0x8008DBCC: ApproxLen3 of `(+0xB8 - view +0xB8) >> 10`, 1/64 world unit) is past the kind's
            // range *(0x800CC6A4 + 24) = 6400 (100 world units) - or 6400 + 32000 for group 0 (the LOD
            // byte +0x08, which is the class for a prop) - or negative.
            if (!pc_.maxDetail && !PropInRange(instance, eye16)) continue; // maximum detail: no draw range
            if (pc_.maxDetail && instance.cell >= 0 && // a placement-record prop: only in a cell drawn this view
                (static_cast<size_t>(instance.cell) >= pcResident_.size() || !pcResident_[static_cast<size_t>(instance.cell)]))
                continue;
            if (pc_.cull && instance.group < propRadius_.size() && !SphereVisible(instance.centre, propRadius_[instance.group])) {
                ++stats_.propsCulled; // the PC graphics settings: the prop's bounding sphere against the frustum
                continue;
            }
            ++stats_.props;
            gl.Uniform1i(debugLocation_, forSubject(Subject::Props));
            gl.Uniform1f(debugIdLocation_, static_cast<float>(propsDrawn));
            ++propsDrawn;
            drawnProps.push_back(&instance);
            gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, instance.matrix);
            OtObject(instance.entity); // its cell's table (race_scene_ot.cpp)
            if (instance.group < propLight_.size()) // the model light (race_scene_shadow.cpp)
                ApplyModelLight(propLight_[instance.group], LookLightOff() ? -1 : instance.flags);
            // At the PORTED model draw's own SXY when it drew this group (the prop's class)
            Mat4 pm;
            std::memcpy(pm.m, instance.matrix, sizeof(pm.m));
            std::vector<rr::TriangleSoup::Vertex> gs;
            if (instance.captured != nullptr && instance.group < propGroups_.size() &&
                GteObjectSoup(instance.captured, propModelId_, static_cast<int>(instance.group), pm, propGroups_[instance.group],
                              propSoup_.vertices.data() + propFirst_[instance.group],
                              static_cast<size_t>(propCount_[instance.group]), gs)) {
                GteStreamUpload(gteStreamVao_, gteStreamVbo_, gs);
                OtSource(gteStreamVbo_);
                glDrawArrays(GL_TRIANGLES, 0, propCount_[instance.group]);
                gl.BindVertexArray(propVao_);
                OtSource(propVbo_);
            } else {
                glDrawArrays(GL_TRIANGLES, propFirst_[instance.group], propCount_[instance.group]);
                DrawModelLayers(propLayers_, instance.group); // its decals on top
            }
        }
        gl.Uniform1i(modelLightLocation_, 0);
        gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, identity.m);
        gl.Uniform1i(nclipLocation_, 0);
        gl.Uniform1i(texturedLocation_, 0);
        gl.Uniform1i(debugLocation_, 0);
    }

    DrawMachine(request, forSubject(Subject::Bike), otherDebug);
}

// ------------------------------------------------------------------ the bike's shadow
// `SLUS 0x80025EE0`, found by watching the rr-race frame's first 0x2A packet being written
// (`rrverify trace --watch`, stores at 0x800268B4..0x800268F8). For each object it walks the
// model's `quadsD` list (DOD3 + 0x2C: a count, then u16[4] vertex indices - rmd3.md 2.3, whose
// purpose is closed by this: they are the SHADOW hull; the bike carries 18, the rider 47, and the
// frame holds 65 shadow packets), projects the posed vertices onto the ground plane along the light
// vector at 0x80052364 (a shear matrix built from it and the ground normal, 0x800D7FA8..), and emits
// one flat F4 per quad, code 0x2A, colour 0x80052348, inside `E1 0x740` (semi-transparency mode 2,
// B - F) and `E6 3` .. `E6 0` (mask bit set and tested), so a pixel is darkened once however many
// quads cover it.
// The light vector and the colour are the capture's (identical in all four states: colour
// (38, 37, 35), light (-3644, 2820, -1870)); they are written at level load and their source on the
// disc is not established. The light's horizontal part is -(sin, cos) of the sun yaw 0x80052388
// (715) times 4096, which ties it to the sky's own unknown.
void RaceScene::LoadShadow(const rr::DiscImage& disc, const std::vector<rr::PartSlot>& bikeSlots,
                           const std::vector<rr::PartSlot>& riderSlots) {
    const auto bikeFile = disc.Find("DATA/BBLEVEL1.GEO");
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!bikeFile || !overlay) return;
    const std::vector<rr::Model> machine = rr::ParseGeo(disc.ReadFile(*bikeFile));
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    const auto hull = [&](uint32_t id, const std::vector<rr::PartSlot>& slots,
                          std::vector<std::array<float, 3>>& out, float* groundY) {
        out.clear();
        for (const rr::Model& model : machine) {
            if (model.id != id || model.groups.empty()) continue;
            const rr::ModelGroup& group = model.groups.front();
            const rr::Assembly assembly = rr::AssembleGroup(group, skeleton);
            std::vector<rr::PartMatrix> local;
            if (!slots.empty() && slots.size() == group.subMeshes.size()) {
                local.assign(group.subMeshes.size(), rr::PartMatrix{});
                for (size_t i = 1; i < slots.size(); ++i) local[i] = slots[i].rot;
            }
            const rr::PosedGroup posed = rr::PoseGroup(group, skeleton, assembly, local);
            const int factor = rr::LodFactor(group);
            std::vector<size_t> owner(group.verts.size(), 0);
            for (size_t part = 0; part < group.subMeshes.size(); ++part)
                for (uint32_t i = 0; i < group.subMeshes[part].vertCount; ++i)
                    if (group.subMeshes[part].vertBase + i < owner.size()) owner[group.subMeshes[part].vertBase + i] = part;
            // The same placement `BuildPosedTriangleSoup` gives a drawn vertex.
            const auto place = [&](size_t index) {
                std::array<float, 3> p{};
                const rr::SVector& v = group.verts[index];
                const int32_t l[3] = {v.x * factor, v.y * factor, v.z * factor};
                const size_t part = owner[index];
                for (int r = 0; r < 3; ++r) {
                    int64_t sum = 0;
                    for (int c = 0; c < 3; ++c) sum += static_cast<int64_t>(posed.world[part].m[r * 3 + c]) * l[c];
                    p[static_cast<size_t>(r)] = static_cast<float>(static_cast<int32_t>(sum >> 12) +
                                                                   posed.origin[part][static_cast<size_t>(r)]);
                }
                return p;
            };
            for (const std::array<uint16_t, 4>& quad : group.quadsD) {
                bool inRange = true;
                for (uint16_t i : quad) inRange = inRange && i < group.verts.size();
                if (!inRange) continue;
                for (uint16_t i : quad) out.push_back(place(i));
            }
            if (groundY) {
                *groundY = 0.0f;
                for (size_t i = 0; i < group.verts.size(); ++i) *groundY = std::max(*groundY, place(i)[1]);
            }
            return;
        }
    };
    hull(100, bikeSlots, bikeShadow_, &bikeGroundY_);
    hull(150, riderSlots, riderShadow_, nullptr);
    if (bikeShadow_.empty() && riderShadow_.empty()) return;
    shadowProgram_ = BuildEdgeProgram(kShadowVertexShader, kShadowFragmentShader, {}, 2, "shadow"); // the console's fill rule
    shadowViewProjLocation_ = gl.GetUniformLocation(shadowProgram_, "uViewProj");
    shadowColourLocation_ = gl.GetUniformLocation(shadowProgram_, "uColour");
    shadowDebugLocation_ = gl.GetUniformLocation(shadowProgram_, "uDebug");
    gl.GenVertexArrays(1, &shadowVao_);
    gl.BindVertexArray(shadowVao_);
    gl.GenBuffers(1, &shadowVbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, shadowVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * static_cast<GLsizei>(sizeof(float)), reinterpret_cast<void*>(0));
    gl.EnableVertexAttribArray(0);
    std::printf("shadow: %zu bike quads, %zu rider quads (quadsD), ground at model y %.0f\n", bikeShadow_.size() / 4,
                riderShadow_.size() / 4, bikeGroundY_);
}

void RaceScene::DrawShadow(const DrawRequest& request, int debug) {
    if (shadowProgram_ == 0 || request.debugMode == 5 || request.debugMode == 6) return;
    if (DrawPortedShadows(request, debug)) return; // the PORTED SLUS 0x80025EE0 (race_scene_shadow.cpp)
    const float kLight[3] = {skyMutation_ == 5 ? 0.0f : -3644.0f, skyMutation_ == 5 ? 4096.0f : 2820.0f,
                             skyMutation_ == 5 ? 0.0f : -1870.0f}; // 0x80052364, y down
    constexpr float kColour[3] = {38.0f / 255.0f, 37.0f / 255.0f, 35.0f / 255.0f}; // 0x80052348
    const float* b = request.bikeModel.m;
    // The ground plane: through the entity's +0xB8 (the model origin - `0x800251E4` passes +0xB8, or
    // +0x1F8, which holds the same point in rr-race), normal = the bike's up (+0x20A in the original).
    float up[3] = {-b[4], -b[5], -b[6]};
    const float upLength = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (upLength <= 0.0f) return;
    for (float& u : up) u /= upLength;
    const float ground[3] = {b[12], b[13], b[14]};
    // The original works in CAMERA space, whose second row is scaled by 3412/4096:
    // the normal, the light and the ground point all go through the camera matrix first, so the
    // clamp below and the plane it projects onto are those of the scaled space. Reproduced here by
    // mapping in (C = diag(1, 3412/4096, 1) * [right; down; forward]), projecting, and mapping back.
    // The axes, unit length whatever the caller's vectors carry (a capture's down row is 3412/4096
    // long).
    CellView cv = request.haveCellView ? request.cellView : lastCellView_;
    for (float* axis : {cv.right, cv.up, cv.forward}) {
        const float l = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (l > 1e-6f)
            for (int k = 0; k < 3; ++k) axis[k] /= l;
    }
    const float kShadowRow = kGteAspectRow;
    const auto toCam = [&](const float v[3], float out[3]) {
        out[0] = cv.right[0] * v[0] + cv.right[1] * v[1] + cv.right[2] * v[2];
        out[1] = -(cv.up[0] * v[0] + cv.up[1] * v[1] + cv.up[2] * v[2]) * kShadowRow;
        out[2] = cv.forward[0] * v[0] + cv.forward[1] * v[1] + cv.forward[2] * v[2];
    };
    const auto fromCam = [&](const float c[3], float out[3]) {
        const float d = -c[1] / kShadowRow;
        for (int k = 0; k < 3; ++k) out[k] = cv.right[k] * c[0] + cv.up[k] * d + cv.forward[k] * c[2];
    };
    float nc[3], lc[3], groundC[3];
    toCam(up, nc);
    toCam(kLight, lc);
    // Everything relative to the ground point: the capture's axes are orthogonal only to about
    // 1e-3, which at world coordinates in the thousands would move a point by whole units.
    groundC[0] = groundC[1] = groundC[2] = 0.0f;
    float nl = nc[0] * lc[0] + nc[1] * lc[1] + nc[2] * lc[2];
    if (nl < 0.0f) { // the normal on the light's side, as +0x20A is in the captures
        for (float& n : nc) n = -n;
        nl = -nl;
    }
    // `0x80025EE0` limits how long a shadow gets: when (n . L) >> 8 with n at 4096 = 1 - i.e. n . L
    // under 2896 - it builds L' = 0.7071 (n + p) with p the unit part of L across n (two outer
    // products, the normaliser 0x8002E468 and the blend 0x8002EB78(n, p, out, 0xB504, 0xB504)).
    // The capture's light is 55 degrees off vertical, so the clamp is what sets the shadow's length.
    if (nl < 2896.0f) {
        const float nn = nc[0] * nc[0] + nc[1] * nc[1] + nc[2] * nc[2];
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = lc[k] * nn - nc[k] * nl; // (n x L) x n
        const float pl = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (pl > 1e-6f)
            for (int k = 0; k < 3; ++k) lc[k] = 0.70710678f * (nc[k] + p[k] / pl) * 4096.0f;
        nl = nc[0] * lc[0] + nc[1] * lc[1] + nc[2] * lc[2];
    }
    if (std::fabs(nl) < 1e-3f) return;
    std::vector<float> tris;
    const auto emit = [&](const std::vector<std::array<float, 3>>& hull, const Mat4& model) {
        for (size_t q = 0; q + 3 < hull.size(); q += 4) {
            float w[4][3];
            for (int c = 0; c < 4; ++c) {
                const std::array<float, 3>& p = hull[q + static_cast<size_t>(c)];
                float x[3];
                for (int k = 0; k < 3; ++k)
                    x[k] = model.m[0 + k] * p[0] + model.m[4 + k] * p[1] + model.m[8 + k] * p[2] + model.m[12 + k];
                float xc[3];
                for (int k = 0; k < 3; ++k) x[k] -= ground[k];
                toCam(x, xc);
                const float t = (nc[0] * (groundC[0] - xc[0]) + nc[1] * (groundC[1] - xc[1]) + nc[2] * (groundC[2] - xc[2])) / nl;
                float sc[3];
                for (int k = 0; k < 3; ++k) sc[k] = xc[k] + t * lc[k];
                fromCam(sc, w[c]);
                for (int k = 0; k < 3; ++k) w[c][k] += ground[k];
                for (int k = 0; k < 3; ++k) w[c][k] += up[k] * 0.01f;
                shadowWorld_.push_back({w[c][0], w[c][1], w[c][2]});
            }
            // The packet's corner order is (q0, q1, q3, q2), which the GPU splits into
            // (q0, q1, q3) and (q1, q3, q2).
            const int order[6] = {0, 1, 3, 1, 3, 2};
            for (int i : order)
                for (int k = 0; k < 3; ++k) tris.push_back(w[i][k]);
        }
    };
    emit(bikeShadow_, request.bikeModel);
    emit(riderShadow_, request.riderModel);
    if (tris.empty()) return;
    gl.UseProgram(shadowProgram_);
    UploadViewProj(shadowViewProjLocation_, request.viewProj);
    gl.Uniform3f(shadowColourLocation_, kColour[0], kColour[1], kColour[2]);
    gl.Uniform1i(shadowDebugLocation_, debug);
    gl.BindVertexArray(shadowVao_);
    gl.BindBuffer(GL_ARRAY_BUFFER, shadowVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)), tris.data(), GL_DYNAMIC_DRAW);
    // No depth test: the GPU has none - the shadow sits in the ordering table at its own depth and the
    // road under it is already drawn - and the ground plane (the entity's +0xB8) can lie a hair
    // under our road polygons, which would bury half of it. The machine is drawn after and covers it.
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    if (debug == 0) {
        glEnable(GL_BLEND);
        gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(GL_ONE, GL_ONE);
    }
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / 3));
    glDisable(GL_BLEND);
    gl.BlendEquation(GL_FUNC_ADD);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    gl.UseProgram(program_);
}

// ------------------------------------------------------------------ the PS1 look
void RaceScene::PostProcess(int x, int y, int width, int height, bool dither) {
    if (width <= 0 || height <= 0) return;
    if (postProgram_ == 0) {
        postProgram_ = BuildProgram(kPostVertexShader, kPostFragmentShader);
        postFrameLocation_ = gl.GetUniformLocation(postProgram_, "uFrame");
        postDitherLocation_ = gl.GetUniformLocation(postProgram_, "uDither");
        postOriginLocation_ = gl.GetUniformLocation(postProgram_, "uOrigin");
        postScaleLocation_ = gl.GetUniformLocation(postProgram_, "uScale");
        gl.GenVertexArrays(1, &postVao_);
        glGenTextures(1, &postTexture_);
    }
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, postTexture_);
    if (width != postWidth_ || height != postHeight_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        postWidth_ = width;
        postHeight_ = height;
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, width, height);
    gl.UseProgram(postProgram_);
    gl.Uniform1i(postFrameLocation_, 0);
    gl.Uniform1i(postDitherLocation_, dither ? 1 : 0);
    gl.Uniform2f(postOriginLocation_, static_cast<float>(x), static_cast<float>(y));
    gl.Uniform1f(postScaleLocation_, static_cast<float>(height) / consoleLines_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    gl.BindVertexArray(postVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

// The segments `RASHCDG 0x8006396C` draws: eight from the yaw's first segment. A window wider than
// the console's 4:3 sees up to one segment further left (the eight reach at least 39 degrees left of
// the view axis - exactly the 4:3 half-field - and 62 right), so one more on each side is drawn;
// in a 4:3 frame those two are off-screen, which is the original's own cull.
// OURS (DrawRequest::worldSky): the gradient of RASHCDG 0x80063C5C laid on a sphere
// around the eye instead of the screen. The console's strip, for a level camera, puts the top colour A at the top of
// the picture (tan = 120 / (H 3412/4096), 31.3 degrees up), the blend of C and D at the sun's elevation (the middle
// row's RTPS direction) and B ten console rows under the horizon; between them the colour runs linearly down the
// screen, i.e. linearly in tan(elevation). The dome evaluates exactly that per ring of directions, and the middle
// colour per azimuth: SkyBlendWeight(azimuth - sun) - the strip's two edge colours are the same function at the yaw
// +-426. Above 31.3 degrees A, under the band B. Drawn like the panorama: at infinity, no depth test or write.
void RaceScene::DrawSkyDome(const DrawRequest& request) {
    if (!skyGradient_.valid || domeFailed_) return;
    if (domeProgram_ == 0) {
        static const char* const kDomeVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColour;
uniform mat4 uViewProj;
out vec3 vColour;
void main() {
    vColour = aColour;
    // the world sky: a direction (w = 0) - at infinity, rotation only, the same in both eyes; z on the far plane
    gl_Position = uViewProj * vec4(aPos, 0.0);
    gl_Position.z = gl_Position.w * 0.999999;
}
)";
        static const char* const kDomeFs = R"(#version 330 core
in vec3 vColour;
out vec4 oColor;
void main() { oColor = vec4(vColour, 1.0); }
)";
        try {
            domeProgram_ = BuildProgram(kDomeVs, kDomeFs);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sky dome: %s\n", e.what());
            domeFailed_ = true;
            return;
        }
        domeViewProjLocation_ = gl.GetUniformLocation(domeProgram_, "uViewProj");
        gl.GenVertexArrays(1, &domeVao_);
        gl.BindVertexArray(domeVao_);
        gl.GenBuffers(1, &domeVbo_);
        gl.BindBuffer(GL_ARRAY_BUFFER, domeVbo_);
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void*>(3 * sizeof(float)));
        gl.EnableVertexAttribArray(1);
        gl.BindVertexArray(0);
    }
    if (domeSunAngle_ != gradSunAngle_) {
        domeSunAngle_ = gradSunAngle_;
        constexpr double kPi = 3.14159265358979323846;
        const double hy = static_cast<double>(kGteH) * static_cast<double>(kGteAspectRow);
        const double tTop = 120.0 / hy;                                            // the picture's top row
        const double tMid = std::tan(static_cast<double>(sunElevation_) * 2.0 * kPi / 4096.0); // the middle row
        const double tHor = -10.0 / hy;                                            // ten rows under the horizon
        const auto row = [&](double t) {
            if (t >= tTop) return 0.0;
            if (t >= tMid) return 0.5 * (tTop - t) / (tTop - tMid);
            if (t >= tHor) return 0.5 + 0.5 * (tMid - t) / (tMid - tHor);
            return 1.0;
        };
        // rings every ~1 degree through the band (Gouraud between rings is linear along the chord, close to the strip's
        // linear-in-tan rows at that spacing; coarser rings showed as steps)
        std::vector<double> rings = {-90.0, -30.0};
        const double eHor = std::atan(tHor) * 180.0 / kPi, eMid = std::atan(tMid) * 180.0 / kPi,
                     eTop = std::atan(tTop) * 180.0 / kPi;
        for (int i = 0; i < 14; ++i) rings.push_back(eHor + (eMid - eHor) * i / 14.0);
        for (int i = 0; i < 20; ++i) rings.push_back(eMid + (eTop - eMid) * i / 20.0);
        rings.push_back(eTop);
        rings.push_back(60.0);
        rings.push_back(90.0);
        constexpr int kSegments = 64;
        constexpr float kRadius = 1000.0f; // world units around the eye (inside every far plane)
        const auto colourAt = [&](double elevDeg, int seg, float out[3]) {
            const double r = row(std::tan(std::clamp(elevDeg, -89.9, 89.9) * kPi / 180.0));
            const int32_t az = static_cast<int32_t>(std::lround(4096.0 * seg / kSegments)) & 0xFFF;
            uint8_t mid[3];
            rr::SkyBlendColour(skyGradient_, rr::SkyBlendWeight(az - gradSunAngle_), mid);
            for (int k = 0; k < 3; ++k) {
                const double a = skyGradient_.rgb[0][k], m = mid[k], b = skyGradient_.rgb[1][k];
                const double c = r < 0.5 ? a + (m - a) * (r * 2.0) : m + (b - m) * ((r - 0.5) * 2.0);
                out[k] = static_cast<float>(c / 255.0);
            }
        };
        std::vector<float> v;
        v.reserve((rings.size() - 1) * kSegments * 36);
        const auto vertex = [&](size_t ring, int seg) {
            const double el = rings[ring] * kPi / 180.0, az = 2.0 * kPi * seg / kSegments;
            // the world's up is -y; azimuth as the camera's yaw: atan2(x, z)
            v.push_back(kRadius * static_cast<float>(std::cos(el) * std::sin(az)));
            v.push_back(-kRadius * static_cast<float>(std::sin(el)));
            v.push_back(kRadius * static_cast<float>(std::cos(el) * std::cos(az)));
            float c[3];
            colourAt(rings[ring], seg % kSegments, c);
            v.insert(v.end(), c, c + 3);
        };
        for (size_t i = 0; i + 1 < rings.size(); ++i)
            for (int s = 0; s < kSegments; ++s) {
                vertex(i, s);
                vertex(i + 1, s);
                vertex(i, s + 1);
                vertex(i, s + 1);
                vertex(i + 1, s);
                vertex(i + 1, s + 1);
            }
        domeVertexCount_ = static_cast<GLsizei>(v.size() / 6);
        gl.BindBuffer(GL_ARRAY_BUFFER, domeVbo_);
        gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), GL_STATIC_DRAW);
    }
    Mat4 atEye;
    for (int k = 0; k < 3; ++k) atEye.m[12 + k] = request.eye[k];
    gl.UseProgram(domeProgram_);
    UploadViewProj(domeViewProjLocation_, request.viewProj, &atEye);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    gl.BindVertexArray(domeVao_);
    glDrawArrays(GL_TRIANGLES, 0, domeVertexCount_);
    gl.BindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void RaceScene::DrawClouds(const DrawRequest& request, int debug) {
    if (cloudProgram_ == 0 || cloudRing_.empty()) return;
    Mat4 atEye;
    for (int k = 0; k < 3; ++k) atEye.m[12 + k] = request.eye[k];
    gl.UseProgram(cloudProgram_);
    UploadViewProj(cloudViewProjLocation_, request.viewProj, &atEye); // Multiply(viewProj, atEye); per eye in stereo
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cloudTexture_);
    gl.Uniform1i(cloudTexLocation_, 0);
    gl.Uniform1i(cloudDebugLocation_, debug);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (debug == 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    gl.BindVertexArray(cloudVao_);
    constexpr int n = rr::SkyClouds::kSegments;
    // DrawRequest::worldSky (a stereo eye, the head camera): the whole ring at infinity, perspective-correct, each
    // segment from its own slice. The console's eight segments from the camera's yaw cover 39 degrees left of the view
    // axis - a headset's eye and a turned or tilted head see past that, and with only those eight the edge of the
    // clouds would jump by 15 degrees with the head's yaw.
    gl.Uniform1i(cloudWorldLocation_, request.worldSky ? 1 : 0);
    const int from = request.worldSky ? 0 : -1, to = request.worldSky ? n : rr::SkyClouds::kDrawn + 1;
    for (int i = from; i < to; ++i) {
        const int segment = ((cloudFirst_ + i) % n + n) % n;
        if (request.worldSky) {
            const rr::SkyCloudSlice slice = rr::SkyCloudSliceOf(skyMutation_ == 4 ? segment + 1 : segment);
            gl.Uniform4f(cloudRectLocation_, static_cast<float>(slice.u0), static_cast<float>(slice.u1),
                         static_cast<float>(slice.v0), static_cast<float>(slice.v1));
        }
        glDrawArrays(GL_TRIANGLES, segment * 6, 6);
    }
    gl.Uniform1i(cloudWorldLocation_, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void SetPartLight(ModelPart& part, const rr::ModelGroup& group) {
    part.lit = !group.normals.empty() && !group.vertNormalIndex.empty();
    part.modelClass = static_cast<int>((group.flags >> 19) & 0xFu); // DOD3 +0x0E bits 3..6
}

void RaceScene::SetModelLight(bool enabled, const int32_t light[3]) {
    modelLightOn_ = enabled && modelRampValid_;
    for (int k = 0; k < 3; ++k) modelLight_[k] = light[k];
}

void RaceScene::ApplyModelLight(const ModelPart& part, int64_t objFlags) const {
    if (!modelLightOn_ || objFlags < 0) {
        gl.Uniform1i(modelLightLocation_, 0);
        return;
    }
    const uint32_t f = static_cast<uint32_t>(objFlags);
    const bool unlitFlag = (f >> 11) & 1u;
    const int shift = ((f >> 5) & 3u) != 0 && !unlitFlag ? 1 : 0;
    gl.Uniform1i(modelLightLocation_, part.lit && !unlitFlag && modelLit_ ? 1 : 2);
    gl.Uniform1i(lightXLocation_, modelLight_[0]);
    gl.Uniform1i(lightYLocation_, modelLight_[1]);
    gl.Uniform1i(lightZLocation_, modelLight_[2]);
    gl.Uniform1i(rampShiftLocation_, shift);
    gl.Uniform1i(unlitLocation_, std::clamp(modelUnlit_ + (part.modelClass == 3 ? 8 : 0), 0, 31));
    for (int k = 0; k < 32; ++k) gl.Uniform3f(rampLocation_[k], modelRamp_[k][0], modelRamp_[k][1], modelRamp_[k][2]);
}

void RaceScene::DrawMachine(const DrawRequest& request, int bikeDebug, int otherDebug) {
    if (!request.haveMachine || bikeParts_.empty()) return;
    if (!request.sidecar && DrawRivalMachine(request, bikeDebug, otherDebug)) return; // its own machine (race_scene_rivals.cpp)
    const int bikeLod = (request.bikeLod > 0 && request.bikeLod < 4 && bikeLod_[request.bikeLod]) ? request.bikeLod : 0;
    const int riderLod = (request.riderLod > 0 && request.riderLod < 4 && riderLod_[request.riderLod]) ? request.riderLod : 0;
    if (riderLod == 0 && riderPose_) UploadRiderPose(*riderPose_, bikeVbo_, request.riderLocal); // rider_pose_draw.h
    if (bikeLod == 0 && bikePose_ && request.bikeLocal) UploadRiderPose(*bikePose_, bikeVbo_, request.bikeLocal); // the bike's part slots
    if (riderLod > 0) UploadLodPose(*riderLod_[riderLod], bikeVbo_, request.riderLocal, 17); // lod_pose_draw.h
    if (bikeLod > 0) UploadLodPose(*bikeLod_[bikeLod], bikeVbo_, request.bikeLocal, 5);
    ApplyCaptured(request, 100u, 150u, bikePose_.get(), bikeLod > 0 ? bikeLod_[bikeLod].get() : nullptr, riderPose_.get(),
                  riderLod > 0 ? riderLod_[riderLod].get() : nullptr, bikeVbo_, bikeLod, riderLod); // the model draw's own vertices
    // The shadow first, under the machine: the original's shadow packets sit behind the bike's own. (With the
    // ordering-table order the ported one is drawn after the whole opaque view instead: FlushOtShadows.)
    DrawShadow(request, otherDebug);
    const Mat4 identity;
    gl.Uniform3f(tintLocation_, 0.78f, 0.74f, 0.66f); // fallback when untextured
    gl.BindVertexArray(bikeVao_);
    OtSource(bikeVbo_);
    OtObject(request.bikeObject); // its cell's table (race_scene_ot.cpp)
    const bool rig2 = request.sidecarRig == 1; // player 2's own rig (race_scene_sidecar2.cpp)
    const bool sidecar = request.sidecar && (rig2 ? HasSidecarFor(1) : !sidecarParts_.empty()); // the rig replaces the bike (race_scene_sidecar.cpp)
    for (size_t n = 0; n < bikeParts_.size(); ++n) {
        const ModelPart& part = bikeParts_[n];
        if (part.lod != (part.owner == 0 ? bikeLod : riderLod)) continue; // the LOD this machine is drawn at
        if (sidecar && part.owner == 0) continue;
        if (part.owner == 1 && request.riderHideParts != 0 && // the head camera (head_camera.h): its own head not drawn
            (part.lod != 0 || (part.sub >= 0 && part.sub < 32 && ((request.riderHideParts >> part.sub) & 1u) != 0)))
            continue;
        gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE,
                            part.owner == 0 ? request.bikeModel.m : request.riderModel.m);
        ApplyModelLight(part, part.owner == 0 ? request.bikeObjFlags : request.riderObjFlags);
        const bool textured = static_cast<size_t>(part.sheet) < riderSheetTextures_.size() &&
                              riderSheetTextures_[static_cast<size_t>(part.sheet)].Valid();
        if (textured) {
            BindIndexed(riderSheetTextures_[static_cast<size_t>(part.sheet)]);
            gl.Uniform1i(debugLocation_, bikeDebug);
        } else {
            gl.Uniform1i(texturedLocation_, 0);
            gl.Uniform1i(debugLocation_, otherDebug);
        }
        gl.Uniform1f(debugIdLocation_, static_cast<float>(n));
        glDrawArrays(GL_TRIANGLES, part.first, part.count);
        DrawModelLayers(machineLayers_, n); // its decals on top
    }
    gl.Uniform1i(modelLightLocation_, 0);
    if (sidecar && rig2) SwapSidecarRig();
    if (sidecar) DrawSidecar(request, bikeDebug, otherDebug, request.bikeLod);
    if (sidecar && rig2) SwapSidecarRig();
    gl.Uniform1i(modelLightLocation_, 0);
    gl.Uniform1i(nclipLocation_, 0); // ApplyCaptured set the emitter's back-face test
    gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, identity.m);
    gl.Uniform1i(texturedLocation_, 0);
    gl.Uniform1i(debugLocation_, 0);
}

void RaceScene::DrawMachineOnly(const DrawRequest& request) {
    gl.UseProgram(program_);
    SetRenderOrigin(otOrder_ || (std::getenv("RRJB_ORIGIN") != nullptr && std::string(std::getenv("RRJB_ORIGIN")) == "off")
                        ? nullptr
                        : request.eye); // camera-relative with the depth buffer (Draw)
    UploadViewProj(viewProjLocation_, request.viewProj);
    DrawMachine(request, 0, 0);
}

Mat4 MachineMatrix(const RoadFrame& frame, const float up[3], const float position[3], float scale) {
    // Model-to-world scale is UNRESOLVED, and `scale` is a placeholder chosen to make the prototype
    // legible - do not treat it as a finding. Two independent anchors disagree by about a factor of
    // two: the road width (XSAI gives ~39 world units for a two-lane road, i.e. ~0.18 m per unit)
    // and the traffic car's collision box (5.72 units for a ~4.5 m car, i.e. ~0.79 m per unit).
    // Settling it needs the projection the game itself applies.
    Mat4 model;
    for (int k = 0; k < 3; ++k) {
        model.m[0 + k] = frame.lateral[k] * scale; // model X -> lateral
        model.m[4 + k] = -up[k] * scale;           // model Y is down
        model.m[8 + k] = frame.tangent[k] * scale; // model Z -> along the road
        model.m[12 + k] = position[k] + up[k] * 1.0f;
    }
    return model;
}

Mat4 MachineMatrixAt(const RoadFrame& frame, const float up[3], const float origin[3], float scale) {
    Mat4 model = MachineMatrix(frame, up, origin, scale);
    for (int k = 0; k < 3; ++k) model.m[12 + k] = origin[k];
    return model;
}

Mat4 RiderMatrix(const Mat4& bike, const float attach[3], const float riderRelative[9]) {
    // `RASHCDG 0x80066B98` gives the child object's POSITION - the parent's attachment vertex
    // through the parent's orientation - and nothing else; the child's own 3x3 comes from the
    // animation tables, whose payload is not decoded (rmd3.md section 5).
    Mat4 rider = bike;
    for (int k = 0; k < 3; ++k)
        rider.m[12 + k] = bike.m[12 + k] + bike.m[0 + k] * attach[0] + bike.m[4 + k] * attach[1] +
                          bike.m[8 + k] * attach[2];
    for (int k = 0; k < 3; ++k) {
        const float axis[3] = {bike.m[0 + k], bike.m[4 + k], bike.m[8 + k]};
        for (int column = 0; column < 3; ++column)
            rider.m[4 * column + k] = axis[0] * riderRelative[0 * 3 + column] +
                                      axis[1] * riderRelative[1 * 3 + column] +
                                      axis[2] * riderRelative[2 * 3 + column];
    }
    return rider;
}

} // namespace rr::render
