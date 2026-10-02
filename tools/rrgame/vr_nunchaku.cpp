// VR nunchaku: the jointed weapons' chain physics in the tracked hand (vr_nunchaku.h).
#include "vr_nunchaku.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>

namespace rrgame {

namespace {

using V3 = std::array<float, 3>;
V3 Make3(const float* p) { return {p[0], p[1], p[2]}; }
V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Norm(const V3& a) {
    const float l = Len(a);
    return l > 1e-9f ? Mul(a, 1.0f / l) : V3{0, 0, 0};
}

// 3x3, row-major: r[row * 3 + col]
struct M3 {
    float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
M3 Mul(const M3& a, const M3& b) {
    M3 o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += a.r[i * 3 + k] * b.r[k * 3 + j];
            o.r[i * 3 + j] = s;
        }
    return o;
}
M3 T(const M3& a) {
    M3 o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o.r[i * 3 + j] = a.r[j * 3 + i];
    return o;
}
V3 Apply(const M3& a, const V3& v) {
    return {a.r[0] * v[0] + a.r[1] * v[1] + a.r[2] * v[2], a.r[3] * v[0] + a.r[4] * v[1] + a.r[5] * v[2],
            a.r[6] * v[0] + a.r[7] * v[1] + a.r[8] * v[2]};
}
M3 Rot(const rr::render::Mat4& m) { // the 3x3 of a column-major Mat4 (with its scale)
    M3 o;
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) o.r[k * 3 + j] = m.m[j * 4 + k];
    return o;
}

constexpr int kSubSteps = 4, kPasses = 4;
constexpr float kGravity = 9.81f;     // m/s^2
constexpr float kDamping = 1.5f;      // 1/s
constexpr float kMaxHandAcc = 600.0f; // m/s^2: beyond it the anchor jumped (a teleport, a tracking jump)

} // namespace

bool VrNunchaku::Load(const std::vector<rr::Model>& models, const rr::SkeletonTable& skeleton) {
    const rr::Model* w = nullptr;
    for (const rr::Model& m : models)
        if (m.id == 800) w = &m;
    bool any = false;
    std::string line = "vr nunchaku: model 800";
    for (int i = 0; i < 2 && w != nullptr; ++i) {
        const size_t gi = i == 0 ? 0 : 4;
        Model& md = models_[i];
        md = Model{};
        if (gi >= w->groups.size()) continue;
        const rr::ModelGroup& g = w->groups[gi];
        const rr::Assembly as = rr::AssembleGroup(g, skeleton);
        if (!rr::render::ChainShapeOf(g, skeleton, as, md.shape) || md.shape.parts != 4) continue;
        md.restSoup = rr::BuildPosedTriangleSoup(g, rr::PoseGroup(g, skeleton, as, std::span<const rr::PartMatrix>{}));
        md.ok = !md.restSoup.vertices.empty();
        any = any || md.ok;
        char b[200];
        std::snprintf(b, sizeof(b), "%s group %zu (%s): 4 parts, lengths %.0f %.0f %.0f %.0f model units, program %zu",
                      i ? ";" : "", gi, i == 0 ? "chain, weapon 0" : "nunchaku, weapon 4", double(md.shape.length[0]), double(md.shape.length[1]),
                      double(md.shape.length[2]), double(md.shape.length[3]), as.programIndex);
        line += b;
    }
    std::printf("%s%s\n", line.c_str(), any ? "" : " - NOT usable (no chain physics)");
    return any;
}

bool VrNunchaku::Jointed(int weapon) const {
    return (weapon == 0 && models_[0].ok) || (weapon == 4 && models_[1].ok);
}

void VrNunchaku::Observe(const rr::render::WeaponHandOverride& o) {
    if (!active_ || model_ == nullptr || !haveHandFrame_ || !o.active || o.hidden) return;
    // the weapon's model matrix as drawn, in this frame's hand frame: C = H^T M (rotation x scale), H^T (t - pos)
    const rr::render::Mat4 m = rr::render::WeaponHandMatrix(model_->restSoup, o);
    M3 H;
    std::copy(std::begin(handRot_), std::end(handRot_), H.r);
    const M3 C = Mul(T(H), Rot(m));
    std::copy(std::begin(C.r), std::end(C.r), relRot_);
    const V3 t = Apply(T(H), Sub(V3{m.m[12], m.m[13], m.m[14]}, Make3(handPos_)));
    for (int k = 0; k < 3; ++k) relPos_[k] = t[k];
    haveRel_ = true;
    relWeapon_ = weapon_;
    ++totals_.observed;
}

void VrNunchaku::Update(int weapon, bool valid, const float pos[3], const float right[3], const float up[3], const float fwd[3],
                        const float down[3], float dt, float upm) {
    haveHandFrame_ = false;
    if (!valid || !Jointed(weapon) || !(dt > 0.0f)) {
        active_ = false;
        haveHand_ = false;
        weapon_ = -1;
        return;
    }
    model_ = weapon == 0 ? &models_[0] : &models_[1];
    const Model& md = *model_;
    const rr::render::ChainShape& sh = md.shape;
    // the hand's frame: columns right, up, back (the grip's +Z)
    M3 H;
    for (int k = 0; k < 3; ++k) H.r[k * 3 + 0] = right[k], H.r[k * 3 + 1] = up[k], H.r[k * 3 + 2] = -fwd[k];
    std::copy(std::begin(H.r), std::end(H.r), handRot_);
    for (int k = 0; k < 3; ++k) handPos_[k] = pos[k];
    haveHandFrame_ = true;
    // the weapon's model matrix: the hand frame times the relation last drawn, or (first) the default placement
    M3 W;
    V3 origin;
    if (haveRel_ && relWeapon_ == weapon) {
        M3 C;
        std::copy(std::begin(relRot_), std::end(relRot_), C.r);
        W = Mul(H, C);
        origin = Add(Make3(pos), Apply(H, Make3(relPos_)));
    } else {
        rr::render::WeaponHandOverride o;
        for (int k = 0; k < 3; ++k) o.origin[k] = pos[k], o.along[k] = fwd[k], o.side[k] = right[k];
        o.scale = 1.0f / 1024.0f;
        const rr::render::Mat4 m = rr::render::WeaponHandMatrix(md.restSoup, o);
        W = Rot(m);
        origin = {m.m[12], m.m[13], m.m[14]};
    }
    float sc = 0.0f;
    for (int j = 0; j < 3; ++j) sc += Len(V3{W.r[j], W.r[3 + j], W.r[6 + j]}) / 3.0f;
    if (sc > 1e-12f) scale_ = sc;
    const V3 j0 = origin;
    const V3 j1 = Add(j0, Mul(Norm(Apply(W, Make3(sh.axis[0]))), sh.length[0] * scale_));
    const bool fresh = !active_ || weapon != weapon_;
    if (fresh) { // hanging straight on from the handle, at rest against the hand
        weapon_ = weapon;
        active_ = true;
        haveHand_ = false;
        const V3 dir = Norm(Sub(j1, j0));
        j_[0] = j0;
        j_[1] = j1;
        for (int k = 1; k < 4; ++k) j_[k + 1] = Add(j_[k], Mul(dir, sh.length[k] * scale_));
        for (int k = 0; k < 5; ++k) prev_[k] = Sub(j_[k], j_[1]);
        ++totals_.resets;
    }
    // the hand's (the fixed joint's) acceleration, clamped
    V3 acc{0, 0, 0};
    if (haveHand_) {
        const V3 vel = Mul(Sub(j1, hand_), 1.0f / dt);
        acc = Mul(Sub(vel, handVel_), 1.0f / dt);
        if (Len(acc) > kMaxHandAcc * upm) {
            acc = {0, 0, 0}; // a jump of the anchor: the chain keeps its shape against the hand
            ++totals_.clamps;
            handVel_ = {0, 0, 0};
        } else {
            handVel_ = vel;
        }
    }
    hand_ = j1;
    haveHand_ = true;
    // hand-relative Verlet: y = x - j1, y' = y + (y - y_prev)(1 - damping h) + (g - a) h^2
    const V3 g = Mul(Norm(Make3(down)), kGravity * upm);
    const float h = dt / static_cast<float>(kSubSteps);
    V3 y[5], yp[5];
    for (int k = 0; k < 5; ++k) y[k] = Sub(j_[k], j_[1]), yp[k] = prev_[k]; // last frame's, against its fixed joint
    const V3 tipBefore = y[4];
    for (int s = 0; s < kSubSteps; ++s) {
        for (int k = 2; k < 5; ++k) {
            const V3 v = Mul(Sub(y[k], yp[k]), 1.0f - kDamping * h);
            yp[k] = y[k];
            y[k] = Add(Add(y[k], v), Mul(Sub(g, acc), h * h));
        }
        y[1] = {0, 0, 0};
        for (int pass = 0; pass < kPasses; ++pass)
            for (int k = 1; k < 4; ++k) { // segment k: joints k -> k + 1 at part k's length
                const float L = sh.length[k] * scale_;
                const V3 d = Sub(y[k + 1], y[k]);
                const float l = Len(d);
                if (l < 1e-9f) continue;
                const V3 corr = Mul(d, (l - L) / l);
                if (k == 1) {
                    y[k + 1] = Sub(y[k + 1], corr);
                } else {
                    y[k] = Add(y[k], Mul(corr, 0.5f));
                    y[k + 1] = Sub(y[k + 1], Mul(corr, 0.5f));
                }
            }
    }
    totals_.maxTip = std::max(totals_.maxTip, Len(Sub(y[4], tipBefore)) / dt / upm);
    for (int k = 0; k < 5; ++k) prev_[k] = yp[k];
    j_[0] = j0;
    j_[1] = j1;
    for (int k = 2; k < 5; ++k) j_[k] = Add(j1, y[k]);
    totals_.maxDrop = std::max(totals_.maxDrop, Dot(Sub(j_[4], j1), Norm(Make3(down))) / upm);
    totals_.chainLength = (sh.length[1] + sh.length[2] + sh.length[3]) * scale_ / upm;
    for (int k = 1; k < 4; ++k) { // the widest bend between two parts (the log)
        const float c = Dot(Norm(Sub(j_[k], j_[k - 1])), Norm(Sub(j_[k + 1], j_[k])));
        totals_.maxAngle = std::max(totals_.maxAngle, std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f);
    }
    ++totals_.frames;
}

bool VrNunchaku::Collider(float root[3], float tip[3]) const {
    if (!active_) return false;
    for (int k = 0; k < 3; ++k) root[k] = j_[3][k], tip[k] = j_[4][k];
    return true;
}

bool VrNunchaku::Fill(rr::render::WeaponHandOverride& o) const {
    if (!active_ || model_ == nullptr) return false;
    o.chainCount = model_->shape.parts;
    for (int k = 0; k < 4; ++k)
        for (int c = 0; c < 3; ++c) o.chain[k][c] = j_[k + 1][c];
    return true;
}

std::vector<std::array<float, 3>> VrNunchaku::Joints() const {
    if (!active_) return {};
    return std::vector<std::array<float, 3>>(std::begin(j_), std::end(j_));
}

std::string VrNunchaku::Totals() const {
    char b[320];
    std::snprintf(b, sizeof(b),
                  "vr nunchaku: the chain simulated in %zu frame(s) (reset %zu, anchor jumps ignored %zu, the "
                  "drawn grip observed %zu); the swinging end's fastest %.2f m/s against the hand, the widest bend between "
                  "two parts %.0f deg, the tip hung at most %.2f m below the handle's end (the chain beyond it %.2f m)",
                  totals_.frames, totals_.resets, totals_.clamps, totals_.observed, double(totals_.maxTip),
                  double(totals_.maxAngle), double(totals_.maxDrop), double(totals_.chainLength));
    return b;
}

} // namespace rrgame
