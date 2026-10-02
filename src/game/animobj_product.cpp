#include "game/animobj_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/sim/anim.h"

namespace rr::game {
namespace {

namespace s = rr::sim;

struct Totals {
    bool setUp = false, on = true;
    int32_t count = 0;
    uint32_t objects = 0, programs = 0;
    s::RaceStartCounts start;
    uint32_t stanceRefused = 0;
    // the frame measurement
    uint64_t frames = 0, sharedFrames = 0, sharedObjects = 0, staleOwners = 0;
    uint32_t sharedMax = 0, firstSharedFrame = 0, firstObject = 0, firstOwner = 0, firstOther = 0;
    uint32_t usedMax = 0;
};
Totals& T() {
    static Totals t;
    return t;
}

// SLUS 0x8001447C's block rule (model_runtime.cpp's Bump): size (n + 11) & ~7, the caller gets +4.
struct BumpMalloc final : s::AnimObjectsCallees {
    uint32_t next = 0, limit = 0;
    bool refused = false;
    bool Malloc(uint32_t bytes, uint32_t, uint32_t, uint32_t& v0) override {
        const uint32_t size = (bytes + 11u) & ~7u;
        if (next + size > limit) {
            refused = true;
            v0 = 0;
            return false;
        }
        v0 = next + 4u;
        next += size;
        return true;
    }
};

// The arm callees: the session runs the live / dormant arm at the end of its set-up (race_session.cpp, after
// StartSpeech) - counted here, performed there.
struct StartCallees final : s::RaceStartCallees {
    const std::function<bool(uint32_t, uint32_t, uint32_t)>& stance;
    explicit StartCallees(const std::function<bool(uint32_t, uint32_t, uint32_t)>& st) : stance(st) {}
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override {
        if (!stance(ev, r, p)) ++T().stanceRefused;
        return true; // a refused stance leaves that rider as the layer left it (the session notes it)
    }
    bool EntityCell(uint32_t, uint32_t) override { return true; }
    bool BuildObb(uint32_t, uint32_t) override { return true; }
    bool Transition(uint32_t, uint32_t, uint32_t) override { return true; }
    bool Tail(uint32_t, int, uint32_t, uint32_t) override { return false; } // never asked: the loop only
};

} // namespace

bool AnimObjOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_ANIMOBJ");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

std::string AnimObjectsSetup(s::GuestRam& g, uint32_t at, uint32_t limit, uint32_t& objects, uint32_t& programs, int32_t count) {
    Totals& t = T();
    t.setUp = true;
    t.on = true;
    const int32_t n = count >= 0 ? count : s::AnimObjectCount(g); // SetUpRace's own (the race loader) when given
    t.count = n;
    BumpMalloc bump;
    bump.next = at - 4u; // the first block's user pointer is `at`
    bump.limit = limit;
    char b[512];
    if (n <= 0 || !s::AnimObjectsInit(g, s::kAnimDescriptor, n, 0x801FF000u, bump) || g.Faulted()) {
        g.ClearFault();
        std::snprintf(b, sizeof(b),
                      "the rider animation objects: RASHCDI 0x8005D1A0 (PORTED) REFUSED for %d object(s)%s", n,
                      bump.refused ? " - they do not fit below the AI planner's stack (OURS: where)" : "");
        objects = programs = 0;
        return b;
    }
    objects = t.objects = g.U32(s::kAnimDescriptor + 0u);
    programs = t.programs = g.U32(s::kAnimDescriptor + 4u);
    std::snprintf(b, sizeof(b),
                  "the rider animation objects are the ORIGINAL's: RASHCDI 0x8005D1A0 (PORTED) made %d object(s) "
                  "(the loader's count 0x80063844: pool 1 %u + 4 x pedestrian switch %u + 1, race type %u) at 0x%08X, "
                  "programs 0x%08X (OURS: where - SLUS 0x8001447C's block rule from rr-race's address), all free",
                  n, g.U32(s::kAoPool1Count), g.U32(s::kAoPedSwitch), g.U8(g.U32(s::kAnimGameStatePtr) + 4u), objects,
                  programs);
    return b;
}

std::string AnimObjectsStart(s::GuestRam& g, uint32_t sp, const std::function<bool(uint32_t, uint32_t, uint32_t)>& stance) {
    Totals& t = T();
    StartCallees c(stance);
    s::RaceStartCounts n;
    const bool ok = s::RaceStartBikes(g, sp, c, &n);
    t.start = n;
    char b[600];
    std::snprintf(b, sizeof(b),
                  "each rider's animation object and starting stance are the ORIGINAL's: SLUS 0x800119C0's loop "
                  "(PORTED)%s - ViewSlot 0x80012884 gave %u rider(s) and %u passenger rider(s) an object (%u got none), "
                  "BankSwitch 0x80012858 on %u, the stance event 0x800C4550 6 / 4 (%u refused by the layer); the "
                  "bike's arm (EntityCell / BuildObb %u, Transition(e, 1) %u) runs at the end of the set-up (OURS "
                  "order); objects in use %u of %u",
                  ok ? "" : " REFUSED part way (a view fault)", n.riders, n.passengers, n.noObject, n.switched,
                  t.stanceRefused, n.live, n.dormant, g.U32(s::kAnimDescriptor + 8u), g.U32(s::kAnimDescriptor + 12u));
    if (!ok) g.ClearFault();
    return b;
}

void AnimObjFrame(s::GuestRam& g) {
    Totals& t = T();
    ++t.frames;
    const uint32_t desc = s::kAnimDescriptor;
    const uint32_t objs = g.U32(desc + 0u);
    const int32_t count = g.S32(desc + 12u);
    const uint32_t used = g.U32(desc + 8u);
    if (used > t.usedMax) t.usedMax = used;
    if (objs < 0x80000000u || count <= 0 || count > 64) return;
    uint32_t shared = 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t a = objs + s::kAnimObjectBytes * static_cast<uint32_t>(i);
        if (g.U32(a + s::animf::kFlags) == 0u) continue;
        const uint32_t owner = g.U32(a + s::animf::kOwner);
        if (owner < 0x80000000u || owner >= 0x80200000u) continue;
        if (g.U32(owner + 0x21Cu) == a || g.U32(owner + 0x22Cu) == a) continue;
        // a weapon record (WeaponObject 0x800958F0): the object is held by the rider it is attached to (+0x34)
        // at the rider's +0x22C
        const uint32_t parent = g.U32(owner + 0x34u);
        if (parent >= 0x80000000u && parent < 0x80200000u && g.U32(parent + 0x22Cu) == a) continue;
        // In use, and its owner's animation word names another object: find who else holds it.
        ++shared;
        if (t.sharedObjects == 0 && shared == 1) {
            t.firstSharedFrame = static_cast<uint32_t>(t.frames);
            t.firstObject = a;
            t.firstOwner = owner;
            t.firstOther = g.U32(owner + 0x21Cu);
        }
    }
    // Owners of pool 0 (bikes) and pool 1 (riders) whose object names another owner.
    for (uint32_t pool = 0; pool < 2; ++pool) {
        const uint32_t tab = 0x800CE4D0u + 16u * pool;
        const uint32_t base = g.U32(tab), stride = g.U32(tab + 4u), highp = g.U32(tab + 12u);
        if (base < 0x80000000u || highp < 0x80000000u || stride == 0u || stride > 0x1000u) continue;
        const int32_t high = g.S32(highp);
        for (int32_t k = 0; k <= high && k < 64; ++k) {
            const uint32_t e = base + stride * static_cast<uint32_t>(k);
            const uint32_t a = g.U32(e + 0x21Cu);
            if (a < objs || a >= objs + s::kAnimObjectBytes * static_cast<uint32_t>(count)) continue;
            if (g.U32(a + s::animf::kOwner) != e) ++t.staleOwners;
        }
    }
    g.ClearFault();
    if (shared != 0) {
        ++t.sharedFrames;
        t.sharedObjects += shared;
        if (shared > t.sharedMax) t.sharedMax = shared;
    }
}

std::string AnimObjTotals() {
    const Totals& t = T();
    char b[900];
    if (!AnimObjOn()) {
        std::snprintf(b, sizeof(b),
                      "animobj: SWITCHED OFF (RRJB_ANIMOBJ=off: the session's own object per rider, +0x24 left 0); "
                      "objects whose owner holds another object: %llu object-frame(s) in %llu of %llu frame(s) (at most "
                      "%u; first at frame %u: object 0x%08X, its owner 0x%08X holds 0x%08X); owners (pools 0 / 1) whose "
                      "object names another owner: %llu owner-frame(s); objects in use at most %u\n",
                      static_cast<unsigned long long>(t.sharedObjects), static_cast<unsigned long long>(t.sharedFrames),
                      static_cast<unsigned long long>(t.frames), t.sharedMax, t.firstSharedFrame, t.firstObject,
                      t.firstOwner, t.firstOther, static_cast<unsigned long long>(t.staleOwners), t.usedMax);
        return b;
    }
    std::snprintf(b, sizeof(b),
                  "animobj: the ORIGINAL's objects (RASHCDI 0x8005D1A0 %d object(s)%s, SLUS 0x800119C0's ViewSlot "
                  "loop %u + %u passenger(s), %u without an object); objects whose owner holds another object: %llu "
                  "object-frame(s) in %llu of %llu frame(s) (at most %u; first at frame %u: object 0x%08X, its owner "
                  "0x%08X holds 0x%08X); owners (pools 0 / 1) whose object names another owner: %llu owner-frame(s); "
                  "objects in use at most %u\n",
                  t.count, t.setUp ? "" : " - NOT SET UP", t.start.riders, t.start.passengers, t.start.noObject,
                  static_cast<unsigned long long>(t.sharedObjects), static_cast<unsigned long long>(t.sharedFrames),
                  static_cast<unsigned long long>(t.frames), t.sharedMax, t.firstSharedFrame, t.firstObject,
                  t.firstOwner, t.firstOther, static_cast<unsigned long long>(t.staleOwners), t.usedMax);
    return b;
}

} // namespace rr::game
