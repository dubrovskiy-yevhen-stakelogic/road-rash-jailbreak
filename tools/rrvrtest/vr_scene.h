#pragma once
// The VR proof's scene: the world of one race drawn by the SHARED renderer
// (render/race_scene.h, the same RaceScene rrgame and rrview draw) from any view the OpenXR layer asks for.
//
// No simulation runs: the world is loaded exactly as rrgame loads it (LoadRaceWorld, JoinCellRegion7, the level
// look, the cell soup, the cell textures, the panorama and the gradient, the props, the player's machine), and the
// viewer stands on the route's centre line at a fixed distance with the machine parked a few metres ahead - a real,
// textured frame of the game in stereo, with nothing but the head moving. The phase-2 integration replaces this with
// rrgame's own per-view draw fed by the ported race camera.
#include "platform/xr/xr_math.h"
#include "render/race_scene.h"
#include "render/scene_geometry.h"
#include "rrvfs/disc_image.h"
#include "game/world.h"

#include <memory>
#include <vector>

namespace rrvr {

class ProofScene {
public:
    // Loads race (set, raceId) and places the viewer `distance` world units along its route. A GL context must be
    // current. Throws std::runtime_error when the race has no road.
    ProofScene(const rr::DiscImage& disc, int set, int raceId, float distance);

    // The XR space's origin on the road: the centre line at the viewer's distance, the route's tangent ahead, the
    // road's normal up. `eyeHeight` metres are added by the runtime's own head height (LOCAL space: the head starts
    // at y = 0, so the anchor is lifted by a seated rider's eye height).
    rr::xr::WorldAnchor Anchor(float unitsPerMetre, float eyeHeightMetres) const;
    // Moves the viewer (and the parked machine) to another route distance (wraps at the route's end).
    void Place(float distance, bool log);
    float Distance() const { return distance_; }

    // Draws the frame for one view into the bound framebuffer's viewport (x, y, w, h): clears it, the sky gradient,
    // the panorama, the cells, the props and the machine - RaceScene::Draw with the view's own camera.
    void DrawView(const rr::render::Mat4& proj, const rr::xr::WorldEye& view, int x, int y, int w, int h, bool ps1Look);

    // The desktop camera of the same spot (the original's vertical field, the machine ahead): what the theatre quad
    // and the desktop mirror show.
    void DrawFlat(int x, int y, int w, int h, bool ps1Look);

    size_t CellCount() const { return drawCells_.size(); }
    const rr::RaceWorld& World() const { return world_; }

private:
    rr::RaceWorld world_;
    std::vector<rr::CellData> drawCells_;
    rr::render::RaceScene scene_;
    rr::render::RoadFrame frame_;
    float up_[3] = {0, -1, 0};
    float distance_ = 0.0f;
    bool roadKnown_ = false;
    uint16_t road_ = 0, roadDistance_ = 0;
    rr::render::Mat4 bikeModel_, riderModel_;
};

} // namespace rrvr
