// VR comfort (vr_comfort.h).
#include "vr_comfort.h"
#include "vr_pacing.h" // the judder log's path and columns

#include "render/multiview.h"
#include "render/shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace rrgame {

using rr::render::gl;

namespace {

bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }
uint32_t U32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
int32_t S32(const uint8_t* ram, uint32_t a) { return static_cast<int32_t>(U32(ram, a)); }
int16_t S16(const uint8_t* ram, uint32_t a) {
    int16_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
    return v;
}
uint16_t U16(const uint8_t* ram, uint32_t a) { return static_cast<uint16_t>(S16(ram, a)); }

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool Normalise(float v[3]) {
    const float n = std::sqrt(Dot(v, v));
    if (!(n > 1e-8f)) return false;
    for (int k = 0; k < 3; ++k) v[k] /= n;
    return true;
}

bool EnvIs(const char* name, const char* value) {
    const char* e = std::getenv(name);
    return e != nullptr && std::strcmp(e, value) == 0;
}

// ---- the command line
double& MockHz() {
    static double hz = 0.0;
    return hz;
}
struct PadEvent {
    long turn = 0, turns = 1;
    std::string what;
    float value = 0.0f, value2 = 0.0f;
};
std::vector<PadEvent>& PadScript() {
    static std::vector<PadEvent> s;
    return s;
}
bool& PadScriptOn() {
    static bool on = false;
    return on;
}
bool& PadRace() {
    static bool on = false;
    return on;
}

void ParsePadScript(const std::string& text) {
    std::vector<PadEvent>& out = PadScript();
    std::stringstream all(text);
    std::string item;
    while (std::getline(all, item, ';')) {
        std::stringstream in(item);
        PadEvent e;
        if (!(in >> e.turn >> e.what)) continue;
        if (e.what == "drift") {
            if (!(in >> e.value)) throw std::runtime_error("--vr-mock-pad: drift <x> [turns]");
        }
        if (e.what == "stick") { // the left stick held at (x, y): x right, y up, -1..1
            if (!(in >> e.value >> e.value2)) throw std::runtime_error("--vr-mock-pad: stick <x> <y> [turns]");
        }
        long n = 0;
        if (in >> n) e.turns = std::max(1L, n);
        static const char* const kKnown[] = {"chord", "up", "down", "a", "b", "menu", "ltrigger", "rtrigger",
                                             "stickleft", "stickright", "drift", "stick"};
        bool known = false;
        for (const char* k : kKnown) known = known || e.what == k;
        if (!known) throw std::runtime_error("--vr-mock-pad: unknown command '" + e.what + "'");
        out.push_back(e);
    }
    PadScriptOn() = true;
}

} // namespace

bool ApplyComfortFlag(int argc, char** argv, int& i) {
    const std::string a = argv[i];
    if (a == "--vr-mock-hz") {
        if (i + 1 >= argc) throw std::runtime_error("--vr-mock-hz needs a rate");
        MockHz() = std::atof(argv[++i]);
        if (!(MockHz() >= 20.0 && MockHz() <= 300.0)) throw std::runtime_error("--vr-mock-hz: 20..300");
        return true;
    }
    if (a == "--vr-mock-pad-race") {
        PadRace() = true;
        return true;
    }
    if (a == "--vr-mock-pad") {
        if (i + 1 >= argc) throw std::runtime_error("--vr-mock-pad needs a script");
        ParsePadScript(argv[++i]);
        return true;
    }
    return false;
}

double MockDisplayHz() { return MockHz() > 0.0 ? MockHz() : MockTimingOn() ? 72.0 : 0.0; } // --vr-mock-timing: 72 Hz
bool MockPadScripted() { return PadScriptOn(); }
bool MockPadDrivesRace() { return PadScriptOn() && PadRace(); }

bool MockPad(long turn, rr::xr::XrPad& pad) {
    if (!PadScriptOn()) return false;
    pad = rr::xr::XrPad{};
    pad.connected = true;
    for (const PadEvent& e : PadScript()) {
        if (turn < e.turn || turn >= e.turn + e.turns) continue;
        if (e.what == "chord") pad.touch |= rr::xr::kTouchLeftStick | rr::xr::kTouchRightStick;
        else if (e.what == "up") pad.stick[0][1] = 1.0f;
        else if (e.what == "down") pad.stick[0][1] = -1.0f;
        else if (e.what == "a") pad.touch |= rr::xr::kTouchA;
        else if (e.what == "b") pad.touch |= rr::xr::kTouchB;
        else if (e.what == "menu") pad.touch |= rr::xr::kTouchMenu;
        else if (e.what == "ltrigger") pad.leftTrigger = 1.0f;
        else if (e.what == "rtrigger") pad.rightTrigger = 1.0f;
        else if (e.what == "stickleft") pad.stick[0][0] = -1.0f;
        else if (e.what == "stickright") pad.stick[0][0] = 1.0f;
        else if (e.what == "drift") pad.stick[0][0] = e.value;
        else if (e.what == "stick") {
            pad.stick[0][0] = e.value;
            pad.stick[0][1] = e.value2;
        }
    }
    pad.source = "the mock's script";
    return true;
}

bool MenuStickChangesValues() {
    static const bool on = EnvIs("RRJB_VR_MENU_STICK", "values");
    return on;
}
bool HapticsHoldOff() {
    static const bool off = EnvIs("RRJB_VR_HAPTICS_HOLD", "off");
    return off;
}
bool FallViewLegacy() {
    static const bool legacy = EnvIs("RRJB_FALL_VIEW", "legacy");
    return legacy;
}

bool SmoothInterpOn() {
    static const bool on = !EnvIs("RRJB_SMOOTH", "stable");
    return on;
}
void LeashAlong(double x[3], const double z[3], const float fwd[3], double across, double along) {
    double e[3], a = 0.0;
    for (int k = 0; k < 3; ++k) e[k] = x[k] - z[k], a += e[k] * fwd[k];
    double c[3], c2 = 0.0;
    for (int k = 0; k < 3; ++k) c[k] = e[k] - a * fwd[k], c2 += c[k] * c[k];
    a = std::clamp(a, -along, along);
    const double s = c2 > across * across ? across / std::sqrt(c2) : 1.0;
    for (int k = 0; k < 3; ++k) x[k] = z[k] + a * fwd[k] + c[k] * s;
}

double SmoothAlongLeash() {
    static const double v = [] {
        const char* e = std::getenv("RRJB_SMOOTH_ALONG"); // DEVELOPMENT: 0.06 = the across leash (the control)
        return e != nullptr ? std::clamp(std::atof(e), 0.0, 1.0) : FrameInterp::kMaxOffAlong;
    }();
    return v;
}

bool SmoothStableOn() {
    static const bool on = !EnvIs("RRJB_SMOOTH", "interp");
    return on;
}

ComfortCounters& Comfort() {
    static ComfortCounters c;
    return c;
}

std::string ComfortTotals() {
    const ComfortCounters& c = Comfort();
    char b[1400];
    std::snprintf(b, sizeof(b),
                  "vr comfort: menu opened %zu, value changes by the triggers %zu, by the stick's left / right "
                  "%zu, by the keys %zu, confirms %zu, rows moved %zu%s; haptics: %zu turn(s) held by a menu / the pause / "
                  "not racing (%zu with the VR menu, %zu paused) - xrApplyHapticFeedback %llu (VR menu %llu, pause %llu), "
                  "stop %llu; %zu racing turn(s) - apply %llu, stop %llu%s; falls %zu, re-seats %zu, %zu frame(s) off the "
                  "bike (the anchor moves up to %.3f world units and turns up to %.2f deg a frame), %zu faded, the eye's point on the bike moved up to %.3f world units at a re-seat, "
                  "the head view back after a re-seat %zu frame(s) (chase %zu), the re-latch an eye without the re-seat rule would have "
                  "taken %.3f from the kept one%s; smooth motion: %zu frame(s) drawn between two steps, %zu entity "
                  "field(s), %zu cut(s), %zu restore(s), %zu restore mismatch(es); the head view's bike stabilised %zu frame(s), drawn off the game's bike RMS %.1f mm max %.1f, RMS %.2f deg max %.2f\n",
                  c.menuOpened, c.menuTrigger, c.menuStick, c.menuKeys, c.menuConfirm, c.menuRows,
                  MenuStickChangesValues() ? " (RRJB_VR_MENU_STICK=values)" : "", c.hapticHeldTurns, c.menuTurns,
                  c.pauseTurns, static_cast<unsigned long long>(c.appliesHeld),
                  static_cast<unsigned long long>(c.appliesMenu), static_cast<unsigned long long>(c.appliesPause),
                  static_cast<unsigned long long>(c.stopsHeld), c.hapticRacingTurns,
                  static_cast<unsigned long long>(c.appliesRacing), static_cast<unsigned long long>(c.stopsRacing),
                  HapticsHoldOff() ? " (RRJB_VR_HAPTICS_HOLD=off)" : "", c.falls, c.reseats, c.offFrames,
                  c.maxEyeStepOff, c.maxRollOff, c.fadeFrames, c.maxEyeStepReseat, c.headFramesAfterReseat,
                  c.chaseAfterReseat, c.latchDrift, FallViewLegacy() ? " (RRJB_FALL_VIEW=legacy)" : "", c.interpFrames,
                  c.interpEntities, c.interpCuts, c.restores, c.restoreMismatch, c.stabilised,
                  c.stabilised ? 1000.0 * std::sqrt(c.stabPosSq / static_cast<double>(c.stabilised)) : 0.0, 1000.0 * c.stabPosMax,
                  c.stabilised ? std::sqrt(c.stabAngSq / static_cast<double>(c.stabilised)) : 0.0, c.stabAngMax);
    return b;
}

// ---------------------------------------------------------------- FrameInterp
void FrameInterp::Capture(const uint8_t* ram) {
    prev_.swap(cur_);
    cur_.clear();
    if (ram == nullptr) return;
    // the entity pool table 0x800CE4D0 (docs\formats\population.md): base, stride, -> live, -> high, pools 0..4
    for (uint32_t pool = 0; pool <= 4; ++pool) {
        const uint32_t t = 0x800CE4D0u + 16u * pool;
        const uint32_t base = U32(ram, t), highPtr = U32(ram, t + 12u);
        const int32_t stride = S32(ram, t + 4u);
        if (!InRam(base) || !InRam(highPtr) || stride < 0x1C4 || stride > 0x1000) continue;
        const int32_t high = S32(ram, highPtr);
        for (int32_t i = 0; i <= high && i < 256; ++i) {
            const uint32_t e = base + static_cast<uint32_t>(stride) * static_cast<uint32_t>(i);
            if (!InRam(e + static_cast<uint32_t>(stride))) break;
            Entry en;
            en.addr = e;
            en.handle = U16(ram, e + 0xACu); // (pool 0 slot 0 - the player's bike - has the handle 0: kept; a released
                                             // slot is not drawn, and a slot taken again far away is a cut)
            en.pool = static_cast<uint8_t>(pool);
            for (uint32_t k = 0; k < 3; ++k) en.pos[k] = S32(ram, e + 0xB8u + 4u * k);
            if (pool == 1) {
                for (uint32_t k = 0; k < 3; ++k) en.root[k] = S16(ram, e + 0x1Cu + 2u * k);
            } else {
                for (uint32_t k = 0; k < 9; ++k) en.rows[k] = S16(ram, e + 0x1B0u + 2u * k);
            }
            if (pool == 0) en.owner = U32(ram, e + 0x354u);
            cur_.push_back(en);
        }
    }
    have_ = std::min(have_ + 1, 2);
}

void FrameInterp::Save(uint8_t* ram, uint32_t addr, uint8_t size) {
    for (const Saved& s : saved_)
        if (s.addr == addr && s.size == size) return; // the first saved bytes are the step's
    Saved s;
    s.addr = addr;
    s.size = size;
    std::memcpy(s.bytes, ram + (addr & 0x1FFFFFu), size);
    saved_.push_back(s);
    applied_ = true;
}

void FrameInterp::Write(uint8_t* ram, uint32_t addr, const void* data, uint8_t size) {
    if (ram == nullptr || size == 0 || size > 18) return;
    Save(ram, addr, size);
    std::memcpy(ram + (addr & 0x1FFFFFu), data, size);
}

bool FrameInterp::StepOrigin(uint32_t addr, double out[3]) const {
    for (const Entry& e : cur_)
        if (e.addr == addr) {
            for (int k = 0; k < 3; ++k) out[k] = static_cast<double>(e.pos[k]) / 65536.0;
            return true;
        }
    return false;
}

void FrameInterp::SetMoved(uint32_t addr, const float d[3]) {
    for (Moved& m : moved_)
        if (m.addr == addr) {
            std::memcpy(m.d, d, sizeof(m.d));
            return;
        }
    Moved m;
    m.addr = addr;
    std::memcpy(m.d, d, sizeof(m.d));
    moved_.push_back(m);
}

bool FrameInterp::Apply(uint8_t* ram, float alpha) {
    applied_ = false;
    saved_.clear();
    moved_.clear();
    stab_.axes = false;
    if (ram == nullptr || have_ < 2 || !(alpha < 0.999f)) return false;
    const float t = std::max(0.0f, alpha);
    const auto save = [&](uint32_t addr, uint8_t size) { Save(ram, addr, size); };
    size_t p = 0;
    for (const Entry& b : cur_) {
        // the same slot in the step before (both lists are in pool / slot order)
        while (p < prev_.size() && (prev_[p].pool < b.pool || (prev_[p].pool == b.pool && prev_[p].addr < b.addr))) ++p;
        if (p >= prev_.size() || prev_[p].addr != b.addr || prev_[p].handle != b.handle) continue;
        const Entry& a = prev_[p];
        // the arena must still hold the captured state (nothing wrote it since the step)
        bool same = true;
        for (uint32_t k = 0; k < 3; ++k) same = same && S32(ram, b.addr + 0xB8u + 4u * k) == b.pos[k];
        if (!same) continue;
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double d = (static_cast<double>(b.pos[k]) - static_cast<double>(a.pos[k])) / 65536.0;
            d2 += d * d;
        }
        if (d2 > kCut * kCut) {
            ++Comfort().interpCuts;
            continue;
        }
        int32_t pos[3];
        Moved mv;
        mv.addr = b.addr;
        for (int k = 0; k < 3; ++k) {
            const double v = static_cast<double>(a.pos[k]) + (static_cast<double>(b.pos[k]) - static_cast<double>(a.pos[k])) * t;
            pos[k] = static_cast<int32_t>(std::llround(v));
            mv.d[k] = static_cast<float>((static_cast<double>(pos[k]) - static_cast<double>(b.pos[k])) / 65536.0);
        }
        save(b.addr + 0xB8u, 12);
        std::memcpy(ram + ((b.addr + 0xB8u) & 0x1FFFFFu), pos, 12);
        moved_.push_back(mv);
        ++Comfort().interpEntities;
        if (b.pool == 1) {
            bool closeBy = true;
            for (int k = 0; k < 3; ++k) closeBy = closeBy && std::abs(static_cast<int>(b.root[k]) - static_cast<int>(a.root[k])) < 2000;
            if (!closeBy) continue;
            int16_t root[3];
            for (int k = 0; k < 3; ++k)
                root[k] = static_cast<int16_t>(std::lround(a.root[k] + (static_cast<float>(b.root[k]) - a.root[k]) * t));
            save(b.addr + 0x1Cu, 6);
            std::memcpy(ram + ((b.addr + 0x1Cu) & 0x1FFFFFu), root, 6);
        } else {
            bool closeBy = true;
            for (int k = 0; k < 9; ++k) closeBy = closeBy && std::abs(static_cast<int>(b.rows[k]) - static_cast<int>(a.rows[k])) < 2048;
            if (!closeBy) continue;
            int16_t rows[9];
            for (int k = 0; k < 9; ++k)
                rows[k] = static_cast<int16_t>(std::lround(a.rows[k] + (static_cast<float>(b.rows[k]) - a.rows[k]) * t));
            save(b.addr + 0x1B0u, 18);
            std::memcpy(ram + ((b.addr + 0x1B0u) & 0x1FFFFFu), rows, 18);
        }
    }
    // a rider on its bike is drawn at the bike (+ its root): its shift is the bike's
    for (const Entry& b : cur_) {
        if (b.pool != 0 || !InRam(b.owner)) continue;
        float d[3];
        if (!Shift(b.addr, d)) continue;
        bool found = false;
        for (Moved& m : moved_)
            if (m.addr == b.owner) {
                std::memcpy(m.d, d, sizeof(d));
                found = true;
            }
        if (!found) {
            Moved m;
            m.addr = b.owner;
            std::memcpy(m.d, d, sizeof(d));
            moved_.push_back(m);
        }
    }
    applied_ = !saved_.empty();
    if (applied_) ++Comfort().interpFrames;
    return applied_;
}

bool FrameInterp::Stabilize(uint8_t* ram, uint32_t bike, double dt, double stepSeconds, bool reset) {
    stab_.axes = false;
    if (ram == nullptr || !InRam(bike) || !(dt > 0.0)) return false;
    const Entry* now = nullptr;
    const Entry* before = nullptr;
    for (const Entry& e : cur_)
        if (e.addr == bike) now = &e;
    for (const Entry& e : prev_)
        if (e.addr == bike) before = &e;
    if (now == nullptr || now->pool != 0) return false;
    // what is drawn this frame (the step, or the interpolated state Apply wrote)
    double z[3];
    float f[3], u[3];
    for (uint32_t k = 0; k < 3; ++k) {
        z[k] = static_cast<double>(S32(ram, bike + 0xB8u + 4u * k)) / 65536.0;
        f[k] = static_cast<float>(S16(ram, bike + 0x1B0u + 12u + 2u * k)); // model axis 2: forward
        u[k] = -static_cast<float>(S16(ram, bike + 0x1B0u + 6u + 2u * k)); // model axis 1: down
    }
    if (!Normalise(f)) return false;
    {
        const float d = Dot(u, f);
        for (int k = 0; k < 3; ++k) u[k] -= d * f[k];
    }
    if (!Normalise(u)) return false;
    // heading, pitch, roll (roll as VisualLean: the up against the level up through forward, signed about forward)
    const float worldUp[3] = {0.0f, -1.0f, 0.0f};
    float lv[3] = {worldUp[0], worldUp[1], worldUp[2]};
    {
        const float d = Dot(lv, f);
        for (int k = 0; k < 3; ++k) lv[k] -= d * f[k];
    }
    if (!Normalise(lv)) return false; // standing on its nose: nothing to do
    const float cr[3] = {lv[1] * u[2] - lv[2] * u[1], lv[2] * u[0] - lv[0] * u[2], lv[0] * u[1] - lv[1] * u[0]};
    const double ang[3] = {std::atan2(double(f[0]), double(f[2])), std::asin(std::clamp(double(-f[1]), -1.0, 1.0)),
                           std::atan2(double(Dot(cr, f)), double(Dot(lv, u)))};
    Stab& st = stab_;
    double jump2 = 0.0;
    for (int k = 0; k < 3; ++k) jump2 += (z[k] - st.x[k]) * (z[k] - st.x[k]);
    if (reset || !st.have || st.bike != bike || jump2 > kCut * kCut) {
        st = Stab{};
        st.have = true;
        st.bike = bike;
        for (int k = 0; k < 3; ++k) {
            st.x[k] = z[k];
            st.v[k] = before != nullptr && stepSeconds > 0.0 && before->handle == now->handle
                          ? (static_cast<double>(now->pos[k]) - static_cast<double>(before->pos[k])) / 65536.0 / stepSeconds
                          : 0.0;
            st.a[k] = ang[k];
        }
    } else {
        const double pi = 3.14159265358979323846;
        const auto gh = [dt](double& x, double& v, double meas, double g) {
            const double h = g * g / (2.0 - g);
            const double pred = x + v * dt, r = meas - pred;
            x = pred + g * r;
            v += h * r / dt;
        };
        static const double* g = [] {
            static double v[2] = {kPosG, kAngG};
            if (const char* e = std::getenv("RRJB_SMOOTH_G")) std::sscanf(e, "%lf,%lf", &v[0], &v[1]);
            return v;
        }();
        for (int k = 0; k < 3; ++k) gh(st.x[k], st.v[k], z[k], g[0]);
        double yaw = ang[0]; // unwrapped against the filter's heading
        while (yaw - st.a[0] > pi) yaw -= 2 * pi;
        while (yaw - st.a[0] < -pi) yaw += 2 * pi;
        gh(st.a[0], st.w[0], yaw, g[1]);
        gh(st.a[1], st.w[1], ang[1], g[1]);
        gh(st.a[2], st.w[2], ang[2], g[1]);
        // the leash: kMaxOff across the bike / kMaxOffAlong along its forward (the game's own one-step
        // hitches along the road - 13..20 cm at a stoppie's start - are spread instead of passed on) / kMaxOffRad
        LeashAlong(st.x, z, f, kMaxOff, SmoothAlongLeash());
    }
    // the filtered frame: forward from heading / pitch, up = the level up turned by the roll about forward
    const auto frameOf = [&worldUp](const double a[3], float fo[3], float uo[3]) {
        fo[0] = static_cast<float>(std::cos(a[1]) * std::sin(a[0]));
        fo[1] = static_cast<float>(-std::sin(a[1]));
        fo[2] = static_cast<float>(std::cos(a[1]) * std::cos(a[0]));
        Normalise(fo);
        float l[3] = {worldUp[0], worldUp[1], worldUp[2]};
        const float d = Dot(l, fo);
        for (int k = 0; k < 3; ++k) l[k] -= d * fo[k];
        if (!Normalise(l)) return false;
        const float x[3] = {fo[1] * l[2] - fo[2] * l[1], fo[2] * l[0] - fo[0] * l[2], fo[0] * l[1] - fo[1] * l[0]};
        const float cs = static_cast<float>(std::cos(a[2])), sn = static_cast<float>(std::sin(a[2]));
        for (int k = 0; k < 3; ++k) uo[k] = cs * l[k] + sn * x[k];
        return true;
    };
    // The camera takes the filter as it is; the DRAWN bike stays within kMaxOffRad of the game's (the leash). They are
    // the same frame while the filter is within the leash (all ordinary riding: the eye rigid on the bike); a snap of
    // the game's pose (a landing: 17 degrees of pitch in a frame) turns the drawn bike at once and the view smoothly.
    double drawnAng[3];
    {
        double yaw = ang[0];
        while (yaw - st.a[0] > 3.14159265358979323846) yaw -= 2 * 3.14159265358979323846;
        while (yaw - st.a[0] < -3.14159265358979323846) yaw += 2 * 3.14159265358979323846;
        const double meas[3] = {yaw, ang[1], ang[2]};
        for (int k = 0; k < 3; ++k) drawnAng[k] = std::clamp(st.a[k], meas[k] - kMaxOffRad, meas[k] + kMaxOffRad);
    }
    float f2[3], u2[3], cf[3], cu[3];
    if (!frameOf(drawnAng, f2, u2) || !frameOf(st.a, cf, cu)) return false;
    // Q = B' B^T with B = [f, u, f x u]: turns every raw row (the model's axes, their scale kept) onto the filtered frame
    const auto cross = [](const float a[3], const float b[3], float o[3]) {
        o[0] = a[1] * b[2] - a[2] * b[1];
        o[1] = a[2] * b[0] - a[0] * b[2];
        o[2] = a[0] * b[1] - a[1] * b[0];
    };
    float r1[3], r2[3];
    cross(f, u, r1);
    cross(f2, u2, r2);
    const float* b1[3] = {f, u, r1};
    const float* b2[3] = {f2, u2, r2};
    double q[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int c = 0; c < 3; ++c) q[i][j] += static_cast<double>(b2[c][i]) * static_cast<double>(b1[c][j]);
    int16_t rows[9];
    for (uint32_t r = 0; r < 3; ++r) {
        double v[3];
        for (uint32_t k = 0; k < 3; ++k) v[k] = static_cast<double>(S16(ram, bike + 0x1B0u + 6u * r + 2u * k));
        for (uint32_t i = 0; i < 3; ++i) {
            const double o = q[i][0] * v[0] + q[i][1] * v[1] + q[i][2] * v[2];
            rows[3 * r + i] = static_cast<int16_t>(std::clamp<long long>(std::llround(o), -32768LL, 32767LL));
        }
    }
    int32_t pos[3];
    float d[3];
    for (int k = 0; k < 3; ++k) {
        pos[k] = static_cast<int32_t>(std::llround(st.x[k] * 65536.0));
        d[k] = static_cast<float>((static_cast<double>(pos[k]) - static_cast<double>(now->pos[k])) / 65536.0);
    }
    Save(ram, bike + 0xB8u, 12);
    Save(ram, bike + 0x1B0u, 18);
    std::memcpy(ram + ((bike + 0xB8u) & 0x1FFFFFu), pos, 12);
    std::memcpy(ram + ((bike + 0x1B0u) & 0x1FFFFFu), rows, 18);
    SetMoved(bike, d);
    if (InRam(now->owner)) SetMoved(now->owner, d);
    { // how far the drawn bike is from the game's (at the drawn time): the filter's price
        ComfortCounters& c = Comfort();
        double e2 = 0.0;
        for (int k = 0; k < 3; ++k) e2 += (st.x[k] - z[k]) * (st.x[k] - z[k]);
        const double ang1 = std::acos(std::clamp(static_cast<double>(Dot(f, f2)), -1.0, 1.0)),
                     ang2 = std::acos(std::clamp(static_cast<double>(Dot(u, u2)), -1.0, 1.0));
        const double deg = std::max(ang1, ang2) * 57.29577951308232;
        c.stabPosSq += e2;
        c.stabPosMax = std::max(c.stabPosMax, std::sqrt(e2));
        c.stabAngSq += deg * deg;
        c.stabAngMax = std::max(c.stabAngMax, deg);
    }
    std::memcpy(st.fwd, cf, sizeof(cf));
    std::memcpy(st.up, cu, sizeof(cu));
    st.axes = true;
    ++Comfort().stabilised;
    return true;
}

bool FrameInterp::StableAxes(float fwd[3], float up[3]) const {
    if (!stab_.axes) return false;
    std::memcpy(fwd, stab_.fwd, sizeof(stab_.fwd));
    std::memcpy(up, stab_.up, sizeof(stab_.up));
    return true;
}

void FrameInterp::Restore(uint8_t* ram) {
    if (!applied_ || ram == nullptr) return;
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) std::memcpy(ram + (it->addr & 0x1FFFFFu), it->bytes, it->size);
    stab_.axes = false;
    // the arena is the captured step again (the check the next Capture relies on)
    for (const Entry& b : cur_)
        for (uint32_t k = 0; k < 3; ++k)
            if (S32(ram, b.addr + 0xB8u + 4u * k) != b.pos[k]) {
                ++Comfort().restoreMismatch;
                break;
            }
    ++Comfort().restores;
    saved_.clear();
    moved_.clear();
    applied_ = false;
}

bool FrameInterp::Shift(uint32_t object, float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (!applied_) return false;
    for (const Moved& m : moved_)
        if (m.addr == object) {
            std::memcpy(out, m.d, sizeof(m.d));
            return true;
        }
    return false;
}

FrameInterp& ProductFrameInterp() {
    static FrameInterp f;
    return f;
}

// ---------------------------------------------------------------- the cameras
CamPose LerpCam(const CamPose& a, const CamPose& b, float t) {
    if (!a.valid || !b.valid || !(t < 1.0f)) return b;
    float d2 = 0.0f, fa[3], fb[3];
    for (int k = 0; k < 3; ++k) {
        d2 += (b.eye[k] - a.eye[k]) * (b.eye[k] - a.eye[k]);
        fa[k] = a.fwd[k];
        fb[k] = b.fwd[k];
    }
    if (!Normalise(fa) || !Normalise(fb)) return b;
    if (d2 > static_cast<float>(FrameInterp::kCut * FrameInterp::kCut) || Dot(fa, fb) < 0.7f) return b;
    CamPose c;
    c.valid = true;
    for (int k = 0; k < 3; ++k) {
        c.eye[k] = a.eye[k] + (b.eye[k] - a.eye[k]) * t;
        c.fwd[k] = a.fwd[k] + (b.fwd[k] - a.fwd[k]) * t;
        c.up[k] = a.up[k] + (b.up[k] - a.up[k]) * t;
    }
    if (!Normalise(c.fwd)) return b;
    const float d = Dot(c.up, c.fwd);
    for (int k = 0; k < 3; ++k) c.up[k] -= d * c.fwd[k];
    if (!Normalise(c.up)) return b;
    return c;
}

CamPose FallView::Frame(bool seated, const CamPose& head, const CamPose& chase, const float rider[3], bool fixed, float dt,
                        float& fade) {
    constexpr float kBehind = 3.2f, kAbove = 1.1f, kFollow = 0.2f, kFade = 0.3f;
    const float worldUp[3] = {0.0f, -1.0f, 0.0f}; // the PlayStation's Y points down
    changed_ = false;
    const auto heading = [&](const CamPose& from) {
        float h[3] = {from.fwd[0], 0.0f, from.fwd[2]};
        if (!Normalise(h)) h[0] = 0.0f, h[1] = 0.0f, h[2] = 1.0f;
        std::memcpy(heading_, h, sizeof(h));
    };
    const auto target = [&](float out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = rider[k] - heading_[k] * kBehind + worldUp[k] * kAbove;
    };
    if (!started_) {
        started_ = true;
        off_ = !seated;
        if (off_) {
            heading(chase.valid ? chase : head);
            target(eye_);
        }
    } else if (seated == off_) {
        changed_ = true;
        off_ = !seated;
        fade_ = 1.0f;
        if (off_) {
            heading(last_.valid ? last_ : chase);
            target(eye_);
        }
    } else {
        fade_ = std::max(0.0f, fade_ - dt / kFade);
    }
    fade = fade_;
    if (!off_) {
        last_ = head;
        return head;
    }
    if (!fixed) return chase;
    float want[3];
    target(want);
    const float k = 1.0f - std::exp(-std::max(0.0f, dt) / kFollow);
    for (int c = 0; c < 3; ++c) eye_[c] += (want[c] - eye_[c]) * k;
    CamPose c;
    c.valid = true;
    std::memcpy(c.eye, eye_, sizeof(eye_));
    std::memcpy(c.fwd, heading_, sizeof(heading_));
    std::memcpy(c.up, worldUp, sizeof(worldUp));
    return c;
}

// ---------------------------------------------------------------- the fade
namespace {
const char* const kFadeVs = R"(#version 330 core
// rr:multiview (drawn into both eyes' layers at once when the eyes are single-pass, render/multiview.h)
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
const char* const kFadeFs = R"(#version 330 core
uniform float uAlpha;
out vec4 oColor;
void main() { oColor = vec4(0.0, 0.0, 0.0, uAlpha); }
)";
} // namespace

void VrFade::Draw(float alpha) {
    if (alpha <= 0.004f) return;
    if (program_ == 0) {
        program_ = rr::render::BuildProgram(kFadeVs, kFadeFs);
        alphaLoc_ = gl.GetUniformLocation(program_, "uAlpha");
        gl.GenVertexArrays(1, &vao_);
    }
    gl.UseProgram(program_);
    gl.Uniform1f(alphaLoc_, std::min(alpha, 1.0f));
    gl.BindVertexArray(vao_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------- the motion meter
void MotionMeter::Frame(long frame, int ticks, float alpha, double period, const rr::xr::WorldAnchor& a,
                        const rr::render::Mat4* bike, const PacingRow* pacing) {
    if (!csvTried_) {
        csvTried_ = true;
        const std::string p = JudderLogPath(); // --vr-judder-log / RRJB_JUDDER_LOG (vr_pacing.h; the Quest: its files dir)
        if (!p.empty()) {
            csv_ = std::fopen(p.c_str(), "wb");
            std::printf("vr motion: the judder log %s %s\n", p.c_str(), csv_ ? "opened" : "could NOT be opened");
            if (csv_) std::fprintf(csv_, "frame,ticks,alpha,eyeX,eyeY,eyeZ,aheadX,aheadY,aheadZ,upX,upY,upZ,barX,barY,barZ,"
                                         "worldMm,bikeMm,bfX,bfY,bfZ,buX,buY,buZ,boX,boY,boZ,displayMs,loopMs,periodMs,carry,"
                                         "missed,drawnErrMs,pacing\n");
        }
    }
    Sample s;
    std::memcpy(s.eye, a.origin, sizeof(s.eye));
    std::memcpy(s.right, a.right, sizeof(s.right));
    std::memcpy(s.up, a.up, sizeof(s.up));
    std::memcpy(s.ahead, a.ahead, sizeof(s.ahead));
    s.bike = bike != nullptr;
    if (s.bike) { // the handlebars' centre: model (0, -400, 600) - the model's y is down, one unit a millimetre
        const float* m = bike->m;
        for (int k = 0; k < 3; ++k) s.bar[k] = m[12 + k] + m[4 + k] * -400.0f + m[8 + k] * 600.0f;
    }
    // the frames' display times (the pacing's display interval; a hold longer than 4.5 periods is not
    // motion). The differences below are taken over those times and scaled to the nominal period - uniform frames give
    // the plain per-frame differences; a missed display (two periods between two frames) is not a spike.
    const double nominal = pacing != nullptr && pacing->periodMs > 0.0 ? pacing->periodMs * 1e-3 : period;
    const double dt = pacing != nullptr && pacing->displayMs > 0.0 ? pacing->displayMs * 1e-3 : period;
    if (n_ > 0 && dt > 4.5 * nominal) n_ = 0;
    s.t = n_ > 0 ? s_[n_ - 1].t + dt : 0.0;
    if (n_ < 4) s_[n_++] = s;
    else {
        for (int i = 0; i < 3; ++i) s_[i] = s_[i + 1];
        s_[3] = s;
    }
    ++frames_;
    const double mm = 1000.0 / std::max(1e-6f, a.unitsPerMetre);
    const auto view = [](const Sample& v, const float p[3], double out[3]) {
        const float d[3] = {p[0] - v.eye[0], p[1] - v.eye[1], p[2] - v.eye[2]};
        out[0] = Dot(d, v.right);
        out[1] = Dot(d, v.up);
        out[2] = Dot(d, v.ahead);
    };
    // the second derivative over three samples' times, times the nominal period squared (a per-frame difference)
    const auto second = [nominal](double a0, double b0, double c0, double tA, double tB, double tC) {
        const double h1 = tB - tA, h2 = tC - tB;
        if (h1 <= 0.0 || h2 <= 0.0) return c0 - 2.0 * b0 + a0;
        return 2.0 * ((c0 - b0) / h2 - (b0 - a0) / h1) / (h1 + h2) * nominal * nominal;
    };
    double worldMm = -1.0, bikeMm = -1.0;
    if (n_ >= 3) {
        const Sample &A = s_[n_ - 3], &B = s_[n_ - 2], &C = s_[n_ - 1];
        float w[3];
        for (int k = 0; k < 3; ++k) w[k] = B.eye[k] + B.ahead[k] * 5.0f * a.unitsPerMetre;
        double va[3], vb[3], vc[3];
        view(A, w, va);
        view(B, w, vb);
        view(C, w, vc);
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double x = second(va[k], vb[k], vc[k], A.t, B.t, C.t);
            d2 += x * x;
        }
        worldMm = std::sqrt(d2) * mm;
        worldSq_ += worldMm * worldMm;
        worldMax_ = std::max(worldMax_, worldMm);
        ++worldN_;
        if (A.bike && B.bike && C.bike) {
            view(A, A.bar, va);
            view(B, B.bar, vb);
            view(C, C.bar, vc);
            d2 = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double x = second(va[k], vb[k], vc[k], A.t, B.t, C.t);
                d2 += x * x;
            }
            bikeMm = std::sqrt(d2) * mm;
            bikeSq_ += bikeMm * bikeMm;
            bikeMax_ = std::max(bikeMax_, bikeMm);
            ++bikeN_;
        }
        double v2 = 0.0;
        for (int k = 0; k < 3; ++k) v2 += (C.eye[k] - B.eye[k]) * (C.eye[k] - B.eye[k]);
        speedSum_ += std::sqrt(v2) / a.unitsPerMetre / std::max(1e-6, C.t - B.t > 0.0 ? C.t - B.t : period);
    }
    if (n_ >= 4 && nominal > 0.0) {
        double j2 = 0.0;
        const double h = s_[2].t - s_[1].t > 0.0 ? s_[2].t - s_[1].t : nominal;
        for (int k = 0; k < 3; ++k) { // the change of the second derivative between the two centred triples
            const double a1 = second(s_[0].eye[k], s_[1].eye[k], s_[2].eye[k], s_[0].t, s_[1].t, s_[2].t);
            const double a2 = second(s_[1].eye[k], s_[2].eye[k], s_[3].eye[k], s_[1].t, s_[2].t, s_[3].t);
            const double j = (a2 - a1) / (nominal * nominal) / h / a.unitsPerMetre;
            j2 += j * j;
        }
        jerkSq_ += j2;
        ++jerkN_;
    }
    if (csv_) {
        float bf[3] = {0, 0, 0}, bu[3] = {0, 0, 0}, bo[3] = {0, 0, 0}; // the drawn bike's forward, up, origin
        if (bike != nullptr) {
            for (int k = 0; k < 3; ++k) {
                bf[k] = bike->m[8 + k];
                bu[k] = -bike->m[4 + k];
                bo[k] = bike->m[12 + k];
            }
            Normalise(bf);
            Normalise(bu);
        }
        std::fprintf(csv_, "%ld,%d,%.4f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.5f,%.5f,%.5f,%.3f,%.3f,%.6f,%.6f,%.6f,"
                           "%.6f,%.6f,%.6f,%.5f,%.5f,%.5f", frame, ticks,
                     double(alpha), double(s.eye[0]), double(s.eye[1]), double(s.eye[2]), double(s.ahead[0]),
                     double(s.ahead[1]), double(s.ahead[2]), double(s.up[0]), double(s.up[1]), double(s.up[2]),
                     double(s.bar[0]), double(s.bar[1]), double(s.bar[2]), worldMm, bikeMm, double(bf[0]), double(bf[1]),
                     double(bf[2]), double(bu[0]), double(bu[1]), double(bu[2]), double(bo[0]), double(bo[1]), double(bo[2]));
        // the frame's display clock and step (vr_pacing.h)
        const PacingRow r = pacing != nullptr ? *pacing : PacingRow{};
        std::fprintf(csv_, ",%.4f,%.4f,%.4f,%.4f,%d,%.4f,%s\n", r.displayMs, r.loopMs, r.periodMs, r.carry, r.missed,
                     r.drawnErrMs, pacing == nullptr ? "none" : r.mode == PacingMode::kWallClock ? "wallclock" : "display");
        if (++csvRows_ % 64 == 0) std::fflush(csv_); // a Quest session ends by being killed: the rows so far stay
    }
}

std::string MotionMeter::Totals() const {
    char b[600];
    std::snprintf(b, sizeof(b),
                  "vr motion: %zu head-view frame(s) measured; the world 5 m ahead in the eye: second "
                  "difference RMS %.2f mm, max %.2f (%zu); the handlebars in the eye: RMS %.2f mm, max %.2f (%zu); the "
                  "eye's jerk RMS %.1f m/s^3 (%zu); mean speed %.2f m/s\n",
                  frames_, worldN_ ? std::sqrt(worldSq_ / static_cast<double>(worldN_)) : 0.0, worldMax_, worldN_,
                  bikeN_ ? std::sqrt(bikeSq_ / static_cast<double>(bikeN_)) : 0.0, bikeMax_, bikeN_,
                  jerkN_ ? std::sqrt(jerkSq_ / static_cast<double>(jerkN_)) : 0.0, jerkN_,
                  worldN_ ? speedSum_ / static_cast<double>(worldN_) : 0.0);
    return b;
}

MotionMeter& ProductMotionMeter() {
    static MotionMeter m;
    return m;
}

} // namespace rrgame
