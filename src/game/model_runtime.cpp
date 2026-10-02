// The model draw in the product (model_runtime.h).
#include "game/model_runtime.h"
#include "game/sim/shadow.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/sim/effects.h"
#include "game/sim/fx_glow.h"
#include "game/sim/ground.h"
#include "game/sim/traffic_bind.h"
#include "rrformats/level_bank.h"

namespace rr::game {
namespace {

using rr::sim::GuestRam;
namespace M = rr::sim::model;

constexpr uint32_t kRegistry = 0x800CE1B0; // 50 x 16 (the model registry)
constexpr uint32_t kTagRmd3 = 0x33444D52, kTagDod3 = 0x33444F44, kTagDpd3 = 0x33445044, kTagBbd3 = 0x33444242;
constexpr uint32_t kBufferVerts = 1200;     // every race capture: 0x4B00 / 0x12C0 / 0x4B0 bytes apart
constexpr uint32_t kWeaponModel = 800;
constexpr uint32_t kPool3Slots = 0x800CF660, kCarBytes = 512;

uint32_t HostU32(const std::vector<uint8_t>& f, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, f.data() + at, 4);
    return v;
}

// OURS: the loader's mallocs (SLUS 0x8001447C's block rule: size (n + 11) & ~7, the caller gets +4).
struct Bump {
    uint32_t next = 0, limit = 0;
    bool failed = false;
    uint32_t Alloc(uint32_t n) {
        if (n == 0) return 0;
        const uint32_t size = (n + 11u) & ~7u;
        if (next + size > limit) {
            failed = true;
            return 0;
        }
        const uint32_t user = next + 4u;
        next += size;
        return user;
    }
};

// RASHCDI 0x8005C0C4's walk over one RMD3 chunk and its four handlers (weapon_session.cpp's
// transcription, restated for the machines' models).
struct ChunkLoader {
    GuestRam& g;
    Bump& heap;
    int32_t Rmd(uint32_t id, uint32_t chunk) { // 0x8005CB9C: the first free registry slot
        for (int32_t k = 0; k < 50; ++k) {
            const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
            if (g.U32(reg) != 0) continue;
            g.W32(reg, id);
            const uint32_t gc = g.U8(chunk + 12);
            g.W8(reg + 4, static_cast<uint8_t>(gc));
            const uint32_t p = heap.Alloc(gc * 12u);
            g.W32(reg + 8, p);
            if (p != 0) g.W32(p, 0);
            return k;
        }
        return -1;
    }
    int32_t Dod(uint32_t id, uint32_t chunk, int32_t gi) { // 0x8005CC4C
        for (int32_t k = 0; k < 50; ++k) {
            const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
            if (g.U32(reg) != id) continue;
            const uint32_t parts = heap.Alloc(4u * g.U16(chunk + 24));
            const uint32_t grp = g.U32(reg + 8) + 12u * static_cast<uint32_t>(gi);
            g.W32(grp + 4, parts);
            g.W32(grp + 0, chunk);
            g.W32(chunk + 32, chunk + g.U32(chunk + 32));
            g.W32(chunk + 36, chunk + g.U32(chunk + 36));
            for (uint32_t off : {40u, 48u, 44u}) {
                const uint32_t v = g.U32(chunk + off);
                g.W32(chunk + off, v != 0 ? chunk + v : 0u);
            }
            return k;
        }
        return -1;
    }
    int32_t Dpd(uint32_t id, uint32_t chunk, int32_t gi, int32_t si) { // 0x8005CD60
        int32_t k = 0;
        for (; k < 50; ++k)
            if (g.U32(kRegistry + 16u * static_cast<uint32_t>(k)) == id) break;
        if (k == 50) return -1;
        const uint32_t v = g.U32(chunk + 20);
        g.W32(chunk + 20, v != 0 ? chunk + v : 0u);
        const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
        const uint32_t grp = g.U32(reg + 8) + 12u * static_cast<uint32_t>(gi);
        g.W32(g.U32(grp + 4) + 4u * g.U8(chunk + 13), chunk);
        if (static_cast<uint32_t>(gi + 1) == g.U8(reg + 4) && static_cast<uint32_t>(si + 1) == g.U16(g.U32(grp) + 24))
            g.W8(reg + 6, 1);
        return 0;
    }
    int32_t Bbd(uint32_t id, uint32_t chunk, int32_t gi) { // 0x8005CE78
        int32_t k = 0;
        for (; k < 50; ++k)
            if (g.U32(kRegistry + 16u * static_cast<uint32_t>(k)) == id) break;
        if (k == 50) return -1;
        g.W32(g.U32(kRegistry + 16u * static_cast<uint32_t>(k) + 8) + 12u * static_cast<uint32_t>(gi) + 8u, chunk);
        return 0;
    }
    int32_t Walk(uint32_t buf, int32_t size) { // 0x8005C0C4
        uint32_t s0 = buf;
        bool inModel = false;
        int32_t gi = -1, si = 0, slot = -1;
        while (static_cast<int32_t>(s0 - buf) < size) {
            const uint32_t tag = g.U32(s0), len = g.U32(s0 + 4), id = g.U32(s0 + 8);
            if (g.Faulted() || (len == 0 && tag != kTagRmd3)) return -1;
            if (tag == kTagRmd3) {
                slot = Rmd(id, s0);
                if (slot == -1) return -1;
                inModel = true;
                s0 += 16;
                continue;
            }
            if (tag == kTagDod3) {
                ++gi;
                si = 0;
                if (inModel && Dod(id, s0, gi) == -1) return -1;
            } else if (tag == kTagDpd3) {
                if (Dpd(id, s0, gi, si) == -1) return -1;
                ++si;
            } else if (tag == kTagBbd3) {
                if (Bbd(id, s0, gi) == -1) return -1;
            }
            s0 += len;
        }
        if (slot >= 0) g.W32(kRegistry + 16u * static_cast<uint32_t>(slot) + 12u, 0u); // 0x8005C258
        return slot;
    }
};

uint32_t RegistryOf(GuestRam& g, uint32_t id) {
    for (uint32_t k = 0; k < 50; ++k)
        if (g.U32(kRegistry + 16u * k) == id) return kRegistry + 16u * k;
    return 0;
}

// SLUS 0x800251E4's head, the part the product runs, in its order: the weapon glow 0x80027B80, the effect
// capture 0x80029CA4 and the bike light 0x80028534, each under the emitter's condition (+0x24 read on entry).
struct CaptureHead final : M::DrawCallees {
    GuestRam& g;
    ModelRuntime& rt;
    CaptureHead(GuestRam& gg, ModelRuntime& r) : g(gg), rt(r) {}
    int32_t ofx = 192, ofy = 120; // the view's GTE offset (the shadow's RTPT, shadow_product.h)
    bool Emit(uint32_t obj, uint32_t view) override {
        ++rt.drawCalls;
        const uint32_t w24 = g.U32(obj + 36u);
        const int32_t lod = g.S8(obj + 8u);
        if (((g.U16(g.U32(obj) + 14u) & 0x78u) >> 3) == 5u) {
            ++rt.weaponEmits;
            if (lod >= 6) ++rt.weaponEmitsHigh;
        }
        if (rr::sim::WeaponGlowWanted(g, obj)) {
            ++rt.weaponGlows;
            if (!Glow(obj, false)) return false;
        }
        const int32_t head = g.S8(obj + 0x49u);
        if (head != -1) {
            const uint32_t st = (g.U32(rr::sim::kEffectPool + rr::sim::kEffectRecordBytes * static_cast<uint32_t>(head)) >> 6) & 0xFu;
            if (lod < 2 || st == 5u || st == 6u) {
                rr::sim::EffectCaptureChain(g, obj);
                ++rt.captures;
            }
        }
        if (rr::sim::BikeLightWanted(g, obj, w24)) {
            ++rt.headlights;
            if (!Glow(obj, true)) return false;
        }
        ShadowTail(g, obj, view, w24, ofx, ofy, rt.shadows); // the emitter's tail: the shadow (shadow_product.h)
        return !g.Faulted();
    }
    // One glow sprite into the pass's ordering table (fx_glow.h); counted, not run, without the pass's
    // environment or under RRJB_FXDRAW=off.
    bool Glow(uint32_t obj, bool light) {
        const char* off = std::getenv("RRJB_FXDRAW");
        if (rt.fx == nullptr || (off != nullptr && std::strcmp(off, "off") == 0)) {
            ++rt.glowsNotRun;
            return true;
        }
        const uint32_t before = rt.fx->drawing;
        rt.fx->drawing = 0; // no effect record: the sink files the packet as state 0
        const uint32_t heap = g.U32(g.U32(rr::sim::kFxPacketHeapPtr) + 0x10Cu);
        if (light) rr::sim::BikeLight(g, *rt.fx, obj);
        else rr::sim::WeaponGlow(g, *rt.fx, obj);
        rt.fx->drawing = before;
        if (g.U32(g.U32(rr::sim::kFxPacketHeapPtr) + 0x10Cu) != heap) ++(light ? rt.lightPackets : rt.glowPackets);
        return !rt.fx->refused && !g.Faulted();
    }
};

} // namespace

std::string BuildModelArenaLine(GuestRam& g, const rr::DiscImage& disc, int bank, uint32_t& from, uint32_t limit) {
    // LoadBikeBank RASHCDI 0x8005C45C's file (rrformats/level_bank.h: bblevJBD / bblevJBK in Jailbreak)
    const uint32_t gsp = g.U32(0x8005B2F8u);
    const std::string file = rr::LevelBankFile(rr::LevelBankIndex(bank, g.U8(gsp + 4u), g.S32(gsp + 0x48u)), ".GEO");
    const auto f = disc.Find(file);
    if (!f) return "the model arena was NOT built: " + file + " is not on the disc";
    const std::vector<uint8_t> bytes = disc.ReadFile(*f);
    Bump heap;
    heap.next = (from + 7u) & ~7u;
    heap.limit = limit;
    const uint32_t start = heap.next;
    std::string ids;
    size_t at = 0;
    int models = 0, preloaded = 0; // preloaded: registered by the race loader's PORTED LoadBikeBank already
    while (at + 16 <= bytes.size()) {
        const uint32_t tag = HostU32(bytes, at), len = HostU32(bytes, at + 4), id = HostU32(bytes, at + 8);
        if (len == 0 || at + len > bytes.size()) break;
        if (tag == kTagRmd3 && id != kWeaponModel && RegistryOf(g, id) != 0) {
            ++preloaded;
            ids += (ids.empty() ? "" : ", ") + std::to_string(id) + "@" + std::to_string((RegistryOf(g, id) - kRegistry) / 16u);
        }
        if (tag == kTagRmd3 && id != kWeaponModel && RegistryOf(g, id) == 0) {
            if (heap.next + len > limit) return "the model arena was NOT built: no room for model " + std::to_string(id);
            const uint32_t chunk = heap.next;
            g.WriteBlock(chunk, bytes.data() + at, len);
            heap.next = (heap.next + len + 7u) & ~7u;
            ChunkLoader ld{g, heap};
            const int32_t slot = ld.Walk(chunk, static_cast<int32_t>(len));
            if (slot < 0 || heap.failed || g.Faulted())
                return "the model arena was NOT built: the chunk walker refused model " + std::to_string(id);
            g.W8(kRegistry + 16u * static_cast<uint32_t>(slot) + 7u, 0xFF); // no page table here: no key
            ids += (ids.empty() ? "" : ", ") + std::to_string(id) + "@" + std::to_string(slot);
            ++models;
        }
        at += len;
    }
    // LoadBikeBank's tail 0x8005C4EC..0x8005C5F0 (rules.md 16.5): each player p < gs+0x30 whose bike index
    // gs+0x48 + 4p is 6..8 / 15..17 and whose model 100 + index is not registered (0x8005C010) loads
    // DATA\<name>.MRO (name table 0x8006B51C, 9 bytes apart, via 0x80064034(0, index)) through 0x8005C30C: the
    // leading LECT chunk goes to the texture uploader 0x8005DDB8 (VRAM: the renderer's), the rest is copied to a
    // malloc'd block and walked by 0x8005C0C4 with a3 = 1.
    const char* rigOff = std::getenv("RRJB_RIGBIND"); // negative control: "off" = no rig model in the arena
    for (uint32_t p = 0; p < g.U32(gsp + 0x30u) && p < 2u && !(rigOff != nullptr && rigOff[0] == 'o'); ++p) {
        const uint32_t idx = g.U32(gsp + 0x48u + 4u * p);
        if (!((idx - 6u) < 3u || (idx - 15u) < 3u)) continue;
        const std::string mro = std::string("DATA/") + (idx < 9u ? "CRUISE" : "SPORT") + "ABS"[(idx % 9u) / 3u] +
                                static_cast<char>('1' + idx % 3u) + ".MRO";
        if (const uint32_t reg = RegistryOf(g, 100u + idx); reg != 0) { // registered already (the PORTED LoadBikeBank)
            ++preloaded;
            ids += (ids.empty() ? "" : ", ") + std::to_string(100u + idx) + "@" + std::to_string((reg - kRegistry) / 16u) +
                   " (" + mro + ")";
            continue;
        }
        const auto mf = disc.Find(mro);
        if (!mf) return "the model arena was NOT built: " + mro + " is not on the disc";
        const std::vector<uint8_t> mb = disc.ReadFile(*mf);
        const size_t skip = (mb.size() >= 8 && HostU32(mb, 0) == 0x5443454Cu) ? HostU32(mb, 4) : 0; // "LECT"
        if (skip >= mb.size() || heap.next + (mb.size() - skip) > limit)
            return "the model arena was NOT built: no room for " + mro;
        const uint32_t chunk = heap.next;
        g.WriteBlock(chunk, mb.data() + skip, static_cast<uint32_t>(mb.size() - skip));
        heap.next = (heap.next + static_cast<uint32_t>(mb.size() - skip) + 7u) & ~7u;
        ChunkLoader ld{g, heap};
        const int32_t slot = ld.Walk(chunk, static_cast<int32_t>(mb.size() - skip));
        if (slot < 0 || heap.failed || g.Faulted()) return "the model arena was NOT built: the chunk walker refused " + mro;
        g.W8(kRegistry + 16u * static_cast<uint32_t>(slot) + 7u, 0xFF);
        ids += (ids.empty() ? "" : ", ") + std::to_string(g.U32(kRegistry + 16u * static_cast<uint32_t>(slot))) + "@" +
               std::to_string(slot) + " (" + mro + ")";
        ++models;
    }
    const uint32_t verts = heap.Alloc(16u * kBufferVerts), screen = heap.Alloc(4u * kBufferVerts),
                   flags = heap.Alloc(kBufferVerts);
    if (heap.failed || verts == 0) return "the model arena was NOT built: no room for the vertex buffers";
    g.W32(M::kVertsPtr, verts);
    g.W32(M::kScreenPtr, screen);
    g.W32(M::kFlagsPtr, flags);
    from = heap.next;
    char b2[600];
    std::snprintf(b2, sizeof(b2),
                  "the model arena (model_runtime.h; RASHCDI 0x8005C0C4 and its handlers %s): %s models %s "
                  "(registry slot after '@'), the vertex buffers *(0x8005ACB0) 0x%08X / *(0x8005ACB4) 0x%08X / "
                  "*(0x8005ACB8) 0x%08X (1200 vertices, as every capture), placed OURS at 0x%08X..0x%08X%s",
                  models == 0 ? "PORTED under LoadBikeBank" : "transcribed", file.c_str(), ids.c_str(), verts,
                  screen, flags, start, heap.next, g.Faulted() ? " - FAULTED" : "");
    if (models == 0 && preloaded != 0) // the bank's models were registered by the PORTED LoadBikeBank
        return std::string(b2) + " - the bank's " + std::to_string(preloaded) + " model(s) registered by the PORTED LoadBikeBank";
    return models == 0 ? std::string("the model arena was NOT built: no model in ") + file : std::string(b2);
}

uint32_t ProductBikeModel(int bank, bool police) {
    const uint32_t b = static_cast<uint32_t>(bank < 0 ? 0 : (bank > 2 ? 2 : bank));
    return (police ? 118u : 100u) + b;
}

std::string BindModels(GuestRam& g, const std::vector<ModelBinding>& list) {
    size_t bound = 0, missing = 0;
    for (const ModelBinding& m : list) {
        const uint32_t reg = RegistryOf(g, m.model);
        if (reg == 0 || m.object < 0x80000000u) {
            ++missing;
            continue;
        }
        const uint32_t dod = g.U32(g.U32(reg + 8u));
        const uint32_t kind = (g.U16(dod + 14u) & 0x78u) >> 3;
        g.W32(m.object + 0x60u, reg);
        g.W32(m.object + 0x64u, kind == 1u ? 0x80054198u : kind == 2u ? 0x80054178u : 0u); // RegistryBind's
        g.W8(m.object + 8u, static_cast<uint8_t>(g.U8(reg + 4u) - 1u)); // RegistryBind leaves the last LOD
        g.W8(m.object + 10u, 0);
        g.W8(m.object + 11u, 0);
        rr::sim::LodSelect(g, m.object, 0);
        if (!g.Faulted()) ++bound;
    }
    char b[300];
    std::snprintf(b, sizeof(b),
                  "the machines' model binding (model_runtime.h): %zu object(s) bound at LOD 0 through the PORTED "
                  "LodSelect SLUS 0x8001298C (+0x00 DOD3, +0x28, the part slots' DPD3), %zu without a model; OURS: "
                  "bikes model %u, police %u, riders %u (the product draws 100 / 150)",
                  bound, missing, ProductBikeModel(0, false), ProductBikeModel(0, true), kProductRiderModel);
    return b;
}

bool ModelRuntime::Frame(GuestRam& g, const std::vector<uint32_t>& entities, std::vector<uint32_t>& drawn,
                         uint32_t view, int32_t ofx, int32_t ofy) {
    drawn.clear();
    if (g.U32(M::kVertsPtr) == 0) return false;
    ++frames;
    uint32_t cr[32] = {}, dr[32] = {};
    cr[24] = static_cast<uint32_t>(ofx) << 16; // OFX / OFY: the view's (192 / 120 every capture's); H: 237
    cr[25] = static_cast<uint32_t>(ofy) << 16;
    cr[26] = 237;
    M::ModelGte gte = M::ModelGte::From(cr, dr);
    CaptureHead head(g, *this);
    head.ofx = ofx;
    head.ofy = ofy;
    if (view == 0) shadows.Clear();
    g.W32(M::kDrawList, 0);
    for (uint32_t e : entities) {
        const bool car = e >= kPool3Slots && e < kPool3Slots + 16u * kCarBytes;
        // RASHCDG 0x8008CFDC's pool-3 loop (the product runs its bike loop in the world pass): the PORTED
        // ViewDistance 0x8008DBCC for a live car (+0xAC and +0x140 non-zero, FxEntities' test)
        if (car && view == 0) {
            rr::sim::ViewDistance(g, e);
            if (static_cast<uint32_t>(g.S32(e + 0x2Cu)) < minCarDist) minCarDist = static_cast<uint32_t>(g.S32(e + 0x2Cu));
        }
        if (car && view == 0 && g.U32(e + 0x64u) != 0) {
            M::ModelLod(g, e, 1);
            ++carLods;
            const size_t l = static_cast<size_t>(g.S8(e + 10u) < 0 ? 0 : g.S8(e + 10u));
            if (l > maxCarLod) maxCarLod = l;
        }
        if (g.U32(e + 0x60u) == 0) continue; // not bound: nothing to draw
        if (g.U32(e + 0x34u) == 0 && g.S32(e + 0xB0u) == -1) {
            // OURS, named: an entity in no cell (+0xB0 = -1: the product's cars - the pass that files entities
            // into the cells is not ported) makes ModelVisible return at its first test. Such a car keeps what
            // the product did before (its effect pass, no capture), and ModelVisible's LOD step is applied
            // here: LodSelect SLUS 0x8001298C to the LOD ModelLod chose.
            if (car && view == 0 && g.U32(e + 0x64u) != 0) {
                rr::sim::LodSelect(g, e, static_cast<uint32_t>(static_cast<int32_t>(g.S8(e + 10u))));
                ++cellless;
            }
            drawn.push_back(e);
            continue;
        }
        const uint32_t visibleNow = M::ModelVisible(g, gte, e, view);
        if (std::getenv("RRJB_MODEL_TRACE")) // DEVELOPMENT: each entity's visibility test
            std::printf("modeltrace: view %u obj %08X kind %u dist %d cell %d lod %d -> %u\n", view, e,
                        (g.U16(g.U32(e) + 14u) & 0x78u) >> 3, g.S32(e + 0x2Cu + 4u * view), g.S32(e + 0xB0u), g.S8(e + 8u),
                        visibleNow);
        if (std::getenv("RRJB_MODEL_TRACE") && ((g.U16(g.U32(e) + 14u) & 0x78u) >> 3) == 4u) {
            const uint32_t s0 = g.U32(e + 4u) + 4u;
            std::printf("modeltrace:   +0x48 %d slot0 %d %d %d / %d %d %d / %d %d %d  +0x68 %d %d %d / %d %d %d / %d %d %d  rows %d %d %d / %d %d %d / %d %d %d\n",
                        g.S8(e + 0x48u), g.S16(s0), g.S16(s0 + 2), g.S16(s0 + 4), g.S16(s0 + 6), g.S16(s0 + 8), g.S16(s0 + 10),
                        g.S16(s0 + 12), g.S16(s0 + 14), g.S16(s0 + 16), g.S16(e + 0x68), g.S16(e + 0x6A), g.S16(e + 0x6C),
                        g.S16(e + 0x6E), g.S16(e + 0x70), g.S16(e + 0x72), g.S16(e + 0x74), g.S16(e + 0x76), g.S16(e + 0x78),
                        g.S16(e + 0x1B0), g.S16(e + 0x1B2), g.S16(e + 0x1B4), g.S16(e + 0x1B6), g.S16(e + 0x1B8),
                        g.S16(e + 0x1BA), g.S16(e + 0x1BC), g.S16(e + 0x1BE), g.S16(e + 0x1C0));
        }
        if (visibleNow == 0) {
            ++hidden;
            continue;
        }
        ++visible;
        rr::sim::shadow::BeginEntry(g, e); // 0x80067690's two stores before the entry's draw (the shadow's counter)
        if (!M::ModelDraw(g, gte, e, view, head) || g.Faulted()) {
            if (refused++ == 0) {
                firstRefusedObj = e;
                firstRefusedFault = g.Faulted() ? g.FaultAddress() : 0u;
                firstRefusedKind = (g.U16(g.U32(e) + 14u) & 0x78u) >> 3;
            }
            g.ClearFault();
            continue;
        }
        drawn.push_back(e);
    }
    g.W32(M::kDrawList, 0);
    return true;
}

std::string ModelRuntime::Totals() const {
    char b[900];
    std::snprintf(b, sizeof(b),
                  "the PORTED model draw (model_runtime.h): %zu frame(s), ModelVisible 0x80067AC4 %zu visible / %zu "
                  "hidden, ModelDraw 0x80068468 with its parts and seats (%zu emitter call(s) served: %zu effect "
                  "capture(s) 0x80029CA4 PORTED, %zu weapon glow(s) WeaponGlow 0x80027B80 PORTED -> %zu packet(s), %zu bike "
                  "light(s) BikeLight 0x80028534 PORTED -> %zu packet(s), %zu glow call(s) NOT RUN%s; weapon objects at the "
                  "emitter %zu, at LOD 6..8 (prod, stun gun, spray) %zu), %zu refused; ModelLod 0x800667C4 on %zu car frame(s), LOD up to %zu, the nearest car "
                  "%u (+0x2C); %zu car frame(s) in no cell (+0xB0 = -1: LOD applied, not drawn, pass as before)\n",
                  frames, visible, hidden, drawCalls, captures, weaponGlows, glowPackets, headlights, lightPackets,
                  glowsNotRun, (std::getenv("RRJB_FXDRAW") != nullptr && std::strcmp(std::getenv("RRJB_FXDRAW"), "off") == 0)
                      ? " - RRJB_FXDRAW=off" : "",
                  weaponEmits, weaponEmitsHigh, refused, carLods, maxCarLod,
                  minCarDist, cellless);
    std::string out = b;
    if (refused != 0) {
        char r[200];
        std::snprintf(r, sizeof(r), "  the first refused ModelDraw: object 0x%08X (DOD3 kind %u), %s0x%08X\n", firstRefusedObj,
                      firstRefusedKind, firstRefusedFault != 0 ? "a guest fault at " : "no fault, callee refused ", firstRefusedFault);
        out += r;
    }
    return out;
}

} // namespace rr::game
