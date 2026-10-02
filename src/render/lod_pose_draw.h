#pragma once
// The bike's and the rider's lower levels of detail, posed from the object's part slots.
//
// A model object keeps ONE part-slot array (17 slots a rider, 5 a bike) whatever LOD it is drawn at:
// LodSelect SLUS 0x8001298C only re-points each slot's DPD3 word. The attachment program the draw walks
// (RASHCDG 0x80067064) is picked by the LOD's part count - RiderParts 0x8006745C: 17 / 12 / 4 parts ->
// programs 0 / 1 / 2; BikeParts 0x80066EC4: 5 -> 4, 6 -> 3, 2..4 -> 5, 1 -> none - and each of its links
// turns part k by the matrix of slot (word >> 23) & 0x1F, which for programs 1 and 2 is NOT k: the
// 12-part rider reads slots 0 1 2 3 5 6 8 9 11 12 14 15, the 4-part one 0 3 5 8 - exactly the slots
// LodChoice's part masks 0x80052390 keep animating at LODs 2 and 3 (0xDB6F, 0x129). So the posing here
// takes the whole slot array and follows the program's matrix index; rrformats PoseGroup (which wants
// one matrix per sub-mesh) is the LOD-0 special case.
#include <cstddef>
#include <memory>
#include <vector>

#include "render/gl_api.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

namespace rr::render {

struct LodPoseMesh;

enum class LodOwner { Bike, Rider };

// The program the original walks for a group of `parts` sub-meshes, or -1 (no walk: one part).
int LodProgram(LodOwner owner, size_t parts);

// `firstVertex`: where the group's rest-pose triangles start inside the machine's buffer.
std::shared_ptr<LodPoseMesh> MakeLodPoseMesh(const rr::ModelGroup& group, const rr::SkeletonTable& skeleton,
                                             LodOwner owner, size_t firstVertex);
// The group's triangles posed from `slots` (`count` of them; nullptr: the program's rest pose).
rr::TriangleSoup PosedLodSoup(const LodPoseMesh& mesh, const rr::PartMatrix* slots, size_t count);
// The captured pose: the group's triangles with each model vertex at `modelPos` (rider_pose_draw.h
// UploadCapturedPose). False when the count differs.
bool UploadCapturedLod(LodPoseMesh& mesh, GLuint vbo, const std::vector<float>& modelPos,
                       const std::vector<uint32_t>* sxy = nullptr); // the GPU rule on these SXY
// Rewrites the group's vertices in `vbo` for these slots. Skips the upload when unchanged.
void UploadLodPose(LodPoseMesh& mesh, GLuint vbo, const rr::PartMatrix* slots, size_t count);

} // namespace rr::render
