// The player's own bike in the head view: the meter and the drawn slots (vr_bike_shake.h).
#include "vr_bike_shake.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>

namespace rrgame {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;
constexpr float kModelUnitsPerWorldUnit = 1024.0f; // vr_bar_grips.cpp
constexpr float kEndBand = 0.03f;                   // world units: the bar ends (vr_bar_grips.cpp)

bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }
uint32_t U32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
int16_t S16(const uint8_t* ram, uint32_t a) {
    int16_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
    return v;
}
double Wrap(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a <= -kPi) a += 2 * kPi;
    return a;
}
int16_t Q12(double v) { return static_cast<int16_t>(std::clamp<long long>(std::llround(v * 4096.0), -32768, 32767)); }

// The slot matrices BikeInstance / WheelSlots build (bike_parts.cpp RotMatrix with the zero angles taken out):
// RotMatrix(0, b, 0), RotMatrix(a, 0, c), RotMatrix(a, 0, 0).
void ForkMatrix(double b, int16_t m[9]) {
    const double s = std::sin(b), c = std::cos(b);
    const int16_t out[9] = {Q12(c), 0, Q12(s), 0, 4096, 0, Q12(-s), 0, Q12(c)};
    std::memcpy(m, out, sizeof(out));
}
void PitchMatrix(double a, double c, int16_t m[9]) {
    const double sA = std::sin(a), cA = std::cos(a), sC = std::sin(c), cC = std::cos(c);
    const int16_t out[9] = {Q12(cC), Q12(-sC * cA), Q12(sA * sC), Q12(sC), Q12(cC * cA), Q12(-sA * cC), 0, Q12(sA), Q12(cA)};
    std::memcpy(m, out, sizeof(out));
}
void WheelMatrix(double a, int16_t m[9]) {
    const double s = std::sin(a), c = std::cos(a);
    const int16_t out[9] = {4096, 0, 0, 0, Q12(c), Q12(-s), 0, Q12(s), Q12(c)};
    std::memcpy(m, out, sizeof(out));
}
// Is `m` a rotation of that form (the zero entries within a few units)?
bool Near0(int16_t v) { return v >= -8 && v <= 8; }

void PosedPoint(const rr::PosedGroup& posed, size_t part, const rr::SVector& v, float factor, double out[3]) {
    const double lp[3] = {v.x * double(factor), v.y * double(factor), v.z * double(factor)};
    for (int r = 0; r < 3; ++r) {
        double s = 0.0;
        for (int c = 0; c < 3; ++c) s += static_cast<double>(posed.world[part].m[r * 3 + c]) * lp[c];
        out[r] = s / 4096.0 + posed.origin[part][static_cast<size_t>(r)];
    }
}

} // namespace

const char* BikeShakeName(int mode) {
    return mode == kShakeOff ? "Off" : mode == kShakeOriginal ? "Original" : "Low";
}

bool ParseBikeShake(const std::string& v, int& mode) {
    if (v == "off" || v == "0") return mode = kShakeOff, true;
    if (v == "low" || v == "1") return mode = kShakeLow, true;
    if (v == "original" || v == "2") return mode = kShakeOriginal, true;
    return false;
}

// ================================================================ the filter
void BikeShakeFilter::Capture(const uint8_t* ram, uint32_t bike) {
    prev_ = cur_;
    cur_ = Slots{};
    have_ = std::min(have_ + 1, 2);
    if (ram == nullptr || !InRam(bike)) return;
    const uint32_t model = U32(ram, bike), parts = U32(ram, bike + 4u);
    if (!InRam(model) || !InRam(parts) || !InRam(parts + 24u * 5u)) return;
    if ((U32(ram, model + 24u) & 0xFFFFu) != 5u) return; // the single-seat bike's five parts only
    Slots s;
    s.bike = bike;
    s.parts = parts;
    for (uint32_t k = 0; k < 5; ++k)
        for (uint32_t e = 0; e < 9; ++e) s.raw[k][e] = k == 0 ? (e % 4 == 0 ? 4096 : 0) : S16(ram, parts + 24u * k + 4u + 2u * e);
    const int16_t* f = s.raw[1];
    const int16_t* p = s.raw[2];
    // the forms BikeInstance writes (anything else - a slot some other code wrote - is left alone)
    const bool forkOk = Near0(f[1]) && Near0(f[3]) && Near0(f[5]) && Near0(f[7]) && f[4] > 4088;
    const bool pitchOk = Near0(p[6]);
    bool wheelsOk = true;
    for (int w = 3; w <= 4; ++w) {
        const int16_t* m = s.raw[w];
        wheelsOk = wheelsOk && m[0] > 4088 && Near0(m[1]) && Near0(m[2]) && Near0(m[3]) && Near0(m[6]);
    }
    if (!forkOk || !pitchOk || !wheelsOk) return;
    s.fork = std::atan2(-static_cast<double>(f[6]), static_cast<double>(f[0]));
    s.pitch = std::atan2(static_cast<double>(p[7]), static_cast<double>(p[8]));
    s.roll = std::atan2(static_cast<double>(p[3]), static_cast<double>(p[0]));
    for (int w = 0; w < 2; ++w) s.wheel[w] = std::atan2(static_cast<double>(s.raw[3 + w][7]), static_cast<double>(s.raw[3 + w][8]));
    s.valid = true;
    cur_ = s;
}

bool BikeShakeFilter::GameParts(uint32_t bike, rr::PartMatrix out[5]) const {
    if (!cur_.valid || cur_.bike != bike) return false;
    for (int k = 0; k < 5; ++k) std::memcpy(out[k].m, cur_.raw[k], sizeof(out[k].m));
    return true;
}

bool BikeShakeFilter::Apply(uint8_t* ram, uint32_t bike, int mode, float alpha, double dt) {
    saved_.clear();
    ++frames_;
    if (ram == nullptr || !InRam(bike)) return false;
    const auto save = [&](uint32_t addr) {
        Saved sv;
        sv.addr = addr;
        std::memcpy(sv.bytes, ram + (addr & 0x1FFFFFu), 18);
        saved_.push_back(sv);
    };
    // (1) The bike's orientation rows +0x1B0 as the frame draws them (the game's, or smooth motion's), at their length:
    // the game rebuilds them each step from rows whose length creeps up to ~0.6 % over ~25 ticks and snaps back (a
    // 12 Hz sawtooth in the SCALE of the drawn bike - 5 mm at the handlebars, the eye fixed on the bike's frame).
    {
        double r[3][3], len[3];
        for (uint32_t c = 0; c < 3; ++c) {
            len[c] = 0.0;
            for (uint32_t k = 0; k < 3; ++k) {
                r[c][k] = static_cast<double>(S16(ram, bike + 0x1B0u + 6u * c + 2u * k));
                len[c] += r[c][k] * r[c][k];
            }
            len[c] = std::sqrt(len[c]) / 4096.0;
        }
        rowLenMin_ = std::min(rowLenMin_, std::min(len[0], std::min(len[1], len[2])));
        rowLenMax_ = std::max(rowLenMax_, std::max(len[0], std::max(len[1], len[2])));
        if (mode != kShakeOriginal && len[0] > 0.5 && len[1] > 0.5 && len[2] > 0.5) {
            double f[3], u[3], x[3];
            for (int k = 0; k < 3; ++k) f[k] = r[2][k] / (len[2] * 4096.0);
            const double d = r[1][0] * f[0] + r[1][1] * f[1] + r[1][2] * f[2];
            double lu = 0.0;
            for (int k = 0; k < 3; ++k) u[k] = r[1][k] - d * f[k], lu += u[k] * u[k];
            lu = std::sqrt(lu);
            if (lu > 1e-6) {
                for (double& v : u) v /= lu;
                x[0] = u[1] * f[2] - u[2] * f[1]; // row 0 = row 1 x row 2, turned to the game's own row 0
                x[1] = u[2] * f[0] - u[0] * f[2];
                x[2] = u[0] * f[1] - u[1] * f[0];
                if (x[0] * r[0][0] + x[1] * r[0][1] + x[2] * r[0][2] < 0.0)
                    for (double& v : x) v = -v;
                const double* rows[3] = {x, u, f};
                int16_t out[9];
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k) out[3 * c + k] = Q12(rows[c][k]);
                save(bike + 0x1B0u);
                std::memcpy(ram + ((bike + 0x1B0u) & 0x1FFFFFu), out, 18);
            }
        }
    }
    // (2) the part slots: the step's must still be in the arena
    bool slotsOk = cur_.valid && cur_.bike == bike && U32(ram, bike + 4u) == cur_.parts;
    for (uint32_t k = 1; slotsOk && k < 5; ++k)
        slotsOk = std::memcmp(ram + ((cur_.parts + 24u * k + 4u) & 0x1FFFFFu), cur_.raw[k], 18) == 0;
    if (!slotsOk) {
        ++skipped_;
        state_.have = false;
        return !saved_.empty();
    }
    const bool between = have_ >= 2 && prev_.valid && prev_.bike == bike && prev_.parts == cur_.parts;
    const double t = between ? std::clamp(static_cast<double>(alpha), 0.0, 1.0) : 1.0;
    const auto lerp = [t](double a, double b) { return a + Wrap(b - a) * t; };
    const Slots& a = between ? prev_ : cur_;
    const double fork = lerp(a.fork, cur_.fork), pitch = lerp(a.pitch, cur_.pitch), roll = lerp(a.roll, cur_.roll);
    const double wheel[2] = {lerp(a.wheel[0], cur_.wheel[0]), lerp(a.wheel[1], cur_.wheel[1])};
    // the low-pass runs in every mode (it is warm when the setting changes mid-race)
    const double u[2] = {pitch, roll};
    const double h = std::clamp(dt, 1e-4, 0.05);
    if (!state_.have || state_.bike != bike) {
        state_ = State{};
        state_.have = true;
        state_.bike = bike;
        for (int k = 0; k < 2; ++k) state_.x[k] = u[k];
    } else {
        const double w = 2.0 * kPi * kLowHz;
        for (int k = 0; k < 2; ++k) { // critically damped, semi-implicit
            state_.v[k] += (w * w * Wrap(u[k] - state_.x[k]) - 2.0 * w * state_.v[k]) * h;
            state_.x[k] += state_.v[k] * h;
        }
    }
    double drawnPitch = cur_.pitch, drawnRoll = cur_.roll;
    if (mode == kShakeLow) drawnPitch = kLowGain * state_.x[0], drawnRoll = kLowGain * state_.x[1];
    else if (mode == kShakeOff) drawnPitch = 0.0, drawnRoll = 0.0;
    { // the pitch the game draws against the pitch drawn (degrees)
        const double g = std::abs(cur_.pitch) * kDeg, d = std::abs(drawnPitch) * kDeg;
        pitchGameSq_ += g * g;
        pitchDrawnSq_ += d * d;
        pitchGameMax_ = std::max(pitchGameMax_, g);
        pitchDrawnMax_ = std::max(pitchDrawnMax_, d);
        ++pitchN_;
    }
    if (mode == kShakeOriginal) return !saved_.empty();
    int16_t m[5][9];
    ForkMatrix(fork, m[1]);
    PitchMatrix(drawnPitch, drawnRoll, m[2]);
    WheelMatrix(wheel[0], m[3]);
    WheelMatrix(wheel[1], m[4]);
    for (uint32_t k = 1; k < 5; ++k) {
        const uint32_t addr = cur_.parts + 24u * k + 4u;
        save(addr);
        std::memcpy(ram + (addr & 0x1FFFFFu), m[k], 18);
    }
    return true;
}

void BikeShakeFilter::Restore(uint8_t* ram) {
    if (saved_.empty() || ram == nullptr) return;
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) std::memcpy(ram + (it->addr & 0x1FFFFFu), it->bytes, 18);
    if (cur_.valid && U32(ram, cur_.bike + 4u) == cur_.parts)
        for (uint32_t k = 1; k < 5; ++k)
            if (std::memcmp(ram + ((cur_.parts + 24u * k + 4u) & 0x1FFFFFu), cur_.raw[k], 18) != 0) {
                ++restoreMismatch_;
                break;
            }
    ++restores_;
    saved_.clear();
}

std::string BikeShakeFilter::Totals() const {
    char b[520];
    const double n = pitchN_ ? static_cast<double>(pitchN_) : 1.0;
    std::snprintf(b, sizeof(b),
                  "bike shake filter: %zu head-view frame(s) (%zu without the step's part slots), %zu "
                  "restore(s), %zu restore mismatch(es); the game's orientation rows' length %.4f .. %.4f; the body's pitch "
                  "slot - the game's RMS %.3f deg (max %.3f), drawn RMS %.3f deg (max %.3f)\n",
                  frames_, skipped_, restores_, restoreMismatch_, rowLenMin_ > 1e8 ? 0.0 : rowLenMin_, rowLenMax_,
                  std::sqrt(pitchGameSq_ / n), pitchGameMax_, std::sqrt(pitchDrawnSq_ / n), pitchDrawnMax_);
    return b;
}

BikeShakeFilter& ProductBikeShake() {
    static BikeShakeFilter f;
    return f;
}

// ================================================================ the meter
bool BikeShakeMeter::Load(const rr::DiscImage& disc) {
    loaded_ = false;
    const auto geo = disc.Find("DATA/BBLEVEL1.GEO");
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!geo || !overlay) return false;
    const std::vector<rr::Model> models = rr::ParseGeo(disc.ReadFile(*geo));
    const rr::Model* bike = nullptr;
    for (const rr::Model& m : models)
        if (m.id == 100) bike = &m;
    if (bike == nullptr || bike->groups.empty() || bike->groups.front().subMeshes.size() != 5) return false;
    skeleton_ = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    bike_ = bike->groups.front();
    assembly_ = rr::AssembleGroup(bike_, skeleton_);
    if (!assembly_.assembled) return false;
    const float bf = static_cast<float>(rr::LodFactor(bike_));
    // the fork's bar ends (vr_bar_grips.cpp Load)
    const rr::SubMesh& fm = bike_.subMeshes[1];
    float minX = 1e9f, maxX = -1e9f;
    for (uint32_t k = 0; k < fm.vertCount; ++k) {
        const float x = bike_.verts[fm.vertBase + k].x * bf / kModelUnitsPerWorldUnit;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
    }
    probe_.clear();
    probePart_.clear();
    probeGroups_.clear();
    std::fill(std::begin(groupCount_), std::end(groupCount_), size_t{0});
    for (size_t part = 1; part < 5; ++part) {
        const rr::SubMesh& sm = bike_.subMeshes[part];
        for (uint32_t k = 0; k < sm.vertCount && sm.vertBase + k < bike_.verts.size(); ++k) {
            uint8_t g = 0;
            if (part == 1) {
                g |= 1u << kFork;
                const float x = bike_.verts[sm.vertBase + k].x * bf / kModelUnitsPerWorldUnit;
                if (x <= minX + kEndBand || x >= maxX - kEndBand) g |= 1u << kBarEnds;
            } else if (part == 4) {
                g |= 1u << kBody;
            } else {
                g |= 1u << kWheels;
            }
            probe_.push_back(sm.vertBase + k);
            probePart_.push_back(static_cast<uint8_t>(part));
            probeGroups_.push_back(g);
            for (int gi = 0; gi < kGroups; ++gi)
                if (g & (1u << gi)) ++groupCount_[gi];
        }
    }
    // the chain (which sub-mesh hangs from which, turned by which slot) - the log
    std::string chain;
    if (assembly_.programIndex < skeleton_.Programs().size()) {
        const rr::AttachProgram& p = skeleton_.Programs()[assembly_.programIndex];
        for (size_t i = 0; i < p.links.size(); ++i) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s%zu <- %u (slot %u)", i ? ", " : "", i + 1, unsigned(p.links[i].parent),
                          unsigned(p.links[i].matrixPart));
            chain += b;
        }
    }
    std::printf("bike shake meter: model 100's chain: sub-mesh %s; probed vertices: bar ends %zu, fork %zu, "
                "body %zu, wheels %zu\n",
                chain.c_str(), groupCount_[kBarEnds], groupCount_[kFork], groupCount_[kBody], groupCount_[kWheels]);
    loaded_ = true;
    return true;
}

void BikeShakeMeter::Frame(long frame, const float eye[3], const float fwd[3], const float up[3], float unitsPerMetre,
                           double period, const rr::render::Mat4& model, const rr::PartMatrix* parts,
                           const rr::PartMatrix* gameParts) {
    if (!loaded_) return;
    if (!csvTried_) {
        csvTried_ = true;
        if (const char* p = std::getenv("RRJB_SHAKE_LOG")) {
            csv_ = std::fopen(p, "wb");
            if (csv_)
                std::fprintf(csv_, "frame,period,eyeR,eyeU,eyeF,barElevDrawn,barElevRoot,barElevSlot1,barElevSlot2,barElevSlot3,"
                                   "barElevSlot4,barAzimDrawn,forkGame,pitchGame,rollGame,forkDrawn,pitchDrawn,rollDrawn,camFR,camFU,camFF,"
                                   "camUR,camUU,camUF,scale\n");
        }
    }
    mmPerUnit_ = 1000.0f / std::max(1e-6f, unitsPerMetre);
    // the camera's axes
    double f[3] = {fwd[0], fwd[1], fwd[2]}, u[3] = {up[0], up[1], up[2]};
    const auto norm = [](double v[3]) {
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-12)
            for (int k = 0; k < 3; ++k) v[k] /= l;
    };
    norm(f);
    {
        const double d = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
        for (int k = 0; k < 3; ++k) u[k] -= d * f[k];
    }
    norm(u);
    const double r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    // the six poses
    rr::PartMatrix cfg[kConfigs][5];
    for (int c = 0; c < kConfigs; ++c)
        for (int k = 0; k < 5; ++k) cfg[c][k] = rr::PartMatrix{};
    if (parts != nullptr) {
        for (int k = 1; k < 5; ++k) cfg[0][k] = parts[k];
        for (int k = 1; k < 5; ++k) cfg[1 + k][k] = parts[k];
    }
    const float bf = static_cast<float>(rr::LodFactor(bike_));
    Sample s;
    for (int c = 0; c < kConfigs; ++c) {
        const rr::PosedGroup posed = rr::PoseGroup(bike_, skeleton_, assembly_, std::span<const rr::PartMatrix>(cfg[c], 5));
        s.dir[c].resize(probe_.size() * 3);
        for (size_t i = 0; i < probe_.size(); ++i) {
            double p[3];
            PosedPoint(posed, probePart_[i], bike_.verts[probe_[i]], bf, p);
            double w[3];
            for (int k = 0; k < 3; ++k)
                w[k] = static_cast<double>(model.m[12 + k]) + model.m[k] * p[0] + model.m[4 + k] * p[1] + model.m[8 + k] * p[2] -
                       static_cast<double>(eye[k]);
            if (c == 0) // how near the eye comes to the drawn bike (the seat moved back / forward)
                nearest_ = std::min(nearest_, std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * mmPerUnit_);
            double d[3] = {w[0] * r[0] + w[1] * r[1] + w[2] * r[2], w[0] * u[0] + w[1] * u[1] + w[2] * u[2],
                           w[0] * f[0] + w[1] * f[1] + w[2] * f[2]};
            norm(d);
            for (int k = 0; k < 3; ++k) s.dir[c][3 * i + k] = static_cast<float>(d[k]);
        }
    }
    // the eye and the camera in the drawn bike's frame
    {
        double ax[3][3];
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) ax[c][k] = model.m[4 * c + k];
            norm(ax[c]);
        }
        double rel[3];
        for (int k = 0; k < 3; ++k) rel[k] = static_cast<double>(eye[k]) - model.m[12 + k];
        const double* cam[3] = {r, u, f};
        for (int c = 0; c < 3; ++c) {
            s.eyeLocal[c] = static_cast<float>(rel[0] * ax[c][0] + rel[1] * ax[c][1] + rel[2] * ax[c][2]);
            for (int a = 0; a < 3; ++a)
                s.camLocal[3 * a + c] = static_cast<float>(cam[a][0] * ax[c][0] + cam[a][1] * ax[c][1] + cam[a][2] * ax[c][2]);
        }
    }
    if (n_ < 3) s_[n_++] = std::move(s);
    else {
        s_[0] = std::move(s_[1]);
        s_[1] = std::move(s_[2]);
        s_[2] = std::move(s);
    }
    ++frames_;
    const Sample& C = s_[n_ - 1];
    // the series: the bar ends' mean direction's elevation (deg) per config; slot k's own = alone minus the root
    {
        double el[kConfigs] = {}, az = 0.0;
        for (int c = 0; c < kConfigs; ++c) {
            double m[3] = {};
            size_t cnt = 0;
            for (size_t i = 0; i < probe_.size(); ++i)
                if (probeGroups_[i] & (1u << kBarEnds)) {
                    for (int k = 0; k < 3; ++k) m[k] += C.dir[c][3 * i + k];
                    ++cnt;
                }
            if (cnt == 0) continue;
            el[c] = std::atan2(m[1], std::sqrt(m[0] * m[0] + m[2] * m[2])) * kDeg;
            if (c == 0) az = std::atan2(m[0], m[2]) * kDeg;
        }
        series_[0].push_back(static_cast<float>(el[0]));
        series_[1].push_back(static_cast<float>(el[1]));
        for (int k = 0; k < 4; ++k) series_[2 + k].push_back(static_cast<float>(el[2 + k] - el[1]));
        periods_.push_back(static_cast<float>(period));
        if (csv_) {
            const auto ang = [](const rr::PartMatrix* p, double out[3]) {
                out[0] = out[1] = out[2] = 0.0;
                if (p == nullptr) return;
                out[0] = std::atan2(-static_cast<double>(p[1].m[6]), static_cast<double>(p[1].m[0])) * kDeg;
                out[1] = std::atan2(static_cast<double>(p[2].m[7]), static_cast<double>(p[2].m[8])) * kDeg;
                out[2] = std::atan2(static_cast<double>(p[2].m[3]), static_cast<double>(p[2].m[0])) * kDeg;
            };
            double g[3], d[3];
            ang(gameParts, g);
            ang(parts, d);
            std::fprintf(csv_, "%ld,%.6f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f,%.6f,%.6f,"
                               "%.6f,%.6f,%.6f,%.6f\n",
                         frame, period, double(C.eyeLocal[0]), double(C.eyeLocal[1]), double(C.eyeLocal[2]), el[0], el[1],
                         el[2] - el[1], el[3] - el[1], el[4] - el[1], el[5] - el[1], az, g[0], g[1], g[2], d[0], d[1], d[2],
                         double(C.camLocal[6]), double(C.camLocal[7]), double(C.camLocal[8]), double(C.camLocal[3]),
                         double(C.camLocal[4]), double(C.camLocal[5]),
                         std::sqrt(double(model.m[4]) * model.m[4] + double(model.m[5]) * model.m[5] + double(model.m[6]) * model.m[6]) *
                             double(kModelUnitsPerWorldUnit)); // the drawn bike's scale (its up axis, 1.0 = the model's size)
        }
    }
    if (n_ < 3) return;
    const Sample &A = s_[0], &B = s_[1];
    ++n2_;
    // per config and group: the first difference (C - B) and the second (C - 2B + A), angles in degrees
    for (int c = 0; c < kConfigs; ++c) {
        double v2[kGroups] = {}, a2[kGroups] = {};
        for (size_t i = 0; i < probe_.size(); ++i) {
            double dv = 0.0, da = 0.0;
            for (int k = 0; k < 3; ++k) {
                const size_t j = 3 * i + static_cast<size_t>(k);
                const double x1 = C.dir[c][j] - B.dir[c][j], x2 = C.dir[c][j] - 2.0 * B.dir[c][j] + A.dir[c][j];
                dv += x1 * x1;
                da += x2 * x2;
            }
            for (int g = 0; g < kGroups; ++g)
                if (probeGroups_[i] & (1u << g)) v2[g] += dv, a2[g] += da;
        }
        for (int g = 0; g < kGroups; ++g) {
            const double nG = std::max<double>(1.0, static_cast<double>(groupCount_[g]));
            vel_[c][g] += v2[g] / nG * kDeg * kDeg;
            acc_[c][g] += a2[g] / nG * kDeg * kDeg;
            if (c == 0) accMax_[g] = std::max(accMax_[g], std::sqrt(a2[g] / nG) * kDeg);
        }
    }
    // slot k's own displacement (alone minus the rest pose)
    for (int k = 0; k < 4; ++k) {
        double v2[kGroups] = {}, a2[kGroups] = {};
        for (size_t i = 0; i < probe_.size(); ++i) {
            double dv = 0.0, da = 0.0;
            for (int q = 0; q < 3; ++q) {
                const size_t j = 3 * i + static_cast<size_t>(q);
                const double dc = C.dir[2 + k][j] - C.dir[1][j], db = B.dir[2 + k][j] - B.dir[1][j],
                             dA = A.dir[2 + k][j] - A.dir[1][j];
                dv += (dc - db) * (dc - db);
                da += (dc - 2.0 * db + dA) * (dc - 2.0 * db + dA);
            }
            for (int g = 0; g < kGroups; ++g)
                if (probeGroups_[i] & (1u << g)) v2[g] += dv, a2[g] += da;
        }
        for (int g = 0; g < kGroups; ++g) {
            const double nG = std::max<double>(1.0, static_cast<double>(groupCount_[g]));
            slotVel_[k][g] += v2[g] / nG * kDeg * kDeg;
            slotAcc_[k][g] += a2[g] / nG * kDeg * kDeg;
        }
    }
    // the eye's anchor in the bike's frame: its position (mm) and axes (deg), second differences
    {
        double p2 = 0.0, a2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double x = (C.eyeLocal[k] - 2.0 * B.eyeLocal[k] + A.eyeLocal[k]) * mmPerUnit_;
            p2 += x * x;
        }
        for (int k = 0; k < 9; ++k) {
            const double x = C.camLocal[k] - 2.0 * B.camLocal[k] + A.camLocal[k];
            a2 += x * x;
        }
        eyePosAcc_ += p2;
        eyePosMax_ = std::max(eyePosMax_, std::sqrt(p2));
        eyeAngAcc_ += a2 / 3.0 * kDeg * kDeg; // per axis
    }
}

std::string BikeShakeMeter::Totals() const {
    if (frames_ == 0) return {};
    const double n = n2_ ? static_cast<double>(n2_) : 1.0;
    double meanPeriod = 0.0;
    for (float p : periods_) meanPeriod += p;
    meanPeriod = periods_.empty() || meanPeriod <= 0.0 ? 1.0 / 72.0 : meanPeriod / static_cast<double>(periods_.size());
    // the dominant frequency of a series: detrended by a centred 0.5 s mean, the peak of the DFT between 0.5 Hz and
    // Nyquist in 0.1 Hz steps; `amp`: the detrended signal's RMS (deg)
    const auto spectrum = [meanPeriod](const std::vector<float>& x, double& peakHz, double& amp) {
        peakHz = 0.0;
        amp = 0.0;
        const size_t N = x.size();
        if (N < 32) return;
        const int half = std::max(1, static_cast<int>(std::lround(0.25 / meanPeriod)));
        std::vector<double> y(N);
        for (size_t i = 0; i < N; ++i) {
            const size_t lo = i >= static_cast<size_t>(half) ? i - static_cast<size_t>(half) : 0;
            const size_t hi = std::min(N - 1, i + static_cast<size_t>(half));
            double m = 0.0;
            for (size_t j = lo; j <= hi; ++j) m += x[j];
            y[i] = x[i] - m / static_cast<double>(hi - lo + 1);
            amp += y[i] * y[i];
        }
        amp = std::sqrt(amp / static_cast<double>(N));
        double best = -1.0;
        const double nyq = 0.5 / meanPeriod;
        for (double hz = 0.5; hz < nyq; hz += 0.1) {
            double re = 0.0, im = 0.0;
            const double w = 2.0 * kPi * hz * meanPeriod;
            for (size_t i = 0; i < N; ++i) {
                re += y[i] * std::cos(w * static_cast<double>(i));
                im += y[i] * std::sin(w * static_cast<double>(i));
            }
            const double p = re * re + im * im;
            if (p > best) best = p, peakHz = hz;
        }
    };
    static const char* const kGroupName[kGroups] = {"bar ends", "fork", "body", "wheels"};
    std::string out;
    char b[900];
    std::snprintf(b, sizeof(b),
                  "bike shake: %zu head-view frame(s) measured (%zu with three in a row), %.1f Hz; the "
                  "drawn bike's vertices in the eye, RMS per frame of the second difference (the shake, deg) / the first "
                  "(the motion, deg):\n",
                  frames_, n2_, 1.0 / meanPeriod);
    out += b;
    for (int g = 0; g < kGroups; ++g) {
        std::snprintf(b, sizeof(b),
                      "  %-8s drawn %.4f / %.4f (max %.4f); root (slots at rest) %.4f / %.4f; own: slot 1 fork %.4f / %.4f, "
                      "slot 2 pitch %.4f / %.4f, slot 3 %.4f / %.4f, slot 4 %.4f / %.4f\n",
                      kGroupName[g], std::sqrt(acc_[0][g] / n), std::sqrt(vel_[0][g] / n), accMax_[g], std::sqrt(acc_[1][g] / n),
                      std::sqrt(vel_[1][g] / n), std::sqrt(slotAcc_[0][g] / n), std::sqrt(slotVel_[0][g] / n),
                      std::sqrt(slotAcc_[1][g] / n), std::sqrt(slotVel_[1][g] / n), std::sqrt(slotAcc_[2][g] / n),
                      std::sqrt(slotVel_[2][g] / n), std::sqrt(slotAcc_[3][g] / n), std::sqrt(slotVel_[3][g] / n));
        out += b;
    }
    std::snprintf(b, sizeof(b), "  the eye's anchor in the drawn bike's frame: second difference RMS %.3f mm (max %.3f), axes %.4f deg; "
                  "the drawn bike's nearest vertex to the eye %.0f mm\n",
                  std::sqrt(eyePosAcc_ / n), eyePosMax_, std::sqrt(eyeAngAcc_ / n), nearest_ > 1e8 ? -1.0 : nearest_);
    out += b;
    static const char* const kSeries[6] = {"drawn", "root", "slot 1 fork", "slot 2 pitch", "slot 3", "slot 4"};
    out += "  the bar ends' elevation in the eye, high-passed (0.5 s): ";
    for (int k = 0; k < 6; ++k) {
        double hz = 0.0, amp = 0.0;
        spectrum(series_[k], hz, amp);
        std::snprintf(b, sizeof(b), "%s%s %.4f deg RMS at %.1f Hz", k ? ", " : "", kSeries[k], amp, hz);
        out += b;
    }
    out += "\n";
    return out;
}

BikeShakeMeter& ProductBikeShakeMeter() {
    static BikeShakeMeter m;
    return m;
}

} // namespace rrgame
