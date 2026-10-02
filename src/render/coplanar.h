#pragma once
// Coplanar layers: primitives of one soup that lie in (or within a few centimetres of) the
// plane of another primitive they overlap - a decal on a board, a face drawn twice with two mappings, a sign's plate over
// its post, two strips of ivy over a tunnel mouth. The console orders them by its table (a whole primitive over the
// other); a depth buffer compares the two depths pixel by pixel and the float rounding of each frame's transform decides
// - a pattern that shimmers as the eye moves, differently in each eye of a headset. With the depth buffer the winner of
// each pair is drawn once more after its run, pulled toward the eye along its own view rays (shaders.cpp uLayerPull: the
// picture keeps its place, only the depth moves) by a margin plus its LIFT - the misfit of the pair (how far the plate
// lies from the winner's plane over their shared area) divided by the cosine between the winner's normal and the ray,
// i.e. that misfit measured along the ray, whatever the angle it is seen at.
//
// The pair finder is shared by the static cell soup (race_scene_pc.cpp PcLayers) and the models (ModelLayers: the
// props, the traffic cars, the player's machine), each with its own tolerances in its own soup units.
#include "render/gl_api.h"
#include "rrformats/rmd3.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace rr::render {

// One primitive of a soup: `size` soup vertices (a multiple of 3) from `first`, the run (a caller's grouping) it
// belongs to, its area (filled by CoplanarMeasure) and its bounds.
struct CoplanarPrim {
    size_t run = 0;
    GLint first = 0;
    int size = 0;
    float area = 0.0f;
    float box[6] = {};
    int priority = 0; // the winner of a pair: the higher priority first (models: a one-sided face over a two-sided post)
};

struct CoplanarRule {
    float plane = 0.05f;      // soup units: the part of each triangle over the other lies within this of the other's plane
    float grid = 16.0f;       // soup units: the candidate grid's cell
    float largest = 512.0f;   // soup units: a primitive longer than this in any direction takes no pair (open ground)
    bool sameRunOnly = false; // pairs only inside one run (a posed model: a run is one rigid part)
    bool facing = false;      // two ONE-SIDED primitives facing opposite ways are no pair (the back-face test drops one)
    bool legacy = false;      // DEVELOPMENT (RRJB_LAYERS=1bx, the control): the older rule - parallel within
                              // ~3.6 degrees, every corner of each within `plane` of the other's plane, misfit 0
};

struct CoplanarPair {
    uint32_t a = 0, b = 0; // indices into the primitives, a < b
    float misfit = 0.0f;   // soup units: how far the part of each over the other lies from the other's plane, at most
};

// Twice the triangle's area, its (unnormalised) normal in `n`.
float CoplanarTriArea2(const rr::TriangleSoup::Vertex* v, float n[3]);
// The area and bounds of `p` over `soup`.
void CoplanarMeasure(const std::vector<rr::TriangleSoup::Vertex>& soup, CoplanarPrim& p);
// Every pair of `prims` with a triangle of each within ~26 degrees of parallel, the part of each over the other (inside
// the prism of the other's edges) of positive area and within `rule.plane` of the other's plane. (The stricter older
// rule, every CORNER within the plane of the other and ~3.6 degrees, misses two strips of one surface drawn at slightly
// different angles - the tunnel ceiling of race 1/25; this is its superset.)
std::vector<CoplanarPair> FindCoplanarPairs(const std::vector<rr::TriangleSoup::Vertex>& soup, const std::vector<CoplanarPrim>& prims,
                                            const CoplanarRule& rule);
// The unit normal of the primitive's first triangle.
void CoplanarNormal(const std::vector<rr::TriangleSoup::Vertex>& soup, const CoplanarPrim& p, float n[3]);
// Each primitive's layer (0: drawn once): the winner of a pair - the higher priority, then the smaller primitive (areas
// to 1 %), of two alike the first in the soup, as the tables mostly resolve it - one above the highest layer it covers,
// at most `cap`; with `lift`, each primitive's lift: the misfit to what it covers plus that one's own lift. Pairs `skip`
// answers true for are ignored.
std::vector<int> CoplanarLayerOf(const std::vector<CoplanarPrim>& prims, const std::vector<CoplanarPair>& pairs, int cap,
                                 const std::function<bool(uint32_t, uint32_t)>& skip = {}, std::vector<float>* lift = nullptr);

// Shader attribute 8 (shaders.cpp aLift): per vertex, a decal's lift vector - its plane's unit normal times its lift, in
// soup units (the shader turns it by uModel); 0 for every other vertex.
constexpr GLuint kLiftAttribute = 8;
// Uploads `lift` (3 floats a soup vertex) into `vbo` as attribute kLiftAttribute of `vao`.
void UploadLift(GLuint vao, GLuint& vbo, const std::vector<float>& lift);

// A model soup's layers: per draw range (first, count) of the soup, the winners' vertices again, from an element
// buffer bound to the soup's vertex array. Built once per soup. Drawn by RaceScene::DrawModelLayers after the range,
// only with the depth buffer.
class ModelLayers {
public:
    struct Draw {
        int layer = 0;
        GLsizei first = 0, count = 0; // indices into the element buffer
    };
    // `vao` the soup's vertex array (the element buffer is bound to it); `unitsPerWorld` soup units per world unit;
    // `posed` a soup whose parts the draw moves (the machine: pairs only inside one range - one rigid part - and no lift
    // vectors, which would turn with the rest pose; the pull's margin and the polygon offset alone).
    void Build(GLuint vao, const std::vector<rr::TriangleSoup::Vertex>& soup, const std::vector<std::pair<GLint, GLsizei>>& ranges,
               float unitsPerWorld, bool posed);
    const std::vector<Draw>& Of(size_t range) const;
    size_t Pairs() const { return pairs_; }
    size_t Winners() const { return winners_; }

private:
    GLuint ebo_ = 0, liftVbo_ = 0;
    std::vector<std::vector<Draw>> draws_;
    size_t pairs_ = 0, winners_ = 0;
};

} // namespace rr::render
