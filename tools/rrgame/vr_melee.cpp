// VR physical combat (vr_melee.h). The contact logic follows GTA San Andreas VR (the gta-sa-vr-quest project,
// native\src\Melee.cpp): a fist / weapon segment swept between two frames, armed by a calm hand, a tracking jump never
// a blow; the targets and the blow's application are this game's (vr_melee.h).
#include "vr_melee.h"

#include "game/rider_pose.h"
#include "game_host_vr.h"
#include "render/gl_api.h"
#include "render/multiview.h"
#include "render/shaders.h"
#include "render/weapon_draw.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "vr_handlebars.h"
#include "vr_hands_draw.h"
#include "vr_holsters.h"     // the weapon hand, the swing's rules, the mock's holster actions
#include "vr_weapon_calib.h" // the weapon's collider along its calibrated grip
#include "vr_nunchaku.h" // the nunchaku's chain in the hand
#include "vr_settings.h"
#include "vr_snatch.h"   // a free hand takes a rival's weapon mid-swing

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rrgame {

using rr::render::gl;

namespace {

constexpr int kParts = 17;          // the rider model's sub-meshes (head_camera.h)
constexpr int kBikePart = 17;       // the bike's capsule ("Blows on the bike count")
constexpr float kModelPerWorld = 1024.0f;
constexpr float kKnuckles = 0.07f;  // metres from the grip pose to the knuckles along the aim
constexpr float kWeaponRadius = 0.05f;
constexpr float kGripFist = 0.5f;   // the grip button closes the hand into a fist
constexpr float kCalmFist = 0.48f, kCalmWeapon = 0.40f; // m/s against the seat: a calm hand arms (GTA SA VR)
constexpr double kRearm = 0.45;     // s after a blow the hand re-arms anyway (GTA SA VR)
constexpr double kTargetCooldown = 0.5; // s: one hand, one rider
constexpr float kJump = 0.65f;      // metres in one frame against the seat: a tracking jump (GTA SA VR)
constexpr float kApproach = 0.30f;  // the part of the speed that must go into the body
constexpr float kReach = 3.0f;      // metres: riders farther than this from the hand are not tested
constexpr float kScriptMove = 1.0f; // m/s: the mock script's hand to a blow's start and back (below any minimum)
constexpr float kArmReach = 0.85f;  // m from the eye point: as far as the mock's snatching hand goes

using V3 = std::array<float, 3>;
V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Norm(const V3& a) {
    const float l = Len(a);
    return l > 1e-6f ? Mul(a, 1.0f / l) : V3{0, 0, 0};
}
V3 Lerp(const V3& a, const V3& b, float t) { return Add(a, Mul(Sub(b, a), t)); }
V3 Make(const float* p) { return {p[0], p[1], p[2]}; }

// The closest points of segments p0-p1 and q0-q1: the parameters s, t in [0, 1] (Ericson, Real-Time Collision
// Detection 5.1.9).
float SegSeg(const V3& p0, const V3& p1, const V3& q0, const V3& q1, float& s, float& t) {
    const V3 d1 = Sub(p1, p0), d2 = Sub(q1, q0), r = Sub(p0, q0);
    const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    if (a <= 1e-12f && e <= 1e-12f) {
        s = t = 0.0f;
    } else if (a <= 1e-12f) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Dot(d1, r);
        if (e <= 1e-12f) {
            t = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Dot(d1, d2), denom = a * e - b * b;
            s = denom != 0.0f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return Len(Sub(Add(p0, Mul(d1, s)), Add(q0, Mul(d2, t))));
}

// Columns X, Y, Z (a proper rotation) -> quaternion x, y, z, w (as vr_handlebars.cpp).
void QuatFromAxes(const V3& X, const V3& Y, const V3& Z, float q[4]) {
    const float m00 = X[0], m10 = X[1], m20 = X[2], m01 = Y[0], m11 = Y[1], m21 = Y[2], m02 = Z[0], m12 = Z[1], m22 = Z[2];
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s;
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int k = 0; k < 4; ++k) q[k] = n > 1e-6f ? q[k] / n : (k == 3 ? 1.0f : 0.0f);
}

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
uint32_t U32(const uint8_t* ram, uint32_t a) { return static_cast<uint32_t>(S32(ram, a)); }
bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }

// ------------------------------------------------------------------------------------------------ the mock's script
// "<frame> <command> [args]" per line or ';'-separated (a file, or the text itself). Hands: left | right | both.
//   grip H V              the grip button (0..1): >= 0.5 is a fist
//   swing H PART SPEED    a straight blow: the hand from 45 cm on the player's side of the nearest rider's part PART
//                         (a number 0..16, or pelvis / torso / chest / head) through its centre to 20 cm past it, at
//                         SPEED m/s relative to the rider, the knuckles leading; it gets there at 1 m/s (below any
//                         blow's minimum speed) and goes back to its rest the same way
//   sweep H PART SPEED [D] a weapon's sweep: the hand D metres out from the part on the player's side (default 0.8 x
//                         the weapon's length) pointing at it, moved 1.2 m along the rider's heading at SPEED
//   offset DX DY DZ       the next blows' paths shifted by metres in the rider's right / up / forward (a miss)
//   target B              the next blows aim at bike B (default: the nearest rider within 3 m)
//   shot PATH             the eyes of this frame into PATH (the mock's shot)
//   shots PREFIX          from now on the eyes of every contact's frame into PREFIX_f<frame>_<blow | touch>.png
//   snatch H [miss] [N]   (vr_snatch.h) the hand (open) tracks the nearest rival's weapon object and
//                         closes on it when he swings it (his stance a strike, vr_snatch.h Swinging; `miss`: 45 cm
//                         above it - the control), holds 0.7 s, goes back; N attempts (default 3), 1 s apart
struct ScriptEvent {
    long frame = 0;
    std::string command, text;
    int hands = 0;
    int part = 2;
    float v[4] = {0, 0, 0, 0};
};

int PartByName(const std::string& p) {
    if (p == "pelvis") return 0;
    if (p == "torso") return 2;
    if (p == "chest") return 3;
    if (p == "head") return 4;
    char* end = nullptr;
    const long n = std::strtol(p.c_str(), &end, 10);
    if (p.empty() || end == nullptr || *end != '\0' || n < 0 || n >= kParts)
        throw std::runtime_error("vr melee script: '" + p + "' is not a rider part (0..16, pelvis, torso, chest, head)");
    return static_cast<int>(n);
}

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
            throw std::runtime_error("vr melee script: '" + h + "' is not left / right / both in: " + line);
        };
        std::string h, p;
        if (e.command == "grip") {
            w >> h;
            e.hands = hands(h);
            if (!(w >> e.v[0])) throw std::runtime_error("vr melee script: grip H VALUE: " + line);
        } else if (e.command == "swing" || e.command == "sweep") {
            w >> h >> p;
            e.hands = hands(h);
            if (e.hands == 3) throw std::runtime_error("vr melee script: one hand swings: " + line);
            e.part = PartByName(p);
            if (!(w >> e.v[0])) throw std::runtime_error("vr melee script: " + e.command + " H PART SPEED: " + line);
            if (!(w >> e.v[1])) e.v[1] = -1.0f;
        } else if (e.command == "snatch") {
            w >> h;
            e.hands = hands(h);
            if (e.hands == 3) throw std::runtime_error("vr melee script: one hand snatches: " + line);
            e.v[0] = 0.0f, e.v[1] = 3.0f;
            std::string x;
            while (w >> x) {
                if (x == "miss") e.v[0] = 1.0f;
                else e.v[1] = static_cast<float>(std::atoi(x.c_str()));
            }
            if (e.v[1] < 1.0f) throw std::runtime_error("vr melee script: snatch H [miss] [N >= 1]: " + line);
        } else if (e.command == "offset") {
            if (!(w >> e.v[0] >> e.v[1] >> e.v[2])) throw std::runtime_error("vr melee script: offset DX DY DZ: " + line);
        } else if (e.command == "target") {
            if (!(w >> e.v[0])) throw std::runtime_error("vr melee script: target BIKE: " + line);
        } else if (e.command == "shot" || e.command == "shots") {
            if (!(w >> e.text)) throw std::runtime_error("vr melee script: " + e.command + " PATH: " + line);
        } else {
            throw std::runtime_error("vr melee script: unknown command '" + e.command + "' in: " + line);
        }
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptEvent& a, const ScriptEvent& b) { return a.frame < b.frame; });
    return out;
}

// The debug lines (the colliders): position + colour, drawn over the scene.
const char* const kLineVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uViewProj;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";
const char* const kLineFs = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

} // namespace

struct VrMelee::Impl {
    // ---- the models: BBLEVEL1.GEO, the file the renderer draws every machine from (race_scene_rivals.cpp)
    bool loaded = false;
    rr::SkeletonTable skeleton;
    std::vector<rr::Model> models;
    struct PartCapsule {
        bool valid = false;
        float a[3] = {}, b[3] = {}; // part-local, model units
        float radius = 0.0f;        // model units
    };
    struct RiderModel {             // a 17-part rider model, its parts' capsules
        bool ok = false;
        rr::ModelGroup group;
        rr::Assembly assembly;
        PartCapsule parts[kParts];
    };
    std::map<uint32_t, RiderModel> riderModels;          // by model id (150 the player's, 159 ... the rivals'), on first use
    std::map<uint32_t, std::array<float, 3>> seats;      // bike model id -> SeatVertex 0x80066A84 at LOD 0
    const RiderModel* Rider(uint32_t id);
    const float* Seat(uint32_t id);
    float weaponLength[9] = {};     // world units, grip to tip, per weapon (model 800 group)
    // ---- the targets
    struct Capsule {
        V3 a{}, b{};
        float r = 0.0f;
        bool valid = false;
    };
    struct Target {
        uint32_t bike = 0, rider = 0;
        size_t index = 0;
        bool hittable = false;       // live, the rider on his bike
        V3 origin{}, right{}, up{}, fwd{};
        V3 prevOrigin{}, prevRight{}, prevUp{}, prevFwd{}; // last frame's frame (the body's own motion)
        Capsule now[kParts + 1], prev[kParts + 1];
        bool havePrev = false;
        bool weaponHeld = false;     // the weapon object in his hand, posed as drawn (vr_snatch.h)
        V3 weaponA{}, weaponB{};
        int weaponId = 9;
    };
    std::vector<Target> targets;
    uint32_t playerEntity = 0, playerRider = 0;
    int weapon = 9;                  // the player's weapon in hand, riderDef +0x2E
    int weaponHand = 1;              // the tracked hand that holds it (VrHandlebars::WeaponHand)
    // ---- the hands
    struct Hand {
        bool valid = false, grabbed = false;
        float grip = 0.0f, trigger = 0.0f;
        V3 pos{}, right{}, up{}, fwd{}, aim{}; // world
        V3 local{};                  // seat space, metres
        // the motion (GTA SA VR's Motion)
        bool havePrev = false, armed = false;
        V3 prevRoot{}, prevTip{}, prevLocal{};
        int prevKind = -1;           // 0 fist, 1 weapon, -1 none
        double lastStrike = -1.0;
        std::map<uint32_t, double> lastOn; // per rider: the time of this hand's last blow
        float pulse = 0.0f, pulseAmp = 0.0f;
        // this frame's collider (the draw)
        int kind = -1;
        V3 root{}, tip{};
        float radius = 0.0f;
    } hand[2];
    double clock = 0.0;              // seconds of hand time (the display period per frame when scripted)
    std::vector<rr::game::PhysicalBlowRequest> pending;
    VrSnatch snatch;
    VrNunchaku nunchaku;
    // ---- the mock's script
    bool scripted = false;
    std::vector<ScriptEvent> script;
    size_t nextEvent = 0;
    float scriptGrip[2] = {0, 0};
    V3 scriptOffset{};
    int scriptTarget = -1;
    std::string shotPrefix;         // the script's `shots`: a shot of every contact
    std::string shotWanted;         // this frame's contact shot (UpdateHands requests it)
    struct Swing {
        bool active = false, sweep = false;
        int phase = 0;               // 0 to the start, 1 the blow, 2 hold, 3 back, 4 home to the rest
        long phaseStart = 0;
        long phaseFrames = 1;
        size_t target = 0;
        int part = 2;
        float speed = 1.0f, dist = 0.5f;
        V3 offset{};
        V3 dir{};                    // (rider-relative) from the part toward the player, horizontal, rider axes
        V3 restRel{}, restDir{};     // the hand's rest at the blow's start, in the rider's frame (the way back)
        V3 aimAt{};                  // the part's centre at the start, in the rider's bike frame (world units)
    } swing[2];
    struct SnatchScript {            // `snatch H [miss] [N]`
        bool active = false, miss = false;
        int tries = 0, phase = 0;    // phase 0 tracking open, 1 closed (held), 2 back to the rest
        long phaseStart = 0;
        V3 local{};                  // the hand's last scripted place (seat space)
        bool havePlace = false;
    } snatchScript[2];
    // ---- the frame
    bool barsActive = false, physicalNow = false, headView = false, overrideSet = false, hapticsOn = false;
    bool anyHand = false;           // a tracked hand this frame (the Stick mode draws them, the rider's arms hidden)
    rr::xr::WorldAnchor anchor;
    float upm = 1.0f;
    long frame = 0;
    std::chrono::steady_clock::time_point last{};
    struct Contact {
        uint32_t bike = 0;          // the rider's bike, and the point in its frame (it is drawn riding with him)
        V3 local{};
        float ttl = 0.0f;
        int hand = -1;
    } contact;
    // ---- drawing
    VrHandsDraw hands;
    GLuint program = 0, vao = 0, vbo = 0;
    GLint viewProjLoc = -1;
    std::vector<float> lines;
    // ---- the log
    struct Totals {
        size_t trackedFrames = 0, hits = 0, fistHits = 0, weaponHits = 0, slow = 0, grazing = 0, cooling = 0, unarmed = 0;
        size_t heldBars = 0, jumps = 0, sent = 0, posed = 0, swings = 0, swingsSkipped = 0;
        float fastest = 0.0f;
        std::string hitList;
    } totals;

    // world <-> the seat's space (metres; x right, y up, z toward the rider)
    V3 ToLocal(const V3& w) const {
        const V3 r = Sub(w, Make(anchor.origin));
        return {Dot(r, Make(anchor.right)) / upm, Dot(r, Make(anchor.up)) / upm, -Dot(r, Make(anchor.ahead)) / upm};
    }
    V3 DirToLocal(const V3& d) const { return {Dot(d, Make(anchor.right)), Dot(d, Make(anchor.up)), -Dot(d, Make(anchor.ahead))}; }
    V3 FromLocal(const V3& l) const {
        V3 w = Make(anchor.origin);
        for (int k = 0; k < 3; ++k) w[k] += (anchor.right[k] * l[0] + anchor.up[k] * l[1] - anchor.ahead[k] * l[2]) * upm;
        return w;
    }
    bool ComputeTarget(const uint8_t* ram, Target& t);
    void RunScript(VrHost& vr, long f);
    bool ScriptHands(long f, rr::xr::HandPose out[2], float dt);
    void Detect(int h, float dt);
    void WireCapsule(const V3& a, const V3& b, float r, const V3& rgb);
    void Line(const V3& a, const V3& b, const V3& rgb) {
        lines.insert(lines.end(), {a[0], a[1], a[2], rgb[0], rgb[1], rgb[2], b[0], b[1], b[2], rgb[0], rgb[1], rgb[2]});
    }
};

VrMelee::VrMelee() : impl_(std::make_unique<Impl>()) {}
VrMelee::~VrMelee() = default;

bool VrMelee::Load(const rr::DiscImage& disc, bool mock) {
    Impl& m = *impl_;
    m.loaded = false;
    try {
        const auto geo = disc.Find("DATA/BBLEVEL1.GEO");
        const auto overlay = disc.Find("RASHCDG.BIN");
        if (!geo || !overlay) throw std::runtime_error("DATA/BBLEVEL1.GEO or RASHCDG.BIN is missing");
        m.models = rr::ParseGeo(disc.ReadFile(*geo));
        m.skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
        const rr::Model* weapons = nullptr;
        for (const rr::Model& model : m.models)
            if (model.id == 800) weapons = &model;
        if (m.Rider(150) == nullptr || m.Seat(100) == nullptr) throw std::runtime_error("models 100 / 150 not usable");
        m.snatch.Load(m.models, m.skeleton);
        m.nunchaku.Load(m.models, m.skeleton);
        // the weapons' lengths: the farthest vertex of the rest-posed group from its grip (weapon_draw.cpp HandModel's
        // long axis)
        if (weapons != nullptr)
            for (size_t w = 0; w < weapons->groups.size() && w < 9; ++w) {
                const rr::ModelGroup& g = weapons->groups[w];
                const rr::Assembly as = rr::AssembleGroup(g, m.skeleton);
                std::vector<rr::PartMatrix> rest(g.subMeshes.size());
                const rr::PosedGroup posed = rr::PoseGroup(g, m.skeleton, as, std::span<const rr::PartMatrix>(rest));
                const rr::TriangleSoup soup = rr::BuildPosedTriangleSoup(g, posed);
                float far2 = 0.0f;
                for (const auto& v : soup.vertices) far2 = std::max(far2, v.x * v.x + v.y * v.y + v.z * v.z);
                m.weaponLength[w] = std::sqrt(far2) / kModelPerWorld;
            }
        m.loaded = true;
    } catch (const std::exception& e) {
        std::printf("vr melee: NOT loaded - %s (no physical blows)\n", e.what());
    }
    const std::string& arg = MeleeScriptArgument();
    if (!arg.empty()) {
        if (!mock) {
            std::printf("vr melee: --vr-melee-script ignored (only the desktop VR mock takes scripted controllers)\n");
        } else {
            m.script = ParseScript(arg);
            m.scripted = true;
            std::printf("vr melee: the mock's hands from a script, %zu event(s)\n", m.script.size());
        }
    }
    if (m.loaded) {
        char b[400];
        std::snprintf(b, sizeof(b), "vr melee: %s; rider model 150 part capsules (radius, length, mm):", VrPrefs().melee.Describe().c_str());
        std::string line = b;
        const Impl::RiderModel* rm = m.Rider(150);
        for (int p = 0; p < kParts; ++p) {
            const Impl::PartCapsule& c = rm->parts[p];
            const float len = std::sqrt((c.b[0] - c.a[0]) * (c.b[0] - c.a[0]) + (c.b[1] - c.a[1]) * (c.b[1] - c.a[1]) +
                                        (c.b[2] - c.a[2]) * (c.b[2] - c.a[2]));
            std::snprintf(b, sizeof(b), " %d:%.0f/%.0f", p, double(c.radius), double(len));
            line += b;
        }
        line += "; weapon lengths (m):";
        for (int w = 0; w < 9; ++w) {
            std::snprintf(b, sizeof(b), " %d:%.2f", w, double(m.weaponLength[w]));
            line += b;
        }
        std::printf("%s\n", line.c_str());
    }
    return m.loaded;
}

// ------------------------------------------------------------------------------------------------ the targets
// The model an object shows: its registration *(obj + 0x60), whose first word is the model id (race_render.cpp
// ObjectLook, ModelBind); 0 when there is none.
static uint32_t ObjectModel(const uint8_t* ram, uint32_t obj) {
    if (!InRam(obj)) return 0;
    const uint32_t reg = U32(ram, obj + 0x60u);
    return InRam(reg) ? U32(ram, reg) : 0u;
}

const VrMelee::Impl::RiderModel* VrMelee::Impl::Rider(uint32_t id) {
    auto it = riderModels.find(id);
    if (it == riderModels.end()) {
        RiderModel r;
        for (const rr::Model& model : models) {
            if (model.id != id || model.groups.empty() || model.groups.front().subMeshes.size() != kParts) continue;
            r.group = model.groups.front();
            r.assembly = rr::AssembleGroup(r.group, skeleton);
            r.ok = r.assembly.assembled;
        }
        // each part's capsule from its own vertex extent (part-local model units): its long axis, the mean half-width
        // of the other two as the radius
        const float f = r.ok ? static_cast<float>(rr::LodFactor(r.group)) : 0.0f;
        for (int p = 0; p < kParts && r.ok; ++p) {
            const rr::SubMesh& sm = r.group.subMeshes[static_cast<size_t>(p)];
            if (sm.vertCount == 0 || sm.vertBase + sm.vertCount > r.group.verts.size()) continue;
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            for (uint32_t i = 0; i < sm.vertCount; ++i) {
                const rr::SVector& v = r.group.verts[sm.vertBase + i];
                const float q[3] = {v.x * f, v.y * f, v.z * f};
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], q[k]);
                    hi[k] = std::max(hi[k], q[k]);
                }
            }
            int axis = 0;
            for (int k = 1; k < 3; ++k)
                if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;
            const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
            PartCapsule& c = r.parts[p];
            c.radius = std::max(30.0f, 0.25f * ((hi[o1] - lo[o1]) + (hi[o2] - lo[o2])));
            const float half = std::max(0.0f, 0.5f * (hi[axis] - lo[axis]) - c.radius);
            for (int k = 0; k < 3; ++k) c.a[k] = c.b[k] = 0.5f * (lo[k] + hi[k]);
            c.a[axis] -= half;
            c.b[axis] += half;
            c.valid = true;
        }
        it = riderModels.emplace(id, r).first;
    }
    return it->second.ok ? &it->second : nullptr;
}

const float* VrMelee::Impl::Seat(uint32_t id) {
    auto it = seats.find(id);
    if (it == seats.end()) {
        std::array<float, 3> seat{};
        bool ok = false;
        for (const rr::Model& model : models) { // SeatVertex 0x80066A84 at LOD 0 (race_scene.cpp LoadMachine's rule)
            if (model.id != id || model.groups.empty() || model.groups.front().subMeshes.empty()) continue;
            const rr::ModelGroup& bg = model.groups.front();
            const size_t at = bg.subMeshes.front().vertBase + (bg.subMeshes.size() < 6 ? 3u : 4u);
            if (at >= bg.verts.size()) continue;
            const float bf = static_cast<float>(rr::LodFactor(bg));
            seat = {bg.verts[at].x * bf, bg.verts[at].y * bf, bg.verts[at].z * bf};
            ok = true;
        }
        if (!ok) return nullptr; // (not cached: a later lookup finds nothing either, and it is cheap)
        it = seats.emplace(id, seat).first;
    }
    return it->second.data();
}

bool VrMelee::Impl::ComputeTarget(const uint8_t* ram, Target& t) {
    rr::game::RiderPoseView pose;
    if (!InRam(t.bike) || !InRam(t.rider) || !rr::game::ReadRiderPose(ram, t.rider, pose)) return false;
    // the rider's and the bike's own models, as the renderer draws them (race_render.cpp: ObjectLook, RiderAttachFor)
    const RiderModel* rm = Rider(ObjectModel(ram, t.rider));
    if (rm == nullptr) rm = Rider(150);
    const float* seat = Seat(ObjectModel(ram, t.bike));
    if (seat == nullptr) seat = Seat(100);
    if (rm == nullptr || seat == nullptr) return false;
    const PartCapsule* parts = rm->parts;
    float axis[3][3];
    for (uint32_t c = 0; c < 3; ++c) {
        float l2 = 0.0f;
        for (uint32_t k = 0; k < 3; ++k) {
            axis[c][k] = static_cast<float>(S16(ram, t.bike + 0x1B0u + 6u * c + 2u * k)) / 4096.0f;
            l2 += axis[c][k] * axis[c][k];
        }
        if (l2 < 0.25f) return false;
    }
    double origin[3];
    for (uint32_t k = 0; k < 3; ++k) origin[k] = static_cast<double>(S32(ram, t.bike + 0xB8u + 4u * k)) / 65536.0;
    for (int k = 0; k < 3; ++k) {
        t.origin[k] = static_cast<float>(origin[k]);
        t.fwd[k] = axis[2][k];
        t.up[k] = -axis[1][k];
        t.right[k] = axis[0][k];
    }
    t.fwd = Norm(t.fwd);
    t.up = Norm(t.up);
    t.right = Norm(Cross(t.fwd, t.up));
    const rr::PosedGroup posed =
        rr::PoseGroup(rm->group, skeleton, rm->assembly, std::span<const rr::PartMatrix>(pose.local, rm->group.subMeshes.size()));
    const double base[3] = {seat[0] + static_cast<double>(pose.root[0]), seat[1] + static_cast<double>(pose.root[1]),
                            seat[2] + static_cast<double>(pose.root[2])};
    const auto toWorld = [&](const float local[3], size_t part) -> V3 { // part-local model units -> world
        double m[3];
        for (int r = 0; r < 3; ++r) {
            double s = 0.0;
            for (int c = 0; c < 3; ++c) s += static_cast<double>(posed.world[part].m[r * 3 + c]) * local[c];
            m[r] = s / 4096.0 + posed.origin[part][static_cast<size_t>(r)];
        }
        V3 w{};
        for (int k = 0; k < 3; ++k) {
            double v = origin[k];
            for (int c = 0; c < 3; ++c) v += axis[c][k] * (base[c] + m[c]) / kModelPerWorld;
            w[k] = static_cast<float>(v);
        }
        return w;
    };
    for (int p = 0; p < kParts; ++p) {
        Capsule& c = t.now[p];
        c.valid = parts[p].valid;
        if (!c.valid) continue;
        c.a = toWorld(parts[p].a, static_cast<size_t>(p));
        c.b = toWorld(parts[p].b, static_cast<size_t>(p));
        c.r = parts[p].radius / kModelPerWorld;
    }
    {   // the weapon object in his hand, posed as the renderer draws it (vr_snatch.h)
        float wa[3], wb[3];
        t.weaponHeld = snatch.HeldWeapon(ram, t.rider,
                                         [&](const float* l, size_t part, float* out) {
                                             const V3 v = toWorld(l, part);
                                             out[0] = v[0], out[1] = v[1], out[2] = v[2];
                                         },
                                         wa, wb, t.weaponId);
        if (t.weaponHeld) t.weaponA = Make(wa), t.weaponB = Make(wb);
    }
    // the bike (OURS, approximate: a capsule along its heading at the tank, 1.7 x 0.56 world units)
    Capsule& bk = t.now[kBikePart];
    bk.valid = true;
    const V3 centre = Add(t.origin, Mul(t.up, 0.05f));
    bk.a = Sub(centre, Mul(t.fwd, 0.85f));
    bk.b = Add(centre, Mul(t.fwd, 0.85f));
    bk.r = 0.28f;
    return true;
}

void VrMelee::UpdateTargets(const rr::game::RaceSession& s) {
    Impl& m = *impl_;
    if (!m.loaded) return;
    const uint8_t* ram = s.ArenaRam();
    const auto& bikes = s.Bikes();
    if (bikes.empty() || ram == nullptr) return;
    m.playerEntity = bikes[0].entityAddress;
    m.playerRider = bikes[0].ownerAddress;
    const uint32_t rd = U32(ram, m.playerEntity + 0x43Cu);
    m.weapon = InRam(rd) ? ram[(rd + 0x2Eu) & 0x1FFFFFu] : 9;
    std::vector<Impl::Target> next;
    const size_t players = static_cast<size_t>(std::max(1, s.Players()));
    for (size_t i = players; i < bikes.size(); ++i) {
        Impl::Target t;
        t.bike = bikes[i].entityAddress;
        t.rider = bikes[i].ownerAddress;
        t.index = i;
        if (!m.ComputeTarget(ram, t)) continue;
        t.hittable = S16(ram, t.bike + 0x140u) != 0 && U32(ram, t.rider + 0x25Cu) < 2u;
        for (const Impl::Target& old : m.targets)
            if (old.bike == t.bike) {
                std::copy(std::begin(old.now), std::end(old.now), std::begin(t.prev));
                t.prevOrigin = old.origin, t.prevRight = old.right, t.prevUp = old.up, t.prevFwd = old.fwd;
                t.havePrev = true;
            }
        if (!t.havePrev) {
            std::copy(std::begin(t.now), std::end(t.now), std::begin(t.prev));
            t.prevOrigin = t.origin, t.prevRight = t.right, t.prevUp = t.up, t.prevFwd = t.fwd;
        }
        next.push_back(t);
    }
    m.targets.swap(next);
    m.snatch.BeginRivals();
    for (const Impl::Target& t : m.targets)
        m.snatch.Rival(t.bike, t.index, t.hittable, t.weaponHeld, t.weaponA.data(), t.weaponB.data(), t.weaponId,
                       t.origin.data(), t.right.data(), t.up.data(), t.fwd.data(), VrSnatch::Swinging(ram, t.bike));
    m.snatch.EndRivals(m.weapon);
    ++m.totals.posed;
    static const bool trace = std::getenv("RRJB_MELEE_TRACE") != nullptr; // DEVELOPMENT: the riders near the player
    if (trace && m.totals.posed % 50 == 0) {
        V3 me{};
        for (uint32_t k = 0; k < 3; ++k)
            me[k] = static_cast<float>(static_cast<double>(S32(ram, m.playerEntity + 0xB8u + 4u * k)) / 65536.0);
        std::string line;
        for (const Impl::Target& t : m.targets) {
            const float d = Len(Sub(t.origin, me));
            if (d > 30.0f) continue;
            char b[64];
            std::snprintf(b, sizeof(b), " b%zu %.1f%s", t.index, double(d), t.hittable ? "" : "(not live)");
            line += b;
        }
        std::printf("vr melee trace: update %zu riders within 30 units:%s\n", m.totals.posed, line.c_str());
    }
}

// ------------------------------------------------------------------------------------------------ the script
void VrMelee::Impl::RunScript(VrHost& vr, long f) {
    while (nextEvent < script.size() && script[nextEvent].frame <= f) {
        const ScriptEvent& e = script[nextEvent++];
        if (e.command == "grip") {
            for (int h = 0; h < 2; ++h)
                if (e.hands & (1 << h)) scriptGrip[h] = std::clamp(e.v[0], 0.0f, 1.0f);
        } else if (e.command == "offset") {
            scriptOffset = {e.v[0], e.v[1], e.v[2]};
        } else if (e.command == "target") {
            scriptTarget = static_cast<int>(e.v[0]);
        } else if (e.command == "shot") {
            vr.RequestShot(e.text);
        } else if (e.command == "shots") {
            shotPrefix = e.text;
        } else if (e.command == "snatch") {
            SnatchScript& ss = snatchScript[e.hands == 1 ? 0 : 1];
            ss = SnatchScript{};
            ss.active = true;
            ss.miss = e.v[0] > 0.5f;
            ss.tries = static_cast<int>(e.v[1]);
            ss.phaseStart = f;
            std::printf("vr melee script: frame %ld snatch %s%s, %d attempt(s)\n", f, e.hands == 1 ? "left" : "right",
                        ss.miss ? " miss (closing 45 cm above the weapon - the control)" : "", ss.tries);
        } else { // swing / sweep: the nearest rider (or the named one) within 3 m of the eye
            const int h = e.hands == 1 ? 0 : 1;
            Swing& sw = swing[h];
            if (sw.active) { // that hand's last blow is still on its way back: this one waits for it
                --nextEvent;
                break;
            }
            const V3 eye = Make(anchor.origin);
            int best = -1;
            float bestD = kReach * upm, nearest = 1e9f;
            size_t nearestIndex = 0;
            for (size_t i = 0; i < targets.size(); ++i) {
                const Target& t = targets[i];
                if (!t.now[e.part].valid) continue;
                const V3 c = Lerp(t.now[e.part].a, t.now[e.part].b, 0.5f);
                const float d = Len(Sub(c, eye));
                if (t.hittable && d < nearest) nearest = d, nearestIndex = t.index;
                if (scriptTarget >= 0 ? t.index == static_cast<size_t>(scriptTarget) : (t.hittable && d < bestD)) {
                    best = static_cast<int>(i);
                    bestD = d;
                }
            }
            if (best < 0) {
                ++totals.swingsSkipped;
                std::printf("vr melee script: frame %ld %s %s - no rider within reach (the nearest, b%zu, %.2f m), "
                            "skipped\n",
                            f, e.command.c_str(), h ? "right" : "left", nearestIndex, double(nearest / upm));
                continue;
            }
            const Target& t = targets[static_cast<size_t>(best)];
            sw = Swing{};
            sw.active = true;
            sw.sweep = e.command == "sweep";
            sw.phase = 0;
            sw.phaseStart = f;
            sw.target = t.index;
            sw.part = e.part;
            sw.speed = std::max(0.05f, e.v[0]);
            const bool armedHand = weapon < 9 && h == weaponHand;
            sw.dist = e.v[1] > 0.0f ? e.v[1] : (armedHand ? 0.8f * weaponLength[std::clamp(weapon, 0, 8)] / upm : 0.5f);
            sw.offset = scriptOffset;
            // from the part toward the player, in the rider's own (right, up, fwd) terms, levelled
            const V3 c = Lerp(t.now[e.part].a, t.now[e.part].b, 0.5f);
            const V3 toMe = Sub(eye, c);
            sw.dir = Norm(V3{Dot(toMe, t.right), 0.0f, Dot(toMe, t.fwd)});
            const V3 fromBike = Sub(c, t.origin);
            sw.aimAt = {Dot(fromBike, t.right), Dot(fromBike, t.up), Dot(fromBike, t.fwd)};
            ++totals.swings;
            std::printf("vr melee script: frame %ld %s %s part %d at %.1f m/s on rival b%zu (%.2f m from the eye)%s\n", f,
                        e.command.c_str(), h ? "right" : "left", e.part, double(sw.speed), t.index, double(bestD / upm),
                        Len(sw.offset) > 0.0f ? " (offset)" : "");
        }
    }
}

// The scripted hands as controller poses in the seat's space: at rest beside the body, or on a blow's path - a world
// path relative to the target rider's part (it rides with him), taken back into the seat's space so it goes through
// the same placement as a headset's hands.
bool VrMelee::Impl::ScriptHands(long f, rr::xr::HandPose out[2], float dt) {
    for (int h = 0; h < 2; ++h) {
        out[h] = rr::xr::HandPose{};
        V3 local{h ? 0.34f : -0.34f, -0.30f, -0.15f}; // the rest: beside the body (clear of the tank), below the eye
        V3 fwdLocal{0.0f, 0.0f, -1.0f};
        Swing& sw = swing[h];
        SnatchScript& ss = snatchScript[h];
        if (ss.active && !sw.active) { // the hand after the nearest rival's weapon, open, closing on it
            const float frameTime = dt > 0.0f ? dt : 1.0f / 72.0f;
            const long inPhase = f - ss.phaseStart;
            const V3 rest = local;
            if (!ss.havePlace) ss.local = rest, ss.havePlace = true;
            const VrSnatch::RivalWeapon* rw = snatch.Nearest(anchor.origin, 2.5f * upm);
            V3 goal = rest;
            if (rw != nullptr && ss.phase < 2) { // 55 % out along his weapon, 4 cm toward the player
                const V3 a = Make(rw->a), b = Make(rw->b);
                const V3 p = Lerp(a, b, 0.55f);
                goal = Add(ToLocal(p), Mul(Norm(Mul(ToLocal(p), -1.0f)), 0.04f));
                if (ss.miss) goal = Add(goal, V3{0.0f, 0.45f, 0.0f}); // the control: 45 cm above it
                if (Len(goal) > kArmReach) goal = Mul(Norm(goal), kArmReach); // an arm's reach: beyond it, it waits there
            }
            const float maxStep = (ss.phase == 1 ? 6.0f : 3.0f) * frameTime; // m per frame: a hand that keeps up with it
            const V3 d = Sub(goal, ss.local);
            ss.local = Len(d) <= maxStep ? goal : Add(ss.local, Mul(Norm(d), maxStep));
            local = ss.local;
            if (rw != nullptr) fwdLocal = Norm(Sub(ToLocal(Lerp(Make(rw->a), Make(rw->b), 0.55f)), ss.local));
            const bool there = rw != nullptr && Len(Sub(goal, ss.local)) < 0.06f;
            if (ss.phase == 0) {
                scriptGrip[h] = 0.0f;
                if (there && inPhase > 12 && rw->swinging) {
                    ss.phase = 1, ss.phaseStart = f;
                    scriptGrip[h] = 1.0f;
                    std::printf("vr melee script: frame %ld snatch %s closes on rival b%zu's weapon %d (%s; its tip %.1f m/s "
                                "against his bike)\n",
                                f, h ? "right" : "left", rw->index, rw->weapon,
                                rw->swinging ? "he swings it" : "he holds it, not swinging", double(rw->speed / upm));
                }
            } else if (ss.phase == 1) {
                scriptGrip[h] = 1.0f;
                if (inPhase >= static_cast<long>(0.7f / frameTime)) ss.phase = 2, ss.phaseStart = f;
            } else {
                scriptGrip[h] = 0.0f;
                if (inPhase >= static_cast<long>(1.0f / frameTime)) {
                    ss.phase = 0, ss.phaseStart = f;
                    if (--ss.tries <= 0) ss.active = false;
                }
            }
        } else if (!ss.active) {
            ss.havePlace = false;
        }
        const Target* t = nullptr;
        for (const Target& x : targets)
            if (sw.active && x.index == sw.target) t = &x;
        if (sw.active && (t == nullptr || !t->hittable)) sw.active = false; // the rider went away (or down)
        if (sw.active) {
            // everything in the rider's own frame round the part's centre (metres: right, up, forward), so the hand
            // rides with him: the approach, the blow and the way back are relative to the target, as a player's arm is
            // the aim point rides with the rider's bike (his limbs' own animation does not drag the hand about)
            const V3 c = Add(t->origin, Add(Add(Mul(t->right, sw.aimAt[0]), Mul(t->up, sw.aimAt[1])), Mul(t->fwd, sw.aimAt[2])));
            const auto fromRel = [&](const V3& r) {
                return Add(c, Mul(Add(Add(Mul(t->right, r[0]), Mul(t->up, r[1])), Mul(t->fwd, r[2])), upm));
            };
            const auto toRel = [&](const V3& w) {
                const V3 d = Sub(w, c);
                return V3{Dot(d, t->right) / upm, Dot(d, t->up) / upm, Dot(d, t->fwd) / upm};
            };
            const auto dirToRel = [&](const V3& d) { return V3{Dot(d, t->right), Dot(d, t->up), Dot(d, t->fwd)}; };
            const auto dirFromRel = [&](const V3& r) { return Add(Add(Mul(t->right, r[0]), Mul(t->up, r[1])), Mul(t->fwd, r[2])); };
            const V3 blowDir = Mul(sw.dir, -1.0f); // the knuckles / the weapon point at the part
            // the blow's path: s = 0 .. 1
            const auto path = [&](float s) -> V3 {
                if (sw.sweep) return Add(sw.offset, Add(Mul(sw.dir, sw.dist), V3{0.0f, 0.0f, 0.6f - 1.2f * s}));
                return Add(sw.offset, Mul(sw.dir, 0.45f - 0.65f * s));
            };
            const float length = sw.sweep ? 1.2f : 0.65f;
            const float frameTime = dt > 0.0f ? dt : 1.0f / 72.0f;
            const V3 rest = FromLocal(local);
            const V3 restDirW = Norm(Make(anchor.ahead)); // the rest: the fist forward (the seat's -z)
            const auto moveFrames = [&](const V3& a, const V3& b) {
                return std::max(24L, static_cast<long>(std::ceil(Len(Sub(a, b)) / kScriptMove / frameTime)));
            };
            V3 rel{}, dir = blowDir;
            long inPhase = f - sw.phaseStart;
            if (sw.phase == 0 && inPhase == 0) { // where the hand rests, in the rider's frame now
                sw.restRel = toRel(rest);
                sw.restDir = dirToRel(restDirW);
                sw.phaseFrames = moveFrames(sw.restRel, path(0.0f));
            }
            const auto frac = [&]() { return std::min(1.0f, static_cast<float>(inPhase) / static_cast<float>(sw.phaseFrames)); };
            const auto next = [&](int phase, long frames) {
                sw.phase = phase;
                sw.phaseStart = f;
                sw.phaseFrames = frames;
            };
            V3 pos{};
            if (sw.phase == 0) { // to the start at kScriptMove, turning the hand onto the blow
                rel = Lerp(sw.restRel, path(0.0f), frac());
                dir = Norm(Lerp(sw.restDir, blowDir, frac()));
                if (inPhase >= sw.phaseFrames) next(1, std::max(1L, static_cast<long>(std::lround(length / sw.speed / frameTime))));
                pos = fromRel(rel);
            } else if (sw.phase == 1) { // the blow
                rel = path(frac());
                if (inPhase >= sw.phaseFrames) next(2, 12);
                pos = fromRel(rel);
            } else if (sw.phase == 2) { // hold
                rel = path(1.0f);
                if (inPhase >= sw.phaseFrames) next(3, moveFrames(path(1.0f), sw.restRel));
                pos = fromRel(rel);
            } else if (sw.phase == 3) { // back the same way at kScriptMove, the hand turning back
                rel = Lerp(path(1.0f), sw.restRel, frac());
                dir = Norm(Lerp(blowDir, sw.restDir, frac()));
                if (inPhase >= sw.phaseFrames) next(4, 24);
                pos = fromRel(rel);
            } else { // home: from there to the rest beside the body (both where they are this frame)
                pos = Lerp(fromRel(sw.restRel), rest, frac());
                dir = sw.restDir;
                if (inPhase >= sw.phaseFrames) sw.active = false;
            }
            local = ToLocal(pos);
            static const bool trace = std::getenv("RRJB_MELEE_TRACE") != nullptr; // DEVELOPMENT: the scripted path
            if (trace) {
                const V3 fromBike = Sub(pos, t->origin);
                std::printf("vr melee trace: f%ld %s phase %d rel %+.3f %+.3f %+.3f (bike frame %+.3f %+.3f %+.3f)\n", f,
                            h ? "R" : "L", sw.phase, double(rel[0]), double(rel[1]), double(rel[2]),
                            double(Dot(fromBike, t->right)), double(Dot(fromBike, t->up)), double(Dot(fromBike, t->fwd)));
            }
            fwdLocal = DirToLocal(Norm(dirFromRel(dir)));
        }
        // the grip frame: -Z = forward, X = the palm's side (the seat's up x forward), Y = Z x X
        const V3 Z = Norm(Mul(fwdLocal, -1.0f));
        V3 X = Norm(Cross(V3{0.0f, 1.0f, 0.0f}, Z));
        if (Len(X) < 0.5f) X = {1.0f, 0.0f, 0.0f};
        const V3 Y = Cross(Z, X);
        float q[4];
        QuatFromAxes(X, Y, Z, q);
        out[h].gripValid = out[h].aimValid = true;
        for (int k = 0; k < 3; ++k) out[h].grip[k] = out[h].aim[k] = local[k];
        for (int k = 0; k < 4; ++k) out[h].grip[3 + k] = out[h].aim[3 + k] = q[k];
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ the sweep
// Hand h's collider this frame against every rider near it, swept from last frame's positions (vr_melee.h).
void VrMelee::Impl::Detect(int h, float dt) {
    Hand& hd = hand[h];
    const MeleeSettings& s = VrPrefs().melee;
    hd.kind = -1;
    if (!hd.valid) {
        hd.havePrev = hd.armed = false;
        return;
    }
    const bool armedWith = weapon >= 0 && weapon <= 8 && h == weaponHand;
    float chainRoot[3], chainTip[3];
    const bool chainEnd = armedWith && nunchaku.Collider(chainRoot, chainTip);
    if (chainEnd) { // the nunchaku's swinging end strikes
        hd.kind = 1;
        hd.root = Make(chainRoot);
        hd.tip = Make(chainTip);
        hd.radius = kWeaponRadius;
    } else if (armedWith) { // the weapon: from the grip along the grip's forward, the model's own length
        hd.kind = 1;
        hd.root = hd.pos;
        hd.tip = Add(hd.pos, Mul(hd.fwd, weaponLength[weapon] * static_cast<float>(s.weaponPct) / 100.0f));
        hd.radius = kWeaponRadius;
        HandFrameW hf; // the drawn weapon's own axis, as its calibrated grip holds it (vr_weapon_calib.h)
        hf.valid = true;
        hf.left = h == 0; // the mirrored grip in the left hand
        for (int k = 0; k < 3; ++k)
            hf.pos[k] = hd.pos[k], hf.right[k] = hd.right[k], hf.up[k] = hd.up[k], hf.fwd[k] = hd.fwd[k], hf.aim[k] = hd.aim[k];
        WeaponInHandSegment(weapon, hf, upm, VrPrefs().weapons.grip[weapon], static_cast<float>(s.weaponPct), hd.root.data(),
                            hd.tip.data());
    } else { // the fist: the grip pose to the knuckles
        hd.kind = 0;
        hd.root = hd.pos;
        hd.tip = Add(hd.pos, Mul(hd.aim, kKnuckles * upm));
        hd.radius = static_cast<float>(s.fistCm) / 100.0f * upm;
    }
    const bool holdsRival = snatch.Holding(h); // a hand closed on a rival's weapon is not a fist
    const bool live = !hd.grabbed && (hd.kind == 1 || hd.grip >= kGripFist) && !holdsRival;
    const auto keep = [&]() {
        hd.prevRoot = hd.root, hd.prevTip = hd.tip, hd.prevLocal = hd.local, hd.prevKind = hd.kind;
    };
    const V3 tipSeat = ToLocal(hd.tip); // the swing's travel (vr_holsters.h SwingTrack)
    if (!hd.havePrev || hd.prevKind != hd.kind || dt <= 0.0f) {
        SwingTrack(h, hd.kind, tipSeat.data(), dt, true);
        hd.havePrev = true;
        hd.armed = false;
        keep();
        return;
    }
    const float seatStep = Len(Sub(hd.local, hd.prevLocal));
    SwingTrack(h, hd.kind, tipSeat.data(), dt, seatStep > kJump);
    if (seatStep > kJump) { // recentre / tracking lost and found: never a blow (GTA SA VR)
        ++totals.jumps;
        std::printf("vr melee: frame %ld - the %s hand jumped %.2f m in one frame (tracking): disarmed, no blow\n", frame,
                    h ? "right" : "left", double(seatStep));
        hd.armed = false;
        keep();
        return;
    }
    if (seatStep / dt <= (hd.kind == 1 ? kCalmWeapon : kCalmFist)) hd.armed = true;
    if (!hd.armed && hd.lastStrike >= 0.0 && clock - hd.lastStrike >= kRearm) hd.armed = true;
    // the earliest ENTRY of the swept collider into any capsule of any rider near it
    struct Best {
        float s = 2.0f, u = 0.0f, w = 0.0f;
        const Target* t = nullptr;
        int part = -1;
        V3 A{}, B{}, CA{}, CB{};
    } best;
    const V3 r0 = hd.prevRoot, r1 = hd.root, t0 = hd.prevTip, t1 = hd.tip;
    for (const Target& t : targets) {
        if (!t.hittable || Len(Sub(r1, t.origin)) > kReach * upm) continue;
        for (int p = 0; p <= kBikePart; ++p) {
            if (p == kBikePart && !s.bikes) continue;
            const Capsule& cn = t.now[p];
            const Capsule& cp = t.prev[p];
            if (!cn.valid || !cp.valid) continue;
            const float rr = hd.radius + cn.r;
            float u = 0.0f, w = 0.0f;
            if (SegSeg(r0, t0, cp.a, cp.b, u, w) < rr) continue; // already inside: not an entry
            const float travel = std::max(Len(Sub(Sub(r1, r0), Sub(cn.a, cp.a))), Len(Sub(Sub(t1, t0), Sub(cn.b, cp.b))));
            const int steps = std::clamp(static_cast<int>(std::ceil(travel / (0.02f * upm))), 1, 24);
            for (int j = 1; j <= steps; ++j) {
                const float f = static_cast<float>(j) / static_cast<float>(steps);
                if (f >= best.s) break;
                const V3 A = Lerp(r0, r1, f), B = Lerp(t0, t1, f), CA = Lerp(cp.a, cn.a, f), CB = Lerp(cp.b, cn.b, f);
                if (SegSeg(A, B, CA, CB, u, w) < rr) {
                    best.s = f, best.u = u, best.w = w, best.t = &t, best.part = p;
                    best.A = A, best.B = B, best.CA = CA, best.CB = CB;
                    break;
                }
            }
        }
    }
    if (best.t != nullptr) {
        const Target& t = *best.t;
        // the speed of the touching point of the collider against the RIDER'S BODY FRAME (his bike's motion, m/s): both
        // ride at racing speed, and his own limbs swinging into a still hand are his motion, not the player's blow
        const V3 dr = Sub(r1, r0), dtp = Sub(t1, t0);
        const V3 handPoint = Lerp(best.A, best.B, best.u), bodyPoint = Lerp(best.CA, best.CB, best.w);
        // the body frame's own step at the touching point: the point fixed in the rider's frame, where it was last frame
        // (his bike's travel and turn; not his limbs' animation)
        const V3 inFrame = Sub(bodyPoint, t.origin);
        const float lr = Dot(inFrame, t.right), lu = Dot(inFrame, t.up), lf = Dot(inFrame, t.fwd);
        const V3 before = Add(t.prevOrigin, Add(Add(Mul(t.prevRight, lr), Mul(t.prevUp, lu)), Mul(t.prevFwd, lf)));
        const V3 vh = Add(dr, Mul(Sub(dtp, dr), best.u)), vb = Sub(bodyPoint, before);
        const V3 vrel = Mul(Sub(vh, vb), 1.0f / (dt * upm));
        V3 n = Norm(Sub(bodyPoint, handPoint));
        if (Len(n) < 0.5f) n = Norm(vrel);
        const float speed = Len(vrel), into = Dot(vrel, n);
        const float minSpeed = s.MinSpeed() * (hd.kind == 1 ? 2.0f / 3.0f : 1.0f);
        const char* what = hd.kind == 1 ? (chainEnd ? "nunchaku end" : "weapon") : "fist"; // the chain's end
        const auto again = hd.lastOn.find(t.bike);
        const char* why = nullptr;
        size_t* tally = nullptr;
        if (!live) why = hd.grabbed ? "the hand holds the bars" : holdsRival ? "the hand holds a rival's weapon" : "an open hand (the grip button not closed)",
                   tally = hd.grabbed ? &totals.heldBars : &totals.unarmed;
        else if (!hd.armed) why = "the hand not armed (not calm since its last blow)", tally = &totals.unarmed;
        else if (speed < minSpeed) why = "too slow", tally = &totals.slow;
        else if (into < kApproach * speed) why = "a graze (not into the body)", tally = &totals.grazing;
        else if (again != hd.lastOn.end() && clock - again->second < kTargetCooldown)
            why = "the same rider again within 0.5 s", tally = &totals.cooling;
        int vetoKind = 0; // the swing's rules (vr_holsters.h SwingVeto): speed, travel, into, cooldown
        if (why == nullptr && (why = SwingVeto(h, hd.kind, speed, into, clock, vetoKind)) != nullptr)
            tally = vetoKind == 0 ? &totals.slow : vetoKind == 1 ? &totals.unarmed : vetoKind == 2 ? &totals.grazing : &totals.cooling;
        if (!shotPrefix.empty() && shotWanted.empty())
            shotWanted = shotPrefix + "_f" + std::to_string(frame) + (why != nullptr ? "_touch.png" : "_blow.png");
        if (why != nullptr) {
            ++*tally;
            std::printf("vr melee: frame %ld - the %s %s touched rival b%zu part %d at %.2f m/s (%.2f into it): no blow, %s\n",
                        frame, h ? "right" : "left", what, t.index, best.part, double(speed), double(into), why);
        } else {
            rr::game::PhysicalBlowRequest q;
            q.pending = true;
            q.victim = t.bike;
            q.right = h == 1;
            q.weapon = hd.kind == 1;
            q.strength = s.strengthFromSpeed ? std::clamp(speed / 4.0f, 0.5f, 1.5f) : 1.0f;
            q.part = best.part;
            q.speed = speed;
            if (pending.size() < 4) pending.push_back(q);
            hd.armed = false;
            hd.lastStrike = clock;
            hd.lastOn[t.bike] = clock;
            hd.pulse = 0.12f;
            hd.pulseAmp = std::min(1.0f, 0.55f + speed / 10.0f);
            {   // the touching point in the rider's frame at that sub-step (his frame between last frame's and this one's)
                const V3 at = Add(handPoint, Mul(n, hd.radius));
                const V3 o = Lerp(t.prevOrigin, t.origin, best.s);
                const V3 d = Sub(at, o);
                contact.bike = t.bike;
                contact.local = {Dot(d, Norm(Lerp(t.prevRight, t.right, best.s))), Dot(d, Norm(Lerp(t.prevUp, t.up, best.s))),
                                 Dot(d, Norm(Lerp(t.prevFwd, t.fwd, best.s)))};
            }
            contact.ttl = 0.5f;
            contact.hand = h;
            ++totals.hits;
            ++(hd.kind == 1 ? totals.weaponHits : totals.fistHits);
            totals.fastest = std::max(totals.fastest, speed);
            if (totals.hitList.size() < 400) {
                char b[96];
                std::snprintf(b, sizeof(b), " f%ld:%s-%s>b%zu/p%d/%.1f", frame, h ? "R" : "L", what, t.index, best.part,
                              double(speed));
                totals.hitList += b;
            }
            std::printf("vr melee: frame %ld - the %s %s hits rival b%zu part %d at %.2f m/s (%.2f into it, sub-step %.2f): "
                        "a blow (strength x%.2f), haptic %.2f\n",
                        frame, h ? "right" : "left", what, t.index, best.part, double(speed), double(into), double(best.s),
                        double(q.strength), double(hd.pulseAmp));
        }
    }
    keep();
}

void VrMelee::UpdateHands(VrHost& vr, const rr::xr::WorldAnchor& anchor, const VrHandlebars& bars, bool headView, bool paused,
                          long frame) {
    Impl& m = *impl_;
    const MeleeSettings& s = VrPrefs().melee;
    m.frame = frame;
    m.anchor = anchor;
    m.upm = anchor.unitsPerMetre > 0.0f ? anchor.unitsPerMetre : 1.0f;
    m.headView = headView;
    m.barsActive = bars.Active();
    m.weaponHand = HolsterWeaponHand(bars.WeaponHand()); // the hand that drew it from a holster
    const auto now = std::chrono::steady_clock::now();
    float dt = static_cast<float>(vr.DisplayPeriod());
    if (!m.scripted && m.last != std::chrono::steady_clock::time_point{})
        dt = std::clamp(std::chrono::duration<float>(now - m.last).count(), 0.001f, 0.1f);
    m.last = now;
    m.clock += dt;
    m.physicalNow = m.loaded && s.Physical() && headView && !paused;
    // the hands: the mock's script, the Handlebars mode's (it tracks them), or the controllers' in the Stick mode
    rr::xr::HandPose poses[2];
    bool fromPoses = false;
    float scriptGrip[2] = {m.scriptGrip[0], m.scriptGrip[1]};
    if (m.scripted && m.loaded) {
        if (!paused) m.RunScript(vr, frame);
        fromPoses = m.ScriptHands(frame, poses, dt);
        HolsterScriptHands(frame, paused, poses, scriptGrip); // the mock's holster actions over them
    } else if (!m.barsActive) {
        fromPoses = vr.LocateHands(poses);
    }
    const rr::xr::XrPad& touch = vr.TouchState();
    bool any = false;
    for (int h = 0; h < 2; ++h) {
        Impl::Hand& hd = m.hand[h];
        VrHandlebars::HandView bv;
        const bool barsHand = m.barsActive && bars.Hand(h, bv);
        if (fromPoses) {
            hd.valid = poses[h].gripValid;
            if (hd.valid) {
                rr::xr::Pose p;
                for (int k = 0; k < 3; ++k) p.position[k] = poses[h].grip[k];
                for (int k = 0; k < 4; ++k) p.orientation[k] = poses[h].grip[3 + k];
                const rr::xr::WorldEye w = rr::xr::PlaceInWorld(p, anchor);
                hd.pos = Make(w.eye), hd.right = Make(w.right), hd.up = Make(w.up), hd.fwd = Make(w.forward);
                hd.aim = hd.fwd;
                if (poses[h].aimValid) {
                    rr::xr::Pose a;
                    for (int k = 0; k < 4; ++k) a.orientation[k] = poses[h].aim[3 + k];
                    hd.aim = Make(rr::xr::PlaceInWorld(a, anchor).forward);
                }
                hd.local = Make(p.position);
            }
            hd.grip = m.scripted ? scriptGrip[h] : (h == 0 ? touch.leftGrip : touch.rightGrip);
            hd.trigger = m.scripted ? 0.0f : (h == 0 ? touch.leftTrigger : touch.rightTrigger);
            hd.grabbed = barsHand && bv.grabbed;
        } else if (barsHand) {
            hd.valid = true;
            hd.pos = Make(bv.world.eye), hd.right = Make(bv.world.right), hd.up = Make(bv.world.up);
            hd.fwd = Make(bv.world.forward);
            hd.aim = Make(bv.aimForward);
            hd.local = Make(bv.local);
            hd.grip = bv.grip, hd.trigger = bv.trigger, hd.grabbed = bv.grabbed;
        } else {
            hd.valid = false;
        }
        any = any || hd.valid;
    }
    m.anyHand = any;
    if (any) ++m.totals.trackedFrames;
    // the nunchaku's chain in the weapon hand, stepped before the sweep takes its swinging end (the weapon's
    // placement in the hand is re-applied from the final override below, vr_nunchaku.h Place)
    {
        const bool shown = m.loaded && s.Physical() && headView && s.nunchaku;
        const Impl::Hand& wh = m.hand[m.weaponHand];
        float down[3];
        for (int k = 0; k < 3; ++k) down[k] = -anchor.up[k];
        m.nunchaku.Update(m.weapon, shown && wh.valid && !paused, wh.pos.data(), wh.right.data(), wh.up.data(),
                          wh.fwd.data(), down, dt, m.upm);
    }
    {   // a free hand closing on a rival's swinging weapon
        VrSnatch::HandIn in[2];
        for (int h = 0; h < 2; ++h) {
            const Impl::Hand& hd = m.hand[h];
            in[h].valid = hd.valid;
            in[h].free = !hd.grabbed && !(m.weapon < 9 && h == m.weaponHand);
            in[h].grip = hd.grip;
            for (int k = 0; k < 3; ++k) in[h].pos[k] = hd.pos[k];
            in[h].radius = static_cast<float>(s.fistCm) / 100.0f * m.upm;
            in[h].reach = Len(hd.local);
        }
        std::vector<rr::game::PhysicalBlowRequest> grabs;
        m.snatch.Update(in, m.clock, dt, m.upm, frame, m.physicalNow && s.snatch, grabs);
        for (const auto& q : grabs)
            if (m.pending.size() < 4) m.pending.push_back(q);
        const int taken = m.snatch.TakenHand();
        if (m.snatch.CaughtNow()) HolsterHintHand(m.snatch.CaughtHand()); // the steal's weapon arrives in that hand
        if (!m.shotPrefix.empty() && m.shotWanted.empty() && (taken >= 0 || m.snatch.CaughtNow())) // the script's `shots`
            m.shotWanted = m.shotPrefix + "_f" + std::to_string(frame) + (taken >= 0 ? "_taken.png" : "_grab.png");
        if (taken >= 0) { // the weapon is in the hand that took it (the weapon hand from now on), a strong thump
            VrPrefs().melee.weaponHand = taken;
            m.weaponHand = taken;
            m.hand[taken].pulse = 0.2f;
            m.hand[taken].pulseAmp = 1.0f;
        }
    }
    for (int h = 0; h < 2; ++h) {
        if (m.physicalNow) {
            m.Detect(h, dt);
        } else {
            m.hand[h].havePrev = m.hand[h].armed = false;
            m.hand[h].kind = -1;
        }
    }
    if (!m.shotWanted.empty()) { // the script's `shots`: this contact's eyes
        vr.RequestShot(m.shotWanted);
        m.shotWanted.clear();
    }
    // the haptics: the contact thump over the Handlebars mode's own (the host keeps the last call's levels)
    float bars2[2] = {0.0f, 0.0f};
    bars.Haptics(bars2);
    float amp[2];
    bool pulse = false;
    for (int h = 0; h < 2; ++h) {
        Impl::Hand& hd = m.hand[h];
        amp[h] = bars2[h];
        if (hd.pulse > 0.0f) {
            amp[h] = std::max(amp[h], hd.pulseAmp);
            hd.pulse -= dt;
            pulse = true;
        }
    }
    if (m.barsActive || pulse || m.hapticsOn) vr.SetHandHaptics(amp[0], amp[1]);
    m.hapticsOn = pulse;
    if (m.contact.ttl > 0.0f) m.contact.ttl -= dt;
    // the weapon in the tracked hand, shown before the rider's weapon object exists (weapon_draw.h unheld)
    if (s.Physical() && headView && m.loaded) {
        rr::render::WeaponHandOverride o;
        if (m.barsActive) {
            o = rr::render::CurrentWeaponHandOverride(); // the Handlebars mode placed it (vr_handlebars.cpp)
        } else {
            const Impl::Hand& wh = m.hand[m.weaponHand];
            o.active = true;
            o.rider = m.playerRider;
            o.hidden = !wh.valid;
            for (int k = 0; k < 3; ++k) {
                o.origin[k] = wh.pos[k];
                o.along[k] = wh.fwd[k]; // out of the fist's thumb side (the grip's -Z), as the Handlebars mode
                o.side[k] = wh.right[k];
            }
            o.scale = 1.0f / 1024.0f;
        }
        o.unheld = m.weapon < 9 ? m.weapon : -1;
        o.chainCount = 0;
        if (s.nunchaku) m.nunchaku.Fill(o); // the chain as simulated (weapon_draw.h chainCount)
        rr::render::SetWeaponHandOverride(o);
        m.overrideSet = true;
    } else if (m.overrideSet && !m.barsActive) {
        rr::render::SetWeaponHandOverride(rr::render::WeaponHandOverride{});
        m.overrideSet = false;
    }
}

void VrMelee::ApplyToPad(rr::game::PadState& pad) {
    Impl& m = *impl_;
    if (m.pending.empty()) return;
    pad.blow = m.pending.front();
    m.pending.erase(m.pending.begin());
    ++m.totals.sent;
}

uint32_t VrMelee::HiddenRiderParts(bool headView) const {
    const Impl& m = *impl_;
    const MeleeSettings& s = VrPrefs().melee;
    // the Stick mode draws the tracked hands: the rider's forearms 6 / 9 and gloves 7 / 10 would be a second pair (with
    // no hand tracked - the controllers put down - the rider's own arms stay)
    return m.loaded && headView && m.anyHand && !m.barsActive && s.Physical() && s.showHands ? 0x6C0u
                                                                                                                  : 0u;
}

void VrMelee::Impl::WireCapsule(const V3& a, const V3& b, float r, const V3& rgb) {
    V3 dir = Norm(Sub(b, a));
    if (Len(dir) < 0.5f) dir = {0.0f, -1.0f, 0.0f};
    const V3 u = Norm(Cross(dir, std::fabs(dir[1]) < 0.9f ? V3{0.0f, 1.0f, 0.0f} : V3{1.0f, 0.0f, 0.0f}));
    const V3 v = Cross(dir, u);
    constexpr int kSeg = 16;
    constexpr float kPi = 3.14159265f;
    for (int ring = 0; ring < 2; ++ring) { // the two end circles and the half-circle caps in both planes
        const V3 c = ring == 0 ? a : b;
        const float sign = ring == 0 ? -1.0f : 1.0f;
        for (int i = 0; i < kSeg; ++i) {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / kSeg, a1 = 2.0f * kPi * static_cast<float>(i + 1) / kSeg;
            Line(Add(c, Add(Mul(u, r * std::cos(a0)), Mul(v, r * std::sin(a0)))),
                 Add(c, Add(Mul(u, r * std::cos(a1)), Mul(v, r * std::sin(a1)))), rgb);
            const float h0 = kPi * static_cast<float>(i) / kSeg, h1 = kPi * static_cast<float>(i + 1) / kSeg;
            for (const V3& side : {u, v})
                Line(Add(c, Add(Mul(side, r * std::cos(h0)), Mul(dir, sign * r * std::sin(h0)))),
                     Add(c, Add(Mul(side, r * std::cos(h1)), Mul(dir, sign * r * std::sin(h1)))), rgb);
        }
    }
    for (const V3& side : {u, v, Mul(u, -1.0f), Mul(v, -1.0f)}) Line(Add(a, Mul(side, r)), Add(b, Mul(side, r)), rgb);
}

void VrMelee::Draw(const rr::render::Mat4& viewProj) {
    Impl& m = *impl_;
    const MeleeSettings& s = VrPrefs().melee;
    if (m.loaded) m.nunchaku.Observe(rr::render::CurrentWeaponHandOverride()); // the grip as drawn
    if (!m.loaded || !m.headView || !s.Physical()) return;
    // the Stick mode's hands (the Handlebars mode draws its own)
    if (!m.barsActive && s.showHands) {
        GloveDraw gloves[2];
        GripMarker markers[2];
        for (int h = 0; h < 2; ++h) {
            const Impl::Hand& hd = m.hand[h];
            GloveDraw& g = gloves[h];
            g.right = h == 1;
            g.visible = hd.valid;
            if (!hd.valid) continue;
            for (int k = 0; k < 3; ++k) {
                g.origin[k] = hd.pos[k];
                g.gripRight[k] = hd.right[k];
                g.gripUp[k] = hd.up[k];
                g.gripForward[k] = hd.fwd[k];
                g.aimForward[k] = hd.aim[k];
            }
            g.scale = m.upm;
            g.grip = std::clamp(hd.grip, 0.0f, 1.0f);
            g.trigger = std::max(std::clamp(hd.trigger, 0.0f, 1.0f), g.grip);
        }
        m.hands.Draw(viewProj, gloves, markers);
    }
    if (!s.debug) return;
    // the colliders: the fists green when armed (dark green not), the weapon yellow, grey not live, red for half a
    // second after its blow (and a red cross where it landed); the riders near the player cyan
    m.lines.clear(); // the riders first, the hands and the contact over them
    const V3 eye = Make(m.anchor.origin);
    for (const Impl::Target& t : m.targets) {
        if (Len(Sub(t.origin, eye)) > 6.0f * m.upm) continue;
        for (int p = 0; p <= kBikePart; ++p) {
            if (p == kBikePart && !s.bikes) continue;
            const Impl::Capsule& c = t.now[p];
            if (c.valid) m.WireCapsule(c.a, c.b, c.r, t.hittable ? V3{0.1f, 0.85f, 1.0f} : V3{0.3f, 0.3f, 0.6f});
        }
    }
    for (int h = 0; h < 2; ++h) {
        const Impl::Hand& hd = m.hand[h];
        if (!hd.valid || hd.kind < 0) continue;
        const bool live = !hd.grabbed && (hd.kind == 1 || hd.grip >= kGripFist);
        const V3 rgb = m.contact.ttl > 0.0f && m.contact.hand == h ? V3{1.0f, 0.15f, 0.1f} // it just landed a blow
                     : !live ? V3{0.5f, 0.5f, 0.5f} : hd.kind == 1 ? V3{1.0f, 0.9f, 0.1f}
                     : hd.armed ? V3{0.1f, 1.0f, 0.2f} : V3{0.1f, 0.5f, 0.2f};
        m.WireCapsule(hd.root, hd.tip, hd.radius, rgb);
    }
    m.snatch.Lines(eye.data(), 6.0f * m.upm, [&](const float* a, const float* b, float r, const float* rgb) {
        m.WireCapsule(Make(a), Make(b), r, Make(rgb));
    });
    {
        const auto j = m.nunchaku.Joints(); // the chain's joints, magenta
        for (size_t k = 1; k < j.size(); ++k) m.Line(Make(j[k - 1].data()), Make(j[k].data()), V3{1.0f, 0.2f, 1.0f});
    }
    for (const Impl::Target& t : m.targets) {
        if (m.contact.ttl <= 0.0f || t.bike != m.contact.bike) continue;
        const V3 at = Add(t.origin, Add(Add(Mul(t.right, m.contact.local[0]), Mul(t.up, m.contact.local[1])),
                                        Mul(t.fwd, m.contact.local[2])));
        const float k = 0.12f * m.upm;
        const V3 red{1.0f, 0.1f, 0.1f};
        m.Line(Add(at, V3{-k, 0, 0}), Add(at, V3{k, 0, 0}), red);
        m.Line(Add(at, V3{0, -k, 0}), Add(at, V3{0, k, 0}), red);
        m.Line(Add(at, V3{0, 0, -k}), Add(at, V3{0, 0, k}), red);
    }
    if (m.lines.empty()) return;
    if (m.program == 0) {
        m.program = rr::render::BuildProgram(kLineVs, kLineFs);
        m.viewProjLoc = gl.GetUniformLocation(m.program, "uViewProj");
        gl.GenVertexArrays(1, &m.vao);
        gl.BindVertexArray(m.vao);
        gl.GenBuffers(1, &m.vbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, m.vbo);
        const GLsizei stride = 6 * static_cast<GLsizei>(sizeof(float));
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
        gl.EnableVertexAttribArray(1);
    }
    glDisable(GL_DEPTH_TEST); // the colliders show through the bodies
    glDisable(GL_BLEND);
    gl.BindVertexArray(m.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, m.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<rr::render::GLsizeiptr>(m.lines.size() * sizeof(float)), m.lines.data(),
                  GL_DYNAMIC_DRAW);
    gl.UseProgram(m.program);
    rr::render::UploadViewProj(m.viewProjLoc, viewProj);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m.lines.size() / 6));
    gl.BindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

std::string VrMelee::Totals() const {
    const Impl& m = *impl_;
    const auto& t = m.totals;
    char b[1400];
    std::snprintf(b, sizeof(b),
                  "vr melee: %s - hands tracked in %zu frame(s), riders posed in %zu; blows %zu (fist %zu, weapon %zu), "
                  "touches without a blow: too slow %zu, grazing %zu, not armed / open hand %zu, the same rider again %zu, "
                  "a hand on the bars %zu; tracking jumps %zu; the fastest blow %.2f m/s; sent to the game %zu; script "
                  "blows %zu (no rider in reach %zu);%s",
                  VrPrefs().melee.Describe().c_str(), t.trackedFrames, t.posed, t.hits, t.fistHits, t.weaponHits, t.slow,
                  t.grazing, t.unarmed, t.cooling, t.heldBars, t.jumps, double(t.fastest), t.sent, t.swings, t.swingsSkipped,
                  t.hitList.empty() ? " (none)" : t.hitList.c_str());
    return std::string(b) + "\n" + m.snatch.Totals() + "\n" + m.nunchaku.Totals();
}

// The hands and a segment's nearest rider (vr_holsters.h), for the holsters and the prod's discharge
bool VrMelee::Hand(int h, MeleeHandView& out) const {
    const Impl& m = *impl_;
    out = MeleeHandView{};
    if (h < 0 || h > 1 || !m.loaded) return false;
    const Impl::Hand& hd = m.hand[h];
    out.valid = hd.valid;
    out.grabbed = hd.grabbed;
    out.grip = hd.grip, out.trigger = hd.trigger;
    for (int k = 0; k < 3; ++k) {
        out.pos[k] = hd.pos[k], out.right[k] = hd.right[k], out.up[k] = hd.up[k];
        out.fwd[k] = hd.fwd[k], out.aim[k] = hd.aim[k], out.local[k] = hd.local[k];
    }
    return hd.valid;
}

bool VrMelee::Touching(const float root[3], const float tip[3], float radius, MeleeTouch& out) const {
    const Impl& m = *impl_;
    const MeleeSettings& s = VrPrefs().melee;
    const V3 a = Make(root), b = Make(tip);
    bool found = false;
    for (const Impl::Target& t : m.targets) {
        if (!t.hittable || Len(Sub(a, t.origin)) > kReach * m.upm) continue;
        for (int p = 0; p <= kBikePart; ++p) {
            if (p == kBikePart && !s.bikes) continue;
            const Impl::Capsule& c = t.now[p];
            if (!c.valid) continue;
            float u = 0.0f, w = 0.0f;
            const float gap = SegSeg(a, b, c.a, c.b, u, w) - c.r - radius;
            if (gap > 0.0f || (found && gap >= out.gap)) continue;
            found = true;
            out.bike = t.bike, out.index = t.index, out.part = p, out.gap = gap;
        }
    }
    return found;
}

} // namespace rrgame
