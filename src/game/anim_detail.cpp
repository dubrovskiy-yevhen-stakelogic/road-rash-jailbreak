// Maximum detail's full animation at every distance (anim_detail.h). OURS, render-only.
#include "game/anim_detail.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "game/sim/anim.h"
#include "game/sim/pose.h"

namespace rr::game {
namespace {

namespace af = rr::sim::animf;

constexpr uint32_t kGp = 0x8005AC8C;
constexpr uint32_t kRam = 0x200000u;
constexpr uint32_t kParts = 17u, kSlotBytes = 24u;
constexpr uint32_t kOwnerParts = 4u, kOwnerRoot = 0x1Cu, kOwnerObject = 0x21Cu;
constexpr uint32_t kPedFlags = 0x235u; // bit 3: the clip held by PedRelease 0x800CB558
constexpr uint32_t kBlendMask = af::kBlend + 8u; // B.mask (TransitionCapture)

bool g_on = false;

bool TraceOn() {
    static const bool on = std::getenv("RRJB_ANIM_DETAIL_TRACE") != nullptr;
    return on;
}
bool CheckOn() {
    static const bool on = std::getenv("RRJB_ANIM_DETAIL_CHECK") != nullptr;
    return on;
}

uint32_t Rd32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
bool InRam(uint32_t a, uint32_t n) { return (a & 0xFFE00000u) == 0x80000000u && (a & 0x1FFFFFu) + n <= kRam; }

// This frame's copy (AnimDetailSnapshot).
struct Snapshot {
    uint64_t generation = 0;
    uint32_t objects = 0, programs = 0; // the descriptor's two arrays
    int32_t count = 0;
    std::vector<uint8_t> obj, prog;     // count x 0x83C, count x 60
    std::vector<uint8_t> visible;       // owner +9 per object at the pass
} g_snap;

struct Entry {
    uint64_t generation = 0; // the copy the cached pose belongs to
    bool ok = false;
    RiderPoseView pose;
    AnimDetailInfo info;
};
std::map<uint32_t, Entry> g_entries; // by owner

struct Totals {
    size_t snapshots = 0, poses = 0, full = 0, masked = 0, stepped = 0, frozen = 0, held = 0, refused = 0, checked = 0, checkEqual = 0, checkDiffer = 0, ramChecks = 0, ramChanged = 0;
    double snapUs = 0.0, poseUs = 0.0;
} g_totals;

uint64_t Hash(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Index of `a` in the copy, or -1.
int IndexOf(uint32_t a) {
    if (g_snap.count <= 0 || a < g_snap.objects) return -1;
    const uint32_t off = a - g_snap.objects;
    if (off % rr::sim::kAnimObjectBytes != 0) return -1;
    const uint32_t i = off / rr::sim::kAnimObjectBytes;
    return i < static_cast<uint32_t>(g_snap.count) ? static_cast<int>(i) : -1;
}

AnimDetailInfo Describe(const uint8_t* ram, uint32_t owner, int& index, uint32_t& a) {
    AnimDetailInfo info;
    index = -1;
    a = 0;
    if (!InRam(owner, 0x240u)) return info;
    a = Rd32(ram, owner + kOwnerObject);
    index = IndexOf(a);
    if (index < 0) return info;
    const uint8_t* o = g_snap.obj.data() + static_cast<size_t>(index) * rr::sim::kAnimObjectBytes;
    uint32_t objOwner = 0, flags = 0, mask = 0;
    std::memcpy(&objOwner, o + af::kOwner, 4);
    std::memcpy(&flags, o + af::kFlags, 4);
    std::memcpy(&mask, o + af::kMask, 4);
    if (objOwner != owner) {
        index = -1;
        return info;
    }
    info.object = true;
    info.playing = (flags & 2u) != 0;
    info.visible = static_cast<uint8_t>(g_snap.visible[static_cast<size_t>(index)] & 3u);
    info.posed = info.playing && (info.visible != 0 || !(flags & 0x10u)); // AnimationPass's test
    info.interp = (flags & 4u) != 0;
    info.mask = mask;
    const uint32_t model = Rd32(ram, owner);
    const uint32_t kind = InRam(model, 16u) ? ((Rd32(ram, model + 12u) >> 16) & 0x78u) >> 3 : 0u; // DOD3 +0x0E
    info.lod0Mask = Rd32(ram, rr::sim::kPosePartMasks + (kind == 4u ? 4u : 0u)); // LodChoice at LOD 0
    info.held = !info.playing && kind == 4u && (ram[(owner + kPedFlags) & 0x1FFFFFu] & 8u) != 0;
    return info;
}

// The pose half of ApplyFrame 0x8005D2A8 on `objBytes` / `progBytes` (this frame's copy) swapped into the arena at
// `a`, with the LOD-0 mask and interpolation. Everything written is put back.
bool RunPose(uint8_t* ram, uint32_t a, uint32_t owner, uint32_t lod0Mask, const uint8_t* objBytes,
             const uint8_t* progBytes, RiderPoseView& out) {
    const uint32_t progAddr = Rd32(ram, a + af::kProgram);
    const uint32_t parts = Rd32(ram, owner + kOwnerParts);
    if (!InRam(a, rr::sim::kAnimObjectBytes) || !InRam(progAddr, rr::sim::kAnimProgramBytes) ||
        !InRam(parts, kParts * kSlotBytes) || !InRam(owner + kOwnerRoot, 6u))
        return false;
    uint8_t* liveObj = ram + (a & 0x1FFFFFu);
    uint8_t* liveProg = ram + (progAddr & 0x1FFFFFu);
    uint8_t* liveParts = ram + (parts & 0x1FFFFFu);
    uint8_t* liveRoot = ram + ((owner + kOwnerRoot) & 0x1FFFFFu);
    uint8_t saveObj[rr::sim::kAnimObjectBytes], saveProg[rr::sim::kAnimProgramBytes], saveParts[kParts * kSlotBytes], saveRoot[6];
    std::memcpy(saveObj, liveObj, sizeof(saveObj));
    std::memcpy(saveProg, liveProg, sizeof(saveProg));
    std::memcpy(saveParts, liveParts, sizeof(saveParts));
    std::memcpy(saveRoot, liveRoot, sizeof(saveRoot));
    std::memcpy(liveObj, objBytes, sizeof(saveObj));
    std::memcpy(liveProg, progBytes, sizeof(saveProg));
    rr::sim::GuestRam g(ram, kGp);
    rr::sim::AnimPose pose(g);
    g.W32(a + af::kMask, lod0Mask);                         // LodChoice at LOD 0
    g.W32(a + af::kFlags, g.U32(a + af::kFlags) | 4u);      // interpolation on
    const uint32_t op = progAddr + 12u * g.U32(a + af::kPc);
    if (g.U8(op + 1u) == 3u) {                              // ApplyFrame's op-3 arm: the blend
        g.W32(a + kBlendMask, lod0Mask);
        pose.Blend(a);
    } else {
        pose.Sample(a, a + af::kTrack);
        if (!pose.Failed()) pose.Pose(a);
    }
    const bool ok = !pose.Failed();
    if (ok) {
        const uint8_t* ro = liveRoot;
        for (uint32_t k = 0; k < kParts; ++k)
            for (uint32_t e = 0; e < 9; ++e) {
                int16_t v;
                std::memcpy(&v, liveParts + kSlotBytes * k + 4u + 2u * e, 2);
                out.local[k].m[e] = v;
            }
        for (uint32_t k = 0; k < 3; ++k) std::memcpy(&out.root[k], ro + 2u * k, 2);
        out.stance = static_cast<uint16_t>(Rd32(ram, owner + 0x220u) & 0xFFFFu);
    }
    g.ClearFault();
    std::memcpy(liveObj, saveObj, sizeof(saveObj));
    std::memcpy(liveProg, saveProg, sizeof(saveProg));
    std::memcpy(liveParts, saveParts, sizeof(saveParts));
    std::memcpy(liveRoot, saveRoot, sizeof(saveRoot));
    return ok;
}

} // namespace

void SetAnimDetail(bool on) {
    static const bool off = std::getenv("RRJB_ANIM_DETAIL") != nullptr && std::strcmp(std::getenv("RRJB_ANIM_DETAIL"), "off") == 0;
    g_on = on && !off; // RRJB_ANIM_DETAIL=off: the negative control (the game's own slots drawn)
}
bool AnimDetailOn() { return g_on; }

void AnimDetailSnapshot(const uint8_t* ram) {
    if (!g_on && !TraceOn()) return;
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t desc = rr::sim::kAnimDescriptor;
    const uint32_t objects = Rd32(ram, desc), programs = Rd32(ram, desc + 4u);
    const int32_t count = static_cast<int32_t>(Rd32(ram, desc + 12u));
    ++g_snap.generation;
    g_snap.count = 0;
    if (count <= 0 || count > 64 || !InRam(objects, static_cast<uint32_t>(count) * rr::sim::kAnimObjectBytes) ||
        !InRam(programs, static_cast<uint32_t>(count) * rr::sim::kAnimProgramBytes))
        return;
    g_snap.objects = objects;
    g_snap.programs = programs;
    g_snap.count = count;
    const size_t n = static_cast<size_t>(count);
    g_snap.obj.assign(ram + (objects & 0x1FFFFFu), ram + (objects & 0x1FFFFFu) + n * rr::sim::kAnimObjectBytes);
    g_snap.prog.resize(n * rr::sim::kAnimProgramBytes);
    g_snap.visible.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* o = g_snap.obj.data() + i * rr::sim::kAnimObjectBytes;
        uint32_t owner = 0, prog = 0;
        std::memcpy(&owner, o + af::kOwner, 4);
        std::memcpy(&prog, o + af::kProgram, 4);
        if (InRam(owner, 16u)) g_snap.visible[i] = ram[(owner + 9u) & 0x1FFFFFu];
        if (InRam(prog, rr::sim::kAnimProgramBytes))
            std::memcpy(g_snap.prog.data() + i * rr::sim::kAnimProgramBytes, ram + (prog & 0x1FFFFFu), rr::sim::kAnimProgramBytes);
    }
    ++g_totals.snapshots;
    g_totals.snapUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

AnimDetailInfo AnimDetailDescribe(const uint8_t* ram, uint32_t owner) {
    int index;
    uint32_t a;
    return Describe(ram, owner, index, a);
}

bool AnimDetailPose(uint8_t* ram, uint32_t owner, RiderPoseView& out, AnimDetailInfo* infoOut) {
    Entry& en = g_entries[owner];
    if (en.generation == g_snap.generation && g_snap.generation != 0) { // a later view of the same frame
        if (infoOut) *infoOut = en.info;
        if (en.ok) out = en.pose;
        return en.ok;
    }
    const auto t0 = std::chrono::steady_clock::now();
    en.generation = g_snap.generation;
    en.ok = false;
    int index;
    uint32_t a;
    AnimDetailInfo info = Describe(ram, owner, index, a);
    if (info.object) {
        if (info.held) ++g_totals.held;
        else if (!info.playing || !info.posed) ++g_totals.frozen;
        else if ((info.mask & info.lod0Mask) != info.lod0Mask) ++g_totals.masked;
        else if (!info.interp) ++g_totals.stepped;
        else ++g_totals.full;
    }
    if (info.object && info.playing) {
        const size_t ob = static_cast<size_t>(index) * rr::sim::kAnimObjectBytes;
        const size_t pb = static_cast<size_t>(index) * rr::sim::kAnimProgramBytes;
        uint64_t before = 0;
        if (CheckOn()) before = Hash(ram, kRam);
        en.ok = RunPose(ram, a, owner, info.lod0Mask, g_snap.obj.data() + ob, g_snap.prog.data() + pb, en.pose);
        if (CheckOn()) {
            ++g_totals.ramChecks;
            if (Hash(ram, kRam) != before) ++g_totals.ramChanged;
            // where the game itself posed this object at LOD 0 this frame, the two poses must be equal
            if (en.ok && info.Full()) {
                RiderPoseView live;
                if (ReadRiderPose(ram, owner, live)) {
                    ++g_totals.checked;
                    bool same = std::memcmp(live.root, en.pose.root, sizeof(live.root)) == 0;
                    for (uint32_t k = 0; k < kParts && same; ++k)
                        same = std::memcmp(live.local[k].m, en.pose.local[k].m, sizeof(live.local[k].m)) == 0;
                    ++(same ? g_totals.checkEqual : g_totals.checkDiffer);
                }
            }
        }
        ++(en.ok ? g_totals.poses : g_totals.refused);
    }
    info.ours = en.ok;
    en.info = info;
    if (infoOut) *infoOut = info;
    if (en.ok) out = en.pose;
    g_totals.poseUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return en.ok;
}

std::string AnimDetailLine() {
    const Totals& t = g_totals;
    char b[900];
    std::snprintf(b, sizeof(b),
                  "ANIM DETAIL (anim_detail.h, maximum detail's full animation, OURS render-only): %zu frame copies "
                  "(%.1f us each), %zu full poses computed (%.1f us per frame); objects the game animated this frame: "
                  "full %zu, part-masked by LodChoice %zu, stepped (no interpolation) %zu, stopped or not posed %zu, "
                  "held pedestrian %zu; refused %zu; check: arena unchanged %zu of %zu, "
                  "pose equal to the game's where it posed at LOD 0 %zu of %zu (differ %zu)\n",
                  t.snapshots, t.snapshots ? t.snapUs / static_cast<double>(t.snapshots) : 0.0, t.poses,
                  t.snapshots ? t.poseUs / static_cast<double>(t.snapshots) : 0.0, t.full, t.masked, t.stepped,
                  t.frozen, t.held, t.refused, t.ramChecks - t.ramChanged, t.ramChecks, t.checkEqual,
                  t.checked, t.checkDiffer);
    return b;
}

} // namespace rr::game
