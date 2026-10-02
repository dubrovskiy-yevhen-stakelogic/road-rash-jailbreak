#include "game/solid_product.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "game/sim/collision.h"
#include "game/sim/fixed.h"
#include "game/sim/solid.h"
#include "game/sim/world_pop.h"
#include "game/world_pop_product.h"

namespace rr::game {
namespace {

namespace s = rr::sim;

constexpr uint32_t kPool0Ptr = 0x8005B3A0;  // -> pool-0 slot 0, stride 1096
constexpr uint32_t kPoolTab4 = 0x800CE510;  // pool 4: base, stride, -> live, -> high
constexpr uint32_t kPoleReact = 0x800AF0A0; // (bike, shape, n, code, [flags, push])
constexpr int kChainFrames = 90;

struct Chain {
    int left = 0;
    uint32_t stance = 0, mount = 0, req = 0;
    int bike = -1;
};

struct State {
    uint64_t frame = 0;
    std::string pending;
    std::map<uint32_t, Chain> chains;       // rider -> its tracking
    std::map<uint32_t, int> propTilt;       // prop -> 0 upright, 1 tipping, 2 fallen (row 1's y below 0.5)
    std::map<uint64_t, bool> inside;        // (bike slot, shape) -> overlapping last frame
    // totals
    size_t poleResolves = 0, leanTests = 0, leanHits = 0, poleHits = 0, playerPoleHits = 0;
    std::map<uint32_t, size_t> hitClass;    // flagsC & 0x3F after the reaction -> count
    size_t topples = 0, kicks = 0, knocks = 0, carProps = 0, upRows = 0, surfaceSounds = 0;
    size_t chainKnockOffs = 0, chainLaunches = 0;
    size_t propsTipped = 0, propsFallen = 0;
    size_t insideFrames = 0, insideEntries = 0, animFrames = 0, animRefused = 0;
    int32_t maxInsideDepth = 0;
};
State& St() {
    static State st;
    return st;
}

void Note(const char* fmt, ...) {
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (St().pending.size() < 4096) St().pending += b; // bounded: without --log nobody drains it
}

int BikeSlot(s::GuestRam& g, uint32_t e) {
    const uint32_t base = g.U32(kPool0Ptr);
    if (e < base || (e - base) % 1096u != 0 || (e - base) / 1096u >= 18u) return -1;
    return static_cast<int>((e - base) / 1096u);
}

// What a pole-like shape is: "vol<slot> cls<n>" for a pool-6 record, "prop<slot> grp<n>" for a prop's +0xAC.
std::string ShapeName(s::GuestRam& g, uint32_t sh) {
    const uint32_t h = g.U16(sh);
    char b[64];
    if ((h >> 5) == 6u) std::snprintf(b, sizeof(b), "vol%u cls%u", h & 31u, g.U32(sh + 8));
    else if ((h >> 5) == 4u) std::snprintf(b, sizeof(b), "prop%u grp%u", h & 31u, g.U32(sh + 8));
    else std::snprintf(b, sizeof(b), "h%03X", h);
    return b;
}

void Track(s::GuestRam& g, uint32_t rider, int bike) {
    if (rider == 0) return;
    Chain& c = St().chains[rider];
    if (c.left == 0) {
        c.stance = g.U16(rider + 544);
        c.mount = g.U32(rider + 604);
        c.req = g.U32(rider + 552) & 0x8000u;
    }
    c.left = kChainFrames;
    c.bike = bike;
}

// Every pole the collision pass can meet: pool-6 records with a class (the pole resolver's), and pool-4
// props whose model is a pole (+0x0E bit 1) and not moving (+0x250 & 0x1A06 == 0) - the dispatch of
// 0x8005B910. Returns the shape addresses (the record, or the prop's +0xAC).
std::vector<uint32_t> Poles(s::GuestRam& g) {
    std::vector<uint32_t> out;
    const uint32_t v6 = g.U32(s::kWpPool6Ptr);
    const int32_t hi6 = g.S32(s::kWpPool6Ctrl + 8u);
    for (int32_t i = 0; v6 != 0 && i <= hi6 && i < 32; ++i) {
        const uint32_t v = v6 + 280u * static_cast<uint32_t>(i);
        if (g.U16(v) != 0 && g.U32(v + 8) != 0) out.push_back(v);
    }
    const uint32_t b4 = g.U32(kPoolTab4);
    const int32_t hi4 = g.S32(g.U32(kPoolTab4 + 12));
    for (int32_t i = 0; b4 != 0 && i <= hi4 && i < 32; ++i) {
        const uint32_t p = b4 + 596u * static_cast<uint32_t>(i);
        if (g.U16(p + 172) == 0) continue;
        const uint32_t model = g.U32(p);
        if (model == 0 || !(g.U16(model + 14) & 2u) || (g.U32(p + 592) & 0x1A06u)) continue;
        out.push_back(p + 172);
    }
    return out;
}

void Measure(s::GuestRam& g) {
    State& st = St();
    // (1) a riding bike whose box footprint overlaps a pole's (PoleResolve's lg / lt against the box widened by
    // the pole's radius): it is inside the pole
    const uint32_t base = g.U32(kPool0Ptr);
    const std::vector<uint32_t> poles = Poles(g);
    for (uint32_t k = 0; base != 0 && k < 18; ++k) {
        const uint32_t e = base + 1096u * k;
        if (g.S16(e + 320) == 0 || (g.U32(e + 568) & 0x600u)) continue;
        const int32_t bx = g.S32(e + 184), bz = g.S32(e + 192), c = g.S32(e + 296), sn = g.S32(e + 300);
        const int32_t hw = g.S32(e + 304), hl = g.S32(e + 308);
        for (uint32_t sh : poles) {
            const int32_t dx = g.S32(sh + 12) - bx, dz = g.S32(sh + 20) - bz;
            if (dx > 0x100000 || dx < -0x100000 || dz > 0x100000 || dz < -0x100000) continue;
            const int32_t lg = s::FixMul(sn, dx) + s::FixMul(c, dz);
            const int32_t lt = s::FixMul(c, dx) - s::FixMul(sn, dz);
            // the pole's footprint radius as PoleResolve takes it (its lower radius; a prop's smaller half extent)
            int32_t r = g.S32(sh + 132);
            if ((g.U16(sh) >> 5) != 6u && g.S32(sh + 136) < r) r = g.S32(sh + 136);
            r -= 16;
            const int32_t dl = hl + r - (lg < 0 ? -lg : lg), dw = hw + r - (lt < 0 ? -lt : lt);
            const bool in = dl > 0 && dw > 0 && std::abs(g.S32(sh + 16) - g.S32(e + 188)) < 0x40000;
            if (k == 0 && std::getenv("RRJB_SOLID_DEBUG") != nullptr && dl > -0x10000 && dw > -0x10000)
                Note(" near b0 %s lg %.2f lt %.2f hl %.2f hw %.2f r %.2f dy %.2f", ShapeName(g, sh).c_str(), lg / 65536.0,
                     lt / 65536.0, hl / 65536.0, hw / 65536.0, r / 65536.0, (g.S32(sh + 16) - g.S32(e + 188)) / 65536.0);
            const uint64_t key = (static_cast<uint64_t>(k) << 32) | sh;
            if (in) {
                ++st.insideFrames;
                const int32_t depth = dl < dw ? dl : dw;
                if (depth > st.maxInsideDepth) st.maxInsideDepth = depth;
                if (!st.inside[key]) {
                    ++st.insideEntries;
                    Note(" INSIDE b%u %s depth %.2f", k, ShapeName(g, sh).c_str(), depth / 65536.0);
                }
            }
            st.inside[key] = in;
        }
    }
    // (2) the crash chain of every bike that hit a pole
    for (auto it = st.chains.begin(); it != st.chains.end();) {
        Chain& c = it->second;
        const uint32_t r = it->first;
        const uint32_t stance = g.U16(r + 544), mount = g.U32(r + 604), req = g.U32(r + 552) & 0x8000u;
        if (stance != c.stance || mount != c.mount || req != c.req) {
            Note(" chain b%d stance %u->%u mount %u->%u%s", c.bike, c.stance, stance, c.mount, mount,
                 (req && !c.req) ? " knock-off request +0x228|=0x8000" : "");
            if (mount == 2 && c.mount < 2) ++st.chainKnockOffs;
            if (mount == 3 && c.mount != 3) ++st.chainLaunches;
            c.stance = stance;
            c.mount = mount;
            c.req = req;
        }
        if (--c.left <= 0) it = st.chains.erase(it);
        else ++it;
    }
    // (3) props that tip and fall: row 1 (+0x1B6)'s y below 0.5
    const uint32_t b4 = g.U32(kPoolTab4);
    const int32_t hi4 = g.S32(g.U32(kPoolTab4 + 12));
    for (int32_t i = 0; b4 != 0 && i <= hi4 && i < 32; ++i) {
        const uint32_t p = b4 + 596u * static_cast<uint32_t>(i);
        if (g.U16(p + 172) == 0) {
            st.propTilt.erase(p);
            continue;
        }
        const uint32_t f = g.U32(p + 592);
        int& state = st.propTilt[p];
        const int32_t upY = g.S16(p + 440);
        if (state == 0 && (f & 0x203u)) {
            state = 1;
            ++st.propsTipped;
            Note(" prop%d grp%u %s (flags 0x%03X, speed %.2f)", i, g.U32(p + 180), (f & 0x200u) ? "knocked, toppling" : "kicked, tumbling",
                 f, g.S32(p + 480) / 65536.0);
        }
        if (state == 1 && upY < 2048) {
            state = 2;
            ++st.propsFallen;
            Note(" prop%d grp%u FELL (row 1 y %.3f, flags 0x%03X)", i, g.U32(p + 180), upY / 4096.0, f);
        }
    }
}

} // namespace

bool SolidOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_SOLID");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

void SolidObserve(s::GuestRam& g, uint32_t fn, const uint32_t* a, int n, uint32_t v0) {
    State& st = St();
    switch (fn) {
    case s::solid::kLeanPoleTest:
        ++st.leanTests;
        if (v0 < 6u) ++st.leanHits;
        break;
    case 0x800AE794u: ++st.poleResolves; break;
    case kPoleReact: {
        if (n < 4) break;
        const uint32_t e = a[0], sh = a[1];
        const uint32_t pool = g.U16(sh) >> 5;
        if (pool != 6u && pool != 4u) break;                                   // a pole: a volume or a prop
        const int slot = BikeSlot(g, e);
        ++st.poleHits;
        const uint32_t cls = g.U32(e + 568) & 0x3Fu;
        ++st.hitClass[cls];
        if (slot >= 0 && static_cast<uint32_t>(slot) < g.U32(g.U32(0x8005B2F8u) + 48u)) ++st.playerPoleHits;
        Note(" POLE b%d %s code %u class 0x%02X +0x240 %.2f +0x1E0 %.2f", slot, ShapeName(g, sh).c_str(), a[3], cls,
             g.S32(e + 576) / 65536.0, g.S32(e + 480) / 65536.0);
        Track(g, g.U32(e + 852), slot);
        break;
    }
    case s::solid::kPropTopple: ++st.topples; break;
    case s::solid::kPropKick: ++st.kicks; break;
    case s::solid::kPropKnock: ++st.knocks; break;
    case s::solid::kTrafficVsProp: ++st.carProps; break;
    case s::solid::kPropUpRows: ++st.upRows; break;
    case s::solid::kSurfaceSound: ++st.surfaceSounds; break;
    default: break;
    }
}

bool SolidEnginePass(s::GuestRam& g, int32_t dt, uint32_t sp, const s::BikeTables& t, s::PopulationCallees& pop,
                     const std::function<void(const std::string&)>& note) {
    State& st = St();
    ++st.frame;
    bool ok = true;
    if (SolidOn()) {
        WorldProductCallees wc(g, pop, t);
        wc.note = note;
        ++st.animFrames;
        ok = s::PropAnimPass(g, dt, sp, t, wc) && !g.Faulted();
        if (!ok) {
            ++st.animRefused;
            if (note)
                note("RASHCDG 0x800A2A64 PropAnimPass (PORTED, solid.h) refused a frame (a fault, Normalize's overflow, "
                     "QuatToMatrix's trapping add, or a refused callee): what it wrote up to there stays");
            g.ClearFault();
        }
    }
    Measure(g);
    return ok;
}

std::string SolidFrameLog() {
    State& st = St();
    if (st.pending.empty()) return std::string();
    std::string s = "        solid" + st.pending + "\n";
    st.pending.clear();
    return s;
}

std::string SolidTotals() {
    State& st = St();
    char b[900];
    std::string cls;
    for (const auto& [c, k] : st.hitClass) {
        char x[32];
        std::snprintf(x, sizeof(x), " 0x%02X:%zu", c, k);
        cls += x;
    }
    std::snprintf(b, sizeof(b),
                  "the solid objects (solid_product.h)%s: PoleResolve 0x800AE794 %zu call(s), LeanPoleTest 0x800ADC74 %zu (%zu "
                  "contact(s)); pole hits (PoleReact on a volume / prop) %zu, the players' %zu, hit class after the "
                  "reaction:%s; the crash chains after a pole hit: %zu knock-off(s) (mount 2), %zu launch(es) (mount 3); "
                  "PropTopple %zu, PropKick %zu, PropKnock %zu, TrafficVsProp %zu, PropUpRows %zu, SurfaceSound %zu; "
                  "PropAnimPass 0x800A2A64 %zu frame(s), %zu refused; props tipped %zu, fallen %zu; a riding bike with a "
                  "pole's footprint overlapping its box: %zu frame(s), %zu entry(ies), deepest %.2f\n",
                  SolidOn() ? "" : " SWITCHED OFF (RRJB_SOLID=off, the negative control)", st.poleResolves, st.leanTests,
                  st.leanHits, st.poleHits, st.playerPoleHits, cls.empty() ? " none" : cls.c_str(), st.chainKnockOffs,
                  st.chainLaunches, st.topples, st.kicks, st.knocks, st.carProps, st.upRows, st.surfaceSounds, st.animFrames,
                  st.animRefused, st.propsTipped, st.propsFallen, st.insideFrames, st.insideEntries,
                  st.maxInsideDepth / 65536.0);
    return b;
}

} // namespace rr::game
