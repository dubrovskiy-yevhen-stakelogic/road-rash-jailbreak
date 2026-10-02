// VR weapon snatching (vr_snatch.h).
#include "vr_snatch.h"

#include "game/fight_session.h"
#include "game/race_session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rrgame {

namespace {

constexpr float kWeaponRadius = 0.05f; // m: the rival's weapon capsule (vr_melee.cpp's own weapon's)
constexpr double kCatchWindow = 0.25;  // s after the grip closed: a hand that closes ON the weapon, not one that was shut
constexpr double kHoldMax = 0.6;       // s: the longest a grab is held for the rival's steal window
constexpr float kKeep = 0.35f;         // m: the hand stays this near the weapon while it holds it
constexpr float kGripClosed = 0.5f;    // the grip button closes the hand (vr_melee.cpp's fist)
constexpr uint32_t kObjSlots = 0x800CF018, kObjBytes = 172; // the eight weapon model objects (weapon_draw.cpp)

using V3 = std::array<float, 3>;
V3 Make(const float* p) { return {p[0], p[1], p[2]}; }
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Lerp(const V3& a, const V3& b, float t) { return Add(a, Mul(Sub(b, a), t)); }
float PointSeg(const V3& p, const V3& a, const V3& b) {
    const V3 ab = Sub(b, a);
    const float l2 = Dot(ab, ab);
    const float t = l2 > 1e-12f ? std::clamp(Dot(Sub(p, a), ab) / l2, 0.0f, 1.0f) : 0.0f;
    return Len(Sub(p, Add(a, Mul(ab, t))));
}
uint32_t Word(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
int16_t Half(const uint8_t* ram, uint32_t a) {
    int16_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
    return v;
}

} // namespace

bool VrSnatch::Load(const std::vector<rr::Model>& models, const rr::SkeletonTable& skeleton) {
    groups_.clear();
    skeleton_ = &skeleton;
    for (const rr::Model& m : models)
        if (m.id == 800)
            for (const rr::ModelGroup& g : m.groups) groups_.push_back({g, rr::AssembleGroup(g, skeleton)});
    return !groups_.empty();
}

void VrSnatch::BeginRivals() { next_.clear(); }

bool VrSnatch::Swinging(const uint8_t* ram, uint32_t bike) {
    if (ram == nullptr || bike < 0x80000000u || bike >= 0x80200000u) return false;
    const uint32_t rider = Word(ram, bike + 0x354u), rd = Word(ram, bike + 0x43Cu);
    if (rider < 0x80000000u || rider >= 0x80200000u || rd < 0x80000000u || rd >= 0x80200000u) return false;
    const uint32_t stance = static_cast<uint16_t>(Word(ram, rider + 0x220u) & 0xFFFFu);
    const uint32_t category = static_cast<uint16_t>(Word(ram, 0x800541D4u + 8u * stance + 2u) & 0xFFFFu);
    return category == 3u && (ram[(rd + 60u) & 0x1FFFFFu] & 0x80u) != 0;
}

bool VrSnatch::HeldWeapon(const uint8_t* ram, uint32_t rider, const std::function<void(const float*, size_t, float*)>& toWorld,
                          float a[3], float b[3], int& weapon) const {
    if (ram == nullptr || skeleton_ == nullptr || rider < 0x80000000u || rider >= 0x80200000u) return false;
    const uint32_t obj = Word(ram, rider + 0x38u);
    if (obj < kObjSlots || obj >= kObjSlots + 8u * kObjBytes || (obj - kObjSlots) % kObjBytes != 0) return false;
    const uint32_t hand = Word(ram, rider + 0x3Cu);
    const int group = static_cast<int8_t>(ram[(obj + 8u) & 0x1FFFFFu]);
    if (hand >= 17u || group < 0 || static_cast<size_t>(group) >= groups_.size()) return false;
    const Group& wg = groups_[static_cast<size_t>(group)];
    // the weapon's own parts from the object's part slots (+0x04: 24 bytes each, the 3x3 at +4), as weapon_draw.cpp
    const size_t parts = wg.group.subMeshes.size();
    std::vector<rr::PartMatrix> slots(parts);
    const uint32_t array = Word(ram, obj + 4u);
    if (array >= 0x80000000u && array < 0x80200000u)
        for (size_t k = 0; k < parts; ++k)
            for (uint32_t e = 0; e < 9; ++e) slots[k].m[e] = Half(ram, array + 24u * static_cast<uint32_t>(k) + 4u + 2u * e);
    const rr::PosedGroup posed = rr::PoseGroup(wg.group, *skeleton_, wg.assembly, std::span<const rr::PartMatrix>(slots));
    const rr::TriangleSoup soup = rr::BuildPosedTriangleSoup(wg.group, posed);
    if (soup.vertices.empty()) return false;
    float far[3] = {0, 0, 0}, far2 = -1.0f;
    for (const auto& v : soup.vertices) {
        const float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
        if (d2 > far2) far2 = d2, far[0] = v.x, far[1] = v.y, far[2] = v.z;
    }
    const float zero[3] = {0, 0, 0};
    toWorld(zero, hand, a);
    toWorld(far, hand, b);
    weapon = group;
    return true;
}

void VrSnatch::Rival(uint32_t bike, size_t index, bool hittable, bool held, const float a[3], const float b[3], int weapon,
                     const float origin[3], const float right[3], const float up[3], const float fwd[3], bool swinging) {
    RivalWeapon r;
    r.swinging = held && swinging;
    r.bike = bike;
    r.index = index;
    r.hittable = hittable;
    r.have = held;
    r.weapon = held ? weapon : 9;
    for (int k = 0; k < 3; ++k) {
        r.a[k] = a[k], r.b[k] = b[k];
        r.origin[k] = origin[k], r.right[k] = right[k], r.up[k] = up[k], r.fwd[k] = fwd[k];
    }
    for (const RivalWeapon& o : rivals_)
        if (o.bike == bike && o.have && held) {
            r.havePrev = true;
            for (int k = 0; k < 3; ++k) r.prevA[k] = o.a[k], r.prevB[k] = o.b[k], r.prevOrigin[k] = o.origin[k];
            // the tip's speed against the rider's bike (his own swing, not the race's speed)
            const V3 tipNow = Sub(Make(r.b), Make(r.origin)), tipBefore = Sub(Make(o.b), Make(o.origin));
            r.speed = Len(Sub(tipNow, tipBefore)) / static_cast<float>(std::max(1e-3, lastDt_));
        }
    if (!r.havePrev)
        for (int k = 0; k < 3; ++k) r.prevA[k] = r.a[k], r.prevB[k] = r.b[k], r.prevOrigin[k] = r.origin[k];
    static const bool trace = std::getenv("RRJB_SNATCH_TRACE") != nullptr; // DEVELOPMENT: the rivals' weapons
    if (trace && held)
        std::printf("vr snatch trace: f%ld b%zu w%d a (%.3f %.3f %.3f) b (%.3f %.3f %.3f) bike (%.3f %.3f %.3f) tip %.2f u/s\n",
                    frame_, index, weapon, double(a[0]), double(a[1]), double(a[2]), double(b[0]), double(b[1]), double(b[2]),
                    double(origin[0]), double(origin[1]), double(origin[2]), double(r.speed));
    next_.push_back(r);
}

void VrSnatch::EndRivals(int playerWeapon) {
    rivals_.swap(next_);
    next_.clear();
    // the player's weapon in hand turned into the one a hand holds: the steal went through (this frame's
    // WeaponSteal, or FightUpdate's own on a later frame of the punch the grab started)
    for (int h = 0; h < 2; ++h) {
        Hold& hd = hold_[h];
        if (!hd.active) continue;
        if (playerWeapon == hd.weapon && playerWeapon_ != hd.weapon) {
            ++totals_.taken;
            takenHand_ = h;
            takenWeapon_ = hd.weapon;
            char b[96];
            std::snprintf(b, sizeof(b), " f%ld:%s-w%d>b%zu", frame_, h ? "R" : "L", hd.weapon, hd.index);
            if (totals_.list.size() < 300) totals_.list += b;
            std::printf("vr snatch: frame %ld - the %s hand TAKEN rival b%zu's weapon %d (the player's weapon in hand %d -> "
                        "%d, WeaponSteal); it is the weapon hand now\n",
                        frame_, h ? "right" : "left", hd.index, hd.weapon, playerWeapon_, playerWeapon);
            hd = Hold{};
        }
    }
    playerWeapon_ = playerWeapon;
}

const VrSnatch::RivalWeapon* VrSnatch::Nearest(const float eye[3], float range) const {
    const RivalWeapon* best = nullptr;
    float bestD = range;
    for (const RivalWeapon& r : rivals_) {
        if (!r.have || !r.hittable) continue;
        const float d = PointSeg(Make(eye), Make(r.a), Make(r.b));
        if (d < bestD) bestD = d, best = &r;
    }
    return best;
}

bool VrSnatch::Holding(int hand) const { return hand >= 0 && hand < 2 && hold_[hand].active; }

int VrSnatch::TakenHand() {
    const int h = takenHand_;
    takenHand_ = -1;
    return h;
}

void VrSnatch::Finish(int h, const char* why) {
    if (!hold_[h].active) return;
    std::printf("vr snatch: frame %ld - the %s hand lets go of rival b%zu's weapon %d: %s (%d grab(s) sent)\n", frame_,
                h ? "right" : "left", hold_[h].index, hold_[h].weapon, why, hold_[h].sent);
    hold_[h] = Hold{};
}

void VrSnatch::Update(const HandIn hands[2], double clock, float dt, float upm, long frame, bool active,
                      std::vector<rr::game::PhysicalBlowRequest>& out) {
    frame_ = frame;
    upm_ = upm > 0.0f ? upm : 1.0f;
    if (dt > 0.0f) lastDt_ = dt;
    const rr::game::SnatchResult& last = rr::game::LastSnatch();
    for (int h = 0; h < 2; ++h) {
        const HandIn& in = hands[h];
        HandState& st = hand_[h];
        Hold& hd = hold_[h];
        if (!active || !in.valid) {
            if (hd.active) Finish(h, active ? "the hand is not tracked" : "the snatch is off here (not the head view / paused / not Physical)");
            st.havePrev = false;
            st.prevGrip = in.grip;
            continue;
        }
        if (in.grip >= kGripClosed && st.prevGrip < kGripClosed) {
            st.closedAt = clock;
            ++totals_.closes;
        }
        const V3 pos = Make(in.pos);
        const V3 before = st.havePrev ? Make(st.prevPos) : pos;
        // ---- a hand holding a rival's weapon: the game's answer to the last grab, then the next grab
        if (hd.active) {
            const RivalWeapon* r = nullptr;
            for (const RivalWeapon& x : rivals_)
                if (x.bike == hd.bike) r = &x;
            if (hd.sentSerial != 0 && last.serial == hd.sentSerial && last.victim == hd.bike) {
                hd.sentSerial = 0;
                switch (last.outcome) {
                case rr::game::SnatchOutcome::kWait: ++totals_.waits; break;
                case rr::game::SnatchOutcome::kDeclined:
                    ++totals_.declined;
                    hd.declined = true; // the punch is under way: FightUpdate offers WeaponSteal again - watch it
                    break;
                case rr::game::SnatchOutcome::kStolen: break; // EndRivals sees the weapon change hands
                default:
                    ++totals_.skipped;
                    if (last.outcome == rr::game::SnatchOutcome::kSkipped) ++totals_.notSwinging;
                    Finish(h, "the game did not take it (its log line says why)");
                    break;
                }
            }
            if (!hd.active) {
            } else if (in.grip < kGripClosed) {
                ++totals_.dropped;
                Finish(h, "the grip opened");
            } else if (clock - hd.since > kHoldMax) {
                ++totals_.dropped;
                Finish(h, "held 0.6 s without the steal window");
            } else if (r == nullptr || !r->have) {
                // the weapon object left his hand: taken (EndRivals said so already) or put away
                Finish(h, "the rival holds no weapon object any more");
            } else if (PointSeg(pos, Make(r->a), Make(r->b)) > kKeep * upm_) {
                ++totals_.dropped;
                Finish(h, "the weapon left the hand (35 cm)");
            } else if (!hd.declined && hd.sentSerial == 0) {
                rr::game::PhysicalBlowRequest q;
                q.pending = true;
                q.snatch = true;
                q.victim = hd.bike;
                q.right = h == 1;
                q.weapon = false;
                out.push_back(q);
                hd.sentSerial = last.serial + 1;
                ++hd.sent;
                ++totals_.sent;
            }
        } else if (in.free && in.grip >= kGripClosed && clock - st.closedAt <= kCatchWindow) {
            // ---- a hand that just closed: does its palm meet a rival's weapon (both swept since the last frame)?
            const float rr = in.radius + kWeaponRadius * upm_;
            const RivalWeapon* got = nullptr;
            for (const RivalWeapon& r : rivals_) {
                if (!r.have || !r.hittable || r.weapon > 8) continue;
                const float travel = std::max({Len(Sub(pos, before)), Len(Sub(Make(r.a), Make(r.prevA))), Len(Sub(Make(r.b), Make(r.prevB)))});
                const int steps = std::clamp(static_cast<int>(std::ceil(travel / (0.02f * upm_))), 1, 24);
                for (int j = 0; j <= steps && got == nullptr; ++j) {
                    const float f = static_cast<float>(j) / static_cast<float>(steps);
                    if (PointSeg(Lerp(before, pos, f), Lerp(Make(r.prevA), Make(r.a), f), Lerp(Make(r.prevB), Make(r.b), f)) < rr)
                        got = &r;
                }
                if (got != nullptr) break;
            }
            if (got != nullptr) {
                hd = Hold{};
                hd.active = true;
                hd.bike = got->bike;
                hd.index = got->index;
                hd.weapon = got->weapon;
                hd.since = clock;
                ++totals_.catches;
                caughtFrame_ = frame;
                caughtHand_ = h;
                std::printf("vr snatch: frame %ld - the %s hand closed on rival b%zu's weapon %d (%s; its tip at %.1f m/s "
                            "against his bike, %.2f s after the grip closed, the hand %.2f m from the eye): holding it for "
                            "WeaponSteal\n",
                            frame, h ? "right" : "left", got->index, got->weapon, got->swinging ? "he swings it" : "not swinging",
                            double(got->speed / upm_), double(clock - st.closedAt), double(in.reach));
                rr::game::PhysicalBlowRequest q;
                q.pending = true;
                q.snatch = true;
                q.victim = hd.bike;
                q.right = h == 1;
                out.push_back(q);
                hd.sentSerial = last.serial + 1;
                ++hd.sent;
                ++totals_.sent;
            } else if (clock - st.closedAt <= dt * 1.5) {
                // (the closing frame only) a grip that closed near no swinging weapon: counted for the log
                for (const RivalWeapon& r : rivals_)
                    if (r.have && r.hittable && PointSeg(pos, Make(r.a), Make(r.b)) < 0.5f * upm_) ++totals_.missed;
            }
        }
        st.prevGrip = in.grip;
        st.havePrev = true;
        for (int k = 0; k < 3; ++k) st.prevPos[k] = in.pos[k];
    }
}

void VrSnatch::Lines(const float eye[3], float range,
                     const std::function<void(const float*, const float*, float, const float*)>& capsule) const {
    for (const RivalWeapon& r : rivals_) {
        if (!r.have || PointSeg(Make(eye), Make(r.a), Make(r.b)) > range) continue;
        const bool held = (hold_[0].active && hold_[0].bike == r.bike) || (hold_[1].active && hold_[1].bike == r.bike);
        const float rgb[3] = {1.0f, held ? 0.1f : 0.55f, 0.05f};
        capsule(r.a, r.b, kWeaponRadius * upm_, rgb);
    }
}

std::string VrSnatch::Totals() const {
    const auto& t = totals_;
    char b[900];
    std::snprintf(b, sizeof(b),
                  "vr snatch: grips closed %zu, closed on a rival's weapon %zu, grabs sent %zu, weapons TAKEN "
                  "%zu, held for his steal window %zu, the punch started but WeaponSteal declined %zu, not taken by the "
                  "game %zu (not swinging / too late %zu), let go %zu, grips closed near a weapon without touching it %zu;%s",
                  t.closes, t.catches, t.sent, t.taken, t.waits, t.declined, t.skipped, t.notSwinging, t.dropped, t.missed,
                  t.list.empty() ? " (none taken)" : t.list.c_str());
    return b;
}

} // namespace rrgame
