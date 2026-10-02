#pragma once
// Drawing the rider in the pose the PORTED animation machine produced.
//
// The machine writes 17 part slots per rider (Pose 0x8005D63C / the blend 0x8005D36C, through
// QuatToMatrix SLUS 0x8001005C): slot k is part k's rotation relative to its parent in the attachment
// program, and slot 0 the root part's - measured against the captures to be the rider's orientation
// RELATIVE TO THE BIKE (`rrgame --posecheck`: captured rider slot 0 = bike slot 0 x ported slot 0).
// The renderer walks the program exactly as `rrformats/pose.h` PoseGroup does (world(child) =
// world(parent) x local(child), origin(child) = origin(parent) + world(parent) x vertex), rebuilds
// the rider's vertices with every vertex following the part that authored it (the original
// transforms each part's vertex block into one shared buffer, 0x800220A4, and primitives index it),
// and replaces the rider's range of the machine's vertex buffer before each machine is drawn.
//
// The rider OBJECT is placed as RASHCDG 0x80066B98 places a child: at the bike's attachment vertex
// plus the root word triple Pose wrote to owner +0x1C (SetRoot 0x80066A60). OURS: the bike's slot-2
// matrix that 0x80066B98 turns the root by is the identity here (the bike's parts are not posed).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "render/gl_api.h"
#include "render/mat4.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

namespace rr::render {

struct RiderPoseMesh;

// `firstVertex`: where the rider group's rest-pose triangles start inside the machine's buffer.
std::shared_ptr<RiderPoseMesh> MakeRiderPoseMesh(const rr::ModelGroup& group, const rr::SkeletonTable& skeleton,
                                                 size_t firstVertex);

// Rewrites the rider's vertices in `vbo` for the 17 part slots `local` (nullptr: the rest pose).
// Skips the upload when the buffer already holds that pose.
void UploadRiderPose(RiderPoseMesh& mesh, GLuint vbo, const rr::PartMatrix* local);

// The captured pose: the group's triangles with each model vertex at `modelPos` (3 floats per vertex of
// the group, its model space) - the PORTED model draw's own vertices of the object - in place of the posing.
// False (nothing written) when the count differs. The next UploadRiderPose writes again whatever it is given.
bool UploadCapturedPose(RiderPoseMesh& mesh, GLuint vbo, const std::vector<float>& modelPos,
                        const std::vector<uint32_t>* sxy = nullptr); // the GPU rule on these SXY

// The rider object's model matrix: the bike's axes, at the attachment vertex plus the root triple.
Mat4 PosedRiderMatrix(const Mat4& bike, const float attach[3], const int16_t root[3]);

// The re-seat (game/rider_pose.h RiderOwnFrame, MachineClimbSlots):
// `m`'s axes turned by the part slot `s` (m x s: model axis a = sum_r s[r][a] axis r), the origin kept - the bike's
// part 0 during the re-seat's climb.
Mat4 TurnedBySlot(const Mat4& m, const rr::PartMatrix& s);
// ChildPlace 0x80066B98 in full: the seated rider at the attachment vertex plus the root turned by the bike's slot 2,
// in `bike`'s axes (PosedRiderMatrix with slot 2 the identity).
Mat4 ChildRiderMatrix(const Mat4& bike, const float attach[3], const int16_t root[3], const rr::PartMatrix& slot2);
// A rider off his bike: model axis c = axis[c] (unit, world), the model origin at `origin` (world units), one model
// unit 1 / kModelUnitsPerWorldUnit world unit as the bike's.
Mat4 OwnRiderMatrix(const float axis[3][3], const float origin[3]);

} // namespace rr::render
