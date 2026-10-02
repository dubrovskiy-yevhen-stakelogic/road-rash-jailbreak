// The race set-up's camera, light and collision ports in the product (loader2_cam_product.h).
#include "game/loader2_cam_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

#include "game/loader_product.h"
#include "game/sim/camera_setup.h"
#include "rrformats/level_bundle.h"

namespace rr::game {

namespace s = rr::sim;
using s::GuestRam;

namespace {
struct Counts {
    size_t camSetUp = 0, camFileRead = 0, camInit = 0, cdReads = 0, levelLight = 0, levelShade = 0, collSetUp = 0;
};
Counts g_n;

std::string GuestString(GuestRam& g, uint32_t a, size_t max = 128) {
    std::string r;
    for (size_t k = 0; k < max; ++k) {
        const uint8_t c = g.U8(a + static_cast<uint32_t>(k));
        if (c == 0 || g.Faulted()) break;
        r.push_back(static_cast<char>(c));
    }
    return r;
}
} // namespace

bool Loader2CamPorted() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_LOADER2_CAM");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

std::string PortedCameraSetUp(GuestRam& g, const DiscImage& disc, uint32_t sp, bool noShots, bool& ok, uint32_t fileOffset) {
    ok = false;
    LoaderOverlay ov(g, disc); // RASHCDI's code and file-name formats (0x8005B8F8 .. 0x8005B910) where the console has them
    if (!ov.ok()) return "the camera set-up NOT run: RASHCDI.BIN is not on the disc";
    std::string fileName, cannotOpen;
    std::vector<uint8_t> file;
    s::RoadRuntimeNative road;
    ProductLoaderCallees* self = nullptr;
    ProductLoaderCallees c(g, nullptr, &disc, nullptr, 0,
                           [&](uint32_t fn, const uint32_t* a, int, uint32_t csp, uint32_t& v0, bool& handled) {
                               handled = true;
                               switch (fn) {
                               case s::kCamFileReadFn:                     // PORTED (row camera_file_read)
                                   ++g_n.camFileRead;
                                   return s::CameraFileRead(g, a[0], csp, *self);
                               case s::kCameraInitFn:                      // PORTED (row camera_init)
                                   ++g_n.camInit;
                                   return s::CameraInit(g, a[0], a[1], a[2], a[3], a[4], csp, road);
                               case s::kCdOpen: {                          // the CD: the disc image
                                   fileName = GuestString(g, a[0]);
                                   file.clear();
                                   if (const auto f = disc.Find(fileName)) file = disc.ReadFile(*f);
                                   v0 = file.empty() ? 0xFFFFFFFFu : 1u;
                                   return true;
                               }
                               case s::kCdRead: {
                                   uint32_t n = 0;
                                   for (; n < a[2] && fileOffset + n < file.size(); ++n)
                                       g.W8(a[1] + n, file[fileOffset + n]);
                                   ++g_n.cdReads;
                                   v0 = n;
                                   return !g.Faulted();
                               }
                               case s::kCdClose:
                                   return true;
                               case s::kPrintf:                            // "Cannot open %s for reading"
                                   cannotOpen = GuestString(g, a[1]);
                                   return true;
                               default:
                                   handled = false;                        // sprintf: the host's (loader_product)
                                   return true;
                               }
                           });
    self = &c;
    ++g_n.camSetUp;
    const bool ran = s::CameraSetUp(g, sp, c);
    if (!ran || g.Faulted() || !c.error.empty()) {
        g.ClearFault();
        return "the camera set-up CameraSetUp RASHCDI 0x80067564 (PORTED) REFUSED" + (c.error.empty() ? std::string() : ": " + c.error);
    }
    ok = true;
    // OURS, named: without a shot table the intro director CameraInit starts (+0x304 = 1) has nothing to fly - the
    // view record is put back on the chase camera exactly as CameraInit's other arm leaves it.
    std::string deviation;
    const uint32_t players = g.U32(s::kCamPlayer2Bike) != 0 ? 2u : 1u;
    for (uint32_t k = 0; noShots && k < players; ++k) {
        const uint32_t v = s::kCamView0 + s::kCamViewStride * k;
        if (g.U32(v + 0x304u) == 0) continue;
        g.W32(v + 0x304u, 0);
        g.W32(v + 0x21Cu, g.U32(v + 0x220u));
        g.W32(v + 0x224u, g.U32(v + 0x224u) | 0x106u);
        g.W32(v + 0x228u, g.U32(v + 0x228u) & ~0x40u);
        deviation += " view " + std::to_string(k);
    }
    char b[700];
    std::snprintf(b, sizeof(b),
                  "the camera set-up PORTED (loader2_cam): CameraSetUp RASHCDI 0x80067564 (row camera_set_up) %zu, "
                  "CameraFileRead RASHCDI 0x80069618 (row camera_file_read) %zu (%s from the disc image%s%s), CameraInit "
                  "SLUS 0x8002F308 (row camera_init) %zu; view record 0's +0x21C mode %u, +0x224 0x%X, +0x304 %u%s%s%s",
                  g_n.camSetUp, g_n.camFileRead, fileName.c_str(), cannotOpen.empty() ? "" : " - CANNOT OPEN ",
                  cannotOpen.c_str(), g_n.camInit, g.U32(s::kCamView0 + 0x21Cu), g.U32(s::kCamView0 + 0x224u),
                  g.U32(s::kCamView0 + 0x304u), players == 2u ? "; two players: view record 1 (handle 0x9E) too" : "",
                  deviation.empty() ? "" : "; OURS: no shot table, the intro director switched back off on",
                  deviation.c_str());
    return b;
}

std::string PortedLevelLight(GuestRam& g, const DiscImage& disc, int raceId, uint32_t sp) {
    const auto bin = disc.Find("DATA/GAMEBIN1.DAT");
    if (!bin) return "the level's light stores were NOT made: DATA/GAMEBIN1.DAT is not on the disc";
    const std::vector<uint8_t> file = disc.ReadFile(*bin);
    rr::LevelBundle bundle;
    try {
        bundle = rr::ParseLevelBundle(file, rr::LevelBundleIndexForRace(raceId));
    } catch (const std::exception& e) {
        return std::string("the level's light stores were NOT made: ") + e.what();
    }
    // The payload staged OURS on the guest stack 4 KiB below `sp` (below every frame of the two), then put back.
    const uint32_t stage = (sp - 0x1000u - 0x500u) & ~15u;
    std::vector<uint8_t> saved(0x500u);
    g.ReadBlock(stage, saved.data(), 0x500u);
    std::string done;
    if (const rr::LevelBundleSection* s2 = bundle.Find(2); s2 && s2->payload + 36 <= s2->limit && s2->payload + 36 <= file.size()) {
        g.WriteBlock(stage, file.data() + s2->payload, 36u);
        if (s::LevelLight(g, stage, sp)) {
            ++g_n.levelLight;
            done += "LevelLight RASHCDI 0x8006250C (row level_light) " + std::to_string(g_n.levelLight) + " (light (" +
                    std::to_string(g.S16(s::kLightBlock + 0x28u)) + ", " + std::to_string(g.S16(s::kLightBlock + 0x2Au)) +
                    ", " + std::to_string(g.S16(s::kLightBlock + 0x2Cu)) + "))";
        }
    }
    if (const rr::LevelBundleSection* s7 = bundle.Find(7); s7 && s7->payload + 0x486 <= s7->limit && s7->payload + 0x488 <= file.size()) {
        g.WriteBlock(stage, file.data() + s7->payload, 0x488u);
        if (s::LevelShade(g, stage)) {
            ++g_n.levelShade;
            char c[160];
            std::snprintf(c, sizeof(c), "%sLevelShade RASHCDI 0x80062430 (row level_shade) %zu (unlit step %u, shadow colour 0x%06X)",
                          done.empty() ? "" : ", ", g_n.levelShade, g.U16(s::kLightBlock + 0x44u), g.U32(s::kLightBlock + 0x0Cu));
            done += c;
        }
    }
    g.WriteBlock(stage, saved.data(), 0x500u);
    if (g.Faulted()) {
        g.ClearFault();
        return "the level's light stores (PORTED) FAULTED";
    }
    return "the level's light stores PORTED (loader2_cam, camera_setup.h): " +
           (done.empty() ? std::string("none - no type-2 / type-7 section") : done);
}

std::string PortedCollisionSetUp(GuestRam& g) {
    s::CollisionSetUp(g);
    ++g_n.collSetUp;
    return "PORTED CollisionSetUp RASHCDG 0x800A41EC (row collision_set_up) " + std::to_string(g_n.collSetUp);
}

} // namespace rr::game
