// Riding with the hands on the handlebars (vr_handlebars.h).
#include "vr_handlebars.h"

#include "game/handling_modern.h" // the Modern handling's drawn lean
#include "game/weapon_session.h"
#include "game_host_vr.h"
#include "render/weapon_draw.h"
#include "vr_bars_settings.h"
#include "vr_comfort.h"
#include "vr_settings.h"
#include "vr_holsters.h" // HolsterHandBusy

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rrgame {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kGrabOn = 0.65f, kGrabOff = 0.30f; // the grip button's hysteresis (GT2 / GTA SA VR)
constexpr float kGrabRadius = 0.20f;               // metres from the grip
// a fist already clenched takes its grip when it ENTERS the reach slowly (a hand brought to the bar, not a
// swing through it); within a second of holding the bar the reach is a little wider (the re-grab after a punch)
constexpr float kClenchedEntrySpeed = 1.0f;        // m/s in the seat's space
constexpr float kRegrabRadius = 0.28f;             // metres, for kRegrabSeconds after the hand let go
constexpr float kRegrabSeconds = 1.0f;
constexpr float kMarkerRadius = 0.45f;             // the grip's marker shows within this
constexpr float kJump = 45.0f * kPi / 180.0f;      // a bar angle jump larger than this re-references (GTA SA VR)
constexpr float kTwistDead = 1.5f * kPi / 180.0f, kTwistFull = 35.0f * kPi / 180.0f;
constexpr float kPunchSpeed = 1.8f;                // m/s of a free hand
constexpr float kPunchCooldown = 0.45f;            // s per hand
constexpr float kGlitchSpeed = 15.0f;              // m/s: above this a hand jumped (tracking lost and found)
constexpr int kPressFrames = 3;                    // game frames a swing holds its combat bit

using V3 = std::array<float, 3>;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float Wrap(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
// The bar angle of a vector in the bike's frame (right, up, fwd): about the up axis, 0 along right, positive toward
// fwd (the right grip forward = the bars turned LEFT).
// The bars' angle in the SEAT's space (the recentred tracking space: x right, y up, z toward the rider): about its up axis,
// positive = the right end forward (-z) = the bars turned LEFT; and a vector turned by that angle.
float SeatAngle(const V3& v) { return std::atan2(-v[2], v[0]); }
V3 RotateSeat(const V3& v, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return {v[0] * c + v[2] * s, v[1], v[2] * c - v[0] * s};
}
V3 RotateAboutUp(const V3& v, float angle) { // in the bike's frame
    const float c = std::cos(angle), s = std::sin(angle);
    return {v[0] * c - v[2] * s, v[1], v[0] * s + v[2] * c};
}

void QuatMul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
bool QuatNormalise(float q[4]) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f) || !std::isfinite(n)) return false;
    for (int k = 0; k < 4; ++k) q[k] /= n;
    return true;
}
// Columns X, Y, Z (a proper rotation) -> quaternion x, y, z, w.
void QuatFromAxes(const V3& X, const V3& Y, const V3& Z, float q[4]) {
    const float m00 = X[0], m10 = X[1], m20 = X[2], m01 = Y[0], m11 = Y[1], m21 = Y[2], m02 = Z[0], m12 = Z[1], m22 = Z[2];
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25f * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25f * s;
        q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25f * s;
    }
    QuatNormalise(q);
}

// a model matrix (column-major, race_render.h) on a point / a direction, and a unit vector.
V3 MatPoint(const rr::render::Mat4& m, const float p[3]) {
    V3 o{};
    for (int k = 0; k < 3; ++k) o[k] = m.m[k] * p[0] + m.m[4 + k] * p[1] + m.m[8 + k] * p[2] + m.m[12 + k];
    return o;
}
V3 MatDir(const rr::render::Mat4& m, const V3& d) {
    V3 o{};
    for (int k = 0; k < 3; ++k) o[k] = m.m[k] * d[0] + m.m[4 + k] * d[1] + m.m[8 + k] * d[2];
    return o;
}
V3 Unit(const V3& v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 1e-9f ? V3{v[0] / l, v[1] / l, v[2] / l} : V3{0, 0, 0};
}
V3 Minus(const V3& a, const V3& b, float t) { return {a[0] - b[0] * t, a[1] - b[1] * t, a[2] - b[2] * t}; }
float Dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

uint8_t AxisByte(float v) { return static_cast<uint8_t>(std::lround(127.5f + std::clamp(v, -1.0f, 1.0f) * 127.5f)); }

// ------------------------------------------------------------------------------------------------ the mock's script
// "<frame> <command> [args]" per line or ';'-separated (a file, or the text itself). Commands (hands: left | right |
// both): grab H (the hand onto its grip, the grip button squeezed), release H (the button let go; the hand stays),
// turn DEG [FRAMES] (the held bars turned, positive LEFT, over FRAMES), twist DEG [FRAMES] (the right wrist rolled
// toward the rider), trigger H VALUE (0..1), grip H VALUE (the grip button alone: a fist), move H DX DY DZ FRAMES (a free hand moved by metres in the seat's space -
// x right, y up, z toward the rider - over FRAMES, from where it was let go), home H (back to where it was let go),
// lift DY DZ FRAMES [H] (the held hands - both, or H - raised DY and pulled back DZ, metres, over FRAMES), follow SHARE (the held hands
// ride the drawn grips by SHARE 0..1 - a player tracking the drawn bars through the lean, the hills and the shake - the
// rest staying put in the seat's space), bob AMP PERIOD (both held hands moved up and down together by AMP metres, a
// sine of PERIOD frames; 0 stops it).
struct ScriptEvent {
    long frame = 0;
    std::string command;
    int hands = 0; // bit 0 left, bit 1 right
    float v[4] = {0, 0, 0, 0};
};

struct Ramp { // a scalar moved linearly between two frames
    float from = 0, to = 0;
    long start = 0, frames = 0;
    float At(long f) const {
        if (frames <= 0 || f >= start + frames) return to;
        if (f <= start) return from;
        return from + (to - from) * static_cast<float>(f - start) / static_cast<float>(frames);
    }
    void Set(float target, long f, long n, float current) {
        from = current;
        to = target;
        start = f;
        frames = n;
    }
};

std::vector<ScriptEvent> ParseScript(const std::string& arg) {
    std::string text = arg;
    {
        std::ifstream in(arg);
        if (in) {
            std::ostringstream o;
            o << in.rdbuf();
            text = o.str();
        }
    }
    for (char& c : text)
        if (c == ';') c = '\n';
    std::vector<ScriptEvent> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream w(line);
        ScriptEvent e;
        if (!(w >> e.frame >> e.command)) continue;
        const auto hands = [&](const std::string& h) {
            if (h == "left") return 1;
            if (h == "right") return 2;
            if (h == "both") return 3;
            throw std::runtime_error("vr bars script: '" + h + "' is not left / right / both in: " + line);
        };
        std::string h;
        if (e.command == "grab" || e.command == "release" || e.command == "home") {
            w >> h;
            e.hands = hands(h);
        } else if (e.command == "turn" || e.command == "twist") {
            if (!(w >> e.v[0])) throw std::runtime_error("vr bars script: " + e.command + " needs degrees: " + line);
            if (!(w >> e.v[1])) e.v[1] = 0;
        } else if (e.command == "trigger" || e.command == "grip") {
            w >> h;
            e.hands = hands(h);
            if (!(w >> e.v[0])) throw std::runtime_error("vr bars script: " + e.command + " needs a value: " + line);
        } else if (e.command == "lift") { // the held hands raised DY and pulled back DZ over FRAMES [left | right]
            e.hands = 3;
            if (!(w >> e.v[0] >> e.v[1] >> e.v[2])) throw std::runtime_error("vr bars script: lift DY DZ FRAMES: " + line);
            if (w >> h) e.hands = hands(h);
        } else if (e.command == "follow") {
            e.hands = 3;
            if (!(w >> e.v[0])) throw std::runtime_error("vr bars script: follow SHARE: " + line);
        } else if (e.command == "bob") {
            e.hands = 3;
            if (!(w >> e.v[0] >> e.v[1])) throw std::runtime_error("vr bars script: bob AMP PERIOD: " + line);
        } else if (e.command == "move") {
            w >> h;
            e.hands = hands(h);
            if (!(w >> e.v[0] >> e.v[1] >> e.v[2] >> e.v[3])) throw std::runtime_error("vr bars script: move H DX DY DZ FRAMES: " + line);
        } else {
            throw std::runtime_error("vr bars script: unknown command '" + e.command + "' in: " + line);
        }
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptEvent& a, const ScriptEvent& b) { return a.frame < b.frame; });
    return out;
}

} // namespace

struct VrHandlebars::Impl {
    BarGrips grips;
    VrHandsDraw draw;
    bool loaded = false;
    // ---- the mock's script
    std::vector<ScriptEvent> script;
    size_t nextEvent = 0;
    bool scripted = false;
    struct ScriptHand {
        bool on = false;
        float grip = 0, trigger = 0;
        V3 rest{};              // the local position where it was let go
        float restQ[4] = {0, 0, 0, 1};
        Ramp move[3];
        bool haveRest = false;
        bool placed = false;    // on the bars: its hold point taken (centre + offset, the seat's space)
        V3 centre{}, offset{};
    } sh[2];
    Ramp turn, twist;
    Ramp liftUp, liftBack; // "lift DY DZ FRAMES" - both held hands raised / pulled back (metres)
    int liftHands = 3;     // ... the hands it moves (bit 0 left, bit 1 right)
    float follow = 0.0f;   // "follow SHARE": the held hands ride the drawn grips by this share
    float bobAmp = 0.0f;   // "bob AMP PERIOD": both held hands up and down together
    long bobPeriod = 0, bobStart = 0;
    // ---- the hands
    struct Hand {
        bool valid = false, grabbed = false, gripDown = false, havePrev = false;
        bool wasInside = false;  // within the grab's reach last frame (the clenched fist's entry)
        float sinceHeld = 99.0f; // seconds since the hand last held its grip
        V3 local{}, prevLocal{}, velocity{};
        float q[4] = {0, 0, 0, 1};
        V3 bike{};              // the hand in the bike's frame, metres
        float cooldown = 0, freeTime = 0, pulse = 0, pulseAmp = 0;
        int press = 0;          // game frames of the swing's combat bit left
        int pressAction = 1;    // ... its combat action (1 R1, 2 L1, 5 R1+Up, 6 L1+Up)
        float grip = 0, trigger = 0;
        rr::xr::WorldEye world{};
        float aimForward[3] = {0, 0, 1}; // the aim pose's forward in the world (the fingers' direction)
    } hand[2];
    // ---- the bars
    float angle = 0;            // the bars' physical angle, radians, positive LEFT
    bool twoValid = false, oneValid = false;
    float twoRef = 0, oneRefAngle = 0;
    V3 oneRefHand{}, oneSeed{};
    int mask = 0;
    bool reachable = false, held = false, paused = false;
    float steer = 0;            // -1 left .. 1 right, after the dead zone
    V3 grip[2]{};               // the neutral grips in the bike's frame, metres (the height setting included)
    float upm = 1.0f;
    // ---- the throttle
    bool twistValid = false;
    float twistRef[4] = {0, 0, 0, 1};
    float twistAngle = 0, throttle = 0, brake = 0;
    int kick = 0;               // game frames of the kick bit left
    int weapon = 9;             // the player's weapon in hand, riderDef+0x2E (9 = fists; UpdateBike)
    int weaponPart = -1;        // the rider part holding its weapon object (7 / 10), -1 none
    uint32_t rider = 0;         // the player's rider (pool 1)
    bool weaponOverride = false;
    // The tracked hand that holds the weapon: the side of the rider's glove part the weapon object hangs on.
    int WeaponHand() const {
        // in the Physical combat mode the weapon stays in the hand the player chose (it hits with it)
        if (VrPrefs().melee.Physical()) return VrPrefs().melee.weaponHand == 0 ? 0 : 1; // (and Buttons + physical)
        return weaponPart >= 0 && weaponPart == grips.LeftGlovePart() ? 0 : 1;
    }
    float sentHaptics[2] = {0.0f, 0.0f}; // the levels given the host this frame
    // The grips as the game draws the bar this frame (BarGrips::PosedGrips, the height setting), in the world, and the
    // bar's world direction left -> right.
    void PosedGripsWorld(float out[2][3], V3& axis) const {
        float p[2][3];
        if (!grips.PosedGrips(p)) grips.Grips(p);
        const BikeFrame& f = grips.Frame();
        for (int h = 0; h < 2; ++h) {
            float b[3] = {p[h][0], p[h][1] + static_cast<float>(VrPrefs().bars.heightCm) / 100.0f * upm, p[h][2]};
            grips.ToWorld(b, out[h]);
        }
        float len = 0.0f;
        for (int k = 0; k < 3; ++k) {
            axis[k] = out[1][k] - out[0][k];
            len += axis[k] * axis[k];
        }
        len = std::sqrt(len);
        for (int k = 0; k < 3; ++k) axis[k] = len > 1e-6f ? axis[k] / len : f.right[k];
    }
    // ---- the held hands on the grips of the bike as drawn
    // A grip's frame in the world through a bike model matrix: the point (the fist's centre goes there), the bar's
    // direction left -> right, and the fork's own up and forward (the bike's heading, lean and pitch and the fork's
    // turn from rest: GTA SA VR's live handlebar frame).
    struct GripPose {
        V3 point{}, bar{}, up{}, fwd{};
    };
    bool GripPoses(const rr::render::Mat4& model, const rr::PartMatrix* parts, GripPose out[2]) const {
        float pos[2][3], rot[9];
        if (!grips.ModelGrips(parts, pos, rot)) return false;
        const V3 bar = Unit(MatDir(model, {pos[1][0] - pos[0][0], pos[1][1] - pos[0][1], pos[1][2] - pos[0][2]}));
        // the model's up is its -y, forward +z (x right, y down, z forward), turned with the fork
        V3 up = Unit(MatDir(model, {-rot[1], -rot[4], -rot[7]}));
        up = Unit(Minus(up, bar, Dot3(up, bar)));
        V3 fwd = MatDir(model, {rot[2], rot[5], rot[8]});
        fwd = Unit(Minus(Minus(fwd, bar, Dot3(fwd, bar)), up, Dot3(fwd, up)));
        if (Dot3(bar, bar) < 0.5f || Dot3(up, up) < 0.5f || Dot3(fwd, fwd) < 0.5f) return false;
        for (int h = 0; h < 2; ++h) {
            out[h].point = MatPoint(model, pos[h]);
            out[h].bar = bar;
            out[h].up = up;
            out[h].fwd = fwd;
        }
        return true;
    }
    // A held hand: the palm-down hold IN THE GRIP'S FRAME (a Touch controller's grip pose so held: -Z along the bar
    // inward - the thumb's side - +X up for the right hand / down for the left, the aim - the fingers - forward and 30
    // degrees down, the right one rolled about the bar by the twist grip), the fingers closed round the bar (GT2's
    // holdingWheel), and the fist's centre (vr_hands_draw.h kFistCentre) ON the grip point. Hands follow tilt off:
    // the controller's own wrist rotation, still seated on the grip.
    void HeldGlove(int h, const GripPose& gp, GloveDraw& g) const {
        const Hand& hd = hand[h];
        if (VrPrefs().bars.handsFollowTilt) {
            const V3& barRight = gp.bar;
            const V3 inward = h == 1 ? V3{-barRight[0], -barRight[1], -barRight[2]} : barRight;
            V3 xAxis = h == 1 ? gp.up : V3{-gp.up[0], -gp.up[1], -gp.up[2]};
            V3 aim{};
            for (int k = 0; k < 3; ++k) aim[k] = gp.fwd[k] * 0.866f - gp.up[k] * 0.5f;
            if (h == 1 && twistAngle != 0.0f) { // Rodrigues about the grip's +Z (outward, = barRight) by the twist
                const float c = std::cos(twistAngle), sn = std::sin(twistAngle);
                const auto roll = [&](const V3& v) {
                    const V3 kx = Cross(barRight, v);
                    const float kd = Dot3(barRight, v);
                    V3 o{};
                    for (int k = 0; k < 3; ++k) o[k] = v[k] * c + kx[k] * sn + barRight[k] * kd * (1.0f - c);
                    return o;
                };
                xAxis = roll(xAxis);
                aim = roll(aim);
            }
            const V3 zAxis{-inward[0], -inward[1], -inward[2]};
            const V3 yAxis = Cross(zAxis, xAxis);
            for (int k = 0; k < 3; ++k) {
                g.gripRight[k] = xAxis[k];
                g.gripUp[k] = yAxis[k];
                g.gripForward[k] = inward[k];
                g.aimForward[k] = aim[k];
            }
        } else {
            for (int k = 0; k < 3; ++k) {
                g.gripRight[k] = hd.world.right[k];
                g.gripUp[k] = hd.world.up[k];
                g.gripForward[k] = hd.world.forward[k];
                g.aimForward[k] = hd.aimForward[k];
            }
        }
        g.grip = 1.0f;
        g.trigger = 1.0f;
        VrHandsDraw::SeatFist(g, gp.point.data());
    }
    // The measurement (the log, Totals): per frame and held hand, the drawn fist's centre against the grip point of the
    // bike AS DRAWN (the renderer's own matrix and part slots), the hand's across-the-palm axis against that bar, and
    // the renderer's matrix against this frame's own read of the record (0: the same frame, no lag).
    struct HandsCheck {
        size_t handFrames = 0, drawnFrames = 0, readFrames = 0;
        double maxMm = 0.0, maxDeg = 0.0, sumMm = 0.0, maxModelDiff = 0.0, maxLean = 0.0, maxFork = 0.0;
        double maxRealMm = 0.0, maxRoll = 0.0; // the tracked hand to its drawn grip; the original's own roll
        long maxRealFrame = -1;
        // the eye (the seat's anchor: the rider's animated head) in the drawn bike's frame, its travel since the hold began
        bool haveEyeRef = false;
        float eyeRef[3] = {};
        double maxEyeMm = 0.0;
        long maxMmFrame = -1;
        size_t partsDiffer = 0;
        std::FILE* csv = nullptr;
        bool csvTried = false;
        long frame = -1;
    } check;
    void MeasureHeld(const GloveDraw gloves[2], const rr::render::Mat4* drawnModel, const rr::PartMatrix* drawnParts) {
        HandsCheck& c = check;
        if (c.frame == frame || drawnModel == nullptr || !grips.Frame().valid) return; // once a frame (two eyes)
        c.frame = frame;
        GripPose ref[2];
        if (!GripPoses(*drawnModel, drawnParts, ref)) return;
        ++c.drawnFrames;
        double diff = 0.0; // the renderer's matrix and slots against this frame's own read of the record
        for (int i = 0; i < 16; ++i) diff = std::max(diff, double(std::fabs(grips.Model().m[i] - drawnModel->m[i])));
        c.maxModelDiff = std::max(c.maxModelDiff, diff);
        const rr::PartMatrix* own = grips.Parts();
        if ((own == nullptr) != (drawnParts == nullptr) ||
            (own != nullptr && std::memcmp(own, drawnParts, 5 * sizeof(rr::PartMatrix)) != 0))
            ++c.partsDiffer;
        // the drawn bike's lean (its right axis against the level; the world's up is -y) and the fork's turn
        const V3 right = Unit(MatDir(*drawnModel, {1, 0, 0})), fwd = Unit(MatDir(*drawnModel, {0, 0, 1}));
        const double lean = std::asin(std::clamp(double(-right[1]), -1.0, 1.0)) * 57.29577951308232;
        const double fork = std::atan2(double(Dot3(ref[0].bar, fwd)), double(Dot3(ref[0].bar, right))) * 57.29577951308232;
        double mm[2] = {-1.0, -1.0}, deg[2] = {-1.0, -1.0}, real[2] = {-1.0, -1.0};
        const double roll = grips.Lean().rollDegrees;
        for (int h = 0; h < 2; ++h) {
            if (!gloves[h].visible || !hand[h].grabbed) continue;
            float fist[3];
            VrHandsDraw::FistCentre(gloves[h], fist);
            double d2 = 0.0;
            for (int k = 0; k < 3; ++k) d2 += double(fist[k] - ref[h].point[k]) * double(fist[k] - ref[h].point[k]);
            mm[h] = std::sqrt(d2) / double(upm) * 1000.0;
            const rr::render::Mat4 hm = VrHandsDraw::HandModel(gloves[h]); // its z: across the palm, along the bar held
            const V3 across = Unit({hm.m[8], hm.m[9], hm.m[10]});
            deg[h] = std::acos(std::clamp(double(std::fabs(Dot3(across, ref[h].bar))), 0.0, 1.0)) * 57.29577951308232;
            double r2 = 0.0; // the player's real hand (the tracked grip pose through the seat's anchor) to the drawn grip
            for (int k = 0; k < 3; ++k)
                r2 += double(hand[h].world.eye[k] - ref[h].point[k]) * double(hand[h].world.eye[k] - ref[h].point[k]);
            real[h] = std::sqrt(r2) / double(upm) * 1000.0;
            if (real[h] > c.maxRealMm) {
                c.maxRealMm = real[h];
                c.maxRealFrame = frame;
            }
            c.maxRoll = std::max(c.maxRoll, std::fabs(roll));
            ++c.handFrames;
            c.sumMm += mm[h];
            if (mm[h] > c.maxMm) {
                c.maxMm = mm[h];
                c.maxMmFrame = frame;
            }
            c.maxDeg = std::max(c.maxDeg, deg[h]);
            c.maxLean = std::max(c.maxLean, std::fabs(lean));
            c.maxFork = std::max(c.maxFork, std::fabs(fork));
        }
        if (!c.csvTried) { // DEVELOPMENT: RRJB_HANDS_LOG=<csv> - the measurement per frame
            c.csvTried = true;
            if (const char* path = std::getenv("RRJB_HANDS_LOG")) {
                c.csv = std::fopen(path, "wb");
                if (c.csv) std::fprintf(c.csv, "frame,lean_deg,fork_deg,left_mm,left_deg,right_mm,right_deg,model_diff,roll_deg,"
                                               "left_real_mm,right_real_mm\n");
            }
        }
        if (c.csv)
            std::fprintf(c.csv, "%ld,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3g,%.2f,%.1f,%.1f\n", frame, lean, fork, mm[0], deg[0], mm[1],
                         deg[1], diff, roll, real[0], real[1]);
    }
    int lastPunchHand = -1;
    float sinceLastPunch = 99.0f;
    // ---- this frame's pad
    rr::xr::XrPad touch;
    uint8_t lx = 0x80, ry = 0x80;
    // ---- the log
    long frame = 0;
    std::chrono::steady_clock::time_point last{};
    struct Totals {
        size_t grabs[2] = {0, 0}, framesBoth = 0, framesOne = 0, punches[2] = {0, 0}, kicks = 0, landedPulses = 0;
        size_t clenchedGrabs = 0, wideGrabs = 0, fastEntries = 0;
        size_t padFrames = 0, barPadFrames = 0;
        float angleMin = 0, angleMax = 0, steerMin = 0, steerMax = 0, throttleMax = 0, twistMax = 0;
        // the bike's heading change (degrees, positive = to the right) while the held bars steered left / right
        // (|steering| > 0.3, moving faster than 5 units / s), and the top speed while held
        double turnedLeft = 0.0, turnedRight = 0.0;
        float topSpeedHeld = 0.0f, lastHeading = 0.0f;
        bool haveHeading = false;
        int lxMin = 255, lxMax = 0;
    } totals;
    bool trace = false, gripsLogged = false;
    bool eyeLatched = false; // SeatEye: the eye's point on the drawn bike (right / up / fwd, world units)
    float eyeLocal[3] = {};
    // the latch is KEPT across a fall (the first seated frame after it is a rider still climbing on - the
    // eye re-latched there sat off the seat for the rest of the race); a new bike or seat takes a new one
    bool eyeAway = false;  // the head view was not drawn since the eye was last used
    uint32_t eyeBike = 0;  // the latch's bike and seat
    const float* eyeSeat = nullptr;

    void ReleaseAll() {
        for (Hand& h : hand) {
            h.grabbed = false;
            h.wasInside = false;
        }
        twoValid = oneValid = twistValid = false;
        mask = 0;
        angle = 0;
        held = false;
        steer = 0;
        twistAngle = 0;
    }
    void RunScript(long f);
    bool ScriptHands(const rr::xr::WorldAnchor& a, long f, rr::xr::HandPose out[2]);
    void Solve(const BarsSettings& s, float dt);
    void Swings(const BarsSettings& s, float dt, const rr::xr::WorldAnchor& a);
};

VrHandlebars::VrHandlebars() : impl_(std::make_unique<Impl>()) {}
VrHandlebars::~VrHandlebars() {
    if (impl_ && impl_->check.csv) std::fclose(impl_->check.csv);
}

bool VrHandlebars::Load(const rr::DiscImage& disc, bool mock) {
    Impl& m = *impl_;
    m.loaded = m.grips.Load(disc);
    m.trace = std::getenv("RRJB_BARS_TRACE") != nullptr;
    const std::string& arg = BarsScriptArgument();
    if (!arg.empty()) {
        if (!mock) {
            std::printf("vr bars: --vr-bars-script ignored (only the desktop VR mock takes scripted controllers)\n");
        } else {
            m.script = ParseScript(arg);
            m.scripted = true;
            std::printf("vr bars: the mock's controllers from a script, %zu event(s)\n", m.script.size());
        }
    }
    std::printf("vr bars: %s; the grips from the rider's gloves (parts %d / %d of model 150): %s\n",
                VrPrefs().bars.Describe().c_str(), BarGrips::kLeftGlove, BarGrips::kRightGlove,
                m.loaded ? "loaded" : "NOT loaded (no handlebars)");
    return m.loaded;
}

bool VrHandlebars::Active() const { return impl_->loaded && VrPrefs().bars.steering == BarsSettings::kHandlebars; }

void VrHandlebars::UpdateBike(const uint8_t* ram, uint32_t bike, uint32_t rider, const float* seat, bool headView) {
    if (!impl_->loaded) return;
    const float leanScale = headView ? static_cast<float>(VrPrefs().bars.visualLean) / 100.0f : 1.0f;
    if (!headView) {
        impl_->eyeAway = true;
        if (FallViewLegacy()) impl_->eyeLatched = false; // the control: re-latched on the next seated frame
    }
    if (impl_->eyeLatched && (bike != impl_->eyeBike || seat != impl_->eyeSeat)) impl_->eyeLatched = false;
    impl_->eyeBike = bike;
    impl_->eyeSeat = seat;
    float modernRoll = 0.0f; // the Modern handling draws its own lean (handling_modern.h), leanScale of it
    const bool modern = headView && rr::game::PlayerHandling().VisualRoll(modernRoll);
    // Modern's (SA's) lean is drawn whole - the Visual bike lean % is Original's; the view keeps its own
    // share of it against the horizon lock (handling_modern.h cameraRollPct)
    impl_->grips.Update(ram, bike, rider, seat, modern ? 1.0f : leanScale, modern ? &modernRoll : nullptr);
    // the weapon in hand: riderDef (bike +0x43C) +0x2E, 9 = fists (CombatDecode's weapon byte)
    uint32_t rd = 0;
    if (ram != nullptr && bike >= 0x80000000u && bike < 0x80200000u) std::memcpy(&rd, ram + ((bike + 0x43Cu) & 0x1FFFFFu), 4);
    impl_->weapon = rd >= 0x80000000u && rd < 0x80200000u ? ram[(rd + 0x2Eu) & 0x1FFFFFu] : 9;
    // the weapon object's hand (weapon_session.h ReadWeaponInHand: the rider's seat 0 kind +0x3C, part 7 or 10)
    impl_->rider = rider;
    impl_->weaponPart = -1;
    if (ram != nullptr && rider >= 0x80000000u && rider < 0x80200000u) {
        rr::game::WeaponInHand w;
        if (rr::game::ReadWeaponInHand(ram, rider, w)) impl_->weaponPart = w.hand;
    }
}

bool VrHandlebars::WheelContacts(float front[2], float rear[2]) const {
    return impl_->loaded && impl_->grips.WheelContacts(front, rear);
}

bool VrHandlebars::SeatEye(const float headEye[3], float out[3], float back) {
    Impl& m = *impl_;
    if (!m.loaded || !VrPrefs().bars.eyeOnBike || !m.grips.Frame().valid) {
        m.eyeLatched = false;
        return false;
    }
    if (m.eyeAway) { // back in the head view (a re-seat): where a latch taken now would put the eye
        float leaned[3], now[3];
        m.grips.Lean().Point(headEye, leaned);
        m.grips.ToBike(leaned, now);
        static bool haveLast = false;
        static float last[3] = {};
        if (m.eyeLatched || haveLast) {
            const float* ref = m.eyeLatched ? m.eyeLocal : last;
            double d2 = 0.0;
            for (int k = 0; k < 3; ++k) d2 += double(now[k] - ref[k]) * double(now[k] - ref[k]);
            Comfort().latchDrift = std::max(Comfort().latchDrift, std::sqrt(d2)); // (kept: not applied)
        }
        if (m.eyeLatched) {
            haveLast = true;
            std::memcpy(last, m.eyeLocal, sizeof(last));
        }
        m.eyeAway = false;
    }
    if (!m.eyeLatched) {
        static bool haveLast = false; // (the control's re-latch: how far it moved the eye on the bike)
        static float last[3] = {};
        float leaned[3];
        m.grips.Lean().Point(headEye, leaned); // onto the drawn bike, then into its frame
        m.grips.ToBike(leaned, m.eyeLocal);
        if (haveLast) {
            double d2 = 0.0;
            for (int k = 0; k < 3; ++k) d2 += double(m.eyeLocal[k] - last[k]) * double(m.eyeLocal[k] - last[k]);
            Comfort().maxEyeStepReseat = std::max(Comfort().maxEyeStepReseat, std::sqrt(d2));
        }
        haveLast = true;
        std::memcpy(last, m.eyeLocal, sizeof(last));
        m.eyeLatched = true;
        std::printf("vr bars: frame %ld - the eye fixed on the bike at right %+.3f up %+.3f fwd %+.3f (world units)\n", m.frame,
                    double(m.eyeLocal[0]), double(m.eyeLocal[1]), double(m.eyeLocal[2]));
    }
    const float local[3] = {m.eyeLocal[0], m.eyeLocal[1], m.eyeLocal[2] - back}; // the seat moved back
    static float loggedBack = 0.0f;
    if (back != loggedBack) {
        loggedBack = back;
        std::printf("vr bars: frame %ld - the seat moved %.3f world units %s along the bike: the eye at right %+.3f up %+.3f "
                    "fwd %+.3f ([vr] seat_back_cm)\n",
                    m.frame, double(std::fabs(back)), back > 0.0f ? "back" : "forward", double(local[0]), double(local[1]),
                    double(local[2]));
    }
    m.grips.ToWorld(local, out);
    return true;
}

bool VrHandlebars::ForkOverride(rr::PartMatrix& out) const {
    if (!Active() || !impl_->grips.ForkTurned()) return false;
    out = impl_->grips.Fork();
    return true;
}

const VisualLean& VrHandlebars::Lean() const {
    static const VisualLean kNone;
    return impl_->loaded ? impl_->grips.Lean() : kNone;
}

uint32_t VrHandlebars::HiddenRiderParts(bool headView) const {
    // the whole rider (all 17 sub-meshes of model 150): in the head view the player IS the rider, and the player's
    // own tracked hands are drawn on the grips instead (the Stick mode keeps head_camera.h's partial hiding)
    return Active() && headView ? 0x1FFFFu : 0u;
}

// ------------------------------------------------------------------------------------------------ the script
void VrHandlebars::Impl::RunScript(long f) {
    while (nextEvent < script.size() && script[nextEvent].frame <= f) {
        const ScriptEvent& e = script[nextEvent++];
        for (int h = 0; h < 2; ++h) {
            if (!(e.hands & (1 << h))) continue;
            ScriptHand& s = sh[h];
            if (e.command == "grab") {
                s.on = true;
                s.grip = 1.0f;
            } else if (e.command == "release") {
                s.on = false;
                s.grip = 0.0f;
                for (Ramp& r : s.move) r = Ramp{}; // it rests where it held the grip (rest, set while on)
                s.placed = false;
            } else if (e.command == "trigger") {
                s.trigger = std::clamp(e.v[0], 0.0f, 1.0f);
            } else if (e.command == "grip") { // the grip button alone: a fist off the bars
                s.grip = std::clamp(e.v[0], 0.0f, 1.0f);
            } else if (e.command == "move") {
                for (int k = 0; k < 3; ++k) s.move[k].Set(e.v[k], f, static_cast<long>(e.v[3]), s.move[k].At(f));
            } else if (e.command == "home") {
                for (int k = 0; k < 3; ++k) s.move[k].Set(0.0f, f, 6, s.move[k].At(f));
            }
        }
        if (e.command == "turn") turn.Set(e.v[0] * kPi / 180.0f, f, static_cast<long>(e.v[1]), turn.At(f));
        if (e.command == "twist") twist.Set(e.v[0] * kPi / 180.0f, f, static_cast<long>(e.v[1]), twist.At(f));
        if (e.command == "lift") {
            liftUp.Set(e.v[0], f, static_cast<long>(e.v[2]), liftUp.At(f));
            liftBack.Set(e.v[1], f, static_cast<long>(e.v[2]), liftBack.At(f));
            liftHands = e.hands;
        }
        if (e.command == "follow") follow = std::clamp(e.v[0], 0.0f, 1.0f);
        if (e.command == "bob") {
            bobAmp = e.v[0];
            bobPeriod = std::max(1L, static_cast<long>(e.v[1]));
            bobStart = f;
        }
        std::printf("vr bars script: frame %ld %s\n", f, e.command.c_str());
    }
    touch = rr::xr::XrPad{};
    touch.connected = true;
    touch.leftGrip = sh[0].grip;
    touch.rightGrip = sh[1].grip;
    touch.leftTrigger = sh[0].trigger;
    touch.rightTrigger = sh[1].trigger;
    touch.source = "the bars script";
}

// The scripted hands as controller poses in the seat's (recentred) space: on the bars at the grip where it was taken
// (the grip's world point then, taken back through the anchor) turned by the script's angle about the bars' centre,
// fixed in the seat's space like a player's hands, the grip frame of a palm-down hold (Z along
// the bar outward for the right hand, inward for the left; X into the right palm = up, out of the left palm = down),
// the right one rolled by the twist; off the bars where they were let go, plus the move.
bool VrHandlebars::Impl::ScriptHands(const rr::xr::WorldAnchor& a, long f, rr::xr::HandPose out[2]) {
    out[0] = out[1] = rr::xr::HandPose{};
    float g[2][3];
    const bool haveGrips = grips.Frame().valid && grips.Grips(g);
    const float t = turn.At(f), tw = twist.At(f);
    const auto toLocal = [&](const float w[3]) -> V3 { // a world point -> the seat's space, metres
        float r[3];
        for (int k = 0; k < 3; ++k) r[k] = w[k] - a.origin[k];
        return {Dot(r, a.right) / a.unitsPerMetre, Dot(r, a.up) / a.unitsPerMetre, -Dot(r, a.ahead) / a.unitsPerMetre};
    };
    for (int h = 0; h < 2; ++h) {
        ScriptHand& s = sh[h];
        V3 pos{};
        float q[4] = {0, 0, 0, 1};
        if (s.on) {
            if (!s.placed) {
                // taking hold: the hand goes to its grip as the bike is NOW, and from then on stays put in the seat's
                // space, as a player's hand does (it does not ride the bike's lean or the rider's head animation);
                // the script's turn swings it about the bars' centre there
                if (!haveGrips) continue;
                float centre[3], mine[3], world[3];
                for (int k = 0; k < 3; ++k) {
                    centre[k] = 0.5f * (g[0][k] + g[1][k]);
                    mine[k] = g[h][k];
                }
                const float lift = static_cast<float>(VrPrefs().bars.heightCm) / 100.0f * upm;
                centre[1] += lift;
                mine[1] += lift;
                grips.ToWorld(centre, world);
                s.centre = toLocal(world);
                grips.ToWorld(mine, world);
                const V3 at = toLocal(world);
                const V3 back = RotateSeat({at[0] - s.centre[0], at[1] - s.centre[1], at[2] - s.centre[2]}, -t);
                for (int k = 0; k < 3; ++k) s.offset[k] = back[k];
                s.placed = true;
            }
            const V3 rot = RotateSeat(s.offset, t);
            for (int k = 0; k < 3; ++k) pos[k] = s.centre[k] + rot[k];
            if (follow > 0.0f && haveGrips) { // riding the drawn grips (their seat-space place this frame) by `follow`
                float centre[3], mine[3], world[3];
                const float lift = static_cast<float>(VrPrefs().bars.heightCm) / 100.0f * upm;
                for (int k = 0; k < 3; ++k) {
                    centre[k] = 0.5f * (g[0][k] + g[1][k]);
                    mine[k] = g[h][k];
                }
                centre[1] += lift;
                mine[1] += lift;
                grips.ToWorld(centre, world);
                const V3 c = toLocal(world);
                grips.ToWorld(mine, world);
                const V3 at = toLocal(world);
                const V3 r = RotateSeat({at[0] - c[0], at[1] - c[1], at[2] - c[2]}, t);
                for (int k = 0; k < 3; ++k) pos[k] += follow * (c[k] + r[k] - pos[k]);
            }
            if (bobAmp != 0.0f)
                pos[1] += bobAmp * std::sin(2.0f * kPi * static_cast<float>(f - bobStart) / static_cast<float>(bobPeriod));
            if (liftHands & (1 << h)) {
                pos[1] += liftUp.At(f); // the lift (the seat's space: y up, z toward the rider)
                pos[2] += liftBack.At(f);
            }

            // the palm-down hold in the seat's own axes (x right, y up, z toward the rider), the bars turned by t
            const V3 barRight{std::cos(t), 0.0f, -std::sin(t)};
            V3 Z = h == 1 ? barRight : V3{-barRight[0], -barRight[1], -barRight[2]};
            V3 X = h == 1 ? V3{0, 1, 0} : V3{0, -1, 0};
            V3 Y = Cross(Z, X);
            QuatFromAxes(X, Y, Z, q);
            if (h == 1 && tw != 0.0f) { // the wrist's roll about its own Z
                const float r[4] = {0.0f, 0.0f, std::sin(tw * 0.5f), std::cos(tw * 0.5f)};
                float o[4];
                QuatMul(q, r, o);
                std::memcpy(q, o, sizeof(q));
            }
            s.rest = pos;
            std::memcpy(s.restQ, q, sizeof(q));
            s.haveRest = true;
        } else {
            if (!s.haveRest) { // never on the bars yet: resting behind and below its grip
                if (!haveGrips) continue;
                float world[3];
                grips.ToWorld(g[h], world);
                s.rest = toLocal(world);
                s.rest[1] -= 0.25f; // out of the grab's reach (a hand in the lap)
                s.rest[2] += 0.15f;
                s.haveRest = true;
            }
            for (int k = 0; k < 3; ++k) pos[k] = s.rest[k] + s.move[k].At(f);
            std::memcpy(q, s.restQ, sizeof(q));
        }
        out[h].gripValid = out[h].aimValid = true;
        for (int k = 0; k < 3; ++k) out[h].grip[k] = out[h].aim[k] = pos[k];
        for (int k = 0; k < 4; ++k) out[h].grip[3 + k] = q[k];
        {   // the aim: a Touch controller held so, points its ring forward and ~30 degrees down (the fingers' way)
            const float yaw = s.on ? t : 0.0f;
            const V3 fingers = RotateSeat({0.0f, -0.5f, -0.866f}, yaw); // seat space: y up, -z ahead
            const V3 Z{-fingers[0], -fingers[1], -fingers[2]};
            V3 X = Cross({0.0f, 1.0f, 0.0f}, Z);
            const float xl = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
            for (float& c : X) c /= xl;
            const V3 Y = Cross(Z, X);
            float aq[4];
            QuatFromAxes(X, Y, Z, aq);
            for (int k = 0; k < 4; ++k) out[h].aim[3 + k] = aq[k];
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ the solver
void VrHandlebars::Impl::Solve(const BarsSettings& s, float dt) {
    bool just[2] = {false, false};
    for (int h = 0; h < 2; ++h) {
        Hand& hd = hand[h];
        // the grip where the bars are now: turned by their angle about their centre
        V3 centre{};
        for (int k = 0; k < 3; ++k) centre[k] = 0.5f * (grip[0][k] + grip[1][k]);
        V3 g{};
        const V3 rel{grip[h][0] - centre[0], grip[h][1] - centre[1], grip[h][2] - centre[2]};
        const V3 rot = RotateAboutUp(rel, angle);
        for (int k = 0; k < 3; ++k) g[k] = centre[k] + rot[k];
        float d2 = 0;
        for (int k = 0; k < 3; ++k) d2 += (hd.bike[k] - g[k]) * (hd.bike[k] - g[k]);
        if (hd.grabbed && (!hd.valid || hd.grip <= kGrabOff)) hd.grabbed = false;
        // the reach: 20 cm, 28 cm within a second of letting go; a press there takes the grip, and so
        // does a fist already clenched that ENTERS it slower than 1 m/s (brought to the bar, not swung through it)
        const bool wide = hd.sinceHeld < kRegrabSeconds;
        const float reach = wide ? kRegrabRadius : kGrabRadius;
        const bool inside = hd.valid && d2 <= reach * reach;
        const float speed = std::sqrt(Dot(hd.velocity.data(), hd.velocity.data()));
        const bool entering = inside && !hd.wasInside && hd.havePrev;
        const bool clenchedEntry = entering && hd.gripDown && speed <= kClenchedEntrySpeed;
        if (entering && hd.gripDown && hd.grip >= kGrabOn && !clenchedEntry) ++totals.fastEntries;
        // (a hand holding a weapon drawn from a holster does not take the bars - vr_holsters.h)
        if (!hd.grabbed && hd.valid && hd.grip >= kGrabOn && inside && (!hd.gripDown || clenchedEntry) && !HolsterHandBusy(h)) {
            hd.grabbed = just[h] = true;
            ++totals.grabs[h];
            if (clenchedEntry) ++totals.clenchedGrabs;
            if (wide && d2 > kGrabRadius * kGrabRadius) ++totals.wideGrabs;
            hd.pulse = 0.08f;
            hd.pulseAmp = 0.6f;
            std::printf("vr bars: frame %ld - the %s hand takes its grip (%.0f mm from it%s%s)\n", frame, h ? "right" : "left",
                        double(std::sqrt(d2) * 1000.0f), clenchedEntry ? ", the fist already clenched" : "",
                        wide ? ", the re-grab reach" : "");
        }
        hd.wasInside = inside;
        hd.sinceHeld = hd.grabbed ? 0.0f : std::min(hd.sinceHeld + dt, 99.0f);
        if (hd.grip <= kGrabOff) hd.gripDown = false;
        else if (hd.grip >= kGrabOn) hd.gripDown = true;
    }
    const bool L = hand[0].grabbed, R = hand[1].grabbed;
    const int newMask = (L ? 1 : 0) | (R ? 2 : 0);
    const float fullLock = s.FullLockDegrees() * kPi / 180.0f;
    // The bars' angle is measured from the hands in the SEAT's space - where the player's body is: the bike's lean and
    // the rider's head animation (which carries the seat) move the bars, not the player's hands against each other.
    const float width = std::sqrt((grip[1][0] - grip[0][0]) * (grip[1][0] - grip[0][0]) +
                                  (grip[1][2] - grip[0][2]) * (grip[1][2] - grip[0][2]));
    const V3 neutral{width, 0.0f, 0.0f};
    const float previous = angle;
    float desired = 0.0f;
    if (newMask == 3) { // two hands: the line between them (GTA SA VR's motorcycle chord)
        oneValid = false;
        const V3 chord{hand[1].local[0] - hand[0].local[0], hand[1].local[1] - hand[0].local[1], hand[1].local[2] - hand[0].local[2]};
        const float raw = Wrap(SeatAngle(chord) - SeatAngle(neutral));
        if (!twoValid || mask != newMask || just[0] || just[1]) {
            twoRef = Wrap(raw - previous);
            twoValid = true;
        }
        desired = Wrap(raw - twoRef);
        if (!std::isfinite(desired) || std::fabs(Wrap(desired - previous)) > kJump) {
            desired = previous;
            twoRef = Wrap(raw - desired);
        }
        const float clamped = std::clamp(desired, -fullLock * 1.25f, fullLock * 1.25f);
        if (clamped != desired) twoRef = Wrap(raw - clamped);
        desired = clamped;
    } else if (newMask != 0 && s.oneHand) { // one hand: its travel mirrored to the other grip
        twoValid = false;
        const int h = newMask == 1 ? 0 : 1;
        if (!oneValid || mask != newMask || just[h]) {
            oneRefHand = hand[h].local;
            oneSeed = RotateSeat(neutral, previous);
            oneRefAngle = previous;
            oneValid = true;
        }
        const float sign = h == 0 ? -2.0f : 2.0f;
        V3 chord{};
        for (int k = 0; k < 3; ++k) chord[k] = oneSeed[k] + (hand[h].local[k] - oneRefHand[k]) * sign;
        desired = oneRefAngle + Wrap(SeatAngle(chord) - SeatAngle(oneSeed));
        bool rebase = false;
        if (!std::isfinite(desired) || std::fabs(Wrap(desired - previous)) > kJump) {
            desired = previous;
            rebase = true;
        }
        const float clamped = std::clamp(desired, -fullLock * 1.25f, fullLock * 1.25f);
        if (rebase || clamped != desired) {
            oneRefHand = hand[h].local;
            oneSeed = RotateSeat(neutral, clamped);
            oneRefAngle = clamped;
        }
        desired = clamped;
    } else { // no hands (or one hand without the one-hand option): the bars centre
        twoValid = oneValid = false;
        desired = 0.0f;
    }
    mask = newMask;
    angle = desired;
    held = newMask == 3 || (newMask != 0 && s.oneHand);
    // the bars -> the steering: full lock at FullLockDegrees, the dead zone, right positive
    float v = std::clamp(-angle / fullLock, -1.0f, 1.0f);
    const float dz = static_cast<float>(s.deadzone) / 100.0f;
    v = std::fabs(v) <= dz ? 0.0f : std::copysign((std::fabs(v) - dz) / (1.0f - dz), v);
    steer = held ? v : 0.0f;
    // the twist grip: the right hand's roll about its own grip axis since the grab (GTA SA VR UpdateBikeThrottle)
    twistAngle = 0.0f;
    float twistThrottle = 0.0f;
    if (R && s.twistThrottle) {
        if (!twistValid || just[1]) {
            std::memcpy(twistRef, hand[1].q, sizeof(twistRef));
            twistValid = QuatNormalise(twistRef);
        } else {
            float cur[4];
            std::memcpy(cur, hand[1].q, sizeof(cur));
            if (QuatNormalise(cur)) {
                if (twistRef[0] * cur[0] + twistRef[1] * cur[1] + twistRef[2] * cur[2] + twistRef[3] * cur[3] < 0.0f)
                    for (float& c : cur) c = -c;
                const float inv[4] = {-twistRef[0], -twistRef[1], -twistRef[2], twistRef[3]};
                float rel[4];
                QuatMul(inv, cur, rel);
                float tw[4] = {0.0f, 0.0f, rel[2], rel[3]};
                if (QuatNormalise(tw)) {
                    twistAngle = Wrap(2.0f * std::atan2(tw[2], tw[3]));
                    twistThrottle = twistAngle <= kTwistDead ? 0.0f
                                                             : std::clamp((twistAngle - kTwistDead) / (kTwistFull - kTwistDead), 0.0f, 1.0f);
                }
            }
        }
    } else {
        twistValid = false;
    }
    throttle = std::max(twistThrottle, touch.rightTrigger);
    brake = touch.leftTrigger;
    if (newMask == 3) ++totals.framesBoth;
    else if (newMask != 0) ++totals.framesOne;
}

void VrHandlebars::Impl::Swings(const BarsSettings& s, float dt, const rr::xr::WorldAnchor& a) {
    for (int h = 0; h < 2; ++h) {
        Hand& hd = hand[h];
        hd.cooldown = std::max(0.0f, hd.cooldown - dt);
        hd.freeTime = hd.grabbed || !hd.valid ? 0.0f : hd.freeTime + dt;
        // the gesture throws blows only in the Gesture combat mode (vr_melee_settings.h); Physical = a contact
        if (VrPrefs().melee.mode != MeleeSettings::kGesture) continue;
        if (!s.motionPunches || hd.grabbed || !hd.valid || !hd.havePrev || hd.cooldown > 0.0f || hd.freeTime < 0.10f) continue;
        const float speed = std::sqrt(hd.velocity[0] * hd.velocity[0] + hd.velocity[1] * hd.velocity[1] + hd.velocity[2] * hd.velocity[2]);
        if (speed < kPunchSpeed || speed > kGlitchSpeed) continue; // (faster: a tracking jump, not an arm)
        if (reachable) { // a hand going back to its grip is reaching for the bars, not punching
            const BikeFrame& bf = grips.Frame();
            V3 world{}, local{};
            for (int k = 0; k < 3; ++k) {
                const float d[3] = {grip[h][0] - hd.bike[0], grip[h][1] - hd.bike[1], grip[h][2] - hd.bike[2]};
                world[k] = bf.right[k] * d[0] + bf.up[k] * d[1] + bf.fwd[k] * d[2];
            }
            local = {Dot(world.data(), a.right), Dot(world.data(), a.up), -Dot(world.data(), a.ahead)};
            const float len = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
            if (len > 0.05f && (local[0] * hd.velocity[0] + local[1] * hd.velocity[1] + local[2] * hd.velocity[2]) / len > 0.5f * speed)
                continue;
        }
        // a blow goes AWAY from the body: forward (-z), outward on its own side, or up; a pull back toward the chest or
        // across the body is not one
        const float forward = -hd.velocity[2], outward = h == 1 ? hd.velocity[0] : -hd.velocity[0], upward = hd.velocity[1];
        const bool down = -hd.velocity[1] > 0.75f * speed;
        const bool uppercut = upward > 0.7f * speed && forward > 0.0f;
        if (!down && !uppercut && forward < 0.35f * speed && outward < 0.6f * speed) continue;
        hd.cooldown = kPunchCooldown;
        if (down) { // mostly downward: the kick (R2, combat action 3)
            kick = kPressFrames;
            ++totals.kicks;
            std::printf("vr bars: frame %ld - the %s hand swings down at %.1f m/s: kick (R2, combat action 3)\n", frame,
                        h ? "right" : "left", double(speed));
        } else {
            // the combat action of this swing (CombatDecode RASHCDG 0x800C2348, the weapon in hand riderDef+0x2E):
            // fists (9) and weapons 0..5 take the hand's own action - right 1 (R1), left 2 (L1): the punch, or the
            // armed swing 142 / 146 with a weapon; the weapons whose actions 1 / 2 are only a taunt swing through
            // their own: 6 and 7 action 6 (L1 + Up), 8 action 5 (R1 + Up) - both give the armed swing 148
            int action = h ? 1 : 2;
            if (weapon == 6 || weapon == 7) action = 6;
            else if (weapon == 8) action = 5;
            hd.press = kPressFrames;
            hd.pressAction = action;
            ++totals.punches[h];
            static const char* const kBits[9] = {"", "R1", "L1", "R2", "R2+Up", "R1+Up", "L1+Up", "R1+Down", "R2+Down"};
            std::printf("vr bars: frame %ld - the %s hand swings at %.1f m/s (%+.2f %+.2f %+.2f): %s (%s, combat action %d, "
                        "weapon in hand %d)\n",
                        frame, h ? "right" : "left", double(speed), double(hd.velocity[0]), double(hd.velocity[1]),
                        double(hd.velocity[2]), weapon >= 9 ? "punch" : "weapon swing", kBits[action], action, weapon);
        }
        hd.pulse = 0.06f;
        hd.pulseAmp = 0.35f;
        lastPunchHand = h;
        sinceLastPunch = 0.0f;
    }
}

void VrHandlebars::UpdateHands(VrHost& vr, const rr::xr::WorldAnchor& anchor, bool headView, bool paused, long frame,
                               size_t landed) {
    Impl& m = *impl_;
    const BarsSettings& s = VrPrefs().bars;
    m.frame = frame;
    if (!Active()) {
        if (m.hand[0].valid || m.hand[1].valid || m.held) {
            m.ReleaseAll();
            for (auto& h : m.hand) h = Impl::Hand{};
        }
        if (m.weaponOverride) { // the weapon back in the rider's own hand
            rr::render::SetWeaponHandOverride(rr::render::WeaponHandOverride{});
            m.weaponOverride = false;
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    float dt = static_cast<float>(vr.DisplayPeriod());
    if (!m.scripted && m.last != std::chrono::steady_clock::time_point{})
        dt = std::clamp(std::chrono::duration<float>(now - m.last).count(), 0.001f, 0.1f);
    m.last = now;
    m.upm = anchor.unitsPerMetre > 0.0f ? anchor.unitsPerMetre : 1.0f;
    // the grips (metres, the bike's frame, the height setting)
    float g[2][3];
    const BikeFrame& bf = m.grips.Frame();
    m.reachable = headView && bf.valid && bf.seated && m.grips.Grips(g);
    if (m.grips.Grips(g))
        for (int h = 0; h < 2; ++h) {
            for (int k = 0; k < 3; ++k) m.grip[h][k] = g[h][k] / m.upm;
            m.grip[h][1] += static_cast<float>(s.heightCm) / 100.0f;
        }
    float gloves[2][3];
    if (m.reachable && !m.gripsLogged && m.grips.Gloves(gloves)) { // once: the grips against the eye and the gloves
        m.gripsLogged = true;
        float eye[3], posed[2][3];
        m.grips.ToBike(anchor.origin, eye);
        m.grips.PosedGrips(posed);
        std::printf("vr bars: in the bike's frame (right / up / fwd, world units): the grips left %+.3f %+.3f %+.3f right %+.3f "
                    "%+.3f %+.3f (as posed now: %+.3f %+.3f %+.3f / %+.3f %+.3f %+.3f), the eye %+.3f %+.3f %+.3f, the "
                    "rider's gloves %+.3f %+.3f %+.3f / %+.3f %+.3f %+.3f\n",
                    double(g[0][0]), double(g[0][1]), double(g[0][2]), double(g[1][0]), double(g[1][1]), double(g[1][2]),
                    double(posed[0][0]), double(posed[0][1]), double(posed[0][2]), double(posed[1][0]), double(posed[1][1]),
                    double(posed[1][2]), double(eye[0]), double(eye[1]), double(eye[2]), double(gloves[0][0]),
                    double(gloves[0][1]), double(gloves[0][2]), double(gloves[1][0]), double(gloves[1][1]), double(gloves[1][2]));
    }
    // the controllers: a headset's, or the mock's script
    rr::xr::HandPose hands[2];
    if (m.scripted) {
        m.RunScript(frame);
        m.ScriptHands(anchor, frame, hands);
    } else {
        m.touch = vr.TouchState();
        if (!vr.LocateHands(hands)) hands[0] = hands[1] = rr::xr::HandPose{};
    }
    for (int h = 0; h < 2; ++h) {
        Impl::Hand& hd = m.hand[h];
        hd.grip = h == 0 ? m.touch.leftGrip : m.touch.rightGrip;
        hd.trigger = h == 0 ? m.touch.leftTrigger : m.touch.rightTrigger;
        hd.valid = hands[h].gripValid;
        if (!hd.valid) {
            hd.havePrev = false;
            continue;
        }
        rr::xr::Pose p;
        for (int k = 0; k < 3; ++k) p.position[k] = hands[h].grip[k];
        for (int k = 0; k < 4; ++k) p.orientation[k] = hands[h].grip[3 + k];
        std::memcpy(hd.q, p.orientation, sizeof(hd.q));
        hd.prevLocal = hd.local;
        hd.local = {p.position[0], p.position[1], p.position[2]};
        if (hd.havePrev && dt > 0.0f) {
            for (int k = 0; k < 3; ++k) {
                const float raw = (hd.local[k] - hd.prevLocal[k]) / dt;
                hd.velocity[k] = 0.5f * hd.velocity[k] + 0.5f * raw;
            }
        } else {
            hd.velocity = {0, 0, 0};
        }
        hd.havePrev = true;
        hd.world = rr::xr::PlaceInWorld(p, anchor);
        {   // the aim pose (its forward: where the fingers point, vr_hands_draw.h); the grip's when there is none
            rr::xr::Pose ap = p;
            if (hands[h].aimValid)
                for (int k = 0; k < 4; ++k) ap.orientation[k] = hands[h].aim[3 + k];
            const rr::xr::WorldEye aw = rr::xr::PlaceInWorld(ap, anchor);
            for (int k = 0; k < 3; ++k) hd.aimForward[k] = aw.forward[k];
        }
        if (bf.valid) {
            float b[3];
            m.grips.ToBike(hd.world.eye, b);
            for (int k = 0; k < 3; ++k) hd.bike[k] = b[k] / m.upm;
        }
    }
    if (m.held && m.reachable && bf.valid) { // the seat (the player's body) on the drawn bike while held
        float eye[3];
        m.grips.ToBike(anchor.origin, eye);
        if (!m.check.haveEyeRef) std::memcpy(m.check.eyeRef, eye, sizeof(eye));
        m.check.haveEyeRef = true;
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) d2 += double(eye[k] - m.check.eyeRef[k]) * double(eye[k] - m.check.eyeRef[k]);
        m.check.maxEyeMm = std::max(m.check.maxEyeMm, std::sqrt(d2) / double(m.upm) * 1000.0);
    } else {
        m.check.haveEyeRef = false;
    }
    m.paused = paused;
    if (m.reachable && paused) {
        // the pause menu: the hands keep their hold (no re-grab after it), the bars and the throttle stand still
    } else if (m.reachable) {
        m.Solve(s, dt);
    } else {
        m.ReleaseAll();
        m.throttle = m.touch.rightTrigger;
        m.brake = m.touch.leftTrigger;
    }
    // the fork drawn at the hands' angle while they hold the bars (the grips, the held hands, the weapon
    // on the bar and the renderer's slot all from this one turn; the game's own slot is not touched)
    // (DEVELOPMENT RRJB_BARS_FORK=game: the game's own slot - the measurement's control)
    static const bool gameFork = std::getenv("RRJB_BARS_FORK") != nullptr && std::strcmp(std::getenv("RRJB_BARS_FORK"), "game") == 0;
    m.grips.SetForkTurn(m.reachable && m.held && !gameFork, m.angle);
    if (!paused) m.Swings(s, dt, anchor);
    // the haptics: the pulses, the road through the held grips, a thump when the blow lands
    m.sinceLastPunch += dt;
    if (landed > 0 && m.lastPunchHand >= 0 && m.sinceLastPunch < 1.0f) {
        m.hand[m.lastPunchHand].pulse = 0.15f;
        m.hand[m.lastPunchHand].pulseAmp = 1.0f;
        ++m.totals.landedPulses;
    }
    float amp[2] = {0, 0};
    for (int h = 0; h < 2; ++h) {
        Impl::Hand& hd = m.hand[h];
        if (hd.pulse > 0.0f) {
            amp[h] = hd.pulseAmp;
            hd.pulse -= dt;
        }
        if (hd.grabbed) amp[h] = std::max(amp[h], 0.03f + 0.10f * std::clamp(std::fabs(bf.speed) / 50.0f, 0.0f, 1.0f));
    }
    vr.SetHandHaptics(amp[0], amp[1]);
    m.sentHaptics[0] = amp[0], m.sentHaptics[1] = amp[1]; // vr_melee.cpp adds its thump to them
    // the weapon: in the head view the rider is not drawn, so the player's weapon goes into the tracked hand on the
    // side the rider holds it (weapon_draw.h WeaponHandOverride); in the chase view the rider holds it as the original
    // draws it
    {
        rr::render::WeaponHandOverride o;
        o.active = headView;
        o.rider = m.rider;
        if (o.active) {
            const int h = m.WeaponHand();
            const Impl::Hand& hd = m.hand[h];
            o.hidden = !hd.valid;
            // a hand on the bar holds it up, in the grip's frame of the bike as this frame drew it
            Impl::GripPose gp[2];
            const bool onBar = hd.grabbed && bf.valid && m.GripPoses(m.grips.Model(), m.grips.Parts(), gp);
            for (int k = 0; k < 3; ++k) {
                o.origin[k] = onBar ? gp[h].point[k] : hd.world.eye[k];
                // out of the fist's thumb side (the grip's -Z); a hand on the bar holds it up
                o.along[k] = onBar ? gp[h].up[k] : hd.world.forward[k];
                o.side[k] = onBar ? gp[h].fwd[k] : hd.world.right[k];
            }
            o.scale = 1.0f / 1024.0f; // a game object: model units (race_scene.h kModelUnitsPerWorldUnit) per world unit
        }
        rr::render::SetWeaponHandOverride(o);
        m.weaponOverride = o.active;
    }
    // the log
    auto& t = m.totals;
    t.angleMin = std::min(t.angleMin, m.angle * 180.0f / kPi);
    t.angleMax = std::max(t.angleMax, m.angle * 180.0f / kPi);
    t.steerMin = std::min(t.steerMin, m.steer);
    t.steerMax = std::max(t.steerMax, m.steer);
    t.throttleMax = std::max(t.throttleMax, m.throttle);
    t.twistMax = std::max(t.twistMax, m.twistAngle * 180.0f / kPi);
    if (bf.valid && !paused) {
        float d = bf.headingDegrees - t.lastHeading;
        while (d > 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        if (t.haveHeading && m.held && std::fabs(bf.speed) > 5.0f) {
            if (m.steer < -0.3f) t.turnedLeft += d;
            if (m.steer > 0.3f) t.turnedRight += d;
        }
        if (m.held) t.topSpeedHeld = std::max(t.topSpeedHeld, std::fabs(bf.speed));
        t.lastHeading = bf.headingDegrees;
        t.haveHeading = true;
    }
    if ((m.scripted || m.trace) && frame % 24 == 0)
        std::printf("vr bars: f=%ld held=%c%c angle=%+.1f deg steer=%+.2f lx=0x%02X throttle=%.2f (twist %+.1f deg) brake=%.2f "
                    "ry=0x%02X heading=%+.1f deg speed=%.2f reach=%d\n",
                    frame, m.hand[0].grabbed ? 'L' : '-', m.hand[1].grabbed ? 'R' : '-', double(m.angle * 180.0f / kPi),
                    double(m.steer), unsigned(m.lx), double(m.throttle), double(m.twistAngle * 180.0f / kPi),
                    double(m.brake), unsigned(m.ry), double(bf.headingDegrees), double(bf.speed), m.reachable ? 1 : 0);
}

void VrHandlebars::ApplyToPad(rr::game::PadState& pad, long frame) {
    Impl& m = *impl_;
    (void)frame;
    if (!Active()) return;
    if (m.held && !m.paused) {
        // The bars in the hands (not in the pause menu, whose pad stays the digital one): the DualShock's ANALOG mode (pad_product.h, the device 0x73 the pad reader switches on
        // per frame): left stick X = the bars, right stick Y = the throttle (up) and the brake (down), the brake
        // winning. The pad's DIGITAL throttle and brake built before this (a keyboard, a desktop controller, a test
        // script's --hold T) are folded in as full deflections, since the analogue branch does not read them on the bike.
        const float throttle = std::max(m.throttle, pad.throttle ? 1.0f : 0.0f);
        const float brake = std::max(m.brake, pad.brake ? 1.0f : 0.0f);
        m.lx = AxisByte(m.steer);
        m.ry = brake > 0.05f ? AxisByte(brake) : AxisByte(-throttle);
        pad.device.analog = true;
        pad.device.lx = m.lx;
        pad.device.ly = AxisByte(-m.touch.stick[0][1]);
        pad.device.ry = m.ry;
        m.totals.lxMin = std::min<int>(m.totals.lxMin, m.lx);
        m.totals.lxMax = std::max<int>(m.totals.lxMax, m.lx);
        ++m.totals.barPadFrames;
    } else {
        // No hand on the bars (reaching for them, fighting, off the bike, the chase view): the digital pad as in the
        // Stick mode - the left stick steers through its bindings; the triggers (kept off the bindings in this mode,
        // game_host_vr.cpp) are the throttle and brake keys.
        m.lx = m.ry = 0x80;
        if (m.touch.rightTrigger > 0.25f) pad.throttle = true;
        if (m.touch.leftTrigger > 0.25f) pad.brake = true;
    }
    // the swings: held for a few frames so the combat decode sees the press
    for (int h = 0; h < 2; ++h)
        if (m.hand[h].press > 0) {
            const int a = m.hand[h].pressAction; // the combat input map's control and modifier (main.cpp --punch-action)
            if (a == 1 || a == 5 || a == 7) pad.r1 = true;
            if (a == 2 || a == 6) pad.l1 = true;
            if (a == 5 || a == 6) pad.padUp = true;
            if (a == 7) pad.padDown = true;
            --m.hand[h].press;
        }
    if (m.kick > 0) {
        pad.r2 = true;
        --m.kick;
    }
    ++m.totals.padFrames;
}

void VrHandlebars::Draw(const rr::render::Mat4& viewProj, const rr::render::Mat4* drawnModel,
                        const rr::PartMatrix* drawnParts) {
    Impl& m = *impl_;
    if (!Active()) return;
    GloveDraw gloves[2];
    GripMarker markers[2];
    const BikeFrame& bf = m.grips.Frame();
    V3 centre{};
    for (int k = 0; k < 3; ++k) centre[k] = 0.5f * (m.grip[0][k] + m.grip[1][k]);
    // the grips of the bike AS THE RENDERER DREW IT this frame - its own model matrix (the heading, the
    // lean, the origin) and part slots (the fork's turn, the body's pitch); a held hand is rigid on them, the controller
    // only steers and twists. (DEVELOPMENT RRJB_BARS_HANDS=legacy: the grip point of this frame's
    // re-normalised bike frame, the hold in the bike's axes without the fork's turn - the measurement's control.)
    static const bool legacy = std::getenv("RRJB_BARS_HANDS") != nullptr && std::strcmp(std::getenv("RRJB_BARS_HANDS"), "legacy") == 0;
    const rr::render::Mat4& model = drawnModel != nullptr ? *drawnModel : m.grips.Model();
    const rr::PartMatrix* parts = drawnModel != nullptr ? drawnParts : m.grips.Parts();
    Impl::GripPose gp[2];
    const bool haveGrips = bf.valid && m.GripPoses(model, parts, gp);
    const float lift = static_cast<float>(VrPrefs().bars.heightCm) / 100.0f * m.upm; // the reach only (the marker)
    for (int h = 0; h < 2; ++h) {
        const Impl::Hand& hd = m.hand[h];
        GloveDraw& g = gloves[h];
        g.right = h == 1;
        g.scale = m.upm;
        if (!hd.valid) continue;
        g.visible = true;
        if (hd.grabbed && haveGrips && !legacy) {
            m.HeldGlove(h, gp[h], g);
        } else if (hd.grabbed && bf.valid && legacy) {
            V3 barRight{};
            float posedWorld[2][3];
            m.PosedGripsWorld(posedWorld, barRight);
            V3 up{}, aim{};
            for (int k = 0; k < 3; ++k) {
                up[k] = bf.up[k];
                aim[k] = bf.fwd[k] * 0.866f - up[k] * 0.5f;
            }
            const V3 inward = h == 1 ? V3{-barRight[0], -barRight[1], -barRight[2]} : barRight;
            const V3 xAxis = h == 1 ? up : V3{-up[0], -up[1], -up[2]};
            const V3 yAxis = Cross({-inward[0], -inward[1], -inward[2]}, xAxis);
            for (int k = 0; k < 3; ++k) {
                g.origin[k] = posedWorld[h][k];
                g.gripRight[k] = xAxis[k];
                g.gripUp[k] = yAxis[k];
                g.gripForward[k] = inward[k];
                g.aimForward[k] = aim[k];
            }
            g.grip = g.trigger = 1.0f;
        } else { // free: at the controller's grip pose, the fingers along its aim
            for (int k = 0; k < 3; ++k) {
                g.origin[k] = hd.world.eye[k];
                g.gripRight[k] = hd.world.right[k];
                g.gripUp[k] = hd.world.up[k];
                g.gripForward[k] = hd.world.forward[k];
                g.aimForward[k] = hd.aimForward[k];
            }
            // the grip button closes the hand into a FIST (the index finger with it); the trigger alone curls the
            // index finger; both released: an open hand
            g.grip = std::clamp(hd.grip, 0.0f, 1.0f);
            g.trigger = std::max(std::clamp(hd.trigger, 0.0f, 1.0f), g.grip);
        }
        // the marker of a grip within reach of its free hand: on the drawn grip (moved by the bars' height setting,
        // where the reach is tested)
        if (m.reachable && !hd.grabbed && haveGrips) {
            const V3 rel{m.grip[h][0] - centre[0], m.grip[h][1] - centre[1], m.grip[h][2] - centre[2]};
            const V3 rot = RotateAboutUp(rel, m.angle);
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float d = (hd.bike[k] - (centre[k] + rot[k]));
                d2 += d * d;
            }
            if (d2 < kMarkerRadius * kMarkerRadius) {
                GripMarker& mk = markers[h];
                mk.visible = true;
                mk.inReach = d2 <= kGrabRadius * kGrabRadius;
                mk.scale = m.upm;
                for (int k = 0; k < 3; ++k) {
                    mk.centre[k] = gp[h].point[k] + gp[h].up[k] * lift;
                    mk.axis[k] = gp[h].bar[k];
                    mk.up[k] = gp[h].up[k];
                }
            }
        }
    }
    m.MeasureHeld(gloves, drawnModel, drawnParts);
    m.draw.Draw(viewProj, gloves, markers);
}

// ---- VR physical combat (vr_melee.h): the accessors
bool VrHandlebars::Hand(int h, HandView& out) const {
    const Impl& m = *impl_;
    if (!Active() || h < 0 || h > 1 || !m.hand[h].valid) return false;
    const Impl::Hand& hd = m.hand[h];
    out.grabbed = hd.grabbed;
    out.grip = hd.grip;
    out.trigger = hd.trigger;
    out.world = hd.world;
    for (int k = 0; k < 3; ++k) {
        out.aimForward[k] = hd.aimForward[k];
        out.local[k] = hd.local[k];
        out.bike[k] = hd.bike[k];
    }
    // its grip where the bars are now: turned by their angle about their centre (the solver's own grip point)
    V3 centre{};
    for (int k = 0; k < 3; ++k) centre[k] = 0.5f * (m.grip[0][k] + m.grip[1][k]);
    const V3 rot = RotateAboutUp({m.grip[h][0] - centre[0], m.grip[h][1] - centre[1], m.grip[h][2] - centre[2]}, m.angle);
    for (int k = 0; k < 3; ++k) out.gripBike[k] = centre[k] + rot[k];
    out.bikeValid = m.grips.Frame().valid;
    return true;
}

int VrHandlebars::WeaponHand() const { return impl_->WeaponHand(); }

void VrHandlebars::Haptics(float out[2]) const {
    const bool on = Active();
    out[0] = on ? impl_->sentHaptics[0] : 0.0f;
    out[1] = on ? impl_->sentHaptics[1] : 0.0f;
}

std::string VrHandlebars::Totals() const {
    const Impl& m = *impl_;
    const auto& t = m.totals;
    char b[1100];
    std::snprintf(b, sizeof(b),
                  "vr bars: %s - grabs left %zu right %zu, frames held with both hands %zu / one %zu, bar angle %+.1f .. %+.1f "
                  "deg, steering %+.2f .. %+.2f (the analogue device on %zu of %zu pad frame(s), left stick X byte 0x%02X .. "
                  "0x%02X); the heading turned %+.1f deg while the bars steered left, %+.1f deg while they steered right; "
                  "top speed held %.1f; throttle up to %.2f (twist up to %+.1f deg), punches right %zu left %zu, kicks %zu, "
                  "landed pulses %zu; grabs with the fist already clenched %zu, in the re-grab reach %zu, clenched fists "
                  "entering too fast %zu",
                  VrPrefs().bars.Describe().c_str(), t.grabs[0], t.grabs[1], t.framesBoth, t.framesOne, double(t.angleMin),
                  double(t.angleMax), double(t.steerMin), double(t.steerMax), t.barPadFrames, t.padFrames,
                  unsigned(t.barPadFrames == 0 ? 0x80 : t.lxMin), unsigned(t.barPadFrames == 0 ? 0x80 : t.lxMax),
                  t.turnedLeft, t.turnedRight, double(t.topSpeedHeld), double(t.throttleMax), double(t.twistMax), t.punches[1],
                  t.punches[0], t.kicks, t.landedPulses, t.clenchedGrabs, t.wideGrabs, t.fastEntries);
    // the held hands against the grips of the bike as drawn
    const Impl::HandsCheck& c = m.check;
    char h[640];
    std::snprintf(h, sizeof(h),
                  "\nvr hands on the bars: %zu held hand-frame(s) over %zu drawn frame(s) - the fist's centre to the drawn "
                  "grip max %.3f mm (mean %.3f, worst at frame %ld), the palm across the drawn bar max %.2f deg; the bike "
                  "leaning up to %.1f deg, its fork turned up to %.1f deg; the renderer's bike matrix against this frame's "
                  "read max %.3g, part slots differing on %zu frame(s); the original's roll up to %.1f deg drawn at %d%%, the "
                  "tracked hand to its drawn grip max %.0f mm (frame %ld), the eye's travel on the drawn bike while held "
                  "max %.0f mm",
                  c.handFrames, c.drawnFrames, c.maxMm, c.handFrames ? c.sumMm / double(c.handFrames) : 0.0, c.maxMmFrame,
                  c.maxDeg, c.maxLean, c.maxFork, c.maxModelDiff, c.partsDiffer, c.maxRoll, VrPrefs().bars.visualLean,
                  c.maxRealMm, c.maxRealFrame, c.maxEyeMm);
    return std::string(b) + h;
}

} // namespace rrgame
