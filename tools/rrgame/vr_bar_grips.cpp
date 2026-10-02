// The handlebar grips from the bike model's own bar (vr_bar_grips.h). The posing is the renderer's (race_scene.cpp's
// bike parts, head_camera.cpp's rider), the bike's frame head_camera.cpp's.
#include "vr_bar_grips.h"

#include "game/bike_pose_product.h"
#include "game/rider_pose.h"
#include "vr_wheelie.h"


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace rrgame {

namespace {

constexpr float kModelUnitsPerWorldUnit = 1024.0f; // head_camera.h kModelUnitsPerWorldUnitHead
constexpr float kEndBand = 0.03f;                   // world units: the bar-end vertices' band at the lateral extreme
constexpr float kHalfHand = 0.045f;                 // world units (metres at scale 100 %): the fist's centre inboard

int16_t S16(const uint8_t* ram, uint32_t a) {
    int16_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
    return v;
}
int32_t S32(const uint8_t* ram, uint32_t a) {
    int32_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }
float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Normalise(float v[3]) {
    const float l = std::sqrt(Dot(v, v));
    if (l > 1e-6f)
        for (int k = 0; k < 3; ++k) v[k] /= l;
}

// A part-local vertex of a posed group, in the group's model units (PoseGroup's origin + world * v).
void PosedPoint(const rr::PosedGroup& posed, size_t part, const rr::SVector& v, float factor, double out[3]) {
    const float lp[3] = {v.x * factor, v.y * factor, v.z * factor};
    for (int r = 0; r < 3; ++r) {
        double s = 0.0;
        for (int c = 0; c < 3; ++c) s += static_cast<double>(posed.world[part].m[r * 3 + c]) * lp[c];
        out[r] = s / 4096.0 + posed.origin[part][static_cast<size_t>(r)];
    }
}

} // namespace

bool BarGrips::Load(const rr::DiscImage& disc) {
    loaded_ = false;
    const auto geo = disc.Find("DATA/BBLEVEL1.GEO");
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!geo || !overlay) return false;
    const std::vector<rr::Model> models = rr::ParseGeo(disc.ReadFile(*geo));
    const rr::Model* bike = nullptr;
    const rr::Model* rider = nullptr;
    for (const rr::Model& m : models) {
        if (m.id == 100) bike = &m;
        if (m.id == 150) rider = &m;
    }
    if (bike == nullptr || bike->groups.empty()) return false;
    skeleton_ = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    bike_ = bike->groups.front();
    if (bike_.subMeshes.size() != 5) return false; // the bike's five part slots (bike_pose_product.h)
    bikeAssembly_ = rr::AssembleGroup(bike_, skeleton_);
    if (!bikeAssembly_.assembled) return false;
    // SeatVertex 0x80066A84 at LOD 0 (head_camera.cpp Load, race_scene.cpp LoadMachine's riderAttach_ - the same rule)
    const size_t at = bike_.subMeshes.front().vertBase + (bike_.subMeshes.size() < 6 ? 3u : 4u);
    if (at >= bike_.verts.size()) return false;
    const float bf = static_cast<float>(rr::LodFactor(bike_));
    seat_[0] = static_cast<float>(bike_.verts[at].x) * bf;
    seat_[1] = static_cast<float>(bike_.verts[at].y) * bf;
    seat_[2] = static_cast<float>(bike_.verts[at].z) * bf;
    // The handlebar part: of the moving parts, the one reaching widest among those that reach above the seat.
    const bool probe = std::getenv("RRJB_BARS_PROBE") != nullptr; // DEVELOPMENT: the sub-meshes' extents
    int fork = -1;
    float widest = 0.0f;
    for (size_t i = 0; i < bike_.subMeshes.size(); ++i) {
        const rr::SubMesh& sm = bike_.subMeshes[i];
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (uint32_t k = 0; k < sm.vertCount && sm.vertBase + k < bike_.verts.size(); ++k) {
            const rr::SVector& v = bike_.verts[sm.vertBase + k];
            const float p[3] = {v.x * bf / kModelUnitsPerWorldUnit, -v.y * bf / kModelUnitsPerWorldUnit, v.z * bf / kModelUnitsPerWorldUnit};
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], p[c]);
                hi[c] = std::max(hi[c], p[c]);
            }
        }
        if (probe)
            std::printf("bars probe: bike 100 sub-mesh %zu: %u verts, right %+.3f..%+.3f up %+.3f..%+.3f fwd %+.3f..%+.3f "
                        "(its own frame)\n",
                        i, unsigned(sm.vertCount), double(lo[0]), double(hi[0]), double(lo[1]), double(hi[1]), double(lo[2]),
                        double(hi[2]));
        const float reach = std::max(-lo[0], hi[0]);
        if (i > 0 && sm.vertCount > 0 && hi[1] > -seat_[1] / kModelUnitsPerWorldUnit && reach > widest) {
            widest = reach;
            fork = static_cast<int>(i);
        }
    }
    if (fork != kForkPart) {
        std::printf("vr bars: the handlebar part of model 100 is sub-mesh %d, not %d - no grips\n", fork, kForkPart);
        return false;
    }
    // its bar ends: the vertices within kEndBand of the lateral extreme, each side
    const rr::SubMesh& fm = bike_.subMeshes[static_cast<size_t>(fork)];
    float minX = 1e9f, maxX = -1e9f;
    for (uint32_t k = 0; k < fm.vertCount; ++k) {
        const float x = bike_.verts[fm.vertBase + k].x * bf / kModelUnitsPerWorldUnit;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
    }
    for (auto& e : ends_) e.clear();
    for (uint32_t k = 0; k < fm.vertCount; ++k) {
        const float x = bike_.verts[fm.vertBase + k].x * bf / kModelUnitsPerWorldUnit;
        if (x <= minX + kEndBand) ends_[0].push_back(fm.vertBase + k);
        if (x >= maxX - kEndBand) ends_[1].push_back(fm.vertBase + k);
    }
    if (ends_[0].empty() || ends_[1].empty()) return false;
    const rr::PosedGroup rest = rr::PoseGroup(bike_, skeleton_, bikeAssembly_, {});
    if (!BarEnds(rest, rest_)) return false;
    restFork_ = rest.world[static_cast<size_t>(fork)];
    // the wheels' contact point: the rest model's lowest vertex along its up (the visual lean's axis, vr_visual_lean.h)
    {
        double lowest = 1e30;
        for (size_t part = 0; part < bike_.subMeshes.size(); ++part) {
            const rr::SubMesh& sm = bike_.subMeshes[part];
            for (uint32_t k = 0; k < sm.vertCount && sm.vertBase + k < bike_.verts.size(); ++k) {
                double p[3];
                PosedPoint(rest, part, bike_.verts[sm.vertBase + k], bf, p);
                lowest = std::min(lowest, -p[1] / kModelUnitsPerWorldUnit);
            }
        }
        contactUp_ = lowest < 1e29 ? static_cast<float>(std::min(lowest, 0.0)) : 0.0f;
    }
    // each wheel's contact (vr_horizon.h) - the lowest vertices (within 1 cm) of the wheel sub-meshes 2 and 3
    // at rest, their mean forward; the front one is the one further forward
    {
        haveWheels_ = false;
        float up[2] = {}, fwd[2] = {};
        bool ok = true;
        for (int w = 0; w < 2 && ok; ++w) {
            const size_t part = 2u + static_cast<size_t>(w);
            const rr::SubMesh& sm = bike_.subMeshes[part];
            double low = 1e30;
            for (uint32_t k = 0; k < sm.vertCount && sm.vertBase + k < bike_.verts.size(); ++k) {
                double p[3];
                PosedPoint(rest, part, bike_.verts[sm.vertBase + k], bf, p);
                low = std::min(low, -p[1] / kModelUnitsPerWorldUnit);
            }
            double sum = 0.0;
            int n = 0;
            for (uint32_t k = 0; k < sm.vertCount && sm.vertBase + k < bike_.verts.size(); ++k) {
                double p[3];
                PosedPoint(rest, part, bike_.verts[sm.vertBase + k], bf, p);
                if (-p[1] / kModelUnitsPerWorldUnit <= low + 0.01) sum += p[2] / kModelUnitsPerWorldUnit, ++n;
            }
            ok = n > 0;
            up[w] = static_cast<float>(low);
            fwd[w] = n > 0 ? static_cast<float>(sum / n) : 0.0f;
        }
        if (ok) {
            const int front = fwd[0] >= fwd[1] ? 0 : 1;
            wheelFront_[0] = up[front];
            wheelFront_[1] = fwd[front];
            wheelRear_[0] = up[1 - front];
            wheelRear_[1] = fwd[1 - front];
            haveWheels_ = wheelFront_[1] - wheelRear_[1] > 0.3f; // (a bike, not a degenerate model)
            std::printf("vr bars: the wheels touch the road at up %+.3f fwd %+.3f (front) and up %+.3f fwd %+.3f (rear), "
                        "world units in the bike's frame\n",
                        double(wheelFront_[0]), double(wheelFront_[1]), double(wheelRear_[0]), double(wheelRear_[1]));
        }
    }
    std::memcpy(posed_, rest_, sizeof(posed_));
    // the rider (the gloves, logged once against the bars)
    haveRider_ = false;
    if (rider != nullptr && !rider->groups.empty() && rider->groups.front().subMeshes.size() == 17) {
        rider_ = rider->groups.front();
        riderAssembly_ = rr::AssembleGroup(rider_, skeleton_);
        haveRider_ = riderAssembly_.assembled;
    }
    std::printf("vr bars: the handlebar of model 100 (sub-mesh %d, %zu + %zu bar-end vertices): grips at rest left %+.3f "
                "%+.3f %+.3f right %+.3f %+.3f %+.3f (the bike's frame right / up / fwd, world units; %.3f apart)\n",
                fork, ends_[0].size(), ends_[1].size(), double(rest_[0][0]), double(rest_[0][1]), double(rest_[0][2]),
                double(rest_[1][0]), double(rest_[1][1]), double(rest_[1][2]), double(rest_[1][0] - rest_[0][0]));
    Reset();
    loaded_ = true;
    return true;
}

// The two grips of a posed bike, in the bike's frame (right, up, fwd), world units: each side's bar-end mean, half a
// hand inboard along the bar.
bool BarGrips::BarEnds(const rr::PosedGroup& posed, float out[2][3]) const {
    const float bf = static_cast<float>(rr::LodFactor(bike_));
    float end[2][3];
    for (int s = 0; s < 2; ++s) {
        double sum[3] = {};
        for (uint32_t index : ends_[s]) {
            double p[3];
            PosedPoint(posed, static_cast<size_t>(kForkPart), bike_.verts[index], bf, p);
            for (int k = 0; k < 3; ++k) sum[k] += p[k];
        }
        const double n = static_cast<double>(ends_[s].size());
        // model units (x right, y down, z forward) -> the bike's frame in world units
        end[s][0] = static_cast<float>(sum[0] / n / kModelUnitsPerWorldUnit);
        end[s][1] = static_cast<float>(-sum[1] / n / kModelUnitsPerWorldUnit);
        end[s][2] = static_cast<float>(sum[2] / n / kModelUnitsPerWorldUnit);
    }
    float axis[3] = {end[1][0] - end[0][0], end[1][1] - end[0][1], end[1][2] - end[0][2]};
    if (Dot(axis, axis) < 0.01f) return false;
    Normalise(axis);
    for (int k = 0; k < 3; ++k) {
        out[0][k] = end[0][k] + axis[k] * kHalfHand;
        out[1][k] = end[1][k] - axis[k] * kHalfHand;
    }
    return true;
}

void BarGrips::Reset() {
    frame_ = BikeFrame{};
    haveGloves_ = false;
}

bool BarGrips::Update(const uint8_t* ram, uint32_t bike, uint32_t rider, const float* seat, float leanScale,
                      const float* drawnRoll) {
    frame_.valid = false;
    if (!loaded_ || ram == nullptr || !InRam(bike) || !InRam(rider)) return false;
    float axis[3][3]; // model axis c = row c of +0x1B0 (4096 = 1.0)
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t k = 0; k < 3; ++k) axis[c][k] = static_cast<float>(S16(ram, bike + 0x1B0u + 6u * c + 2u * k)) / 4096.0f;
        if (Dot(axis[c], axis[c]) < 0.25f) return false;
    }
    BikeFrame f;
    for (uint32_t k = 0; k < 3; ++k) f.origin[k] = static_cast<float>(static_cast<double>(S32(ram, bike + 0xB8u + 4u * k)) / 65536.0);
    for (int k = 0; k < 3; ++k) {
        f.fwd[k] = axis[2][k];
        f.up[k] = -axis[1][k];
    }
    Normalise(f.fwd);
    Normalise(f.up);
    f.right[0] = f.fwd[1] * f.up[2] - f.fwd[2] * f.up[1]; // right = fwd x up (head_camera.cpp)
    f.right[1] = f.fwd[2] * f.up[0] - f.fwd[0] * f.up[2];
    f.right[2] = f.fwd[0] * f.up[1] - f.fwd[1] * f.up[0];
    Normalise(f.right);
    f.speed = static_cast<float>(static_cast<double>(S32(ram, bike + 0x1E0u)) / 65536.0);
    f.headingDegrees = static_cast<float>(std::atan2(double(f.fwd[0]), double(f.fwd[2])) * 57.29577951308232);
    f.seated = S32(ram, rider + 0x25Cu) < 2;
    f.valid = true;
    frame_ = f;
    // the model matrix as race_render.cpp RecordModelMatrix builds it (the rows raw, not re-normalised)
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t k = 0; k < 3; ++k)
            model_.m[4 * c + k] = static_cast<float>(S16(ram, bike + 0x1B0u + 6u * c + 2u * k)) / 4096.0f / kModelUnitsPerWorldUnit;
        model_.m[4 * c + 3] = 0.0f;
    }
    for (uint32_t k = 0; k < 3; ++k) model_.m[12 + k] = static_cast<float>(static_cast<double>(S32(ram, bike + 0xB8u + 4u * k)) / 65536.0);
    model_.m[15] = 1.0f;
    // the drawn bike's visual lean - the model matrix and the frame turned about the contact line alike
    lean_ = drawnRoll != nullptr ? VisualLean::ToRoll(model_, leanScale * *drawnRoll, contactUp_)
                                 : VisualLean::From(model_, leanScale, contactUp_);
    AddWheeliePitch(lean_, model_, ram, bike, contactUp_); // the wheelie's drawn pitch after the lean
    if (lean_.on) {

        lean_.Matrix(model_);
        lean_.Point(f.origin, f.origin);
        lean_.Dir(f.right, f.right);
        lean_.Dir(f.up, f.up);
        lean_.Dir(f.fwd, f.fwd);
        frame_ = f;
    }
    // the bar as the game posed the fork this frame
    rr::PartMatrix parts[5];
    haveParts_ = rr::game::ReadBikeParts(ram, bike, parts);
    forkTurned_ = false;
    if (haveParts_) {
        std::memcpy(parts_, parts, sizeof(parts_));
        gameFork_ = parts_[kForkPart];
        const rr::PosedGroup posed = rr::PoseGroup(bike_, skeleton_, bikeAssembly_, std::span<const rr::PartMatrix>(parts, 5));
        if (!BarEnds(posed, posed_)) std::memcpy(posed_, rest_, sizeof(posed_));
    } else {
        std::memcpy(posed_, rest_, sizeof(posed_));
    }
    // once, seated: the rider's gloves against the bars (the log)
    if (!haveGloves_ && haveRider_ && f.seated) {
        rr::game::RiderPoseView pose;
        if (rr::game::ReadRiderPose(ram, rider, pose)) {
            const rr::PosedGroup posed = rr::PoseGroup(rider_, skeleton_, riderAssembly_,
                                                       std::span<const rr::PartMatrix>(pose.local, rider_.subMeshes.size()));
            const float* s = seat != nullptr ? seat : seat_;
            const double base[3] = {s[0] + pose.root[0], s[1] + pose.root[1], s[2] + pose.root[2]};
            const float lf = static_cast<float>(rr::LodFactor(rider_));
            int g = 0;
            for (int part : {kLeftGlove, kRightGlove}) {
                const rr::SubMesh& sm = rider_.subMeshes[static_cast<size_t>(part)];
                double sum[3] = {};
                for (uint32_t i = 0; i < sm.vertCount; ++i) {
                    double p[3];
                    PosedPoint(posed, static_cast<size_t>(part), rider_.verts[sm.vertBase + i], lf, p);
                    for (int k = 0; k < 3; ++k) sum[k] += p[k];
                }
                const double n = std::max<double>(sm.vertCount, 1.0);
                glove_[g][0] = static_cast<float>((base[0] + sum[0] / n) / kModelUnitsPerWorldUnit);
                glove_[g][1] = static_cast<float>(-(base[1] + sum[1] / n) / kModelUnitsPerWorldUnit);
                glove_[g][2] = static_cast<float>((base[2] + sum[2] / n) / kModelUnitsPerWorldUnit);
                ++g;
            }
            leftGlovePart_ = kLeftGlove;
            if (glove_[0][0] > glove_[1][0]) {
                std::swap(glove_[0], glove_[1]);
                leftGlovePart_ = kRightGlove;
            }
            std::printf("vr bars: the rider's left glove is part %d, the right part %d\n", leftGlovePart_,
                        leftGlovePart_ == kLeftGlove ? kRightGlove : kLeftGlove);
            haveGloves_ = true;
        }
    }
    return true;
}

bool BarGrips::Grips(float out[2][3]) const {
    if (!loaded_) return false;
    std::memcpy(out, rest_, sizeof(rest_));
    return true;
}

bool BarGrips::PosedGrips(float out[2][3]) const {
    if (!loaded_) return false;
    std::memcpy(out, posed_, sizeof(posed_));
    return true;
}

bool BarGrips::Gloves(float out[2][3]) const {
    if (!haveGloves_) return false;
    std::memcpy(out, glove_, sizeof(glove_));
    return true;
}

void BarGrips::SetForkTurn(bool on, float radians) {
    if (!loaded_ || !haveParts_) return;
    if (!on && !forkTurned_) return;
    if (on) {
        // RotMatrix(0, b, 0): the rotation about the part's own y; model x right, y down, z forward - the right end
        // (+x) goes to (cos, 0, -sin) of the slot's angle, so the right end forward is the angle -radians
        const double c = std::cos(-static_cast<double>(radians)), s = std::sin(-static_cast<double>(radians));
        const auto q = [](double v) { return static_cast<int16_t>(std::lround(v * 4096.0)); };
        rr::PartMatrix m;
        m.m[0] = q(c), m.m[1] = 0, m.m[2] = q(s);
        m.m[3] = 0, m.m[4] = 4096, m.m[5] = 0;
        m.m[6] = q(-s), m.m[7] = 0, m.m[8] = q(c);
        parts_[kForkPart] = m;
    } else {
        parts_[kForkPart] = gameFork_;
    }
    forkTurned_ = on;
    const rr::PosedGroup posed = rr::PoseGroup(bike_, skeleton_, bikeAssembly_, std::span<const rr::PartMatrix>(parts_, 5));
    if (!BarEnds(posed, posed_)) std::memcpy(posed_, rest_, sizeof(posed_));
}

bool BarGrips::ModelGrips(const rr::PartMatrix* parts, float pos[2][3], float rot[9]) const {
    if (!loaded_) return false;
    const rr::PosedGroup posed = parts != nullptr
                                     ? rr::PoseGroup(bike_, skeleton_, bikeAssembly_, std::span<const rr::PartMatrix>(parts, 5))
                                     : rr::PoseGroup(bike_, skeleton_, bikeAssembly_, {});
    if (posed.world.size() <= static_cast<size_t>(kForkPart)) return false;
    const float bf = static_cast<float>(rr::LodFactor(bike_));
    double end[2][3];
    for (int s = 0; s < 2; ++s) {
        double sum[3] = {};
        for (uint32_t index : ends_[s]) {
            double p[3];
            PosedPoint(posed, static_cast<size_t>(kForkPart), bike_.verts[index], bf, p);
            for (int k = 0; k < 3; ++k) sum[k] += p[k];
        }
        for (int k = 0; k < 3; ++k) end[s][k] = sum[k] / static_cast<double>(ends_[s].size());
    }
    double axis[3] = {end[1][0] - end[0][0], end[1][1] - end[0][1], end[1][2] - end[0][2]};
    const double len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (len < 0.1 * kModelUnitsPerWorldUnit) return false;
    for (double& a : axis) a /= len;
    const double inboard = static_cast<double>(kHalfHand) * kModelUnitsPerWorldUnit; // BarEnds' half hand, model units
    for (int k = 0; k < 3; ++k) {
        pos[0][k] = static_cast<float>(end[0][k] + axis[k] * inboard);
        pos[1][k] = static_cast<float>(end[1][k] - axis[k] * inboard);
    }
    // the fork's turn from its rest: posed * rest^T (both part-local -> model, orthonormal in Q12)
    const rr::PartMatrix& a = posed.world[static_cast<size_t>(kForkPart)];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) sum += static_cast<double>(a.m[r * 3 + k]) * restFork_.m[c * 3 + k];
            rot[r * 3 + c] = static_cast<float>(sum / (4096.0 * 4096.0));
        }
    return true;
}

void BarGrips::ToWorld(const float local[3], float world[3]) const {
    for (int k = 0; k < 3; ++k)
        world[k] = frame_.origin[k] + frame_.right[k] * local[0] + frame_.up[k] * local[1] + frame_.fwd[k] * local[2];
}

void BarGrips::ToBike(const float world[3], float local[3]) const {
    float rel[3];
    for (int k = 0; k < 3; ++k) rel[k] = world[k] - frame_.origin[k];
    local[0] = Dot(rel, frame_.right);
    local[1] = Dot(rel, frame_.up);
    local[2] = Dot(rel, frame_.fwd);
}

} // namespace rrgame
