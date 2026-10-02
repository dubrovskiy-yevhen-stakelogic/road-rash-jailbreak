// Holsters, the inventory in the hands and the swing's rules in VR (vr_holsters.h).
#include "vr_holsters.h"

#include "game/cheats.h"            // CheatWeaponName: the weapons' art tags from the disc
#include "game/sim/fight.h"         // the last-blow table, the fight stats (the prod's attack resolved)
#include "game/weapon_inventory.h"
#include "game_host_vr.h"
#include "platform/input_bindings.h"
#include "render/gl_api.h"
#include "render/multiview.h"
#include "render/shaders.h"
#include "render/weapon_draw.h"
#include "vr_handlebars.h"
#include "vr_melee.h"
#include "vr_settings.h"
#include "vr_weapon_calib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rrgame {

using rr::render::gl;

namespace {

constexpr float kGripOn = 0.65f, kGripOff = 0.30f; // GTA SA VR's hysteresis (Holster.cpp)
constexpr float kStrokeSpeed = 2.0f;  // m/s of the collider's far end against the seat: the swing is under way (a hand
                                      // brought up or wound back slower is not counted)
constexpr float kStrokeGap = 0.08f;   // s under that speed end the swing (its travel starts again)
constexpr float kDischargeReach = 0.15f; // metres beyond the weapon's surface the prod's discharge reaches
constexpr int kAttackPressFrames = 2;    // the pad bits of the original's prod attack (as --punch presses)
constexpr float kScriptSpeed = 1.0f;  // m/s: the mock's hands to a holster and back
constexpr float kScriptFrame = 1.0f / 72.0f;

using V3 = std::array<float, 3>;
V3 Make(const float* p) { return {p[0], p[1], p[2]}; }
V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Lerp(const V3& a, const V3& b, float t) { return Add(a, Mul(Sub(b, a), t)); }
V3 Norm(const V3& a) {
    const float l = Len(a);
    return l > 1e-6f ? Mul(a, 1.0f / l) : V3{0, 0, 0};
}

// quaternions (x, y, z, w)
void QMul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
void QAxis(const V3& axis, float radians, float o[4]) {
    const V3 n = Norm(axis);
    const float s = std::sin(radians * 0.5f);
    o[0] = n[0] * s, o[1] = n[1] * s, o[2] = n[2] * s, o[3] = std::cos(radians * 0.5f);
}

// ------------------------------------------------------------------------------------------------ the mock's script
// "<frame> <command> [args]" per line or ';'-separated (a file, or the text itself), on top of --vr-melee-script's hands.
//   draw H SIDE          hand H (left | right) to holster SIDE at 1 m/s, the grip open, squeezed there, back again
//   stow H [SIDE]        hand H (holding) to holster SIDE (default: its weapon's own), the grip released there, back
//   release H N          hand H's grip open for N frames where it is (a release away from the holsters)
//   button A|B|X|Y N     the Touch button held N frames (through the VR bindings, as a headset's)
//   menu ROW DIR         the "Weapons and holsters" page's row ROW changed (DIR -1 / +1, 0 = A)
//   calib W H | calib off  the grip calibration page as if open on weapon W in hand H (left | right)
//   align DX DY DZ PITCH YAW N  the calibration page's other hand takes the weapon (10 cm before the palm), moves it
//                        DX / DY / DZ metres (the holding hand's right / up / forward) and turns it PITCH / YAW degrees
//                        over N frames, lets go, goes back
//   shot PATH            the eyes of this frame into PATH
//   gripcheck W          weapon W's grip in both hands as they are this frame, mirror compared (one line)
struct ScriptEvent {
    long frame = 0;
    std::string command, text;
    int hand = 1, side = 1;
    float v[6] = {0, 0, 0, 0, 0, 0};
};

int HandOf(const std::string& h, const std::string& line) {
    if (h == "left") return 0;
    if (h == "right") return 1;
    throw std::runtime_error("vr holster script: '" + h + "' is not left / right in: " + line);
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
        std::string a, b;
        if (e.command == "draw") {
            if (!(w >> a >> b)) throw std::runtime_error("vr holster script: draw H SIDE: " + line);
            e.hand = HandOf(a, line), e.side = HandOf(b, line);
        } else if (e.command == "stow") {
            if (!(w >> a)) throw std::runtime_error("vr holster script: stow H [SIDE]: " + line);
            e.hand = HandOf(a, line);
            e.side = (w >> b) ? HandOf(b, line) : -1;
        } else if (e.command == "release") {
            if (!(w >> a >> e.v[0])) throw std::runtime_error("vr holster script: release H N: " + line);
            e.hand = HandOf(a, line);
        } else if (e.command == "button") {
            if (!(w >> e.text >> e.v[0]) || (e.text != "A" && e.text != "B" && e.text != "X" && e.text != "Y"))
                throw std::runtime_error("vr holster script: button A|B|X|Y N: " + line);
        } else if (e.command == "menu") {
            if (!(w >> e.v[0] >> e.v[1])) throw std::runtime_error("vr holster script: menu ROW DIR: " + line);
        } else if (e.command == "calib") {
            if (!(w >> a)) throw std::runtime_error("vr holster script: calib W H | calib off: " + line);
            if (a == "off") {
                e.v[0] = -1;
            } else {
                e.v[0] = static_cast<float>(std::atoi(a.c_str()));
                if (!(w >> b)) throw std::runtime_error("vr holster script: calib W H: " + line);
                e.hand = HandOf(b, line);
            }
        } else if (e.command == "align") {
            for (int k = 0; k < 6; ++k)
                if (!(w >> e.v[k])) throw std::runtime_error("vr holster script: align DX DY DZ PITCH YAW N: " + line);
        } else if (e.command == "shot") {
            if (!(w >> e.text)) throw std::runtime_error("vr holster script: shot PATH: " + line);
        } else if (e.command == "gripcheck") { // vr_weapon_calib.h GripMirrorCheck
            if (!(w >> e.v[0]) || e.v[0] < 0 || e.v[0] > 8) throw std::runtime_error("vr holster script: gripcheck W: " + line);
        } else {
            throw std::runtime_error("vr holster script: unknown command '" + e.command + "' in: " + line);
        }
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptEvent& x, const ScriptEvent& y) { return x.frame < y.frame; });
    return out;
}

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

struct VrHolsters::Impl {
    bool loaded = false, mock = false;
    // ---- the game's inventory (UpdateGame)
    bool started = false;
    uint16_t owned = 0;
    int lastSeen = 9;          // riderDef +0x2E as last seen (or written)
    int gameInHand = 9;
    int want = -1;             // a write pending: the weapon for +0x2E (9 = the hand emptied), -1 none
    int lastDrawn = -1;
    uint32_t nibbles = 0;      // riderDef +0x30: the weapons' swings
    int spraySwings = -1;      // the spray's swings while holstered (it has no nibble: +0x2F only; OURS)
    uint32_t rider = 0, rd = 0;
    // ---- the hands (UpdateHands)
    struct Hand {
        bool valid = false, grabbed = false, down = false;
        float grip = 0.0f;
        HandFrameW frame;
        V3 local{};
        int held = -1;         // the weapon this hand holds
        float pulse = 0.0f, pulseAmp = 0.5f;
    } hand[2];
    int hint = -1;             // HolsterHintHand
    bool active = false, headView = false, paused = false, wasOn = true, overrideSet = false;
    rr::xr::WorldAnchor anchor;
    float upm = 1.0f;
    long frame = 0;
    V3 holster[2]{};           // the holsters' points, world
    V3 holsterLocal[2]{};      // and in the seat's space (metres)
    bool inReach[2] = {false, false};
    // ---- the swing (SwingTrack / SwingVeto)
    struct Stroke {
        bool have = false;
        int kind = -1;
        V3 prev{};
        float travel = 0.0f, below = 0.0f;
        double lastBlow = -100.0;
    } stroke[2];
    // ---- the buttons, the prod
    bool bWas = false;
    rr::game::PhysicalBlowRequest discharge;
    double clock = 0.0, lastDischarge = -100.0; // seconds of display time; the last discharge's blow
    // B with the prod / stun gun and no rider touched - the ORIGINAL's attack on the pad (combat action 6,
    // L1 + d-pad Up), then watched until the fight code resolves it (the last-blow table) for the hand's feedback
    int attackPress = 0; // frames the pad bits are still held
    struct Watch {
        bool active = false, based = false;
        int hand = 1, weapon = 7;
        long frame = 0;
        double clock = 0.0;
        uint32_t victim = 0, time = 0;
        uint8_t landed = 0;
        std::vector<uint8_t> health; // the riders' +0x0F at the press
    } watch;
    int gripCheck = -1; // the script's `gripcheck W`, pending
    // ---- the mock's script
    std::vector<ScriptEvent> script;
    size_t nextEvent = 0;
    struct Action {
        bool active = false;
        std::string kind;
        int side = 1;
        int phase = 0;
        long phaseStart = 0, phaseFrames = 1;
        V3 from{}, at{}, delta{};
        float quat[4] = {0, 0, 0, 1};
        float pitch = 0.0f, yaw = 0.0f;
        long frames = 0;
    } action[2];
    long scriptButtonUntil[4] = {-1, -1, -1, -1}; // A B X Y
    size_t weaponPresses = 0; // frames A / X pressed a weapon 6..8's own attack
    bool scriptCalib = false;
    std::string shotPending;
    // ---- drawing
    GLuint program = 0, vao = 0, vbo = 0;
    GLint viewProjLoc = -1;
    std::vector<float> lines;
    // ---- the log
    struct Totals {
        size_t draws[2] = {0, 0}, backs = 0, backsAt = 0, gripLockKept = 0, writes = 0, deferred = 0, startStows = 0;
        size_t arrivals = 0, stolen = 0, putAway = 0, discharges = 0, dischargeBlows = 0, dischargeAir = 0, dischargeCool = 0;
        size_t attacks = 0, attackLanded = 0, attackMissed = 0, attackNoBlow = 0; // the original's attack on B
        size_t suppressed = 0, vetoSlow = 0, vetoShort = 0, vetoInto = 0, vetoCool = 0, accepted = 0, calibSaves = 0, swaps = 0;
        size_t holsterFrames = 0, drawnHolsters = 0;
        std::string events;
    } totals;

    void Note(const std::string& e) {
        if (totals.events.size() < 600) totals.events += " " + e;
    }
    int HeldWeapon() const { return hand[0].held >= 0 ? hand[0].held : hand[1].held; }
    // owned, and (with "Weapons with no swings" hidden) swings left in its nibble - the spray has none (PickWeapon's
    // rule), and the weapon in the hand counts as it is
    bool Usable(int w) const {
        if (w < 0 || w > 8 || !((owned >> w) & 1)) return false;
        if (!VrPrefs().weapons.hideEmpty || w == 8 || w == HeldWeapon() || w == gameInHand) return true;
        return ((nibbles >> (4 * w)) & 0xFu) != 0;
    }
    int HeldHand() const { return hand[0].held >= 0 ? 0 : hand[1].held >= 0 ? 1 : -1; }
    V3 FromLocal(const V3& l) const {
        V3 w = Make(anchor.origin);
        for (int k = 0; k < 3; ++k) w[k] += (anchor.right[k] * l[0] + anchor.up[k] * l[1] - anchor.ahead[k] * l[2]) * upm;
        return w;
    }
    void AutoAssign();
    int OwnHolster(int weapon) const {
        const WeaponsSettings& s = VrPrefs().weapons;
        return s.right == weapon ? 1 : s.left == weapon ? 0 : -1;
    }
    void RunScript(long f);
    bool ScriptPose(int h, long f, rr::xr::HandPose& pose, float& grip, const rr::xr::HandPose poses[2]);
    void Ring(const V3& c, const V3& a, const V3& b, float r, const V3& rgb);
};

namespace {

VrHolsters::Impl* g_state = nullptr;
int g_ownedMask = -1; // the player's owned weapons in the running race (-1: none runs) - the menu's rows

} // namespace

// ================================================================================================ the hooks
int HolsterWeaponHand(int fallback) {
    const VrHolsters::Impl* m = g_state;
    if (m == nullptr || !m->active) return fallback;
    const int h = m->HeldHand();
    return h >= 0 ? h : fallback;
}

bool HolsterHandBusy(int hand) {
    const VrHolsters::Impl* m = g_state;
    // where the buttons attack, the hand holding a weapon takes its grip too (the weapon stays the game's
    // current one, +0x2E - A / X swing it from the bars; the grip let go puts it back); Physical only: the hand holding
    // it does not take the bars
    return m != nullptr && m->active && hand >= 0 && hand < 2 && m->hand[hand].held >= 0 && !VrPrefs().melee.ButtonsAttack();
}

void HolsterHintHand(int hand) {
    if (g_state != nullptr && (hand == 0 || hand == 1)) g_state->hint = hand;
}

int HolsterHeldWeapon() { return g_state != nullptr ? g_state->HeldWeapon() : -1; }
int HolsterHeldHand() { return g_state != nullptr ? g_state->HeldHand() : -1; }

void SwingTrack(int h, int kind, const float tipSeat[3], float dt, bool reset) {
    VrHolsters::Impl* m = g_state;
    if (m == nullptr || h < 0 || h > 1) return;
    VrHolsters::Impl::Stroke& s = m->stroke[h];
    const V3 tip = Make(tipSeat);
    if (reset || !s.have || s.kind != kind || dt <= 0.0f) {
        s.have = true, s.kind = kind, s.prev = tip, s.travel = 0.0f, s.below = 0.0f;
        return;
    }
    const float step = Len(Sub(tip, s.prev));
    if (step / dt >= kStrokeSpeed) {
        s.travel += step;
        s.below = 0.0f;
    } else if ((s.below += dt) > kStrokeGap) {
        s.travel = 0.0f;
    }
    s.prev = tip;
}

const char* SwingVeto(int h, int kind, float speed, float into, double clock, int& category) {
    VrHolsters::Impl* m = g_state;
    if (m == nullptr || h < 0 || h > 1) return nullptr;
    const WeaponsSettings& s = VrPrefs().weapons;
    VrHolsters::Impl::Stroke& st = m->stroke[h];
    static char why[160];
    if (kind == 1 && speed < s.WeaponSpeed()) {
        ++m->totals.vetoSlow;
        category = 0;
        std::snprintf(why, sizeof(why), "too slow for a weapon (%.2f m/s at the contact, the weapon swing speed %.1f)",
                      double(speed), double(s.WeaponSpeed()));
        return why;
    }
    const float need = static_cast<float>(kind == 1 ? s.weaponTravelCm : s.fistTravelCm) / 100.0f;
    if (st.travel < need) {
        ++m->totals.vetoShort;
        category = 1;
        std::snprintf(why, sizeof(why), "a short swing (%.0f cm of travel, %s %d cm)", double(st.travel * 100.0f),
                      kind == 1 ? "the weapon swing travel" : "the punch travel", kind == 1 ? s.weaponTravelCm : s.fistTravelCm);
        return why;
    }
    if (into < static_cast<float>(s.intoPct) / 100.0f * speed) {
        ++m->totals.vetoInto;
        category = 2;
        std::snprintf(why, sizeof(why), "not into the body (%.0f %% of the speed, into the target %d %%)",
                      double(speed > 0.0f ? 100.0f * into / speed : 0.0f), s.intoPct);
        return why;
    }
    if (clock - st.lastBlow < static_cast<double>(s.cooldownMs) / 1000.0) {
        ++m->totals.vetoCool;
        category = 3;
        std::snprintf(why, sizeof(why), "the hand's blow cooldown (%.2f s since its last, %d ms)", clock - st.lastBlow,
                      s.cooldownMs);
        return why;
    }
    std::printf("vr holsters: the %s hand's %s swing: %.0f cm of travel, %.2f m/s at the contact, %.0f %% into the body - "
                "a blow\n", h ? "right" : "left", kind == 1 ? "weapon" : "fist", double(st.travel * 100.0f), double(speed),
                double(speed > 0.0f ? 100.0f * into / speed : 0.0f));
    st.lastBlow = clock;
    st.travel = 0.0f;
    ++m->totals.accepted;
    return nullptr;
}

std::string HolsterWeaponLabel(int weapon) {
    if (weapon < 0 || weapon > 8) return "Empty";
    std::string s = rr::game::CheatWeaponName(weapon);
    if (g_ownedMask >= 0 && !((g_ownedMask >> weapon) & 1)) s += " - not owned";
    else if (g_state != nullptr && g_ownedMask >= 0 && !g_state->Usable(weapon)) s += " - no swings";
    return s;
}

std::string HolsterOwnedLabel() {
    if (g_ownedMask < 0) return "(in a race)";
    std::string s;
    for (int w = 0; w <= 8; ++w)
        if ((g_ownedMask >> w) & 1) s += (s.empty() ? "" : ", ") + rr::game::CheatWeaponName(w);
    return s.empty() ? "none" : s;
}

int CycleHolsterWeapon(int current, int other, int direction) {
    std::vector<int> ring{-1};
    for (int w = 0; w <= 8; ++w) // (in a race: the weapons it offers - owned, and with swings when those are hidden)
        if (w != other && (g_ownedMask < 0 || (g_state != nullptr ? g_state->Usable(w) : ((g_ownedMask >> w) & 1) != 0)))
            ring.push_back(w);
    auto it = std::find(ring.begin(), ring.end(), current);
    int i = it == ring.end() ? 0 : static_cast<int>(it - ring.begin());
    const int n = static_cast<int>(ring.size());
    i = ((i + (direction < 0 ? -1 : 1)) % n + n) % n;
    return ring[static_cast<size_t>(i)];
}

// ================================================================================================ the class
VrHolsters::VrHolsters() : impl_(std::make_unique<Impl>()) { g_state = impl_.get(); }

VrHolsters::~VrHolsters() {
    if (g_state == impl_.get()) g_state = nullptr;
    g_ownedMask = -1;
    rr::render::SetExtraWeapons({});
    if (impl_->overrideSet) rr::render::SetWeaponHandOverride(rr::render::WeaponHandOverride{});
    SetCalibPageOpen(false, -1, -1);
}

bool VrHolsters::Load(const rr::DiscImage& disc, bool mock) {
    Impl& m = *impl_;
    m.mock = mock;
    m.loaded = LoadWeaponShapes(disc);
    std::printf("%s\n", DescribeWeaponShapes().c_str());
    std::printf("vr holsters: %s\n", VrPrefs().weapons.Describe().c_str());
    const std::string& arg = HolsterScriptArgument();
    if (!arg.empty()) {
        if (!mock) {
            std::printf("vr holsters: --vr-holster-script ignored (only the desktop VR mock takes scripted controllers)\n");
        } else {
            m.script = ParseScript(arg);
            std::printf("vr holsters: the mock's holster actions from a script, %zu event(s)\n", m.script.size());
        }
    }
    return m.loaded;
}

void VrHolsters::Impl::AutoAssign() {
    WeaponsSettings& s = VrPrefs().weapons;
    const auto usable = [&](int w) { return Usable(w); };
    // the weapon in hand first, then the others by id
    std::vector<int> order;
    if (usable(gameInHand)) order.push_back(gameInHand);
    for (int w = 0; w <= 8; ++w)
        if (usable(w) && w != gameInHand) order.push_back(w);
    for (int w : order) {
        if (s.left == w || s.right == w) continue;
        int* slot = !usable(s.right) ? &s.right : !usable(s.left) ? &s.left : nullptr;
        if (slot == nullptr) break;
        *slot = w;
        std::printf("vr holsters: frame %ld - weapon %d (%s) takes the %s holster\n", frame, w,
                    rr::game::CheatWeaponName(w).c_str(), slot == &s.right ? "right" : "left");
    }
}

void VrHolsters::UpdateGame(const rr::game::RaceSession& s) {
    Impl& m = *impl_;
    if (s.Bikes().empty() || s.ArenaRam() == nullptr) return;
    m.rider = s.Bikes()[0].ownerAddress;
    m.rd = s.Bikes()[0].riderDefAddress;
    const rr::game::WeaponRecord rec = rr::game::ReadWeaponRecord(s.ArenaRam(), m.rider, m.rd);
    if (!rec.valid) return;
    const WeaponsSettings& set = VrPrefs().weapons;
    g_ownedMask = rec.owned;
    m.gameInHand = rec.inHand;
    m.nibbles = rec.nibbles;
    if (!m.started) {
        m.started = true;
        m.owned = rec.owned;
        m.lastSeen = rec.inHand;
        {
            uint16_t raw = 0;
            std::memcpy(&raw, s.ArenaRam() + ((m.rd + 0x2Cu) & 0x1FFFFFu), 2);
            std::printf("vr holsters: the player's record at the start: +0x2C 0x%04X (owned %s), in hand %d, swings %d, the "
                        "swing nibbles +0x30 0x%08X\n",
                        raw, rr::game::OwnedList(rec.owned).c_str(), rec.inHand, rec.swings, rec.nibbles);
        }
        m.AutoAssign();
        if (set.holsters && rec.inHand < 9) { // the race starts with the weapon the record carries in its holster
            m.want = 9;
            m.lastDrawn = rec.inHand;
            ++m.totals.startStows;
            m.Note("start-stow:" + std::to_string(rec.inHand));
            std::printf("vr holsters: the race starts with weapon %d (%s) in the rider's hand - it goes to its holster "
                        "(owned %s)\n", rec.inHand, rr::game::CheatWeaponName(rec.inHand).c_str(),
                        rr::game::OwnedList(rec.owned).c_str());
        }
        return;
    }
    const uint16_t lost = static_cast<uint16_t>(m.owned & ~rec.owned), gained = static_cast<uint16_t>(rec.owned & ~m.owned);
    for (int h = 0; h < 2; ++h) {
        Impl::Hand& hd = m.hand[h];
        if (hd.held < 0 || !((lost >> hd.held) & 1)) continue;
        ++m.totals.stolen;
        m.Note("stolen:" + std::to_string(hd.held));
        for (size_t i = 1; i < s.Bikes().size(); ++i) { // who has it now (WeaponSteal moved it into the thief's hand)
            const rr::game::RaceBike& b = s.Bikes()[i];
            const rr::game::WeaponRecord other = rr::game::ReadWeaponRecord(s.ArenaRam(), b.ownerAddress, b.riderDefAddress);
            if (other.valid && other.inHand == hd.held && rr::game::Owns(other, hd.held))
                std::printf("vr holsters: frame %ld - weapon %d is now in rival b%zu's hand (his owned %s, swings %d)\n", m.frame,
                            hd.held, i, rr::game::OwnedList(other.owned).c_str(), other.swings);
        }
        std::printf("vr holsters: frame %ld - weapon %d (%s) in the %s hand was TAKEN by a rival (the record's mask 0x%03X -> "
                    "0x%03X, in hand %d): gone from the hand and from the inventory, its holster empty\n",
                    m.frame, hd.held, rr::game::CheatWeaponName(hd.held).c_str(), h ? "right" : "left", m.owned, rec.owned,
                    rec.inHand);
        hd.held = -1;
        m.want = -1;
    }
    if (lost != 0 && m.HeldWeapon() < 0)
        for (int w = 0; w <= 8; ++w)
            if ((lost >> w) & 1)
                std::printf("vr holsters: frame %ld - weapon %d is no longer owned (mask 0x%03X -> 0x%03X)\n", m.frame, w,
                            m.owned, rec.owned);
    if (rec.inHand != m.lastSeen) { // the game changed the weapon in hand: a steal either way, a cheat, a switch
        m.want = -1;
        if (rec.inHand < 9 && set.holsters) {
            int h = m.hint >= 0 ? m.hint : (VrPrefs().melee.weaponHand == 0 ? 0 : 1);
            if (m.hand[h].grabbed && !m.hand[1 - h].grabbed) h = 1 - h; // the free hand
            if (m.hand[1 - h].held >= 0) m.hand[1 - h].held = -1;  // one weapon in the hands: the other went back
            m.hand[h].held = rec.inHand;
            m.lastDrawn = rec.inHand;
            ++m.totals.arrivals;
            m.Note("arrived:" + std::to_string(rec.inHand) + (h ? "R" : "L"));
            std::printf("vr holsters: frame %ld - weapon %d (%s) arrived in the player's hand (the record: %d -> %d, owned "
                        "%s) - into the %s hand\n",
                        m.frame, rec.inHand, rr::game::CheatWeaponName(rec.inHand).c_str(), m.lastSeen, rec.inHand,
                        rr::game::OwnedList(rec.owned).c_str(), h ? "right" : "left");
        } else if (rec.inHand >= 9) {
            for (auto& hd : m.hand)
                if (hd.held >= 0) {
                    ++m.totals.putAway;
                    std::printf("vr holsters: frame %ld - the game emptied the hand (weapon %d, still owned: %s)\n", m.frame,
                                hd.held, ((rec.owned >> hd.held) & 1) ? "yes" : "no");
                    hd.held = -1;
                }
        }
    }
    m.hint = -1;
    m.lastSeen = rec.inHand;
    m.owned = rec.owned;
    if (gained != 0) m.AutoAssign();
}

// ------------------------------------------------------------------------------------------------ the script
void VrHolsters::Impl::RunScript(long f) {
    while (nextEvent < script.size() && script[nextEvent].frame <= f) {
        const ScriptEvent& e = script[nextEvent++];
        if (e.command == "button") {
            static const char* const kNames = "ABXY";
            for (int k = 0; k < 4; ++k)
                if (e.text[0] == kNames[k]) scriptButtonUntil[k] = f + std::max(1L, static_cast<long>(e.v[0]));
            std::printf("vr holster script: frame %ld - Touch %s held %d frame(s)\n", f, e.text.c_str(), int(e.v[0]));
        } else if (e.command == "menu") {
            std::string note;
            const int row = static_cast<int>(e.v[0]), dir = static_cast<int>(e.v[1]);
            const int r = WeaponsMenuActivate(VrPrefs().weapons, row, dir, note);
            const std::vector<WeaponMenuRow> rows = WeaponsMenuRows(VrPrefs().weapons);
            std::printf("vr holster script: frame %ld - the menu's Weapons and holsters row %d (%s) %+d: %s -> %s\n", f, row,
                        row < static_cast<int>(rows.size()) ? rows[static_cast<size_t>(row)].label.c_str() : "?", dir,
                        r ? "changed" : "no change",
                        row < static_cast<int>(rows.size()) ? rows[static_cast<size_t>(row)].value.c_str() : "");
        } else if (e.command == "calib") {
            if (e.v[0] < 0) {
                SetCalibPageOpen(false, -1, -1);
                scriptCalib = false;
            } else {
                SetCalibPageOpen(false, -1, -1);
                SetCalibPageOpen(true, static_cast<int>(e.v[0]), e.hand);
                scriptCalib = true;
            }
        } else if (e.command == "shot") {
            shotPending = e.text;
        } else if (e.command == "gripcheck") {
            gripCheck = static_cast<int>(e.v[0]);
        } else { // draw / stow / release / align: a hand's action
            const int h = e.command == "align" ? 1 - CalibHand() : e.hand;
            Action& a = action[h];
            if (a.active) { // the hand is busy: this action waits for it
                --nextEvent;
                break;
            }
            a = Action{};
            a.active = true;
            a.kind = e.command;
            a.phase = -1; // started on the pose of the frame (ScriptPose)
            a.side = e.side;
            if (e.command == "stow" && a.side < 0) {
                const int own = OwnHolster(hand[h].held);
                a.side = own >= 0 ? own : h;
            }
            a.frames = static_cast<long>(e.command == "release" ? e.v[0] : e.v[5]);
            a.delta = {e.v[0], e.v[1], e.v[2]};
            a.pitch = e.v[3], a.yaw = e.v[4];
            std::printf("vr holster script: frame %ld - %s %s%s\n", f, e.command.c_str(), h ? "right" : "left",
                        e.command == "draw" || e.command == "stow" ? (a.side ? " at the right holster" : " at the left holster")
                                                                  : "");
        }
    }
}

// Hand h's pose while an action runs (seat space); false: no action (the melee script's pose stands).
bool VrHolsters::Impl::ScriptPose(int h, long f, rr::xr::HandPose& pose, float& grip, const rr::xr::HandPose poses[2]) {
    Action& a = action[h];
    if (!a.active) return false;
    const V3 live = Make(pose.grip);
    const auto moveFrames = [](const V3& x, const V3& y) {
        return std::max(12L, static_cast<long>(std::ceil(Len(Sub(x, y)) / kScriptSpeed / kScriptFrame)));
    };
    const auto next = [&](int phase, long frames) {
        a.phase = phase;
        a.phaseStart = f;
        a.phaseFrames = std::max(1L, frames);
    };
    if (a.phase < 0) { // the start: where the hand is now
        a.from = live;
        std::memcpy(a.quat, pose.grip + 3, sizeof(a.quat));
        if (a.kind == "draw" || a.kind == "stow") {
            a.at = holsterLocal[a.side];
            next(0, moveFrames(a.from, a.at));
        } else if (a.kind == "release") {
            next(0, std::max(1L, a.frames));
        } else { // align: 10 cm before the holding hand's palm, along its forward
            const rr::xr::HandPose& hp = poses[1 - h];
            float c[9];
            rr::xr::QuatToMatrix3(hp.grip + 3, c);
            const V3 fwd{-c[6], -c[7], -c[8]};
            a.at = Add(Make(hp.grip), Mul(fwd, 0.10f));
            next(0, moveFrames(a.from, a.at));
        }
    }
    const long in = f - a.phaseStart;
    const float t = std::min(1.0f, static_cast<float>(in) / static_cast<float>(a.phaseFrames));
    V3 p = live;
    float q[4];
    std::memcpy(q, pose.grip + 3, sizeof(q));
    grip = 0.0f;
    const bool done = in >= a.phaseFrames;
    if (a.kind == "release") {
        grip = 0.0f;
        if (done) a.active = false;
    } else if (a.kind == "draw" || a.kind == "stow") {
        const bool drawing = a.kind == "draw";
        switch (a.phase) {
        case 0: p = Lerp(a.from, a.at, t), grip = drawing ? 0.0f : 1.0f; if (done) next(1, 8); break;
        case 1: p = a.at, grip = drawing ? 0.0f : 1.0f; if (done) next(2, 10); break;
        case 2: p = a.at, grip = drawing ? 1.0f : 0.0f; if (done) next(3, moveFrames(a.at, live)); break;
        default: p = Lerp(a.at, live, t), grip = drawing ? 1.0f : 0.0f; if (done) a.active = false; break;
        }
    } else { // align
        switch (a.phase) {
        case 0: p = Lerp(a.from, a.at, t); if (done) next(1, 6); break;
        case 1: p = a.at; if (done) next(2, 6); break;
        case 2: p = a.at, grip = 1.0f; if (done) next(3, std::max(1L, a.frames)); break;
        case 3:
        case 4: {
            const float k = a.phase == 3 ? t : 1.0f;
            const rr::xr::HandPose& hp = poses[1 - h];
            float c[9];
            rr::xr::QuatToMatrix3(hp.grip + 3, c);
            const V3 right{c[0], c[1], c[2]}, up{c[3], c[4], c[5]}, fwd{-c[6], -c[7], -c[8]};
            p = Add(a.at, Mul(Add(Add(Mul(right, a.delta[0]), Mul(up, a.delta[1])), Mul(fwd, a.delta[2])), k));
            float qp[4], qy[4], r1[4], r2[4];
            QAxis(right, a.pitch * k * 3.14159265f / 180.0f, qp);
            QAxis(up, a.yaw * k * 3.14159265f / 180.0f, qy);
            QMul(qy, qp, r1);
            QMul(r1, a.quat, r2);
            std::memcpy(q, r2, sizeof(q));
            grip = a.phase == 3 ? 1.0f : 0.0f;
            if (done) {
                if (a.phase == 3) {
                    a.from = p; // where it lets go
                    std::memcpy(a.quat, q, sizeof(q));
                    next(4, 8);
                } else {
                    next(5, moveFrames(p, live));
                }
            }
            break;
        }
        default:
            p = Lerp(a.from, live, t);
            std::memcpy(q, a.quat, sizeof(q));
            if (done) a.active = false;
            break;
        }
    }
    for (int k = 0; k < 3; ++k) pose.grip[k] = pose.aim[k] = p[k];
    for (int k = 0; k < 4; ++k) pose.grip[3 + k] = pose.aim[3 + k] = q[k];
    pose.gripValid = pose.aimValid = true;
    return true;
}

void HolsterScriptHands(long frame, bool paused, rr::xr::HandPose poses[2], float grip[2]) {
    VrHolsters::Impl* m = g_state;
    if (m == nullptr || m->script.empty()) return;
    if (!paused) m->RunScript(frame);
    const rr::xr::HandPose before[2] = {poses[0], poses[1]};
    for (int h = 0; h < 2; ++h) {
        float g = grip[h];
        if (m->ScriptPose(h, frame, poses[h], g, before)) grip[h] = g;
    }
}

// ------------------------------------------------------------------------------------------------ the hands
void VrHolsters::UpdateHands(VrHost& vr, const rr::xr::WorldAnchor& anchor, const VrMelee& melee, const VrHandlebars& bars,
                             bool headView, bool paused, long frame) {
    Impl& m = *impl_;
    WeaponsSettings& s = VrPrefs().weapons;
    m.frame = frame;
    m.anchor = anchor;
    m.upm = anchor.unitsPerMetre > 0.0f ? anchor.unitsPerMetre : 1.0f;
    m.headView = headView;
    m.paused = paused;
    m.clock += vr.DisplayPeriod();
    if (!m.shotPending.empty()) {
        vr.RequestShot(m.shotPending);
        m.shotPending.clear();
    }
    // the hands, as the contact test saw them this frame
    HandFrameW frames[2];
    float grips[2] = {0.0f, 0.0f};
    bool fresh[2] = {false, false}, released[2] = {false, false};
    for (int h = 0; h < 2; ++h) {
        Impl::Hand& hd = m.hand[h];
        MeleeHandView v;
        hd.valid = melee.Hand(h, v) && v.valid;
        hd.grabbed = hd.valid && v.grabbed;
        hd.grip = hd.valid ? v.grip : 0.0f;
        if (hd.valid) {
            hd.frame.valid = true;
            hd.frame.left = h == 0; // the left hand holds the mirror image of the grip
            for (int k = 0; k < 3; ++k) {
                hd.frame.pos[k] = v.pos[k], hd.frame.right[k] = v.right[k], hd.frame.up[k] = v.up[k];
                hd.frame.fwd[k] = v.fwd[k], hd.frame.aim[k] = v.aim[k];
            }
            hd.local = Make(v.local);
        } else {
            hd.frame.valid = false;
        }
        frames[h] = hd.frame;
        grips[h] = hd.grip;
        fresh[h] = hd.valid && hd.grip >= kGripOn && !hd.down;
        released[h] = hd.down && (!hd.valid || hd.grip <= kGripOff);
        if (!hd.valid || hd.grip <= kGripOff) hd.down = false;
        else if (hd.grip >= kGripOn) hd.down = true;
        if (hd.pulse > 0.0f) hd.pulse -= static_cast<float>(vr.DisplayPeriod());
        if (hd.pulse <= 0.0f) hd.pulseAmp = 0.5f; // (the prod's feedback sets its own)
    }
    if (m.gripCheck >= 0) { // the script's `gripcheck W`
        std::printf("%s (frame %ld)\n", GripMirrorCheck(m.gripCheck, frames, m.upm, anchor.origin, anchor.right).c_str(), frame);
        m.gripCheck = -1;
    }
    // the calibration page: its weapon in its hand, the other hand moving it
    if (CalibPageOpen() && !vr.MenuOpen() && !m.scriptCalib) SetCalibPageOpen(false, -1, -1);
    const bool calib = CalibPageOpen();
    if (calib && CalibOtherHand(frames, grips, m.upm, s)) {
        ++m.totals.calibSaves;
        vr.SaveSettings();
    }
    // the holsters: the seat's space (the player's body)
    for (int side = 0; side < 2; ++side) {
        m.holsterLocal[side] = {(side ? 1.0f : -1.0f) * static_cast<float>(s.spreadCm) / 100.0f,
                                -static_cast<float>(s.heightCm) / 100.0f, -static_cast<float>(s.forwardCm) / 100.0f};
        m.holster[side] = m.FromLocal(m.holsterLocal[side]);
    }
    const bool on = m.loaded && s.holsters && m.started;
    if (!on && m.wasOn && m.started) { // the holsters turned off: the weapon last held (or one owned) back in the hand
        for (auto& hd : m.hand) hd.held = -1;
        if (m.gameInHand >= 9 && m.owned != 0) {
            int w = m.lastDrawn >= 0 && ((m.owned >> m.lastDrawn) & 1) ? m.lastDrawn : -1;
            for (int k = 0; k <= 8 && w < 0; ++k)
                if ((m.owned >> k) & 1) w = k;
            m.want = w;
            std::printf("vr holsters: frame %ld - holsters off: weapon %d back in the rider's hand\n", frame, w);
        }
    }
    if (on && !m.wasOn && m.gameInHand < 9) { // turned on with a weapon in the hand: it is in the weapon hand
        const int h = VrPrefs().melee.weaponHand == 0 ? 0 : 1;
        m.hand[h].held = m.gameInHand;
    }
    m.wasOn = on;
    m.active = on && headView;
    const float reach = static_cast<float>(s.reachCm) / 100.0f;
    const auto nearest = [&](const Impl::Hand& hd) {
        int best = -1;
        float bestD = reach;
        for (int side = 0; side < 2; ++side) {
            const float d = Len(Sub(Make(hd.frame.pos), m.holster[side])) / m.upm;
            if (d <= bestD) bestD = d, best = side;
        }
        return best;
    };
    const auto owns = [&](int w) { return m.Usable(w); };
    int* assigned[2] = {&s.left, &s.right};
    if (m.active && !paused && !calib) {
        ++m.totals.holsterFrames;
        for (int h = 0; h < 2; ++h) {
            Impl::Hand& hd = m.hand[h];
            Impl::Hand& other = m.hand[1 - h];
            if (hd.held < 0 && fresh[h] && !hd.grabbed) { // a fresh squeeze at a holster draws its weapon
                const int side = nearest(hd);
                const int w = side >= 0 ? *assigned[side] : -1;
                if (side >= 0 && owns(w)) {
                    if (other.held >= 0) {
                        std::printf("vr holsters: frame %ld - weapon %d leaves the %s hand for its holster (one weapon at "
                                    "a time)\n", frame, other.held, h ? "left" : "right");
                        other.held = -1;
                    }
                    hd.held = w;
                    m.lastDrawn = w;
                    m.want = w;
                    ++m.totals.draws[side];
                    hd.pulse = 0.08f;
                    m.Note(std::string("draw:") + std::to_string(w) + (h ? "R" : "L") + "@" + (side ? "R" : "L") + "f" +
                           std::to_string(frame));
                    std::printf("vr holsters: frame %ld - the %s hand DRAWS weapon %d (%s) from the %s holster (%.0f mm from "
                                "it)\n",
                                frame, h ? "right" : "left", w, rr::game::CheatWeaponName(w).c_str(), side ? "right" : "left",
                                double(Len(Sub(Make(hd.frame.pos), m.holster[side])) / m.upm * 1000.0f));
                }
            } else if (hd.held >= 0 && released[h]) { // the grip let go
                const int w = hd.held;
                const int side = hd.valid ? nearest(hd) : -1;
                if (side >= 0) { // at a holster: into THAT holster (the assignments swap)
                    const int own = m.OwnHolster(w);
                    if (own != side) {
                        const int there = *assigned[side];
                        *assigned[side] = w;
                        if (own >= 0) *assigned[own] = there;
                        ++m.totals.swaps;
                        std::printf("vr holsters: frame %ld - weapon %d now lives on the %s holster (%s there before)\n",
                                    frame, w, side ? "right" : "left", HolsterWeaponLabel(there).c_str());
                    }
                    ++m.totals.backsAt;
                } else if (s.gripLock) {
                    ++m.totals.gripLockKept;
                    continue;
                }
                hd.held = -1;
                m.want = 9;
                ++m.totals.backs;
                hd.pulse = 0.06f;
                m.Note(std::string("back:") + std::to_string(w) + (h ? "R" : "L") + (side >= 0 ? "@holster" : "@away") + "f" +
                       std::to_string(frame));
                std::printf("vr holsters: frame %ld - the %s hand lets go of weapon %d %s: %s\n", frame, h ? "right" : "left", w,
                            side >= 0 ? "at a holster" : "away from the holsters",
                            m.OwnHolster(w) == 1   ? "back to the right holster"
                            : m.OwnHolster(w) == 0 ? "back to the left holster"
                                                   : "stowed (still owned; no holster has it - the menu gives it one)");
            }
        }
    }
    // the weapon in the hand: the calibrated grip (weapon_draw.h's override, over vr_melee.cpp's)
    const int heldHand = m.HeldHand();
    const int weapon = calib ? CalibWeapon() : m.active ? m.HeldWeapon() : m.gameInHand;
    const int inHand = calib ? CalibHand() : m.active ? heldHand : HolsterWeaponHand(bars.WeaponHand());
    // (a weapon hand on the bars - where the buttons attack - still shows the weapon in its glove)
    const bool drawn = weapon >= 0 && weapon <= 8 && inHand >= 0 && m.hand[inHand].valid &&
                       (!m.hand[inHand].grabbed || (m.active && m.hand[inHand].held >= 0 && VrPrefs().melee.ButtonsAttack()));
    // (holsters off: only where vr_melee.cpp / vr_handlebars.cpp already put the weapon into a tracked hand)
    if (headView && m.loaded && (calib || m.active || (drawn && rr::render::CurrentWeaponHandOverride().active))) {
        rr::render::WeaponHandOverride o = rr::render::CurrentWeaponHandOverride();
        o.active = true;
        o.rider = m.rider;
        o.hidden = !drawn;
        o.unheld = drawn ? weapon : -1;
        o.useMatrix = drawn && WeaponInHandMatrix(weapon, m.hand[inHand].frame, m.upm, s.grip[weapon], o.matrix);
        // and the same grip as origin / along / side, for whatever reads the override that way (vr_nunchaku.h Observe)
        if (o.useMatrix) WeaponInHandFrame(weapon, m.hand[inHand].frame, m.upm, s.grip[weapon], o.origin, o.along, o.side);
        rr::render::SetWeaponHandOverride(o);
        m.overrideSet = true;
    }
    // the weapons on the hips
    std::vector<rr::render::ExtraWeaponDraw> extras;
    m.inReach[0] = m.inReach[1] = false;
    if (m.active || (calib && headView && on)) {
        const V3 down = Mul(Make(anchor.up), -1.0f), back = Mul(Make(anchor.ahead), -1.0f);
        for (int side = 0; side < 2; ++side) {
            const int w = *assigned[side];
            if (!owns(w) || m.HeldWeapon() == w) continue;
            const V3 out = Mul(Make(anchor.right), side ? 1.0f : -1.0f);
            rr::render::ExtraWeaponDraw e;
            e.group = w;
            if (!WeaponHolsteredMatrix(w, m.holster[side].data(), down.data(), back.data(), out.data(), e.matrix)) continue;
            extras.push_back(e);
            for (const auto& hd : m.hand)
                if (hd.valid && hd.held < 0 && !hd.grabbed && Len(Sub(Make(hd.frame.pos), m.holster[side])) / m.upm <= reach)
                    m.inReach[side] = true;
        }
        m.totals.drawnHolsters += extras.size();
    }
    rr::render::SetExtraWeapons(extras);
    // a click on the drawing / holstering hand (over the others' levels this frame)
    if (m.hand[0].pulse > 0.0f || m.hand[1].pulse > 0.0f) {
        float lv[2] = {0.0f, 0.0f};
        bars.Haptics(lv);
        vr.SetHandHaptics(std::max(lv[0], m.hand[0].pulse > 0.0f ? m.hand[0].pulseAmp : 0.0f),
                          std::max(lv[1], m.hand[1].pulse > 0.0f ? m.hand[1].pulseAmp : 0.0f));
    }
}

// ------------------------------------------------------------------------------------------------ the buttons
void VrHolsters::FilterButtons(rr::platform::GamepadState& gp, const VrHost& vr, const VrMelee& melee, long frame) {
    Impl& m = *impl_;
    const WeaponsSettings& s = VrPrefs().weapons;
    using rr::platform::Bit;
    using rr::platform::PadButton;
    // the mock's scripted Touch buttons, through the VR bindings as a headset's (game_host_vr.cpp TouchToPhysical)
    bool bDown = (vr.TouchState().touch & rr::xr::kTouchB) != 0;
    bool any = false;
    rr::platform::PhysicalPad p;
    p.connected = true;
    p.lx = p.ly = p.rx = p.ry = 0x80;
    static const PadButton kPos[4] = {PadButton::Cross, PadButton::Circle, PadButton::Square, PadButton::Triangle};
    for (int k = 0; k < 4; ++k)
        if (frame < m.scriptButtonUntil[k]) {
            p.buttons |= Bit(kPos[k]);
            any = true;
            if (k == 1) bDown = true;
        }
    if (any) {
        const rr::platform::GamepadState t = rr::platform::StateFromPhysical(p, rr::platform::VrBindings());
        gp.r1 = gp.r1 || t.r1;
        gp.l1 = gp.l1 || t.l1;
        gp.r2 = gp.r2 || t.r2;
        gp.padUp = gp.padUp || t.padUp;
        gp.padDown = gp.padDown || t.padDown;
        gp.toBike = gp.toBike || t.toBike; // A off the bike (the mock's scripted A)
    }
    const bool bEdge = bDown && !m.bWas;
    m.bWas = bDown;
    // the prod / the stun gun in a tracked hand: B is its discharge (not the kick it is bound to)
    const int h = m.HeldHand();
    const int w = m.HeldWeapon();
    const bool prod = s.prodButton && m.active && !m.paused && h >= 0 && (w == 6 || w == 7) && m.hand[h].valid;
    if (prod && bDown) {
        const rr::platform::Bindings& vb = rr::platform::VrBindings();
        for (int k = 0; k < rr::platform::kBindSlots; ++k)
            if (vb.pad[static_cast<int>(rr::platform::GameAction::Attack3)][k] == PadButton::Circle) gp.r2 = false;
    }
    if (prod && bEdge) {
        ++m.totals.discharges;
        float root[3], tip[3];
        MeleeTouch touch;
        const float radius = (0.05f + kDischargeReach) * m.upm;
        const double since = m.clock - m.lastDischarge;
        const bool reaches = WeaponInHandSegment(w, m.hand[h].frame, m.upm, s.grip[w],
                                                 static_cast<float>(VrPrefs().melee.weaponPct), root, tip) &&
                             melee.Touching(root, tip, radius, touch);
        if (reaches && since < static_cast<double>(s.cooldownMs) / 1000.0) {
            ++m.totals.dischargeCool;
            std::printf("vr holsters: frame %ld - B: the %s hand's weapon %d is recharging (%.2f s since its last blow, the "
                        "blow cooldown %d ms)\n", frame, h ? "right" : "left", w, since, s.cooldownMs);
        } else if (reaches) {
            m.lastDischarge = m.clock;
            ++m.totals.dischargeBlows;
            m.discharge = rr::game::PhysicalBlowRequest{};
            m.discharge.pending = true;
            m.discharge.victim = touch.bike;
            m.discharge.right = h == 1;
            m.discharge.weapon = true;
            m.discharge.part = touch.part;
            m.hand[h].pulse = 0.15f;
            m.Note("discharge:b" + std::to_string(touch.index) + "f" + std::to_string(frame));
            std::printf("vr holsters: frame %ld - B: the %s hand's %s (weapon %d) DISCHARGES on rival b%zu part %d (%.0f mm "
                        "from its surface) - a blow through the fight code\n",
                        frame, h ? "right" : "left", w == 6 ? "prod" : "stun gun", w, touch.index, touch.part,
                        double(std::max(0.0f, touch.gap) / m.upm * 1000.0f));
        } else if (s.prodReach && !m.watch.active) {
            // no rider touched - B is the ORIGINAL's attack with these weapons: combat action 6 (L1 + d-pad
            // Up, the combat input map 0x800CCB78; actions 1 / 2 are only a taunt with 6 / 7), for which CombatDecode
            // RASHCDG 0x800C2348 issues command 148 and FightPush the target the fight code picks itself; the swing's
            // strike is FightUpdate's with ReachTest 0x800C159C - the original's own reach decides, not a contact
            ++m.totals.attacks;
            m.attackPress = kAttackPressFrames;
            m.watch = Impl::Watch{};
            m.watch.active = true;
            m.watch.hand = h, m.watch.weapon = w, m.watch.frame = frame, m.watch.clock = m.clock;
            m.Note("attack:" + std::to_string(w) + "f" + std::to_string(frame));
            std::printf("vr holsters: frame %ld - B: the %s hand's %s (weapon %d) touches no rider - the ORIGINAL's attack: "
                        "combat action 6 (L1 + d-pad Up) -> CombatDecode 0x800C2348's command 148, the fight code's own "
                        "target and ReachTest 0x800C159C\n",
                        frame, h ? "right" : "left", w == 6 ? "prod" : "stun gun", w);
        } else {
            ++m.totals.dischargeAir;
            m.hand[h].pulse = 0.05f;
            std::printf("vr holsters: frame %ld - B: the %s hand's weapon %d discharges into the air (no rider within %.0f cm "
                        "of it%s)\n", frame, h ? "right" : "left", w, double(kDischargeReach * 100.0f),
                        s.prodReach ? "; the original's attack still under way" : "");
        }
    }
    // the combat mode alone decides whether the buttons attack, in both steering modes: the mode Physical only - they
    // do not; every other mode - they go to the pad as bound (A R1, X L1, B R2)
    if (!VrPrefs().melee.ButtonsAttack() && (gp.r1 || gp.l1 || gp.r2)) {
        ++m.totals.suppressed;
        if (m.totals.suppressed <= 4 || m.totals.suppressed % 100 == 0)
            std::printf("vr holsters: frame %ld - combat Physical only: the combat buttons (%s%s%s) do not attack (%zu "
                        "frame(s))\n", frame, gp.r1 ? "R1 " : "", gp.l1 ? "L1 " : "", gp.r2 ? "R2" : "", m.totals.suppressed);
        gp.r1 = gp.l1 = gp.r2 = false;
    } else if ((gp.r1 || gp.l1) && m.gameInHand >= 6 && m.gameInHand <= 8) {
        // A / X swing the weapon in hand (riderDef +0x2E) through the ORIGINAL's pad path; with the weapons
        // whose actions 1 / 2 are only a taunt they press that weapon's own attack, as the Handlebars gesture does
        // (CombatDecode RASHCDG 0x800C2348: 6 / 7 action 6 = L1 + Up, 8 action 5 = R1 + Up - command 148)
        const bool spray = m.gameInHand == 8;
        if (m.weaponPresses++ < 4 || m.weaponPresses % 100 == 0)
            std::printf("vr holsters: frame %ld - %s with weapon %d in hand: its own attack, combat action %d (%s + Up)\n", frame,
                        gp.r1 ? "A (R1)" : "X (L1)", m.gameInHand, spray ? 5 : 6, spray ? "R1" : "L1");
        gp.r1 = spray;
        gp.l1 = !spray;
        gp.padUp = true;
    }
    // the prod / stun gun's own attack on the pad bits (after the Handlebars rule - B with them is always an
    // attack), held as long as the desktop's --punch presses a combat action
    if (m.attackPress > 0) {
        gp.l1 = true;
        gp.padUp = true;
        --m.attackPress;
    }
}

void VrHolsters::ApplyToPad(rr::game::PadState& pad, rr::game::RaceSession& s, long frame) {
    Impl& m = *impl_;
    if (m.want >= 0 && m.want != m.gameInHand && m.rd != 0 && s.MutableArenaRam() != nullptr) {
        uint8_t* sw = s.MutableArenaRam() + ((m.rd + 0x2Fu) & 0x1FFFFFu);
        const int sprayBefore = m.gameInHand == 8 ? *sw : -1; // the spray's swings live only in +0x2F
        const rr::game::WeaponWrite r = m.want >= 9 ? rr::game::HolsterWeapon(s.MutableArenaRam(), m.rider, m.rd)
                                                    : rr::game::DrawWeapon(s.MutableArenaRam(), m.rider, m.rd, m.want);
        if (r == rr::game::WeaponWrite::kDone) {
            if (sprayBefore >= 0) m.spraySwings = sprayBefore;
            if (m.want == 8 && m.spraySwings >= 0) *sw = static_cast<uint8_t>(m.spraySwings); // (OURS: kept while holstered)
            ++m.totals.writes;
            std::printf("vr holsters: frame %ld - the record's weapon in hand %d -> %d (+0x2F %d)\n", frame, m.gameInHand,
                        m.want, s.ArenaRam()[(m.rd + 0x2Fu) & 0x1FFFFFu]);
            m.lastSeen = m.gameInHand = m.want;
            m.want = -1;
        } else if (r == rr::game::WeaponWrite::kDeferred) {
            ++m.totals.deferred;
        } else {
            std::printf("vr holsters: frame %ld - weapon %d not written: %s\n", frame, m.want, rr::game::WeaponWriteName(r));
            m.want = -1;
        }
    } else if (m.want >= 0 && m.want == m.gameInHand) {
        m.want = -1;
    }
    if (m.discharge.pending && !pad.blow.pending) {
        pad.blow = m.discharge;
        m.discharge = rr::game::PhysicalBlowRequest{};
    }
    // the prod / stun gun's original attack, watched until the fight code resolves it - the player's entry
    // of the last-blow table (NoteHit 0x800BF424 stamps it on a miss too) and his landed-blows stat FightStat(me, 2, 0)
    // (ReachTest's reach bit) - for the hand's feedback: a hard thump when it lands, a buzz when it does not
    if (m.watch.active && s.ArenaRam() != nullptr) {
        namespace F = rr::sim::fight;
        const uint8_t* ram = s.ArenaRam();
        const auto u32 = [&](uint32_t a) {
            uint32_t v = 0;
            std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
            return v;
        };
        const uint32_t victim = u32(F::kLastBlow + 4u), when = u32(F::kLastBlow + 8u);
        const uint8_t landed = ram[(F::kFightStats + 2u) & 0x1FFFFFu];
        Impl::Watch& wt = m.watch;
        Impl::Hand& hd = m.hand[wt.hand];
        if (!wt.based) {
            wt.based = true;
            wt.victim = victim, wt.time = when, wt.landed = landed;
            for (const rr::game::RaceBike& b : s.Bikes()) wt.health.push_back(ram[(b.riderDefAddress + 0x0Fu) & 0x1FFFFFu]);
        } else if (victim != wt.victim || when != wt.time) {
            size_t bike = 0;
            for (size_t i = 0; i < s.Bikes().size(); ++i)
                if (s.Bikes()[i].entityAddress == victim) bike = i;
            const int before = bike < wt.health.size() ? wt.health[bike] : -1;
            const int now = bike < s.Bikes().size() ? ram[(s.Bikes()[bike].riderDefAddress + 0x0Fu) & 0x1FFFFFu] : -1;
            const bool hit = landed != wt.landed;
            ++(hit ? m.totals.attackLanded : m.totals.attackMissed);
            hd.pulse = hit ? 0.20f : 0.30f;
            hd.pulseAmp = hit ? 1.0f : 0.30f;
            std::printf("vr holsters: frame %ld - the %s's original attack (B at frame %ld) %s rival b%zu: the fight code's "
                        "blow %s (health %d -> %d)%s\n",
                        frame, wt.weapon == 6 ? "prod" : "stun gun", wt.frame, hit ? "LANDS on" : "MISSES", bike,
                        hit ? "within ReachTest's reach" : "out of ReachTest's reach", before, now,
                        hit ? " - a hard thump on the hand" : " - a buzz on the hand");
            wt.active = false;
        } else if (m.clock - wt.clock > 1.5) {
            ++m.totals.attackNoBlow;
            hd.pulse = 0.30f;
            hd.pulseAmp = 0.30f;
            std::printf("vr holsters: frame %ld - the %s's original attack (B at frame %ld): no blow within 1.5 s (no rider "
                        "within the fight code's reach) - a buzz on the hand\n",
                        frame, wt.weapon == 6 ? "prod" : "stun gun", wt.frame);
            wt.active = false;
        }
    }
    // a weapon that arrives in this step (a bare-fisted blow's WeaponSteal, a snatch) comes to the hand that struck
    if (pad.blow.pending) m.hint = pad.blow.right ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------ drawing
void VrHolsters::Impl::Ring(const V3& c, const V3& a, const V3& b, float r, const V3& rgb) {
    constexpr int kSeg = 24;
    for (int i = 0; i < kSeg; ++i) {
        const float t0 = 6.2831853f * static_cast<float>(i) / kSeg, t1 = 6.2831853f * static_cast<float>(i + 1) / kSeg;
        const V3 p0 = Add(c, Add(Mul(a, r * std::cos(t0)), Mul(b, r * std::sin(t0))));
        const V3 p1 = Add(c, Add(Mul(a, r * std::cos(t1)), Mul(b, r * std::sin(t1))));
        lines.insert(lines.end(), {p0[0], p0[1], p0[2], rgb[0], rgb[1], rgb[2], p1[0], p1[1], p1[2], rgb[0], rgb[1], rgb[2]});
    }
}

void VrHolsters::Draw(const rr::render::Mat4& viewProj) {
    Impl& m = *impl_;
    const WeaponsSettings& s = VrPrefs().weapons;
    if (!m.active || !s.markers || !m.headView) return;
    m.lines.clear();
    const auto owns = [&](int w) { return m.Usable(w); };
    const int assigned[2] = {s.left, s.right};
    const V3 right = Make(m.anchor.right), up = Make(m.anchor.up), ahead = Make(m.anchor.ahead);
    for (int side = 0; side < 2; ++side) {
        if (!owns(assigned[side]) || m.HeldWeapon() == assigned[side]) continue;
        const V3 rgb = m.inReach[side] ? V3{0.15f, 1.0f, 0.3f} : V3{0.1f, 0.6f, 0.75f};
        const float r = static_cast<float>(s.reachCm) / 100.0f * m.upm * (m.inReach[side] ? 0.55f : 0.35f);
        m.Ring(m.holster[side], right, ahead, r, rgb);
        m.Ring(m.holster[side], up, ahead, r, rgb);
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
    glDisable(GL_BLEND);
    gl.BindVertexArray(m.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, m.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<rr::render::GLsizeiptr>(m.lines.size() * sizeof(float)), m.lines.data(),
                  GL_DYNAMIC_DRAW);
    gl.UseProgram(m.program);
    rr::render::UploadViewProj(m.viewProjLoc, viewProj);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m.lines.size() / 6));
    gl.BindVertexArray(0);
}

std::string VrHolsters::Totals() const {
    const Impl& m = *impl_;
    const auto& t = m.totals;
    char b[2400];
    std::snprintf(b, sizeof(b),
                  "vr holsters: %s - holster frames %zu (weapons drawn on the hips %zu); draws left %zu right "
                  "%zu, put back %zu (at a holster %zu, holster swaps %zu, kept by the grip lock %zu), the record written %zu "
                  "(deferred %zu), race-start stows %zu, arrivals %zu, taken by rivals %zu, emptied by the game %zu; the "
                  "prod's discharges %zu (blows %zu, in the air %zu, recharging %zu), the original's attack on B %zu "
                  "(landed %zu, missed %zu, no blow %zu); combat mode %s: button frames suppressed %zu, A / X "
                  "as a weapon's own attack %zu; swing vetoes: a weapon too slow %zu, a short swing %zu, not into the body %zu, cooldown "
                  "%zu, blows allowed %zu; grip calibrations saved %zu; owned at the end %s, in hand %d, holsters left %d "
                  "right %d;%s",
                  VrPrefs().weapons.Describe().c_str(), t.holsterFrames, t.drawnHolsters, t.draws[0], t.draws[1], t.backs,
                  t.backsAt, t.swaps, t.gripLockKept, t.writes, t.deferred, t.startStows, t.arrivals, t.stolen, t.putAway,
                  t.discharges, t.dischargeBlows, t.dischargeAir, t.dischargeCool, t.attacks, t.attackLanded, t.attackMissed,
                  t.attackNoBlow, MeleeModeName(VrPrefs().melee.mode), t.suppressed, m.weaponPresses, t.vetoSlow, t.vetoShort,
                  t.vetoInto, t.vetoCool, t.accepted, t.calibSaves, rr::game::OwnedList(m.owned).c_str(), m.gameInHand,
                  VrPrefs().weapons.left, VrPrefs().weapons.right, t.events.empty() ? " (no events)" : t.events.c_str());
    return b;
}

} // namespace rrgame
