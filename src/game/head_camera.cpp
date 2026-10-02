#include "game/head_camera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "game/rider_pose.h"

namespace rr::game {
namespace {

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

void Normalise(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f)
        for (int k = 0; k < 3; ++k) v[k] /= l;
}
void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A point of the posed rider (model units, the rider object's frame = the bike's axes) through part `part`.
void PartPoint(const rr::PosedGroup& posed, size_t part, const float local[3], double out[3]) {
    for (int r = 0; r < 3; ++r) {
        double s = 0.0;
        for (int c = 0; c < 3; ++c) s += static_cast<double>(posed.world[part].m[r * 3 + c]) * local[c];
        out[r] = s / 4096.0 + posed.origin[part][static_cast<size_t>(r)];
    }
}

} // namespace

bool HeadCameraRig::Load(const rr::DiscImage& disc) {
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
    if (bike == nullptr || rider == nullptr || bike->groups.empty() || rider->groups.empty()) return false;
    rider_ = rider->groups.front();
    if (rider_.subMeshes.size() != 17) return false;
    skeleton_ = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    assembly_ = rr::AssembleGroup(rider_, skeleton_);
    if (!assembly_.assembled) return false;
    // The seat: SeatVertex 0x80066A84 at LOD 0 - sub-mesh 0's vertex 3 (fewer than 6 parts) or 4 (race_scene.cpp
    // LoadMachine's riderAttach_, the same rule).
    const rr::ModelGroup& bg = bike->groups.front();
    if (bg.subMeshes.empty()) return false;
    const size_t at = bg.subMeshes.front().vertBase + (bg.subMeshes.size() < 6 ? 3u : 4u);
    if (at >= bg.verts.size()) return false;
    const float bf = static_cast<float>(rr::LodFactor(bg));
    seat_[0] = static_cast<float>(bg.verts[at].x) * bf;
    seat_[1] = static_cast<float>(bg.verts[at].y) * bf;
    seat_[2] = static_cast<float>(bg.verts[at].z) * bf;
    // The eye in the head part's own frame, from its vertices' extent (x along the spine to the crown, -y the face).
    const rr::SubMesh& head = rider_.subMeshes[kHeadPart];
    if (head.vertCount == 0 || head.vertBase + head.vertCount > rider_.verts.size()) return false;
    const float f = static_cast<float>(rr::LodFactor(rider_));
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t i = 0; i < head.vertCount; ++i) {
        const rr::SVector& v = rider_.verts[head.vertBase + i];
        const float p[3] = {v.x * f, v.y * f, v.z * f};
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    const float cy = 0.5f * (lo[1] + hi[1]);
    eyeLocal_[0] = hi[0] - 0.4f * (hi[0] - lo[0]);
    eyeLocal_[1] = cy + 0.75f * (lo[1] - cy);
    eyeLocal_[2] = 0.5f * (lo[2] + hi[2]);
    Reset();
    loaded_ = true;
    return true;
}

void HeadCameraRig::Reset() {
    for (Player& p : players_) p = Player{};
}

bool HeadCameraRig::Compute(const uint8_t* ram, uint32_t bike, uint32_t rider, const float* seat, HeadPose& out) const {
    out = HeadPose{};
    if (!loaded_ || ram == nullptr || !InRam(bike) || !InRam(rider)) return false;
    RiderPoseView pose;
    if (!ReadRiderPose(ram, rider, pose)) return false;
    // The bike's frame: model axis k = row k of +0x1B0 (RecordModelMatrix in rrgame), origin +0xB8.
    float axis[3][3];
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t k = 0; k < 3; ++k) axis[c][k] = static_cast<float>(S16(ram, bike + 0x1B0u + 6u * c + 2u * k)) / 4096.0f;
        if (Dot(axis[c], axis[c]) < 0.25f) return false; // not a rotation (no frame written yet)
    }
    for (uint32_t k = 0; k < 3; ++k) out.bikeOrigin[k] = static_cast<float>(static_cast<double>(S32(ram, bike + 0xB8u + 4u * k)) / 65536.0);
    for (int k = 0; k < 3; ++k) {
        out.bikeFwd[k] = axis[2][k];
        out.bikeUp[k] = -axis[1][k];
    }
    Normalise(out.bikeFwd);
    Normalise(out.bikeUp);
    Cross(out.bikeFwd, out.bikeUp, out.bikeRight);
    Normalise(out.bikeRight);
    // The posed rider, as rider_pose_draw.cpp poses it for the renderer.
    const rr::PosedGroup posed =
        rr::PoseGroup(rider_, skeleton_, assembly_, std::span<const rr::PartMatrix>(pose.local, rider_.subMeshes.size()));
    const float* s = seat != nullptr ? seat : seat_;
    const double base[3] = {s[0] + pose.root[0], s[1] + pose.root[1], s[2] + pose.root[2]};
    // off the bike the rider is where the game has him (rider_pose.h RiderOwnFrame), not on the seat
    float ownAxis[3][3], ownOrigin[3];
    const bool own = RiderOwnFrame(ram, rider, ownAxis, ownOrigin);
    const auto toWorld = [&](const double m[3], float w[3]) { // bike model units -> world
        for (int k = 0; k < 3; ++k) {
            double v = own ? ownOrigin[k] : out.bikeOrigin[k];
            for (int c = 0; c < 3; ++c)
                v += own ? ownAxis[c][k] * m[c] / kModelUnitsPerWorldUnitHead
                         : axis[c][k] * (base[c] + m[c]) / kModelUnitsPerWorldUnitHead;
            w[k] = static_cast<float>(v);
        }
    };
    double eyeModel[3];
    PartPoint(posed, kHeadPart, eyeLocal_, eyeModel);
    toWorld(eyeModel, out.rawEye);
    // The head's centre: the mean of its posed vertices (what the renderer draws).
    const float f = static_cast<float>(rr::LodFactor(rider_));
    const auto centre = [&](size_t part, float w[3]) {
        const rr::SubMesh& sm = rider_.subMeshes[part];
        double sum[3] = {};
        for (uint32_t i = 0; i < sm.vertCount; ++i) {
            const rr::SVector& v = rider_.verts[sm.vertBase + i];
            const float lp[3] = {v.x * f, v.y * f, v.z * f};
            double p[3];
            PartPoint(posed, part, lp, p);
            for (int k = 0; k < 3; ++k) sum[k] += p[k];
        }
        for (double& v : sum) v /= static_cast<double>(std::max<uint32_t>(sm.vertCount, 1u));
        toWorld(sum, w);
    };
    centre(kHeadPart, out.head);
    centre(0, out.root);
    float rel[3];
    for (int k = 0; k < 3; ++k) rel[k] = out.rawEye[k] - out.bikeOrigin[k];
    out.eyeHeight = Dot(rel, out.bikeUp);
    for (int k = 0; k < 3; ++k) {
        out.eye[k] = out.rawEye[k];
        out.fwd[k] = out.bikeFwd[k];
        out.up[k] = out.bikeUp[k];
        out.right[k] = out.bikeRight[k];
    }
    out.seated = S32(ram, rider + 0x25Cu) < 2;
    out.valid = true;
    return true;
}

bool HeadCameraRig::PoseWorld(const uint8_t* ram, uint32_t rider, const float axis[3][3], const float origin[3],
                              std::vector<float>& world) const {
    world.clear();
    if (!loaded_ || ram == nullptr || !InRam(rider)) return false;
    RiderPoseView pose;
    if (!ReadRiderPose(ram, rider, pose)) return false;
    const rr::PosedGroup posed =
        rr::PoseGroup(rider_, skeleton_, assembly_, std::span<const rr::PartMatrix>(pose.local, rider_.subMeshes.size()));
    const float f = static_cast<float>(rr::LodFactor(rider_));
    world.assign(3 * rider_.verts.size(), 0.0f);
    for (size_t part = 0; part < rider_.subMeshes.size(); ++part) {
        const rr::SubMesh& sm = rider_.subMeshes[part];
        for (uint32_t i = 0; i < sm.vertCount && sm.vertBase + i < rider_.verts.size(); ++i) {
            const rr::SVector& v = rider_.verts[sm.vertBase + i];
            const float lp[3] = {v.x * f, v.y * f, v.z * f};
            double m[3];
            PartPoint(posed, part, lp, m);
            for (int k = 0; k < 3; ++k) {
                double w = origin[k];
                for (int c = 0; c < 3; ++c) w += axis[c][k] * m[c] / kModelUnitsPerWorldUnitHead;
                world[3 * (sm.vertBase + i) + static_cast<size_t>(k)] = static_cast<float>(w);
            }
        }
    }
    return true;
}

bool HeadCameraRig::Update(const uint8_t* ram, uint32_t bike, uint32_t rider, int player, const float* seat) {
    if (player < 0 || player > 1) return false;
    Player& p = players_[player];
    ++stats_.updates;
    HeadPose now;
    if (!Compute(ram, bike, rider, seat, now)) {
        ++stats_.invalid;
        p = Player{};
        return false;
    }
    if (now.seated) ++stats_.seated;
    // The eye in the bike's frame (right, up, fwd), smoothed there, so it follows the bike at any speed.
    float rel[3], off[3];
    for (int k = 0; k < 3; ++k) rel[k] = now.rawEye[k] - now.bikeOrigin[k];
    off[0] = Dot(rel, now.bikeRight);
    off[1] = Dot(rel, now.bikeUp);
    off[2] = Dot(rel, now.bikeFwd);
    if (std::getenv("RRJB_HEADCAM_TRACE") && stats_.updates % 50 == 1) // DEVELOPMENT: the eye in the bike's frame
        std::printf("headcam: eye in the bike frame right %.3f up %.3f fwd %.3f (world units), seated %d\n", off[0], off[1],
                    off[2], now.seated ? 1 : 0);
    const float a = std::clamp(smoothing, 0.0f, 1.0f);
    const bool restart = !p.have || !now.seated || !p.pose.seated;
    for (int k = 0; k < 3; ++k) p.offset[k] = restart ? off[k] : p.offset[k] + a * (off[k] - p.offset[k]);
    float fwd[3], up[3];
    for (int k = 0; k < 3; ++k) {
        fwd[k] = restart ? now.bikeFwd[k] : p.fwd[k] + a * (now.bikeFwd[k] - p.fwd[k]);
        up[k] = restart ? now.bikeUp[k] : p.up[k] + a * (now.bikeUp[k] - p.up[k]);
    }
    Normalise(fwd);
    // leanFactor < 1: part of the bike's roll taken out - up toward the world's up (-y) made orthogonal to fwd
    if (leanFactor < 1.0f) {
        float level[3] = {0.0f, -1.0f, 0.0f};
        const float lf = Dot(level, fwd);
        for (int k = 0; k < 3; ++k) level[k] -= lf * fwd[k];
        Normalise(level);
        const float w = std::clamp(leanFactor, 0.0f, 1.0f);
        for (int k = 0; k < 3; ++k) up[k] = level[k] + w * (up[k] - level[k]);
    }
    // up made orthogonal to fwd
    const float d = Dot(up, fwd);
    for (int k = 0; k < 3; ++k) up[k] -= d * fwd[k];
    Normalise(up);
    for (int k = 0; k < 3; ++k) {
        p.fwd[k] = fwd[k];
        p.up[k] = up[k];
    }
    float right[3];
    Cross(fwd, up, right);
    Normalise(right);
    // the look-down tilt about `right` (lookDownDegrees)
    const float t = lookDownDegrees * 3.14159265f / 180.0f, ct = std::cos(t), st = std::sin(t);
    for (int k = 0; k < 3; ++k) {
        now.fwd[k] = fwd[k] * ct - up[k] * st;
        now.up[k] = up[k] * ct + fwd[k] * st;
        now.right[k] = right[k];
        now.eye[k] = now.bikeOrigin[k] + p.offset[0] * now.bikeRight[k] + p.offset[1] * now.bikeUp[k] +
                     p.offset[2] * now.bikeFwd[k];
    }
    // DEVELOPMENT (a rendering check): RRJB_HEADCAM_PROBE=<d> puts the camera d world units ahead of the
    // eye, looking back at the rider's face - with RRJB_HEADCAM_HIDE the hidden parts are seen to be the head.
    static const char* probe = std::getenv("RRJB_HEADCAM_PROBE");
    if (probe != nullptr) {
        const float ahead = static_cast<float>(std::atof(probe));
        for (int k = 0; k < 3; ++k) {
            now.eye[k] += ahead * now.bikeFwd[k];
            now.fwd[k] = -now.bikeFwd[k];
            now.up[k] = now.bikeUp[k];
            now.right[k] = -now.bikeRight[k];
        }
    }
    p.havePrev = p.have && p.pose.valid;
    for (int k = 0; k < 3; ++k) p.prevHead[k] = p.pose.head[k];
    p.pose = now;
    p.have = true;
    return true;
}

bool HeadCameraRig::Get(int player, HeadPose& out) const {
    if (player < 0 || player > 1 || !players_[player].have) return false;
    out = players_[player].pose;
    return out.valid;
}

void HeadCameraRig::NoteCaptured(int player, uint32_t model, int lod, const double eye[3], const std::vector<float>& rel) {
    if (player < 0 || player > 1 || !players_[player].have || !players_[player].pose.seated || model != 150u || lod != 0)
        return;
    if (rel.size() != 3 * rider_.verts.size()) return;
    const rr::SubMesh& head = rider_.subMeshes[kHeadPart];
    double c[3] = {};
    for (uint32_t i = 0; i < head.vertCount; ++i)
        for (int k = 0; k < 3; ++k) c[k] += rel[3 * (head.vertBase + i) + static_cast<size_t>(k)];
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double w = eye[k] + c[k] / static_cast<double>(head.vertCount);
        const double e = w - players_[player].pose.head[k];
        d2 += e * e;
    }
    const double d = std::sqrt(d2);
    if (std::getenv("RRJB_HEADCAM_TRACE")) { // DEVELOPMENT: the difference in the bike's frame, and the root part's
        const HeadPose& hp = players_[player].pose;
        float e[3];
        for (int k = 0; k < 3; ++k)
            e[k] = static_cast<float>(eye[k] + c[k] / static_cast<double>(head.vertCount) - hp.head[k]);
        const rr::SubMesh& root = rider_.subMeshes[0];
        double r[3] = {};
        for (uint32_t i = 0; i < root.vertCount; ++i)
            for (int k = 0; k < 3; ++k) r[k] += rel[3 * (root.vertBase + i) + static_cast<size_t>(k)];
        float er[3];
        for (int k = 0; k < 3; ++k) er[k] = static_cast<float>(eye[k] + r[k] / root.vertCount - hp.root[k]);
        std::printf("headcam: head d %.4f (right %.4f up %.4f fwd %.4f)  root d (right %.4f up %.4f fwd %.4f)\n", d,
                    Dot(e, hp.bikeRight), Dot(e, hp.bikeUp), Dot(e, hp.bikeFwd), Dot(er, hp.bikeRight), Dot(er, hp.bikeUp),
                    Dot(er, hp.bikeFwd));
    }
    ++stats_.compared;
    stats_.sumDist += d;
    stats_.maxDist = std::max(stats_.maxDist, d);
    if (players_[player].havePrev) {
        double p2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double e = eye[k] + c[k] / static_cast<double>(head.vertCount) - players_[player].prevHead[k];
            p2 += e * e;
        }
        const double dp = std::sqrt(p2);
        ++stats_.comparedPrev;
        stats_.sumPrev += dp;
        stats_.maxPrev = std::max(stats_.maxPrev, dp);
    }
}

uint32_t HeadCameraRig::HiddenRiderParts() const {
    static const char* over = std::getenv("RRJB_HEADCAM_HIDE");
    return over != nullptr ? static_cast<uint32_t>(std::strtoul(over, nullptr, 16)) : (1u << kHeadPart);
}

uint32_t HeadCameraRig::HiddenRiderPartsVr() const {
    static const char* over = std::getenv("RRJB_HEADCAM_HIDE");
    if (over != nullptr) return static_cast<uint32_t>(std::strtoul(over, nullptr, 16));
    constexpr uint32_t kAllParts = (1u << 17) - 1u;                              // the 17-part rider (model 150)
    constexpr uint32_t kKept = (1u << 6) | (1u << 7) | (1u << 9) | (1u << 10); // the forearms and the gloves
    return kAllParts & ~kKept;
}

HeadCameraRig& ProductHeadCamera() {
    static HeadCameraRig rig;
    return rig;
}

bool HeadCamera(int player, float eye[3], float fwd[3], float up[3]) {
    HeadPose p;
    if (!ProductHeadCamera().Get(player, p) || !p.seated) return false;
    for (int k = 0; k < 3; ++k) {
        eye[k] = p.eye[k];
        fwd[k] = p.fwd[k];
        up[k] = p.up[k];
    }
    return true;
}

} // namespace rr::game
