// The race frame's drawing of one view (race_render.h), per view in RaceMain's frame loop. The statements keep the
// frame loop's order; the parity gates and the reference shots compare the frames byte for byte.
#include "race_render.h"
#include "vr_comfort.h" // the shadows of a frame drawn between two steps

#include "game/anim_detail.h"
#include "game/bike_pose_product.h"
#include "game/cell_sort_product.h"
#include "game/cell_view.h"
#include "game/fx_runtime.h"
#include "game/hazard_product.h"
#include "game/head_camera.h"
#include "game/model_runtime.h"
#include "game/mp2_product.h"
#include "game/peds_product.h"
#include "game/race_session.h"
#include "game/rider_pose.h"
#include "game/shadow_product.h"
#include "game/sky_product.h"
#include "game/stream_product.h"
#include "game/world.h"
#include "game/world_pop_product.h"
#include "render/rider_pose_draw.h"
#include "render/scene_geometry.h"
#include "render/sky_gpu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

using namespace rr::render;

namespace rrgame {

size_t g_renderCameraFrames = 0, g_renderCameraFallbacks = 0;

RiderPlacement& RiderPlacementStats() {
    static RiderPlacement s;
    return s;
}

std::string RiderPlacementLine() {
    const RiderPlacement& s = RiderPlacementStats();
    const auto check = [](const char* what, const RiderPlacement::Check& k) {
        char b[260];
        if (k.n == 0) {
            std::snprintf(b, sizeof(b), "%s: none", what);
        } else {
            const double n = static_cast<double>(k.n);
            std::snprintf(b, sizeof(b), "%s: %zu frame(s), RMS mean %.3f max %.3f (pre-1cp placement %.3f / %.3f)", what, k.n,
                          k.sum / n, k.max, k.preSum / n, k.preMax);
        }
        return std::string(b);
    };
    char b[900];
    std::snprintf(b, sizeof(b),
                  "rider placement%s: the player's rider drawn off the bike %zu frame(s) in %zu stretch(es), the "
                  "longest %zu (up to %.2f world units from the seat), on the bike in the climb %zu; rivals' riders off the bike %zu rider-frame(s) (%zu drawn for "
                  "their rider alone), in the climb %zu; passengers off %zu. The player's posed rider against the PORTED model "
                  "draw's vertices (world units) - ",
                  rr::game::RiderPlaceSeatOnly() ? " SEAT ONLY (RRJB_RIDER_PLACE=seat)" : "", s.playerOwn, s.playerFalls,
                  s.playerLongest, s.playerMaxFromSeat,
                  s.playerClimb, s.rivalOwn, s.rivalsByRider, s.rivalClimb, s.passengerOwn);
    return std::string(b) + check("riding", s.seated) + "; " + check("off the bike", s.off) + "; " + check("climb", s.climb) + "\n";
}

namespace {

// The model-to-world scale (race_scene.h kModelUnitsPerWorldUnit).
constexpr float kModelToWorld = 1.0f / kModelUnitsPerWorldUnit;

// The GL camera from the render camera record of view `vp` (see its call). Leaves the three untouched
// when the record is not in RAM, not orthogonal-looking, or its eye is more than 4 world units from the view's.
void RenderCameraView(const uint8_t* ram, int vp, float eye[3], float target[3], float up[3]) {
    static const bool off = std::getenv("RRJB_GL_CAMERA") != nullptr && std::strcmp(std::getenv("RRJB_GL_CAMERA"), "view") == 0;
    if (off) return;
    const auto word = [ram](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
        return v;
    };
    const auto half = [ram](uint32_t a) {
        int16_t v = 0;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
        return static_cast<double>(v);
    };
    const uint32_t rc = word(0x8005AEC0u + 4u * static_cast<uint32_t>(vp == 1 ? 1 : 0));
    ++g_renderCameraFallbacks;
    if (rc < 0x80000000u || rc >= 0x80200000u) return;
    double row[3][3], len[3];
    for (uint32_t r = 0; r < 3; ++r) {
        len[r] = 0.0;
        for (uint32_t k = 0; k < 3; ++k) {
            row[r][k] = half(rc + 0x5Cu + 6u * r + 2u * k);
            len[r] += row[r][k] * row[r][k];
        }
        len[r] = std::sqrt(len[r]);
        if (len[r] < 1000.0) return;
    }
    double e[3];
    for (uint32_t k = 0; k < 3; ++k) e[k] = static_cast<double>(static_cast<int32_t>(word(rc + 0x1Cu + 4u * k))) / 64.0;
    const double d = std::sqrt((e[0] - eye[0]) * (e[0] - eye[0]) + (e[1] - eye[1]) * (e[1] - eye[1]) + (e[2] - eye[2]) * (e[2] - eye[2]));
    if (d > 4.0) return;
    --g_renderCameraFallbacks;
    ++g_renderCameraFrames;
    for (int k = 0; k < 3; ++k) {
        eye[k] = static_cast<float>(e[k]);
        target[k] = static_cast<float>(e[k] + row[2][k] / len[2]);
        up[k] = static_cast<float>(-row[1][k] / len[1]); // GTE y is down
    }
}

// The console's render camera of view `vp` (the record *(0x8005AEC0 + 4 vp), RenderCamera SLUS 0x8002F17C, PORTED; see
// RenderCameraView): its eye (+0x1C, 1/64 world unit) and its three rows (+0x5C: right, down - the second scaled by
// 3412/4096 in the record -, ahead), normalised. The effect pass projected the frame's packets with it (fx_runtime.h),
// so FxDraw::DrawWorld takes them back into the world through it. False when the record is not in RAM or degenerate.
bool ConsoleRenderCamera(const uint8_t* ram, int vp, float eye[3], float rows[3][3]) {
    uint32_t rc = 0;
    std::memcpy(&rc, ram + ((0x8005AEC0u + 4u * static_cast<uint32_t>(vp == 1 ? 1 : 0)) & 0x1FFFFFu), 4);
    if (rc < 0x80000000u || rc >= 0x80200000u) return false;
    for (uint32_t r = 0; r < 3; ++r) {
        double row[3], len = 0.0;
        for (uint32_t k = 0; k < 3; ++k) {
            int16_t v = 0;
            std::memcpy(&v, ram + ((rc + 0x5Cu + 6u * r + 2u * k) & 0x1FFFFFu), 2);
            row[k] = static_cast<double>(v);
            len += row[k] * row[k];
        }
        len = std::sqrt(len);
        if (len < 1000.0) return false;
        for (uint32_t k = 0; k < 3; ++k) rows[r][k] = static_cast<float>(row[k] / len);
    }
    for (uint32_t k = 0; k < 3; ++k) {
        int32_t v = 0;
        std::memcpy(&v, ram + ((rc + 0x1Cu + 4u * k) & 0x1FFFFFu), 4);
        eye[k] = static_cast<float>(static_cast<double>(v) / 64.0);
    }
    return true;
}

// A machine's model matrix as the original's model draw loads it: RASHCDG
// 0x80084E10 copies the entity's rows +0x1B0..+0x1C0 TRANSPOSED into part 0's slot - model axis k is row k,
// the bike's lean included - with the origin at +0xB8.
Mat4 RecordModelMatrix(const uint8_t* ram, uint32_t entity, float scale) {
    const auto half = [ram](uint32_t a) {
        int16_t v = 0;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
        return static_cast<float>(v);
    };
    Mat4 m;
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t k = 0; k < 3; ++k) m.m[4 * c + k] = half(entity + 0x1B0u + 6u * c + 2u * k) / 4096.0f * scale;
    for (uint32_t k = 0; k < 3; ++k) {
        int32_t v = 0;
        std::memcpy(&v, ram + ((entity + 0xB8u + 4u * k) & 0x1FFFFFu), 4);
        m.m[12 + k] = static_cast<float>(static_cast<double>(v) / 65536.0);
    }
    return m;
}

// The two-seat machine (race_scene_sidecar.cpp, rules.md 16): bike `e` rides the scene's rig when its +0xB4 is
// the rig's index (ModelBind's model 100 + index); then both seated children hang on the rig's seat
// (ChildPlace RASHCDG 0x80066B98 places every child on SeatVertex's vertex) and a rider in seat 1 (+0x40:
// Attach SLUS 0x80012838(bike, rider, 2, 1), SpawnPassenger / the boarding) is drawn as the passenger.
void SidecarRequest(const RaceScene& scene, const uint8_t* ram, uint32_t e, int vp, DrawRequest& r,
                    rr::game::RiderPoseView& pose, bool lod0 = false) {
    const auto word = [ram](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
        return v;
    };
    const int rig = r.sidecarRig; // two players: 1 = player 2's own rig, set by the caller (race_scene_sidecar2.cpp)
    if (!scene.HasSidecarFor(rig) || 100u + word(e + 0xB4u) != scene.SidecarModelIdFor(rig)) return;
    r.sidecar = true;
    const float* seat = scene.SidecarAttachFor(rig, r.bikeLod);
    const uint32_t passenger = word(e + 0x40u);
    if (passenger < 0x80000000u || passenger >= 0x80200000u) return;
    // A passenger whose part array +0x04 is not bound (the Side Car grid's: SpawnPassenger's rider) is still
    // drawn by the original's model draw; the renderer then takes it from those vertices only (passengerLocal null:
    // race_scene_sidecar.cpp draws it when its capture was taken)
    const bool posed = rr::game::ReadRiderPose(ram, passenger, pose);
    // maximum detail draws the full-detail pose (anim_detail.h; the call puts back every arena byte it
    // touches, and the arena is the session's own writable buffer)
    if (lod0 && posed && rr::game::AnimDetailOn()) rr::game::AnimDetailPose(const_cast<uint8_t*>(ram), passenger, pose);
    r.passenger = true;
    r.passengerObject = passenger; // its PORTED model draw vertices (DrawRequest::passengerCaptured)
    r.passengerLocal = posed ? pose.local : nullptr;
    int16_t root[3] = {};
    for (uint32_t k = 0; k < 3; ++k) root[k] = posed ? pose.root[k] : 0;
    r.passengerModel = PosedRiderMatrix(r.bikeModel, seat, root);
    float ownAxis[3][3], ownOrigin[3]; // a passenger off the machine where the game has him (rider_pose.h)
    if (rr::game::RiderOwnFrame(ram, passenger, ownAxis, ownOrigin)) {
        r.passengerModel = OwnRiderMatrix(ownAxis, ownOrigin);
        if (vp == 0) ++RiderPlacementStats().passengerOwn;
    }
    r.passengerLod = lod0 ? 0 : rr::game::DrawLod(ram, passenger, vp);
    r.passengerObjFlags = word(passenger + 0x24u); // the model light
}

// DEVELOPMENT (RRJB_VIEW_PROFILE=1): the CPU time of RenderView's sections, printed every 600 views - the set-up, the
// props list, the captures and requests, RaceScene::Draw, the rivals, the traffic and pedestrians, the tail.
struct ViewProfile {
    static bool On() {
        static const bool on = std::getenv("RRJB_VIEW_PROFILE") != nullptr;
        return on;
    }
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    int last = -1;
    static double& Sum(int k) {
        static double s[8] = {};
        return s[k];
    }
    void Mark(int k) {
        if (!On()) return;
        const auto now = std::chrono::steady_clock::now();
        Sum(k) += std::chrono::duration<double, std::milli>(now - t).count();
        t = now;
        last = k;
    }
    ~ViewProfile() {
        if (!On()) return;
        Mark(6);
        static long views = 0;
        if (++views % 600 != 0) return;
        std::printf("view profile (ms a view over 600): set-up %.2f, props %.2f, captures %.2f, scene %.2f, rivals %.2f, "
                    "traffic+peds %.2f, tail %.2f\n", Sum(0) / 600, Sum(1) / 600, Sum(2) / 600, Sum(3) / 600, Sum(4) / 600,
                    Sum(5) / 600, Sum(6) / 600);
        for (int k = 0; k < 8; ++k) Sum(k) = 0;
    }
};

// DEVELOPMENT TRACE (RRJB_ANIM_DETAIL_TRACE=<file>): per drawn pedestrian (P) / rival rider (R) of view 0
// and frame, its distance to the eye and to the view (+0x2C, 1/64 units), +0x09, what the game's animation did this
// frame (anim_detail.h AnimDetailInfo) and whether the full-detail pose was drawn.
bool AnimDetailTrace() {
    static const bool on = std::getenv("RRJB_ANIM_DETAIL_TRACE") != nullptr;
    return on;
}
void TraceAnimDetail(long frame, char what, uint32_t owner, const uint8_t* ram, const float eye[3],
                     const rr::game::AnimDetailInfo& d) {
    static FILE* f = std::fopen(std::getenv("RRJB_ANIM_DETAIL_TRACE"), "w");
    if (f == nullptr) return;
    int32_t pos[3], viewDist = 0;
    for (uint32_t k = 0; k < 3; ++k) std::memcpy(&pos[k], ram + ((owner + 0xB8u + 4u * k) & 0x1FFFFFu), 4);
    std::memcpy(&viewDist, ram + ((owner + 0x2Cu) & 0x1FFFFFu), 4);
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double v = static_cast<double>(pos[k]) / 65536.0 - static_cast<double>(eye[k]);
        d2 += v * v;
    }
    const char* game = !d.object ? "none" : d.held ? "held" : !d.playing ? "stopped" : !d.posed ? "not-posed"
                     : (d.mask & d.lod0Mask) != d.lod0Mask ? "part-masked" : !d.interp ? "stepped" : "full";
    std::fprintf(f, "%ld %c %08X dist %.1f view %d lod %d vis %u game %s mask %05X/%05X drawn %s\n", frame, what, owner,
                 std::sqrt(d2), viewDist, static_cast<int>(static_cast<int8_t>(ram[(owner + 10u) & 0x1FFFFFu])),
                 static_cast<unsigned>(ram[(owner + 9u) & 0x1FFFFFu]), game, d.mask & 0xFFFFFu, d.lod0Mask & 0xFFFFFu,
                 d.ours ? "full" : "game");
    static uint32_t seenTable[64] = {};
    uint32_t table = 0; // the LOD table +0x64 ({far, near} s32 pairs), printed once per table
    std::memcpy(&table, ram + ((owner + 0x64u) & 0x1FFFFFu), 4);
    bool seen = (table & 0xFF000000u) != 0x80000000u;
    for (uint32_t& t : seenTable) {
        if (seen) break;
        if (t == table) seen = true;
        else if (t == 0) {
            t = table;
            int32_t w[8];
            std::memcpy(w, ram + (table & 0x1FFFFFu), sizeof(w));
            std::fprintf(f, "  LOD table %08X of %c %08X: %d/%d %d/%d %d/%d %d/%d\n", table, what, owner, w[0], w[1], w[2],
                         w[3], w[4], w[5], w[6], w[7]);
            seen = true;
        }
    }
}

} // namespace

bool BikeFrameFromGround() {
    static const bool ground = std::getenv("RRJB_BIKE_FRAME") != nullptr && std::strcmp(std::getenv("RRJB_BIKE_FRAME"), "ground") == 0;
    return ground;
}

bool RivalsAsPlayer() {
    static const bool off = std::getenv("RRJB_RIVALS") != nullptr && std::strcmp(std::getenv("RRJB_RIVALS"), "player") == 0;
    return off;
}

void ObjectLook(const uint8_t* ram, uint32_t obj, uint32_t& modelId, int& a1) {
    const auto word = [ram](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
        return v;
    };
    const auto inRam = [](uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; };
    modelId = 0;
    a1 = -1;
    if (!inRam(obj)) return;
    const uint32_t reg = word(obj + 0x60u);
    if (inRam(reg)) modelId = word(reg);
    a1 = static_cast<int>((word(obj + 0x24u) >> 12) & 0x3Fu);
}

RaceRenderer::RaceRenderer(rr::game::RaceSession& session, const rr::RaceWorld& world, RaceScene& scene, TrafficDraw& traffic,
                           PedDraw& peds, WeaponDraw& weapons, rr::game::FxRuntime& fx, rr::game::ModelRuntime& models,
                           FxDraw& fxDraw, const std::map<uint32_t, size_t>& cellIndexById, const Options& opts)
    : options(opts), session_(session), world_(world), scene_(scene), traffic_(traffic), peds_(peds), weapons_(weapons),
      fx_(fx), models_(models), fxDraw_(fxDraw), cellIndexById_(cellIndexById) {}

GameView RaceRenderer::GameCamera(int vp, int x, int y, int w, int h, unsigned fb) const {
    GameView v;
    v.player = vp;
    v.framebuffer = fb;
    // One player: the part of the draw area a television shows (race_scene.h kShown*), the rectangle the
    // HUD overlay is laid over the picture with - the whole 240 lines drew the world ~5 % smaller than
    // the HUD and the original. RRJB_SHOWN=off: the whole area (the negative control);
    // --parity: the whole area, the console's own frame pixel for pixel.
    const bool shownPart = !options.parity && !options.shownOff;
    float viewFovY = shownPart ? rr::render::ShownVerticalFov() : OriginalVerticalFov();
    int viewX = x, viewY = y, viewW = w, viewH = h;
    session_.ViewCamera(v.eye, v.target, v.up);
    // Two players (mp_session.cpp): one picture per player, drawn from
    // its own view record into the rectangle VIEWS.VI gives it of the 384 x 240 draw area, with the
    // projection centred there and the original's H (SLUS 0x80011C4C's GTE offset) - so the field is
    // the one the rectangle's height makes. One player: one view, the whole picture.
    if (session_.SplitRect(vp, v.splitRect)) {
        v.split = true;
        session_.ViewCamera(v.eye, v.target, v.up, vp);
        // the 384 x 240 area laid over the picture as the HUD overlay is (its shown part x 9..373,
        // y 8..231 fills the picture), GL's y up
        const double sx = w / 365.0, sy = h / 224.0;
        viewX = x + static_cast<int>(std::lround((v.splitRect[0] - 9) * sx));
        viewW = static_cast<int>(std::lround(v.splitRect[2] * sx));
        viewH = static_cast<int>(std::lround(v.splitRect[3] * sy));
        viewY = y + h - static_cast<int>(std::lround((v.splitRect[1] - 8) * sy)) - viewH;
        viewFovY = 2.0f * std::atan(static_cast<float>(v.splitRect[3]) * 0.5f / (kGteH * kGteAspectRow));
    }
    v.viewport[0] = viewX;
    v.viewport[1] = viewY;
    v.viewport[2] = viewW;
    v.viewport[3] = viewH;
    // The frame is drawn from the RENDER camera the original draws with
    // (*(0x8005AEC0 + 4 v): +0x5C the view's +0x1B0 rows, +0x1C its eye +0xB8 >> 10 - RenderCamera SLUS
    // 0x8002F17C, PORTED, run by the effect runtime after the frame), not from the view record's look target
    // +0x22C and up row: in rr-pack the two differ by ~5 px. RRJB_GL_CAMERA=view: the view record (control).
    RenderCameraView(session_.ArenaRam(), vp, v.eye, v.target, v.up);
    v.view = LookAt(v.eye, v.target, v.up);
    // The original's projection (race_scene.h OriginalVerticalFov): H = 237 on a
    // 384 x 240 frame with the camera's second row scaled by 3412/4096 - a 62.6 degree
    // vertical field. The window is wider than 4:3, so the picture keeps the original's
    // vertical field and shows more at the sides. The near plane 0.25 world units is ours.
    // --parity: the window is the console's 384 x 240 draw area shown 4:3, pixel for pixel (parity.cpp)
    v.shownView = shownPart && session_.Players() != 2;
    v.fovY = viewFovY;
    v.aspect = options.parity ? 4.0f / 3.0f
                              : static_cast<float>(viewW) / static_cast<float>(viewH) *
                                    (v.shownView ? rr::render::ShownAspectStretch() : 1.0f);
    v.proj = Perspective(viewFovY, v.aspect, v.nearPlane, v.farPlane);
    if (v.shownView) { // the shown rectangle's centre is half a column left of the GTE's 192
        const float shift = rr::render::ShownCentreShiftNdc() * (4.0f / 3.0f) * rr::render::ShownAspectStretch() / v.aspect;
        for (int c = 0; c < 4; ++c) v.proj.m[4 * c + 0] += shift * v.proj.m[4 * c + 3];
    }
    return v;
}

void RaceRenderer::Reproject(GameView& v) const {
    v.view = LookAt(v.eye, v.target, v.up);
    v.proj = Perspective(v.fovY, v.aspect, v.nearPlane, v.farPlane);
    if (v.shownView) {
        const float shift = rr::render::ShownCentreShiftNdc() * (4.0f / 3.0f) * rr::render::ShownAspectStretch() / v.aspect;
        for (int c = 0; c < 4; ++c) v.proj.m[4 * c + 0] += shift * v.proj.m[4 * c + 3];
    }
}

void RaceRenderer::RenderView(const GameView& gv, long frames) {
    ViewProfile prof; // DEVELOPMENT: RRJB_VIEW_PROFILE=1 (the sections' CPU time)
    rr::game::RaceSession& session = session_;
    RaceScene& scene = scene_;
    const int vp = gv.player;
    const int viewW = gv.viewport[2], viewH = gv.viewport[3];
    float eye[3], target[3], up[3];
    for (int k = 0; k < 3; ++k) {
        eye[k] = gv.eye[k];
        target[k] = gv.target[k];
        up[k] = gv.up[k];
    }
    if (gv.split) glClear(GL_DEPTH_BUFFER_BIT);
    glViewport(gv.viewport[0], gv.viewport[1], viewW, viewH);
    // the ordering-table order is the console camera's (a head or a stereo camera draws with the depth buffer)
    scene.SetOtOrder(options.otOrder && gv.consoleCamera);
    // maximum detail (the PC graphics settings): every model at LOD 0, whatever LodChoice 0x800667C4 wrote
    const bool maxDetail = options.maxDetail;
    rr::game::SetAnimDetail(maxDetail); // the full animation at every distance (anim_detail.h)
    const auto lodOf = [&session, vp, maxDetail](uint32_t obj) { return maxDetail ? 0 : rr::game::DrawLod(session.ArenaRam(), obj, vp); };
    scene.SetConsoleLines(gv.shownView ? rr::render::kShownH : 240.0f);
    const float pictureAspect = gv.aspect;
    const Mat4& proj = gv.proj;
    if (vp == 0 && gv.consoleCamera) { // cell_sort.h `widen`: the cell cull's side planes opened by how
        // much wider than the console's (x = 192 / 237 of z) the picture is; 4:3 and --parity: the console's test exactly
        const double tanH = std::tan(static_cast<double>(gv.fovY) * 0.5) * pictureAspect, console = 192.0 / 237.0;
        rr::game::SetCellSortWiden(options.parity || tanH <= console ? 4096
                                                                    : static_cast<int32_t>(std::lround(4096.0 * tanH / console)));
    }
    const Mat4 viewProj = Multiply(proj, gv.view);

    // The player's machine on the road (BikeFrameFromGround's frame).
    float position[3], tangent[3], lateral[3], normal[3];
    session.BikePlacement(0, position, tangent, lateral, normal);
    const float upSense = normal[1] <= 0.0f ? 1.0f : -1.0f;
    float bikeUp[3];
    for (int k = 0; k < 3; ++k) bikeUp[k] = normal[k] * upSense;

    {   // The model light (race_scene.h SetModelLight): RASHCDG 0x80068468 turns the level's
        // sun vector S by the TRANSPOSE of the view's camera matrix (its projection record *(0x8005AEC0 + 4 v)
        // + 0x5C, the second row scaled by 3412/4096; MVMVA sf = 1) and dots the model's own normals with it.
        // The switch is game_state+4 bit 4 clear.
        const uint8_t* ar = session.ArenaRam();
        const auto w32 = [ar](uint32_t a) {
            uint32_t v = 0;
            std::memcpy(&v, ar + (a & 0x1FFFFFu), 4);
            return v;
        };
        const auto h16 = [ar](uint32_t a) {
            int16_t v = 0;
            std::memcpy(&v, ar + (a & 0x1FFFFFu), 2);
            return static_cast<int64_t>(v);
        };
        const uint32_t rec = w32(0x8005AEC0u + 4u * static_cast<uint32_t>(vp));
        const uint32_t gs = w32(0x8005B2F8u);
        int32_t light[3] = {0, 0, 0};
        const int32_t* sun = scene.SunVector();
        for (uint32_t i = 0; i < 3; ++i) {
            int64_t acc = 0;
            for (uint32_t j = 0; j < 3; ++j) acc += h16(rec + 0x5Cu + 2u * (3u * j + i)) * sun[j];
            light[i] = static_cast<int32_t>(std::clamp<int64_t>(acc >> 12, -32768, 32767));
        }
        const bool inRam = rec >= 0x80000000u && rec < 0x80200000u && gs >= 0x80000000u && gs < 0x80200000u;
        // game_state+4 bit 4 set (two players) clears SLUS 0x800251E4's lit flag (0x80025644..0x80025674):
        // every group takes the unlit step ramp[*(0x80052380) >> shift] (mp2_product.h; RRJB_MP2=off: the
        // old lambert, the light switched off)
        const bool bit4 = inRam && (ar[(gs + 4u) & 0x1FFFFFu] & 0x10u) != 0;
        const bool on = !options.modelLightOff && inRam && (!bit4 || rr::game::Mp2On()) && scene.ModelLightReady();
        scene.SetModelLight(on, light);
        scene.SetModelLit(!bit4);
        if (on && vp == 0) ++counters.modelLightViews;
        if (on && bit4) ++rr::game::Mp2().unlitViews;
        if (std::getenv("RRJB_MP2_TRACE") && frames % 500 == 0) // DEVELOPMENT: the light's switch per view
            std::printf("mp2 light: frame %ld view %d rec 0x%08X bit4 %d ready %d on %d\n", frames, vp, rec,
                        bit4 ? 1 : 0, scene.ModelLightReady() ? 1 : 0, on ? 1 : 0);
    }

    // Which cells and which backdrop are live: the game's own residency windows, keyed on
    // the road the player is on and how far along it they are.
    size_t sliceIndex = 0;
    SampleRoad(world_.path, static_cast<float>(session.RouteDistance(static_cast<size_t>(vp))), &sliceIndex);
    const bool roadKnown = sliceIndex < world_.pathRoad.size() && !world_.pathIsJunction[sliceIndex];
    const uint16_t currentRoad = roadKnown ? world_.pathRoad[sliceIndex] : 0;
    const uint16_t currentRoadDistance = roadKnown ? world_.pathRoadDistance[sliceIndex] : 0;

    scene.PrepareSkyGradient(viewProj, eye, target, options.sunOff ? 0 : scene.SunYaw()); // the level's sun
    { // the panorama the game's own pick shows (RASHCDG 0x80065174, stream_product.h)
        rr::sim::GuestRam sg(const_cast<uint8_t*>(session.ArenaRam()), 0x8005AC8Cu);
        uint32_t skyId = 0;
        if (!rr::game::StreamSkyShown(sg, skyId) || !scene.SelectSkyId(skyId))
            scene.SelectSky(roadKnown, currentRoad, currentRoadDistance);
    }

    {   // the ordering-table sort (race_scene_ot.cpp): the arena, the cells' ids and the cell the view is in
        // (*(0x800CD948 + 0x46C p), SLUS 0x800353C4; the bike's own cell +0xB0 when that is not a drawn cell)
        const uint8_t* ar = session.ArenaRam();
        const auto w32 = [ar](uint32_t a) {
            uint32_t v = 0;
            std::memcpy(&v, ar + (a & 0x1FFFFFu), 4);
            return v;
        };
        int cameraCell = -1;
        for (uint32_t id : {w32(0x800CD948u + 0x46Cu * static_cast<uint32_t>(vp)),
                            w32(session.Bikes()[0].entityAddress + 0xB0u)})
            if (const auto it = cellIndexById_.find(id); cameraCell < 0 && it != cellIndexById_.end())
                cameraCell = static_cast<int>(it->second);
        scene.SetOtFrame(ar, &cellIndexById_, cameraCell);
        // The arena's own sort - where the arena has a draw list (a release list)
        scene.SetOtArena(rr::game::CellSortOn() && w32(0x8005AC8Cu + 0x88Cu) != 0, vp);
        if (scene.OtOrder() && vp == 0) ++counters.otViews;
    }
    DrawRequest request;
    request.viewProj = viewProj;
    if (gv.haveCullViewProj) request.cullViewProj = &gv.cullViewProj; // VR: both eyes culled alike
    for (int k = 0; k < 3; ++k) request.eye[k] = eye[k];
    request.currentRoadKnown = roadKnown;
    request.currentRoad = currentRoad;
    request.currentRoadDistance = currentRoadDistance;
    request.haveMachine = true;
    request.haveCellView = true;
    request.bikeObject = session.Bikes()[0].entityAddress; // its cell's table
    std::vector<size_t> cellOrder; // the PORTED draw list SLUS 0x80035F48, keyed on the player (cell_view.h)
    if (bool rls = false; true) {
        const std::vector<uint32_t> ids = rr::game::CellDrawIds(session.ArenaRam(), vp, rls);
        for (uint32_t id : ids)
            if (const auto it = cellIndexById_.find(id); it != cellIndexById_.end()) cellOrder.push_back(it->second);
        if (rls) request.cellOrder = &cellOrder;
    }
    {
        float forward[3], right[3], length = 0.0f;
        for (int k = 0; k < 3; ++k) forward[k] = target[k] - eye[k];
        for (float f : forward) length += f * f;
        length = std::sqrt(length);
        for (float& f : forward) f = length > 0.0f ? f / length : 0.0f;
        right[0] = up[1] * forward[2] - up[2] * forward[1];
        right[1] = up[2] * forward[0] - up[0] * forward[2];
        right[2] = up[0] * forward[1] - up[1] * forward[0];
        length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        for (int k = 0; k < 3; ++k) {
            request.cellView.eye[k] = eye[k];
            request.cellView.forward[k] = forward[k];
            request.cellView.right[k] = length > 0.0f ? right[k] / length : 0.0f;
            request.cellView.up[k] = up[k];
        }
    }
    RoadFrame frame;
    for (int k = 0; k < 3; ++k) {
        frame.pos[k] = position[k];
        frame.tangent[k] = tangent[k];
        frame.lateral[k] = lateral[k];
        frame.normal[k] = normal[k];
    }
    request.bikeModel = BikeFrameFromGround() ? MachineMatrixAt(frame, bikeUp, position, kModelToWorld)
                                              : RecordModelMatrix(session.ArenaRam(), session.Bikes()[0].entityAddress,
                                                                  kModelToWorld);
    gv.playerLean.Matrix(request.bikeModel); // VR head view: the visual lean (off: nothing)
    // the seat of the LOD the bike is drawn at (SeatVertex 0x80066A84, race_scene.h RiderAttach(lod))
    const float* playerSeat = scene.RiderAttach(lodOf(session.Bikes()[0].entityAddress));
    rr::game::RiderPoseView passengerPose; // the two-seat machine (SidecarRequest)
    request.bikeLod = lodOf(session.Bikes()[0].entityAddress);
    SidecarRequest(scene, session.ArenaRam(), session.Bikes()[0].entityAddress, vp, request, passengerPose, maxDetail);
    if (request.sidecar) playerSeat = scene.SidecarAttach(request.bikeLod);
    // the re-seat's climb turns the bike's part 0 by its slot 0 (rider_pose.h MachineClimbSlots). Not in
    // the head view (VR, desktop --camera head): its eye is fixed on the upright bike's seat through the climb, and a
    // bike lifted off its side under it would put the eye in the ground
    rr::PartMatrix climbSlot0, climbSlot2;
    const bool playerClimb = !request.sidecar && gv.playerFromCaptures && rr::game::MachineClimbSlots(session.ArenaRam(), session.Bikes()[0].entityAddress,
                                                                             climbSlot0, climbSlot2);
    const Mat4 seatFrameBike = request.bikeModel; // (the pre-1cp placement, for the check below)
    if (playerClimb) request.bikeModel = TurnedBySlot(request.bikeModel, climbSlot0);
    request.riderModel = RiderMatrix(request.bikeModel, playerSeat, scene.RiderRelative());
    rr::game::RiderPoseView playerPose; // the PORTED pose's part slots and root (rider_pose.h)
    const bool playerPosed = rr::game::ReadRiderPose(session.ArenaRam(), session.Bikes()[0].ownerAddress, playerPose);
    if (playerPosed) {
        request.riderLocal = playerPose.local;
        request.riderModel = playerClimb ? ChildRiderMatrix(request.bikeModel, playerSeat, playerPose.root, climbSlot2)
                                         : PosedRiderMatrix(request.bikeModel, playerSeat, playerPose.root);
        if (std::getenv("RRJB_POSE_TRACE") && frames % 10 == 0)
            std::printf("pose: frame %ld player stance %u root (%d,%d,%d)\n", frames, playerPose.stance,
                        playerPose.root[0], playerPose.root[1], playerPose.root[2]);
    }
    // a rider off his bike is drawn where the game has him (thrown, lying, getting up, walking back)
    float playerOwnAxis[3][3], playerOwnOrigin[3];
    const bool playerOwn = rr::game::RiderOwnFrame(session.ArenaRam(), session.Bikes()[0].ownerAddress, playerOwnAxis, playerOwnOrigin);
    if (playerOwn) request.riderModel = OwnRiderMatrix(playerOwnAxis, playerOwnOrigin);
    if (vp == 0) {
        RiderPlacement& rp = RiderPlacementStats();
        if (playerOwn) {
            ++rp.playerOwn;
            if (rp.playerLastOff != frames) { // a stretch off the bike (view 0 is drawn once a frame)
                rp.playerRun = rp.playerLastOff == frames - 1 ? rp.playerRun + 1 : 1;
                if (rp.playerRun == 1) ++rp.playerFalls;
                rp.playerLongest = std::max(rp.playerLongest, rp.playerRun);
                rp.playerLastOff = frames;
            }
            const Mat4 seated = PosedRiderMatrix(seatFrameBike, playerSeat, playerPose.root);
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) d2 += (seated.m[12 + k] - playerOwnOrigin[k]) * (seated.m[12 + k] - playerOwnOrigin[k]);
            rp.playerMaxFromSeat = std::max(rp.playerMaxFromSeat, static_cast<double>(std::sqrt(d2)));
        }
        if (playerClimb) ++rp.playerClimb;
    }
    if (std::getenv("RRJB_JAIL_TRACE") && frames % std::atoi(std::getenv("RRJB_JAIL_TRACE")) == 0) {
        const uint8_t* jr = session.ArenaRam();
        auto w = [jr](uint32_t a) { uint32_t v = 0; std::memcpy(&v, jr + (a & 0x1FFFFFu), 4); return v; };
        const uint32_t e = session.Bikes()[0].entityAddress, gs = w(0x8005B2F8u), rr0 = w(e + 852u);
        const uint32_t ph = (w(gs + 0x38u) >> 8) & 0xFFu, ms = 0x80053174u + 4u * (ph - 1u);
        const uint32_t P = w(e + 856u), dp = P ? (w(P + 944u) >> 16) & 0xFFu : 0u;
        if (std::getenv("RRJB_VOL_TRACE")) { // the pool-6 volumes near player 1 (their records' first words)
            const uint32_t v6 = w(0x800CD6C4u);
            const int32_t hi6 = static_cast<int32_t>(w(0x800CD6A8u + 8u));
            for (int32_t i = 0; v6 != 0u && i <= hi6 && i < 32; ++i) {
                const uint32_t v = v6 + 280u * static_cast<uint32_t>(i);
                const double dx = (static_cast<int32_t>(w(v + 12u)) - static_cast<int32_t>(w(e + 184u))) / 65536.0;
                const double dz = (static_cast<int32_t>(w(v + 20u)) - static_cast<int32_t>(w(e + 192u))) / 65536.0;
                if ((w(v) & 0xFFFFu) == 0u || dx * dx + dz * dz > 400.0) continue;
                std::printf("vol: f%ld vol%d d (%.1f, %.1f) pos (%.1f, %.1f, %.1f) words", frames, i, dx, dz,
                            static_cast<int32_t>(w(v + 12u)) / 65536.0, static_cast<int32_t>(w(v + 16u)) / 65536.0,
                            static_cast<int32_t>(w(v + 20u)) / 65536.0);
                for (uint32_t k = 0; k < 280u; k += 4) std::printf(" %08X", w(v + k));
                std::printf("\n");
            }
        }
        if (frames == 0)
            for (uint32_t k = 0; k < 6u; ++k)
                std::printf("jail: milestone %u = road %u along %u\n", k, w(0x80053174u + 4u * k) & 0xFFFFu,
                            w(0x80053174u + 4u * k) >> 16);
        std::printf("jail: f%ld phase %u road %u along %.1f stop %u/%u lat %.1f spd %.2f | rider mount %d stance %u "
                    "+0x23C 0x%02X | P depth %u top %u | P-rider mount %d stance %u +0x23C 0x%02X pos %.0f,%.0f | "
                    "+0x230 0x%08X +0x234 0x%08X aim %.0f,%.0f\n",
                    frames, ph, w(e + 360u), static_cast<int32_t>(w(e + 368u)) / 65536.0, w(ms) & 0xFFFFu, w(ms) >> 16,
                    static_cast<int32_t>(w(e + 344u)) / 65536.0, static_cast<int32_t>(w(e + 480u)) / 65536.0,
                    static_cast<int8_t>(w(rr0 + 72u) & 0xFFu), w(rr0 + 544u) & 0xFFFFu, w(rr0 + 572u) & 0xFFu, dp,
                    dp ? w(P + 956u + 8u * (dp - 1u)) & 0xFFFFu : 0u,
                    P ? static_cast<int8_t>(w(w(P + 852u) + 72u) & 0xFFu) : 0, P ? w(w(P + 852u) + 544u) & 0xFFFFu : 0u,
                    P ? w(w(P + 852u) + 572u) & 0xFFu : 0u, P ? static_cast<int32_t>(w(w(P + 852u) + 184u)) / 65536.0 : 0.0,
                    P ? static_cast<int32_t>(w(w(P + 852u) + 192u)) / 65536.0 : 0.0, w(e + 560u), w(e + 564u),
                    static_cast<int32_t>(w(e + 880u)) / 65536.0, static_cast<int32_t>(w(e + 888u)) / 65536.0);
    }
    rr::PartMatrix playerBikeParts[5]; // the fork, the pitch, the wheels (bike_pose_product.h)
    if (rr::game::ReadBikeParts(session.ArenaRam(), session.Bikes()[0].entityAddress, playerBikeParts))
        request.bikeLocal = playerBikeParts;
    if (gv.haveForkOverride && request.bikeLocal != nullptr) playerBikeParts[1] = gv.playerFork; // VR: the hands' bars
    if (vp == 0) { // the machine as drawn (race_render.h DrawnMachine: VR's hands on its grips)
        drawnPlayerMachine.valid = !request.sidecar;
        drawnPlayerMachine.posed = request.bikeLocal != nullptr;
        drawnPlayerMachine.frame = frames;
        drawnPlayerMachine.model = request.bikeModel;
        if (request.bikeLocal != nullptr) std::memcpy(drawnPlayerMachine.parts, playerBikeParts, sizeof(playerBikeParts));
    }
    rr::PartMatrix rigParts[8]; // the sidecar rig's six (jail_session.cpp JailRigParts)
    const int rigN = request.sidecar ? rr::game::ReadMachineParts(session.ArenaRam(), session.Bikes()[0].entityAddress,
                                                                  rigParts, 8) : 0;
    if (rigN > 0 && rigN == static_cast<int>(scene.SidecarParts()) && !options.rigOff) {
        request.sidecarLocal = rigParts;
        ++counters.rigPosed;
        for (int k = 1; k < rigN; ++k) {
            if (std::memcmp(&rigParts[k], &rigLast_[k], sizeof(rr::PartMatrix)) != 0) ++counters.rigMoved[k];
            rigLast_[k] = rigParts[k];
        }
    }
    request.bikeLod = lodOf(session.Bikes()[0].entityAddress);  // cell_view.h
    request.riderLod = lodOf(session.Bikes()[0].ownerAddress);
    {   // the objects' +0x24 flags for the model light (race_scene.h DrawRequest::bikeObjFlags)
        uint32_t fb = 0, fr = 0;
        std::memcpy(&fb, session.ArenaRam() + ((session.Bikes()[0].entityAddress + 0x24u) & 0x1FFFFFu), 4);
        std::memcpy(&fr, session.ArenaRam() + ((session.Bikes()[0].ownerAddress + 0x24u) & 0x1FFFFFu), 4);
        request.bikeObjFlags = fb;
        request.riderObjFlags = fr;
    }
    // A car, prop or pedestrian the PORTED ModelVisible hid in this view (+0x09 bit vp clear
    // after the model pass) is not drawn - the original's draw list holds only the visible ones. A picture wider than
    // 4:3 keeps them (ModelVisible's side test is the console's 4:3 frame's); RRJB_GTE_OBJECTS=off: every one drawn.
    const bool objCull = rr::game::GteObjectsOn() && (!options.wideScreen || options.parity) && gv.consoleCamera &&
                         !maxDetail; // ModelVisible's verdict is the console camera's (and has a draw range)
    // maximum detail: an object is drawn wherever it is, frustum-culled by its bounding sphere (after scene.Draw set
    // the frustum), not only in the view's draw-list cells
    const auto inCells = [&session, vp, maxDetail](uint32_t e) { return maxDetail || rr::game::InDrawnCell(session.ArenaRam(), e, vp); };
    const auto sphereCulled = [this, &scene](uint32_t e, float radius) {
        if (!options.cull) return false;
        float c[3];
        for (uint32_t k = 0; k < 3; ++k) {
            int32_t v = 0;
            std::memcpy(&v, session_.ArenaRam() + ((e + 0xB8u + 4u * k) & 0x1FFFFFu), 4);
            c[k] = static_cast<float>(static_cast<double>(v) / 65536.0);
        }
        if (scene.SphereVisible(c, radius)) return false;
        ++counters.objectsCulled;
        return true;
    };
    const auto objVisible = [&session, objCull, vp](uint32_t e) {
        return !objCull || (session.ArenaRam()[(e + 9u) & 0x1FFFFFu] & (1u << vp)) != 0;
    };
    prof.Mark(0);
    if (!options.propsFromRecords) { // the props the PORTED cell walker keeps in pools 4 / 5
        std::vector<rr::render::PropInstance>& props = scene.MutableProps();
        props.clear();
        const size_t groups = scene.PropFirst().size();
        for (const rr::game::LiveProp& lp : rr::game::LiveProps(session.ArenaRam())) {
            if (lp.cls >= groups) continue;
            // the model draw 0x80067690 draws an object filed in a cell of the view's draw list
            if (!inCells(lp.entity)) continue;
            if (!objVisible(lp.entity)) continue; // ModelVisible hid it in this view
            rr::render::PropInstance in;
            in.group = lp.cls;
            in.entity = lp.entity; // its cell's table
            for (int k = 0; k < 3; ++k) {
                in.pos[k] = lp.pos[k];
                in.centre[k] = static_cast<float>(lp.pos[k]) / 65536.0f;
            }
            // part 0's matrix (0x8008D56C: the rows +0x1B0 transposed): model axis k = row k
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k)
                    in.matrix[4 * c + k] = static_cast<float>(lp.rows[3 * c + k]) / 4096.0f / rr::render::kModelUnitsPerWorldUnit;
            for (int k = 0; k < 3; ++k) in.matrix[12 + k] = in.centre[k];
            in.matrix[15] = 1.0f;
            {   // +0x24 for the model light (race_scene.h PropInstance::flags)
                uint32_t f24 = 0;
                std::memcpy(&f24, session.ArenaRam() + ((lp.entity + 0x24u) & 0x1FFFFFu), 4);
                in.flags = f24;
            }
            props.push_back(in);
        }
        size_t hazardsDrawn = 0; // the hazard records: group +0x08 of the prop model, 0x800A2138's frame
        for (const rr::game::LiveHazard& h : rr::game::LiveHazards(session.ArenaRam())) {
            if (h.group >= groups) continue;
            if (!inCells(h.record)) continue;
            if (!maxDetail && (h.viewBits & (1u << vp)) == 0) continue; // ModelVisible's verdict for this view
            rr::render::PropInstance in;
            in.group = h.group;
            in.entity = h.record; // its cell's table
            for (int k = 0; k < 3; ++k) {
                in.pos[k] = h.pos[k];
                in.centre[k] = static_cast<float>(h.pos[k]) / 65536.0f;
            }
            for (int c = 0; c < 3; ++c) // part 0's matrix: the rows +0xC4 transposed (0x800A23A4..0x800A240C)
                for (int k = 0; k < 3; ++k)
                    in.matrix[4 * c + k] = static_cast<float>(h.rows[3 * c + k]) / 4096.0f / rr::render::kModelUnitsPerWorldUnit;
            for (int k = 0; k < 3; ++k) in.matrix[12 + k] = in.centre[k];
            in.matrix[15] = 1.0f;
            {   // +0x24 for the model light
                uint32_t f24 = 0;
                std::memcpy(&f24, session.ArenaRam() + ((h.record + 0x24u) & 0x1FFFFFu), 4);
                in.flags = f24;
            }
            props.push_back(in);
            ++hazardsDrawn;
        }
        if (maxDetail) { // the placement records of every prop the pools do not hold (race_scene.cpp keeps those of drawn cells)
            const size_t live = props.size();
            for (const rr::render::PropInstance& rp : scene.RecordProps()) {
                bool held = false;
                for (size_t k = 0; k < live && !held; ++k) {
                    const rr::render::PropInstance& lp = props[k];
                    const float dx = lp.centre[0] - rp.centre[0], dy = lp.centre[1] - rp.centre[1], dz = lp.centre[2] - rp.centre[2];
                    held = lp.group == rp.group && dx * dx + dy * dy + dz * dz < 1.0f;
                }
                if (held) continue;
                props.push_back(rp);
                if (vp == 0) ++counters.recordPropsAdded;
            }
        }
        if (vp == 0) rr::game::NoteHazardDrawn(hazardsDrawn);
        if (vp == 0) {
            counters.worldPropFrames += props.empty() ? 0u : 1u;
            counters.worldPropsMax = std::max(counters.worldPropsMax, props.size());
        }
    }
    // The bikes' and riders' vertices of this view as the PORTED model draw left them
    prof.Mark(1);
    std::map<uint32_t, CapturedVerts> captured;
    for (const auto& [key, c] : models_.shadows.poses) {
        if ((key >> 32) != static_cast<uint64_t>(vp)) continue;
        CapturedVerts& v = captured[static_cast<uint32_t>(key)];
        v.model = c.model;
        v.lod = c.lod;
        for (int k = 0; k < 3; ++k) v.eye[k] = c.eye[k];
        v.rel = c.rel;
        v.sxy = c.sxy;
        v.mac3 = c.mac3;
        v.exp = c.exp;
    }
    const auto capturedOf = [&captured](uint32_t obj) -> const CapturedVerts* {
        const auto it = captured.find(obj);
        return it == captured.end() ? nullptr : &it->second;
    };
    if (rr::game::GteObjectsOn()) // the props' own captures (race_scene.cpp draws them at their SXY)
        for (rr::render::PropInstance& in : scene.MutableProps()) in.captured = in.entity != 0 ? capturedOf(in.entity) : nullptr;
    request.bikeCaptured = capturedOf(session.Bikes()[0].entityAddress);
    request.riderCaptured = capturedOf(session.Bikes()[0].ownerAddress);
    request.riderHideParts = gv.riderHideParts; // the head camera: not its own head
    // the head camera, a VR eye: the gradient on a world dome (DEVELOPMENT RRJB_WORLD_SKY=off: the console's screen
    // strip, the control)
    static const bool worldSkyOff = std::getenv("RRJB_WORLD_SKY") != nullptr && std::strcmp(std::getenv("RRJB_WORLD_SKY"), "off") == 0;
    request.worldSky = !gv.consoleCamera && !worldSkyOff;
    if (vp == 0 && request.riderCaptured != nullptr && onPlayerRiderCaptured) onPlayerRiderCaptured(*request.riderCaptured);
    if (vp == 0 && request.riderCaptured != nullptr) { // the drawn rider against the PORTED model draw's vertices
        const rr::render::CapturedVerts& c = *request.riderCaptured;
        const uint32_t R = session.Bikes()[0].ownerAddress;
        std::vector<float> ours, pre;
        const auto frameOf = [](const Mat4& m, float axis[3][3], float origin[3]) {
            for (int a = 0; a < 3; ++a)
                for (int k = 0; k < 3; ++k) axis[a][k] = m.m[4 * a + k] * kModelUnitsPerWorldUnit;
            for (int k = 0; k < 3; ++k) origin[k] = m.m[12 + k];
        };
        const auto rms = [&c](const std::vector<float>& w, double& mx) {
            double s = 0.0;
            mx = 0.0;
            for (size_t v = 0; v + 2 < w.size(); v += 3) {
                double d2 = 0.0;
                for (size_t k = 0; k < 3; ++k) {
                    const double d = c.eye[k] + c.rel[v + k] - w[v + k];
                    d2 += d * d;
                }
                s += d2;
                mx = std::max(mx, std::sqrt(d2));
            }
            return std::sqrt(s / static_cast<double>(std::max<size_t>(w.size() / 3, 1)));
        };
        float ax[3][3], og[3], pax[3][3], pog[3];
        frameOf(request.riderModel, ax, og);
        frameOf(PosedRiderMatrix(seatFrameBike, playerSeat, playerPose.root), pax, pog);
        const rr::game::HeadCameraRig& rig = rr::game::ProductHeadCamera();
        if (!request.sidecar && c.model == 150u && c.lod == 0 && rig.PoseWorld(session.ArenaRam(), R, ax, og, ours) &&
            rig.PoseWorld(session.ArenaRam(), R, pax, pog, pre) && ours.size() == c.rel.size() && pre.size() == c.rel.size()) {
            RiderPlacement::Check& k = playerOwn ? RiderPlacementStats().off
                                     : playerClimb ? RiderPlacementStats().climb : RiderPlacementStats().seated;
            double mx = 0.0, pmx = 0.0;
            const double r = rms(ours, mx), pr = rms(pre, pmx);
            ++k.n;
            k.sum += r;
            k.max = std::max(k.max, r);
            k.preSum += pr;
            k.preMax = std::max(k.preMax, pr);
        }
    }
    if (!gv.playerFromCaptures) { // the head camera: the machine posed from +0xB8 / +0x1B0 and the part slots
        request.bikeCaptured = nullptr;
        request.riderCaptured = nullptr;
    }
    request.passengerCaptured = request.passenger ? capturedOf(request.passengerObject) : nullptr;
    {   // the PORTED model shadow SLUS 0x80025EE0 of this view (shadow_product.h)
        std::vector<RaceScene::PortedShadowQuad> sq;
        for (const rr::game::ShadowQuad& q : models_.shadows.quads) {
            if (q.view != static_cast<uint32_t>(vp)) continue;
            RaceScene::PortedShadowQuad o;
            float moved[3]; // a frame drawn between two steps moves the shadow with its object (vr_comfort.h)
            rrgame::ProductFrameInterp().Shift(q.object, moved);
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 3; ++k) o.world[c][k] = q.world[c][k] + moved[k];
            for (int k = 0; k < 3; ++k) o.colour[k] = q.colour[k];
            o.slot = q.slot; // drawn at the slot the port linked it into, in its object's table
            for (int c = 0; c < 4; ++c) { // the packet's points (its order v0 v1 v3 v2) as q0..q3
                const int w = c < 2 ? c : 5 - c;
                o.sxy[c][0] = q.sxy[w][0];
                o.sxy[c][1] = q.sxy[w][1];
            }
            o.haveSxy = true;
            o.object = q.object;
            sq.push_back(o);
        }
        scene.SetPortedShadows(std::move(sq), rr::game::ShadowPortOn() &&
                                                  (session.Players() == 1 || rr::game::Mp2On())); // two players: 0x80026960's sprites (below)
    }
    // The picture's width over the console's 384 columns (the 4:3 frame and --parity: 1)
    request.sideSqueeze = (options.wideScreen && !options.parity && viewH > 0)
                              ? std::max(1.0f, (static_cast<float>(viewW) / viewH) / (4.0f / 3.0f)) : 1.0f;
    {   // render\gte_proj.h: the cells at the GTE's own SXY, mapped onto the picture
        // by the same projection: ndc x = m0 (SX - 192) / H - m8, ndc y = -m5 (SY - 120) / (H 3412/4096) - m9
        // (x / w = (SX - 192) / H and the GL camera's y is the GTE's -y), clip z = -m10 w + m14. One player only.
        const float h = kGteH, hy = kGteH * kGteAspectRow;
        // Two players too - each view's GTE state is its own: the cells' slots 12 v..
        // (GteBegin), the model draw's per-view captures, and the offset (OFX, OFY) = the view rectangle's centre the
        // pass projected with (fx_runtime.h), which is also where this view's float camera is centred.
        // RRJB_GTE_2P=off: two players on the float camera (the control).
        static const bool twoOff = std::getenv("RRJB_GTE_2P") != nullptr && std::strcmp(std::getenv("RRJB_GTE_2P"), "off") == 0;
        const bool two = session.Players() == 2;
        const float ofx = two ? static_cast<float>(fx_.ViewOffsetX(static_cast<uint32_t>(vp))) : 192.0f;
        const float ofy = two ? static_cast<float>(fx_.ViewOffsetY(static_cast<uint32_t>(vp))) : 120.0f;
        request.gteProj = gv.consoleCamera && !options.preciseVertices && (session.Players() == 1 || (two && !twoOff));
        request.gteMap[0] = proj.m[0] / h;
        request.gteMap[1] = -proj.m[0] * ofx / h - proj.m[8];
        request.gteMap[2] = -proj.m[5] / hy;
        request.gteMap[3] = proj.m[5] * ofy / hy - proj.m[9];
        request.gteDepth[0] = -proj.m[10];
        request.gteDepth[1] = proj.m[14];
        request.gteOfx = static_cast<int32_t>(ofx);
        request.gteOfy = static_cast<int32_t>(ofy);
        // sky_product.h: this view's sky packets, and how far its picture reaches in console x
        // (the wide picture's sky is drawn on to the edges from the next frame). Not a console camera (the head camera,
        // a stereo eye): the renderer's own sky in world directions.
        rr::game::SkySetPicture(vp, (-1.0 - request.gteMap[1]) / request.gteMap[0], (1.0 - request.gteMap[1]) / request.gteMap[0]);
        rr::render::SkyGpuSubmit(gv.consoleCamera ? rr::game::SkyFramePacketsOf(vp) : nullptr, &rr::game::SkyHostVram(),
                                 rr::game::SkyGradientPorted());
    }
    prof.Mark(2);
    scene.Draw(request);
    prof.Mark(3);
    {
        const rr::render::SubdivStats& st = scene.LastSubdiv();
        SubdivStats& run = counters.subdiv;
        run.roadStrips += st.roadStrips;
        run.roadPieces += st.roadPieces;
        run.nearPrims += st.nearPrims;
        run.splitPrims += st.splitPrims;
        run.pieces += st.pieces;
        run.culledPrims += st.culledPrims;
        run.refused += st.refused;
        run.backPrims += st.backPrims;
    }
    weapons_.Draw(scene, viewProj, session.ArenaRam(), session.Bikes()[0].ownerAddress, request.riderModel, request.riderLocal); // weapon_draw.h
    weapons_.DrawExtras(scene, viewProj); // VR: the weapons on the player's hips (none on the desktop)

    // The opponents, drawn with the same machine model - the ported AI decides where they
    // are, and the renderer does not care which bike it is drawing.
    if (rivalDrawnLast_.size() != session.Bikes().size()) {
        rivalDrawnLast_.assign(session.Bikes().size(), 0);
        rivalAirLast_.assign(session.Bikes().size(), 0);
    }
    for (size_t i = 1; i < session.Bikes().size(); ++i) {
        const uint32_t rivalEntity = session.Bikes()[i].entityAddress;
        uint32_t rivalFlagsC = 0;
        std::memcpy(&rivalFlagsC, session.ArenaRam() + ((rivalEntity + 0x238u) & 0x1FFFFFu), 4);
        bool drawn = false;
        const auto account = [&]() { // after the tests below, view 0 only
            if (vp != 0) return;
            if (rivalDrawnLast_[i] && rivalAirLast_[i] && !drawn && !session.BikeLive(i)) {
                ++counters.rivalsVanishedInAir;
                char b[200];
                std::snprintf(b, sizeof(b),
                              "        VANISH bike %zu f%ld: drawn in the air last frame, retired to the dormant "
                              "list this frame (view distance +0x2C %d)\n",
                              i, frames, static_cast<int>(session.ArenaRam()[(rivalEntity + 0x2Cu) & 0x1FFFFFu] |
                                                          (session.ArenaRam()[(rivalEntity + 0x2Du) & 0x1FFFFFu] << 8) |
                                                          (session.ArenaRam()[(rivalEntity + 0x2Eu) & 0x1FFFFFu] << 16) |
                                                          (session.ArenaRam()[(rivalEntity + 0x2Fu) & 0x1FFFFFu] << 24)));
                counters.vanishNote += b;
            }
            rivalDrawnLast_[i] = drawn ? 1 : 0;
            rivalAirLast_[i] = (rivalFlagsC & 0x400u) ? 1 : 0;
        };
        // A dormant bike (+0x140 == 0) is only a road coordinate in the original: it is not drawn.
        if (!session.BikeLive(i)) { account(); continue; }
        // a rider off his bike is an object of its own in the original's draw list (rider_pose.h
        // RiderOwnFrame) - the pair is drawn when the bike or that rider passes the tests below
        const uint32_t rivalRider = session.Bikes()[i].ownerAddress;
        float rivalOwnAxis[3][3], rivalOwnOrigin[3];
        const bool rivalOwn = rr::game::RiderOwnFrame(session.ArenaRam(), rivalRider, rivalOwnAxis, rivalOwnOrigin);
        const auto riderVisible = [&]() {
            if (!rivalOwn || !inCells(rivalRider)) return false;
            if (!options.drawRangeOff && !maxDetail && !rr::game::InDrawRange(session.ArenaRam(), rivalRider, vp)) return false;
            return !options.cull || scene.SphereVisible(rivalOwnOrigin, 2.5f);
        };
        bool bikePasses = inCells(rivalEntity); // the model draw 0x80067690 draws a machine only in a cell of the view's draw list (cell_view.h)
        // ...and ModelVisible 0x80067AC4 only within its kind's range: 120 world units for a bike
        // (cell_view.h InDrawRange). A live rival is kept up to 350 units by the population window,
        // so without this test one flying off a crest 300 units ahead was seen to go up and, on its
        // retirement to the dormant list, vanish in the air.
        if (bikePasses && !rr::game::InDrawRange(session.ArenaRam(), rivalEntity, vp)) {
            if (vp == 0) ++counters.rivalFramesPastRange;
            if (!options.drawRangeOff && !maxDetail) bikePasses = false; // maximum detail: no draw range
        }
        if (bikePasses && sphereCulled(rivalEntity, 4.0f)) bikePasses = false; // the frustum (maximum detail's cull)
        if (!bikePasses) {
            if (!riderVisible()) { account(); continue; }
            if (vp == 0) ++RiderPlacementStats().rivalsByRider;
        }
        ++counters.objectsDrawn;
        drawn = true;
        account();
        float p[3], t[3], l[3], n[3];
        session.BikePlacement(i, p, t, l, n);
        const float sense = n[1] <= 0.0f ? 1.0f : -1.0f;
        float rivalUp[3];
        for (int k = 0; k < 3; ++k) rivalUp[k] = n[k] * sense;
        RoadFrame rivalFrame;
        for (int k = 0; k < 3; ++k) {
            rivalFrame.pos[k] = p[k];
            rivalFrame.tangent[k] = t[k];
            rivalFrame.lateral[k] = l[k];
            rivalFrame.normal[k] = n[k];
        }
        DrawRequest rival;
        rival.viewProj = viewProj;
        for (int k = 0; k < 3; ++k) rival.eye[k] = eye[k];
        rival.haveMachine = true;
        rival.bikeObject = rivalEntity; // its cell's table
        rival.bikeModel = BikeFrameFromGround() ? MachineMatrixAt(rivalFrame, rivalUp, p, kModelToWorld)
                                                : RecordModelMatrix(session.ArenaRam(), rivalEntity, kModelToWorld);
        if (!RivalsAsPlayer()) { // its own models and palettes (race_scene_rivals.cpp)
            ObjectLook(session.ArenaRam(), rivalEntity, rival.bikeModelId, rival.bikeA1);
            ObjectLook(session.ArenaRam(), session.Bikes()[i].ownerAddress, rival.riderModelId, rival.riderA1);
        }
        const float* rivalSeat = // SeatVertex per LOD (race_scene.h RiderAttach(lod)) of its own bike model
            rival.bikeModelId != 0
                ? scene.RiderAttachFor(rival.bikeModelId, rival.riderModelId, lodOf(session.Bikes()[i].entityAddress))
                : scene.RiderAttach(lodOf(session.Bikes()[i].entityAddress));
        rr::game::RiderPoseView rivalPassenger; // the two-seat machine (SidecarRequest)
        rival.bikeLod = lodOf(session.Bikes()[i].entityAddress);
        // player 2's machine rides its own rig (race_scene_sidecar2.cpp; RRJB_MP2=off: player 1's)
        rival.sidecarRig = (i == 1 && session.Players() == 2 && scene.HasSidecarFor(1)) ? 1 : 0;
        SidecarRequest(scene, session.ArenaRam(), rivalEntity, vp, rival, rivalPassenger, maxDetail);
        if (rival.sidecar) rivalSeat = scene.SidecarAttachFor(rival.sidecarRig, rival.bikeLod);
        rr::PartMatrix rivalClimb0, rivalClimb2; // the re-seat's climb (rider_pose.h MachineClimbSlots)
        const bool rivalClimb = !rival.sidecar && rr::game::MachineClimbSlots(session.ArenaRam(), rivalEntity, rivalClimb0, rivalClimb2);
        if (rivalClimb) rival.bikeModel = TurnedBySlot(rival.bikeModel, rivalClimb0);
        if (vp == 0) {
            if (rivalClimb) ++RiderPlacementStats().rivalClimb;
            if (rivalOwn) ++RiderPlacementStats().rivalOwn;
            if (rivalOwn && std::getenv("RRJB_RIDER_PLACE_TRACE")) { // DEVELOPMENT: where the thrown riders are drawn
                double db = 0.0, de = 0.0;
                for (int k = 0; k < 3; ++k) {
                    db += (rivalOwnOrigin[k] - rival.bikeModel.m[12 + k]) * (rivalOwnOrigin[k] - rival.bikeModel.m[12 + k]);
                    de += (rivalOwnOrigin[k] - eye[k]) * (rivalOwnOrigin[k] - eye[k]);
                }
                std::printf("rider place: f%ld bike %zu rider off, %.1f units from its bike, %.1f from the eye%s\n", frames, i,
                            std::sqrt(db), std::sqrt(de), bikePasses ? "" : " (the bike not drawn)");
            }
        }
        rival.riderModel = RiderMatrix(rival.bikeModel, rivalSeat, scene.RiderRelative());
        rr::game::RiderPoseView rivalPose; // rider_pose.h
        if (rr::game::ReadRiderPose(session.ArenaRam(), session.Bikes()[i].ownerAddress, rivalPose)) {
            rr::game::AnimDetailInfo detail; // maximum detail - the full animation at every distance
            if (rr::game::AnimDetailOn()) rr::game::AnimDetailPose(session_.MutableArenaRam(), session.Bikes()[i].ownerAddress, rivalPose, &detail);
            else if (AnimDetailTrace()) detail = rr::game::AnimDetailDescribe(session.ArenaRam(), session.Bikes()[i].ownerAddress);
            if (vp == 0 && AnimDetailTrace()) TraceAnimDetail(frames, 'R', session.Bikes()[i].ownerAddress, session.ArenaRam(), gv.eye, detail);
            rival.riderLocal = rivalPose.local;
            rival.riderModel = rivalClimb ? ChildRiderMatrix(rival.bikeModel, rivalSeat, rivalPose.root, rivalClimb2)
                                          : PosedRiderMatrix(rival.bikeModel, rivalSeat, rivalPose.root);
        }
        if (rivalOwn) rival.riderModel = OwnRiderMatrix(rivalOwnAxis, rivalOwnOrigin);
        rr::PartMatrix rivalBikeParts[5]; // bike_pose_product.h
        if (rr::game::ReadBikeParts(session.ArenaRam(), session.Bikes()[i].entityAddress, rivalBikeParts))
            rival.bikeLocal = rivalBikeParts;
        rr::PartMatrix rig2Parts[8]; // player 2's own rig: its bike's own six slots (jail_session.cpp JailRigParts)
        if (rival.sidecar && rival.sidecarRig == 1) {
            if (!options.rigOff && rr::game::ReadMachineParts(session.ArenaRam(), rivalEntity, rig2Parts, 8) ==
                                       static_cast<int>(scene.SidecarPartsFor(1)))
                rival.sidecarLocal = rig2Parts;
            if (vp == 0) ++rr::game::Mp2().rig2Frames;
        }
        rival.bikeLod = lodOf(session.Bikes()[i].entityAddress);  // cell_view.h
        rival.riderLod = lodOf(session.Bikes()[i].ownerAddress);
        {   // the model light's object flags (race_scene.h DrawRequest::bikeObjFlags)
            uint32_t fb = 0, fr = 0;
            std::memcpy(&fb, session.ArenaRam() + ((rivalEntity + 0x24u) & 0x1FFFFFu), 4);
            std::memcpy(&fr, session.ArenaRam() + ((session.Bikes()[i].ownerAddress + 0x24u) & 0x1FFFFFu), 4);
            rival.bikeObjFlags = fb;
            rival.riderObjFlags = fr;
        }
        rival.bikeCaptured = capturedOf(rivalEntity);
        rival.riderCaptured = capturedOf(session.Bikes()[i].ownerAddress);
        rival.passengerCaptured = rival.passenger ? capturedOf(rival.passengerObject) : nullptr;
        scene.DrawMachineOnly(rival);
        weapons_.Draw(scene, viewProj, session.ArenaRam(), session.Bikes()[i].ownerAddress, rival.riderModel, rival.riderLocal); // weapon_draw.h
    }
    prof.Mark(4);
    // The traffic cars (pool 3) the PORTED spawner and traffic pass keep in the arena.
    // the model draw's cell test and ModelVisible's kind-3 range (12480 of +0x2C)
    size_t& carsPastRange = counters.carsPastRange;
    const std::function<bool(uint32_t)> carKeep = [&session, vp, &carsPastRange, &objVisible, maxDetail, &sphereCulled,
                                                   this](uint32_t car) {
        if (maxDetail) { // no cell test, no draw range: the frustum
            if (sphereCulled(car, 6.0f)) return false;
            ++counters.objectsDrawn;
            return true;
        }
        if (!rr::game::InDrawnCell(session.ArenaRam(), car, vp)) return false;
        if (!objVisible(car)) return false; // ModelVisible hid it in this view
        if (!rr::game::InDrawRange(session.ArenaRam(), car, vp)) {
            if (vp == 0) ++carsPastRange;
            return false;
        }
        return true;
    };
    // Each car at the PORTED model draw's SXY of this view
    const std::function<const CapturedVerts*(uint32_t)> carCaptured = [&capturedOf](uint32_t car) { return capturedOf(car); };
    traffic_.forceLod0 = maxDetail;
    counters.trafficDrawn = traffic_.Draw(scene, viewProj, session.ArenaRam(), kModelToWorld, &counters.trafficUnknown,
                                          options.drawRangeOff && !maxDetail ? nullptr : &carKeep,
                                          rr::game::GteObjectsOn() && !maxDetail ? &carCaptured : nullptr);
    counters.trafficMaxDrawn = std::max(counters.trafficMaxDrawn, counters.trafficDrawn);
    {   // the pedestrians (ped_draw.h): live ones filed in a cell of this view's draw list
        std::vector<rr::render::PedInstance> pi;
        for (const rr::game::LivePed& lp : rr::game::LivePeds(session.ArenaRam())) {
            if (!inCells(lp.entity)) continue;
            if (!objVisible(lp.entity)) continue; // ModelVisible hid it in this view
            if (maxDetail && sphereCulled(lp.entity, 2.5f)) continue;
            if (maxDetail) ++counters.objectsDrawn;
            rr::render::PedInstance in;
            in.model = lp.model;
            in.entity = lp.entity; // its cell's table
            in.sheet = lp.sheet;
            for (int k = 0; k < 3; ++k) {
                in.pos[k] = lp.pos[k];
                in.root[k] = lp.root[k];
            }
            for (int k = 0; k < 9; ++k) in.rows[k] = lp.rows[k];
            if (rr::game::GteObjectsOn() && !maxDetail) { // the LOD ModelVisible chose and the model draw's capture
                in.lod = static_cast<int>(static_cast<int8_t>(session.ArenaRam()[(lp.entity + 8u) & 0x1FFFFFu]));
                in.captured = std::getenv("RRJB_GTE_PEDS") ? nullptr : capturedOf(lp.entity); // DEVELOPMENT
            }
            for (int j = 0; j < 17; ++j)
                for (int k = 0; k < 9; ++k) in.slots[j].m[k] = lp.parts[j][k];
            {   // maximum detail - the full animation at every distance (anim_detail.h)
                rr::game::AnimDetailInfo detail;
                rr::game::RiderPoseView full;
                if (rr::game::AnimDetailOn() && rr::game::AnimDetailPose(session_.MutableArenaRam(), lp.entity, full, &detail)) {
                    for (int j = 0; j < 17; ++j) in.slots[j] = full.local[j];
                    for (int k = 0; k < 3; ++k) in.root[k] = full.root[k];
                } else if (!rr::game::AnimDetailOn() && AnimDetailTrace()) {
                    detail = rr::game::AnimDetailDescribe(session.ArenaRam(), lp.entity);
                }
                if (vp == 0 && AnimDetailTrace()) TraceAnimDetail(frames, 'P', lp.entity, session.ArenaRam(), gv.eye, detail);
            }
            {   // +0x24 for the model light (ped_draw.h PedInstance::flags)
                uint32_t f24 = 0;
                std::memcpy(&f24, session.ArenaRam() + ((lp.entity + 0x24u) & 0x1FFFFFu), 4);
                in.flags = f24;
            }
            pi.push_back(in);
        }
        const size_t pd = peds_.Draw(scene, viewProj, pi);
        if (vp == 0 && pd > 0) {
            rr::game::PedRunTotals().drawn += pd;
            ++rr::game::PedRunTotals().drawFrames;
        }
    }
    // The ported shadow at its slots over the view's opaque world, then the clip planes off;
    // the effect packets at their slots of the (second) table
    prof.Mark(5);
    scene.FlushOtShadows(viewProj);
    scene.OtEnd();
    fxDraw_.SetOt(scene.OtOrder(), scene.OtFxPass().nearOffset, scene.OtFxPass().shift, scene.OtFxPass().maxSlot,
                  scene.OtFxPass().base);
    if (rr::game::CellSortOn()) { // each packet in its entity's cell's table (0x80067770)
        const auto table = [&scene](int t) {
            const auto& m = scene.OtPass(t);
            return rr::render::FxDraw::OtTable{m.nearOffset, m.shift, m.maxSlot, m.base};
        };
        fxDraw_.SetOtTables(table(1), table(2), [&scene](uint32_t e) { return scene.OtTableOfEntity(e); }, 2);
    }
    if (counters.trafficDrawn > 0 && vp == 0) ++counters.trafficFramesWithCars;

    {   // the effects' packets (fx_draw.h), over the world and under the HUD - this view's: the pass ran
        // per view with its render camera and GTE offset (fx_runtime.h Frame), two players included. Console-screen
        // sprites: only on the game's own camera.
        rr::render::FxDraw::Place fxPlace;
        fxPlace.view = static_cast<uint32_t>(vp);
        if (session.Players() == 2) { // the GTE offset the pass projected this view with
            fxPlace.cx = static_cast<float>(fx_.ViewOffsetX(fxPlace.view));
            fxPlace.cy = static_cast<float>(fx_.ViewOffsetY(fxPlace.view));
            fxPlace.halfH = static_cast<float>(gv.splitRect[3]) * 0.5f;
        } else if (gv.shownView) { // the shown rectangle about its own centre (race_scene.h kShown*)
            fxPlace.cx = rr::render::kShownX + rr::render::kShownW * 0.5f;
            fxPlace.cy = rr::render::kShownY + rr::render::kShownH * 0.5f;
            fxPlace.halfH = rr::render::kShownH * 0.5f;
        }
        if (!models_.shadows.sprites.empty() && gv.consoleCamera) // two players: SLUS 0x80026960's sprites (mp2_product.h)
            rr::game::Mp2().shadowDrawn +=
                fxDraw_.Draw(models_.shadows.sprites, fx_.Vram(), pictureAspect, gv.nearPlane, gv.farPlane, fxPlace);
        size_t n = !options.drawFx || !gv.consoleCamera ? 0 : fxDraw_.Draw(fx_.Packets(), fx_.Vram(), pictureAspect,
                                                                           gv.nearPlane, gv.farPlane, fxPlace);
        // not the console's camera (the head camera, a VR eye): the same packets as billboards at their world places
        float camEye[3], camRows[3][3];
        if (options.drawFx && !gv.consoleCamera && ConsoleRenderCamera(session.ArenaRam(), vp, camEye, camRows))
            n = fxDraw_.DrawWorld(fx_.Packets(), fx_.Vram(), fxPlace.view, camEye, camRows,
                                  static_cast<float>(fx_.ViewOffsetX(fxPlace.view)),
                                  static_cast<float>(fx_.ViewOffsetY(fxPlace.view)), gv.view, viewProj);
        counters.fxDrawnMax = std::max(counters.fxDrawnMax, n);
        if (n > 0) ++counters.fxFramesDrawn;
        if (n > 0 && std::getenv("RRJB_FX_TRACE")) { // DEVELOPMENT: which frames draw which states
            size_t st[8] = {};
            for (const rr::game::FxPacket& p : fx_.Packets())
                if (p.view == fxPlace.view) ++st[p.state & 7u];
            std::printf("fx: frame %ld view %d drawn %zu (spark %zu spray %zu burst %zu)\n", frames, vp, n, st[1],
                        st[2], st[7]);
            for (const rr::game::FxPacket& p : fx_.Packets())
                if (p.view == fxPlace.view && std::getenv("RRJB_FX_TRACE")[0] == '2')
                    std::printf("fx:   view %u state %u at (%d,%d) centre (%d,%d)\n", p.view, p.state, p.x[0],
                                p.y[0], fx_.ViewOffsetX(p.view), fx_.ViewOffsetY(p.view));
        }
    }
}

} // namespace rrgame
