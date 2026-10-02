#pragma once
// The player's hands in the headset (vr_handlebars.h): the GT2 VR hands - UltimateXR's BigHands meshes and albedo
// (third_party\vrhands, MIT; embedded by cmake\vr-hand-assets.cmake), each vertex carrying four baked finger poses (open,
// grip, trigger, grip + trigger) blended by the controller's grip and trigger values, and the hand basis from the
// OpenXR grip / aim poses - both after the gt2-play project's src\gt2view\vr_driving_visuals.cpp (MiamiVR's VRHandModel
// morphs and basis). Drawn textured and lit into the eye with the scene's depth; the vertex stage declares
// `uniform mat4 uViewProj;` and uploads it with UploadViewProj, so single-pass stereo (render/multiview.h) takes it.
// Portable GL 3.3 / ES 3.2.
//
// A hand is given as an OpenXR grip pose in the WORLD: its position, the world images of its +X (`gripRight`), +Y
// (`gripUp`) and -Z (`gripForward`) axes, and the aim pose's forward (the fingers' direction; the grip's forward when
// there is no aim pose), `scale` world units per metre.
#include "render/gl_api.h"
#include "render/mat4.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rrgame {

// The embedded assets (the generated vr_hand_assets.cpp): 0 the left mesh (UXRH), 1 the right, 2 the albedo PNG.
std::span<const uint8_t> VrHandAsset(int index);

struct GloveDraw {
    bool visible = false;
    bool right = false;
    float origin[3] = {};                               // the grip pose's position (world)
    float gripRight[3] = {1, 0, 0}, gripUp[3] = {0, 1, 0}, gripForward[3] = {0, 0, 1};
    float aimForward[3] = {0, 0, 1};
    float scale = 1.0f;                                 // world units per metre
    float grip = 0.0f;                                  // 0 open .. 1 the fingers closed (a fist, or round a grip)
    float trigger = 0.0f;                               // 0 .. 1 the index finger closed
};

// A grip's marker: where to reach (cyan), within reach (green).
struct GripMarker {
    bool visible = false;
    bool inReach = false;
    float centre[3] = {};           // world
    float axis[3] = {1, 0, 0};      // along the bar (unit)
    float up[3] = {0, -1, 0};       // the bike's up (unit)
    float scale = 1.0f;
};

class VrHandsDraw {
public:
    // Parses the embedded meshes and decodes the albedo once (a GL context current); false (and one line printed) when
    // they are unusable - the hands are then not drawn.
    bool Load();
    void Draw(const rr::render::Mat4& viewProj, const GloveDraw gloves[2], const GripMarker markers[2]);
    // The model matrix (the mesh's metres -> the world) Draw gives a hand, and the centre of the closed
    // fist in the mesh: the axis of whatever the hand holds runs through it (BigHandRight's grip + trigger pose,
    // probed: the largest empty circle of its central slices, x 0.034..0.048 y 0.000..0.006 m, radius 2.3..2.8 cm;
    // the grip pose itself sits at x kPalmBack = 0.055). The left mesh is the right one mirrored in y.
    static rr::render::Mat4 HandModel(const GloveDraw& g);
    static constexpr float kFistCentre[3] = {0.040f, 0.0f, 0.0f};
    // Moves `g.origin` so the fist's centre lands on `point` (world) with the basis g already has.
    static void SeatFist(GloveDraw& g, const float point[3]);
    // The fist centre of `g` in the world (HandModel applied to kFistCentre).
    static void FistCentre(const GloveDraw& g, float out[3]);
    // The number of hand vertices the last Draw sent (the log).
    size_t LastVertices() const { return lastVertices_; }

private:
    struct Vertex { // the UXRH record (104 bytes): four poses' positions and normals, the texel
        float position[4][3], normal[4][3], u, v;
    };
    struct Mesh {
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
    };
    void Box(const float c[3], const float a[3], const float b[3], const float n[3], const float rgb[3]);
    void Marker(const GripMarker& m);
    void Pose(int hand, int gripStep, int triggerStep);
    bool loaded_ = false, failed_ = false;
    Mesh meshes_[2];
    GLuint handProgram_ = 0, handVao_[2] = {0, 0}, handVbo_[2] = {0, 0}, albedo_ = 0;
    GLint handViewProjLoc_ = -1, handModelLoc_ = -1, handLightLoc_ = -1, handTexLoc_ = -1;
    int gripStep_[2] = {-1, -1}, triggerStep_[2] = {-1, -1};
    size_t handCount_[2] = {0, 0};
    GLuint program_ = 0, vao_ = 0, vbo_ = 0; // the markers
    GLint viewProjLoc_ = -1, lightLoc_ = -1;
    std::vector<float> verts_;
    size_t lastVertices_ = 0;
};

} // namespace rrgame
