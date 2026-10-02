// rrview - the renderer's instrument.
//
// It draws the same scene `rrgame` draws - one shared renderer, `src\render` - and then draws it
// again with one surface reporting what it sampled instead of what it looks like, so that every
// visible pixel can be compared with the palette entry the binding rule predicts. That is what
// `--texcheck`, `--cellcheck`, `--bikecheck`, `--skycheck` and `--skygradcheck` do, each with its
// own negative control. It also still views a single `*.GEO` straight off the player's disc image.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <GL/gl.h>

#include "game/audio/mixer.h"
#include "game/world.h"
#include "game/rider_pose.h"
#include "platform/audio_device.h"
#include "platform/png.h"
#include "render/gl_api.h"
#include "render/mat4.h"
#include "render/race_scene.h"
#include "render/scene_geometry.h"
#include "render/shaders.h"
#include "render/window_win32.h"
#include "rrformats/audio.h"
#include "rrformats/cell.h"
#include "rrformats/chunk.h"
#include "rrformats/model_texture.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/level_bundle.h"
#include "rrformats/sky_gradient.h"
#include "rrvfs/disc_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The renderer lives in `rr::render` and this tool is its instrument, so it is used wholesale
// rather than name by name.
using namespace rr::render;

namespace {

// ---------------------------------------------------------------- camera keys
// Orbit mode. Drive mode's motion is PLACEHOLDER here - `rrview` is the instrument, not the game;
// the ported simulation runs in `rrgame`.
float g_orbit = 0.6f;
float g_pitch = 0.35f;
float g_distance = 1.0f; // multiplied by the model radius
bool g_spin = true;
float g_rideSpeed = 6.6f;   // world units per frame
float g_rideLateral = 0.0f; // offset from the centre line, world units
bool g_driveKeys = false;   // arrows steer and change speed instead of orbiting

void ViewerKey(int key) {
    if (g_driveKeys) {
        if (key == VK_LEFT) g_rideLateral -= 1.5f;
        if (key == VK_RIGHT) g_rideLateral += 1.5f;
        if (key == VK_UP) g_rideSpeed += 1.0f;
        if (key == VK_DOWN) g_rideSpeed = std::max(0.0f, g_rideSpeed - 1.0f);
        g_rideLateral = std::clamp(g_rideLateral, -18.0f, 18.0f);
        return;
    }
    if (key == VK_LEFT) g_orbit -= 0.1f;
    if (key == VK_RIGHT) g_orbit += 0.1f;
    if (key == VK_UP) g_pitch += 0.05f;
    if (key == VK_DOWN) g_pitch -= 0.05f;
    if (key == VK_OEM_PLUS || key == VK_ADD) g_distance *= 0.9f;
    if (key == VK_OEM_MINUS || key == VK_SUBTRACT) g_distance *= 1.1f;
    if (key == VK_SPACE) g_spin = !g_spin;
}

// ---------------------------------------------------------------- music
// Feeds the output device from the mixer. Render() runs on the device's thread.
class MixerSink final : public rr::platform::AudioSink {
public:
    explicit MixerSink(rr::audio::Mixer& mixer) : mixer_(mixer) {}
    void Render(int16_t* out, size_t frames) override { mixer_.Mix(out, frames); }

private:
    rr::audio::Mixer& mixer_;
};

// Decodes a bounded slice of one ALBUM.ALB track and returns it as a loopable source. The album is
// 63 MB, so a whole track is not held in memory: `maxUnits` 16 KiB units are taken from its start.
std::shared_ptr<rr::audio::MemorySource> LoadMusicTrack(const rr::DiscImage& disc, size_t trackIndex,
                                                        size_t maxUnits, double& secondsOut) {
    const auto exeFile = disc.Find("SLUS_010.53");
    const auto albumFile = disc.Find("DATA/ALBUM.ALB");
    if (!exeFile || !albumFile) return nullptr;

    const std::vector<rr::AlbTrack> tracks = rr::ParseAlbumTrackTable(disc.ReadFile(*exeFile));
    if (trackIndex >= tracks.size()) return nullptr;
    const rr::AlbTrack& track = tracks[trackIndex];

    const size_t units = std::min(track.Units(), maxUnits);
    if (units == 0) return nullptr;
    std::vector<uint8_t> bytes(units * rr::kAlbUnitSize);
    disc.ReadForm1(albumFile->lba, track.StartUnit() * rr::kAlbUnitSize, bytes.data(), bytes.size());

    rr::PcmBuffer pcm = rr::DecodeAlb(bytes);
    secondsOut = pcm.Seconds();
    return std::make_shared<rr::audio::MemorySource>(std::move(pcm.samples), pcm.channels, pcm.sampleRate);
}

int Usage() {
    std::fprintf(stderr,
                 "usage: rrview <disc.bin> <path-on-disc> [object] [group] [--shot <out.png>]\n"
                 "       rrview <disc.bin> <RACEn_m.STP> --road [--drive] [--at <dist>] [--shot <out.png>]\n"
                 "  e.g. rrview \"...Road Rash - Jailbreak (USA).bin\" DATA/BBLEVEL1.GEO 0 0\n"
                 "  --tex                   texture the roadside props from HAZARD<n>.TEX (0; with --state\n"
                 "                          the capture's own set; --hazard <n> names one)\n"
                 "  --propcheck <report.txt> the props against the original's packets and the capture's\n"
                 "                          live props (needs --state and --orig-prims); its controls:\n"
                 "                          --propcheck-mutate cull|scale|mirror|uv\n"
                 "  --texcheck <report.txt> check every visible prop pixel against the palette entry\n"
                 "                          the binding rule says it must show; needs --tex --shot\n"
                 "  --texcheck-mutate       the negative control for --texcheck: expect the wrong\n"
                 "                          palette row, and every prop pixel must then mismatch\n"
                 "  --cellcheck <report.txt> check every visible scene-cell pixel against the palette\n"
                 "                          entry its type-2 page says it must show; needs --tex --shot\n"
                 "  --cellcheck-mutate      negative control: expect the NEXT palette row\n"
                 "  --bikecheck <report.txt> check every visible pixel of the player's bike and\n"
                 "                          rider against the sheet and palette the rule predicts\n"
                 "  --bikecheck-mutate      negative control: expect the NEXT KNBP palette block\n"
                 "  --bikecheck-mutate-page negative control: expect the other sheet of the pair\n"
                 "  --chase <units>         how far behind the bike the chase camera sits (95)\n"
                 "  --no-sky                do not draw the type-4 panorama behind the world\n"
                 "  --skydump <out.png>     write the decoded 1760x128 panorama out as a PNG\n"
                 "  --skycheck <report.txt> check every visible backdrop pixel against that image\n"
                 "  --skycheck-mutate       negative control: expect the next texel COLUMN\n"
                 "  --skycheck-mutate-band  negative control: expect the next texel BAND\n"
                 "  --skygrad <n>           which GAMEBIN1.DAT level bundle the sky gradient above\n"
                 "                          the skyline takes its four colours from (default 3)\n"
                 "  --sunangle <n>          the per-level sun yaw the colour blend is measured from,\n"
                 "                          4096 per turn (default 0; 715 in the captured states)\n"
                 "  --no-skygrad            leave the sky above the skyline black, as it was\n"
                 "  --skygradcheck <report.txt> check every sky-gradient pixel against the blend of\n"
                 "                          the four colours read out of DATA\\GAMEBIN1.DAT\n"
                 "  --skygradcheck-mutate   negative control: predict the NEXT bundle's colours\n"
                 "  --pose <ram.bin>        put the player's bike and rider in the pose the live part\n"
                 "                          matrices of that guest RAM image hold, instead of the\n"
                 "                          rest pose and a guessed rider orientation\n"
                 "  --posecheck <report.txt> check those matrices against RotMatrix of an integer\n"
                 "                          Euler triple; needs --pose\n"
                 "  --posecheck-mutate      negative control: build the triple in XYZ order instead\n"
                 "  --cellcheck-mutate-page negative control: expect the right row of another page\n"
                 "  --band1                 draw the B groups from region 7 (fine) instead of 6\n"
                 "  --only-band1            diagnostic: the region-7 groups with nothing else\n"
                 "  --band2                 also draw band 2 (no fix-up pass resolves its texture)\n"
                 "  --state <dir>           reproduce a captured frame: camera (view record 0x800CD898),\n"
                 "                          player bike, rider pose and resident cells from <dir>\\ram.bin,\n"
                 "                          drawn with the original's projection (H 237, 4:3); use with\n"
                 "                          --race <set> <id> of the capture and --size 384 240\n"
                 "  --size <w> <h>          the frame size (default 1280 720)\n"
                 "  --legacy                draw the way the renderer did before render7: every group\n"
                 "                          coarse, no colour table, no texture windows, no band-2 road,\n"
                 "                          the 0-2 quad diagonal, a 57.3 degree field, the old bike drop\n"
                 "  --roadcheck <report.txt> compare the band-2 road (strips, UVs, palettes, colours, lane\n"
                 "                          lines) with the ORIGINAL's packets for the same frame; needs\n"
                 "                          --state and --orig-prims <csv> (tools\\scout\\psxgpu.py --prims)\n"
                 "  --skypacketcheck <report.txt> the cloud ring and the panorama against the original's\n"
                 "                          packets (needs --state and --orig-prims): corners, UVs, texels\n"
                 "  --skypacketcheck-mutate rotate|offs|yaw|slice   its negative controls\n"
                 "  --shadowcheck <report.txt> the bike's shadow quads against the original's 0x2A packets\n"
                 "  --shadowcheck-mutate    negative control: the light straight down\n"
                 "  --roadcheck-mutate-kind     negative control: every lane strip takes the other UV kind\n"
                 "  --roadcheck-mutate-palette  negative control: the next palette row\n"
                 "  --roadcheck-mutate-depth    negative control: the near path for every quad\n"
                 "  --texprobe              print, per prop group, what the shader will sample\n"
                 "  --textest               the indexed-texture path alone, on one big quad\n"
                 "  arrows orbit, +/- zoom, space stops the spin, Esc quits\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    rr::render::HideIfScriptedRun(argc, argv); // a script never gets a window, focus or sound
    std::vector<std::string> positional;
    std::string shotPath;
    bool rawParts = false;
    bool roadMode = false;
    bool driveMode = false;
    float driveDistance = 0.0f;
    int raceSet = 0, raceId = 0;
    bool noCells = false;
    bool drawRibbon = false;
    int bandMode = 0; // 0 = coarse everywhere, 1 = the B groups fine, 2 = the fine groups alone
    bool wantBand2 = false;
    bool stripQuads = false;
    int musicTrack = -1;
    bool useTextures = false;
    bool textureTest = false;
    bool texProbe = false;
    std::string texCheckPath;
    bool texCheckMutate = false;
    std::string cellCheckPath;
    int cellCheckMutate = 0; // 1 = the next palette row, 2 = the same row of a different page
    std::string bikeCheckPath;
    float chaseDistance = 95.0f; // how far behind the bike the chase camera sits
    int bikeCheckMutate = 0; // 1 = the next KNBP block, 2 = the other sheet of the pair
    bool drawSky = true;
    std::string skyDumpPath, skyCheckPath;
    int skyCheckMutate = 0; // 1 = the next texel column, 2 = the band above
    bool drawSkyGradient = true;
    size_t skyGradientBundle = 3; // the bundle whose colours are live in all four captured states
    bool skyGradientExplicit = false;
    int32_t sunAngle = 0;         // the per-level yaw the blend is measured from; 4096 = one turn
    std::string skyGradCheckPath;
    bool skyGradCheckMutate = false;
    std::string posePath, poseCheckPath;
    bool poseCheckMutate = false;
    size_t chunkBudget = 4000;
    // render7: the original's frame for a captured state, the level look, and the road check.
    std::string statePath;          // --state <dir>: camera, bike and resident cells from a capture
    int frameWidth = 1280, frameHeight = 720;
    bool legacy = false;            // --legacy: draw the way the renderer did before render7
    std::string roadCheckPath, origPrimsPath;
    int roadCheckMutate = 0;        // 1 kind, 2 palette row, 3 near/far depth
    std::string skyPacketCheckPath; // --skypacketcheck: clouds and panorama against the packets
    int skyPacketMutate = 0;        // RaceScene::SetSkyMutation: 1 rotate, 2 offs, 3 yaw, 4 slice
    bool sunAngleExplicit = false;
    std::string shadowCheckPath;    // --shadowcheck: the bike's shadow against the 0x2A packets
    std::string riderCheckPath;     // --ridercheck: the ported rider pose against the rider packets (rider_pose.h)
    std::string propCheckPath;      // --propcheck: the roadside props against the original's packets
    std::string propCheckMutate;    // --propcheck-mutate cull|scale|mirror|uv: its negative controls
    int hazardSet = -1;             // --hazard <n>: DATA\HAZARD<n>; default the capture's (--state) or 0
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            shotPath = argv[++i];
        else if (std::strcmp(argv[i], "--raw") == 0)
            rawParts = true; // draw the parts unassembled, as they sit in the file
        else if (std::strcmp(argv[i], "--road") == 0)
            roadMode = true; // draw the road surface of a .STP instead of a model
        else if (std::strcmp(argv[i], "--drive") == 0)
            driveMode = true; // chase camera riding the decoded centre line
        else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc)
            driveDistance = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--race") == 0 && i + 2 < argc) {
            raceSet = std::atoi(argv[++i]);
            raceId = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--music") == 0 && i + 1 < argc)
            musicTrack = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--tex") == 0)
            useTextures = true; // texture the roadside props - see the note at the prop atlas load
        else if (std::strcmp(argv[i], "--texcheck-mutate") == 0)
            texCheckMutate = true; // the negative control: every prop pixel must then MISMATCH
        else if (std::strcmp(argv[i], "--texcheck") == 0 && i + 1 < argc)
            texCheckPath = argv[++i]; // check the drawn props against the palettes, write a report
        else if (std::strcmp(argv[i], "--cellcheck-mutate") == 0)
            cellCheckMutate = 1; // negative control: expect the NEXT palette row of the right page
        else if (std::strcmp(argv[i], "--cellcheck-mutate-page") == 0)
            cellCheckMutate = 2; // negative control: expect the right row of a DIFFERENT page
        else if (std::strcmp(argv[i], "--cellcheck") == 0 && i + 1 < argc)
            cellCheckPath = argv[++i]; // check the drawn scene cells against their page's palettes
        else if (std::strcmp(argv[i], "--bikecheck-mutate") == 0)
            bikeCheckMutate = 1; // negative control: expect the NEXT KNBP palette block
        else if (std::strcmp(argv[i], "--bikecheck-mutate-page") == 0)
            bikeCheckMutate = 2; // negative control: send every run to the OTHER sheet
        else if (std::strcmp(argv[i], "--no-sky") == 0)
            drawSky = false; // the type-4 panorama behind the world, on by default in race mode
        else if (std::strcmp(argv[i], "--skydump") == 0 && i + 1 < argc)
            skyDumpPath = argv[++i]; // write the decoded 1760x128 panorama out as a PNG
        else if (std::strcmp(argv[i], "--skycheck-mutate") == 0)
            skyCheckMutate = 1; // negative control: expect the texel one COLUMN to the right
        else if (std::strcmp(argv[i], "--skycheck-mutate-band") == 0)
            skyCheckMutate = 2; // negative control: expect the texel one BAND up
        else if (std::strcmp(argv[i], "--skycheck") == 0 && i + 1 < argc)
            skyCheckPath = argv[++i]; // check every visible backdrop pixel against the decoded image
        else if (std::strcmp(argv[i], "--no-skygrad") == 0)
            drawSkyGradient = false; // leave the sky above the skyline black
        else if (std::strcmp(argv[i], "--skygrad") == 0 && i + 1 < argc) {
            skyGradientBundle = static_cast<size_t>(std::atoi(argv[++i]));
            skyGradientExplicit = true;
        }
        else if (std::strcmp(argv[i], "--sunangle") == 0 && i + 1 < argc)
            sunAngle = static_cast<int32_t>(std::atoi(argv[++i])), sunAngleExplicit = true;
        else if (std::strcmp(argv[i], "--skygradcheck-mutate") == 0)
            skyGradCheckMutate = true; // negative control: predict the NEXT bundle's colours
        else if (std::strcmp(argv[i], "--skygradcheck") == 0 && i + 1 < argc)
            skyGradCheckPath = argv[++i]; // check the sky gradient against the disc's own colours
        else if (std::strcmp(argv[i], "--pose") == 0 && i + 1 < argc)
            posePath = argv[++i]; // a guest RAM image to read the live part matrices out of
        else if (std::strcmp(argv[i], "--posecheck-mutate") == 0)
            poseCheckMutate = true; // negative control: the XYZ Euler order instead of ZYX
        else if (std::strcmp(argv[i], "--posecheck") == 0 && i + 1 < argc)
            poseCheckPath = argv[++i]; // check the captured matrices against RotMatrix
        else if (std::strcmp(argv[i], "--chase") == 0 && i + 1 < argc)
            chaseDistance = std::strtof(argv[++i], nullptr); // pull the chase camera in behind the bike
        else if (std::strcmp(argv[i], "--bikecheck") == 0 && i + 1 < argc)
            bikeCheckPath = argv[++i]; // check the player's bike and rider against their sheets
        else if (std::strcmp(argv[i], "--texprobe") == 0)
            texProbe = true; // print what the shader WOULD sample, per prop group, on the CPU
        else if (std::strcmp(argv[i], "--textest") == 0)
            textureTest = true; // the indexed-texture path alone, on a screen-filling quad
        else if (std::strcmp(argv[i], "--ribbon") == 0)
            drawRibbon = true; // the synthetic road ribbon, off by default: see the draw site
        else if (std::strcmp(argv[i], "--no-ribbon") == 0)
            drawRibbon = false; // kept so older scripts and reports still run
        else if (std::strcmp(argv[i], "--no-cells") == 0)
            noCells = true; // road alone, to tell a road problem from a roadside-world problem
        else if (std::strcmp(argv[i], "--band1") == 0)
            bandMode = 1; // draw the B groups from region 7 instead of region 6
        else if (std::strcmp(argv[i], "--only-band1") == 0)
            bandMode = 2; // diagnostic: the region-7 groups on their own
        else if (std::strcmp(argv[i], "--quad-strip") == 0)
            stripQuads = true; // the old, wrong quad split, for the before/after measurement
        else if (std::strcmp(argv[i], "--band2") == 0)
            wantBand2 = true; // also draw band 2, whose texture reference no fix-up pass resolves
        else if (std::strcmp(argv[i], "--budget") == 0 && i + 1 < argc)
            chunkBudget = static_cast<size_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc)
            statePath = argv[++i];
        else if (std::strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
            frameWidth = std::atoi(argv[++i]);
            frameHeight = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--legacy") == 0)
            legacy = true;
        else if (std::strcmp(argv[i], "--roadcheck") == 0 && i + 1 < argc)
            roadCheckPath = argv[++i];
        else if (std::strcmp(argv[i], "--orig-prims") == 0 && i + 1 < argc)
            origPrimsPath = argv[++i];
        else if (std::strcmp(argv[i], "--roadcheck-mutate-kind") == 0)
            roadCheckMutate = 1;
        else if (std::strcmp(argv[i], "--roadcheck-mutate-palette") == 0)
            roadCheckMutate = 2;
        else if (std::strcmp(argv[i], "--roadcheck-mutate-depth") == 0)
            roadCheckMutate = 3;
        else if (std::strcmp(argv[i], "--roadcheck-mutate-window") == 0)
            roadCheckMutate = 4;
        else if (std::strcmp(argv[i], "--skypacketcheck") == 0 && i + 1 < argc)
            skyPacketCheckPath = argv[++i];
        else if (std::strcmp(argv[i], "--shadowcheck") == 0 && i + 1 < argc)
            shadowCheckPath = argv[++i];
        else if (std::strcmp(argv[i], "--ridercheck") == 0 && i + 1 < argc)
            riderCheckPath = argv[++i];
        else if (std::strcmp(argv[i], "--propcheck") == 0 && i + 1 < argc)
            propCheckPath = argv[++i];
        else if (std::strcmp(argv[i], "--propcheck-mutate") == 0 && i + 1 < argc) {
            propCheckMutate = argv[++i];
            if (propCheckMutate != "cull" && propCheckMutate != "scale" && propCheckMutate != "mirror" &&
                propCheckMutate != "uv") {
                std::fprintf(stderr, "--propcheck-mutate takes cull, scale, mirror or uv\n");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--hazard") == 0 && i + 1 < argc)
            hazardSet = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--shadowcheck-mutate") == 0)
            skyPacketMutate = 5; // RaceScene::SetSkyMutation 5: the light straight down
        else if (std::strcmp(argv[i], "--skypacketcheck-mutate") == 0 && i + 1 < argc) {
            const std::string m = argv[++i];
            skyPacketMutate = m == "rotate" ? 1 : m == "offs" ? 2 : m == "yaw" ? 3 : m == "slice" ? 4 : -1;
            if (skyPacketMutate < 0) {
                std::fprintf(stderr, "--skypacketcheck-mutate takes rotate, offs, yaw or slice\n");
                return 2;
            }
        }
        else
            positional.push_back(argv[i]);
    }
    // --state: a captured console state. Everything the frame needs is read out of its guest RAM at
    // run time - the view record, the player's bike, the part matrices, the resident cell list - so
    // that the frame drawn is the ORIGINAL's frame for that state, not one we placed.
    std::vector<uint8_t> stateRam;
    if (!statePath.empty()) {
        const std::string ramPath = statePath + "/ram.bin";
        if (FILE* in = std::fopen(ramPath.c_str(), "rb")) {
            stateRam.resize(0x200000);
            if (std::fread(stateRam.data(), 1, stateRam.size(), in) != stateRam.size()) stateRam.clear();
            std::fclose(in);
        }
        if (stateRam.empty()) {
            std::fprintf(stderr, "state: %s is not a 2 MB guest RAM image\n", ramPath.c_str());
            return 1;
        }
        if (posePath.empty()) posePath = ramPath;
        // The blend's reference yaw is the level's sun yaw at 0x80052388: the
        // capture's own value unless one is named.
        if (!sunAngleExplicit)
            sunAngle = static_cast<int16_t>(stateRam[0x52388] | (stateRam[0x52389] << 8));
        driveMode = true;
        g_spin = false;
    }
    // --race names the content itself, so it needs no path on the disc.
    if (positional.empty() || (positional.size() < 2 && raceSet == 0)) return Usage();
    // Which surface the reporting passes are about. Only one subject at a time: the other one is
    // then drawn with uDebug = 4, same coverage, blue 0.
    const bool cellCheck = !cellCheckPath.empty();
    const bool bikeCheck = !bikeCheckPath.empty();
    const bool skyCheck = !skyCheckPath.empty();
    const bool skyGradCheck = !skyGradCheckPath.empty();
    if (static_cast<int>(cellCheck) + static_cast<int>(bikeCheck) + static_cast<int>(skyCheck) +
            static_cast<int>(skyGradCheck) + static_cast<int>(!texCheckPath.empty()) >
        1) {
        std::fprintf(stderr,
                     "--texcheck, --cellcheck, --bikecheck, --skycheck and --skygradcheck report "
                     "different subjects; run them separately\n");
        return 2;
    }
    try {
        rr::DiscImage disc(positional[0]);
        std::optional<rr::DiscFile> found;
        if (positional.size() >= 2) {
            found = disc.Find(positional[1]);
            if (!found) {
                std::fprintf(stderr, "not on disc: %s\n", positional[1].c_str());
                return 1;
            }
        }
        char title[256];
        rr::TriangleSoup roadSoup;
        std::vector<rr::RoadSlice> roadPath;
        std::vector<rr::CellData> raceCells;
        std::vector<rr::RouteLeg> raceLegs; // the same stream ranges the world loader scanned
        std::vector<uint16_t> pathRoad, pathRoadDistance;
        if (raceSet > 0) {
            // The whole route: every road of the race, in order, streamed out of STREAM<set>.STR.
            const rr::RaceWorld race = rr::LoadRaceWorld(disc, raceSet, raceId, chunkBudget);
            roadPath = race.path;
            pathRoad = race.pathRoad;
            pathRoadDistance = race.pathRoadDistance;
            roadSoup = BuildRibbon(roadPath, 9.766f * 2.0f);
            raceCells = race.cells;
            raceLegs = race.legs;
            const float length = roadPath.empty() ? 0.0f
                                                  : static_cast<float>(roadPath.back().distance) / 65536.0f;
            std::printf("set %d race %d: %zu legs, %zu road chunks, %zu cells, %zu slices, route %.0f world units, "
                        "%zu chunks scanned\n",
                        raceSet, raceId, race.legs.size(), race.roadChunksRead, race.cells.size(), roadPath.size(),
                        length, race.chunksScanned);
            if (roadSoup.vertices.empty()) {
                std::fprintf(stderr, "no road geometry for that race\n");
                return 1;
            }
            // Bands 1 and 2 of a type-8 cell ship in a chunk of their own, under the same resource
            // id (scene_cell.md 3.2). `LoadRaceWorld` reads the cells and passes the type-9 chunks
            // by, so the same stream ranges are walked again for them here. Nothing is guessed: the
            // pairing is by resource id, exactly as `SLUS_010.53 0x80032B7C` does it.
            // render7: the per-group level of detail needs region 7 of every cell, so the join runs
            // unless the frame is the legacy one.
            if (bandMode != 0 || !legacy) {
                size_t joined = 0, wanted = 0, failed = 0;
                for (const rr::CellData& cell : raceCells)
                    if (!cell.region7Present) ++wanted;
                const size_t scanned = ScanRaceStream(
                    disc, raceSet, raceLegs, [](uint8_t type) { return type == 9; },
                    [&](uint32_t, const std::vector<uint8_t>& chunk) {
                        const rr::ChunkHeader header = rr::ParseChunkHeader(chunk);
                        for (rr::CellData& cell : raceCells) {
                            if (cell.region7Present || cell.header.id != header.id) continue;
                            try {
                                rr::AttachCellRegion7(cell, chunk);
                                ++joined;
                            } catch (const std::exception&) {
                                ++failed;
                            }
                        }
                    });
                size_t withFine = 0, fineTris = 0, fineQuads = 0;
                for (const rr::CellData& cell : raceCells)
                    if (cell.region7Present) {
                        ++withFine;
                        for (const rr::CellPrimitive& prim : cell.band1) (prim.quad ? fineQuads : fineTris)++;
                    }
                std::printf("region 7: %zu of %zu cells had it in their own chunk, %zu joined from a type-9 "
                            "chunk (%zu failed), %zu chunks scanned; %zu cells can draw fine, band 1 holds "
                            "%zu triangles and %zu quads\n",
                            raceCells.size() - wanted, raceCells.size(), joined, failed, scanned, withFine,
                            fineTris, fineQuads);
            }
            std::snprintf(title, sizeof(title), "rrview - set %d race %d", raceSet, raceId);
            roadMode = true;
        } else if (roadMode) {
            size_t roadChunks = 0, sliceCount = 0;
            roadSoup = BuildRoadSurface(disc, *found, roadChunks, sliceCount, &roadPath);
            std::printf("%s: %zu road chunk(s), %zu slices, %zu triangles\n", found->path.c_str(), roadChunks,
                        sliceCount, roadSoup.vertices.size() / 3);
            if (roadSoup.vertices.empty()) {
                std::fprintf(stderr, "no road geometry in this file\n");
                return 1;
            }
            std::snprintf(title, sizeof(title), "rrview - road of %s", found->path.c_str());
        }

        const std::vector<rr::Model> models = roadMode ? std::vector<rr::Model>{}
                                                       : rr::ParseGeo(disc.ReadFile(*found));
        const size_t objectIndex = positional.size() > 2 ? static_cast<size_t>(std::stoul(positional[2])) : 0;
        if (!roadMode && objectIndex >= models.size()) {
            std::fprintf(stderr, "file has %zu object(s)\n", models.size());
            return 1;
        }
        rr::TriangleSoup soup;
        if (roadMode) {
            soup = std::move(roadSoup);
        } else {
            const rr::Model& model = models[objectIndex];
            const size_t groupIndex = positional.size() > 3 ? static_cast<size_t>(std::stoul(positional[3])) : 0;
            if (groupIndex >= model.groups.size()) {
                std::fprintf(stderr, "object %zu has %zu group(s)\n", objectIndex, model.groups.size());
                return 1;
            }
            const rr::ModelGroup& group = model.groups[groupIndex];

            // Stand the model up: the part topology lives in the race overlay on the player's own disc.
            rr::Assembly assembly;
            if (rawParts) {
                soup = rr::BuildTriangleSoup(group);
            } else {
                const auto overlay = disc.Find("RASHCDG.BIN");
                if (!overlay) {
                    std::fprintf(stderr, "RASHCDG.BIN is not on this disc; use --raw to draw unassembled parts\n");
                    return 1;
                }
                const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
                assembly = rr::AssembleGroup(group, skeleton);
                soup = rr::BuildAssembledTriangleSoup(group, assembly);
            }
            std::printf("%s object %zu (id %u) group %zu: %zu verts, %zu triangles, radius %d, lod factor %d\n",
                        found->path.c_str(), objectIndex, model.id, groupIndex, group.verts.size(),
                        soup.vertices.size() / 3, group.bbox.radius, rr::LodFactor(group));
            if (!rawParts && group.subMeshes.size() > 1)
                std::printf("assembly: %s, program %zu, bounding box off by %d units\n",
                            assembly.assembled ? "fitted" : "NO PROGRAM FITS", assembly.programIndex,
                            assembly.boxError);
            std::snprintf(title, sizeof(title), "rrview - %s object %zu group %zu (model id %u)", found->path.c_str(),
                          objectIndex, groupIndex, model.id);
        }
        // Music, if asked for. Kept alive for the lifetime of the window.
        rr::audio::Mixer mixer(44100);
        MixerSink sink(mixer);
        std::unique_ptr<rr::platform::AudioDevice> audioDevice;
        if (musicTrack >= 0) {
            double seconds = 0.0;
            auto source = LoadMusicTrack(disc, static_cast<size_t>(musicTrack), 64, seconds);
            if (source) {
                rr::audio::VoiceDesc voice;
                voice.source = source;
                voice.loop = true;
                mixer.Play(voice);
                audioDevice = rr::platform::OpenAudioDevice(sink, mixer.OutputRate());
                audioDevice->Start();
                std::printf("music: album track %d, %.1f s buffered, device %s at %d Hz\n", musicTrack, seconds,
                            audioDevice->BackendName(), audioDevice->SampleRate());
            } else {
                std::fprintf(stderr, "music: album track %d could not be loaded\n", musicTrack);
            }
        }

        g_driveKeys = driveMode;
        rr::render::g_window.onKeyDown = ViewerKey;
        Window window = CreateGlWindow(frameWidth, frameHeight, title, "rrview", "RRVIEW_FOCUS");

        // The scene the game draws too. `rrview` owns one extra buffer of its own - the single
        // model, or the synthetic road ribbon - and draws it through the same program.
        RaceScene scene;
        scene.CreatePrograms();
        scene.SetSkyMutation(skyPacketMutate);
        const GLuint program = scene.Program();
        GLuint vao = 0, vbo = 0;
        gl.GenVertexArrays(1, &vao);
        gl.BindVertexArray(vao);
        gl.GenBuffers(1, &vbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                      soup.vertices.data(), GL_STATIC_DRAW);
        const GLsizei stride = static_cast<GLsizei>(sizeof(rr::TriangleSoup::Vertex));
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 3));
        gl.EnableVertexAttribArray(1);
        const GLint viewProjLocation = gl.GetUniformLocation(program, "uViewProj");
        const GLint modelLocation = gl.GetUniformLocation(program, "uModel");
        const GLint tintLocation = gl.GetUniformLocation(program, "uTint");
        const GLint texturedLocation = gl.GetUniformLocation(program, "uTextured");
        const GLint indexLocation = gl.GetUniformLocation(program, "uIndex");
        const GLint paletteLocation = gl.GetUniformLocation(program, "uPalette");
        const GLint texSizeLocation = gl.GetUniformLocation(program, "uTexSize");
        const GLint paletteCountLocation = gl.GetUniformLocation(program, "uPaletteCount");
        const GLint paletteSizeLocation = gl.GetUniformLocation(program, "uPaletteSize");
        // A uniform the shader stops *using* is optimised away and its location silently becomes
        // -1, which then makes every glUniform on it a no-op rather than an error. Worth knowing
        // about before cutting the shader down to bisect something.

        // --textest: the indexed-texture path on its own - one screen-filling quad, identity
        // matrices, no models and no scene. Isolates the GL side from everything else.
        GLuint testVao = 0, testVbo = 0;
        GpuIndexedTexture testTexture;
        if (textureTest) {
            const auto propTexFile = disc.Find("DATA/HAZARD0.TEX");
            if (!propTexFile) {
                std::fprintf(stderr, "HAZARD0.TEX is not on this disc\n");
                return 1;
            }
            const rr::IndexedTexture atlas = rr::BuildPropAtlas(disc.ReadFile(*propTexFile));
            testTexture = UploadIndexedTexture(atlas);
            std::printf("texture test: %dx%d, %d palettes of %d\n", atlas.width, atlas.height, atlas.paletteCount,
                        atlas.paletteSize);

            // Two triangles covering clip space, carrying the whole atlas as texel coordinates.
            rr::TriangleSoup quad;
            const float w = static_cast<float>(atlas.width), h = static_cast<float>(atlas.height);
            const float corners[6][4] = {{-1, -1, 0, h}, {1, -1, w, h}, {-1, 1, 0, 0},
                                         {1, -1, w, h},  {1, 1, w, 0},  {-1, 1, 0, 0}};
            for (const auto& corner : corners) {
                rr::TriangleSoup::Vertex v;
                v.x = corner[0];
                v.y = corner[1];
                v.z = 0.0f;
                v.ny = -1.0f;
                v.u = corner[2];
                v.v = corner[3];
                v.tpage = 0;
                quad.vertices.push_back(v);
            }
            gl.GenVertexArrays(1, &testVao);
            gl.BindVertexArray(testVao);
            gl.GenBuffers(1, &testVbo);
            gl.BindBuffer(GL_ARRAY_BUFFER, testVbo);
            gl.BufferData(GL_ARRAY_BUFFER,
                          static_cast<GLsizeiptr>(quad.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                          quad.vertices.data(), GL_STATIC_DRAW);
            const GLsizei testStride = static_cast<GLsizei>(sizeof(rr::TriangleSoup::Vertex));
            gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, testStride, reinterpret_cast<void*>(0));
            gl.EnableVertexAttribArray(0);
            gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, testStride,
                                   reinterpret_cast<void*>(sizeof(float) * 3));
            gl.EnableVertexAttribArray(1);
            gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, testStride,
                                   reinterpret_cast<void*>(sizeof(float) * 6));
            gl.EnableVertexAttribArray(2);
            gl.VertexAttribPointer(3, 1, GL_UNSIGNED_SHORT, GL_FALSE, testStride,
                                   reinterpret_cast<void*>(sizeof(float) * 8 + sizeof(uint16_t)));
            gl.EnableVertexAttribArray(3);
        }

        // ------------------------------------------------------------------ the shared scene
        // Everything from here to the machine is loaded into `RaceScene`, which is the renderer
        // `rrgame` draws with too. The evidence for each surface is cited at its loader in
        // `src\render\race_scene.cpp`.
        if (roadMode && !noCells) {
            size_t cellCount = raceCells.size();
            std::vector<CellRange> cellRanges;
            // render7: the level's look first (the soup bakes every vertex's shade and texture
            // window from it), then one run per (cell, group, page) so the original's per-group level
            // of detail can choose every frame. `--legacy` and the band diagnostics keep the old soup.
            const bool perGroup = raceSet > 0 && !legacy && bandMode == 0;
            if (perGroup) {
                scene.LoadLevel(disc, raceSet, raceId);
                scene.SetCellData(&raceCells, raceSet);
                scene.MutableLook().band2Mutate = roadCheckMutate <= 3 ? roadCheckMutate : 0;
            }
            const rr::TriangleSoup cellSoup =
                raceSet > 0 ? (perGroup ? rr::render::BuildCellSoup(raceCells, 3, false, stripQuads, &cellRanges,
                                                                    &scene.Look())
                                        : rr::render::BuildCellSoup(raceCells, bandMode, wantBand2, stripQuads,
                                                                    &cellRanges))
                            : rr::render::BuildSceneCells(disc, *found, cellCount);
            const size_t runs = cellRanges.size();
            scene.SetCells(cellSoup, std::move(cellRanges));
            if (!cellSoup.vertices.empty())
                std::printf("roadside: %zu scene cells, %zu vertices, %zu draw runs\n", cellCount,
                            cellSoup.vertices.size(), runs);
            if (useTextures && raceSet > 0 && runs != 0)
                scene.LoadCellTextures(disc, raceSet, raceLegs, raceCells, bandMode != 0 || !legacy);
        }

        if (roadMode && raceSet > 0 && drawSky) scene.LoadSky(disc, raceSet, raceLegs);
        if (!skyDumpPath.empty() && scene.SkyHeaders().size() > 0) {
            // The decoded panorama as a picture, so the C++ decoder can be compared with the
            // Python probe `tools\scout\sky.py` pixel for pixel.
            scene.BindSky(0);
            if (scene.SkyBound() != RaceScene::kNoSky) {
                rr::WritePng(skyDumpPath, rr::Panorama::kWidth, rr::Panorama::kHeight,
                             scene.SkyImage().rgba);
                std::printf("wrote %s (%dx%d)\n", skyDumpPath.c_str(), rr::Panorama::kWidth,
                            rr::Panorama::kHeight);
            }
        }
        // The sky gradient is section 1 of the race's own level bundle (level_bundle.h: RASHCDI
        // 0x80061A80 takes entry raceId - 1); --skygrad still overrides it.
        if (!skyGradientExplicit && raceSet > 0 && !legacy) skyGradientBundle = rr::LevelBundleIndexForRace(raceId);
        if (roadMode && drawSkyGradient) scene.LoadSkyGradient(disc, skyGradientBundle);
        if (roadMode && !noCells) {
            // Which HAZARD<n> the capture loaded: the one whose group 0 (the only group that differs
            // between the six files, rmd3.md 349) the registry's model 200 holds (slot table 0x800CE1B0).
            if (hazardSet < 0 && !stateRam.empty()) {
                const auto r32 = [&](uint32_t a) {
                    uint32_t v = 0;
                    std::memcpy(&v, stateRam.data() + (a & 0x1FFFFFu), 4);
                    return v;
                };
                for (uint32_t k = 0; k < 50 && hazardSet < 0; ++k) {
                    const uint32_t reg = 0x800CE1B0u + 16u * k;
                    if (r32(reg) != 200u) continue;
                    const uint32_t dod = r32(r32(reg + 8u));      // group 0's DOD3
                    const uint32_t verts = r32(dod + 0x24u);       // relocated vertex array: count, then 8 each
                    const uint32_t count = r32(verts);
                    for (int n = 0; n < 6 && hazardSet < 0; ++n) {
                        const auto f = disc.Find("DATA/HAZARD" + std::to_string(n) + ".GEO");
                        if (!f) continue;
                        const std::vector<rr::Model> hazModels = rr::ParseGeo(disc.ReadFile(*f));
                        const rr::ModelGroup& g0 = hazModels.front().groups.front();
                        if (g0.verts.size() != count) continue;
                        bool same = true;
                        for (size_t v = 0; v < g0.verts.size() && same; ++v) {
                            int16_t xyz[3];
                            std::memcpy(xyz, stateRam.data() + ((verts + 4u + 8u * v) & 0x1FFFFFu), 6);
                            same = xyz[0] == g0.verts[v].x && xyz[1] == g0.verts[v].y && xyz[2] == g0.verts[v].z;
                        }
                        if (same) hazardSet = n;
                    }
                }
                std::printf("state: model 200 is DATA\\HAZARD%d.GEO\n", hazardSet);
            }
            scene.LoadProps(disc, raceCells, useTextures, texProbe, hazardSet < 0 ? 0 : hazardSet);
        }

        // ------------------------------------------------------------------- the measured pose
        // `--pose <ram.bin>` replaces the rest pose and the guessed rider orientation with the
        // live part matrices of a captured state. Guest addresses, from rmd3.md 10.1:
        // the player object at 0x801B65D4 has `obj+0x04 = 0x801BDF1C` (5 slots of 24 bytes) and
        // its child at 0x801BB2EC has `obj+0x04 = 0x801BDF9C` (17 slots). Nothing of that image
        // is stored here; it is read at run time exactly like the overlay's attachment table.
        constexpr uint32_t kPlayerPartsAddress = 0x801BDF1Cu;
        constexpr uint32_t kRiderPartsAddress = 0x801BDF9Cu;
        std::vector<uint8_t> poseRam;
        std::vector<rr::PartSlot> bikeSlots, riderSlots;
        if (!posePath.empty()) {
            if (FILE* in = std::fopen(posePath.c_str(), "rb")) {
                std::fseek(in, 0, SEEK_END);
                const long size = std::ftell(in);
                std::fseek(in, 0, SEEK_SET);
                poseRam.resize(size > 0 ? static_cast<size_t>(size) : 0);
                if (!poseRam.empty() && std::fread(poseRam.data(), 1, poseRam.size(), in) != poseRam.size())
                    poseRam.clear();
                std::fclose(in);
            }
            if (poseRam.size() < 0x200000) {
                std::fprintf(stderr, "pose: %s is not a 2 MB guest RAM image\n", posePath.c_str());
                return 1;
            }
            bikeSlots = rr::ReadPartSlots(poseRam, kPlayerPartsAddress, 5);
            riderSlots = rr::ReadPartSlots(poseRam, kRiderPartsAddress, 17);
            size_t rotations = 0;
            for (const rr::PartSlot& slot : bikeSlots) rotations += rr::IsRotation(slot.rot) ? 1 : 0;
            for (const rr::PartSlot& slot : riderSlots) rotations += rr::IsRotation(slot.rot) ? 1 : 0;
            std::printf("pose: %s, %zu of 22 slots read as rotations\n", posePath.c_str(), rotations);
            if (rotations != 22) {
                std::fprintf(stderr, "pose: the slots at 0x%08X / 0x%08X are not rotation matrices in "
                                     "this image\n",
                             kPlayerPartsAddress, kRiderPartsAddress);
                return 1;
            }
        }

        // `--posecheck`: the captured matrices against the construction they are supposed to be.
        // Every live part 3x3 must be `RotMatrix(vx, vy, vz)` of an INTEGER Euler triple in the
        // PS1 unit of 4096 per turn, in the psyq order M = Rz * Ry * Rx, built from the game's own
        // (sin, cos) table. The triple is recovered by the closed-form inverse and refined over a
        // +-3 neighbourhood, then the matrix is rebuilt and compared element by element - so this
        // tests the CONSTRUCTION, not the recovery: a wrong axis order or a wrong table cannot be
        // absorbed by a three-integer search.
        if (!poseCheckPath.empty()) {
            if (poseRam.empty()) {
                std::fprintf(stderr, "posecheck: needs --pose <ram.bin>\n");
                return 1;
            }
            const auto exeFile = disc.Find("SLUS_010.53");
            if (!exeFile) {
                std::fprintf(stderr, "posecheck: SLUS_010.53 is not on this disc\n");
                return 1;
            }
            const rr::SineTable sine = rr::SineTable::FromExe(disc.ReadFile(*exeFile));
            constexpr int kTolerance = 3;
            std::string report;
            report += "rrview --posecheck: the live per-part 3x3 of the player's bike and rider\n";
            report += "against RotMatrix of an integer Euler triple, 4096 units per turn, built\n";
            report += "from the game's own sine table at guest 0x8005624C.\n";
            report += "Tolerance: 3 of 4096 per element. One angle unit moves an element by up to\n";
            report += "2*pi*4096/4096 = 6.3, so the tolerance is tighter than the smallest error a\n";
            report += "wrong angle can produce; it only absorbs the sine table's own quantisation\n";
            report += "and the order of the two 12-bit shifts in the product.\n\n";
            size_t compared = 0, matching = 0, differing = 0, moved = 0, movedMatching = 0;
            int worst = 0;
            const auto run = [&](const char* what, uint32_t address, const std::vector<rr::PartSlot>& slots) {
                for (size_t i = 0; i < slots.size(); ++i) {
                    int angles[3] = {0, 0, 0};
                    rr::DecomposeZYX(sine, slots[i].rot, angles);
                    rr::PartMatrix built = rr::RotMatrixZYX(sine, angles[0], angles[1], angles[2]);
                    rr::PartMatrix control = built;
                    if (poseCheckMutate) {
                        // The competing hypothesis, and the one a renderer would reach for first:
                        // the same three angles applied in the opposite order, M = Rx * Ry * Rz.
                        const rr::PartMatrix rx = rr::RotMatrixZYX(sine, angles[0], 0, 0);
                        const rr::PartMatrix ry = rr::RotMatrixZYX(sine, 0, angles[1], 0);
                        const rr::PartMatrix rz = rr::RotMatrixZYX(sine, 0, 0, angles[2]);
                        control = rr::Multiply3x3(rr::Multiply3x3(rx, ry), rz);
                    }
                    int error = 0, changed = 0;
                    for (int k = 0; k < 9; ++k) {
                        error = std::max(error, std::abs(static_cast<int>(control.m[k]) -
                                                         static_cast<int>(slots[i].rot.m[k])));
                        changed = std::max(changed, std::abs(static_cast<int>(control.m[k]) -
                                                             static_cast<int>(built.m[k])));
                    }
                    ++compared;
                    worst = std::max(worst, error);
                    const bool ok = error <= kTolerance;
                    if (ok) ++matching; else ++differing;
                    if (poseCheckMutate && changed > kTolerance) {
                        ++moved;
                        if (ok) ++movedMatching;
                    }
                    char line[256];
                    std::snprintf(line, sizeof(line),
                                  "%s part %2zu at 0x%08X  angles (%5d,%5d,%5d)  worst element %3d  %s%s\n",
                                  what, i, address + static_cast<uint32_t>(i) * 24,
                                  angles[0] > 2048 ? angles[0] - 4096 : angles[0],
                                  angles[1] > 2048 ? angles[1] - 4096 : angles[1],
                                  angles[2] > 2048 ? angles[2] - 4096 : angles[2], error,
                                  ok ? "MATCH" : "DIFFER",
                                  (poseCheckMutate && changed <= kTolerance)
                                      ? "  (the mutation changed nothing here)"
                                      : "");
                    report += line;
                }
            };
            run("bike ", kPlayerPartsAddress, bikeSlots);
            run("rider", kRiderPartsAddress, riderSlots);
            char summary[640];
            std::snprintf(summary, sizeof(summary),
                          "\nmatrices compared %zu, matching %zu, differing %zu, worst element %d of 4096\n"
                          "matrices the mutation did not move at all %zu, of the rest still matching %zu\n"
                          "verdict %s\n",
                          compared, matching, differing, worst,
                          poseCheckMutate ? compared - moved : 0, poseCheckMutate ? movedMatching : 0,
                          poseCheckMutate
                              ? (movedMatching == 0
                                     ? "PASS (negative control: no matrix the mutation moved still matched)"
                                     : "FAIL (negative control: a moved matrix still matched)")
                              : (differing == 0 && compared == 22 ? "PASS" : "FAIL"));
            report += summary;
            if (FILE* out = std::fopen(poseCheckPath.c_str(), "wb")) {
                std::fwrite(report.data(), 1, report.size(), out);
                std::fclose(out);
            }
            std::fputs(summary, stdout);
            std::printf("posecheck report: %s\n", poseCheckPath.c_str());
        }

        // How the rider object sits on the bike object, as a 3x3 in the MODEL frame: row-major,
        // mapping rider model coordinates into bike model coordinates. Without `--pose` the scene
        // keeps the axis permutation rrview has been guessing (rider +X is up, +Z is lateral), the
        // only assignment that puts the head above the tank. With `--pose` it is measured:
        // `transpose(bike slot 0) * rider slot 0`, both of which are the objects' own world
        // orientations in the captured frame, so their product is the rider's orientation relative
        // to the bike and nothing else.
        if (!bikeSlots.empty() && !riderSlots.empty()) {
            const rr::PartMatrix relative =
                rr::Multiply3x3(rr::Transpose3x3(bikeSlots[0].rot), riderSlots[0].rot);
            float measured[9];
            for (int k = 0; k < 9; ++k) measured[k] = static_cast<float>(relative.m[k]) / 4096.0f;
            scene.SetRiderRelative(measured);
            std::printf("rider orientation measured from the capture: [%6.3f %6.3f %6.3f | %6.3f %6.3f "
                        "%6.3f | %6.3f %6.3f %6.3f]\n",
                        measured[0], measured[1], measured[2], measured[3], measured[4], measured[5],
                        measured[6], measured[7], measured[8]);
        }
        if (driveMode) scene.LoadMachine(disc, useTextures, bikeSlots, riderSlots);

        // ------------------------------------------------------------------- the captured frame
        // Guest addresses: view record 0 at 0x800CD898 -
        // eye +0xB8 (16.16 world), render frame rows +0x1B0 (lateral), +0x1B6 (the screen's DOWN
        // axis: the GTE puts SY = OFY + H * row1.d / z), +0x1BC (forward); the player's bike at
        // 0x801B65D4 - box centre +0xB8, ground rows +0x204 / +0x20A / +0x210 (RaceSession::
        // BikePlacement reads the same); the resident cell list 0x800D9B80 with its count at
        // gp+2268 (`SLUS 0x800358C0` hands both to the level-of-detail pass `0x80035680`).
        struct StateView {
            float eye[3] = {}, right[3] = {}, down[3] = {}, forward[3] = {};
            float bike[3] = {}, bikeLateral[3] = {}, bikeNormal[3] = {}, bikeTangent[3] = {};
            int32_t bikeHalfUp = 0;
            std::vector<size_t> cellOrder;
        } sv;
        const auto guestU32 = [&](uint32_t address) {
            const size_t at = address & 0x1FFFFFu;
            return static_cast<uint32_t>(stateRam[at]) | (static_cast<uint32_t>(stateRam[at + 1]) << 8) |
                   (static_cast<uint32_t>(stateRam[at + 2]) << 16) | (static_cast<uint32_t>(stateRam[at + 3]) << 24);
        };
        const auto guestS16 = [&](uint32_t address) {
            const size_t at = address & 0x1FFFFFu;
            return static_cast<int16_t>(stateRam[at] | (stateRam[at + 1] << 8));
        };
        if (!stateRam.empty()) {
            constexpr uint32_t kView = 0x800CD898u, kBike = 0x801B65D4u, kResidentList = 0x800D9B80u;
            for (uint32_t k = 0; k < 3; ++k) {
                sv.eye[k] = static_cast<float>(static_cast<int32_t>(guestU32(kView + 0xB8 + 4 * k))) / 65536.0f;
                sv.right[k] = guestS16(kView + 0x1B0 + 2 * k) / 4096.0f;
                sv.down[k] = guestS16(kView + 0x1B6 + 2 * k) / 4096.0f;
                sv.forward[k] = guestS16(kView + 0x1BC + 2 * k) / 4096.0f;
                sv.bike[k] = static_cast<float>(static_cast<int32_t>(guestU32(kBike + 0xB8 + 4 * k))) / 65536.0f;
                sv.bikeLateral[k] = guestS16(kBike + 0x204 + 2 * k) / 4096.0f;
                sv.bikeNormal[k] = -guestS16(kBike + 0x20A + 2 * k) / 4096.0f;
                sv.bikeTangent[k] = guestS16(kBike + 0x210 + 2 * k) / 4096.0f;
            }
            sv.bikeHalfUp = static_cast<int32_t>(guestU32(kBike + 0x138));
            // gp from the capture's own register file: the count word lives at gp+2268.
            uint32_t gp = 0;
            if (FILE* in = std::fopen((statePath + "/cpu.json").c_str(), "rb")) {
                char buffer[65536];
                const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, in);
                buffer[got] = 0;
                std::fclose(in);
                if (const char* at = std::strstr(buffer, "\"gp\":")) gp = static_cast<uint32_t>(std::strtoul(at + 5, nullptr, 10));
            }
            const uint32_t count = gp != 0 ? guestU32(gp + 2268) : 0;
            size_t unknown = 0;
            std::vector<uint32_t> listedSlots; // the slots the frame drew (the eye is solved from these)
            for (uint32_t n = 0; n < count && n < 24; ++n) {
                const uint32_t slot = guestU32(kResidentList + 4 * n);
                if (slot == 0xFFFFFFFFu || slot >= 24) continue;
                listedSlots.push_back(slot);
                const uint32_t id = guestU32(0x800D87E8u + 112 * slot);
                bool listed = false;
                for (size_t c = 0; c < raceCells.size(); ++c)
                    if (raceCells[c].header.id == id) {
                        sv.cellOrder.push_back(c);
                        listed = true;
                        break;
                    }
                if (!listed) ++unknown;
            }
            // The eye the FRAME was drawn from. `SLUS 0x800353C4` gives every resident cell slot its
            // own GTE matrix (slot +0x10, the camera rows with the second scaled by 3412/4096) and
            // translation (slot +0x24, cell units) and the cell draw hands exactly those to the GTE -
            // the snapshot's own GTE registers hold slot 2's (LLM = +0x10, BK = +0x24). Solving
            // TR = M (origin - eye * 64) for the eye gives the camera of the captured frame. In
            // rr-race it is (1156.270, -15.225, 5591.278) from slots 2 and 3 alike, 0.04 world units
            // from the view record's +0xB8. Which code puts the difference there is NOT established
            // (it is not one frame of motion: the trace's next frames move the eye 0.14 units each);
            // the slots are what the frame was drawn with, so they are what is used here.
            {
                int solved = 0;
                float firstEye[3] = {};
                for (uint32_t slot = 0; slot < 24 && solved < 2; ++slot) {
                    // Only a slot on the resident list the frame walked: a slot off it keeps the matrix
                    // of the frame it was last drawn in (rr-pack's slot 0 puts the eye 1000 units away).
                    if (!listedSlots.empty() &&
                        std::find(listedSlots.begin(), listedSlots.end(), slot) == listedSlots.end())
                        continue;
                    const uint32_t base = 0x800D87E8u + 112 * slot;
                    const uint32_t body = guestU32(base + 4);
                    const int32_t tr[3] = {static_cast<int32_t>(guestU32(base + 0x24)), static_cast<int32_t>(guestU32(base + 0x28)),
                                           static_cast<int32_t>(guestU32(base + 0x2C))};
                    if (body < 0x80000000u || body >= 0x80200000u || (tr[0] == 0 && tr[1] == 0 && tr[2] == 0)) continue;
                    double m[3][3];
                    for (int r = 0; r < 3; ++r)
                        for (int k = 0; k < 3; ++k) m[r][k] = guestS16(base + 0x10 + 2 * static_cast<uint32_t>(3 * r + k)) / 4096.0;
                    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
                    if (std::fabs(det) < 1e-6) continue;
                    // Cramer's rule for d = M^-1 tr, then eye = (origin - d) / 64.
                    double d[3];
                    for (int col = 0; col < 3; ++col) {
                        double a[3][3];
                        for (int r = 0; r < 3; ++r)
                            for (int k = 0; k < 3; ++k) a[r][k] = k == col ? tr[r] : m[r][k];
                        d[col] = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                                  a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;
                    }
                    float eyeFromSlot[3];
                    for (uint32_t k = 0; k < 3; ++k)
                        eyeFromSlot[k] = static_cast<float>((static_cast<int32_t>(guestU32(body + 8 + 4 * k)) - d[k]) / 64.0);
                    std::printf("state: slot %u's GTE translation puts the frame's eye at (%.3f, %.3f, %.3f)\n", slot,
                                eyeFromSlot[0], eyeFromSlot[1], eyeFromSlot[2]);
                    if (solved == 0)
                        for (int k = 0; k < 3; ++k) firstEye[k] = eyeFromSlot[k];
                    ++solved;
                }
                if (solved > 0) {
                    std::printf("state: the view record's eye +0xB8 is (%.3f, %.3f, %.3f); drawing from the slots' eye\n",
                                sv.eye[0], sv.eye[1], sv.eye[2]);
                    for (int k = 0; k < 3; ++k) sv.eye[k] = firstEye[k];
                }
            }
            std::printf("state %s: eye (%.3f, %.3f, %.3f), bike (%.3f, %.3f, %.3f), gp 0x%08X, %u resident cells "
                        "listed, %zu found in the race's stream, %zu not\n",
                        statePath.c_str(), sv.eye[0], sv.eye[1], sv.eye[2], sv.bike[0], sv.bike[1], sv.bike[2], gp,
                        count, sv.cellOrder.size(), unknown);
        }

        // Frame the geometry we are actually about to draw: assembly and the LOD shift both move it.
        float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
        for (size_t i = 0; i < soup.vertices.size(); ++i) {
            const float p[3] = {soup.vertices[i].x, soup.vertices[i].y, soup.vertices[i].z};
            for (int k = 0; k < 3; ++k) {
                if (i == 0 || p[k] < lo[k]) lo[k] = p[k];
                if (i == 0 || p[k] > hi[k]) hi[k] = p[k];
            }
        }
        const float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
        float radius = 0.0f;
        for (int k = 0; k < 3; ++k) radius = std::max(radius, (hi[k] - lo[k]) * 0.5f);
        if (radius <= 0.0f) radius = 1000.0f;
        std::printf("extent %.0f x %.0f x %.0f units\n", hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);

        glEnable(GL_DEPTH_TEST);
        while (!rr::render::g_window.quit) {
            MSG message;
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            if (g_spin) {
                // 6.6 world units per field is a measured speed from the captured RAM dumps, but note
                // what it is a speed OF: docs\formats\population.md showed the array it came from is
                // one traffic car's bounding box, not a racer, so this is traffic pace rather than
                // racing pace. It is a placeholder either way - real speed comes with the physics.
                if (driveMode) driveDistance += g_rideSpeed;
                else g_orbit += 0.01f;
            }

            int width = 0, height = 0;
            rr::render::FrameSize(window, width, height);
            if (width <= 0 || height <= 0) continue;
            glViewport(0, 0, width, height);
            glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            float eye[3];
            float target[3];
            float up[3] = {0.0f, -1.0f, 0.0f}; // PS1 space is y-down, so "up" is negative y
            float nearZ = radius * 0.01f, farZ = radius * 40.0f;

            RoadFrame frame;
            float upSense = 1.0f;
            float ridePos[3] = {};
            uint16_t currentRoad = 0, currentRoadDistance = 0;
            bool currentRoadKnown = false;
            if (driveMode && !roadPath.empty()) {
                // Ride the centre line the game itself stores: position, tangent and surface normal
                // all come straight from the slice, nothing is reconstructed.
                size_t sampleIndex = 0;
                frame = SampleRoad(roadPath, driveDistance, &sampleIndex);
                if (sampleIndex < pathRoad.size() && sampleIndex < pathRoadDistance.size()) {
                    currentRoad = pathRoad[sampleIndex];
                    currentRoadDistance = pathRoadDistance[sampleIndex];
                    currentRoadKnown = true;
                }
                // The normal points away from the tarmac; in y-down space "up" is whichever sense has
                // the negative y component.
                upSense = frame.normal[1] <= 0.0f ? 1.0f : -1.0f;
                // The chase camera. `--chase <units>` pulls it in behind the bike so the machine
                // fills enough of the frame for a per-pixel check to have something to measure;
                // the default 95 is the wide view the roadside work used.
                const float back = chaseDistance;
                for (int k = 0; k < 3; ++k) {
                    up[k] = frame.normal[k] * upSense;
                    // Steering moves the rider across the road; the camera follows.
                    ridePos[k] = frame.pos[k] + frame.lateral[k] * g_rideLateral;
                    eye[k] = ridePos[k] + up[k] * (back * 0.337f) - frame.tangent[k] * back;
                    target[k] = ridePos[k] + up[k] * (back * 0.084f) + frame.tangent[k] * (back * 0.737f);
                }
                nearZ = 1.0f;
                farZ = 20000.0f;
            } else {
                const float dist = radius * 2.8f * g_distance;
                eye[0] = centre[0] + dist * std::cos(g_pitch) * std::sin(g_orbit);
                eye[1] = centre[1] - dist * std::sin(g_pitch);
                eye[2] = centre[2] + dist * std::cos(g_pitch) * std::cos(g_orbit);
                for (int k = 0; k < 3; ++k) target[k] = centre[k];
            }
            if (!stateRam.empty()) {
                // The captured camera, and the captured bike under it.
                for (int k = 0; k < 3; ++k) {
                    eye[k] = sv.eye[k];
                    target[k] = sv.eye[k] + sv.forward[k];
                    up[k] = -sv.down[k];
                    frame.pos[k] = sv.bike[k];
                    frame.lateral[k] = sv.bikeLateral[k];
                    frame.normal[k] = sv.bikeNormal[k];
                    frame.tangent[k] = sv.bikeTangent[k];
                }
                upSense = frame.normal[1] <= 0.0f ? 1.0f : -1.0f;
                // The panorama is chosen by the player's road and distance: the slice nearest the bike.
                size_t best = 0;
                float bestD = 1e30f;
                for (size_t n = 0; n < roadPath.size(); ++n) {
                    const float dx = rr::WorldX(roadPath[n]) - sv.bike[0], dz = rr::WorldZ(roadPath[n]) - sv.bike[2];
                    if (dx * dx + dz * dz < bestD) {
                        bestD = dx * dx + dz * dz;
                        best = n;
                    }
                }
                if (best < pathRoad.size() && best < pathRoadDistance.size()) {
                    currentRoad = pathRoad[best];
                    currentRoadDistance = pathRoadDistance[best];
                    currentRoadKnown = true;
                }
                nearZ = 0.25f;
                farZ = 20000.0f;
            }
            const Mat4 view = LookAt(eye, target, up);
            // The projection. Legacy: the 57.3 degree vertical field this renderer had before render7.
            // Otherwise the ORIGINAL's (race_scene.h OriginalVerticalFov): with --state the aspect is the
            // console's 4:3 whatever the frame size, so a 384 x 240 frame maps pixel for pixel onto
            // the original's (x = 192 + 237 X/Z, y = 120 + 237 * 3412/4096 * Y/Z); without it the
            // window's own aspect, i.e. the original's vertical field and more at the sides.
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            const Mat4 proj = legacy ? Perspective(1.0f, stateRam.empty() ? aspect : 4.0f / 3.0f, nearZ, farZ)
                                     : Perspective(rr::render::OriginalVerticalFov(),
                                                   stateRam.empty() ? aspect : 4.0f / 3.0f, nearZ, farZ);
            const Mat4 viewProj = Multiply(proj, view);
            CellView cellView;
            {
                float forward[3], right[3], length = 0.0f;
                for (int k = 0; k < 3; ++k) forward[k] = target[k] - eye[k];
                for (float f : forward) length += f * f;
                length = std::sqrt(length);
                for (float& f : forward) f = length > 0.0f ? f / length : 0.0f;
                right[0] = up[1] * forward[2] - up[2] * forward[1];
                right[1] = up[2] * forward[0] - up[0] * forward[2];
                right[2] = up[0] * forward[1] - up[1] * forward[0];
                length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
                for (int k = 0; k < 3; ++k) {
                    cellView.eye[k] = eye[k];
                    cellView.forward[k] = forward[k];
                    cellView.right[k] = length > 0.0f ? right[k] / length : 0.0f;
                    cellView.up[k] = up[k];
                }
                if (!stateRam.empty())
                    for (int k = 0; k < 3; ++k) cellView.right[k] = sv.right[k];
            }

            if (textureTest) {
                const Mat4 unit;
                gl.UseProgram(program);
                gl.UniformMatrix4fv(viewProjLocation, 1, GL_FALSE, unit.m);
                gl.UniformMatrix4fv(modelLocation, 1, GL_FALSE, unit.m);
                gl.Uniform3f(tintLocation, 1.0f, 1.0f, 1.0f);
                gl.Uniform1i(texturedLocation, 1);
                gl.ActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, testTexture.indexTexture);
                gl.Uniform1i(indexLocation, 0);
                gl.ActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, testTexture.paletteTexture);
                gl.Uniform1i(paletteLocation, 1);
                gl.ActiveTexture(GL_TEXTURE0);
                gl.Uniform2f(texSizeLocation, testTexture.width, testTexture.height);
                gl.Uniform1f(paletteCountLocation, testTexture.paletteCount);
                gl.Uniform1f(paletteSizeLocation, testTexture.paletteSize);
                gl.BindVertexArray(testVao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                if (!shotPath.empty()) {
                    glFinish();
                    SaveShot(shotPath, width, height, ReadFrame(width, height));
                    SwapBuffers(window.dc);
                    break;
                }
                SwapBuffers(window.dc);
                continue;
            }

            // ------------------------------------------------------- this frame's sky gradient
            // Where the horizon lands on screen and the two blend weights the original reads at
            // the screen edges. Both live in the shared scene, so the game gets the same sky.
            scene.PrepareSkyGradient(viewProj, eye, target, sunAngle);

            // The player's machine on the road. The placement is the viewer's - `rrgame` puts it
            // where the ported simulation says it is - but the matrices and the draw are the same
            // shared code, which is what makes `--bikecheck` a statement about the game's frame.
            // The model scale is DERIVED (race_scene.h kModelUnitsPerWorldUnit): 1/1024, with the
            // model's origin at the entity's +0xB8. --legacy keeps the viewer's old 0.0046 on the
            // road ribbon, and with --state the old game's drop to the bottom of the collision box.
            const float modelToWorld = 1.0f / rr::render::kModelUnitsPerWorldUnit;
            const bool haveMachine = driveMode && !scene.BikeParts().empty() && !roadPath.empty();
            Mat4 bikeModel;
            if (!stateRam.empty()) {
                float bikeUp[3], origin[3];
                for (int k = 0; k < 3; ++k) bikeUp[k] = frame.normal[k] * upSense;
                if (legacy) {
                    const float drop = static_cast<float>(sv.bikeHalfUp) / 65536.0f - 516.0f * modelToWorld + 1.0f;
                    for (int k = 0; k < 3; ++k) origin[k] = sv.bike[k] - bikeUp[k] * drop;
                    bikeModel = rr::render::MachineMatrix(frame, bikeUp, origin, modelToWorld);
                } else {
                    bikeModel = rr::render::MachineMatrixAt(frame, bikeUp, sv.bike, modelToWorld);
                }
            } else if (legacy) {
                bikeModel = rr::render::MachineMatrix(frame, up, ridePos, 0.0046f);
            } else {
                // On the centre line with no physics under it: OURS, the lowest model vertex (375
                // model units below the origin, BBLEVEL1.GEO id 100) put on the road.
                float origin[3];
                for (int k = 0; k < 3; ++k) origin[k] = ridePos[k] + up[k] * (375.0f * modelToWorld);
                bikeModel = rr::render::MachineMatrixAt(frame, up, origin, modelToWorld);
            }
            const Mat4 riderModel =
                rr::render::RiderMatrix(bikeModel, scene.RiderAttach(), scene.RiderRelative());

            // What the reporting passes read back out of the scene.
            const std::vector<const PropInstance*>& drawnProps = scene.drawnProps;
            const std::vector<const CellRange*>& drawnCells = scene.drawnCells;
            const std::vector<PropInstance>& props = scene.Props();
            const rr::IndexedTexture& propAtlas = scene.PropAtlas();
            const bool propTextureValid = scene.PropTextureValid();
            const std::map<uint16_t, rr::IndexedTexture>& cellAtlases = scene.CellAtlases();
            const std::vector<rr::IndexedTexture>& riderSheets = scene.RiderSheets();
            const std::vector<rr::IndexedTexture>& riderSheetsAlt = scene.RiderSheetsAlt();
            const std::vector<ModelPart>& bikeParts = scene.BikeParts();
            const rr::SkyGradient& skyGradient = scene.SkyGradientColours();
            const rr::SkyGradient& skyGradientAlt = scene.SkyGradientAlt();
            const uint8_t* gradMidLeft = scene.GradMidLeft();
            const uint8_t* gradMidRight = scene.GradMidRight();
            const rr::Panorama& skyImage = scene.SkyImage();

            // The whole frame, as one callable, so --texcheck / --cellcheck can draw the SAME frame
            // again with the subject reporting what it sampled instead of what it looks like.
            // Drawing the identical scene in the identical order is what makes the two readbacks
            // comparable: depth ties resolve the same way in every pass, so the same fragment wins
            // every time. In a reporting pass everything that is NOT the subject is drawn with
            // uDebug = 4 - unchanged geometry and unchanged discard, but blue 0 - so a reported
            // fragment can never be confused with another surface's colour.
            const auto drawScene = [&](int debugMode) {
                DrawRequest request;
                request.viewProj = viewProj;
                for (int k = 0; k < 3; ++k) request.eye[k] = eye[k];
                request.debugMode = debugMode;
                request.subject = cellCheck      ? Subject::Cells
                                  : bikeCheck    ? Subject::Bike
                                  : skyCheck     ? Subject::Sky
                                  : skyGradCheck ? Subject::Gradient
                                                 : Subject::Props;
                request.currentRoadKnown = currentRoadKnown;
                request.currentRoad = currentRoad;
                request.currentRoadDistance = currentRoadDistance;
                request.haveMachine = haveMachine;
                request.bikeModel = bikeModel;
                request.riderModel = riderModel;
                // render7: the original's per-group level of detail and the band-2 road need the
                // camera; with --state the console's own resident list and its horizontal cull too.
                request.haveCellView = !legacy;
                request.cellView = cellView;
                request.keepCulled = stateRam.empty();
                if (!stateRam.empty()) request.cellOrder = &sv.cellOrder;
                // The synthetic road ribbon: a strip we build from the centre line, with a guessed
                // half width. It is OFF by default now that the cell's band 0 carries the real,
                // textured tarmac - the two z-fight, and the ribbon wins in patches, so it hid the
                // road it was standing in for. Worse, it is itself full of holes: measured on race
                // 1/20 at 18000, the ribbon accounted for 101253 of the 101420 background pixels
                // enclosed by the skyline. Turning it off drops that to 167. Pass --ribbon to get
                // it back; --no-cells draws it regardless, because then it is the only road there is.
                if (drawRibbon || noCells)
                    request.afterCells = [&](int otherDebug) {
                        scene.SetTint(0.62f, 0.66f, 0.72f); // the road itself
                        scene.SetDebug(otherDebug);
                        gl.BindVertexArray(vao);
                        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(soup.vertices.size()));
                        scene.SetDebug(0);
                    };
                scene.Draw(request);
            }; // drawScene
            const size_t& propsDrawn = scene.propsDrawn;

            // Which backdrop is live, chosen the way `RASHCDG 0x80065580` chooses it: among the
            // registered panoramas take the one with a residency window on the driver's road whose
            // MIDPOINT is nearest the driver's distance along it. Falls back to the first one when
            // the route position is unknown, so an orbit camera still gets a backdrop.
            scene.SelectSky(currentRoadKnown, currentRoad, currentRoadDistance);
            const size_t skyBound = scene.SkyBound();
            // --propcheck-mutate: the negative controls change what the renderer draws, before the shot.
            //   cull   - the other side of every one-sided primitive (uNclip with the other sign);
            //   scale  - the model-to-world scale the props had before (0.0046 per 1/16 unit, 4.7 x ours);
            //   mirror - the matrix they had before: model X -> (-nz, 0, nx), a reflection of the console's.
            if (propCheckMutate == "cull") scene.SetPropNclip(-rr::render::kPropNclip);
            if (propCheckMutate == "scale" || propCheckMutate == "mirror")
                for (PropInstance& instance : scene.MutableProps()) {
                    if (propCheckMutate == "scale")
                        for (int c = 0; c < 3; ++c)
                            for (int k = 0; k < 3; ++k) instance.matrix[4 * c + k] *= 0.0046f * 1024.0f;
                    else
                        for (int k = 0; k < 3; ++k) instance.matrix[k] = -instance.matrix[k];
                }
            drawScene(0);
            if (!shotPath.empty())
                {
                    // Drawing nothing and drawing everything wrong look the same in a count, so say
                    // how far the nearest prop actually is: a count of 0 with the nearest prop 40
                    // units away is a draw bug, a count of 0 with it 9000 units away is a camera
                    // standing somewhere the world was never assembled.
                    double nearest = 1e30;
                    for (const PropInstance& instance : props) {
                        const double dx = instance.centre[0] - eye[0];
                        const double dy = instance.centre[1] - eye[1];
                        const double dz = instance.centre[2] - eye[2];
                        nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
                    }
                    std::printf("camera at (%.0f,%.0f,%.0f); nearest prop %.0f units, draw range %.0f (ModelVisible)\n",
                                eye[0], eye[1], eye[2], props.empty() ? -1.0 : nearest,
                                rr::render::kPropDrawRange / 64.0);
                }
                std::printf("props drawn this frame: %zu of %zu; textured %s\n", propsDrawn, props.size(),
                            propTextureValid ? "yes" : "no");

            // The back buffer is undefined after SwapBuffers, so the shot has to be taken first.
            if (!shotPath.empty()) { // one frame, then out - this is the scripted check
                glFinish();
                const std::vector<uint8_t> shot = ReadFrame(width, height);
                SaveShot(shotPath, width, height, shot);
                if (!roadCheckPath.empty()) {
                    // --roadcheck: the band-2 road of THIS frame against the ORIGINAL's packets for the
                    // same captured state (docs\formats\scene_cell.md 13.6). The packets are the ones our
                    // interpreter recorded when it ran the original from the capture
                    // (`rrverify trace`), turned into one row per polygon by `tools\scout\psxgpu.py
                    // frame --prims`. Per primitive:
                    //   * a textured road GT4 (tpage = the road page): its screen centroid must fall in
                    //     one of OUR strips projected with the original's projection; the palette row
                    //     its CLUT id names (guest 0x800D5EC8, the loader's table) must be that strip's;
                    //     at each of its four vertices our strip's UV (bilinear, found by casting the
                    //     pixel's ray into the strip) must agree within the rounding the console's
                    //     subdivision leaves (1.5 texels plus twice the UV change across one pixel), and
                    //     the colour within 4;
                    //   * a flat F4 in one of the lane-line colours: its centroid must fall in one of our
                    //     lane lines, of exactly that colour.
                    // Plus the road page itself: our page from DATA\GAMEBIN1.DAT against the capture's
                    // VRAM texel for texel, and the twelve CLUTs against the VRAM rows their ids name.
                    std::string report;
                    report += "rrview --roadcheck: the band-2 road against the original's packets\n";
                    bool ok = !stateRam.empty();
                    if (stateRam.empty()) report += "needs --state\n";
                    std::vector<std::vector<std::string>> rows;
                    if (FILE* in = std::fopen(origPrimsPath.c_str(), "rb")) {
                        char line[1024];
                        while (std::fgets(line, sizeof(line), in)) {
                            std::vector<std::string> cols;
                            std::string cur;
                            for (const char* c = line; *c && *c != '\n' && *c != '\r'; ++c) {
                                if (*c == ',') {
                                    cols.push_back(cur);
                                    cur.clear();
                                } else {
                                    cur += *c;
                                }
                            }
                            cols.push_back(cur);
                            rows.push_back(cols);
                        }
                        std::fclose(in);
                    }
                    if (rows.size() < 2) {
                        report += "no original packets read from --orig-prims " + origPrimsPath + "\n";
                        ok = false;
                    }
                    const rr::RoadPage& page = scene.RoadPageData();
                    const CellLook& look = scene.Look();
                    const Band2Frame& band2 = scene.LastBand2();
                    const uint32_t roadTpage = static_cast<uint32_t>(page.pageX / 64) | static_cast<uint32_t>((page.pageY >> 8) << 4);
                    uint16_t clutIds[12] = {};
                    for (uint32_t k = 0; k < 12 && !stateRam.empty(); ++k)
                        clutIds[k] = static_cast<uint16_t>(guestU32(0x800D5EC8u + 2 * k) & 0xFFFF);

                    // The page and the CLUTs against the capture's VRAM.
                    size_t pageTexels = 0, pageSame = 0, clutEntries = 0, clutSame = 0;
                    {
                        std::vector<uint8_t> vram;
                        if (FILE* in = std::fopen((statePath + "/vram.bin").c_str(), "rb")) {
                            vram.resize(1024 * 512 * 2);
                            if (std::fread(vram.data(), 1, vram.size(), in) != vram.size()) vram.clear();
                            std::fclose(in);
                        }
                        const auto half = [&](int x, int y) {
                            const size_t at = (static_cast<size_t>(y) * 1024 + static_cast<size_t>(x)) * 2;
                            return static_cast<uint16_t>(vram[at] | (vram[at + 1] << 8));
                        };
                        if (!vram.empty() && page.covered.size() == 256 * 256) {
                            // Every texel one of the three uploaded TIMs covers.
                            for (int y = 0; y < 256; ++y)
                                for (int x = 0; x < 256; ++x) {
                                    if (!page.covered[static_cast<size_t>(y) * 256 + static_cast<size_t>(x)]) continue;
                                    const uint16_t word = half(page.pageX + x / 4, page.pageY + y);
                                    const uint8_t texel = static_cast<uint8_t>((word >> ((x & 3) * 4)) & 0xF);
                                    ++pageTexels;
                                    if (texel == page.indices[static_cast<size_t>(y) * 256 + static_cast<size_t>(x)]) ++pageSame;
                                }
                            for (int k = 0; k < 12; ++k)
                                for (int i = 0; i < 16; ++i) {
                                    const int cx = (clutIds[k] & 0x3F) * 16 + i, cy = (clutIds[k] >> 6) & 0x1FF;
                                    ++clutEntries;
                                    if (half(cx, cy) == page.cluts[static_cast<size_t>(k)][static_cast<size_t>(i)]) ++clutSame;
                                }
                        }
                    }

                    // The original's projection of a world point into its 384 x 240 frame.
                    const float aspectRow = rr::render::kGteAspectRow;
                    const auto project = [&](const float p[3], double& sx, double& sy) {
                        double d[3], x = 0, y = 0, z = 0;
                        for (int k = 0; k < 3; ++k) d[k] = static_cast<double>(p[k]) - sv.eye[k];
                        for (int k = 0; k < 3; ++k) {
                            x += d[k] * sv.right[k];
                            y += d[k] * sv.down[k];
                            z += d[k] * sv.forward[k];
                        }
                        if (z <= 0.01) return false;
                        sx = 192.0 + 237.0 * x / z;
                        sy = 120.0 + 237.0 * aspectRow * y / z;
                        return true;
                    };
                    // Parameter corners as AppendBand2Soup uses them: (0,0), (0,1), (1,0), (1,1).
                    const auto corner = [](const Band2Strip& s, int k) {
                        const int map[4] = {0, s.nearPath ? 1 : 2, s.nearPath ? 2 : 1, 3};
                        return map[k];
                    };
                    const auto bilinear = [&](const Band2Strip& s, double u, double t, double out[3]) {
                        const double w[4] = {(1 - u) * (1 - t), (1 - u) * t, u * (1 - t), u * t};
                        for (int a = 0; a < 3; ++a) {
                            out[a] = 0;
                            for (int k = 0; k < 4; ++k) out[a] += w[k] * s.p[corner(s, k)][a];
                        }
                    };
                    // Casts the ray of screen point (sx, sy) into strip s: returns (u, t) on the strip.
                    const auto cast = [&](const Band2Strip& s, double sx, double sy, double& u, double& t) {
                        double dir[3];
                        for (int k = 0; k < 3; ++k)
                            dir[k] = sv.forward[k] + (sx - 192.0) / 237.0 * sv.right[k] +
                                     (sy - 120.0) / (237.0 * aspectRow) * sv.down[k];
                        u = 0.5;
                        t = 0.5;
                        double lambda = 0;
                        {
                            double m[3];
                            bilinear(s, 0.5, 0.5, m);
                            for (int k = 0; k < 3; ++k) lambda += (m[k] - sv.eye[k]) * sv.forward[k];
                        }
                        for (int iter = 0; iter < 12; ++iter) {
                            double b[3], du[3], dt[3], f[3];
                            bilinear(s, u, t, b);
                            double bu[3], bt[3];
                            bilinear(s, u + 1e-4, t, bu);
                            bilinear(s, u, t + 1e-4, bt);
                            for (int k = 0; k < 3; ++k) {
                                du[k] = (bu[k] - b[k]) / 1e-4;
                                dt[k] = (bt[k] - b[k]) / 1e-4;
                                f[k] = b[k] - sv.eye[k] - lambda * dir[k];
                            }
                            // Solve [du dt -dir] x = -f by Cramer's rule.
                            const double c3[3] = {-dir[0], -dir[1], -dir[2]};
                            const auto det = [](const double a[3], const double b2[3], const double c[3]) {
                                return a[0] * (b2[1] * c[2] - b2[2] * c[1]) - b2[0] * (a[1] * c[2] - a[2] * c[1]) +
                                       c[0] * (a[1] * b2[2] - a[2] * b2[1]);
                            };
                            const double nf[3] = {-f[0], -f[1], -f[2]};
                            const double D = det(du, dt, c3);
                            if (std::fabs(D) < 1e-12) return false;
                            u += det(nf, dt, c3) / D;
                            t += det(du, nf, c3) / D;
                            lambda += det(du, dt, nf) / D;
                        }
                        return lambda > 0;
                    };
                    const auto uvAt = [&](const Band2Strip& s, double u, double t, double out[2]) {
                        const double w[4] = {(1 - u) * (1 - t), (1 - u) * t, u * (1 - t), u * t};
                        out[0] = out[1] = 0;
                        for (int k = 0; k < 4; ++k)
                            for (int a = 0; a < 2; ++a) out[a] += w[k] * s.uv[corner(s, k)][a];
                    };
                    const auto rgbAt = [&](const Band2Strip& s, double u, double t, double out[3]) {
                        const double w[4] = {(1 - u) * (1 - t), (1 - u) * t, u * (1 - t), u * t};
                        out[0] = out[1] = out[2] = 0;
                        for (int k = 0; k < 4; ++k)
                            for (int a = 0; a < 3; ++a) out[a] += w[k] * s.rgb[corner(s, k)][a];
                    };
                    const auto findStrip = [&](double sx, double sy, double& bu, double& bt) -> const Band2Strip* {
                        const Band2Strip* best = nullptr;
                        double bestMargin = -1e9;
                        for (const Band2Strip& s : band2.strips) {
                            double u = 0, t = 0;
                            if (!cast(s, sx, sy, u, t)) continue;
                            const double margin = std::min(std::min(u, 1 - u), std::min(t, 1 - t));
                            if (margin > bestMargin) {
                                bestMargin = margin;
                                best = &s;
                                bu = u;
                                bt = t;
                            }
                        }
                        return bestMargin >= -0.01 ? best : nullptr;
                    };

                    size_t packets = 0, packetsNear = 0, packetsFar = 0, unplaced = 0, paletteBad = 0;
                    size_t vertices = 0, verticesSmall = 0, uvBad = 0, rgbBad = 0;
                    size_t lines = 0, linesMatched = 0, linesWrongColour = 0;
                    size_t sliversByCorner = 0, sliversWrong = 0, sliversNotPlaced = 0, farNotMatched = 0;
                    double worstUv = 0, worstRgb = 0;
                    std::vector<std::string> examples;
                    std::vector<char> stripHit(band2.strips.size(), 0);
                    for (size_t r = 1; r < rows.size(); ++r) {
                        const std::vector<std::string>& c = rows[r];
                        if (c.size() < 32) continue;
                        const uint32_t cmd = static_cast<uint32_t>(std::strtoul(c[0].c_str(), nullptr, 16));
                        double vx[4], vy[4], vu[4], vv[4], vr[4][3];
                        for (int k = 0; k < 4; ++k) {
                            vx[k] = std::atof(c[4 + 7 * k].c_str());
                            vy[k] = std::atof(c[5 + 7 * k].c_str());
                            vu[k] = std::atof(c[6 + 7 * k].c_str());
                            vv[k] = std::atof(c[7 + 7 * k].c_str());
                            for (int a = 0; a < 3; ++a) vr[k][a] = std::atof(c[8 + 7 * k + a].c_str());
                        }
                        const double cx = (vx[0] + vx[1] + vx[2] + vx[3]) / 4, cy = (vy[0] + vy[1] + vy[2] + vy[3]) / 4;
                        if ((cmd & 0xFC) == 0x28 && !(cmd & 4)) {
                            // An untextured flat quad: a lane line if its colour is one of the table's.
                            const uint32_t colour = static_cast<uint32_t>(vr[0][0]) | (static_cast<uint32_t>(vr[0][1]) << 8) |
                                                    (static_cast<uint32_t>(vr[0][2]) << 16);
                            bool isLine = false;
                            for (int k = 1; k < 4; ++k) isLine = isLine || look.tables.lineColour[static_cast<size_t>(k)] == colour;
                            if (!isLine) continue;
                            ++lines;
                            bool inLine = false, sameColour = false;
                            for (const Band2Line& l : band2.lines) {
                                double px[4], py[4];
                                bool on = true;
                                for (int k = 0; k < 4; ++k) on = on && project(l.p[k], px[k], py[k]);
                                if (!on) continue;
                                const auto inTri = [&](int a, int b, int d) {
                                    const auto side = [&](int p, int q) {
                                        return (px[q] - px[p]) * (cy - py[p]) - (py[q] - py[p]) * (cx - px[p]);
                                    };
                                    const double s1 = side(a, b), s2 = side(b, d), s3 = side(d, a);
                                    const double e = 1.5 * std::max({std::hypot(px[b] - px[a], py[b] - py[a]),
                                                                     std::hypot(px[d] - px[b], py[d] - py[b]),
                                                                     std::hypot(px[a] - px[d], py[a] - py[d])});
                                    return (s1 >= -e && s2 >= -e && s3 >= -e) || (s1 <= e && s2 <= e && s3 <= e);
                                };
                                if (inTri(0, 1, 2) || inTri(1, 3, 2)) {
                                    inLine = true;
                                    if (l.colour == colour) sameColour = true;
                                }
                            }
                            if (inLine) ++linesMatched;
                            if (inLine && !sameColour) ++linesWrongColour;
                            continue;
                        }
                        if (cmd != 0x3C && cmd != 0x3E) continue;
                        const uint32_t tpage = static_cast<uint32_t>(std::strtoul(c[1].c_str(), nullptr, 16));
                        if ((tpage & 0x1F) != roadTpage || ((tpage >> 7) & 3) != 0) continue;
                        const uint16_t clut = static_cast<uint16_t>(std::strtoul(c[2].c_str(), nullptr, 16));
                        int row = -1;
                        for (int k = 0; k < 12; ++k)
                            if (clutIds[k] == clut) row = k;
                        ++packets;
                        // A sliver - a quad seen edge-on at the horizon, a pixel or two tall - has no
                        // inside to cast a ray into. A far-path quad is drawn unsubdivided, so its four
                        // vertices ARE a quad of ours: those are matched corner to corner (1.5 px) and
                        // must carry exactly our UVs, palette and colours. A near-path sliver is a
                        // piece of a subdivided strip and cannot be placed; it is counted, not judged.
                        const double area =
                            0.5 * std::fabs((vx[1] - vx[0]) * (vy[2] - vy[0]) - (vx[2] - vx[0]) * (vy[1] - vy[0])) +
                            0.5 * std::fabs((vx[1] - vx[3]) * (vy[2] - vy[3]) - (vx[2] - vx[3]) * (vy[1] - vy[3]));
                        const double spanX = std::max({vx[0], vx[1], vx[2], vx[3]}) - std::min({vx[0], vx[1], vx[2], vx[3]});
                        const double spanY = std::max({vy[0], vy[1], vy[2], vy[3]}) - std::min({vy[0], vy[1], vy[2], vy[3]});
                        double castU = 0, castT = 0;
                        const bool castable = findStrip(cx, cy, castU, castT) != nullptr;
                        if (area < 16.0 || spanX < 4.0 || spanY < 4.0 || (!castable && row >= 0 && (row & 3) == 0)) {
                            const Band2Strip* same = nullptr;
                            for (const Band2Strip& s : band2.strips) {
                                if (s.nearPath) continue;
                                bool all = true;
                                for (int k = 0; k < 4 && all; ++k) {
                                    double px = 0, py = 0;
                                    all = project(s.p[k], px, py) && std::fabs(px - vx[k]) <= 1.5 && std::fabs(py - vy[k]) <= 1.5;
                                }
                                if (all) {
                                    same = &s;
                                    break;
                                }
                            }
                            if (!same && row >= 0 && (row & 3) == 0) {
                                // A far-path packet (palette g, never g + 1 + clutsel) with no far quad of
                                // ours at its corners: we drew that quad another way.
                                ++farNotMatched;
                                if (examples.size() < 80) {
                                    char b[160];
                                    std::snprintf(b, sizeof(b), "  packet %zu: far-path quad of the original, no far quad of ours at its corners", r);
                                    examples.emplace_back(b);
                                }
                                continue;
                            }
                            if (!same) {
                                ++sliversNotPlaced;
                                // Not judged, but it does show the original drew something where the
                                // strip its centroid falls in lies.
                                double hu = 0, ht = 0;
                                if (const Band2Strip* hit = findStrip(cx, cy, hu, ht))
                                    stripHit[static_cast<size_t>(hit - band2.strips.data())] = 1;
                                continue;
                            }
                            ++sliversByCorner;
                            ++packetsFar;
                            stripHit[static_cast<size_t>(same - band2.strips.data())] = 1;
                            bool exact = row == same->paletteRow;
                            for (int k = 0; k < 4; ++k) {
                                exact = exact && same->uv[k][0] == vu[k] && same->uv[k][1] == vv[k];
                                for (int a = 0; a < 3; ++a) exact = exact && std::fabs(same->rgb[k][a] - vr[k][a]) <= 4.0;
                            }
                            if (!exact) {
                                ++sliversWrong;
                                if (examples.size() < 80) {
                                    char b[200];
                                    std::snprintf(b, sizeof(b), "  packet %zu: a far quad of ours at its corners, but UV / palette / colour differ",
                                                  r);
                                    examples.emplace_back(b);
                                }
                            }
                            continue;
                        }
                        double bu = 0, bt = 0;
                        const Band2Strip* strip = findStrip(cx, cy, bu, bt);
                        if (!strip) {
                            ++unplaced;
                            if (examples.size() < 80) {
                                char b[160];
                                std::snprintf(b, sizeof(b), "  packet %zu (centroid %.0f,%.0f): no strip of ours there", r, cx, cy);
                                examples.emplace_back(b);
                            }
                            continue;
                        }
                        stripHit[static_cast<size_t>(strip - band2.strips.data())] = 1;
                        (strip->nearPath ? packetsNear : packetsFar)++;
                        if (row != strip->paletteRow) {
                            ++paletteBad;
                            if (examples.size() < 80) {
                                char b[160];
                                std::snprintf(b, sizeof(b), "  packet %zu: CLUT %04X = row %d, ours %d", r, clut, row, strip->paletteRow);
                                examples.emplace_back(b);
                            }
                        }
                        for (int k = 0; k < 4; ++k) {
                            double u = 0, t = 0, u1 = 0, t1 = 0, u2 = 0, t2 = 0;
                            if (!cast(*strip, vx[k], vy[k], u, t)) continue;
                            cast(*strip, vx[k] + 1, vy[k], u1, t1);
                            cast(*strip, vx[k], vy[k] + 1, u2, t2);
                            // A vertex of a piece of this strip cannot lie outside it: where the cast
                            // says it does, the console's subdivider has pushed an edge midpoint out by
                            // about a pixel to close cracks (`RASHCDG 0x80069E50..0x80069EE4`) or the
                            // integer screen position rounded it there. Its UV is the edge's.
                            u = std::clamp(u, 0.0, 1.0);
                            t = std::clamp(t, 0.0, 1.0);
                            double uv[2], uvx[2], uvy[2], rgb[3];
                            uvAt(*strip, u, t, uv);
                            uvAt(*strip, u1, t1, uvx);
                            uvAt(*strip, u2, t2, uvy);
                            rgbAt(*strip, u, t, rgb);
                            const double gradient = std::max({std::fabs(uvx[0] - uv[0]), std::fabs(uvx[1] - uv[1]),
                                                              std::fabs(uvy[0] - uv[0]), std::fabs(uvy[1] - uv[1])});
                            if (gradient > 4.0) {
                                ++verticesSmall; // a sliver: one pixel spans more than four texels
                                continue;
                            }
                            ++vertices;
                            // 1.5 texels of integer truncation in the subdivider's UV averaging, plus five
                            // pixels' worth of UV change: the integer screen position and the crack-closing
                            // nudge of about a pixel per subdivision level (0x80069CF0 recurses up to five).
                            const double tolerance = 1.5 + 5.0 * gradient;
                            const double e = std::max(std::fabs(uv[0] - vu[k]), std::fabs(uv[1] - vv[k]));
                            worstUv = std::max(worstUv, e - tolerance);
                            if (e > tolerance) {
                                ++uvBad;
                                if (examples.size() < 80) {
                                    char b[200];
                                    std::snprintf(b, sizeof(b),
                                                  "  packet %zu vertex %d (%.0f,%.0f): original UV (%.0f,%.0f), ours (%.1f,%.1f), "
                                                  "tolerance %.1f (%s strip, s %.2f t %.2f)", r, k, vx[k], vy[k], vu[k], vv[k], uv[0],
                                                  uv[1], tolerance, strip->nearPath ? "near" : "far", u, t);
                                    examples.emplace_back(b);
                                }
                            }
                            double ce = 0;
                            for (int a = 0; a < 3; ++a) ce = std::max(ce, std::fabs(rgb[a] - vr[k][a]));
                            worstRgb = std::max(worstRgb, ce);
                            if (ce > 4.0) ++rgbBad;
                        }
                    }
                    // Our strips wholly on the original's screen that no original packet landed in.
                    size_t onScreen = 0, onScreenMissed = 0;
                    for (size_t n = 0; n < band2.strips.size(); ++n) {
                        bool inside = true;
                        for (int k = 0; k < 4; ++k) {
                            double sx = 0, sy = 0;
                            inside = inside && project(band2.strips[n].p[k], sx, sy) && sx >= 2 && sx <= 381 && sy >= 2 && sy <= 237;
                        }
                        if (!inside) continue;
                        {
                            // A strip under 64 px or 4 px tall on screen can be covered by slivers alone; those are
                            // not placed, so it is not asked for.
                            double px[4], py[4];
                            for (int k = 0; k < 4; ++k) project(band2.strips[n].p[k], px[k], py[k]);
                            const double a = 0.5 * std::fabs((px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0])) +
                                             0.5 * std::fabs((px[1] - px[3]) * (py[2] - py[3]) - (px[2] - px[3]) * (py[1] - py[3]));
                            const double h = std::max({py[0], py[1], py[2], py[3]}) - std::min({py[0], py[1], py[2], py[3]});
                            if (a < 64.0 || h < 4.0) continue;
                        }
                        ++onScreen;
                        if (!stripHit[n]) {
                            ++onScreenMissed;
                            if (examples.size() < 80) {
                                double px[4], py[4];
                                for (int k = 0; k < 4; ++k) project(band2.strips[n].p[k], px[k], py[k]);
                                char b[200];
                                std::snprintf(b, sizeof(b), "  our %s strip %zu (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f): no original packet in it",
                                              band2.strips[n].nearPath ? "near" : "far", n, px[0], py[0], px[1], py[1], px[2], py[2], px[3], py[3]);
                                examples.emplace_back(b);
                            }
                        }
                    }
                    // Bands 0 and 1 against the original's packets, primitive by primitive: every textured
                    // packet whose vertices land, in the GPU's own order, on the projected corners of a
                    // cell primitive of ours (1.5 px) must carry that primitive's UVs, the texture
                    // window our rule gives it (band 0: band0Window[flags >> 4], band 1:
                    // band1Window[flags >> 4]; the window's row is compared modulo 128, the half of the
                    // VRAM page being the loader's choice) and its colour-table colours (band 0 one per
                    // primitive, attr & 0xFF; band 1 one per vertex, w & 0xFF). Matching in GPU vertex
                    // order is itself the check of the quad split: (i0, i1, i3, i2).
                    size_t cellPackets = 0, cellMatched = 0, cellWindowBad = 0, cellUvBad = 0, cellColourBad = 0, cellSmall = 0, cellAmbiguous = 0;
                    size_t cellMatchedBand[2] = {0, 0};
                    {
                        struct OurPrim {
                            double x[4], y[4];
                            int n = 0;
                            int band = 0;
                            uint8_t u[4], v[4];
                            uint32_t colour[4];
                            uint16_t attr = 0;
                            uint8_t shadeIndex[4] = {};
                            uint8_t flags = 0;
                            rr::TextureWindow window;
                        };
                        std::vector<OurPrim> ours;
                        const std::vector<uint32_t>& words = scene.LastLodWords();
                        for (size_t c : sv.cellOrder) {
                            const rr::CellData& cell = raceCells[c];
                            const uint32_t word = c < words.size() ? words[c] : 0u;
                            const auto collect = [&](const std::vector<rr::CellPrimitive>& prims, int band) {
                                for (const rr::CellPrimitive& prim : prims) {
                                    const int g = static_cast<int>(prim.group), a = cell.countA, b = cell.countB;
                                    if (band == 0 && g >= a) {
                                        const uint32_t nibble = (word >> (4 * (g - a))) & 0xFu;
                                        if ((nibble & 3u) != 1u) continue;
                                    }
                                    if (band == 1) {
                                        const uint32_t nibble = (word >> (4 * (g - a - b))) & 0xFu;
                                        if ((nibble & 3u) != 3u) continue;
                                    }
                                    OurPrim o;
                                    o.band = band;
                                    o.n = prim.quad ? 4 : 3;
                                    const int order[4] = {0, 1, 3, 2};
                                    bool on = true;
                                    for (int k = 0; k < o.n; ++k) {
                                        const int i = prim.quad ? order[k] : k;
                                        const uint16_t index = prim.index[i];
                                        const float p[3] = {rr::CellWorldX(cell, index), rr::CellWorldY(cell, index),
                                                            rr::CellWorldZ(cell, index)};
                                        on = on && project(p, o.x[k], o.y[k]);
                                        o.shadeIndex[k] = static_cast<uint8_t>(cell.vertexW[index] & 0xFF);
                                        o.u[k] = prim.u[i];
                                        o.v[k] = prim.v[i];
                                        o.colour[k] = look.colours[band == 0 ? (prim.attr & 0xFFu)
                                                                              : (static_cast<uint16_t>(cell.vertexW[index]) & 0xFFu)];
                                    }
                                    if (!on) continue;
                                    o.attr = prim.attr;
                                    o.flags = prim.flags;
                                    const unsigned tile = prim.flags >> 4;
                                    const unsigned probe = roadCheckMutate == 4 ? (tile + 1) % 15 : tile; // the window control
                                    const uint32_t e2 = band == 0 ? (tile < 15 ? look.tables.band0Window[probe] : 0xE2000000u)
                                                                  : look.tables.band1Window[roadCheckMutate == 4 && tile < 15 ? (tile + 1) % 15 : tile & 15];
                                    o.window = rr::DecodeTextureWindow(e2);
                                    ours.push_back(o);
                                }
                            };
                            collect(cell.band0, 0);
                            if (cell.region7Present) collect(cell.band1, 1);
                        }
                        for (size_t r = 1; r < rows.size(); ++r) {
                            const std::vector<std::string>& c = rows[r];
                            if (c.size() < 34) continue;
                            const uint32_t cmd = static_cast<uint32_t>(std::strtoul(c[0].c_str(), nullptr, 16));
                            if (cmd < 0x24 || cmd > 0x3F || !(cmd & 4)) continue;
                            const uint32_t tpage = static_cast<uint32_t>(std::strtoul(c[1].c_str(), nullptr, 16));
                            if ((tpage & 0x1F) == roadTpage) continue;
                            const int n = std::atoi(c[33].c_str());
                            double vx[4], vy[4];
                            int vu[4], vv[4];
                            uint32_t vc[4];
                            for (int k = 0; k < n; ++k) {
                                vx[k] = std::atof(c[4 + 7 * k].c_str());
                                vy[k] = std::atof(c[5 + 7 * k].c_str());
                                vu[k] = std::atoi(c[6 + 7 * k].c_str());
                                vv[k] = std::atoi(c[7 + 7 * k].c_str());
                                vc[k] = static_cast<uint32_t>(std::atoi(c[8 + 7 * k].c_str())) |
                                        (static_cast<uint32_t>(std::atoi(c[9 + 7 * k].c_str())) << 8) |
                                        (static_cast<uint32_t>(std::atoi(c[10 + 7 * k].c_str())) << 16);
                            }
                            // Near the horizon many primitives shrink onto the same few pixels, and a corner
                            // match there is a guess: only packets 4 px or more across both ways, whose
                            // corners fit exactly ONE primitive of ours, are judged.
                            const double spanX = *std::max_element(vx, vx + n) - *std::min_element(vx, vx + n);
                            const double spanY = *std::max_element(vy, vy + n) - *std::min_element(vy, vy + n);
                            ++cellPackets;
                            if (spanX < 4.0 || spanY < 4.0) {
                                ++cellSmall;
                                continue;
                            }
                            const OurPrim* match = nullptr;
                            size_t fits = 0;
                            for (const OurPrim& o : ours) {
                                if (o.n != n) continue;
                                bool all = true;
                                for (int k = 0; k < n && all; ++k) all = std::fabs(o.x[k] - vx[k]) <= 1.5 && std::fabs(o.y[k] - vy[k]) <= 1.5;
                                if (all) {
                                    match = &o;
                                    ++fits;
                                }
                            }
                            if (fits > 1) {
                                ++cellAmbiguous;
                                continue;
                            }
                            if (!match) continue;
                            ++cellMatched;
                            ++cellMatchedBand[match->band];
                            const rr::TextureWindow theirs =
                                rr::DecodeTextureWindow(static_cast<uint32_t>(std::strtoul(c[32].c_str(), nullptr, 16)));
                            const bool windowSame = theirs.width == match->window.width && theirs.x == match->window.x &&
                                                    (theirs.y % 128) == (match->window.y % 128);
                            if (!windowSame) {
                                ++cellWindowBad;
                                if (examples.size() < 80) {
                                    char b[200];
                                    std::snprintf(b, sizeof(b), "  cell packet %zu (band %d): window %d at (%d,%d), ours %d at (%d,%d)",
                                                  r, match->band, theirs.width, theirs.x, theirs.y, match->window.width,
                                                  match->window.x, match->window.y);
                                    examples.emplace_back(b);
                                }
                            }
                            bool uvSame = true, colourSame = true;
                            const bool gouraud = (cmd & 0x10) != 0;
                            for (int k = 0; k < n; ++k) {
                                uvSame = uvSame && vu[k] == match->u[k] && (vv[k] % 128) == (match->v[k] % 128);
                                const uint32_t want = match->colour[gouraud ? k : 0];
                                colourSame = colourSame && vc[gouraud ? k : 0] == want;
                            }
                            if (!uvSame) ++cellUvBad;
                            if (!colourSame) {
                                ++cellColourBad;
                                if (examples.size() < 80) {
                                    char b[160];
                                    std::snprintf(b, sizeof(b), "  cell packet %zu (band %d, cmd %02X, flags %02X attr %04X, w %02X %02X %02X %02X): colour %06X, ours %06X", r,
                                                  match->band, cmd, match->flags, match->attr, match->shadeIndex[0], match->shadeIndex[1],
                                                  match->shadeIndex[2], match->shadeIndex[3], vc[0], match->colour[0]);
                                    examples.emplace_back(b);
                                }
                            }
                        }
                    }
                    char summary[2000];
                    std::snprintf(summary, sizeof(summary),
                                  "road page (VRAM %d,%d), the texels TIMs 0..2 cover: %zu of %zu equal; CLUTs: %zu of %zu entries equal\n"
                                  "our band 2 this frame: %zu strips (%zu quads far, %zu near), %zu lane lines, %zu "
                                  "triangles not drawn, %zu palette rows out of range\n"
                                  "original road packets %zu: placed in a strip of ours %zu (near-path %zu, far-path %zu), "
                                  "unplaced %zu, palette row differs %zu\n"
                                  "  of them slivers (under 16 px, under 4 px across, or far-path with no inside to cast into): far quads matched to ours corner for corner %zu, of those "
                                  "differing in UV / palette / colour %zu, far-path ones with no far quad of ours there %zu; near-path slivers not judged %zu\n"
                                  "vertices compared %zu (skipped, one pixel spans over 4 texels: %zu): UV outside tolerance "
                                  "%zu (worst excess %.2f texels), colour off by "
                                  "more than 4: %zu (worst %.1f)\n"
                                  "original lane-line packets %zu: in a line of ours %zu, of a different colour %zu\n"
                                  "our strips wholly on the original's screen %zu, none of whose pixels the original drew %zu\n"
                                  "cell packets (bands 0 and 1, textured) %zu, under 4 px across %zu, fitting two of ours %zu; on exactly one primitive of ours in GPU vertex order %zu (band 0 %zu, band 1 %zu); "
                                  "texture window differs %zu, UV differs %zu, colour differs %zu\n",
                                  page.pageX, page.pageY, pageSame, pageTexels, clutSame, clutEntries, band2.strips.size(),
                                  band2.quadsFar, band2.quadsNear, band2.lines.size(), band2.trianglesNotDrawn,
                                  band2.paletteOutOfRange, packets, packetsNear + packetsFar, packetsNear, packetsFar,
                                  unplaced, paletteBad, sliversByCorner, sliversWrong, farNotMatched, sliversNotPlaced, vertices, verticesSmall,
                                  uvBad, worstUv < 0 ? 0.0 : worstUv, rgbBad, worstRgb, lines, linesMatched,
                                  linesWrongColour, onScreen, onScreenMissed, cellPackets, cellSmall, cellAmbiguous, cellMatched, cellMatchedBand[0],
                                  cellMatchedBand[1], cellWindowBad, cellUvBad, cellColourBad);
                    // The level-of-detail words: ours (CellLodWord) against the capture's own slot +0x38,
                    // which `SLUS 0x80035680` wrote for this very frame.
                    size_t lodCompared = 0, lodSame = 0;
                    {
                        std::string lodLine = "level-of-detail words, ours / the capture's slot +0x38:";
                        const std::vector<uint32_t>& words = scene.LastLodWords();
                        for (uint32_t slot = 0; slot < 24; ++slot) {
                            const uint32_t base = 0x800D87E8u + 112 * slot;
                            const uint32_t id = guestU32(base);
                            for (size_t c : sv.cellOrder)
                                if (raceCells[c].header.id == id && c < words.size()) {
                                    const uint32_t theirs = guestU32(base + 0x38);
                                    char b[96];
                                    std::snprintf(b, sizeof(b), " cell %08X %X/%X", id, words[c], theirs);
                                    lodLine += b;
                                    ++lodCompared;
                                    if (words[c] == theirs) ++lodSame;
                                }
                        }
                        report += lodLine + "\n";
                    }
                    const bool clean = packets > 0 && vertices > 0 && unplaced == 0 && paletteBad == 0 && uvBad == 0 &&
                                       sliversWrong == 0 && farNotMatched == 0 && sliversByCorner > 0 &&
                                       cellMatchedBand[0] > 0 && cellMatchedBand[1] > 0 && cellWindowBad == 0 &&
                                       cellUvBad == 0 && cellColourBad == 0 &&
                                       lodSame == lodCompared && lodCompared > 0 &&
                                       rgbBad == 0 && linesMatched == lines && linesWrongColour == 0 && onScreenMissed == 0 &&
                                       pageSame == pageTexels && pageTexels > 0 && clutSame == clutEntries;
                    std::string verdict;
                    if (!ok) verdict = "FAIL (no state or no packets)";
                    else if (roadCheckMutate != 0)
                        verdict = clean ? "FAIL (negative control: the mutated road still matched the original)"
                                        : "PASS (negative control: the mutated road does not match the original)";
                    else
                        verdict = clean ? "PASS" : "FAIL";
                    report += summary;
                    report += "verdict " + verdict + "\n";
                    for (const std::string& line : examples) report += line + "\n";
                    report += "\nour strips, projected into the original's frame (corners in parameter order (0,0) (0,1) (1,0) (1,1)):\n";
                    for (const Band2Strip& st : band2.strips) {
                        char b[256];
                        double px[4], py[4];
                        for (int k = 0; k < 4; ++k)
                            if (!project(st.p[corner(st, k)], px[k], py[k])) px[k] = py[k] = -9999;
                        std::snprintf(b, sizeof(b), "  %s row %2d  (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)\n",
                                      st.nearPath ? "near" : "far ", st.paletteRow, px[0], py[0], px[1], py[1], px[2],
                                      py[2], px[3], py[3]);
                        report += b;
                    }
                    if (FILE* out = std::fopen(roadCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("roadcheck verdict %s\nroadcheck report: %s\n", verdict.c_str(), roadCheckPath.c_str());
                }
                if (!riderCheckPath.empty()) { // rider_pose.h CheckRiderPackets
                    rr::game::RiderPacketCheck rc;
                    rc.disc = &disc;
                    rc.ram = &stateRam;
                    rc.primsCsv = origPrimsPath;
                    for (int k = 0; k < 16; ++k) rc.bike[k] = bikeModel.m[k];
                    for (int k = 0; k < 3; ++k) rc.attach[k] = scene.RiderAttach()[k];
                    rc.ctx = &sv;
                    rc.project = [](void* c, const float p[3], double& sx, double& sy) {
                        const auto& s = *static_cast<const decltype(sv)*>(c);
                        double x = 0, y = 0, z = 0;
                        for (int k = 0; k < 3; ++k) {
                            const double d = static_cast<double>(p[k]) - s.eye[k];
                            x += d * s.right[k];
                            y += d * s.down[k];
                            z += d * s.forward[k];
                        }
                        if (z <= 0.01) return false;
                        sx = 192.0 + 237.0 * x / z;
                        sy = 120.0 + 237.0 * rr::render::kGteAspectRow * y / z;
                        return true;
                    };
                    bool pass = false;
                    const std::string report = rr::game::CheckRiderPackets(rc, pass);
                    if (FILE* out = std::fopen(riderCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(report.c_str(), stdout);
                }
                if (!propCheckPath.empty()) {
                    // --propcheck: the roadside props of THIS frame against the ORIGINAL's packets for the
                    // same captured state. A prop primitive is a textured quad the model emitter SLUS
                    // 0x800251E4 sends as (i0, uv +4), (i1, uv +0), (i3, uv +10), (i2, uv +8). Each such
                    // packet must be ONE primitive of ours: the four texels in that corner order (so the
                    // texel association is checked, not only the texel set) and the four corners, placed by
                    // our instance matrix and projected with the capture's camera, within 2.5 px. Then
                    // the face: our frame, read back (pass 1: palette row, index), must show that
                    // primitive's palette row at the packet's centre - which is what the one-sided test
                    // decides when a front and a back plate share the place. And the other way: a one-sided
                    // primitive of ours that the console's test keeps, wholly inside the frame and not a
                    // sliver, must have its packet.
                    std::string report = "rrview --propcheck: the roadside props against the original's packets\n";
                    bool ok = !stateRam.empty();
                    struct Pk { long x[4], y[4]; int u[4], v[4]; };
                    std::vector<Pk> packets;
                    if (FILE* in = std::fopen(origPrimsPath.c_str(), "rb")) {
                        char line[1024];
                        std::vector<std::string> header;
                        bool first = true;
                        while (std::fgets(line, sizeof(line), in)) {
                            std::vector<std::string> cols;
                            std::string cur;
                            for (const char* c = line; *c && *c != '\n' && *c != '\r'; ++c) {
                                if (*c == ',') {
                                    cols.push_back(cur);
                                    cur.clear();
                                } else {
                                    cur += *c;
                                }
                            }
                            cols.push_back(cur);
                            if (first) {
                                header = cols;
                                first = false;
                                continue;
                            }
                            const auto col = [&](const std::string& name) -> long {
                                for (size_t k = 0; k < header.size() && k < cols.size(); ++k)
                                    if (header[k] == name) return std::strtol(cols[k].c_str(), nullptr, 0);
                                return -1;
                            };
                            const long cmd = col("cmd");
                            // textured 4-corner polygons: GP0 0x2C..0x2F, 0x3C..0x3F
                            if (col("nverts") != 4 || !((cmd & 0xEC) == 0x2C)) continue;
                            Pk pk{};
                            for (int c = 0; c < 4; ++c) {
                                pk.x[c] = col("x" + std::to_string(c));
                                pk.y[c] = col("y" + std::to_string(c));
                                pk.u[c] = static_cast<int>(col("u" + std::to_string(c)));
                                pk.v[c] = static_cast<int>(col("v" + std::to_string(c)));
                            }
                            packets.push_back(pk);
                        }
                        std::fclose(in);
                    }
                    if (packets.empty()) ok = false;
                    const float aspectRow = rr::render::kGteAspectRow;
                    const auto projectPoint = [&](const float p[3], double& sx, double& sy) {
                        double x = 0, y = 0, z = 0;
                        for (int k = 0; k < 3; ++k) {
                            const double d = static_cast<double>(p[k]) - sv.eye[k];
                            x += d * sv.right[k];
                            y += d * sv.down[k];
                            z += d * sv.forward[k];
                        }
                        if (z <= 0.01) return false;
                        sx = 192.0 + 237.0 * x / z;
                        sy = 120.0 + 237.0 * aspectRow * y / z;
                        return true;
                    };
                    // Our primitives: every quad of every prop the frame drew (drawnProps: the draw range
                    // already applied), corners i0..i3 found in the quad's six soup vertices.
                    struct Ours { size_t prop, group, quad; double x[4], y[4]; int u[4], v[4]; int page; bool oneSided, visible; };
                    std::vector<Ours> ours;
                    const rr::TriangleSoup& psoup = scene.PropSoup();
                    for (size_t n = 0; n < drawnProps.size(); ++n) {
                        const PropInstance& inst = *drawnProps[n];
                        const size_t first = static_cast<size_t>(scene.PropFirst()[inst.group]);
                        const size_t count = static_cast<size_t>(scene.PropCount()[inst.group]);
                        for (size_t q = 0; q + 6 <= count; q += 6) {
                            Ours o{};
                            o.prop = n;
                            o.group = inst.group;
                            o.quad = q / 6;
                            o.visible = true;
                            // where each corner first appears among a quad's six soup vertices (rmd3.h SoupCorners)
                            size_t corner[4] = {0, 0, 0, 0};
                            {
                                rr::Primitive quadPrim;
                                quadPrim.clut = 0x2000u;
                                const int* sc = rr::SoupCorners(quadPrim);
                                for (int c = 0; c < 4; ++c)
                                    for (int k = 5; k >= 0; --k)
                                        if (sc[k] == c) corner[c] = static_cast<size_t>(k);
                            }
                            for (int c = 0; c < 4; ++c) {
                                const rr::TriangleSoup::Vertex& vx = psoup.vertices[first + q + corner[c]];
                                float w[3];
                                for (int k = 0; k < 3; ++k)
                                    w[k] = inst.matrix[12 + k] + inst.matrix[0 + k] * vx.x + inst.matrix[4 + k] * vx.y +
                                           inst.matrix[8 + k] * vx.z;
                                o.visible = o.visible && projectPoint(w, o.x[c], o.y[c]);
                                o.u[c] = static_cast<int>(vx.u);
                                o.v[c] = static_cast<int>(vx.v);
                                o.page = static_cast<int>(vx.tpage);
                                o.oneSided = vx.window[3] != 0;
                            }
                            if (propCheckMutate == "uv") { // the disc's byte order: i0 takes +0, i1 takes +4
                                std::swap(o.u[0], o.u[1]);
                                std::swap(o.v[0], o.v[1]);
                            }
                            ours.push_back(o);
                        }
                    }
                    constexpr double kTolerance = 2.5; // pixels
                    const int gpuOrder[4] = {0, 1, 3, 2};  // packet vertex k is our corner gpuOrder[k]
                    std::vector<char> ourMatched(ours.size(), 0);
                    size_t propPackets = 0, matched = 0, faceAgrees = 0, faceDiffers = 0, uvShapeOnly = 0;
                    double worst = 0;
                    std::string lines;
                    const std::vector<uint8_t> faces = [&] {
                        drawScene(1);
                        glFinish();
                        std::vector<uint8_t> f = ReadFrame(width, height);
                        drawScene(0);
                        return f;
                    }();
                    for (const Pk& pk : packets) {
                        // A prop packet: its texel set is that of some prop primitive of ours (in any order).
                        double best = 1e30;
                        long bestIdx = -1;
                        bool anyShape = false;
                        for (size_t i = 0; i < ours.size(); ++i) {
                            const Ours& o = ours[i];
                            bool sameSet = true;
                            for (int k = 0; k < 4 && sameSet; ++k) {
                                bool inSet = false;
                                for (int c = 0; c < 4; ++c) inSet = inSet || (o.u[c] == pk.u[k] && o.v[c] == pk.v[k]);
                                sameSet = inSet;
                            }
                            if (!sameSet) continue;
                            anyShape = true;
                            if (!o.visible) continue;
                            bool sameOrder = true;
                            double e = 0;
                            for (int k = 0; k < 4; ++k) {
                                const int c = gpuOrder[k];
                                sameOrder = sameOrder && o.u[c] == pk.u[k] && o.v[c] == pk.v[k];
                                e = std::max(e, std::hypot(o.x[c] - static_cast<double>(pk.x[k]), o.y[c] - static_cast<double>(pk.y[k])));
                            }
                            if (!sameOrder) continue;
                            if (e < best) {
                                best = e;
                                bestIdx = static_cast<long>(i);
                            }
                        }
                        if (!anyShape) continue; // not a prop packet
                        ++propPackets;
                        char b[320];
                        if (bestIdx < 0 || best > kTolerance) {
                            if (bestIdx < 0) ++uvShapeOnly;
                            std::snprintf(b, sizeof(b),
                                          "  packet (%ld,%ld) (%ld,%ld) (%ld,%ld) (%ld,%ld) uv0 (%d,%d): NO primitive of ours "
                                          "(%s, nearest %.2f px)\n",
                                          pk.x[0], pk.y[0], pk.x[1], pk.y[1], pk.x[2], pk.y[2], pk.x[3], pk.y[3], pk.u[0], pk.v[0],
                                          bestIdx < 0 ? "no texel association in the packet's corner order" : "too far",
                                          bestIdx < 0 ? -1.0 : best);
                            lines += b;
                            continue;
                        }
                        ++matched;
                        worst = std::max(worst, best);
                        const Ours& o = ours[static_cast<size_t>(bestIdx)];
                        ourMatched[static_cast<size_t>(bestIdx)] = 1;
                        // The face at the packet's centre, read back from our frame (bottom row first).
                        const long cx = (pk.x[0] + pk.x[1] + pk.x[2] + pk.x[3] + 2) / 4;
                        const long cy = (pk.y[0] + pk.y[1] + pk.y[2] + pk.y[3] + 2) / 4;
                        int seenPage = -1;
                        if (cx >= 0 && cy >= 0 && cx < width && cy < height) {
                            const size_t at = (static_cast<size_t>(height - 1 - cy) * static_cast<size_t>(width) + static_cast<size_t>(cx)) * 4;
                            if (faces[at + 2] == 255) seenPage = faces[at];
                        }
                        const bool agrees = seenPage == o.page;
                        if (agrees) ++faceAgrees;
                        else ++faceDiffers;
                        std::snprintf(b, sizeof(b),
                                      "  packet (%ld,%ld) (%ld,%ld) (%ld,%ld) (%ld,%ld): prop %zu group %zu quad %zu, %.2f px; "
                                      "%s, our frame at (%ld,%ld) shows palette row %d (%s)\n",
                                      pk.x[0], pk.y[0], pk.x[1], pk.y[1], pk.x[2], pk.y[2], pk.x[3], pk.y[3], o.prop, o.group,
                                      o.quad, best, o.oneSided ? "one-sided" : "two-sided", cx, cy, seenPage,
                                      agrees ? "its own" : "ANOTHER face");
                        lines += b;
                    }
                    // Ours without a packet: a primitive the console's test keeps (two-sided, or NCLIP over
                    // i0, i1, i2 >= 0 on its y-down screen), wholly inside the frame and at least 20 px in area.
                    size_t oursExpected = 0, oursMissing = 0;
                    for (size_t i = 0; i < ours.size(); ++i) {
                        const Ours& o = ours[i];
                        if (!o.visible) continue;
                        bool inside = true;
                        for (int c = 0; c < 4; ++c) inside = inside && o.x[c] >= 0 && o.y[c] >= 0 && o.x[c] < 384 && o.y[c] < 240;
                        const double mac0 = (o.x[1] - o.x[0]) * (o.y[2] - o.y[0]) - (o.x[2] - o.x[0]) * (o.y[1] - o.y[0]);
                        if (!inside || std::fabs(mac0) < 40.0) continue;
                        if (o.oneSided && mac0 < 0) continue;
                        ++oursExpected;
                        if (!ourMatched[i]) {
                            ++oursMissing;
                            char b[200];
                            std::snprintf(b, sizeof(b), "  ours: prop %zu group %zu quad %zu at (%.0f,%.0f) has no packet\n", o.prop,
                                          o.group, o.quad, o.x[0], o.y[0]);
                            lines += b;
                        }
                    }
                    // The capture's own live props (pool 4, *(0x800CD6D4), 0x254 bytes, up to *(0x800CD6D0)): each
                    // must be an instance of ours at its place (x, z within 0.1; the spawner settles y on the
                    // road) whose rotation is the entity's part-0 matrix *(+4)+4 to 2/4096 - the rotation of
                    // EVERY live prop, drawn this frame or not, which the packets of one frame cannot all show.
                    size_t ramProps = 0, ramPlaced = 0, ramRotation = 0;
                    {
                        const uint32_t base = guestU32(0x800CD6D4u);
                        const int32_t high = static_cast<int32_t>(guestU32(0x800CD6D0u));
                        for (int32_t k = 0; base >= 0x80000000u && k <= high && k < 64; ++k) {
                            const uint32_t e = base + 0x254u * static_cast<uint32_t>(k);
                            if (guestS16(e + 0xACu) == 0) continue;
                            ++ramProps;
                            const int32_t px = static_cast<int32_t>(guestU32(e + 0xB8u));
                            const int32_t pz = static_cast<int32_t>(guestU32(e + 0xC0u));
                            const uint32_t part = guestU32(e + 4u) + 4u;
                            const PropInstance* hit = nullptr;
                            for (const PropInstance& inst : scene.Props())
                                if (std::abs(static_cast<double>(inst.pos[0]) - px) < 6554.0 &&
                                    std::abs(static_cast<double>(inst.pos[2]) - pz) < 6554.0)
                                    hit = &inst;
                            if (hit == nullptr) {
                                char b[160];
                                std::snprintf(b, sizeof(b), "  capture prop %d at (%.2f, %.2f): no instance of ours there\n", k,
                                              px / 65536.0, pz / 65536.0);
                                lines += b;
                                continue;
                            }
                            ++ramPlaced;
                            double worstEl = 0;
                            for (uint32_t r = 0; r < 3; ++r)
                                for (uint32_t c = 0; c < 3; ++c) {
                                    const double want = guestS16(part + 2u * (3u * r + c)) / 4096.0;
                                    const double have = hit->matrix[4 * c + r] * rr::render::kModelUnitsPerWorldUnit;
                                    worstEl = std::max(worstEl, std::fabs(want - have));
                                }
                            if (worstEl <= 2.0 / 4096.0) ++ramRotation;
                            char b[200];
                            std::snprintf(b, sizeof(b), "  capture prop %d at (%.2f, %.2f) group %zu: rotation %s (worst element %.4f)\n", k,
                                          px / 65536.0, pz / 65536.0, hit->group, worstEl <= 2.0 / 4096.0 ? "equal" : "DIFFERS",
                                          worstEl);
                            lines += b;
                        }
                    }
                    char summary[800];
                    std::snprintf(summary, sizeof(summary),
                                  "the capture's live props %zu: %zu placed by us, %zu with its rotation; "
                                  "props drawn %zu (%zu quads of ours); original prop packets %zu: %zu matched within %.1f px "
                                  "(worst %.2f), %zu with no texel association of ours; face at the packet centre: %zu "
                                  "agree, %zu another; ours kept by the console's test and on screen: %zu, %zu without a packet\n",
                                  ramProps, ramPlaced, ramRotation, drawnProps.size(), ours.size(), propPackets, matched,
                                  kTolerance, worst, uvShapeOnly, faceAgrees, faceDiffers, oursExpected, oursMissing);
                    const bool clean = ok && propPackets > 0 && matched == propPackets && faceDiffers == 0 && oursMissing == 0 &&
                                       ramProps > 0 && ramRotation == ramProps;
                    std::string verdict;
                    if (!ok) verdict = "FAIL (no state or no packets)";
                    else if (!propCheckMutate.empty())
                        verdict = clean ? "FAIL (negative control: the mutated props still matched the original)"
                                        : "PASS (negative control: the mutated props do not match the original)";
                    else
                        verdict = clean ? "PASS" : "FAIL";
                    report += summary;
                    report += "propcheck verdict " + verdict + "\n" + lines;
                    if (FILE* out = std::fopen(propCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("propcheck verdict %s\npropcheck report: %s\n", verdict.c_str(), propCheckPath.c_str());
                }
                if (!shadowCheckPath.empty()) {
                    // --shadowcheck: the shadow of THIS frame against the original's shadow packets
                    // (`SLUS 0x80025EE0`, code 0x2A, 65 in the rr-race frame: the bike's 18 quadsD and
                    // the rider's 47). Each packet carries its quad's corners as q0, q1, q3, q2; ours
                    // are projected with the capture's camera and matched to the nearest packet.
                    std::string report = "rrview --shadowcheck: the bike's shadow against the original's packets\n";
                    bool ok = !stateRam.empty();
                    std::vector<std::array<long, 8>> packets;
                    if (FILE* in = std::fopen(origPrimsPath.c_str(), "rb")) {
                        char line[1024];
                        std::vector<std::string> header;
                        bool first = true;
                        while (std::fgets(line, sizeof(line), in)) {
                            std::vector<std::string> cols;
                            std::string cur;
                            for (const char* c = line; *c && *c != '\n' && *c != '\r'; ++c) {
                                if (*c == ',') {
                                    cols.push_back(cur);
                                    cur.clear();
                                } else {
                                    cur += *c;
                                }
                            }
                            cols.push_back(cur);
                            if (first) {
                                header = cols;
                                first = false;
                                continue;
                            }
                            if (cols.empty() || cols[0] != "0x2A") continue;
                            std::array<long, 8> xy{};
                            for (int c = 0; c < 4; ++c)
                                for (int a = 0; a < 2; ++a) {
                                    const std::string name = std::string(a == 0 ? "x" : "y") + std::to_string(c);
                                    for (size_t k = 0; k < header.size() && k < cols.size(); ++k)
                                        if (header[k] == name) xy[static_cast<size_t>(c * 2 + a)] = std::strtol(cols[k].c_str(), nullptr, 0);
                                }
                            packets.push_back(xy);
                        }
                        std::fclose(in);
                    }
                    if (packets.empty()) ok = false;
                    const float aspectRow = rr::render::kGteAspectRow;
                    const auto projectPoint = [&](const std::array<float, 3>& p, double& sx, double& sy) {
                        double x = 0, y = 0, z = 0;
                        for (int k = 0; k < 3; ++k) {
                            const double d = static_cast<double>(p[static_cast<size_t>(k)]) - sv.eye[k];
                            x += d * sv.right[k];
                            y += d * sv.down[k];
                            z += d * sv.forward[k];
                        }
                        if (z <= 0.01) return false;
                        sx = 192.0 + 237.0 * x / z;
                        sy = 120.0 + 237.0 * aspectRow * y / z;
                        return true;
                    };
                    const std::vector<std::array<float, 3>>& ours = scene.ShadowQuads();
                    std::vector<std::array<double, 8>> projected;
                    for (size_t q = 0; q + 3 < ours.size(); q += 4) {
                        std::array<double, 8> xy{};
                        const int order[4] = {0, 1, 3, 2}; // the packet's corner order
                        bool visible = true;
                        for (int c = 0; c < 4; ++c)
                            visible = visible && projectPoint(ours[q + static_cast<size_t>(order[c])], xy[static_cast<size_t>(c * 2)],
                                                              xy[static_cast<size_t>(c * 2 + 1)]);
                        if (visible) projected.push_back(xy);
                    }
                    constexpr double kTolerance = 3.0; // pixels
                    constexpr double kBikeTolerance = 4.0; // pixels: the GTE's integer shear against float
                    int within = 0;
                    double worst = 0, sum = 0;
                    std::string lines;
                    for (const std::array<long, 8>& pk : packets) {
                        double best = 1e30, bdx = 0, bdy = 0;
                        for (const std::array<double, 8>& xy : projected) {
                            double e = 0, dx = 0, dy = 0;
                            for (int c = 0; c < 4; ++c) {
                                const double ex = xy[static_cast<size_t>(c * 2)] - static_cast<double>(pk[static_cast<size_t>(c * 2)]);
                                const double ey = xy[static_cast<size_t>(c * 2 + 1)] - static_cast<double>(pk[static_cast<size_t>(c * 2 + 1)]);
                                e = std::max(e, std::hypot(ex, ey));
                                dx += ex / 4.0;
                                dy += ey / 4.0;
                            }
                            if (e < best) {
                                best = e;
                                bdx = dx;
                                bdy = dy;
                            }
                        }
                        if (best <= kTolerance) ++within;
                        worst = std::max(worst, best);
                        sum += best;
                        char b[200];
                        std::snprintf(b, sizeof(b),
                                      "  packet (%ld,%ld) (%ld,%ld) (%ld,%ld) (%ld,%ld): nearest quad of ours %.2f px, "
                                      "mean offset (%.1f, %.1f)\n",
                                      pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], pk[6], pk[7], best, bdx, bdy);
                        lines += b;
                    }
                    char summary[512];
                    std::snprintf(summary, sizeof(summary),
                                  "shadow: %zu original packets, %zu quads of ours; %d packets within %.0f px of one of ours "
                                  "(mean %.2f, worst %.2f)\n",
                                  packets.size(), projected.size(), within, kTolerance,
                                  packets.empty() ? 0.0 : sum / static_cast<double>(packets.size()), worst);
                    // The other direction, per quad of OURS: the bike's hull (the first ShadowBikeQuads) is
                    // placed by the bike's own matrix, the rider's by the rider's - and the rider's pose and
                    // placement are stand-ins, so the verdict is on the bike's.
                    const size_t bikeQuads = std::min(scene.ShadowBikeQuads(), projected.size());
                    int bikeWithin = 0, riderWithin = 0;
                    double bikeWorst = 0, riderSum = 0;
                    for (size_t i = 0; i < projected.size(); ++i) {
                        double best = 1e30;
                        for (const std::array<long, 8>& pk : packets) {
                            double e = 0;
                            for (int c = 0; c < 4; ++c)
                                e = std::max(e, std::hypot(projected[i][static_cast<size_t>(c * 2)] - static_cast<double>(pk[static_cast<size_t>(c * 2)]),
                                                           projected[i][static_cast<size_t>(c * 2 + 1)] - static_cast<double>(pk[static_cast<size_t>(c * 2 + 1)])));
                            best = std::min(best, e);
                        }
                        if (i < bikeQuads) {
                            bikeWorst = std::max(bikeWorst, best);
                            if (best <= kBikeTolerance) ++bikeWithin;
                        } else {
                            riderSum += best;
                            if (best <= kBikeTolerance) ++riderWithin;
                        }
                    }
                    char more[256];
                    std::snprintf(more, sizeof(more),
                                  "  ours: bike %zu quads, %d within %.0f px of an original packet (worst %.2f); rider %zu quads, "
                                  "%d within (mean %.2f) - the rider's pose is a stand-in\n",
                                  bikeQuads, bikeWithin, kBikeTolerance, bikeWorst, projected.size() - bikeQuads, riderWithin,
                                  projected.size() > bikeQuads ? riderSum / static_cast<double>(projected.size() - bikeQuads) : 0.0);
                    report += more;
                    std::fputs(more, stdout);
                    const bool clean = ok && projected.size() == packets.size() && bikeQuads > 0 &&
                                       bikeWithin == static_cast<int>(bikeQuads);
                    std::string verdict;
                    if (!ok) verdict = "FAIL (no state or no packets)";
                    else if (skyPacketMutate == 5)
                        verdict = clean ? "FAIL (negative control: the mutated shadow still matched the original)"
                                        : "PASS (negative control: the mutated shadow does not match the original)";
                    else
                        verdict = clean ? "PASS" : "FAIL";
                    report += summary;
                    report += "verdict " + verdict + "\n" + lines;
                    if (FILE* out = std::fopen(shadowCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("shadowcheck verdict %s\nshadowcheck report: %s\n", verdict.c_str(), shadowCheckPath.c_str());
                }
                if (!skyPacketCheckPath.empty()) {
                    // --skypacketcheck: the sky layers of THIS frame against the ORIGINAL's packets for
                    // the same captured state (the rows `psxgpu.py frame --prims` writes, as --roadcheck
                    // reads them), and the textures against the capture's VRAM.
                    //  * clouds (code 0x2F, `RASHCDG 0x8006396C`): every packet's four corners against the
                    //    nearest of our 24 segments projected with the capture's camera (TR = 0), its UVs
                    //    against that segment's slice, the image's 4-bit texels and CLUT against VRAM;
                    //  * panorama (code 0x2C on a 15-bit page at y 256, `RASHCDG 0x800644F4`): every
                    //    packet's corners against the nearest of our tiles, and its 16 x 16 texels as the
                    //    packet maps them onto the screen against the texels our tile maps onto the same
                    //    screen positions (mean absolute difference per channel; the MDEC hardware
                    //    against our IDCT and the 5-bit VRAM leave a few levels).
                    std::string report = "rrview --skypacketcheck: clouds and panorama against the original's packets\n";
                    bool ok = !stateRam.empty();
                    std::vector<uint8_t> vram;
                    if (FILE* in = std::fopen((statePath + "/vram.bin").c_str(), "rb")) {
                        vram.resize(1024 * 512 * 2);
                        if (std::fread(vram.data(), 1, vram.size(), in) != vram.size()) vram.clear();
                        std::fclose(in);
                    }
                    if (vram.empty()) ok = false;
                    const auto half = [&](int x, int y) -> uint16_t {
                        if (vram.empty()) return 0;
                        const size_t at = (static_cast<size_t>(y & 511) * 1024 + static_cast<size_t>(x & 1023)) * 2;
                        return static_cast<uint16_t>(vram[at] | (vram[at + 1] << 8));
                    };
                    std::vector<std::vector<std::string>> rows;
                    if (FILE* in = std::fopen(origPrimsPath.c_str(), "rb")) {
                        char line[1024];
                        while (std::fgets(line, sizeof(line), in)) {
                            std::vector<std::string> cols;
                            std::string cur;
                            for (const char* c = line; *c && *c != '\n' && *c != '\r'; ++c) {
                                if (*c == ',') {
                                    cols.push_back(cur);
                                    cur.clear();
                                } else {
                                    cur += *c;
                                }
                            }
                            cols.push_back(cur);
                            rows.push_back(cols);
                        }
                        std::fclose(in);
                    }
                    if (rows.size() < 2) ok = false;
                    std::map<std::string, size_t> column;
                    if (!rows.empty())
                        for (size_t k = 0; k < rows[0].size(); ++k) column[rows[0][k]] = k;
                    const auto num = [&](const std::vector<std::string>& r, const std::string& name) -> long {
                        const auto it = column.find(name);
                        if (it == column.end() || it->second >= r.size() || r[it->second].empty()) return 0;
                        return std::strtol(r[it->second].c_str(), nullptr, 0);
                    };
                    // A direction through the capture's camera, TR = 0, into the 384 x 240 frame.
                    const double aspectRow = rr::render::kGteAspectRow;
                    const auto projectDir = [&](const float d[3], double& sx, double& sy) {
                        double x = 0, y = 0, z = 0;
                        for (int k = 0; k < 3; ++k) {
                            x += static_cast<double>(d[k]) * sv.right[k];
                            y += static_cast<double>(d[k]) * sv.down[k];
                            z += static_cast<double>(d[k]) * sv.forward[k];
                        }
                        if (z <= 1.0) return false;
                        sx = 192.0 + 237.0 * x / z;
                        sy = 120.0 + 237.0 * aspectRow * y / z;
                        return true;
                    };
                    using SkyQuad = rr::render::RaceScene::SkyQuad;
                    const auto nearest = [&](const std::vector<SkyQuad>& quads, const long px[4], const long py[4],
                                             double& best) -> const SkyQuad* {
                        const SkyQuad* found = nullptr;
                        best = 1e30;
                        for (const SkyQuad& q : quads) {
                            double worst = 0;
                            bool visible = true;
                            for (int c = 0; c < 4 && visible; ++c) {
                                double sx = 0, sy = 0;
                                visible = projectDir(q.dir[c], sx, sy);
                                worst = std::max(worst, std::hypot(sx - static_cast<double>(px[c]),
                                                                   sy - static_cast<double>(py[c])));
                            }
                            if (visible && worst < best) {
                                best = worst;
                                found = &q;
                            }
                        }
                        return found;
                    };
                    const auto corners = [&](const std::vector<std::string>& r, long x[4], long y[4], long u[4], long v[4]) {
                        for (int c = 0; c < 4; ++c) {
                            const std::string k = std::to_string(c);
                            x[c] = num(r, "x" + k);
                            y[c] = num(r, "y" + k);
                            u[c] = num(r, "u" + k);
                            v[c] = num(r, "v" + k);
                        }
                    };
                    constexpr double kCornerTolerance = 2.0; // pixels: the GTE's integer RTPS against float
                    // ---- clouds
                    const rr::SkyClouds& clouds = scene.Clouds();
                    int cloudPackets = 0, cloudPlaced = 0, cloudUvSame = 0;
                    double cloudWorst = 0;
                    int cloudTexels = 0, cloudTexelSame = 0, cloudClutSame = 0;
                    long cloudTpage = -1, cloudClut = -1;
                    std::string cloudLines;
                    // `RASHCDI 0x80060F58` uploads the image to the configuration table's group 12 (x from
                    // the page, y 192 in every set - the packets' v runs 192..254).
                    constexpr long kCloudRow = 192;
                    for (size_t n = 1; n < rows.size(); ++n) {
                        const std::vector<std::string>& r = rows[n];
                        if (r.empty() || r[0] != "0x2F") continue;
                        ++cloudPackets;
                        long x[4], y[4], u[4], v[4];
                        corners(r, x, y, u, v);
                        cloudTpage = num(r, "tpage");
                        cloudClut = num(r, "clut");
                        double err = 0;
                        const SkyQuad* q = nearest(scene.CloudQuads(), x, y, err);
                        if (q && err <= kCornerTolerance) ++cloudPlaced;
                        cloudWorst = std::max(cloudWorst, q ? err : 1e9);
                        bool uvSame = q != nullptr;
                        for (int c = 0; c < 4 && q; ++c)
                            uvSame = uvSame && std::lround(q->tex[c][0]) == u[c] &&
                                     std::lround(q->tex[c][1]) + kCloudRow == v[c];
                        if (uvSame) ++cloudUvSame;
                        char b[256];
                        std::snprintf(b, sizeof(b),
                                      "  packet (%ld,%ld)-(%ld,%ld) u %ld..%ld: segment %d, corner error %.2f px, uv %s\n",
                                      x[0], y[0], x[3], y[3], u[0], u[1], q ? q->column : -1, q ? err : -1.0,
                                      uvSame ? "same" : "DIFFERENT");
                        cloudLines += b;
                    }
                    if (clouds.valid && cloudTpage >= 0) {
                        const int pageX = static_cast<int>(cloudTpage & 0xF) * 64;
                        for (int yy = 0; yy < clouds.rows; ++yy)
                            for (int xx = 0; xx < clouds.width; ++xx) {
                                const uint16_t word = half(pageX + xx / 4, static_cast<int>(kCloudRow) + yy);
                                const uint8_t texel = static_cast<uint8_t>((word >> ((xx & 3) * 4)) & 0xF);
                                ++cloudTexels;
                                if (texel == clouds.indices[static_cast<size_t>(yy) * static_cast<size_t>(clouds.width) +
                                                            static_cast<size_t>(xx)])
                                    ++cloudTexelSame;
                            }
                        for (int i = 0; i < 16; ++i)
                            if (half(static_cast<int>(cloudClut & 0x3F) * 16 + i, static_cast<int>((cloudClut >> 6) & 0x1FF)) ==
                                clouds.clut[static_cast<size_t>(i)])
                                ++cloudClutSame;
                    }
                    // ---- panorama
                    const rr::Panorama& pano = scene.SkyImage();
                    int panoPackets = 0, panoPlaced = 0, panoTileSame = 0;
                    double panoWorst = 0, madWorst = 0;
                    constexpr double kTileTolerance = 8.0;
                    std::string panoLines;
                    for (size_t n = 1; n < rows.size(); ++n) {
                        const std::vector<std::string>& r = rows[n];
                        if (r.empty() || r[0] != "0x2C") continue;
                        const long tpage = num(r, "tpage");
                        if (((tpage >> 7) & 3) != 2 || (tpage & 0x10) == 0) continue; // 15-bit page at y 256
                        ++panoPackets;
                        long x[4], y[4], u[4], v[4];
                        corners(r, x, y, u, v);
                        double err = 0;
                        const SkyQuad* q = nearest(scene.PanoramaQuads(), x, y, err);
                        if (q && err <= kCornerTolerance) ++panoPlaced;
                        panoWorst = std::max(panoWorst, q ? err : 1e9);
                        double mad = 1e9;
                        if (q && !pano.rgba.empty()) {
                            const int pageX = static_cast<int>(tpage & 0xF) * 64, pageY = 256;
                            double sum = 0;
                            for (int t = 0; t < 16; ++t)
                                for (int sIdx = 0; sIdx < 16; ++sIdx) {
                                    // The packet's texel at screen step (s, t): corner 0 plus s/15 of the
                                    // way to corner 1 and t/15 of the way to corner 2.
                                    const long pu = u[0] + (u[1] - u[0]) * sIdx / 15 + (u[2] - u[0]) * t / 15;
                                    const long pv = v[0] + (v[1] - v[0]) * sIdx / 15 + (v[2] - v[0]) * t / 15;
                                    const uint16_t word = half(pageX + static_cast<int>(pu), pageY + static_cast<int>(pv));
                                    // Ours at the same place: the tile's texel coordinates interpolated at
                                    // the texel centre and floored, as the shader floors them.
                                    const double fs = (sIdx + 0.5) / 16.0, ft = (t + 0.5) / 16.0;
                                    const double tx = q->tex[0][0] + (q->tex[1][0] - q->tex[0][0]) * fs +
                                                      (q->tex[2][0] - q->tex[0][0]) * ft;
                                    const double ty = q->tex[0][1] + (q->tex[1][1] - q->tex[0][1]) * fs +
                                                      (q->tex[2][1] - q->tex[0][1]) * ft;
                                    const int ix = static_cast<int>(std::floor(tx)), iy = static_cast<int>(std::floor(ty));
                                    if (ix < 0 || iy < 0 || ix >= rr::Panorama::kWidth || iy >= rr::Panorama::kHeight) {
                                        sum += 255.0 * 3;
                                        continue;
                                    }
                                    const uint8_t* ours =
                                        &pano.rgba[(static_cast<size_t>(iy) * rr::Panorama::kWidth + static_cast<size_t>(ix)) * 4];
                                    // 0x0000 is the GPU's transparent texel (the HORZ cut-out); ours is
                                    // alpha 0. Transparent on one side only is a full mismatch.
                                    const bool theirsClear = word == 0, oursClear = ours[3] == 0;
                                    if (theirsClear || oursClear) {
                                        if (theirsClear != oursClear) sum += 255.0 * 3;
                                        continue;
                                    }
                                    for (int ch = 0; ch < 3; ++ch)
                                        sum += std::fabs(static_cast<double>(((word >> (5 * ch)) & 31) << 3) -
                                                         static_cast<double>(ours[ch]));
                                }
                            mad = sum / (16.0 * 16.0 * 3.0);
                        }
                        if (mad <= kTileTolerance) ++panoTileSame;
                        madWorst = std::max(madWorst, mad);
                        char b[256];
                        std::snprintf(b, sizeof(b),
                                      "  packet (%ld,%ld) uv (%ld,%ld): column %d band %d row %d, corner error %.2f px, "
                                      "texel MAD %.2f\n",
                                      x[0], y[0], u[0], v[0], q ? q->column : -1, q ? q->band : -1, q ? q->row : -1,
                                      q ? err : -1.0, mad);
                        panoLines += b;
                    }
                    char summary[2048];
                    std::snprintf(summary, sizeof(summary),
                                  "clouds: %d packets, %d placed within %.1f px of one of our segments (worst %.2f), "
                                  "%d with the same UVs; texels %d of %d equal to VRAM, CLUT %d of 16\n"
                                  "panorama: %d packets, %d placed within %.1f px of one of our tiles (worst %.2f), "
                                  "%d tiles with texel MAD <= %.0f (worst %.2f)\n",
                                  cloudPackets, cloudPlaced, kCornerTolerance, cloudWorst, cloudUvSame, cloudTexelSame,
                                  cloudTexels, cloudClutSame, panoPackets, panoPlaced, kCornerTolerance, panoWorst,
                                  panoTileSame, kTileTolerance, madWorst);
                    const bool clean = ok && cloudPackets > 0 && cloudPlaced == cloudPackets &&
                                       cloudUvSame == cloudPackets && cloudTexels > 0 && cloudTexelSame == cloudTexels &&
                                       cloudClutSame == 16 && panoPackets > 0 && panoPlaced == panoPackets &&
                                       panoTileSame == panoPackets;
                    std::string verdict;
                    if (!ok) verdict = "FAIL (no state, VRAM or packets)";
                    else if (skyPacketMutate != 0)
                        verdict = clean ? "FAIL (negative control: the mutated sky still matched the original)"
                                        : "PASS (negative control: the mutated sky does not match the original)";
                    else
                        verdict = clean ? "PASS" : "FAIL";
                    report += summary;
                    report += "verdict " + verdict + "\n\nclouds:\n" + cloudLines + "\npanorama:\n" + panoLines;
                    if (FILE* out = std::fopen(skyPacketCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("skypacketcheck verdict %s\nskypacketcheck report: %s\n", verdict.c_str(),
                                skyPacketCheckPath.c_str());
                }
                if (!texCheckPath.empty()) {
                    if (!propTextureValid || propAtlas.Empty()) {
                        std::fprintf(stderr, "texcheck: no prop atlas is bound (pass --tex)\n");
                        return 1;
                    }
                    // The same frame twice more, whole and unchanged except for what the prop
                    // fragments write. Three channels are not enough for palette, index AND which
                    // prop, so it takes two reporting passes; both have exactly the coverage of the
                    // shot above, because the geometry, the order and the discard are the same.
                    // Blue 255 is the "this is a prop fragment" marker: no lit surface can reach it
                    // (the brightest tint in the scene is 0.72, i.e. 184).
                    std::vector<size_t> groupOf(drawnProps.size());
                    for (size_t n = 0; n < drawnProps.size(); ++n) groupOf[n] = drawnProps[n]->group;
                    drawScene(1);
                    glFinish();
                    const std::vector<uint8_t> sampled = ReadFrame(width, height); // palette, index
                    drawScene(2);
                    glFinish();
                    const std::vector<uint8_t> owner = ReadFrame(width, height); // which prop
                    drawScene(0); // leave the window showing the frame that was shot

                    // Now the comparison. A prop group carries no normals, so it is drawn unlit and
                    // its pixel IS the palette entry, byte for byte - which is the point of doing it
                    // this way: the test distinguishes right from wrong for a white or grey sign
                    // exactly as well as for a red one, where a "is it colourful enough" test cannot.
                    struct PropTally {
                        size_t group = 0, pixels = 0, matched = 0, changed = 0;
                    };
                    std::vector<PropTally> tally(std::min<size_t>(drawnProps.size(), 255));
                    for (size_t n = 0; n < tally.size(); ++n) tally[n].group = groupOf[n];
                    size_t pixels = 0, matched = 0, badId = 0;
                    std::vector<std::string> examples;
                    for (size_t p = 0; p + 3 < sampled.size(); p += 4) {
                        if (sampled[p + 2] != 255 || owner[p + 2] != 255) continue; // not a prop fragment
                        const size_t palette = sampled[p];
                        const size_t index = sampled[p + 1];
                        const size_t which = owner[p];
                        if (which >= tally.size() || palette >= static_cast<size_t>(propAtlas.paletteCount) ||
                            index >= static_cast<size_t>(propAtlas.paletteSize)) {
                            ++badId;
                            continue;
                        }
                        // The binding rule: palette `prim.tpage` of the atlas,
                        // entry `index`, is what this fragment must show.
                        //
                        // --texcheck-mutate is the negative control, and it is the more important
                        // half: it compares against the NEXT palette row instead, so a check that
                        // cannot fail is exposed as one that was measuring nothing.
                        const size_t row = texCheckMutate
                                               ? (palette + 1) % static_cast<size_t>(propAtlas.paletteCount)
                                               : palette;
                        const uint32_t want = propAtlas.palettes[row * 16 + index];
                        // Whether the control moved this pixel's expectation at all (see below).
                        if (want != propAtlas.palettes[palette * 16 + index]) ++tally[which].changed;
                        ++pixels;
                        ++tally[which].pixels;
                        const bool same = shot[p] == static_cast<uint8_t>(want & 0xFF) &&
                                          shot[p + 1] == static_cast<uint8_t>((want >> 8) & 0xFF) &&
                                          shot[p + 2] == static_cast<uint8_t>((want >> 16) & 0xFF);
                        if (same) {
                            ++matched;
                            ++tally[which].matched;
                        } else if (examples.size() < 8) {
                            char line[160];
                            std::snprintf(line, sizeof(line),
                                          "  prop %zu group %zu: palette %zu entry %zu wants "
                                          "%02X%02X%02X, frame has %02X%02X%02X",
                                          which, tally[which].group, palette, index,
                                          static_cast<unsigned>(want & 0xFF),
                                          static_cast<unsigned>((want >> 8) & 0xFF),
                                          static_cast<unsigned>((want >> 16) & 0xFF),
                                          static_cast<unsigned>(shot[p]), static_cast<unsigned>(shot[p + 1]),
                                          static_cast<unsigned>(shot[p + 2]));
                            examples.emplace_back(line);
                        }
                    }
                    size_t propsOk = 0, propsBad = 0, propsBlank = 0;
                    // A prop the mutation never moved - every one of its pixels has the same colour in
                    // the next palette row, which a 1-pixel prop far away easily does - asks the
                    // control nothing; it is counted apart and left out of the control's verdict,
                    // exactly as --cellcheck does (docs\formats\scene_cell.md 12.7.3). Not a threshold.
                    size_t propsVacuous = 0, propsOkTested = 0;
                    for (const PropTally& t : tally) {
                        if (t.pixels == 0) continue;
                        if (texCheckMutate && t.changed == 0) ++propsVacuous;
                        else if (t.matched == t.pixels) ++propsOkTested;
                    }
                    std::string report;
                    if (texCheckMutate)
                        report += "props the mutation did not move at all: " + std::to_string(propsVacuous) + "\n";
                    report += "rrview --texcheck: every visible prop pixel against the palette entry\n";
                    report += "the model -> LECT -> palette rule says it must show.\n\n";
                    for (size_t n = 0; n < tally.size(); ++n) {
                        const char* verdict = tally[n].pixels == 0 ? "not visible"
                                              : tally[n].matched == tally[n].pixels ? "MATCH"
                                                                                    : "MISMATCH";
                        if (tally[n].pixels == 0) ++propsBlank;
                        else if (tally[n].matched == tally[n].pixels) ++propsOk;
                        else ++propsBad;
                        char line[160];
                        std::snprintf(line, sizeof(line), "prop %3zu  group %2zu  pixels %6zu  matching %6zu  %s\n",
                                      n, tally[n].group, tally[n].pixels, tally[n].matched, verdict);
                        report += line;
                    }
                    char summary[1024];
                    std::snprintf(summary, sizeof(summary),
                                  "\nprops drawn %zu, of which fully matching %zu, mismatching %zu, "
                                  "not visible %zu\nprop pixels %zu, matching %zu, mismatching %zu, "
                                  "unreadable ids %zu\nverdict %s\n",
                                  tally.size(), propsOk, propsBad, propsBlank, pixels, matched,
                                  pixels - matched, badId,
                                  // The control asks whether any prop still matches THROUGHOUT under
                                  // the wrong palette row, not whether every single pixel changed.
                                  // Demanding zero matching pixels is too strict and gives a false
                                  // alarm: neighbouring rows of one palette can hold the same colour,
                                  // so a stray pixel coincides (measured on race 1/20 at 12000: one
                                  // pixel in 8207, a FAIL with nothing wrong). The
                                  // per-prop criterion is the one `--cellcheck` uses and it has no
                                  // tunable threshold in it.
                                  texCheckMutate
                                      ? (pixels > 0 && propsOkTested == 0
                                             ? "PASS (negative control: no prop the mutation moved matched throughout)"
                                             : "FAIL (negative control: a prop still matched throughout)")
                                      : ((pixels > 0 && matched == pixels && badId == 0 && propsOk > 0) ? "PASS"
                                                                                                        : "FAIL"));
                    report += summary;
                    for (const std::string& line : examples) report += line + "\n";
                    if (FILE* out = std::fopen(texCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("texcheck report: %s\n", texCheckPath.c_str());
                }
                if (cellCheck) {
                    // The same check as --texcheck, on the roadside world instead of the props.
                    // Pass 1 has every textured cell fragment report the palette row it selected
                    // (the primitive's own `pal`) and the index it sampled; pass 2 has it report
                    // which draw run it belongs to, which is what says WHICH page - and therefore
                    // which type-2 chunk - the comparison must use. A cell primitive carries no
                    // normal, so it is drawn unlit and its pixel IS the palette entry: the
                    // comparison is exact equality on all three bytes, which distinguishes right
                    // from wrong for a grey wall exactly as well as for a red one.
                    if (scene.CellTexturesEmpty()) {
                        std::fprintf(stderr, "cellcheck: no cell page is bound (pass --tex and --race)\n");
                        return 1;
                    }
                    std::vector<uint16_t> keyOf(drawnCells.size());
                    for (size_t n = 0; n < drawnCells.size(); ++n) keyOf[n] = drawnCells[n]->texKey;
                    drawScene(1);
                    glFinish();
                    const std::vector<uint8_t> sampled = ReadFrame(width, height); // palette, index
                    drawScene(2);
                    glFinish();
                    const std::vector<uint8_t> owner = ReadFrame(width, height); // which run
                    // render7: the vertex colour each fragment was modulated by (scene_cell.md 13):
                    // pass 7 reports (r, g), pass 8 (b, mode). A modulated pixel must be
                    // round(palette * colour / 128) per channel, saturating at 255.
                    drawScene(7);
                    glFinish();
                    const std::vector<uint8_t> shadeRG = ReadFrame(width, height);
                    drawScene(8);
                    glFinish();
                    const std::vector<uint8_t> shadeBM = ReadFrame(width, height);
                    drawScene(5);
                    glFinish();
                    const std::vector<uint8_t> covered = ReadFrame(width, height); // discard suppressed
                    drawScene(6);
                    glFinish();
                    const std::vector<uint8_t> silhouette = ReadFrame(width, height); // every surface
                    drawScene(0); // leave the window showing the frame that was shot
                    size_t coveredPixels = 0;
                    for (size_t p = 0; p + 3 < covered.size(); p += 4)
                        if (covered[p + 2] == 255 && covered[p] == 0 && covered[p + 1] == 0) ++coveredPixels;

                    // How much of the world has a hole in it. A hole is background that the world
                    // encloses, so it is counted against the frame's own skyline rather than
                    // against a guessed horizon row: in each column, the highest row that is not
                    // background is the skyline, and every background pixel BELOW it in that
                    // column is a hole. A column that never leaves the background contributes
                    // nothing, which is why the drawn coverage is reported next to it - filling
                    // holes drives one down and the other up, while drawing nothing at all drives
                    // both to zero and cannot be mistaken for success.
                    //
                    // "Background" is read off the silhouette pass, not off the shot's colours: in
                    // that pass every surface writes blue 255 through the same discard the real
                    // frame uses, so a pixel that is not blue is a pixel nothing covered. No guess
                    // about what the clear colour rounds to, and no way for a dark texel to be
                    // mistaken for a hole.
                    const auto isBackground = [&](size_t p) { return silhouette[p + 2] != 255; };
                    size_t drawnPixels = 0, holePixels = 0, skylineColumns = 0;
                    for (int x = 0; x < width; ++x) {
                        int skyline = -1;
                        for (int y = height - 1; y >= 0; --y) {
                            const size_t p = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                              static_cast<size_t>(x)) * 4;
                            if (!isBackground(p)) {
                                skyline = y;
                                break;
                            }
                        }
                        if (skyline < 0) continue;
                        ++skylineColumns;
                        for (int y = 0; y < skyline; ++y) {
                            const size_t p = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                              static_cast<size_t>(x)) * 4;
                            if (isBackground(p)) ++holePixels;
                        }
                    }
                    for (size_t p = 0; p + 3 < silhouette.size(); p += 4)
                        if (!isBackground(p)) ++drawnPixels;

                    struct RunTally {
                        uint16_t key = 0;
                        int band = 0;
                        size_t pixels = 0, matched = 0, changed = 0;
                    };
                    std::vector<RunTally> tally(std::min<size_t>(drawnCells.size(), 255));
                    for (size_t n = 0; n < tally.size(); ++n) {
                        tally[n].key = keyOf[n];
                        tally[n].band = drawnCells[n]->band;
                    }
                    size_t pixels = 0, matched = 0, badId = 0, untextured = 0, modulatedPixels = 0;
                    std::vector<std::string> examples;
                    for (size_t p = 0; p + 3 < sampled.size(); p += 4) {
                        if (sampled[p + 2] != 255 || owner[p + 2] != 255) continue; // not a cell fragment
                        const size_t palette = sampled[p];
                        const size_t index = sampled[p + 1];
                        const size_t which = owner[p];
                        if (which >= tally.size()) {
                            ++badId;
                            continue;
                        }
                        const auto atlas = cellAtlases.find(tally[which].key);
                        if (atlas == cellAtlases.end()) {
                            ++untextured;
                            continue;
                        }
                        const rr::IndexedTexture& page = atlas->second;
                        if (palette >= static_cast<size_t>(page.paletteCount) ||
                            index >= static_cast<size_t>(page.paletteSize)) {
                            ++badId;
                            continue;
                        }
                        // The binding rule: palette `prim.pal` of the type-2 page the cell's header
                        // pair names, entry `index`. The two mutations are the negative controls,
                        // and they are the more important half: one takes the next palette row of
                        // the right page, the other the right row of a different page. Under either
                        // one, no run may come out fully matching - a check that cannot fail is a
                        // check that measured nothing.
                        const rr::IndexedTexture* from = &page;
                        if (cellCheckMutate == 2) {
                            // The next page round the map whose palette block is actually
                            // DIFFERENT. Two cells of one road often ship the same terrain art
                            // under different resource ids, and binding a byte-identical page is
                            // not a wrong binding - it is the same image, so a control built on it
                            // would be asking the check to notice a difference that does not
                            // exist. This is not a threshold: it is the statement of the control.
                            auto other = cellAtlases.upper_bound(tally[which].key);
                            for (size_t step = 0; step < cellAtlases.size(); ++step) {
                                if (other == cellAtlases.end()) other = cellAtlases.begin();
                                if (other->second.palettes != page.palettes) break;
                                ++other;
                            }
                            if (other == cellAtlases.end()) other = cellAtlases.begin();
                            from = &other->second;
                        }
                        const size_t row = cellCheckMutate == 1
                                               ? (palette + 1) % static_cast<size_t>(page.paletteCount)
                                               : palette;
                        uint32_t want = from->palettes[row * 16 + index];
                        // The modulation: exact up to the GPU's own float-to-unorm rounding, the same
                        // one-level allowance --bikecheck makes for its lit pixels.
                        const bool modulated = shadeBM[p + 2] == 255 && shadeBM[p + 1] == 1 && shadeRG[p + 2] == 255;
                        if (modulated) {
                            const uint32_t shadeOf[3] = {shadeRG[p], shadeRG[p + 1], shadeBM[p]};
                            uint32_t out = 0;
                            for (int c = 0; c < 3; ++c) {
                                const uint32_t channel = (want >> (8 * c)) & 0xFF;
                                const uint32_t product = std::min<uint32_t>(255u, (channel * shadeOf[c] + 64u) / 128u);
                                out |= product << (8 * c);
                            }
                            want = out;
                            ++modulatedPixels;
                        }
                        // What the rule itself predicts, so a control can say whether it actually
                        // moved the expectation for this pixel. Two terrain pages of one road do
                        // share palette rows, and where the mutation lands on the same colour it
                        // has not tested anything - counting that as "the check failed to notice"
                        // would be a false alarm, and counting it as a pass would be worse.
                        const uint32_t truth = page.palettes[palette * 16 + index];
                        if (want != truth) ++tally[which].changed;
                        ++pixels;
                        ++tally[which].pixels;
                        const int slack = modulated ? 1 : 0;
                        const bool same = std::abs(static_cast<int>(shot[p]) - static_cast<int>(want & 0xFF)) <= slack &&
                                          std::abs(static_cast<int>(shot[p + 1]) - static_cast<int>((want >> 8) & 0xFF)) <= slack &&
                                          std::abs(static_cast<int>(shot[p + 2]) - static_cast<int>((want >> 16) & 0xFF)) <= slack;
                        if (same) {
                            ++matched;
                            ++tally[which].matched;
                        } else if (examples.size() < 8) {
                            char line[192];
                            std::snprintf(line, sizeof(line),
                                          "  run %zu key %04X: palette %zu entry %zu wants "
                                          "%02X%02X%02X, frame has %02X%02X%02X",
                                          which, tally[which].key, palette, index,
                                          static_cast<unsigned>(want & 0xFF),
                                          static_cast<unsigned>((want >> 8) & 0xFF),
                                          static_cast<unsigned>((want >> 16) & 0xFF),
                                          static_cast<unsigned>(shot[p]), static_cast<unsigned>(shot[p + 1]),
                                          static_cast<unsigned>(shot[p + 2]));
                            examples.emplace_back(line);
                        }
                    }
                    size_t runsOk = 0, runsBad = 0, runsBlank = 0, runsNoPage = 0;
                    // A run the mutation never moved is not a control: the wrong page or the wrong
                    // row happened to hold the same colours everywhere this run looked, so there
                    // was nothing for the check to catch. Those runs are counted apart and left
                    // out of the control's verdict.
                    size_t runsVacuous = 0, runsOkTested = 0;
                    for (const RunTally& run : tally) {
                        if (!cellAtlases.count(run.key)) ++runsNoPage;
                        if (run.pixels == 0) continue;
                        if (cellCheckMutate != 0 && run.changed == 0) ++runsVacuous;
                        else if (run.matched == run.pixels) ++runsOkTested;
                    }
                    std::string report;
                    report += "rrview --cellcheck: every visible scene-cell pixel against the palette\n";
                    report += "entry the cell -> texture chunk -> palette rule (scene_cell.md 12) says it\n";
                    report += "must show. One run is one cell's primitives of one band on one page: band 0\n";
                    report += "samples the type-2 chunk of the cell's own resource id, band 1 the two type-1\n";
                    report += "chunks of it, and key 7800 is the fixed DATA\\G_OBJ01.GTP page.\n\n";
                    for (size_t n = 0; n < tally.size(); ++n) {
                        const char* verdict = tally[n].pixels == 0 ? "not visible"
                                              : tally[n].matched == tally[n].pixels ? "MATCH"
                                                                                    : "MISMATCH";
                        if (tally[n].pixels == 0) ++runsBlank;
                        else if (tally[n].matched == tally[n].pixels) ++runsOk;
                        else ++runsBad;
                        char line[224];
                        std::snprintf(line, sizeof(line),
                                      "run %3zu  band %d  key %04X  pixels %6zu  matching %6zu  %s%s\n", n,
                                      tally[n].band, tally[n].key, tally[n].pixels, tally[n].matched, verdict,
                                      (cellCheckMutate != 0 && tally[n].pixels > 0 && tally[n].changed == 0)
                                          ? "  (the mutation changed nothing here)"
                                          : "");
                        report += line;
                    }
                    {
                        char coverage[384];
                        std::snprintf(coverage, sizeof(coverage),
                                      "\nframe %d x %d = %zu pixels; not background %zu (%.2f%%); "
                                      "background enclosed by the skyline %zu (%.2f%% of the frame), "
                                      "over %zu columns that reach the world\n",
                                      width, height, static_cast<size_t>(width) * height, drawnPixels,
                                      100.0 * static_cast<double>(drawnPixels) /
                                          static_cast<double>(static_cast<size_t>(width) * height),
                                      holePixels,
                                      100.0 * static_cast<double>(holePixels) /
                                          static_cast<double>(static_cast<size_t>(width) * height),
                                      skylineColumns);
                        report += coverage;
                        std::fputs(coverage, stdout);
                    }
                    char summary[1024];
                    std::snprintf(summary, sizeof(summary),
                                  "\ncell runs drawn %zu, of which fully matching %zu, mismatching %zu, "
                                  "not visible %zu\ncell pixels %zu (modulated by the level's colour table %zu), "
                                  "matching %zu, mismatching %zu, "
                                  "unreadable ids %zu\nruns whose key has no page at all %zu, their "
                                  "reported pixels %zu\ncell pixels the mesh covers before the "
                                  "transparent-texel discard %zu, i.e. %zu discarded\n"
                                  "runs the mutation did not move at all %zu, of the rest still "
                                  "matching throughout %zu\nverdict %s\n",
                                  tally.size(), runsOk, runsBad, runsBlank, pixels, modulatedPixels, matched, pixels - matched,
                                  badId, runsNoPage, untextured, coveredPixels,
                                  coveredPixels > pixels ? coveredPixels - pixels : 0, runsVacuous, runsOkTested,
                                  pixels == 0 ? "EMPTY (no cell fragment in this frame)"
                                  : cellCheckMutate
                                      // A wrong palette row still lands on the right colour for the
                                      // odd pixel, because two rows of one terrain page do share
                                      // entries. What cannot survive a wrong rule is a whole RUN
                                      // matching on every one of its pixels, so that is the
                                      // criterion - no threshold to tune. A run the mutation never
                                      // moved is left out of it: the wrong page held the same
                                      // colours everywhere that run looked, so it was never asked
                                      // a question, and the report names how many such runs there
                                      // were rather than hiding them.
                                      ? (pixels > 0 && runsOkTested == 0
                                             ? "PASS (negative control: no run the mutation moved matched throughout)"
                                             : "FAIL (negative control: a run still matched throughout)")
                                      : ((pixels > 0 && matched == pixels && badId == 0 && runsOk > 0) ? "PASS"
                                                                                                       : "FAIL"));
                    report += summary;
                    for (const std::string& line : examples) report += line + "\n";
                    if (FILE* out = std::fopen(cellCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("cellcheck report: %s\n", cellCheckPath.c_str());
                }
                if (bikeCheck) {
                    // The same shape of check as --texcheck and --cellcheck, on the player's bike
                    // and rider. Two differences, both forced by the data:
                    //   * the sheet a run samples is not a property of the primitive but of the
                    //     SUB-MESH (`prim.clut` bit 7, read once per sub-mesh), so pass 2 reports
                    //     the run and the run names the sheet;
                    //   * `BBLEVEL1.GEO` model 100 group 0 carries 48 normals and model 150 group
                    //     0 carries 49, so unlike the props and the cells these surfaces are LIT
                    //     and the pixel is not the palette entry. Pass 3 reports the quantised
                    //     shade the lit path applied, so the expected value is
                    //     `round(palette * shade / 255)` - still an exact prediction, with no
                    //     tolerance and no guess about the lighting term.
                    if (scene.RiderSheetTexturesEmpty() || bikeParts.empty()) {
                        std::fprintf(stderr, "bikecheck: no bike sheet is bound (pass --tex --drive)\n");
                        return 1;
                    }
                    drawScene(1);
                    glFinish();
                    const std::vector<uint8_t> sampled = ReadFrame(width, height); // palette, index
                    drawScene(2);
                    glFinish();
                    const std::vector<uint8_t> owner = ReadFrame(width, height);   // which run
                    drawScene(3);
                    glFinish();
                    const std::vector<uint8_t> shaded = ReadFrame(width, height);  // the lit term
                    drawScene(0); // leave the window showing the frame that was shot

                    struct RunTally {
                        int sheet = 0, owner = 0;
                        size_t pixels = 0, matched = 0, near1 = 0, changed = 0;
                    };
                    std::vector<RunTally> tally(std::min<size_t>(bikeParts.size(), 255));
                    for (size_t n = 0; n < tally.size(); ++n) {
                        tally[n].sheet = bikeParts[n].sheet;
                        tally[n].owner = bikeParts[n].owner;
                    }
                    size_t pixels = 0, matched = 0, near1 = 0, badId = 0, overIndex = 0;
                    std::vector<std::string> examples;
                    for (size_t p = 0; p + 3 < sampled.size(); p += 4) {
                        if (sampled[p + 2] != 255 || owner[p + 2] != 255 || shaded[p + 2] != 255) continue;
                        const size_t palette = sampled[p];
                        const size_t index = sampled[p + 1];
                        const size_t which = owner[p];
                        const double shade = static_cast<double>(shaded[p]);
                        if (which >= tally.size()) {
                            ++badId;
                            continue;
                        }
                        const int realSheet = tally[which].sheet;
                        // The two negative controls. One expects the NEXT `KNBP` block (a different
                        // racer's colour scheme on the same art), the other expects the same texel
                        // read out of the OTHER sheet of the pair - which is exactly the mistake a
                        // renderer that ignored `prim.clut` bit 7 would make, sending the wheels to
                        // the bike sheet and everything else to the rim sheet.
                        int useSheet = realSheet;
                        if (bikeCheckMutate == 2) useSheet = realSheet == 0 ? 1 : (realSheet == 1 ? 0 : 0);
                        const std::vector<rr::IndexedTexture>& from =
                            (bikeCheckMutate == 1 && riderSheetsAlt.size() == riderSheets.size())
                                ? riderSheetsAlt
                                : riderSheets;
                        if (static_cast<size_t>(useSheet) >= from.size() ||
                            static_cast<size_t>(realSheet) >= riderSheets.size()) {
                            ++badId;
                            continue;
                        }
                        const rr::IndexedTexture& sheet = from[static_cast<size_t>(useSheet)];
                        const rr::IndexedTexture& truthSheet = riderSheets[static_cast<size_t>(realSheet)];
                        if (palette >= static_cast<size_t>(sheet.paletteCount)) {
                            ++badId;
                            continue;
                        }
                        // An 8bpp sheet gets a 128-entry palette, which is the size the engine's own
                        // racer bank has. An index of 128 or more would be
                        // clamped by the sampler and could not be predicted, so it is counted
                        // rather than quietly compared.
                        if (index >= static_cast<size_t>(sheet.paletteSize)) {
                            ++overIndex;
                            continue;
                        }
                        const uint32_t want = sheet.palettes[palette * static_cast<size_t>(sheet.paletteSize) + index];
                        const uint32_t truth =
                            truthSheet.palettes[palette * static_cast<size_t>(truthSheet.paletteSize) + index];
                        if (want != truth) ++tally[which].changed;
                        ++pixels;
                        ++tally[which].pixels;
                        int diff = 0;
                        bool same = true;
                        for (int c = 0; c < 3; ++c) {
                            const long expected =
                                std::lround(static_cast<double>((want >> (8 * c)) & 0xFFu) * shade / 255.0);
                            const int got = shot[p + static_cast<size_t>(c)];
                            const int d = std::abs(static_cast<int>(expected) - got);
                            diff = std::max(diff, d);
                            if (d != 0) same = false;
                        }
                        if (same) {
                            ++matched;
                            ++tally[which].matched;
                        }
                        if (diff <= 1) {
                            ++near1;
                            ++tally[which].near1;
                        } else if (examples.size() < 8) {
                            char line[200];
                            std::snprintf(line, sizeof(line),
                                          "  run %zu sheet %d: palette %zu entry %zu shade %.0f wants "
                                          "%02X%02X%02X, frame has %02X%02X%02X",
                                          which, realSheet, palette, index, shade,
                                          static_cast<unsigned>(want & 0xFF),
                                          static_cast<unsigned>((want >> 8) & 0xFF),
                                          static_cast<unsigned>((want >> 16) & 0xFF),
                                          static_cast<unsigned>(shot[p]), static_cast<unsigned>(shot[p + 1]),
                                          static_cast<unsigned>(shot[p + 2]));
                            examples.emplace_back(line);
                        }
                    }
                    size_t runsOk = 0, runsBad = 0, runsBlank = 0, runsVacuous = 0, runsOkTested = 0;
                    std::string report;
                    report += "rrview --bikecheck: every visible pixel of the player's bike and rider\n";
                    report += "against the entry the model -> LECT -> KNBP rule\n";
                    report += "predicts. Sheet 0 is the bike sheet, 1 the RIMA1.TIM rim\n";
                    report += "sheet that prim.clut bit 7 selects, 2 the rider sheet; owner 0 is the\n";
                    report += "bike model and 1 the rider model. These meshes carry normals and are\n";
                    report += "LIT, so the prediction is round(palette * shade / 255) with the shade\n";
                    report += "read back from the frame itself, not the raw palette entry.\n\n";
                    for (size_t n = 0; n < tally.size(); ++n) {
                        const char* verdict = tally[n].pixels == 0 ? "not visible"
                                              : tally[n].near1 == tally[n].pixels ? "MATCH"
                                                                                  : "MISMATCH";
                        if (tally[n].pixels == 0) ++runsBlank;
                        else if (tally[n].near1 == tally[n].pixels) ++runsOk;
                        else ++runsBad;
                        if (tally[n].pixels != 0 && bikeCheckMutate != 0) {
                            if (tally[n].changed == 0) ++runsVacuous;
                            else if (tally[n].near1 == tally[n].pixels) ++runsOkTested;
                        }
                        char line[224];
                        std::snprintf(line, sizeof(line),
                                      "run %3zu  owner %d  sheet %d  pixels %6zu  exact %6zu  within 1 %6zu  %s%s\n",
                                      n, tally[n].owner, tally[n].sheet, tally[n].pixels, tally[n].matched,
                                      tally[n].near1, verdict,
                                      (bikeCheckMutate != 0 && tally[n].pixels > 0 && tally[n].changed == 0)
                                          ? "  (the mutation changed nothing here)"
                                          : "");
                        report += line;
                    }
                    char summary[640];
                    std::snprintf(summary, sizeof(summary),
                                  "\nbike/rider runs drawn %zu, of which fully matching %zu, mismatching %zu, "
                                  "not visible %zu\npixels %zu, exact %zu, within 1 %zu, mismatching %zu, "
                                  "unreadable ids %zu, index past the palette %zu\n"
                                  "runs the mutation did not move at all %zu, of the rest still matching "
                                  "throughout %zu\nverdict %s\n",
                                  tally.size(), runsOk, runsBad, runsBlank, pixels, matched, near1,
                                  pixels - near1, badId, overIndex, runsVacuous, runsOkTested,
                                  pixels == 0 ? "EMPTY (the bike drew no pixel in this frame)"
                                  : bikeCheckMutate
                                      // Same criterion as --cellcheck: a wrong palette still lands
                                      // on the right colour for the odd pixel, so what must not
                                      // survive is a whole RUN matching on every one of its pixels.
                                      ? (runsOkTested == 0
                                             ? "PASS (negative control: no run the mutation moved matched throughout)"
                                             : "FAIL (negative control: a run still matched throughout)")
                                      : ((near1 == pixels && badId == 0 && overIndex == 0 && runsOk > 0) ? "PASS"
                                                                                                         : "FAIL"));
                    report += summary;
                    for (const std::string& line : examples) report += line + "\n";
                    if (FILE* out = std::fopen(bikeCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("bikecheck report: %s\n", bikeCheckPath.c_str());
                }
                if (skyCheck) {
                    // The backdrop's counterpart of --texcheck / --cellcheck / --bikecheck. It has
                    // no palette at all - a panorama strip decodes straight to RGB - so what the
                    // frame is checked against is the decoded image itself: pass 1 has every
                    // backdrop fragment report the texel COLUMN it sampled (as a high and a low
                    // byte, because the panorama is 1760 wide) and pass 2 the texel ROW, and the
                    // frame pixel must then be that texel exactly. The backdrop is drawn unshaded,
                    // so this is equality on all three bytes with no tolerance.
                    if (skyBound == static_cast<size_t>(-1)) {
                        std::fprintf(stderr, "skycheck: no panorama is bound (needs --race and a route position)\n");
                        return 1;
                    }
                    drawScene(1);
                    glFinish();
                    const std::vector<uint8_t> sampledU = ReadFrame(width, height);
                    drawScene(2);
                    glFinish();
                    const std::vector<uint8_t> sampledV = ReadFrame(width, height);
                    drawScene(0);

                    struct ColumnTally {
                        size_t pixels = 0, matched = 0, changed = 0;
                    };
                    std::vector<ColumnTally> tally(rr::Panorama::kColumns);
                    size_t pixels = 0, matched = 0, badId = 0;
                    std::vector<std::string> examples;
                    for (size_t p = 0; p + 3 < sampledU.size(); p += 4) {
                        if (sampledU[p + 2] != 255 || sampledV[p + 2] != 255) continue;
                        const int u = sampledU[p] * 256 + sampledU[p + 1];
                        const int v = sampledV[p];
                        if (u < 0 || u >= rr::Panorama::kWidth || v < 0 || v >= rr::Panorama::kHeight) {
                            ++badId;
                            continue;
                        }
                        // The two negative controls: the texel one column (16 texels) to the right,
                        // and the texel one band (16 rows) down, wrapping. Either is the kind of
                        // off-by-one a wrong strip-to-column or band-to-row rule would produce.
                        int wu = u, wv = v;
                        if (skyCheckMutate == 1) wu = (u + rr::Panorama::kTile) % rr::Panorama::kWidth;
                        if (skyCheckMutate == 2) wv = (v + rr::Panorama::kTile) % rr::Panorama::kHeight;
                        const size_t want = (static_cast<size_t>(wv) * rr::Panorama::kWidth + static_cast<size_t>(wu)) * 4;
                        const size_t truth = (static_cast<size_t>(v) * rr::Panorama::kWidth + static_cast<size_t>(u)) * 4;
                        const size_t column = static_cast<size_t>(u) / rr::Panorama::kTile;
                        ++pixels;
                        ++tally[column].pixels;
                        bool moved = false;
                        for (int c = 0; c < 4; ++c) moved = moved || skyImage.rgba[want + c] != skyImage.rgba[truth + c];
                        if (moved) ++tally[column].changed;
                        const bool same = shot[p] == skyImage.rgba[want + 0] &&
                                          shot[p + 1] == skyImage.rgba[want + 1] &&
                                          shot[p + 2] == skyImage.rgba[want + 2];
                        if (same) {
                            ++matched;
                            ++tally[column].matched;
                        } else if (examples.size() < 8) {
                            char line[192];
                            std::snprintf(line, sizeof(line),
                                          "  texel (%d,%d) wants %02X%02X%02X, frame has %02X%02X%02X", u, v,
                                          skyImage.rgba[want + 0], skyImage.rgba[want + 1], skyImage.rgba[want + 2],
                                          static_cast<unsigned>(shot[p]), static_cast<unsigned>(shot[p + 1]),
                                          static_cast<unsigned>(shot[p + 2]));
                            examples.emplace_back(line);
                        }
                    }
                    size_t colsOk = 0, colsBad = 0, colsBlank = 0, colsVacuous = 0, colsOkTested = 0;
                    std::string report;
                    report += "rrview --skycheck: every visible backdrop pixel against the texel the\n";
                    report += "type-4 panorama decodes to. One run is one 16-texel column of the\n";
                    report += "1760 x 128 image, i.e. one of the 110 columns STEN describes.\n\n";
                    for (size_t n = 0; n < tally.size(); ++n) {
                        const char* verdict = tally[n].pixels == 0 ? "not visible"
                                              : tally[n].matched == tally[n].pixels ? "MATCH"
                                                                                    : "MISMATCH";
                        if (tally[n].pixels == 0) ++colsBlank;
                        else if (tally[n].matched == tally[n].pixels) ++colsOk;
                        else ++colsBad;
                        if (tally[n].pixels != 0 && skyCheckMutate != 0) {
                            if (tally[n].changed == 0) ++colsVacuous;
                            else if (tally[n].matched == tally[n].pixels) ++colsOkTested;
                        }
                        if (tally[n].pixels == 0) continue;
                        char line[192];
                        std::snprintf(line, sizeof(line), "column %3zu  pixels %6zu  matching %6zu  %s%s\n", n,
                                      tally[n].pixels, tally[n].matched, verdict,
                                      (skyCheckMutate != 0 && tally[n].changed == 0)
                                          ? "  (the mutation changed nothing here)"
                                          : "");
                        report += line;
                    }
                    char summary[640];
                    std::snprintf(summary, sizeof(summary),
                                  "\npanorama chunk id %u, %d tiles decoded\nbackdrop columns visible %zu, of "
                                  "which fully matching %zu, mismatching %zu\nbackdrop pixels %zu, matching %zu, "
                                  "mismatching %zu, texel out of range %zu\ncolumns the mutation did not move at "
                                  "all %zu, of the rest still matching throughout %zu\nverdict %s\n",
                                  scene.SkyHeaders()[skyBound].id, skyImage.tiles,
                                  tally.size() - colsBlank, colsOk, colsBad, pixels, matched, pixels - matched,
                                  badId, colsVacuous, colsOkTested,
                                  pixels == 0 ? "EMPTY (no backdrop pixel in this frame)"
                                  : skyCheckMutate
                                      ? (colsOkTested == 0
                                             ? "PASS (negative control: no column the mutation moved matched throughout)"
                                             : "FAIL (negative control: a column still matched throughout)")
                                      : ((matched == pixels && badId == 0 && colsOk > 0) ? "PASS" : "FAIL"));
                    report += summary;
                    for (const std::string& line : examples) report += line + "\n";
                    if (FILE* out = std::fopen(skyCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("skycheck report: %s\n", skyCheckPath.c_str());
                }
                if (!skyGradCheckPath.empty()) {
                    // The sky gradient's counterpart of the other four. It has no texture and no
                    // palette, so what the frame is checked against is the BLEND: pass 1 has every
                    // gradient fragment report the two interpolation parameters it was given -
                    // quantised to the same 1/255 grid the framebuffer uses, so the CPU sees the
                    // identical inputs - and the pixel must then be the colour that blend of the
                    // four bundle entries produces. The gradient is drawn unshaded, so the only
                    // tolerance is the one bikecheck already needs: float-to-unorm rounding in the
                    // shader against double arithmetic on the CPU, which is at most one level.
                    if (!scene.SkyGradientReady()) {
                        std::fprintf(stderr, "skygradcheck: no sky gradient is bound (needs --road)\n");
                        return 1;
                    }
                    drawScene(1);
                    glFinish();
                    const std::vector<uint8_t> reported = ReadFrame(width, height);
                    drawScene(0);

                    const rr::SkyGradient& truth =
                        skyGradCheckMutate && skyGradientAlt.valid ? skyGradientAlt : skyGradient;
                    uint8_t wantMidLeft[3], wantMidRight[3];
                    if (&truth == &skyGradient) {
                        for (int k = 0; k < 3; ++k) {
                            wantMidLeft[k] = gradMidLeft[k];
                            wantMidRight[k] = gradMidRight[k];
                        }
                    } else {
                        // The control keeps the same weights and changes only the four colours, so
                        // it is the level bundle that is under test and nothing else.
                        const double turn = 4096.0 / (2.0 * 3.14159265358979323846);
                        float forward[3] = {target[0] - eye[0], 0.0f, target[2] - eye[2]};
                        const float length = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
                        if (length > 1e-6f) {
                            forward[0] /= length;
                            forward[2] /= length;
                        }
                        const int32_t yaw =
                            static_cast<int32_t>(std::lround(
                                std::atan2(static_cast<double>(forward[0]), static_cast<double>(forward[2])) *
                                turn)) - sunAngle;
                        rr::SkyBlendColour(truth, rr::SkyBlendWeight(yaw - rr::kSkyEdgeAngle), wantMidLeft);
                        rr::SkyBlendColour(truth, rr::SkyBlendWeight(yaw + rr::kSkyEdgeAngle), wantMidRight);
                    }
                    // A run is one sixteenth of the way down the gradient, so a wrong bundle has to
                    // move most of them and the per-run criterion has something to bite on.
                    constexpr int kBands = 16;
                    struct BandTally {
                        size_t pixels = 0, matched = 0, exact = 0, changed = 0;
                    };
                    std::vector<BandTally> tally(kBands);
                    size_t pixels = 0, matched = 0, exact = 0;
                    std::vector<std::string> examples;
                    const auto predict = [](const rr::SkyGradient& sky, const uint8_t midL[3],
                                            const uint8_t midR[3], double row, double side, double out[3]) {
                        for (int c = 0; c < 3; ++c) {
                            const double mid = midL[c] * (1.0 - side) + midR[c] * side;
                            const double top = sky.rgb[0][c], horizon = sky.rgb[1][c];
                            out[c] = row < 0.5 ? top * (1.0 - row * 2.0) + mid * (row * 2.0)
                                               : mid * (1.0 - (row - 0.5) * 2.0) + horizon * ((row - 0.5) * 2.0);
                        }
                    };
                    for (size_t p = 0; p + 3 < reported.size(); p += 4) {
                        if (reported[p + 2] != 255) continue; // not a gradient fragment
                        const double row = reported[p] / 255.0;
                        const double side = reported[p + 1] / 255.0;
                        double want[3], real[3];
                        predict(truth, wantMidLeft, wantMidRight, row, side, want);
                        predict(skyGradient, gradMidLeft, gradMidRight, row, side, real);
                        const size_t band = std::min<size_t>(kBands - 1,
                                                             static_cast<size_t>(row * kBands));
                        ++pixels;
                        ++tally[band].pixels;
                        bool moved = false;
                        for (int c = 0; c < 3; ++c)
                            moved = moved || std::lround(want[c]) != std::lround(real[c]);
                        if (moved) ++tally[band].changed;
                        int worst = 0;
                        for (int c = 0; c < 3; ++c) {
                            const long diff = static_cast<long>(shot[p + static_cast<size_t>(c)]) -
                                              std::lround(want[c]);
                            worst = std::max(worst, static_cast<int>(diff < 0 ? -diff : diff));
                        }
                        if (worst == 0) {
                            ++exact;
                            ++tally[band].exact;
                        }
                        if (worst <= 1) {
                            ++matched;
                            ++tally[band].matched;
                        } else if (examples.size() < 8) {
                            char line[192];
                            std::snprintf(line, sizeof(line),
                                          "  row %.3f side %.3f wants %02lX%02lX%02lX, frame has %02X%02X%02X",
                                          row, side, std::lround(want[0]), std::lround(want[1]),
                                          std::lround(want[2]), static_cast<unsigned>(shot[p]),
                                          static_cast<unsigned>(shot[p + 1]), static_cast<unsigned>(shot[p + 2]));
                            examples.emplace_back(line);
                        }
                    }
                    size_t bandsOk = 0, bandsBad = 0, bandsBlank = 0, bandsVacuous = 0, bandsOkTested = 0;
                    std::string report;
                    report += "rrview --skygradcheck: every sky-gradient pixel against the blend of the\n";
                    report += "four colours DATA\\GAMEBIN1.DAT carries for this level bundle. A run is one\n";
                    report += "sixteenth of the way from the top of the sky to the horizon.\n\n";
                    for (size_t n = 0; n < tally.size(); ++n) {
                        if (tally[n].pixels == 0) {
                            ++bandsBlank;
                            continue;
                        }
                        if (tally[n].matched == tally[n].pixels) ++bandsOk; else ++bandsBad;
                        if (skyGradCheckMutate) {
                            if (tally[n].changed == 0) ++bandsVacuous;
                            else if (tally[n].matched == tally[n].pixels) ++bandsOkTested;
                        }
                        char line[192];
                        std::snprintf(line, sizeof(line),
                                      "band %2zu  pixels %6zu  matching %6zu  exact %6zu  %s%s\n", n,
                                      tally[n].pixels, tally[n].matched, tally[n].exact,
                                      tally[n].matched == tally[n].pixels ? "MATCH" : "MISMATCH",
                                      (skyGradCheckMutate && tally[n].changed == 0)
                                          ? "  (the mutation changed nothing here)"
                                          : "");
                        report += line;
                    }
                    char summary[640];
                    std::snprintf(summary, sizeof(summary),
                                  "\nsky gradient bundle %zu (control %s)\nbands visible %zu, of which fully "
                                  "matching %zu, mismatching %zu\ngradient pixels %zu, matching within 1 %zu, "
                                  "exact %zu, mismatching %zu\nbands the mutation did not move at all %zu, of "
                                  "the rest still matching throughout %zu\nverdict %s\n",
                                  skyGradient.bundle, skyGradCheckMutate ? "on" : "off",
                                  tally.size() - bandsBlank, bandsOk, bandsBad, pixels, matched, exact,
                                  pixels - matched, bandsVacuous, bandsOkTested,
                                  pixels == 0 ? "EMPTY (no sky-gradient pixel in this frame)"
                                  : skyGradCheckMutate
                                      ? (bandsOkTested == 0
                                             ? "PASS (negative control: no band the mutation moved matched throughout)"
                                             : "FAIL (negative control: a band still matched throughout)")
                                      : ((matched == pixels && bandsOk > 0) ? "PASS" : "FAIL"));
                    report += summary;
                    for (const std::string& line : examples) report += line + "\n";
                    if (FILE* out = std::fopen(skyGradCheckPath.c_str(), "wb")) {
                        std::fwrite(report.data(), 1, report.size(), out);
                        std::fclose(out);
                    }
                    std::fputs(summary, stdout);
                    std::printf("skygradcheck report: %s\n", skyGradCheckPath.c_str());
                }
                SwapBuffers(window.dc);
                break;
            }
            SwapBuffers(window.dc);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
