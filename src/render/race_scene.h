#pragma once
// The race scene as the renderer draws it: the sky gradient, the type-4 panorama, the scene cells,
// the roadside props and the player's machine.
//
// This is the renderer `rrview` and `rrgame` SHARE. `rrview` is the instrument - it draws the same
// scene again with the subject surface reporting what it sampled, and checks every pixel against
// the palette entry the binding rule predicts (`--texcheck`, `--cellcheck`, `--bikecheck`,
// `--skycheck`, `--skygradcheck`). `rrgame` draws the same scene for the player. One draw path
// means the checks are about the game's own frame and not about a copy of it.
//
// The evidence behind each surface is in the documents the draw code cites:
// docs\formats\scene_cell.md 12 (cell -> texture chunk -> palette), rrformats/model_texture.h (model -> LECT ->
// palette, and the bike's KNBP bank), rrformats/chunk.h (the panorama) and rrformats/sky_gradient.h (the sky gradient).
#include "render/coplanar.h" // the coplanar layers of the cells and the models
#include "render/gl_api.h"
#include "render/gpu_texture.h"
#include "render/gte_proj.h"
#include "render/mat4.h"
#include "render/multiview.h" // UploadViewProj: single-pass stereo
#include "render/scene_geometry.h"
#include "render/rider_pose_draw.h"
#include "render/lod_pose_draw.h"
#include "rrformats/chunk.h"
#include "rrformats/model_texture.h"
#include "rrformats/pose.h"
#include "rrformats/sky_clouds.h"
#include "rrformats/sky_gradient.h"
#include "rrvfs/disc_image.h"

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rr::render {

struct CapturedVerts;

// One placed roadside prop: group `cls` of model id 200 at an absolute world position.
struct PropInstance {
    float matrix[16] = {};
    size_t group = 0;
    float centre[3] = {};
    int32_t pos[3] = {}; // the record's 16.16 world position, for the draw range (RaceScene::Draw)
    int64_t flags = -1;  // the object's +0x24 for the model light (ApplyObjectLight); -1: off
    uint32_t entity = 0; // the object (its cell +0xB0 picks its ordering table, race_scene_ot.cpp); 0 unknown
    const CapturedVerts* captured = nullptr; // the PORTED model draw's vertices of it this view (gte_proj.cpp)
    int cell = -1; // LoadProps: the index of the cell whose placement record it is (the maximum-detail draw)
};

// Which DATA\HAZARD<n>.GEO / .TEX pair a race loads, by the loader's own rule (RASHCDI.BIN, SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06): 0x8006383C passes `s0` to the hazard loader 0x8005C7F0,
// which loads set 0 when `(mode & 0x18) == 8` and set `s0 % 10` otherwise; `s0` is 1 when `mode & 0x10`
// and else 0x8006AD4C's answer: `sel = count ? Rand() % count : 0` over the `count = ENV.EN[+0xCC]` pairs
// at `ENV.EN + 0xCD` (the file 0x800674F8 reads into 0x8007B8B8), then `clamp((s8)pair[sel][0], 1, 5)`.
// (Its search past pairs whose bytes equal the class table 0x8005B328 finds none on the disc: the pairs
// hold 1..5 and 6, the table class 9.) `mode` is game_state+4; `seed` the Rand seed (gp+2076) the draw
// starts from. Returns -1 when ENV.EN is not on the disc.
int PickHazardSet(const rr::DiscImage& disc, uint8_t mode, uint32_t seed);

// The draw range of a prop, ModelVisible RASHCDG 0x80067AC4's kind-6 test: `*(0x800CC6A4 + 4 * 6)`, the
// same 6400 in all four race captures (1/64 world unit, i.e. 100 world units), widened by 32000 for group 0.
constexpr int32_t kPropDrawRange = 6400;
// `eye16` is the view's +0xB8 (the camera eye) in 16.16 world units.
bool PropInRange(const PropInstance& instance, const int32_t eye16[3]);
// uNclip for the props (shaders.cpp): the side GL calls back-facing is the console's visible one.
constexpr int kPropNclip = 1;

// One draw run of the player's machine. `sheet` indexes `riderSheets`, `owner` says which model
// matrix places it (0 the bike, 1 the rider).
struct ModelPart {
    GLint first = 0;
    GLsizei count = 0;
    int sheet = 0;
    int owner = 0;
    int lod = 0; // the level of detail this run belongs to (lod_pose_draw.h)
    int sub = -1; // the sub-mesh of its group this run draws (the head camera's hidden parts, head_camera.h)
    // The model light (SetModelLight): the group carries normals and a vertex -> normal table (DOD3 +0x28 / +0x30,
    // lit), and its class (DOD3 +0x0E bits 3..6; class 3 takes the unlit step + 8, SLUS 0x800251E4).
    bool lit = false;
    int modelClass = 0;
};
// A group's model-light fields (race_scene.cpp).
void SetPartLight(ModelPart& part, const rr::ModelGroup& group);

// Which surface a reporting pass is about. Everything else is drawn with uDebug 4 - unchanged
// geometry and unchanged discard, but blue 0 - so a reported fragment can never be confused with
// another surface's colour.
enum class Subject { Props, Cells, Bike, Sky, Gradient, Clouds };

// The PORTED model draw's own vertices of one object: the model id and LOD it was drawn
// at, and world = eye + rel for each of that LOD's model vertices - what the original's GPU packets of the machines
// are made of (every vertex of the three captures' machine packets is one of them).
struct CapturedVerts {
    uint32_t model = 0;
    int lod = -1;
    double eye[3] = {};
    std::vector<float> rel;
    std::vector<uint32_t> sxy;  // the model draw's own SXY per vertex (gte_proj.h)
    std::vector<int32_t> mac3;  // ... and MAC3 (64 << exp a world unit)
    int exp = 0;
};

struct DrawRequest {
    Mat4 viewProj;
    // VR (tools/rrgame/vr_rig.h): the frustum of the PC cull when it is not viewProj's - both eyes' union, so the two
    // eyes of a stereo frame draw the same cell runs and props. Null: viewProj.
    const Mat4* cullViewProj = nullptr;
    float eye[3] = {};
    int debugMode = 0; // 0 draws the frame; 1/2/3 report; 5 coverage; 6 silhouette
    Subject subject = Subject::Props;
    bool currentRoadKnown = false;
    uint16_t currentRoad = 0;
    uint16_t currentRoadDistance = 0;
    bool haveMachine = false;
    Mat4 bikeModel;
    Mat4 riderModel;
    // The rider's 17 posed part slots (rider_pose_draw.h); nullptr draws the rest pose.
    const rr::PartMatrix* riderLocal = nullptr;
    // The bike's 5 part slots (slot 0 the identity; the fork, the pitch, the wheels: bike_pose_product.h);
    // nullptr keeps what the buffer holds.
    const rr::PartMatrix* bikeLocal = nullptr;
    // The LOD the machine is drawn at: the bike's and the rider's +0x0A + view (LodChoice 0x800667C4, which
    // the model draw's LodSelect 0x8001298C reads; lod_pose_draw.h). 0: the full models.
    int bikeLod = 0;
    int riderLod = 0;
    // The two-seat machine (race_scene_sidecar.cpp, rules.md 16): draw the sidecar rig LoadSidecar loaded
    // in place of the bike model, and - when `passenger` - a second rider object (the one seated in seat 1,
    // Attach SLUS 0x80012838(bike, rider, 2, 1)) at `passengerModel` with its own part slots and LOD.
    bool sidecar = false;
    bool passenger = false;
    Mat4 passengerModel;
    const rr::PartMatrix* passengerLocal = nullptr;
    int passengerLod = 0;
    int64_t passengerObjFlags = -1; // the passenger's +0x24 for the model light
    const rr::PartMatrix* sidecarLocal = nullptr; // the rig's own part slots (6 at LOD 0), or null: rest pose
    int sidecarRig = 0; // which rig: 0 player 1's, 1 player 2's (LoadSidecarFor, race_scene_sidecar2.cpp)
    // A rival's OWN machine (race_scene_rivals.cpp): the model ids its bike and rider objects are
    // registered with (+0x60 -> the registration's first word) and their palettes a1 (+0x24 bits 12..17). 0 / -1:
    // the player's machine and sheets (LoadMachine).
    uint32_t bikeModelId = 0, riderModelId = 0;
    uint32_t bikeObject = 0; // the bike object (its cell +0xB0 picks its ordering table, race_scene_ot.cpp); 0 unknown
    int bikeA1 = -1, riderA1 = -1;
    // The bike's and the rider's object flags +0x24 for the model light (SetModelLight): bit 11 set draws the object
    // unlit, bits 5..6 non-zero halve the ramp step (SLUS 0x800251E4). -1: the old lambert shade.
    int64_t bikeObjFlags = -1, riderObjFlags = -1;
    // The bike's and the rider's vertices from the PORTED model draw (null: the renderer poses them).
    const CapturedVerts* bikeCaptured = nullptr;
    const CapturedVerts* riderCaptured = nullptr;
    // The passenger's (the seat-1 rider object +0x40 of the rig's bike) vertices from the PORTED model draw; the
    // rig's own are `bikeCaptured` (the rig IS the bike object, model 100 + index).
    uint32_t passengerObject = 0;
    const CapturedVerts* passengerCaptured = nullptr;
    // The head camera (head_camera.h): the player rider's LOD-0 sub-meshes not drawn, a bit per sub-mesh
    // (the head in the first-person view); at a lower rider LOD a non-zero mask hides the whole rider. 0: all drawn.
    uint32_t riderHideParts = 0;
    // OURS (a camera that is not the console's: the head camera, a VR eye): the sky
    // gradient laid on a dome in WORLD directions (DrawSkyDome) instead of the console's screen-space strip, so it
    // stays level with the world while the head rolls or pitches. Its colours are the strip's, evaluated per direction.
    bool worldSky = false;
    float cellDrawRadius = 900.0f;
    // The camera the cell draw code needs for the original's per-group level of detail and the
    // band-2 road (scene_geometry.h `CellLodWord`, `CollectBand2`). Without it every group is drawn
    // coarse and band 2 is not drawn - the renderer's behaviour before render7.
    bool haveCellView = false;
    CellView cellView;
    // Draw groups the ORIGINAL culls on its 4:3 horizontal frustum coarse instead (our window is
    // wider). `rrview --roadcheck` turns it off to reproduce the console's own choice.
    bool keepCulled = true;
    // The picture's width over the console's 384 columns (>= 1): the subdividers' screen outcodes are taken
    // with x pulled toward the centre by it, so pieces the console drops off its sides stay in a wider window.
    float sideSqueeze = 1.0f;
    // The GTE projection (gte_proj.h): draw the cells, band 2 and the near polygons at the GTE's own SXY
    // (RTPS with each cell's slot RT / TR). gteMap takes a console pixel to NDC (x = sx * [0] + [1], y = sy * [2] +
    // [3]) under the SAME projection as viewProj; gteDepth the clip z of a view depth w (z = [0] w + [1]); the
    // view's GTE offset. Off (rrview, two players): the float camera only.
    bool gteProj = false;
    float gteMap[4] = {};
    float gteDepth[2] = {};
    int32_t gteOfx = 192, gteOfy = 120;
    // When set, the resident cells and the order the level-of-detail pass visits them in, as indices
    // into the cell data - the console's own resident list (`0x800D9B80`, count at gp+2268, read by
    // `SLUS 0x800358C0`) when a capture is being reproduced. Otherwise residency is the residency
    // windows and the order is nearest first.
    const std::vector<size_t>* cellOrder = nullptr;
    // Drawn between the cells and the props, with the "not the subject" debug value - the slot the
    // synthetic road ribbon and the single-model view occupy in `rrview`.
    std::function<void(int otherDebug)> afterCells;
};

class RaceScene {
public:
    // Builds the indexed-texture program. A GL context must be current.
    void CreatePrograms();

    void SetCells(const rr::TriangleSoup& soup, std::vector<CellRange> ranges);
    // The level's colour table, road page and the overlay's draw tables (DATA\GAMEBIN1.DAT bundle
    // `LevelBundleIndexForRace(raceId)`, RASHCDG.BIN, SLUS_010.53). Must run before the cell soup is
    // built, because the soup bakes the shade and texture window of every vertex from `Look()`.
    // Returns false (and leaves the look without them) when a piece is missing.
    bool LoadLevel(const rr::DiscImage& disc, int set, int raceId);
    const CellLook& Look() const { return look_; }
    CellLook& MutableLook() { return look_; }
    // The parsed cells the soup was built from, kept for the per-frame level of detail and band 2.
    // The vector must outlive the scene's use of it.
    void SetCellData(const std::vector<rr::CellData>* cells, int set) {
        cellData_ = cells;
        raceSet_ = set;
    }
    const Band2Frame& LastBand2() const { return band2_; }
    // The near subdivision (scene_geometry.h SubdivStats): what the last Draw cut up, and the switches - RRJB_AFFINE=off
    // draws everything perspective-correct without the original's subdivision (the control),
    // RRJB_SUBDIV=off keeps the affine interpolation but not the subdivision (a diagnostic).
    const SubdivStats& LastSubdiv() const { return subdivStats_; }
    bool Affine() const { return affine_; }
    bool Subdivides() const { return subdivide_; }
    const std::vector<uint32_t>& LastLodWords() const { return lodWords_; }
    // Coplanar layers (race_scene_pc.cpp): overlapping coplanar pairs of the static soup, the primitives
    // drawn again on top with the depth buffer and the deepest layer (0 until the first depth-buffer Draw).
    size_t CoplanarPairs() const { return layerPairs_.size(); }
    size_t LayeredPrimitives() const { return layeredPrims_; }
    int LayerMax() const { return layerMax_; }
    // The models' layers (coplanar.h): after a model range drawn from `layers`' soup, its winners again,
    // pulled toward the eye - only with the depth buffer (nothing in the ordering-table order).
    void DrawModelLayers(const ModelLayers& layers, size_t range) const;
    // The props' and the player's machine's pairs and winners (the report line).
    size_t ModelPairs() const { return propLayers_.Pairs() + machineLayers_.Pairs(); }
    size_t ModelWinners() const { return propLayers_.Winners() + machineLayers_.Winners(); }
    // DEVELOPMENT (tools\rrgame\zfight_probe.cpp): every decal pass - the lane lines, the cells' and the
    // models' layers - moved this many world units FARTHER along its view rays; a pixel that changes is decided by
    // less than that (the depth-fight detector's "thin" count). 0 in play.
    static void SetDecalProbeShift(float worldUnits) { decalProbeShift_ = worldUnits; }
    const rr::RoadPage& RoadPageData() const { return roadPage_; }
    void LoadCellTextures(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs,
                          const std::vector<rr::CellData>& cells, bool withBand1);
    // `hazardSet` names DATA\HAZARD<n>.GEO / .TEX (PickHazardSet for a race; rrview's default is 0).
    void LoadProps(const rr::DiscImage& disc, const std::vector<rr::CellData>& cells, bool useTextures,
                   bool texProbe, int hazardSet = 0);
    void LoadMachine(const rr::DiscImage& disc, bool useTextures,
                     const std::vector<rr::PartSlot>& bikeSlots,
                     const std::vector<rr::PartSlot>& riderSlots);
    void LoadSky(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs);
    void LoadSkyGradient(const rr::DiscImage& disc, size_t bundle);

    // Chooses the live backdrop the way `RASHCDG 0x80065580` does: among the registered panoramas
    // take the one with a residency window on the driver's road whose midpoint is nearest the
    // driver's distance along it.
    void SelectSky(bool roadKnown, uint16_t road, uint16_t roadDistance);
    // The panorama of chunk id `id` (the game's own pick, stream_product.h StreamSkyShown); false when this race's
    // panoramas hold no such id (nothing is rebound).
    bool SelectSkyId(uint32_t id);
    void BindSky(size_t which);

    // Builds this frame's sky-gradient strip and its two edge colours. Must run before `Draw`.
    void PrepareSkyGradient(const Mat4& viewProj, const float eye[3], const float target[3],
                            int32_t sunAngle);

    void Draw(const DrawRequest& request);
    // Just the machine, at the matrices the request carries: what a second rider on the same road
    // is. The scene holds one bike mesh and the renderer does not care which rider it is drawing.
    void DrawMachineOnly(const DrawRequest& request);
    // The PS1 look over the finished 3D frame in the viewport (x, y, w, h), before the HUD: every
    // pixel quantised to 15-bit colour, with the GPU's 4x4 ordered dither laid on the console's
    // 384 x 240 grid when `dither` is set. Needs only a current GL context.
    void PostProcess(int x, int y, int width, int height, bool dither);

    // ---- the PC graphics settings. All off (the defaults, rrview, every scripted
    // run): the original's frame, unchanged.
    struct PcOptions {
        bool maxDetail = false;      // every resident cell group fine, band 2's lane lines at every distance
        int drawDistance = 0;        // 0 the view's draw list (the arena's), 1 the residency windows, 2 every loaded cell
        bool cull = false;           // CPU frustum culling of the cell runs, band-2 pieces and props (bounding boxes)
        bool smoothTextures = false; // bilinear / trilinear filtering (smooth_texture.h)
        float farPlane = 20000.0f;   // the projection's far plane: draw distance 2 draws what lies within it
        bool centroid = false;       // texels looked up at the centroid (a multisampled picture; shaders.cpp vTexelPC)
    };
    void SetPcOptions(const PcOptions& o) { pc_ = o; }
    const PcOptions& Pc() const { return pc_; }
    // The texture mapping at run time (RRJB_AFFINE / RRJB_SUBDIV set the start): `affine` the GPU's screen-space
    // interpolation, `subdivide` the original's cutting of near polygons (only with affine).
    void SetAffine(bool affine, bool subdivide) {
        affine_ = affine;
        subdivide_ = affine && subdivide;
    }
    // What the last Draw drew and culled (the profiler).
    struct FrameStats {
        size_t cellRuns = 0, cellRunsCulled = 0, band2Pieces = 0, band2Culled = 0, props = 0, propsCulled = 0;
    };
    const FrameStats& LastStats() const { return stats_; }
    // For the draw paths that bind a sheet themselves (traffic_draw, ped_draw, weapon_draw): after binding `t` as
    // uIndex / uPalette, its smooth version when smooth textures are on (else the smoothing off for that draw).
    void BindSmooth(const GpuIndexedTexture& t) const;
    // The placement-record props LoadProps found (the maximum-detail draw adds those the pools do not hold).
    const std::vector<PropInstance>& RecordProps() const { return recordProps_; }
    // A sphere against the last Draw's frustum (its viewProj).
    bool SphereVisible(const float centre[3], float radius) const;
    // How many console lines the picture's height holds: 240 for the whole draw area, 224 for the part a
    // television shows (kShownH) - the dither cell and the gradient's 10-row horizon offset are console rows.
    void SetConsoleLines(float lines) { consoleLines_ = lines; }

    // ---- what a check needs to see
    const rr::IndexedTexture& PropAtlas() const { return propAtlas_; }
    bool PropTextureValid() const { return propTexture_.Valid(); }
    const std::map<uint16_t, rr::IndexedTexture>& CellAtlases() const { return cellAtlases_; }
    bool CellTexturesEmpty() const { return cellTextures_.empty(); }
    const std::vector<rr::IndexedTexture>& RiderSheets() const { return riderSheets_; }
    const std::vector<rr::IndexedTexture>& RiderSheetsAlt() const { return riderSheetsAlt_; }
    bool RiderSheetTexturesEmpty() const { return riderSheetTextures_.empty(); }
    const std::vector<ModelPart>& BikeParts() const { return bikeParts_; }
    const std::vector<PropInstance>& Props() const { return props_; }
    // For rrview --propcheck: the prop soup (group g is vertices [PropFirst()[g], + PropCount()[g]), six a
    // quad, corners 0, 1, 2 / 0, 2, 3), the instances to mutate for its negative controls, and the uNclip
    // sign the props are drawn with (kPropNclip unless a control flips it).
    const rr::TriangleSoup& PropSoup() const { return propSoup_; }
    const std::vector<GLint>& PropFirst() const { return propFirst_; }
    const std::vector<GLsizei>& PropCount() const { return propCount_; }
    std::vector<PropInstance>& MutableProps() { return props_; }
    void SetPropNclip(int sign) { propNclip_ = sign; }
    // The cell emitters' back-face test (RASHCDG 0x8006A630 / 0x8006C888 / 0x8006D350 /
    // 0x8006DC20, GTE NCLIP over (i0, i1, i2); record +0 bit 2 = two-sided): the fine near groups on the CPU on their
    // own projected corners (scene_geometry.h CellLook::nearNclip), the static soup's one-sided primitives in the shader
    // (uNclip, the props' sign). Off: every cell primitive drawn from both sides.
    void SetCellNclip(bool on, int staticSign = kPropNclip) {
        look_.nearNclip = on;
        cellNclip_ = on ? staticSign : 0;
    }
    size_t CellBackPrims() const { return subdivStats_.backPrims; }
    const rr::Panorama& SkyImage() const { return skyImage_; }
    size_t SkyBound() const { return skyBound_; }
    const std::vector<rr::ChunkHeader>& SkyHeaders() const { return skyHeaders_; }
    bool SkyGradientReady() const { return gradProgram_ != 0 && skyGradient_.valid; }
    const rr::SkyGradient& SkyGradientColours() const { return skyGradient_; }
    const rr::SkyGradient& SkyGradientAlt() const { return skyGradientAlt_; }
    // The level's sun (level_bundle.h ParseLevelSun, the bundle's type-2 section): the yaw the gradient's
    // blend is measured from, for PrepareSkyGradient's `sunAngle`.
    int32_t SunYaw() const { return sunYaw_; }
    // The level's sun VECTOR S (0x80052364/66/68, RASHCDI 0x8006250C: (-sin yaw, sin elevationAngle, -cos yaw)
    // in the EXE's own sine table, 4096 = 1.0), after LoadLevel.
    const int32_t* SunVector() const { return sunVector_; }
    // The model light for the machines drawn next: `enabled` is game_state+4 bit 4
    // clear (the original's switch), `light` the view's L' = (camera^T S) >> 12 (RASHCDG 0x80068468). Objects
    // whose request carries their +0x24 flags are then coloured by the level's model ramp (LoadLevel).
    void SetModelLight(bool enabled, const int32_t light[3]);
    // game_state+4 bit 4 clear (the emitter's lit flag, SLUS 0x80025644..0x80025674): false draws every group with
    // the unlit step, normals or not (two players).
    void SetModelLit(bool lit) { modelLit_ = lit; }
    // The same light for an object another renderer draws (traffic_draw, ped_draw, weapon_draw, the props): the rule
    // of ApplyModelLight for a group with (`lit`: DOD3 +0x28 / +0x30 present) or without normals, of DOD3 class
    // `modelClass`, with the object's +0x24 `objFlags` (-1: off). race_scene_shadow.cpp.
    void ApplyObjectLight(bool lit, int modelClass, int64_t objFlags) const;
    void ClearObjectLight() const;
    // RRJB_LOOK_LIGHT=off: the objects above keep a lambert shade (the negative control of the model light).
    static bool LookLightOff();
    // The level's ramp entry `step` (0..31) and the unlit step 0x80052380, for a check (LoadLevel).
    const float* ModelRamp(int step) const { return modelRamp_[step & 31]; }
    int32_t ModelUnlitStep() const { return modelUnlit_; }
    bool ModelLightReady() const { return modelRampValid_; }
    // The cloud layer (rrformats/sky_clouds.h): the level bundle's type-3 section, the ring of 25
    // column pairs `RASHCDI 0x80061100` builds (x = sin, z = cos of the EXE's table, radius 4096),
    // and the first of the segments this frame draws.
    // One quad of the sky layers as built for GL: four world DIRECTIONS (TR = 0; the ring's own
    // units, radius 4096) and four texel coordinates, in the GPU's corner order (top-left,
    // top-right, bottom-left, bottom-right). `rrview --skypacketcheck` projects them with the
    // capture's camera and compares them, and the texels they sample, with the original's packets.
    struct SkyQuad {
        float dir[4][3] = {};
        float tex[4][2] = {};
        int column = 0, band = 0, row = 0; // panorama: column, STEN band, drawn row; clouds: segment
    };
    const std::vector<SkyQuad>& PanoramaQuads() const { return skyQuads_; }
    const std::vector<SkyQuad>& CloudQuads() const { return cloudQuads_; } // one per segment 0..23
    // Negative controls for that check, set BEFORE the sky is loaded: 1 draws the panorama tiles
    // unrotated, 2 ignores OFFS, 3 draws cloud segment j where segment j+1 belongs, 4 gives it slice
    // j+1's texels.
    void SetSkyMutation(int mutation) { skyMutation_ = mutation; }
    // The shadow quads of the last `Draw`, on the ground, four world corners each in the quad's own
    // order q0..q3 (the packet carries them as q0, q1, q3, q2). Mutation 5 (SetSkyMutation) drops the
    // light straight down - the negative control of `rrview --shadowcheck`.
    const std::vector<std::array<float, 3>>& ShadowQuads() const { return shadowWorld_; }
    // The PORTED shadow's quads of this view (race_scene_shadow.cpp): with `active`,
    // DrawShadow draws these - once per Draw, for every machine - instead of its own approximation.
    struct PortedShadowQuad {
        float world[4][3] = {}; // q0..q3
        float colour[3] = {};
        int slot = -1; // the ordering-table slot the port linked the packet into (SetOtOrder), -1 unknown
        uint32_t object = 0; // whose shadow (its cell's table pass)
        int16_t sxy[4][2] = {}; // the packet's own screen points, q0..q3 (drawn there while the GTE projection is on)
        bool haveSxy = false;
    };
    void SetPortedShadows(std::vector<PortedShadowQuad> quads, bool active);
    size_t PortedShadowDraws() const { return portedShadowDraws_; }
    size_t CapturedDraws() const { return capturedDraws_; }  // groups drawn from the ported model draw's vertices
    size_t CapturedMissed() const { return capturedMissed_; } // captures offered but not of the model / LOD drawn
    size_t ShadowBikeQuads() const { return bikeShadow_.size() / 4; } // the first ones of each machine
    const rr::SkyClouds& Clouds() const { return skyClouds_; }
    const std::vector<std::array<int32_t, 2>>& CloudRing() const { return cloudRing_; }
    int CloudFirstSegment() const { return cloudFirst_; }
    int32_t CameraYaw() const { return cameraYaw_; }
    const uint8_t* GradMidLeft() const { return gradMidLeft_; }
    const uint8_t* GradMidRight() const { return gradMidRight_; }
    const float* RiderAttach() const { return riderAttach_; }
    // The seat the rider hangs on when the bike is drawn at LOD `lod` (SeatVertex RASHCDG 0x80066A84 inside
    // ChildPlace 0x80066B98, race_scene.cpp LoadMachine): LOD 0's for lod 0 or a LOD the model lacks.
    const float* RiderAttach(int lod) const { return (lod > 0 && lod < 4) ? riderAttachLod_[lod] : riderAttach_; }
    const float* RiderRelative() const { return riderRelative_; }
    void SetRiderRelative(const float m[9]);

    // The sidecar rig of bike index `bikeIndex` (6..8 / 15..17): model 100 + index, the RMD3 chunk of
    // DATA\<name>.MRO with the name from RASHCDI's table 0x8006B51C (CRUISES1..3 / SPORTS1..3), as the loader
    // RASHCDI 0x8005C45C / 0x8005C30C loads it for a player on such a bike. Its sheet: the page whose id is
    // the rig's DOD3+0x1C, i.e. the level bundle's (index `bundle` of rrformats/level_bank.h) LECT of that id
    // when the bundle has one (0x8005DDB8 skips an id already paged), else the MRO's own; its palette: the
    // object's, TSLP block `paletteBlock` (jail_session.h PlayerPaletteBlock; the passenger's two blocks on).
    // After LoadMachine. False (and nothing drawn in its place) when the index is not a sidecar or the file is
    // missing. race_scene_sidecar.cpp.
    bool LoadSidecar(const rr::DiscImage& disc, uint32_t bikeIndex, int bundle = 0, int paletteBlock = 0);
    bool HasSidecar() const { return !sidecarParts_.empty(); }
    uint32_t SidecarModelId() const { return sidecarModel_; }
    size_t SidecarParts() const; // the rig's LOD-0 part count (race_scene_sidecar.cpp)
    // Rig / passenger groups drawn from the PORTED model draw's vertices, and captures not of the model / LOD
    size_t SidecarCaptured() const { return sidecarCaptured_; }
    size_t PassengerCaptured() const { return passengerCaptured_; }
    size_t SidecarCaptureMissed() const { return sidecarCaptureMissed_; }
    // SeatVertex / ChildPlace's seat on the rig at LOD `lod` - the same vertex for both seated children.
    const float* SidecarAttach(int lod) const { return (lod > 0 && lod < 4) ? sidecarAttachLod_[lod] : sidecarAttachLod_[0]; }
    // Two players (race_scene_sidecar2.cpp): rig 1 is player 2's own (its index, its palette
    // block), kept aside and swapped into the members above to load and to draw; rig 0 is the one above.
    bool LoadSidecarFor(int rig, const rr::DiscImage& disc, uint32_t bikeIndex, int bundle, int paletteBlock);
    bool HasSidecarFor(int rig) const;
    uint32_t SidecarModelIdFor(int rig) const;
    size_t SidecarPartsFor(int rig) const;
    const float* SidecarAttachFor(int rig, int lod) const;
    void SwapSidecarRig();

    // The other riders' own machines (race_scene_rivals.cpp): one set per (bike model, rider model) pair of
    // BBLEVEL1.GEO, drawn by DrawMachine / DrawMachineOnly for a request with `bikeModelId` set, each object in
    // its own palette a1. After LoadMachine.
    void LoadRivalMachines(const rr::DiscImage& disc, const std::vector<std::pair<uint32_t, uint32_t>>& pairs);
    // The seat of that pair's bike at LOD `lod` (the player's machine's RiderAttach when the pair is not loaded).
    const float* RiderAttachFor(uint32_t bikeModelId, uint32_t riderModelId, int lod) const;
    size_t RivalMachinesDrawn() const { return rivalMachinesDrawn_; } // draws through a rival set, run total

    // The shared indexed-texture program, for a caller that draws geometry of its own (the road
    // ribbon, a single model) into the same frame.
    GLuint Program() const { return program_; }
    void SetModelMatrix(const Mat4& m) const;
    void SetTint(float r, float g, float b) const;
    void SetTextured(int on) const;
    void SetDebug(int mode) const;

    // ---- the ordering-table order
    // The PS1 draws the 3D scene with no depth buffer: each primitive goes into the frame's ordering table at the slot
    // its emitter's depth key picks (the models SLUS 0x800251E4: (z0 + z1 + 2 z2) / 4 of the record's first three
    // corners; the cells RASHCDG 0x8006D350 / 0x8006DC20 / 0x8006A630 / 0x8006C888 and band 2 0x8006E474: the largest
    // corner depth; the shadow 0x80025EE0: AVSZ4; all through the same slot map), and the GPU paints the table far
    // end first. With the order on, every primitive the main program draws is given its slot as its depth (shaders.cpp
    // uOtOrder), so overlaps resolve as on the console; the ported shadow is drawn after the opaque world at its own
    // slot (FlushOtShadows). Off (rrview, RRJB_OT_ORDER=zbuffer in rrgame): a z-buffer.
    void SetOtOrder(bool on) { otOrder_ = on; }
    bool OtOrder() const { return otOrder_; }
    // The frame's cells as the original orders them, and the table passes (race_scene_ot.cpp): the arena (the
    // EXE's shift table 0x80053224, the objects' cell +0xB0, and in --parity the capture's own cell depths for the
    // check), the id -> cell index map of the cells SetCellData holds, and the index of the cell the view is in
    // (*(0x800CD948 + 0x46C p), SLUS 0x800353C4), or -1.
    void SetOtFrame(const uint8_t* ram, const std::map<uint32_t, size_t>* cellIndexById, int cameraCell) {
        otRam_ = ram;
        otCellIds_ = cellIndexById;
        otCameraCell_ = cameraCell;
    }
    // The arena's own cell sort of view `view` - the PORTED 0x800353C4 / 0x80036438 /
    // 0x80035680 the session ran (cell_sort_product.h), or in --parity the capture's: the two tables, the cells' link
    // order and their draw words +0x34 come from its list 0x800D9B80 + 48 view and slot records, as 0x80035958 reads
    // them, not from the renderer's float depths (off: RRJB_CELL_SORT=off, the control).
    void SetOtArena(bool on, int view) {
        otArena_ = on;
        otView_ = view;
    }
    // The table an entity's effect packets are linked into (fx_draw.h SetOtTables): 1 / 2; -1 when its cell is in
    // neither (the original does not reach it); 0 without the arena's sort.
    int OtTableOfEntity(uint32_t e) const;
    // One ordering table of the frame: SLUS 0x80035958 draws the far cells (and the objects filed in them) through
    // one table and then the near ones through another, painted over the first; SLUS 0x80021988 sizes each from its
    // cells' depth range.
    struct OtPassMap {
        int nearOffset = 0, shift = 5, maxSlot = 0x512;
        float base = 0.5f; // the depth range it is drawn in: [0.5, 1) the first pass, [0, 0.5) the second
        bool used = false;
    };
    const OtPassMap& OtPass(int pass) const { return otPass_[pass == 2 ? 1 : 0]; }
    // The effect packets' table (fx_draw.h): the second pass's when there is one.
    const OtPassMap& OtFxPass() const { return otPass_[1].used ? otPass_[1] : otPass_[0]; }
    // Binds `vbo` - the vertex buffer of the vertex array being drawn - as the program's uOtVerts (a model's or a static
    // cell's key reads the other corners of its primitive). Every draw with Program() calls it with its buffer.
    void OtSource(GLuint vbo) const;
    // A model object's table and rank: the pass of the cell it is filed in (+0xB0), linked before that pass's cells.
    void OtObject(uint32_t obj) const;
    int OtPassOfObject(uint32_t obj) const;
    // The last plan's check against the arena's own cell depths (+0x2C / +0x30 of the cell records 0x800D87EC, which
    // SLUS 0x800353C4 / 0x80035040 write - the capture's in --parity) and its table map *(0x8005B4D4 / D8 / ADFC).
    struct OtCheck {
        size_t cells = 0, depthsEqual = 0, passes = 0, twoPassFrames = 0, frames = 0;
        int32_t worst = 0; // the largest |ours - the arena's| of a depth bound, cell units
        bool mapEqual = false;
    };
    const OtCheck& OtChecked() const { return otCheck_; }
    // Only where the arena holds the original's own values: --parity (the product does not run 0x800353C4).
    void SetOtCheck(bool on) { otCheckOn_ = on; }
    // After the view's opaque world: the ported shadow at its slots (depth-tested, not written), then the clip planes
    // off again for the programs that do not write them (the effects, the HUD, the next view's sky).
    void FlushOtShadows(const Mat4& viewProj);
    void OtEnd() const;
    size_t OtSources() const { return otSources_; }
    size_t OtShadowQuads() const { return otShadowQuads_; }
    // The GTE projection (gte_proj.cpp): the run's counters, one log line.
    std::string GteTotals() const;
    // gte_proj.cpp: `count` soup vertices `rest` of `group` (BuildTriangleSoup's order, six a
    // primitive) with each vertex at the PORTED model draw's own SXY / MAC3 of `c` under the model matrix `m`, the GPU's
    // large-polygon rejection applied; false (nothing written) when `c` is not model `model` at LOD `lod` with this
    // group's vertex count, or the GTE projection is off. The traffic cars, the props and the pedestrians.
    bool GteObjectSoup(const CapturedVerts* c, uint32_t model, int lod, const Mat4& m, const rr::ModelGroup& group,
                       const rr::TriangleSoup::Vertex* rest, size_t count, std::vector<rr::TriangleSoup::Vertex>& out) const;
    bool GteActive() const { return gteOn_; }

    // Filled by the last `Draw`, in draw order, so a reporting pass can replay exactly these.
    std::vector<const PropInstance*> drawnProps;
    std::vector<const CellRange*> drawnCells;
    size_t propsDrawn = 0;

    static constexpr size_t kNoSky = static_cast<size_t>(-1);

private:
    void BindIndexed(const GpuIndexedTexture& texture) const;
    void LoadCloudRing(const rr::DiscImage& disc);
    void RebuildSkyMesh();
    void RebuildSkyBand(); // the panorama's tiles re-laid as one continuous band (DrawRequest::worldSky)
    void LoadShadow(const rr::DiscImage& disc, const std::vector<rr::PartSlot>& bikeSlots,
                    const std::vector<rr::PartSlot>& riderSlots);
    void DrawShadow(const DrawRequest& request, int debug);
    bool DrawPortedShadows(const DrawRequest& request, int debug); // race_scene_shadow.cpp; true: handled
    // race_scene_shadow.cpp: the machine's groups rewritten from the request's captured vertices when
    // they are of the model and LOD drawn. `bikeId` / `riderId` the models the meshes hold.
    // One group from the captured vertices `c` when it is model `id` at `lod` (m0 / mL its LOD-0 / lower mesh in
    // `vbo`); false (nothing written) otherwise or with RRJB_POSE_FROM=ours. race_scene_shadow.cpp.
    bool UploadCapturedGroup(const CapturedVerts* c, uint32_t id, const Mat4& m, struct RiderPoseMesh* m0,
                             struct LodPoseMesh* mL, GLuint vbo, int lod);
    void ApplyCaptured(const DrawRequest& request, uint32_t bikeId, uint32_t riderId, struct RiderPoseMesh* bike0,
                       LodPoseMesh* bikeL, struct RiderPoseMesh* rider0, LodPoseMesh* riderL, GLuint vbo, int bikeLod,
                       int riderLod);
    void DrawClouds(const DrawRequest& request, int debug);
    void DrawSkyDome(const DrawRequest& request); // DrawRequest::worldSky
    void DrawMachine(const DrawRequest& request, int bikeDebug, int otherDebug);
    void DrawSidecar(const DrawRequest& request, int bikeDebug, int otherDebug, int bikeLod); // race_scene_sidecar.cpp
    bool DrawRivalMachine(const DrawRequest& request, int bikeDebug, int otherDebug); // race_scene_rivals.cpp
    const GpuIndexedTexture* RivalSheet(uint16_t texId, bool rim, int a1);
    std::vector<std::shared_ptr<struct RivalMachine>> rivalMachines_;
    std::map<uint64_t, GpuIndexedTexture> rivalSheets_;
    std::vector<uint8_t> rivalTex_, rivalRim_;
    size_t rivalMachinesDrawn_ = 0;

    GLuint sidecarVao_ = 0, sidecarVbo_ = 0;
    std::vector<ModelPart> sidecarParts_;
    std::vector<GpuIndexedTexture> sidecarSheets_; // 0 the rig, 1 its rim, 2 the passenger rider
    std::shared_ptr<struct RiderPoseMesh> sidecarPose_; // LOD 0 of the rig, re-posed from its part slots
    size_t sidecarPoseParts_ = 0;
    float sidecarAttachLod_[4][3] = {};
    uint32_t sidecarModel_ = 0;
    std::shared_ptr<struct LodPoseMesh> sidecarLod_[4]; // LODs 1..3 of the rig, for the model draw's vertices
    size_t sidecarCaptured_ = 0, passengerCaptured_ = 0, sidecarCaptureMissed_ = 0;
    bool sidecarLodDirty_[4] = {};
    std::shared_ptr<struct SidecarRigSlot> sidecarRig2_; // rig 1 (race_scene_sidecar2.cpp: SwapSidecarRig swaps ALL the sidecar*_ above)

    GLuint program_ = 0;
    GLint viewProjLocation_ = -1, modelLocation_ = -1, tintLocation_ = -1, texturedLocation_ = -1;
    GLint indexLocation_ = -1, paletteLocation_ = -1, texSizeLocation_ = -1;
    GLint paletteCountLocation_ = -1, paletteSizeLocation_ = -1;
    GLint debugLocation_ = -1, debugIdLocation_ = -1;
    GLint nclipLocation_ = -1; // uNclip: the model emitter's back-face test (shaders.cpp)
    // the ordering-table order (SetOtOrder)
    bool otOrder_ = false;
    const uint8_t* otRam_ = nullptr;
    const std::map<uint32_t, size_t>* otCellIds_ = nullptr;
    int otCameraCell_ = -1;
    bool otArena_ = false;
    int otView_ = 0;
    // The arena's draw words +0x34 of the listed cells into lodWords_; false without the arena's sort.
    bool OtArenaLodWords();
    void OtPlanArena();
    OtPassMap otPass_[2];
    std::vector<int8_t> otCellPass_;  // per cell index: 0 not listed, 1 / 2 the pass
    std::vector<int16_t> otCellSeq_;  // the order the pass links it in (0 first, i.e. the farthest)
    OtCheck otCheck_;
    bool otCheckOn_ = false;
    // The frame's plan: the cells' depth range (0x80035040), the camera's cell first and the rest by depth
    // (0x800353C4, 0x80036438), the pass of each (0x80035958) and each pass's table (0x80021988).
    void OtPlan(const std::vector<size_t>& visit, const CellView& view);
    // The table and rank of a cell primitive of region-2 group `group` (a lane line when `line`).
    void OtCell(size_t cell, int group, bool line) const;
    void OtUsePass(const OtPassMap& map, int rank) const;
    GLint otOrderLocation_ = -1, otVertsLocation_ = -1, otStrideLocation_ = -1, otRankLocation_ = -1;
    GLint otNearLocation_ = -1, otShiftLocation_ = -1, otMaxLocation_ = -1, otBaseLocation_ = -1;
    GLint shadowOtOrderLocation_ = -1, shadowOtDepthLocation_ = -1;
    mutable std::map<GLuint, GLuint> otTextures_; // vertex buffer -> its buffer texture
    bool otFlushing_ = false; // FlushOtShadows is drawing the deferred shadow
    void OtPrograms();        // race_scene_ot.cpp: the uniforms, after the program is built
    void OtBeginDraw();       // the frame's slot map into the program, the depth test and the clip planes
    mutable size_t otSources_ = 0;
    size_t otShadowQuads_ = 0;
    GLint affineLocation_ = -1; // uAffine: screen-space texels and colours (the near subdivision)
    bool affine_ = true, subdivide_ = true;
    SubdivStats subdivStats_;
    std::vector<CellRange> nearRanges_; // this frame's fine near groups, drawn from nearVao_
    GLuint nearVao_ = 0, nearVbo_ = 0;
    // The GTE projection (gte_proj.cpp): the static soup's screen buffer (attribute 7) and its buffer texture, each soup
    // vertex's cell vertex, this frame's vertex passes per cell, the per-frame soups' screen buffers.
    void GtePrograms();
    void GteKeepSoup(const rr::TriangleSoup& soup);
    void GteIndexSoup();
    void GteBegin(const DrawRequest& request);
    const GteCell* GteCellOf(size_t cell);
    void GteFillRange(const CellRange& range);
    void GteStaticSource(bool on);
    void GteEnd();
    // The machines' captured vertices at the model draw's own SXY (gte_proj.cpp): each model-space point in `pos` whose
    // SXY the GTE could place is moved to the point the frame's projection maps onto that SXY at its MAC3.
    void GteModelPoints(const CapturedVerts& c, const Mat4& m, std::vector<float>& pos) const;
    Mat4 gteViewProj_;
    float gteMap_[4] = {}, gteDepth_[2] = {};
    mutable size_t gteModelVertices_ = 0, gteModelFloat_ = 0;
    // The objects drawn from their captures / offered one of another model or LOD, triangles the GPU rule
    // dropped (every model path), vertices drawn at a saturated SXY
    mutable size_t gteObjDraws_ = 0, gteObjMissed_ = 0, gteSaturated_ = 0;
    GLuint gteStreamVao_ = 0, gteStreamVbo_ = 0; // the props' per-draw soup (gte_proj.cpp GteStream*)
    // The world point the frame's projection maps onto console pixel (sx, sy) (+ uGtePix) at view depth w
    // (world units) - inverse(gteViewProj_), made by GteBegin
    bool GteScreenToWorld(double sx, double sy, double w, float out[3]) const;
    double gteInv_[16] = {};
    bool gteInvOk_ = false;
    mutable size_t gteShadowQuads_ = 0;
    std::vector<rr::ModelGroup> propGroups_;     // LoadProps: each group of the prop model, for GteObjectSoup
    uint32_t propModelId_ = 0;
    bool gteOn_ = false;
    float gtePix_ = 0.5f, gteRound_ = 1.0f / 256.0f;
    GLint texRoundLocation_ = -1;
    int32_t gteOfx_ = 192, gteOfy_ = 120;
    GLint gteLocation_ = -1, gteMapLocation_ = -1, gteDepthLocation_ = -1, gtePixLocation_ = -1;
    GLint otScreenLocation_ = -1, otScreenOnLocation_ = -1;
    std::vector<float> gteSoupPos_;
    std::vector<uint16_t> gteSoupIndex_;
    size_t gteSoupMissing_ = 0;
    GteScreens gteScreenFill_;
    std::vector<uint32_t> gteSlotOf_;
    std::vector<GteCell> gteCells_;
    std::vector<char> gteCellDone_;
    GLuint gteCellScreenVbo_ = 0, gteCellScreenTex_ = 0;
    bool gteScreenStale_ = false;
    GLuint gteNearScreenVbo_ = 0, gteRoadScreenVbo_ = 0, gteLineScreenVbo_ = 0;
    GteStats gteStats_;

    CellLook look_;
    rr::RoadPage roadPage_;
    const std::vector<rr::CellData>* cellData_ = nullptr;
    int raceSet_ = 1;
    std::vector<uint32_t> lodWords_;
    Band2Frame band2_;
    CellRange band2Range_;
    GLuint band2Vao_ = 0, band2Vbo_ = 0, lineVao_ = 0, lineVbo_ = 0;
    GLuint cellVao_ = 0, cellVbo_ = 0;
    GLsizei cellVertexCount_ = 0;
    std::vector<CellRange> cellRanges_;
    std::map<uint16_t, GpuIndexedTexture> cellTextures_;
    std::map<uint16_t, rr::IndexedTexture> cellAtlases_;

    GLuint propVao_ = 0, propVbo_ = 0;
    std::vector<GLint> propFirst_;
    std::vector<GLsizei> propCount_;
    std::vector<ModelPart> propLight_; // per group: the model light's inputs (SetPartLight)
    std::vector<PropInstance> props_;
    rr::TriangleSoup propSoup_;
    int propNclip_ = kPropNclip;
    int cellNclip_ = 0;
    GpuIndexedTexture propTexture_;
    rr::IndexedTexture propAtlas_;

    GLuint bikeVao_ = 0, bikeVbo_ = 0;
    std::shared_ptr<struct RiderPoseMesh> riderPose_; // rider_pose_draw.h: the rider's range of bikeVbo_
    std::shared_ptr<struct RiderPoseMesh> bikePose_;  // the same for the bike's range
    std::shared_ptr<LodPoseMesh> bikeLod_[4], riderLod_[4]; // LODs 1..3 of both (lod_pose_draw.h)
    std::vector<ModelPart> bikeParts_;
    std::vector<rr::IndexedTexture> riderSheets_;
    std::vector<rr::IndexedTexture> riderSheetsAlt_;
    std::vector<GpuIndexedTexture> riderSheetTextures_;
    float riderAttach_[3] = {0.0f, 0.0f, 0.0f};
    float riderAttachLod_[4][3] = {};
    // How the rider object sits on the bike object, as a 3x3 in the MODEL frame. Without a
    // measured pose this is the axis permutation rrview has been guessing (rider +X is up, +Z is
    // lateral), the only assignment that puts the head above the tank.
    float riderRelative_[9] = {0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

    struct SkyChunk {
        rr::ChunkHeader header;
        std::vector<uint8_t> bytes;
    };
    std::vector<SkyChunk> skyChunks_;
    std::vector<rr::ChunkHeader> skyHeaders_;
    rr::MdecCodebook skyBook_;
    GLuint skyProgram_ = 0, skyVao_ = 0, skyVbo_ = 0, skyTexture_ = 0;
    GLsizei skyVertexCount_ = 0;
    GLint skyViewProjLocation_ = -1, skyTexLocation_ = -1, skySizeLocation_ = -1, skyDebugLocation_ = -1;
    GLint skyWorldLocation_ = -1; // DrawRequest::worldSky: at infinity, the band's texels filtered
    // RebuildSkyBand: every tile turned and put where the cylinder draws it, in one image whose texel
    // neighbours are the neighbours on the sky (column c at x 16 c, grid row r at y 16 (r - first row)), and one grid of
    // quads over it sharing every edge - no seams, no reads of an unrelated tile, filtered like the world's textures.
    GLuint skyBandVao_ = 0, skyBandVbo_ = 0, skyBandTexture_ = 0;
    GLsizei skyBandVertexCount_ = 0;
    int skyBandWidth_ = 0, skyBandHeight_ = 0, skyBandFilter_ = -1;
    size_t skyBound_ = kNoSky;
    rr::Panorama skyImage_;
    std::vector<int16_t> sinCos_; // SLUS 0x8005624C, {sin, cos}[4096]

    GLuint gradProgram_ = 0, gradVao_ = 0, gradVbo_ = 0;
    GLint gradTopLocation_ = -1, gradHorizonLocation_ = -1, gradMidLeftLocation_ = -1,
          gradMidRightLocation_ = -1, gradDebugLocation_ = -1;
    rr::SkyGradient skyGradient_, skyGradientAlt_;
    int32_t sunYaw_ = 0, sunElevation_ = 123; // ParseLevelSun; 123 = every race capture's, if a bundle has none
    int32_t sunVector_[3] = {0, 0, 0};
    float modelRamp_[32][3] = {};
    int32_t modelUnlit_ = 12;
    bool modelRampValid_ = false, modelLightOn_ = false;
    bool modelLit_ = true; // SetModelLit
    int32_t modelLight_[3] = {0, 0, 0};
    GLint modelLightLocation_ = -1, lightXLocation_ = -1, lightYLocation_ = -1, lightZLocation_ = -1;
    GLint rampShiftLocation_ = -1, unlitLocation_ = -1, rampLocation_[32] = {};
    // Sets the model-light uniforms for one run of an object with these +0x24 flags (-1: off).
    void ApplyModelLight(const ModelPart& part, int64_t objFlags) const;
    uint8_t gradMidLeft_[3] = {}, gradMidRight_[3] = {};
    GLsizei gradVertexCount_ = 0;
    int32_t gradSunAngle_ = 0; // PrepareSkyGradient's sunAngle (the dome's colours)
    // the world sky dome (DrawSkyDome): built once per sun angle
    GLuint domeProgram_ = 0, domeVao_ = 0, domeVbo_ = 0;
    GLint domeViewProjLocation_ = -1;
    GLsizei domeVertexCount_ = 0;
    int32_t domeSunAngle_ = -1;
    bool domeFailed_ = false;
    float consoleLines_ = 240.0f; // SetConsoleLines

    rr::SkyClouds skyClouds_;
    std::vector<std::array<int32_t, 2>> cloudRing_; // 25 points: (x, z)
    GLuint cloudProgram_ = 0, cloudVao_ = 0, cloudVbo_ = 0, cloudTexture_ = 0;
    GLint cloudViewProjLocation_ = -1, cloudTexLocation_ = -1, cloudDebugLocation_ = -1;
    GLint cloudWorldLocation_ = -1, cloudRectLocation_ = -1; // DrawRequest::worldSky
    int cloudFirst_ = 0;
    int32_t cameraYaw_ = 0;
    std::vector<SkyQuad> skyQuads_, cloudQuads_;
    int skyMutation_ = 0;

    // The shadow (race_scene.cpp DrawShadow): the `quadsD` lists of the bike and the rider, four
    // model-space corners per quad in the same space as the machine's draw soup.
    std::vector<std::array<float, 3>> bikeShadow_, riderShadow_;
    float bikeGroundY_ = 0.0f; // the lowest point of the bike (model y is down)
    std::vector<std::array<float, 3>> shadowWorld_;
    CellView lastCellView_; // the camera of the last Draw, for DrawMachineOnly's shadows
    GLuint shadowProgram_ = 0, shadowVao_ = 0, shadowVbo_ = 0;
    GLint shadowViewProjLocation_ = -1, shadowColourLocation_ = -1, shadowDebugLocation_ = -1;
    std::vector<PortedShadowQuad> portedShadows_; // SetPortedShadows
    bool portedShadowsActive_ = false, portedShadowsDrawn_ = false;
    size_t portedShadowDraws_ = 0;
    size_t capturedDraws_ = 0, capturedMissed_ = 0;

    // The PS1 look (PostProcess).
    GLuint postProgram_ = 0, postVao_ = 0, postTexture_ = 0;
    GLint postFrameLocation_ = -1, postDitherLocation_ = -1, postOriginLocation_ = -1, postScaleLocation_ = -1;
    int postWidth_ = 0, postHeight_ = 0;

    // the PC graphics settings (SetPcOptions; race_scene_pc.cpp)
    PcOptions pc_;
    FrameStats stats_;
    std::vector<PropInstance> recordProps_;
    float frustum_[6][4] = {};                   // the last Draw's planes (a x + b y + c z + d >= 0 inside)
    std::vector<std::array<float, 6>> rangeBox_; // per cell run: min xyz, max xyz (world units)
    std::vector<std::array<float, 6>> cellBox_;  // per cell: the union of its runs
    GLint smoothLocation_ = -1, smoothTexLocation_ = -1, smoothLayerLocation_ = -1;
    GLint centroidLocation_ = -1; // uCentroidTexel
    void PcPrograms();                     // the uniforms (CreatePrograms)
    void PcBoxes(const rr::TriangleSoup& soup); // rangeBox_ / cellBox_ and the palette rows of each page (SetCells)
    void PcFrustum(const Mat4& viewProj);  // frustum_
    bool BoxVisible(const std::array<float, 6>& box) const;
    // Maximum detail's static band 2: every fine group's strips and lines built once with the near path and the far
    // template (CollectBand2 Band2Options), drawn per (cell, group) where no dynamic near build replaces it.
    struct StaticBand2Piece {
        size_t cell = 0;
        int group = 0;
        GLint roadFirst = 0, lineFirst = 0;
        GLsizei roadCount = 0, lineCount = 0;
    };
    std::vector<StaticBand2Piece> staticBand2_;
    std::map<std::pair<size_t, int>, size_t> staticBand2Index_;
    GLuint staticRoadVao_ = 0, staticRoadVbo_ = 0, staticLineVao_ = 0, staticLineVbo_ = 0;
    bool staticBand2Built_ = false, staticBand2Full_ = false;
    void BuildStaticBand2();
    GLuint pcRoadVao_ = 0, pcRoadVbo_ = 0, pcLineVao_ = 0, pcLineVbo_ = 0; // maximum detail's per-frame near band 2
    // Maximum detail's band 2 (race_scene_pc.cpp): the near groups built per frame, the rest from the static band.
    void PcDrawBand2(const DrawRequest& request, const std::vector<char>& isResident, int cellsDebug, int otherDebug);
    std::vector<float> propRadius_; // per prop group: the bounding radius, world units (the props' cull)
    std::map<uint16_t, std::vector<int>> pageRows_; // per cell page: the palette rows its runs select (smooth textures)
    std::vector<char> pcResident_; // the cells the last Draw drew with the PC settings' draw distance (its record props)
    bool pageRowsNoted_ = false;
    // Coplanar layers (race_scene_pc.cpp): primitives of the static soup that lie in the plane of
    // another primitive they overlap (a decal, a face drawn twice with two mappings). The console orders them by its
    // table; a depth buffer mixes them pixel by pixel. With the depth buffer the winner of each pair is drawn again
    // after its run, from an element buffer, with a polygon offset per layer.
    struct LayerPrim {
        size_t run = 0;
        GLint first = 0;
        int size = 0;
        float area = 0.0f;
        float n[3] = {}; // its first triangle's unit normal (the lift)
    };
    struct LayerDraw {
        int layer = 0;
        GLsizei first = 0, count = 0; // indices into layerEbo_
    };
    std::vector<LayerPrim> layerPrims_;                  // the primitives in some coplanar pair (SetCells)
    std::vector<std::pair<uint32_t, uint32_t>> layerPairs_; // pairs of layerPrims_ indices that overlap
    std::vector<float> layerPairMisfit_;                  // per pair: how far apart their planes lie (coplanar.h)
    GLuint layerLiftVbo_ = 0;                             // the winners' lifts, attribute 8 of cellVao_
    std::vector<std::vector<LayerDraw>> runLayers_;       // per cell run: its layers' element ranges
    GLuint layerEbo_ = 0;
    bool layersBuilt_ = false;
    size_t layeredPrims_ = 0;
    int layerMax_ = 0;
    void PcLayers(const rr::TriangleSoup& soup); // the pairs (SetCells)
    void PcBuildLayers();                         // the layers and the element buffer (first depth-buffer Draw)
    // after run `run`'s own draw, depth-buffer order only; `eyeClipZ` the view's clip z of the eye (shaders.cpp uLayerPull)
    void PcDrawLayers(size_t run, float eyeClipZ) const;
    GLint layerPullLocation_ = -1;
    // Decal passes with the depth buffer (race_scene_pc.cpp): `pull` world units toward the eye along the
    // view rays plus `steps` polygon offset steps; `writeDepth` off for a decal whose plate is already drawn (the lane
    // lines over the road, a model's winners over its own losers), so what is drawn later meets the plate's depth.
    void BeginDecal(float pull, float steps, bool writeDepth) const;
    void EndDecal() const;
    void BeginLines() const; // band 2's lane lines: a decal pass with the depth buffer, the polygon offset in the table order
    float eyeClipZ_ = 0.0f; // this Draw's clip z of the eye (shaders.cpp uLayerPull)
    float eyeWorld_[3] = {}; // this Draw's eye (shaders.cpp uLayerEye: the rays the lifts are measured along)
    GLint layerEyeLocation_ = -1;
    ModelLayers propLayers_, machineLayers_;
    static inline float decalProbeShift_ = 0.0f;
};

// The bike's model matrix on the road: model X along the road's lateral axis, model Y down, model Z
// along the tangent. `scale` is the model-to-world factor; MachineMatrix lifts the origin one unit
// along `up` (the viewer's legibility offset), MachineMatrixAt puts it exactly at `origin`.
Mat4 MachineMatrix(const RoadFrame& frame, const float up[3], const float position[3], float scale);
Mat4 MachineMatrixAt(const RoadFrame& frame, const float up[3], const float origin[3], float scale);

// The model-to-world scale, derived: the original draws an exponent-4
// model with its .GEO vertices fed straight to the GTE and the object's
// translation in the same unit, and that translation is the camera-frame offset of the entity's
// +0xB8 in world units times 1024 - measured in the `rr-race` trace: model 100's TR (0, 912, 3440)
// against +0xB8 - eye = (-0.005, 1.085, 3.362) world units in the camera frame (y scaled by the
// aspect row 3412/4096). One model unit is 1/1024 of a world unit.
constexpr float kModelUnitsPerWorldUnit = 1024.0f;

// The original's projection: the GTE's H = 237 (`RASHCDI 0x8005CF1C`
// stores it in the view projection record 0x800D82B0 + 4, and it is 237 in all 8912 COP2 records of
// the rr-race trace), screen offset (192, 120) on a 384 x 240 frame, and the camera matrix's second
// row scaled by 3412/4096 (every camera-derived RT in the trace; the record's +0x5C copy too). The
// vertical field is therefore 2 atan(120 / (237 * 3412 / 4096)) = 62.6 degrees and the horizontal
// 2 atan(192 / 237) = 78.0 degrees, i.e. a 4:3 picture.
constexpr float kGteH = 237.0f;
constexpr float kGteAspectRow = 3412.0f / 4096.0f;
inline float OriginalVerticalFov() { return 2.0f * std::atan(120.0f / (kGteH * kGteAspectRow)); }
// The part of the 384 x 240 draw area a television shows: every race capture's GPU state (gpu.json crtc)
// shows x 9..373 (365 pixels) and y 8..231 (224 lines). The HUD overlay and the split views lay exactly this
// rectangle over the picture, so the one-player 3D frame must too: with the
// whole 240 lines in the picture the world would be drawn ~5 % smaller than the HUD and than the original shows it.
constexpr float kShownX = 9.0f, kShownY = 8.0f, kShownW = 365.0f, kShownH = 224.0f;
// The vertical field of the shown lines, and how much wider than 4:3 its 365 columns are (the television
// shows the rectangle as 4:3; this keeps a console pixel under the same screen point as the HUD's).
inline float ShownVerticalFov() { return 2.0f * std::atan(kShownH * 0.5f / (kGteH * kGteAspectRow)); }
inline float ShownAspectStretch() {
    return (kShownW * 0.5f / kGteH) / (kShownH * 0.5f / (kGteH * kGteAspectRow)) / (4.0f / 3.0f);
}
// The shown rectangle's centre is half a column left of the GTE's (192, 120): the NDC x shift at 4:3.
inline float ShownCentreShiftNdc() { return (192.0f - (kShownX + kShownW * 0.5f)) / (kShownW * 0.5f); }
// The rider's matrix, hung off the bike's attachment vertex through `riderRelative`.
Mat4 RiderMatrix(const Mat4& bike, const float attach[3], const float riderRelative[9]);

} // namespace rr::render
