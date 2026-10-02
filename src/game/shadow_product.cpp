// The model shadow in the product (shadow_product.h).
#include "game/shadow_product.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "game/loader2_cam_product.h" // loader2_cam: the level's light stores PORTED
#include "game/mp2_product.h" // the two-player shadow 0x80026960
#include "game/sim/shadow.h"
#include "game/sim/vec.h"
#include "rrformats/level_bundle.h"

namespace rr::game {

bool GteObjectsOn() {
    static const bool off = std::getenv("RRJB_GTE_OBJECTS") != nullptr && std::strcmp(std::getenv("RRJB_GTE_OBJECTS"), "off") == 0;
    return !off;
}

bool ShadowPortOn() {
    static const bool on = !(std::getenv("RRJB_SHADOW") != nullptr && std::strcmp(std::getenv("RRJB_SHADOW"), "ours") == 0);
    return on;
}

namespace {

namespace S = rr::sim::shadow;

struct Collector final : S::Sink {
    rr::sim::GuestRam& g;
    ShadowFrame& out;
    uint32_t view, obj;
    double inv[9] = {}, eye[3] = {};
    bool ok = false;
    Collector(rr::sim::GuestRam& gg, ShadowFrame& o, uint32_t v, uint32_t ob) : g(gg), out(o), view(v), obj(ob) {
        const uint32_t cam = g.U32(S::kRenderCams + 4u * view);
        double r[9];
        for (uint32_t k = 0; k < 9; ++k) r[k] = static_cast<double>(g.S16(cam + 0x5Cu + 2u * k)) / 4096.0;
        for (uint32_t k = 0; k < 3; ++k) eye[k] = static_cast<double>(g.S32(cam + 0x1Cu + 4u * k));
        const double det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) +
                           r[2] * (r[3] * r[7] - r[4] * r[6]);
        if (det > 1e-9 || det < -1e-9) {
            inv[0] = (r[4] * r[8] - r[5] * r[7]) / det;
            inv[1] = (r[2] * r[7] - r[1] * r[8]) / det;
            inv[2] = (r[1] * r[5] - r[2] * r[4]) / det;
            inv[3] = (r[5] * r[6] - r[3] * r[8]) / det;
            inv[4] = (r[0] * r[8] - r[2] * r[6]) / det;
            inv[5] = (r[2] * r[3] - r[0] * r[5]) / det;
            inv[6] = (r[3] * r[7] - r[4] * r[6]) / det;
            inv[7] = (r[1] * r[6] - r[0] * r[7]) / det;
            inv[8] = (r[0] * r[4] - r[1] * r[3]) / det;
            ok = true;
        }
    }
    void Packet(uint32_t address, uint32_t slot, const int32_t cam[4][3]) override {
        ++out.packets;
        if (!ok) return;
        ShadowQuad q;
        q.slot = static_cast<int>(slot); // the renderer draws it at this slot (race_scene_ot.cpp)
        q.view = view;
        q.object = obj;
        const uint32_t colour = g.U32(address + 12u);
        q.colour[0] = static_cast<float>(colour & 0xFFu) / 255.0f;
        q.colour[1] = static_cast<float>((colour >> 8) & 0xFFu) / 255.0f;
        q.colour[2] = static_cast<float>((colour >> 16) & 0xFFu) / 255.0f;
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 3; ++k) {
                const double w = eye[k] + inv[3 * k + 0] * cam[c][0] + inv[3 * k + 1] * cam[c][1] + inv[3 * k + 2] * cam[c][2];
                q.world[c][k] = static_cast<float>(w / 64.0);
            }
        for (uint32_t w = 0; w < 4; ++w) {
            const uint32_t xy = g.U32(address + 16u + 4u * w);
            q.sxy[w][0] = static_cast<int16_t>(xy & 0xFFFFu);
            q.sxy[w][1] = static_cast<int16_t>(xy >> 16);
        }
        out.quads.push_back(q);
        // DEVELOPMENT CHECK (RRJB_SHADOW_DUMP=<file>): the packet's four SXY words (v0 v1 v3 v2) and colour, one
        // line a packet, for a comparison with the original's 0x2A packets of the same capture
        if (const char* dump = std::getenv("RRJB_SHADOW_DUMP")) {
            if (FILE* f = std::fopen(dump, "a")) {
                std::fprintf(f, "%08X", colour);
                for (uint32_t w = 16; w <= 28; w += 4) {
                    const uint32_t xy = g.U32(address + w);
                    std::fprintf(f, " %d %d", static_cast<int16_t>(xy & 0xFFFFu), static_cast<int16_t>(xy >> 16));
                }
                std::fprintf(f, "\n");
                std::fclose(f);
            }
        }
    }
};

} // namespace

void ShadowTail(rr::sim::GuestRam& g, uint32_t obj, uint32_t view, uint32_t w24, int32_t ofx, int32_t ofy,
                ShadowFrame& out) {
    // DEVELOPMENT CHECK (RRJB_POSE_DUMP=<file>): every emitted object's projected vertices *(0x8005ACB4) as the
    // PORTED model draw left them, for a comparison with the original's model packets
    if (const char* dump = std::getenv("RRJB_POSE_DUMP")) {
        if (FILE* f = std::fopen(dump, "a")) {
            const uint32_t dod = g.U32(obj), vtx = g.U32(dod + 0x24u), n = g.U32(vtx), sxy = g.U32(0x8005ACB4u);
            const uint32_t reg = g.U32(obj + 0x60u);
            std::fprintf(f, "obj %08X model %u kind %u lod %d verts %u:", obj, reg != 0 ? g.U32(reg) : 0u,
                         (g.U16(dod + 14u) & 0x78u) >> 3, g.S8(obj + 8u), n);
            for (uint32_t k = 0; k < n && k < 1200u; ++k)
                std::fprintf(f, " %d,%d", g.S16(sxy + 4u * k), g.S16(sxy + 4u * k + 2u));
            std::fprintf(f, "\n");
            std::fclose(f);
        }
    }
    if (!g.Faulted()) {   // a bike's or rider's vertices as the PORTED model draw left them
        const uint32_t dod = g.U32(obj);
        const uint32_t kind = (g.U16(dod + 14u) & 0x78u) >> 3, exp = g.U16(dod + 14u) >> 12;
        const uint32_t n = g.U32(g.U32(dod + 0x24u)), verts = g.U32(0x8005ACB0u), reg = g.U32(obj + 0x60u);
        // every kind the model draw emits - the cars, the props, the pedestrians too
        if ((kind == 1u || kind == 2u || GteObjectsOn()) && n > 0u && n <= 1200u && !g.Faulted()) {
            Collector cam(g, out, view, obj); // the render camera's inverse and eye
            if (cam.ok) {
                CapturedObject& c = out.poses[(static_cast<uint64_t>(view) << 32) | obj];
                c.model = reg != 0 ? g.U32(reg) : 0u;
                c.lod = g.S8(obj + 8u);
                for (int k = 0; k < 3; ++k) c.eye[k] = cam.eye[k] / 64.0;
                c.rel.resize(3u * n);
                c.sxy.resize(n);
                c.mac3.resize(n);
                c.exp = static_cast<int>(exp);
                const uint32_t screen = g.U32(0x8005ACB4u);
                for (uint32_t v = 0; v < n; ++v) {
                    c.sxy[v] = g.U32(screen + 4u * v);
                    c.mac3[v] = g.S32(verts + 16u * v + 8u);
                }
                const double unit = 1.0 / static_cast<double>(64u << exp);
                for (uint32_t v = 0; v < n; ++v) {
                    const double m[3] = {static_cast<double>(g.S32(verts + 16u * v)), static_cast<double>(g.S32(verts + 16u * v + 4u)),
                                         static_cast<double>(g.S32(verts + 16u * v + 8u))};
                    for (int k = 0; k < 3; ++k)
                        c.rel[3u * v + static_cast<uint32_t>(k)] =
                            static_cast<float>((cam.inv[3 * k] * m[0] + cam.inv[3 * k + 1] * m[1] + cam.inv[3 * k + 2] * m[2]) * unit);
                }
            }
        }
        g.ClearFault(); // a read of ours that faulted (none was pending on entry)
    }
    if (!ShadowPortOn() || ((w24 >> 7) & 1u) == 0) return;
    S::Gte gte;
    gte.ofx = ofx * 65536;
    gte.ofy = ofy * 65536;
    Collector sink(g, out, view, obj);
    bool twoPlayer = false;
    ++out.calls;
    const size_t before = out.quads.size();
    // RRJB_SHADOW_MUTATE=1 (the negative control of --shadowcheck): the ground normal read from the light vector
    // 0x80052364 in place of +0x20A - a plane across the light, so every quad lands elsewhere.
    static const bool mutate = std::getenv("RRJB_SHADOW_MUTATE") != nullptr;
    const uint32_t base = g.U32(obj + 0x34u) != 0 ? g.U32(obj + 0x34u) : obj;
    const bool ok = mutate ? S::Shadow(g, gte, obj, view, base + 0x1F8u, S::kLight, &sink)
                           : S::EmitterShadow(g, gte, obj, view, w24, &sink, twoPlayer);
    if (!ok || g.Faulted()) {
        if (out.refused++ == 0) out.firstRefused = obj;
        g.ClearFault();
        return;
    }
    if (twoPlayer) ++out.twoPlayer;
    if (twoPlayer) Mp2ShadowTail(g, gte, obj, view, w24, out); // SLUS 0x80026960 (PORTED, mp2_product.h)
    if (out.quads.size() != before) ++out.objects;
}

std::string LevelLightStores(rr::sim::GuestRam& g, const rr::DiscImage& disc, int raceId) {
    if (std::getenv("RRJB_LEVEL_LIGHT") != nullptr && std::strcmp(std::getenv("RRJB_LEVEL_LIGHT"), "exe") == 0)
        return "the level's light stores (RASHCDI 0x8006250C / 0x80062430) SWITCHED OFF (RRJB_LEVEL_LIGHT=exe): the "
               "arena keeps the EXE's defaults";
    if (Loader2CamPorted()) // loader2_cam: LevelLight / LevelShade PORTED (camera_setup.h); RRJB_LOADER2_CAM=off: below
        return PortedLevelLight(g, disc, raceId, 0x801FF000u);
    const auto bin = disc.Find("DATA/GAMEBIN1.DAT");
    if (!bin) return "the level's light stores were NOT made: DATA/GAMEBIN1.DAT is not on the disc";
    const std::vector<uint8_t> file = disc.ReadFile(*bin);
    rr::LevelBundle bundle;
    try {
        bundle = rr::ParseLevelBundle(file, rr::LevelBundleIndexForRace(raceId));
    } catch (const std::exception& e) {
        return std::string("the level's light stores were NOT made: ") + e.what();
    }
    const auto word = [&file](size_t at) {
        uint32_t v = 0;
        if (at + 4 <= file.size()) std::memcpy(&v, file.data() + at, 4);
        return v;
    };
    std::string done;
    if (const rr::LevelBundleSection* s2 = bundle.Find(2); s2 && s2->payload + 36 <= s2->limit) { // RASHCDI 0x8006250C
        uint32_t w[9];
        for (size_t k = 0; k < 9; ++k) w[k] = word(s2->payload + 4 * k);
        constexpr uint32_t base = 0x8005233Cu, sin = 0x8005624Cu;
        const int32_t s5 = static_cast<int32_t>(w[7] * 11u);
        const uint32_t i = static_cast<uint32_t>(s5) & 0xFFFu;
        g.W16(base + 0x28u, static_cast<uint16_t>(0u - g.U16(sin + 4u * i)));
        const int32_t s0 = static_cast<int32_t>(w[8] * 11u);
        const int32_t v1 = s0 < 0 ? s0 + 3 : s0;
        const int32_t s3 = v1 >> 2;
        g.W16(base + 0x2Cu, static_cast<uint16_t>(0u - g.U16(sin + 4u * i + 2u)));
        g.W16(base + 0x2Au, g.U16(sin + (static_cast<uint32_t>(v1) & 0x3FFCu)));
        const int16_t light[3] = {g.S16(base + 0x28u), g.S16(base + 0x2Au), g.S16(base + 0x2Cu)};
        int32_t out[3];
        rr::sim::Scale(static_cast<int32_t>(0xFF9C0000u), light, out);
        for (uint32_t k = 0; k < 3; ++k) g.W16(base + 0x10u + 2u * k, static_cast<uint16_t>(out[k] >> 10));
        g.W16(base + 0x2Au, g.U16(sin + 4u * (static_cast<uint32_t>(s0) & 0xFFFu)));
        g.W32(base + 0x4Cu, static_cast<uint32_t>(s5));
        g.W32(base + 0x50u, static_cast<uint32_t>(s3));
        g.W32(base + 0x18u, w[6]);
        for (uint32_t k = 0; k < 3; ++k) g.W32(base + 0x1Cu + 4u * k, w[k]);
        g.W32(base + 0x30u, w[3]);
        const auto fixMul = [](int32_t a, int32_t b) {
            return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(a) * b) >> 16));
        };
        for (uint32_t k = 0; k < 3; ++k)
            g.W32(base + 0x34u + 4u * k, static_cast<uint32_t>(fixMul(static_cast<int32_t>(0x10000u - w[3]), static_cast<int32_t>(w[k]))));
        g.W32(base + 0x04u, w[4]);
        g.W32(base + 0x08u, w[5]);
        g.W32(base, (((w[2] >> 16) & 0xFFu) << 16) | (((w[1] >> 16) & 0xFFu) << 8) | ((w[0] >> 16) & 0xFFu));
        done += "0x8006250C (light (" + std::to_string(g.S16(base + 0x28u)) + ", " + std::to_string(g.S16(base + 0x2Au)) +
                ", " + std::to_string(g.S16(base + 0x2Cu)) + "))";
    }
    if (const rr::LevelBundleSection* s7 = bundle.Find(7); s7 && s7->payload + 0x486 <= s7->limit) { // RASHCDI 0x80062430
        constexpr uint32_t base = 0x8005233Cu, table = 0x800D4CA8u;
        g.W32(base + 0x40u, word(s7->payload + 0x480));
        const uint16_t step = static_cast<uint16_t>((word(s7->payload + 0x484) & 0xFFFFu) - 0x100u);
        g.W16(base + 0x44u, step);
        g.W32(base + 0x48u, 0x100u - step);
        g.WriteBlock(table, file.data() + s7->payload, 1152u);
        const auto sixth = [](int32_t x) { // mult by 0x2AAAAAAB, hi, minus the sign
            const int32_t hi = static_cast<int32_t>((static_cast<int64_t>(x) * 0x2AAAAAAB) >> 32);
            return hi - (x >> 31);
        };
        const int32_t r = sixth(255 - g.U8(table)), gg = sixth(255 - g.U8(table + 1u)), b = sixth(255 - g.U8(table + 2u));
        g.W32(base + 0x0Cu, (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(gg) << 8) | static_cast<uint32_t>(r));
        char c[120];
        std::snprintf(c, sizeof(c), "%s0x80062430 (unlit step %u, shadow colour 0x%06X)", done.empty() ? "" : ", ", step,
                      g.U32(base + 0x0Cu));
        done += c;
    }
    if (g.Faulted()) {
        g.ClearFault();
        return "the level's light stores FAULTED";
    }
    return "the level's light stores, transcribed (shadow_product.h): " + (done.empty() ? std::string("none - no type-2 / type-7 section") : done);
}

std::string BuildRsqrtArena(rr::sim::GuestRam& g, const uint16_t* table, uint32_t& from, uint32_t limit) {
    const uint32_t was = g.U32(rr::sim::shadow::kRsqrtPtr);
    if (was >= 0x80000000u && was < 0x80200000u)
        return "the rsqrt table *(0x8005B560) is already in the arena (0x" + std::to_string(was) + ")";
    const uint32_t at = (from + 7u) & ~7u;
    if (table == nullptr || at + 2048u > limit) return "the rsqrt table was NOT placed in the arena: no room";
    for (uint32_t k = 0; k < 1024u; ++k) g.W16(at + 2u * k, table[k]);
    g.W32(rr::sim::shadow::kRsqrtPtr, at);
    from = at + 2048u;
    char b[260];
    std::snprintf(b, sizeof(b),
                  "the rsqrt table *(gp+2260) = *(0x8005B560) (SLUS 0x8002E080's fill, PORTED) placed in the arena OURS at "
                  "0x%08X (the pointer was 0x%08X) - Normalize 0x8002E468 through it (the shadow's bend) reads it there",
                  at, was);
    return b;
}

namespace {
uint32_t g_heapBase = 0, g_heapEnd = 0;
bool HeapControl() {
    return std::getenv("RRJB_PACKET_HEAP") != nullptr && std::strcmp(std::getenv("RRJB_PACKET_HEAP"), "hud") == 0;
}
} // namespace

std::string BuildPacketHeap(uint32_t& from, uint32_t limit, uint32_t minBytes) {
    g_heapBase = g_heapEnd = 0;
    if (HeapControl()) return "the packet heap stays the HUD placement's tail (RRJB_PACKET_HEAP=hud: the control)";
    const uint32_t at = (from + 15u) & ~15u;
    const uint32_t room = limit > at ? ((limit - at) & ~15u) : 0u;
    const uint32_t bytes = room < kPacketHeapBytes ? room : kPacketHeapBytes;
    if (bytes < minBytes || bytes == 0u) return "the packet heap was NOT moved: no room in the session's bump region";
    g_heapBase = at;
    g_heapEnd = at + bytes;
    from = g_heapEnd;
    char b[260];
    std::snprintf(b, sizeof(b),
                  "the frame's packet heap (shadow_product.h): %u bytes OURS at 0x%08X..0x%08X, the "
                  "original's order (rr-race's runs to *(0x8005B4D0) = 0x800F0460, ~57 KiB); the effects' OT is cut "
                  "from its top", bytes, g_heapBase, g_heapEnd);
    return b;
}

bool ApplyPacketHeap(rr::sim::GuestRam& g, uint32_t heapRecord, uint32_t& heapBase, uint32_t& heapEnd) {
    if (g_heapBase == 0) return false;
    heapBase = g_heapBase;
    heapEnd = g_heapEnd;
    g.W32(heapRecord + 268u, heapBase);
    g.W32(0x8005B4D0u, heapEnd);
    return !g.Faulted();
}

std::string ShadowPacketCheck(const ShadowFrame& frame, const std::string& primsCsv) {
    std::map<std::array<int, 8>, int> orig, ours;
    size_t nOrig = 0, nOurs = 0;
    if (FILE* f = std::fopen(primsCsv.c_str(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "0x2A,", 5) != 0) continue;
            std::vector<std::string> cols;
            std::string cur;
            for (const char* c = line; *c != 0 && *c != '\n'; ++c) {
                if (*c == ',') {
                    cols.push_back(cur);
                    cur.clear();
                } else {
                    cur += *c;
                }
            }
            cols.push_back(cur);
            if (cols.size() < 27) continue;
            std::array<int, 8> k{};
            const int at[8] = {4, 5, 11, 12, 18, 19, 25, 26}; // x0 y0 .. x3 y3
            for (int i = 0; i < 8; ++i) k[static_cast<size_t>(i)] = std::atoi(cols[static_cast<size_t>(at[i])].c_str());
            ++orig[k];
            ++nOrig;
        }
        std::fclose(f);
    }
    for (const ShadowQuad& q : frame.quads) {
        if (q.view != 0) continue;
        std::array<int, 8> k{};
        for (int w = 0; w < 4; ++w) {
            k[static_cast<size_t>(2 * w)] = q.sxy[w][0];
            k[static_cast<size_t>(2 * w + 1)] = q.sxy[w][1];
        }
        ++ours[k];
        ++nOurs;
    }
    size_t same = 0;
    for (const auto& [k, n] : orig)
        if (const auto it = ours.find(k); it != ours.end()) same += static_cast<size_t>(std::min(n, it->second));
    const bool pass = nOrig > 0 && same == nOrig && nOurs == nOrig;
    char b[400];
    std::snprintf(b, sizeof(b),
                  "shadowcheck: the original's 0x2A packets %zu, ours %zu, identical (four screen points) %zu; "
                  "shadowcheck verdict %s\n",
                  nOrig, nOurs, same, pass ? "PASS" : "FAIL");
    return b;
}

} // namespace rr::game
