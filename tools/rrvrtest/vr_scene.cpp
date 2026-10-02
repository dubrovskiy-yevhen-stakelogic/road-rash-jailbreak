#include "vr_scene.h"

#include "render/gl_api.h"
#include "rrformats/level_bundle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace rrvr {
namespace {

using rr::render::Mat4;

void Normalise(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
        for (int k = 0; k < 3; ++k) v[k] /= l;
}

} // namespace

ProofScene::ProofScene(const rr::DiscImage& disc, int set, int raceId, float distance) {
    if (const auto gb = disc.Find("DATA/GAMEBIN1.DAT")) // the level bundle this race loads (as rrgame decides it)
        std::printf("%s\n", rr::SelectLevelBundle(disc.ReadFile(*gb), raceId).c_str());
    world_ = rr::LoadRaceWorld(disc, set, raceId);
    if (world_.path.size() < 2) throw std::runtime_error("rrvrtest: that race assembled no drivable road");
    // The cells exactly as rrgame builds them (tools\rrgame\main.cpp): region 7 joined from the type-9 chunks, the
    // level's look before the soup (the soup bakes every vertex's shade and texture window from it), one run per
    // (cell, group, page) so the per-group level of detail can choose every frame.
    drawCells_ = world_.cells;
    size_t joinFailed = 0;
    const size_t joined = rr::render::JoinCellRegion7(disc, set, world_.legs, drawCells_, &joinFailed);
    scene_.CreatePrograms();
    scene_.SetOtOrder(false); // the depth buffer: the ordering-table order is a flat-screen parity feature
    // Perspective-correct texturing and no subdivision: the original's subdividers decide what to cut and what to drop
    // by the console's 384 x 240 screen outcodes, which leave the ground within ~3.5 m of a VR eye undrawn (measured:
    // rrvrtest --mock, the lower 30 % of both eyes black with them, complete without them).
    scene_.SetAffine(false, false);
    scene_.LoadLevel(disc, set, raceId);
    std::vector<rr::render::CellRange> ranges;
    const rr::TriangleSoup soup = rr::render::BuildCellSoup(drawCells_, 3, false, false, &ranges, &scene_.Look());
    scene_.SetCells(soup, ranges);
    scene_.SetCellData(&drawCells_, set);
    scene_.LoadCellTextures(disc, set, world_.legs, drawCells_, true);
    scene_.LoadSky(disc, set, world_.legs);
    scene_.LoadSkyGradient(disc, rr::LevelBundleIndexForRace(raceId));
    scene_.LoadProps(disc, world_.cells, true, false, 0);
    scene_.LoadMachine(disc, true, {}, {});
    std::printf("rrvrtest: race %d/%d: %zu legs, %zu slices, %zu cells (region 7 joined %zu, %zu failed), %zu runs\n", set,
                raceId, world_.legs.size(), world_.path.size(), drawCells_.size(), joined, joinFailed, ranges.size());
    Place(distance, true);
}

void ProofScene::Place(float distance, bool log) {
    const float length = world_.path.empty() ? 0.0f : static_cast<float>(world_.assembledLength);
    distance_ = std::fmod(std::max(distance, 0.0f), std::max(length - 10.0f, 1.0f));
    distance = distance_;
    size_t slice = 0;
    frame_ = rr::render::SampleRoad(world_.path, distance, &slice);
    // in the y-down world "up" is the sense of the road normal with the negative y (rrview/main.cpp)
    const float sense = frame_.normal[1] <= 0.0f ? 1.0f : -1.0f;
    for (int k = 0; k < 3; ++k) up_[k] = frame_.normal[k] * sense;
    roadKnown_ = slice < world_.pathRoad.size() && !world_.pathIsJunction[slice];
    road_ = roadKnown_ ? world_.pathRoad[slice] : 0;
    roadDistance_ = roadKnown_ ? world_.pathRoadDistance[slice] : 0;
    scene_.SelectSky(roadKnown_, road_, roadDistance_);
    // The machine 4 m ahead on the centre line: its lowest vertex (375 model units below the origin, BBLEVEL1.GEO id
    // 100) on the road, as rrview parks it (rrview/main.cpp).
    const float modelToWorld = 1.0f / rr::render::kModelUnitsPerWorldUnit;
    const rr::render::RoadFrame ahead = rr::render::SampleRoad(world_.path, distance + 4.0f);
    float origin[3];
    for (int k = 0; k < 3; ++k) origin[k] = ahead.pos[k] + up_[k] * (375.0f * modelToWorld);
    bikeModel_ = rr::render::MachineMatrixAt(ahead, up_, origin, modelToWorld);
    riderModel_ = rr::render::RiderMatrix(bikeModel_, scene_.RiderAttach(), scene_.RiderRelative());
    if (log)
        std::printf("rrvrtest: viewer at route distance %.1f of %.0f (slice %zu, road %u at %u%s), world (%.1f, %.1f, %.1f)\n",
                    double(distance), world_.assembledLength, slice, unsigned(road_), unsigned(roadDistance_),
                    roadKnown_ ? "" : ", junction", double(frame_.pos[0]), double(frame_.pos[1]), double(frame_.pos[2]));
}

rr::xr::WorldAnchor ProofScene::Anchor(float unitsPerMetre, float eyeHeightMetres) const {
    // The XR space stays level whatever the road's camber or slope (a tilted horizon in a headset is what makes
    // people sick): up is the world's vertical (y is down in this world), ahead the tangent's horizontal part.
    float origin[3];
    const float vertical[3] = {0.0f, -1.0f, 0.0f};
    for (int k = 0; k < 3; ++k) origin[k] = frame_.pos[k] + vertical[k] * eyeHeightMetres * unitsPerMetre;
    const float ahead[3] = {frame_.tangent[0], 0.0f, frame_.tangent[2]};
    return rr::xr::AnchorFrom(origin, ahead, vertical, unitsPerMetre);
}

namespace {

// One RaceScene frame from (eye, forward, up) under `proj` - the DrawRequest rrgame fills (tools\rrgame\main.cpp)
// minus everything the simulation owns.
void DrawScene(rr::render::RaceScene& scene, const Mat4& proj, const float eye[3], const float forwardIn[3],
               const float upIn[3], bool roadKnown, uint16_t road, uint16_t roadDistance, const Mat4& bikeModel,
               const Mat4& riderModel, int x, int y, int w, int h, bool ps1Look) {
    float forward[3] = {forwardIn[0], forwardIn[1], forwardIn[2]};
    Normalise(forward);
    const float target[3] = {eye[0] + forward[0], eye[1] + forward[1], eye[2] + forward[2]};
    const Mat4 view = rr::render::LookAt(eye, target, upIn);
    const Mat4 viewProj = rr::render::Multiply(proj, view);
    glViewport(x, y, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    scene.SetConsoleLines(240.0f);
    scene.PrepareSkyGradient(viewProj, eye, target, scene.SunYaw());
    rr::render::DrawRequest request;
    request.viewProj = viewProj;
    for (int k = 0; k < 3; ++k) request.eye[k] = eye[k];
    request.currentRoadKnown = roadKnown;
    request.currentRoad = road;
    request.currentRoadDistance = roadDistance;
    request.haveMachine = true;
    request.bikeModel = bikeModel;
    request.riderModel = riderModel;
    request.haveCellView = true;
    // the cell view's right is up x forward, as rrgame and rrview build it
    float right[3] = {upIn[1] * forward[2] - upIn[2] * forward[1], upIn[2] * forward[0] - upIn[0] * forward[2],
                      upIn[0] * forward[1] - upIn[1] * forward[0]};
    Normalise(right);
    for (int k = 0; k < 3; ++k) {
        request.cellView.eye[k] = eye[k];
        request.cellView.forward[k] = forward[k];
        request.cellView.right[k] = right[k];
        request.cellView.up[k] = upIn[k];
    }
    scene.Draw(request);
    if (ps1Look) scene.PostProcess(x, y, w, h, false);
}

} // namespace

void ProofScene::DrawView(const Mat4& proj, const rr::xr::WorldEye& v, int x, int y, int w, int h, bool ps1Look) {
    DrawScene(scene_, proj, v.eye, v.forward, v.up, roadKnown_, road_, roadDistance_, bikeModel_, riderModel_, x, y, w,
              h, ps1Look);
}

void ProofScene::DrawFlat(int x, int y, int w, int h, bool ps1Look) {
    // The chase camera of the rr-race trace (race_scene.h kModelUnitsPerWorldUnit): 3.36 world units behind the
    // machine and 1.09 above it, looking at it.
    float eye[3], forward[3];
    const float bike[3] = {bikeModel_.m[12], bikeModel_.m[13], bikeModel_.m[14]};
    for (int k = 0; k < 3; ++k) eye[k] = bike[k] - frame_.tangent[k] * 3.4f + up_[k] * 1.1f;
    for (int k = 0; k < 3; ++k) forward[k] = bike[k] + up_[k] * 0.5f - eye[k];
    const Mat4 proj = rr::render::Perspective(rr::render::OriginalVerticalFov(), float(w) / float(h), 0.25f, 20000.0f);
    DrawScene(scene_, proj, eye, forward, up_, roadKnown_, road_, roadDistance_, bikeModel_, riderModel_, x, y, w, h,
              ps1Look);
}

} // namespace rrvr
