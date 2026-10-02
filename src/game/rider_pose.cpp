#include "game/rider_pose.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "game/sim/anim.h"
#include "game/sim/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

#include <array>
#include <cmath>

namespace rr::game {
namespace {

constexpr uint32_t kGp = 0x8005AC8C;
constexpr uint32_t kBikeParts0 = 0x801BDF1C;   // rr-race: player bike +4
constexpr uint32_t kRiderParts0 = 0x801BDF9C;  // rr-race: player rider +4
constexpr uint32_t kPairParts = 0x801E7F3C;    // rr-race: bike 1 +4; rider 1 +4 = this + 0x80
constexpr uint32_t kPairStride = 0x220;
constexpr uint32_t kModelRecords = 0x801EA360; // OURS: rider model record, bike model record at +0x20
constexpr uint32_t kRiderSlots = 17, kBikeSlots = 5;
constexpr uint32_t kOwnerParts = 4, kOwnerModel = 0, kOwnerFlags9 = 9, kOwnerAnim = 0x21C;

// Where pairs 1.. and the model records sit - rr-race's addresses, or (a two-seat race, whose short animation
// set runs past kPairParts when its passenger bank finds no room in the object area) right after the banks, OURS,
// inside the bank area [.., kAnimFilesTo) (race_session.cpp kArenaAnimFilesTo).
constexpr uint32_t kAnimFilesTo = 0x801EE000;
uint32_t g_pairParts = kPairParts, g_modelRecords = kModelRecords;
uint32_t BikeParts(size_t i) { return i == 0 ? kBikeParts0 : g_pairParts + kPairStride * static_cast<uint32_t>(i - 1); }
uint32_t RiderParts(size_t i) {
    return i == 0 ? kRiderParts0 : g_pairParts + 0x80u + kPairStride * static_cast<uint32_t>(i - 1);
}

void Identity(rr::sim::GuestRam& g, uint32_t parts, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t s = parts + 24u * k;
        g.W32(s, 0);
        for (uint32_t e = 0; e < 9; ++e) g.W16(s + 4u + 2u * e, (e % 4u == 0u) ? 4096u : 0u);
        g.W16(s + 22u, 0);
    }
}

// The DOD3 group-0 fields the pose reads, at `at`: +0x0C flags (+0x0E the type / class / shift
// halfword), +0x10 radius, +0x14 scale, +0x18 the sub-mesh count, +0x1C the texture slot.
void ModelRecord(rr::sim::GuestRam& g, uint32_t at, const rr::ModelGroup& group) {
    for (uint32_t k = 0; k < 32; k += 4) g.W32(at + k, 0);
    g.W32(at + 0x0C, group.flags);
    g.W32(at + 0x10, group.radius);
    g.W32(at + 0x14, group.scale);
    g.W16(at + 0x18, static_cast<uint16_t>(group.subMeshes.size()));
    g.W32(at + 0x1C, group.slot);
}

} // namespace

std::string BuildPoseArena(rr::sim::GuestRam& g, const rr::DiscImage& disc, uint32_t bankEnd,
                           const std::vector<PoseBinding>& bikes, uint32_t pairsAt, bool originalBind) {
    g_pairParts = kPairParts;
    g_modelRecords = kModelRecords;
    const uint32_t pairs = bikes.size() > 1 ? static_cast<uint32_t>(bikes.size() - 1) : 0u;
    bool moved = false;
    if (pairsAt != 0u && pairs <= 18u) { // the session's reserve in the object area
        g_pairParts = pairsAt;
        g_modelRecords = pairsAt + 18u * kPairStride;
        moved = true;
    } else if (bankEnd + 8u > kPairParts || kPairParts + kPairStride * pairs > kModelRecords) {
        // the banks (or a 19th rider) run into rr-race's part arrays - they go after the banks, OURS
        const uint32_t at = (bankEnd + 8u + 15u) & ~15u;
        const uint32_t records = at + kPairStride * pairs;
        if (records + 0x40u > kAnimFilesTo) {
            char e[200];
            std::snprintf(e, sizeof(e), "the rider pose arena was NOT built: the animation banks (to 0x%08X) run into "
                          "rr-race's part arrays and leave no room for %u pairs before the bank area's end 0x%08X",
                          bankEnd, pairs, kAnimFilesTo);
            return e;
        }
        g_pairParts = at;
        g_modelRecords = records;
        moved = true;
    }
    const auto geo = disc.Find("DATA/BBLEVEL1.GEO");
    if (!geo) return "the rider pose arena was NOT built: DATA\\BBLEVEL1.GEO is not on the disc";
    const std::vector<rr::Model> models = rr::ParseGeo(disc.ReadFile(*geo));
    const rr::ModelGroup* rider = nullptr;
    const rr::ModelGroup* bike = nullptr;
    for (const rr::Model& m : models) {
        if (m.id == 150 && !m.groups.empty()) rider = &m.groups.front();
        if (m.id == 100 && !m.groups.empty()) bike = &m.groups.front();
    }
    if (rider == nullptr || bike == nullptr) return "the rider pose arena was NOT built: models 150 / 100 missing";
    ModelRecord(g, g_modelRecords, *rider);
    ModelRecord(g, g_modelRecords + 0x20u, *bike);
    size_t keptBound = 0;
    for (size_t i = 0; i < bikes.size(); ++i) {
        const auto bound = [&](uint32_t obj) {
            return originalBind && g.U32(obj + kOwnerModel) != 0u && g.U32(obj + kOwnerParts) != 0u && g.U32(obj + 0x60u) != 0u;
        };
        if (bound(bikes[i].bike)) {
            ++keptBound;
        } else {
            Identity(g, BikeParts(i), kBikeSlots);
            g.W32(bikes[i].bike + kOwnerModel, g_modelRecords + 0x20u);
            g.W32(bikes[i].bike + kOwnerParts, BikeParts(i));
        }
        if (bound(bikes[i].rider)) {
            ++keptBound;
        } else {
            Identity(g, RiderParts(i), kRiderSlots);
            g.W32(bikes[i].rider + kOwnerModel, g_modelRecords);
            g.W32(bikes[i].rider + kOwnerParts, RiderParts(i));
        }
        g.W8(bikes[i].rider + kOwnerFlags9, static_cast<uint8_t>(g.U8(bikes[i].rider + kOwnerFlags9) | 3u));
    }
    if (originalBind) {
        char k[420];
        std::snprintf(k, sizeof(k),
                      "the rider pose runs PORTED: %zu riders; %zu bound object(s) keep the ORIGINAL's model "
                      "and part array (ModelBind SLUS 0x8002FAD4 PORTED: the registry's DOD3, the part array on heap 0); "
                      "%zu unbound (OURS: the model records at 0x%08X, the slots at 0x%08X + 0x220 i)",
                      bikes.size(), keptBound, 2u * bikes.size() - keptBound, g_modelRecords, g_pairParts);
        return k;
    }
    char b[480];
    if (moved)
        std::snprintf(b, sizeof(b),
                      "the rider pose runs PORTED: %zu riders; the animation banks end at 0x%08X; "
                      "pairs 1.. sit OURS at 0x%08X + 0x220 i (the player's at "
                      "0x801BDF1C / 0x801BDF9C), the model records at 0x%08X (look3; DOD3 fields of BBLEVEL1.GEO models "
                      "150 / 100: type %u, class %u)",
                      bikes.size(), bankEnd, g_pairParts, g_modelRecords, ((rider->flags >> 16) & 0x78u) >> 3,
                      ((bike->flags >> 16) & 0xF80u) >> 7);
    else
        std::snprintf(b, sizeof(b),
                      "the rider pose runs PORTED: %zu riders with part slots at rr-race's addresses "
                      "(0x801BDF9C, 0x801E7FBC + 0x220 i), their bikes' at 0x801BDF1C / 0x801E7F3C + 0x220 i; OURS: the "
                      "model records at 0x%08X (DOD3 fields of BBLEVEL1.GEO models 150 / 100: type %u, class %u)",
                      bikes.size(), g_modelRecords, ((rider->flags >> 16) & 0x78u) >> 3, ((bike->flags >> 16) & 0xF80u) >> 7);
    return b;
}

void PoseLodPass(rr::sim::GuestRam& g, const std::vector<PoseBinding>& bikes) {
    for (const PoseBinding& p : bikes) {
        const uint32_t model = g.U32(p.rider + kOwnerModel);
        if (model == 0u) continue;
        const uint32_t type = (g.U16(model + 14u) & 0x78u) >> 3;       // 0x800667C4: kind 1 or 4
        if (type == 1u || type == 4u) {
            const uint32_t a = g.U32(p.rider + kOwnerAnim);
            if (a != 0u) {
                const uint32_t lod = 0;                                  // OURS: the product draws LOD 0
                g.W32(a + rr::sim::animf::kMask, g.U32(rr::sim::kPosePartMasks + 4u * (type == 4u ? lod + 1u : lod)));
                g.W32(a + rr::sim::animf::kFlags, (g.U32(a + rr::sim::animf::kFlags) & ~4u) | 4u); // LOD 0: interpolate
            }
        }
        g.W8(p.rider + kOwnerFlags9, static_cast<uint8_t>(g.U8(p.rider + kOwnerFlags9) & 0xF7u));
    }
}

bool ReadRiderPose(const uint8_t* ram, uint32_t rider, RiderPoseView& out) {
    const auto u32 = [&](uint32_t a) {
        const uint32_t o = a & 0x1FFFFFu;
        return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
               (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
    };
    const auto s16 = [&](uint32_t a) {
        const uint32_t o = a & 0x1FFFFFu;
        return static_cast<int16_t>(static_cast<uint16_t>(ram[o] | (ram[o + 1] << 8)));
    };
    const uint32_t parts = u32(rider + kOwnerParts);
    if (parts < 0x80000000u || (parts & 0x1FFFFFu) + 24u * kRiderSlots > 0x200000u) return false;
    for (uint32_t k = 0; k < kRiderSlots; ++k)
        for (uint32_t e = 0; e < 9; ++e) out.local[k].m[e] = s16(parts + 24u * k + 4u + 2u * e);
    for (uint32_t k = 0; k < 3; ++k) out.root[k] = s16(rider + 0x1Cu + 2u * k);
    out.stance = static_cast<uint16_t>(s16(rider + 0x220u));
    return true;
}

bool RiderPlaceSeatOnly() {
    static const bool seat = std::getenv("RRJB_RIDER_PLACE") != nullptr && std::strcmp(std::getenv("RRJB_RIDER_PLACE"), "seat") == 0;
    return seat;
}

bool RiderOwnFrame(const uint8_t* ram, uint32_t rider, float axis[3][3], float origin[3]) {
    if (ram == nullptr || rider < 0x80000000u || rider >= 0x80200000u || RiderPlaceSeatOnly()) return false;
    const auto at = [ram](uint32_t a) { return ram + (a & 0x1FFFFFu); };
    const int8_t kind48 = static_cast<int8_t>(*at(rider + 0x48u));
    if (kind48 == 1) return false; // a seated child (ModelVisible 0x80067AC4's first arm)
    int16_t rows[9], root[3];
    std::memcpy(rows, at(rider + 0x1B0u), sizeof(rows));
    std::memcpy(root, at(rider + 0x1Cu), sizeof(root));
    int32_t pos[3];
    std::memcpy(pos, at(rider + 0xB8u), sizeof(pos));
    const bool turned = kind48 == 3; // +0x68 = the rows transposed; otherwise part 0 in world axes
    float len2 = 0.0f;
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) {
            axis[c][k] = turned ? static_cast<float>(rows[3 * c + k]) / 4096.0f : (c == k ? 1.0f : 0.0f);
            len2 += axis[c][k] * axis[c][k];
        }
    if (len2 < 0.75f) return false; // no frame written yet
    const bool addRoot = turned && (*at(rider + 9u) & 4u) != 0u; // ModelVisible's +0x09 bit 2 arm
    for (int k = 0; k < 3; ++k) {
        origin[k] = static_cast<float>(static_cast<double>(pos[k]) / 65536.0);
        if (addRoot)
            for (int c = 0; c < 3; ++c) origin[k] += axis[c][k] * static_cast<float>(root[c]) / 1024.0f;
    }
    return true;
}

bool MachineClimbSlots(const uint8_t* ram, uint32_t bike, rr::PartMatrix& slot0, rr::PartMatrix& slot2) {
    if (ram == nullptr || bike < 0x80000000u || bike >= 0x80200000u || RiderPlaceSeatOnly()) return false;
    if (static_cast<int8_t>(ram[(bike + 0x48u) & 0x1FFFFFu]) != 3) return false;
    uint32_t parts = 0;
    std::memcpy(&parts, ram + ((bike + kOwnerParts) & 0x1FFFFFu), 4);
    if (parts < 0x80000000u || (parts & 0x1FFFFFu) + 24u * kBikeSlots > 0x200000u) return false;
    std::memcpy(slot0.m, ram + ((parts + 4u) & 0x1FFFFFu), sizeof(slot0.m));
    std::memcpy(slot2.m, ram + ((parts + 48u + 4u) & 0x1FFFFFu), sizeof(slot2.m));
    return true;
}

// ------------------------------------------------------------------------------ the capture check
int CheckRiderPose(const std::string& ramPath, bool mutate) {
    std::vector<uint8_t> ram;
    {
        std::ifstream f(ramPath, std::ios::binary);
        ram.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (ram.size() < 0x200000u) {
        std::printf("posecheck: %s is not a 2 MiB RAM image\n", ramPath.c_str());
        return 2;
    }
    ram.resize(0x200000u);
    std::vector<uint8_t> work = ram;
    rr::sim::GuestRam g(work.data(), kGp);
    const uint32_t desc = rr::sim::kAnimDescriptor;
    const uint32_t objs = g.U32(desc);
    const int32_t count = g.S32(desc + 12u);
    if (objs < 0x80000000u || count <= 0 || count > 32) {
        std::printf("posecheck: %s has no animation descriptor (not a race image)\n", ramPath.c_str());
        return 2;
    }
    size_t objects = 0, matrices = 0, equal = 0, roots = 0, rootsEqual = 0, skipped = 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t a = objs + rr::sim::kAnimObjectBytes * static_cast<uint32_t>(i);
        const uint32_t owner = g.U32(a);
        if (owner < 0x80000000u) continue;
        const uint32_t f = g.U32(a + rr::sim::animf::kFlags);
        const uint32_t op = g.U32(a + 4u) + 12u * g.U32(a + 12u);
        // Reproducible from the captured channels alone: playing, not blending, not interpolating,
        // not the frame-dependent stance-42 tilt.
        if (!(f & 2u) || (f & 4u) || g.U8(op + 1u) == 3u || g.U16(owner + 544u) == 42u) { ++skipped; continue; }
        ++objects;
        const uint32_t parts = g.U32(owner + 4u);
        const uint32_t clip = g.U32(a + rr::sim::animf::kClip);
        const uint32_t n = g.U8(clip + 15u);
        if (mutate) g.W8(op + 2u, static_cast<uint8_t>(g.U8(op + 2u) ^ 1u)); // the negative control: mirrored
        rr::sim::AnimPose pose(g);
        pose.Pose(a);
        if (pose.Failed()) {
            std::printf("posecheck: object %d: the port refused\n", i);
            return 1;
        }
        const uint32_t mask = (g.U8(op + 2u) & 0x40u) ? 0xFFFFFFFFu : g.U32(a + rr::sim::animf::kMask);
        const bool mirrored = (g.U8(op + 2u) & 1u) != 0;
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t slot = mirrored ? g.U8(rr::sim::kPoseMirrorSlots + k) : k;
            if (!((mask >> (slot & 31u)) & 1u)) continue;
            const uint32_t at = (parts + 24u * slot + 4u) & 0x1FFFFFu;
            ++matrices;
            if (std::memcmp(work.data() + at, ram.data() + at, 18) == 0) ++equal;
        }
        ++roots;
        const uint32_t ro = (owner + 0x1Cu) & 0x1FFFFFu;
        if (std::memcmp(work.data() + ro, ram.data() + ro, 6) == 0) ++rootsEqual;
    }
    std::printf("posecheck %s%s: %zu objects posed (%zu skipped: not playing / blending / interpolating / "
                "stance 42)\n  part matrices (slot 0 included) %zu of %zu equal, roots %zu of %zu equal\n",
                ramPath.c_str(), mutate ? " [MUTATED: mirror flipped]" : "", objects, skipped, equal, matrices,
                rootsEqual, roots);
    return (equal == matrices && rootsEqual == roots && matrices != 0) ? 0 : 1;
}

// ------------------------------------------------------------------------------ the packet check
std::string CheckRiderPackets(const RiderPacketCheck& in, bool& pass) {
    pass = false;
    std::string rep = "rrview --ridercheck: the player's rider against the ORIGINAL's packets\n";
    struct Packet {
        int n = 0;
        double x[4] = {}, y[4] = {};
        int u[4] = {}, v[4] = {};
    };
    std::vector<Packet> packets;
    if (FILE* f = std::fopen(in.primsCsv.c_str(), "rb")) {
        char line[2048];
        std::vector<std::string> header;
        bool first = true;
        while (std::fgets(line, sizeof(line), f)) {
            std::vector<std::string> cols;
            std::string cur;
            for (const char* c = line; *c && *c != '\n' && *c != '\r'; ++c) {
                if (*c == ',') { cols.push_back(cur); cur.clear(); } else cur += *c;
            }
            cols.push_back(cur);
            if (first) { header = cols; first = false; continue; }
            const auto col = [&](const std::string& name) -> std::string {
                for (size_t k = 0; k < header.size() && k < cols.size(); ++k)
                    if (header[k] == name) return cols[k];
                return std::string();
            };
            const std::string clut = col("clut");
            if (clut.empty() || static_cast<uint16_t>(std::strtoul(clut.c_str(), nullptr, 0)) != in.clut) continue;
            Packet p;
            p.n = std::atoi(col("nverts").c_str());
            if (p.n < 3 || p.n > 4) continue;
            for (int k = 0; k < p.n; ++k) {
                const std::string s = std::to_string(k);
                p.x[k] = std::atof(col("x" + s).c_str());
                p.y[k] = std::atof(col("y" + s).c_str());
                p.u[k] = std::atoi(col("u" + s).c_str());
                p.v[k] = std::atoi(col("v" + s).c_str());
            }
            packets.push_back(p);
        }
        std::fclose(f);
    }
    if (packets.empty() || in.disc == nullptr || in.ram == nullptr || in.ram->size() < 0x200000u || in.project == nullptr)
        return rep + "verdict FAIL (no packets, no disc or no state)\n";
    const auto geo = in.disc->Find("DATA/BBLEVEL1.GEO");
    const auto ovl = in.disc->Find("RASHCDG.BIN");
    if (!geo || !ovl) return rep + "verdict FAIL (BBLEVEL1.GEO / RASHCDG.BIN missing)\n";
    const std::vector<rr::Model> models = rr::ParseGeo(in.disc->ReadFile(*geo));
    const rr::ModelGroup* group = nullptr;
    for (const rr::Model& m : models)
        if (m.id == 150 && !m.groups.empty()) group = &m.groups.front();
    if (group == nullptr || group->subMeshes.size() != 17) return rep + "verdict FAIL (model 150 missing)\n";
    int du = 0, dv = 0; // the texture's place in its page: packet texel = file texel + (du, dv)
    {
        int umin = 999, umax = -1, vmin = 999, vmax = -1, pumin = 999, pumax = -1, pvmin = 999, pvmax = -1;
        for (const rr::SubMesh& sub : group->subMeshes)
            for (const rr::Primitive& prim : sub.prims)
                for (int c = 0; c < 4; ++c) {
                    umin = std::min(umin, int(prim.u[c])); umax = std::max(umax, int(prim.u[c]));
                    vmin = std::min(vmin, int(prim.v[c])); vmax = std::max(vmax, int(prim.v[c]));
                }
        for (const Packet& p : packets)
            for (int c = 0; c < p.n; ++c) {
                pumin = std::min(pumin, p.u[c]); pumax = std::max(pumax, p.u[c]);
                pvmin = std::min(pvmin, p.v[c]); pvmax = std::max(pvmax, p.v[c]);
            }
        du = pumin - umin;
        dv = pvmin - vmin;
        (void)umax; (void)vmax; (void)pumax; (void)pvmax;
    }
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(in.disc->ReadFile(*ovl));
    const rr::Assembly assembly = rr::AssembleGroup(*group, skeleton);
    const int factor = rr::LodFactor(*group);

    // The capture's slots and root, and the ported ones (Pose on a copy).
    std::vector<uint8_t> work = *in.ram;
    rr::sim::GuestRam g(work.data(), kGp);
    constexpr uint32_t kRider = 0x801BB2ECu;
    const uint32_t a = g.U32(kRider + kOwnerAnim);
    const uint32_t parts = g.U32(kRider + kOwnerParts);
    const uint32_t bparts = g.U32(g.U32(kRider + 52u) + kOwnerParts);
    const auto slotAt = [&](const std::vector<uint8_t>& img, uint32_t addr) {
        rr::PartMatrix m;
        for (uint32_t e = 0; e < 9; ++e) {
            const uint32_t o = (addr + 2u * e) & 0x1FFFFFu;
            m.m[e] = static_cast<int16_t>(static_cast<uint16_t>(img[o] | (img[o + 1] << 8)));
        }
        return m;
    };
    const auto rootAt = [&](const std::vector<uint8_t>& img, float out[3]) {
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t o = (kRider + 0x1Cu + 2u * k) & 0x1FFFFFu;
            out[k] = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(img[o] | (img[o + 1] << 8))));
        }
    };
    {
        rr::sim::AnimPose pose(g);
        pose.Pose(a);
        if (pose.Failed()) return rep + "verdict FAIL (the ported Pose refused)\n";
    }
    const rr::PartMatrix b0 = slotAt(*in.ram, bparts + 4u);
    const rr::PartMatrix r0 = slotAt(*in.ram, parts + 4u);
    const rr::PartMatrix relCaptured = rr::Multiply3x3(rr::Transpose3x3(b0), r0);

    struct Mode {
        const char* name;
        std::vector<rr::PartMatrix> local;
        rr::PartMatrix rel;
        float root[3];
    };
    std::vector<Mode> modes(3);
    modes[0].name = "ported pose (Pose 0x8005D63C run on the capture)";
    modes[1].name = "captured slots (the emulator's own matrices)";
    modes[2].name = "rest pose (every part the identity; negative control)";
    for (Mode& md : modes) md.local.assign(17, rr::PartMatrix{});
    // Slot 0 is the root part's rotation RELATIVE TO THE BIKE (`--posecheck`: the captured slot 0 is
    // exactly the ported one in all 18 images), so the rider object takes the bike's axes.
    for (uint32_t k = 0; k < 17; ++k) {
        modes[0].local[k] = slotAt(work, parts + 24u * k + 4u);
        modes[1].local[k] = slotAt(*in.ram, parts + 24u * k + 4u);
    }
    modes[2].local[0] = r0; // the negative control keeps the root part's rotation, drops the limbs'
    (void)relCaptured;
    modes[0].rel = rr::PartMatrix{};
    modes[1].rel = rr::PartMatrix{};
    modes[2].rel = rr::PartMatrix{};
    rootAt(work, modes[0].root);
    rootAt(*in.ram, modes[1].root);
    rootAt(*in.ram, modes[2].root);

    std::vector<size_t> owner(group->verts.size(), 0);
    for (size_t p = 0; p < group->subMeshes.size(); ++p)
        for (uint32_t i = 0; i < group->subMeshes[p].vertCount; ++i)
            if (group->subMeshes[p].vertBase + i < owner.size()) owner[group->subMeshes[p].vertBase + i] = p;

    char b[400];
    std::snprintf(b, sizeof(b), "  %zu original rider packets (CLUT 0x%04X), the page's texel offset (%d, %d)\n",
                  packets.size(), in.clut, du, dv);
    rep += b;
    // Two placements of the bike: rrview's (its ground rows +0x204..) and the capture's own bike slot 0
    // (the matrix the console drew the bike with), same origin and scale.
    float bikeSlot0[16];
    {
        const float scale = std::sqrt(in.bike[0] * in.bike[0] + in.bike[1] * in.bike[1] + in.bike[2] * in.bike[2]);
        for (int k = 0; k < 16; ++k) bikeSlot0[k] = in.bike[k];
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) bikeSlot0[4 * c + r] = static_cast<float>(b0.m[r * 3 + c]) / 4096.0f * scale;
    }
    double means[6] = {};
    size_t matchedN[6] = {};
    for (size_t mi = 0; mi < 2 * modes.size(); ++mi) {
        const Mode& md = modes[mi % modes.size()];
        const float* bike = mi < modes.size() ? in.bike : bikeSlot0;
        if (mi == 0) rep += " on rrview's bike matrix (the ground rows +0x204 / +0x20A / +0x210):\n";
        if (mi == modes.size()) rep += " on the capture's bike slot 0 (the console's own bike matrix):\n";
        const rr::PosedGroup posed = rr::PoseGroup(*group, skeleton, assembly, md.local);
        struct Corner { double x, y; int u, v; };
        std::vector<std::array<Corner, 4>> prims;
        std::vector<size_t> primPart;
        size_t curPart = 0;
        for (const rr::SubMesh& sub : group->subMeshes) {
            ++curPart;
            for (const rr::Primitive& prim : sub.prims) {
                std::array<Corner, 4> cs{};
                bool ok = true;
                for (int c = 0; c < 4; ++c) {
                    const uint16_t idx = prim.index[c];
                    if (idx >= group->verts.size()) { ok = false; break; }
                    const rr::SVector& vx = group->verts[idx];
                    const size_t part = owner[idx];
                    const int32_t lv[3] = {vx.x * factor, vx.y * factor, vx.z * factor};
                    float pp[3];
                    for (int r = 0; r < 3; ++r) {
                        int64_t s = 0;
                        for (int k = 0; k < 3; ++k) s += static_cast<int64_t>(posed.world[part].m[r * 3 + k]) * lv[k];
                        pp[r] = static_cast<float>((s >> 12) + posed.origin[part][static_cast<size_t>(r)]);
                    }
                    float bm[3];
                    for (int r = 0; r < 3; ++r) {
                        float s = 0;
                        for (int k = 0; k < 3; ++k) s += md.rel.m[r * 3 + k] / 4096.0f * pp[k];
                        bm[r] = in.attach[r] + md.root[r] + s;
                    }
                    float w[3];
                    for (int r = 0; r < 3; ++r)
                        w[r] = bike[12 + r] + bike[0 + r] * bm[0] + bike[4 + r] * bm[1] + bike[8 + r] * bm[2];
                    double sx = 0, sy = 0;
                    if (!in.project(in.ctx, w, sx, sy)) { ok = false; break; }
                    cs[static_cast<size_t>(c)] = Corner{sx, sy, prim.u[c] + du, prim.v[c] + dv};
                }
                if (ok) { prims.push_back(cs); primPart.push_back(curPart - 1); }
            }
            }
        // Each packet's primitive of ours: the one with the same texel corners and the nearest screen
        // corners. Then the error raw, and again with the one mean screen offset of all matched
        // corners taken out (what is left is the SHAPE: the pose, not the placement).
        struct Pair { double ox, oy, px, py; };
        std::vector<std::vector<Pair>> pairs;
        std::vector<size_t> pairPart;
        for (const Packet& pk : packets) {
            double best = 1e30;
            std::vector<Pair> bestPairs;
            size_t bestPrim = 0;
            for (size_t pi = 0; pi < prims.size(); ++pi) {
                const auto& cs = prims[pi];
                double e = 0;
                bool all = true;
                std::vector<Pair> pp;
                for (int k = 0; k < pk.n && all; ++k) {
                    double d = 1e30;
                    Pair pr{};
                    for (const Corner& c : cs)
                        if (c.u == pk.u[k] && c.v == pk.v[k]) {
                            const double dd = std::hypot(c.x - pk.x[k], c.y - pk.y[k]);
                            if (dd < d) { d = dd; pr = Pair{c.x, c.y, pk.x[k], pk.y[k]}; }
                        }
                    if (d >= 1e29) all = false;
                    else { e = std::max(e, d); pp.push_back(pr); }
                }
                if (all && e < best) { best = e; bestPairs = pp; bestPrim = pi; }
            }
            if (!bestPairs.empty()) { pairs.push_back(bestPairs); pairPart.push_back(primPart[bestPrim]); }
        }
        double mx = 0, my = 0;
        size_t np = 0;
        for (const auto& v : pairs)
            for (const Pair& q : v) { mx += q.ox - q.px; my += q.oy - q.py; ++np; }
        if (np) { mx /= static_cast<double>(np); my /= static_cast<double>(np); }
        const auto stats = [&](double ox, double oy, size_t& w2, size_t& w4, double& mean, double& worst) {
            w2 = w4 = 0;
            mean = worst = 0;
            for (const auto& v : pairs) {
                double e = 0;
                for (const Pair& q : v) e = std::max(e, std::hypot(q.ox - ox - q.px, q.oy - oy - q.py));
                mean += e;
                worst = std::max(worst, e);
                if (e <= 2.0) ++w2;
                if (e <= 4.0) ++w4;
            }
            if (!pairs.empty()) mean /= static_cast<double>(pairs.size());
        };
        size_t w2 = 0, w4 = 0, c2 = 0, c4 = 0;
        double mean = 0, worst = 0, cmean = 0, cworst = 0;
        stats(0, 0, w2, w4, mean, worst);
        stats(mx, my, c2, c4, cmean, cworst);
        if (std::getenv("RR_RIDERCHECK_PARTS")) {
            double ocx = 0, ocy = 0, pcx = 0, pcy = 0; size_t n = 0;
            for (const auto& v : pairs) for (const Pair& q : v) { ocx += q.ox; ocy += q.oy; pcx += q.px; pcy += q.py; ++n; }
            const double dn = static_cast<double>(n);
            ocx /= dn; ocy /= dn; pcx /= dn; pcy /= dn;
            double so = 0, sp = 0, sxo = 0, sxp = 0, syo = 0, syp = 0;
            for (const auto& v : pairs) for (const Pair& q : v) {
                so += std::hypot(q.ox - ocx, q.oy - ocy); sp += std::hypot(q.px - pcx, q.py - pcy);
                sxo += std::fabs(q.ox - ocx); sxp += std::fabs(q.px - pcx); syo += std::fabs(q.oy - ocy); syp += std::fabs(q.py - pcy);
            }
            std::printf("  mode %zu spread ours/theirs %.3f (x %.3f, y %.3f)\n", mi, so / sp, sxo / sxp, syo / syp);
            for (size_t part = 0; part < 17; ++part) {
                double sx = 0, sy = 0; size_t np2 = 0;
                for (size_t q = 0; q < pairs.size(); ++q)
                    if (pairPart[q] == part)
                        for (const Pair& pr : pairs[q]) { sx += pr.ox - pr.px; sy += pr.oy - pr.py; ++np2; }
                if (np2) std::printf("  mode %zu part %2zu: %3zu corners, mean offset (%6.2f, %6.2f)\n", mi, part, np2,
                                     sx / static_cast<double>(np2), sy / static_cast<double>(np2));
            }
        }
        means[mi] = pairs.empty() ? 1e30 : cmean;
        matchedN[mi] = pairs.size();
        std::snprintf(b, sizeof(b),
                      "  %s\n    matched %zu of %zu packets; raw: within 2 px %zu, 4 px %zu, mean %.2f, worst %.2f px;\n"
                      "    mean offset (%.2f, %.2f) px taken out: within 2 px %zu, 4 px %zu, mean %.2f, worst %.2f px\n",
                      md.name, pairs.size(), packets.size(), w2, w4, mean, worst, mx, my, c2, c4, cmean, cworst);
        rep += b;
    }
    // PASS: the ported pose places the rider exactly as the capture's own matrices do, and both are
    // clearly closer to the original's packets than the rest pose.
    pass = matchedN[3] * 10 >= packets.size() * 9 && std::fabs(means[3] - means[4]) < 0.05 && means[3] * 2.0 < means[5];
    rep += std::string("verdict ") + (pass ? "PASS" : "FAIL") + "\n";
    return rep;
}

} // namespace rr::game
