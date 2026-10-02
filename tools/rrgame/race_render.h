#pragma once
// The race frame's drawing of ONE view, out of rrgame's main loop.
//
// `RaceRenderer::RenderView` draws everything the player sees of one view - sky, cells, road, props, the machines,
// the traffic, the pedestrians, the shadows and the effect packets - from a `GameView`: a camera (view and
// projection matrices plus the eye frame they were made of), the framebuffer and viewport to draw into, and which
// player's game state (view record, draw list, captures) the frame belongs to. It does not clear the target, draw
// the HUD, post-process or swap: the caller owns the frame. `GameCamera` builds the view the game itself draws for
// player `vp` (the PORTED camera's render record, the original's projection, the split-screen rectangle) - rrgame
// calls the two once per player and gets exactly the frame it drew before this file existed (the parity gates).
//
// FOR A STEREO (VR) CALLER: take `GameCamera(0, ...)`, then per eye replace `eye / target / up`, `view` (must be
// LookAt(eye, target, up): the cell level of detail and culling read the eye frame), `proj` (any, asymmetric
// frusta included), `nearPlane / farPlane`, `framebuffer` and `viewport`, and set `consoleCamera = false`. Then the
// passes that only exist in the console's own screen space are left out or replaced: the GTE screen points (the
// float camera is used), the PORTED sky packets (the renderer's own sky cylinder, cloud ring and gradient dome are drawn
// in world directions instead), the effect packets (taken back into the world through the console's render camera and
// drawn as billboards: FxDraw::DrawWorld) and the cell cull's widening. Single-pass stereo: the caller sets
// render/multiview.h SetStereoViewProj around the call and passes the mid eye's view. The
// view matrix and projection are used as given for everything else. Nothing here relies on GL features outside
// OpenGL ES 3.2 beyond what RaceScene already uses.
#include "render/fx_draw.h"
#include "render/mat4.h"
#include "render/ped_draw.h"
#include "render/race_scene.h"
#include "render/traffic_draw.h"
#include "render/weapon_draw.h"
#include "vr_visual_lean.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rr {
struct RaceWorld;
namespace game {
class RaceSession;
class FxRuntime;
class ModelRuntime;
struct RiderPoseView;
} // namespace game
} // namespace rr

namespace rrgame {

// ---- helpers shared with main.cpp (moved from it)
// RRJB_BIKE_FRAME=ground: the machines on the ground frame (the negative control).
bool BikeFrameFromGround();
// RRJB_RIVALS=player: every rival drawn as the player's machine (the negative control).
bool RivalsAsPlayer();
// An object's registered model id and palette a1 (race_scene_rivals.cpp).
void ObjectLook(const uint8_t* ram, uint32_t obj, uint32_t& modelId, int& a1);
// The frames drawn from the render camera record, and those that fell back to the view record.
extern size_t g_renderCameraFrames, g_renderCameraFallbacks;

// where the riders were drawn (game/rider_pose.h RiderOwnFrame / MachineClimbSlots), view 0, and the
// player's posed rider against the PORTED model draw's own vertices of it (model 150 at LOD 0, when captured) per
// placement: on the seat riding, off the bike, on the seat in the climb; `pre` = the same frames placed the pre-1cp way
// (always on the seat, the bike never turned).
struct RiderPlacement {
    size_t playerOwn = 0, playerClimb = 0, rivalOwn = 0, rivalClimb = 0, rivalsByRider = 0, passengerOwn = 0;
    double playerMaxFromSeat = 0.0; // world units: the drawn rider's origin from where the seat would have put him
    size_t playerFalls = 0, playerRun = 0, playerLongest = 0; // stretches off the bike, the current one, the longest (frames)
    long playerLastOff = -2;                                  // the last frame drawn off the bike
    struct Check {
        size_t n = 0;
        double sum = 0.0, max = 0.0, preSum = 0.0, preMax = 0.0; // per-frame RMS over the vertices, world units
    } seated, off, climb;
};
RiderPlacement& RiderPlacementStats();
std::string RiderPlacementLine(); // the race log's `rider placement` line

// One view of the race frame: the camera and where it is drawn.
struct GameView {
    int player = 0;                         // whose view record, draw list and captures (0, 1)
    rr::render::Mat4 view, proj;            // the camera; `view` = LookAt(eye, target, up)
    float eye[3] = {}, target[3] = {}, up[3] = {};
    float fovY = 0.0f, aspect = 1.0f;       // the vertical field and the picture's aspect `proj` was made with
    float nearPlane = 0.25f, farPlane = 20000.0f;
    bool consoleCamera = true;              // view / proj are the game's own camera of `player` (see the top)
    bool shownView = false;                 // one player: the shown 365 x 224 part of the draw area (race_scene.h kShown*)
    bool split = false;                     // two players: the view's rectangle of VIEWS.VI
    int16_t splitRect[4] = {0, 0, 384, 240};
    unsigned framebuffer = 0;               // the GL framebuffer drawn into (0: the window's)
    int viewport[4] = {0, 0, 0, 0};         // x, y (GL: bottom-up), width, height in that framebuffer
    // The head camera (head_camera.h): the player rider's LOD-0 sub-meshes not drawn (race_scene.h
    // DrawRequest::riderHideParts), and the player's machine posed by the renderer instead of from the model draw's
    // captured vertices (which sit a frame behind a camera that rides the head).
    uint32_t riderHideParts = 0;
    bool playerFromCaptures = true;
    // VR (game_host_vr.h): the frustum the PC cull uses instead of view / proj's - one frustum for both eyes, so the
    // pair draws the same cells, props and objects (vr_rig.h UnionViewProj)
    rr::render::Mat4 cullViewProj;
    bool haveCullViewProj = false;
    // VR head view (vr_visual_lean.h): the player's own bike drawn with a share of the original's roll -
    // its model matrix turned about the wheels' contact line (the rider on it follows); off: as the record has it
    VisualLean playerLean;
    // VR head view, the bars in the hands: the player's fork slot drawn turned to the hands' angle
    // (VrHandlebars::ForkOverride) instead of the game's own
    bool haveForkOverride = false;
    rr::PartMatrix playerFork;
};

class RaceRenderer {
public:
    struct Options {
        bool drawFx = true;             // --no-fx: the effect packets are not drawn
        bool drawRangeOff = false;      // --draw-range-off (the rivals' draw range's negative control)
        bool propsFromRecords = false;  // --props-from-records (the cell walker's negative control)
        bool parity = false;            // --parity: the capture's frame on the whole draw area
        bool shownOff = false;          // RRJB_SHOWN=off
        bool sunOff = false;            // RRJB_SUN=off
        bool modelLightOff = false;     // RRJB_MODEL_LIGHT=off
        bool rigOff = false;            // RRJB_RIG=off
        bool wideScreen = false;        // the picture is wider than 4:3 (F2 / --wide)
        bool otOrder = true;            // the ordering-table order on the game's camera (RRJB_OT_ORDER, the settings)
        // The PC graphics settings (graphics_settings.h): the float camera instead of the GTE's screen points, and
        // maximum detail - every model at LOD 0, no draw ranges, the objects of every drawn cell (frustum-culled).
        bool preciseVertices = false;
        bool maxDetail = false;
        bool cull = false; // the objects' bounding spheres against the view's frustum
    };
    // The run's counters the race report prints (they were locals of RaceMain).
    struct Counters {
        size_t modelLightViews = 0, otViews = 0;
        size_t worldPropFrames = 0, worldPropsMax = 0, carsPastRange = 0;
        size_t trafficDrawn = 0, trafficUnknown = 0, trafficMaxDrawn = 0, trafficFramesWithCars = 0;
        size_t rivalsVanishedInAir = 0, rivalFramesPastRange = 0;
        size_t fxDrawnMax = 0, fxFramesDrawn = 0;
        size_t rigPosed = 0, rigMoved[8] = {};
        rr::render::SubdivStats subdiv;
        std::string vanishNote; // the frame's VANISH lines (taken by the frame log)
        size_t objectsDrawn = 0, objectsCulled = 0; // this frame's machines, cars and pedestrians (the profiler; reset by the caller)
        size_t recordPropsAdded = 0;               // maximum detail: placement-record props drawn (run total)
    };

    RaceRenderer(rr::game::RaceSession& session, const rr::RaceWorld& world, rr::render::RaceScene& scene,
                 rr::render::TrafficDraw& traffic, rr::render::PedDraw& peds, rr::render::WeaponDraw& weapons,
                 rr::game::FxRuntime& fx, rr::game::ModelRuntime& models, rr::render::FxDraw& fxDraw,
                 const std::map<uint32_t, size_t>& cellIndexById, const Options& options);

    // The view the game draws for player `vp` inside the picture rectangle (x, y, w, h) of framebuffer `fb`.
    GameView GameCamera(int vp, int x, int y, int w, int h, unsigned fb = 0) const;
    // Recomputes `view` and `proj` from the eye frame, the field, the aspect and the planes (after a caller moved
    // the eye or the near plane: the head camera), the shown-part shift included.
    void Reproject(GameView& view) const;
    // Draws one view (see the top of this file). `frame` is the frame counter (traces, the VANISH lines).
    void RenderView(const GameView& view, long frame);
    // Called with the player rider's captured vertices of view 0 (the head camera's oracle, head_camera.h).
    std::function<void(const rr::render::CapturedVerts&)> onPlayerRiderCaptured;
    // The player's machine exactly as the last RenderView of player 0 handed it to the scene: its model matrix
    // (DrawRequest::bikeModel, the lean included) and the part slots it was posed with (DrawRequest::bikeLocal). VR
    // puts the held hands on the grips of THIS bike - no second computation of the frame to drift from it.
    struct DrawnMachine {
        bool valid = false;         // a single-seat bike drawn this frame (the two-seat rig: false)
        bool posed = false;         // `parts` holds the slots (false: the rest pose)
        long frame = -1;
        rr::render::Mat4 model;
        rr::PartMatrix parts[5];
    };
    DrawnMachine drawnPlayerMachine;

    Options options;
    Counters counters;

private:
    rr::game::RaceSession& session_;
    const rr::RaceWorld& world_;
    rr::render::RaceScene& scene_;
    rr::render::TrafficDraw& traffic_;
    rr::render::PedDraw& peds_;
    rr::render::WeaponDraw& weapons_;
    rr::game::FxRuntime& fx_;
    rr::game::ModelRuntime& models_;
    rr::render::FxDraw& fxDraw_;
    const std::map<uint32_t, size_t>& cellIndexById_;
    std::vector<char> rivalDrawnLast_, rivalAirLast_;
    rr::PartMatrix rigLast_[8] = {}; // the sidecar rig's last part slots, for the moved counters
};

} // namespace rrgame
