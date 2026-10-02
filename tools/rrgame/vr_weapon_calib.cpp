// The weapon's grip in the tracked hand (vr_weapon_calib.h).
#include "vr_weapon_calib.h"

#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "vr_hands_draw.h" // the drawn fist's centre (the mirror check)
#include "vr_holsters.h"   // the weapons' labels
#include "vr_settings.h"   // the left hand's trim

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>

namespace rrgame {

namespace {

constexpr float kModelPerWorld = 1024.0f;
constexpr float kPalmBehindGrip = 0.015f; // metres: the closed fist's centre behind the grip pose along the aim
                                          // (vr_hands_draw.h: kFistCentre x 0.040, the grip pose at x 0.055)
constexpr float kPalmInset = 72.0f;       // model units (7 cm at the game's scale): the palm up the handle from its end
constexpr float kPi = 3.14159265f;

using V3 = std::array<float, 3>;
V3 Make(const float* p) { return {p[0], p[1], p[2]}; }
V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Norm(const V3& a) {
    const float l = Len(a);
    return l > 1e-6f ? Mul(a, 1.0f / l) : V3{0, 0, 0};
}

using M3 = std::array<std::array<float, 3>, 3>; // [row][column]
M3 MulM(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    return r;
}
M3 Transpose(const M3& a) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = a[j][i];
    return r;
}
V3 MulV(const M3& a, const V3& v) {
    return {a[0][0] * v[0] + a[0][1] * v[1] + a[0][2] * v[2], a[1][0] * v[0] + a[1][1] * v[1] + a[1][2] * v[2],
            a[2][0] * v[0] + a[2][1] * v[1] + a[2][2] * v[2]};
}
// R = Ry(yaw) Rx(pitch) Rz(roll) in the hand's frame (x right, y up, z back)
M3 Euler(float pitchDeg, float yawDeg, float rollDeg) {
    const float p = pitchDeg * kPi / 180.0f, y = yawDeg * kPi / 180.0f, r = rollDeg * kPi / 180.0f;
    const float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y), cr = std::cos(r), sr = std::sin(r);
    const M3 rx{{{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}}};
    const M3 ry{{{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}};
    const M3 rz{{{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}}};
    return MulM(ry, MulM(rx, rz));
}
void ToEuler(const M3& m, float& pitchDeg, float& yawDeg, float& rollDeg) {
    const float sp = std::clamp(-m[1][2], -1.0f, 1.0f);
    pitchDeg = std::asin(sp) * 180.0f / kPi;
    yawDeg = std::atan2(m[0][2], m[2][2]) * 180.0f / kPi;
    rollDeg = std::atan2(m[1][0], m[1][1]) * 180.0f / kPi;
}

struct Basis {
    V3 o{}, x{}, y{}, z{}; // the hand's frame: right, up, back (world), the grip pose's position
};
Basis HandBasis(const HandFrameW& h) {
    Basis b;
    b.o = Make(h.pos);
    V3 f = Norm(Make(h.aim));
    if (Len(f) < 0.5f) f = Norm(Make(h.fwd));
    V3 r = Make(h.right);
    r = Norm(Sub(r, Mul(f, Dot(r, f))));
    if (Len(r) < 0.5f) r = Norm(Cross(f, Make(h.up)));
    b.x = r;
    b.y = Cross(r, f);
    b.z = Mul(f, -1.0f);
    return b;
}
V3 ToWorldDir(const Basis& b, const V3& l) { return Add(Add(Mul(b.x, l[0]), Mul(b.y, l[1])), Mul(b.z, l[2])); }
V3 ToLocalDir(const Basis& b, const V3& w) { return {Dot(w, b.x), Dot(w, b.y), Dot(w, b.z)}; }

WeaponShape g_shapes[9];
bool g_loaded = false;
const WeaponShape g_none{};

V3 PalmLocal(const WeaponGrip& g) { // the palm point in the hand's frame, metres
    return {static_cast<float>(g.offMm[0]) / 1000.0f, static_cast<float>(g.offMm[1]) / 1000.0f,
            kPalmBehindGrip - static_cast<float>(g.offMm[2]) / 1000.0f};
}
M3 EulerOf(const WeaponGrip& g) {
    return Euler(static_cast<float>(g.rotDeg[0]), static_cast<float>(g.rotDeg[1]), static_cast<float>(g.rotDeg[2]));
}
V3 TrimLocal(const WeaponGrip& t) { // the trim's move in the hand's frame (right, up, back), metres
    return {static_cast<float>(t.offMm[0]) / 1000.0f, static_cast<float>(t.offMm[1]) / 1000.0f,
            -static_cast<float>(t.offMm[2]) / 1000.0f};
}
bool Mirrored(bool left) { return left && GripMirrorOn(); }

// the grip's rotation and palm point in THIS hand's frame - the calibration as is in the right hand, its
// mirror image (then the left hand's trim: moved, then turned about the palm) in the left.
void Effective(const WeaponGrip& g, bool left, M3& r, V3& p) {
    if (!Mirrored(left)) {
        r = EulerOf(g);
        p = PalmLocal(g);
        return;
    }
    const WeaponGrip m = MirrorGrip(g);
    const WeaponGrip& t = VrPrefs().weapons.leftTrim;
    r = MulM(EulerOf(t), EulerOf(m));
    p = Add(PalmLocal(m), TrimLocal(t));
}
// ... and back: the (right-hand) calibration that puts the weapon at (r, p) in this hand
WeaponGrip FromEffective(const M3& r0, const V3& p0, bool left) {
    M3 r = r0;
    V3 p = p0;
    if (Mirrored(left)) {
        const WeaponGrip& t = VrPrefs().weapons.leftTrim;
        r = MulM(Transpose(EulerOf(t)), r);
        p = Sub(p, TrimLocal(t));
    }
    WeaponGrip g;
    float pitch = 0, yaw = 0, roll = 0;
    ToEuler(r, pitch, yaw, roll);
    g.offMm[0] = std::clamp(static_cast<int>(std::lround(p[0] * 1000.0f)), -300, 300);
    g.offMm[1] = std::clamp(static_cast<int>(std::lround(p[1] * 1000.0f)), -300, 300);
    g.offMm[2] = std::clamp(static_cast<int>(std::lround((kPalmBehindGrip - p[2]) * 1000.0f)), -300, 300);
    g.rotDeg[0] = static_cast<int>(std::lround(pitch));
    g.rotDeg[1] = static_cast<int>(std::lround(yaw));
    g.rotDeg[2] = static_cast<int>(std::lround(roll));
    return Mirrored(left) ? MirrorGrip(g) : g;
}
// The model point that sits at the palm: the handle's cross-section centroid - reflected across the weapon's own
// axis-up plane in the mirrored hand, so its long axis lies where the mirror puts it
V3 ModelGrip(const WeaponShape& s, bool left) {
    const V3 grip = Make(s.grip);
    if (!Mirrored(left)) return grip;
    const V3 sd = Make(s.side);
    return Sub(grip, Mul(sd, 2.0f * Dot(grip, sd)));
}

// The linear part (model units -> world) and the translation of the weapon in the hand (and the model point at the palm).
bool InHand(int weapon, const HandFrameW& hand, float upm, const WeaponGrip& g, M3& lin, V3& trans, V3* modelGrip = nullptr) {
    const WeaponShape& s = WeaponShapeOf(weapon);
    if (!s.valid || !hand.valid) return false;
    const Basis b = HandBasis(hand);
    M3 ru{};
    V3 palmLocal{};
    Effective(g, hand.left, ru, palmLocal);
    const V3 a = Make(s.axis), u = Make(s.up), sd = Make(s.side);
    const M3 bt{{{sd[0], sd[1], sd[2]}, {u[0], u[1], u[2]}, {-a[0], -a[1], -a[2]}}}; // model -> the hand's default
    const M3 w{{{b.x[0], b.y[0], b.z[0]}, {b.x[1], b.y[1], b.z[1]}, {b.x[2], b.y[2], b.z[2]}}};
    lin = MulM(w, MulM(ru, bt));
    for (auto& row : lin)
        for (float& v : row) v /= kModelPerWorld;
    const V3 palm = Add(b.o, Mul(ToWorldDir(b, palmLocal), upm));
    const V3 mg = ModelGrip(s, hand.left);
    trans = Sub(palm, MulV(lin, mg));
    if (modelGrip != nullptr) *modelGrip = mg;
    return true;
}

rr::render::Mat4 ToMat4(const M3& lin, const V3& trans) {
    rr::render::Mat4 m;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) m.m[c * 4 + r] = lin[r][c];
        m.m[c * 4 + 3] = 0.0f;
    }
    for (int r = 0; r < 3; ++r) m.m[12 + r] = trans[r];
    m.m[15] = 1.0f;
    return m;
}

// ---- the calibration page
struct Calib {
    bool open = false;
    int weapon = 1, hand = 1;
    bool holding = false, otherDown = false;
    M3 r0{};
    V3 p0{};
    M3 q0{};
    V3 q0pos{};
    size_t holds = 0;
} g_calib;

} // namespace

bool LoadWeaponShapes(const rr::DiscImage& disc) {
    if (g_loaded) return true;
    try {
        const auto geo = disc.Find("DATA/BBLEVEL1.GEO");
        const auto overlay = disc.Find("RASHCDG.BIN");
        if (!geo || !overlay) throw std::runtime_error("DATA/BBLEVEL1.GEO or RASHCDG.BIN is missing");
        const std::vector<rr::Model> models = rr::ParseGeo(disc.ReadFile(*geo));
        const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
        const rr::Model* weapons = nullptr;
        for (const rr::Model& m : models)
            if (m.id == 800) weapons = &m;
        if (weapons == nullptr) throw std::runtime_error("model 800 (the weapons) is missing");
        for (size_t w = 0; w < weapons->groups.size() && w < 9; ++w) {
            const rr::ModelGroup& grp = weapons->groups[w];
            const rr::Assembly as = rr::AssembleGroup(grp, skeleton);
            std::vector<rr::PartMatrix> rest(grp.subMeshes.size());
            for (auto& p : rest)
                for (int e = 0; e < 9; ++e) p.m[e] = static_cast<int16_t>(e % 4 == 0 ? 4096 : 0);
            const rr::PosedGroup posed = rr::PoseGroup(grp, skeleton, as, std::span<const rr::PartMatrix>(rest));
            const rr::TriangleSoup soup = rr::BuildPosedTriangleSoup(grp, posed);
            if (soup.vertices.empty()) continue;
            WeaponShape& s = g_shapes[w];
            // the long axis: the origin (the attach point, the handle's end) to the farthest vertex (weapon_draw.cpp
            // HandModel's axis)
            V3 farV{-1, 0, 0};
            float far2 = 0.0f;
            for (const auto& v : soup.vertices) {
                const float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
                if (d2 > far2) far2 = d2, farV = {v.x, v.y, v.z};
            }
            const V3 a = Norm(farV);
            V3 up = Sub(V3{0, -1, 0}, Mul(a, -a[1])); // the model's up (-y) square to the axis
            if (Len(up) < 0.3f) up = Sub(V3{0, 0, 1}, Mul(a, a[2]));
            up = Norm(up);
            const V3 side = Cross(a, up);
            s.tMin = 1e9f, s.tMax = -1e9f;
            for (const auto& v : soup.vertices) {
                const float t = Dot(V3{v.x, v.y, v.z}, a);
                s.tMin = std::min(s.tMin, t);
                s.tMax = std::max(s.tMax, t);
            }
            const float length = s.tMax - s.tMin;
            s.gripT = s.tMin + std::min(kPalmInset, 0.3f * length);
            // the handle's cross-section centroid (off the axis for the prod's grip box)
            const float region = s.tMin + std::max(2.0f * kPalmInset, 0.35f * length);
            V3 perp{0, 0, 0};
            int n = 0;
            for (const auto& v : soup.vertices) {
                const V3 p{v.x, v.y, v.z};
                const float t = Dot(p, a);
                if (t > region) continue;
                perp = Add(perp, Sub(p, Mul(a, t)));
                ++n;
            }
            if (n > 0) perp = Mul(perp, 1.0f / static_cast<float>(n));
            const V3 grip = Add(Mul(a, s.gripT), perp);
            for (int k = 0; k < 3; ++k) s.axis[k] = a[k], s.up[k] = up[k], s.side[k] = side[k], s.grip[k] = grip[k];
            s.valid = true;
        }
        g_loaded = true;
    } catch (const std::exception& e) {
        std::printf("vr weapons: the weapon shapes NOT loaded - %s\n", e.what());
    }
    return g_loaded;
}

const WeaponShape& WeaponShapeOf(int weapon) { return weapon >= 0 && weapon < 9 ? g_shapes[weapon] : g_none; }

std::string DescribeWeaponShapes() {
    if (!g_loaded) return "vr weapons: no weapon shapes";
    std::string out = "vr weapons: the default grips from model 800 (weapon: length cm, palm cm up the handle, off the axis cm):";
    char b[96];
    for (int w = 0; w < 9; ++w) {
        const WeaponShape& s = g_shapes[w];
        if (!s.valid) continue;
        const float perp = std::sqrt(std::max(0.0f, s.grip[0] * s.grip[0] + s.grip[1] * s.grip[1] + s.grip[2] * s.grip[2] -
                                                        s.gripT * s.gripT));
        std::snprintf(b, sizeof(b), " %d: %.0f, %.1f, %.1f;", w, double((s.tMax - s.tMin) / kModelPerWorld * 100.0f),
                      double((s.gripT - s.tMin) / kModelPerWorld * 100.0f), double(perp / kModelPerWorld * 100.0f));
        out += b;
    }
    return out;
}

bool WeaponInHandMatrix(int weapon, const HandFrameW& hand, float upm, const WeaponGrip& g, rr::render::Mat4& out) {
    M3 lin{};
    V3 trans{};
    if (!InHand(weapon, hand, upm, g, lin, trans)) return false;
    out = ToMat4(lin, trans);
    return true;
}

bool WeaponInHandSegment(int weapon, const HandFrameW& hand, float upm, const WeaponGrip& g, float lengthPct, float root[3],
                         float tip[3]) {
    M3 lin{};
    V3 trans{}, grip{};
    if (!InHand(weapon, hand, upm, g, lin, trans, &grip)) return false;
    const WeaponShape& s = WeaponShapeOf(weapon);
    const V3 a = Make(s.axis);
    const V3 r = Add(MulV(lin, Add(grip, Mul(a, s.tMin - s.gripT))), trans);
    const V3 t = Add(MulV(lin, Add(grip, Mul(a, s.tMax - s.gripT))), trans);
    const V3 tt = Add(r, Mul(Sub(t, r), lengthPct / 100.0f));
    for (int k = 0; k < 3; ++k) root[k] = r[k], tip[k] = tt[k];
    return true;
}

bool WeaponInHandFrame(int weapon, const HandFrameW& hand, float upm, const WeaponGrip& g, float origin[3], float along[3],
                       float side[3]) {
    M3 lin{};
    V3 trans{};
    if (!InHand(weapon, hand, upm, g, lin, trans)) return false;
    const WeaponShape& s = WeaponShapeOf(weapon);
    const V3 a = Make(s.axis);
    V3 p = std::fabs(a[0]) > 0.9f ? V3{0, 0, 1} : V3{1, 0, 0}; // weapon_draw.cpp HandModel's second axis
    p = Norm(Sub(p, Mul(a, Dot(p, a))));
    const V3 wa = Norm(MulV(lin, a)), wp = Norm(MulV(lin, p));
    for (int k = 0; k < 3; ++k) origin[k] = trans[k], along[k] = wa[k], side[k] = wp[k];
    return true;
}

bool WeaponPalmPoint(const HandFrameW& hand, float upm, const WeaponGrip& g, float out[3]) {
    if (!hand.valid) return false;
    const Basis b = HandBasis(hand);
    M3 r{};
    V3 palmLocal{};
    Effective(g, hand.left, r, palmLocal);
    const V3 p = Add(b.o, Mul(ToWorldDir(b, palmLocal), upm));
    for (int k = 0; k < 3; ++k) out[k] = p[k];
    return true;
}

bool WeaponHolsteredMatrix(int weapon, const float at[3], const float down[3], const float back[3], const float out[3],
                           rr::render::Mat4& result) {
    const WeaponShape& s = WeaponShapeOf(weapon);
    if (!s.valid) return false;
    // hanging: the handle up at the holster, the weapon down, a little back and out
    const V3 d = Norm(Add(Add(Make(down), Mul(Make(back), 0.35f)), Mul(Make(out), 0.18f)));
    V3 fwd = Mul(Make(back), -1.0f);
    V3 wu = Norm(Sub(fwd, Mul(d, Dot(fwd, d)))); // the model's up faces forward
    if (Len(wu) < 0.5f) wu = Norm(Cross(d, Make(out)));
    const V3 ws = Cross(d, wu);
    const V3 a = Make(s.axis), u = Make(s.up), sd = Make(s.side);
    M3 lin{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) lin[i][j] = (ws[i] * sd[j] + wu[i] * u[j] + d[i] * a[j]) / kModelPerWorld;
    const V3 trans = Sub(Make(at), MulV(lin, Make(s.grip)));
    result = ToMat4(lin, trans);
    return true;
}

// ------------------------------------------------------------------------------------------------ the page
void SetCalibPageOpen(bool open, int weaponInHand, int holdingHand) {
    if (open && !g_calib.open) {
        if (weaponInHand >= 0 && weaponInHand <= 8) g_calib.weapon = weaponInHand;
        if (holdingHand == 0 || holdingHand == 1) g_calib.hand = holdingHand;
        std::printf("vr weapons: the grip calibration page opened - weapon %d in the %s hand\n", g_calib.weapon,
                    g_calib.hand ? "right" : "left");
    }
    if (!open) g_calib.holding = false;
    g_calib.open = open;
}
bool CalibPageOpen() { return g_calib.open; }
int CalibWeapon() { return g_calib.weapon; }
int CalibHand() { return g_calib.hand; }
bool CalibHolding() { return g_calib.holding; }

bool CalibOtherHand(const HandFrameW hands[2], const float grip[2], float upm, WeaponsSettings& s) {
    Calib& c = g_calib;
    const int other = 1 - c.hand;
    const HandFrameW& hh = hands[c.hand];
    const HandFrameW& ho = hands[other];
    const float g = grip[other];
    const bool fresh = g >= 0.65f && !c.otherDown;
    if (g <= 0.30f) c.otherDown = false;
    else if (g >= 0.65f) c.otherDown = true;
    if (!c.open || !hh.valid || !ho.valid) {
        const bool was = c.holding;
        c.holding = false;
        return was;
    }
    const Basis bh = HandBasis(hh), bo = HandBasis(ho);
    // the other hand in the holding hand's frame: its axes as columns, its position (metres)
    M3 q{};
    const V3 cols[3] = {ToLocalDir(bh, bo.x), ToLocalDir(bh, bo.y), ToLocalDir(bh, bo.z)};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) q[i][j] = cols[j][i];
    const V3 qp = Mul(ToLocalDir(bh, Sub(bo.o, bh.o)), 1.0f / upm);
    WeaponGrip& wg = s.grip[std::clamp(c.weapon, 0, 8)];
    const bool left = c.hand == 0; // the left hand moves the mirrored grip (the calibration stays the right's)
    if (!c.holding && fresh) {
        c.holding = true;
        ++c.holds;
        Effective(wg, left, c.r0, c.p0);
        c.q0 = q;
        c.q0pos = qp;
        std::printf("vr weapons: the %s hand takes hold of weapon %d to move its grip\n", other ? "right" : "left", c.weapon);
    }
    if (c.holding && g <= 0.30f) {
        c.holding = false;
        std::printf("vr weapons: weapon %d's grip set to %+d %+d %+d mm, %+d %+d %+d deg (moved with the other hand)\n",
                    c.weapon, wg.offMm[0], wg.offMm[1], wg.offMm[2], wg.rotDeg[0], wg.rotDeg[1], wg.rotDeg[2]);
        return true;
    }
    if (!c.holding) return false;
    const M3 dr = MulM(q, Transpose(c.q0));
    const V3 dt = Sub(qp, MulV(dr, c.q0pos));
    const M3 r = MulM(dr, c.r0);
    const V3 p = Add(MulV(dr, c.p0), dt);
    wg = FromEffective(r, p, left);
    return false;
}

std::vector<WeaponMenuRow> CalibMenuRows(const WeaponsSettings& s) {
    std::vector<WeaponMenuRow> r;
    // the left hand's rows show the calibration as the left hand holds it (mirrored)
    const WeaponGrip& stored = s.grip[std::clamp(g_calib.weapon, 0, 8)];
    const WeaponGrip g = Mirrored(g_calib.hand == 0) ? MirrorGrip(stored) : stored;
    char b[64];
    r.push_back({"Weapon", HolsterWeaponLabel(g_calib.weapon)});
    r.push_back({"Hand", g_calib.hand ? "Right" : GripMirrorOn() ? "Left (the right's mirror)" : "Left"});
    static const char* const kOff[3] = {"Move right", "Move up", "Move forward"};
    static const char* const kRot[3] = {"Pitch (tip up)", "Yaw (tip left)", "Roll"};
    for (int k = 0; k < 3; ++k) {
        std::snprintf(b, sizeof(b), "%+d mm", g.offMm[k]);
        r.push_back({kOff[k], b});
    }
    for (int k = 0; k < 3; ++k) {
        std::snprintf(b, sizeof(b), "%+d deg", g.rotDeg[k]);
        r.push_back({kRot[k], b});
    }
    r.push_back({"Move it with the other hand", g_calib.holding ? "Holding - release to set" : "Squeeze the other grip"});
    r.push_back({"Reset this weapon", g.IsDefault() ? "(the model's default)" : ""});
    // the left hand's own fine adjustment, every weapon (rows kCalibTrimFirst..)
    static const char* const kTrim[6] = {"Left hand trim: right", "Left hand trim: up", "Left hand trim: forward",
                                         "Left hand trim: pitch", "Left hand trim: yaw", "Left hand trim: roll"};
    for (int k = 0; k < 6; ++k) {
        std::snprintf(b, sizeof(b), k < 3 ? "%+d mm" : "%+d deg", k < 3 ? s.leftTrim.offMm[k] : s.leftTrim.rotDeg[k - 3]);
        r.push_back({kTrim[k], b});
    }
    r.push_back({"Reset the left hand trim", s.leftTrim.IsDefault() ? "(none)" : ""});
    return r;
}

bool CalibMenuActivate(WeaponsSettings& s, int row, int direction, std::string& note) {
    const int d = direction == 0 ? 1 : direction;
    WeaponGrip& stored = s.grip[std::clamp(g_calib.weapon, 0, 8)];
    const bool mirror = Mirrored(g_calib.hand == 0); // a left-hand edit is an edit of the mirror image
    WeaponGrip g = mirror ? MirrorGrip(stored) : stored;
    const auto turn = [&](int& v, int step) {
        v += step;
        if (v > 180) v -= 360;
        if (v < -180) v += 360;
    };
    constexpr int kTrimFirst = 10;
    switch (row) {
    case 0: g_calib.weapon = (g_calib.weapon + d + 9) % 9; return false;
    case 1: g_calib.hand = 1 - g_calib.hand; return false;
    case 2: case 3: case 4: g.offMm[row - 2] = std::clamp(g.offMm[row - 2] + 5 * d, -300, 300); break;
    case 5: case 6: case 7: turn(g.rotDeg[row - 5], 5 * d); break;
    case 9:
        if (direction != 0) return false;
        g = WeaponGrip{};
        note = "The grip is back to the model's default";
        break;
    case kTrimFirst: case kTrimFirst + 1: case kTrimFirst + 2: // the left hand's trim: 2 mm / 2 deg steps
        s.leftTrim.offMm[row - kTrimFirst] = std::clamp(s.leftTrim.offMm[row - kTrimFirst] + 2 * d, -300, 300);
        return true;
    case kTrimFirst + 3: case kTrimFirst + 4: case kTrimFirst + 5:
        turn(s.leftTrim.rotDeg[row - kTrimFirst - 3], 2 * d);
        return true;
    case kTrimFirst + 6:
        if (direction != 0) return false;
        s.leftTrim = WeaponGrip{};
        note = "The left hand holds the right hand's exact mirror image";
        return true;
    default: return false;
    }
    stored = mirror ? MirrorGrip(g) : g;
    return true;
}

// ------------------------------------------------------------------------------------------------ the mirrored grip
bool GripMirrorOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_GRIP_MIRROR");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

WeaponGrip MirrorGrip(const WeaponGrip& g) {
    WeaponGrip m = g;
    m.offMm[0] = -g.offMm[0];
    m.rotDeg[1] = -g.rotDeg[1];
    m.rotDeg[2] = -g.rotDeg[2];
    return m;
}

std::string GripMirrorCheck(int weapon, const HandFrameW hands[2], float upm, const float planeOrigin[3],
                            const float planeNormal[3]) {
    const WeaponShape& s = WeaponShapeOf(weapon);
    if (!s.valid || !hands[0].valid || !hands[1].valid) return "vr weapons: grip check - no weapon shape / hands";
    const WeaponGrip& g = VrPrefs().weapons.grip[std::clamp(weapon, 0, 8)];
    struct Held {
        V3 rel{}, axis{}, root{}, tip{}, pos{}, aim{};
        float up = 0.0f, leftDeg = 0.0f;
    } h[2];
    const V3 a = Make(s.axis);
    for (int k = 0; k < 2; ++k) {
        HandFrameW hf = hands[k];
        hf.left = k == 0;
        M3 lin{};
        V3 trans{}, mg{};
        if (!InHand(weapon, hf, upm, g, lin, trans, &mg)) return "vr weapons: grip check - not placed";
        const Basis b = HandBasis(hf);
        GloveDraw gd; // the hand as vr_hands_draw.cpp draws it with this pose: its closed fist's centre
        gd.right = k == 1;
        for (int i = 0; i < 3; ++i) {
            gd.origin[i] = hf.pos[i], gd.gripRight[i] = hf.right[i], gd.gripUp[i] = hf.up[i];
            gd.gripForward[i] = hf.fwd[i], gd.aimForward[i] = hf.aim[i];
        }
        gd.scale = upm;
        float fist[3];
        VrHandsDraw::FistCentre(gd, fist);
        const V3 station = Add(MulV(lin, Mul(a, s.gripT)), trans); // the weapon's axis at the palm's station
        const V3 loc = Mul(ToLocalDir(b, Sub(station, Make(fist))), 1000.0f / upm);
        h[k].rel = {loc[0], loc[1], -loc[2]}; // mm: the hand's right, up, forward
        h[k].axis = ToLocalDir(b, Norm(MulV(lin, a)));
        h[k].up = std::asin(std::clamp(h[k].axis[1], -1.0f, 1.0f)) * 180.0f / kPi;
        h[k].leftDeg = std::atan2(-h[k].axis[0], -h[k].axis[2]) * 180.0f / kPi;
        h[k].root = Add(MulV(lin, Mul(a, s.tMin)), trans); // the drawn model's own axis (not the collider's line
        h[k].tip = Add(MulV(lin, Mul(a, s.tMax)), trans);  // through the handle's centroid)
        h[k].pos = Make(hf.pos);
        h[k].aim = Norm(Make(hf.aim));
    }
    // the mirror difference in the hands' own frames: the left's right-component and yaw negated
    const V3 lm{-h[0].rel[0], h[0].rel[1], h[0].rel[2]};
    const float dMm = Len(Sub(lm, h[1].rel));
    const V3 am{-h[0].axis[0], h[0].axis[1], h[0].axis[2]};
    const float dDeg = std::acos(std::clamp(Dot(am, h[1].axis), -1.0f, 1.0f)) * 180.0f / kPi;
    // the world: the right hand and its weapon reflected across the plane, against the left's
    const V3 n = Norm(Make(planeNormal)), o = Make(planeOrigin);
    const auto point = [&](const V3& p) { return Sub(p, Mul(n, 2.0f * Dot(Sub(p, o), n))); };
    const auto dir = [&](const V3& d) { return Sub(d, Mul(n, 2.0f * Dot(d, n))); };
    const float handMm = Len(Sub(point(h[1].pos), h[0].pos)) / upm * 1000.0f;
    const float handDeg = std::acos(std::clamp(Dot(dir(h[1].aim), h[0].aim), -1.0f, 1.0f)) * 180.0f / kPi;
    const float rootMm = Len(Sub(point(h[1].root), h[0].root)) / upm * 1000.0f;
    const float tipMm = Len(Sub(point(h[1].tip), h[0].tip)) / upm * 1000.0f;
    char b[900];
    std::snprintf(b, sizeof(b),
                  "vr weapons: grip check%s - weapon %d, calibration %+d %+d %+d mm %+d %+d %+d deg: in the "
                  "right hand its axis at the palm %+.1f %+.1f %+.1f mm from the drawn fist's centre (the hand's right / up "
                  "/ forward), %+.2f deg up and %+.2f deg left of the aim ray; in the left hand %+.1f %+.1f %+.1f mm, %+.2f "
                  "deg up, %+.2f deg left; mirror difference (the left's right and yaw negated) %.2f mm, %.3f deg; the "
                  "hands mirror images across the seat's mid-plane within %.2f mm / %.3f deg, the weapons' ends (handle, "
                  "tip) within %.2f / %.2f mm",
                  GripMirrorOn() ? "" : " (RRJB_GRIP_MIRROR=off)", weapon, g.offMm[0], g.offMm[1], g.offMm[2], g.rotDeg[0],
                  g.rotDeg[1], g.rotDeg[2], double(h[1].rel[0]), double(h[1].rel[1]), double(h[1].rel[2]),
                  double(h[1].up), double(h[1].leftDeg), double(h[0].rel[0]), double(h[0].rel[1]), double(h[0].rel[2]),
                  double(h[0].up), double(h[0].leftDeg), double(dMm), double(dDeg), double(handMm), double(handDeg),
                  double(rootMm), double(tipMm));
    return b;
}

} // namespace rrgame
