// The head view's horizon and the riders near it (vr_horizon.h).
#include "vr_horizon.h"

#include "vr_comfort.h" // FrameInterp::Write (the drawn state, put back with its Restore)
#include "game/wheelie.h" // the held wheelie's pitch (HeldPitch)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rrgame {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;

bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }
int32_t S32(const uint8_t* ram, uint32_t a) {
    int32_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
int16_t S16(const uint8_t* ram, uint32_t a) {
    int16_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
    return v;
}
double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool Unit(double v[3]) {
    const double n = std::sqrt(Dot(v, v));
    if (!(n > 1e-9)) return false;
    for (int k = 0; k < 3; ++k) v[k] /= n;
    return true;
}
// the pitch of a direction (radians, the nose up positive: the PlayStation's Y points down)
double PitchOf(const double f[3]) {
    const double n = std::sqrt(Dot(f, f));
    return n > 1e-9 ? std::asin(std::clamp(-f[1] / n, -1.0, 1.0)) : 0.0;
}
// critically damped second-order low-pass, semi-implicit (vr_bike_shake.cpp's)
void LowPass(double& x, double& v, double u, double hz, double dt) {
    const double w = 2.0 * kPi * hz, h = std::clamp(dt, 1e-4, 0.05);
    v += (w * w * (u - x) - 2.0 * w * v) * h;
    x += v * h;
}
// Rodrigues: v turned by `a` about the unit axis k
void Turn(const double k[3], double a, const double v[3], double out[3]) {
    const double c = std::cos(a), s = std::sin(a), d = Dot(k, v);
    const double x[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    for (int i = 0; i < 3; ++i) out[i] = v[i] * c + x[i] * s + k[i] * d * (1.0 - c);
}

} // namespace

const char* ViewPitchName(int mode) {
    return mode == kPitchRoad ? "Road only" : mode == kPitchOriginal ? "Original" : "Low";
}

bool ParseViewPitch(const std::string& v, int& mode) {
    if (v == "road" || v == "0") return mode = kPitchRoad, true;
    if (v == "low" || v == "1") return mode = kPitchLow, true;
    if (v == "original" || v == "2") return mode = kPitchOriginal, true;
    return false;
}

// ================================================================ ViewPitch
void ViewPitch::Capture(const uint8_t* ram, uint32_t bike) {
    prev_ = cur_;
    cur_ = Cap{};
    have_ = std::min(have_ + 1, 2);
    if (ram == nullptr || !InRam(bike)) return;
    const double g[3] = {static_cast<double>(S16(ram, bike + 0x210u)), static_cast<double>(S16(ram, bike + 0x212u)),
                         static_cast<double>(S16(ram, bike + 0x214u))}; // the ground frame's forward (Q12)
    if (!(Dot(g, g) > 1e4)) return;
    cur_.valid = true;
    cur_.bike = bike;
    cur_.grade = PitchOf(g);
    cur_.own = static_cast<double>(S32(ram, bike + 0x268u)) / 65536.0; // the pitch move (the wheelie / stoppie)
    for (uint32_t k = 0; k < 3; ++k) cur_.origin[k] = S32(ram, bike + 0xB8u + 4u * k);
    static std::FILE* trace = [] { // DEVELOPMENT RRJB_VIEW_PITCH_TRACE=<csv>: the placement fields per step
        const char* p = std::getenv("RRJB_VIEW_PITCH_TRACE");
        std::FILE* f = p ? std::fopen(p, "wb") : nullptr;
        if (f) std::fprintf(f, "step,ox,oy,oz,cx,cy,cz,x31c,y31c,z31c,own,grade,r1x,r1y,r1z,fx,fy,fz\n");
        return f;
    }();
    if (trace) {
        static long step = 0;
        const auto w = [&](uint32_t a) { return static_cast<double>(S32(ram, bike + a)) / 65536.0; };
        std::fprintf(trace, "%ld,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%d,%d,%d,%d,%d,%d\n", step++, w(0xB8),
                     w(0xBC), w(0xC0), w(0x1F8), w(0x1FC), w(0x200), w(0x31C), w(0x320), w(0x324), cur_.own * kDeg,
                     cur_.grade * kDeg, S16(ram, bike + 0x1B6u), S16(ram, bike + 0x1B8u), S16(ram, bike + 0x1BAu),
                     S16(ram, bike + 0x1BCu), S16(ram, bike + 0x1BEu), S16(ram, bike + 0x1C0u));
    }
}

bool ViewPitch::Apply(uint8_t* ram, uint32_t bike, int mode, float alpha, double dt, const WheelContacts& wheels,
                      FrameInterp& interp) {
    ++frames_;
    row_ = Row{};
    lastHave_ = false;
    removedOn_ = false;
    heldNow_ = 0.0f;
    if (ram == nullptr || !InRam(bike) || !cur_.valid || cur_.bike != bike) {
        ++skipped_;
        state_.have = false;
        return false;
    }
    // the step's values at the drawn time (a cut - a respawn, a teleport - is drawn at the last step)
    bool between = have_ >= 2 && prev_.valid && prev_.bike == bike;
    if (between) {
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double d = (static_cast<double>(cur_.origin[k]) - prev_.origin[k]) / 65536.0;
            d2 += d * d;
        }
        between = d2 < FrameInterp::kCut * FrameInterp::kCut;
    }
    const double t = between ? std::clamp(static_cast<double>(alpha), 0.0, 1.0) : 1.0;
    const double grade = between ? prev_.grade + (cur_.grade - prev_.grade) * t : cur_.grade;
    const double own268 = between ? prev_.own + (cur_.own - prev_.own) * t : cur_.own; // (the CSV)
    // the drawn bike (the step, or smooth motion's state between the last two)
    double O[3], rows[3][3];
    for (uint32_t k = 0; k < 3; ++k) {
        O[k] = static_cast<double>(S32(ram, bike + 0xB8u + 4u * k)) / 65536.0;
        for (uint32_t c = 0; c < 3; ++c) rows[c][k] = static_cast<double>(S16(ram, bike + 0x1B0u + 6u * c + 2u * k));
    }
    double f[3] = {rows[2][0], rows[2][1], rows[2][2]}, u[3] = {-rows[1][0], -rows[1][1], -rows[1][2]};
    if (!Unit(f)) {
        ++skipped_;
        return false;
    }
    {
        const double d = Dot(u, f);
        for (int k = 0; k < 3; ++k) u[k] -= d * f[k];
    }
    if (!Unit(u)) {
        ++skipped_;
        return false;
    }
    const double game = PitchOf(f);
    // The bike's own pitch: the drawn rows' pitch over the road's grade. On the road it is +0x268 (within 2 deg on 1/23's
    // stoppies, 0.1 deg on 1/1's start wheelie); but in the air or in a long wheelie up a steep grade the ground
    // forward +0x210 itself turns with the bike and +0x268 is not in the rows at all (1/20 with the sparring partner:
    // +0x268 at +24.6 deg, the rows' pitch equal to +0x210's) - so the rows are what is split, never +0x268.
    const double own = game - grade;
    // the order of the two layers: (1) here the game's rows are split; the part of the game's own
    // pitch the player's HELD wheelie covers (src\game\wheelie.h HeldPitch: the wheelie layer draws only what it holds
    // above the original's pop) is taken out of the rows whole, never filtered or shared; (2) the wheelie layer
    // (vr_wheelie.cpp AddWheeliePitch, a VisualLean pitch after the lean) then draws its WHOLE held pitch - its own share
    // plus the part taken out here (RemovedWheelie) - on the bike, the rider, the grips and the hands, and the view keeps
    // wheelie_view_pitch of it (WheelieViewComfort). Only the rest of the game's pitch (a pop above the held wheelie, a
    // stoppie) goes through the low-pass and the share below.
    float held = 0.0f;
    static const bool filterAll = [] { // DEVELOPMENT RRJB_VIEW_PITCH_WHEELIE=filter: the held part filtered too (the control)
        const char* e = std::getenv("RRJB_VIEW_PITCH_WHEELIE");
        return e != nullptr && std::strcmp(e, "filter") == 0;
    }();
    const bool isHeld = rr::game::PlayerWheelie().HeldPitch(held);
    heldNow_ = isHeld ? held : 0.0f;
    const double covered = !filterAll && mode != kPitchOriginal && wheels.valid && own > 0.0 && isHeld
                               ? std::min(own, static_cast<double>(held)) : 0.0;
    const double ownRest = own - covered;
    // the filters run in every mode (warm when the setting changes mid-race)
    State& s = state_;
    if (!s.have || s.bike != bike) {
        s = State{};
        s.have = true;
        s.bike = bike;
        s.g = grade;
        s.o = ownRest;
    } else {
        LowPass(s.g, s.gv, grade, kGradeHz, dt);
        LowPass(s.o, s.ov, ownRest, kOwnHz, dt);
        const double leash = kGradeLeashDeg / kDeg; // a crest: the drawn grade never lags the road by more
        if (s.g > grade + leash) s.g = grade + leash, s.gv = std::min(s.gv, 0.0);
        if (s.g < grade - leash) s.g = grade - leash, s.gv = std::max(s.gv, 0.0);
    }
    const double share = mode == kPitchLow ? kLowShare : 0.0;
    const double dOwn = share * s.o - own, dGrade = s.g - grade; // (the covered part out whole: share x rest remains)
    row_.grade = grade * kDeg;
    row_.own268 = own268 * kDeg;
    row_.ownRows = own * kDeg;
    row_.game = game * kDeg;
    row_.target = (game + dOwn + dGrade) * kDeg;
    // the game's contact height under the drawn bike (the meter's eye height): the origin down to the wheels
    const double midUp = wheels.valid ? 0.5 * (wheels.frontUp + wheels.rearUp) : 0.0;
    const double midFwd = wheels.valid ? 0.5 * (wheels.frontFwd + wheels.rearFwd) : 0.0;
    lastContactY_ = O[1] + u[1] * midUp + f[1] * midFwd;
    row_.gFront = -(O[1] + u[1] * wheels.frontUp + f[1] * wheels.frontFwd);
    row_.gRear = -(O[1] + u[1] * wheels.rearUp + f[1] * wheels.rearFwd);
    row_.dFront = row_.gFront;
    row_.dRear = row_.gRear;
    lastGamePitch_ = game;
    lastHave_ = true;
    {   // the counters: the own pitch moves, how far the rows' pitch over the grade is from +0x268
        const double ownDeg = std::abs(own) * kDeg;
        ownMaxDeg_ = std::max(ownMaxDeg_, ownDeg);
        if (!ownOn_ && ownDeg > 2.0) ++ownMoves_;
        ownOn_ = ownDeg > 2.0 ? true : ownDeg < 1.0 ? false : ownOn_;
        ownDiffMaxDeg_ = std::max(ownDiffMaxDeg_, std::abs(row_.ownRows - row_.own268));
    }
    if (mode == kPitchOriginal || !wheels.valid) return false;
    // (1) the own pitch move undone down to its filtered share: about the bike's right axis, through the wheel that
    // stays on the road (the rear in a wheelie, the front in a stoppie; between the wheels when there is none)
    double r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]}; // f x u
    Unit(r);
    const double w = std::clamp(std::abs(own) * kDeg / 2.0, 0.0, 1.0);
    const double pUp = (1.0 - w) * midUp + w * (own > 0.0 ? wheels.rearUp : wheels.frontUp);
    const double pFwd = (1.0 - w) * midFwd + w * (own > 0.0 ? wheels.rearFwd : wheels.frontFwd);
    // a positive angle turns forward toward up (nose up): Rodrigues turns f toward k x f, and (u x f) x f = -u, so the
    // axis is f x u
    const double* axis = r;
    double P[3], Op[3], R2[3][3];
    for (int k = 0; k < 3; ++k) P[k] = O[k] + u[k] * pUp + f[k] * pFwd;
    {
        double rel[3], relT[3];
        for (int k = 0; k < 3; ++k) rel[k] = O[k] - P[k];
        Turn(axis, dOwn, rel, relT);
        for (int k = 0; k < 3; ++k) Op[k] = P[k] + relT[k];
        for (int c = 0; c < 3; ++c) Turn(axis, dOwn, rows[c], R2[c]);
    }
    // (2) the grade toward its filtered value: about the level right axis (a pure pitch in the world), through the
    // contact between the wheels of the turned bike
    {
        double f2[3] = {R2[2][0], R2[2][1], R2[2][2]}, u2[3] = {-R2[1][0], -R2[1][1], -R2[1][2]};
        Unit(f2);
        Unit(u2);
        const double W[3] = {0.0, -1.0, 0.0};
        double k[3] = {f2[1] * W[2] - f2[2] * W[1], f2[2] * W[0] - f2[0] * W[2], f2[0] * W[1] - f2[1] * W[0]}; // f x W
        if (Unit(k)) {
            double Q[3], rel[3], relT[3];
            for (int i = 0; i < 3; ++i) Q[i] = Op[i] + u2[i] * midUp + f2[i] * midFwd;
            for (int i = 0; i < 3; ++i) rel[i] = Op[i] - Q[i];
            Turn(k, dGrade, rel, relT);
            for (int i = 0; i < 3; ++i) Op[i] = Q[i] + relT[i];
            for (int c = 0; c < 3; ++c) {
                double tmp[3];
                Turn(k, dGrade, R2[c], tmp);
                std::memcpy(R2[c], tmp, sizeof(tmp));
            }
        }
    }
    int16_t out[9];
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) out[3 * c + k] = static_cast<int16_t>(std::clamp<long long>(std::llround(R2[c][k]), -32768LL, 32767LL));
    int32_t pos[3];
    double shift2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        pos[k] = static_cast<int32_t>(std::llround(Op[k] * 65536.0));
        shift2 += (Op[k] - O[k]) * (Op[k] - O[k]);
    }
    interp.Write(ram, bike + 0x1B0u, out, 18);
    interp.Write(ram, bike + 0xB8u, pos, 12);
    {   // the drawn wheels (the CSV)
        double f2[3] = {R2[2][0], R2[2][1], R2[2][2]}, u2[3] = {-R2[1][0], -R2[1][1], -R2[1][2]};
        Unit(f2);
        Unit(u2);
        row_.dFront = -(Op[1] + u2[1] * wheels.frontUp + f2[1] * wheels.frontFwd);
        row_.dRear = -(Op[1] + u2[1] * wheels.rearUp + f2[1] * wheels.rearFwd);
    }
    ++planned_;
    row_.planned = true;
    if (covered > 0.0) { // (the wheelie layer draws it back: RemovedWheelie)
        removedOn_ = true;
        removedBike_ = bike;
        removed_ = covered;
        ++coveredFrames_;
        coveredMaxDeg_ = std::max(coveredMaxDeg_, covered * kDeg);
    }
    deltaMaxDeg_ = std::max(deltaMaxDeg_, std::abs(dOwn + dGrade) * kDeg);
    pivotShiftMax_ = std::max(pivotShiftMax_, std::sqrt(shift2));
    return true;
}

double ViewPitch::HighPass::Feed(double value, double dt, bool warm) {
    if (!have) {
        have = true;
        x = value;
        v = 0.0;
    } else {
        LowPass(x, v, value, 0.5, dt);
    }
    const double hp = value - x;
    if (warm) {
        sq += hp * hp;
        max = std::max(max, std::abs(hp));
        ++n;
    }
    return hp;
}

void ViewPitch::Meter(long frame, const rr::xr::WorldAnchor& a, double dt, const rr::render::Mat4* drawnBike) {
    if (drawnBike != nullptr && lastHave_ && heldNow_ * kDeg >= 30.0) { // a held wheelie as drawn and seen
        const double f[3] = {drawnBike->m[8], drawnBike->m[9], drawnBike->m[10]}, v[3] = {a.ahead[0], a.ahead[1], a.ahead[2]};
        const double dp = PitchOf(f) * kDeg - row_.grade, vp = PitchOf(v) * kDeg - row_.grade;
        ++heldFrames_;
        heldDrawnMin_ = std::min(heldDrawnMin_, dp);
        heldDrawnMax_ = std::max(heldDrawnMax_, dp);
        heldViewMin_ = std::min(heldViewMin_, vp);
        heldViewMax_ = std::max(heldViewMax_, vp);
    }
    if (!csvTried_) {
        csvTried_ = true;
        if (const char* p = std::getenv("RRJB_VIEW_PITCH_LOG")) {
            csv_ = std::fopen(p, "wb");
            if (csv_) std::fprintf(csv_, "frame,grade,own268,ownRows,game,target,view,eyeMm,hpView,hpGame,hpEye,planned,"
                                         "gFront,gRear,dFront,dRear\n");
        }
    }
    if (!meter_.have) { // (after a break: the filters start again, the run's sums stay)
        meter_.have = true;
        meter_.warm = 0;
        meter_.view.have = meter_.eye.have = meter_.game.have = false;
        meter_.haveLast = false;
        eyeN_ = 0;
    }
    {   // the eye's fore-aft second difference (a regular display period: the mock's and the headset's)
        if (eyeN_ == 3) {
            std::memmove(eyeHist_[0], eyeHist_[1], sizeof(eyeHist_[0]) * 2);
            eyeN_ = 2;
        }
        for (int k = 0; k < 3; ++k) eyeHist_[eyeN_][k] = a.origin[k];
        ++eyeN_;
        if (eyeN_ == 3) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += (eyeHist_[2][k] - 2.0 * eyeHist_[1][k] + eyeHist_[0][k]) * a.ahead[k];
            if (foreAft_.size() < 400000) // (a session's memory stays flat)
                foreAft_.push_back(static_cast<float>(std::abs(s) / (a.unitsPerMetre > 0.0f ? a.unitsPerMetre : 1.0f) * 1000.0));
        }
    }
    const double ahead[3] = {a.ahead[0], a.ahead[1], a.ahead[2]};
    const double view = PitchOf(ahead) * kDeg;
    const double upm = a.unitsPerMetre > 0.0f ? a.unitsPerMetre : 1.0f;
    const double eyeMm = lastHave_ ? (lastContactY_ - a.origin[1]) / upm * 1000.0 : 0.0; // above the contact (Y down)
    const bool warm = ++meter_.warm > 72; // (the low-passes' first second is their own transient)
    ++meter_.frames;
    const double hv = meter_.view.Feed(view, dt, warm);
    const double hg = lastHave_ ? meter_.game.Feed(lastGamePitch_ * kDeg, dt, warm) : 0.0;
    const double he = lastHave_ ? meter_.eye.Feed(eyeMm, dt, warm) : 0.0;
    if (lastHave_) {
        const double g = lastGamePitch_ * kDeg;
        if (meter_.haveLast) {
            const double dv = std::abs(view - meter_.lastView), dg = std::abs(g - meter_.lastGame);
            stepViewMax_ = std::max(stepViewMax_, dv);
            stepGameMax_ = std::max(stepGameMax_, dg);
            stepViewSq_ += dv * dv;
            stepGameSq_ += dg * dg;
            ++stepN_;
        }
        meter_.lastView = view;
        meter_.lastGame = g;
        meter_.haveLast = true;
    }
    if (csv_) {
        std::fprintf(csv_, "%ld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.4f,%.4f,%.2f,%d,%.5f,%.5f,%.5f,%.5f\n", frame, row_.grade,
                     row_.own268, row_.ownRows, row_.game, row_.target, view, eyeMm, hv, hg, he, row_.planned ? 1 : 0,
                     row_.gFront, row_.gRear, row_.dFront, row_.dRear);
        if (++csvRows_ % 64 == 0) std::fflush(csv_);
    }
}

std::string ViewPitch::Totals() const {
    const auto rms = [](const HighPass& h) { return h.n ? std::sqrt(h.sq / static_cast<double>(h.n)) : 0.0; };
    double faP99 = 0.0, faMax = 0.0, faSq = 0.0;
    if (!foreAft_.empty()) {
        std::vector<float> v = foreAft_;
        std::sort(v.begin(), v.end());
        faP99 = v[std::min(v.size() - 1, v.size() * 99 / 100)];
        faMax = v.back();
        for (float x : v) faSq += double(x) * x;
        faSq /= static_cast<double>(v.size());
    }
    char b[1500];
    std::snprintf(b, sizeof(b),
                  "view pitch: %zu head-view frame(s), %zu drawn with the planned pitch, %zu without a "
                  "capture; the bike's own pitch moves (the rows' pitch over the grade past 2 deg) %zu, up to %.2f deg "
                  "(against +0x268 up to %.2f deg); the drawn pitch turned up to %.2f deg, the origin moved up to %.3f "
                  "world units; the view's pitch high-passed (0.5 Hz) RMS %.3f deg max %.3f, the game's bike pitch RMS %.3f "
                  "max %.3f; the eye's height over the road high-passed RMS %.1f mm max %.1f (%zu frame(s)); the pitch's "
                  "change a frame - the view RMS %.3f max %.2f deg, the game's bike RMS %.3f max %.2f; the eye's fore-aft "
                  "second difference RMS %.2f mm, p99 %.2f, max %.1f (%zu); the game's own pitch under the held wheelie "
                  "left to the wheelie layer on %zu frame(s), up to %.1f deg; the held wheelie (30 deg or more) %zu frame(s): "
                  "the drawn bike over the grade %.1f .. %.1f deg, the view over the grade %.1f .. %.1f deg\n",
                  frames_, planned_, skipped_, ownMoves_, ownMaxDeg_, ownDiffMaxDeg_, deltaMaxDeg_, pivotShiftMax_,
                  rms(meter_.view), meter_.view.max, rms(meter_.game), meter_.game.max, rms(meter_.eye), meter_.eye.max,
                  meter_.view.n, stepN_ ? std::sqrt(stepViewSq_ / static_cast<double>(stepN_)) : 0.0, stepViewMax_,
                  stepN_ ? std::sqrt(stepGameSq_ / static_cast<double>(stepN_)) : 0.0, stepGameMax_, std::sqrt(faSq), faP99,
                  faMax, foreAft_.size(), coveredFrames_, coveredMaxDeg_, heldFrames_, heldFrames_ ? heldDrawnMin_ : 0.0,
                  heldFrames_ ? heldDrawnMax_ : 0.0, heldFrames_ ? heldViewMin_ : 0.0, heldFrames_ ? heldViewMax_ : 0.0);
    return b;
}

ViewPitch& ProductViewPitch() {
    static ViewPitch v;
    return v;
}

// ================================================================ RivalSmooth
bool RivalSmoothOff() {
    static const bool off = [] {
        const char* e = std::getenv("RRJB_RIVAL_SMOOTH");
        return e != nullptr && std::strcmp(e, "off") == 0;
    }();
    return off;
}

size_t RivalSmooth::Apply(uint8_t* ram, const std::vector<uint32_t>& bikes, const std::vector<uint32_t>& riders,
                          const float eye[3], double dt, long frame, FrameInterp& interp) {
    ++frames_;
    if (ram == nullptr || !(dt > 0.0) || RivalSmoothOff()) return 0;
    const double h = kG * kG / (2.0 - kG);
    std::vector<Track> next;
    size_t written = 0;
    for (size_t i = 0; i < bikes.size(); ++i) {
        const uint32_t bike = bikes[i];
        if (!InRam(bike)) continue;
        double z[3], step[3], d2eye = 0.0;
        for (uint32_t k = 0; k < 3; ++k) {
            z[k] = static_cast<double>(S32(ram, bike + 0xB8u + 4u * k)) / 65536.0; // as drawn (interpolated)
            d2eye += (z[k] - eye[k]) * (z[k] - eye[k]);
        }
        if (d2eye > kRange * kRange || !interp.StepOrigin(bike, step)) continue;
        Track t;
        for (const Track& o : tracks_)
            if (o.bike == bike) t = o;
        t.bike = bike;
        double jump2 = 0.0;
        for (int k = 0; k < 3; ++k) jump2 += (z[k] - t.x[k]) * (z[k] - t.x[k]);
        if (t.last != frame - 1 || jump2 > FrameInterp::kCut * FrameInterp::kCut) {
            if (t.n > 0) ++resets_;
            t.n = 0;
        }
        t.last = frame;
        if (t.n == 0) {
            for (int k = 0; k < 3; ++k) t.x[k] = z[k], t.v[k] = 0.0;
            t.n = 1;
            next.push_back(t);
            continue;
        }
        if (t.n == 1) { // the velocity from the first two drawn frames, then the filter
            for (int k = 0; k < 3; ++k) t.v[k] = (z[k] - t.x[k]) / dt, t.x[k] = z[k];
            t.n = 2;
            next.push_back(t);
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            const double pred = t.x[k] + t.v[k] * dt, r = z[k] - pred;
            t.x[k] = pred + kG * r;
            t.v[k] += h * r / dt;
        }
        // the leash as the player's (vr_comfort.h): kLeash across the bike, longer along its forward
        float fwd[3] = {static_cast<float>(S16(ram, bike + 0x1BCu)), static_cast<float>(S16(ram, bike + 0x1BEu)),
                        static_cast<float>(S16(ram, bike + 0x1C0u))};
        {
            const float n = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
            if (n > 1.0f)
                for (float& c : fwd) c /= n;
            else
                fwd[0] = 0.0f, fwd[1] = 0.0f, fwd[2] = 1.0f;
        }
        double before[3];
        std::memcpy(before, t.x, sizeof(before));
        LeashAlong(t.x, z, fwd, kLeash, std::min(kLeashAlong, SmoothAlongLeash()));
        double e2 = 0.0, moved2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            e2 += (t.x[k] - z[k]) * (t.x[k] - z[k]);
            moved2 += (t.x[k] - before[k]) * (t.x[k] - before[k]);
        }
        if (moved2 > 1e-12) ++leashed_;
        offSq_ += e2;
        offMax_ = std::max(offMax_, std::sqrt(e2));
        ++offN_;
        int32_t pos[3];
        float moved[3];
        for (int k = 0; k < 3; ++k) {
            pos[k] = static_cast<int32_t>(std::llround(t.x[k] * 65536.0));
            moved[k] = static_cast<float>(static_cast<double>(pos[k]) / 65536.0 - step[k]);
        }
        interp.Write(ram, bike + 0xB8u, pos, 12);
        interp.SetShift(bike, moved); // its shadow; the rider on it is drawn from the bike's matrix
        if (i < riders.size() && InRam(riders[i])) interp.SetShift(riders[i], moved);
        ++written;
        next.push_back(t);
    }
    tracks_.swap(next);
    written_ += written;
    return written;
}

std::string RivalSmooth::Totals() const {
    char b[400];
    std::snprintf(b, sizeof(b),
                  "rival smoothing: %zu frame(s), %zu rival bike frame(s) drawn through the g-h filter, %zu "
                  "restart(s), %zu on the leash; drawn off the interpolated state RMS %.1f mm max %.1f%s\n",
                  frames_, written_, resets_, leashed_, offN_ ? 1000.0 * std::sqrt(offSq_ / static_cast<double>(offN_)) : 0.0,
                  1000.0 * offMax_, RivalSmoothOff() ? " (RRJB_RIVAL_SMOOTH=off)" : "");
    return b;
}

RivalSmooth& ProductRivalSmooth() {
    static RivalSmooth r;
    return r;
}

// ================================================================ NearMeter
void NearMeter::Frame(long frame, const uint8_t* ram, const std::vector<uint32_t>& bikes, const rr::xr::WorldAnchor& a,
                      double dt, double period) {
    if (!csvTried_) {
        csvTried_ = true;
        if (const char* p = std::getenv("RRJB_NEAR_LOG")) {
            csv_ = std::fopen(p, "wb");
            if (csv_) std::fprintf(csv_, "frame,bike,distM,x,y,z,d2Mm,alongMm,worldMm,wx,wy,wz,interp,shiftMm\n");
        }
    }
    if (ram == nullptr) return;
    if (lastFrame_ != frame - 1) tracks_.clear();
    lastFrame_ = frame;
    t_ += dt > 0.0 ? dt : period;
    const double upm = a.unitsPerMetre > 0.0f ? a.unitsPerMetre : 1.0f;
    const double mm = 1000.0 / upm;
    const double nominal = period > 0.0 ? period : 1.0 / 72.0;
    std::vector<Track> next;
    for (uint32_t bike : bikes) {
        if (!InRam(bike)) continue;
        double p[3], d[3], e[3];
        for (uint32_t k = 0; k < 3; ++k) p[k] = static_cast<double>(S32(ram, bike + 0xB8u + 4u * k)) / 65536.0;
        for (int k = 0; k < 3; ++k) d[k] = p[k] - a.origin[k];
        const double right[3] = {a.right[0], a.right[1], a.right[2]}, up[3] = {a.up[0], a.up[1], a.up[2]},
                     ahead[3] = {a.ahead[0], a.ahead[1], a.ahead[2]};
        e[0] = Dot(d, right);
        e[1] = Dot(d, up);
        e[2] = Dot(d, ahead);
        const double dist = std::sqrt(Dot(d, d)) / upm;
        Track tr;
        for (const Track& o : tracks_)
            if (o.bike == bike) tr = o;
        tr.bike = bike;
        if (tr.lastFrame != frame - 1) tr.n = 0;
        tr.lastFrame = frame;
        if (tr.n == 3) {
            for (int i = 0; i < 2; ++i) {
                std::memcpy(tr.eye[i], tr.eye[i + 1], sizeof(tr.eye[i]));
                std::memcpy(tr.world[i], tr.world[i + 1], sizeof(tr.world[i]));
                tr.t[i] = tr.t[i + 1];
            }
            tr.n = 2;
        }
        std::memcpy(tr.eye[tr.n], e, sizeof(e));
        std::memcpy(tr.world[tr.n], p, sizeof(p));
        tr.t[tr.n] = t_;
        ++tr.n;
        if (tr.n == 3) {
            const double h1 = tr.t[1] - tr.t[0], h2 = tr.t[2] - tr.t[1];
            const auto second = [&](double x0, double x1, double x2) {
                if (!(h1 > 0.0) || !(h2 > 0.0)) return x2 - 2.0 * x1 + x0;
                return 2.0 * ((x2 - x1) / h2 - (x1 - x0) / h1) / (h1 + h2) * nominal * nominal;
            };
            double s2 = 0.0, w2 = 0.0;
            double sd[3];
            for (int k = 0; k < 3; ++k) {
                sd[k] = second(tr.eye[0][k], tr.eye[1][k], tr.eye[2][k]) * mm;
                s2 += sd[k] * sd[k];
                const double wd = second(tr.world[0][k], tr.world[1][k], tr.world[2][k]) * mm;
                w2 += wd * wd;
            }
            const double m = std::sqrt(s2);
            const int b = dist < 5.0 ? 0 : dist < 15.0 ? 1 : dist < 40.0 ? 2 : -1;
            if (b >= 0) {
                Band& bd = band_[b];
                ++bd.n;
                bd.sq += s2;
                bd.max = std::max(bd.max, m);
                bd.alongSq += sd[2] * sd[2];
                bd.worldSq += w2;
                if (bd.eye.size() < 200000) { // (the percentiles from the first ~1.3 h of samples: a session's memory stays flat)
                    bd.eye.push_back(static_cast<float>(m));
                    bd.along.push_back(static_cast<float>(std::abs(sd[2])));
                    bd.world.push_back(static_cast<float>(std::sqrt(w2)));
                }
            }
            if (csv_) {
                float sh[3] = {0, 0, 0};
                const bool interp = ProductFrameInterp().Shift(bike, sh); // drawn between two steps (vr_comfort.h)
                std::fprintf(csv_, "%ld,%08X,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.5f,%.5f,%.5f,%d,%.2f\n", frame, bike, dist,
                             e[0] / upm, e[1] / upm, e[2] / upm, m, sd[2], std::sqrt(w2), p[0], p[1], p[2], interp ? 1 : 0,
                             std::sqrt(double(sh[0]) * sh[0] + double(sh[1]) * sh[1] + double(sh[2]) * sh[2]) * mm);
                if (++csvRows_ % 256 == 0) std::fflush(csv_);
            }
        }
        next.push_back(tr);
    }
    tracks_.swap(next);
}

std::string NearMeter::Totals() const {
    static const char* const kName[kBands] = {"under 5 m", "5..15 m", "15..40 m"};
    std::string s = "near riders: the rival bikes as drawn, in the eye, second difference over the display "
                    "times (mm per nominal frame):";
    for (int b = 0; b < kBands; ++b) {
        const Band& bd = band_[b];
        const double n = bd.n ? static_cast<double>(bd.n) : 1.0;
        const auto pct = [](std::vector<float> v, double p) {
            if (v.empty()) return 0.0;
            const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
            return static_cast<double>(v[i]);
        };
        char x[400];
        std::snprintf(x, sizeof(x),
                      " %s - %zu sample(s), RMS %.2f mm max %.2f (median %.2f, p90 %.2f), along the view RMS %.2f (median "
                      "%.2f, p90 %.2f), in the world RMS %.2f (median %.2f, p90 %.2f);",
                      kName[b], bd.n, std::sqrt(bd.sq / n), bd.max, pct(bd.eye, 0.5), pct(bd.eye, 0.9),
                      std::sqrt(bd.alongSq / n), pct(bd.along, 0.5), pct(bd.along, 0.9), std::sqrt(bd.worldSq / n),
                      pct(bd.world, 0.5), pct(bd.world, 0.9));
        s += x;
    }
    s += "\n";
    return s;
}

NearMeter& ProductNearMeter() {
    static NearMeter m;
    return m;
}

} // namespace rrgame
